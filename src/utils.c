#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "config.h"
#include "utils.h"
#include "log.h"

// 通用工具函数：时间、文件读取、字节序、增益等。

static int zh_month_from_build_date(const char *abbr) {
    static const char *k_months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    for (int i = 0; i < 12; ++i) {
        if (strncmp(abbr, k_months[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

static int zh_get_build_time(struct timespec *out_ts) {
    char month[4];
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int month_index;
    struct tm tm_build;
    time_t build_time;

    if (!out_ts) {
        return -1;
    }
    if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) {
        return -1;
    }
    if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return -1;
    }

    month[3] = '\0';
    month_index = zh_month_from_build_date(month);
    if (month_index < 0) {
        return -1;
    }

    memset(&tm_build, 0, sizeof(tm_build));
    tm_build.tm_year = year - 1900;
    tm_build.tm_mon = month_index;
    tm_build.tm_mday = day;
    tm_build.tm_hour = hour;
    tm_build.tm_min = minute;
    tm_build.tm_sec = second;
    tm_build.tm_isdst = -1;

    build_time = mktime(&tm_build);
    if (build_time == (time_t)-1) {
        return -1;
    }

    out_ts->tv_sec = build_time;
    out_ts->tv_nsec = 0;
    return 0;
}

// 获取指定网卡的MAC地址二进制
int zh_get_mac_addr(const char *ifname, unsigned char *out, size_t out_len) {
    int fd;
    struct ifreq ifr_hwaddr_req;

    if (!ifname || !out || out_len < 6) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGE(__func__, "socket failed errno=%d", errno);
        return -1;
    }

    memset(&ifr_hwaddr_req, 0, sizeof(ifr_hwaddr_req));
    strncpy(ifr_hwaddr_req.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr_hwaddr_req) != 0) {
        LOGE(__func__, "SIOCGIFHWADDR %s failed errno=%d", ifname, errno);
        close(fd);
        return -1;
    }

    memcpy(out, ifr_hwaddr_req.ifr_hwaddr.sa_data, 6);

    close(fd);
    return 0;
}

// 读取文件内容，去除前后空白字符，存入out缓冲区
int zh_read_file(const char *path, char *out, size_t out_len) {
    FILE *fp;
    size_t n;
    char *start;
    size_t len;

    if (!path || !out || out_len == 0) {
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        LOGE(__func__, "open file failed: %s errno=%d", path, errno);
        return -1;
    }

    n = fread(out, 1, out_len - 1, fp);
    fclose(fp);
    out[n] = '\0';

    start = out;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != out) {
        memmove(out, start, strlen(start) + 1);
    }

    len = strlen(out);
    while (len > 0 && isspace((unsigned char)out[len - 1])) {
        out[--len] = '\0';
    }

    return (len > 0) ? 0 : -1;
}

// 执行cmd，从stdout中拿到指定前缀开头的一行
// prefix为空或NULL时，返回所有输出行
int zh_cmd_get_json_line(const char *cmd, const char *prefix, char *out, size_t out_len) {
    FILE *fp;
    char line[8192];
    size_t prefix_len;
    int match_all;
    size_t used;

    if (!cmd || !out || out_len == 0) {
        return -1;
    }

    prefix_len = (prefix != NULL) ? strlen(prefix) : 0;
    match_all = (prefix == NULL || prefix[0] == '\0');

    // popen 读取子进程 stdout（调用端可自行重定向 stderr）
    fp = popen(cmd, "r");
    if (!fp) {
        LOGE(__func__, "popen failed errno=%d", errno);
        return -1;
    }

    // 只保留最后一条匹配行；match_all=1 时拼接所有输出
    out[0] = '\0';
    used = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (match_all) {
            size_t line_len = strlen(line);
            size_t remain = (out_len > used) ? (out_len - used - 1) : 0;
            if (remain == 0) {
                break;
            }
            if (line_len > remain) {
                line_len = remain;
            }
            memcpy(out + used, line, line_len);
            used += line_len;
            out[used] = '\0';
        } else {
            char *s = line;
            char *hit;
            while (*s && isspace((unsigned char)*s)) {
                s++;
            }
            // 匹配指定前缀开头的行；若同一行有前缀，截取前缀起点
            if (prefix_len == 0 || strncmp(s, prefix, prefix_len) == 0) {
                strncpy(out, s, out_len - 1);
                out[out_len - 1] = '\0';
            } else {
                hit = strstr(s, prefix);
                if (hit) {
                    strncpy(out, hit, out_len - 1);
                    out[out_len - 1] = '\0';
                }
            }
        }
    }

    if (pclose(fp) == -1) {
        LOGE(__func__, "pclose failed errno=%d", errno);
    }

    return (out[0] != '\0') ? 0 : -1;
}

int zh_realtime_clock_needs_bootstrap(time_t now, time_t build_time) {
    const time_t slack = 24 * 60 * 60;

    if (build_time <= 0) {
        return 0;
    }
    if (now < 0) {
        return 1;
    }
    if (build_time <= slack) {
        return now < build_time;
    }
    return now < (build_time - slack);
}

int zh_ensure_realtime_clock_valid(void) {
    struct timespec now_ts;
    struct timespec build_ts;

    if (clock_gettime(CLOCK_REALTIME, &now_ts) != 0) {
        LOGW(__func__, "clock_gettime failed errno=%d", errno);
        return -1;
    }
    if (zh_get_build_time(&build_ts) != 0) {
        LOGW(__func__, "parse build timestamp failed: %s %s", __DATE__, __TIME__);
        return -1;
    }
    if (!zh_realtime_clock_needs_bootstrap(now_ts.tv_sec, build_ts.tv_sec)) {
        return 0;
    }
    if (clock_settime(CLOCK_REALTIME, &build_ts) != 0) {
        LOGW(__func__,
             "realtime clock invalid now=%lld build=%lld, clock_settime failed errno=%d",
             (long long)now_ts.tv_sec,
             (long long)build_ts.tv_sec,
             errno);
        return -1;
    }

    LOGW(__func__,
         "realtime clock bootstrap applied now=%lld -> build=%lld to satisfy TLS validity checks",
         (long long)now_ts.tv_sec,
         (long long)build_ts.tv_sec);
    return 1;
}

// 单调时钟毫秒。
uint64_t zh_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

// 获取当前时间往后ms的绝对时间（用于条件变量超时等待）。
void zh_cond_time_after_ms(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

// 读取大端16位。
uint16_t zh_read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

// 读取大端32位。
uint32_t zh_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// 写入大端16位。
void zh_write_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

// 应用简单线性增益并做限幅。
int16_t zh_apply_gain(int16_t sample, float gain) {
    float v = (float)sample * gain;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}
