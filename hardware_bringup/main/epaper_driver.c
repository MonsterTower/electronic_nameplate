#include "epaper_driver.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epaper_bus.h"

#define EPAPER_WIDTH 400U
#define EPAPER_HEIGHT 300U
#define EPAPER_BYTES_PER_LINE (EPAPER_WIDTH / 8U)

/* 三色全刷需要较长时间；超时只防止接线异常时无限阻塞。 */
#define EPAPER_BUSY_TIMEOUT_MS 30000U
#define EPAPER_BUSY_POLL_INTERVAL_MS 20U

static esp_err_t epaper_send_command_data(uint8_t command, const uint8_t *data, size_t length)
{
    esp_err_t err = epaper_bus_write_command(command);
    if (err == ESP_OK && length > 0U) {
        err = epaper_bus_write_data(data, length);
    }
    return err;
}

static esp_err_t epaper_wait_until_idle(const char *phase)
{
    const TickType_t start_tick = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(EPAPER_BUSY_TIMEOUT_MS);

    while (epaper_bus_is_busy()) {
        if (xTaskGetTickCount() - start_tick >= timeout_ticks) {
            printf("epaper: busy timeout during %s, level=%d\n", phase,
                   epaper_bus_is_busy() ? 1 : 0);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(EPAPER_BUSY_POLL_INTERVAL_MS));
    }
    return ESP_OK;
}

static esp_err_t epaper_set_full_window(void)
{
    const uint8_t data_entry_mode = 0x03;
    const uint8_t x_window[] = {0x00, (EPAPER_WIDTH / 8U) - 1U};
    const uint8_t y_window[] = {
        0x00, 0x00,
        (EPAPER_HEIGHT - 1U) & 0xFFU, (EPAPER_HEIGHT - 1U) >> 8U,
    };
    const uint8_t x_counter = 0x00;
    const uint8_t y_counter[] = {0x00, 0x00};

    esp_err_t err = epaper_send_command_data(0x11, &data_entry_mode, sizeof(data_entry_mode));
    if (err == ESP_OK) {
        err = epaper_send_command_data(0x44, x_window, sizeof(x_window));
    }
    if (err == ESP_OK) {
        err = epaper_send_command_data(0x45, y_window, sizeof(y_window));
    }
    if (err == ESP_OK) {
        err = epaper_send_command_data(0x4E, &x_counter, sizeof(x_counter));
    }
    if (err == ESP_OK) {
        err = epaper_send_command_data(0x4F, y_counter, sizeof(y_counter));
    }
    return err;
}

static bool epaper_is_black_pixel(uint16_t x, uint16_t y)
{
    const bool border = x == 0U || x == EPAPER_WIDTH - 1U || y == 0U || y == EPAPER_HEIGHT - 1U;
    const bool lower_left_block = x >= 24U && x < 176U && y >= 24U && y < 124U;
    return border || lower_left_block;
}

static bool epaper_is_red_pixel(uint16_t x, uint16_t y)
{
    return x >= 224U && x < 376U && y >= 176U && y < 276U;
}

static esp_err_t epaper_write_test_plane(bool red_plane)
{
    uint8_t line[EPAPER_BYTES_PER_LINE];
    esp_err_t err = epaper_bus_begin_data_stream();
    if (err != ESP_OK) {
        return err;
    }

    for (uint16_t controller_y = 0U; controller_y < EPAPER_HEIGHT; ++controller_y) {
        /* 控制器通常从屏幕顶部开始写入；对外坐标系固定为左下角 (0, 0)。 */
        const uint16_t y = EPAPER_HEIGHT - 1U - controller_y;

        for (uint16_t byte_index = 0U; byte_index < EPAPER_BYTES_PER_LINE; ++byte_index) {
            uint8_t value = red_plane ? 0x00U : 0xFFU;
            for (uint8_t bit_index = 0U; bit_index < 8U; ++bit_index) {
                const uint16_t x = byte_index * 8U + bit_index;
                const uint8_t bit_mask = 0x80U >> bit_index;

                if (epaper_is_black_pixel(x, y)) {
                    if (!red_plane) {
                        value &= (uint8_t)~bit_mask;
                    }
                } else if (epaper_is_red_pixel(x, y) && red_plane) {
                    value |= bit_mask;
                }
            }
            line[byte_index] = value;
        }

        /* 除最后一行外保持 CS 有效，符合控制器一次写入完整 RAM 平面的要求。 */
        err = epaper_bus_write_data_chunk(line, sizeof(line), controller_y + 1U < EPAPER_HEIGHT);
        if (err != ESP_OK) {
            break;
        }
    }
    epaper_bus_end_data_stream();
    return err;
}

