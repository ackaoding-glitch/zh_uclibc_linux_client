#include <errno.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "audio_playback_rockit.h"
#include "bithion_core.h"
#include "bithion_core_bridge.h"
#include "config.h"
#include "log.h"
#include "music_player.h"
#include "prompt_tone.h"
#include "udp_tts.h"
#include "utils.h"
#include "ws.h"

#define ZH_TTS_WAIT_MUSIC_IDLE_MS 300
#define ZH_TTS_SUBTITLE_QUEUE_SIZE 32

static volatile int g_tts_play_running = 0;
static volatile int g_tts_flush_pending = 0;
static volatile int g_tts_playing = 0;
static volatile int g_tts_drop_stale_pending = 0;
static volatile uint32_t g_tts_interrupt_generation = 0;
static volatile uint64_t g_tts_packet_count = 0;
static volatile int32_t g_tts_packet_chat_count = -1;
static volatile int32_t g_tts_playback_stopped_reported_chat = -1;
// 百度回调 TTS 与前置 TTS 共用 chat_count；收到新子轮首帧前不能沿用上一段的完成状态。
static volatile int g_tts_subround_waiting_first_frame = 0;
static pthread_t g_tts_play_thread;
static zh_ws_session_t *g_tts_ws = NULL;
static uint64_t g_tts_last_audio_ms = 0;
static uint64_t g_tts_open_fail_log_ms = 0;
static pthread_mutex_t g_tts_playback_mutex = PTHREAD_MUTEX_INITIALIZER;
static zh_ao_playback_t *g_tts_active_playback = NULL;

typedef struct {
    int used;
    uint32_t chat_count;
    uint32_t seq_start;
    char text[1024];
} zh_tts_subtitle_t;

static pthread_mutex_t g_tts_subtitle_mutex = PTHREAD_MUTEX_INITIALIZER;
static zh_tts_subtitle_t g_tts_subtitles[ZH_TTS_SUBTITLE_QUEUE_SIZE];
static size_t g_tts_subtitle_count = 0;
static int g_tts_playback_progress_valid = 0;
static uint32_t g_tts_playback_progress_chat = 0;
static uint32_t g_tts_playback_progress_seq = 0;

typedef struct {
    int enabled;
    unsigned int seq;
    uint32_t frames;
    char dir[256];
    char tmp_path[544];
    char final_path[512];
    FILE *fp;
} zh_tts_dump_t;

static zh_tts_dump_t g_tts_dump;

static int zh_tts_dump_mkdir_p(const char *path) {
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

static void zh_tts_dump_write_u16le(FILE *fp, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xffU), (uint8_t)((v >> 8) & 0xffU)};
    fwrite(b, 1, sizeof(b), fp);
}

static void zh_tts_dump_write_u32le(FILE *fp, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xffU),
        (uint8_t)((v >> 8) & 0xffU),
        (uint8_t)((v >> 16) & 0xffU),
        (uint8_t)((v >> 24) & 0xffU),
    };
    fwrite(b, 1, sizeof(b), fp);
}

static void zh_tts_dump_write_wav_header(FILE *fp, uint32_t frames) {
    const uint16_t channels = (uint16_t)ZH_TTS_CHANNELS;
    const uint32_t data_bytes = frames * channels * sizeof(int16_t);
    const uint32_t byte_rate = ZH_TTS_SAMPLE_RATE * channels * sizeof(int16_t);
    const uint16_t block_align = channels * sizeof(int16_t);

    fwrite("RIFF", 1, 4, fp);
    zh_tts_dump_write_u32le(fp, 36U + data_bytes);
    fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    zh_tts_dump_write_u32le(fp, 16);
    zh_tts_dump_write_u16le(fp, 1);
    zh_tts_dump_write_u16le(fp, channels);
    zh_tts_dump_write_u32le(fp, ZH_TTS_SAMPLE_RATE);
    zh_tts_dump_write_u32le(fp, byte_rate);
    zh_tts_dump_write_u16le(fp, block_align);
    zh_tts_dump_write_u16le(fp, 16);
    fwrite("data", 1, 4, fp);
    zh_tts_dump_write_u32le(fp, data_bytes);
}

