#ifndef AUDIO_DEVICE_H
#define AUDIO_DEVICE_H

#include <alsa/asoundlib.h>

// ALSA 设备接口：录音/播放设备打开与关闭。

int zh_audio_capture_open(snd_pcm_t **pcm_out,
                          snd_pcm_hw_params_t **params_out,
                          unsigned int *actual_rate,
                          unsigned int *actual_channels); // 打开录音设备
void zh_audio_capture_close(snd_pcm_t *pcm, snd_pcm_hw_params_t *params); // 关闭录音设备

#endif
