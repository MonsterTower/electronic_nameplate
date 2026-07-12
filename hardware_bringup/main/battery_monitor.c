#include "battery_monitor.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 真实 PCB：ADC_BAT 接 GPIO1，ADC_CTRL 接 GPIO2。 */
#define BATTERY_ADC_GPIO GPIO_NUM_1
#define BATTERY_CTRL_GPIO GPIO_NUM_2

/* PMOS 栅极低电平导通，采样以外的时间必须关闭分压支路。 */
#define BATTERY_CTRL_ON_LEVEL 0
#define BATTERY_CTRL_OFF_LEVEL 1

/* 项目电路约定：ADC 原始值 0~4095 对应 0~3.3V，分压后需乘 2 还原电池电压。 */
#define BATTERY_ADC_MAX_RAW 4095.0f
#define BATTERY_ADC_REF_VOLTAGE 3.3f
#define BATTERY_DIVIDER_RATIO 2.0f

#define BATTERY_SETTLE_MS 10U
#define BATTERY_SAMPLE_INTERVAL_MS 6000U
#define BATTERY_SAMPLE_COUNT 8

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_channel_t s_adc_channel;
static TickType_t s_last_sample_tick;
static int s_last_raw;
static float s_last_voltage;
static bool s_has_sample;
static bool s_initialized;

static float raw_to_battery_voltage(int raw)
{
    const float adc_voltage = ((float)raw * BATTERY_ADC_REF_VOLTAGE) / BATTERY_ADC_MAX_RAW;
    return adc_voltage * BATTERY_DIVIDER_RATIO;
}

static void battery_sampling_enable(bool enable)
{
    gpio_set_level(BATTERY_CTRL_GPIO, enable ? BATTERY_CTRL_ON_LEVEL : BATTERY_CTRL_OFF_LEVEL);
}

static esp_err_t read_adc_average(int *out_raw)
{
    int raw_sum = 0;

    for (int i = 0; i < BATTERY_SAMPLE_COUNT; ++i) {
        int raw = 0;
        const esp_err_t err = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
        if (err != ESP_OK) {
            return err;
        }
        raw_sum += raw;
    }

    *out_raw = raw_sum / BATTERY_SAMPLE_COUNT;
    return ESP_OK;
}

static esp_err_t sample_battery_once(void)
{
    int raw = 0;

    battery_sampling_enable(true);
    vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS));

    /* 无论 ADC 是否成功，都要关闭 PMOS，避免分压电阻持续耗电。 */
    const esp_err_t err = read_adc_average(&raw);
    battery_sampling_enable(false);
    if (err != ESP_OK) {
        printf("battery: adc read failed: %s\n", esp_err_to_name(err));
        return err;
    }

    s_last_raw = raw;
    s_last_voltage = raw_to_battery_voltage(raw);
    s_has_sample = true;

    const int voltage_mv = (int)(s_last_voltage * 1000.0f + 0.5f);
    printf("battery: raw=%d voltage=%d.%03dV\n", raw, voltage_mv / 1000, voltage_mv % 1000);
    return ESP_OK;
}

void battery_monitor_init(void)
{
    if (s_initialized) {
        return;
    }

    const gpio_config_t ctrl_config = {
        .pin_bit_mask = (1ULL << BATTERY_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ctrl_config));
    battery_sampling_enable(false);

    adc_unit_t adc_unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &adc_unit, &s_adc_channel));

    const adc_oneshot_unit_init_cfg_t adc_init_config = {
        .unit_id = adc_unit,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init_config, &s_adc_handle));

    const adc_oneshot_chan_cfg_t adc_channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &adc_channel_config));

    s_last_raw = 0;
    s_last_voltage = 0.0f;
    s_has_sample = false;
    s_last_sample_tick = xTaskGetTickCount() - pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS);
    s_initialized = true;
}

void battery_monitor_update(void)
{
    const TickType_t now = xTaskGetTickCount();
    if (now - s_last_sample_tick < pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS)) {
        return;
    }

    s_last_sample_tick = now;
    (void)sample_battery_once();
}

esp_err_t battery_monitor_sample_now(void)
{
    s_last_sample_tick = xTaskGetTickCount();
    return sample_battery_once();
}

bool battery_monitor_has_sample(void)
{
    return s_has_sample;
}

int battery_monitor_get_raw(void)
{
    return s_last_raw;
}

float battery_monitor_get_voltage(void)
{
    return s_last_voltage;
}
