#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdbool.h>

void battery_monitor_init(void);
void battery_monitor_update(void);
bool battery_monitor_has_sample(void);
int battery_monitor_get_raw(void);
float battery_monitor_get_voltage(void);

#endif
