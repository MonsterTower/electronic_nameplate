#include "app_cache.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nvs.h"
#include "nvs_flash.h"

#define APP_CACHE_NAMESPACE "app_cache"
#define APP_CACHE_MODEL_KEY "model"
#define APP_CACHE_MAGIC 0x454E5043UL
/* 修改 app_model_t 的二进制布局或缓存语义时，需要递增该版本号。 */
#define APP_CACHE_VERSION 3U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int64_t saved_epoch;
    app_model_t model;
} app_cache_record_t;

static void app_cache_validate_restored_model(app_model_t *model)
{
    if (model->page >= APP_PAGE_COUNT) {
        model->page = APP_PAGE_NAMEPLATE;
    }

    if (model->schedule_item_count > APP_MODEL_SCHEDULE_ITEM_COUNT) {
        model->schedule_item_count = 0U;
        model->schedule_data_valid = false;
    }
}

esp_err_t app_cache_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

bool app_cache_restore(app_model_t *model)
{
    if (model == NULL) {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_CACHE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("cache: no saved data\n");
        return false;
    }
    if (err != ESP_OK) {
        printf("cache: open failed: %s\n", esp_err_to_name(err));
        return false;
    }

    app_cache_record_t record = {0};
    size_t record_size = sizeof(record);
    err = nvs_get_blob(handle, APP_CACHE_MODEL_KEY, &record, &record_size);
    nvs_close(handle);
    if (err != ESP_OK || record_size != sizeof(record) || record.magic != APP_CACHE_MAGIC ||
        record.version != APP_CACHE_VERSION) {
        printf("cache: saved data is invalid\n");
        return false;
    }

    *model = record.model;
    app_cache_validate_restored_model(model);
    printf("cache: restored page=%d schedule=%u saved=%lld\n", (int)model->page,
           (unsigned)model->schedule_item_count, (long long)record.saved_epoch);
    return true;
}

bool app_cache_save(const app_model_t *model)
{
    if (model == NULL) {
        return false;
    }

    app_cache_record_t record = {
        .magic = APP_CACHE_MAGIC,
        .version = APP_CACHE_VERSION,
        .saved_epoch = (int64_t)time(NULL),
        .model = *model,
    };
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_CACHE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, APP_CACHE_MODEL_KEY, &record, sizeof(record));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        printf("cache: save failed: %s\n", esp_err_to_name(err));
        return false;
    }

    printf("cache: saved page=%d schedule=%u\n", (int)model->page,
           (unsigned)model->schedule_item_count);
    return true;
}
