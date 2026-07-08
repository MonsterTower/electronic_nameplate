#ifndef EPAPER_BUS_H
#define EPAPER_BUS_H

#include <stddef.h>
#include <stdint.h>

void epaper_bus_init(void);
void epaper_bus_reset(void);
void epaper_bus_write_command(uint8_t command);
void epaper_bus_write_data(uint8_t data);
void epaper_bus_write_data_buffer(const uint8_t *data, size_t length);
int epaper_bus_is_busy(void);
void epaper_bus_delay_ms(uint32_t delay_ms);

#endif
