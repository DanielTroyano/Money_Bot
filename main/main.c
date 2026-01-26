/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

/* ESP System */
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_mac.h"

/* Wi-Fi */
#include "esp_wifi.h"

/* HTTP Server for Captive Portal */
#include "esp_http_server.h"
#include "dns_server.h"

/* SNTP */
#include "esp_sntp.h"

/* MQTT */
#include "mqtt_client.h"

/* JSON parsing */
#include "cJSON.h"

/* QR Code */
#include "qrcode.h"

/* Hardware */
#include "led_strip.h"

/* LCD and LVGL */
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* Timer */
#include "esp_timer.h"

/* ============================================================================
 * CONFIGURATION
 * ============================================================================ */

/* Hardware Pins */
#define LED_GPIO 38
#define LCD_SPI_HOST SPI2_HOST
#define LCD_SCLK 12
#define LCD_MOSI 11
#define LCD_CS 10
#define LCD_DC 9
#define LCD_RST 8
#define LCD_BLK 7
#define LCD_RES 240

/* Animation */
#define NUM_TOKENS 10
#define TOKEN_SPACING (200 / (NUM_TOKENS - 1))
#define RAIN_TIME_MS 1800

/* Colors */
#define COL_BG 0x1A1A2E
#define COL_ROBOT 0x4A4A4A
#define COL_ACCENT 0x6A6A6A
#define COL_CYAN 0x00FFFF
#define COL_GREEN 0x00FF00
#define COL_GOLD 0xFFD700
#define COL_MONEY_GREEN 0x228B22
#define COL_RED 0xFF0000

/* Device Identity */
#define DEFAULT_DEVICE_ID "moneybot-dev-001"
#define NVS_NAMESPACE "moneybot"
#define NVS_KEY_DEVICE_ID "device_id"

/* AWS IoT MQTT Configuration */
#define AWS_IOT_ENDPOINT "a3krir0duhayc0-ats.iot.us-east-1.amazonaws.com"
#define AWS_IOT_PORT 8883
#define MQTT_BROKER_URI "mqtts://" AWS_IOT_ENDPOINT ":8883"

/* Provisioning */
#define PROV_SERVICE_NAME_PREFIX "PROV_MoneyBot_"
#define PROV_POP "abcd1234" /* Proof of possession - change in production */

/* Timing */
#define ANIMATION_DEBOUNCE_MS 1000
#define SNTP_SYNC_TIMEOUT_MS 30000 /* Increased for hotspot latency */
#define SNTP_RETRY_COUNT 3         /* Retry SNTP if it fails */
#define WIFI_CONNECT_TIMEOUT_MS 5000
#define WIFI_RETRY_MAX 2

/* ============================================================================
 * EMBEDDED CERTIFICATES (from build)
 * ============================================================================ */
extern const uint8_t client_cert_pem_start[] asm("_binary_device_cert_pem_crt_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_device_cert_pem_crt_end");
extern const uint8_t client_key_pem_start[] asm("_binary_private_key_pem_key_start");
extern const uint8_t client_key_pem_end[] asm("_binary_private_key_pem_key_end");
extern const uint8_t server_cert_pem_start[] asm("_binary_amazon_root_ca_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_amazon_root_ca_pem_end");

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */
static const char *TAG = "MoneyBot";

/* Hardware handles */
static led_strip_handle_t led;
static lv_disp_t *disp;
static esp_mqtt_client_handle_t mqtt_client = NULL;

/* UI elements */
static lv_obj_t *pupils[2], *antenna_ball;
static lv_obj_t *mouth, *mouth_text, *grille_lines[3];
static lv_obj_t *tokens[NUM_TOKENS];
/* status_label reserved for future use */
static lv_obj_t *qr_canvas = NULL;
static lv_obj_t *main_screen = NULL;
static lv_obj_t *prov_screen = NULL;

/* Device identity */
static char device_id[32] = {0};
static char cmd_topic[64] = {0};

/* Event groups */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define PROV_END_BIT BIT2

/* Animation queue */
static QueueHandle_t animation_queue;
typedef struct
{
    int32_t amount;
    char currency[8];
    char event_id[64];
} sale_event_t;

/* Debounce */
static int64_t last_animation_time = 0;

/* Connection state */
typedef enum
{
    CONN_STATE_DISCONNECTED,
    CONN_STATE_WIFI_CONNECTING,
    CONN_STATE_WIFI_PROVISIONING,
    CONN_STATE_WIFI_CONNECTED,
    CONN_STATE_MQTT_CONNECTING,
    CONN_STATE_MQTT_CONNECTED
} conn_state_t;

static conn_state_t connection_state = CONN_STATE_DISCONNECTED;

/* ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================ */
static void init_display(void);
static void create_robot_face(lv_obj_t *scr);
static void create_tokens(lv_obj_t *scr);
static void trigger_sale_animation(const sale_event_t *event);
static void show_provisioning_screen(void);
static void show_main_screen(void);
static void update_connection_indicator(conn_state_t state);

/* ============================================================================
 * NVS & DEVICE IDENTITY
 * ============================================================================ */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void load_device_id(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK)
    {
        size_t len = sizeof(device_id);
        err = nvs_get_str(nvs, NVS_KEY_DEVICE_ID, device_id, &len);
        nvs_close(nvs);
    }

    if (err != ESP_OK || strlen(device_id) == 0)
    {
        strncpy(device_id, DEFAULT_DEVICE_ID, sizeof(device_id) - 1);
        ESP_LOGI(TAG, "Using default device ID: %s", device_id);
    }
    else
    {
        ESP_LOGI(TAG, "Loaded device ID from NVS: %s", device_id);
    }

    /* Build topic */
    snprintf(cmd_topic, sizeof(cmd_topic), "moneybot/%s/cmd", device_id);
    ESP_LOGI(TAG, "Subscribe topic: %s", cmd_topic);
}

