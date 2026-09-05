#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bithion_core_bridge.h"
#include "bithion_core.h"
#include "config.h"
#include "core_json_compat.h"
#include "face_recognition.h"
#include "http.h"
#include "log.h"
#include "music_player.h"
#include "prompt_tone.h"
#include "udp_tts.h"
#include "utils.h"
#include "vision_capture.h"
#include "ws.h"

struct zh_ws_session {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool authenticated;
    bool should_stop;
    bool stt_completed;
    bool stt_completed_chat_valid;
    uint32_t stt_completed_chat_count;
    bool tts_completed;
    bool vad_started;
    int vision_upload_ready;
    int vision_done_ready;
    int fortune_vision_result_ready;
    zh_ws_vision_upload_response_t vision_upload_resp;
    zh_ws_vision_done_t vision_done;
    zh_ws_fortune_vision_result_t fortune_vision_result;
    char **pending_music_urls;
    size_t pending_music_count;
    bool web_search_wait_pending;
    bool web_search_wait_thread_running;
};

static bithion_core_t *g_core = NULL;
static zh_config_t g_core_cfg;
static struct zh_ws_session g_ws_session;
static pthread_once_t g_ws_session_once = PTHREAD_ONCE_INIT;

struct zh_core_vad {
    bithion_core_vad_t *impl;
};

// 初始化全局 WS 会话使用的互斥锁与条件变量。
static void zh_ws_session_init_once(void) {
    pthread_mutex_init(&g_ws_session.mutex, NULL);
    pthread_cond_init(&g_ws_session.cond, NULL);
}

// 安全地将字符串复制到固定长度缓冲区。
static void zh_copy_str(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static void zh_json_escape_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (bithion_core_json_escape(src, dst, dst_len) != 0) {
        dst[0] = '\0';
    }
}

static long long zh_json_get_bool_as_long(bithion_core_json_view_t msg,
                                          const char *path,
                                          long long dflt) {
    bithion_core_json_view_t tok = mg_json_get_tok(msg, path);

    if (!tok.buf || tok.len == 0) {
        return dflt;
    }
    if (tok.len == 4 && memcmp(tok.buf, "true", 4) == 0) {
        return 1;
    }
    if (tok.len == 5 && memcmp(tok.buf, "false", 5) == 0) {
        return 0;
    }
    return mg_json_get_long(msg, path, dflt);
}

static int zh_ws_build_fortune_url(const char *src, char *out, size_t out_len) {
    const char *path = NULL;
    const char *query = NULL;
    size_t prefix_len = 0;
    int n = 0;

    if (!src || src[0] == '\0' || !out || out_len == 0) {
        return -1;
    }

    path = strstr(src, "/ws/");
    if (!path) {
        path = strchr(strstr(src, "://") ? strstr(src, "://") + 3 : src, '/');
    }
    if (!path) {
        n = snprintf(out, out_len, "%s/ws/fortune", src);
        return (n >= 0 && (size_t)n < out_len) ? 0 : -1;
    }

    query = strchr(path, '?');
    prefix_len = (size_t)(path - src);
    if (query) {
        n = snprintf(out, out_len, "%.*s/ws/fortune%s", (int)prefix_len, src, query);
    } else {
        n = snprintf(out, out_len, "%.*s/ws/fortune", (int)prefix_len, src);
    }
    return (n >= 0 && (size_t)n < out_len) ? 0 : -1;
}

// 释放一组由兼容层接管所有权的 URL 字符串数组。
static void zh_ws_free_url_list(char **urls, size_t count) {
    if (!urls) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(urls[i]);
    }
    free(urls);
}

// 在已持锁前提下清空会话中延迟播放的音乐列表。
static void zh_ws_clear_pending_music_locked(struct zh_ws_session *session) {
    if (!session) {
        return;
    }
    zh_ws_free_url_list(session->pending_music_urls, session->pending_music_count);
    session->pending_music_urls = NULL;
    session->pending_music_count = 0;
}

// 在已持锁前提下重置单轮对话相关状态。
static void zh_ws_reset_round_state_locked(struct zh_ws_session *session) {
    if (!session) {
        return;
    }
    session->stt_completed = false;
    session->stt_completed_chat_valid = false;
    session->stt_completed_chat_count = 0;
    session->tts_completed = false;
    session->vad_started = false;
}

// 重置兼容层维护的整份会话状态。
static void zh_ws_session_reset(struct zh_ws_session *session) {
    pthread_once(&g_ws_session_once, zh_ws_session_init_once);
    pthread_mutex_lock(&session->mutex);
    session->authenticated = false;
    session->should_stop = false;
    zh_ws_reset_round_state_locked(session);
    session->vision_upload_ready = 0;
    session->vision_done_ready = 0;
    session->fortune_vision_result_ready = 0;
    memset(&session->vision_upload_resp, 0, sizeof(session->vision_upload_resp));
    memset(&session->vision_done, 0, sizeof(session->vision_done));
    memset(&session->fortune_vision_result, 0, sizeof(session->fortune_vision_result));
    zh_ws_clear_pending_music_locked(session);
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
}

// 立即播放一组已经转移所有权的音乐 URL，并在结束后释放其内存。
static void zh_ws_play_music_urls_owned(char **urls, size_t count) {
    if (!urls || count == 0) {
        zh_ws_free_url_list(urls, count);
        return;
    }
    zh_music_player_play_urls((const char **)urls, count);
    zh_ws_free_url_list(urls, count);
}

// 取出会话中等待 TTS 结束后再播放的音乐 URL 列表。
static void zh_ws_take_pending_music(struct zh_ws_session *session, char ***out_urls, size_t *out_count) {
    char **urls = NULL;
    size_t count = 0;

    if (!session || !out_urls || !out_count) {
        return;
    }

    pthread_mutex_lock(&session->mutex);
    urls = session->pending_music_urls;
    count = session->pending_music_count;
    session->pending_music_urls = NULL;
    session->pending_music_count = 0;
    pthread_mutex_unlock(&session->mutex);

    *out_urls = urls;
    *out_count = count;
}

