#include "config.h"

#include <errno.h>
#include <stdlib.h>

#include "audio_preprocess.h"

struct zh_audio_preprocess {
    zh_audio_preprocess_config_t cfg;
};

static int zh_audio_preprocess_validate(const zh_audio_preprocess_config_t *cfg) {
    if (!cfg || cfg->sample_rate <= 0 || cfg->bits_per_sample != 16 ||
        cfg->frame_samples <= 0 || cfg->channels <= 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int zh_audio_preprocess_create(zh_audio_preprocess_t **out,
                               const zh_audio_preprocess_config_t *cfg) {
    if (!out || zh_audio_preprocess_validate(cfg) != 0) {
        errno = EINVAL;
        return -1;
    }

    zh_audio_preprocess_t *ctx = (zh_audio_preprocess_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }
    ctx->cfg = *cfg;
    *out = ctx;
    return 0;
}

int zh_audio_preprocess_process(zh_audio_preprocess_t *ctx,
                                const int16_t *in,
                                size_t frames,
                                int16_t *out,
                                size_t out_samples) {
    if (!ctx || !in || !out || frames == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t channels = (size_t)ctx->cfg.channels;
    size_t required = frames * channels;
    size_t mic_channel = (size_t)ctx->cfg.mic_channel_index;
    if (channels == 0 || out_samples < required || mic_channel >= channels) {
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < frames; ++i) {
        int16_t sample = in[i * channels + mic_channel];
        for (size_t ch = 0; ch < channels; ++ch) {
            out[i * channels + ch] = sample;
        }
    }
    return (int)required;
}

void zh_audio_preprocess_destroy(zh_audio_preprocess_t *ctx) {
    free(ctx);
}
