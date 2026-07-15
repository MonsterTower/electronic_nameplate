#include "audio_service.h"

#include <limits.h>
#include <stdio.h>

#include "driver/i2s_std.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* 该引脚组与官方 xiaozhi-esp32 的 otto-robot 板型完全一致。 */
#define AUDIO_MIC_WS_GPIO GPIO_NUM_4
#define AUDIO_MIC_BCLK_GPIO GPIO_NUM_5
#define AUDIO_MIC_DATA_GPIO GPIO_NUM_6
#define AUDIO_SPEAKER_LRCLK_GPIO GPIO_NUM_16
#define AUDIO_SPEAKER_BCLK_GPIO GPIO_NUM_15
#define AUDIO_SPEAKER_DATA_GPIO GPIO_NUM_7

#define AUDIO_SERVICE_DMA_FRAME_COUNT 128U
#define AUDIO_SERVICE_CAPTURE_QUEUE_LENGTH 3U
#define AUDIO_SERVICE_OPUS_QUEUE_LENGTH 4U
#define AUDIO_SERVICE_CAPTURE_TASK_STACK_SIZE 12288U
#define AUDIO_SERVICE_CAPTURE_TASK_PRIORITY 8U
#define AUDIO_SERVICE_CAPTURE_TASK_CORE 0
#define AUDIO_SERVICE_CODEC_TASK_STACK_SIZE (2048U * 12U)
#define AUDIO_SERVICE_CODEC_TASK_PRIORITY 2U
#define AUDIO_SERVICE_SPEAKER_GAIN_PERCENT 60U
#define AUDIO_SERVICE_PLAYBACK_DEFAULT_SAMPLE_RATE_HZ 24000U
#define AUDIO_SERVICE_PLAYBACK_MAX_SAMPLE_RATE_HZ 24000U
#define AUDIO_SERVICE_PLAYBACK_MAX_FRAME_MS 120U
#define AUDIO_SERVICE_PLAYBACK_MAX_SAMPLES \
    (AUDIO_SERVICE_PLAYBACK_MAX_SAMPLE_RATE_HZ * AUDIO_SERVICE_PLAYBACK_MAX_FRAME_MS / 1000U)

static i2s_chan_handle_t s_microphone_channel;
static i2s_chan_handle_t s_speaker_channel;
static QueueHandle_t s_capture_queue;
static QueueHandle_t s_opus_queue;
static TaskHandle_t s_capture_task;
static TaskHandle_t s_codec_task;
static volatile bool s_session_active;
static volatile bool s_capture_enabled;
static bool s_initialized;
static bool s_speaker_enabled;
static uint32_t s_speaker_sample_rate_hz;
static void *s_opus_encoder;
static void *s_opus_decoder;
static uint32_t s_decoder_sample_rate_hz;
static uint32_t s_decoder_frame_duration_ms;
static int s_encoder_input_size;
static int s_encoder_output_size;
static uint32_t s_playback_packet_count;
static uint32_t s_playback_sample_count;
static int16_t s_playback_minimum;
static int16_t s_playback_maximum;
static uint64_t s_playback_sum_squares;

/* 采集、编码和解码任务均复用静态缓冲，避免大数组占用任务栈。 */
static int32_t s_capture_raw_samples[AUDIO_SERVICE_DMA_FRAME_COUNT];
static audio_service_capture_frame_t s_capture_assembly_frame;
static audio_service_capture_frame_t s_capture_discarded_frame;
static audio_service_capture_frame_t s_codec_input_frame;
static audio_service_opus_packet_t s_codec_output_packet;
static int16_t s_decoded_samples[AUDIO_SERVICE_PLAYBACK_MAX_SAMPLES];
/* NS4168 已由本地纯音验证为 16 位立体声时序，双槽写入相同单声道采样。 */
static int16_t s_speaker_samples[AUDIO_SERVICE_DMA_FRAME_COUNT * 2U];

static esp_err_t audio_service_open_opus_encoder(void);
static esp_err_t audio_service_open_opus_decoder(uint32_t sample_rate_hz,
                                                  uint32_t frame_duration_ms);

