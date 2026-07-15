#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "battery_monitor.h"
#include "audio_service.h"
#include "audio_test.h"
#include "ai_service.h"
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
#define NETWORK_UPDATE_TASK_STACK_SIZE 16384U
#define NETWORK_UPDATE_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

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

typedef struct {
    bool running;
    bool completed;
    bool time_synced;
    bool weather_updated;
    bool schedule_updated;
    network_service_data_t network_data;
    app_model_t updated_model;
} network_update_job_t;

static button_led_pair_t s_button_led_pairs[] = {
    {.button_gpio = BUTTON_BOOT_GPIO, .led_gpio = LED_BOOT_GPIO, .name = "BOOT"},
    {.button_gpio = BUTTON_MINUS_GPIO, .led_gpio = LED_MINUS_GPIO, .name = "BUT-"},
    {.button_gpio = BUTTON_PLUS_GPIO, .led_gpio = LED_PLUS_GPIO, .name = "BUT+"},
};
static SemaphoreHandle_t s_network_job_mutex;
static network_update_job_t s_network_job;

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
    ai_service_stop_session();
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

static void merge_network_data(app_model_t *model, const app_model_t *updated_model)
{
    if (model == NULL || updated_model == NULL) {
        return;
    }

    model->wifi_connected = updated_model->wifi_connected;
    snprintf(model->wifi_text, sizeof(model->wifi_text), "%s", updated_model->wifi_text);
    model->time_synced = updated_model->time_synced;
    snprintf(model->date, sizeof(model->date), "%s", updated_model->date);
    snprintf(model->weekday, sizeof(model->weekday), "%s", updated_model->weekday);
    snprintf(model->time, sizeof(model->time), "%s", updated_model->time);
    snprintf(model->weather, sizeof(model->weather), "%s", updated_model->weather);
    memcpy(model->schedule_items, updated_model->schedule_items, sizeof(model->schedule_items));
    model->schedule_item_count = updated_model->schedule_item_count;
    model->teaching_week = updated_model->teaching_week;
    model->schedule_data_valid = updated_model->schedule_data_valid;
}

static void network_update_task(void *argument)
{
    (void)argument;

    app_model_t updated_model = {0};
    xSemaphoreTake(s_network_job_mutex, portMAX_DELAY);
    updated_model = s_network_job.updated_model;
    xSemaphoreGive(s_network_job_mutex);

    const bool time_synced = network_service_update_once();
    network_service_data_t network_data = {0};
    network_service_get_snapshot(&network_data);
    const bool update_succeeded = time_synced && network_data.wifi_connected;
    bool weather_updated = false;
    bool schedule_updated = false;

    if (network_data.wifi_connected) {
        update_model_wifi_from_network(&updated_model, &network_data);
    }
    if (update_succeeded) {
        update_model_time_from_network(&updated_model, &network_data);
        weather_updated = weather_service_refresh(&updated_model);
        schedule_updated = schedule_service_refresh(&updated_model, &network_data.local_time);
        update_model_battery(&updated_model);
        printf("network: weather=%s schedule=%s\n", weather_updated ? "updated" : "cached",
               schedule_updated ? "updated" : "cached");
    } else {
        printf("weather: skipped because network time is unavailable\n");
        printf("schedule: skipped because network time is unavailable\n");
    }

    xSemaphoreTake(s_network_job_mutex, portMAX_DELAY);
    s_network_job.time_synced = update_succeeded;
    s_network_job.weather_updated = weather_updated;
    s_network_job.schedule_updated = schedule_updated;
    s_network_job.network_data = network_data;
    s_network_job.updated_model = updated_model;
    s_network_job.running = false;
    s_network_job.completed = true;
    xSemaphoreGive(s_network_job_mutex);
    vTaskDelete(NULL);
}

