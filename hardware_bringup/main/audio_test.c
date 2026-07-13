#include "audio_test.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

/* INMP441：ESP32-S3 作为 I2S 主机，提供时钟并从左声道读取数据。 */
#define AUDIO_MIC_WS_GPIO GPIO_NUM_4
#define AUDIO_MIC_BCLK_GPIO GPIO_NUM_5
#define AUDIO_MIC_DATA_GPIO GPIO_NUM_6

/* NS4168：ESP32-S3 作为 I2S 主机，输出标准 Philips 格式的 PCM 数据。 */
#define AUDIO_SPEAKER_LRCLK_GPIO GPIO_NUM_16
#define AUDIO_SPEAKER_BCLK_GPIO GPIO_NUM_15
#define AUDIO_SPEAKER_DATA_GPIO GPIO_NUM_7

#define AUDIO_SAMPLE_RATE_HZ 16000U
#define AUDIO_SPEAKER_TONE_HZ 1000U
#define AUDIO_SPEAKER_TONE_DURATION_MS 1000U
#define AUDIO_SPEAKER_BUFFER_FRAMES 128U
#define AUDIO_MIC_SAMPLE_DURATION_MS 5000U
#define AUDIO_MIC_REPORT_INTERVAL_MS 500U
#define AUDIO_MIC_BUFFER_FRAMES 128U

/* 约为满量程 5%，首次测试保持较低音量。16000 / 1000 = 16 个采样点一个周期。 */
static const int16_t s_test_tone_wave[] = {
    0, 689, 1273, 1663, 1800, 1663, 1273, 689,
    0, -689, -1273, -1663, -1800, -1663, -1273, -689,
};

static esp_err_t audio_create_speaker_channel(i2s_chan_handle_t *channel)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = AUDIO_SPEAKER_BUFFER_FRAMES;
    channel_config.auto_clear = true;

    esp_err_t err = i2s_new_channel(&channel_config, channel, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
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
    err = i2s_channel_init_std_mode(*channel, &standard_config);
    if (err == ESP_OK) {
        err = i2s_channel_enable(*channel);
    }
    return err;
}

static esp_err_t audio_create_microphone_channel(i2s_chan_handle_t *channel)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = AUDIO_MIC_BUFFER_FRAMES;

    esp_err_t err = i2s_new_channel(&channel_config, NULL, channel);
    if (err != ESP_OK) {
        return err;
    }

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
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
    err = i2s_channel_init_std_mode(*channel, &standard_config);
    if (err == ESP_OK) {
        err = i2s_channel_enable(*channel);
    }
    return err;
}

static void audio_destroy_channel(i2s_chan_handle_t channel)
{
    if (channel == NULL) {
        return;
    }
    (void)i2s_channel_disable(channel);
    (void)i2s_del_channel(channel);
}

static esp_err_t audio_play_speaker_tone(void)
{
    i2s_chan_handle_t speaker_channel = NULL;
    esp_err_t err = audio_create_speaker_channel(&speaker_channel);
    if (err != ESP_OK) {
        audio_destroy_channel(speaker_channel);
        return err;
    }

    int16_t frames[AUDIO_SPEAKER_BUFFER_FRAMES * 2U] = {0};
    const size_t tone_samples = sizeof(s_test_tone_wave) / sizeof(s_test_tone_wave[0]);
    const uint32_t total_frames = AUDIO_SAMPLE_RATE_HZ * AUDIO_SPEAKER_TONE_DURATION_MS / 1000U;
    uint32_t sent_frames = 0U;
    size_t tone_index = 0U;

    printf("audio: speaker tone start, %u Hz, %u ms\n", AUDIO_SPEAKER_TONE_HZ,
           AUDIO_SPEAKER_TONE_DURATION_MS);
    while (sent_frames < total_frames) {
        const uint32_t remaining_frames = total_frames - sent_frames;
        const size_t frames_this_write = remaining_frames < AUDIO_SPEAKER_BUFFER_FRAMES ?
                                             remaining_frames : AUDIO_SPEAKER_BUFFER_FRAMES;
        for (size_t frame = 0U; frame < frames_this_write; ++frame) {
            const int16_t sample = s_test_tone_wave[tone_index];
            frames[frame * 2U] = sample;
            frames[frame * 2U + 1U] = sample;
            tone_index = (tone_index + 1U) % tone_samples;
        }

        size_t bytes_written = 0U;
        err = i2s_channel_write(speaker_channel, frames, frames_this_write * sizeof(int16_t) * 2U,
                                &bytes_written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_written != frames_this_write * sizeof(int16_t) * 2U) {
            err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
            break;
        }
        sent_frames += (uint32_t)frames_this_write;
    }
    audio_destroy_channel(speaker_channel);
    printf("audio: speaker tone %s\n", err == ESP_OK ? "complete" : "failed");
    return err;
}

static uint32_t audio_integer_sqrt(uint64_t value)
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

