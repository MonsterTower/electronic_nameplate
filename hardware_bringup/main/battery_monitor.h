#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* ICR18650 锂电池参数，可按后续实测或电池资料调整。 */
#define BATTERY_FULL_VOLTAGE 4.20f
#define BATTERY_LOW_VOLTAGE 3.50f
#define BATTERY_EMPTY_VOLTAGE 3.30f

void battery_monitor_init(void);
esp_err_t battery_monitor_sample_now(void);
void battery_monitor_update(void);
bool battery_monitor_has_sample(void);
int battery_monitor_get_raw(void);
float battery_monitor_get_voltage(void);
uint8_t battery_monitor_get_percentage(void);
bool battery_monitor_is_low(void);
bool battery_monitor_is_critical(void);

#endif
