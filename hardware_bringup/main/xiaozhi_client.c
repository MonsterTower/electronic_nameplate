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
#include "lwip/inet.h"
#include "nvs.h"

#include "audio_service.h"
#include "network_service.h"

/* 官方开源固件当前默认使用此接口获取设备激活状态与通信参数。 */
#define XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#define XIAOZHI_OTA_TIMEOUT_MS 15000U
#define XIAOZHI_WSS_CONNECT_TIMEOUT_MS 10000U
#define XIAOZHI_WSS_HELLO_TIMEOUT_MS 10000U
#define XIAOZHI_WSS_READ_SLICE_MS 100U
#define XIAOZHI_CLIENT_TASK_STACK_SIZE 16384U
#define XIAOZHI_CLIENT_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define XIAOZHI_CLIENT_TASK_CORE 1

#define XIAOZHI_RESPONSE_BUFFER_SIZE 4096U
#define XIAOZHI_URL_MAX_LEN 256U
#define XIAOZHI_TOKEN_MAX_LEN 384U
#define XIAOZHI_UUID_LEN 37U
#define XIAOZHI_DEVICE_ID_LEN 18U
#define XIAOZHI_SESSION_ID_MAX_LEN 96U
#define XIAOZHI_WSS_RECEIVE_BUFFER_SIZE 4096U
#define XIAOZHI_BINARY_V2_HEADER_SIZE 16U
#define XIAOZHI_BINARY_V3_HEADER_SIZE 4U
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

typedef struct {
    char session_id[XIAOZHI_SESSION_ID_MAX_LEN];
    uint32_t sample_rate_hz;
    uint32_t frame_duration_ms;
} xiaozhi_server_session_t;

/*
 * esp_transport_read() 不保证一次读完一个 WebSocket 帧。此结构把同一条
 * 文本或二进制消息累计到完整后再交给 JSON、Opus 等上层逻辑处理。
 */
typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t length;
    size_t frame_expected_length;
    size_t frame_received_length;
    int opcode;
    bool started;
} xiaozhi_ws_message_buffer_t;

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

static void xiaozhi_reset_ws_message_buffer(xiaozhi_ws_message_buffer_t *message)
{
    if (message == NULL) {
        return;
    }
    message->length = 0U;
    message->frame_expected_length = 0U;
    message->frame_received_length = 0U;
    message->opcode = WS_TRANSPORT_OPCODES_CONT;
    message->started = false;
}

