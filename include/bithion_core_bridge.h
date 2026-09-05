#ifndef ZH_BITHION_CORE_BRIDGE_H
#define ZH_BITHION_CORE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct zh_ws_session zh_ws_session_t;
typedef struct zh_core_vad zh_core_vad_t;

typedef struct {
    uint32_t chat_count;
    uint32_t packet_seq;
} zh_core_tts_opus_meta_t;

typedef struct {
    int l1_active;
    int speech_active;
    int speech_started;
    int speech_ended;
    float l2_prob;
} zh_core_vad_result_t;

int zh_core_tts_transport_start(const zh_config_t *cfg, zh_ws_session_t *ws);
void zh_core_tts_transport_stop(void);
int zh_core_tts_transport_wait(int timeout_ms);
int zh_core_tts_read_opus(void *buf, size_t buf_len, size_t *out_len, int timeout_ms);
int zh_core_tts_read_opus_with_meta(void *buf,
                                    size_t buf_len,
                                    size_t *out_len,
                                    int timeout_ms,
                                    zh_core_tts_opus_meta_t *out_meta);
void zh_core_tts_transport_interrupt(void);
int zh_core_tts_transport_has_pending_data(void);
int zh_core_tts_transport_is_running(void);
void zh_core_ws_on_tts_round_done(void);

int zh_core_vad_preload(void);
zh_core_vad_t *zh_core_vad_create(void);
void zh_core_vad_destroy(zh_core_vad_t *vad);
void zh_core_vad_reset(zh_core_vad_t *vad);
int zh_core_vad_process(zh_core_vad_t *vad,
                        const int16_t *pcm,
                        size_t frame_samples,
                        size_t channels,
                        zh_core_vad_result_t *out_result);

int zh_core_uplink_start(void);
void zh_core_uplink_stop(void);
int zh_core_uplink_begin_segment(void);
int zh_core_uplink_send_opus(const void *data, size_t len);
void zh_core_uplink_reset(void);
void zh_core_uplink_flush(void);
int zh_core_uplink_flush_wait(int timeout_ms);

#endif
