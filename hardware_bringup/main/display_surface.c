#include "display_surface.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "epaper_driver.h"
#include "u8g2.h"

#define U8G2_TILE_WIDTH (DISPLAY_SURFACE_WIDTH / 8)
#define U8G2_TILE_HEIGHT ((DISPLAY_SURFACE_HEIGHT + 7) / 8)
#define U8G2_BUFFER_SIZE (U8G2_TILE_WIDTH * U8G2_TILE_HEIGHT * 8)
#define U8G2_TEXT_TOP_MARGIN 2

extern const uint8_t u8g2_font_wqy16_t_gb2312[];

static uint8_t s_black_buffer[EPAPER_FRAMEBUFFER_SIZE];
static uint8_t s_red_buffer[EPAPER_FRAMEBUFFER_SIZE];

/* U8g2 的临时文字层只在渲染单段 UTF-8 文本时使用，不参与墨水屏传输。 */
static u8g2_t s_font_context;
static uint8_t s_font_buffer[U8G2_BUFFER_SIZE];
static bool s_font_initialized;

static const u8x8_display_info_t s_font_display_info = {
    .chip_enable_level = 0,
    .chip_disable_level = 1,
    .post_chip_enable_wait_ns = 0,
    .pre_chip_disable_wait_ns = 0,
    .reset_pulse_width_ms = 0,
    .post_reset_wait_ms = 0,
    .sda_setup_time_ns = 0,
    .sck_pulse_width_ns = 0,
    .sck_clock_hz = 0,
    .spi_mode = 0,
    .i2c_bus_clock_100kHz = 0,
    .data_setup_time_ns = 0,
    .write_pulse_width_ns = 0,
    .tile_width = U8G2_TILE_WIDTH,
    .tile_height = U8G2_TILE_HEIGHT,
    .default_x_offset = 0,
    .flipmode_x_offset = 0,
    .pixel_width = DISPLAY_SURFACE_WIDTH,
    .pixel_height = DISPLAY_SURFACE_HEIGHT,
};

static uint8_t display_surface_u8g2_display_cb(u8x8_t *u8x8, uint8_t message,
                                                uint8_t argument, void *argument_ptr)
{
    (void)argument;
    (void)argument_ptr;

    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &s_font_display_info);
    }
    return 1;
}

static void display_surface_font_init(void)
{
    if (s_font_initialized) {
        return;
    }

    u8g2_SetupDisplay(&s_font_context, display_surface_u8g2_display_cb,
                      u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&s_font_context, s_font_buffer, U8G2_TILE_HEIGHT,
                     u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_SetFont(&s_font_context, u8g2_font_wqy16_t_gb2312);
    u8g2_SetFontMode(&s_font_context, 1); /* 透明文字层，只写入字形像素。 */
    u8g2_SetFontPosTop(&s_font_context);
    s_font_initialized = true;
}

static bool display_surface_font_prepare(const char *text, int *min_x, int *min_y,
                                         int *max_x, int *max_y)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    display_surface_font_init();
    u8g2_ClearBuffer(&s_font_context);
    /*
     * U8g2 字模的顶部可能有少量像素落在绘制基准点之外。
     * 预留顶边空间，避免临时画布裁掉每段文字的顶部笔画；
     * 后续会根据实际包围盒重新定位，因此不会改变页面排版坐标。
     */
    (void)u8g2_DrawUTF8(&s_font_context, 0, U8G2_TEXT_TOP_MARGIN, text);

    *min_x = DISPLAY_SURFACE_WIDTH;
    *min_y = DISPLAY_SURFACE_HEIGHT;
    *max_x = -1;
    *max_y = -1;

    for (int source_y = 0; source_y < DISPLAY_SURFACE_HEIGHT; ++source_y) {
        const size_t row_offset = (size_t)(source_y / 8) * DISPLAY_SURFACE_WIDTH;
        const uint8_t mask = (uint8_t)(1U << (source_y % 8));

        for (int source_x = 0; source_x < DISPLAY_SURFACE_WIDTH; ++source_x) {
            if ((s_font_buffer[row_offset + source_x] & mask) == 0U) {
                continue;
            }

            if (source_x < *min_x) {
                *min_x = source_x;
            }
            if (source_x > *max_x) {
                *max_x = source_x;
            }
            if (source_y < *min_y) {
                *min_y = source_y;
            }
            if (source_y > *max_y) {
                *max_y = source_y;
            }
        }
    }

    return *max_x >= *min_x && *max_y >= *min_y;
}

static void display_surface_draw_ascii_char(int x, int y, char character,
                                            display_color_t color, int scale)
{
    static const uint8_t glyph_c[5] = {0x3E, 0x41, 0x41, 0x41, 0x22};
    static const uint8_t glyph_d[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C};
    static const uint8_t glyph_e[5] = {0x7F, 0x49, 0x49, 0x49, 0x41};
    static const uint8_t glyph_o[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E};
    static const uint8_t glyph_x[5] = {0x63, 0x14, 0x08, 0x14, 0x63};
    const uint8_t *glyph = NULL;

    switch (character) {
    case 'C': glyph = glyph_c; break;
    case 'D': glyph = glyph_d; break;
    case 'E': glyph = glyph_e; break;
    case 'O': glyph = glyph_o; break;
    case 'X': glyph = glyph_x; break;
    default: return;
    }

    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 7; ++row) {
            if ((glyph[column] & (1U << row)) != 0U) {
                display_surface_fill_rect(x + column * scale, y + (6 - row) * scale,
                                          scale, scale, color);
            }
        }
    }
}

