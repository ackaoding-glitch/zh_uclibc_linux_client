#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <opus/opus.h>

#include "audio_capture.h"
#include "audio_playback_ref.h"
#include "audio_preprocess.h"
#include "audio_uplink.h"
#include "bithion_core_bridge.h"
#include "config.h"
#include "face_recognition.h"
#include "log.h"
#include "music_player.h"
#include "udp_tts.h"
#include "utils.h"
#include "version.h"
#include "ws.h"

static volatile int g_uplink_running = 0;
static pthread_t g_uplink_thread;
static pthread_mutex_t g_uplink_ws_mutex = PTHREAD_MUTEX_INITIALIZER;
static zh_ws_session_t *g_uplink_ws = NULL;
// 复用协议消息：维持历史兼容（music_playing + required_keyword）。
#define ZH_MSG_TYPE_ASR_UPLOAD_META "ASR_UPLOAD_META"
// 新增门控消息：仅承载当前句门控参数，不影响旧协议语义。
#define ZH_MSG_TYPE_ASR_GATE_META "ASR_GATE_META"
#ifndef ZH_UPLINK_END_FLUSH_WAIT_MS
#define ZH_UPLINK_END_FLUSH_WAIT_MS 80
#endif
#ifndef ZH_AUDIO_CAPTURE_OPEN_RETRY_MS
#define ZH_AUDIO_CAPTURE_OPEN_RETRY_MS 1000
#endif

static int16_t zh_abs_i16(int16_t v) {
    if (v >= 0) {
        return v;
    }
    if (v == INT16_MIN) {
        return INT16_MAX;
    }
    return (int16_t)(-v);
}

static int zh_audio_vad_gate_filter(const int16_t *in,
                                    int16_t *out,
                                    size_t samples,
                                    int *gate_open,
                                    int *below_close_frames,
                                    int16_t *out_peak) {
#if ZH_VAD_GATE_ENABLE
    int16_t peak = 0;
    int open_threshold = ZH_VAD_GATE_OPEN_PEAK;
    int close_threshold = ZH_VAD_GATE_CLOSE_PEAK;
    int hold_frames = ZH_VAD_GATE_HOLD_FRAMES;

    if (!in || !out || !gate_open || !below_close_frames || !out_peak || samples == 0) {
        return 0;
    }
    if (close_threshold > open_threshold) {
        close_threshold = open_threshold;
    }
    if (hold_frames < 1) {
        hold_frames = 1;
    }

    for (size_t i = 0; i < samples; ++i) {
        int16_t abs_sample = zh_abs_i16(in[i]);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
    }
    *out_peak = peak;

    if (*gate_open) {
        if (peak < close_threshold) {
            *below_close_frames += 1;
            if (*below_close_frames >= hold_frames) {
                *gate_open = 0;
            }
        } else {
            *below_close_frames = 0;
        }
    } else if (peak >= open_threshold) {
        *gate_open = 1;
        *below_close_frames = 0;
    }

    if (*gate_open) {
        memcpy(out, in, sizeof(int16_t) * samples);
    } else {
        memset(out, 0, sizeof(int16_t) * samples);
    }
    return *gate_open;
#else
    int16_t peak = 0;

    if (!in || !out || !out_peak || samples == 0) {
        return 0;
    }
    for (size_t i = 0; i < samples; ++i) {
        int16_t abs_sample = zh_abs_i16(in[i]);
        if (abs_sample > peak) {
            peak = abs_sample;
        }
    }
    *out_peak = peak;
    memcpy(out, in, sizeof(int16_t) * samples);
    return 1;
#endif
}

static void zh_audio_uplink_reset_vad_round(zh_core_vad_t *vad,
                                            int *speech_active,
                                            size_t *preroll_pos,
                                            int *preroll_full,
                                            int *vad_gate_open,
                                            int *vad_gate_below_close_frames) {
    if (speech_active) {
        *speech_active = 0;
    }
    if (preroll_pos) {
        *preroll_pos = 0;
    }
    if (preroll_full) {
        *preroll_full = 0;
    }
    if (vad_gate_open) {
        *vad_gate_open = 0;
    }
    if (vad_gate_below_close_frames) {
        *vad_gate_below_close_frames = 0;
    }
    zh_face_recognition_set_active(0);
    if (vad) {
        zh_core_vad_reset(vad);
    }
    zh_core_uplink_reset();
}

#if ZH_VAD_RECORD_DUMP_ENABLE
static void zh_pre_opus_pcm_dump_write(FILE *fp, const int16_t *pcm, size_t samples) {
    if (!fp || !pcm || samples == 0) {
        return;
    }
    fwrite(pcm, sizeof(int16_t), samples, fp);
    fflush(fp);
}
#endif

