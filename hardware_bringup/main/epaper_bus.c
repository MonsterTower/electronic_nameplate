#include "epaper_bus.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"

/* 下面的 GPIO 编号已由 ESP32-S3-WROOM-1 模组管脚号换算而来。 */
#define EPAPER_BUSY_GPIO GPIO_NUM_10
#define EPAPER_RESET_GPIO GPIO_NUM_8
#define EPAPER_DC_GPIO GPIO_NUM_3
#define EPAPER_CS_GPIO GPIO_NUM_9
#define EPAPER_SCLK_GPIO GPIO_NUM_17
#define EPAPER_MOSI_GPIO GPIO_NUM_18

#define EPAPER_SPI_HOST SPI2_HOST
#define EPAPER_SPI_CLOCK_HZ (2U * 1000U * 1000U)

static spi_device_handle_t s_epaper_spi;
static bool s_epaper_bus_initialized;
static bool s_epaper_data_stream_active;

static esp_err_t epaper_bus_write(bool data_mode, const uint8_t *data, size_t length, bool keep_cs_active)
{
    if (!s_epaper_bus_initialized || data == NULL || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_set_level(EPAPER_DC_GPIO, data_mode ? 1 : 0);

    spi_transaction_t transaction = {
        .length = length * 8U,
        .tx_buffer = data,
        .flags = keep_cs_active ? SPI_TRANS_CS_KEEP_ACTIVE : 0U,
    };
    return spi_device_polling_transmit(s_epaper_spi, &transaction);
}

esp_err_t epaper_bus_init(void)
{
    if (s_epaper_bus_initialized) {
        return ESP_OK;
    }

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << EPAPER_RESET_GPIO) | (1ULL << EPAPER_DC_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << EPAPER_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_config(&busy_config);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(EPAPER_RESET_GPIO, 1);
    gpio_set_level(EPAPER_DC_GPIO, 0);

    const spi_bus_config_t bus_config = {
        .mosi_io_num = EPAPER_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = EPAPER_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    err = spi_bus_initialize(EPAPER_SPI_HOST, &bus_config, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return err;
    }

    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = EPAPER_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = EPAPER_CS_GPIO,
        .queue_size = 1,
    };
    err = spi_bus_add_device(EPAPER_SPI_HOST, &device_config, &s_epaper_spi);
    if (err != ESP_OK) {
        spi_bus_free(EPAPER_SPI_HOST);
        return err;
    }

    s_epaper_bus_initialized = true;
    return ESP_OK;
}

esp_err_t epaper_bus_write_command(uint8_t command)
{
    return epaper_bus_write(false, &command, sizeof(command), false);
}

esp_err_t epaper_bus_write_data(const uint8_t *data, size_t length)
{
    return epaper_bus_write(true, data, length, false);
}

esp_err_t epaper_bus_begin_data_stream(void)
{
    if (!s_epaper_bus_initialized || s_epaper_data_stream_active) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = spi_device_acquire_bus(s_epaper_spi, portMAX_DELAY);
    if (err == ESP_OK) {
        gpio_set_level(EPAPER_DC_GPIO, 1);
        s_epaper_data_stream_active = true;
    }
    return err;
}

esp_err_t epaper_bus_write_data_chunk(const uint8_t *data, size_t length, bool keep_cs_active)
{
    if (!s_epaper_data_stream_active) {
        return ESP_ERR_INVALID_STATE;
    }
    return epaper_bus_write(true, data, length, keep_cs_active);
}

void epaper_bus_end_data_stream(void)
{
    if (s_epaper_data_stream_active) {
        spi_device_release_bus(s_epaper_spi);
        s_epaper_data_stream_active = false;
    }
}

void epaper_bus_set_reset(bool level)
{
    gpio_set_level(EPAPER_RESET_GPIO, level ? 1 : 0);
}

bool epaper_bus_is_busy(void)
{
    /* GDEY042Z98 的 BUSY 为高电平时表示控制器仍在工作。 */
    return gpio_get_level(EPAPER_BUSY_GPIO) != 0;
}
