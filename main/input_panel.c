#include "input_panel.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED0 GPIO_NUM_13
#define LED1 GPIO_NUM_14
#define LED2 GPIO_NUM_21

#define BUT_BOOT INPUT_PANEL_BOOT_GPIO
#define BUT_PLUS GPIO_NUM_39
#define BUT_MINUS GPIO_NUM_40

// 按键扫描参数：防抖时间、BOOT 长按判定时间。
#define BUTTON_DEBOUNCE_MS 30
#define BOOT_LONG_PRESS_MS 200

// PLUS/MINUS 控制的状态范围。
#define STATE_MIN 0
#define STATE_MAX 3
#define STATE_COUNT (STATE_MAX - STATE_MIN + 1)

// 单个按键的运行时状态，用于防抖、LED 显示和长按判定。
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
static button_state_t s_boot_button;
static button_state_t s_plus_button;
static button_state_t s_minus_button;
static bool s_ignore_boot_until_release;
static bool s_activity_event;

// 按键使用内部上拉，按下时 GPIO 读到低电平。
static bool is_button_pressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

// 判断指定毫秒数是否已经经过，使用 TickType_t 可兼容 tick 回绕。
static bool ticks_elapsed(TickType_t now, TickType_t start, uint32_t timeout_ms)
{
    return (now - start) >= pdMS_TO_TICKS(timeout_ms);
}

// 根据 PLUS/MINUS 输入增减当前状态，并通过串口输出。
static void update_state(int delta)
{
    s_state = (s_state - STATE_MIN + delta) % STATE_COUNT;
    if (s_state < 0) {
        s_state += STATE_COUNT;
    }
    s_state += STATE_MIN;

    printf("state: %d\n", s_state);
}

// BOOT 短按处理入口，后续短按业务逻辑可以集中写在这里。
static void handle_boot_short_press(void)
{
    printf("BOOT short press\n");
}

// BOOT 长按处理入口，后续长按业务逻辑可以集中写在这里。
static void handle_boot_long_press(void)
{
    printf("BOOT long press\n");
}

// 初始化按键状态，避免上电瞬间的当前电平被误认为一次按键事件。
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

// 更新防抖后的稳定按键状态；只有稳定状态改变时返回 true。
static bool update_button(button_state_t *button, TickType_t now)
{
    const bool raw_pressed = is_button_pressed(button->pin);

    // 原始电平变化后先记录时间，等待防抖时间结束再确认。
    if (raw_pressed != button->raw_pressed) {
        button->raw_pressed = raw_pressed;
        button->last_raw_change_tick = now;
    }

    if (raw_pressed == button->stable_pressed ||
        !ticks_elapsed(now, button->last_raw_change_tick, BUTTON_DEBOUNCE_MS)) {
        return false;
    }

    // 防抖确认后更新稳定状态；按下瞬间记录时间用于长按判断。
    button->stable_pressed = raw_pressed;
    if (button->stable_pressed) {
        button->press_start_tick = now;
        button->long_press_fired = false;
    }

    return true;
}

// BOOT 长按触发后设置 long_press_fired，松手时不会再执行短按。
static void handle_boot_button(button_state_t *button, bool changed, TickType_t now)
{
    // Deep Sleep 由 BOOT 唤醒时，这一次按压只负责唤醒，不再混入短按或长按业务。
    if (s_ignore_boot_until_release) {
        if (changed && !button->stable_pressed) {
            s_ignore_boot_until_release = false;
        }
        return;
    }

    if (button->stable_pressed && !button->long_press_fired &&
        ticks_elapsed(now, button->press_start_tick, BOOT_LONG_PRESS_MS)) {
        button->long_press_fired = true;
        handle_boot_long_press();
    }

    if (changed && !button->stable_pressed && !button->long_press_fired) {
        handle_boot_short_press();
    }
}

void input_panel_init(void)
{
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

    init_button_state(&s_boot_button, BUT_BOOT, LED0);
    init_button_state(&s_plus_button, BUT_PLUS, LED1);
    init_button_state(&s_minus_button, BUT_MINUS, LED2);
    s_ignore_boot_until_release = false;
    s_activity_event = false;
}

void input_panel_update(void)
{
    const TickType_t now = xTaskGetTickCount();

    // 每轮扫描三个按键，得到防抖后的状态变化事件。
    const bool boot_changed = update_button(&s_boot_button, now);
    const bool plus_changed = update_button(&s_plus_button, now);
    const bool minus_changed = update_button(&s_minus_button, now);

    // 所有按键的稳定按下都算作用户活动，用于延长低功耗状态机的交互窗口。
    if ((boot_changed && s_boot_button.stable_pressed) ||
        (plus_changed && s_plus_button.stable_pressed) ||
        (minus_changed && s_minus_button.stable_pressed)) {
        s_activity_event = true;
    }

    // LED 使用稳定后的按键状态，避免抖动时 LED 闪烁。
    gpio_set_level(s_boot_button.led_pin, s_boot_button.stable_pressed);
    gpio_set_level(s_plus_button.led_pin, s_plus_button.stable_pressed);
    gpio_set_level(s_minus_button.led_pin, s_minus_button.stable_pressed);

    handle_boot_button(&s_boot_button, boot_changed, now);

    // PLUS/MINUS 在确认按下的瞬间触发，松手不重复执行。
    if (plus_changed && s_plus_button.stable_pressed) {
        update_state(1);
    }

    if (minus_changed && s_minus_button.stable_pressed) {
        update_state(-1);
    }
}

int input_panel_get_state(void)
{
    return s_state;
}

void input_panel_set_state(int state)
{
    if (state < STATE_MIN || state > STATE_MAX) {
        s_state = STATE_MIN;
        return;
    }

    s_state = state;
}

void input_panel_ignore_boot_wakeup_press(void)
{
    // 如果芯片完成启动时按键已经松开，就无需忽略后续真正的用户按键。
    s_ignore_boot_until_release = s_boot_button.stable_pressed;
}

bool input_panel_take_activity_event(void)
{
    const bool active = s_activity_event;
    s_activity_event = false;
    return active;
}