typedef struct {
    int enabled;
    unsigned int seq;
    uint32_t frames;
    char label[16];
    char dir[256];
    char tmp_path[544];
    char final_path[512];
    FILE *fp;
} zh_l2vad_dump_t;

static int zh_l2vad_dump_mkdir_p(const char *path) {
    char tmp[256];
    size_t len = 0;

    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void zh_l2vad_dump_write_u16le(FILE *fp, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xffU), (uint8_t)((v >> 8) & 0xffU)};
    fwrite(b, 1, sizeof(b), fp);
}

static void zh_l2vad_dump_write_u32le(FILE *fp, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xffU),
        (uint8_t)((v >> 8) & 0xffU),
        (uint8_t)((v >> 16) & 0xffU),
        (uint8_t)((v >> 24) & 0xffU),
    };
    fwrite(b, 1, sizeof(b), fp);
}

static void zh_l2vad_dump_write_wav_header(FILE *fp, uint32_t frames) {
    uint32_t data_bytes = frames * sizeof(int16_t);

    fwrite("RIFF", 1, 4, fp);
    zh_l2vad_dump_write_u32le(fp, 36U + data_bytes);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    zh_l2vad_dump_write_u32le(fp, 16);
    zh_l2vad_dump_write_u16le(fp, 1);
    zh_l2vad_dump_write_u16le(fp, 1);
    zh_l2vad_dump_write_u32le(fp, ZH_AUDIO_SAMPLE_RATE);
    zh_l2vad_dump_write_u32le(fp, ZH_AUDIO_SAMPLE_RATE * sizeof(int16_t));
    zh_l2vad_dump_write_u16le(fp, sizeof(int16_t));
    zh_l2vad_dump_write_u16le(fp, 16);
    fwrite("data", 1, 4, fp);
    zh_l2vad_dump_write_u32le(fp, data_bytes);
}

static void zh_l2vad_dump_init(zh_l2vad_dump_t *dump) {
    const char *dir = getenv("ZH_L2VAD_DUMP_DIR");

    memset(dump, 0, sizeof(*dump));
    if (!dir || dir[0] == '\0') {
        return;
    }
    snprintf(dump->dir, sizeof(dump->dir), "%s", dir);
    snprintf(dump->label, sizeof(dump->label), "%s", "l2vad");
    dump->enabled = 1;
    if (zh_l2vad_dump_mkdir_p(dump->dir) != 0) {
        LOGE(__func__, "l2vad dump dir create failed: %s errno=%d", dump->dir, errno);
        dump->enabled = 0;
        return;
    }
    LOGI(__func__, "%s dump enabled: dir=%s", dump->label, dump->dir);
}

static void zh_tts_aec_dump_init(zh_l2vad_dump_t *dump) {
    const char *dir = getenv("ZH_TTS_AEC_DUMP_DIR");

    memset(dump, 0, sizeof(*dump));
    if (!dir || dir[0] == '\0') {
        return;
    }
    snprintf(dump->dir, sizeof(dump->dir), "%s", dir);
    snprintf(dump->label, sizeof(dump->label), "%s", "tts_aec");
    dump->enabled = 1;
    if (zh_l2vad_dump_mkdir_p(dump->dir) != 0) {
        LOGE(__func__, "%s dump dir create failed: %s errno=%d",
             dump->label, dump->dir, errno);
        dump->enabled = 0;
        return;
    }
    LOGI(__func__, "%s dump enabled: dir=%s", dump->label, dump->dir);
}

static void zh_l2vad_dump_close(zh_l2vad_dump_t *dump) {
    if (!dump || !dump->fp) {
        return;
    }
    fflush(dump->fp);
    if (fseek(dump->fp, 0, SEEK_SET) == 0) {
        zh_l2vad_dump_write_wav_header(dump->fp, dump->frames);
        fflush(dump->fp);
    }
    fclose(dump->fp);
    dump->fp = NULL;
    if (dump->frames > 0) {
        if (rename(dump->tmp_path, dump->final_path) == 0) {
            LOGI(__func__,
                 "%s dump saved: %s frames=%u",
                 dump->label[0] ? dump->label : "audio",
                 dump->final_path,
                 dump->frames);
        } else {
            LOGE(__func__,
                 "%s dump rename failed: %s -> %s errno=%d",
                 dump->label[0] ? dump->label : "audio",
                 dump->tmp_path,
                 dump->final_path,
                 errno);
        }
    } else {
        unlink(dump->tmp_path);
    }
    dump->frames = 0;
    dump->tmp_path[0] = '\0';
    dump->final_path[0] = '\0';
}

