#ifndef ZH_APP_H
#define ZH_APP_H

#include "config.h"

// 应用入口接口：启动主业务循环。

int zh_app_run(zh_config_t *cfg); // 阻塞运行并负责重连
int zh_app_start_with_config(zh_config_t *cfg); // 使用已加载配置进入完整启动和配网流程
int zh_app_start(void); // 应用总入口（含开机与配网）

#endif
