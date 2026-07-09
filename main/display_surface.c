#include "display_surface.h"

#include <ctype.h>
#include <string.h>

#include "epaper_graphics.h"
#include "font_zh16.h"

static epaper_graphics_color_t display_surface_to_epaper_color(display_color_t color)
{
    switch (color) {
    case DISPLAY_COLOR_BLACK:
        return EPAPER_GRAPHICS_BLACK;
    case DISPLAY_COLOR_RED:
        return EPAPER_GRAPHICS_RED;
    case DISPLAY_COLOR_WHITE:
    default:
        return EPAPER_GRAPHICS_WHITE;
    }
}

static bool font5x7_get_columns(char ch, uint8_t columns[5])
{
    /* 极小 ASCII 字体，先服务 Wi-Fi/版本/日期等调试信息；中文后续走字模或字体库。 */
    static const uint8_t blank[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t *pattern = blank;

    ch = (char)toupper((unsigned char)ch);

    switch (ch) {
    case '0': { static const uint8_t p[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E}; pattern = p; break; }
    case '1': { static const uint8_t p[5] = {0x00, 0x42, 0x7F, 0x40, 0x00}; pattern = p; break; }
    case '2': { static const uint8_t p[5] = {0x42, 0x61, 0x51, 0x49, 0x46}; pattern = p; break; }
    case '3': { static const uint8_t p[5] = {0x21, 0x41, 0x45, 0x4B, 0x31}; pattern = p; break; }
    case '4': { static const uint8_t p[5] = {0x18, 0x14, 0x12, 0x7F, 0x10}; pattern = p; break; }
    case '5': { static const uint8_t p[5] = {0x27, 0x45, 0x45, 0x45, 0x39}; pattern = p; break; }
    case '6': { static const uint8_t p[5] = {0x3C, 0x4A, 0x49, 0x49, 0x30}; pattern = p; break; }
    case '7': { static const uint8_t p[5] = {0x01, 0x71, 0x09, 0x05, 0x03}; pattern = p; break; }
    case '8': { static const uint8_t p[5] = {0x36, 0x49, 0x49, 0x49, 0x36}; pattern = p; break; }
    case '9': { static const uint8_t p[5] = {0x06, 0x49, 0x49, 0x29, 0x1E}; pattern = p; break; }
    case 'A': { static const uint8_t p[5] = {0x7E, 0x11, 0x11, 0x11, 0x7E}; pattern = p; break; }
    case 'B': { static const uint8_t p[5] = {0x7F, 0x49, 0x49, 0x49, 0x36}; pattern = p; break; }
    case 'C': { static const uint8_t p[5] = {0x3E, 0x41, 0x41, 0x41, 0x22}; pattern = p; break; }
    case 'D': { static const uint8_t p[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C}; pattern = p; break; }
    case 'E': { static const uint8_t p[5] = {0x7F, 0x49, 0x49, 0x49, 0x41}; pattern = p; break; }
    case 'F': { static const uint8_t p[5] = {0x7F, 0x09, 0x09, 0x09, 0x01}; pattern = p; break; }
    case 'G': { static const uint8_t p[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A}; pattern = p; break; }
    case 'H': { static const uint8_t p[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F}; pattern = p; break; }
    case 'I': { static const uint8_t p[5] = {0x00, 0x41, 0x7F, 0x41, 0x00}; pattern = p; break; }
    case 'J': { static const uint8_t p[5] = {0x20, 0x40, 0x41, 0x3F, 0x01}; pattern = p; break; }
    case 'K': { static const uint8_t p[5] = {0x7F, 0x08, 0x14, 0x22, 0x41}; pattern = p; break; }
    case 'L': { static const uint8_t p[5] = {0x7F, 0x40, 0x40, 0x40, 0x40}; pattern = p; break; }
    case 'M': { static const uint8_t p[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F}; pattern = p; break; }
    case 'N': { static const uint8_t p[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F}; pattern = p; break; }
    case 'O': { static const uint8_t p[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E}; pattern = p; break; }
    case 'P': { static const uint8_t p[5] = {0x7F, 0x09, 0x09, 0x09, 0x06}; pattern = p; break; }
    case 'Q': { static const uint8_t p[5] = {0x3E, 0x41, 0x51, 0x21, 0x5E}; pattern = p; break; }
    case 'R': { static const uint8_t p[5] = {0x7F, 0x09, 0x19, 0x29, 0x46}; pattern = p; break; }
    case 'S': { static const uint8_t p[5] = {0x46, 0x49, 0x49, 0x49, 0x31}; pattern = p; break; }
    case 'T': { static const uint8_t p[5] = {0x01, 0x01, 0x7F, 0x01, 0x01}; pattern = p; break; }
    case 'U': { static const uint8_t p[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F}; pattern = p; break; }
    case 'V': { static const uint8_t p[5] = {0x1F, 0x20, 0x40, 0x20, 0x1F}; pattern = p; break; }
    case 'W': { static const uint8_t p[5] = {0x3F, 0x40, 0x38, 0x40, 0x3F}; pattern = p; break; }
    case 'X': { static const uint8_t p[5] = {0x63, 0x14, 0x08, 0x14, 0x63}; pattern = p; break; }
    case 'Y': { static const uint8_t p[5] = {0x07, 0x08, 0x70, 0x08, 0x07}; pattern = p; break; }
    case 'Z': { static const uint8_t p[5] = {0x61, 0x51, 0x49, 0x45, 0x43}; pattern = p; break; }
    case '-': { static const uint8_t p[5] = {0x08, 0x08, 0x08, 0x08, 0x08}; pattern = p; break; }
    case ':': { static const uint8_t p[5] = {0x00, 0x36, 0x36, 0x00, 0x00}; pattern = p; break; }
    case '.': { static const uint8_t p[5] = {0x00, 0x60, 0x60, 0x00, 0x00}; pattern = p; break; }
    case '/': { static const uint8_t p[5] = {0x20, 0x10, 0x08, 0x04, 0x02}; pattern = p; break; }
    case '%': { static const uint8_t p[5] = {0x23, 0x13, 0x08, 0x64, 0x62}; pattern = p; break; }
    case ' ': pattern = blank; break;
    default:
        return false;
    }

    memcpy(columns, pattern, 5);
    return true;
}

static void display_surface_draw_char(int x, int y, char ch, display_color_t color, int scale)
{
    uint8_t columns[5];

    if (scale < 1) {
        scale = 1;
    }
    if (!font5x7_get_columns(ch, columns)) {
        return;
    }

    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if ((columns[col] & (1U << row)) == 0) {
                continue;
            }
            display_surface_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static int display_surface_ascii_advance(int scale)
{
    return 6 * scale;
}

static int display_surface_zh_advance(int scale)
{
    return (FONT_ZH16_WIDTH + 1) * scale;
}

static int display_surface_utf8_line_height(int scale)
{
    return (FONT_ZH16_HEIGHT + 2) * scale;
}

static const char *display_surface_next_codepoint(const char *text, uint32_t *codepoint)
{
    const uint8_t *bytes = (const uint8_t *)text;

    if (bytes[0] < 0x80) {
        *codepoint = bytes[0];
        return text + 1;
    }

    if ((bytes[0] & 0xE0) == 0xC0 &&
        bytes[1] != '\0' &&
        (bytes[1] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(bytes[0] & 0x1F) << 6) |
                     (uint32_t)(bytes[1] & 0x3F);
        return text + 2;
    }

    if ((bytes[0] & 0xF0) == 0xE0 &&
        bytes[1] != '\0' &&
        bytes[2] != '\0' &&
        (bytes[1] & 0xC0) == 0x80 &&
        (bytes[2] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(bytes[0] & 0x0F) << 12) |
                     ((uint32_t)(bytes[1] & 0x3F) << 6) |
                     (uint32_t)(bytes[2] & 0x3F);
        return text + 3;
    }

    // 遇到非法 UTF-8 时只跳过当前字节，避免整行显示被一个坏字符拖住。
    *codepoint = '?';
    return text + 1;
}

static int display_surface_codepoint_width(uint32_t codepoint, int scale)
{
    if (codepoint == '\0') {
        return 0;
    }
    if (codepoint < 0x80) {
        return display_surface_ascii_advance(scale);
    }

    return display_surface_zh_advance(scale);
}

static void display_surface_draw_zh16(int x, int y, uint32_t codepoint, display_color_t color, int scale)
{
    const uint16_t *rows = NULL;

    if (!font_zh16_get_glyph(codepoint, &rows)) {
        rows = font_zh16_get_missing_glyph();
    }

    for (int row = 0; row < FONT_ZH16_HEIGHT; ++row) {
        const uint16_t bits = rows[row];
        for (int col = 0; col < FONT_ZH16_WIDTH; ++col) {
            if ((bits & (1U << (FONT_ZH16_WIDTH - 1 - col))) == 0) {
                continue;
            }
            display_surface_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void display_surface_draw_codepoint(int x, int y, uint32_t codepoint, display_color_t color, int scale)
{
    if (codepoint < 0x80) {
        display_surface_draw_char(x, y, (char)codepoint, color, scale);
        return;
    }

    display_surface_draw_zh16(x, y, codepoint, color, scale);
}

void display_surface_init(void)
{
    epaper_graphics_init();
}

void display_surface_clear(display_color_t color)
{
    epaper_graphics_clear(color != DISPLAY_COLOR_BLACK);
    if (color == DISPLAY_COLOR_RED) {
        display_surface_fill_rect(0, 0, DISPLAY_SURFACE_WIDTH, DISPLAY_SURFACE_HEIGHT, DISPLAY_COLOR_RED);
    }
}

void display_surface_draw_pixel(int x, int y, display_color_t color)
{
    epaper_graphics_draw_pixel_color(x, y, display_surface_to_epaper_color(color));
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
            break;
        }
        const int error2 = 2 * error;
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
    if (width <= 0 || height <= 0) {
        return;
    }

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            display_surface_draw_pixel(x + col, y + row, color);
        }
    }
}

void display_surface_draw_text(int x, int y, const char *text, display_color_t color, int scale)
{
    if (text == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    const int char_step = 6 * scale;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        display_surface_draw_char(x + (int)i * char_step, y, text[i], color, scale);
    }
}

void display_surface_draw_utf8_text(int x, int y, const char *text, display_color_t color, int scale)
{
    int cursor_x = x;

    if (text == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }

    while (*text != '\0') {
        uint32_t codepoint = 0;
        text = display_surface_next_codepoint(text, &codepoint);
        if (codepoint == '\n') {
            cursor_x = x;
            y += display_surface_utf8_line_height(scale);
            continue;
        }

        display_surface_draw_codepoint(cursor_x, y, codepoint, color, scale);
        cursor_x += display_surface_codepoint_width(codepoint, scale);
    }
}

void display_surface_draw_utf8_text_box(int x, int y, int width, int height,
                                        const char *text, display_color_t color, int scale)
{
    const int right = x + width;
    const int bottom = y + height;
    int line_height;
    int cursor_x = x;
    int cursor_y = y;

    if (text == NULL || width <= 0 || height <= 0) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    line_height = display_surface_utf8_line_height(scale);

    while (*text != '\0') {
        uint32_t codepoint = 0;
        text = display_surface_next_codepoint(text, &codepoint);

        if (codepoint == '\n') {
            cursor_x = x;
            cursor_y += line_height;
            if (cursor_y + line_height > bottom) {
                return;
            }
            continue;
        }

        const int glyph_width = display_surface_codepoint_width(codepoint, scale);
        if (cursor_x != x && cursor_x + glyph_width > right) {
            cursor_x = x;
            cursor_y += line_height;
        }
        if (cursor_y + line_height > bottom) {
            return;
        }

        display_surface_draw_codepoint(cursor_x, cursor_y, codepoint, color, scale);
        cursor_x += glyph_width;
    }
}

bool display_surface_draw_image2lcd_centered(const unsigned char *image, size_t image_size)
{
    return epaper_graphics_draw_image2lcd_centered(image, image_size);
}

void display_surface_refresh(void)
{
    epaper_graphics_display();
}
