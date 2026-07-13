#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery_monitor.h"
#include "app_model.h"
#include "display_pages.h"
#include "network_service.h"
#include "schedule_service.h"
#include "weather_service.h"

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

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_BOOT,
    BUTTON_EVENT_MINUS,
    BUTTON_EVENT_PLUS,
} button_event_t;

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

static button_event_t button_led_update(void)
{
    button_event_t event = BUTTON_EVENT_NONE;
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
            if (!button_is_pressed(pair->stable_level)) {
                continue;
            }

            if (pair->button_gpio == BUTTON_BOOT_GPIO) {
                event = BUTTON_EVENT_BOOT;
            } else if (pair->button_gpio == BUTTON_MINUS_GPIO) {
                event = BUTTON_EVENT_MINUS;
            } else if (pair->button_gpio == BUTTON_PLUS_GPIO) {
                event = BUTTON_EVENT_PLUS;
            }
        }
    }
    return event;
}

static void render_current_page(app_model_t *model)
{
    app_model_update_battery(model, battery_monitor_has_sample(), battery_monitor_get_voltage());
    display_pages_render(model);
    display_pages_sleep();
}

static void update_model_from_network(app_model_t *model, network_service_data_t *network_data)
{
    if (model == NULL || network_data == NULL) {
        return;
    }
    network_service_get_snapshot(network_data);

    model->wifi_connected = network_data->wifi_connected;
    snprintf(model->wifi_text, sizeof(model->wifi_text), "%s", network_data->wifi_text);
    model->time_synced = network_data->time_synced;
    snprintf(model->date, sizeof(model->date), "%s", network_data->date);
    snprintf(model->weekday, sizeof(model->weekday), "%s", network_data->weekday);
    snprintf(model->time, sizeof(model->time), "%s", network_data->time);
}

void app_main(void)
{
    button_led_init();
    battery_monitor_init();
    (void)battery_monitor_sample_now();
    printf("hardware bring-up: button to LED test started\n");

    app_model_t model;
    app_model_init(&model);

    ESP_ERROR_CHECK(network_service_init());
    const bool time_synced = network_service_update_once();
    network_service_data_t network_data = {0};
    update_model_from_network(&model, &network_data);
    if (time_synced && model.wifi_connected) {
        (void)weather_service_refresh(&model);
        (void)schedule_service_refresh(&model, &network_data.local_time);
    } else {
        printf("weather: skipped because network time is unavailable\n");
        printf("schedule: skipped because network time is unavailable\n");
    }

    display_pages_init();
    render_current_page(&model);

    while (true) {
        const button_event_t event = button_led_update();
        battery_monitor_update();

        if (event == BUTTON_EVENT_MINUS) {
            app_model_previous_page(&model);
            printf("page: switched to %d\n", (int)model.page);
            render_current_page(&model);
        } else if (event == BUTTON_EVENT_PLUS) {
            app_model_next_page(&model);
            printf("page: switched to %d\n", (int)model.page);
            render_current_page(&model);
        } else if (event == BUTTON_EVENT_BOOT) {
            printf("page: BOOT retained for future action\n");
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}
