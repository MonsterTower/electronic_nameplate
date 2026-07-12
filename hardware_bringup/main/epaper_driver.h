#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define EPAPER_WIDTH 400U
#define EPAPER_HEIGHT 300U
#define EPAPER_BYTES_PER_LINE (EPAPER_WIDTH / 8U)
#define EPAPER_FRAMEBUFFER_SIZE (EPAPER_BYTES_PER_LINE * EPAPER_HEIGHT)

/* SSD1683 驱动层：只接受显存数据，不了解页面内容和字体。 */
esp_err_t epaper_driver_init(void);
esp_err_t epaper_driver_write_framebuffers(const uint8_t *black_buffer,
                                           const uint8_t *red_buffer,
                                           size_t buffer_size);
esp_err_t epaper_driver_refresh(void);
esp_err_t epaper_driver_sleep(void);