/* 返回 true 表示当前读到的数据已被接收；message_complete 表示一条消息已完整。 */
static bool xiaozhi_append_ws_read(esp_transport_handle_t websocket,
                                   xiaozhi_ws_message_buffer_t *message,
                                   size_t read_length, bool *message_complete)
{
    if (websocket == NULL || message == NULL || message_complete == NULL ||
        message->data == NULL || read_length == 0U) {
        return false;
    }
    *message_complete = false;

    const int read_opcode = esp_transport_ws_get_read_opcode(websocket);
    if (!message->started) {
        if (read_opcode != WS_TRANSPORT_OPCODES_TEXT && read_opcode != WS_TRANSPORT_OPCODES_BINARY) {
            return false;
        }
        message->opcode = read_opcode;
        message->started = true;
    } else if (message->frame_expected_length == 0U &&
               read_opcode != WS_TRANSPORT_OPCODES_CONT) {
        /* 非分片消息已结束，却没有重置缓存，避免把两条消息拼接。 */
        return false;
    }

    if (message->frame_expected_length == 0U) {
        const int payload_length = esp_transport_ws_get_read_payload_len(websocket);
        if (payload_length <= 0 || (size_t)payload_length >= message->capacity - message->length) {
            return false;
        }
        message->frame_expected_length = (size_t)payload_length;
        message->frame_received_length = 0U;
    }

    if (read_length > message->frame_expected_length - message->frame_received_length ||
        read_length >= message->capacity - message->length) {
        return false;
    }
    message->length += read_length;
    message->frame_received_length += read_length;

    if (message->frame_received_length == message->frame_expected_length) {
        if (esp_transport_ws_get_fin_flag(websocket)) {
            *message_complete = true;
        } else {
            /* WebSocket 分片消息：继续接收下一段 CONT 帧。 */
            message->frame_expected_length = 0U;
            message->frame_received_length = 0U;
        }
    }
    return true;
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

static bool xiaozhi_parse_server_hello(const char *message, size_t length,
                                       xiaozhi_server_session_t *session)
{
    if (session == NULL) {
        return false;
    }
    *session = (xiaozhi_server_session_t){
        .sample_rate_hz = 24000U,
        .frame_duration_ms = 60U,
    };
    cJSON *root = cJSON_ParseWithLength(message, length);
    if (root == NULL) {
        return false;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    bool valid = cJSON_IsString(type) && cJSON_IsString(transport) &&
                 strcmp(type->valuestring, "hello") == 0 &&
                 strcmp(transport->valuestring, "websocket") == 0;
    const cJSON *session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON *audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    const cJSON *sample_rate = cJSON_IsObject(audio_params) ?
                                   cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate") : NULL;
    const cJSON *frame_duration = cJSON_IsObject(audio_params) ?
                                      cJSON_GetObjectItemCaseSensitive(audio_params, "frame_duration") : NULL;
    /* 官方实现将 session_id 视为可选字段，缺省时仍接受 hello 并发送空会话 ID。 */
    if (valid && cJSON_IsString(session_id) &&
        !xiaozhi_copy_text(session->session_id, sizeof(session->session_id),
                           session_id->valuestring)) {
        valid = false;
    }
    if (valid && cJSON_IsNumber(sample_rate) && sample_rate->valueint > 0) {
        session->sample_rate_hz = (uint32_t)sample_rate->valueint;
    }
    if (valid && cJSON_IsNumber(frame_duration) && frame_duration->valueint > 0) {
        session->frame_duration_ms = (uint32_t)frame_duration->valueint;
    }
    if (valid) {
        printf("xiaozhi: server hello received, session=%s sample_rate=%lu frame=%lu ms\n",
               session->session_id[0] != '\0' ? session->session_id : "(none)",
               (unsigned long)session->sample_rate_hz,
               (unsigned long)session->frame_duration_ms);
    }
    cJSON_Delete(root);
    return valid;
}

static bool xiaozhi_send_text(esp_transport_handle_t websocket, const char *message)
{
    const int message_length = (int)strlen(message);
    return esp_transport_ws_send_raw(websocket,
                                     WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                                     message, message_length,
                                     XIAOZHI_WSS_CONNECT_TIMEOUT_MS) == message_length;
}

static uint16_t xiaozhi_read_u16_be(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8U) | data[1];
}

static uint32_t xiaozhi_read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static bool xiaozhi_send_audio(esp_transport_handle_t websocket, uint32_t protocol_version,
                               const uint8_t *opus_packet, size_t opus_length,
                               uint8_t *serialized_packet, size_t serialized_capacity)
{
    size_t header_length = 0U;
    if (opus_packet == NULL || serialized_packet == NULL || opus_length == 0U ||
        opus_length > AUDIO_SERVICE_OPUS_PACKET_MAX_SIZE ||
        serialized_capacity < XIAOZHI_BINARY_V2_HEADER_SIZE + opus_length) {
        return false;
    }

    if (protocol_version == 2U) {
        const uint16_t version = htons(2U);
        const uint32_t timestamp = htonl((uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
        const uint32_t payload_length = htonl((uint32_t)opus_length);
        memset(serialized_packet, 0, XIAOZHI_BINARY_V2_HEADER_SIZE);
        memcpy(serialized_packet, &version, sizeof(version));
        memcpy(serialized_packet + 8U, &timestamp, sizeof(timestamp));
        memcpy(serialized_packet + 12U, &payload_length, sizeof(payload_length));
        header_length = XIAOZHI_BINARY_V2_HEADER_SIZE;
    } else if (protocol_version == 3U) {
        const uint16_t payload_length = htons((uint16_t)opus_length);
        serialized_packet[0] = 0U;
        serialized_packet[1] = 0U;
        memcpy(serialized_packet + 2U, &payload_length, sizeof(payload_length));
        header_length = XIAOZHI_BINARY_V3_HEADER_SIZE;
    } else {
        header_length = 0U;
    }

    memcpy(serialized_packet + header_length, opus_packet, opus_length);
    const int packet_length = (int)(header_length + opus_length);
    return esp_transport_ws_send_raw(websocket,
                                     WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN,
                                     (const char *)serialized_packet, packet_length,
                                     XIAOZHI_WSS_CONNECT_TIMEOUT_MS) == packet_length;
}

static bool xiaozhi_extract_audio_packet(const uint8_t *input, size_t input_length,
                                         uint32_t protocol_version, const uint8_t **opus_packet,
                                         size_t *opus_length)
{
    size_t header_length = 0U;
    size_t payload_length = input_length;
    if (input == NULL || opus_packet == NULL || opus_length == NULL) {
        return false;
    }
    if (protocol_version == 2U) {
        if (input_length < XIAOZHI_BINARY_V2_HEADER_SIZE || xiaozhi_read_u16_be(input) != 2U ||
            xiaozhi_read_u16_be(input + 2U) != 0U) {
            return false;
        }
        header_length = XIAOZHI_BINARY_V2_HEADER_SIZE;
        payload_length = xiaozhi_read_u32_be(input + 12U);
    } else if (protocol_version == 3U) {
        if (input_length < XIAOZHI_BINARY_V3_HEADER_SIZE || input[0] != 0U) {
            return false;
        }
        header_length = XIAOZHI_BINARY_V3_HEADER_SIZE;
        payload_length = xiaozhi_read_u16_be(input + 2U);
    }
    if (payload_length == 0U || payload_length > input_length - header_length) {
        return false;
    }
    *opus_packet = input + header_length;
    *opus_length = payload_length;
    return true;
}

static bool xiaozhi_send_listen_start(esp_transport_handle_t websocket,
                                      const xiaozhi_server_session_t *session)
{
    char message[XIAOZHI_SESSION_ID_MAX_LEN + 80U] = {0};
    snprintf(message, sizeof(message),
             "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}",
             session->session_id);
    return xiaozhi_send_text(websocket, message);
}

static void xiaozhi_handle_text_message(const char *message, size_t length, bool *upload_enabled)
{
    cJSON *root = cJSON_ParseWithLength(message, length);
    if (root == NULL) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "tts") == 0 && cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "start") == 0) {
            *upload_enabled = false;
            audio_service_discard_capture_frames();
            printf("xiaozhi: server TTS started; microphone upload paused\n");
        } else if (strcmp(state->valuestring, "stop") == 0) {
            printf("xiaozhi: server TTS complete; press BOOT to end this session\n");
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "stt") == 0) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            printf("xiaozhi: STT %s\n", text->valuestring);
        }
    }
    cJSON_Delete(root);
}

