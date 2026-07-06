#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED0 GPIO_NUM_13
#define LED1 GPIO_NUM_14
#define LED2 GPIO_NUM_21

#define BUT_BOOT GPIO_NUM_0
#define BUT_PLUS GPIO_NUM_39
#define BUT_MINUS GPIO_NUM_40

void app_main(void)
{
    printf("Hello, ESP32-S3!\n");

    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED0) | (1ULL << LED1) | (1ULL << LED2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_config);

    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUT_BOOT) | (1ULL << BUT_PLUS) | (1ULL << BUT_MINUS),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_config);

    while (true) {
        gpio_set_level(LED0, !gpio_get_level(BUT_BOOT));
        gpio_set_level(LED1, !gpio_get_level(BUT_PLUS));
        gpio_set_level(LED2, !gpio_get_level(BUT_MINUS));

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
