#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

// 这些宏集中描述桌牌的休眠策略，后续可按真实电池容量和使用频率调整。
#define POWER_MANAGER_NORMAL_SLEEP_SECONDS (10U * 60U)
#define POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS 10U
#define POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS (60U * 60U)
#define POWER_MANAGER_INTERACTION_WINDOW_MS (10U * 1000U)
#define POWER_MANAGER_LOW_BATTERY_VOLTAGE 3.40f

// Wokwi 目前不能正确保持本项目的 Deep Sleep 状态，仿真阶段只记录休眠日志。
// 烧录到实物 ESP32 前把该宏改为 1，即可启用定时器和 BOOT 唤醒。
#define POWER_MANAGER_ENABLE_DEEP_SLEEP 0

typedef enum {
    POWER_WAKE_COLD_START,
    POWER_WAKE_TIMER,
    POWER_WAKE_BOOT_BUTTON,
    POWER_WAKE_OTHER,
} power_wakeup_reason_t;

typedef struct {
    bool interactive;
    bool should_connect_network;
    uint32_t sleep_seconds;
} power_manager_plan_t;

void power_manager_init(void);
power_wakeup_reason_t power_manager_get_wakeup_reason(void);
const char *power_manager_wakeup_reason_text(power_wakeup_reason_t reason);
bool power_manager_is_low_battery(float voltage, bool sample_valid);
power_manager_plan_t power_manager_make_plan(power_wakeup_reason_t reason, bool low_battery,
                                             bool network_attempted, bool network_success);
void power_manager_enter_deep_sleep(uint32_t sleep_seconds);

#endif
