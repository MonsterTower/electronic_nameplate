#ifndef APP_STORAGE_H
#define APP_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_model.h"
#include "esp_err.h"

// 初始化 NVS，并读取或保存深睡眠之间需要保留的页面和有效日历缓存。
esp_err_t app_storage_init(void);
bool app_storage_load(uint8_t *page, app_model_t *model);
esp_err_t app_storage_save(uint8_t page, const app_model_t *model);

#endif