static esp_err_t epaper_driver_init(void)
{
    esp_err_t err = epaper_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    /* 硬复位后再等待空闲，避免控制器仍处于上电恢复流程时接收数据。 */
    epaper_bus_set_reset(false);
    vTaskDelay(pdMS_TO_TICKS(10U));
    epaper_bus_set_reset(true);
    vTaskDelay(pdMS_TO_TICKS(10U));
    err = epaper_wait_until_idle("hardware reset");
    if (err != ESP_OK) {
        return err;
    }

    err = epaper_bus_write_command(0x12); /* 软件复位 */
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10U));
    err = epaper_wait_until_idle("software reset");
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t driver_output[] = {0x2B, 0x01, 0x00};
    const uint8_t border_waveform = 0x05;
    const uint8_t temperature_sensor = 0x80;
    err = epaper_send_command_data(0x01, driver_output, sizeof(driver_output));
    if (err == ESP_OK) {
        err = epaper_send_command_data(0x3C, &border_waveform, sizeof(border_waveform));
    }
    if (err == ESP_OK) {
        err = epaper_send_command_data(0x18, &temperature_sensor, sizeof(temperature_sensor));
    }
    return err;
}

esp_err_t epaper_driver_show_test_pattern(void)
{
    esp_err_t err = epaper_driver_init();
    if (err != ESP_OK) {
        return err;
    }

    printf("epaper: writing black/white plane\n");
    err = epaper_set_full_window();
    if (err == ESP_OK) {
        err = epaper_bus_write_command(0x24);
    }
    if (err == ESP_OK) {
        err = epaper_write_test_plane(false);
    }

    if (err == ESP_OK) {
        printf("epaper: writing red plane\n");
        err = epaper_set_full_window();
    }
    if (err == ESP_OK) {
        err = epaper_bus_write_command(0x26);
    }
    if (err == ESP_OK) {
        err = epaper_write_test_plane(true);
    }

    if (err == ESP_OK) {
        const uint8_t full_refresh = 0xF7;
        printf("epaper: starting full refresh\n");
        err = epaper_send_command_data(0x22, &full_refresh, sizeof(full_refresh));
    }
    if (err == ESP_OK) {
        err = epaper_bus_write_command(0x20);
    }
    if (err == ESP_OK) {
        err = epaper_wait_until_idle("full refresh");
    }

    if (err == ESP_OK) {
        printf("epaper: test pattern refresh complete\n");
    }
    return err;
}

esp_err_t epaper_driver_sleep(void)
{
    const uint8_t power_off = 0xC3;
    esp_err_t err = epaper_send_command_data(0x22, &power_off, sizeof(power_off));
    if (err == ESP_OK) {
        err = epaper_bus_write_command(0x20);
    }
    if (err == ESP_OK) {
        err = epaper_wait_until_idle("power off");
    }
    if (err == ESP_OK) {
        const uint8_t deep_sleep = 0x11;
        err = epaper_send_command_data(0x10, &deep_sleep, sizeof(deep_sleep));
    }
    if (err == ESP_OK) {
        printf("epaper: controller entered deep sleep\n");
    }
    return err;
}