static const char *get_device_id(void)
{
    return device_id;
}

/* ============================================================================
 * LED CONTROL
 * ============================================================================ */
static void set_led(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

/* ============================================================================
 * DISPLAY INITIALIZATION
 * ============================================================================ */
static void init_display(void)
{
    gpio_config_t bk_cfg = {.mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << LCD_BLK};
    gpio_config(&bk_cfg);
    gpio_set_level(LCD_BLK, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_RES * LCD_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40000000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_RES * 50,
        .double_buffer = true,
        .hres = LCD_RES,
        .vres = LCD_RES,
    };
    disp = lvgl_port_add_disp(&disp_cfg);
}

/* ============================================================================
 * ROBOT FACE UI
 * ============================================================================ */
static void create_robot_face(lv_obj_t *scr)
{
    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 200, 180);
    lv_obj_align(head, LV_ALIGN_CENTER, 0, 15);
    lv_obj_set_style_radius(head, 30, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(COL_ROBOT), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 4, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(COL_ACCENT), 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ant = lv_obj_create(scr);
    lv_obj_remove_style_all(ant);
    lv_obj_set_size(ant, 8, 30);
    lv_obj_align(ant, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_radius(ant, 4, 0);
    lv_obj_set_style_bg_color(ant, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(ant, LV_OPA_COVER, 0);

    antenna_ball = lv_obj_create(scr);
    lv_obj_remove_style_all(antenna_ball);
    lv_obj_set_size(antenna_ball, 16, 16);
    lv_obj_align(antenna_ball, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_radius(antenna_ball, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(antenna_ball, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_opa(antenna_ball, LV_OPA_COVER, 0);

    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *eye = lv_obj_create(head);
        lv_obj_remove_style_all(eye);
        lv_obj_set_size(eye, 55, 40);
        lv_obj_align(eye, i == 0 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT, i == 0 ? 20 : -20, 25);
        lv_obj_set_style_radius(eye, 8, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0x001515), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 3, 0);
        lv_obj_set_style_border_color(eye, lv_color_hex(0x333333), 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);

        pupils[i] = lv_obj_create(eye);
        lv_obj_remove_style_all(pupils[i]);
        lv_obj_set_size(pupils[i], 40, 26);
        lv_obj_center(pupils[i]);
        lv_obj_set_style_radius(pupils[i], 5, 0);
        lv_obj_set_style_bg_color(pupils[i], lv_color_hex(COL_CYAN), 0);
        lv_obj_set_style_bg_opa(pupils[i], LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(pupils[i], 15, 0);
        lv_obj_set_style_shadow_color(pupils[i], lv_color_hex(COL_CYAN), 0);
    }

    mouth = lv_obj_create(head);
    lv_obj_remove_style_all(mouth);
    lv_obj_set_size(mouth, 80, 35);
    lv_obj_align(mouth, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_set_style_radius(mouth, 8, 0);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mouth, 2, 0);
    lv_obj_set_style_border_color(mouth, lv_color_hex(0x444444), 0);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++)
    {
        grille_lines[i] = lv_obj_create(mouth);
        lv_obj_remove_style_all(grille_lines[i]);
        lv_obj_set_size(grille_lines[i], 60, 3);
        lv_obj_align(grille_lines[i], LV_ALIGN_TOP_MID, 0, 8 + i * 10);
        lv_obj_set_style_bg_color(grille_lines[i], lv_color_hex(0x111111), 0);
        lv_obj_set_style_bg_opa(grille_lines[i], LV_OPA_COVER, 0);
    }

    mouth_text = lv_label_create(mouth);
    lv_label_set_text(mouth_text, "CHA-CHING!");
    lv_obj_set_style_text_font(mouth_text, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(mouth_text, lv_color_hex(COL_GOLD), 0);
    lv_obj_center(mouth_text);
    lv_obj_add_flag(mouth_text, LV_OBJ_FLAG_HIDDEN);
}

static void create_tokens(lv_obj_t *scr)
{
    for (int i = 0; i < NUM_TOKENS; i++)
    {
        tokens[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(tokens[i]);
        lv_obj_set_size(tokens[i], 28, 28);
        lv_obj_set_style_radius(tokens[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(tokens[i], lv_color_hex(COL_GOLD), 0);
        lv_obj_set_style_bg_opa(tokens[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tokens[i], 3, 0);
        lv_obj_set_style_border_color(tokens[i], lv_color_hex(0xDAA520), 0);
        lv_obj_set_style_shadow_width(tokens[i], 8, 0);
        lv_obj_set_style_shadow_color(tokens[i], lv_color_hex(COL_GOLD), 0);
        lv_obj_clear_flag(tokens[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *dollar = lv_label_create(tokens[i]);
        lv_label_set_text(dollar, "$");
        lv_obj_set_style_text_font(dollar, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(dollar, lv_color_hex(COL_MONEY_GREEN), 0);
        lv_obj_center(dollar);

        lv_obj_set_pos(tokens[i], 20 + (i * TOKEN_SPACING), -35);
        lv_obj_add_flag(tokens[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================================
 * ANIMATION HELPERS
 * ============================================================================ */
static void anim_y_cb(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }
static void anim_opa_cb(void *var, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)var, v, 0); }

static void set_eye_color(uint32_t color)
{
    lv_color_t c = lv_color_hex(color);
    for (int i = 0; i < 2; i++)
    {
        lv_obj_set_style_bg_color(pupils[i], c, 0);
        lv_obj_set_style_shadow_color(pupils[i], c, 0);
    }
    lv_obj_set_style_bg_color(antenna_ball, c, 0);
}

static void open_mouth(void)
{
    lv_obj_set_size(mouth, 130, 45);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(mouth, 3, 0);
    lv_obj_set_style_border_color(mouth, lv_color_hex(COL_GOLD), 0);
    for (int i = 0; i < 3; i++)
        lv_obj_add_flag(grille_lines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(mouth_text, LV_OBJ_FLAG_HIDDEN);
}

static void close_mouth(void)
{
    lv_obj_set_size(mouth, 80, 35);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(mouth, 2, 0);
    lv_obj_set_style_border_color(mouth, lv_color_hex(0x444444), 0);
    for (int i = 0; i < 3; i++)
        lv_obj_clear_flag(grille_lines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_text, LV_OBJ_FLAG_HIDDEN);
}

static void start_rain(void)
{
    for (int i = 0; i < NUM_TOKENS; i++)
    {
        lv_obj_clear_flag(tokens[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(tokens[i], LV_OPA_COVER, 0);
        int x = 20 + (i * TOKEN_SPACING) + (rand() % 10) - 5;
        int y_start = -30 - (rand() % 20);
        lv_obj_set_pos(tokens[i], x, y_start);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, tokens[i]);
        lv_anim_set_values(&a, y_start, 260);
        lv_anim_set_time(&a, RAIN_TIME_MS + (rand() % 300));
        lv_anim_set_delay(&a, (i % 3) * 100);
        lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_start(&a);

        lv_anim_t f;
        lv_anim_init(&f);
        lv_anim_set_var(&f, tokens[i]);
        lv_anim_set_values(&f, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&f, 400);
        lv_anim_set_delay(&f, ((i % 3) * 100) + RAIN_TIME_MS - 300);
        lv_anim_set_exec_cb(&f, anim_opa_cb);
        lv_anim_start(&f);
    }
}

static void hide_tokens(void)
{
    for (int i = 0; i < NUM_TOKENS; i++)
        lv_obj_add_flag(tokens[i], LV_OBJ_FLAG_HIDDEN);
}

/* ============================================================================
 * SALE ANIMATION TRIGGER
 * ============================================================================ */
static void trigger_sale_animation(const sale_event_t *event)
{
    if (event)
    {
        ESP_LOGI(TAG, "💰 SALE! Amount: %ld %s, Event: %s",
                 (long)event->amount, event->currency, event->event_id);
    }
    else
    {
        ESP_LOGI(TAG, "💰 SALE! (unknown amount)");
    }

    /* Phase 1: Celebrate - Gold eyes, open mouth, rain tokens */
    set_led(255, 180, 0);
    lvgl_port_lock(0);
    set_eye_color(COL_GOLD);
    open_mouth();
    start_rain();
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(2200));

    /* Phase 2: Success - Green eyes, close mouth */
    set_led(0, 255, 0);
    lvgl_port_lock(0);
    set_eye_color(COL_GREEN);
    close_mouth();
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Phase 3: Return to idle */
    lvgl_port_lock(0);
    hide_tokens();
    set_eye_color(COL_CYAN);
    lvgl_port_unlock();

    /* Restore connection-appropriate LED color */
    if (connection_state == CONN_STATE_MQTT_CONNECTED)
    {
        set_led(0, 50, 0); /* Dim green = fully connected */
    }
    else
    {
        set_led(0, 50, 50); /* Cyan = idle/partial */
    }
}

/* ============================================================================
 * CONNECTION STATUS INDICATOR
 * ============================================================================ */
static void update_connection_indicator(conn_state_t state)
{
    connection_state = state;
    uint32_t color;

    switch (state)
    {
    case CONN_STATE_MQTT_CONNECTED:
        color = COL_GREEN;
        set_led(0, 50, 0);
        break;
    case CONN_STATE_WIFI_CONNECTED:
    case CONN_STATE_MQTT_CONNECTING:
        color = COL_CYAN;
        set_led(0, 50, 50);
        break;
    case CONN_STATE_WIFI_PROVISIONING:
        color = COL_GOLD;
        set_led(50, 40, 0);
        break;
    case CONN_STATE_DISCONNECTED:
    case CONN_STATE_WIFI_CONNECTING:
    default:
        color = COL_RED;
        set_led(50, 0, 0);
        break;
    }

    if (antenna_ball)
    {
        lvgl_port_lock(0);
        lv_obj_set_style_bg_color(antenna_ball, lv_color_hex(color), 0);
        lvgl_port_unlock();
    }
}

/* ============================================================================
 * QR CODE / PROVISIONING SCREEN
 * ============================================================================ */

/* Captive portal HTTP server handle */
static httpd_handle_t captive_httpd = NULL;
static char captive_ssid[32] = {0}; /* SoftAP SSID for QR code */

/* QR code rendering context for callback */
static struct
{
    lv_obj_t *canvas;
    lv_obj_t *screen;
} qr_render_ctx;

/* Custom display function for QR code rendering to LVGL canvas */
static void qr_display_to_canvas(esp_qrcode_handle_t qrcode)
{
    int qr_size = esp_qrcode_get_size(qrcode);
    int module_px = 3; /* pixels per QR module */
    int canvas_size = 120;
    int margin = (canvas_size - (qr_size * module_px)) / 2;

    if (margin < 2)
    {
        module_px = 2;
        margin = (canvas_size - (qr_size * module_px)) / 2;
    }

    /* Draw QR modules */
    for (int y = 0; y < qr_size; y++)
    {
        for (int x = 0; x < qr_size; x++)
        {
            lv_color_t color = esp_qrcode_get_module(qrcode, x, y) ? lv_color_black() : lv_color_white();
            for (int dy = 0; dy < module_px; dy++)
            {
                for (int dx = 0; dx < module_px; dx++)
                {
                    int px = margin + x * module_px + dx;
                    int py = margin + y * module_px + dy;
                    if (px < canvas_size && py < canvas_size)
                    {
                        lv_canvas_set_px_color(qr_render_ctx.canvas, px, py, color);
                    }
                }
            }
        }
    }
}

static void show_provisioning_screen(void)
{
    lvgl_port_lock(0);

    if (prov_screen == NULL)
    {
        prov_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(prov_screen, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(prov_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(prov_screen, LV_OBJ_FLAG_SCROLLABLE);

        /* Title */
        lv_obj_t *title = lv_label_create(prov_screen);
        lv_label_set_text(title, "Setup WiFi");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(COL_CYAN), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        /*
         * QR code uses standard WiFi format that phones natively support.
         * When scanned, phone will auto-connect to the device's SoftAP.
         * Format: WIFI:T:nopass;S:<SSID>;P:;;
         */
        char qr_payload[100];
        snprintf(qr_payload, sizeof(qr_payload), "WIFI:T:nopass;S:%s;P:;;", captive_ssid);

        /* Create canvas for QR code */
        static lv_color_t cbuf[120 * 120];
        qr_canvas = lv_canvas_create(prov_screen);
        lv_canvas_set_buffer(qr_canvas, cbuf, 120, 120, LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(qr_canvas, lv_color_white(), LV_OPA_COVER);
        lv_obj_align(qr_canvas, LV_ALIGN_CENTER, 0, -8);

        /* Store canvas in render context for callback */
        qr_render_ctx.canvas = qr_canvas;
        qr_render_ctx.screen = prov_screen;

        /* Generate QR code using ESP-IDF API */
        esp_qrcode_config_t qr_cfg = {
            .display_func = qr_display_to_canvas,
            .max_qrcode_version = 10,
            .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
        };

        /* Unlock before QR generation (it may take time) */
        lvgl_port_unlock();
        esp_err_t ret = esp_qrcode_generate(&qr_cfg, qr_payload);
        lvgl_port_lock(0);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to generate QR code: %s", esp_err_to_name(ret));
            /* Show error text instead */
            lv_obj_t *err_label = lv_label_create(prov_screen);
            lv_label_set_text(err_label, "QR Error");
            lv_obj_set_style_text_color(err_label, lv_color_hex(COL_RED), 0);
            lv_obj_align(err_label, LV_ALIGN_CENTER, 0, 0);
        }

        /* Instructions with URL */
        lv_obj_t *instr = lv_label_create(prov_screen);
        lv_label_set_text(instr, "Scan, then visit:\n192.168.4.1");
        lv_obj_set_style_text_font(instr, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(instr, LV_ALIGN_BOTTOM_MID, 0, -10);

        /* Also print to console for debugging */
        ESP_LOGI(TAG, "WiFi QR: %s", qr_payload);
        ESP_LOGI(TAG, "After connecting, visit http://192.168.4.1");
    }

    lv_disp_load_scr(prov_screen);
    lvgl_port_unlock();
}

static void show_main_screen(void)
{
    lvgl_port_lock(0);

    if (main_screen == NULL)
    {
        main_screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(main_screen, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
        create_robot_face(main_screen);
        create_tokens(main_screen);
    }

    lv_disp_load_scr(main_screen);
    lvgl_port_unlock();
}

/* ============================================================================
 * WI-FI EVENT HANDLERS
 * ============================================================================ */
static int wifi_retry_count = 0;
static bool provisioning_mode = false; /* When true, don't attempt STA connections */
static bool scanning_mode = false;     /* When true, don't auto-connect on STA_START */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            if (!provisioning_mode && !scanning_mode)
            {
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "WiFi disconnected (reason: %d)", event->reason);
            update_connection_indicator(CONN_STATE_DISCONNECTED);
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

            if (!provisioning_mode)
            {
                wifi_retry_count++;
                if (wifi_retry_count < WIFI_RETRY_MAX)
                {
                    ESP_LOGI(TAG, "Retry %d/%d...", wifi_retry_count, WIFI_RETRY_MAX);
                    esp_wifi_connect();
                }
                else
                {
                    ESP_LOGW(TAG, "Max retries reached, signaling failure");
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Station connected to SoftAP (MAC: " MACSTR ")", MAC2STR(event->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "Station disconnected from SoftAP");
            break;
        default:
            break;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        update_connection_indicator(CONN_STATE_WIFI_CONNECTED);
    }
}

/* ============================================================================
 * CAPTIVE PORTAL HTML
 * ============================================================================ */
static const char captive_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MoneyBot WiFi Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#fff;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;}"
    ".card{background:#252545;padding:2rem;border-radius:1rem;width:90%;max-width:320px;box-shadow:0 4px 20px rgba(0,0,0,0.3);}"
    "h1{color:#00ffff;font-size:1.4rem;margin:0 0 1.5rem;text-align:center;}"
    "label{display:block;margin:0.5rem 0 0.25rem;color:#aaa;font-size:0.9rem;}"
    "input{width:100%;padding:0.75rem;border:1px solid #444;border-radius:0.5rem;"
    "background:#1a1a2e;color:#fff;font-size:1rem;box-sizing:border-box;}"
    "input:focus{outline:none;border-color:#00ffff;}"
    "button{width:100%;padding:0.875rem;margin-top:1.5rem;border:none;border-radius:0.5rem;"
    "background:linear-gradient(135deg,#00ffff,#00cc99);color:#1a1a2e;font-size:1rem;"
    "font-weight:600;cursor:pointer;}"
    "button:active{transform:scale(0.98);}"
    ".info{text-align:center;color:#666;font-size:0.8rem;margin-top:1rem;}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>🤖 MoneyBot WiFi</h1>"
    "<form action='/save' method='POST'>"
    "<label>WiFi Network</label><input name='ssid' required autocomplete='off' placeholder='Enter SSID'>"
    "<label>Password</label><input name='pass' type='password' placeholder='Enter password'>"
    "<button type='submit'>Connect</button>"
    "</form>"
    "<p class='info'>Device will restart after saving</p>"
    "</div></body></html>";

static const char success_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Success</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#fff;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0;text-align:center;}"
    ".card{background:#252545;padding:2rem;border-radius:1rem;}"
    "h1{color:#00ff00;font-size:2rem;margin:0 0 1rem;}"
    "p{color:#aaa;}"
    "</style></head><body>"
    "<div class='card'><h1>✓ Saved!</h1><p>MoneyBot is restarting...</p></div>"
    "</body></html>";

/* ============================================================================
 * CAPTIVE PORTAL HTTP HANDLERS
 * ============================================================================ */
static esp_err_t captive_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, captive_html, strlen(captive_html));
    return ESP_OK;
}

/* Handler for captive portal detection endpoints */
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    /* Most devices check specific URLs for captive portal detection.
     * Redirect them to our config page. */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* URL decode helper - decodes %XX and + to space */
/* Encode a Unicode code point to UTF-8, returns number of bytes written */
static int encode_utf8(uint32_t codepoint, char *out)
{
    if (codepoint < 0x80)
    {
        out[0] = (char)codepoint;
        return 1;
    }
    else if (codepoint < 0x800)
    {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    else if (codepoint < 0x10000)
    {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    else if (codepoint < 0x110000)
    {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

/* Pass 1: URL decode %XX and + to space */
static void url_decode_pass1(char *dst, const char *src, size_t dst_size)
{
    char *d = dst;
    const char *s = src;
    size_t remaining = dst_size - 1;

    while (*s && remaining > 0)
    {
        if (*s == '+')
        {
            *d++ = ' ';
            s++;
            remaining--;
        }
        else if (*s == '%' && s[1] && s[2])
        {
            char hex[3] = {s[1], s[2], '\0'};
            *d++ = (char)strtol(hex, NULL, 16);
            s += 3;
            remaining--;
        }
        else
        {
            *d++ = *s++;
            remaining--;
        }
    }
    *d = '\0';
}

/* Pass 2: Decode HTML entities &#NNNNN; to UTF-8 */
static void html_entity_decode(char *dst, const char *src, size_t dst_size)
{
    char *d = dst;
    const char *s = src;
    size_t remaining = dst_size - 1;

    while (*s && remaining > 0)
    {
        if (*s == '&' && s[1] == '#')
        {
            /* HTML numeric entity: &#NNNNN; or &#xHHHH; */
            const char *start = s + 2;
            uint32_t codepoint = 0;
            const char *end = start;

            if (*start == 'x' || *start == 'X')
            {
                /* Hex entity: &#xHHHH; */
                end = start + 1;
                while ((*end >= '0' && *end <= '9') ||
                       (*end >= 'a' && *end <= 'f') ||
                       (*end >= 'A' && *end <= 'F'))
                {
                    end++;
                }
                if (end > start + 1)
                {
                    codepoint = strtoul(start + 1, NULL, 16);
                }
            }
            else
            {
                /* Decimal entity: &#NNNNN; */
                while (*end >= '0' && *end <= '9')
                {
                    end++;
                }
                if (end > start)
                {
                    codepoint = strtoul(start, NULL, 10);
                }
            }

            if (codepoint > 0 && *end == ';')
            {
                /* Valid entity, convert to UTF-8 */
                char utf8[4];
                int len = encode_utf8(codepoint, utf8);
                if (len > 0 && (size_t)len <= remaining)
                {
                    memcpy(d, utf8, len);
                    d += len;
                    remaining -= len;
                    s = end + 1; /* Skip past the semicolon */
                    continue;
                }
            }
            /* Invalid entity, copy as-is */
            *d++ = *s++;
            remaining--;
        }
        else
        {
            *d++ = *s++;
            remaining--;
        }
    }
    *d = '\0';
}

/* Full decode: URL decode then HTML entity decode */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    char temp[256];
    url_decode_pass1(temp, src, sizeof(temp));
    html_entity_decode(dst, temp, dst_size);
}

static esp_err_t captive_save_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    ESP_LOGI(TAG, "Received form data: %s", buf);

    /* Parse form data: ssid=xxx&pass=xxx */
    char ssid_raw[64] = {0};
    char pass_raw[64] = {0};
    char ssid[64] = {0};
    char pass[64] = {0};

    char *ssid_start = strstr(buf, "ssid=");
    char *pass_start = strstr(buf, "pass=");

    if (ssid_start)
    {
        ssid_start += 5;
        char *ssid_end = strchr(ssid_start, '&');
        if (ssid_end)
        {
            size_t len = ssid_end - ssid_start;
            if (len >= sizeof(ssid_raw))
                len = sizeof(ssid_raw) - 1;
            memcpy(ssid_raw, ssid_start, len);
        }
        else
        {
            strncpy(ssid_raw, ssid_start, sizeof(ssid_raw) - 1);
        }
    }

    if (pass_start)
    {
        pass_start += 5;
        char *pass_end = strchr(pass_start, '&');
        if (pass_end)
        {
            size_t len = pass_end - pass_start;
            if (len >= sizeof(pass_raw))
                len = sizeof(pass_raw) - 1;
            memcpy(pass_raw, pass_start, len);
        }
        else
        {
            strncpy(pass_raw, pass_start, sizeof(pass_raw) - 1);
        }
    }

    /* URL decode */
    url_decode(ssid, ssid_raw, sizeof(ssid));
    url_decode(pass, pass_raw, sizeof(pass));

    ESP_LOGI(TAG, "Received WiFi credentials - SSID: %s, Pass length: %d", ssid, (int)strlen(pass));

    /* Save to NVS */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        nvs_set_str(nvs, "wifi_ssid", ssid);
        nvs_set_str(nvs, "wifi_pass", pass);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
    }

    /* Send success page */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, success_html, strlen(success_html));

    /* Restart after a short delay to let response be sent */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* Start captive portal HTTP server */
static void start_captive_portal(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.stack_size = 8192;

    if (httpd_start(&captive_httpd, &config) == ESP_OK)
    {
        /* Main config page */
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = captive_root_handler,
        };
        httpd_register_uri_handler(captive_httpd, &root);

        /* Form submission */
        httpd_uri_t save = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = captive_save_handler,
        };
        httpd_register_uri_handler(captive_httpd, &save);

        /* Captive portal detection endpoints - redirect to config */
        const char *detect_uris[] = {
            "/generate_204",              /* Android */
            "/gen_204",                   /* Android alternate */
            "/hotspot-detect.html",       /* Apple */
            "/library/test/success.html", /* Apple */
            "/ncsi.txt",                  /* Windows */
            "/connecttest.txt",           /* Windows */
            "/redirect",                  /* Generic */
            "/canonical.html",            /* Firefox */
        };

        for (int i = 0; i < sizeof(detect_uris) / sizeof(detect_uris[0]); i++)
        {
            httpd_uri_t detect = {
                .uri = detect_uris[i],
                .method = HTTP_GET,
                .handler = captive_redirect_handler,
            };
            httpd_register_uri_handler(captive_httpd, &detect);
        }

        ESP_LOGI(TAG, "Captive portal HTTP server started");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

static void stop_captive_portal(void)
{
    if (captive_httpd)
    {
        httpd_stop(captive_httpd);
        captive_httpd = NULL;
        ESP_LOGI(TAG, "Captive portal HTTP server stopped");
    }
}

/* ============================================================================
 * WI-FI PROVISIONING (CAPTIVE PORTAL)
 * ============================================================================ */
static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
}

static void start_provisioning(void)
{
    ESP_LOGI(TAG, "Starting WiFi provisioning (captive portal)...");

    /* Set provisioning mode to prevent STA connection attempts */
    provisioning_mode = true;
    wifi_retry_count = 0;

    update_connection_indicator(CONN_STATE_WIFI_PROVISIONING);

    /* Generate SoftAP SSID from MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(captive_ssid, sizeof(captive_ssid), "%s%02X%02X",
             PROV_SERVICE_NAME_PREFIX, mac[4], mac[5]);

    /* Configure SoftAP */
    wifi_config_t ap_config = {
        .ap = {
            .ssid_len = strlen(captive_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(ap_config.ap.ssid, captive_ssid, strlen(captive_ssid));

    /* Use pure AP mode for provisioning - no STA connection attempts */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Start DNS server for captive portal detection */
    dns_server_start();

    /* Start HTTP server for configuration page */
    start_captive_portal();

    /* Show QR code screen */
    show_provisioning_screen();

    ESP_LOGI(TAG, "Captive portal active - SSID: %s", captive_ssid);
    ESP_LOGI(TAG, "Connect and visit http://192.168.4.1 to configure WiFi");
}

/* Check if we have stored WiFi credentials */
static bool has_stored_credentials(char *ssid_out, size_t ssid_len, char *pass_out, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "No NVS namespace found");
        return false;
    }

    size_t len = ssid_len;
    err = nvs_get_str(nvs, "wifi_ssid", ssid_out, &len);
    if (err != ESP_OK || len == 0)
    {
        ESP_LOGI(TAG, "No stored SSID found");
        nvs_close(nvs);
        return false;
    }

    len = pass_len;
    err = nvs_get_str(nvs, "wifi_pass", pass_out, &len);
    if (err != ESP_OK)
    {
        /* Password might be empty for open networks */
        ESP_LOGI(TAG, "No stored password found (open network?)");
        pass_out[0] = '\0';
    }

    ESP_LOGI(TAG, "Loaded credentials - SSID: %s, Pass length: %d", ssid_out, (int)strlen(pass_out));
    nvs_close(nvs);
    return strlen(ssid_out) > 0;
}

/* Scan and print all visible WiFi networks for debugging */
static void wifi_scan_networks(const char *target_ssid)
{
    ESP_LOGI(TAG, "=== Scanning for WiFi networks ===");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %d networks:", ap_count);

    if (ap_count == 0)
    {
        ESP_LOGW(TAG, "No networks found! Check antenna/location.");
        return;
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    bool target_found = false;
    for (int i = 0; i < ap_count; i++)
    {
        const char *band = (ap_list[i].primary <= 14) ? "2.4GHz" : "5GHz";
        ESP_LOGI(TAG, "  [%d] SSID: %-32s | Ch: %2d (%s) | RSSI: %d dBm",
                 i + 1, ap_list[i].ssid, ap_list[i].primary, band, ap_list[i].rssi);

        if (target_ssid && strcmp((char *)ap_list[i].ssid, target_ssid) == 0)
        {
            target_found = true;
            ESP_LOGI(TAG, "  >>> TARGET NETWORK FOUND! <<<");
        }
    }

    if (target_ssid && !target_found)
    {
        ESP_LOGW(TAG, "Target network '%s' NOT FOUND in scan!", target_ssid);
        ESP_LOGW(TAG, "Possible reasons: 5GHz only, out of range, or hidden SSID");
    }

    free(ap_list);
    ESP_LOGI(TAG, "=== End of WiFi scan ===");
}

static bool wifi_connect(void)
{
    char stored_ssid[64] = {0};
    char stored_pass[64] = {0};

    wifi_init();

    /* Check if we have stored credentials */
    if (has_stored_credentials(stored_ssid, sizeof(stored_ssid), stored_pass, sizeof(stored_pass)))
    {
        ESP_LOGI(TAG, "Found stored credentials, connecting to: %s", stored_ssid);
        update_connection_indicator(CONN_STATE_WIFI_CONNECTING);
        show_main_screen();

        /* Start WiFi in STA mode first to do a scan */
        scanning_mode = true; /* Prevent auto-connect on STA_START */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* Scan to see what networks are available */
        wifi_scan_networks(stored_ssid);
        scanning_mode = false; /* Allow connections now */

        wifi_config_t sta_config = {0};
        strncpy((char *)sta_config.sta.ssid, stored_ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, stored_pass, sizeof(sta_config.sta.password) - 1);

        wifi_retry_count = 0;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        esp_wifi_connect();

        /* Wait for connection with shorter timeout */
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT)
        {
            return true;
        }

        /* Connection failed, stop WiFi and fall through to provisioning */
        ESP_LOGW(TAG, "Stored WiFi connection failed, starting provisioning...");
        esp_wifi_stop();
    }
    else
    {
        ESP_LOGI(TAG, "No stored credentials, starting provisioning immediately...");
    }

    /* Start captive portal provisioning */
    start_provisioning();

    /* Wait forever for user to configure WiFi (device will restart after config) */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return false;
}

/* ============================================================================
 * SNTP TIME SYNCHRONIZATION
 * ============================================================================ */
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized!");
}

static bool obtain_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.cloudflare.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    /* Wait for time to be set with extended timeout */
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int max_retries = SNTP_SYNC_TIMEOUT_MS / 500;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < max_retries)
    {
        if (retry % 10 == 0)
        {
            ESP_LOGI(TAG, "Waiting for SNTP sync... (%d/%d)", retry, max_retries);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year < (2016 - 1900))
    {
        ESP_LOGE(TAG, "SNTP sync timeout!");
        return false;
    }

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s", strftime_buf);

    return true;
}

/* ============================================================================
 * MQTT MESSAGE HANDLING
 * ============================================================================ */
static void handle_mqtt_message(const char *data, int data_len)
{
    /* Debounce check */
    int64_t now = esp_timer_get_time() / 1000; /* Convert to ms */
    if (now - last_animation_time < ANIMATION_DEBOUNCE_MS)
    {
        ESP_LOGW(TAG, "Debouncing: ignoring message within %d ms", ANIMATION_DEBOUNCE_MS);
        return;
    }

    /* Copy to null-terminated buffer */
    char *json_str = malloc(data_len + 1);
    if (!json_str)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON");
        return;
    }
    memcpy(json_str, data, data_len);
    json_str[data_len] = '\0';

    ESP_LOGI(TAG, "Received message: %s", json_str);

    sale_event_t event = {0};
    bool trigger = false;

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_str);
    if (root)
    {
        cJSON *type = cJSON_GetObjectItem(root, "type");
        cJSON *status = cJSON_GetObjectItem(root, "status");
        cJSON *amount = cJSON_GetObjectItem(root, "amount");
        cJSON *currency = cJSON_GetObjectItem(root, "currency");
        cJSON *event_id = cJSON_GetObjectItem(root, "eventId");

        /* Check for sale with succeeded status */
        if (cJSON_IsString(type) && strcmp(type->valuestring, "sale") == 0)
        {
            if (!status || !cJSON_IsString(status) ||
                strcmp(status->valuestring, "succeeded") == 0)
            {
                trigger = true;

                if (cJSON_IsNumber(amount))
                {
                    event.amount = (int32_t)amount->valuedouble;
                }
                if (cJSON_IsString(currency))
                {
                    strncpy(event.currency, currency->valuestring, sizeof(event.currency) - 1);
                }
                if (cJSON_IsString(event_id))
                {
                    strncpy(event.event_id, event_id->valuestring, sizeof(event.event_id) - 1);
                }
            }
        }

        cJSON_Delete(root);
    }
    else
    {
        ESP_LOGW(TAG, "JSON parse failed, triggering animation anyway (MVP tolerance)");
        trigger = true; /* MVP: trigger even on parse failure */
    }

    free(json_str);

    if (trigger)
    {
        last_animation_time = now;
        /* Queue animation event */
        xQueueSend(animation_queue, &event, 0);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to AWS IoT Core");
        update_connection_indicator(CONN_STATE_MQTT_CONNECTED);

        /* Subscribe to command topic */
        int msg_id = esp_mqtt_client_subscribe(mqtt_client, cmd_topic, 0);
        ESP_LOGI(TAG, "Subscribing to %s, msg_id=%d", cmd_topic, msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        update_connection_indicator(CONN_STATE_WIFI_CONNECTED);
        /* Auto-reconnect is handled by esp-mqtt */
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data received on topic: %.*s", event->topic_len, event->topic);
        handle_mqtt_message(event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type: %d", event->error_handle->error_type);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGE(TAG, "TCP transport error - errno: %d (%s)",
                     event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGE(TAG, "Connection refused, code: %d", event->error_handle->connect_return_code);
        }
        if (event->error_handle->esp_tls_last_esp_err != 0)
        {
            ESP_LOGE(TAG, "TLS error: 0x%x", event->error_handle->esp_tls_last_esp_err);
        }
        if (event->error_handle->esp_tls_stack_err != 0)
        {
            ESP_LOGE(TAG, "TLS stack error: 0x%x", event->error_handle->esp_tls_stack_err);
        }
        ESP_LOGE(TAG, "Check: 1) AWS IoT Thing exists 2) Cert attached 3) Policy allows connect");
        break;

    default:
        ESP_LOGD(TAG, "MQTT event: %d", (int)event_id);
        break;
    }
}

static void mqtt_start(void)
{
    ESP_LOGI(TAG, "Starting MQTT client...");
    ESP_LOGI(TAG, "  Endpoint: %s", AWS_IOT_ENDPOINT);
    ESP_LOGI(TAG, "  Client ID: %s", device_id);

    /* Verify certificates are embedded */
    size_t cert_len = client_cert_pem_end - client_cert_pem_start;
    size_t key_len = client_key_pem_end - client_key_pem_start;
    size_t ca_len = server_cert_pem_end - server_cert_pem_start;
    ESP_LOGI(TAG, "  Device cert: %d bytes", (int)cert_len);
    ESP_LOGI(TAG, "  Private key: %d bytes", (int)key_len);
    ESP_LOGI(TAG, "  Root CA: %d bytes", (int)ca_len);

    if (cert_len < 100 || key_len < 100 || ca_len < 100)
    {
        ESP_LOGE(TAG, "Certificate files appear to be missing or too small!");
        ESP_LOGE(TAG, "Check that certs/ folder contains valid PEM files.");
        return;
    }

    update_connection_indicator(CONN_STATE_MQTT_CONNECTING);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
            .verification = {
                .certificate = (const char *)server_cert_pem_start,
            },
        },
        .credentials = {
            .client_id = device_id,
            .authentication = {
                .certificate = (const char *)client_cert_pem_start,
                .key = (const char *)client_key_pem_start,
            },
        },
        .session = {
            .keepalive = 60,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
        },
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!mqtt_client)
    {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

/* ============================================================================
 * ANIMATION TASK
 * ============================================================================ */
static void animation_task(void *pvParameters)
{
    sale_event_t event;

    while (1)
    {
        if (xQueueReceive(animation_queue, &event, portMAX_DELAY) == pdTRUE)
        {
            trigger_sale_animation(&event);
        }
    }
}

/* ============================================================================
 * MAIN APPLICATION
 * ============================================================================ */
void app_main(void)
{
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "  MoneyBot Starting");
    ESP_LOGI(TAG, "  AWS IoT MQTT Edition");
    ESP_LOGI(TAG, "======================================");

    /* Initialize NVS */
    init_nvs();

    /* Load device identity */
    load_device_id();

    /* Create animation queue */
    animation_queue = xQueueCreate(5, sizeof(sale_event_t));

    /* Initialize LED */
    led_strip_config_t led_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_cfg, &rmt_cfg, &led));
    led_strip_clear(led);
    set_led(50, 0, 0); /* Red = starting up */

    /* Initialize display */
    init_display();

    /* Show main screen initially (will switch to provisioning if needed) */
    show_main_screen();
    update_connection_indicator(CONN_STATE_DISCONNECTED);

    /* Connect to WiFi (with provisioning if needed) */
    if (!wifi_connect())
    {
        ESP_LOGE(TAG, "Failed to connect to WiFi!");
        /* Continue anyway - will keep trying to reconnect */
    }

    /* Sync time via SNTP (required for TLS) - retry until success */
    int sntp_attempts = 0;
    while (!obtain_time() && sntp_attempts < SNTP_RETRY_COUNT)
    {
        sntp_attempts++;
        ESP_LOGW(TAG, "Time sync failed, retrying... (%d/%d)", sntp_attempts, SNTP_RETRY_COUNT);
        esp_sntp_stop();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (sntp_attempts >= SNTP_RETRY_COUNT)
    {
        ESP_LOGE(TAG, "Time sync failed after %d attempts - TLS may not work!", SNTP_RETRY_COUNT);
    }

    /* Start MQTT client */
    mqtt_start();

    /* Create animation task (runs independently from LVGL) */
    xTaskCreate(animation_task, "animation", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Setup complete! Waiting for MQTT messages...");
    ESP_LOGI(TAG, "Device ID: %s", get_device_id());
    ESP_LOGI(TAG, "Subscribed topic: %s", cmd_topic);

    /* Main loop - just handle idle state */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
