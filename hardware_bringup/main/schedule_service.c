#include "schedule_service.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#define SCHEDULE_SERVICE_URL \
    "https://cdn.jsdelivr.net/gh/MonsterTower/esp32-calendar-data@main/schedule.json"
#define SCHEDULE_HTTP_TIMEOUT_MS 8000U
#define SCHEDULE_RESPONSE_MAX 12288U
#define SCHEDULE_SCHEMA_VERSION 1
#define SCHEDULE_PERIOD_MAX 11

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} schedule_response_t;

typedef struct {
    bool valid;
    char start[6];
    char end[6];
} schedule_period_t;

static char s_response_buffer[SCHEDULE_RESPONSE_MAX];
static app_schedule_item_t s_parsed_items[APP_MODEL_SCHEDULE_ITEM_COUNT];
static schedule_period_t s_periods[SCHEDULE_PERIOD_MAX + 1];

static void schedule_copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0U) {
        return;
    }
    snprintf(destination, destination_size, "%s", source != NULL ? source : "");
}

static esp_err_t schedule_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    schedule_response_t *response = event->user_data;
    const size_t data_length = (size_t)event->data_len;
    if (response == NULL || data_length > response->capacity - response->length - 1U) {
        if (response != NULL) {
            response->overflow = true;
        }
        return ESP_FAIL;
    }

    memcpy(&response->data[response->length], event->data, data_length);
    response->length += data_length;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool schedule_parse_date(const char *text, struct tm *result)
{
    int year = 0;
    int month = 0;
    int day = 0;
    if (text == NULL || result == NULL ||
        sscanf(text, "%d-%d-%d", &year, &month, &day) != 3 ||
        year < 2016 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }

    *result = (struct tm){
        .tm_year = year - 1900,
        .tm_mon = month - 1,
        .tm_mday = day,
        .tm_hour = 12,
        .tm_isdst = -1,
    };
    return mktime(result) != (time_t)-1;
}

static int schedule_calculate_teaching_week(const cJSON *root, const struct tm *local_time)
{
    const cJSON *semester = cJSON_GetObjectItemCaseSensitive(root, "semester");
    const cJSON *start_date = semester != NULL ?
        cJSON_GetObjectItemCaseSensitive(semester, "startDate") : NULL;
    const cJSON *end_date = semester != NULL ?
        cJSON_GetObjectItemCaseSensitive(semester, "endDate") : NULL;
    struct tm semester_start = {0};
    struct tm today = *local_time;

    if (!cJSON_IsString(start_date) || !schedule_parse_date(start_date->valuestring, &semester_start)) {
        return 0;
    }

    today.tm_hour = 12;
    today.tm_min = 0;
    today.tm_sec = 0;
    today.tm_isdst = -1;
    const time_t today_epoch = mktime(&today);
    const time_t start_epoch = mktime(&semester_start);
    if (today_epoch == (time_t)-1 || start_epoch == (time_t)-1) {
        return 0;
    }
    const int days_from_start = (int)(difftime(today_epoch, start_epoch) / 86400.0);
    if (days_from_start < 0) {
        return 0;
    }

    if (cJSON_IsString(end_date)) {
        struct tm semester_end = {0};
        if (schedule_parse_date(end_date->valuestring, &semester_end) &&
            difftime(today_epoch, mktime(&semester_end)) > 0.0) {
            return 0;
        }
    }
    return days_from_start / 7 + 1;
}

static bool schedule_week_contains(const cJSON *weeks, int teaching_week)
{
    if (!cJSON_IsArray(weeks)) {
        return false;
    }

    const cJSON *week = NULL;
    cJSON_ArrayForEach(week, weeks) {
        if (cJSON_IsNumber(week) && week->valueint == teaching_week) {
            return true;
        }
    }
    return false;
}

