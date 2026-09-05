#ifndef ZH_MUSIC_PLAYER_H
#define ZH_MUSIC_PLAYER_H

#include <stddef.h>

// 音乐播放模块：HTTP下载MP3 -> 解码 -> ALSA播放（独立线程）。

// 启动音乐播放线程（重复调用安全）。
int zh_music_player_start(void);
// 停止音乐播放线程并清理资源。
void zh_music_player_stop(void);
// 播放指定URL列表（新消息打断：清空队列并中止当前播放）。
void zh_music_player_play_urls(const char **urls, size_t count);
// 播放指定URL列表并应用增益（0~1更小声，>1放大）。
void zh_music_player_play_urls_with_gain(const char **urls, size_t count, float gain);
// 播放百度有声资源自然语言查询（走客户端直连 SSE）。
void zh_music_player_play_baidu_query(const char *query, const char *tag);
// 服务端已完成本次百度资源说明 TTS，允许开始播放缓存音频。
void zh_music_player_baidu_tts_done(const char *request_id);
// 取消当前百度资源请求并清空已缓存音频（OBS回退兜底，0723修改）。
void zh_music_player_baidu_cancel(const char *request_id);
// 仅中断当前/排队音乐，不退出音乐线程。
void zh_music_player_interrupt(void);
// 当前是否处于音乐播放/排队状态。
int zh_music_player_is_active(void);
// 当前百度资源正在等待服务端说明 TTS，尚未开始歌曲播放。
int zh_music_player_is_waiting_baidu_tts(void);

#endif