static void zh_l2vad_dump_open(zh_l2vad_dump_t *dump, float l2_prob) {
    time_t now = 0;
    struct tm tm_now;
    char ts[32];

    if (!dump || !dump->enabled) {
        return;
    }
    zh_l2vad_dump_close(dump);

    now = time(NULL);
    memset(&tm_now, 0, sizeof(tm_now));
    if (now >= 0) {
        localtime_r(&now, &tm_now);
    }
    if (strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now) == 0) {
        snprintf(ts, sizeof(ts), "unknown_time");
    }
    dump->seq++;
    snprintf(dump->final_path,
             sizeof(dump->final_path),
             "%s/l2vad_%s_%04u_p%03d.wav",
             dump->dir,
             ts,
             dump->seq,
             (int)(l2_prob * 1000.0f + 0.5f));
    snprintf(dump->tmp_path, sizeof(dump->tmp_path), "%s.tmp", dump->final_path);
    dump->fp = fopen(dump->tmp_path, "wb");
    dump->frames = 0;
    if (!dump->fp) {
        LOGE(__func__, "l2vad dump open failed: %s errno=%d", dump->tmp_path, errno);
        return;
    }
    zh_l2vad_dump_write_wav_header(dump->fp, 0);
}

static void zh_tts_aec_dump_open(zh_l2vad_dump_t *dump) {
    time_t now = 0;
    struct tm tm_now;
    char ts[32];

    if (!dump || !dump->enabled || dump->fp) {
        return;
    }

    now = time(NULL);
    memset(&tm_now, 0, sizeof(tm_now));
    if (now >= 0) {
        localtime_r(&now, &tm_now);
    }
    if (strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now) == 0) {
        snprintf(ts, sizeof(ts), "unknown_time");
    }
    dump->seq++;
    snprintf(dump->final_path,
             sizeof(dump->final_path),
             "%s/tts_%s_%04u_aec.wav",
             dump->dir,
             ts,
             dump->seq);
    snprintf(dump->tmp_path, sizeof(dump->tmp_path), "%s.tmp", dump->final_path);
    dump->fp = fopen(dump->tmp_path, "wb");
    dump->frames = 0;
    if (!dump->fp) {
        LOGE(__func__, "tts_aec dump open failed: %s errno=%d", dump->tmp_path, errno);
        return;
    }
    zh_l2vad_dump_write_wav_header(dump->fp, 0);
}

static int zh_audio_dump_open_path(zh_l2vad_dump_t *dump,
                                   const char *label,
                                   const char *path) {
    if (!dump || !label || !path) {
        return -1;
    }
    memset(dump, 0, sizeof(*dump));
    snprintf(dump->label, sizeof(dump->label), "%s", label);
    snprintf(dump->final_path, sizeof(dump->final_path), "%s", path);
    snprintf(dump->tmp_path, sizeof(dump->tmp_path), "%s.tmp", dump->final_path);
    dump->enabled = 1;
    dump->fp = fopen(dump->tmp_path, "wb");
    dump->frames = 0;
    if (!dump->fp) {
        LOGE(__func__, "%s dump open failed: %s errno=%d",
             dump->label, dump->tmp_path, errno);
        return -1;
    }
    zh_l2vad_dump_write_wav_header(dump->fp, 0);
    return 0;
}

static void zh_l2vad_dump_write_mono(zh_l2vad_dump_t *dump,
                                     const int16_t *pcm,
                                     size_t frames) {
    if (!dump || !dump->fp || !pcm || frames == 0) {
        return;
    }
    if (fwrite(pcm, sizeof(int16_t), frames, dump->fp) == frames) {
        dump->frames += (uint32_t)frames;
    }
}

static void zh_l2vad_dump_write_frame(zh_l2vad_dump_t *dump,
                                      const int16_t *pcm,
                                      size_t samples) {
    int16_t mono[ZH_AUDIO_FRAME_SAMPLES];

    if (!dump || !dump->fp || !pcm || samples < ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS) {
        return;
    }
    for (size_t i = 0; i < ZH_AUDIO_FRAME_SAMPLES; ++i) {
        mono[i] = pcm[i * ZH_AUDIO_CHANNELS];
    }
    if (fwrite(mono, sizeof(int16_t), ZH_AUDIO_FRAME_SAMPLES, dump->fp) == ZH_AUDIO_FRAME_SAMPLES) {
        dump->frames += ZH_AUDIO_FRAME_SAMPLES;
    }
}

typedef struct {
    int enabled;
    int active;
    unsigned int seq;
    char dir[256];
    zh_l2vad_dump_t raw;
    zh_l2vad_dump_t ref;
    zh_l2vad_dump_t aec;
    int16_t ref_frame[ZH_AUDIO_FRAME_SAMPLES];
} zh_aec_triplet_dump_t;