static bool schedule_read_periods(const cJSON *root)
{
    const cJSON *periods = cJSON_GetObjectItemCaseSensitive(root, "periods");
    if (!cJSON_IsArray(periods)) {
        return false;
    }

    const cJSON *period = NULL;
    cJSON_ArrayForEach(period, periods) {
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(period, "period");
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(period, "start");
        const cJSON *end = cJSON_GetObjectItemCaseSensitive(period, "end");
        if (!cJSON_IsNumber(number) || !cJSON_IsString(start) || !cJSON_IsString(end) ||
            number->valueint < 1 || number->valueint > SCHEDULE_PERIOD_MAX) {
            continue;
        }

        schedule_period_t *target = &s_periods[number->valueint];
        schedule_copy_text(target->start, sizeof(target->start), start->valuestring);
        schedule_copy_text(target->end, sizeof(target->end), end->valuestring);
        target->valid = true;
    }
    return true;
}

static void schedule_copy_first_teacher(char *destination, size_t destination_size,
                                        const cJSON *teachers)
{
    const cJSON *teacher = cJSON_IsArray(teachers) ? cJSON_GetArrayItem(teachers, 0) : NULL;
    schedule_copy_text(destination, destination_size,
                       cJSON_IsString(teacher) ? teacher->valuestring : "");
}

static int schedule_time_to_minutes(const char *text)
{
    int hour = 0;
    int minute = 0;
    if (text == NULL || sscanf(text, "%d:%d", &hour, &minute) != 2) {
        return -1;
    }
    return hour * 60 + minute;
}

static int schedule_weekday_from_tm(const struct tm *local_time)
{
    return (local_time->tm_wday + 6) % 7 + 1;
}

static void schedule_sort_items(app_schedule_item_t *items, uint8_t item_count, int today_weekday)
{
    for (uint8_t index = 1; index < item_count; ++index) {
        const app_schedule_item_t current = items[index];
        uint8_t position = index;
        const int current_day_offset = (current.weekday - today_weekday + 7) % 7;
        while (position > 0) {
            const app_schedule_item_t *previous = &items[position - 1];
            const int previous_day_offset = (previous->weekday - today_weekday + 7) % 7;
            if (current_day_offset > previous_day_offset ||
                (current_day_offset == previous_day_offset && strcmp(current.start, previous->start) >= 0)) {
                break;
            }
            items[position] = items[position - 1];
            --position;
        }
        items[position] = current;
    }
}

static void schedule_mark_current_or_next(app_schedule_item_t *items, uint8_t item_count,
                                          const struct tm *local_time)
{
    const int today = schedule_weekday_from_tm(local_time);
    const int now_minutes = local_time->tm_hour * 60 + local_time->tm_min;
    bool has_current_course = false;
    int next_index = -1;
    int next_day_offset = 8;
    int next_start_minutes = 24 * 60;

    for (uint8_t index = 0; index < item_count; ++index) {
        app_schedule_item_t *item = &items[index];
        const int start_minutes = schedule_time_to_minutes(item->start);
        const int end_minutes = schedule_time_to_minutes(item->end);
        item->is_today = item->weekday == today;
        item->is_current = item->is_today && start_minutes <= now_minutes && now_minutes < end_minutes;
        item->is_next = false;
        if (item->is_current) {
            has_current_course = true;
        }
    }

    if (has_current_course) {
        return;
    }

    for (uint8_t index = 0; index < item_count; ++index) {
        const app_schedule_item_t *item = &items[index];
        const int start_minutes = schedule_time_to_minutes(item->start);
        const int day_offset = (item->weekday - today + 7) % 7;
        if ((day_offset == 0 && start_minutes <= now_minutes) || start_minutes < 0) {
            continue;
        }
        if (day_offset < next_day_offset ||
            (day_offset == next_day_offset && start_minutes < next_start_minutes)) {
            next_index = (int)index;
            next_day_offset = day_offset;
            next_start_minutes = start_minutes;
        }
    }

    if (next_index >= 0) {
        items[next_index].is_next = true;
    }
}