static bool network_update_start(const app_model_t *model)
{
    if (model == NULL || s_network_job_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_network_job_mutex, portMAX_DELAY);
    if (s_network_job.running || s_network_job.completed) {
        xSemaphoreGive(s_network_job_mutex);
        return false;
    }
    s_network_job = (network_update_job_t){
        .running = true,
        .updated_model = *model,
    };
    xSemaphoreGive(s_network_job_mutex);

    const BaseType_t created = xTaskCreate(network_update_task, "network_update",
                                           NETWORK_UPDATE_TASK_STACK_SIZE,
                                           NULL, NETWORK_UPDATE_TASK_PRIORITY, NULL);
    if (created == pdPASS) {
        printf("network: update task started\n");
        return true;
    }

    xSemaphoreTake(s_network_job_mutex, portMAX_DELAY);
    s_network_job.running = false;
    xSemaphoreGive(s_network_job_mutex);
    printf("network: update task creation failed\n");
    return false;
}

static bool network_update_is_running(void)
{
    if (s_network_job_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_network_job_mutex, portMAX_DELAY);
    const bool running = s_network_job.running;
    xSemaphoreGive(s_network_job_mutex);
    return running;
}

static bool network_is_connected(void)
{
    network_service_data_t network_data = {0};
    network_service_get_snapshot(&network_data);
    return network_data.wifi_connected;
}

static void render_page_after_switch(app_model_t *model)
{
    const bool calendar_waiting = model->page == APP_PAGE_CALENDAR && network_update_is_running();
    if (model->page == APP_PAGE_AI && !network_is_connected()) {
        /* AI 页面需要联网；若此前网络任务已结束，则重新发起一次有限时的连接周期。 */
        if (!network_update_is_running() && !network_update_start(model)) {
            model->wifi_connected = false;
            snprintf(model->wifi_text, sizeof(model->wifi_text), "%s", "未连接");
            render_current_page(model);
            return;
        }
        display_pages_render_network_waiting(model);
        return;
    }
    if (calendar_waiting) {
        display_pages_render_network_waiting(model);
        return;
    }
    render_current_page(model);
}

static bool network_update_take_result(network_update_job_t *result)
{
    if (result == NULL || s_network_job_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_network_job_mutex, portMAX_DELAY);
    if (!s_network_job.completed) {
        xSemaphoreGive(s_network_job_mutex);
        return false;
    }
    *result = s_network_job;
    s_network_job.completed = false;
    xSemaphoreGive(s_network_job_mutex);
    return true;
}

