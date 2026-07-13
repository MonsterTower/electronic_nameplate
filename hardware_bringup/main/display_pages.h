#pragma once

#include "app_model.h"

/* 页面层只读取数据模型，再把绘制请求交给显示表面。 */
void display_pages_init(void);
void display_pages_render(const app_model_t *model);
void display_pages_render_network_waiting(const app_model_t *model);
void display_pages_sleep(void);
