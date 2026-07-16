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
#define PAGE_CALENDAR_COLUMN_COUNT 7
#define PAGE_CALENDAR_ROW_COUNT 6
#define PAGE_CALENDAR_COLUMN_WIDTH (PAGE_CONTENT_WIDTH / PAGE_CALENDAR_COLUMN_COUNT)
#define PAGE_CALENDAR_WEEKDAY_Y 192
#define PAGE_CALENDAR_FIRST_ROW_Y 166
#define PAGE_CALENDAR_ROW_HEIGHT 24

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
    char battery_text[32];
    char wifi_text[48];

    if (model->has_battery_sample) {
        snprintf(battery_text, sizeof(battery_text), "电池 %.2fV %u%%",
                 (double)model->battery_voltage, (unsigned)model->battery_percent);
    } else {
        snprintf(battery_text, sizeof(battery_text), "%s", "电池 读取中");
    }
    snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi %s", model->wifi_text);

    display_surface_draw_line(PAGE_CONTENT_LEFT, PAGE_STATUS_LINE_Y,
                              PAGE_CONTENT_RIGHT, PAGE_STATUS_LINE_Y, DISPLAY_COLOR_RED);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT, 16, battery_text,
                              model->battery_low ? DISPLAY_COLOR_RED : DISPLAY_COLOR_BLACK, 1);

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
    page_draw_fitted_centered(90, model->organization, DISPLAY_COLOR_BLACK, 3);
    page_draw_fitted_centered(50, model->topic, DISPLAY_COLOR_BLACK, 3);
}

static bool page_calendar_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int page_calendar_days_in_month(int year, int month)
{
    static const uint8_t days[] = {31U, 28U, 31U, 30U, 31U, 30U,
                                   31U, 31U, 30U, 31U, 30U, 31U};
    if (month < 1 || month > 12) {
        return 0;
    }
    return month == 2 && page_calendar_is_leap_year(year) ? 29 : days[month - 1];
}

/* 返回星期日为 0、星期六为 6，避免页面层依赖联网模块的时间对象。 */
static int page_calendar_weekday(int year, int month, int day)
{
    static const int month_offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        --year;
    }
    return (year + year / 4 - year / 100 + year / 400 + month_offsets[month - 1] + day) % 7;
}

static bool page_calendar_parse_date(const char *text, int *year, int *month, int *day)
{
    if (text == NULL || year == NULL || month == NULL || day == NULL ||
        sscanf(text, "%d-%d-%d", year, month, day) != 3) {
        return false;
    }
    const int days = page_calendar_days_in_month(*year, *month);
    return *year >= 2000 && *year <= 2099 && days > 0 && *day >= 1 && *day <= days;
}

