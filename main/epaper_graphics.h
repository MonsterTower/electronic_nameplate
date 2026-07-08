#ifndef EPAPER_GRAPHICS_H
#define EPAPER_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epaper_driver.h"

typedef enum {
    EPAPER_GRAPHICS_WHITE = 0,
    EPAPER_GRAPHICS_BLACK,
    EPAPER_GRAPHICS_RED,
} epaper_graphics_color_t;

void epaper_graphics_init(void);
void epaper_graphics_clear(bool white);
void epaper_graphics_draw_pixel(int x, int y, bool black);
void epaper_graphics_draw_pixel_color(int x, int y, epaper_graphics_color_t color);
bool epaper_graphics_draw_image2lcd_centered(const unsigned char *image, size_t image_size);
void epaper_graphics_display(void);

#endif
