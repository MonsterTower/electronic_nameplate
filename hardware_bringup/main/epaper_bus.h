#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 墨水屏总线层：仅负责 SPI 传输和与屏幕直接相连的 GPIO。 */
esp_err_t epaper_bus_init(void);
esp_err_t epaper_bus_write_command(uint8_t command);
esp_err_t epaper_bus_write_data(const uint8_t *data, size_t length);
esp_err_t epaper_bus_begin_data_stream(void);
esp_err_t epaper_bus_write_data_chunk(const uint8_t *data, size_t length, bool keep_cs_active);
void epaper_bus_end_data_stream(void);
void epaper_bus_set_reset(bool level);
bool epaper_bus_is_busy(void);
