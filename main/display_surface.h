#ifndef DISPLAY_SURFACE_H
#define DISPLAY_SURFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epaper_driver.h"

#define DISPLAY_SURFACE_WIDTH EPAPER_LOGICAL_WIDTH
#define DISPLAY_SURFACE_HEIGHT EPAPER_LOGICAL_HEIGHT

typedef enum {
    DISPLAY_COLOR_WHITE = 0,
    DISPLAY_COLOR_BLACK,
    DISPLAY_COLOR_RED,
} display_color_t;

/* 页面层只依赖这一组接口，后续从 Wokwi 2.9 寸切到真实 4.2 寸屏时尽量不改页面代码。 */
void display_surface_init(void);
void display_surface_clear(display_color_t color);
void display_surface_draw_pixel(int x, int y, display_color_t color);
void display_surface_draw_line(int x0, int y0, int x1, int y1, display_color_t color);
void display_surface_draw_rect(int x, int y, int width, int height, display_color_t color);
void display_surface_fill_rect(int x, int y, int width, int height, display_color_t color);
/* 5x7 ASCII 小字体，适合 Wi-Fi/IP/版本等短文本。 */
void display_surface_draw_text(int x, int y, const char *text, display_color_t color, int scale);
/* UTF-8 文本接口：ASCII 使用 5x7，小批量中文使用 16x16 点阵，未收录汉字显示占位框。 */
void display_surface_draw_utf8_text(int x, int y, const char *text, display_color_t color, int scale);
void display_surface_draw_utf8_text_box(int x, int y, int width, int height,
                                        const char *text, display_color_t color, int scale);
bool display_surface_draw_image2lcd_centered(const unsigned char *image, size_t image_size);
void display_surface_refresh(void);
void display_surface_sleep(void);

#endif
