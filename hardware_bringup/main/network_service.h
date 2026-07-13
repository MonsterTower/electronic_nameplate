#pragma once

#include <stdbool.h>
#include <time.h>

#include "app_model.h"
#include "esp_err.h"

/* 一次联网工作周期的结果；页面只通过 app_model 获取这些数据。 */
typedef struct {
    bool wifi_connected;
    char wifi_text[APP_MODEL_WIFI_TEXT_LEN];
    bool time_synced;
    char date[APP_MODEL_DATE_LEN];
    char weekday[APP_MODEL_WEEKDAY_LEN];
    char time[APP_MODEL_TIME_LEN];
    struct tm local_time;
} network_service_data_t;

esp_err_t network_service_init(void);
bool network_service_update_once(void);
void network_service_get_snapshot(network_service_data_t *snapshot);
