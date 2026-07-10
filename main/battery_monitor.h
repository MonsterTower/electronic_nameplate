#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdbool.h>

#include "esp_err.h"

void battery_monitor_init(void);
// 低功耗工作周期中使用：只执行一次完整采样，不依赖周期性轮询。
esp_err_t battery_monitor_sample_now(void);
void battery_monitor_update(void);
bool battery_monitor_has_sample(void);
int battery_monitor_get_raw(void);
float battery_monitor_get_voltage(void);

#endif
