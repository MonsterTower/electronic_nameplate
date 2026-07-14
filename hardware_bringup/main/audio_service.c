#include "audio_service.h"

#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* 已在硬件联调阶段验证的 INMP441 与 NS4168 I2S 引脚。 */
#define AUDIO_MIC_WS_GPIO GPIO_NUM_4
#define AUDIO_MIC_BCLK_GPIO GPIO_NUM_5
#define AUDIO_MIC_DATA_GPIO GPIO_NUM_6
#define AUDIO_SPEAKER_LRCLK_GPIO GPIO_NUM_16
#define AUDIO_SPEAKER_BCLK_GPIO GPIO_NUM_15
#define AUDIO_SPEAKER_DATA_GPIO GPIO_NUM_7

#define AUDIO_SERVICE_DMA_FRAME_COUNT 128U
#define AUDIO_SERVICE_CAPTURE_QUEUE_LENGTH 3U
#define AUDIO_SERVICE_CAPTURE_TASK_STACK_SIZE 6144U
#define AUDIO_SERVICE_CAPTURE_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

static i2s_chan_handle_t s_microphone_channel;
static i2s_chan_handle_t s_speaker_channel;
static QueueHandle_t s_capture_queue;
static TaskHandle_t s_capture_task;
static volatile bool s_session_active;
static bool s_initialized;
static bool s_speaker_enabled;
static uint32_t s_speaker_sample_rate_hz;

static esp_err_t audio_service_create_channels(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2s_chan_config_t microphone_channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    microphone_channel_config.dma_desc_num = 6;
    microphone_channel_config.dma_frame_num = AUDIO_SERVICE_DMA_FRAME_COUNT;
    esp_err_t err = i2s_new_channel(&microphone_channel_config, NULL, &s_microphone_channel);
    if (err != ESP_OK) {
        return err;
    }

    const i2s_std_config_t microphone_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_MIC_BCLK_GPIO,
            .ws = AUDIO_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_MIC_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_microphone_channel, &microphone_config);
    if (err != ESP_OK) {
        return err;
    }

    i2s_chan_config_t speaker_channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    speaker_channel_config.dma_desc_num = 6;
    speaker_channel_config.dma_frame_num = AUDIO_SERVICE_DMA_FRAME_COUNT;
    speaker_channel_config.auto_clear = true;
    err = i2s_new_channel(&speaker_channel_config, &s_speaker_channel, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const i2s_std_config_t speaker_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_SPEAKER_BCLK_GPIO,
            .ws = AUDIO_SPEAKER_LRCLK_GPIO,
            .dout = AUDIO_SPEAKER_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_speaker_channel, &speaker_config);
    if (err != ESP_OK) {
        return err;
    }

    s_capture_queue = xQueueCreate(AUDIO_SERVICE_CAPTURE_QUEUE_LENGTH,
                                   sizeof(audio_service_capture_frame_t));
    if (s_capture_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_speaker_sample_rate_hz = AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ;
    s_initialized = true;
    return ESP_OK;
}

static void audio_service_capture_task(void *argument)
{
    (void)argument;

    int32_t raw_samples[AUDIO_SERVICE_DMA_FRAME_COUNT * 2U] = {0};
    audio_service_capture_frame_t capture_frame = {0};
    size_t frame_sample_count = 0U;

    while (s_session_active) {
        size_t bytes_read = 0U;
        const esp_err_t err = i2s_channel_read(s_microphone_channel, raw_samples, sizeof(raw_samples),
                                               &bytes_read, pdMS_TO_TICKS(200));
        if (err != ESP_OK || bytes_read == 0U) {
            printf("audio: microphone read failed: %s\n", esp_err_to_name(err));
            continue;
        }

        const size_t raw_frame_count = bytes_read / (sizeof(int32_t) * 2U);
        for (size_t index = 0U; index < raw_frame_count; ++index) {
            /* INMP441 左声道为 24 位左对齐数据，转换为后续编码器使用的有符号 16 位 PCM。 */
            capture_frame.samples[frame_sample_count++] = raw_samples[index * 2U] >> 16;
            if (frame_sample_count < AUDIO_SERVICE_CAPTURE_FRAME_SAMPLES) {
                continue;
            }

            if (xQueueSend(s_capture_queue, &capture_frame, 0U) != pdPASS) {
                audio_service_capture_frame_t discarded_frame;
                (void)xQueueReceive(s_capture_queue, &discarded_frame, 0U);
                (void)xQueueSend(s_capture_queue, &capture_frame, 0U);
            }
            frame_sample_count = 0U;
        }
    }

    s_capture_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_service_set_speaker_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_speaker_sample_rate_hz == sample_rate_hz) {
        return ESP_OK;
    }

    if (s_speaker_enabled) {
        (void)i2s_channel_disable(s_speaker_channel);
        s_speaker_enabled = false;
    }
    const i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    const esp_err_t err = i2s_channel_reconfig_std_clock(s_speaker_channel, &clock_config);
    if (err == ESP_OK) {
        s_speaker_sample_rate_hz = sample_rate_hz;
    }
    return err;
}