// 标记收到一次服务端 VAD_START，并返回是否需要执行首轮打断。
static bool zh_ws_mark_vad_start(struct zh_ws_session *session) {
    bool should_interrupt = false;

    if (!session) {
        return false;
    }

    pthread_mutex_lock(&session->mutex);
    if (!session->vad_started) {
        session->vad_started = true;
        session->stt_completed = false;
        session->stt_completed_chat_valid = false;
        session->stt_completed_chat_count = 0;
        session->tts_completed = false;
        should_interrupt = true;
    }
    pthread_mutex_unlock(&session->mutex);
    return should_interrupt;
}

static void *zh_ws_web_search_wait_thread_main(void *arg) {
    struct zh_ws_session *session = (struct zh_ws_session *)arg;

    while (session && zh_udp_tts_is_busy()) {
        bool pending = false;
        pthread_mutex_lock(&session->mutex);
        pending = session->web_search_wait_pending;
        pthread_mutex_unlock(&session->mutex);
        if (!pending) {
            break;
        }
        usleep(50 * 1000);
    }

    if (session) {
        bool should_start = false;
        pthread_mutex_lock(&session->mutex);
        should_start = session->web_search_wait_pending;
        session->web_search_wait_pending = false;
        session->web_search_wait_thread_running = false;
        pthread_mutex_unlock(&session->mutex);
        if (should_start) {
            LOGI(__func__, "tts playback idle, start web search wait tone");
            zh_prompt_tone_start_web_search_wait();
        }
    }
    return NULL;
}

static void zh_ws_start_web_search_wait_after_tts_idle(struct zh_ws_session *session) {
    pthread_t tid;
    int create_thread = 0;

    if (!session) {
        return;
    }

    if (!zh_udp_tts_is_busy()) {
        LOGI(__func__, "start web search wait tone");
        zh_prompt_tone_start_web_search_wait();
        return;
    }

    pthread_mutex_lock(&session->mutex);
    session->web_search_wait_pending = true;
    if (!session->web_search_wait_thread_running) {
        session->web_search_wait_thread_running = true;
        create_thread = 1;
    }
    pthread_mutex_unlock(&session->mutex);

    if (!create_thread) {
        LOGI(__func__, "web search wait tone already pending until tts idle");
        return;
    }

    if (pthread_create(&tid, NULL, zh_ws_web_search_wait_thread_main, session) != 0) {
        pthread_mutex_lock(&session->mutex);
        session->web_search_wait_pending = false;
        session->web_search_wait_thread_running = false;
        pthread_mutex_unlock(&session->mutex);
        LOGE(__func__, "web search wait thread create failed");
        return;
    }
    pthread_detach(tid);
    LOGI(__func__, "web search wait tone pending until tts idle");
}

// 从服务端 JSON 消息中抽取音乐 URL 数组。
static int zh_ws_collect_music_urls(bithion_core_json_view_t msg, char ***out_urls, size_t *out_count) {
    size_t count = 0;
    size_t capacity = 0;
    char **urls = NULL;

    if (!out_urls || !out_count) {
        return -1;
    }

    for (int i = 0;; ++i) {
        char path[32];
        char *url = NULL;

        snprintf(path, sizeof(path), "$.url[%d]", i);
        url = mg_json_get_str(msg, path);
        if (!url) {
            break;
        }
        if (count >= capacity) {
            size_t new_capacity = capacity == 0 ? 4 : capacity * 2;
            char **new_urls = (char **)realloc(urls, new_capacity * sizeof(char *));
            if (!new_urls) {
                free(url);
                break;
            }
            urls = new_urls;
            capacity = new_capacity;
        }
        urls[count++] = url;
    }

    *out_urls = urls;
    *out_count = count;
    return count > 0 ? 0 : -1;
}

// 处理 core 上报的通用事件并同步宿主会话状态。
static void zh_core_host_event_notify(void *userdata,
                                      bithion_core_event_t event,
                                      const char *detail) {
    struct zh_ws_session *session = (struct zh_ws_session *)userdata;

    if (!session) {
        return;
    }

    switch (event) {
    case BITHION_CORE_EVENT_WEB_SEARCH_WAIT_AUDIO_START:
        zh_ws_start_web_search_wait_after_tts_idle(session);
        return;
    case BITHION_CORE_EVENT_TTS_MISSING_UDP:
        LOGW(__func__, "%s",
             (detail && detail[0] != '\0')
                 ? detail
                 : "received STT_COMPLETED and TTS_COMPLETED without any UDP packet in between!!!");
        if (zh_music_player_start() == 0) {
            const char *urls[1] = { ZH_PROMPT_NET_DISCONNECTED_MP3_URL };
            zh_music_player_play_urls_with_gain(urls, 1, ZH_PROMPT_TONE_GAIN);
            int waited_ms = 0;
            while (zh_music_player_is_active() && waited_ms < 4000) {
                usleep(50 * 1000);
                waited_ms += 50;
            }
        }
        LOGE(__func__, "fatal: received BITHION_CORE_EVENT_TTS_MISSING_UDP, exiting");
        exit(1);
        return;
    case BITHION_CORE_EVENT_WS_ERROR:
    case BITHION_CORE_EVENT_WS_CLOSED: {
        int first_disconnect = 0;

        pthread_mutex_lock(&session->mutex);
        first_disconnect = !session->should_stop;
        session->should_stop = true;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mutex);
        if (!first_disconnect) {
            return;
        }
        zh_prompt_tone_play(ZH_PROMPT_TONE_NET_DISCONNECTED);
        {
            int waited_ms = 0;
            while (zh_music_player_is_active() && waited_ms < 4000) {
                usleep(50 * 1000);
                waited_ms += 50;
            }
        }
        LOGE(__func__, "fatal: received %s, exiting after net disconnect prompt",
             event == BITHION_CORE_EVENT_WS_ERROR ? "BITHION_CORE_EVENT_WS_ERROR"
                                                  : "BITHION_CORE_EVENT_WS_CLOSED");
        exit(1);
        return;
    }
    default:
        return;
    }
}

// 供 core 查询宿主侧是否正在播放本地音乐。
static int zh_core_host_audio_is_music_active(void *userdata) {
    (void)userdata;
    return zh_music_player_is_active() ? 1 : 0;
}

