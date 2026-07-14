#include "xiaozhi_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "network_service.h"

/* 官方开源固件当前默认使用此接口获取设备激活状态与通信参数。 */
#define XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_OTA_TIMEOUT_MS 15000U
#define XIAOZHI_WSS_CONNECT_TIMEOUT_MS 10000U
#define XIAOZHI_WSS_HELLO_TIMEOUT_MS 10000U
#define XIAOZHI_WSS_READ_SLICE_MS 1000U
#define XIAOZHI_CLIENT_TASK_STACK_SIZE 8192U
#define XIAOZHI_CLIENT_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

#define XIAOZHI_RESPONSE_BUFFER_SIZE 4096U
#define XIAOZHI_URL_MAX_LEN 256U
#define XIAOZHI_TOKEN_MAX_LEN 384U
#define XIAOZHI_UUID_LEN 37U
#define XIAOZHI_DEVICE_ID_LEN 18U
#define XIAOZHI_NVS_NAMESPACE "xiaozhi"
#define XIAOZHI_NVS_CLIENT_ID_KEY "client_id"
#define XIAOZHI_NVS_WS_URL_KEY "ws_url"
#define XIAOZHI_NVS_WS_TOKEN_KEY "ws_token"
#define XIAOZHI_NVS_WS_VERSION_KEY "ws_ver"

typedef struct {
    char url[XIAOZHI_URL_MAX_LEN];
    char token[XIAOZHI_TOKEN_MAX_LEN];
    uint32_t version;
} xiaozhi_websocket_config_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflowed;
} xiaozhi_response_buffer_t;

typedef enum {
    XIAOZHI_CONFIG_FETCH_FAILED = 0,
    XIAOZHI_CONFIG_FETCH_ACTIVATION_REQUIRED,
    XIAOZHI_CONFIG_FETCH_SUCCEEDED,
} xiaozhi_config_fetch_result_t;

static TaskHandle_t s_session_task;
static volatile bool s_stop_requested;

static bool xiaozhi_copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U || source == NULL) {
        return false;
    }
    const int length = snprintf(destination, destination_size, "%s", source);
    return length >= 0 && (size_t)length < destination_size;
}

static void xiaozhi_get_device_id(char device_id[XIAOZHI_DEVICE_ID_LEN])
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, XIAOZHI_DEVICE_ID_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void xiaozhi_generate_uuid(char uuid[XIAOZHI_UUID_LEN])
{
    uint8_t bytes[16] = {0};
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0FU) | 0x40U;
    bytes[8] = (bytes[8] & 0x3FU) | 0x80U;

    snprintf(uuid, XIAOZHI_UUID_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
             bytes[15]);
}

static bool xiaozhi_load_or_create_client_id(char client_id[XIAOZHI_UUID_LEN])
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(XIAOZHI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        printf("xiaozhi: open NVS failed: %s\n", esp_err_to_name(err));
        return false;
    }

    size_t length = XIAOZHI_UUID_LEN;
    err = nvs_get_str(handle, XIAOZHI_NVS_CLIENT_ID_KEY, client_id, &length);
    if (err == ESP_OK && length == XIAOZHI_UUID_LEN) {
        nvs_close(handle);
        return true;
    }

    xiaozhi_generate_uuid(client_id);
    err = nvs_set_str(handle, XIAOZHI_NVS_CLIENT_ID_KEY, client_id);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        printf("xiaozhi: save client id failed: %s\n", esp_err_to_name(err));
        return false;
    }

    printf("xiaozhi: generated client id %s\n", client_id);
    return true;
}

