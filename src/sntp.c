#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "sntp.h"
#include "utils.h"

// SNTPv4 简单模式客户端（RFC 4330）最小实现：
// 对服务器列表逐个发送 48 字节查询，任一成功即用其 transmit timestamp
// 设置系统实时时钟（UTC）。全部失败返回 -1，由调用方回退到编译时间兜底。

#define SNTP_PORT 123                  // NTP/SNTP UDP 端口
#define SNTP_PACKET_LEN 48             // SNTPv4 报文长度
#define SNTP_MODE_CLIENT 3             // 请求：客户端模式
#define SNTP_MODE_SERVER 4             // 响应：服务器模式
#define SNTP_EPOCH_OFFSET 2208988800UL // NTP 纪元(1900) 到 Unix 纪元(1970) 秒差

// 解析服务器地址：支持 IPv4 字面量或域名（跟随 bithion-core http.c 的
// gethostbyname 风格，不引入 getaddrinfo 依赖）。
static int sntp_resolve(const char *host, struct in_addr *out) {
    struct hostent *he;

    if (!host || !host[0] || !out) {
        return -1;
    }
    if (inet_aton(host, out) == 1) {
        return 0; // 已是 IPv4 字面量
    }
    he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
        return -1;
    }
    memcpy(out, he->h_addr_list[0], sizeof(*out));
    return 0;
}

// 向单个 NTP 服务器查询当前时间；成功返回 0 并填充 out_sec（Unix 秒）。
static int sntp_query(const char *host, int timeout_ms, time_t *out_sec) {
    struct sockaddr_in srv;
    struct in_addr ip;
    struct timeval tv;
    uint8_t pkt[SNTP_PACKET_LEN];
    uint8_t req_xmit[4];
    struct timespec ts;
    ssize_t n;
    uint32_t ntp_sec_be;
    uint64_t ntp_sec;
    int fd = -1;
    int ret = -1;

    if (sntp_resolve(host, &ip) != 0) {
        LOGW(__func__, "resolve %s failed errno=%d", host, errno);
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOGW(__func__, "socket failed errno=%d", errno);
        return -1;
    }

    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(SNTP_PORT);
    srv.sin_addr = ip;

    // 接收超时，避免服务器无响应时长时间阻塞启动流程。
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 请求：LI=0 VN=4 Mode=3，transmit 时间戳填本地时间（服务器一般不校验）。
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (4 << 3) | SNTP_MODE_CLIENT;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 0) {
        ntp_sec_be = htonl((uint32_t)ts.tv_sec + SNTP_EPOCH_OFFSET);
        memcpy(pkt + 40, &ntp_sec_be, sizeof(ntp_sec_be));
    }
    memcpy(req_xmit, pkt + 40, sizeof(req_xmit));

    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        LOGW(__func__, "sendto %s failed errno=%d", host, errno);
        goto done;
    }

    n = recvfrom(fd, pkt, sizeof(pkt), 0, NULL, NULL);
    if (n < SNTP_PACKET_LEN) {
        LOGW(__func__, "recvfrom %s timeout/error n=%zd errno=%d", host, n, errno);
        goto done;
    }

    // 响应校验：服务器模式、时钟已同步、stratum 有效、transmit 时间戳非零、
    // origin 时间戳与请求一致（排除陈旧/异常响应）。
    if ((pkt[0] & 0x07) != SNTP_MODE_SERVER) {
        LOGW(__func__, "bad mode=%d from %s", pkt[0] & 0x07, host);
        goto done;
    }
    if (((pkt[0] >> 6) & 0x03) == 3) {
        LOGW(__func__, "server %s clock unsynchronized", host);
        goto done;
    }
    if (pkt[1] == 0 || pkt[1] >= 16) { // stratum 0=KoD/未初始化，>=16 非法
        LOGW(__func__, "server %s stratum=%u invalid", host, pkt[1]);
        goto done;
    }
    if (memcmp(pkt + 24, req_xmit, sizeof(req_xmit)) != 0) {
        LOGW(__func__, "origin mismatch from %s", host);
        goto done;
    }
    ntp_sec = zh_read_be32(pkt + 40);
    if (ntp_sec < SNTP_EPOCH_OFFSET) {
        ntp_sec += 0x100000000ULL; // 2036 年 NTP 纪元回绕
    }
    *out_sec = (time_t)(ntp_sec - SNTP_EPOCH_OFFSET);
    ret = 0;

done:
    close(fd);
    return ret;
}

int zh_sntp_sync(const char *servers, int timeout_ms) {
    char list[512];
    char *save = NULL;
    char *tok;
    time_t now;
    struct timespec ts;
    int ret = -1;

    if (!servers || !servers[0]) {
        servers = ZH_NTP_SERVERS;
    }
    if (timeout_ms <= 0) {
        timeout_ms = ZH_NTP_TIMEOUT_MS;
    }

    snprintf(list, sizeof(list), "%s", servers);
    for (tok = strtok_r(list, " \t,", &save); tok != NULL;
         tok = strtok_r(NULL, " \t,", &save)) {
        if (sntp_query(tok, timeout_ms, &now) != 0) {
            continue;
        }
        ts.tv_sec = now;
        ts.tv_nsec = 0;
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            LOGW(__func__, "clock_settime %lld failed errno=%d",
                 (long long)now, errno);
            continue;
        }
        LOGI(__func__, "time synced via %s: %lld", tok, (long long)now);
        ret = 0;
        break;
    }
    if (ret != 0) {
        LOGW(__func__, "sntp sync failed for all servers: %s", servers);
    }
    return ret;
}