// 供 core 查询宿主侧是否正在播放 TTS。
static int zh_core_host_audio_is_tts_playing(void *userdata) {
    (void)userdata;
    return zh_udp_tts_is_playing();
}

// 处理 core 上报的本地 VAD active 变化，并同步宿主轮次状态。
static void zh_core_host_audio_vad_state_changed(void *userdata, int active) {
    struct zh_ws_session *session = (struct zh_ws_session *)userdata;
    zh_face_recognition_set_active(active ? 1 : 0);
    if (!active || !session) {
        return;
    }

    pthread_mutex_lock(&session->mutex);
    zh_ws_reset_round_state_locked(session);
    pthread_mutex_unlock(&session->mutex);
}

// 收取并解析 core 转发的原始 WS 文本消息。
static void zh_core_host_ws_text_message(void *userdata, const char *text, size_t len) {
    struct zh_ws_session *session = (struct zh_ws_session *)userdata;
    bithion_core_json_view_t msg = mg_str_n(text ? text : "", len);
    char *type = NULL;
    char *status = NULL;
    char *vad_status = NULL;

    if (!session || !text || len == 0) {
        return;
    }

    type = mg_json_get_str(msg, "$.type");
    if (type && strcmp(type, "FORTUNE_ASR_RESULT") == 0) {
        char *asr_text = mg_json_get_str(msg, "$.text");
        long long hit = zh_json_get_bool_as_long(msg, "$.hit", 0);
        LOGI(__func__, "fortune asr result: hit=%lld text=%s", hit, asr_text ? asr_text : "");
        free(asr_text);
    } else if (type && strcmp(type, "FORTUNE_UPDATE") == 0) {
        char *source = mg_json_get_str(msg, "$.source");
        char *reason = mg_json_get_str(msg, "$.reason");
        long long delta = mg_json_get_long(msg, "$.delta", 0);
        long long total = mg_json_get_long(msg, "$.total", 0);
        LOGI(__func__, "fortune update: source=%s reason=%s delta=%lld total=%lld",
             source ? source : "",
             reason ? reason : "",
             delta,
             total);
        free(source);
        free(reason);
    } else if (type && strcmp(type, "FORTUNE_STATE") == 0) {
        long long total = mg_json_get_long(msg, "$.total", 0);
        LOGI(__func__, "fortune state: total=%lld", total);
    } else if (type && strcmp(type, "FORTUNE_VISION_RESULT") == 0) {
        char *resp_status = mg_json_get_str(msg, "$.status");
        char *device_id = mg_json_get_str(msg, "$.device_id");
        char *frame_id = mg_json_get_str(msg, "$.frame_id");
        char *reason = mg_json_get_str(msg, "$.reason");
        char *error_code = mg_json_get_str(msg, "$.error_code");
        double confidence = 0.0;
        long long is_offering = zh_json_get_bool_as_long(msg, "$.is_offering", 0);
        long long negative = zh_json_get_bool_as_long(msg, "$.negative", 0);
        long long awarded = zh_json_get_bool_as_long(msg, "$.awarded", 0);
        long long elapsed_ms = mg_json_get_long(msg, "$.elapsed_ms", 0);
        long long next_capture_ms = mg_json_get_long(msg, "$.next_capture_ms", 0);

        (void)mg_json_get_num(msg, "$.confidence", &confidence);
        pthread_mutex_lock(&session->mutex);
        zh_copy_str(session->fortune_vision_result.status,
                    sizeof(session->fortune_vision_result.status),
                    resp_status);
        zh_copy_str(session->fortune_vision_result.device_id,
                    sizeof(session->fortune_vision_result.device_id),
                    device_id);
        zh_copy_str(session->fortune_vision_result.frame_id,
                    sizeof(session->fortune_vision_result.frame_id),
                    frame_id);
        zh_copy_str(session->fortune_vision_result.reason,
                    sizeof(session->fortune_vision_result.reason),
                    reason);
        zh_copy_str(session->fortune_vision_result.error_code,
                    sizeof(session->fortune_vision_result.error_code),
                    error_code);
        session->fortune_vision_result.is_offering = is_offering ? 1 : 0;
        session->fortune_vision_result.negative = negative ? 1 : 0;
        session->fortune_vision_result.awarded = awarded ? 1 : 0;
        session->fortune_vision_result.confidence = confidence;
        session->fortune_vision_result.elapsed_ms = elapsed_ms;
        session->fortune_vision_result.next_capture_ms = next_capture_ms;
        session->fortune_vision_result_ready = 1;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mutex);

        LOGI(__func__,
             "fortune vision result: frame=%s status=%s offering=%lld awarded=%lld conf=%.3f reason=%s elapsed_ms=%lld",
             frame_id ? frame_id : "",
             resp_status ? resp_status : "",
             is_offering,
             awarded,
             confidence,
             reason ? reason : "",
             elapsed_ms);
        free(resp_status);
        free(device_id);
        free(frame_id);
        free(reason);
        free(error_code);
    }
    if (type && (strcmp(type, "vision_capture_request") == 0 ||
                 strcmp(type, "capture_photo_request") == 0 ||
                 strcmp(type, "take_photo") == 0 ||
                 strcmp(type, "vision_request") == 0)) {
        (void)zh_vision_capture_request_async(type);
    }
    if (type && strcmp(type, "vision_upload_response") == 0) {
        char *resp_status = mg_json_get_str(msg, "$.status");
        char *error_code = mg_json_get_str(msg, "$.error_code");
        char *upload_id = mg_json_get_str(msg, "$.upload_id");
        char *upload_url = mg_json_get_str(msg, "$.upload_url");
        char *object_key = mg_json_get_str(msg, "$.object_key");
        long long expires_at = mg_json_get_long(msg, "$.expires_at", 0);
        long long next_capture_ms = mg_json_get_long(msg, "$.next_capture_ms", 0);

        pthread_mutex_lock(&session->mutex);
        zh_copy_str(session->vision_upload_resp.status,
                    sizeof(session->vision_upload_resp.status),
                    resp_status);
        zh_copy_str(session->vision_upload_resp.error_code,
                    sizeof(session->vision_upload_resp.error_code),
                    error_code);
        zh_copy_str(session->vision_upload_resp.upload_id,
                    sizeof(session->vision_upload_resp.upload_id),
                    upload_id);
        zh_copy_str(session->vision_upload_resp.upload_url,
                    sizeof(session->vision_upload_resp.upload_url),
                    upload_url);
        zh_copy_str(session->vision_upload_resp.object_key,
                    sizeof(session->vision_upload_resp.object_key),
                    object_key);
        session->vision_upload_resp.expires_at = expires_at;
        session->vision_upload_resp.next_capture_ms = next_capture_ms;
        session->vision_upload_ready = 1;
        pthread_cond_broadcast(&session->cond);
        pthread_mutex_unlock(&session->mutex);

        free(resp_status);
        free(error_code);
        free(upload_id);
        free(upload_url);
        free(object_key);
    }
    free(type);

    status = mg_json_get_str(msg, "$.status");
    if (status) {
        if (strcmp(status, "authenticated") == 0) {
            pthread_mutex_lock(&session->mutex);
            session->authenticated = true;
            pthread_mutex_unlock(&session->mutex);
        } else if (strcmp(status, "error") == 0) {
            LOGE(__func__, "ws error msg: %.*s", (int)len, text);
        } else if (strcmp(status, "STT_COMPLETED") == 0) {
            long long stt_chat_count = mg_json_get_long(msg, "$.chat_count", -1);

            pthread_mutex_lock(&session->mutex);
            session->stt_completed = true;
            if (stt_chat_count >= 0 && stt_chat_count <= 0xffffffffLL) {
                session->stt_completed_chat_count = (uint32_t)stt_chat_count;
                session->stt_completed_chat_valid = true;
            } else {
                session->stt_completed_chat_valid = false;
                session->stt_completed_chat_count = 0;
                LOGW(__func__, "stt completed without valid chat_count");
            }
            pthread_mutex_unlock(&session->mutex);
        } else if (strcmp(status, "TTS_COMPLETED") == 0) {
            pthread_mutex_lock(&session->mutex);
            session->tts_completed = true;
            pthread_mutex_unlock(&session->mutex);
        } else if (strcmp(status, "vision_done") == 0) {
            char *result = mg_json_get_str(msg, "$.result");
            char *device_id = mg_json_get_str(msg, "$.device_id");
            char *frame_id = mg_json_get_str(msg, "$.frame_id");
            char *error_code = mg_json_get_str(msg, "$.error_code");

            pthread_mutex_lock(&session->mutex);
            zh_copy_str(session->vision_done.result, sizeof(session->vision_done.result), result);
            zh_copy_str(session->vision_done.device_id, sizeof(session->vision_done.device_id), device_id);
            zh_copy_str(session->vision_done.frame_id, sizeof(session->vision_done.frame_id), frame_id);
            zh_copy_str(session->vision_done.error_code, sizeof(session->vision_done.error_code), error_code);
            session->vision_done_ready = 1;
            pthread_cond_broadcast(&session->cond);
            pthread_mutex_unlock(&session->mutex);

            free(result);
            free(device_id);
            free(frame_id);
            free(error_code);
        } else if (strcmp(status, "vision_capture") == 0 ||
                   strcmp(status, "capture_photo") == 0 ||
                   strcmp(status, "take_photo") == 0 ||
                   strcmp(status, "vision_request") == 0) {
            (void)zh_vision_capture_request_async(status);
        } else if (strcmp(status, "VAD_START") == 0) {
            zh_prompt_tone_stop_web_search_wait();
            if (zh_ws_mark_vad_start(session)) {
                LOGI(__func__, "recv VAD_START, interrupt tts playback");
                zh_udp_tts_interrupt();
            }
        } else if (strcmp(status, "face_recog") == 0) {
            if (zh_face_enroll_on_recog() != 0) {
                LOGE(__func__, "face_recog enroll capture failed");
            }
        } else if (strcmp(status, "face_owner") == 0) {
            char *name = mg_json_get_str(msg, "$.message");
            if (zh_face_enroll_on_owner(name ? name : "") != 0) {
                LOGE(__func__, "face_owner enroll failed");
            }
            free(name);
        } else if (strcmp(status, "music_server") == 0) {
            char **urls = NULL;
            size_t count = 0;

            if (zh_ws_collect_music_urls(msg, &urls, &count) == 0) {
                if (zh_udp_tts_is_busy()) {
                    pthread_mutex_lock(&session->mutex);
                    zh_ws_clear_pending_music_locked(session);
                    session->pending_music_urls = urls;
                    session->pending_music_count = count;
                    pthread_mutex_unlock(&session->mutex);
                    LOGI(__func__, "tts busy, queue music until playback completed");
                } else {
                    zh_ws_play_music_urls_owned(urls, count);
                }
            } else {
                LOGE(__func__, "music_server without urls");
            }
        } else if (strcmp(status, "baidu_audio") == 0) {
            char *query = mg_json_get_str(msg, "$.query");
            char *tag = mg_json_get_str(msg, "$.tag");
            if (query && query[0]) {
                zh_music_player_play_baidu_query(query, tag ? tag : "");
            } else {
                LOGE(__func__, "baidu_audio without query");
            }
            free(query);
            free(tag);
        } else if (strcmp(status, "BAIDU_AUDIO_TTS_DONE") == 0) {
            char *request_id = mg_json_get_str(msg, "$.request_id");
            zh_music_player_baidu_tts_done(request_id ? request_id : "");
            free(request_id);
        } else if (strcmp(status, "BAIDU_AUDIO_CANCEL") == 0) {
            char *request_id = mg_json_get_str(msg, "$.request_id");
            LOGI(__func__, "baidu audio cancel received: request_id=%s",
                 request_id ? request_id : "");
            zh_music_player_baidu_cancel(request_id ? request_id : "");
            free(request_id);
        } else if (strcmp(status, "BAIDU_AUDIO_TTS_START") == 0) {
            zh_udp_tts_begin_subround();
        } else if (strcmp(status, "music_play_pre") == 0) {
            long code = mg_json_get_long(msg, "$.status_code", -1);
            if (code == 200 && bithion_core_ws_send(g_core, "START") != 0) {
                LOGE(__func__, "ws START send failed");
            }
        }
    }

    vad_status = mg_json_get_str(msg, "$.vad_start.status");
    if (vad_status && strcmp(vad_status, "VAD_START") == 0) {
        zh_prompt_tone_stop_web_search_wait();
        if (zh_ws_mark_vad_start(session)) {
            LOGI(__func__, "recv VAD_START, interrupt tts playback");
            zh_udp_tts_interrupt();
        }
    }

    free(vad_status);
    free(status);
}

