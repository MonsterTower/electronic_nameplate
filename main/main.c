#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_model.h"
#include "app_storage.h"
#include "battery_monitor.h"
#include "display_pages.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_panel.h"
#include "network_service.h"
#include "power_manager.h"

static bool text_changed(const char *before, const char *after)
{
    return strcmp(before, after) != 0;
}

static bool app_model_display_changed(const app_model_t *before, const app_model_t *after)
{
    // 只比较页面实际会显示的字段，忽略 ADC 浮点微小抖动等不可见变化。
    return text_changed(before->calendar.date, after->calendar.date) ||
           text_changed(before->calendar.time, after->calendar.time) ||
           text_changed(before->calendar.weekday, after->calendar.weekday) ||
           text_changed(before->calendar.weather, after->calendar.weather) ||
           text_changed(before->calendar.schedule, after->calendar.schedule) ||
           before->calendar.time_synced != after->calendar.time_synced ||
           text_changed(before->battery.voltage_text, after->battery.voltage_text) ||
           text_changed(before->battery.percent_text, after->battery.percent_text) ||
           text_changed(before->network.wifi_text, after->network.wifi_text) ||
           text_changed(before->network.ntp_text, after->network.ntp_text);
}

static void display_current_page(int page, const app_model_t *model, bool *display_initialized)
{
    if (!*display_initialized) {
        display_pages_init();
        *display_initialized = true;
    }
    display_pages_show_state(page, model);
}

static int run_interaction_window(int page, app_model_t *model, bool *display_initialized)
{
    TickType_t last_activity_tick = xTaskGetTickCount();
    int current_page = page;

    printf("power: interaction window=%u ms\n", (unsigned)POWER_MANAGER_INTERACTION_WINDOW_MS);
    while (xTaskGetTickCount() - last_activity_tick < pdMS_TO_TICKS(POWER_MANAGER_INTERACTION_WINDOW_MS)) {
        input_panel_update();
        if (input_panel_take_activity_event()) {
            // 在真正的屏幕刷新前重置计时，刷新阻塞不会吞掉用户刚获得的交互时间。
            last_activity_tick = xTaskGetTickCount();
            printf("power: interaction extended\n");
        }

        const int selected_page = input_panel_get_state();
        if (selected_page != current_page) {
            current_page = selected_page;
            // 按键只改变页面状态；页面层读取模型，不直接触碰按键或网络逻辑。
            app_model_update_runtime(model);
            display_current_page(current_page, model, display_initialized);
        }
        vTaskDelay(pdMS_TO_TICKS(INPUT_PANEL_SCAN_INTERVAL_MS));
    }

    return current_page;
}

static void app_run_work_cycle(void)
{
    const int64_t work_start_us = esp_timer_get_time();
    app_model_t model;
    uint8_t saved_page = 0;
    bool display_initialized = false;

    const power_wakeup_reason_t wake_reason = power_manager_get_wakeup_reason();
    printf("Hello, ESP32-S3! low-power mode\n");
    printf("power: wake=%s\n", power_manager_wakeup_reason_text(wake_reason));

    const esp_err_t storage_err = app_storage_init();
    if (storage_err != ESP_OK) {
        printf("storage: init failed: %s\n", esp_err_to_name(storage_err));
    }

    app_model_init(&model);
    if (storage_err == ESP_OK) {
        (void)app_storage_load(&saved_page, &model);
    }
    const app_model_t model_before_work = model;

    input_panel_init();
    input_panel_set_state(saved_page);
    if (wake_reason == POWER_WAKE_BOOT_BUTTON) {
        input_panel_ignore_boot_wakeup_press();
    }

    battery_monitor_init();
    (void)battery_monitor_sample_now();
    (void)network_service_init();
    app_model_update_runtime(&model);

    const bool low_battery = power_manager_is_low_battery(model.battery.voltage, model.battery.valid);
    power_manager_plan_t plan = power_manager_make_plan(wake_reason, low_battery, false, false);
    bool network_success = false;
    bool network_attempted = false;

    if (plan.should_connect_network) {
        network_attempted = true;
        network_success = network_service_update_once();
        network_service_shutdown();
        app_model_update_runtime(&model);
    }

    if (plan.interactive) {
        saved_page = (uint8_t)run_interaction_window(saved_page, &model, &display_initialized);
    } else if (low_battery) {
        // 低电量时主动展示告警，再转入更长的睡眠周期。
        if (!display_initialized) {
            display_pages_init();
            display_initialized = true;
        }
        display_pages_show_low_battery(&model);
    } else if (wake_reason == POWER_WAKE_COLD_START ||
               app_model_display_changed(&model_before_work, &model)) {
        display_current_page(saved_page, &model, &display_initialized);
    }

#if !POWER_MANAGER_ENABLE_DEEP_SLEEP
    if (!plan.interactive && wake_reason == POWER_WAKE_COLD_START) {
        // 没有真实睡眠就不会产生外部唤醒事件；仿真时在首次显示后保留交互窗口。
        saved_page = (uint8_t)run_interaction_window(saved_page, &model, &display_initialized);
    }
#endif

    plan = power_manager_make_plan(wake_reason, low_battery, network_attempted, network_success);
    if (storage_err == ESP_OK) {
        (void)app_storage_save(saved_page, &model);
    }

    if (display_initialized) {
        display_pages_sleep();
    }

    const int64_t work_ms = (esp_timer_get_time() - work_start_us) / 1000;
    printf("power: work=%lld ms, network=%s, sleep=%u s\n", work_ms,
           network_attempted ? (network_success ? "ok" : "failed") : "skipped",
           (unsigned)plan.sleep_seconds);
    power_manager_enter_deep_sleep(plan.sleep_seconds);
}

void app_main(void)
{
    power_manager_init();

    while (true) {
        // 实物 Deep Sleep 不会从该函数返回；Wokwi 模拟模式会在 BOOT 按下后返回并开始下一轮。
        app_run_work_cycle();
    }
}
