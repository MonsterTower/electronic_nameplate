#include "display_pages.h"

#include <stdio.h>

#include "display_surface.h"

void display_pages_init(void)
{
    display_surface_init();
}

void display_pages_show_nameplate(const app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    display_surface_clear(DISPLAY_COLOR_WHITE);
    display_surface_draw_rect(8, 8, DISPLAY_SURFACE_WIDTH - 16,
                              DISPLAY_SURFACE_HEIGHT - 16, DISPLAY_COLOR_BLACK);

    /* 顶部保留红色识别文字，用于同时验证 ASCII 和红色平面。 */
    display_surface_draw_ascii(155, 265, "CODEX", DISPLAY_COLOR_RED, 3);
    display_surface_draw_line(24, 244, DISPLAY_SURFACE_WIDTH - 25, 244, DISPLAY_COLOR_BLACK);

    const int name_scale = 5;
    const int name_x = (DISPLAY_SURFACE_WIDTH -
                        display_surface_measure_utf8(model->name, name_scale)) / 2;
    display_surface_draw_utf8(name_x, 144, model->name, DISPLAY_COLOR_BLACK, name_scale);

    display_surface_draw_line(80, 128, DISPLAY_SURFACE_WIDTH - 81, 128, DISPLAY_COLOR_RED);

    const int detail_scale = 3;
    const int organization_x = (DISPLAY_SURFACE_WIDTH -
                                display_surface_measure_utf8(model->organization, detail_scale)) / 2;
    const int topic_x = (DISPLAY_SURFACE_WIDTH -
                         display_surface_measure_utf8(model->topic, detail_scale)) / 2;
    display_surface_draw_utf8(organization_x, 76, model->organization,
                              DISPLAY_COLOR_BLACK, detail_scale);
    display_surface_draw_utf8(topic_x, 24, model->topic, DISPLAY_COLOR_BLACK, detail_scale);

    printf("display: rendering nameplate page\n");
    const esp_err_t err = display_surface_refresh();
    if (err != ESP_OK) {
        printf("display: nameplate refresh failed: %s\n", esp_err_to_name(err));
    }
}

void display_pages_sleep(void)
{
    (void)display_surface_sleep();
}
