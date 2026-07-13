#pragma once

#include <stdbool.h>

#define APP_MODEL_NAME_LEN 32
#define APP_MODEL_ORG_LEN 48
#define APP_MODEL_TOPIC_LEN 64
#define APP_MODEL_DATE_LEN 24
#define APP_MODEL_WEEKDAY_LEN 16
#define APP_MODEL_WEATHER_LEN 32
#define APP_MODEL_EVENT_LEN 64
#define APP_MODEL_COURSE_LEN 64
#define APP_MODEL_TIME_LEN 32
#define APP_MODEL_ROOM_LEN 32
#define APP_MODEL_WIFI_TEXT_LEN 32

typedef enum {
    APP_PAGE_NAMEPLATE = 0,
    APP_PAGE_CALENDAR,
    APP_PAGE_SCHEDULE,
    APP_PAGE_STATUS,
    APP_PAGE_COUNT,
} app_page_t;

/* 数据模型不依赖显示、按键或网络模块，后续只由各数据源更新。 */
typedef struct {
    app_page_t page;
    char name[APP_MODEL_NAME_LEN];
    char organization[APP_MODEL_ORG_LEN];
    char topic[APP_MODEL_TOPIC_LEN];
    char date[APP_MODEL_DATE_LEN];
    char weekday[APP_MODEL_WEEKDAY_LEN];
    char time[APP_MODEL_TIME_LEN];
    bool time_synced;
    char weather[APP_MODEL_WEATHER_LEN];
    char calendar_event[APP_MODEL_EVENT_LEN];
    bool has_course;
    char next_course[APP_MODEL_COURSE_LEN];
    char next_course_time[APP_MODEL_TIME_LEN];
    char next_course_room[APP_MODEL_ROOM_LEN];
    bool wifi_connected;
    char wifi_text[APP_MODEL_WIFI_TEXT_LEN];
    bool has_battery_sample;
    float battery_voltage;
} app_model_t;

void app_model_init(app_model_t *model);
void app_model_next_page(app_model_t *model);
void app_model_previous_page(app_model_t *model);
void app_model_update_battery(app_model_t *model, bool has_sample, float voltage);