static void page_draw_calendar(const app_model_t *model)
{
    static const char *const weekday_texts[] = {"日", "一", "二", "三", "四", "五", "六"};

    int year = 0;
    int month = 0;
    int today = 0;
    if (!model->time_synced || !page_calendar_parse_date(model->date, &year, &month, &today)) {
        page_begin(model, "电子日历");
        page_draw_fitted_centered(158, "时间未校准", DISPLAY_COLOR_BLACK, 3);
        page_draw_fitted_centered(104, model->weather, DISPLAY_COLOR_BLACK, 1);
        return;
    }

    char month_text[24];
    snprintf(month_text, sizeof(month_text), "%d年%d月", year, month);
    page_begin(model, month_text);
    const int weather_width = display_surface_measure_utf8(model->weather, 1);
    const int weather_x = PAGE_CONTENT_RIGHT - weather_width;
    display_surface_draw_utf8(weather_x > 166 ? weather_x : 166, 218, model->weather,
                              DISPLAY_COLOR_BLACK, 1);

    /* 星期栏借用黑白红三色，周末单独标红以增强月视图的扫读性。 */
    display_surface_fill_rect(PAGE_CONTENT_LEFT, PAGE_CALENDAR_WEEKDAY_Y,
                              PAGE_CONTENT_WIDTH, 20, DISPLAY_COLOR_BLACK);
    display_surface_fill_rect(PAGE_CONTENT_LEFT, PAGE_CALENDAR_WEEKDAY_Y,
                              PAGE_CALENDAR_COLUMN_WIDTH, 20, DISPLAY_COLOR_RED);
    display_surface_fill_rect(PAGE_CONTENT_LEFT + PAGE_CALENDAR_COLUMN_WIDTH * 6,
                              PAGE_CALENDAR_WEEKDAY_Y, PAGE_CALENDAR_COLUMN_WIDTH, 20,
                              DISPLAY_COLOR_RED);
    for (int weekday = 0; weekday < PAGE_CALENDAR_COLUMN_COUNT; ++weekday) {
        const int text_width = display_surface_measure_utf8(weekday_texts[weekday], 1);
        const int x = PAGE_CONTENT_LEFT + weekday * PAGE_CALENDAR_COLUMN_WIDTH +
                      (PAGE_CALENDAR_COLUMN_WIDTH - text_width) / 2;
        display_surface_draw_utf8(x, PAGE_CALENDAR_WEEKDAY_Y + 2, weekday_texts[weekday],
                                  DISPLAY_COLOR_WHITE, 1);
    }

    const int first_weekday = page_calendar_weekday(year, month, 1);
    const int days_in_month = page_calendar_days_in_month(year, month);
    for (int day = 1; day <= days_in_month; ++day) {
        const int cell_index = first_weekday + day - 1;
        const int row = cell_index / PAGE_CALENDAR_COLUMN_COUNT;
        const int weekday = cell_index % PAGE_CALENDAR_COLUMN_COUNT;
        if (row >= PAGE_CALENDAR_ROW_COUNT) {
            break;
        }

        char day_text[12];
        snprintf(day_text, sizeof(day_text), "%d", day);
        const int text_width = display_surface_measure_utf8(day_text, 2);
        const int x = PAGE_CONTENT_LEFT + weekday * PAGE_CALENDAR_COLUMN_WIDTH +
                      (PAGE_CALENDAR_COLUMN_WIDTH - text_width) / 2;
        const int y = PAGE_CALENDAR_FIRST_ROW_Y - row * PAGE_CALENDAR_ROW_HEIGHT;
        const bool is_today = day == today;
        if (is_today) {
            display_surface_fill_rect(PAGE_CONTENT_LEFT + weekday * PAGE_CALENDAR_COLUMN_WIDTH + 3,
                                      y - 3, PAGE_CALENDAR_COLUMN_WIDTH - 6, 25,
                                      DISPLAY_COLOR_RED);
        }
        const display_color_t color = is_today ? DISPLAY_COLOR_WHITE :
                                      (weekday == 0 || weekday == 6 ? DISPLAY_COLOR_RED :
                                                                       DISPLAY_COLOR_BLACK);
        display_surface_draw_utf8(x, y, day_text, color, 2);
    }
}

static void page_copy_utf8_prefix(char *destination, size_t destination_size,
                                  const char *source, size_t max_characters)
{
    size_t destination_length = 0;
    size_t character_count = 0;

    if (destination_size == 0U) {
        return;
    }
    destination[0] = '\0';
    if (source == NULL) {
        return;
    }

    while (*source != '\0' && character_count < max_characters) {
        const unsigned char first = (unsigned char)*source;
        size_t sequence_length = 1;
        if ((first & 0xF0U) == 0xF0U) {
            sequence_length = 4;
        } else if ((first & 0xE0U) == 0xE0U) {
            sequence_length = 3;
        } else if ((first & 0xC0U) == 0xC0U) {
            sequence_length = 2;
        }
        if (destination_length + sequence_length >= destination_size) {
            break;
        }
        memcpy(&destination[destination_length], source, sequence_length);
        destination_length += sequence_length;
        source += sequence_length;
        ++character_count;
    }
    destination[destination_length] = '\0';
}

static void page_draw_compact_text(int x, int y, int width, const char *text,
                                   display_color_t color, int preferred_scale,
                                   size_t max_characters)
{
    char compact_text[APP_MODEL_COURSE_LEN] = {0};
    page_copy_utf8_prefix(compact_text, sizeof(compact_text), text, max_characters);

    int scale = preferred_scale;
    while (scale > 1 && display_surface_measure_utf8(compact_text, scale) > width) {
        --scale;
    }
    display_surface_draw_utf8(x, y, compact_text, color, scale);
}

