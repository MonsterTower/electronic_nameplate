#include "schedule_service.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#define SCHEDULE_JSON_SCHEMA_VERSION 1
#define SCHEDULE_PERIOD_MAX 11
#define SCHEDULE_HTTP_RESPONSE_MAX 12288

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} schedule_http_response_t;

typedef struct {
    bool valid;
    char start[APP_MODEL_COURSE_TIME_LEN];
    char end[APP_MODEL_COURSE_TIME_LEN];
} schedule_period_t;

static char s_http_response_buffer[SCHEDULE_HTTP_RESPONSE_MAX];
/* 课程解析只在主任务中串行执行，工作数组放入静态区以节省任务栈。 */
static app_course_info_t s_parsed_courses[APP_MODEL_COURSE_COUNT];
static schedule_period_t s_periods[SCHEDULE_PERIOD_MAX + 1];

static void schedule_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static esp_err_t schedule_service_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    schedule_http_response_t *response = event->user_data;
    const size_t data_len = (size_t)event->data_len;
    if (response == NULL || data_len > response->capacity - response->length - 1U) {
        if (response != NULL) {
            response->overflow = true;
        }
        return ESP_FAIL;
    }

    memcpy(&response->data[response->length], event->data, data_len);
    response->length += data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool schedule_parse_date(const char *text, struct tm *result)
{
    int year = 0;
    int month = 0;
    int day = 0;

    if (text == NULL || result == NULL || sscanf(text, "%d-%d-%d", &year, &month, &day) != 3 ||
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

static bool schedule_read_periods(const cJSON *root, schedule_period_t periods[SCHEDULE_PERIOD_MAX + 1])
{
    const cJSON *periods_json = cJSON_GetObjectItemCaseSensitive(root, "periods");
    if (!cJSON_IsArray(periods_json)) {
        return false;
    }

    const cJSON *period = NULL;
    cJSON_ArrayForEach(period, periods_json) {
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(period, "period");
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(period, "start");
        const cJSON *end = cJSON_GetObjectItemCaseSensitive(period, "end");
        if (!cJSON_IsNumber(number) || !cJSON_IsString(start) || !cJSON_IsString(end) ||
            number->valueint < 1 || number->valueint > SCHEDULE_PERIOD_MAX ||
            strlen(start->valuestring) != 5 || strlen(end->valuestring) != 5) {
            continue;
        }

        schedule_period_t *target = &periods[number->valueint];
        schedule_copy_text(target->start, sizeof(target->start), start->valuestring);
        schedule_copy_text(target->end, sizeof(target->end), end->valuestring);
        target->valid = true;
    }

    return true;
}

static int schedule_calculate_teaching_week(const cJSON *root, const struct tm *local_time)
{
    const cJSON *semester = cJSON_GetObjectItemCaseSensitive(root, "semester");
    const cJSON *start_date = semester != NULL ? cJSON_GetObjectItemCaseSensitive(semester, "startDate") : NULL;
    const cJSON *end_date = semester != NULL ? cJSON_GetObjectItemCaseSensitive(semester, "endDate") : NULL;
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

static int schedule_weekday_from_tm(const struct tm *local_time)
{
    // struct tm 的周日是 0；JSON 约定周一是 1、周日是 7。
    return (local_time->tm_wday + 6) % 7 + 1;
}

static int schedule_time_to_minutes(const char *time_text)
{
    int hour = 0;
    int minute = 0;
    if (time_text == NULL || sscanf(time_text, "%d:%d", &hour, &minute) != 2) {
        return -1;
    }
    return hour * 60 + minute;
}

static void schedule_sort_courses(app_course_info_t *courses, uint8_t count)
{
    for (uint8_t i = 1; i < count; ++i) {
        app_course_info_t current = courses[i];
        uint8_t position = i;
        while (position > 0 && strcmp(current.start, courses[position - 1].start) < 0) {
            courses[position] = courses[position - 1];
            --position;
        }
        courses[position] = current;
    }
}

static void schedule_mark_next_course(app_course_info_t *courses, uint8_t count, const struct tm *local_time)
{
    const int now_minutes = local_time->tm_hour * 60 + local_time->tm_min;
    for (uint8_t i = 0; i < count; ++i) {
        courses[i].is_next = false;
    }
    for (uint8_t i = 0; i < count; ++i) {
        if (schedule_time_to_minutes(courses[i].start) >= now_minutes) {
            courses[i].is_next = true;
            return;
        }
    }
}

static bool schedule_parse_today_courses(const char *json_text, app_model_t *model, const struct tm *local_time)
{
    cJSON *root = cJSON_Parse(json_text);
    uint8_t parsed_count = 0;
    bool valid = false;

    memset(s_parsed_courses, 0, sizeof(s_parsed_courses));
    memset(s_periods, 0, sizeof(s_periods));

    if (root == NULL) {
        printf("schedule: json parse failed\n");
        goto finish;
    }

    const cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    const cJSON *courses_json = cJSON_GetObjectItemCaseSensitive(root, "courses");
    if (!cJSON_IsNumber(schema_version) || schema_version->valueint != SCHEDULE_JSON_SCHEMA_VERSION ||
        !cJSON_IsArray(courses_json) || !schedule_read_periods(root, s_periods)) {
        printf("schedule: unsupported json schema\n");
        goto finish;
    }

    const int teaching_week = schedule_calculate_teaching_week(root, local_time);
    if (teaching_week == 0) {
        printf("schedule: current date outside semester\n");
        goto finish;
    }

    const int weekday = schedule_weekday_from_tm(local_time);
    const cJSON *course = NULL;
    cJSON_ArrayForEach(course, courses_json) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(course, "name");
        const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(course, "sessions");
        if (!cJSON_IsString(name) || !cJSON_IsArray(sessions)) {
            continue;
        }

        const cJSON *session = NULL;
        cJSON_ArrayForEach(session, sessions) {
            const cJSON *session_weekday = cJSON_GetObjectItemCaseSensitive(session, "weekday");
            const cJSON *weeks = cJSON_GetObjectItemCaseSensitive(session, "weeks");
            const cJSON *start_period = cJSON_GetObjectItemCaseSensitive(session, "startPeriod");
            const cJSON *end_period = cJSON_GetObjectItemCaseSensitive(session, "endPeriod");
            const cJSON *location = cJSON_GetObjectItemCaseSensitive(session, "location");
            if (!cJSON_IsNumber(session_weekday) || !cJSON_IsNumber(start_period) || !cJSON_IsNumber(end_period) ||
                !cJSON_IsString(location) || session_weekday->valueint != weekday ||
                !schedule_week_contains(weeks, teaching_week) || parsed_count >= APP_MODEL_COURSE_COUNT ||
                start_period->valueint < 1 || start_period->valueint > SCHEDULE_PERIOD_MAX ||
                end_period->valueint < 1 || end_period->valueint > SCHEDULE_PERIOD_MAX ||
                !s_periods[start_period->valueint].valid || !s_periods[end_period->valueint].valid) {
                continue;
            }

            app_course_info_t *target = &s_parsed_courses[parsed_count++];
            schedule_copy_text(target->name, sizeof(target->name), name->valuestring);
            schedule_copy_text(target->start, sizeof(target->start), s_periods[start_period->valueint].start);
            schedule_copy_text(target->end, sizeof(target->end), s_periods[end_period->valueint].end);
            schedule_copy_text(target->room, sizeof(target->room), location->valuestring);
        }
    }

    schedule_sort_courses(s_parsed_courses, parsed_count);
    schedule_mark_next_course(s_parsed_courses, parsed_count, local_time);
    memset(model->courses, 0, sizeof(model->courses));
    memcpy(model->courses, s_parsed_courses, sizeof(s_parsed_courses));
    model->course_count = parsed_count;
    model->course_data_valid = true;
    printf("schedule: week=%d weekday=%d courses=%u\n", teaching_week, weekday, (unsigned)parsed_count);
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

    schedule_http_response_t response = {
        .data = s_http_response_buffer,
        .capacity = sizeof(s_http_response_buffer),
        .length = 0,
        .overflow = false,
    };
    response.data[0] = '\0';

    esp_http_client_config_t config = {
        .url = SCHEDULE_SERVICE_URL,
        .timeout_ms = SCHEDULE_SERVICE_HTTP_TIMEOUT_MS,
        .event_handler = schedule_service_http_event,
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
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        printf("schedule: http failed err=%s status=%d size=%u\n", esp_err_to_name(err), status_code,
               (unsigned)response.length);
        return false;
    }

    return schedule_parse_today_courses(response.data, model, local_time);
}
