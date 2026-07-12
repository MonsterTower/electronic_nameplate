#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DISPLAY_COLOR_WHITE = 0,
    DISPLAY_COLOR_BLACK,
    DISPLAY_COLOR_RED,
} display_color_t;

#define DISPLAY_SURFACE_WIDTH 400
#define DISPLAY_SURFACE_HEIGHT 300

void display_surface_init(void);
void display_surface_clear(display_color_t color);
void display_surface_draw_pixel(int x, int y, display_color_t color);
void display_surface_draw_line(int x0, int y0, int x1, int y1, display_color_t color);
void display_surface_draw_rect(int x, int y, int width, int height, display_color_t color);
void display_surface_fill_rect(int x, int y, int width, int height, display_color_t color);
void display_surface_draw_ascii(int x, int y, const char *text, display_color_t color, int scale);
void display_surface_draw_utf8(int x, int y, const char *text, display_color_t color, int scale);
int display_surface_measure_utf8(const char *text, int scale);
esp_err_t display_surface_refresh(void);
esp_err_t display_surface_sleep(void);