static const bithion_core_host_hooks_t g_core_hooks = {
    .event_notify = zh_core_host_event_notify,
    .ws_text_message = zh_core_host_ws_text_message,
    .audio_is_music_active = zh_core_host_audio_is_music_active,
    .audio_is_tts_playing = zh_core_host_audio_is_tts_playing,
    .audio_vad_state_changed = zh_core_host_audio_vad_state_changed,
};

// 将 core 当前持有的配置同步回兼容层缓存。
static int zh_core_sync_config(zh_config_t *out_cfg) {
    if (!g_core) {
        return -1;
    }
    if (bithion_core_copy_config(g_core, &g_core_cfg) != 0) {
        return -1;
    }
    if (out_cfg) {
        *out_cfg = g_core_cfg;
    }
    return 0;
}

// 按给定配置创建或复用 core 实例，并注册宿主回调。
static int zh_core_prepare(const zh_config_t *cfg) {
    pthread_once(&g_ws_session_once, zh_ws_session_init_once);
    if (!cfg) {
        return -1;
    }
    if (!g_core) {
        g_core = bithion_core_create(cfg);
        if (!g_core) {
            return -1;
        }
    } else {
        bithion_core_reset(g_core);
        if (bithion_core_apply_config(g_core, cfg) != 0) {
            return -1;
        }
    }
    if (bithion_core_set_advanced_vad_enabled(g_core, ZH_CLIENT_ADVANCED_VAD_ENABLE) != 0) {
        return -1;
    }
    if (bithion_core_set_ui_text_enabled(g_core, ZH_ENABLE_UI_TEXT_CAPABILITY) != 0) {
        return -1;
    }
    if (bithion_core_set_live2d_action_enabled(g_core, ZH_ENABLE_LIVE2D_ACTION_CAPABILITY) != 0) {
        return -1;
    }
    /*
     * 人脸识别本身也使用视觉抓拍/上传链路，因此只要启用人脸识别
     * 或通用视觉抓拍，就向 core 上报视觉能力。
     * 这里仅设置能力位，不改变视觉采集线程的启动策略。
     */
    if (bithion_core_set_vision_capture_enabled(
            g_core, (ZH_ENABLE_FACE || ZH_ENABLE_VISION_CAPTURE) ? 1 : 0) != 0) {
        return -1;
    }
    if (bithion_core_set_baidu_audio_enabled(g_core, ZH_ENABLE_BAIDU_AUDIO_CAPABILITY) != 0) {
        return -1;
    }
    if (bithion_core_set_host_hooks(g_core, &g_core_hooks, &g_ws_session) != 0) {
        return -1;
    }
    g_core_cfg = *cfg;
    return 0;
}