static int16_t audio_service_saturate_pcm16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static uint32_t audio_service_integer_sqrt(uint64_t value)
{
    uint32_t root = 0U;
    for (int bit = 31; bit >= 0; --bit) {
        const uint64_t candidate = (uint64_t)root | (1ULL << bit);
        if (candidate <= value / candidate) {
            root = (uint32_t)candidate;
        }
    }
    return root;
}

static int16_t audio_service_apply_speaker_gain(int16_t sample)
{
    /* 对齐官方音量曲线：40% 对应约 16% 振幅，避免语音峰值推动功放失真。 */
    const int32_t gain_squared = AUDIO_SERVICE_SPEAKER_GAIN_PERCENT *
                                 AUDIO_SERVICE_SPEAKER_GAIN_PERCENT;
    const int32_t scaled = (int32_t)sample * gain_squared / 10000;
    return audio_service_saturate_pcm16(scaled);
}

static esp_err_t audio_service_create_channels(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* 官方 NoAudioCodecSimplex：I2S1 专用于 INMP441 输入，32 位单声道左声道。 */
    i2s_chan_config_t microphone_channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    microphone_channel_config.dma_desc_num = 6;
    microphone_channel_config.dma_frame_num = AUDIO_SERVICE_DMA_FRAME_COUNT;
    esp_err_t err = i2s_new_channel(&microphone_channel_config, NULL, &s_microphone_channel);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t microphone_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_MONO),
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
    microphone_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    err = i2s_channel_init_std_mode(s_microphone_channel, &microphone_config);
    if (err != ESP_OK) {
        return err;
    }

    /* NS4168 的硬件纯音测试已验证为 16 位立体声，左右槽写入同一单声道数据。 */
    i2s_chan_config_t speaker_channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    speaker_channel_config.dma_desc_num = 6;
    speaker_channel_config.dma_frame_num = AUDIO_SERVICE_DMA_FRAME_COUNT;
    speaker_channel_config.auto_clear = true;
    err = i2s_new_channel(&speaker_channel_config, &s_speaker_channel, NULL);
    if (err != ESP_OK) {
        return err;
    }
    i2s_std_config_t speaker_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SERVICE_PLAYBACK_DEFAULT_SAMPLE_RATE_HZ),
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
    s_opus_queue = xQueueCreate(AUDIO_SERVICE_OPUS_QUEUE_LENGTH,
                                sizeof(audio_service_opus_packet_t));
    if (s_capture_queue == NULL || s_opus_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_speaker_sample_rate_hz = AUDIO_SERVICE_PLAYBACK_DEFAULT_SAMPLE_RATE_HZ;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t audio_service_prepare(void)
{
    const esp_err_t err = audio_service_create_channels();
    if (err == ESP_OK) {
        printf("audio: official simplex I2S prepared on core %d\n", xPortGetCoreID());
    }
    return err;
}

static void audio_service_capture_task(void *argument)
{
    (void)argument;
    size_t frame_sample_count = 0U;

    printf("audio: capture task stack free=%lu bytes\n",
           (unsigned long)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    while (s_session_active) {
        size_t bytes_read = 0U;
        const esp_err_t err = i2s_channel_read(s_microphone_channel, s_capture_raw_samples,
                                               sizeof(s_capture_raw_samples), &bytes_read,
                                               pdMS_TO_TICKS(200));
        if (err != ESP_OK || bytes_read == 0U) {
            printf("audio: microphone read failed: %s\n", esp_err_to_name(err));
            continue;
        }

        /* 手动结束一句话后仍保持 I2S 时钟运行，但丢弃后续采样。 */
        if (!s_capture_enabled) {
            frame_sample_count = 0U;
            continue;
        }

        const size_t raw_sample_count = bytes_read / sizeof(int32_t);
        for (size_t index = 0U; index < raw_sample_count; ++index) {
            /* 与官方 NoAudioCodecSimplex 相同：32 位 I2S 数据右移 12 位后饱和为 PCM16。 */
            s_capture_assembly_frame.samples[frame_sample_count++] =
                audio_service_saturate_pcm16(s_capture_raw_samples[index] >> 12);
            if (frame_sample_count < AUDIO_SERVICE_CAPTURE_FRAME_SAMPLES) {
                continue;
            }
            if (xQueueSend(s_capture_queue, &s_capture_assembly_frame, 0U) != pdPASS) {
                (void)xQueueReceive(s_capture_queue, &s_capture_discarded_frame, 0U);
                (void)xQueueSend(s_capture_queue, &s_capture_assembly_frame, 0U);
            }
            frame_sample_count = 0U;
        }
    }

    s_capture_task = NULL;
    vTaskDelete(NULL);
}

static void audio_service_codec_task(void *argument)
{
    (void)argument;

    printf("audio: official Opus task stack free=%lu bytes\n",
           (unsigned long)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    while (s_session_active) {
        if (xQueueReceive(s_capture_queue, &s_codec_input_frame, pdMS_TO_TICKS(200)) != pdPASS) {
            continue;
        }
        if (s_opus_encoder == NULL) {
            continue;
        }

        esp_audio_enc_in_frame_t input = {
            .buffer = (uint8_t *)s_codec_input_frame.samples,
            .len = sizeof(s_codec_input_frame.samples),
        };
        esp_audio_enc_out_frame_t output = {
            .buffer = s_codec_output_packet.data,
            .len = (uint32_t)s_encoder_output_size,
            .encoded_bytes = 0U,
        };
        if (esp_opus_enc_process(s_opus_encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
            output.encoded_bytes == 0U || output.encoded_bytes > sizeof(s_codec_output_packet.data)) {
            printf("audio: Opus encode failed\n");
            continue;
        }

        s_codec_output_packet.length = output.encoded_bytes;
        if (xQueueSend(s_opus_queue, &s_codec_output_packet, 0U) != pdPASS) {
            audio_service_opus_packet_t discarded_packet;
            (void)xQueueReceive(s_opus_queue, &discarded_packet, 0U);
            (void)xQueueSend(s_opus_queue, &s_codec_output_packet, 0U);
        }
    }

    s_codec_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_service_set_speaker_sample_rate(uint32_t sample_rate_hz)
{
    if (sample_rate_hz == 0U || sample_rate_hz > AUDIO_SERVICE_PLAYBACK_MAX_SAMPLE_RATE_HZ) {
        return ESP_ERR_NOT_SUPPORTED;
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

static esp_err_t audio_service_open_opus_encoder(void)
{
    if (s_opus_encoder != NULL) {
        return ESP_OK;
    }

    const esp_opus_enc_config_t config = {
        .sample_rate = AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };
    if (esp_opus_enc_open((void *)&config, sizeof(config), &s_opus_encoder) != ESP_AUDIO_ERR_OK ||
        s_opus_encoder == NULL ||
        esp_opus_enc_get_frame_size(s_opus_encoder, &s_encoder_input_size,
                                    &s_encoder_output_size) != ESP_AUDIO_ERR_OK) {
        if (s_opus_encoder != NULL) {
            esp_opus_enc_close(s_opus_encoder);
            s_opus_encoder = NULL;
        }
        printf("audio: official Opus encoder initialization failed\n");
        return ESP_FAIL;
    }
    if (s_encoder_input_size != (int)sizeof(audio_service_capture_frame_t) ||
        s_encoder_output_size <= 0 || s_encoder_output_size > AUDIO_SERVICE_OPUS_PACKET_MAX_SIZE) {
        printf("audio: unexpected official Opus frame size in=%d out=%d\n",
               s_encoder_input_size, s_encoder_output_size);
        esp_opus_enc_close(s_opus_encoder);
        s_opus_encoder = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    printf("audio: official Opus encoder ready, input=%d output=%d bytes\n",
           s_encoder_input_size, s_encoder_output_size);
    return ESP_OK;
}

static esp_opus_dec_frame_duration_t audio_service_to_opus_duration(uint32_t frame_duration_ms)
{
    switch (frame_duration_ms) {
    case 10U:
        return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
    case 20U:
        return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    case 40U:
        return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
    case 60U:
        return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
    default:
        return ESP_OPUS_DEC_FRAME_DURATION_INVALID;
    }
}

static esp_err_t audio_service_open_opus_decoder(uint32_t sample_rate_hz,
                                                  uint32_t frame_duration_ms)
{
    if (sample_rate_hz == 0U || sample_rate_hz > AUDIO_SERVICE_PLAYBACK_MAX_SAMPLE_RATE_HZ ||
        frame_duration_ms == 0U || frame_duration_ms > AUDIO_SERVICE_PLAYBACK_MAX_FRAME_MS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_opus_dec_frame_duration_t duration =
        audio_service_to_opus_duration(frame_duration_ms);
    if (duration == ESP_OPUS_DEC_FRAME_DURATION_INVALID) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_opus_decoder != NULL && s_decoder_sample_rate_hz == sample_rate_hz &&
        s_decoder_frame_duration_ms == frame_duration_ms) {
        return ESP_OK;
    }

    if (s_opus_decoder != NULL) {
        esp_opus_dec_close(s_opus_decoder);
        s_opus_decoder = NULL;
    }
    const esp_opus_dec_cfg_t config = {
        .sample_rate = sample_rate_hz,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = duration,
        .self_delimited = false,
    };
    if (esp_opus_dec_open((void *)&config, sizeof(config), &s_opus_decoder) != ESP_AUDIO_ERR_OK ||
        s_opus_decoder == NULL) {
        printf("audio: official Opus decoder initialization failed\n");
        return ESP_FAIL;
    }
    s_decoder_sample_rate_hz = sample_rate_hz;
    s_decoder_frame_duration_ms = frame_duration_ms;
    return ESP_OK;
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
    xQueueReset(s_opus_queue);
    err = audio_service_open_opus_encoder();
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(s_microphone_channel);
    if (err != ESP_OK) {
        return err;
    }

    s_session_active = true;
    s_capture_enabled = true;
    if (xTaskCreatePinnedToCore(audio_service_capture_task, "audio_capture",
                                AUDIO_SERVICE_CAPTURE_TASK_STACK_SIZE, NULL,
                                AUDIO_SERVICE_CAPTURE_TASK_PRIORITY, &s_capture_task,
                                AUDIO_SERVICE_CAPTURE_TASK_CORE) != pdPASS ||
        xTaskCreate(audio_service_codec_task, "opus_codec", AUDIO_SERVICE_CODEC_TASK_STACK_SIZE,
                    NULL, AUDIO_SERVICE_CODEC_TASK_PRIORITY, &s_codec_task) != pdPASS) {
        audio_service_stop_session();
        return ESP_ERR_NO_MEM;
    }

    printf("audio: official simplex capture started, %u Hz, %u ms frames\n",
           AUDIO_SERVICE_CAPTURE_SAMPLE_RATE_HZ, AUDIO_SERVICE_CAPTURE_FRAME_MS);
    return ESP_OK;
}

void audio_service_pause_capture(void)
{
    s_capture_enabled = false;
}

void audio_service_finish_playback(void)
{
    bool released = false;
    if (s_speaker_enabled) {
        (void)i2s_channel_disable(s_speaker_channel);
        s_speaker_enabled = false;
        released = true;
    }
    if (s_opus_decoder != NULL) {
        esp_opus_dec_close(s_opus_decoder);
        s_opus_decoder = NULL;
        released = true;
    }
    s_decoder_sample_rate_hz = 0U;
    s_decoder_frame_duration_ms = 0U;
    if (released) {
        printf("audio: TTS playback resources released\n");
    }
}

void audio_service_stop_session(void)
{
    s_session_active = false;
    s_capture_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(250));
    if (s_capture_task != NULL) {
        vTaskDelete(s_capture_task);
        s_capture_task = NULL;
    }
    if (s_codec_task != NULL) {
        vTaskDelete(s_codec_task);
        s_codec_task = NULL;
    }
    if (s_microphone_channel != NULL) {
        (void)i2s_channel_disable(s_microphone_channel);
    }
    audio_service_finish_playback();
    if (s_capture_queue != NULL) {
        xQueueReset(s_capture_queue);
    }
    if (s_opus_queue != NULL) {
        xQueueReset(s_opus_queue);
    }
    if (s_opus_encoder != NULL) {
        esp_opus_enc_close(s_opus_encoder);
        s_opus_encoder = NULL;
    }
    s_encoder_input_size = 0;
    s_encoder_output_size = 0;
    printf("audio: session stopped\n");
}

bool audio_service_is_session_active(void)
{
    return s_session_active;
}

bool audio_service_take_opus_packet(audio_service_opus_packet_t *packet, TickType_t timeout)
{
    return packet != NULL && s_opus_queue != NULL &&
           xQueueReceive(s_opus_queue, packet, timeout) == pdPASS;
}

void audio_service_discard_capture_frames(void)
{
    if (s_capture_queue != NULL) {
        xQueueReset(s_capture_queue);
    }
    if (s_opus_queue != NULL) {
        xQueueReset(s_opus_queue);
    }
}

void audio_service_reset_playback_diagnostics(void)
{
    s_playback_packet_count = 0U;
    s_playback_sample_count = 0U;
    s_playback_minimum = INT16_MAX;
    s_playback_maximum = INT16_MIN;
    s_playback_sum_squares = 0U;
}

void audio_service_print_playback_diagnostics(void)
{
    if (s_playback_sample_count == 0U) {
        printf("audio: TTS PCM decoded no samples\n");
        return;
    }
    const uint64_t mean_square = s_playback_sum_squares / s_playback_sample_count;
    printf("audio: TTS PCM packets=%lu samples=%lu min=%d max=%d rms=%lu\n",
           (unsigned long)s_playback_packet_count, (unsigned long)s_playback_sample_count,
           s_playback_minimum, s_playback_maximum,
           (unsigned long)audio_service_integer_sqrt(mean_square));
}

esp_err_t audio_service_decode_and_play_opus(const uint8_t *packet, size_t packet_length,
                                             uint32_t sample_rate_hz, uint32_t frame_duration_ms)
{
    if (packet == NULL || packet_length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err = audio_service_open_opus_decoder(sample_rate_hz, frame_duration_ms);
    if (err != ESP_OK) {
        return err;
    }

    esp_audio_dec_in_raw_t input = {
        .buffer = (uint8_t *)packet,
        .len = (uint32_t)packet_length,
        .consumed = 0U,
        .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t output = {
        .buffer = (uint8_t *)s_decoded_samples,
        .len = sizeof(s_decoded_samples),
        .decoded_size = 0U,
    };
    esp_audio_dec_info_t information = {0};
    if (esp_opus_dec_decode(s_opus_decoder, &input, &output, &information) != ESP_AUDIO_ERR_OK ||
        output.decoded_size == 0U || output.decoded_size > sizeof(s_decoded_samples)) {
        return ESP_FAIL;
    }
    const size_t sample_count = output.decoded_size / sizeof(int16_t);
    for (size_t index = 0U; index < sample_count; ++index) {
        const int16_t sample = s_decoded_samples[index];
        if (sample < s_playback_minimum) {
            s_playback_minimum = sample;
        }
        if (sample > s_playback_maximum) {
            s_playback_maximum = sample;
        }
        s_playback_sum_squares += (uint64_t)((int64_t)sample * sample);
    }
    ++s_playback_packet_count;
    s_playback_sample_count += (uint32_t)sample_count;
    return audio_service_play_pcm_mono(s_decoded_samples, sample_count, sample_rate_hz);
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

    size_t offset = 0U;
    while (offset < sample_count) {
        const size_t remaining = sample_count - offset;
        const size_t frame_count = remaining < AUDIO_SERVICE_DMA_FRAME_COUNT ?
                                       remaining : AUDIO_SERVICE_DMA_FRAME_COUNT;
        for (size_t index = 0U; index < frame_count; ++index) {
            const int16_t scaled_sample = audio_service_apply_speaker_gain(samples[offset + index]);
            s_speaker_samples[index * 2U] = scaled_sample;
            s_speaker_samples[index * 2U + 1U] = scaled_sample;
        }
        size_t bytes_written = 0U;
        err = i2s_channel_write(s_speaker_channel, s_speaker_samples,
                                frame_count * sizeof(int16_t) * 2U, &bytes_written,
                                pdMS_TO_TICKS(500));
        if (err != ESP_OK || bytes_written != frame_count * sizeof(int16_t) * 2U) {
            return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
        }
        offset += frame_count;
    }
    return ESP_OK;
}
