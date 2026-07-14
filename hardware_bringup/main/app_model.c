#include "app_model.h"

#include <stdio.h>
#include <string.h>

void app_model_init(app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    memset(model, 0, sizeof(*model));

    /* 首轮页面使用本地默认数据；后续 Wi-Fi、NTP 与 JSON 只需更新此模型。 */
    model->page = APP_PAGE_NAMEPLATE;
    snprintf(model->name, sizeof(model->name), "%s", "郑锦泽");
    snprintf(model->organization, sizeof(model->organization), "%s", "厦门大学");
    snprintf(model->topic, sizeof(model->topic), "%s", "电子设计");
    snprintf(model->date, sizeof(model->date), "%s", "");
    snprintf(model->weekday, sizeof(model->weekday), "%s", "");
    snprintf(model->time, sizeof(model->time), "%s", "时间未校准");
    model->time_synced = false;
    snprintf(model->weather, sizeof(model->weather), "%s", "天气未更新");
    snprintf(model->calendar_event, sizeof(model->calendar_event), "%s", "今日：电子设计实验");
    model->schedule_item_count = 0;
    model->teaching_week = 0;
    model->schedule_data_valid = false;
    model->wifi_connected = false;
    snprintf(model->wifi_text, sizeof(model->wifi_text), "%s", "未连接");
    model->battery_percent = 0U;
    model->battery_low = false;
    model->battery_critical = false;
}

void app_model_next_page(app_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->page = (app_page_t)((model->page + 1) % APP_PAGE_COUNT);
}

void app_model_previous_page(app_model_t *model)
{
    if (model == NULL) {
        return;
    }
    model->page = model->page == APP_PAGE_NAMEPLATE ? (APP_PAGE_COUNT - 1) :
                  (app_page_t)(model->page - 1);
}

void app_model_update_battery(app_model_t *model, bool has_sample, float voltage)
{
    if (model == NULL) {
        return;
    }
    model->has_battery_sample = has_sample;
    model->battery_voltage = voltage;
}

void app_model_update_battery_level(app_model_t *model, uint8_t percent, bool low, bool critical)
{
    if (model == NULL) {
        return;
    }

    model->battery_percent = percent;
    model->battery_low = low;
    model->battery_critical = critical;
}