void display_surface_init(void)
{
    (void)epaper_driver_init();
    display_surface_font_init();
    display_surface_clear(DISPLAY_COLOR_WHITE);
}

void display_surface_clear(display_color_t color)
{
    memset(s_black_buffer, color == DISPLAY_COLOR_BLACK ? 0x00 : 0xFF,
           sizeof(s_black_buffer));
    memset(s_red_buffer, color == DISPLAY_COLOR_RED ? 0xFF : 0x00,
           sizeof(s_red_buffer));
}

void display_surface_draw_pixel(int x, int y, display_color_t color)
{
    if (x < 0 || x >= DISPLAY_SURFACE_WIDTH || y < 0 || y >= DISPLAY_SURFACE_HEIGHT) {
        return;
    }

    /* 页面坐标以左下角为原点，显存则按控制器从顶部开始的行顺序保存。 */
    const int controller_y = DISPLAY_SURFACE_HEIGHT - 1 - y;
    const size_t index = (size_t)controller_y * EPAPER_BYTES_PER_LINE + (size_t)(x / 8);
    const uint8_t mask = (uint8_t)(0x80U >> (x % 8));

    switch (color) {
    case DISPLAY_COLOR_BLACK:
        s_black_buffer[index] &= (uint8_t)~mask;
        s_red_buffer[index] &= (uint8_t)~mask;
        break;
    case DISPLAY_COLOR_RED:
        s_black_buffer[index] |= mask;
        s_red_buffer[index] |= mask;
        break;
    case DISPLAY_COLOR_WHITE:
    default:
        s_black_buffer[index] |= mask;
        s_red_buffer[index] &= (uint8_t)~mask;
        break;
    }
}

void display_surface_draw_line(int x0, int y0, int x1, int y1, display_color_t color)
{
    const int dx = x0 < x1 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = y0 < y1 ? y0 - y1 : y1 - y0;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        display_surface_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        const int error2 = error * 2;
        if (error2 >= dy) {
            error += dy;
            x0 += sx;
        }
        if (error2 <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void display_surface_draw_rect(int x, int y, int width, int height, display_color_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    display_surface_draw_line(x, y, x + width - 1, y, color);
    display_surface_draw_line(x, y + height - 1, x + width - 1, y + height - 1, color);
    display_surface_draw_line(x, y, x, y + height - 1, color);
    display_surface_draw_line(x + width - 1, y, x + width - 1, y + height - 1, color);
}

void display_surface_fill_rect(int x, int y, int width, int height, display_color_t color)
{
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            display_surface_draw_pixel(x + column, y + row, color);
        }
    }
}

void display_surface_draw_ascii(int x, int y, const char *text, display_color_t color, int scale)
{
    if (text == NULL || scale < 1) {
        return;
    }
    for (size_t index = 0; text[index] != '\0'; ++index) {
        display_surface_draw_ascii_char(x + (int)index * 6 * scale, y,
                                        text[index], color, scale);
    }
}

int display_surface_measure_utf8(const char *text, int scale)
{
    int min_x;
    int min_y;
    int max_x;
    int max_y;

    if (scale < 1 || !display_surface_font_prepare(text, &min_x, &min_y, &max_x, &max_y)) {
        return 0;
    }
    return (max_x - min_x + 1) * scale;
}

void display_surface_draw_utf8(int x, int y, const char *text, display_color_t color, int scale)
{
    int min_x;
    int min_y;
    int max_x;
    int max_y;

    if (scale < 1 || !display_surface_font_prepare(text, &min_x, &min_y, &max_x, &max_y)) {
        return;
    }

    for (int source_y = min_y; source_y <= max_y; ++source_y) {
        const size_t row_offset = (size_t)(source_y / 8) * DISPLAY_SURFACE_WIDTH;
        const uint8_t mask = (uint8_t)(1U << (source_y % 8));

        for (int source_x = min_x; source_x <= max_x; ++source_x) {
            if ((s_font_buffer[row_offset + source_x] & mask) == 0U) {
                continue;
            }
            display_surface_fill_rect(x + (source_x - min_x) * scale,
                                      y + (max_y - source_y) * scale,
                                      scale, scale, color);
        }
    }
}

esp_err_t display_surface_refresh(void)
{
    esp_err_t err = epaper_driver_write_framebuffers(s_black_buffer, s_red_buffer,
                                                      sizeof(s_black_buffer));
    if (err == ESP_OK) {
        err = epaper_driver_refresh();
    }
    return err;
}

esp_err_t display_surface_sleep(void)
{
    return epaper_driver_sleep();
}