esp_err_t audio_service_start_session(void)
{
    if (s_session_active) {
        return ESP_OK;
    }

    esp_err_t err = audio_service_create_channels();
    if (err != ESP_OK) {
        printf("audio: service init failed: %s\n", esp_err_to_name(err));
        return err;
    }

    xQueueReset(s_capture_queue);
    err = i2s_channel_enable(s_microphone_channel);
    if (err != ESP_OK) {
        return err;
    }
    s_session_active = true;
    if (xTaskCreate(audio_service_capture_task, "audio_capture",
                    AUDIO_SERVICE_CAPTURE_TASK_STACK_SIZE, NULL,
                    AUDIO_SERVICE_CAPTURE_TASK_PRIORITY, &s_capture_task) != pdPASS) {
        s_session_active = false;
        (void)i2s_channel_disable(s_microphone_channel);
        return ESP_ERR_NO_MEM;
    }

    printf("audio: capture started, %u Hz, %u ms frames\n",
           AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ, AUDIO_SERVICE_CAPTURE_FRAME_MS);
    return ESP_OK;
}

void audio_service_stop_session(void)
{
    s_session_active = false;
    if (s_capture_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_capture_task != NULL) {
            vTaskDelete(s_capture_task);
            s_capture_task = NULL;
        }
    }
    if (s_microphone_channel != NULL) {
        (void)i2s_channel_disable(s_microphone_channel);
    }
    if (s_speaker_enabled) {
        (void)i2s_channel_disable(s_speaker_channel);
        s_speaker_enabled = false;
    }
    if (s_capture_queue != NULL) {
        xQueueReset(s_capture_queue);
    }
    printf("audio: session stopped\n");
}

bool audio_service_is_session_active(void)
{
    return s_session_active;
}

bool audio_service_take_capture_frame(audio_service_capture_frame_t *frame, TickType_t timeout)
{
    return frame != NULL && s_capture_queue != NULL &&
           xQueueReceive(s_capture_queue, frame, timeout) == pdPASS;
}

esp_err_t audio_service_play_pcm_mono(const int16_t *samples, size_t sample_count,
                                      uint32_t sample_rate_hz)
{
    if (samples == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = audio_service_create_channels();
    if (err != ESP_OK) {
        return err;
    }
    err = audio_service_set_speaker_sample_rate(sample_rate_hz);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_speaker_enabled) {
        err = i2s_channel_enable(s_speaker_channel);
        if (err != ESP_OK) {
            return err;
        }
        s_speaker_enabled = true;
    }

    int16_t stereo_samples[AUDIO_SERVICE_DMA_FRAME_COUNT * 2U];
    size_t offset = 0U;
    while (offset < sample_count) {
        const size_t remaining = sample_count - offset;
        const size_t frame_count = remaining < AUDIO_SERVICE_DMA_FRAME_COUNT ?
                                       remaining : AUDIO_SERVICE_DMA_FRAME_COUNT;
        for (size_t index = 0U; index < frame_count; ++index) {
            stereo_samples[index * 2U] = samples[offset + index];
            stereo_samples[index * 2U + 1U] = samples[offset + index];
        }
        size_t bytes_written = 0U;
        err = i2s_channel_write(s_speaker_channel, stereo_samples,
                                frame_count * sizeof(int16_t) * 2U, &bytes_written,
                                pdMS_TO_TICKS(500));
        if (err != ESP_OK || bytes_written != frame_count * sizeof(int16_t) * 2U) {
            return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
        }
        offset += frame_count;
    }
    return ESP_OK;
}