static void zh_tts_dump_init(zh_tts_dump_t *dump) {
    const char *dir = getenv("ZH_TTS_DUMP_DIR");

    memset(dump, 0, sizeof(*dump));
    if (!dir || dir[0] == '\0') {
        return;
    }
    snprintf(dump->dir, sizeof(dump->dir), "%s", dir);
    dump->enabled = 1;
    if (zh_tts_dump_mkdir_p(dump->dir) != 0) {
        LOGE(__func__, "tts dump dir create failed: %s errno=%d", dump->dir, errno);
        dump->enabled = 0;
        return;
    }
    LOGI(__func__, "tts dump enabled: dir=%s", dump->dir);
}

static void zh_tts_dump_close(zh_tts_dump_t *dump) {
    if (!dump || !dump->fp) {
        return;
    }
    fflush(dump->fp);
    if (fseek(dump->fp, 0, SEEK_SET) == 0) {
        zh_tts_dump_write_wav_header(dump->fp, dump->frames);
        fflush(dump->fp);
    }
    fclose(dump->fp);
    dump->fp = NULL;
    if (dump->frames > 0) {
        if (rename(dump->tmp_path, dump->final_path) == 0) {
            LOGI(__func__, "tts dump saved: %s frames=%u", dump->final_path, dump->frames);
        } else {
            LOGE(__func__, "tts dump rename failed: %s -> %s errno=%d",
                 dump->tmp_path, dump->final_path, errno);
        }
    } else {
        unlink(dump->tmp_path);
    }
    dump->frames = 0;
    dump->tmp_path[0] = '\0';
    dump->final_path[0] = '\0';
}

static void zh_tts_dump_open(zh_tts_dump_t *dump, int chat_count) {
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
    if (chat_count >= 0) {
        snprintf(dump->final_path,
                 sizeof(dump->final_path),
                 "%s/tts_%s_%04u_chat%d.wav",
                 dump->dir,
                 ts,
                 dump->seq,
                 chat_count);
    } else {
        snprintf(dump->final_path,
                 sizeof(dump->final_path),
                 "%s/tts_%s_%04u_chatx.wav",
                 dump->dir,
                 ts,
                 dump->seq);
    }
    snprintf(dump->tmp_path, sizeof(dump->tmp_path), "%s.tmp", dump->final_path);
    dump->fp = fopen(dump->tmp_path, "wb");
    dump->frames = 0;
    if (!dump->fp) {
        LOGE(__func__, "tts dump open failed: %s errno=%d", dump->tmp_path, errno);
        return;
    }
    zh_tts_dump_write_wav_header(dump->fp, 0);
}

static void zh_tts_dump_write(zh_tts_dump_t *dump, const int16_t *pcm, size_t frames) {
    size_t samples = frames * ZH_TTS_CHANNELS;

    if (!dump || !dump->fp || !pcm || frames == 0) {
        return;
    }
    if (fwrite(pcm, sizeof(int16_t), samples, dump->fp) == samples) {
        dump->frames += (uint32_t)frames;
    }
}

static uint64_t zh_tts_wall_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void zh_tts_subtitle_remove_at_locked(size_t idx) {
    if (idx >= g_tts_subtitle_count) {
        return;
    }
    for (size_t i = idx + 1; i < g_tts_subtitle_count; ++i) {
        g_tts_subtitles[i - 1] = g_tts_subtitles[i];
    }
    if (g_tts_subtitle_count > 0) {
        g_tts_subtitle_count--;
    }
    memset(&g_tts_subtitles[g_tts_subtitle_count], 0, sizeof(g_tts_subtitles[g_tts_subtitle_count]));
}

static void zh_tts_subtitle_clear(void) {
    pthread_mutex_lock(&g_tts_subtitle_mutex);
    memset(g_tts_subtitles, 0, sizeof(g_tts_subtitles));
    g_tts_subtitle_count = 0;
    g_tts_playback_progress_valid = 0;
    g_tts_playback_progress_chat = 0;
    g_tts_playback_progress_seq = 0;
    pthread_mutex_unlock(&g_tts_subtitle_mutex);
}