static void zh_aec_triplet_dump_init(zh_aec_triplet_dump_t *dump) {
    const char *dir = getenv("ZH_AEC_TRIPLET_DUMP_DIR");

    memset(dump, 0, sizeof(*dump));
    if (!dir || dir[0] == '\0') {
        return;
    }
    snprintf(dump->dir, sizeof(dump->dir), "%s", dir);
    dump->enabled = 1;
    if (zh_l2vad_dump_mkdir_p(dump->dir) != 0) {
        LOGE(__func__, "aec_triplet dump dir create failed: %s errno=%d",
             dump->dir, errno);
        dump->enabled = 0;
        return;
    }
    LOGI(__func__, "aec_triplet dump enabled: dir=%s", dump->dir);
}

static void zh_aec_triplet_dump_close(zh_aec_triplet_dump_t *dump) {
    if (!dump) {
        return;
    }
    zh_l2vad_dump_close(&dump->raw);
    zh_l2vad_dump_close(&dump->ref);
    zh_l2vad_dump_close(&dump->aec);
    dump->active = 0;
}

static void zh_aec_triplet_dump_open(zh_aec_triplet_dump_t *dump) {
    time_t now = 0;
    struct tm tm_now;
    char ts[32];
    char raw_path[512];
    char ref_path[512];
    char aec_path[512];

    if (!dump || !dump->enabled || dump->active) {
        return;
    }

    now = time(NULL);
    memset(&tm_now, 0, sizeof(tm_now));
    if (now >= 0) {
        localtime_r(&now, &tm_now);
    }
    if (strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now) == 0) {
        snprintf(ts, sizeof(ts), "unknown_time");
    }
    dump->seq++;
    snprintf(raw_path, sizeof(raw_path), "%s/triplet_%s_%04u_raw.wav",
             dump->dir, ts, dump->seq);
    snprintf(ref_path, sizeof(ref_path), "%s/triplet_%s_%04u_ref.wav",
             dump->dir, ts, dump->seq);
    snprintf(aec_path, sizeof(aec_path), "%s/triplet_%s_%04u_aec.wav",
             dump->dir, ts, dump->seq);

    if (zh_audio_dump_open_path(&dump->raw, "triplet_raw", raw_path) != 0 ||
        zh_audio_dump_open_path(&dump->ref, "triplet_ref", ref_path) != 0 ||
        zh_audio_dump_open_path(&dump->aec, "triplet_aec", aec_path) != 0) {
        zh_aec_triplet_dump_close(dump);
        return;
    }
    dump->active = 1;
}

