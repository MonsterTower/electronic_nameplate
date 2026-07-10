#ifndef DISPLAY_PAGES_H
#define DISPLAY_PAGES_H

#include "app_model.h"

void display_pages_init(void);
void display_pages_show_state(int state, const app_model_t *model);
void display_pages_show_low_battery(const app_model_t *model);
void display_pages_sleep(void);

#endif