static void zh_tts_flush_subtitles_for_progress(uint32_t chat_count, uint32_t packet_seq) {
    char text[1024];

    while (1) {
        int found = 0;

        text[0] = '\0';
        pthread_mutex_lock(&g_tts_subtitle_mutex);
        for (size_t i = 0; i < g_tts_subtitle_count; ++i) {
            if (g_tts_subtitles[i].chat_count < chat_count) {
                LOGD(__func__,
                     "drop stale subtitle: chat=%u current_chat=%u seq_start=%u",
                     g_tts_subtitles[i].chat_count,
                     chat_count,
                     g_tts_subtitles[i].seq_start);
                zh_tts_subtitle_remove_at_locked(i);
                i--;
                continue;
            }
            if (g_tts_subtitles[i].chat_count == chat_count &&
                g_tts_subtitles[i].seq_start <= packet_seq) {
                snprintf(text, sizeof(text), "%s", g_tts_subtitles[i].text);
                zh_tts_subtitle_remove_at_locked(i);
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_tts_subtitle_mutex);

        if (!found) {
            break;
        }
        LOGD(__func__,
             "show synced subtitle: chat=%u packet_seq=%u text_len=%zu",
             chat_count,
             packet_seq,
             strlen(text));
    }
}

void zh_udp_tts_schedule_subtitle(uint32_t chat_count,
                                  uint32_t seq_start,
                                  const char *text) {
    char emit_now[1024];
    int should_emit_now = 0;

    if (!text || text[0] == '\0' || chat_count == 0 || seq_start == 0) {
        return;
    }

    emit_now[0] = '\0';
    pthread_mutex_lock(&g_tts_subtitle_mutex);
    if (g_tts_playback_progress_valid &&
        g_tts_playback_progress_chat == chat_count &&
        g_tts_playback_progress_seq >= seq_start) {
        snprintf(emit_now, sizeof(emit_now), "%s", text);
        should_emit_now = 1;
    } else {
        if (g_tts_subtitle_count == ZH_TTS_SUBTITLE_QUEUE_SIZE) {
            LOGW(__func__,
                 "subtitle queue full, drop oldest: chat=%u seq_start=%u",
                 g_tts_subtitles[0].chat_count,
                 g_tts_subtitles[0].seq_start);
            zh_tts_subtitle_remove_at_locked(0);
        }
        g_tts_subtitles[g_tts_subtitle_count].used = 1;
        g_tts_subtitles[g_tts_subtitle_count].chat_count = chat_count;
        g_tts_subtitles[g_tts_subtitle_count].seq_start = seq_start;
        snprintf(g_tts_subtitles[g_tts_subtitle_count].text,
                 sizeof(g_tts_subtitles[g_tts_subtitle_count].text),
                 "%s",
                 text);
        g_tts_subtitle_count++;
        LOGD(__func__,
             "queue synced subtitle: chat=%u seq_start=%u text_len=%zu",
             chat_count,
             seq_start,
             strlen(text));
    }
    pthread_mutex_unlock(&g_tts_subtitle_mutex);

    if (should_emit_now) {
        LOGD(__func__,
             "show late subtitle immediately: chat=%u seq_start=%u text_len=%zu",
             chat_count,
             seq_start,
             strlen(emit_now));
    }
}

static void zh_tts_update_subtitle_progress(const zh_core_tts_opus_meta_t *meta) {
    if (!meta || meta->chat_count == 0 || meta->packet_seq == 0) {
        return;
    }
    pthread_mutex_lock(&g_tts_subtitle_mutex);
    g_tts_playback_progress_valid = 1;
    g_tts_playback_progress_chat = meta->chat_count;
    g_tts_playback_progress_seq = meta->packet_seq;
    pthread_mutex_unlock(&g_tts_subtitle_mutex);
    zh_tts_flush_subtitles_for_progress(meta->chat_count, meta->packet_seq);
}

static void zh_tts_set_playing_state(int playing) {
    int normalized = playing ? 1 : 0;
    if (g_tts_playing == normalized) {
        return;
    }
    g_tts_playing = normalized;
}

static int zh_tts_get_current_chat_count(uint32_t *out_chat_count) {
    uint32_t chat_count = 0;
    if (!out_chat_count) {
        return -1;
    }
    if (g_tts_packet_chat_count >= 0) {
        *out_chat_count = (uint32_t)g_tts_packet_chat_count;
        return 0;
    }
    if (g_tts_ws && zh_ws_get_stt_completed_chat_count(g_tts_ws, &chat_count) == 0) {
        *out_chat_count = chat_count;
        return 0;
    }
    return -1;
}

static void zh_tts_report_playback_stopped_once(void) {
    char msg[256];
    uint64_t ts_ms = 0;
    uint32_t chat_count = 0;

    if (!g_tts_ws) {
        return;
    }
    if (g_tts_subround_waiting_first_frame) {
        return;
    }
    if (zh_tts_get_current_chat_count(&chat_count) != 0) {
        LOGW(__func__, "skip playback stopped report: no valid chat_count");
        return;
    }
    if ((int32_t)chat_count == g_tts_playback_stopped_reported_chat) {
        return;
    }

    ts_ms = zh_tts_wall_time_ms();
    snprintf(msg, sizeof(msg),
             "{\"type\":\"CLIENT_TTS_PLAYBACK_STOPPED\",\"timestamp\":%llu,\"chat_count\":%u}",
             (unsigned long long)ts_ms,
             (unsigned int)chat_count);
    if (zh_ws_send_str(g_tts_ws, msg) != 0) {
        LOGE(__func__, "CLIENT_TTS_PLAYBACK_STOPPED send failed, chat_count=%u",
             (unsigned int)chat_count);
        return;
    }
    g_tts_playback_stopped_reported_chat = (int32_t)chat_count;
    LOGI(__func__, "reported CLIENT_TTS_PLAYBACK_STOPPED: chat_count=%u", (unsigned int)chat_count);
}

static void zh_tts_release_playback(zh_ao_playback_t **play_ref, int drain) {
    pthread_mutex_lock(&g_tts_playback_mutex);
    if (play_ref && *play_ref) {
        if (drain) {
            zh_ao_playback_drain(*play_ref);
        } else {
            zh_ao_playback_flush(*play_ref);
        }
        zh_ao_playback_close(*play_ref);
        if (g_tts_active_playback == *play_ref) {
            g_tts_active_playback = NULL;
        }
        *play_ref = NULL;
    }
    zh_tts_dump_close(&g_tts_dump);
    pthread_mutex_unlock(&g_tts_playback_mutex);
    g_tts_last_audio_ms = 0;
    zh_tts_set_playing_state(0);
}

static void zh_tts_reset_decoder(OpusDecoder *decoder) {
    if (!decoder) {
        return;
    }
    (void)opus_decoder_ctl(decoder, OPUS_RESET_STATE);
}

static void zh_tts_process_opus(OpusDecoder *decoder,
                                zh_ao_playback_t *playback,
                                const uint8_t *opus_data,
                                size_t opus_len) {
    int16_t pcm[ZH_TTS_MAX_SAMPLES * ZH_TTS_CHANNELS];

    if (!decoder || !playback || !opus_data || opus_len == 0) {
        return;
    }

    if (g_tts_flush_pending || !g_tts_play_running) {
        return;
    }

    int samples = opus_decode(decoder,
                              opus_data,
                              (opus_int32)opus_len,
                              pcm,
                              ZH_TTS_MAX_SAMPLES,
                              0);
    if (samples < 0) {
        LOGE(__func__, "opus decode failed: %d", samples);
        return;
    }
    if (ZH_TTS_GAIN != 1.0f) {
        for (int i = 0; i < samples * ZH_TTS_CHANNELS; ++i) {
            pcm[i] = zh_apply_gain(pcm[i], ZH_TTS_GAIN);
        }
    }
    if (g_tts_flush_pending || !g_tts_play_running) {
        return;
    }
    pthread_mutex_lock(&g_tts_playback_mutex);
    if (playback == g_tts_active_playback && !g_tts_flush_pending && g_tts_play_running) {
        uint64_t now_ms = zh_now_ms();
        int stt_chat_count = -1;
        uint32_t chat_count_tmp = 0;
        if (g_tts_ws && zh_ws_get_stt_completed_chat_count(g_tts_ws, &chat_count_tmp) == 0) {
            stt_chat_count = (int)chat_count_tmp;
        }
        if (stt_chat_count >= 0 && g_tts_packet_chat_count != stt_chat_count) {
            g_tts_packet_chat_count = stt_chat_count;
            g_tts_packet_count = 0;
        }
        uint64_t pkt_idx = ++g_tts_packet_count;
        zh_tts_set_playing_state(1);
        g_tts_last_audio_ms = now_ms;
        zh_tts_dump_open(&g_tts_dump, stt_chat_count);
        zh_tts_dump_write(&g_tts_dump, pcm, (size_t)samples);
        LOGD(__func__,
             "tts play pkt: ts_ms=%llu stt_chat_count=%d pkt_idx=%llu pkt_src=local opus_len=%zu",
             (unsigned long long)now_ms,
             stt_chat_count,
             (unsigned long long)pkt_idx,
             opus_len);
        (void)zh_ao_playback_write(playback, pcm, (size_t)samples);
    }
    pthread_mutex_unlock(&g_tts_playback_mutex);
}

static void zh_udp_tts_wait_music_idle_after_interrupt(void) {
    int waited_ms = 0;

    while (zh_music_player_is_active() && waited_ms < ZH_TTS_WAIT_MUSIC_IDLE_MS) {
        usleep(10 * 1000);
        waited_ms += 10;
    }
    if (zh_music_player_is_active()) {
        LOGW(__func__, "music still active after interrupt wait: waited_ms=%d", waited_ms);
    }
}

static void *zh_udp_tts_play_thread_main(void *arg) {
    uint8_t opus_frame[4000];
    OpusDecoder *decoder = NULL;
    zh_ao_playback_t *playback = NULL;
    int opus_err = 0;

    (void)arg;
    decoder = opus_decoder_create(ZH_TTS_SAMPLE_RATE, ZH_TTS_CHANNELS, &opus_err);
    if (!decoder || opus_err != OPUS_OK) {
        LOGE(__func__, "opus decoder create failed: %d", opus_err);
        g_tts_play_running = 0;
        zh_tts_set_playing_state(0);
        return NULL;
    }

    zh_tts_dump_init(&g_tts_dump);
    LOGI(__func__, "client tts playback thread started");
    while (g_tts_play_running) {
        size_t opus_len = 0;
        int ret = 0;
        uint32_t read_generation = 0;
        zh_core_tts_opus_meta_t opus_meta;

        if (g_tts_flush_pending) {
            zh_tts_release_playback(&playback, 0);
            zh_tts_reset_decoder(decoder);
            g_tts_flush_pending = 0;
        }

        read_generation = g_tts_interrupt_generation;
        memset(&opus_meta, 0, sizeof(opus_meta));
        ret = zh_core_tts_read_opus_with_meta(opus_frame,
                                              sizeof(opus_frame),
                                              &opus_len,
                                              100,
                                              &opus_meta);
        if (!g_tts_play_running) {
            break;
        }
        if (read_generation != g_tts_interrupt_generation) {
            continue;
        }

        if (ret < 0) {
            usleep(10000);
            continue;
        }

        if (ret == 0) {
            int has_pending = zh_core_tts_transport_has_pending_data();
            uint64_t now_ms = zh_now_ms();

            if (g_tts_drop_stale_pending && !has_pending) {
                LOGI(__func__, "interrupt drain finished, accept new round audio");
                zh_tts_reset_decoder(decoder);
                g_tts_drop_stale_pending = 0;
            }

            if (playback && !has_pending && g_tts_last_audio_ms != 0 &&
                now_ms - g_tts_last_audio_ms >= ZH_TTS_IDLE_TIMEOUT_MS) {
                LOGI(__func__, "tts idle timeout, release playback");
                zh_tts_release_playback(&playback, 1);
                zh_tts_reset_decoder(decoder);
            }

            if (g_tts_ws && zh_udp_tts_is_round_done()) {
                zh_tts_report_playback_stopped_once();
                zh_core_ws_on_tts_round_done();
            }

            if (!zh_core_tts_transport_is_running() && !has_pending) {
                break;
            }
            continue;
        }

        if (opus_len == 0) {
            continue;
        }
        g_tts_subround_waiting_first_frame = 0;

        if (g_tts_flush_pending) {
            zh_tts_release_playback(&playback, 0);
            zh_tts_reset_decoder(decoder);
            g_tts_flush_pending = 0;
        }

        if (g_tts_drop_stale_pending) {
            continue;
        }

        zh_ws_cancel_web_search_wait_audio(g_tts_ws);
        if (zh_music_player_is_active() && !zh_music_player_is_waiting_baidu_tts()) {
            LOGI(__func__, "recv tts while music active, interrupt music");
            zh_music_player_interrupt();
        }
        if (!zh_music_player_is_waiting_baidu_tts()) {
            zh_udp_tts_wait_music_idle_after_interrupt();
        }

        if (!playback) {
            if (zh_ao_playback_open(&playback, ZH_TTS_SAMPLE_RATE, ZH_TTS_CHANNELS) != 0) {
                uint64_t now_ms = zh_now_ms();
                if (g_tts_open_fail_log_ms == 0 || now_ms - g_tts_open_fail_log_ms >= 1000) {
                    LOGE(__func__, "tts playback open failed, retry later");
                    g_tts_open_fail_log_ms = now_ms;
                }
                usleep(100000);
                continue;
            }
            pthread_mutex_lock(&g_tts_playback_mutex);
            g_tts_active_playback = playback;
            pthread_mutex_unlock(&g_tts_playback_mutex);
        }

        zh_tts_update_subtitle_progress(&opus_meta);
        zh_tts_process_opus(decoder, playback, opus_frame, opus_len);

        while (g_tts_play_running) {
            zh_core_tts_opus_meta_t drain_meta;

            if (g_tts_flush_pending) {
                zh_tts_release_playback(&playback, 0);
                zh_tts_reset_decoder(decoder);
                g_tts_flush_pending = 0;
                break;
            }

            opus_len = 0;
            read_generation = g_tts_interrupt_generation;
            memset(&drain_meta, 0, sizeof(drain_meta));
            ret = zh_core_tts_read_opus_with_meta(opus_frame,
                                                  sizeof(opus_frame),
                                                  &opus_len,
                                                  0,
                                                  &drain_meta);
            if (read_generation != g_tts_interrupt_generation) {
                continue;
            }
            if (ret <= 0) {
                break;
            }
            if (g_tts_drop_stale_pending) {
                continue;
            }
            if (opus_len > 0) {
                zh_tts_update_subtitle_progress(&drain_meta);
                zh_tts_process_opus(decoder, playback, opus_frame, opus_len);
            }
        }

        if (playback && g_tts_ws && zh_ws_is_tts_completed(g_tts_ws) &&
            !zh_core_tts_transport_has_pending_data()) {
            LOGI(__func__, "tts round done, release playback (tts_completed=%d has_pending=%d)",
                 zh_ws_is_tts_completed(g_tts_ws) ? 1 : 0,
                 zh_core_tts_transport_has_pending_data() ? 1 : 0);
            zh_tts_release_playback(&playback, 1);
            zh_tts_reset_decoder(decoder);
            if (zh_udp_tts_is_round_done()) {
                zh_tts_report_playback_stopped_once();
                zh_core_ws_on_tts_round_done();
            }
        }
    }

    zh_tts_release_playback(&playback, 0);
    zh_tts_dump_close(&g_tts_dump);
    if (decoder) {
        opus_decoder_destroy(decoder);
    }
    g_tts_play_running = 0;
    zh_tts_set_playing_state(0);
    LOGI(__func__, "client tts playback thread exit");
    return NULL;
}

int zh_udp_tts_start(const zh_config_t *cfg, zh_ws_session_t *ws) {
    int err = 0;

    g_tts_ws = ws;
    zh_tts_subtitle_clear();
    if (zh_core_tts_transport_start(cfg, ws) != 0) {
        return -1;
    }

    if (g_tts_play_running) {
        return 0;
    }

    g_tts_flush_pending = 0;
    zh_tts_set_playing_state(0);
    g_tts_drop_stale_pending = 0;
    g_tts_interrupt_generation = 0;
    g_tts_packet_count = 0;
    g_tts_packet_chat_count = -1;
    g_tts_playback_stopped_reported_chat = -1;
    g_tts_subround_waiting_first_frame = 0;
    g_tts_last_audio_ms = 0;
    g_tts_open_fail_log_ms = 0;
    g_tts_play_running = 1;
    err = pthread_create(&g_tts_play_thread, NULL, zh_udp_tts_play_thread_main, NULL);
    if (err != 0) {
        g_tts_play_running = 0;
        zh_core_tts_transport_stop();
        errno = err;
        return -1;
    }
    return 0;
}

void zh_udp_tts_stop(void) {
    g_tts_play_running = 0;
    g_tts_flush_pending = 1;
    zh_tts_set_playing_state(0);
    g_tts_drop_stale_pending = 0;
    g_tts_subround_waiting_first_frame = 0;
    zh_tts_subtitle_clear();
    zh_core_tts_transport_stop();
}

int zh_udp_tts_wait(int timeout_ms) {
    int waited = 0;

    while (g_tts_play_running) {
        usleep(50000);
        waited += 50;
        if (timeout_ms > 0 && waited >= timeout_ms) {
            return -1;
        }
    }

    if (timeout_ms > 0) {
        int remain = timeout_ms - waited;
        return zh_core_tts_transport_wait(remain > 0 ? remain : 1);
    }
    return zh_core_tts_transport_wait(timeout_ms);
}

void zh_udp_tts_interrupt(void) {
    g_tts_flush_pending = 1;
    zh_tts_set_playing_state(0);
    g_tts_drop_stale_pending = 1;
    g_tts_interrupt_generation++;
    g_tts_last_audio_ms = 0;
    g_tts_packet_count = 0;
    g_tts_packet_chat_count = -1;
    g_tts_subround_waiting_first_frame = 0;
    zh_tts_subtitle_clear();

    pthread_mutex_lock(&g_tts_playback_mutex);
    if (g_tts_active_playback) {
        zh_ao_playback_flush(g_tts_active_playback);
    }
    pthread_mutex_unlock(&g_tts_playback_mutex);

    zh_core_tts_transport_interrupt();
}

void zh_udp_tts_set_playing(int playing) {
    zh_tts_set_playing_state(playing);
}

int zh_udp_tts_is_playing(void) {
    return g_tts_playing;
}

void zh_udp_tts_begin_subround(void) {
    g_tts_flush_pending = 1;
    g_tts_drop_stale_pending = 0;
    g_tts_interrupt_generation++;
    g_tts_subround_waiting_first_frame = 1;
    g_tts_packet_count = 0;
    g_tts_packet_chat_count = -1;
    g_tts_playback_stopped_reported_chat = -1;
    g_tts_last_audio_ms = 0;
    bithion_core_tts_reset_subround();
    zh_ws_reset_tts_completed(g_tts_ws);
    LOGI(__func__, "TTS subround reset");
}

int zh_udp_tts_is_busy(void) {
    return (g_tts_playing || zh_core_tts_transport_has_pending_data()) ? 1 : 0;
}

int zh_udp_tts_is_round_done(void) {
    if (g_tts_subround_waiting_first_frame) {
        return 0;
    }
    if (!g_tts_ws || !zh_ws_is_tts_completed(g_tts_ws)) {
        return 0;
    }
    if (g_tts_playing) {
        return 0;
    }
    return zh_core_tts_transport_has_pending_data() ? 0 : 1;
}