static bool xiaozhi_open_websocket(const char *client_id, const char *device_id,
                                   const xiaozhi_websocket_config_t *config)
{
    char host[128] = {0};
    char path[XIAOZHI_URL_MAX_LEN] = {0};
    int port = 0;
    uint8_t *received = NULL;
    uint8_t *serialized_packet = NULL;
    audio_service_opus_packet_t *outgoing_packet = NULL;
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
    bool audio_started = false;
    received = malloc(XIAOZHI_WSS_RECEIVE_BUFFER_SIZE);
    serialized_packet = malloc(XIAOZHI_BINARY_V2_HEADER_SIZE + AUDIO_SERVICE_OPUS_PACKET_MAX_SIZE);
    outgoing_packet = malloc(sizeof(*outgoing_packet));
    if (received == NULL || serialized_packet == NULL || outgoing_packet == NULL) {
        printf("xiaozhi: audio session buffer allocation failed\n");
        connected = false;
        goto cleanup;
    }
    xiaozhi_ws_message_buffer_t incoming = {
        .data = received,
        .capacity = XIAOZHI_WSS_RECEIVE_BUFFER_SIZE,
    };
    xiaozhi_reset_ws_message_buffer(&incoming);
    printf("xiaozhi: websocket connecting to %s:%d\n", host, port);
    if (esp_transport_connect(websocket, host, port, XIAOZHI_WSS_CONNECT_TIMEOUT_MS) < 0) {
        printf("xiaozhi: websocket connection failed, status=%d errno=%d\n",
               esp_transport_ws_get_upgrade_request_status(websocket),
               esp_transport_get_errno(websocket));
        goto cleanup;
    }

    const uint32_t protocol_version =
        config->version >= 1U && config->version <= 3U ? config->version : 1U;
    char hello[192] = {0};
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"version\":%lu,\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
             "\"channels\":1,\"frame_duration\":60}}",
             (unsigned long)protocol_version);
    if (!xiaozhi_send_text(websocket, hello)) {
        printf("xiaozhi: websocket hello send failed\n");
        goto cleanup;
    }

    const TickType_t started = xTaskGetTickCount();
    xiaozhi_server_session_t server_session = {0};
    while (!s_stop_requested &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(XIAOZHI_WSS_HELLO_TIMEOUT_MS)) {
        const int received_length = esp_transport_read(
            websocket, (char *)received + incoming.length,
            XIAOZHI_WSS_RECEIVE_BUFFER_SIZE - incoming.length - 1U, XIAOZHI_WSS_READ_SLICE_MS);
        if (received_length < 0) {
            printf("xiaozhi: websocket read failed\n");
            break;
        }
        if (received_length == 0) {
            continue;
        }
        bool message_complete = false;
        if (!xiaozhi_append_ws_read(websocket, &incoming, (size_t)received_length,
                                    &message_complete)) {
            printf("xiaozhi: invalid handshake frame opcode=%d length=%d\n",
                   esp_transport_ws_get_read_opcode(websocket), received_length);
            xiaozhi_reset_ws_message_buffer(&incoming);
            continue;
        }
        if (!message_complete) {
            continue;
        }
        received[incoming.length] = '\0';
        if (incoming.opcode != WS_TRANSPORT_OPCODES_TEXT) {
            printf("xiaozhi: ignored handshake frame opcode=%d length=%u\n", incoming.opcode,
                   (unsigned)incoming.length);
            xiaozhi_reset_ws_message_buffer(&incoming);
            continue;
        }
        if (xiaozhi_parse_server_hello((const char *)received, incoming.length,
                                       &server_session)) {
            connected = true;
            break;
        }
        printf("xiaozhi: ignored handshake text: %.160s\n", (const char *)received);
        xiaozhi_reset_ws_message_buffer(&incoming);
    }
    if (!connected && !s_stop_requested) {
        printf("xiaozhi: server hello timeout\n");
    }
    if (!connected || s_stop_requested) {
        goto cleanup;
    }
    if (audio_service_start_session() != ESP_OK) {
        printf("xiaozhi: audio session initialization failed\n");
        connected = false;
        goto cleanup;
    }
    audio_started = true;
    if (!xiaozhi_send_listen_start(websocket, &server_session)) {
        printf("xiaozhi: listen start send failed\n");
        connected = false;
        goto cleanup;
    }

    printf("xiaozhi: voice session started; speak after this message\n");
    printf("xiaozhi: session task stack free=%lu bytes\n",
           (unsigned long)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    bool upload_enabled = true;
    xiaozhi_reset_ws_message_buffer(&incoming);
    while (!s_stop_requested) {
        if (upload_enabled && audio_service_take_opus_packet(outgoing_packet, 0U)) {
            if (!xiaozhi_send_audio(websocket, protocol_version, outgoing_packet->data,
                                    outgoing_packet->length,
                                    serialized_packet,
                                    XIAOZHI_BINARY_V2_HEADER_SIZE + AUDIO_SERVICE_OPUS_PACKET_MAX_SIZE)) {
                printf("xiaozhi: microphone upload failed\n");
                connected = false;
                break;
            }
        }

        const int received_length = esp_transport_read(
            websocket, (char *)received + incoming.length,
            XIAOZHI_WSS_RECEIVE_BUFFER_SIZE - incoming.length - 1U, XIAOZHI_WSS_READ_SLICE_MS);
        if (received_length < 0) {
            printf("xiaozhi: websocket session disconnected\n");
            connected = false;
            break;
        }
        if (received_length == 0) {
            continue;
        }
        bool message_complete = false;
        if (!xiaozhi_append_ws_read(websocket, &incoming, (size_t)received_length,
                                    &message_complete)) {
            printf("xiaozhi: invalid incoming frame opcode=%d length=%d\n",
                   esp_transport_ws_get_read_opcode(websocket), received_length);
            xiaozhi_reset_ws_message_buffer(&incoming);
            continue;
        }
        if (!message_complete) {
            continue;
        }
        if (incoming.opcode == WS_TRANSPORT_OPCODES_TEXT) {
            received[incoming.length] = '\0';
            xiaozhi_handle_text_message((const char *)received, incoming.length, &upload_enabled);
        } else if (incoming.opcode == WS_TRANSPORT_OPCODES_BINARY) {
            const uint8_t *opus_packet = NULL;
            size_t opus_length = 0U;
            if (!xiaozhi_extract_audio_packet(received, incoming.length, protocol_version,
                                              &opus_packet, &opus_length)) {
                printf("xiaozhi: invalid incoming audio packet\n");
                xiaozhi_reset_ws_message_buffer(&incoming);
                continue;
            }
            if (audio_service_decode_and_play_opus(opus_packet, opus_length,
                                                   server_session.sample_rate_hz,
                                                   server_session.frame_duration_ms) != ESP_OK) {
                printf("xiaozhi: speaker playback failed\n");
            }
        }
        xiaozhi_reset_ws_message_buffer(&incoming);
    }

cleanup:
    if (audio_started) {
        audio_service_stop_session();
    }
    (void)esp_transport_close(websocket);
    esp_transport_list_destroy(transport_list);
    free(outgoing_packet);
    free(serialized_packet);
    free(received);
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
    printf("xiaozhi: session task running on core %d\n", xPortGetCoreID());

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
    if (xTaskCreatePinnedToCore(xiaozhi_session_task, "xiaozhi_session",
                                XIAOZHI_CLIENT_TASK_STACK_SIZE, NULL,
                                XIAOZHI_CLIENT_TASK_PRIORITY, &s_session_task,
                                XIAOZHI_CLIENT_TASK_CORE) != pdPASS) {
        s_session_task = NULL;
        printf("xiaozhi: session task creation failed\n");
        return false;
    }
    return true;
}

void xiaozhi_client_stop_session(void)
{
    s_stop_requested = true;
    /* 让会话任务在关闭 WebSocket 后自行停止 I2S 和 Opus，避免并发释放编解码器。 */
    const TickType_t started = xTaskGetTickCount();
    while (s_session_task != NULL &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(500)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool xiaozhi_client_is_busy(void)
{
    return s_session_task != NULL;
}
