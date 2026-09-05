#ifndef ZH_UDP_TTS_H
#define ZH_UDP_TTS_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct zh_ws_session zh_ws_session_t;

// 下行TTS播放接口（UDP接收与播放）。
int zh_udp_tts_start(const zh_config_t *cfg, zh_ws_session_t *ws); // 启动UDP接收/播放线程
void zh_udp_tts_stop(void); // 停止UDP线程并清理状态
int zh_udp_tts_wait(int timeout_ms); // 等待线程退出
void zh_udp_tts_schedule_subtitle(uint32_t chat_count,
                                  uint32_t seq_start,
                                  const char *text); // 按下行 UDP 包序号延迟推送 UI 字幕
void zh_udp_tts_interrupt(void); // 立即中断当前TTS播放并清空缓冲
void zh_udp_tts_set_playing(int playing); // 手动设置播放标志
int zh_udp_tts_is_playing(void); // 查询是否正在播放
int zh_udp_tts_is_busy(void); // 是否仍有TTS待播/正在播
int zh_udp_tts_is_round_done(void); // 本轮TTS是否播放结束
// 同一 chat_count 内开始新的 TTS 子轮，清除停止回报去重和 UDP 缓冲。
void zh_udp_tts_begin_subround(void);

#endif