static uint32_t audio_calculate_ac_rms(int64_t sum, uint64_t sum_squares, uint32_t sample_count)
{
    if (sample_count == 0U) {
        return 0U;
    }

    const int64_t average = sum / (int64_t)sample_count;
    const uint64_t mean_square = sum_squares / sample_count;
    const uint64_t dc_square = (uint64_t)(average * average);
    return audio_integer_sqrt(mean_square > dc_square ? mean_square - dc_square : 0U);
}

static esp_err_t audio_sample_microphone(void)
{
    i2s_chan_handle_t microphone_channel = NULL;
    esp_err_t err = audio_create_microphone_channel(&microphone_channel);
    if (err != ESP_OK) {
        audio_destroy_channel(microphone_channel);
        return err;
    }

    int32_t frames[AUDIO_MIC_BUFFER_FRAMES * 2U] = {0};
    const uint32_t target_samples = AUDIO_SAMPLE_RATE_HZ * AUDIO_MIC_SAMPLE_DURATION_MS / 1000U;
    uint32_t sample_count = 0U;
    int32_t minimum = INT32_MAX;
    int32_t maximum = INT32_MIN;
    int64_t sum = 0;
    uint64_t sum_squares = 0U;
    const uint32_t report_sample_count = AUDIO_SAMPLE_RATE_HZ * AUDIO_MIC_REPORT_INTERVAL_MS / 1000U;
    uint32_t window_count = 0U;
    int32_t window_minimum = INT32_MAX;
    int32_t window_maximum = INT32_MIN;
    int64_t window_sum = 0;
    uint64_t window_sum_squares = 0U;

    printf("audio: microphone sampling start, %u ms; stay quiet, then speak or clap\n",
           AUDIO_MIC_SAMPLE_DURATION_MS);
    while (sample_count < target_samples) {
        size_t bytes_read = 0U;
        err = i2s_channel_read(microphone_channel, frames, sizeof(frames), &bytes_read,
                               pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_read == 0U) {
            err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
            break;
        }

        const size_t frame_count = bytes_read / (sizeof(int32_t) * 2U);
        for (size_t frame = 0U; frame < frame_count && sample_count < target_samples; ++frame) {
            /* INMP441 的 24 位有效数据左对齐在 32 位左声道槽位中，缩小为 16 位后统计。 */
            const int32_t sample = frames[frame * 2U] >> 16;
            if (sample < minimum) {
                minimum = sample;
            }
            if (sample > maximum) {
                maximum = sample;
            }
            if (sample < window_minimum) {
                window_minimum = sample;
            }
            if (sample > window_maximum) {
                window_maximum = sample;
            }
            sum += sample;
            sum_squares += (uint64_t)((int64_t)sample * sample);
            ++sample_count;
            window_sum += sample;
            window_sum_squares += (uint64_t)((int64_t)sample * sample);
            ++window_count;

            if (window_count >= report_sample_count || sample_count == target_samples) {
                const int32_t window_average = (int32_t)(window_sum / (int64_t)window_count);
                const uint32_t window_rms = audio_calculate_ac_rms(window_sum, window_sum_squares,
                                                                    window_count);
                printf("audio: microphone window=%" PRIu32 "ms min=%" PRId32 " max=%" PRId32
                       " average=%" PRId32 " ac_rms=%" PRIu32 "\n",
                       sample_count * 1000U / AUDIO_SAMPLE_RATE_HZ, window_minimum, window_maximum,
                       window_average, window_rms);
                window_count = 0U;
                window_minimum = INT32_MAX;
                window_maximum = INT32_MIN;
                window_sum = 0;
                window_sum_squares = 0U;
            }
        }
    }

    audio_destroy_channel(microphone_channel);
    if (err != ESP_OK || sample_count == 0U) {
        printf("audio: microphone sampling failed, err=%s\n", esp_err_to_name(err));
        return err == ESP_OK ? ESP_FAIL : err;
    }

    const int32_t average = (int32_t)(sum / (int64_t)sample_count);
    const uint32_t rms = audio_calculate_ac_rms(sum, sum_squares, sample_count);
    printf("audio: microphone samples=%" PRIu32 " min=%" PRId32 " max=%" PRId32
           " average=%" PRId32 " ac_rms=%" PRIu32 "\n",
           sample_count, minimum, maximum, average, rms);
    return ESP_OK;
}

esp_err_t audio_test_run_once(void)
{
    const esp_err_t speaker_err = audio_play_speaker_tone();
    if (speaker_err == ESP_OK) {
        /* 避免功放关闭瞬态和测试音尾音影响紧随其后的麦克风基线。 */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    const esp_err_t microphone_err = audio_sample_microphone();
    if (speaker_err != ESP_OK) {
        printf("audio: speaker test failed: %s\n", esp_err_to_name(speaker_err));
    }
    if (microphone_err != ESP_OK) {
        printf("audio: microphone test failed: %s\n", esp_err_to_name(microphone_err));
    }
    return speaker_err != ESP_OK ? speaker_err : microphone_err;
}
