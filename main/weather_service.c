#include "weather_service.h"

#include <stdio.h>
#include <string.h>

#include <cJSON.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"

/* 厦门大学附近坐标；仅请求当前温度和天气码，减少流量和解析开销。 */
#define WEATHER_SERVICE_URL \
    "https://api.open-meteo.com/v1/forecast?latitude=24.4410&longitude=118.0957" \
    "&current=temperature_2m,weather_code&timezone=Asia%2FShanghai"
#define WEATHER_SERVICE_HTTP_TIMEOUT_MS 8000
#define WEATHER_SERVICE_RESPONSE_MAX 512

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} weather_http_response_t;

static char s_response_buffer[WEATHER_SERVICE_RESPONSE_MAX];

static esp_err_t weather_service_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    weather_http_response_t *response = event->user_data;
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

static const char *weather_service_text_from_code(int weather_code)
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
        /* 降水、雷暴与降雪先统一显示为“雨”，控制仿真阶段的字模规模。 */
        return "雨";
    }
}

static bool weather_service_parse(const char *json_text, app_model_t *model)
{
    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        printf("weather: json parse failed\n");
        return false;
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temperature = current != NULL ? cJSON_GetObjectItemCaseSensitive(current, "temperature_2m") : NULL;
    const cJSON *weather_code = current != NULL ? cJSON_GetObjectItemCaseSensitive(current, "weather_code") : NULL;
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(weather_code)) {
        printf("weather: unsupported json schema\n");
        cJSON_Delete(root);
        return false;
    }

    const char *weather_text = weather_service_text_from_code(weather_code->valueint);
    snprintf(model->calendar.weather, sizeof(model->calendar.weather), "厦门 %.0fC %s",
             temperature->valuedouble, weather_text);
    printf("weather: %s\n", model->calendar.weather);
    cJSON_Delete(root);
    return true;
}

bool weather_service_refresh(app_model_t *model)
{
    if (model == NULL) {
        return false;
    }

    weather_http_response_t response = {
        .data = s_response_buffer,
        .capacity = sizeof(s_response_buffer),
        .length = 0,
        .overflow = false,
    };
    response.data[0] = '\0';

    const esp_http_client_config_t config = {
        .url = WEATHER_SERVICE_URL,
        .timeout_ms = WEATHER_SERVICE_HTTP_TIMEOUT_MS,
        .event_handler = weather_service_http_event,
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
    if (err != ESP_OK || status_code != 200 || response.overflow || response.length == 0) {
        printf("weather: http failed err=%s status=%d size=%u\n", esp_err_to_name(err), status_code,
               (unsigned)response.length);
        return false;
    }

    return weather_service_parse(response.data, model);
}
