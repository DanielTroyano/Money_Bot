/**
 * =============================================================================
 * Simple LED Blink for ESP32-S3-DevKitC-1-N8R8
 * =============================================================================
 *
 * This program blinks the onboard RGB LED red every 3 seconds.
 *
 * IMPORTANT CONCEPTS FOR BEGINNERS:
 * ---------------------------------
 * 1. This is NOT Arduino code - it's ESP-IDF (Espressif IoT Development Framework)
 *    ESP-IDF is the official development framework from Espressif for ESP32 chips.
 *    It uses C (not C++) and is more powerful but more complex than Arduino.
 *
 * 2. The onboard LED is a WS2812 "addressable" RGB LED (also called NeoPixel).
 *    Unlike simple LEDs that you just turn on/off, addressable LEDs receive
 *    digital data signals to set their color. That's why we need the led_strip driver.
 *
 * 3. The RMT (Remote Control Transceiver) peripheral is used to generate the
 *    precise timing signals that WS2812 LEDs require.
 *
 * 4. FreeRTOS is a Real-Time Operating System that runs on the ESP32.
 *    It handles task scheduling, timing, and multitasking.
 *
 * HARDWARE NOTE:
 * - ESP32-S3-DevKitC-1 v1.0 uses GPIO48 for the RGB LED
 * - ESP32-S3-DevKitC-1 v1.1 uses GPIO38 for the RGB LED (your board!)
 */

/* =============================================================================
 * INCLUDES - Libraries we need
 * =============================================================================*/

/* FreeRTOS - Real-Time Operating System
 * Provides task management, delays, and multitasking capabilities */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF Logging - Allows us to print messages to the serial monitor
 * Much better than printf() as it includes timestamps and log levels */
#include "esp_log.h"

/* LED Strip Driver - Handles communication with addressable RGB LEDs
 * This is a managed component from Espressif's component registry */
#include "led_strip.h"

/* =============================================================================
 * CONFIGURATION - Constants and settings
 * =============================================================================*/

/* GPIO pin connected to the RGB LED data line
 * Change to 48 if you have an older v1.0 board */
#define LED_STRIP_GPIO 38

/* Number of LEDs in the strip (just 1 on the DevKit board) */
#define LED_STRIP_LED_COUNT 1

/* =============================================================================
 * GLOBAL VARIABLES
 * =============================================================================*/

/* Tag used for logging - appears in monitor output like: I (1234) MoneyBot: message */
static const char *TAG = "MoneyBot";

/* Handle to the LED strip driver instance
 * This is like a "pointer" that lets us control the LED after it's initialized */
static led_strip_handle_t led_strip;

/* =============================================================================
 * SETUP FUNCTION - Runs once at startup
 * =============================================================================*/
void setup(void)
{
	/* Log a message to the serial monitor */
	ESP_LOGI(TAG, "Configuring LED strip on GPIO %d", LED_STRIP_GPIO);

	/* ----- LED Strip Configuration -----
	 * This structure tells the driver about our LED hardware */
	led_strip_config_t strip_config = {
		.strip_gpio_num = LED_STRIP_GPIO, /* Which GPIO pin the LED is on */
		.max_leds = LED_STRIP_LED_COUNT,  /* How many LEDs in the chain */
		.led_model = LED_MODEL_WS2812,	  /* Type of LED (WS2812 is common) */
		.flags.invert_out = false,		  /* Don't invert the signal */
	};

	/* ----- RMT (Remote Control Transceiver) Configuration -----
	 * RMT is a hardware peripheral that generates precise timing signals.
	 * WS2812 LEDs need very precise timing (nanosecond accuracy) */
	led_strip_rmt_config_t rmt_config = {
		.clk_src = RMT_CLK_SRC_DEFAULT,	   /* Use default clock source */
		.resolution_hz = 10 * 1000 * 1000, /* 10 MHz = 100ns resolution */
		.mem_block_symbols = 64,		   /* Memory for storing signal patterns */
		.flags.with_dma = false,		   /* Don't use DMA (not needed for 1 LED) */
	};

	/* ----- Create the LED strip driver -----
	 * This initializes the hardware and gives us a handle to control it.
	 * ESP_ERROR_CHECK() will halt the program if this fails (good for debugging) */
	ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));

	/* Give the LED a moment to initialize */
	vTaskDelay(pdMS_TO_TICKS(100)); /* 100 milliseconds */

	/* Turn off the LED initially */
	ESP_ERROR_CHECK(led_strip_clear(led_strip));

	/* ----- Startup Test: Flash white briefly -----
	 * This confirms the LED is working during initialization */
	ESP_LOGI(TAG, "Testing LED with white flash...");
	led_strip_set_pixel(led_strip, 0, 50, 50, 50); /* Pixel 0, R=50, G=50, B=50 (dim white) */
	ESP_ERROR_CHECK(led_strip_refresh(led_strip)); /* Send the data to the LED */
	vTaskDelay(pdMS_TO_TICKS(500));				   /* Wait 500ms */
	ESP_ERROR_CHECK(led_strip_clear(led_strip));   /* Turn off */

	ESP_LOGI(TAG, "Setup complete, starting blink loop...");
}

/* =============================================================================
 * LOOP FUNCTION - Runs repeatedly forever
 * =============================================================================*/
void loop(void)
{
	/* Static variable - keeps its value between function calls
	 * This tracks whether the LED is currently on or off */
	static bool led_on = false;

	/* Toggle the state */
	led_on = !led_on;

	if (led_on)
	{
		/* ----- Turn LED RED -----
		 * led_strip_set_pixel(handle, pixel_index, red, green, blue)
		 * Values range from 0 (off) to 255 (full brightness) */
		led_strip_set_pixel(led_strip, 0, 255, 0, 0);  /* Full red, no green, no blue */
		ESP_ERROR_CHECK(led_strip_refresh(led_strip)); /* Send data to LED */
		ESP_LOGI(TAG, "LED RED");
	}
	else
	{
		/* ----- Turn LED OFF ----- */
		ESP_ERROR_CHECK(led_strip_clear(led_strip)); /* Clears all pixels */
		ESP_LOGI(TAG, "LED OFF");
	}

	/* Wait 3 seconds before next iteration
	 * vTaskDelay() is a FreeRTOS function that pauses this task
	 * pdMS_TO_TICKS() converts milliseconds to OS "ticks" */
	vTaskDelay(pdMS_TO_TICKS(3000));
}

/* =============================================================================
 * APP_MAIN - Entry point (like main() in regular C programs)
 * =============================================================================
 *
 * In ESP-IDF, app_main() is called after the system boots up.
 * Unlike Arduino's setup()/loop(), we need to create our own infinite loop.
 */
void app_main(void)
{
	setup(); /* Run setup once */
	while (1)
	{			/* Infinite loop */
		loop(); /* Run loop forever */
	}
}
