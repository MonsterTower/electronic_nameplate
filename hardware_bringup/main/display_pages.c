#include "display_pages.h"

#include <stdio.h>
#include <string.h>

#include "display_surface.h"

#define PAGE_LEFT 8
#define PAGE_BOTTOM 8
#define PAGE_WIDTH (DISPLAY_SURFACE_WIDTH - PAGE_LEFT * 2)
#define PAGE_HEIGHT (DISPLAY_SURFACE_HEIGHT - PAGE_BOTTOM * 2)
#define PAGE_CONTENT_LEFT 24
#define PAGE_CONTENT_RIGHT (DISPLAY_SURFACE_WIDTH - 24)
#define PAGE_CONTENT_WIDTH (PAGE_CONTENT_RIGHT - PAGE_CONTENT_LEFT)
#define PAGE_TITLE_Y 258
#define PAGE_TITLE_LINE_Y 242
#define PAGE_STATUS_LINE_Y 42

static int page_center_x(const char *text, int scale)
{
    int x = (DISPLAY_SURFACE_WIDTH - display_surface_measure_utf8(text, scale)) / 2;
    return x < PAGE_CONTENT_LEFT ? PAGE_CONTENT_LEFT : x;
}

/* 内容优先缩小局部字体，避免网络数据变长后越出内容区。 */
static void page_draw_fitted_centered(int y, const char *text, display_color_t color,
                                      int preferred_scale)
{
    int scale = preferred_scale;
    while (scale > 1 && display_surface_measure_utf8(text, scale) > PAGE_CONTENT_WIDTH) {
        --scale;
    }
    display_surface_draw_utf8(page_center_x(text, scale), y, text, color, scale);
}

static void page_draw_status_bar(const app_model_t *model)
{
    char battery_text[24];
    char wifi_text[48];

    if (model->has_battery_sample) {
        snprintf(battery_text, sizeof(battery_text), "电池 %.2fV", (double)model->battery_voltage);
    } else {
        snprintf(battery_text, sizeof(battery_text), "%s", "电池 读取中");
    }
    snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi %s", model->wifi_text);

    display_surface_draw_line(PAGE_CONTENT_LEFT, PAGE_STATUS_LINE_Y,
                              PAGE_CONTENT_RIGHT, PAGE_STATUS_LINE_Y, DISPLAY_COLOR_RED);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT, 16, battery_text, DISPLAY_COLOR_BLACK, 1);

    const int wifi_width = display_surface_measure_utf8(wifi_text, 1);
    const int wifi_x = DISPLAY_SURFACE_WIDTH - PAGE_CONTENT_LEFT - wifi_width;
    display_surface_draw_utf8(wifi_x > 180 ? wifi_x : 180, 16, wifi_text, DISPLAY_COLOR_BLACK, 1);
}

static void page_begin(const app_model_t *model, const char *title)
{
    display_surface_clear(DISPLAY_COLOR_WHITE);
    display_surface_draw_rect(PAGE_LEFT, PAGE_BOTTOM, PAGE_WIDTH, PAGE_HEIGHT, DISPLAY_COLOR_BLACK);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT, PAGE_TITLE_Y, title, DISPLAY_COLOR_RED, 2);
    display_surface_draw_line(PAGE_CONTENT_LEFT, PAGE_TITLE_LINE_Y,
                              PAGE_CONTENT_RIGHT, PAGE_TITLE_LINE_Y, DISPLAY_COLOR_BLACK);
    page_draw_status_bar(model);
}

static void page_draw_nameplate(const app_model_t *model)
{
    page_begin(model, "电子桌牌");
    page_draw_fitted_centered(158, model->name, DISPLAY_COLOR_BLACK, 5);
    display_surface_draw_line(78, 145, DISPLAY_SURFACE_WIDTH - 79, 145, DISPLAY_COLOR_RED);
    page_draw_fitted_centered(106, model->organization, DISPLAY_COLOR_BLACK, 3);
    page_draw_fitted_centered(65, model->topic, DISPLAY_COLOR_BLACK, 3);
}

static void page_draw_calendar(const app_model_t *model)
{
    page_begin(model, "电子日历");
    if (!model->time_synced) {
        page_draw_fitted_centered(158, "时间未校准", DISPLAY_COLOR_BLACK, 3);
        page_draw_fitted_centered(104, model->weather, DISPLAY_COLOR_BLACK, 2);
        return;
    }

    page_draw_fitted_centered(190, model->date, DISPLAY_COLOR_BLACK, 3);
    page_draw_fitted_centered(150, model->weekday, DISPLAY_COLOR_RED, 3);
    page_draw_fitted_centered(112, model->time, DISPLAY_COLOR_BLACK, 2);
    page_draw_fitted_centered(78, model->weather, DISPLAY_COLOR_BLACK, 2);
    page_draw_fitted_centered(52, model->calendar_event, DISPLAY_COLOR_BLACK, 1);
}

static void page_draw_schedule(const app_model_t *model)
{
    page_begin(model, "今日课程");
    if (!model->has_course) {
        page_draw_fitted_centered(145, "无课", DISPLAY_COLOR_BLACK, 5);
        return;
    }

    page_draw_fitted_centered(185, "下一节课程", DISPLAY_COLOR_RED, 2);
    page_draw_fitted_centered(142, model->next_course, DISPLAY_COLOR_BLACK, 3);
    page_draw_fitted_centered(99, model->next_course_time, DISPLAY_COLOR_BLACK, 2);
    page_draw_fitted_centered(65, model->next_course_room, DISPLAY_COLOR_BLACK, 2);
}

static void page_draw_device_status(const app_model_t *model)
{
    char battery_text[24];

    page_begin(model, "设备状态");
    if (model->has_battery_sample) {
        snprintf(battery_text, sizeof(battery_text), "%.2fV", (double)model->battery_voltage);
    } else {
        snprintf(battery_text, sizeof(battery_text), "%s", "--.--V");
    }
    page_draw_fitted_centered(170, battery_text, DISPLAY_COLOR_BLACK, 5);
    page_draw_fitted_centered(116, "当前电量", DISPLAY_COLOR_RED, 2);
    page_draw_fitted_centered(78, model->wifi_connected ? "Wi-Fi 已连接" : "Wi-Fi 未连接",
                              DISPLAY_COLOR_BLACK, 2);
}

void display_pages_init(void)
{
    display_surface_init();
}

void display_pages_render(const app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    switch (model->page) {
    case APP_PAGE_NAMEPLATE:
        page_draw_nameplate(model);
        break;
    case APP_PAGE_CALENDAR:
        page_draw_calendar(model);
        break;
    case APP_PAGE_SCHEDULE:
        page_draw_schedule(model);
        break;
    case APP_PAGE_STATUS:
    default:
        page_draw_device_status(model);
        break;
    }

    printf("display: rendering page %d\n", (int)model->page);
    const esp_err_t err = display_surface_refresh();
    if (err != ESP_OK) {
        printf("display: page refresh failed: %s\n", esp_err_to_name(err));
    }
}

void display_pages_sleep(void)
{
    (void)display_surface_sleep();
}