static bool xiaozhi_load_websocket_config(xiaozhi_websocket_config_t *config)
{
    if (config == NULL) {
        return false;
    }
    *config = (xiaozhi_websocket_config_t){0};

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(XIAOZHI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t url_length = sizeof(config->url);
    size_t token_length = sizeof(config->token);
    err = nvs_get_str(handle, XIAOZHI_NVS_WS_URL_KEY, config->url, &url_length);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, XIAOZHI_NVS_WS_TOKEN_KEY, config->token, &token_length);
    }
    if (err == ESP_OK) {
        err = nvs_get_u32(handle, XIAOZHI_NVS_WS_VERSION_KEY, &config->version);
    }
    nvs_close(handle);

    if (err != ESP_OK || config->url[0] == '\0') {
        *config = (xiaozhi_websocket_config_t){0};
        return false;
    }
    if (config->version == 0U) {
        config->version = 1U;
    }
    return true;
}

static bool xiaozhi_save_websocket_config(const xiaozhi_websocket_config_t *config)
{
    if (config == NULL || config->url[0] == '\0') {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(XIAOZHI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, XIAOZHI_NVS_WS_URL_KEY, config->url);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, XIAOZHI_NVS_WS_TOKEN_KEY, config->token);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, XIAOZHI_NVS_WS_VERSION_KEY, config->version);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        printf("xiaozhi: save websocket config failed: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

static esp_err_t xiaozhi_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == NULL || event->data == NULL) {
        return ESP_OK;
    }

    xiaozhi_response_buffer_t *response = (xiaozhi_response_buffer_t *)event->user_data;
    if (response->length + event->data_len >= response->capacity) {
        response->overflowed = true;
        return ESP_FAIL;
    }
    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static char *xiaozhi_create_ota_payload(const char *client_id, const char *device_id)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    cJSON *application = cJSON_CreateObject();
    cJSON *board = cJSON_CreateObject();
    cJSON *display = cJSON_CreateObject();
    if (root == NULL || application == NULL || board == NULL || display == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(application);
        cJSON_Delete(board);
        cJSON_Delete(display);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", "zh-CN");
    cJSON_AddNumberToObject(root, "flash_size", 16 * 1024 * 1024);
    cJSON_AddNumberToObject(root, "minimum_free_heap_size", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "mac_address", device_id);
    cJSON_AddStringToObject(root, "uuid", client_id);
    cJSON_AddStringToObject(root, "chip_model_name", "esp32s3");

    cJSON_AddStringToObject(application, "name", app->project_name);
    cJSON_AddStringToObject(application, "version", app->version);
    cJSON_AddStringToObject(application, "idf_version", app->idf_ver);
    cJSON_AddItemToObject(root, "application", application);

    cJSON_AddStringToObject(board, "type", "electronic_nameplate");
    cJSON_AddStringToObject(board, "name", "Electronic Nameplate");
    cJSON_AddItemToObject(root, "board", board);

    cJSON_AddBoolToObject(display, "monochrome", false);
    cJSON_AddNumberToObject(display, "width", 400);
    cJSON_AddNumberToObject(display, "height", 300);
    cJSON_AddItemToObject(root, "display", display);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static bool xiaozhi_json_get_string(const cJSON *object, const char *name,
                                    char *destination, size_t destination_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != NULL &&
           xiaozhi_copy_text(destination, destination_size, item->valuestring);
}

static xiaozhi_config_fetch_result_t xiaozhi_fetch_websocket_config(
    const char *client_id, const char *device_id, xiaozhi_websocket_config_t *config)
{
    char *response_data = calloc(1U, XIAOZHI_RESPONSE_BUFFER_SIZE);
    if (response_data == NULL) {
        return XIAOZHI_CONFIG_FETCH_FAILED;
    }
    xiaozhi_response_buffer_t response = {
        .data = response_data,
        .capacity = XIAOZHI_RESPONSE_BUFFER_SIZE,
    };
    char *payload = xiaozhi_create_ota_payload(client_id, device_id);
    if (payload == NULL) {
        free(response_data);
        return XIAOZHI_CONFIG_FETCH_FAILED;
    }

    char user_agent[96] = {0};
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(user_agent, sizeof(user_agent), "electronic-nameplate/%s", app->version);
    const esp_http_client_config_t http_config = {
        .url = XIAOZHI_OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = XIAOZHI_OTA_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = xiaozhi_http_event_handler,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        cJSON_free(payload);
        free(response_data);
        return XIAOZHI_CONFIG_FETCH_FAILED;
    }

    (void)esp_http_client_set_header(client, "Activation-Version", "1");
    (void)esp_http_client_set_header(client, "Device-Id", device_id);
    (void)esp_http_client_set_header(client, "Client-Id", client_id);
    (void)esp_http_client_set_header(client, "User-Agent", user_agent);
    (void)esp_http_client_set_header(client, "Accept-Language", "zh-CN");
    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));

    printf("xiaozhi: requesting official configuration\n");
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    cJSON_free(payload);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || response.overflowed) {
        printf("xiaozhi: official configuration failed: err=%s status=%d\n",
               esp_err_to_name(err), status);
        free(response_data);
        return XIAOZHI_CONFIG_FETCH_FAILED;
    }

    cJSON *root = cJSON_Parse(response.data);
    free(response_data);
    if (root == NULL) {
        printf("xiaozhi: official configuration JSON is invalid\n");
        return XIAOZHI_CONFIG_FETCH_FAILED;
    }

    const cJSON *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    const cJSON *activation_code = cJSON_IsObject(activation) ?
                                       cJSON_GetObjectItemCaseSensitive(activation, "code") : NULL;
    if (cJSON_IsString(activation_code) && activation_code->valuestring != NULL) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(activation, "message");
        printf("xiaozhi: activation code=%s\n", activation_code->valuestring);
        if (cJSON_IsString(message) && message->valuestring != NULL) {
            printf("xiaozhi: activation hint=%s\n", message->valuestring);
        }
        printf("xiaozhi: add this device in the Xiaozhi console, then press BOOT again\n");
        cJSON_Delete(root);
        return XIAOZHI_CONFIG_FETCH_ACTIVATION_REQUIRED;
    }

    const cJSON *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    bool valid = cJSON_IsObject(websocket) &&
                 xiaozhi_json_get_string(websocket, "url", config->url, sizeof(config->url));
    if (valid) {
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(websocket, "version");
        config->version = cJSON_IsNumber(version) && version->valueint > 0 ?
                              (uint32_t)version->valueint : 1U;
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(websocket, "token");
        if (cJSON_IsString(token) && token->valuestring != NULL) {
            valid = xiaozhi_copy_text(config->token, sizeof(config->token), token->valuestring);
        }
    }
    cJSON_Delete(root);

    if (!valid) {
        printf("xiaozhi: official response has no usable websocket configuration\n");
        return XIAOZHI_CONFIG_FETCH_FAILED;
    }
    return xiaozhi_save_websocket_config(config) ? XIAOZHI_CONFIG_FETCH_SUCCEEDED :
                                                   XIAOZHI_CONFIG_FETCH_FAILED;
}

