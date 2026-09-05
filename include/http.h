#ifndef ZH_HTTP_H
#define ZH_HTTP_H

#include <stddef.h>

#include "config.h"

// 设备绑定相关HTTP接口。

int zh_http_bind_device(const zh_config_t *cfg); // 绑定设备，不向外返回 api_key
int zh_http_check_bind(const zh_config_t *cfg); // 校验绑定并仅刷新内部路由

// api接口
static const char* API_BIND_DEVICE  = "/auth/bind_device"; // 设备绑定接口路径


#endif
