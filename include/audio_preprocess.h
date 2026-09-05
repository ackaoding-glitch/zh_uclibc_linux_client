#ifndef ZH_AUDIO_PREPROCESS_H
#define ZH_AUDIO_PREPROCESS_H

#include <stddef.h>
#include <stdint.h>

typedef struct zh_audio_preprocess zh_audio_preprocess_t;

typedef struct {
    int sample_rate;
    int bits_per_sample;
    int frame_samples;
    int channels;
    int mic_channel_index;
    int ref_channel_index;
    int ref_channels;
    int aec_frame_samples;
} zh_audio_preprocess_config_t;

int zh_audio_preprocess_create(zh_audio_preprocess_t **out,
                               const zh_audio_preprocess_config_t *cfg);
int zh_audio_preprocess_process(zh_audio_preprocess_t *ctx,
                                const int16_t *in,
                                size_t frames,
                                int16_t *out,
                                size_t out_samples);
void zh_audio_preprocess_destroy(zh_audio_preprocess_t *ctx);

#endif