// 在启用本地路由开关时，用本地配置覆盖 core 的远端路由。
static void zh_core_apply_local_route_override(void) {
    if (!g_core || !ZH_FORCE_LOCAL_ROUTE_CONFIG) {
        return;
    }
    (void)bithion_core_set_ws_url(g_core, ZH_WS_URL);
    (void)bithion_core_set_udp_ip(g_core, ZH_UDP_IP);
    (void)bithion_core_set_udp_port(g_core, ZH_UDP_PORT);
    LOGW(__func__, "ZH_FORCE_LOCAL_ROUTE_CONFIG=1, force using local ws/udp route");
}

// 校验传入会话是否为当前兼容层持有的有效会话对象。
static bool zh_core_valid_session(zh_ws_session_t *session) {
    return session == &g_ws_session && g_core != NULL;
}

// 通过 core 执行设备绑定流程。
int zh_http_bind_device(const zh_config_t *cfg) {
    if (!cfg) {
        return -1;
    }
    if (zh_core_prepare(cfg) != 0) {
        return -1;
    }
    if (bithion_core_bind_device(g_core) != 0) {
        return -1;
    }
    if (zh_core_sync_config(NULL) != 0) {
        return -1;
    }
    return 0;
}

// 通过 core 校验绑定结果并刷新会话路由配置。
int zh_http_check_bind(const zh_config_t *cfg) {
    if (!g_core) {
        if (!cfg || zh_core_prepare(cfg) != 0) {
            return -1;
        }
    }
    if (!g_core) {
        return -1;
    }
    if (bithion_core_check_bind(g_core) != 0) {
        return -1;
    }
    zh_core_apply_local_route_override();
    return zh_core_sync_config(NULL);
}

// 创建一个面向宿主的 WS 会话视图，并按需覆盖 WS 地址。
zh_ws_session_t *zh_ws_connect(const zh_config_t *cfg, const char *url) {
    if (!g_core && (!cfg || zh_core_prepare(cfg) != 0)) {
        return NULL;
    }
    if (!g_core) {
        return NULL;
    }
    zh_ws_session_reset(&g_ws_session);
    if (url && url[0] != '\0') {
        (void)bithion_core_set_ws_url(g_core, url);
    }
    return &g_ws_session;
}

// 等待 core 完成 WS 连接与鉴权，并上报设备状态。
int zh_ws_wait_authenticated(zh_ws_session_t *session, int timeout_ms) {
    char msg[128];
    if (!zh_core_valid_session(session)) {
        return -1;
    }
    if (bithion_core_ws_connect_and_auth(g_core, timeout_ms) != 0) {
        return -1;
    }
    pthread_mutex_lock(&session->mutex);
    session->authenticated = true;
    session->should_stop = false;
    pthread_mutex_unlock(&session->mutex);

    snprintf(msg,
             sizeof(msg),
             "{\"type\":\"Device State\",\"devices\":{\"camera\":\"%s\"}}",
             (ZH_ENABLE_FACE || ZH_ENABLE_VISION_CAPTURE) ? "on" : "off");
    if (zh_ws_send_str(session, msg) != 0) {
        LOGE(__func__, "device state send failed");
    } else {
        LOGI(__func__, "device state sent: %s", msg);
    }
    return 0;
}