static const char *page_weekday_short_text(uint8_t weekday)
{
    static const char *const weekday_texts[] = {
        "", "周一", "周二", "周三", "周四", "周五", "周六", "周日",
    };
    return weekday <= 7U ? weekday_texts[weekday] : "";
}

static const app_schedule_item_t *page_find_highlight_course(const app_model_t *model)
{
    for (uint8_t index = 0; index < model->schedule_item_count; ++index) {
        if (model->schedule_items[index].is_current || model->schedule_items[index].is_next) {
            return &model->schedule_items[index];
        }
    }
    return NULL;
}

static void page_draw_schedule_highlight(const app_schedule_item_t *course)
{
    char title[32];
    char time_range[16];
    char details[APP_MODEL_TEACHER_LEN + APP_MODEL_ROOM_LEN + 4];

    snprintf(time_range, sizeof(time_range), "%s-%s", course->start, course->end);
    if (course->is_current) {
        snprintf(title, sizeof(title), "%s", "当前课");
    } else {
        snprintf(title, sizeof(title), "下一节 %s", page_weekday_short_text(course->weekday));
    }
    snprintf(details, sizeof(details), "%s  %s", course->teacher, course->room);

    display_surface_fill_rect(PAGE_CONTENT_LEFT, 162, PAGE_CONTENT_WIDTH, 72, DISPLAY_COLOR_RED);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT + 8, 216, title, DISPLAY_COLOR_WHITE, 1);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT + 116, 216, time_range, DISPLAY_COLOR_WHITE, 1);
    page_draw_compact_text(PAGE_CONTENT_LEFT + 8, 185, PAGE_CONTENT_WIDTH - 16,
                           course->name, DISPLAY_COLOR_WHITE, 2, 10);
    page_draw_compact_text(PAGE_CONTENT_LEFT + 8, 166, PAGE_CONTENT_WIDTH - 16,
                           details, DISPLAY_COLOR_WHITE, 1, 18);
}

static void page_draw_today_course_row(int y, const app_schedule_item_t *course)
{
    char time_range[16];

    snprintf(time_range, sizeof(time_range), "%s-%s", course->start, course->end);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT, y, time_range, DISPLAY_COLOR_BLACK, 1);
    page_draw_compact_text(122, y, 116, course->name, DISPLAY_COLOR_BLACK, 1, 7);
    page_draw_compact_text(244, y, 132, course->room, DISPLAY_COLOR_BLACK, 1, 8);
}

static int page_next_future_course_index(const app_model_t *model, const bool selected[])
{
    for (uint8_t index = 0; index < model->schedule_item_count; ++index) {
        const app_schedule_item_t *course = &model->schedule_items[index];
        if (course->is_today || selected[index]) {
            continue;
        }
        return (int)index;
    }
    return -1;
}

static void page_draw_future_course_row(int y, const app_schedule_item_t *course)
{
    char prefix[24];
    snprintf(prefix, sizeof(prefix), "%s %s", page_weekday_short_text(course->weekday), course->start);
    display_surface_draw_utf8(PAGE_CONTENT_LEFT, y, prefix, DISPLAY_COLOR_BLACK, 1);
    page_draw_compact_text(142, y, 234, course->name, DISPLAY_COLOR_BLACK, 1, 13);
}

