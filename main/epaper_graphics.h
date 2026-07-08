#ifndef EPAPER_GRAPHICS_H
#define EPAPER_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epaper_driver.h"

void epaper_graphics_init(void);
void epaper_graphics_clear(bool white);
void epaper_graphics_draw_pixel(int x, int y, bool black);
bool epaper_graphics_draw_image2lcd_centered(const unsigned char *image, size_t image_size);
void epaper_graphics_display(void);

#endif
