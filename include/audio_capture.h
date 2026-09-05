#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

// 录音抽象层：统一 ALSA 与 Rockit/RK_MPI 录音接口。

typedef struct zh_audio_capture zh_audio_capture_t;

// 初始化录音设备，返回实际采样率与通道数（可为NULL）。
int zh_audio_capture_init(zh_audio_capture_t **cap,
                          unsigned int *actual_rate,
                          unsigned int *actual_channels);

// 启动录音（部分实现可能为空操作）。
int zh_audio_capture_start(zh_audio_capture_t *cap);

// 读取一帧 PCM16，返回实际读取的样本数（总样本数=帧样本数*通道数），失败返回负值。
int zh_audio_capture_read(zh_audio_capture_t *cap, int16_t *buffer, size_t samples);

// 停止录音（部分实现可能为空操作）。
int zh_audio_capture_stop(zh_audio_capture_t *cap);

// 释放录音资源。
void zh_audio_capture_deinit(zh_audio_capture_t *cap);

// 自测：录制 seconds 秒并写入 PCM16 文件。
int zh_audio_capture_dump(const char *path, int seconds);

#endif