static bool xiaozhi_parse_wss_url(const char *url, char *host, size_t host_size,
                                  char *path, size_t path_size, int *port)
{
    static const char secure_prefix[] = "wss://";
    if (url == NULL || host == NULL || path == NULL || port == NULL ||
        strncmp(url, secure_prefix, sizeof(secure_prefix) - 1U) != 0) {
        return false;
    }

    const char *authority = url + sizeof(secure_prefix) - 1U;
    const char *path_start = strchr(authority, '/');
    const char *authority_end = path_start != NULL ? path_start : authority + strlen(authority);
    const char *port_separator = NULL;
    for (const char *cursor = authority; cursor < authority_end; ++cursor) {
        if (*cursor == ':') {
            port_separator = cursor;
        }
    }

    const char *host_end = port_separator != NULL ? port_separator : authority_end;
    const size_t host_length = (size_t)(host_end - authority);
    if (host_length == 0U || host_length >= host_size) {
        return false;
    }
    memcpy(host, authority, host_length);
    host[host_length] = '\0';

    *port = 443;
    if (port_separator != NULL) {
        char port_text[8] = {0};
        const size_t port_length = (size_t)(authority_end - port_separator - 1);
        if (port_length == 0U || port_length >= sizeof(port_text)) {
            return false;
        }
        memcpy(port_text, port_separator + 1, port_length);
        *port = atoi(port_text);
        if (*port <= 0 || *port > 65535) {
            return false;
        }
    }

    return xiaozhi_copy_text(path, path_size, path_start != NULL ? path_start : "/");
}

