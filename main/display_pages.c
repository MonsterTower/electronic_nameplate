#include "display_pages.h"

#include <stdio.h>

#include "epaper_graphics.h"
#include "xmu_logo.h"

static void display_pages_show_nameplate(void)
{
    /* 状态 0：桌牌页面。当前阶段只显示厦门大学 logo/文字图，后续可叠加姓名等信息。 */
    epaper_graphics_clear(true);

    if (!epaper_graphics_draw_image2lcd_centered(xmu_logo_image, xmu_logo_image_size)) {
        printf("epaper: failed to draw xmu logo\n");
    }

    epaper_graphics_display();
}

void display_pages_init(void)
{
    epaper_graphics_init();
}

void display_pages_show_state(int state)
{
    /* 先保留 0~3 四个页面入口，按钮状态联动放到下一阶段接入。 */
    switch (state) {
    case 0:
        display_pages_show_nameplate();
        break;
    case 1:
    case 2:
    case 3:
    default:
        break;
    }
}
