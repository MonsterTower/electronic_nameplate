#include <stdio.h>

#include "battery_monitor.h"
#include "input_panel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("Hello, ESP32-S3!v2\n");

    input_panel_init();
    battery_monitor_init();

    while (true) {
        input_panel_update();
        battery_monitor_update();
        vTaskDelay(pdMS_TO_TICKS(INPUT_PANEL_SCAN_INTERVAL_MS));
    }
}
