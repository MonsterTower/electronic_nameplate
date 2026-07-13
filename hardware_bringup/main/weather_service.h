#pragma once

#include <stdbool.h>

#include "app_model.h"

/* 请求失败时不写入模型，以保留上一次有效天气数据。 */
bool weather_service_refresh(app_model_t *model);

