#include "network_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_config.h"
#include "nvs_flash.h"

#define NETWORK_CONNECT_TIMEOUT_MS 10000U
#define NETWORK_CONNECT_PROGRESS_MS 500U
#define NETWORK_SNTP_TIMEOUT_MS 15000U
#define NETWORK_SNTP_POLL_MS 100U
#define NETWORK_NTP_SERVER "ntp.aliyun.com"
#define NETWORK_TIMEZONE "CST-8"

#define NETWORK_CONNECTED_BIT BIT0
#define NETWORK_DISCONNECTED_BIT BIT1
#define NETWORK_EAP_CREDENTIAL_MAX_LEN 128U

static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_data_mutex;
static network_service_data_t s_data = {
    .wifi_text = "Wi-Fi 未连接",
    .time = "时间未校准",
};
static bool s_wifi_initialized;
static bool s_wifi_started;
static char s_eap_identity[NETWORK_EAP_CREDENTIAL_MAX_LEN];
static char s_eap_username[NETWORK_EAP_CREDENTIAL_MAX_LEN];
static char s_eap_password[NETWORK_EAP_CREDENTIAL_MAX_LEN];

static bool network_uses_enterprise(void)
{
    const char *const username = NETWORK_WIFI_USERNAME;
    return username != NULL && username[0] != '\0';
}

static const char *network_disconnect_reason_text(int reason)
{
    switch (reason) {
    case 2:
        return "认证过期";
    case 15:
    case 204:
        return "WPA 握手超时";
    case 201:
        return "未发现热点";
    case 202:
        return "认证失败";
    case 203:
        return "关联失败";
    default:
        return "未分类原因";
    }
}

static void network_copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination_size == 0U) {
        return;
    }
    snprintf(destination, destination_size, "%s", source != NULL ? source : "");
}

static void network_update_wifi_state(bool connected, const char *ip_text)
{
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    s_data.wifi_connected = connected;
    network_copy_text(s_data.wifi_text, sizeof(s_data.wifi_text),
                      connected ? (ip_text != NULL ? ip_text : "已连接") : "未连接");
    xSemaphoreGive(s_data_mutex);
}

static void network_update_time_cache(bool synced_now)
{
    static const char *const weekdays[] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六",
    };
    time_t now = 0;
    struct tm local_time = {0};

    time(&now);
    localtime_r(&now, &local_time);
    const bool system_time_valid = local_time.tm_year >= (2016 - 1900);

    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    if (!system_time_valid && !synced_now) {
        s_data.time_synced = false;
        s_data.date[0] = '\0';
        s_data.weekday[0] = '\0';
        memset(&s_data.local_time, 0, sizeof(s_data.local_time));
        network_copy_text(s_data.time, sizeof(s_data.time), "时间未校准");
    } else {
        s_data.time_synced = true;
        s_data.local_time = local_time;
        strftime(s_data.date, sizeof(s_data.date), "%Y-%m-%d", &local_time);
        network_copy_text(s_data.weekday, sizeof(s_data.weekday), weekdays[local_time.tm_wday]);
        strftime(s_data.time, sizeof(s_data.time), "%H:%M:%S", &local_time);
    }
    xSemaphoreGive(s_data_mutex);
}

static void network_event_handler(void *argument, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        const int reason = event != NULL ? event->reason : -1;

        printf("wifi: disconnected, reason=%d (%s)\n", reason,
               network_disconnect_reason_text(reason));
        network_update_wifi_state(false, NULL);
        xEventGroupSetBits(s_wifi_events, NETWORK_DISCONNECTED_BIT);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip_text[APP_MODEL_WIFI_TEXT_LEN] = {0};

        snprintf(ip_text, sizeof(ip_text), IPSTR, IP2STR(&event->ip_info.ip));
        printf("wifi: got ip %s\n", ip_text);
        network_update_wifi_state(true, ip_text);
        xEventGroupSetBits(s_wifi_events, NETWORK_CONNECTED_BIT);
    }
}

static esp_err_t network_prepare_wifi(void)
{
    if (s_wifi_initialized) {
        if (!s_wifi_started) {
            const esp_err_t start_err = esp_wifi_start();
            if (start_err != ESP_OK) {
                return start_err;
            }
            s_wifi_started = true;
        }
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     network_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         network_event_handler, NULL);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        s_wifi_initialized = true;
        s_wifi_started = true;
    }
    return err;
}

