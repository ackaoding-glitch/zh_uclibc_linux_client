#include "config.h"

#if !ZH_AUDIO_BACKEND_ROCKIT

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "audio_playback_ref.h"
#include "audio_playback_rockit.h"
#include "log.h"

struct zh_ao_playback {
    snd_pcm_t *pcm;
    unsigned int rate;
    unsigned int in_channels;
    unsigned int out_channels;
    int16_t *scratch;
    size_t scratch_samples;
};

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

static unsigned int zh_audio_parse_channels_env(const char *name, unsigned int fallback) {
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long value = 0;

    if (!s || s[0] == '\0') {
        return fallback;
    }
    errno = 0;
    value = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value == 0 || value > 8) {
        return fallback;
    }
    return (unsigned int)value;
}

static int zh_ao_playback_prepare_output(zh_ao_playback_t *pb,
                                         const int16_t *pcm,
                                         size_t frames,
                                         const int16_t **out_pcm) {
    if (pb->in_channels == pb->out_channels) {
        *out_pcm = pcm;
        return 0;
    }

    size_t needed = frames * (size_t)pb->out_channels;
    if (needed > pb->scratch_samples) {
        int16_t *tmp = (int16_t *)realloc(pb->scratch, needed * sizeof(int16_t));
        if (!tmp) {
            return -1;
        }
        pb->scratch = tmp;
        pb->scratch_samples = needed;
    }

    for (size_t i = 0; i < frames; ++i) {
        const int16_t *src = pcm + i * (size_t)pb->in_channels;
        int16_t mono = src[0];
        if (pb->in_channels > 1) {
            int32_t sum = 0;
            for (unsigned int ch = 0; ch < pb->in_channels; ++ch) {
                sum += src[ch];
            }
            mono = (int16_t)(sum / (int32_t)pb->in_channels);
        }
        for (unsigned int ch = 0; ch < pb->out_channels; ++ch) {
            pb->scratch[i * (size_t)pb->out_channels + ch] =
                (pb->in_channels == 1) ? mono : src[ch < pb->in_channels ? ch : pb->in_channels - 1];
        }
    }

    *out_pcm = pb->scratch;
    return 0;
}

int zh_ao_playback_open(zh_ao_playback_t **out, unsigned int in_rate, unsigned int in_channels) {
    if (!out || in_rate == 0 || in_channels == 0) {
        errno = EINVAL;
        return -1;
    }

    snd_pcm_t *pcm = NULL;
    const char *device = zh_audio_env_or_default("ZH_TTS_PLAY_DEVICE", ZH_TTS_PLAY_DEVICE);
    unsigned int out_channels = zh_audio_parse_channels_env("ZH_TTS_PLAY_CHANNELS", in_channels);
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_open failed: device=%s error=%s", device, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *params = NULL;
    if (snd_pcm_hw_params_malloc(&params) < 0) {
        snd_pcm_close(pcm);
        return -1;
    }
    if (zh_alsa_check(snd_pcm_hw_params_any(pcm, params), "snd_pcm_hw_params_any", device) != 0 ||
        zh_alsa_check(snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED),
                      "snd_pcm_hw_params_set_access", device) != 0 ||
        zh_alsa_check(snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE),
                      "snd_pcm_hw_params_set_format", device) != 0 ||
        zh_alsa_check(snd_pcm_hw_params_set_channels(pcm, params, out_channels),
                      "snd_pcm_hw_params_set_channels", device) != 0) {
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    unsigned int rate = in_rate;
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
    snd_pcm_uframes_t actual_period = 0;
    snd_pcm_uframes_t actual_buffer = 0;
    int period_dir = 0;
    snd_pcm_hw_params_get_period_size(params, &actual_period, &period_dir);
    snd_pcm_hw_params_get_buffer_size(params, &actual_buffer);
    snd_pcm_hw_params_free(params);
    snd_pcm_prepare(pcm);

    zh_ao_playback_t *pb = (zh_ao_playback_t *)calloc(1, sizeof(*pb));
    if (!pb) {
        snd_pcm_close(pcm);
        return -1;
    }
    pb->pcm = pcm;
    pb->rate = rate;
    pb->in_channels = in_channels;
    pb->out_channels = out_channels;
    if (rate != in_rate) {
        LOGI(__func__, "playback rate adjusted: %u -> %u", in_rate, rate);
    }
    LOGI(__func__,
         "alsa playback opened: device=%s rate=%u in_channels=%u out_channels=%u period=%lu buffer=%lu",
         device, rate, in_channels, out_channels, (unsigned long)actual_period, (unsigned long)actual_buffer);

    *out = pb;
    return 0;
}

int zh_ao_playback_write(zh_ao_playback_t *pb, const int16_t *pcm, size_t frames) {
    if (!pb || !pb->pcm || !pcm || frames == 0) {
        errno = EINVAL;
        return -1;
    }

    zh_audio_playback_ref_push(pcm, frames, pb->in_channels, pb->rate);

    const int16_t *out_pcm = NULL;
    if (zh_ao_playback_prepare_output(pb, pcm, frames, &out_pcm) != 0) {
        return -1;
    }

    size_t written = 0;
    while (written < frames) {
        snd_pcm_sframes_t rc = snd_pcm_writei(pb->pcm,
                                              out_pcm + written * (size_t)pb->out_channels,
                                              frames - written);
        if (rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if (rc == 0) {
            // 设备暂时不可写，短等待后重试，避免卡死在忙等。
            snd_pcm_wait(pb->pcm, 20);
            continue;
        }
        int recover_rc = snd_pcm_recover(pb->pcm, (int)rc, 1);
        if (recover_rc == 0) {
            continue;
        }
        LOGE(__func__, "snd_pcm_writei failed: rc=%d (%s), recover=%d (%s)",
             (int)rc, snd_strerror((int)rc), recover_rc, snd_strerror(recover_rc));
        return -1;
    }
    return 0;
}

void zh_ao_playback_drain(zh_ao_playback_t *pb) {
    if (!pb || !pb->pcm) return;
    snd_pcm_drain(pb->pcm);
    snd_pcm_prepare(pb->pcm);
}

void zh_ao_playback_flush(zh_ao_playback_t *pb) {
    if (!pb || !pb->pcm) return;
    zh_audio_playback_ref_reset();
    snd_pcm_drop(pb->pcm);
    snd_pcm_prepare(pb->pcm);
}

void zh_ao_playback_close(zh_ao_playback_t *pb) {
    if (!pb) return;
    if (pb->pcm) {
        snd_pcm_close(pb->pcm);
    }
    free(pb->scratch);
    free(pb);
}

#endif
