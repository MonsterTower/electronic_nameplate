#pragma once

/* AI 页只通过本接口发起会话，后续协议、音频和网络实现均收敛在 ai_service 内。 */
void ai_service_start_session(void);
