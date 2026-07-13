#pragma once

#include <stdbool.h>
#include <time.h>

#include "app_model.h"

/* 下载并解析本周课表；请求或解析失败时不覆盖模型中的上一次有效数据。 */
bool schedule_service_refresh(app_model_t *model, const struct tm *local_time);

