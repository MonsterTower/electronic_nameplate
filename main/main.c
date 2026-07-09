#include <stdio.h>

#include "app_model.h"
#include "battery_monitor.h"
#include "display_pages.h"
#include "input_panel.h"
#include "network_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DISPLAY_MODEL_REFRESH_MS 10000

void app_main(void)
{
    app_model_t model;

    printf("Hello, ESP32-S3!v2\n");

    app_model_init(&model);
    input_panel_init();
    battery_monitor_init();
    network_service_init();
    display_pages_init();

    battery_monitor_update();
    app_model_update_runtime(&model);

    int last_state = input_panel_get_state();
    TickType_t last_display_tick = xTaskGetTickCount();
    display_pages_show_state(last_state, &model);

    while (true) {
        input_panel_update();
        battery_monitor_update();

        const int current_state = input_panel_get_state();
        const TickType_t now = xTaskGetTickCount();
        const bool page_changed = current_state != last_state;
        const bool refresh_due = (now - last_display_tick) >= pdMS_TO_TICKS(DISPLAY_MODEL_REFRESH_MS);

        if (page_changed || refresh_due) {
            // 主循环统一汇总数据，再把只读模型交给页面层绘制。
            app_model_update_runtime(&model);
            display_pages_show_state(current_state, &model);
            last_state = current_state;
            last_display_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(INPUT_PANEL_SCAN_INTERVAL_MS));
    }
}
