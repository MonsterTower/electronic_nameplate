#include "weather_service.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"

/* 厦门大学附近坐标，只请求当前温度与天气码以减少网络流量。 */
#define WEATHER_SERVICE_URL \
    "https://api.open-meteo.com/v1/forecast?latitude=24.4410&longitude=118.0957" \
    "&current=temperature_2m,weather_code&timezone=Asia%2FShanghai"
#define WEATHER_SERVICE_HTTP_TIMEOUT_MS 8000U
#define WEATHER_SERVICE_RESPONSE_MAX 512U

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} weather_response_t;

static char s_response_buffer[WEATHER_SERVICE_RESPONSE_MAX];

static esp_err_t weather_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    weather_response_t *response = event->user_data;
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

static const char *weather_text_from_code(int weather_code)
{
    switch (weather_code) {
    case 0:
        return "晴";
    case 1:
    case 2:
        return "多云";
    case 3:
    case 45:
    case 48:
        return "阴";
    default:
        return "雨";
    }
}

/*
 * ESP-IDF 6 未内置 cJSON。本接口只使用固定的 current 对象和两个数值字段，
 * 因而受限提取可避免额外引入解析库；字段不存在或不是数值会安全失败。
 */
static bool weather_find_current_number(const char *current_json, const char *field_name,
                                        double *value)
{
    char field_prefix[48];
    snprintf(field_prefix, sizeof(field_prefix), "\"%s\":", field_name);
    const char *number_start = strstr(current_json, field_prefix);
    if (number_start == NULL) {
        return false;
    }
    number_start += strlen(field_prefix);
    while (isspace((unsigned char)*number_start)) {
        ++number_start;
    }

    char *number_end = NULL;
    const double parsed_value = strtod(number_start, &number_end);
    if (number_end == number_start) {
        return false;
    }
    *value = parsed_value;
    return true;
}

static bool weather_parse(const char *json_text, app_model_t *model)
{
    const char *current_json = strstr(json_text, "\"current\":");
    double temperature = 0.0;
    double weather_code = 0.0;
    if (current_json == NULL ||
        !weather_find_current_number(current_json, "temperature_2m", &temperature) ||
        !weather_find_current_number(current_json, "weather_code", &weather_code)) {
        printf("weather: unsupported json schema\n");
        return false;
    }

    snprintf(model->weather, sizeof(model->weather), "厦门 %.0fC %s", temperature,
             weather_text_from_code((int)weather_code));
    printf("weather: %s\n", model->weather);
    return true;
}

bool weather_service_refresh(app_model_t *model)
{
    if (model == NULL) {
        return false;
    }

    weather_response_t response = {
        .data = s_response_buffer,
        .capacity = sizeof(s_response_buffer),
    };
    response.data[0] = '\0';

    const esp_http_client_config_t config = {
        .url = WEATHER_SERVICE_URL,
        .timeout_ms = WEATHER_SERVICE_HTTP_TIMEOUT_MS,
        .event_handler = weather_http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        printf("weather: http client init failed\n");
        return false;
    }

    printf("weather: downloading current conditions\n");
    const esp_err_t err = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0U) {
        printf("weather: http failed err=%s status=%d size=%u\n", esp_err_to_name(err),
               status_code, (unsigned)response.length);
        return false;
    }
    return weather_parse(response.data, model);
}
