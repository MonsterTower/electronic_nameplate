#ifndef INPUT_PANEL_H
#define INPUT_PANEL_H

#include <stdbool.h>

// 输入面板扫描周期，主循环按这个节奏调用 input_panel_update()。
#define INPUT_PANEL_SCAN_INTERVAL_MS 10
#define INPUT_PANEL_BOOT_GPIO GPIO_NUM_0

void input_panel_init(void);
void input_panel_update(void);
int input_panel_get_state(void);
void input_panel_set_state(int state);
void input_panel_ignore_boot_wakeup_press(void);
bool input_panel_take_activity_event(void);

#endif
