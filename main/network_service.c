#include "network_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define NETWORK_WIFI_SSID "Wokwi-GUEST"
#define NETWORK_WIFI_PASSWORD ""
#define NETWORK_WIFI_CHANNEL 6
#define NETWORK_CONNECT_TIMEOUT_MS 10000
#define NETWORK_CONNECT_PROGRESS_MS 500
#define NETWORK_RETRY_INTERVAL_MS 10000
#define NETWORK_ONLINE_POLL_MS 1000
#define NETWORK_TIME_REFRESH_INTERVAL_MS (10 * 60 * 1000)
#define NETWORK_SNTP_TIMEOUT_MS 15000
#define NETWORK_SNTP_POLL_MS 100
#define NETWORK_NTP_SERVER "ntp.aliyun.com"
#define NETWORK_TIMEZONE "CST-8"
#define NETWORK_TASK_STACK_SIZE 8192
#define NETWORK_TASK_PRIORITY 4

#define NETWORK_CONNECTED_BIT BIT0
#define NETWORK_DISCONNECTED_BIT BIT1

static const char *TAG = "network";
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_data_mutex;
static esp_netif_t *s_sta_netif;
static network_service_data_t s_data = {
    .retry_interval_ms = NETWORK_RETRY_INTERVAL_MS,
    .last_error = "network not started",
};
static bool s_initialized;

static bool network_service_is_wifi_connected(void)
{
    bool connected;

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    connected = s_data.wifi_connected;
    xSemaphoreGive(s_data_mutex);

    return connected;
}

static void network_service_copy_text(char *dst, size_t dst_size, const char *src)
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

static void network_service_update_error(const char *message)
{
    if (s_data_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    network_service_copy_text(s_data.last_error, sizeof(s_data.last_error), message);
    xSemaphoreGive(s_data_mutex);
}

static void network_service_update_wifi_state(bool connected, const char *ip_text)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_data.wifi_connected = connected;
    if (connected && ip_text != NULL) {
        network_service_copy_text(s_data.ip_text, sizeof(s_data.ip_text), ip_text);
    } else if (!connected) {
        s_data.ip_text[0] = '\0';
    }
    xSemaphoreGive(s_data_mutex);
}

static time_t network_service_next_minute_epoch(time_t now)
{
    if (now <= 0) {
        return 0;
    }
    return now - (now % 60) + 60;
}

static void network_service_update_time_cache(bool synced_now)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    char date_text[NETWORK_SERVICE_DATE_TEXT_LEN] = {0};
    char weekday_text[NETWORK_SERVICE_WEEKDAY_TEXT_LEN] = {0};
    char time_text[NETWORK_SERVICE_TIME_TEXT_LEN] = {0};
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    time(&now);
    localtime_r(&now, &timeinfo);

    const bool system_time_valid = timeinfo.tm_year >= (2016 - 1900);
    if (!system_time_valid && !synced_now) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        if (!s_data.time_synced) {
            s_data.date_text[0] = '\0';
            s_data.weekday_text[0] = '\0';
            network_service_copy_text(s_data.time_text, sizeof(s_data.time_text), "时间未校准");
            s_data.next_refresh_epoch = 0;
        }
        xSemaphoreGive(s_data_mutex);
        return;
    }

    strftime(date_text, sizeof(date_text), "%Y-%m-%d", &timeinfo);
    snprintf(weekday_text, sizeof(weekday_text), "%s", weekdays[timeinfo.tm_wday]);
    strftime(time_text, sizeof(time_text), "%H:%M:%S", &timeinfo);

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_data.time_synced = true;
    s_data.local_time = timeinfo;
    network_service_copy_text(s_data.date_text, sizeof(s_data.date_text), date_text);
    network_service_copy_text(s_data.weekday_text, sizeof(s_data.weekday_text), weekday_text);
    network_service_copy_text(s_data.time_text, sizeof(s_data.time_text), time_text);
    s_data.next_refresh_epoch = network_service_next_minute_epoch(now);
    xSemaphoreGive(s_data_mutex);
}

static void network_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        printf("wifi: disconnected\n");
        network_service_update_wifi_state(false, NULL);
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, NETWORK_DISCONNECTED_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip_text[NETWORK_SERVICE_IP_TEXT_LEN] = {0};

        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        printf("wifi: got ip %s\n", ip_text);
        network_service_update_wifi_state(true, ip_text);
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, NETWORK_CONNECTED_BIT);
        }
    }
}

