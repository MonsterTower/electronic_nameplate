#include "app_model.h"

#include <stdio.h>
#include <string.h>

void app_model_init(app_model_t *model)
{
    if (model == NULL) {
        return;
    }

    memset(model, 0, sizeof(*model));

    /* 首轮页面使用本地默认数据；后续 Wi-Fi/JSON 只需更新此模型。 */
    snprintf(model->name, sizeof(model->name), "%s", "郑锦泽");
    snprintf(model->organization, sizeof(model->organization), "%s", "厦门大学");
    snprintf(model->topic, sizeof(model->topic), "%s", "电子设计");
}
