/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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

#define LED_GPIO        38
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SCLK        12
#define LCD_MOSI        11
#define LCD_CS          10
#define LCD_DC          9
#define LCD_RST         8
#define LCD_BLK         7
#define LCD_RES         240

#define NUM_TOKENS      10
#define TOKEN_SPACING   (200 / (NUM_TOKENS - 1))
#define RAIN_TIME_MS    1800

#define COL_BG          0x1A1A2E
#define COL_ROBOT       0x4A4A4A
#define COL_ACCENT      0x6A6A6A
#define COL_CYAN        0x00FFFF
#define COL_GREEN       0x00FF00
#define COL_GOLD        0xFFD700
#define COL_MONEY_GREEN 0x228B22

static const char *TAG = "MoneyBot";
static led_strip_handle_t led;
static lv_disp_t *disp;
static lv_obj_t *pupils[2], *antenna_ball;
static lv_obj_t *mouth, *mouth_text, *grille_lines[3];
static lv_obj_t *tokens[NUM_TOKENS];

static void init_display(void)
{
    gpio_config_t bk_cfg = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << LCD_BLK };
    gpio_config(&bk_cfg);
    gpio_set_level(LCD_BLK, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCLK, .mosi_io_num = LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_RES * LCD_RES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC, .cs_gpio_num = LCD_CS, .pclk_hz = 40000000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
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
        .io_handle = io, .panel_handle = panel,
        .buffer_size = LCD_RES * 50, .double_buffer = true,
        .hres = LCD_RES, .vres = LCD_RES,
    };
    disp = lvgl_port_add_disp(&disp_cfg);
}

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

    for (int i = 0; i < 2; i++) {
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

    for (int i = 0; i < 3; i++) {
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
    for (int i = 0; i < NUM_TOKENS; i++) {
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

static void anim_y_cb(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }
static void anim_opa_cb(void *var, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)var, v, 0); }

static void set_eye_color(uint32_t color)
{
    lv_color_t c = lv_color_hex(color);
    for (int i = 0; i < 2; i++) {
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
    for (int i = 0; i < 3; i++) lv_obj_add_flag(grille_lines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(mouth_text, LV_OBJ_FLAG_HIDDEN);
}

static void close_mouth(void)
{
    lv_obj_set_size(mouth, 80, 35);
    lv_obj_set_style_bg_color(mouth, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(mouth, 2, 0);
    lv_obj_set_style_border_color(mouth, lv_color_hex(0x444444), 0);
    for (int i = 0; i < 3; i++) lv_obj_clear_flag(grille_lines[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mouth_text, LV_OBJ_FLAG_HIDDEN);
}

static void start_rain(void)
{
    for (int i = 0; i < NUM_TOKENS; i++) {
        lv_obj_clear_flag(tokens[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(tokens[i], LV_OPA_COVER, 0);
        int x = 20 + (i * TOKEN_SPACING) + (rand() % 10) - 5;
        int y_start = -30 - (rand() % 20);
        lv_obj_set_pos(tokens[i], x, y_start);

        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, tokens[i]);
        lv_anim_set_values(&a, y_start, 260);
        lv_anim_set_time(&a, RAIN_TIME_MS + (rand() % 300));
        lv_anim_set_delay(&a, (i % 3) * 100);
        lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_start(&a);

        lv_anim_t f; lv_anim_init(&f);
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
    for (int i = 0; i < NUM_TOKENS; i++) lv_obj_add_flag(tokens[i], LV_OBJ_FLAG_HIDDEN);
}

static void set_led(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

void app_main(void)
{
    ESP_LOGI(TAG, "MoneyBot Starting");

    led_strip_config_t led_cfg = {
        .strip_gpio_num = LED_GPIO, .max_leds = 1,
        .led_model = LED_MODEL_WS2812, .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = 64, .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&led_cfg, &rmt_cfg, &led));
    led_strip_clear(led);

    init_display();

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    create_robot_face(scr);
    create_tokens(scr);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Setup complete!");

    while (1) {
        set_led(0, 50, 50);
        lvgl_port_lock(0);
        hide_tokens(); set_eye_color(COL_CYAN); close_mouth();
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(2000));

        set_led(255, 180, 0);
        lvgl_port_lock(0);
        set_eye_color(COL_GOLD); open_mouth(); start_rain();
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(2200));

        set_led(0, 255, 0);
        lvgl_port_lock(0);
        set_eye_color(COL_GREEN); close_mouth();
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
