#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <stdbool.h>

#include "app_model.h"

/* 下载厦门当前天气；失败时不会覆盖数据模型中已经缓存的天气。 */
bool weather_service_refresh(app_model_t *model);

#endif
