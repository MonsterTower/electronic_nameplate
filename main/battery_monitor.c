#include "battery_monitor.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ADC_BAT 接到 GPIO1。ESP32-S3 上 GPIO1 是 ADC 可用引脚。
#define BATTERY_ADC_GPIO GPIO_NUM_1

// ADC_CTRL 接到 GPIO2，用来控制外部 PMOS，从而打开或关闭电池分压采样支路。
#define BATTERY_CTRL_GPIO GPIO_NUM_2

// GPIO2 控制 PMOS：LOW 导通分压采样电路，HIGH 截断以降低功耗。
#define BATTERY_CTRL_ON_LEVEL 0
#define BATTERY_CTRL_OFF_LEVEL 1

// 用户电路约定：12bit ADC 原始值 0~4095 对应 0~3.3V，前端分压后需乘 2 还原电池电压。
#define BATTERY_ADC_MAX_RAW 4095.0f
#define BATTERY_ADC_REF_VOLTAGE 3.3f
#define BATTERY_DIVIDER_RATIO 2.0f

// 采样参数：导通 PMOS 后等待电压稳定，低频采样即可。
#define BATTERY_SETTLE_MS 10
#define BATTERY_SAMPLE_INTERVAL_MS 6000

// 多读几次再平均，可以降低 ADC 量化噪声和瞬时抖动对结果的影响。
#define BATTERY_SAMPLE_COUNT 8

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_channel_t s_adc_channel;

// 只记录最近一次采样结果。周期性写 flash 会有磨损风险，所以这里先保存在内存中。
static TickType_t s_last_sample_tick;
static int s_last_raw;
static float s_last_voltage;
static bool s_has_sample;
static bool s_initialized;

static float raw_to_battery_voltage(int raw)
{
    // 先把 ADC 原始值换算成 GPIO1 上看到的电压，再乘分压比例还原电池真实电压。
    const float adc_voltage = ((float)raw * BATTERY_ADC_REF_VOLTAGE) / BATTERY_ADC_MAX_RAW;

    return adc_voltage * BATTERY_DIVIDER_RATIO;
}

static void battery_sampling_enable(bool enable)
{
    // 只在采样窗口内打开分压支路，其他时间关闭，减少电池的静态消耗。
    gpio_set_level(BATTERY_CTRL_GPIO, enable ? BATTERY_CTRL_ON_LEVEL : BATTERY_CTRL_OFF_LEVEL);
}

static esp_err_t read_adc_average(int *out_raw)
{
    int raw = 0;
    int raw_sum = 0;

    for (int i = 0; i < BATTERY_SAMPLE_COUNT; ++i) {
        // oneshot 采样每次读取一个原始值；这里不做校准，按项目约定的 0~3.3V 线性关系换算。
        esp_err_t err = adc_oneshot_read(s_adc_handle, s_adc_channel, &raw);
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

    // 打开 PMOS 后，分压节点和 ADC 输入需要一点时间稳定。
    battery_sampling_enable(true);
    vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS));

    // 读取结束后无论成功失败，都要尽快关闭 PMOS，避免分压电阻持续耗电。
    const esp_err_t err = read_adc_average(&raw);
    battery_sampling_enable(false);
    if (err != ESP_OK) {
        printf("battery: adc read failed: %s\n", esp_err_to_name(err));
        return err;
    }

    s_last_raw = raw;
    s_last_voltage = raw_to_battery_voltage(s_last_raw);
    s_has_sample = true;

    // 避免 printf 浮点格式化的额外开销，转成整数毫伏后再打印成 x.xxxV。
    const int voltage_mv = (int)(s_last_voltage * 1000.0f + 0.5f);
    printf("battery: raw=%d voltage=%d.%03dV\n", s_last_raw, voltage_mv / 1000, voltage_mv % 1000);
    return ESP_OK;
}

void battery_monitor_init(void)
{
    if (s_initialized) {
        return;
    }

    adc_unit_t adc_unit;

    gpio_config_t ctrl_config = {
        .pin_bit_mask = (1ULL << BATTERY_CTRL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ctrl_config));

    // 初始化后默认关闭采样支路，确保未采样时电源检测电路不额外耗电。
    battery_sampling_enable(false);

    // 由 GPIO 自动换算 ADC 单元和通道，避免手写通道号时和芯片封装映射搞错。
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &adc_unit, &s_adc_channel));

    adc_oneshot_unit_init_cfg_t adc_init_config = {
        .unit_id = adc_unit,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t adc_channel_config = {
        // 12dB 衰减可以覆盖较高输入范围；这里仍按仿真约定用 0~3.3V 换算。
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &adc_channel_config));

    s_last_raw = 0;
    s_last_voltage = 0.0f;
    s_has_sample = false;

    // 让系统启动后第一次调用 update() 就能立即采样，而不是等待一个完整周期。
    s_last_sample_tick = xTaskGetTickCount() - pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS);
    s_initialized = true;
}

void battery_monitor_update(void)
{
    const TickType_t now = xTaskGetTickCount();

    // 电池电压变化很慢，低频轮询即可；频繁采样只会增加功耗。
    if ((now - s_last_sample_tick) < pdMS_TO_TICKS(BATTERY_SAMPLE_INTERVAL_MS)) {
        return;
    }

    // 先更新时间戳，再执行采样。即使本次 ADC 异常，也不会立刻高频重试。
    s_last_sample_tick = now;
    (void)sample_battery_once();
}

esp_err_t battery_monitor_sample_now(void)
{
    // 深睡眠唤醒后的工作周期只采样一次，避免保留旧的轮询节奏。
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
