#ifndef EPAPER_DRIVER_H
#define EPAPER_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#define EPAPER_LOGICAL_WIDTH 296
#define EPAPER_LOGICAL_HEIGHT 128
#define EPAPER_MEMORY_WIDTH 128
#define EPAPER_MEMORY_HEIGHT 296
#define EPAPER_MEMORY_BYTES_PER_ROW ((EPAPER_MEMORY_WIDTH + 7) / 8)
#define EPAPER_FRAMEBUFFER_SIZE (EPAPER_MEMORY_BYTES_PER_ROW * EPAPER_MEMORY_HEIGHT)

void epaper_driver_init(void);
void epaper_driver_write_command(uint8_t command);
void epaper_driver_write_data(uint8_t data);
void epaper_driver_wait_busy(void);
void epaper_driver_write_framebuffer(const uint8_t *buffer, size_t length);
void epaper_driver_refresh(void);
void epaper_driver_sleep(void);

#endif
