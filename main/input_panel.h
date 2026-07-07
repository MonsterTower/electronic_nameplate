#ifndef INPUT_PANEL_H
#define INPUT_PANEL_H

// 输入面板扫描周期，主循环按这个节奏调用 input_panel_update()。
#define INPUT_PANEL_SCAN_INTERVAL_MS 10

void input_panel_init(void);
void input_panel_update(void);
int input_panel_get_state(void);

#endif
