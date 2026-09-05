#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

#if !ZH_AUDIO_BACKEND_ROCKIT

#include <alsa/asoundlib.h>

#include "audio_capture.h"
#include "audio_device.h"
#include "log.h"

struct zh_audio_capture {
    snd_pcm_t *pcm;
    snd_pcm_hw_params_t *params;
};

int zh_audio_capture_init(zh_audio_capture_t **cap,
                          unsigned int *actual_rate,
                          unsigned int *actual_channels) {
    if (!cap) {
        errno = EINVAL;
        return -1;
    }

    zh_audio_capture_t *ctx = (zh_audio_capture_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }

    if (zh_audio_capture_open(&ctx->pcm, &ctx->params, actual_rate, actual_channels) != 0) {
        free(ctx);
        return -1;
    }

    *cap = ctx;
    return 0;
}

int zh_audio_capture_start(zh_audio_capture_t *cap) {
    if (!cap || !cap->pcm) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zh_audio_capture_read(zh_audio_capture_t *cap, int16_t *buffer, size_t samples) {
    if (!cap || !cap->pcm || !buffer || samples == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t frames = samples / ZH_AUDIO_CHANNELS;
    if (frames == 0) {
        errno = EINVAL;
        return -1;
    }

    snd_pcm_sframes_t got = snd_pcm_readi(cap->pcm, buffer, frames);
    if (got < 0) {
        if (got == -EPIPE) {
            snd_pcm_prepare(cap->pcm);
            return -EPIPE;
        }
        LOGE(__func__, "snd_pcm_readi failed: %s", snd_strerror(got));
        return -1;
    }

    return (int)(got * ZH_AUDIO_CHANNELS);
}

int zh_audio_capture_stop(zh_audio_capture_t *cap) {
    if (!cap || !cap->pcm) {
        return 0;
    }
    snd_pcm_drop(cap->pcm);
    return 0;
}

void zh_audio_capture_deinit(zh_audio_capture_t *cap) {
    if (!cap) {
        return;
    }
    zh_audio_capture_close(cap->pcm, cap->params);
    free(cap);
}

int zh_audio_capture_dump(const char *path, int seconds) {
    if (!path || seconds <= 0) {
        errno = EINVAL;
        return -1;
    }

    zh_audio_capture_t *cap = NULL;
    unsigned int rate = 0;
    unsigned int ch = 0;
    if (zh_audio_capture_init(&cap, &rate, &ch) != 0) {
        return -1;
    }
    if (zh_audio_capture_start(cap) != 0) {
        zh_audio_capture_deinit(cap);
        return -1;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        zh_audio_capture_deinit(cap);
        return -1;
    }

    size_t samples = ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS;
    int16_t *buf = (int16_t *)calloc(samples, sizeof(int16_t));
    if (!buf) {
        fclose(fp);
        zh_audio_capture_deinit(cap);
        return -1;
    }

    int frames_total = (seconds * 1000) / ZH_AUDIO_FRAME_MS;
    for (int i = 0; i < frames_total; ++i) {
        int got = zh_audio_capture_read(cap, buf, samples);
        if (got < 0) {
            if (got == -EPIPE) {
                usleep(1000);
                i--;
                continue;
            }
            break;
        }
        if (got == (int)samples) {
            fwrite(buf, sizeof(int16_t), samples, fp);
        }
    }

    free(buf);
    fclose(fp);
    zh_audio_capture_stop(cap);
    zh_audio_capture_deinit(cap);
    return 0;
}

#endif
