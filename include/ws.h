#ifndef ZH_WS_H
#define ZH_WS_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

// WebSocket 会话接口：鉴权、状态回执与心跳。

typedef struct zh_ws_session zh_ws_session_t;

typedef struct {
    char status[16];
    char error_code[64];
    char upload_id[128];
    char upload_url[2048];
    char object_key[512];
    long long expires_at;
    long long next_capture_ms;
} zh_ws_vision_upload_response_t;

typedef struct {
    char result[16];
    char device_id[64];
    char frame_id[128];
    char error_code[64];
} zh_ws_vision_done_t;

typedef struct {
    char status[16];
    char device_id[64];
    char frame_id[128];
    char reason[64];
    char error_code[64];
    int is_offering;
    int negative;
    int awarded;
    double confidence;
    long long elapsed_ms;
    long long next_capture_ms;
} zh_ws_fortune_vision_result_t;

zh_ws_session_t *zh_ws_connect(const zh_config_t *cfg, const char *url); // 建立WS连接
int zh_ws_wait_authenticated(zh_ws_session_t *s, int timeout_ms); // 等待鉴权成功
int zh_ws_send_str(zh_ws_session_t *s, const char *msg); // 发送文本消息
int zh_ws_report_baidu_audio_result(const char *request_id, const char *answer,
                                    const char *resource_json);
bool zh_ws_is_stt_completed(zh_ws_session_t *s); // 查询STT完成
int zh_ws_get_stt_completed_chat_count(zh_ws_session_t *s, uint32_t *out_chat_count); // 获取STT完成轮次
bool zh_ws_is_tts_completed(zh_ws_session_t *s); // 查询TTS完成
void zh_ws_reset_tts_completed(zh_ws_session_t *s); // 重置TTS完成标记(百度子轮)
bool zh_ws_consume_vad_start(zh_ws_session_t *s); // 消费VAD_START事件
void zh_ws_cancel_web_search_wait_audio(zh_ws_session_t *s); // 取消联网搜索等待音
void zh_ws_reset_round_state(zh_ws_session_t *s); // 重置本轮状态
void zh_ws_run_loop(zh_ws_session_t *s); // WS事件循环
void zh_ws_close(zh_ws_session_t *s); // 关闭并释放

int zh_ws_send_vision_upload_request(zh_ws_session_t *s,
                                     const char *device_id,
                                     const char *frame_id,
                                     const char *content_type);
int zh_ws_wait_vision_upload_response(zh_ws_session_t *s,
                                      zh_ws_vision_upload_response_t *out,
                                      const int *running_flag);
int zh_ws_send_vision_upload_commit(zh_ws_session_t *s,
                                    const char *device_id,
                                    const char *frame_id,
                                    const char *upload_id,
                                    const char *object_key,
                                    const char *face_name);
int zh_ws_wait_vision_done(zh_ws_session_t *s,
                           const char *frame_id,
                           zh_ws_vision_done_t *out,
                           const int *running_flag);
int zh_ws_wait_fortune_vision_result(zh_ws_session_t *s,
                                     const char *frame_id,
                                     zh_ws_fortune_vision_result_t *out,
                                     const int *running_flag,
                                     int timeout_ms);
const char *zh_ws_get_device_id(zh_ws_session_t *s);

#endif