static void zh_aec_triplet_dump_write(zh_aec_triplet_dump_t *dump,
                                      const int16_t *raw_frame,
                                      const int16_t *aec_frame) {
    if (!dump || !dump->active) {
        return;
    }
    zh_audio_playback_ref_read_delayed(dump->ref_frame, ZH_AUDIO_FRAME_SAMPLES);
    zh_l2vad_dump_write_frame(&dump->raw,
                              raw_frame,
                              ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
    zh_l2vad_dump_write_mono(&dump->ref, dump->ref_frame, ZH_AUDIO_FRAME_SAMPLES);
    zh_l2vad_dump_write_frame(&dump->aec,
                              aec_frame,
                              ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
}

static void zh_l2vad_dump_flush_preroll(zh_l2vad_dump_t *dump,
                                        const int16_t *preroll,
                                        size_t frame_len,
                                        size_t frames,
                                        size_t pos,
                                        int full) {
    if (!dump || !dump->fp || !preroll || frame_len == 0 || frames == 0) {
        return;
    }
    if (!full) {
        for (size_t i = 0; i < pos; ++i) {
            zh_l2vad_dump_write_frame(dump, preroll + i * frame_len, frame_len);
        }
        return;
    }
    for (size_t i = pos; i < frames; ++i) {
        zh_l2vad_dump_write_frame(dump, preroll + i * frame_len, frame_len);
    }
    for (size_t i = 0; i < pos; ++i) {
        zh_l2vad_dump_write_frame(dump, preroll + i * frame_len, frame_len);
    }
}

static zh_ws_session_t *zh_audio_uplink_get_ws(void) {
    zh_ws_session_t *ws = NULL;

    pthread_mutex_lock(&g_uplink_ws_mutex);
    ws = g_uplink_ws;
    pthread_mutex_unlock(&g_uplink_ws_mutex);
    return ws;
}

static void zh_ws_report_asr_upload_meta(zh_ws_session_t *ws, int music_playing) {
    char msg[256];
    time_t ts = time(NULL);

    if (!ws) {
        return;
    }
    if (ts < 0) {
        ts = 0;
    }
    snprintf(msg, sizeof(msg),
             "{\"type\":\"" ZH_MSG_TYPE_ASR_UPLOAD_META "\",\"timestamp\":%lld,"
             "\"code_version\":\"%s\",\"music_playing\":%s,\"required_keyword\":\"%s\"}",
             (long long)ts,
             ZH_APP_VERSION,
             music_playing ? "true" : "false",
             "对话模式");
    if (zh_ws_send_str(ws, msg) != 0) {
        LOGE(__func__, "ASR_UPLOAD_META send failed");
    }
}

static void zh_ws_report_dialog_gate_meta(zh_ws_session_t *ws, int tts_playing) {
    char msg[256];
    time_t ts = time(NULL);

    if (!ws) {
        return;
    }
    if (ts < 0) {
        ts = 0;
    }
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"" ZH_MSG_TYPE_ASR_GATE_META "\",\"timestamp\":%lld,"
                 "\"tts_playing\":%s,\"wake_words\":[\"龙虾\"],\"followup_window_ms\":5000}",
                 (long long)ts,
                 tts_playing ? "true" : "false");
    if (zh_ws_send_str(ws, msg) != 0) {
        LOGE(__func__, "ASR_GATE_META send failed");
    }
}

static int zh_audio_uplink_submit_frame(OpusEncoder *enc,
                                        const int16_t *pcm,
                                        size_t samples
#if ZH_VAD_RECORD_DUMP_ENABLE
                                        ,
                                        FILE *dump_fp
#endif
                                        ) {
    int16_t mono[ZH_OPUS_FRAME_SAMPLES];
    uint8_t opus_buf[4000];
    int opus_len = 0;

    if (!enc || !pcm || samples == 0) {
        return -1;
    }
    if (samples < ZH_OPUS_FRAME_SAMPLES * ZH_AUDIO_CHANNELS) {
        return -1;
    }
    for (size_t i = 0; i < ZH_OPUS_FRAME_SAMPLES; ++i) {
        mono[i] = pcm[i * ZH_AUDIO_CHANNELS];
    }
#if ZH_VAD_RECORD_DUMP_ENABLE
    zh_pre_opus_pcm_dump_write(dump_fp, mono, ZH_OPUS_FRAME_SAMPLES);
#endif
    opus_len = opus_encode(enc, mono, ZH_OPUS_FRAME_SAMPLES, opus_buf, (opus_int32)sizeof(opus_buf));
    if (opus_len < 0) {
        LOGE(__func__, "opus encode failed: %d", opus_len);
        return -1;
    }
    return zh_core_uplink_send_opus(opus_buf, (size_t)opus_len);
}

static int zh_audio_uplink_flush_preroll(OpusEncoder *enc,
                                         const int16_t *preroll,
                                         size_t frame_len,
                                         size_t frames,
                                         size_t pos,
                                         int full
#if ZH_VAD_RECORD_DUMP_ENABLE
                                         ,
                                         FILE *dump_fp
#endif
                                         ) {
    if (!enc || !preroll || frame_len == 0 || frames == 0) {
        return -1;
    }
    if (!full) {
        for (size_t i = 0; i < pos; ++i) {
            if (zh_audio_uplink_submit_frame(enc, preroll + i * frame_len, frame_len
#if ZH_VAD_RECORD_DUMP_ENABLE
                                             ,
                                             dump_fp
#endif
                ) != 0) {
                return -1;
            }
        }
        return 0;
    }
    for (size_t i = pos; i < frames; ++i) {
        if (zh_audio_uplink_submit_frame(enc, preroll + i * frame_len, frame_len
#if ZH_VAD_RECORD_DUMP_ENABLE
                                         ,
                                         dump_fp
#endif
            ) != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < pos; ++i) {
        if (zh_audio_uplink_submit_frame(enc, preroll + i * frame_len, frame_len
#if ZH_VAD_RECORD_DUMP_ENABLE
                                         ,
                                         dump_fp
#endif
            ) != 0) {
            return -1;
        }
    }
    return 0;
}

static void *zh_audio_uplink_thread_main(void *arg) {
    zh_audio_capture_t *cap = NULL;
    zh_audio_preprocess_t *preprocess = NULL;
    zh_core_vad_t *vad = NULL;
    OpusEncoder *enc = NULL;
    int16_t *frame = NULL;
    int16_t *processed_frame = NULL;
    int16_t *vad_frame = NULL;
    int16_t preroll[ZH_VAD_PREROLL_FRAMES][ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS];
    size_t preroll_pos = 0;
    int preroll_full = 0;
    int speech_active = 0;
    int vad_gate_open = 0;
    int vad_gate_below_close_frames = 0;
#if !ZH_ENABLE_AEC
    int stt_completed_seen = 0;
#endif
    int music_state_last_reported = -1;
    int playback_suppressed = 0;
    zh_l2vad_dump_t l2vad_dump;
    zh_l2vad_dump_t tts_aec_dump;
    zh_aec_triplet_dump_t triplet_dump;
    int tts_aec_dump_active = 0;
    unsigned int actual_rate = 0;
    unsigned int actual_channels = 0;
    int opus_err = 0;
#if ZH_VAD_RECORD_DUMP_ENABLE
    FILE *pre_opus_dump_fp = NULL;
#endif

    (void)arg;
    zh_l2vad_dump_init(&l2vad_dump);
    zh_tts_aec_dump_init(&tts_aec_dump);
    zh_aec_triplet_dump_init(&triplet_dump);

    unsigned int capture_open_attempts = 0;
    while (g_uplink_running &&
           zh_audio_capture_init(&cap, &actual_rate, &actual_channels) != 0) {
        capture_open_attempts++;
        LOGW(__func__, "audio capture unavailable, retry in %d ms: attempt=%u",
             ZH_AUDIO_CAPTURE_OPEN_RETRY_MS, capture_open_attempts);
        usleep(ZH_AUDIO_CAPTURE_OPEN_RETRY_MS * 1000);
    }
    if (!g_uplink_running || !cap) {
        goto cleanup;
    }
    if (capture_open_attempts > 0) {
        LOGI(__func__, "audio capture recovered after retries: attempts=%u",
             capture_open_attempts);
    }
    if (zh_audio_capture_start(cap) != 0) {
        goto cleanup;
    }
    if (actual_rate != 0 && actual_channels != 0) {
        LOGI(__func__, "pcm params: rate=%u channels=%u uplink_mic_channel=%d ref_channel=%d ref_channels=%d",
             actual_rate, actual_channels, ZH_AUDIO_MIC_CHANNEL_INDEX,
             ZH_AUDIO_REF_CHANNEL_INDEX, ZH_AUDIO_REF_CHANNELS);
    }

    zh_audio_preprocess_config_t pp_cfg;
    memset(&pp_cfg, 0, sizeof(pp_cfg));
    pp_cfg.sample_rate = ZH_AUDIO_SAMPLE_RATE;
    pp_cfg.bits_per_sample = 16;
    pp_cfg.frame_samples = ZH_AUDIO_FRAME_SAMPLES;
    pp_cfg.channels = ZH_AUDIO_CHANNELS;
    pp_cfg.mic_channel_index = ZH_AUDIO_MIC_CHANNEL_INDEX;
    pp_cfg.ref_channel_index = ZH_AUDIO_REF_CHANNEL_INDEX;
    pp_cfg.ref_channels = ZH_AUDIO_REF_CHANNELS;
    pp_cfg.aec_frame_samples = ZH_AUDIO_AEC_FRAME_SAMPLES;
    if (zh_audio_preprocess_create(&preprocess, &pp_cfg) != 0) {
        LOGE(__func__, "audio preprocess create failed");
        goto cleanup;
    }

    vad = zh_core_vad_create();
    if (!vad) {
        LOGE(__func__, "core vad create failed");
        goto cleanup;
    }

    enc = opus_encoder_create(ZH_OPUS_SAMPLE_RATE, ZH_OPUS_CHANNELS, OPUS_APPLICATION_AUDIO, &opus_err);
    if (!enc || opus_err != OPUS_OK) {
        LOGE(__func__, "opus encoder create failed: %d", opus_err);
        goto cleanup;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));

    frame = (int16_t *)calloc(ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS, sizeof(int16_t));
    if (!frame) {
        LOGE(__func__, "frame alloc failed");
        goto cleanup;
    }
    processed_frame = (int16_t *)calloc(ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS, sizeof(int16_t));
    if (!processed_frame) {
        LOGE(__func__, "processed frame alloc failed");
        goto cleanup;
    }
    vad_frame = (int16_t *)calloc(ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS, sizeof(int16_t));
    if (!vad_frame) {
        LOGE(__func__, "vad frame alloc failed");
        goto cleanup;
    }

#if ZH_VAD_RECORD_DUMP_ENABLE
    pre_opus_dump_fp = fopen(ZH_VAD_RECORD_DUMP_PATH, "ab");
    if (!pre_opus_dump_fp) {
        LOGE(__func__, "pre-opus pcm dump open failed: %s", strerror(errno));
    }
#endif

    LOGI(__func__, "audio uplink thread started");
    while (g_uplink_running) {
        zh_ws_session_t *ws = NULL;
        zh_core_vad_result_t result;
        int16_t vad_peak = 0;
        int got = 0;
        int was_speech_active = 0;
        int was_vad_gate_open = 0;
        int frame_flushed_in_preroll = 0;
        int dump_flushed_in_preroll = 0;
        int music_active = 0;
        int tts_playing_snapshot = 0;
#if !ZH_ENABLE_AEC
        int stt_completed_snapshot = 0;
        int tts_round_done_snapshot = 0;
        int suppress_after_stt = 0;
#endif

        got = zh_audio_capture_read(cap, frame, ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
        if (got < 0) {
            if (got == -EPIPE) {
                continue;
            }
            usleep(10000);
            continue;
        }
        if (got != (int)(ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS)) {
            continue;
        }
        if (zh_audio_preprocess_process(preprocess,
                                        frame,
                                        ZH_AUDIO_FRAME_SAMPLES,
                                        processed_frame,
                                        ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS) < 0) {
            continue;
        }

        ws = zh_audio_uplink_get_ws();
        music_active = zh_music_player_is_active() ? 1 : 0;
        tts_playing_snapshot = zh_udp_tts_is_playing() ? 1 : 0;
#if !ZH_ENABLE_AEC
        stt_completed_snapshot = ws && zh_ws_is_stt_completed(ws);
        tts_round_done_snapshot = ws && zh_udp_tts_is_round_done();
        suppress_after_stt = stt_completed_snapshot && !tts_round_done_snapshot;
        if (music_active || tts_playing_snapshot) {
            if (!playback_suppressed || speech_active || vad_gate_open) {
                LOGI(__func__,
                     "suppress vad during playback: music=%d tts=%d",
                     music_active,
                     tts_playing_snapshot);
                zh_audio_uplink_reset_vad_round(vad,
                                                &speech_active,
                                                &preroll_pos,
                                                &preroll_full,
                                                &vad_gate_open,
                                                &vad_gate_below_close_frames);
            }
            playback_suppressed = 1;
            continue;
        }
        playback_suppressed = 0;
#endif
        if (music_state_last_reported < 0) {
            music_state_last_reported = music_active;
        } else if (music_state_last_reported != music_active) {
            zh_ws_report_asr_upload_meta(ws, music_active);
            zh_ws_report_dialog_gate_meta(ws, tts_playing_snapshot);
            music_state_last_reported = music_active;
        }

#if !ZH_ENABLE_AEC
        if (suppress_after_stt) {
            if (!stt_completed_seen) {
                stt_completed_seen = 1;
                LOGI(__func__, "suppress vad after STT_COMPLETED until TTS playback round done");
            }
            zh_audio_uplink_reset_vad_round(vad,
                                            &speech_active,
                                            &preroll_pos,
                                            &preroll_full,
                                            &vad_gate_open,
                                            &vad_gate_below_close_frames);
            continue;
        } else if (stt_completed_seen) {
            stt_completed_seen = 0;
            LOGI(__func__, "resume vad after TTS playback round done");
        }
#endif

        was_speech_active = speech_active;
        was_vad_gate_open = vad_gate_open;
        (void)zh_audio_vad_gate_filter(processed_frame,
                                       vad_frame,
                                       ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS,
                                       &vad_gate_open,
                                       &vad_gate_below_close_frames,
                                       &vad_peak);
        if (!was_vad_gate_open && vad_gate_open) {
            LOGI(__func__,
                 "vad gate opened: peak=%d open=%d",
                 (int)vad_peak,
                 ZH_VAD_GATE_OPEN_PEAK);
        }
        if (zh_core_vad_process(vad, vad_frame, ZH_AUDIO_FRAME_SAMPLES, ZH_AUDIO_CHANNELS, &result) != 0) {
            continue;
        }
        speech_active = result.speech_active;

        for (size_t i = 0; i < ZH_AUDIO_FRAME_SAMPLES; ++i) {
            for (size_t ch = 0; ch < ZH_AUDIO_CHANNELS; ++ch) {
                size_t idx = i * ZH_AUDIO_CHANNELS + ch;
                processed_frame[idx] = zh_apply_gain(processed_frame[idx], ZH_AUDIO_GAIN);
            }
        }

        if (tts_playing_snapshot) {
            if (!tts_aec_dump_active) {
                zh_tts_aec_dump_open(&tts_aec_dump);
                zh_aec_triplet_dump_open(&triplet_dump);
                tts_aec_dump_active = 1;
            }
            zh_l2vad_dump_write_frame(&tts_aec_dump,
                                      processed_frame,
                                      ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
            zh_aec_triplet_dump_write(&triplet_dump, frame, processed_frame);
        } else if (tts_aec_dump_active) {
            zh_l2vad_dump_close(&tts_aec_dump);
            zh_aec_triplet_dump_close(&triplet_dump);
            tts_aec_dump_active = 0;
        }

        if (!was_speech_active) {
            memcpy(preroll[preroll_pos],
                   processed_frame,
                   sizeof(int16_t) * ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
            preroll_pos = (preroll_pos + 1) % ZH_VAD_PREROLL_FRAMES;
            if (preroll_pos == 0) {
                preroll_full = 1;
            }
        }

        if (result.speech_started) {
            zh_l2vad_dump_open(&l2vad_dump, result.l2_prob);
            zh_l2vad_dump_flush_preroll(&l2vad_dump,
                                        &preroll[0][0],
                                        ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS,
                                        ZH_VAD_PREROLL_FRAMES,
                                        preroll_pos,
                                        preroll_full);
            dump_flushed_in_preroll = 1;
            zh_face_recognition_set_active(1);
            if (ws) {
                zh_ws_report_asr_upload_meta(ws, music_active);
                zh_ws_report_dialog_gate_meta(ws, tts_playing_snapshot);
                if (zh_core_uplink_begin_segment() != 0) {
                    LOGE(__func__, "uplink START send failed");
                } else {
                    if (zh_audio_uplink_flush_preroll(enc,
                                                      &preroll[0][0],
                                                      ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS,
                                                      ZH_VAD_PREROLL_FRAMES,
                                                      preroll_pos,
                                                      preroll_full
#if ZH_VAD_RECORD_DUMP_ENABLE
                                                      ,
                                                      pre_opus_dump_fp
#endif
                        ) == 0) {
                        frame_flushed_in_preroll = 1;
                    }
                }
            }
        }

        if (speech_active && !dump_flushed_in_preroll) {
            zh_l2vad_dump_write_frame(&l2vad_dump,
                                      processed_frame,
                                      ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
        }

        if (speech_active && ws) {
            if (!frame_flushed_in_preroll) {
                (void)zh_audio_uplink_submit_frame(enc,
                                                   processed_frame,
                                                   ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS
#if ZH_VAD_RECORD_DUMP_ENABLE
                                                   ,
                                                   pre_opus_dump_fp
#endif
                );
            }
        }

        if (result.speech_ended) {
            zh_l2vad_dump_close(&l2vad_dump);
            zh_face_recognition_set_active(0);
#if ZH_CLIENT_ADVANCED_VAD_ENABLE
            if (zh_core_uplink_flush_wait(ZH_UPLINK_END_FLUSH_WAIT_MS) != 0) {
                LOGW(__func__,
                     "uplink tail flush wait timeout before END: timeout_ms=%d",
                     ZH_UPLINK_END_FLUSH_WAIT_MS);
            }
            if (ws) {
                if (zh_ws_send_str(ws, "END") != 0) {
                    LOGE(__func__, "uplink END send failed");
                } else {
                    LOGI(__func__, "uplink END sent");
                }
            }
#else
            LOGI(__func__, "local vad ended, skip END because CLIENT_ADVANCED_VAD is disabled");
#endif
        }
    }

cleanup:
    if (vad_frame) {
        free(vad_frame);
    }
    if (processed_frame) {
        free(processed_frame);
    }
    if (frame) {
        free(frame);
    }
    if (enc) {
        opus_encoder_destroy(enc);
    }
    zh_l2vad_dump_close(&l2vad_dump);
    zh_l2vad_dump_close(&tts_aec_dump);
    zh_aec_triplet_dump_close(&triplet_dump);
#if ZH_VAD_RECORD_DUMP_ENABLE
    if (pre_opus_dump_fp) {
        fclose(pre_opus_dump_fp);
    }
#endif
    zh_core_vad_destroy(vad);
    zh_audio_preprocess_destroy(preprocess);
    zh_audio_capture_stop(cap);
    zh_audio_capture_deinit(cap);
    g_uplink_running = 0;
    LOGI(__func__, "audio uplink thread exit");
    return NULL;
}

int zh_audio_uplink_preload(void) {
    return zh_core_vad_preload();
}

int zh_audio_uplink_start(void) {
    int err = 0;

    if (g_uplink_running) {
        return 0;
    }
    if (zh_core_uplink_start() != 0) {
        return -1;
    }
    g_uplink_running = 1;
    err = pthread_create(&g_uplink_thread, NULL, zh_audio_uplink_thread_main, NULL);
    if (err != 0) {
        g_uplink_running = 0;
        zh_core_uplink_stop();
        errno = err;
        return -1;
    }
    return 0;
}

void zh_audio_uplink_stop(void) {
    if (!g_uplink_running) {
        zh_core_uplink_stop();
        return;
    }
    g_uplink_running = 0;
    pthread_join(g_uplink_thread, NULL);
    zh_core_uplink_stop();
}

void zh_audio_uplink_set_ws(zh_ws_session_t *ws) {
    pthread_mutex_lock(&g_uplink_ws_mutex);
    g_uplink_ws = ws;
    pthread_mutex_unlock(&g_uplink_ws_mutex);
}
