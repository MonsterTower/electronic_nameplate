#include "epaper_graphics.h"

#include <string.h>

static uint8_t s_framebuffer[EPAPER_FRAMEBUFFER_SIZE];

static uint16_t image2lcd_read_le16(const unsigned char *data)
{
    /* IMAGE2LCD 导出的头部里，宽高用小端 16 位保存。 */
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

void epaper_graphics_init(void)
{
    epaper_driver_init();
    epaper_graphics_clear(true);
}

void epaper_graphics_clear(bool white)
{
    /* 墨水屏黑白显存约定：1 是白色，0 是黑色。 */
    memset(s_framebuffer, white ? 0xFF : 0x00, sizeof(s_framebuffer));
}

void epaper_graphics_draw_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= EPAPER_LOGICAL_WIDTH || y < 0 || y >= EPAPER_LOGICAL_HEIGHT) {
        return;
    }

    /*
     * 等价于 GxEPD 的 display.setRotation(1)：
     * 页面层是 296x128 横屏坐标，底层仍写 128x296 物理显存。
     */
    const int memory_x = EPAPER_MEMORY_WIDTH - y - 1;
    const int memory_y = x;
    const size_t index = (size_t)memory_y * EPAPER_MEMORY_BYTES_PER_ROW + (size_t)(memory_x / 8);
    const uint8_t mask = (uint8_t)(0x80U >> (memory_x % 8));

    if (black) {
        s_framebuffer[index] &= (uint8_t)~mask;
    } else {
        s_framebuffer[index] |= mask;
    }
}

bool epaper_graphics_draw_image2lcd_centered(const unsigned char *image, size_t image_size)
{
    if (image == NULL || image_size < 6) {
        return false;
    }

    const uint16_t image_width = image2lcd_read_le16(&image[2]);
    const uint16_t image_height = image2lcd_read_le16(&image[4]);
    const uint16_t row_bytes = (uint16_t)((image_width + 7U) / 8U);
    const size_t payload_size = (size_t)row_bytes * image_height;

    /* 头部 6 字节后才是真正的单色位图数据；尺寸不匹配时直接拒绝绘制。 */
    if ((size_t)6 + payload_size > image_size) {
        return false;
    }

    const unsigned char *payload = &image[6];
    const int offset_x = (EPAPER_LOGICAL_WIDTH - image_width) / 2;
    const int offset_y = (EPAPER_LOGICAL_HEIGHT - image_height) / 2;

    /* 当前导出的图片约定 bit=1 为黑色，只绘制黑点，白底由 clear() 负责。 */
    for (uint16_t y = 0; y < image_height; ++y) {
        for (uint16_t x = 0; x < image_width; ++x) {
            const uint8_t pixel_byte = payload[(size_t)y * row_bytes + (x / 8U)];
            const bool pixel_black = (pixel_byte & (0x80U >> (x % 8U))) != 0;

            if (pixel_black) {
                epaper_graphics_draw_pixel(offset_x + x, offset_y + y, true);
            }
        }
    }

    return true;
}

void epaper_graphics_display(void)
{
    epaper_driver_write_framebuffer(s_framebuffer, sizeof(s_framebuffer));
    epaper_driver_refresh();
}