static bool xiaozhi_server_hello_is_valid(const char *message, size_t length)
{
    cJSON *root = cJSON_ParseWithLength(message, length);
    if (root == NULL) {
        return false;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    const bool valid = cJSON_IsString(type) && cJSON_IsString(transport) &&
                       strcmp(type->valuestring, "hello") == 0 &&
                       strcmp(transport->valuestring, "websocket") == 0;
    if (valid) {
        const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        const cJSON *audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        const cJSON *sample_rate = cJSON_IsObject(audio_params) ?
                                       cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate") : NULL;
        printf("xiaozhi: server hello received, session=%s sample_rate=%d\n",
               cJSON_IsString(session_id) ? session_id->valuestring : "(none)",
               cJSON_IsNumber(sample_rate) ? sample_rate->valueint : 0);
    }
    cJSON_Delete(root);
    return valid;
}

static bool xiaozhi_open_websocket(const char *client_id, const char *device_id,
                                   const xiaozhi_websocket_config_t *config)
{
    char host[128] = {0};
    char path[XIAOZHI_URL_MAX_LEN] = {0};
    int port = 0;
    if (!xiaozhi_parse_wss_url(config->url, host, sizeof(host), path, sizeof(path), &port)) {
        printf("xiaozhi: unsupported websocket url %s\n", config->url);
        return false;
    }

    esp_transport_list_handle_t transport_list = esp_transport_list_init();
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    esp_transport_handle_t websocket = ssl != NULL ? esp_transport_ws_init(ssl) : NULL;
    if (transport_list == NULL || ssl == NULL || websocket == NULL) {
        if (transport_list != NULL) {
            esp_transport_list_destroy(transport_list);
        } else {
            if (websocket != NULL) {
                esp_transport_destroy(websocket);
            }
            if (ssl != NULL) {
                esp_transport_destroy(ssl);
            }
        }
        printf("xiaozhi: websocket transport allocation failed\n");
        return false;
    }
    (void)esp_transport_list_add(transport_list, ssl, "wss");
    (void)esp_transport_list_add(transport_list, websocket, "websocket");
    esp_transport_ssl_crt_bundle_attach(ssl, esp_crt_bundle_attach);
    esp_transport_ws_set_path(websocket, path);

    char headers[512] = {0};
    snprintf(headers, sizeof(headers), "Protocol-Version: %lu\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
             (unsigned long)config->version, device_id, client_id);
    (void)esp_transport_ws_set_headers(websocket, headers);
    if (config->token[0] != '\0') {
        char authorization[XIAOZHI_TOKEN_MAX_LEN + 8U] = {0};
        const char *token = config->token;
        if (strchr(token, ' ') == NULL) {
            snprintf(authorization, sizeof(authorization), "Bearer %s", token);
            token = authorization;
        }
        (void)esp_transport_ws_set_auth(websocket, token);
    }

    bool connected = false;
    printf("xiaozhi: websocket connecting to %s:%d\n", host, port);
    if (esp_transport_connect(websocket, host, port, XIAOZHI_WSS_CONNECT_TIMEOUT_MS) < 0) {
        printf("xiaozhi: websocket connection failed, status=%d errno=%d\n",
               esp_transport_ws_get_upgrade_request_status(websocket),
               esp_transport_get_errno(websocket));
        goto cleanup;
    }

    const char hello[] =
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}";
    if (esp_transport_ws_send_raw(websocket,
                                  WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                                  hello, (int)strlen(hello), XIAOZHI_WSS_CONNECT_TIMEOUT_MS) !=
        (int)strlen(hello)) {
        printf("xiaozhi: websocket hello send failed\n");
        goto cleanup;
    }

    const TickType_t started = xTaskGetTickCount();
    char received[1024] = {0};
    while (!s_stop_requested &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(XIAOZHI_WSS_HELLO_TIMEOUT_MS)) {
        const int received_length = esp_transport_read(websocket, received, sizeof(received) - 1U,
                                                        XIAOZHI_WSS_READ_SLICE_MS);
        if (received_length < 0) {
            printf("xiaozhi: websocket read failed\n");
            break;
        }
        if (received_length == 0) {
            continue;
        }
        if (esp_transport_ws_get_read_opcode(websocket) != WS_TRANSPORT_OPCODES_TEXT) {
            continue;
        }
        received[received_length] = '\0';
        if (xiaozhi_server_hello_is_valid(received, (size_t)received_length)) {
            connected = true;
            break;
        }
    }
    if (!connected && !s_stop_requested) {
        printf("xiaozhi: server hello timeout\n");
    }

cleanup:
    (void)esp_transport_close(websocket);
    esp_transport_list_destroy(transport_list);
    return connected;
}

