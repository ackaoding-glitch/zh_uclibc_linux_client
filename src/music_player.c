#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "bithion_core.h"
#include "audio_playback_rockit.h"
#include "log.h"
#include "music_player.h"
#include "ws.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// 音乐播放模块：HTTP下载MP3 -> 解码 -> ALSA播放（独立线程）。

typedef struct zh_music_item {
    char *url;
    char *tag;
    int baidu_audio;
    float gain;
    struct zh_music_item *next;
} zh_music_item_t;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    zh_music_item_t *head;
    zh_music_item_t *tail;
    zh_music_item_t *pending_head;
    zh_music_item_t *pending_tail;
    int running;
    int stop;
    int playing;
    uint64_t token;
} zh_music_player_t;

static zh_music_player_t g_music = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static int g_music_stream_running = 0;
static pthread_mutex_t g_baidu_tts_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_baidu_request_id[64];
static int g_baidu_tts_done = 0;
static int g_baidu_waiting_tts = 0;
static int g_baidu_cancelled = 0;

static void zh_music_clear_locked(zh_music_player_t *p) {
    while (p->head) {
        zh_music_item_t *item = p->head;
        p->head = item->next;
        free(item->url);
        free(item->tag);
        free(item);
    }
    p->tail = NULL;
    while (p->pending_head) {
        zh_music_item_t *item = p->pending_head;
        p->pending_head = item->next;
        free(item->url);
        free(item->tag);
        free(item);
    }
    p->pending_tail = NULL;
}

static int zh_music_should_abort(uint64_t token) {
    int abort = 0;
    pthread_mutex_lock(&g_music.mutex);
    abort = g_music.stop || g_music.token != token;
    pthread_mutex_unlock(&g_music.mutex);
    return abort;
}

typedef struct {
    mp3dec_t dec;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t *resample_pcm;
    size_t resample_cap_frames;
    uint8_t *buf;
    size_t len;
    size_t cap;
    zh_ao_playback_t *play;
    int opened;
    unsigned int play_rate;
    int channels;
} zh_music_decoder_t;

typedef struct {
    zh_music_decoder_t *decoder;
    uint64_t token;
    float gain;
} zh_music_stream_feed_ctx_t;

#define ZH_BAIDU_AUDIO_URL "https://gwgp-tdvpwojkegj.i.bdcloudapi.com/sse/v1/aiagent/chat/completions/content/get"
#define ZH_BAIDU_SSE_MAX_EVENT (1024U * 1024U)

typedef struct {
    zh_music_decoder_t decoder;
    zh_ao_playback_t *pcm_play;
    uint64_t token;
    char *buf;
    size_t len;
    size_t cap;
    size_t audio_bytes;
    size_t audio_chunks;
    size_t audio_base64_chars;
    size_t response_events;
    uint8_t *pending_audio;
    size_t pending_audio_len;
    size_t pending_audio_cap;
    char request_id[64];
    char *answer;
    char *resource_json;
    char *resource_url;
    int resource_match;
    int resource_match_known;
    int resource_has_candidate;
    int result_reported;
    float gain;
    unsigned int sample_rate;
    unsigned int channels;
    int format_pcm16;
    int format_mp3;
    int done;
} zh_baidu_stream_ctx_t;

static int zh_music_play_pcm(zh_ao_playback_t *pb, const int16_t *pcm_data,
                             size_t frames, uint64_t token);

static int zh_baidu_tts_is_done(const char *request_id) {
    int done;
    pthread_mutex_lock(&g_baidu_tts_mutex);
    done = request_id && strcmp(g_baidu_request_id, request_id) == 0 && g_baidu_tts_done;
    pthread_mutex_unlock(&g_baidu_tts_mutex);
    return done;
}

void zh_music_player_baidu_tts_done(const char *request_id) {
    pthread_mutex_lock(&g_baidu_tts_mutex);
    if (request_id && strcmp(g_baidu_request_id, request_id) == 0) {
        g_baidu_tts_done = 1;
        g_baidu_waiting_tts = 0;
        LOGI(__func__, "baidu audio TTS gate released: request_id=%s", request_id);
    } else {
        LOGW(__func__, "ignore stale baidu audio TTS done: request_id=%s current=%s",
             request_id ? request_id : "", g_baidu_request_id);
    }
    pthread_mutex_unlock(&g_baidu_tts_mutex);
}

void zh_music_player_baidu_cancel(const char *request_id) {
    pthread_mutex_lock(&g_baidu_tts_mutex);
    if (request_id && request_id[0] &&
        (g_baidu_request_id[0] == '\0' || strcmp(g_baidu_request_id, request_id) == 0)) {
        g_baidu_cancelled = 1;
        g_baidu_tts_done = 1;
        g_baidu_waiting_tts = 0;
        LOGI(__func__, "baidu audio cancelled: request_id=%s", request_id);
    } else {
        LOGW(__func__, "ignore stale baidu audio cancel: request_id=%s current=%s",
             request_id ? request_id : "", g_baidu_request_id);
    }
    pthread_mutex_unlock(&g_baidu_tts_mutex);
    g_music_stream_running = 0;
}