static void page_draw_schedule(const app_model_t *model)
{
    page_begin(model, "课程表");
    if (!model->schedule_data_valid) {
        page_draw_fitted_centered(145, "课表未更新", DISPLAY_COLOR_BLACK, 3);
        return;
    }

    const app_schedule_item_t *highlight = page_find_highlight_course(model);
    if (highlight != NULL) {
        page_draw_schedule_highlight(highlight);
    } else {
        page_draw_fitted_centered(194, "本周无后续课程", DISPLAY_COLOR_BLACK, 2);
    }

    display_surface_draw_utf8(PAGE_CONTENT_LEFT, 145, "今日", DISPLAY_COLOR_RED, 1);
    uint8_t today_row_count = 0;
    for (uint8_t index = 0; index < model->schedule_item_count && today_row_count < 2U; ++index) {
        const app_schedule_item_t *course = &model->schedule_items[index];
        if (!course->is_today || course == highlight) {
            continue;
        }
        page_draw_today_course_row(125 - (int)today_row_count * 20, course);
        ++today_row_count;
    }
    if (today_row_count == 0U && (highlight == NULL || !highlight->is_today)) {
        display_surface_draw_utf8(PAGE_CONTENT_LEFT + 52, 125, "今日无课", DISPLAY_COLOR_BLACK, 1);
    }

    display_surface_draw_utf8(PAGE_CONTENT_LEFT, 83, "近期", DISPLAY_COLOR_RED, 1);
    bool selected[APP_MODEL_SCHEDULE_ITEM_COUNT] = {0};
    for (uint8_t row = 0; row < 2U; ++row) {
        const int future_index = page_next_future_course_index(model, selected);
        if (future_index < 0) {
            break;
        }
        selected[future_index] = true;
        page_draw_future_course_row(64 - (int)row * 18, &model->schedule_items[future_index]);
    }
}

static void page_draw_device_status(const app_model_t *model)
{
    char battery_text[32];

    page_begin(model, "设备状态");
    if (model->has_battery_sample) {
        snprintf(battery_text, sizeof(battery_text), "%.2fV  %u%%",
                 (double)model->battery_voltage, (unsigned)model->battery_percent);
    } else {
        snprintf(battery_text, sizeof(battery_text), "%s", "--.--V  --%");
    }
    page_draw_fitted_centered(170, battery_text,
                              model->battery_low ? DISPLAY_COLOR_RED : DISPLAY_COLOR_BLACK, 4);
    page_draw_fitted_centered(122, "当前电量", DISPLAY_COLOR_RED, 2);
    page_draw_fitted_centered(78,
                              model->battery_critical ? "电量耗尽，请充电" :
                              (model->battery_low ? "电量低，请充电" :
                               (model->wifi_connected ? "Wi-Fi 已连接" : "Wi-Fi 未连接")),
                              model->battery_low ? DISPLAY_COLOR_RED : DISPLAY_COLOR_BLACK, 2);
}

static void page_draw_ai(const app_model_t *model)
{
    page_begin(model, "小智 AI");
    page_draw_fitted_centered(168, "小智 AI", DISPLAY_COLOR_BLACK, 4);
    if (!model->wifi_connected) {
        /* AI 会话无法离线工作，失败时不显示会造成误导的 BOOT 提示。 */
        page_draw_fitted_centered(112, "网络连接失败", DISPLAY_COLOR_RED, 2);
        page_draw_fitted_centered(72, "请检查 Wi-Fi", DISPLAY_COLOR_BLACK, 2);
        return;
    }
    page_draw_fitted_centered(112, "语音对话", DISPLAY_COLOR_RED, 2);
    page_draw_fitted_centered(72, "按 BOOT 开始", DISPLAY_COLOR_BLACK, 2);
}

static void display_pages_refresh(void)
{
    const esp_err_t err = display_surface_refresh();
    if (err != ESP_OK) {
        printf("display: page refresh failed: %s\n", esp_err_to_name(err));
    }
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
        page_draw_device_status(model);
        break;
    case APP_PAGE_AI:
        page_draw_ai(model);
        break;
    default:
        page_draw_device_status(model);
        break;
    }

    printf("display: rendering page %d\n", (int)model->page);
    display_pages_refresh();
}

void display_pages_render_network_waiting(const app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    page_begin(model, "网络连接");
    page_draw_fitted_centered(158, "正在连接 Wi-Fi", DISPLAY_COLOR_BLACK, 3);
    page_draw_fitted_centered(104, "请稍候", DISPLAY_COLOR_RED, 2);
    printf("display: rendering network waiting page\n");
    display_pages_refresh();
    display_pages_sleep();
}

void display_pages_sleep(void)
{
    (void)display_surface_sleep();
}