static esp_err_t network_configure_enterprise(void)
{
    const char *const configured_identity = NETWORK_WIFI_IDENTITY;

    /* 用户可将 NETWORK_WIFI_USERNAME 定义为 0；复制后可安全处理两种配置。 */
    network_copy_text(s_eap_username, sizeof(s_eap_username), NETWORK_WIFI_USERNAME);
    network_copy_text(s_eap_identity, sizeof(s_eap_identity),
                      configured_identity != NULL ? configured_identity : s_eap_username);
    network_copy_text(s_eap_password, sizeof(s_eap_password), NETWORK_WIFI_PASSWORD);
    if (s_eap_username[0] == '\0' || s_eap_identity[0] == '\0' || s_eap_password[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_eap_client_set_identity((const unsigned char *)s_eap_identity,
                                                (int)strlen(s_eap_identity));
    if (err == ESP_OK) {
        err = esp_eap_client_set_username((const unsigned char *)s_eap_username,
                                          (int)strlen(s_eap_username));
    }
    if (err == ESP_OK) {
        err = esp_eap_client_set_password((const unsigned char *)s_eap_password,
                                          (int)strlen(s_eap_password));
    }
    if (err == ESP_OK && NETWORK_WIFI_EAP_USE_DEFAULT_CERT_BUNDLE != 0) {
        err = esp_eap_client_use_default_cert_bundle(true);
    }
    if (err == ESP_OK && NETWORK_WIFI_EAP_METHOD == NETWORK_WIFI_EAP_METHOD_PEAP) {
        err = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP);
    }
    if (err == ESP_OK && NETWORK_WIFI_EAP_METHOD == NETWORK_WIFI_EAP_METHOD_TTLS) {
        err = esp_eap_client_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
        if (err == ESP_OK) {
            err = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_TTLS);
        }
    }
    if (err == ESP_OK && NETWORK_WIFI_EAP_METHOD != NETWORK_WIFI_EAP_METHOD_PEAP &&
        NETWORK_WIFI_EAP_METHOD != NETWORK_WIFI_EAP_METHOD_TTLS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return err == ESP_OK ? esp_wifi_sta_enterprise_enable() : err;
}

static bool network_connect_wifi(void)
{
    wifi_config_t wifi_config = {0};
    const bool enterprise = network_uses_enterprise();
    network_copy_text((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), NETWORK_WIFI_SSID);
    if (enterprise) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    } else {
        network_copy_text((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password),
                          NETWORK_WIFI_PASSWORD);
    }
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.failure_retry_cnt = 0;

    network_update_wifi_state(false, NULL);
    xEventGroupClearBits(s_wifi_events, NETWORK_CONNECTED_BIT | NETWORK_DISCONNECTED_BIT);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        printf("wifi: set config failed: %s\n", esp_err_to_name(err));
        return false;
    }

    if (enterprise) {
        err = network_configure_enterprise();
        if (err != ESP_OK) {
            printf("wifi: enterprise setup failed: %s\n", esp_err_to_name(err));
            return false;
        }
        printf("wifi: enterprise authentication configured\n");
    }

    printf("wifi: connecting to %s%s\n", NETWORK_WIFI_SSID,
           enterprise ? " with WPA2-Enterprise" : "");
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        printf("wifi: connect start failed: %s\n", esp_err_to_name(err));
        return false;
    }

    for (uint32_t elapsed_ms = 0; elapsed_ms < NETWORK_CONNECT_TIMEOUT_MS;
         elapsed_ms += NETWORK_CONNECT_PROGRESS_MS) {
        const EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                                      NETWORK_CONNECTED_BIT | NETWORK_DISCONNECTED_BIT,
                                                      pdFALSE, pdFALSE,
                                                      pdMS_TO_TICKS(NETWORK_CONNECT_PROGRESS_MS));
        if ((bits & NETWORK_CONNECTED_BIT) != 0U) {
            return true;
        }
        if ((bits & NETWORK_DISCONNECTED_BIT) != 0U) {
            return false;
        }
        printf("wifi: connecting... %lu/%u ms\n",
               (unsigned long)(elapsed_ms + NETWORK_CONNECT_PROGRESS_MS),
               NETWORK_CONNECT_TIMEOUT_MS);
    }

    printf("wifi: connect timeout\n");
    (void)esp_wifi_disconnect();
    return false;
}

static bool network_sync_time(void)
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

    const TickType_t start_tick = xTaskGetTickCount();
    while (xTaskGetTickCount() - start_tick < pdMS_TO_TICKS(NETWORK_SNTP_TIMEOUT_MS)) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            esp_sntp_stop();
            network_update_time_cache(true);
            network_service_data_t snapshot = {0};
            network_service_get_snapshot(&snapshot);
            printf("ntp: synced %s %s %s\n", snapshot.date, snapshot.weekday, snapshot.time);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(NETWORK_SNTP_POLL_MS));
    }

    esp_sntp_stop();
    network_update_time_cache(false);
    printf("ntp: sync timeout\n");
    return false;
}

esp_err_t network_service_init(void)
{
    if (s_data_mutex != NULL) {
        return ESP_OK;
    }
    s_data_mutex = xSemaphoreCreateMutex();
    return s_data_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

bool network_service_update_once(void)
{
    network_update_time_cache(false);
    const esp_err_t err = network_prepare_wifi();
    if (err != ESP_OK) {
        printf("network: init failed: %s\n", esp_err_to_name(err));
        return false;
    }
    if (!network_connect_wifi()) {
        return false;
    }
    return network_sync_time();
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