static bool schedule_parse(const char *json_text, app_model_t *model, const struct tm *local_time)
{
    cJSON *root = cJSON_Parse(json_text);
    uint8_t item_count = 0;
    bool valid = false;

    memset(s_parsed_items, 0, sizeof(s_parsed_items));
    memset(s_periods, 0, sizeof(s_periods));
    if (root == NULL) {
        printf("schedule: json parse failed\n");
        goto finish;
    }

    const cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    const cJSON *courses = cJSON_GetObjectItemCaseSensitive(root, "courses");
    if (!cJSON_IsNumber(schema_version) || schema_version->valueint != SCHEDULE_SCHEMA_VERSION ||
        !cJSON_IsArray(courses) || !schedule_read_periods(root)) {
        printf("schedule: unsupported json schema\n");
        goto finish;
    }

    const int teaching_week = schedule_calculate_teaching_week(root, local_time);
    if (teaching_week == 0) {
        printf("schedule: current date outside semester\n");
        goto finish;
    }

    const cJSON *course = NULL;
    cJSON_ArrayForEach(course, courses) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(course, "name");
        const cJSON *course_teachers = cJSON_GetObjectItemCaseSensitive(course, "teachers");
        const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(course, "sessions");
        if (!cJSON_IsString(name) || !cJSON_IsArray(sessions)) {
            continue;
        }

        const cJSON *session = NULL;
        cJSON_ArrayForEach(session, sessions) {
            const cJSON *weekday = cJSON_GetObjectItemCaseSensitive(session, "weekday");
            const cJSON *weeks = cJSON_GetObjectItemCaseSensitive(session, "weeks");
            const cJSON *start_period = cJSON_GetObjectItemCaseSensitive(session, "startPeriod");
            const cJSON *end_period = cJSON_GetObjectItemCaseSensitive(session, "endPeriod");
            const cJSON *location = cJSON_GetObjectItemCaseSensitive(session, "location");
            const cJSON *session_teachers = cJSON_GetObjectItemCaseSensitive(session, "teachers");
            if (!cJSON_IsNumber(weekday) || !cJSON_IsNumber(start_period) ||
                !cJSON_IsNumber(end_period) || !cJSON_IsString(location) ||
                !schedule_week_contains(weeks, teaching_week) ||
                item_count >= APP_MODEL_SCHEDULE_ITEM_COUNT ||
                weekday->valueint < 1 || weekday->valueint > 7 ||
                start_period->valueint < 1 || start_period->valueint > SCHEDULE_PERIOD_MAX ||
                end_period->valueint < 1 || end_period->valueint > SCHEDULE_PERIOD_MAX ||
                !s_periods[start_period->valueint].valid ||
                !s_periods[end_period->valueint].valid) {
                continue;
            }

            app_schedule_item_t *target = &s_parsed_items[item_count++];
            target->weekday = (uint8_t)weekday->valueint;
            schedule_copy_text(target->name, sizeof(target->name), name->valuestring);
            schedule_copy_text(target->start, sizeof(target->start),
                               s_periods[start_period->valueint].start);
            schedule_copy_text(target->end, sizeof(target->end),
                               s_periods[end_period->valueint].end);
            schedule_copy_text(target->room, sizeof(target->room), location->valuestring);
            schedule_copy_first_teacher(target->teacher, sizeof(target->teacher),
                                        cJSON_IsArray(session_teachers) ? session_teachers : course_teachers);
        }
    }

    schedule_sort_items(s_parsed_items, item_count, schedule_weekday_from_tm(local_time));
    schedule_mark_current_or_next(s_parsed_items, item_count, local_time);
    memcpy(model->schedule_items, s_parsed_items, sizeof(s_parsed_items));
    model->schedule_item_count = item_count;
    model->teaching_week = teaching_week;
    model->schedule_data_valid = true;
    printf("schedule: week=%d items=%u\n", teaching_week, (unsigned)item_count);
    valid = true;

finish:
    cJSON_Delete(root);
    return valid;
}

bool schedule_service_refresh(app_model_t *model, const struct tm *local_time)
{
    if (model == NULL || local_time == NULL || local_time->tm_year < (2016 - 1900)) {
        return false;
    }

    schedule_response_t response = {
        .data = s_response_buffer,
        .capacity = sizeof(s_response_buffer),
    };

    response.data[0] = '\0';
    const esp_http_client_config_t config = {
        .url = SCHEDULE_SERVICE_URL,
        .timeout_ms = SCHEDULE_HTTP_TIMEOUT_MS,
        .event_handler = schedule_http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        printf("schedule: http client init failed\n");
        return false;
    }

    printf("schedule: downloading json\n");
    const esp_err_t err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0U) {
        printf("schedule: http failed err=%s status=%d size=%u\n", esp_err_to_name(err),
               status_code, (unsigned)response.length);
        return false;
    }
    return schedule_parse(response.data, model, local_time);
}
