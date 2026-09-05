#ifndef SNTP_H
#define SNTP_H

#include <time.h>

// SNTP 时间同步（RFC 4330 简单模式，最小实现）。
// servers: 空格分隔的服务器域名/IP 列表；传 NULL 使用 ZH_NTP_SERVERS。
// timeout_ms: 单服务器接收超时；<=0 使用 ZH_NTP_TIMEOUT_MS。
// 成功时用服务器时间设置系统实时时钟（UTC）并返回 0；全部失败返回 -1，
// 调用方应回退到编译时间兜底（zh_ensure_realtime_clock_valid）。
int zh_sntp_sync(const char *servers, int timeout_ms);

#endif
