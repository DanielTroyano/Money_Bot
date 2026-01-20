/* FreeRTOS - Real-Time Operating System: task management, delays, and multitasking capabilities */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF Logging - Allows us to print messages to the serial monitor w/ timestamps and log levels */
#include "esp_log.h"

/* LED Strip Driver - Handles communication with addressable RGB LEDs */
#include "led_strip.h"

/* LCD and Graphics Libraries */
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

/* LVGL - Graphics library for embedded displays */
#include "lvgl.h"
#include "esp_lvgl_port.h"

/* ============================================================================
 * CONFIGURATION - Pin Definitions
 * ============================================================================ */

/* RGB LED Configuration (v1.1 board uses GPIO38) */
#define LED_STRIP_GPIO      38
#define LED_STRIP_LED_COUNT 1

/* LCD SPI Pin Configuration */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIN_SCLK        12      /* SCL - SPI Clock */
#define LCD_PIN_MOSI        11      /* SDA - SPI Data */
#define LCD_PIN_CS          10      /* Chip Select */
#define LCD_PIN_DC          9       /* Data/Command */
#define LCD_PIN_RST         8       /* Reset */
#define LCD_PIN_BLK         7       /* Backlight (optional, using 3V3 direct) */

/* LCD Display Settings */
#define LCD_H_RES           240     /* Horizontal resolution */
#define LCD_V_RES           240     /* Vertical resolution */
#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)  /* 40 MHz */

/* ============================================================================
 * GLOBAL VARIABLES
 * ============================================================================ */

static const char *TAG = "MoneyBot";

/* LED strip handle */
static led_strip_handle_t led_strip;

/* LCD panel and IO handles */
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;

/* LVGL display handle */
static lv_disp_t *lvgl_disp = NULL;

/* LVGL label for displaying text */
static lv_obj_t *money_label = NULL;

/* ============================================================================
 * LCD INITIALIZATION
 * ============================================================================ */

static void lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing LCD display...");

    /* ----- Configure Backlight (if using GPIO control) ----- */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_PIN_BLK
    };
    gpio_config(&bk_gpio_config);
    gpio_set_level(LCD_PIN_BLK, 1);  /* Turn on backlight */

    /* ----- Initialize SPI Bus ----- */
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,              /* Not used for display */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    /* ----- Configure LCD Panel IO ----- */
    ESP_LOGI(TAG, "Configuring LCD panel IO...");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &lcd_io_handle));

    /* ----- Create GC9A01 LCD Panel ----- */
    ESP_LOGI(TAG, "Creating GC9A01 LCD panel...");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(lcd_io_handle, &panel_config, &lcd_panel));

    /* ----- Initialize and Configure Panel ----- */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    ESP_LOGI(TAG, "LCD initialized successfully!");
}

/* ============================================================================
 * LVGL INITIALIZATION
 * ============================================================================ */

static void lvgl_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL...");

    /* ----- Initialize LVGL Port ----- */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* ----- Add LCD Screen to LVGL ----- */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io_handle,
        .panel_handle = lcd_panel,
        .buffer_size = LCD_H_RES * 50,  /* Partial buffer for memory efficiency */
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

    ESP_LOGI(TAG, "LVGL initialized successfully!");
}

/* ============================================================================
 * UI FUNCTIONS
 * ============================================================================ */

static void create_ui(void)
{
    /* Lock LVGL mutex before accessing LVGL functions */
    lvgl_port_lock(0);

    /* Get the active screen */
    lv_obj_t *scr = lv_disp_get_scr_act(lvgl_disp);

    /* Set screen background to dark blue with full opacity */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000033), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Create a label for the money message */
    money_label = lv_label_create(scr);
    lv_label_set_text(money_label, "READY");  /* Start with test text */
    lv_obj_set_style_text_color(money_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  /* White text */
    lv_obj_set_style_text_font(money_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(money_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(money_label, LCD_H_RES - 20);
    lv_obj_center(money_label);

    /* Unlock LVGL mutex */
    lvgl_port_unlock();
    
    ESP_LOGI(TAG, "UI created - test text should be visible");
}

static void show_money_message(void)
{
    lvgl_port_lock(0);
    lv_label_set_text(money_label, "You just made\nmoney!!");
    lv_obj_set_style_text_color(money_label, lv_color_hex(0x00FF00), LV_PART_MAIN);  /* Green */
    lvgl_port_unlock();
}

static void clear_display(void)
{
    lvgl_port_lock(0);
    lv_label_set_text(money_label, "");
    lvgl_port_unlock();
}

/* ============================================================================
 * LED SETUP
 * ============================================================================ */

static void led_init(void)
{
    ESP_LOGI(TAG, "Configuring LED strip on GPIO %d", LED_STRIP_GPIO);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));

    /* Startup flash */
    ESP_LOGI(TAG, "Testing LED with white flash...");
    led_strip_set_pixel(led_strip, 0, 50, 50, 50);
    ESP_ERROR_CHECK(led_strip_refresh(led_strip));
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
}

/* ============================================================================
 * SETUP FUNCTION - Runs once at startup
 * ============================================================================ */

void setup(void)
{
    ESP_LOGI(TAG, "=== MoneyBot Starting ===");

    /* Initialize LED */
    led_init();

    /* Initialize LCD Display */
    lcd_init();

    /* Initialize LVGL Graphics */
    lvgl_init();

    /* Create the UI */
    create_ui();

    ESP_LOGI(TAG, "Setup complete, starting blink loop...");
}

/* ============================================================================
 * LOOP FUNCTION - Runs repeatedly forever
 * ============================================================================ */

void loop(void)
{
    static bool led_on = false;
    led_on = !led_on;

    if (led_on)
    {
        /* ----- LED RED + Show Message ----- */
        led_strip_set_pixel(led_strip, 0, 255, 0, 0);
        ESP_ERROR_CHECK(led_strip_refresh(led_strip));
        show_money_message();
        ESP_LOGI(TAG, "LED RED - Money message displayed!");
    }
    else
    {
        /* ----- LED OFF + Clear Display ----- */
        ESP_ERROR_CHECK(led_strip_clear(led_strip));
        clear_display();
        ESP_LOGI(TAG, "LED OFF - Display cleared");
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* ============================================================================
 * APP_MAIN - Entry point
 * ============================================================================ */

void app_main(void)
{
    setup();
    while (1)
    {
        loop();
    }
}
