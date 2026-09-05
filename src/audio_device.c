#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "config.h"
#include "audio_device.h"
#include "log.h"

// ALSA设备封装：录音/播放设备初始化与关闭。

static const char *zh_audio_env_or_default(const char *env_name, const char *default_value) {
    const char *value = getenv(env_name);
    if (value && value[0] != '\0') {
        return value;
    }
    return default_value;
}

static unsigned int zh_alsa_buffer_periods(void) {
    return (ZH_ALSA_BUFFER_PERIODS < 2) ? 2u : (unsigned int)ZH_ALSA_BUFFER_PERIODS;
}

static int zh_alsa_check(int rc, const char *op, const char *device) {
    if (rc < 0) {
        LOGE(__func__, "%s failed: device=%s error=%s", op, device, snd_strerror(rc));
        return -1;
    }
    return 0;
}

// 打开录音设备并配置采样参数。
int zh_audio_capture_open(snd_pcm_t **pcm_out,
                          snd_pcm_hw_params_t **params_out,
                          unsigned int *actual_rate,
                          unsigned int *actual_channels) {
    if (!pcm_out || !params_out) {
        errno = EINVAL;
        return -1;
    }

    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;
    const char *device = zh_audio_env_or_default("ZH_AUDIO_DEVICE", ZH_AUDIO_DEVICE);

    // 1) 打开录音设备。
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_open failed: device=%s error=%s", device, snd_strerror(err));
        return -1;
    }

    // 2) 配置采样格式/通道/采样率/周期大小。
    if (snd_pcm_hw_params_malloc(&params) < 0) {
        snd_pcm_close(pcm);
        return -1;
    }
    if (zh_alsa_check(snd_pcm_hw_params_any(pcm, params), "snd_pcm_hw_params_any", device) != 0 ||
        zh_alsa_check(snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED),
                      "snd_pcm_hw_params_set_access", device) != 0 ||
        zh_alsa_check(snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE),
                      "snd_pcm_hw_params_set_format", device) != 0 ||
        zh_alsa_check(snd_pcm_hw_params_set_channels(pcm, params, ZH_AUDIO_CHANNELS),
                      "snd_pcm_hw_params_set_channels", device) != 0) {
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    unsigned int rate = ZH_AUDIO_SAMPLE_RATE;
    if (zh_alsa_check(snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0),
                      "snd_pcm_hw_params_set_rate_near", device) != 0) {
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    snd_pcm_uframes_t period = ZH_AUDIO_FRAME_SAMPLES;
    if (zh_alsa_check(snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0),
                      "snd_pcm_hw_params_set_period_size_near", device) != 0) {
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    snd_pcm_uframes_t buffer = period * zh_alsa_buffer_periods();
    if (zh_alsa_check(snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer),
                      "snd_pcm_hw_params_set_buffer_size_near", device) != 0) {
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_hw_params failed: device=%s error=%s", device, snd_strerror(err));
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    // 3) 回填实际配置
    unsigned int ch = 0;
    snd_pcm_uframes_t actual_period = 0;
    snd_pcm_uframes_t actual_buffer = 0;
    int period_dir = 0;
    if (actual_rate) {
        int dir = 0;
        snd_pcm_hw_params_get_rate(params, actual_rate, &dir);
    }
    snd_pcm_hw_params_get_channels(params, &ch);
    snd_pcm_hw_params_get_period_size(params, &actual_period, &period_dir);
    snd_pcm_hw_params_get_buffer_size(params, &actual_buffer);
    if (actual_channels) {
        *actual_channels = ch;
    }
    // 4) 进入可读状态。
    snd_pcm_prepare(pcm);

    LOGI(__func__,
         "alsa capture opened: device=%s rate=%u channels=%u mic_channel=%d period=%lu buffer=%lu",
         device, actual_rate ? *actual_rate : rate, ch, ZH_AUDIO_MIC_CHANNEL_INDEX,
         (unsigned long)actual_period, (unsigned long)actual_buffer);

    *pcm_out = pcm;
    *params_out = params;
    return 0;
}

// 关闭录音设备并释放参数结构。
void zh_audio_capture_close(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
    if (params) {
        snd_pcm_hw_params_free(params);
    }
    if (pcm) {
        snd_pcm_close(pcm);
    }
}