int zh_music_player_is_waiting_baidu_tts(void) {
    int waiting;
    pthread_mutex_lock(&g_baidu_tts_mutex);
    waiting = g_baidu_waiting_tts;
    pthread_mutex_unlock(&g_baidu_tts_mutex);
    return waiting;
}

static int zh_music_is_url(const char *s) {
    if (!s) return 0;
    return (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static int16_t zh_music_apply_gain_sample(int16_t sample, float gain) {
    int32_t v = (int32_t)((float)sample * gain);
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static size_t zh_music_resample_linear(const int16_t *in_pcm, size_t in_frames,
                                       unsigned int in_rate, int channels,
                                       int16_t *out_pcm, size_t out_cap_frames,
                                       unsigned int out_rate) {
    if (!in_pcm || !out_pcm || in_frames == 0 || out_cap_frames == 0 || channels <= 0 ||
        in_rate == 0 || out_rate == 0) {
        return 0;
    }
    if (in_rate == out_rate) {
        size_t n = in_frames < out_cap_frames ? in_frames : out_cap_frames;
        memcpy(out_pcm, in_pcm, n * (size_t)channels * sizeof(int16_t));
        return n;
    }

    const double step = (double)in_rate / (double)out_rate;
    double pos = 0.0;
    size_t out_frames = 0;
    while (out_frames < out_cap_frames) {
        size_t i0 = (size_t)pos;
        if (i0 >= in_frames) {
            break;
        }
        size_t i1 = (i0 + 1 < in_frames) ? (i0 + 1) : i0;
        double frac = pos - (double)i0;
        for (int ch = 0; ch < channels; ++ch) {
            int32_t s0 = in_pcm[i0 * (size_t)channels + (size_t)ch];
            int32_t s1 = in_pcm[i1 * (size_t)channels + (size_t)ch];
            int32_t mixed = (int32_t)((1.0 - frac) * (double)s0 + frac * (double)s1);
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out_pcm[out_frames * (size_t)channels + (size_t)ch] = (int16_t)mixed;
        }
        out_frames++;
        pos += step;
    }
    return out_frames;
}

static void zh_music_decoder_reset(zh_music_decoder_t *d) {
    if (!d) return;
    if (d->play) {
        zh_ao_playback_close(d->play);
        d->play = NULL;
    }
    if (d->buf) {
        free(d->buf);
        d->buf = NULL;
    }
    if (d->resample_pcm) {
        free(d->resample_pcm);
        d->resample_pcm = NULL;
    }
    d->len = 0;
    d->cap = 0;
    d->resample_cap_frames = 0;
    d->opened = 0;
    d->play_rate = 0;
    d->channels = 0;
}

#define ZH_MUSIC_PCM_OPEN_RETRY_MS 200
#define ZH_MUSIC_PCM_OPEN_TIMEOUT_MS 10000

static int zh_music_decoder_open(zh_music_decoder_t *d, int ch, unsigned int rate,
                                 uint64_t token) {
    if (!d) return -1;
    int tries = ZH_MUSIC_PCM_OPEN_TIMEOUT_MS / ZH_MUSIC_PCM_OPEN_RETRY_MS;
    while (tries-- > 0) {
        if (zh_music_should_abort(token)) {
            return -1;
        }
        if (zh_ao_playback_open(&d->play, rate, (unsigned int)ch) == 0) {
            d->opened = 1;
            return 0;
        }
        usleep(ZH_MUSIC_PCM_OPEN_RETRY_MS * 1000);
    }
    LOGE(__func__, "ao playback open failed");
    return -1;
}

static int zh_music_decoder_feed(zh_music_decoder_t *d, const uint8_t *data, size_t len,
                                 uint64_t token, float gain) {
    if (!d || !data || len == 0) return 0;
    if (d->len + len > d->cap) {
        size_t new_cap = d->cap == 0 ? 8192 : d->cap * 2;
        while (new_cap < d->len + len) {
            new_cap *= 2;
        }
        uint8_t *tmp = (uint8_t *)realloc(d->buf, new_cap);
        if (!tmp) return -1;
        d->buf = tmp;
        d->cap = new_cap;
    }
    memcpy(d->buf + d->len, data, len);
    d->len += len;

    size_t offset = 0;
    mp3dec_frame_info_t info;
    while (offset < d->len) {
        if (zh_music_should_abort(token)) {
            return -1;
        }
        int samples = mp3dec_decode_frame(&d->dec, d->buf + offset, d->len - offset,
                                          d->pcm, &info);
        if (info.frame_bytes == 0) {
            break;
        }
        offset += (size_t)info.frame_bytes;
        if (samples <= 0) {
            continue;
        }
        int ch = info.channels > 0 ? info.channels : 1;
        unsigned int rate = info.hz > 0 ? (unsigned int)info.hz : ZH_TTS_SAMPLE_RATE;
        unsigned int out_rate = (rate == ZH_TTS_SAMPLE_RATE) ? rate : ZH_TTS_SAMPLE_RATE;
        if (!d->opened) {
            if (zh_music_decoder_open(d, ch, out_rate, token) != 0) {
                return -1;
            }
            d->play_rate = out_rate;
            d->channels = ch;
        }

        const int16_t *write_pcm = d->pcm;
        size_t write_frames = (size_t)samples;
        if (rate != d->play_rate) {
            size_t need_frames =
                ((size_t)samples * (size_t)d->play_rate + (size_t)rate - 1) / (size_t)rate + 2;
            if (need_frames > d->resample_cap_frames) {
                int16_t *tmp = (int16_t *)realloc(
                    d->resample_pcm, need_frames * (size_t)d->channels * sizeof(int16_t));
                if (!tmp) {
                    return -1;
                }
                d->resample_pcm = tmp;
                d->resample_cap_frames = need_frames;
            }
            write_frames = zh_music_resample_linear(d->pcm, (size_t)samples, rate, d->channels,
                                                    d->resample_pcm, d->resample_cap_frames,
                                                    d->play_rate);
            write_pcm = d->resample_pcm;
            if (write_frames == 0) {
                continue;
            }
        }
        if (gain != 1.0f) {
            int16_t *mutable_pcm = (int16_t *)write_pcm;
            size_t total = write_frames * (size_t)d->channels;
            for (size_t i = 0; i < total; ++i) {
                mutable_pcm[i] = zh_music_apply_gain_sample(mutable_pcm[i], gain);
            }
        }

        if (zh_music_play_pcm(d->play, write_pcm, write_frames, token) != 0) {
            return -1;
        }
    }

    if (offset > 0) {
        size_t remain = d->len - offset;
        if (remain > 0) {
            memmove(d->buf, d->buf + offset, remain);
        }
        d->len = remain;
    }
    return 0;
}

static void zh_music_decoder_finish(zh_music_decoder_t *d, int interrupted) {
    if (!d) return;
    if (d->play && !interrupted) {
        zh_ao_playback_drain(d->play);
    }
    if (d->play && interrupted) {
        zh_ao_playback_flush(d->play);
    }
    zh_music_decoder_reset(d);
}

static int zh_music_http_stream_on_data(void *userdata, const void *data, size_t len) {
    zh_music_stream_feed_ctx_t *ctx = (zh_music_stream_feed_ctx_t *)userdata;

    if (!ctx || !ctx->decoder || !data) {
        return -1;
    }
    return zh_music_decoder_feed(ctx->decoder, (const uint8_t *)data, len, ctx->token, ctx->gain);
}

static int zh_music_http_stream_play(const char *url, uint64_t token, float gain) {
    zh_music_decoder_t dec;
    zh_music_stream_feed_ctx_t ctx;
    int status = 0;
    int rc = -1;
    int interrupted = 0;

    if (!url) {
        return -1;
    }
    if (zh_music_should_abort(token)) {
        return 0;
    }

    memset(&dec, 0, sizeof(dec));
    memset(&ctx, 0, sizeof(ctx));
    ctx.decoder = &dec;
    ctx.token = token;
    ctx.gain = gain;
    mp3dec_init(&dec.dec);

    rc = bithion_core_http_get_stream(url,
                                      1,
                                      zh_music_http_stream_on_data,
                                      &ctx,
                                      &g_music_stream_running,
                                      &status);
    interrupted = zh_music_should_abort(token) || !g_music_stream_running;
    zh_music_decoder_finish(&dec, interrupted);

    if (interrupted) {
        LOGI(__func__, "music stream interrupted: %s", url);
        return 0;
    }
    if (rc != 0) {
        return -1;
    }
    if (status != 200) {
        LOGE(__func__, "music http status=%d url=%s", status, url);
        return -1;
    }
    return 0;
}

static int zh_baidu_base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int zh_baidu_decode_base64(const char *src, size_t len, uint8_t **out, size_t *out_len) {
    uint8_t *dst;
    size_t di = 0;
    int q[4];
    if (!src || !out || !out_len || len == 0 || len % 4 != 0) return -1;
    dst = (uint8_t *)malloc((len / 4) * 3 + 1);
    if (!dst) return -1;
    for (size_t i = 0; i < len; i += 4) {
        for (int j = 0; j < 4; ++j) {
            q[j] = src[i + (size_t)j] == '=' ? -2 : zh_baidu_base64_value((unsigned char)src[i + (size_t)j]);
            if (q[j] < 0 && q[j] != -2) { free(dst); return -1; }
        }
        if (q[0] < 0 || q[1] < 0 || (q[2] == -2 && q[3] != -2)) { free(dst); return -1; }
        dst[di++] = (uint8_t)((q[0] << 2) | (q[1] >> 4));
        if (q[2] != -2) {
            dst[di++] = (uint8_t)(((q[1] & 15) << 4) | (q[2] >> 2));
            if (q[3] != -2) dst[di++] = (uint8_t)(((q[2] & 3) << 6) | q[3]);
        }
    }
    *out = dst;
    *out_len = di;
    return 0;
}

static int zh_baidu_json_bool(bithion_core_json_view_t msg, const char *path, int dflt) {
    bithion_core_json_view_t tok = bithion_core_json_get_tok(msg, path);
    if (!tok.buf) return dflt;
    if (tok.len == 4 && memcmp(tok.buf, "true", 4) == 0) return 1;
    if (tok.len == 5 && memcmp(tok.buf, "false", 5) == 0) return 0;
    return bithion_core_json_get_long(msg, path, dflt) ? 1 : 0;
}

static int zh_baidu_report_result(zh_baidu_stream_ctx_t *ctx) {
    if (!ctx || ctx->result_reported || !ctx->resource_json) return 0;
    if (zh_ws_report_baidu_audio_result(ctx->request_id,
                                        ctx->answer ? ctx->answer : "",
                                        ctx->resource_json) != 0) {
        LOGE(__func__, "baidu audio result report failed: request_id=%s", ctx->request_id);
        return -1;
    }
    ctx->result_reported = 1;
    LOGI(__func__, "baidu audio result reported: request_id=%s answer=%s resource=%s",
         ctx->request_id, ctx->answer ? ctx->answer : "",
         ctx->resource_json ? ctx->resource_json : "null");
    return 0;
}

static int zh_baidu_play_audio(zh_baidu_stream_ctx_t *ctx, uint8_t *audio,
                               size_t audio_len, float gain) {
    if (ctx->format_pcm16) {
        size_t frame_bytes = (size_t)ctx->channels * sizeof(int16_t);
        if (frame_bytes == 0 || audio_len % frame_bytes != 0) return -1;
        if (!ctx->pcm_play &&
            zh_ao_playback_open(&ctx->pcm_play, ctx->sample_rate, ctx->channels) != 0) return -1;
        int16_t *pcm = (int16_t *)audio;
        size_t samples = audio_len / sizeof(int16_t);
        if (gain != 1.0f) {
            for (size_t i = 0; i < samples; ++i) {
                pcm[i] = zh_music_apply_gain_sample(pcm[i], gain);
            }
        }
        return zh_music_play_pcm(ctx->pcm_play, pcm, audio_len / frame_bytes, ctx->token);
    }
    if (ctx->format_mp3) {
        return zh_music_decoder_feed(&ctx->decoder, audio, audio_len, ctx->token, gain);
    }
    LOGE(__func__, "audio.content received before supported audio.header");
    return -1;
}

static int zh_baidu_queue_or_play_audio(zh_baidu_stream_ctx_t *ctx, uint8_t *audio,
                                        size_t audio_len, float gain) {
    if (!zh_baidu_tts_is_done(ctx->request_id)) {
        if (ctx->pending_audio_len + audio_len > ctx->pending_audio_cap) {
            size_t cap = ctx->pending_audio_cap ? ctx->pending_audio_cap : 65536;
            while (cap < ctx->pending_audio_len + audio_len) cap *= 2;
            uint8_t *tmp = (uint8_t *)realloc(ctx->pending_audio, cap);
            if (!tmp) return -1;
            ctx->pending_audio = tmp;
            ctx->pending_audio_cap = cap;
        }
        memcpy(ctx->pending_audio + ctx->pending_audio_len, audio, audio_len);
        ctx->pending_audio_len += audio_len;
        return 0;
    }
    if (ctx->pending_audio_len > 0) {
        if (zh_baidu_play_audio(ctx, ctx->pending_audio, ctx->pending_audio_len, gain) != 0) return -1;
        LOGI(__func__, "baidu audio buffered data released: request_id=%s bytes=%zu",
             ctx->request_id, ctx->pending_audio_len);
        ctx->pending_audio_len = 0;
    }
    return zh_baidu_play_audio(ctx, audio, audio_len, gain);
}

static int zh_baidu_process_event(zh_baidu_stream_ctx_t *ctx, const char *line, size_t len, float gain) {
    bithion_core_json_view_t msg;
    char *type = NULL;
    char *encoded = NULL;
    uint8_t *audio = NULL;
    size_t audio_len = 0;
    int rc = 0;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    if (len < 5 || memcmp(line, "data:", 5) != 0) return 0;
    line += 5; len -= 5;
    if (len > 0 && *line == ' ') { line++; len--; }
    if (len == 6 && memcmp(line, "[DONE]", 6) == 0) {
        ctx->done = 1;
        if (zh_baidu_report_result(ctx) != 0) return -1;
        LOGI(__func__,
             "baidu audio response event: [DONE] events=%zu audio_chunks=%zu base64_chars=%zu audio_bytes=%zu",
             ctx->response_events, ctx->audio_chunks, ctx->audio_base64_chars, ctx->audio_bytes);
        return 0;
    }
    msg = bithion_core_json_view_make(line, len);
    type = bithion_core_json_get_str(msg, "$.type");
    if (!type) return 0;
    ctx->response_events++;
    if (strcmp(type, "audio.content") != 0) {
        LOGI(__func__, "baidu audio response event: %.*s", (int)len, line);
    }
    if (strcmp(type, "audio.header") == 0) {
        char *format = bithion_core_json_get_str(msg, "$.data.format");
        long long rate = bithion_core_json_get_long(msg, "$.data.sample_rate", 0);
        long long channels = bithion_core_json_get_long(msg, "$.data.channel", 0);
        ctx->sample_rate = (rate > 0 && rate <= 192000) ? (unsigned int)rate : 0;
        ctx->channels = (channels > 0 && channels <= 8) ? (unsigned int)channels : 0;
        ctx->format_pcm16 = format && (strcmp(format, "pcm16") == 0 || strcmp(format, "pcm") == 0 || strcmp(format, "s16le") == 0);
        ctx->format_mp3 = format && strcmp(format, "mp3") == 0;
        LOGI(__func__, "baidu audio header: format=%s rate=%u channels=%u",
             format ? format : "", ctx->sample_rate, ctx->channels);
        free(format);
        if ((!ctx->format_pcm16 && !ctx->format_mp3) || ctx->sample_rate == 0 || ctx->channels == 0) rc = -1;
    } else if (strcmp(type, "resource.url") == 0) {
        free(ctx->resource_url);
        ctx->resource_url = bithion_core_json_get_str(msg, "$.data.text");
        LOGI(__func__, "baidu audio resource.url: %s",
             ctx->resource_url ? ctx->resource_url : "(null)");
    } else if (strcmp(type, "atomic.content.result") == 0) {
        bithion_core_json_view_t resource = bithion_core_json_get_tok(msg, "$.data");
        char *track_id = bithion_core_json_get_str(msg, "$.data.trackId");
        char *music_name = bithion_core_json_get_str(msg, "$.data.musicName");
        free(ctx->resource_json);
        ctx->resource_json = resource.buf ? strndup(resource.buf, resource.len) : NULL;
        ctx->resource_match = zh_baidu_json_bool(msg, "$.data.match", 0);
        ctx->resource_match_known = 1;
        ctx->resource_has_candidate = (track_id && track_id[0]) ||
                                      (music_name && music_name[0]);
        LOGI(__func__, "baidu audio resource decision: match=%d candidate=%d",
             ctx->resource_match, ctx->resource_has_candidate);
        free(track_id);
        free(music_name);
    } else if (strcmp(type, "answer") == 0) {
        free(ctx->answer);
        ctx->answer = bithion_core_json_get_str(msg, "$.data.text");
        if (zh_baidu_report_result(ctx) != 0) rc = -1;
    } else if (strcmp(type, "audio.content") == 0) {
        encoded = bithion_core_json_get_str(msg, "$.data.base64Audio");
        if (!encoded || zh_baidu_decode_base64(encoded, strlen(encoded), &audio, &audio_len) != 0) {
            LOGE(__func__, "invalid baidu audio.content");
            rc = -1;
        } else {
            ctx->audio_chunks++;
            ctx->audio_base64_chars += strlen(encoded);
            if ((!ctx->resource_match_known || ctx->resource_match ||
                 ctx->resource_has_candidate) &&
                zh_baidu_queue_or_play_audio(ctx, audio, audio_len, gain) != 0) rc = -1;
            if (rc == 0) ctx->audio_bytes += audio_len;
        }
    } else if (strcmp(type, "system.error") == 0 || strcmp(type, "error") == 0) {
        LOGE(__func__, "baidu audio error event");
        rc = -1;
    }
    free(audio);
    free(encoded);
    free(type);
    return rc;
}

static int zh_baidu_stream_on_data(void *userdata, const void *data, size_t len) {
    zh_baidu_stream_ctx_t *ctx = (zh_baidu_stream_ctx_t *)userdata;
    size_t start = 0;
    if (!ctx || !data || len == 0) return 0;
    if (ctx->len + len > ZH_BAIDU_SSE_MAX_EVENT) return -1;
    if (ctx->len + len + 1 > ctx->cap) {
        size_t cap = ctx->cap ? ctx->cap : 8192;
        while (cap < ctx->len + len + 1) cap *= 2;
        char *tmp = (char *)realloc(ctx->buf, cap);
        if (!tmp) return -1;
        ctx->buf = tmp; ctx->cap = cap;
    }
    memcpy(ctx->buf + ctx->len, data, len);
    ctx->len += len; ctx->buf[ctx->len] = '\0';
    for (size_t i = 0; i < ctx->len; ++i) {
        if (ctx->buf[i] == '\n') {
            if (zh_baidu_process_event(ctx, ctx->buf + start, i - start + 1, ctx->gain) != 0) return -1;
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(ctx->buf, ctx->buf + start, ctx->len - start);
        ctx->len -= start;
    }
    return 0;
}

static int zh_music_baidu_play(const char *query, const char *tag, uint64_t token, float gain) {
    zh_baidu_stream_ctx_t ctx;
    char query_esc[1024];
    char tag_esc[256];
    char body[1536];
    const char *auth = getenv("BAIDU_AUDIO_TOKEN");
    char authorization[1024];
    int status = 0;
    int rc;
    int interrupted;
    if (!query || bithion_core_json_escape(query, query_esc, sizeof(query_esc)) != 0 ||
        bithion_core_json_escape(tag ? tag : "", tag_esc, sizeof(tag_esc)) != 0) return -1;
    if (snprintf(body, sizeof(body),
        "{\"messages\":[{\"contentItems\":[{\"type\":\"text\",\"text\":\"%s\"}]}],"
        "\"metadata\":{\"tag\":\"%s\",\"mp3_flag\":false},"
        "\"response_format\":{\"text_stream\":true,\"enable_tts\":false,\"resource_type\":\"audio\"}}",
        query_esc, tag_esc) >= (int)sizeof(body)) return -1;
    authorization[0] = '\0';
    if (auth && auth[0]) snprintf(authorization, sizeof(authorization), "Bearer %s", auth);
    LOGI(__func__, "baidu audio request: url=%s authorization=%s gain=%.3f body=%s",
         ZH_BAIDU_AUDIO_URL, authorization[0] ? "present" : "absent", gain, body);
    memset(&ctx, 0, sizeof(ctx));
    ctx.token = token;
    ctx.gain = gain;
    snprintf(ctx.request_id, sizeof(ctx.request_id), "%llu-%llu",
             (unsigned long long)time(NULL), (unsigned long long)token);
    pthread_mutex_lock(&g_baidu_tts_mutex);
    snprintf(g_baidu_request_id, sizeof(g_baidu_request_id), "%s", ctx.request_id);
    g_baidu_tts_done = 0;
    g_baidu_waiting_tts = 1;
    pthread_mutex_unlock(&g_baidu_tts_mutex);
    mp3dec_init(&ctx.decoder.dec);
    rc = bithion_core_http_post_stream(ZH_BAIDU_AUDIO_URL, "application/json; charset=utf-8",
        "text/event-stream", authorization[0] ? authorization : NULL, body, strlen(body), 1,
        zh_baidu_stream_on_data, &ctx, &g_music_stream_running, &status);
    LOGI(__func__,
         "baidu audio response summary: rc=%d http=%d events=%zu audio_chunks=%zu base64_chars=%zu audio_bytes=%zu done=%d",
         rc, status, ctx.response_events, ctx.audio_chunks, ctx.audio_base64_chars,
         ctx.audio_bytes, ctx.done);
    if (rc == 0 && ctx.pending_audio_len > 0 &&
        (!ctx.resource_match_known || ctx.resource_match ||
         ctx.resource_has_candidate)) {
        int wait_ms = 0;
        while (!zh_baidu_tts_is_done(ctx.request_id) && wait_ms < 10000 &&
               !zh_music_should_abort(token) && !g_baidu_cancelled) {
            usleep(50000);
            wait_ms += 50;
        }
        if (g_baidu_cancelled) {
            LOGI(__func__, "baidu audio pending playback cancelled: request_id=%s",
                 ctx.request_id);
            rc = -1;
        } else if (!zh_baidu_tts_is_done(ctx.request_id)) {
            LOGE(__func__, "baidu audio TTS wait timeout: request_id=%s", ctx.request_id);
            rc = -1;
        } else if (zh_baidu_play_audio(&ctx, ctx.pending_audio, ctx.pending_audio_len, gain) != 0) {
            rc = -1;
        } else {
            ctx.pending_audio_len = 0;
        }
    }
    if (rc == 0 && ctx.audio_bytes == 0 && ctx.resource_url && ctx.resource_url[0] &&
        (!ctx.resource_match_known || ctx.resource_match ||
         ctx.resource_has_candidate)) {
        int wait_ms = 0;
        while (!zh_baidu_tts_is_done(ctx.request_id) && wait_ms < 10000 &&
               !zh_music_should_abort(token) && !g_baidu_cancelled) {
            usleep(50000);
            wait_ms += 50;
        }
        if (g_baidu_cancelled) {
            LOGI(__func__, "baidu audio resource.url playback cancelled: request_id=%s",
                 ctx.request_id);
            rc = -1;
        } else if (!zh_baidu_tts_is_done(ctx.request_id)) {
            LOGE(__func__, "baidu audio resource.url TTS wait timeout: request_id=%s",
                 ctx.request_id);
            rc = -1;
        } else {
            LOGI(__func__, "baidu audio resource.url play: %s", ctx.resource_url);
            rc = zh_music_http_stream_play(ctx.resource_url, token, gain);
            if (rc == 0) {
                ctx.audio_bytes = 1;
                ctx.done = 1;
            }
        }
    }
    interrupted = zh_music_should_abort(token) || !g_music_stream_running;
    zh_music_decoder_finish(&ctx.decoder, interrupted);
    if (ctx.pcm_play && !interrupted) zh_ao_playback_drain(ctx.pcm_play);
    if (ctx.pcm_play && interrupted) zh_ao_playback_flush(ctx.pcm_play);
    if (ctx.pcm_play) zh_ao_playback_close(ctx.pcm_play);
    free(ctx.buf);
    free(ctx.pending_audio);
    free(ctx.answer);
    free(ctx.resource_json);
    free(ctx.resource_url);
    pthread_mutex_lock(&g_baidu_tts_mutex);
    if (strcmp(g_baidu_request_id, ctx.request_id) == 0) {
        g_baidu_waiting_tts = 0;
        g_baidu_cancelled = 0;
    }
    pthread_mutex_unlock(&g_baidu_tts_mutex);
    if (interrupted) return 0;
    if (ctx.resource_match_known && !ctx.resource_match &&
        !ctx.resource_has_candidate && rc == 0 && status == 200 && ctx.done) {
        LOGI(__func__, "baidu audio no playable resource; answer TTS only");
        return 0;
    }
    if (rc != 0 || status != 200 || ctx.audio_bytes == 0 || !ctx.done) {
        LOGE(__func__, "baidu audio failed: http=%d bytes=%zu done=%d", status, ctx.audio_bytes, ctx.done);
        return -1;
    }
    LOGI(__func__, "baidu audio completed: bytes=%zu", ctx.audio_bytes);
    return 0;
}

#define ZH_MUSIC_PCM_CHUNK_FRAMES 1024

static int zh_music_play_pcm(zh_ao_playback_t *pb, const int16_t *pcm_data,
                             size_t frames, uint64_t token) {
    size_t offset = 0;
    if (!pb || !pcm_data || frames == 0) {
        return -1;
    }
    while (offset < frames) {
        if (zh_music_should_abort(token)) {
            return -1;
        }
        size_t chunk = frames - offset;
        if (chunk > ZH_MUSIC_PCM_CHUNK_FRAMES) {
            chunk = ZH_MUSIC_PCM_CHUNK_FRAMES;
        }
        if (zh_ao_playback_write(pb, pcm_data + offset, chunk) != 0) {
            return -1;
        }
        offset += chunk;
    }
    return 0;
}

static int zh_music_play_mp3(const uint8_t *data, size_t len, uint64_t token, float gain) {
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t offset = 0;
    zh_ao_playback_t *play = NULL;
    int opened = 0;
    size_t decoded_frames = 0;
    int first_frame_logged = 0;

    if (!data || len == 0) {
        return -1;
    }

    mp3dec_init(&dec);
    while (offset < len) {
        int samples = mp3dec_decode_frame(&dec, data + offset, len - offset, pcm, &info);
        if (info.frame_bytes == 0) {
            break;
        }
        offset += (size_t)info.frame_bytes;
        if (samples <= 0) {
            continue;
        }
        int ch = info.channels > 0 ? info.channels : 1;
        if (!first_frame_logged) {
            LOGI(__func__, "mp3 first frame: hz=%d ch=%d samples=%d",
                 info.hz, ch, samples);
            first_frame_logged = 1;
        }
        if (!opened) {
            unsigned int rate = info.hz > 0 ? (unsigned int)info.hz : ZH_TTS_SAMPLE_RATE;
            if (zh_ao_playback_open(&play, rate, (unsigned int)ch) != 0) {
                LOGE(__func__, "ao playback open failed");
                return -1;
            }
            opened = 1;
        }
        if (gain != 1.0f) {
            size_t total = (size_t)samples * (size_t)ch;
            for (size_t i = 0; i < total; ++i) {
                pcm[i] = zh_music_apply_gain_sample(pcm[i], gain);
            }
        }
        if (zh_music_play_pcm(play, pcm, (size_t)samples, token) != 0) {
            break;
        }
        decoded_frames += (size_t)samples;
    }

    if (decoded_frames == 0) {
        LOGE(__func__, "mp3 decode got zero pcm frames");
        if (play) {
            zh_ao_playback_close(play);
        }
        return -1;
    }

    if (play) {
        zh_ao_playback_drain(play);
        zh_ao_playback_close(play);
    }
    LOGI(__func__, "mp3 play done: decoded_frames=%zu", decoded_frames);
    return 0;
}

static int zh_music_play_local_file(const char *path, uint64_t token, float gain) {
    FILE *fp = NULL;
    uint8_t buf[4096];
    zh_music_decoder_t dec;
    size_t n = 0;
    int rc = -1;
    int interrupted = 0;

    if (!path || path[0] == '\0') {
        return -1;
    }
    if (zh_music_should_abort(token)) {
        return 0;
    }
    if (access(path, R_OK) != 0) {
        LOGE(__func__, "local mp3 not readable: %s", path);
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        LOGE(__func__, "open local mp3 failed: %s", path);
        return -1;
    }
    memset(&dec, 0, sizeof(dec));
    mp3dec_init(&dec.dec);
    LOGI(__func__, "music local file: %s", path);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (zh_music_should_abort(token)) {
            interrupted = 1;
            break;
        }
        if (zh_music_decoder_feed(&dec, buf, n, token, gain) != 0) {
            interrupted = zh_music_should_abort(token);
            goto done;
        }
    }
    if (ferror(fp)) {
        goto done;
    }
    rc = 0;

done:
    if (interrupted) {
        rc = 0;
    }
    zh_music_decoder_finish(&dec, interrupted);
    if (fp) fclose(fp);
    if (interrupted) {
        LOGI(__func__, "music local file interrupted: %s", path);
    } else if (rc != 0) {
        LOGE(__func__, "music local file play failed: %s", path);
    }
    return rc;
}

static void *zh_music_thread_main(void *arg) {
    (void)arg;
    while (1) {
        zh_music_item_t *item = NULL;
        uint64_t token = 0;

        pthread_mutex_lock(&g_music.mutex);
        while (!g_music.stop && !g_music.head) {
            if (g_music.pending_head) {
                g_music.head = g_music.pending_head;
                g_music.pending_head = g_music.pending_head->next;
                if (!g_music.pending_head) g_music.pending_tail = NULL;
                g_music.head->next = NULL;
                g_music.tail = g_music.head;
                break;
            }
            pthread_cond_wait(&g_music.cond, &g_music.mutex);
        }
        if (g_music.stop) {
            pthread_mutex_unlock(&g_music.mutex);
            break;
        }
        item = g_music.head;
        if (item) {
            g_music.head = item->next;
            if (!g_music.head) g_music.tail = NULL;
        }
        token = g_music.token;
        g_music.playing = item ? 1 : 0;
        g_music_stream_running = item && (item->baidu_audio || zh_music_is_url(item->url));
        pthread_mutex_unlock(&g_music.mutex);

        if (!item) {
            continue;
        }

        if (!zh_music_should_abort(token)) {
            if (item->baidu_audio) {
                LOGI(__func__, "baidu audio query: %s", item->url);
                if (zh_music_baidu_play(item->url, item->tag, token, item->gain) != 0) {
                    LOGE(__func__, "baidu audio play failed");
                }
            } else if (zh_music_is_url(item->url)) {
                LOGI(__func__, "music stream: %s", item->url);
                if (zh_music_http_stream_play(item->url, token, item->gain) != 0) {
                    LOGE(__func__, "music fetch failed: %s", item->url);
                }
            } else if (zh_music_play_local_file(item->url, token, item->gain) != 0) {
                LOGE(__func__, "music play failed: %s", item->url);
            }
        }

        free(item->url);
        free(item->tag);
        free(item);

        pthread_mutex_lock(&g_music.mutex);
        g_music_stream_running = 0;
        if (!g_music.head) {
            if (g_music.pending_head) {
                g_music.head = g_music.pending_head;
                g_music.pending_head = g_music.pending_head->next;
                if (!g_music.pending_head) g_music.pending_tail = NULL;
                g_music.head->next = NULL;
                g_music.tail = g_music.head;
                pthread_cond_broadcast(&g_music.cond);
            }
            g_music.playing = 0;
            pthread_cond_broadcast(&g_music.cond);
        }
        pthread_mutex_unlock(&g_music.mutex);
    }
    return NULL;
}

int zh_music_player_start(void) {
    pthread_mutex_lock(&g_music.mutex);
    if (g_music.running) {
        pthread_mutex_unlock(&g_music.mutex);
        return 0;
    }
    g_music.stop = 0;
    g_music.running = 1;
    pthread_mutex_unlock(&g_music.mutex);

    int err = pthread_create(&g_music.thread, NULL, zh_music_thread_main, NULL);
    if (err != 0) {
        pthread_mutex_lock(&g_music.mutex);
        g_music.running = 0;
        pthread_mutex_unlock(&g_music.mutex);
        errno = err;
        return -1;
    }
    pthread_detach(g_music.thread);
    return 0;
}

void zh_music_player_stop(void) {
    pthread_mutex_lock(&g_music.mutex);
    g_music.stop = 1;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

void zh_music_player_play_urls_with_gain(const char **urls, size_t count, float gain) {
    if (!urls || count == 0) return;
    if (gain < 0.0f) gain = 0.0f;

    pthread_mutex_lock(&g_music.mutex);
    g_music.token++;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    for (size_t i = 0; i < count; ++i) {
        const char *u = urls[i];
        if (!u || !u[0]) continue;
        zh_music_item_t *item = (zh_music_item_t *)calloc(1, sizeof(*item));
        if (!item) continue;
        item->url = strdup(u);
        if (!item->url) {
            free(item);
            continue;
        }
        item->gain = gain;
        if (i == 0 && !g_music.head) {
            g_music.head = g_music.tail = item;
        } else {
            if (!g_music.pending_tail) {
                g_music.pending_head = g_music.pending_tail = item;
            } else {
                g_music.pending_tail->next = item;
                g_music.pending_tail = item;
            }
        }
    }
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

void zh_music_player_play_urls(const char **urls, size_t count) {
    zh_music_player_play_urls_with_gain(urls, count, ZH_MUSIC_GAIN);
}

void zh_music_player_play_baidu_query(const char *query, const char *tag) {
    zh_music_item_t *item;
    if (!query || !query[0]) return;
    item = (zh_music_item_t *)calloc(1, sizeof(*item));
    if (!item) return;
    item->url = strdup(query);
    item->tag = strdup(tag ? tag : "");
    item->gain = ZH_MUSIC_GAIN;
    item->baidu_audio = 1;
    if (!item->url || !item->tag) { free(item->url); free(item->tag); free(item); return; }
    pthread_mutex_lock(&g_music.mutex);
    g_music.token++;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    g_music.head = g_music.tail = item;
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

void zh_music_player_interrupt(void) {
    pthread_mutex_lock(&g_music.mutex);
    g_music.token++;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

int zh_music_player_is_active(void) {
    int active = 0;
    pthread_mutex_lock(&g_music.mutex);
    active = g_music.playing || g_music.head != NULL || g_music.pending_head != NULL;
    pthread_mutex_unlock(&g_music.mutex);
    return active;
}
