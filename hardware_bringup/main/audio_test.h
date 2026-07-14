#pragma once

#include "esp_err.h"

/* 硬件联调阶段仅在冷启动时执行一次，避免定时唤醒反复播放测试音。 */
#define AUDIO_TEST_RUN_ON_COLD_BOOT 0

/* 置为 1 时，冷启动进入持续麦克风监视，不执行桌牌正常启动流程。 */
#define AUDIO_TEST_CONTINUOUS_MIC_MONITOR 0

/* 播放低音量测试音并读取麦克风统计值，用于确认两条 I2S 硬件通路。 */
esp_err_t audio_test_run_once(void);
void audio_test_run_microphone_monitor(void);
