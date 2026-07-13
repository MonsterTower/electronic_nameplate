#pragma once

#include <stdbool.h>

#include "app_model.h"
#include "esp_err.h"

/* 初始化 NVS，并恢复或保存页面所需的最后一次有效数据。 */
esp_err_t app_cache_init(void);
bool app_cache_restore(app_model_t *model);
bool app_cache_save(const app_model_t *model);
