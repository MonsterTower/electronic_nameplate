#include "power_manager.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_panel.h"

#define POWER_MANAGER_BOOT_RELEASE_WAIT_MS 1000U

#if !POWER_MANAGER_ENABLE_DEEP_SLEEP
static bool s_simulated_boot_wakeup;
#endif

void power_manager_init(void)
{
    // 从深睡眠唤醒后，把 GPIO0 从 RTC 功能切回普通数字输入，交给按键模块扫描。
    (void)rtc_gpio_deinit(INPUT_PANEL_BOOT_GPIO);
}

power_wakeup_reason_t power_manager_get_wakeup_reason(void)
{
#if !POWER_MANAGER_ENABLE_DEEP_SLEEP
    if (s_simulated_boot_wakeup) {
        s_simulated_boot_wakeup = false;
        return POWER_WAKE_BOOT_BUTTON;
    }
#endif

    const uint32_t causes = esp_sleep_get_wakeup_causes();

    // ESP-IDF 6 会返回全部同时生效的唤醒源；按钮优先用于决定交互模式。
    if ((causes & (1UL << ESP_SLEEP_WAKEUP_EXT0)) != 0) {
        return POWER_WAKE_BOOT_BUTTON;
    }
    if ((causes & (1UL << ESP_SLEEP_WAKEUP_TIMER)) != 0) {
        return POWER_WAKE_TIMER;
    }
    // IDF 6 的冷启动会返回 UNDEFINED 对应的位图，而不是空位图。
    if (causes == 0 || (causes & (1UL << ESP_SLEEP_WAKEUP_UNDEFINED)) != 0) {
        return POWER_WAKE_COLD_START;
    }
    return POWER_WAKE_OTHER;
}

const char *power_manager_wakeup_reason_text(power_wakeup_reason_t reason)
{
    switch (reason) {
    case POWER_WAKE_TIMER:
        return "timer";
    case POWER_WAKE_BOOT_BUTTON:
        return "boot button";
    case POWER_WAKE_COLD_START:
        return "cold start";
    case POWER_WAKE_OTHER:
    default:
        return "other";
    }
}

bool power_manager_is_low_battery(float voltage, bool sample_valid)
{
    return sample_valid && voltage <= POWER_MANAGER_LOW_BATTERY_VOLTAGE;
}

power_manager_plan_t power_manager_make_plan(power_wakeup_reason_t reason, bool low_battery,
                                             bool network_attempted, bool network_success)
{
    power_manager_plan_t plan = {
        .interactive = reason == POWER_WAKE_BOOT_BUTTON,
        .should_connect_network = false,
        .sleep_seconds = POWER_MANAGER_NORMAL_SLEEP_SECONDS,
    };

    // 按键唤醒优先给用户交互窗口；低电量时完全跳过高耗电的无线连接。
    plan.should_connect_network = !plan.interactive && !low_battery;

    if (low_battery) {
        plan.sleep_seconds = POWER_MANAGER_LOW_BATTERY_SLEEP_SECONDS;
    } else if (network_attempted && !network_success) {
        plan.sleep_seconds = POWER_MANAGER_NETWORK_RETRY_SLEEP_SECONDS;
    }

    return plan;
}

#if POWER_MANAGER_ENABLE_DEEP_SLEEP
static bool power_manager_wait_boot_release(void)
{
    uint32_t elapsed_ms = 0;
    while (gpio_get_level(INPUT_PANEL_BOOT_GPIO) == 0 &&
           elapsed_ms < POWER_MANAGER_BOOT_RELEASE_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed_ms += 10;
    }
    return gpio_get_level(INPUT_PANEL_BOOT_GPIO) != 0;
}
#endif

#if !POWER_MANAGER_ENABLE_DEEP_SLEEP
static void power_manager_wait_simulated_boot_wake(void)
{
    printf("power: simulated sleep, press BOOT to wake\n");

    while (true) {
        if (gpio_get_level(INPUT_PANEL_BOOT_GPIO) == 0) {
            // 模拟模式也保留一次简短防抖，避免按键毛刺直接结束休眠。
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(INPUT_PANEL_BOOT_GPIO) == 0) {
                while (gpio_get_level(INPUT_PANEL_BOOT_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                s_simulated_boot_wakeup = true;
                printf("power: simulated BOOT wake\n");
                return;
            }
        }

        // 仿真休眠期间只低频检查 BOOT，不扫描页面按键，也不启动其他外设。
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

void power_manager_enter_deep_sleep(uint32_t sleep_seconds)
{
#if POWER_MANAGER_ENABLE_DEEP_SLEEP
    bool boot_wakeup_enabled = power_manager_wait_boot_release();
    if (!boot_wakeup_enabled) {
        // 按键一直按下时配置低电平唤醒会立刻再次醒来；本轮只保留定时器唤醒。
        printf("power: BOOT still pressed, skip button wake once\n");
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_init(INPUT_PANEL_BOOT_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_set_direction(INPUT_PANEL_BOOT_GPIO, RTC_GPIO_MODE_INPUT_ONLY));
        ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_pullup_en(INPUT_PANEL_BOOT_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(rtc_gpio_pulldown_dis(INPUT_PANEL_BOOT_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_ext0_wakeup(INPUT_PANEL_BOOT_GPIO, 0));
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL));
    printf("power: sleep=%u s, boot_wake=%s\n", (unsigned)sleep_seconds,
           boot_wakeup_enabled ? "on" : "off");
    esp_deep_sleep_start();
#else
    // 仿真模式不配置任何硬件唤醒源，也不调用 Deep Sleep，避免 Wokwi 立即复位重启。
    printf("power: simulated sleep=%u s, deep sleep disabled\n", (unsigned)sleep_seconds);
    power_manager_wait_simulated_boot_wake();
#endif
}
