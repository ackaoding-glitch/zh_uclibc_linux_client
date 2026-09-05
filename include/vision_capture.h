#ifndef ZH_VISION_CAPTURE_H
#define ZH_VISION_CAPTURE_H

#include "ws.h"

void zh_vision_capture_set_ws(zh_ws_session_t *ws);
int zh_vision_capture_start(void);
void zh_vision_capture_stop(void);
int zh_vision_capture_request_async(const char *reason);

#endif
