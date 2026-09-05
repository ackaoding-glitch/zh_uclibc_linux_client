#ifndef AUDIO_PLAYBACK_ROCKIT_H
#define AUDIO_PLAYBACK_ROCKIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zh_ao_playback zh_ao_playback_t;

int zh_ao_playback_open(zh_ao_playback_t **out, unsigned int in_rate, unsigned int in_channels);
int zh_ao_playback_write(zh_ao_playback_t *pb, const int16_t *pcm, size_t frames);
void zh_ao_playback_drain(zh_ao_playback_t *pb);
void zh_ao_playback_flush(zh_ao_playback_t *pb);
void zh_ao_playback_close(zh_ao_playback_t *pb);

#ifdef __cplusplus
}
#endif

#endif
