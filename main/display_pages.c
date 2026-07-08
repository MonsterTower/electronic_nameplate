#include "display_pages.h"

#include <stdio.h>

#include "display_surface.h"
#include "xmu_logo.h"

static void display_pages_show_nameplate(void)
{
    /* 状态 0：桌牌页面。当前阶段只显示厦门大学 logo/文字图，后续可叠加姓名等信息。 */
    display_surface_clear(DISPLAY_COLOR_WHITE);
    display_surface_draw_rect(10, 8, DISPLAY_SURFACE_WIDTH - 20, DISPLAY_SURFACE_HEIGHT - 16, DISPLAY_COLOR_BLACK);
    display_surface_draw_line(10, 32, DISPLAY_SURFACE_WIDTH - 11, 32, DISPLAY_COLOR_BLACK);
    display_surface_draw_text(12, 9, "CODEX", DISPLAY_COLOR_BLACK, 3);

    if (!display_surface_draw_image2lcd_centered(xmu_logo_image, xmu_logo_image_size)) {
        printf("epaper: failed to draw xmu logo\n");
    }

    display_surface_refresh();
}

void display_pages_init(void)
{
    display_surface_init();
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
