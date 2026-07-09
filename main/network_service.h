#ifndef NETWORK_SERVICE_H
#define NETWORK_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define NETWORK_SERVICE_IP_TEXT_LEN 16
#define NETWORK_SERVICE_TIME_TEXT_LEN 32
#define NETWORK_SERVICE_DATE_TEXT_LEN 16
#define NETWORK_SERVICE_WEEKDAY_TEXT_LEN 8
#define NETWORK_SERVICE_ERROR_TEXT_LEN 96

typedef struct {
    bool wifi_connected;
    char ip_text[NETWORK_SERVICE_IP_TEXT_LEN];

    bool time_synced;
    struct tm local_time;
    char date_text[NETWORK_SERVICE_DATE_TEXT_LEN];
    char weekday_text[NETWORK_SERVICE_WEEKDAY_TEXT_LEN];
    char time_text[NETWORK_SERVICE_TIME_TEXT_LEN];
    time_t next_refresh_epoch;

    uint32_t retry_interval_ms;
    char last_error[NETWORK_SERVICE_ERROR_TEXT_LEN];
} network_service_data_t;

esp_err_t network_service_init(void);
void network_service_get_snapshot(network_service_data_t *snapshot);

#endif
