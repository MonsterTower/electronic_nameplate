#ifndef APP_MODEL_H
#define APP_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#define APP_MODEL_NAME_LEN 32
#define APP_MODEL_ORG_LEN 48
#define APP_MODEL_TOPIC_LEN 64
#define APP_MODEL_QR_LEN 48
#define APP_MODEL_DATE_LEN 20
#define APP_MODEL_TIME_LEN 16
#define APP_MODEL_WEEKDAY_LEN 8
#define APP_MODEL_WEATHER_LEN 24
#define APP_MODEL_SCHEDULE_LEN 48
#define APP_MODEL_COURSE_NAME_LEN 48
#define APP_MODEL_COURSE_TIME_LEN 6
#define APP_MODEL_COURSE_ROOM_LEN 32
#define APP_MODEL_IP_LEN 16
#define APP_MODEL_STATUS_LEN 24
#define APP_MODEL_ERROR_LEN 64
#define APP_MODEL_VERSION_LEN 16
#define APP_MODEL_COURSE_COUNT 4

typedef struct {
    char name[APP_MODEL_NAME_LEN];
    char org[APP_MODEL_ORG_LEN];
    char topic[APP_MODEL_TOPIC_LEN];
    char qr_text[APP_MODEL_QR_LEN];
} app_nameplate_info_t;

typedef struct {
    char date[APP_MODEL_DATE_LEN];
    char time[APP_MODEL_TIME_LEN];
    char weekday[APP_MODEL_WEEKDAY_LEN];
    char weather[APP_MODEL_WEATHER_LEN];
    char schedule[APP_MODEL_SCHEDULE_LEN];
    bool time_synced;
} app_calendar_info_t;

typedef struct {
    char name[APP_MODEL_COURSE_NAME_LEN];
    char start[APP_MODEL_COURSE_TIME_LEN];
    char end[APP_MODEL_COURSE_TIME_LEN];
    char room[APP_MODEL_COURSE_ROOM_LEN];
    bool is_next;
} app_course_info_t;

typedef struct {
    bool valid;
    float voltage;
    int percent;
    char voltage_text[APP_MODEL_STATUS_LEN];
    char percent_text[APP_MODEL_STATUS_LEN];
} app_battery_info_t;

typedef struct {
    bool wifi_connected;
    bool ntp_synced;
    char ip_text[APP_MODEL_IP_LEN];
    char wifi_text[APP_MODEL_STATUS_LEN];
    char ntp_text[APP_MODEL_STATUS_LEN];
    char last_error[APP_MODEL_ERROR_LEN];
} app_network_info_t;

typedef struct {
    app_nameplate_info_t nameplate;
    app_calendar_info_t calendar;
    app_course_info_t courses[APP_MODEL_COURSE_COUNT];
    uint8_t course_count;
    bool course_data_valid;
    app_battery_info_t battery;
    app_network_info_t network;
    char firmware_version[APP_MODEL_VERSION_LEN];
} app_model_t;

void app_model_init(app_model_t *model);
void app_model_update_runtime(app_model_t *model);

#endif
