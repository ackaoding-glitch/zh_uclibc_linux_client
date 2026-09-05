#ifndef ZH_AUDIO_PLAYBACK_REF_H
#define ZH_AUDIO_PLAYBACK_REF_H

#include <stddef.h>
#include <stdint.h>

int zh_audio_playback_ref_init(unsigned int sample_rate,
                               unsigned int delay_ms,
                               unsigned int capacity_ms,
                               float gain_db);
void zh_audio_playback_ref_push(const int16_t *pcm,
                                size_t frames,
                                unsigned int channels,
                                unsigned int sample_rate);
void zh_audio_playback_ref_read_delayed(int16_t *out, size_t frames);
void zh_audio_playback_ref_reset(void);
void zh_audio_playback_ref_shutdown(void);

#endif
