#include "audio_playback_ref.h"

#include <string.h>

// AEC 参考通道环形缓冲实现仅在 AIVQE 后端启用时编译；
// rv1106 平台使用空实现，AEC 参考走 Rockit 内建 VQE 链路。
int zh_audio_playback_ref_init(unsigned int sample_rate,
                               unsigned int delay_ms,
                               unsigned int capacity_ms,
                               float gain_db) {
    (void)sample_rate;
    (void)delay_ms;
    (void)capacity_ms;
    (void)gain_db;
    return 0;
}

void zh_audio_playback_ref_push(const int16_t *pcm,
                                size_t frames,
                                unsigned int channels,
                                unsigned int sample_rate) {
    (void)pcm;
    (void)frames;
    (void)channels;
    (void)sample_rate;
}

void zh_audio_playback_ref_read_delayed(int16_t *out, size_t frames) {
    if (out && frames > 0) {
        memset(out, 0, frames * sizeof(int16_t));
    }
}

void zh_audio_playback_ref_reset(void) {}
void zh_audio_playback_ref_shutdown(void) {}