// 通过 core 发送一条 WS 文本消息，并同步本地轮次状态。
int zh_ws_send_str(zh_ws_session_t *session, const char *msg) {
    if (!zh_core_valid_session(session) || !msg || msg[0] == '\0') {
        return -1;
    }
    if (bithion_core_ws_send(g_core, msg) != 0) {
        return -1;
    }
    if (strcmp(msg, "START") == 0) {
        pthread_mutex_lock(&session->mutex);
        zh_ws_reset_round_state_locked(session);
        pthread_mutex_unlock(&session->mutex);
    }
    return 0;
}

int zh_ws_report_baidu_audio_result(const char *request_id, const char *answer,
                                    const char *resource_json) {
    char request_esc[128];
    char answer_esc[1024];
    char msg[3072];
    if (!g_core || !request_id || !request_id[0] || !answer) return -1;
    if (bithion_core_json_escape(request_id, request_esc, sizeof(request_esc)) != 0 ||
        bithion_core_json_escape(answer, answer_esc, sizeof(answer_esc)) != 0) return -1;
    if (!resource_json || !resource_json[0]) resource_json = "null";
    if (snprintf(msg, sizeof(msg),
                 "{\"type\":\"BAIDU_AUDIO_RESULT\",\"request_id\":\"%s\","
                 "\"answer\":\"%s\",\"resource\":%s}",
                 request_esc, answer_esc, resource_json) >= (int)sizeof(msg)) return -1;
    return bithion_core_ws_send(g_core, msg);
}

// 查询当前轮次的 STT 是否已经完成。
bool zh_ws_is_stt_completed(zh_ws_session_t *session) {
    bool completed = false;

    if (!zh_core_valid_session(session)) {
        return false;
    }
    pthread_mutex_lock(&session->mutex);
    completed = session->stt_completed;
    pthread_mutex_unlock(&session->mutex);
    return completed;
}

// 读取当前轮次 STT 完成消息携带的 chat_count。
int zh_ws_get_stt_completed_chat_count(zh_ws_session_t *session, uint32_t *out_chat_count) {
    if (!zh_core_valid_session(session) || !out_chat_count) {
        return -1;
    }
    pthread_mutex_lock(&session->mutex);
    if (!session->stt_completed || !session->stt_completed_chat_valid) {
        pthread_mutex_unlock(&session->mutex);
        return -1;
    }
    *out_chat_count = session->stt_completed_chat_count;
    pthread_mutex_unlock(&session->mutex);
    return 0;
}

// 查询当前轮次的 TTS 是否已经完成。
bool zh_ws_is_tts_completed(zh_ws_session_t *session) {
    bool completed = false;

    if (!zh_core_valid_session(session)) {
        return false;
    }
    pthread_mutex_lock(&session->mutex);
    completed = session->tts_completed;
    pthread_mutex_unlock(&session->mutex);
    return completed;
}

void zh_ws_reset_tts_completed(zh_ws_session_t *session) {
    if (!zh_core_valid_session(session)) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    session->tts_completed = false;
    pthread_mutex_unlock(&session->mutex);
}

// 消费一次已记录的 VAD_START 标记。
bool zh_ws_consume_vad_start(zh_ws_session_t *session) {
    bool started = false;

    if (!zh_core_valid_session(session)) {
        return false;
    }
    pthread_mutex_lock(&session->mutex);
    started = session->vad_started;
    session->vad_started = false;
    pthread_mutex_unlock(&session->mutex);
    return started;
}

void zh_ws_cancel_web_search_wait_audio(zh_ws_session_t *session) {
    if (zh_core_valid_session(session)) {
        pthread_mutex_lock(&session->mutex);
        session->web_search_wait_pending = false;
        pthread_mutex_unlock(&session->mutex);
    }
    zh_prompt_tone_stop_web_search_wait();
}

// 手动重置当前轮次相关状态。
void zh_ws_reset_round_state(zh_ws_session_t *session) {
    if (!zh_core_valid_session(session)) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    zh_ws_reset_round_state_locked(session);
    pthread_mutex_unlock(&session->mutex);
}

// 驱动 core 的 WS 主循环直到会话结束。
void zh_ws_run_loop(zh_ws_session_t *session) {
    if (!zh_core_valid_session(session)) {
        return;
    }
    (void)bithion_core_run_loop(g_core);
}

// 关闭当前兼容层会话，并停止 core 的运行。
void zh_ws_close(zh_ws_session_t *session) {
    if (!zh_core_valid_session(session)) {
        return;
    }
    pthread_mutex_lock(&session->mutex);
    session->authenticated = false;
    session->should_stop = true;
    zh_ws_clear_pending_music_locked(session);
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
    bithion_core_stop(g_core);
}

// 发送一次视觉上传申请消息。
int zh_ws_send_vision_upload_request(zh_ws_session_t *session,
                                     const char *device_id,
                                     const char *frame_id,
                                     const char *content_type) {
    char msg[1024];
    char device_id_esc[128];
    char frame_id_esc[256];
    char content_type_esc[128];

    if (!zh_core_valid_session(session) || !device_id || !frame_id || !content_type) {
        return -1;
    }

    pthread_mutex_lock(&session->mutex);
    session->vision_upload_ready = 0;
    memset(&session->vision_upload_resp, 0, sizeof(session->vision_upload_resp));
    pthread_mutex_unlock(&session->mutex);

    zh_json_escape_copy(device_id_esc, sizeof(device_id_esc), device_id);
    zh_json_escape_copy(frame_id_esc, sizeof(frame_id_esc), frame_id);
    zh_json_escape_copy(content_type_esc, sizeof(content_type_esc), content_type);
    snprintf(msg, sizeof(msg),
             "{\"type\":\"vision_upload_request\",\"device_id\":\"%s\",\"frame_id\":\"%s\",\"content_type\":\"%s\"}",
             device_id_esc,
             frame_id_esc,
             content_type_esc);
    return zh_ws_send_str(session, msg);
}

