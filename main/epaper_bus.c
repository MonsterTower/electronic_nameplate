#include "epaper_bus.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define EPAPER_PIN_MOSI GPIO_NUM_18
#define EPAPER_PIN_CLK GPIO_NUM_17
#define EPAPER_PIN_CS GPIO_NUM_9
#define EPAPER_PIN_DC GPIO_NUM_3
#define EPAPER_PIN_RST GPIO_NUM_8
#define EPAPER_PIN_BUSY GPIO_NUM_10

#define EPAPER_SPI_HOST SPI2_HOST
#define EPAPER_SPI_CLOCK_HZ (4 * 1000 * 1000)
#define EPAPER_SPI_MAX_TRANSFER_SIZE 4096

static spi_device_handle_t s_epaper_spi;

/* 总线层只关心“怎么把一个字节送到屏幕”，上层不直接碰 SPI 句柄和 GPIO 细节。 */
static void epaper_bus_write_byte(uint8_t data)
{
    spi_transaction_t transaction;

    memset(&transaction, 0, sizeof(transaction));
    transaction.length = 8;
    transaction.tx_buffer = &data;
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_epaper_spi, &transaction));
}

void epaper_bus_init(void)
{
    /* 2.9 寸墨水屏只用 MOSI/CLK/CS 三根 SPI 线，MISO 不接。 */
    spi_bus_config_t bus_config = {
        .mosi_io_num = EPAPER_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = EPAPER_PIN_CLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = EPAPER_SPI_MAX_TRANSFER_SIZE,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPAPER_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = EPAPER_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = EPAPER_PIN_CS,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(EPAPER_SPI_HOST, &device_config, &s_epaper_spi));

    /* DC 区分命令/数据，RST 用于硬复位；BUSY 只读，由驱动层轮询等待。 */
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << EPAPER_PIN_DC) | (1ULL << EPAPER_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    gpio_config_t busy_config = {
        .pin_bit_mask = (1ULL << EPAPER_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&busy_config));
}

void epaper_bus_reset(void)
{
    /* 常见 SSD1680 墨水屏复位时序：高-低-高，每一步留一点稳定时间。 */
    gpio_set_level(EPAPER_PIN_RST, 1);
    epaper_bus_delay_ms(20);
    gpio_set_level(EPAPER_PIN_RST, 0);
    epaper_bus_delay_ms(2);
    gpio_set_level(EPAPER_PIN_RST, 1);
    epaper_bus_delay_ms(20);
}

void epaper_bus_write_command(uint8_t command)
{
    gpio_set_level(EPAPER_PIN_DC, 0);
    epaper_bus_write_byte(command);
}

void epaper_bus_write_data(uint8_t data)
{
    gpio_set_level(EPAPER_PIN_DC, 1);
    epaper_bus_write_byte(data);
}

void epaper_bus_write_data_buffer(const uint8_t *data, size_t length)
{
    size_t offset = 0;

    gpio_set_level(EPAPER_PIN_DC, 1);
    while (offset < length) {
        /*
         * Wokwi 的 2.9 寸屏模型更接近 GxEPD 的逐字节写法。
         * 先牺牲一点速度换稳定，等仿真跑通后再考虑批量发送优化。
         */
        epaper_bus_write_byte(data[offset]);
        ++offset;
    }
}

int epaper_bus_is_busy(void)
{
    return gpio_get_level(EPAPER_PIN_BUSY);
}

void epaper_bus_delay_ms(uint32_t delay_ms)
{
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}
