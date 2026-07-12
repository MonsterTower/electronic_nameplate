#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 按键和 LED 的实际 PCB 引脚集中定义。PCB 标注的是模组管脚号，此处使用对应 GPIO。 */
#define BUTTON_BOOT_GPIO GPIO_NUM_0
#define BUTTON_MINUS_GPIO GPIO_NUM_39
#define BUTTON_PLUS_GPIO GPIO_NUM_40

#define LED_BOOT_GPIO GPIO_NUM_21
#define LED_MINUS_GPIO GPIO_NUM_13
#define LED_PLUS_GPIO GPIO_NUM_14

/* 当前电路按“按下接地、LED 高电平点亮”处理。 */
#define BUTTON_PRESSED_LEVEL 0
#define LED_ON_LEVEL 1
#define LED_OFF_LEVEL 0

#define BUTTON_SCAN_INTERVAL_MS 10U
#define BUTTON_DEBOUNCE_MS 30U

typedef struct {
    gpio_num_t button_gpio;
    gpio_num_t led_gpio;
    const char *name;
    int raw_level;
    int stable_level;
    TickType_t last_raw_change_tick;
} button_led_pair_t;

static button_led_pair_t s_button_led_pairs[] = {
    {.button_gpio = BUTTON_BOOT_GPIO, .led_gpio = LED_BOOT_GPIO, .name = "BOOT"},
    {.button_gpio = BUTTON_MINUS_GPIO, .led_gpio = LED_MINUS_GPIO, .name = "BUT-"},
    {.button_gpio = BUTTON_PLUS_GPIO, .led_gpio = LED_PLUS_GPIO, .name = "BUT+"},
};

static bool button_is_pressed(int level)
{
    return level == BUTTON_PRESSED_LEVEL;
}

static void button_led_apply(const button_led_pair_t *pair)
{
    gpio_set_level(pair->led_gpio, button_is_pressed(pair->stable_level) ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

static void button_led_init(void)
{
    const uint64_t button_mask = (1ULL << BUTTON_BOOT_GPIO) |
                                 (1ULL << BUTTON_MINUS_GPIO) |
                                 (1ULL << BUTTON_PLUS_GPIO);
    const uint64_t led_mask = (1ULL << LED_BOOT_GPIO) |
                              (1ULL << LED_MINUS_GPIO) |
                              (1ULL << LED_PLUS_GPIO);

    const gpio_config_t button_config = {
        .pin_bit_mask = button_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t led_config = {
        .pin_bit_mask = led_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
    ESP_ERROR_CHECK(gpio_config(&led_config));

    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < sizeof(s_button_led_pairs) / sizeof(s_button_led_pairs[0]); ++i) {
        button_led_pair_t *pair = &s_button_led_pairs[i];
        pair->raw_level = gpio_get_level(pair->button_gpio);
        pair->stable_level = pair->raw_level;
        pair->last_raw_change_tick = now;
        button_led_apply(pair);
    }
}

static void button_led_update(void)
{
    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < sizeof(s_button_led_pairs) / sizeof(s_button_led_pairs[0]); ++i) {
        button_led_pair_t *pair = &s_button_led_pairs[i];
        const int raw_level = gpio_get_level(pair->button_gpio);

        if (raw_level != pair->raw_level) {
            pair->raw_level = raw_level;
            pair->last_raw_change_tick = now;
        }

        if (pair->stable_level != pair->raw_level &&
            now - pair->last_raw_change_tick >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
            pair->stable_level = pair->raw_level;
            button_led_apply(pair);
            printf("button: %s %s\n", pair->name,
                   button_is_pressed(pair->stable_level) ? "pressed" : "released");
        }
    }
}

void app_main(void)
{
    button_led_init();
    printf("hardware bring-up: button to LED test started\n");

    while (true) {
        button_led_update();
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}
