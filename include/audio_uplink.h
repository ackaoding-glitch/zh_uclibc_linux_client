#ifndef ZH_AUDIO_UPLINK_H
#define ZH_AUDIO_UPLINK_H

typedef struct zh_ws_session zh_ws_session_t;

int zh_audio_uplink_preload(void);
int zh_audio_uplink_start(void);
void zh_audio_uplink_stop(void);
void zh_audio_uplink_set_ws(zh_ws_session_t *ws);

#endif
