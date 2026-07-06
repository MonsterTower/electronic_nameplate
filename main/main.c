#include <stdbool.h>
#include <stdint.h>
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

#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_SCAN_INTERVAL_MS 10
#define BOOT_LONG_PRESS_MS 200

#define STATE_MIN 0
#define STATE_MAX 3
#define STATE_COUNT (STATE_MAX - STATE_MIN + 1)

typedef struct {
    gpio_num_t pin;
    gpio_num_t led_pin;
    bool raw_pressed;
    bool stable_pressed;
    TickType_t last_raw_change_tick;
    TickType_t press_start_tick;
    bool long_press_fired;
} button_state_t;

static int s_state = STATE_MIN;

static bool is_button_pressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

static bool ticks_elapsed(TickType_t now, TickType_t start, uint32_t timeout_ms)
{
    return (now - start) >= pdMS_TO_TICKS(timeout_ms);
}

static void update_state(int delta)
{
    s_state = s_state + delta;
    s_state %= STATE_COUNT;
    printf("state: %d\n", s_state);
}

static void handle_boot_short_press(void)
{
    printf("BOOT short press\n");
}

static void handle_boot_long_press(void)
{
    printf("BOOT long press\n");
}

static void init_button_state(button_state_t *button, gpio_num_t pin, gpio_num_t led_pin)
{
    const TickType_t now = xTaskGetTickCount();

    button->pin = pin;
    button->led_pin = led_pin;
    button->raw_pressed = is_button_pressed(pin);
    button->stable_pressed = button->raw_pressed;
    button->last_raw_change_tick = now;
    button->press_start_tick = now;
    button->long_press_fired = false;
}

static bool update_button(button_state_t *button, TickType_t now)
{
    const bool raw_pressed = is_button_pressed(button->pin);

    if (raw_pressed != button->raw_pressed) {
        button->raw_pressed = raw_pressed;
        button->last_raw_change_tick = now;
    }

    if (raw_pressed == button->stable_pressed ||
        !ticks_elapsed(now, button->last_raw_change_tick, BUTTON_DEBOUNCE_MS)) {
        return false;
    }

    button->stable_pressed = raw_pressed;
    if (button->stable_pressed) {
        button->press_start_tick = now;
        button->long_press_fired = false;
    }

    return true;
}

static void handle_boot_button(button_state_t *button, bool changed, TickType_t now)
{
    if (button->stable_pressed && !button->long_press_fired &&
        ticks_elapsed(now, button->press_start_tick, BOOT_LONG_PRESS_MS)) {
        button->long_press_fired = true;
        handle_boot_long_press();
    }

    if (changed && !button->stable_pressed && !button->long_press_fired) {
        handle_boot_short_press();
    }
}

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

    button_state_t boot_button;
    button_state_t plus_button;
    button_state_t minus_button;

    init_button_state(&boot_button, BUT_BOOT, LED0);
    init_button_state(&plus_button, BUT_PLUS, LED1);
    init_button_state(&minus_button, BUT_MINUS, LED2);

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const bool boot_changed = update_button(&boot_button, now);
        const bool plus_changed = update_button(&plus_button, now);
        const bool minus_changed = update_button(&minus_button, now);

        gpio_set_level(boot_button.led_pin, boot_button.stable_pressed);
        gpio_set_level(plus_button.led_pin, plus_button.stable_pressed);
        gpio_set_level(minus_button.led_pin, minus_button.stable_pressed);

        handle_boot_button(&boot_button, boot_changed, now);

        if (plus_changed && plus_button.stable_pressed) {
            update_state(1);
        }

        if (minus_changed && minus_button.stable_pressed) {
            update_state(-1);
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}
