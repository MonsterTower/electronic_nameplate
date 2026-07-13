#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery_monitor.h"
#include "app_cache.h"
#include "app_model.h"
#include "display_pages.h"
#include "network_service.h"
#include "power_manager.h"
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
    app_model_update_battery_level(model, battery_monitor_get_percentage(), battery_monitor_is_low(),
                                   battery_monitor_is_critical());
    display_pages_render(model);
    display_pages_sleep();
}

static void update_model_battery(app_model_t *model)
{
    app_model_update_battery(model, battery_monitor_has_sample(), battery_monitor_get_voltage());
    app_model_update_battery_level(model, battery_monitor_get_percentage(), battery_monitor_is_low(),
                                   battery_monitor_is_critical());
}

static void button_led_turn_off(void)
{
    for (size_t index = 0; index < sizeof(s_button_led_pairs) / sizeof(s_button_led_pairs[0]); ++index) {
        gpio_set_level(s_button_led_pairs[index].led_gpio, LED_OFF_LEVEL);
    }
}

static void enter_sleep(uint32_t sleep_seconds, const char *reason)
{
    display_pages_sleep();
    network_service_shutdown();
    button_led_turn_off();
    power_manager_enter_deep_sleep(sleep_seconds, reason);
}

static void update_model_wifi_from_network(app_model_t *model,
                                           const network_service_data_t *network_data)
{
    if (model == NULL || network_data == NULL) {
        return;
    }
    model->wifi_connected = network_data->wifi_connected;
    snprintf(model->wifi_text, sizeof(model->wifi_text), "%s", network_data->wifi_text);
}

static void update_model_time_from_network(app_model_t *model,
                                           const network_service_data_t *network_data)
{
    if (model == NULL || network_data == NULL) {
        return;
    }

    model->time_synced = network_data->time_synced;
    snprintf(model->date, sizeof(model->date), "%s", network_data->date);
    snprintf(model->weekday, sizeof(model->weekday), "%s", network_data->weekday);
    snprintf(model->time, sizeof(model->time), "%s", network_data->time);
}

static void clear_time_and_weather(app_model_t *model)
{
    model->time_synced = false;
    model->date[0] = '\0';
    model->weekday[0] = '\0';
    snprintf(model->time, sizeof(model->time), "%s", "时间未校准");
    snprintf(model->weather, sizeof(model->weather), "%s", "天气未更新");
}

static void clear_cold_start_network_data(app_model_t *model)
{
    model->wifi_connected = false;
    snprintf(model->wifi_text, sizeof(model->wifi_text), "%s", "未连接");
    clear_time_and_weather(model);
}

void app_main(void)
{
    button_led_init();
    const power_wake_reason_t wake_reason = power_manager_init();
    battery_monitor_init();
    (void)battery_monitor_sample_now();
    printf("hardware bring-up: button to LED test started\n");

    app_model_t model;
    app_model_init(&model);
    ESP_ERROR_CHECK(app_cache_init());
    (void)app_cache_restore(&model);
    update_model_battery(&model);

    display_pages_init();

    const bool cold_boot = wake_reason == POWER_WAKE_COLD_BOOT;
    bool network_updated = false;
    bool render_required = cold_boot;
    if (model.battery_low) {
        printf("power: low battery, Wi-Fi update skipped\n");
        (void)app_cache_save(&model);
        render_required = true;
    } else {
        ESP_ERROR_CHECK(network_service_init());
        const bool connection_started = network_service_start_connection();
        const bool calendar_waiting = connection_started && !cold_boot &&
                                      model.page == APP_PAGE_CALENDAR;
        if (connection_started && (cold_boot || calendar_waiting)) {
            /* Wi-Fi 认证与墨水屏全刷并行进行，避免用户面对空白等待。 */
            display_pages_render_network_waiting(&model);
        }
        const bool time_synced = connection_started && network_service_wait_for_connection() &&
                                 network_service_sync_time_once();
        network_service_data_t network_data = {0};
        network_service_get_snapshot(&network_data);
        if (network_data.wifi_connected) {
            update_model_wifi_from_network(&model, &network_data);
        }

        if (time_synced && network_data.wifi_connected) {
            update_model_time_from_network(&model, &network_data);
            const bool weather_updated = weather_service_refresh(&model);
            const bool schedule_updated = schedule_service_refresh(&model, &network_data.local_time);
            if (cold_boot && !weather_updated) {
                snprintf(model.weather, sizeof(model.weather), "%s", "天气未更新");
            }
            update_model_battery(&model);
            if (cold_boot) {
                model.page = APP_PAGE_NAMEPLATE;
            }
            (void)app_cache_save(&model);
            network_updated = true;
            render_required = true;
            printf("network: weather=%s schedule=%s\n", weather_updated ? "updated" : "cached",
                   schedule_updated ? "updated" : "cached");
        } else {
            printf("weather: skipped because network time is unavailable\n");
            printf("schedule: skipped because network time is unavailable\n");
            if (cold_boot) {
                if (network_data.wifi_connected) {
                    clear_time_and_weather(&model);
                    model.page = APP_PAGE_NAMEPLATE;
                } else {
                    clear_cold_start_network_data(&model);
                }
                (void)app_cache_save(&model);
                render_required = true;
            }
            render_required = render_required || calendar_waiting;
        }
    }

    if (render_required) {
        render_current_page(&model);
    }

    if (model.battery_low) {
        enter_sleep(POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS,
                    model.battery_critical ? "critical battery" : "low battery");
    }
    if (wake_reason == POWER_WAKE_TIMER) {
        enter_sleep(network_updated ? POWER_MANAGER_NORMAL_SLEEP_SECONDS :
                                     POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS,
                    network_updated ? "periodic update complete" : "network retry");
    }

    power_manager_start_interaction();
    while (true) {
        const button_event_t event = button_led_update();
        battery_monitor_update();
        update_model_battery(&model);

        if (event == BUTTON_EVENT_MINUS) {
            app_model_previous_page(&model);
            (void)app_cache_save(&model);
            printf("page: switched to %d\n", (int)model.page);
            render_current_page(&model);
            power_manager_note_activity();
        } else if (event == BUTTON_EVENT_PLUS) {
            app_model_next_page(&model);
            (void)app_cache_save(&model);
            printf("page: switched to %d\n", (int)model.page);
            render_current_page(&model);
            power_manager_note_activity();
        } else if (event == BUTTON_EVENT_BOOT) {
            printf("power: interaction retained by BOOT\n");
            power_manager_note_activity();
        }

        if (model.battery_low) {
            (void)app_cache_save(&model);
            render_current_page(&model);
            enter_sleep(POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS,
                        model.battery_critical ? "critical battery" : "low battery");
        }
        if (power_manager_interaction_expired()) {
            enter_sleep(network_updated ? POWER_MANAGER_NORMAL_SLEEP_SECONDS :
                                         POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS,
                        network_updated ? "interaction timeout" : "network retry");
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}