// 等待服务端返回视觉上传申请结果。
int zh_ws_wait_vision_upload_response(zh_ws_session_t *session,
                                      zh_ws_vision_upload_response_t *out,
                                      const int *running_flag) {
    if (!zh_core_valid_session(session) || !out) {
        return -1;
    }

    pthread_mutex_lock(&session->mutex);
    while (!session->vision_upload_ready && !session->should_stop) {
        struct timespec ts;

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        if (running_flag && !*running_flag) {
            pthread_mutex_unlock(&session->mutex);
            return -1;
        }
        pthread_cond_timedwait(&session->cond, &session->mutex, &ts);
    }
    if (!session->vision_upload_ready) {
        pthread_mutex_unlock(&session->mutex);
        return -1;
    }
    *out = session->vision_upload_resp;
    session->vision_upload_ready = 0;
    pthread_mutex_unlock(&session->mutex);
    return 0;
}

// 发送一次视觉上传完成确认消息。
int zh_ws_send_vision_upload_commit(zh_ws_session_t *session,
                                    const char *device_id,
                                    const char *frame_id,
                                    const char *upload_id,
                                    const char *object_key,
                                    const char *face_name) {
    char msg[2048];
    char device_id_esc[128];
    char frame_id_esc[256];
    char upload_id_esc[256];
    char object_key_esc[1024];
    char face_name_esc[256];

    if (!zh_core_valid_session(session) || !device_id || !frame_id || !upload_id || !object_key) {
        return -1;
    }
    if (!face_name) {
        face_name = "";
    }

    pthread_mutex_lock(&session->mutex);
    session->vision_done_ready = 0;
    session->fortune_vision_result_ready = 0;
    memset(&session->vision_done, 0, sizeof(session->vision_done));
    memset(&session->fortune_vision_result, 0, sizeof(session->fortune_vision_result));
    pthread_mutex_unlock(&session->mutex);

    zh_json_escape_copy(device_id_esc, sizeof(device_id_esc), device_id);
    zh_json_escape_copy(frame_id_esc, sizeof(frame_id_esc), frame_id);
    zh_json_escape_copy(upload_id_esc, sizeof(upload_id_esc), upload_id);
    zh_json_escape_copy(object_key_esc, sizeof(object_key_esc), object_key);
    zh_json_escape_copy(face_name_esc, sizeof(face_name_esc), face_name);
    snprintf(msg, sizeof(msg),
             "{\"type\":\"vision_upload_commit\",\"device_id\":\"%s\",\"frame_id\":\"%s\",\"upload_id\":\"%s\",\"object_key\":\"%s\",\"face_name\":\"%s\"}",
             device_id_esc,
             frame_id_esc,
             upload_id_esc,
             object_key_esc,
             face_name_esc);
    return zh_ws_send_str(session, msg);
}

// 等待指定 frame 对应的 vision_done 结果返回。
int zh_ws_wait_vision_done(zh_ws_session_t *session,
                           const char *frame_id,
                           zh_ws_vision_done_t *out,
                           const int *running_flag) {
    if (!zh_core_valid_session(session) || !frame_id || !out) {
        return -1;
    }

    pthread_mutex_lock(&session->mutex);
    while (!session->should_stop) {
        if (running_flag && !*running_flag) {
            pthread_mutex_unlock(&session->mutex);
            return -1;
        }
        if (session->vision_done_ready) {
            if (strcmp(session->vision_done.frame_id, frame_id) == 0) {
                *out = session->vision_done;
                session->vision_done_ready = 0;
                pthread_mutex_unlock(&session->mutex);
                return 0;
            }
            session->vision_done_ready = 0;
        }
        {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&session->cond, &session->mutex, &ts);
        }
    }
    pthread_mutex_unlock(&session->mutex);
    return -1;
}

// 等待指定 frame 对应的视觉判定完成结果返回。
int zh_ws_wait_fortune_vision_result(zh_ws_session_t *session,
                                     const char *frame_id,
                                     zh_ws_fortune_vision_result_t *out,
                                     const int *running_flag,
                                     int timeout_ms) {
    uint64_t start_ms;

    if (!zh_core_valid_session(session) || !frame_id || !out) {
        return -1;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 20000;
    }

    start_ms = zh_now_ms();
    pthread_mutex_lock(&session->mutex);
    while (!session->should_stop) {
        if (running_flag && !*running_flag) {
            pthread_mutex_unlock(&session->mutex);
            return -1;
        }
        if ((int)(zh_now_ms() - start_ms) >= timeout_ms) {
            pthread_mutex_unlock(&session->mutex);
            return -1;
        }
        if (session->fortune_vision_result_ready) {
            if (strcmp(session->fortune_vision_result.frame_id, frame_id) == 0) {
                *out = session->fortune_vision_result;
                session->fortune_vision_result_ready = 0;
                pthread_mutex_unlock(&session->mutex);
                return 0;
            }
            session->fortune_vision_result_ready = 0;
        }
        {
            struct timespec ts;
            zh_cond_time_after_ms(&ts, 200);
            pthread_cond_timedwait(&session->cond, &session->mutex, &ts);
        }
    }
    pthread_mutex_unlock(&session->mutex);
    return -1;
}

// 返回当前会话绑定的设备 ID。
const char *zh_ws_get_device_id(zh_ws_session_t *session) {
    if (!zh_core_valid_session(session)) {
        return "";
    }
    if (zh_core_sync_config(NULL) != 0) {
        return "";
    }
    return g_core_cfg.device_id;
}

// 启动 core 侧的下行 TTS 传输。
int zh_core_tts_transport_start(const zh_config_t *cfg, zh_ws_session_t *session) {
    (void)cfg;
    if (!zh_core_valid_session(session)) {
        return -1;
    }
    return bithion_core_tts_start(g_core);
}

// 停止 core 侧的下行 TTS 传输。
void zh_core_tts_transport_stop(void) {
    if (!g_core) {
        return;
    }
    bithion_core_tts_stop(g_core);
}

