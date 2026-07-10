#include "app_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_STORAGE_NAMESPACE "nameplate"
#define APP_STORAGE_KEY "sleep_cache"
#define APP_STORAGE_MAGIC 0x4E504C54UL
#define APP_STORAGE_VERSION 2U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t page;
    uint8_t calendar_valid;
    app_calendar_info_t calendar;
    bool battery_valid;
    int battery_percent;
    char battery_voltage_text[APP_MODEL_STATUS_LEN];
    char battery_percent_text[APP_MODEL_STATUS_LEN];
    bool course_data_valid;
    uint8_t course_count;
    app_course_info_t courses[APP_MODEL_COURSE_COUNT];
} app_storage_record_t;

static void app_storage_build_record(app_storage_record_t *record, uint8_t page, const app_model_t *model)
{
    memset(record, 0, sizeof(*record));
    record->magic = APP_STORAGE_MAGIC;
    record->version = APP_STORAGE_VERSION;
    record->page = page;
    record->calendar_valid = model->calendar.time_synced;
    record->calendar = model->calendar;
    if (record->calendar_valid) {
        // 实时时分秒依靠 RTC 延续，不把每次 NTP 同步的时分秒都写入 Flash。
        // 真正断电后若只能读到此缓存，CACHED 明确表示它不是实时钟。
        snprintf(record->calendar.time, sizeof(record->calendar.time), "%s", "CACHED");
    }
    record->battery_valid = model->battery.valid;
    record->battery_percent = model->battery.percent;
    snprintf(record->battery_voltage_text, sizeof(record->battery_voltage_text), "%s",
             model->battery.voltage_text);
    snprintf(record->battery_percent_text, sizeof(record->battery_percent_text), "%s",
             model->battery.percent_text);
    record->course_data_valid = model->course_data_valid;
    record->course_count = model->course_count;
    if (record->course_data_valid) {
        memcpy(record->courses, model->courses, sizeof(record->courses));
    }
}

esp_err_t app_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool app_storage_load(uint8_t *page, app_model_t *model)
{
    if (page == NULL || model == NULL) {
        return false;
    }

    nvs_handle_t handle;
    app_storage_record_t record = {0};
    size_t size = sizeof(record);
    const esp_err_t open_err = nvs_open(APP_STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (open_err != ESP_OK) {
        return false;
    }

    const esp_err_t read_err = nvs_get_blob(handle, APP_STORAGE_KEY, &record, &size);
    nvs_close(handle);
    if (read_err != ESP_OK || size != sizeof(record) || record.magic != APP_STORAGE_MAGIC ||
        record.version != APP_STORAGE_VERSION || record.page > 3) {
        return false;
    }

    *page = record.page;
    if (record.calendar_valid) {
        model->calendar = record.calendar;
    }
    if (record.battery_valid) {
        model->battery.valid = true;
        model->battery.percent = record.battery_percent;
        snprintf(model->battery.voltage_text, sizeof(model->battery.voltage_text), "%s",
                 record.battery_voltage_text);
        snprintf(model->battery.percent_text, sizeof(model->battery.percent_text), "%s",
                 record.battery_percent_text);
    }
    if (record.course_data_valid && record.course_count <= APP_MODEL_COURSE_COUNT) {
        memcpy(model->courses, record.courses, sizeof(model->courses));
        model->course_count = record.course_count;
        model->course_data_valid = true;
    }

    printf("storage: restored page=%u cache=%s\n", (unsigned)*page,
           record.calendar_valid ? "valid" : "empty");
    return true;
}

esp_err_t app_storage_save(uint8_t page, const app_model_t *model)
{
    if (model == NULL || page > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    app_storage_record_t record;
    app_storage_build_record(&record, page, model);

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(APP_STORAGE_NAMESPACE, NVS_READWRITE, &handle), "storage", "nvs open failed");

    app_storage_record_t previous = {0};
    size_t previous_size = sizeof(previous);
    const esp_err_t get_err = nvs_get_blob(handle, APP_STORAGE_KEY, &previous, &previous_size);
    if (get_err == ESP_OK && previous_size == sizeof(previous) && memcmp(&previous, &record, sizeof(record)) == 0) {
        nvs_close(handle);
        return ESP_OK;
    }

    esp_err_t err = nvs_set_blob(handle, APP_STORAGE_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        printf("storage: saved page=%u\n", (unsigned)page);
    } else {
        printf("storage: save failed: %s\n", esp_err_to_name(err));
    }
    return err;
}
