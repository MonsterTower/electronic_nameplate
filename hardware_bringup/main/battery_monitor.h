#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdbool.h>

#include "esp_err.h"

void battery_monitor_init(void);
esp_err_t battery_monitor_sample_now(void);
void battery_monitor_update(void);
bool battery_monitor_has_sample(void);
int battery_monitor_get_raw(void);
float battery_monitor_get_voltage(void);

#endif
