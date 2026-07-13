#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 实物板启用 Deep Sleep；需要连续串口调试时可临时改为 0。 */
#define POWER_MANAGER_ENABLE_DEEP_SLEEP 1
#define POWER_MANAGER_INTERACTION_WINDOW_MS 30000U
#define POWER_MANAGER_NORMAL_SLEEP_SECONDS (30U * 60U)
#define POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS (10U * 60U)
#define POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS (6U * 60U * 60U)

typedef enum {
    POWER_WAKE_COLD_BOOT = 0,
    POWER_WAKE_TIMER,
    POWER_WAKE_BOOT_BUTTON,
} power_wake_reason_t;

power_wake_reason_t power_manager_init(void);
bool power_manager_is_button_wake(power_wake_reason_t reason);
void power_manager_start_interaction(void);
void power_manager_note_activity(void);
bool power_manager_interaction_expired(void);
void power_manager_enter_deep_sleep(uint32_t sleep_seconds, const char *reason);