static bool xiaozhi_wait_for_network(void)
{
    network_service_data_t network = {0};
    network_service_get_snapshot(&network);
    if (network.wifi_connected) {
        return true;
    }

    printf("xiaozhi: Wi-Fi is not ready; wait for the normal network update to complete\n");
    const TickType_t started = xTaskGetTickCount();
    while (!s_stop_requested &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(XIAOZHI_OTA_TIMEOUT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(200));
        network_service_get_snapshot(&network);
        if (network.wifi_connected) {
            return true;
        }
    }
    return false;
}

static void xiaozhi_session_task(void *argument)
{
    (void)argument;

    char client_id[XIAOZHI_UUID_LEN] = {0};
    char device_id[XIAOZHI_DEVICE_ID_LEN] = {0};
    xiaozhi_get_device_id(device_id);

    if (!xiaozhi_wait_for_network() || !xiaozhi_load_or_create_client_id(client_id)) {
        printf("xiaozhi: session cannot start\n");
        goto done;
    }

    xiaozhi_websocket_config_t config = {0};
    const xiaozhi_config_fetch_result_t config_result =
        xiaozhi_fetch_websocket_config(client_id, device_id, &config);
    if (config_result == XIAOZHI_CONFIG_FETCH_ACTIVATION_REQUIRED) {
        goto done;
    }
    if (config_result != XIAOZHI_CONFIG_FETCH_SUCCEEDED) {
        if (s_stop_requested || !xiaozhi_load_websocket_config(&config)) {
            goto done;
        }
        printf("xiaozhi: using cached websocket configuration\n");
    }
    if (!s_stop_requested) {
        const bool success = xiaozhi_open_websocket(client_id, device_id, &config);
        printf("xiaozhi: handshake %s\n", success ? "succeeded" : "failed");
    }

done:
    s_session_task = NULL;
    s_stop_requested = false;
    vTaskDelete(NULL);
}

bool xiaozhi_client_start_session(void)
{
    if (s_session_task != NULL) {
        printf("xiaozhi: session is already running\n");
        return false;
    }

    s_stop_requested = false;
    if (xTaskCreate(xiaozhi_session_task, "xiaozhi_session", XIAOZHI_CLIENT_TASK_STACK_SIZE,
                    NULL, XIAOZHI_CLIENT_TASK_PRIORITY, &s_session_task) != pdPASS) {
        s_session_task = NULL;
        printf("xiaozhi: session task creation failed\n");
        return false;
    }
    return true;
}

void xiaozhi_client_stop_session(void)
{
    s_stop_requested = true;
}

bool xiaozhi_client_is_busy(void)
{
    return s_session_task != NULL;
}