// 等待 core 侧下行 TTS 传输线程退出。
int zh_core_tts_transport_wait(int timeout_ms) {
    if (!g_core) {
        return -1;
    }
    return bithion_core_tts_wait(g_core, timeout_ms);
}

// 从 core 读取一帧已经解包排序后的下行 Opus 数据。
int zh_core_tts_read_opus(void *buf, size_t buf_len, size_t *out_len, int timeout_ms) {
    if (!g_core) {
        return -1;
    }
    return bithion_core_tts_read_opus(g_core, buf, buf_len, out_len, timeout_ms);
}

int zh_core_tts_read_opus_with_meta(void *buf,
                                    size_t buf_len,
                                    size_t *out_len,
                                    int timeout_ms,
                                    zh_core_tts_opus_meta_t *out_meta) {
    bithion_core_tts_opus_meta_t core_meta;
    int ret = 0;

    if (!g_core) {
        return -1;
    }
    memset(&core_meta, 0, sizeof(core_meta));
    ret = bithion_core_tts_read_opus_with_meta(g_core,
                                               buf,
                                               buf_len,
                                               out_len,
                                               timeout_ms,
                                               &core_meta);
    if (out_meta) {
        out_meta->chat_count = core_meta.chat_count;
        out_meta->packet_seq = core_meta.packet_seq;
    }
    return ret;
}

// 通知 core 中断当前下行 TTS 轮次。
void zh_core_tts_transport_interrupt(void) {
    if (!g_core) {
        return;
    }
    bithion_core_tts_interrupt(g_core);
}

// 查询 core 是否仍持有待消费的 TTS 数据。
int zh_core_tts_transport_has_pending_data(void) {
    if (!g_core) {
        return 0;
    }
    return bithion_core_tts_has_pending_data(g_core);
}

// 查询 core 的 TTS 传输线程是否还在运行。
int zh_core_tts_transport_is_running(void) {
    if (!g_core) {
        return 0;
    }
    return bithion_core_tts_is_transport_running(g_core);
}

// 预加载 core 侧 VAD 模型或相关资源。
int zh_core_vad_preload(void) {
    return bithion_core_vad_preload();
}

// 创建一个供客户端线程驱动的 VAD 实例。
zh_core_vad_t *zh_core_vad_create(void) {
    bithion_core_vad_t *impl = NULL;
    zh_core_vad_t *vad = NULL;

    impl = bithion_core_vad_create();
    if (!impl) {
        return NULL;
    }
    vad = (zh_core_vad_t *)calloc(1, sizeof(*vad));
    if (!vad) {
        bithion_core_vad_destroy(impl);
        return NULL;
    }
    vad->impl = impl;
    return vad;
}

// 销毁一个客户端持有的 VAD 实例。
void zh_core_vad_destroy(zh_core_vad_t *vad) {
    if (!vad) {
        return;
    }
    bithion_core_vad_destroy(vad->impl);
    free(vad);
}

// 重置一份 VAD 实例的内部状态。
void zh_core_vad_reset(zh_core_vad_t *vad) {
    if (!vad || !vad->impl) {
        return;
    }
    bithion_core_vad_reset(vad->impl);
}

// 对一帧 PCM 执行一次 VAD 计算。
int zh_core_vad_process(zh_core_vad_t *vad,
                        const int16_t *pcm,
                        size_t frame_samples,
                        size_t channels,
                        zh_core_vad_result_t *out_result) {
    bithion_core_vad_result_t result;

    if (!vad || !vad->impl || !out_result) {
        return -1;
    }
    if (bithion_core_vad_process(vad->impl, pcm, frame_samples, channels, &result) != 0) {
        return -1;
    }
    out_result->l1_active = result.l1_active;
    out_result->speech_active = result.speech_active;
    out_result->speech_started = result.speech_started;
    out_result->speech_ended = result.speech_ended;
    out_result->l2_prob = result.l2_prob;
    return 0;
}

// 启动 core 侧上行 Opus 发送器。
int zh_core_uplink_start(void) {
    if (!g_core) {
        return -1;
    }
    return bithion_core_uplink_start(g_core);
}

// 停止 core 侧上行 Opus 发送器。
void zh_core_uplink_stop(void) {
    if (!g_core) {
        return;
    }
    bithion_core_uplink_stop(g_core);
}

// 通知 core 开启新一轮上行语音并发送 START。
int zh_core_uplink_begin_segment(void) {
    int rc = 0;

    if (!g_core) {
        return -1;
    }
    rc = bithion_core_uplink_begin_segment(g_core);
    if (rc == 0) {
        pthread_mutex_lock(&g_ws_session.mutex);
        zh_ws_reset_round_state_locked(&g_ws_session);
        pthread_mutex_unlock(&g_ws_session.mutex);
    }
    return rc;
}

// 向 core 提交一帧客户端已编码的 Opus 数据。
int zh_core_uplink_send_opus(const void *data, size_t len) {
    if (!g_core) {
        return -1;
    }
    return bithion_core_uplink_send_opus(g_core, data, len);
}

// 请求 core 清空上行发送器状态并回到新轮次起点。
void zh_core_uplink_reset(void) {
    if (!g_core) {
        return;
    }
    bithion_core_uplink_reset(g_core);
}

// 请求 core 立即发出当前缓存的上行尾包。
void zh_core_uplink_flush(void) {
    if (!g_core) {
        return;
    }
    bithion_core_uplink_flush(g_core);
}

int zh_core_uplink_flush_wait(int timeout_ms) {
    if (!g_core) {
        return -1;
    }
    return bithion_core_uplink_flush_wait(g_core, timeout_ms);
}

// 在 TTS 播放结束后启动之前被延后的音乐播放。
void zh_core_ws_on_tts_round_done(void) {
    char **urls = NULL;
    size_t count = 0;

    zh_ws_take_pending_music(&g_ws_session, &urls, &count);
    if (!urls || count == 0) {
        return;
    }

    LOGI(__func__, "tts finished, start queued music: %zu urls", count);
    zh_ws_play_music_urls_owned(urls, count);
}
