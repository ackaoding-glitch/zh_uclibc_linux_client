#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// 通用工具接口。
int zh_get_mac_addr(const char *ifname, unsigned char *out, size_t out_len); // 获取MAC地址
int zh_read_file(const char *path, char *out, size_t out_len); // 读取文件并去空白
int zh_cmd_get_json_line(const char *cmd, const char *prefix, char *out, size_t out_len); // 执行命令取JSON行
int zh_realtime_clock_needs_bootstrap(time_t now, time_t build_time); // 当前时间明显早于编译时间时返回1
int zh_ensure_realtime_clock_valid(void); // 必要时尝试用编译时间校正实时时钟
uint64_t zh_now_ms(void); // 单调时钟毫秒
void zh_cond_time_after_ms(struct timespec *ts, int ms); // 计算超时时刻
uint16_t zh_read_be16(const uint8_t *p); // 读取大端16位
uint32_t zh_read_be32(const uint8_t *p); // 读取大端32位
void zh_write_be16(uint8_t *p, uint16_t v); // 写入大端16位
int16_t zh_apply_gain(int16_t sample, float gain); // 应用线性增益

#endif