static esp_err_t network_service_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t network_service_wifi_init_once(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(network_service_init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif init failed");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                   &network_event_handler, NULL),
                        TAG, "register wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &network_event_handler, NULL),
                        TAG, "register ip handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_initialized = true;
    return ESP_OK;
}

static bool network_service_connect_wifi(void)
{
    wifi_config_t wifi_config = {0};

    network_service_update_wifi_state(false, NULL);
    xEventGroupClearBits(s_wifi_event_group, NETWORK_CONNECTED_BIT | NETWORK_DISCONNECTED_BIT);

    network_service_copy_text((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), NETWORK_WIFI_SSID);
    network_service_copy_text((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), NETWORK_WIFI_PASSWORD);
    wifi_config.sta.channel = NETWORK_WIFI_CHANNEL;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.failure_retry_cnt = 0;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    printf("wifi: connecting to %s on channel %d\n", NETWORK_WIFI_SSID, NETWORK_WIFI_CHANNEL);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        printf("wifi: connect start failed: %s\n", esp_err_to_name(err));
        network_service_update_error("wifi connect start failed");
        return false;
    }

    for (int elapsed_ms = 0; elapsed_ms < NETWORK_CONNECT_TIMEOUT_MS; elapsed_ms += NETWORK_CONNECT_PROGRESS_MS) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               NETWORK_CONNECTED_BIT | NETWORK_DISCONNECTED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(NETWORK_CONNECT_PROGRESS_MS));
        if ((bits & NETWORK_CONNECTED_BIT) != 0) {
            network_service_update_error("");
            return true;
        }

        printf("wifi: connecting... %d/%d ms\n",
               elapsed_ms + NETWORK_CONNECT_PROGRESS_MS, NETWORK_CONNECT_TIMEOUT_MS);
    }

    printf("wifi: connect timeout\n");
    network_service_update_error("wifi connect timeout");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    return false;
}

static bool network_service_sync_time(void)
{
    setenv("TZ", NETWORK_TIMEZONE, 1);
    tzset();

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NETWORK_NTP_SERVER);

    printf("ntp: syncing with %s\n", NETWORK_NTP_SERVER);
    esp_sntp_init();

    bool synced = false;
    const TickType_t start_tick = xTaskGetTickCount();
    while (xTaskGetTickCount() - start_tick < pdMS_TO_TICKS(NETWORK_SNTP_TIMEOUT_MS)) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            synced = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(NETWORK_SNTP_POLL_MS));
    }

    esp_sntp_stop();

    if (!synced) {
        printf("ntp: sync timeout\n");
        network_service_update_error("ntp sync failed");
        network_service_update_time_cache(false);
        return false;
    }

    network_service_update_time_cache(true);

    network_service_data_t snapshot;
    network_service_get_snapshot(&snapshot);
    printf("ntp: synced %s %s %s, next refresh epoch=%lld\n",
           snapshot.date_text, snapshot.weekday_text, snapshot.time_text,
           (long long)snapshot.next_refresh_epoch);
    return true;
}

static void network_service_task(void *arg)
{
    (void)arg;
    TickType_t last_time_attempt_tick = 0;

    esp_err_t err = network_service_wifi_init_once();
    if (err != ESP_OK) {
        printf("network: init failed: %s\n", esp_err_to_name(err));
        network_service_update_error("network init failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        if (!network_service_is_wifi_connected()) {
            if (!network_service_connect_wifi()) {
                network_service_update_time_cache(false);
                printf("network: retry in %d ms\n", NETWORK_RETRY_INTERVAL_MS);
                vTaskDelay(pdMS_TO_TICKS(NETWORK_RETRY_INTERVAL_MS));
                continue;
            }

            last_time_attempt_tick = 0;
        }

        network_service_update_time_cache(false);

        TickType_t now_tick = xTaskGetTickCount();
        if (last_time_attempt_tick == 0 ||
            now_tick - last_time_attempt_tick >= pdMS_TO_TICKS(NETWORK_TIME_REFRESH_INTERVAL_MS)) {
            // 时间同步失败时保留上一次有效时间；未校准时不会伪造日期。
            last_time_attempt_tick = now_tick;
            network_service_sync_time();
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_ONLINE_POLL_MS));
    }
}

esp_err_t network_service_init(void)
{
    if (s_data_mutex == NULL) {
        s_data_mutex = xSemaphoreCreateMutex();
        if (s_data_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreate(network_service_task,
                                "network_service",
                                NETWORK_TASK_STACK_SIZE,
                                NULL,
                                NETWORK_TASK_PRIORITY,
                                NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void network_service_get_snapshot(network_service_data_t *snapshot)
{
    if (snapshot == NULL || s_data_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    *snapshot = s_data;
    xSemaphoreGive(s_data_mutex);
}
