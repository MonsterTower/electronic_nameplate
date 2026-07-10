#include "display_pages.h"

#include <stdio.h>
#include <string.h>

#include "display_surface.h"

#define PAGE_BORDER_X 2
#define PAGE_BORDER_Y 2
#define PAGE_HEADER_Y 28
#define PAGE_CONTENT_X 10

static const char *safe_text(const char *text)
{
    return text != NULL ? text : "";
}

static const char *safe_time_text(const app_model_t *model)
{
    if (model != NULL && model->calendar.time_synced) {
        return model->calendar.time;
    }

    return "--:--";
}

static void draw_page_shell(const char *title, const app_model_t *model, bool show_status)
{
    display_surface_clear(DISPLAY_COLOR_WHITE);
    display_surface_draw_rect(PAGE_BORDER_X, PAGE_BORDER_Y,
                              DISPLAY_SURFACE_WIDTH - PAGE_BORDER_X * 2,
                              DISPLAY_SURFACE_HEIGHT - PAGE_BORDER_Y * 2,
                              DISPLAY_COLOR_BLACK);
    display_surface_draw_utf8_text(8, 7, title, DISPLAY_COLOR_BLACK, 1);

    if (show_status && model != NULL) {
        // 页眉角落只放很短的状态摘要，避免挤占页面主要内容。
        display_surface_draw_text(196, 9, model->network.wifi_connected ? "W" : "-", DISPLAY_COLOR_BLACK, 1);
        display_surface_draw_text(214, 9, model->battery.percent_text, DISPLAY_COLOR_BLACK, 1);
        display_surface_draw_text(244, 9, safe_time_text(model), DISPLAY_COLOR_BLACK, 1);
    }

    display_surface_draw_line(PAGE_BORDER_X, PAGE_HEADER_Y,
                              DISPLAY_SURFACE_WIDTH - PAGE_BORDER_X - 1, PAGE_HEADER_Y,
                              DISPLAY_COLOR_BLACK);
}

static void draw_label_value(int x, int y, const char *label, const char *value, int value_width)
{
    display_surface_draw_utf8_text(x, y, label, DISPLAY_COLOR_BLACK, 1);
    display_surface_draw_utf8_text_box(x + 52, y, value_width, 18,
                                       safe_text(value), DISPLAY_COLOR_BLACK, 1);
}

static void copy_utf8_prefix(char *dst, size_t dst_size, const char *src, size_t max_characters)
{
    size_t dst_length = 0;
    size_t character_count = 0;

    if (dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*src != '\0' && character_count < max_characters) {
        const unsigned char first = (unsigned char)*src;
        size_t sequence_length = 1;
        if ((first & 0xF0U) == 0xF0U) {
            sequence_length = 4;
        } else if ((first & 0xE0U) == 0xE0U) {
            sequence_length = 3;
        } else if ((first & 0xC0U) == 0xC0U) {
            sequence_length = 2;
        }
        if (dst_length + sequence_length >= dst_size) {
            break;
        }

        memcpy(&dst[dst_length], src, sequence_length);
        dst_length += sequence_length;
        src += sequence_length;
        ++character_count;
    }
    dst[dst_length] = '\0';
}

static void display_pages_show_nameplate(const app_model_t *model)
{
    draw_page_shell("电子桌牌", model, false);

    display_surface_draw_utf8_text_box(16, 38, 160, 36,
                                       safe_text(model->nameplate.name), DISPLAY_COLOR_BLACK, 2);
    draw_label_value(16, 78, "单位:", model->nameplate.org, 150);
    draw_label_value(16, 100, "主题:", model->nameplate.topic, 150);

    // 当前阶段只预留二维码区域；后续接入二维码编码后替换这里的占位图。
    display_surface_draw_utf8_text(228, 35, "二维码", DISPLAY_COLOR_BLACK, 1);
    display_surface_draw_rect(222, 55, 58, 58, DISPLAY_COLOR_BLACK);
    display_surface_draw_line(222, 55, 279, 112, DISPLAY_COLOR_BLACK);
    display_surface_draw_line(279, 55, 222, 112, DISPLAY_COLOR_BLACK);
    display_surface_draw_text(234, 78, "XMU", DISPLAY_COLOR_BLACK, 2);

    display_surface_refresh();
}

