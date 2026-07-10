#ifndef SCHEDULE_SERVICE_H
#define SCHEDULE_SERVICE_H

#include <stdbool.h>
#include <time.h>

#include "app_model.h"

// 课表公开数据源与请求超时集中为宏，后续迁移服务器时只需要改这里。
#define SCHEDULE_SERVICE_URL "https://monstertower.github.io/esp32-calendar-data/schedule.json"
#define SCHEDULE_SERVICE_HTTP_TIMEOUT_MS 8000

// 根据当前 NTP 本地时间下载 JSON、筛选当天课程，并更新数据模型。
// 请求或解析失败时不会改写模型中已有的课程缓存。
bool schedule_service_refresh(app_model_t *model, const struct tm *local_time);

#endif
