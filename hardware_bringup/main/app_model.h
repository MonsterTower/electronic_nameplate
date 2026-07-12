#pragma once

#define APP_MODEL_NAME_LEN 32
#define APP_MODEL_ORG_LEN 48
#define APP_MODEL_TOPIC_LEN 64

/* 数据模型不依赖显示、按键或网络模块，后续只由各数据源更新。 */
typedef struct {
    char name[APP_MODEL_NAME_LEN];
    char organization[APP_MODEL_ORG_LEN];
    char topic[APP_MODEL_TOPIC_LEN];
} app_model_t;

void app_model_init(app_model_t *model);