static void display_pages_show_calendar(const app_model_t *model)
{
    draw_page_shell("电子日历", model, true);

    if (model->calendar.time_synced) {
        display_surface_draw_text(PAGE_CONTENT_X + 4, 40, model->calendar.date, DISPLAY_COLOR_BLACK, 2);
        display_surface_draw_utf8_text(PAGE_CONTENT_X + 6, 72, model->calendar.weekday, DISPLAY_COLOR_BLACK, 1);
        display_surface_draw_utf8_text_box(86, 72, 190, 20,
                                           model->calendar.weather, DISPLAY_COLOR_BLACK, 1);
    } else {
        // NTP 未同步时，不显示默认日期，避免用户误以为时间有效。
        display_surface_draw_utf8_text(PAGE_CONTENT_X + 4, 46, "时间未校准", DISPLAY_COLOR_BLACK, 2);
    }

    draw_label_value(PAGE_CONTENT_X + 6, 98, "日程:", model->calendar.schedule, 210);

    display_surface_refresh();
}

static void draw_course_row(int y, const app_course_info_t *course)
{
    char time_range[APP_MODEL_COURSE_TIME_LEN * 2 + 2] = {0};
    char course_name[7] = {0};
    const display_color_t color = course->is_next ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK;

    if (course->is_next) {
        display_surface_fill_rect(6, y - 3, DISPLAY_SURFACE_WIDTH - 12, 23, DISPLAY_COLOR_BLACK);
    }

    snprintf(time_range, sizeof(time_range), "%s-%s", course->start, course->end);
    // 数据模型保留课程全名；当前紧凑课表页只显示前两个 UTF-8 字符。
    copy_utf8_prefix(course_name, sizeof(course_name), course->name, 2);
    display_surface_draw_utf8_text_box(12, y, 34, 18, course_name, color, 1);
    display_surface_draw_text(54, y + 4, time_range, color, 1);
    display_surface_draw_utf8_text_box(126, y, 162, 18, course->room, color, 1);
}

static void display_pages_show_courses(const app_model_t *model)
{
    draw_page_shell("电子课程表", model, true);

    if (model->course_count == 0) {
        display_surface_draw_utf8_text(16, 56, "今日无课程", DISPLAY_COLOR_BLACK, 1);
        display_surface_refresh();
        return;
    }

    const uint8_t visible_count = model->course_count < 3 ? model->course_count : 3;
    for (uint8_t i = 0; i < visible_count; ++i) {
        draw_course_row(38 + (int)i * 29, &model->courses[i]);
    }

    display_surface_refresh();
}

static void display_pages_show_status(const app_model_t *model)
{
    draw_page_shell("状态页", model, false);

    display_surface_draw_utf8_text(18, 36, "电量", DISPLAY_COLOR_BLACK, 1);
    display_surface_draw_text(18, 58, model->battery.percent_text, DISPLAY_COLOR_BLACK, 3);
    display_surface_draw_text(18, 102, model->battery.voltage_text, DISPLAY_COLOR_BLACK, 1);

    display_surface_draw_utf8_text(120, 38, model->network.wifi_text, DISPLAY_COLOR_BLACK, 1);
    draw_label_value(120, 62, "地址:", model->network.ip_text, 118);
    display_surface_draw_utf8_text(120, 84, model->network.ntp_text, DISPLAY_COLOR_BLACK, 1);
    draw_label_value(120, 106, "版本:", model->firmware_version, 118);

    display_surface_refresh();
}

void display_pages_show_low_battery(const app_model_t *model)
{
    display_surface_clear(DISPLAY_COLOR_WHITE);
    display_surface_draw_rect(3, 3, DISPLAY_SURFACE_WIDTH - 6, DISPLAY_SURFACE_HEIGHT - 6,
                              DISPLAY_COLOR_BLACK);
    display_surface_draw_text(43, 32, "LOW BATTERY", DISPLAY_COLOR_BLACK, 2);
    display_surface_draw_utf8_text(112, 76, "电量", DISPLAY_COLOR_BLACK, 2);
    if (model != NULL) {
        display_surface_draw_text(104, 106, model->battery.voltage_text, DISPLAY_COLOR_BLACK, 1);
    }
    display_surface_refresh();
}

void display_pages_init(void)
{
    display_surface_init();
}

void display_pages_show_state(int state, const app_model_t *model)
{
    if (model == NULL) {
        display_surface_clear(DISPLAY_COLOR_WHITE);
        display_surface_draw_text(12, 54, "NO DATA", DISPLAY_COLOR_BLACK, 2);
        display_surface_refresh();
        return;
    }

    switch (state) {
    case 0:
        display_pages_show_nameplate(model);
        break;
    case 1:
        display_pages_show_calendar(model);
        break;
    case 2:
        display_pages_show_courses(model);
        break;
    case 3:
        display_pages_show_status(model);
        break;
    default:
        display_pages_show_nameplate(model);
        break;
    }
}

void display_pages_sleep(void)
{
    display_surface_sleep();
}
