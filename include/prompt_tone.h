#ifndef ZH_PROMPT_TONE_H
#define ZH_PROMPT_TONE_H

typedef enum {
    ZH_PROMPT_TONE_BOOT = 0, // 设备开机
    ZH_PROMPT_TONE_PROVISION, // 进入配网模式
    ZH_PROMPT_TONE_NET_CONNECTED, // 网络连接成功
    ZH_PROMPT_TONE_NET_DISCONNECTED, // 网络断开
    ZH_PROMPT_TONE_READY, // 可开始说话
    ZH_PROMPT_TONE_WEB_SEARCH_WAIT, // 联网搜索等待
} zh_prompt_tone_event_t;

// 初始化提示音子系统（内部复用 music_player 线程，只初始化一次）。
int zh_prompt_tone_init(void);
// 播放指定事件的提示音；若 URL 为空则跳过。
void zh_prompt_tone_play(zh_prompt_tone_event_t event);
// 开始/停止联网搜索等待提示音循环。
void zh_prompt_tone_start_web_search_wait(void);
void zh_prompt_tone_stop_web_search_wait(void);

#endif