void app_main(void)
{
    button_led_init();
    const power_wake_reason_t wake_reason = power_manager_init();
    battery_monitor_init();
    (void)battery_monitor_sample_now();
    printf("hardware bring-up: button to LED test started\n");
#if AUDIO_TEST_CONTINUOUS_MIC_MONITOR
    if (wake_reason == POWER_WAKE_COLD_BOOT) {
        audio_test_run_microphone_monitor();
    }
#elif AUDIO_TEST_RUN_ON_COLD_BOOT
    if (wake_reason == POWER_WAKE_COLD_BOOT) {
        const esp_err_t audio_test_err = audio_test_run_once();
        printf("audio: hardware test %s\n", audio_test_err == ESP_OK ? "complete" : "finished with errors");
    }
#endif
    /* 在主任务的 Core 0 固定 I2S/GDMA 的中断归属，AI 编码任务将运行在 Core 1。 */
    const esp_err_t audio_prepare_err = audio_service_prepare();
    if (audio_prepare_err != ESP_OK) {
        printf("audio: I2S/GDMA prepare failed: %s\n", esp_err_to_name(audio_prepare_err));
    }

    app_model_t model;
    app_model_init(&model);
    ESP_ERROR_CHECK(app_cache_init());
    (void)app_cache_restore(&model);
    update_model_battery(&model);

    display_pages_init();

    const bool cold_boot = wake_reason == POWER_WAKE_COLD_BOOT;
    bool network_updated = false;
    bool network_started = false;
    if (model.battery_low) {
        printf("power: low battery, Wi-Fi update skipped\n");
        (void)app_cache_save(&model);
        render_current_page(&model);
    } else {
        ESP_ERROR_CHECK(network_service_init());
        s_network_job_mutex = xSemaphoreCreateMutex();
        if (s_network_job_mutex == NULL) {
            printf("network: result mutex creation failed\n");
        } else {
            network_started = network_update_start(&model);
        }
        if (network_started &&
            (cold_boot || model.page == APP_PAGE_CALENDAR || model.page == APP_PAGE_AI)) {
            /* Wi-Fi 认证与墨水屏全刷并行进行，避免用户面对空白等待。 */
            display_pages_render_network_waiting(&model);
        } else if (cold_boot) {
            clear_cold_start_network_data(&model);
            (void)app_cache_save(&model);
            render_current_page(&model);
        }
    }

    if (model.battery_low) {
        enter_sleep(POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS,
                    model.battery_critical ? "critical battery" : "low battery");
    }
    if (wake_reason == POWER_WAKE_TIMER && !network_started && model.page != APP_PAGE_AI) {
        enter_sleep(network_updated ? POWER_MANAGER_NORMAL_SLEEP_SECONDS :
                                     POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS,
                    network_updated ? "periodic update complete" : "network retry");
    }

    power_manager_start_interaction();
    while (true) {
        const button_event_t event = button_led_update();
        battery_monitor_update();
        update_model_battery(&model);

        network_update_job_t network_result = {0};
        if (network_update_take_result(&network_result)) {
            network_started = false;
            if (network_result.time_synced) {
                merge_network_data(&model, &network_result.updated_model);
                network_updated = true;
                if (cold_boot) {
                    if (!network_result.weather_updated) {
                        snprintf(model.weather, sizeof(model.weather), "%s", "天气未更新");
                    }
                    model.page = APP_PAGE_NAMEPLATE;
                }
                (void)app_cache_save(&model);
            } else if (cold_boot) {
                if (network_result.network_data.wifi_connected) {
                    update_model_wifi_from_network(&model, &network_result.network_data);
                    clear_time_and_weather(&model);
                    model.page = APP_PAGE_NAMEPLATE;
                } else {
                    clear_cold_start_network_data(&model);
                }
                (void)app_cache_save(&model);
            } else if (network_result.network_data.wifi_connected) {
                /* 仅更新本次确认得到的 Wi-Fi 状态，保留旧时间、天气和课表。 */
                update_model_wifi_from_network(&model, &network_result.network_data);
                (void)app_cache_save(&model);
            }

            if (model.page == APP_PAGE_AI && !network_result.network_data.wifi_connected) {
                /* AI 页面不能沿用缓存中的联网状态，否则会显示不可用的 BOOT 提示。 */
                update_model_wifi_from_network(&model, &network_result.network_data);
            }

            if (cold_boot || model.page == APP_PAGE_CALENDAR || model.page == APP_PAGE_AI) {
                render_current_page(&model);
            }
            if (wake_reason == POWER_WAKE_TIMER && model.page != APP_PAGE_AI) {
                enter_sleep(network_updated ? POWER_MANAGER_NORMAL_SLEEP_SECONDS :
                                             POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS,
                            network_updated ? "periodic update complete" : "network retry");
            }
            power_manager_start_interaction();
        }

        if (event == BUTTON_EVENT_MINUS) {
            app_model_previous_page(&model);
            (void)app_cache_save(&model);
            printf("page: switched to %d\n", (int)model.page);
            render_page_after_switch(&model);
            power_manager_note_activity();
        } else if (event == BUTTON_EVENT_PLUS) {
            app_model_next_page(&model);
            (void)app_cache_save(&model);
            printf("page: switched to %d\n", (int)model.page);
            render_page_after_switch(&model);
            power_manager_note_activity();
        } else if (event == BUTTON_EVENT_BOOT) {
            if (model.page == APP_PAGE_AI) {
                ai_service_start_session();
            } else {
                printf("power: interaction retained by BOOT\n");
            }
            power_manager_note_activity();
        }

        if (model.battery_low) {
            (void)app_cache_save(&model);
            render_current_page(&model);
            enter_sleep(POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS,
                        model.battery_critical ? "critical battery" : "low battery");
        }
        if (model.page != APP_PAGE_AI && !network_update_is_running() &&
            power_manager_interaction_expired()) {
            enter_sleep(network_updated ? POWER_MANAGER_NORMAL_SLEEP_SECONDS :
                                         POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS,
                        network_updated ? "interaction timeout" : "network retry");
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_INTERVAL_MS));
    }
}
