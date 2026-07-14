#pragma once

#include <stdbool.h>

/*
 * 小智官方接入的最小客户端。
 * 当前阶段仅完成 OTA 配置请求、激活码提示和 WebSocket hello 握手，
 * 不会开始录音或播放语音。
 */
bool xiaozhi_client_start_session(void);
void xiaozhi_client_stop_session(void);
bool xiaozhi_client_is_busy(void);
