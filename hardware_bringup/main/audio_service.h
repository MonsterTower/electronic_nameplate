#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ 16000U
#define AUDIO_SERVICE_CAPTURE_FRAME_MS 60U
#define AUDIO_SERVICE_CAPTURE_FRAME_SAMPLES \
    (AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ * AUDIO_SERVICE_CAPTURE_FRAME_MS / 1000U)

/* 一帧对应小智上行所需的 60 ms、16 kHz、单声道 PCM。 */
typedef struct {
    int16_t samples[AUDIO_SERVICE_CAPTURE_FRAME_SAMPLES];
} audio_service_capture_frame_t;

esp_err_t audio_service_start_session(void);
void audio_service_stop_session(void);
bool audio_service_is_session_active(void);
bool audio_service_take_capture_frame(audio_service_capture_frame_t *frame, TickType_t timeout);
esp_err_t audio_service_play_pcm_mono(const int16_t *samples, size_t sample_count,
                                      uint32_t sample_rate_hz);
