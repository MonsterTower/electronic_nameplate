#include "power_manager.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POWER_MANAGER_WAKE_GPIO GPIO_NUM_0
#define POWER_MANAGER_WAKE_LEVEL 0

static TickType_t s_last_activity_tick;
static int64_t s_work_start_us;

static const char *power_manager_reason_text(power_wake_reason_t reason)
{
    switch (reason) {
    case POWER_WAKE_TIMER:
        return "timer";
    case POWER_WAKE_BOOT_BUTTON:
        return "boot button";
    case POWER_WAKE_COLD_BOOT:
    default:
        return "cold boot";
    }
}

power_wake_reason_t power_manager_init(void)
{
    s_work_start_us = esp_timer_get_time();
    s_last_activity_tick = xTaskGetTickCount();

    power_wake_reason_t reason = POWER_WAKE_COLD_BOOT;
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if ((wake_causes & (UINT32_C(1) << ESP_SLEEP_WAKEUP_TIMER)) != 0U) {
        reason = POWER_WAKE_TIMER;
    } else if ((wake_causes & (UINT32_C(1) << ESP_SLEEP_WAKEUP_EXT0)) != 0U) {
        reason = POWER_WAKE_BOOT_BUTTON;
    }
    printf("power: wake reason=%s\n", power_manager_reason_text(reason));
    return reason;
}

bool power_manager_is_button_wake(power_wake_reason_t reason)
{
    return reason == POWER_WAKE_BOOT_BUTTON;
}

void power_manager_start_interaction(void)
{
    s_last_activity_tick = xTaskGetTickCount();
    printf("power: interaction window=%u ms\n", POWER_MANAGER_INTERACTION_WINDOW_MS);
}

void power_manager_note_activity(void)
{
    s_last_activity_tick = xTaskGetTickCount();
}

bool power_manager_interaction_expired(void)
{
    return xTaskGetTickCount() - s_last_activity_tick >=
           pdMS_TO_TICKS(POWER_MANAGER_INTERACTION_WINDOW_MS);
}

void power_manager_enter_deep_sleep(uint32_t sleep_seconds, const char *reason)
{
    const int64_t work_duration_ms = (esp_timer_get_time() - s_work_start_us) / 1000LL;
    printf("power: sleep reason=%s work=%lld ms duration=%lu s\n",
           reason != NULL ? reason : "unspecified", (long long)work_duration_ms,
           (unsigned long)sleep_seconds);

#if POWER_MANAGER_ENABLE_DEEP_SLEEP
    /* BOOT 按键接地时唤醒。GPIO0 属于 ESP32-S3 的 RTC GPIO 范围。 */
    while (gpio_get_level(POWER_MANAGER_WAKE_GPIO) == POWER_MANAGER_WAKE_LEVEL) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    ESP_ERROR_CHECK(gpio_set_direction(POWER_MANAGER_WAKE_GPIO, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(POWER_MANAGER_WAKE_GPIO, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)sleep_seconds * 1000000ULL));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(POWER_MANAGER_WAKE_GPIO, POWER_MANAGER_WAKE_LEVEL));
    fflush(stdout);
    esp_deep_sleep_start();
#else
    printf("power: deep sleep disabled by macro\n");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
#endif
}
