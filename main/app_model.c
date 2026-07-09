#include "app_model.h"

#include <stdio.h>
#include <string.h>

#include "battery_monitor.h"
#include "network_service.h"

static const char *weekday_to_cn(const char *weekday)
{
    if (strcmp(weekday, "Mon") == 0) {
        return "周一";
    }
    if (strcmp(weekday, "Tue") == 0) {
        return "周二";
    }
    if (strcmp(weekday, "Wed") == 0) {
        return "周三";
    }
    if (strcmp(weekday, "Thu") == 0) {
        return "周四";
    }
    if (strcmp(weekday, "Fri") == 0) {
        return "周五";
    }
    if (strcmp(weekday, "Sat") == 0) {
        return "周六";
    }
    if (strcmp(weekday, "Sun") == 0) {
        return "周日";
    }

    return "";
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

static int voltage_to_percent(float voltage)
{
    const float empty_voltage = 3.30f;
    const float full_voltage = 4.20f;

    if (voltage <= empty_voltage) {
        return 0;
    }
    if (voltage >= full_voltage) {
        return 100;
    }

    return (int)(((voltage - empty_voltage) * 100.0f) / (full_voltage - empty_voltage) + 0.5f);
}

static void app_model_set_default_courses(app_model_t *model)
{
    model->course_count = 3;

    copy_text(model->courses[0].name, sizeof(model->courses[0].name), "嵌入式系统");
    copy_text(model->courses[0].start, sizeof(model->courses[0].start), "08:00");
    copy_text(model->courses[0].end, sizeof(model->courses[0].end), "09:35");
    copy_text(model->courses[0].room, sizeof(model->courses[0].room), "海韵教学楼204");
    model->courses[0].is_next = false;

    copy_text(model->courses[1].name, sizeof(model->courses[1].name), "传感器应用");
    copy_text(model->courses[1].start, sizeof(model->courses[1].start), "10:10");
    copy_text(model->courses[1].end, sizeof(model->courses[1].end), "11:45");
    copy_text(model->courses[1].room, sizeof(model->courses[1].room), "实验楼A301");
    model->courses[1].is_next = true;

    copy_text(model->courses[2].name, sizeof(model->courses[2].name), "项目答辩");
    copy_text(model->courses[2].start, sizeof(model->courses[2].start), "14:30");
    copy_text(model->courses[2].end, sizeof(model->courses[2].end), "16:05");
    copy_text(model->courses[2].room, sizeof(model->courses[2].room), "工训中心");
    model->courses[2].is_next = false;
}

void app_model_init(app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    memset(model, 0, sizeof(*model));

    // 默认数据先服务仿真验收；后续 Wi-Fi/JSON 接入后，只更新模型，不直接操作页面。
    copy_text(model->nameplate.name, sizeof(model->nameplate.name), "张三");
    copy_text(model->nameplate.org, sizeof(model->nameplate.org), "厦门大学");
    copy_text(model->nameplate.topic, sizeof(model->nameplate.topic), "电子设计与工艺实训");
    copy_text(model->nameplate.qr_text, sizeof(model->nameplate.qr_text), "https://xmu.edu.cn");

    copy_text(model->calendar.date, sizeof(model->calendar.date), "2026-07-09");
    copy_text(model->calendar.time, sizeof(model->calendar.time), "时间未校准");
    copy_text(model->calendar.weekday, sizeof(model->calendar.weekday), "周四");
    copy_text(model->calendar.weather, sizeof(model->calendar.weather), "厦门 多云");
    copy_text(model->calendar.schedule, sizeof(model->calendar.schedule), "今日 10:10 传感器应用");
    model->calendar.time_synced = false;

    model->battery.valid = false;
    model->battery.voltage = 0.0f;
    model->battery.percent = 0;
    copy_text(model->battery.voltage_text, sizeof(model->battery.voltage_text), "--.--V");
    copy_text(model->battery.percent_text, sizeof(model->battery.percent_text), "--%");

    model->network.wifi_connected = false;
    model->network.ntp_synced = false;
    copy_text(model->network.ip_text, sizeof(model->network.ip_text), "0.0.0.0");
    copy_text(model->network.wifi_text, sizeof(model->network.wifi_text), "Wi-Fi 未连接");
    copy_text(model->network.ntp_text, sizeof(model->network.ntp_text), "时间未校准");
    copy_text(model->network.last_error, sizeof(model->network.last_error), "等待联网");

    copy_text(model->firmware_version, sizeof(model->firmware_version), "v0.1");

    app_model_set_default_courses(model);
}

void app_model_update_runtime(app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    if (battery_monitor_has_sample()) {
        model->battery.valid = true;
        model->battery.voltage = battery_monitor_get_voltage();
        model->battery.percent = voltage_to_percent(model->battery.voltage);
        const int voltage_mv = (int)(model->battery.voltage * 1000.0f + 0.5f);
        snprintf(model->battery.voltage_text, sizeof(model->battery.voltage_text),
                 "%d.%02dV", voltage_mv / 1000, (voltage_mv % 1000) / 10);
        snprintf(model->battery.percent_text, sizeof(model->battery.percent_text),
                 "%d%%", model->battery.percent);
    }

    network_service_data_t network = {0};
    network_service_get_snapshot(&network);

    model->network.wifi_connected = network.wifi_connected;
    model->network.ntp_synced = network.time_synced;
    copy_text(model->network.ip_text, sizeof(model->network.ip_text),
              network.ip_text[0] != '\0' ? network.ip_text : "0.0.0.0");
    copy_text(model->network.wifi_text, sizeof(model->network.wifi_text),
              network.wifi_connected ? "Wi-Fi 已连接" : "Wi-Fi 未连接");
    copy_text(model->network.ntp_text, sizeof(model->network.ntp_text),
              network.time_synced ? "时间同步" : "时间未校准");
    copy_text(model->network.last_error, sizeof(model->network.last_error), network.last_error);

    if (network.time_synced) {
        copy_text(model->calendar.date, sizeof(model->calendar.date), network.date_text);
        copy_text(model->calendar.time, sizeof(model->calendar.time), network.time_text);
        copy_text(model->calendar.weekday, sizeof(model->calendar.weekday), weekday_to_cn(network.weekday_text));
        model->calendar.time_synced = true;
    } else {
        // NTP 未同步时不显示伪造日期，只保留明确的未校准状态。
        copy_text(model->calendar.date, sizeof(model->calendar.date), "--");
        copy_text(model->calendar.time, sizeof(model->calendar.time), "时间未校准");
        model->calendar.time_synced = false;
    }
}
