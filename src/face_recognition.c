#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "bithion_core.h"
#include "core_json_compat.h"
#include "face_recognition.h"
#include "log.h"
#include "utils.h"
#include "ws.h"

#define ZH_FACE_ENGINE_SOCK_PATH "/tmp/face_engine.sock"
#define ZH_FACE_COMPARE_INTERVAL_MS 400 // 每次人脸识别间隔时间
#define ZH_FACE_ENROLL_SAMPLE_COUNT 5 // 人脸录入的次数
#define ZH_FACE_ENROLL_SAMPLE_DELAY_MS 100 // 人脸录入延迟
#define ZH_FACE_CACHE_MAX 64
#define ZH_FACE_CAPTURE_INTERVAL_MS 10 // 拍照间隔
#define ZH_FACE_REPORT_GRACE_MS (10 * 1000) // VAD 结束后继续上报人脸状态的宽限期
#define ZH_FACE_CAPTURE_DIR ZH_WORK_BASE "face/capture_debug/"
#define ZH_FACE_CAPTURE_W 640 // 0=原始宽（仅传一个维度时另一维按比例推算）
#define ZH_FACE_CAPTURE_H 480 // 0=原始高
#define ZH_FACE_CAPTURE_Q 85 // JPEG质量 1~100
#define ZH_FACE_CAPTURE_WARMUP_FRAMES 4 // 抓拍前丢弃帧数
#define ZH_FACE_CAPTURE_CONTENT_TYPE "image/jpeg"

static pthread_t g_face_thread;
static pthread_t g_face_capture_thread;
static pthread_mutex_t g_face_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_face_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_face_engine_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_face_running = 0;
static int g_face_active = 0;
static uint64_t g_face_report_until_ms = 0;
static int g_face_thread_started = 0;
static int g_face_capture_running = 0;
static int g_face_capture_thread_started = 0;
static zh_ws_session_t *g_face_ws = NULL;

static int g_face_uds_fd = -1;
static pid_t g_face_engine_pid = -1;

static pthread_mutex_t g_face_enroll_mutex = PTHREAD_MUTEX_INITIALIZER;
static float g_face_enroll_emb[512];
static int g_face_enroll_valid = 0;
static char g_face_enroll_err[128];

typedef struct {
    char path[512];
    char name[128];
} zh_face_cache_item_t;

static zh_face_cache_item_t g_face_cache[ZH_FACE_CACHE_MAX];
static int g_face_cache_pos = 0;
static pthread_mutex_t g_face_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void zh_face_cleanup_engine_locked(void);
static int zh_face_prepare_engine_locked(void);
static uint64_t zh_wall_time_ms(void);
static int zh_face_detect_name_for_commit(char *out, size_t out_len);

static uint64_t zh_face_report_deadline_ms(int active, uint64_t now_ms) {
    return active ? 0 : (now_ms + ZH_FACE_REPORT_GRACE_MS);
}

static int zh_face_report_should_run(int active, uint64_t report_until_ms, uint64_t now_ms) {
    return active || report_until_ms > now_ms;
}

static uint64_t zh_face_report_sleep_ms(int active,
                                        uint64_t report_until_ms,
                                        uint64_t now_ms,
                                        uint64_t interval_ms) {
    uint64_t remaining_ms;

    if (active) {
        return interval_ms;
    }
    if (report_until_ms <= now_ms) {
        return 0;
    }

    remaining_ms = report_until_ms - now_ms;
    return remaining_ms < interval_ms ? remaining_ms : interval_ms;
}

#ifdef ZH_FACE_TEST_HOOKS
uint64_t zh_face_test_report_deadline_ms(int active, uint64_t now_ms) {
    return zh_face_report_deadline_ms(active, now_ms);
}

int zh_face_test_report_should_run(int active, uint64_t report_until_ms, uint64_t now_ms) {
    return zh_face_report_should_run(active, report_until_ms, now_ms);
}

uint64_t zh_face_test_report_sleep_ms(int active,
                                      uint64_t report_until_ms,
                                      uint64_t now_ms,
                                      uint64_t interval_ms) {
    return zh_face_report_sleep_ms(active, report_until_ms, now_ms, interval_ms);
}

void zh_face_test_reset_runtime_state(void) {
    g_face_running = 0;
    g_face_active = 0;
    g_face_report_until_ms = 0;
}

uint64_t zh_face_test_report_until_ms(void) {
    return g_face_report_until_ms;
}
#endif

static pid_t zh_start_face_engine_uds(void) {
    pid_t pid = fork();
    if (pid < 0) {
        LOGE(__func__, "fork face_engine failed errno=%d", errno);
        return -1;
    }
    if (pid == 0) {
        char uds_arg[256];
        pid_t ppid = getppid();

        // 父进程异常退出时，让 face_engine 也收到 SIGKILL，尽量避免残留。
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
            LOGE(__func__, "prctl PR_SET_PDEATHSIG failed errno=%d", errno);
            _exit(127);
        }
        if (ppid > 1 && getppid() != ppid) {
            LOGE(__func__, "parent changed before exec old_ppid=%d new_ppid=%d",
                 (int)ppid, (int)getppid());
            _exit(127);
        }
        snprintf(uds_arg, sizeof(uds_arg), "--uds_path=%s", ZH_FACE_ENGINE_SOCK_PATH);
        execl(ZH_FACE_ENGINE_PATH,
              ZH_FACE_ENGINE_PATH,
              "--uds",
              uds_arg,
              (char *)NULL);
        LOGE(__func__, "execl %s failed errno=%d", ZH_FACE_ENGINE_PATH, errno);
        _exit(127);
    }
    return pid;
}

static int zh_uds_connect(const char *path, int timeout_ms) {
    int fd;
    int last_errno = 0;
    struct sockaddr_un addr;
    uint64_t start = zh_now_ms();

    for (;;) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        last_errno = errno;
        close(fd);
        if ((int)(zh_now_ms() - start) >= timeout_ms) {
            break;
        }
        usleep(50 * 1000);
    }
    errno = last_errno;
    return -1;
}

static int zh_uds_send_cmd(int fd, const char *cmd, char *out, size_t out_len) {
    size_t len;
    size_t used = 0;
    char ch;
    ssize_t n;

    if (!cmd || !out || out_len == 0) {
        return -1;
    }

    len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        return -1;
    }

    while ((n = read(fd, &ch, 1)) == 1) {
        if (ch == '\n') {
            break;
        }
        if (used + 1 < out_len) {
            out[used++] = ch;
        }
    }
    out[used] = '\0';
    return (used > 0) ? 0 : -1;
}

static int zh_uds_read_line(int fd, char *out, size_t out_len) {
    size_t used = 0;
    char ch;
    ssize_t n;

    if (!out || out_len == 0) {
        return -1;
    }

    while ((n = read(fd, &ch, 1)) == 1) {
        if (ch == '\n') {
            break;
        }
        if (used + 1 < out_len) {
            out[used++] = ch;
        }
    }
    out[used] = '\0';
    return (used > 0) ? 0 : -1;
}

static int zh_uds_read_exact(int fd, void *buf, size_t len) {
    size_t used = 0;
    uint8_t *p = (uint8_t *)buf;
    while (used < len) {
        ssize_t n = read(fd, p + used, len - used);
        if (n <= 0) {
            return -1;
        }
        used += (size_t)n;
    }
    return 0;
}

static int zh_parse_err(const char *json, char *out, size_t out_len) {
    bithion_core_json_view_t s;
    char *err;
    if (!json || !out || out_len == 0) return -1;
    s.buf = (char *)json;
    s.len = strlen(json);
    err = mg_json_get_str(s, "$.err");
    if (!err) return -1;
    snprintf(out, out_len, "%s", err);
    free(err);
    return 0;
}

static int zh_parse_emb(const char *json, float *emb, size_t emb_len) {
    bithion_core_json_view_t s;
    bithion_core_json_view_t arr;
    bithion_core_json_view_t val;
    size_t ofs = 0;
    size_t i = 0;

    if (!json || !emb || emb_len == 0) return -1;
    s.buf = (char *)json;
    s.len = strlen(json);

    arr = mg_json_get_tok(s, "$.emb");
    if (arr.buf == NULL || arr.len == 0 || arr.buf[0] != '[') return -1;

    while ((ofs = mg_json_next(arr, ofs, NULL, &val)) > 0) {
        char tmp[64];
        char *end;
        size_t n = val.len < sizeof(tmp) - 1 ? val.len : sizeof(tmp) - 1;
        memcpy(tmp, val.buf, n);
        tmp[n] = '\0';
        if (i >= emb_len) return -1;
        emb[i] = (float)strtod(tmp, &end);
        if (end == tmp) return -1;
        i++;
    }
    return (i == emb_len) ? 0 : -1;
}

static int zh_capture_photo_bin_locked(uint8_t **out_jpeg,
                                       size_t *out_size,
                                       int *out_w,
                                       int *out_h,
                                       int *out_q,
                                       char *err_buf,
                                       size_t err_len) {
    char header[1024];
    char cmd[128];
    bithion_core_json_view_t s;
    double ok = 0;
    double size_num = 0;
    double w_num = 0;
    double h_num = 0;
    double q_num = 0;
    uint8_t *jpeg = NULL;
    size_t size = 0;

    if (!out_jpeg || !out_size) return -1;
    *out_jpeg = NULL;
    *out_size = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_q) *out_q = 0;

    if (zh_face_prepare_engine_locked() != 0) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "engine_prepare_failed");
        }
        return -1;
    }
    snprintf(cmd, sizeof(cmd), "capture_photo_bin %d %d %d %d",
             ZH_FACE_CAPTURE_W, ZH_FACE_CAPTURE_H, ZH_FACE_CAPTURE_Q,
             ZH_FACE_CAPTURE_WARMUP_FRAMES);
    if (zh_uds_send_cmd(g_face_uds_fd, cmd, header, sizeof(header)) != 0) {
        return -1;
    }

    s.buf = header;
    s.len = strlen(header);
    if (!mg_json_get_num(s, "$.ok", &ok) || (int)ok != 1) {
        if (err_buf && err_len > 0) {
            if (zh_parse_err(header, err_buf, err_len) != 0) {
                snprintf(err_buf, err_len, "capture_failed");
            }
        }
        return -1;
    }
    if (!mg_json_get_num(s, "$.size", &size_num) || size_num <= 0) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "invalid_size");
        }
        return -1;
    }

    size = (size_t)size_num;
    if ((double)size != size_num || size > (10 * 1024 * 1024)) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "size_out_of_range");
        }
        return -1;
    }
    if (mg_json_get_num(s, "$.w", &w_num) && out_w) {
        *out_w = (int)w_num;
    }
    if (mg_json_get_num(s, "$.h", &h_num) && out_h) {
        *out_h = (int)h_num;
    }
    if (mg_json_get_num(s, "$.q", &q_num) && out_q) {
        *out_q = (int)q_num;
    }

    jpeg = (uint8_t *)malloc(size);
    if (!jpeg) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "malloc_failed");
        }
        return -1;
    }
    if (zh_uds_read_exact(g_face_uds_fd, jpeg, size) != 0) {
        free(jpeg);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "read_jpeg_failed");
        }
        return -1;
    }

    *out_jpeg = jpeg;
    *out_size = size;
    return 0;
}

static int zh_mkdir_one(const char *dir_path) {
    struct stat st;

    if (mkdir(dir_path, 0755) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }
    if (stat(dir_path, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int zh_mkdir_recursive(const char *dir_path) {
    char tmp[512];
    size_t len;
    char *p;

    if (!dir_path) return -1;
    len = strlen(dir_path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (zh_mkdir_one(tmp) != 0) {
                return -1;
            }
            *p = '/';
        }
    }
    if (zh_mkdir_one(tmp) != 0) {
        return -1;
    }
    return 0;
}

#ifdef ZH_FACE_TEST_HOOKS
int zh_face_test_mkdir_recursive(const char *dir_path) {
    return zh_mkdir_recursive(dir_path);
}
#endif

static int zh_save_photo_jpeg(const uint8_t *jpeg, size_t size, char *out_path, size_t out_len) {
    char path[512];
    FILE *fp;
    if (!jpeg || size == 0) return -1;
    if (zh_mkdir_recursive(ZH_FACE_CAPTURE_DIR) != 0) {
        LOGE(__func__, "mkdir failed: %s errno=%d", ZH_FACE_CAPTURE_DIR, errno);
        return -1;
    }
    snprintf(path, sizeof(path), "%scapture.jpg",
             ZH_FACE_CAPTURE_DIR);
    fp = fopen(path, "wb");
    if (!fp) {
        LOGE(__func__, "open file failed: %s errno=%d", path, errno);
        return -1;
    }
    if (fwrite(jpeg, 1, size, fp) != size) {
        LOGE(__func__, "write file failed: %s errno=%d", path, errno);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (out_path && out_len > 0) {
        snprintf(out_path, out_len, "%s", path);
    }
    return 0;
}

typedef struct {
    char stage[32];
    int status_code;
    int err_no;
    char err_body[256];
} zh_put_diag_t;

static void zh_put_diag_reset(zh_put_diag_t *diag) {
    if (!diag) return;
    memset(diag, 0, sizeof(*diag));
    snprintf(diag->stage, sizeof(diag->stage), "%s", "unknown");
}

static int zh_write_all_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t used = 0;
    while (used < len) {
        ssize_t n = write(fd, p + used, len - used);
        if (n <= 0) {
            return -1;
        }
        used += (size_t)n;
    }
    return 0;
}

static int zh_parse_https_url(const char *url,
                              char *host,
                              size_t host_len,
                              int *port,
                              char *path_q,
                              size_t path_q_len) {
    const char *scheme = "https://";
    const char *p;
    const char *slash;
    const char *host_end;
    const char *colon;
    char host_port[320];
    size_t host_port_len;
    size_t path_len;

    if (!url || !host || host_len == 0 || !port || !path_q || path_q_len == 0) {
        return -1;
    }
    if (strncmp(url, scheme, strlen(scheme)) != 0) {
        return -1;
    }
    p = url + strlen(scheme);
    if (*p == '\0') {
        return -1;
    }

    slash = strchr(p, '/');
    host_end = slash ? slash : (url + strlen(url));
    host_port_len = (size_t)(host_end - p);
    if (host_port_len == 0 || host_port_len >= sizeof(host_port)) {
        return -1;
    }
    memcpy(host_port, p, host_port_len);
    host_port[host_port_len] = '\0';

    colon = strrchr(host_port, ':');
    *port = 443;
    if (colon && *(colon + 1) != '\0') {
        long parsed = strtol(colon + 1, NULL, 10);
        if (parsed <= 0 || parsed > 65535) {
            return -1;
        }
        *port = (int)parsed;
        host_port[colon - host_port] = '\0';
    }
    if (host_port[0] == '\0' || strlen(host_port) >= host_len) {
        return -1;
    }
    snprintf(host, host_len, "%s", host_port);

    if (!slash) {
        snprintf(path_q, path_q_len, "%s", "/");
        return 0;
    }
    path_len = strlen(slash);
    if (path_len >= path_q_len) {
        return -1;
    }
    snprintf(path_q, path_q_len, "%s", slash);
    return 0;
}

static int zh_parse_http_status_from_file(const char *path, int *out_status, char *out_snippet, size_t out_snippet_len) {
    FILE *fp;
    char line[512];
    int line_count = 0;

    if (out_status) *out_status = 0;
    if (out_snippet && out_snippet_len > 0) out_snippet[0] = '\0';
    if (!path || !out_status) return -1;

    fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    while (fgets(line, sizeof(line), fp) && line_count < 80) {
        int code = 0;
        line_count++;
        if (out_snippet && out_snippet_len > 0 && out_snippet[0] == '\0') {
            size_t ln = strlen(line);
            while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) {
                line[--ln] = '\0';
            }
            if (ln > 0) {
                snprintf(out_snippet, out_snippet_len, "%s", line);
            }
        }
        if (sscanf(line, "HTTP/%*d.%*d %d", &code) == 1 && code > 0) {
            *out_status = code;
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

static int zh_http_put_binary_via_ssl_client(const char *url,
                                             const char *content_type,
                                             const uint8_t *body,
                                             size_t body_len,
                                             int with_content_type,
                                             zh_put_diag_t *diag) {
    char host[256];
    char path_q[2048];
    int port = 443;
    char req_template[] = "/tmp/zh_vision_ssl_req_XXXXXX";
    char resp_template[] = "/tmp/zh_vision_ssl_resp_XXXXXX";
    int req_fd = -1;
    int resp_fd = -1;
    int out_fd = -1;
    pid_t pid;
    int wstatus = 0;
    int resp_status = 0;
    char resp_snippet[256];
    char connect_arg[320];
    char header[4096];
    int header_len;

    if (!url || !body || body_len == 0 || !diag) {
        return -1;
    }
    if (zh_parse_https_url(url, host, sizeof(host), &port, path_q, sizeof(path_q)) != 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_parse_url");
        diag->err_no = EINVAL;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", "invalid https url");
        return -1;
    }

    req_fd = mkstemp(req_template);
    if (req_fd < 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_req_tmp");
        diag->err_no = errno;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", strerror(errno));
        return -1;
    }
    resp_fd = mkstemp(resp_template);
    if (resp_fd < 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_resp_tmp");
        diag->err_no = errno;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", strerror(errno));
        close(req_fd);
        unlink(req_template);
        return -1;
    }
    close(resp_fd);
    resp_fd = -1;

    if (with_content_type) {
        header_len = snprintf(header, sizeof(header),
                              "PUT %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              path_q, host, content_type ? content_type : "application/octet-stream", body_len);
    } else {
        header_len = snprintf(header, sizeof(header),
                              "PUT %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              path_q, host, body_len);
    }
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_header");
        diag->err_no = EOVERFLOW;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", "header too long");
        close(req_fd);
        unlink(req_template);
        unlink(resp_template);
        return -1;
    }
    if (zh_write_all_fd(req_fd, header, (size_t)header_len) != 0 ||
        zh_write_all_fd(req_fd, body, body_len) != 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_req_write");
        diag->err_no = errno;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", strerror(errno));
        close(req_fd);
        unlink(req_template);
        unlink(resp_template);
        return -1;
    }
    if (lseek(req_fd, 0, SEEK_SET) < 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_req_seek");
        diag->err_no = errno;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", strerror(errno));
        close(req_fd);
        unlink(req_template);
        unlink(resp_template);
        return -1;
    }

    if (port == 443) {
        snprintf(connect_arg, sizeof(connect_arg), "%s", host);
    } else {
        snprintf(connect_arg, sizeof(connect_arg), "%s:%d", host, port);
    }
    pid = fork();
    if (pid < 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_fork");
        diag->err_no = errno;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", strerror(errno));
        close(req_fd);
        unlink(req_template);
        unlink(resp_template);
        return -1;
    }
    if (pid == 0) {
        out_fd = open(resp_template, O_WRONLY | O_TRUNC);
        if (out_fd < 0) _exit(127);
        if (dup2(req_fd, STDIN_FILENO) < 0) _exit(127);
        if (dup2(out_fd, STDOUT_FILENO) < 0) _exit(127);
        if (dup2(out_fd, STDERR_FILENO) < 0) _exit(127);
        close(req_fd);
        close(out_fd);
        execlp("ssl_client", "ssl_client", connect_arg, (char *)NULL);
        _exit(127);
    }

    close(req_fd);
    req_fd = -1;
    waitpid(pid, &wstatus, 0);
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_exec");
        diag->err_no = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 255;
        if (zh_parse_http_status_from_file(resp_template, &resp_status, resp_snippet, sizeof(resp_snippet)) == 0) {
            diag->status_code = resp_status;
            snprintf(diag->err_body, sizeof(diag->err_body), "%s", resp_snippet[0] ? resp_snippet : "ssl_client exit");
        } else {
            snprintf(diag->err_body, sizeof(diag->err_body), "%s",
                     resp_snippet[0] ? resp_snippet : "ssl_client exit");
        }
        unlink(req_template);
        unlink(resp_template);
        return -1;
    }

    if (zh_parse_http_status_from_file(resp_template, &resp_status, resp_snippet, sizeof(resp_snippet)) != 0) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_parse_status");
        diag->err_no = EPROTO;
        snprintf(diag->err_body, sizeof(diag->err_body), "%s", resp_snippet[0] ? resp_snippet : "no http status");
        unlink(req_template);
        unlink(resp_template);
        return -1;
    }
    diag->status_code = resp_status;
    if (resp_status >= 200 && resp_status < 300) {
        snprintf(diag->stage, sizeof(diag->stage), "%s", "ssl_ok");
        unlink(req_template);
        unlink(resp_template);
        return 0;
    }
    snprintf(diag->stage, sizeof(diag->stage), "%s", with_content_type ? "ssl_http" : "ssl_http_retry");
    snprintf(diag->err_body, sizeof(diag->err_body), "%s", resp_snippet[0] ? resp_snippet : "http non-2xx");
    unlink(req_template);
    unlink(resp_template);
    return -1;
}

static int zh_http_put_binary(const char *url,
                              const char *content_type,
                              const uint8_t *body,
                              size_t body_len,
                              const int *running_flag,
                              zh_put_diag_t *diag) {
    zh_put_diag_reset(diag);
    (void)running_flag;
    if (!url || !body || body_len == 0) {
        if (diag) {
            snprintf(diag->stage, sizeof(diag->stage), "%s", "invalid_args");
            diag->err_no = EINVAL;
            snprintf(diag->err_body, sizeof(diag->err_body), "%s", "invalid args");
        }
        return -1;
    }
    if (diag && zh_http_put_binary_via_ssl_client(url, content_type, body, body_len, 1, diag) == 0) {
        return 0;
    }
    if (diag && diag->status_code == 400 &&
        zh_http_put_binary_via_ssl_client(url, content_type, body, body_len, 0, diag) == 0) {
        return 0;
    }
    return -1;
}

static void zh_make_frame_id(char *out, size_t out_len) {
    static unsigned int seq = 0;
    uint64_t now = zh_wall_time_ms();
    if (!out || out_len == 0) return;
    seq++;
    snprintf(out, out_len, "f_%llu_%u", (unsigned long long)now, seq);
}

static int zh_face_upload_one_frame(const uint8_t *jpeg, size_t jpeg_size) {
    zh_ws_session_t *ws = g_face_ws;
    zh_ws_vision_upload_response_t upload_resp;
    zh_ws_vision_done_t done;
    zh_put_diag_t put_diag;
    const char *device_id;
    char frame_id[128];
    char face_name[128];

    if (!ws || !jpeg || jpeg_size == 0) return -1;

    device_id = zh_ws_get_device_id(ws);
    if (!device_id || device_id[0] == '\0') {
        LOGE(__func__, "device_id unavailable");
        return -1;
    }

    zh_make_frame_id(frame_id, sizeof(frame_id));

    if (zh_ws_send_vision_upload_request(ws, device_id, frame_id, ZH_FACE_CAPTURE_CONTENT_TYPE) != 0) {
        LOGE(__func__, "send vision_upload_request failed frame_id=%s", frame_id);
        return -1;
    }
    if (zh_ws_wait_vision_upload_response(ws, &upload_resp, &g_face_capture_running) != 0) {
        LOGE(__func__, "wait vision_upload_response failed frame_id=%s", frame_id);
        return -1;
    }
    if (strcmp(upload_resp.status, "ok") != 0 ||
        upload_resp.upload_url[0] == '\0' ||
        upload_resp.upload_id[0] == '\0' ||
        upload_resp.object_key[0] == '\0') {
        LOGE(__func__, "vision_upload_response invalid frame_id=%s status=%s",
             frame_id, upload_resp.status);
        return -1;
    }

    {
        int put_ok = 0;
        int try_idx;
        const int put_retry_max = 30;
        const int put_retry_interval_ms = 100;

        LOGI(__func__,
             "[vision] frame put begin frame_id=%s upload_id=%s object_key=%s",
             frame_id,
             upload_resp.upload_id,
             upload_resp.object_key);

        for (try_idx = 1; try_idx <= put_retry_max; ++try_idx) {
            zh_put_diag_reset(&put_diag);
            if (zh_http_put_binary(upload_resp.upload_url, ZH_FACE_CAPTURE_CONTENT_TYPE,
                                   jpeg, jpeg_size, &g_face_capture_running, &put_diag) == 0) {
                LOGI(__func__,
                     "[vision] frame put success frame_id=%s upload_id=%s try=%d/%d",
                     frame_id,
                     upload_resp.upload_id,
                     try_idx,
                     put_retry_max);
                put_ok = 1;
                break;
            }
            LOGW(__func__,
                 "[vision] frame put retry frame_id=%s upload_id=%s try=%d/%d next_in=%dms "
                 "reason_stage=%s status=%d errno=%d(%s) body=%s",
                 frame_id,
                 upload_resp.upload_id,
                 try_idx,
                 put_retry_max,
                 put_retry_interval_ms,
                 put_diag.stage[0] ? put_diag.stage : "unknown",
                 put_diag.status_code,
                 put_diag.err_no,
                 put_diag.err_no ? strerror(put_diag.err_no) : "none",
                 put_diag.err_body[0] ? put_diag.err_body : "empty");
            if (!g_face_capture_running) {
                break;
            }
            usleep(put_retry_interval_ms * 1000);
        }

        if (!put_ok) {
            LOGE(__func__,
                 "[vision] frame put failed after retries frame_id=%s upload_id=%s object_key=%s max_retry=%d",
                 frame_id,
                 upload_resp.upload_id,
                 upload_resp.object_key,
                 put_retry_max);
            return -1;
        }
    }

    if (zh_face_detect_name_for_commit(face_name, sizeof(face_name)) != 0) {
        snprintf(face_name, sizeof(face_name), "error");
    }

    if (zh_ws_send_vision_upload_commit(ws, device_id, frame_id,
                                        upload_resp.upload_id,
                                        upload_resp.object_key,
                                        face_name) != 0) {
        LOGE(__func__, "send vision_upload_commit failed frame_id=%s", frame_id);
        return -1;
    }
    if (zh_ws_wait_vision_done(ws, frame_id, &done, &g_face_capture_running) != 0) {
        LOGE(__func__, "wait vision_done failed frame_id=%s", frame_id);
        return -1;
    }
    if (strcmp(done.result, "ok") != 0) {
        LOGE(__func__, "vision_done error frame_id=%s error_code=%s",
             frame_id, done.error_code);
        return -1;
    }

    LOGI(__func__, "vision frame done frame_id=%s upload_id=%s", frame_id, upload_resp.upload_id);
    return 0;
}

static size_t zh_json_escape(const char *in, char *out, size_t out_len) {
    size_t used = 0;
    if (!in || !out || out_len == 0) return 0;
    while (*in) {
        char c = *in++;
        const char *rep = NULL;
        char tmp[2] = {0, 0};
        switch (c) {
            case '\\': rep = "\\\\"; break;
            case '"': rep = "\\\""; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default: tmp[0] = c; rep = tmp; break;
        }
        while (*rep) {
            if (used + 1 >= out_len) {
                out[used] = '\0';
                return used;
            }
            out[used++] = *rep++;
        }
    }
    out[used] = '\0';
    return used;
}

static void zh_face_cache_set(const char *path, const char *name) {
    if (!path || !name) return;
    pthread_mutex_lock(&g_face_cache_mutex);
    snprintf(g_face_cache[g_face_cache_pos].path,
             sizeof(g_face_cache[g_face_cache_pos].path), "%s", path);
    snprintf(g_face_cache[g_face_cache_pos].name,
             sizeof(g_face_cache[g_face_cache_pos].name), "%s", name);
    g_face_cache_pos = (g_face_cache_pos + 1) % ZH_FACE_CACHE_MAX;
    pthread_mutex_unlock(&g_face_cache_mutex);
}

static int zh_face_cache_get(const char *path, char *out, size_t out_len) {
    int i;
    if (!path || !out || out_len == 0) return -1;
    pthread_mutex_lock(&g_face_cache_mutex);
    for (i = 0; i < ZH_FACE_CACHE_MAX; ++i) {
        if (g_face_cache[i].path[0] != '\0' &&
            strcmp(g_face_cache[i].path, path) == 0) {
            snprintf(out, out_len, "%s", g_face_cache[i].name);
            pthread_mutex_unlock(&g_face_cache_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_face_cache_mutex);
    return -1;
}

static char *zh_read_file_dyn(const char *path, size_t *out_len) {
    FILE *fp;
    long len;
    char *buf;
    size_t n;

    if (!path) return NULL;
    fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    len = ftell(fp);
    if (len <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if (n != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

static int zh_index_find_name(const char *index_path,
                              const char *emb_path,
                              char *out,
                              size_t out_len) {
    size_t len = 0;
    char *buf;
    bithion_core_json_view_t s;
    size_t ofs = 0;
    bithion_core_json_view_t val;

    if (!index_path || !emb_path || !out || out_len == 0) return -1;
    if (zh_face_cache_get(emb_path, out, out_len) == 0) {
        return 0;
    }
    buf = zh_read_file_dyn(index_path, &len);
    if (!buf) return -1;
    s.buf = buf;
    s.len = len;
    if (s.len == 0 || s.buf[0] != '[') {
        free(buf);
        return -1;
    }
    while ((ofs = mg_json_next(s, ofs, NULL, &val)) > 0) {
        char *path = mg_json_get_str(val, "$.path");
        if (path) {
            if (strcmp(path, emb_path) == 0) {
                char *name = mg_json_get_str(val, "$.name");
                if (name) {
                    snprintf(out, out_len, "%s", name);
                    zh_face_cache_set(emb_path, name);
                    free(name);
                    free(path);
                    free(buf);
                    return 0;
                }
            }
            free(path);
        }
    }
    free(buf);
    return -1;
}

static uint64_t zh_wall_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void zh_format_datetime(char *out, size_t out_len, time_t now) {
    struct tm tm_buf;
    struct tm *tm_now = localtime_r(&now, &tm_buf);
    if (!tm_now) {
        snprintf(out, out_len, "1970-01-01 00:00:00");
        return;
    }
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", tm_now);
}

static int zh_ws_report_face(const char *status, int status_code, const char *face_id) {
    char face_id_esc[256];
    char datetime[32];
    char json[512];
    uint64_t ts_ms = zh_wall_time_ms();
    time_t now = time(NULL);

    if (!status) return -1;
    if (!face_id) face_id = "";

    zh_json_escape(face_id, face_id_esc, sizeof(face_id_esc));
    zh_format_datetime(datetime, sizeof(datetime), now);

    snprintf(json, sizeof(json),
             "{\"face_recognition_result\":{\"status_code\":%d,\"status\":\"%s\",\"face_id\":\"%s\",\"timestamp\":%llu,\"datetime\":\"%s\"}}",
             status_code, status, face_id_esc,
             (unsigned long long)ts_ms, datetime);

    if (!g_face_ws) return 0;
    return zh_ws_send_str(g_face_ws, json);
}

static int zh_write_emb_file(const float *emb, size_t emb_len,
                             const char *id,
                             char *out_path, size_t out_len) {
    char path[512];
    FILE *out;

    if (!emb || emb_len == 0 || !id || !out_path || out_len == 0) return -1;
    if (zh_mkdir_recursive(ZH_FACE_EMB_DIR) != 0) {
        LOGE(__func__, "mkdir %s failed errno=%d", ZH_FACE_EMB_DIR, errno);
        return -1;
    }

    snprintf(path, sizeof(path), "%s%s.emb", ZH_FACE_EMB_DIR, id);
    out = fopen(path, "wb");
    if (!out) {
        LOGE(__func__, "open emb file failed: %s errno=%d", path, errno);
        return -1;
    }
    if (fwrite(emb, sizeof(float), emb_len, out) != emb_len) {
        LOGE(__func__, "write emb file failed: %s errno=%d", path, errno);
        fclose(out);
        return -1;
    }
    fclose(out);
    snprintf(out_path, out_len, "%s", path);
    return 0;
}

static int zh_upsert_index_json(const char *index_path,
                                const char *id,
                                const char *name,
                                const char *emb_path,
                                long ts) {
    FILE *out;
    char *buf = NULL;
    size_t len = 0;
    bithion_core_json_view_t s;
    size_t ofs = 0;
    bithion_core_json_view_t val;
    int first = 1;
    char name_esc[256];
    char path_esc[512];

    if (!index_path || !id || !name || !emb_path) return -1;

    buf = zh_read_file_dyn(index_path, &len);

    out = fopen(index_path, "wb");
    if (!out) {
        free(buf);
        return -1;
    }

    fputc('[', out);
    if (buf && len > 0) {
        s.buf = buf;
        s.len = len;
        if (s.buf[0] == '[') {
            while ((ofs = mg_json_next(s, ofs, NULL, &val)) > 0) {
                char *old_name = mg_json_get_str(val, "$.name");
                char *old_id = NULL;
                char *old_path = NULL;
                double ts_num = 0;
                char entry[1024];
                char old_name_esc[256];
                char old_path_esc[512];

                if (!old_name) {
                    continue;
                }
                if (strcmp(old_name, name) == 0) {
                    free(old_name);
                    continue;
                }
                old_id = mg_json_get_str(val, "$.id");
                old_path = mg_json_get_str(val, "$.path");
                if (!old_id || !old_path || !mg_json_get_num(val, "$.ts", &ts_num)) {
                    free(old_name);
                    free(old_id);
                    free(old_path);
                    continue;
                }

                zh_json_escape(old_name, old_name_esc, sizeof(old_name_esc));
                zh_json_escape(old_path, old_path_esc, sizeof(old_path_esc));
                snprintf(entry, sizeof(entry),
                         "{\"id\":\"%s\",\"name\":\"%s\",\"path\":\"%s\",\"ts\":%ld}",
                         old_id, old_name_esc, old_path_esc, (long)ts_num);

                if (!first) fputc(',', out);
                fputs(entry, out);
                first = 0;

                free(old_name);
                free(old_id);
                free(old_path);
            }
        }
    }
    free(buf);

    zh_json_escape(name, name_esc, sizeof(name_esc));
    zh_json_escape(emb_path, path_esc, sizeof(path_esc));
    if (!first) fputc(',', out);
    fprintf(out,
            "{\"id\":\"%s\",\"name\":\"%s\",\"path\":\"%s\",\"ts\":%ld}",
            id, name_esc, path_esc, ts);
    fputs("]\n", out);
    fclose(out);
    return 0;
}

int zh_face_enroll_on_recog(void) {
    char cmd[128];
    char json[8192];
    char err_buf[128];
    bithion_core_json_view_t s;
    double ok = 0;
    float emb[512];
    int rc = -1;

    err_buf[0] = '\0';
    snprintf(cmd, sizeof(cmd), "get_face_emb_avg %d %d",
             ZH_FACE_ENROLL_SAMPLE_COUNT, ZH_FACE_ENROLL_SAMPLE_DELAY_MS);

    pthread_mutex_lock(&g_face_engine_mutex);
    if (zh_face_prepare_engine_locked() != 0) {
        pthread_mutex_unlock(&g_face_engine_mutex);
        goto done;
    }
    if (zh_uds_send_cmd(g_face_uds_fd, cmd, json, sizeof(json)) != 0) {
        pthread_mutex_unlock(&g_face_engine_mutex);
        goto done;
    }
    pthread_mutex_unlock(&g_face_engine_mutex);

    s.buf = json;
    s.len = strlen(json);
    if (!mg_json_get_num(s, "$.ok", &ok) || (int)ok != 1) {
        if (zh_parse_err(json, err_buf, sizeof(err_buf)) == 0) {
            LOGI(__func__, "face enroll avg failed: %s", err_buf);
        } else {
            LOGI(__func__, "face enroll avg failed: %s", json);
        }
        goto done;
    }

    if (zh_parse_emb(json, emb, 512) != 0) {
        LOGE(__func__, "parse emb failed");
        goto done;
    }

    pthread_mutex_lock(&g_face_enroll_mutex);
    memcpy(g_face_enroll_emb, emb, sizeof(g_face_enroll_emb));
    g_face_enroll_valid = 1;
    g_face_enroll_err[0] = '\0';
    pthread_mutex_unlock(&g_face_enroll_mutex);
    rc = 0;

done:
    if (rc != 0) {
        pthread_mutex_lock(&g_face_enroll_mutex);
        g_face_enroll_valid = 0;
        if (err_buf[0] != '\0') {
            snprintf(g_face_enroll_err, sizeof(g_face_enroll_err), "%s", err_buf);
        } else {
            snprintf(g_face_enroll_err, sizeof(g_face_enroll_err), "no_face_or_failed");
        }
        pthread_mutex_unlock(&g_face_enroll_mutex);
    }
    return rc;
}

int zh_face_enroll_on_owner(const char *name) {
    float emb[512];
    int valid = 0;
    char path[512];
    char index_path[512];
    char id_buf[64];
    time_t now = time(NULL);
    int rc = -1;

    if (!name || name[0] == '\0') {
        zh_ws_report_face("face_collection_failed", 1004, "");
        return -1;
    }

    pthread_mutex_lock(&g_face_enroll_mutex);
    valid = g_face_enroll_valid;
    if (valid) {
        memcpy(emb, g_face_enroll_emb, sizeof(emb));
    }
    pthread_mutex_unlock(&g_face_enroll_mutex);

    if (!valid) {
        zh_ws_report_face("face_collection_failed", 1004, "");
        return -1;
    }

    snprintf(id_buf, sizeof(id_buf), "%llu",
             (unsigned long long)zh_wall_time_ms());
    if (zh_write_emb_file(emb, 512, id_buf, path, sizeof(path)) != 0) {
        goto done;
    }

    snprintf(index_path, sizeof(index_path), "%sindex.json", ZH_FACE_EMB_DIR);
    if (zh_upsert_index_json(index_path, id_buf, name, path, (long)now) != 0) {
        goto done;
    }

    zh_face_cache_set(path, name);
    rc = 0;

done:
    if (rc == 0) {
        zh_ws_report_face("face_collection_successful", 1003, "");
    } else {
        zh_ws_report_face("face_collection_failed", 1004, "");
    }
    return rc;
}

static int zh_face_do_compare_and_report(void) {
    char cmd[512];
    char compare_json[8192];
    bithion_core_json_view_t s;
    double ok = 0;
    char err_buf[128];

    snprintf(cmd, sizeof(cmd), "compare_face_emb_dir %s", ZH_FACE_EMB_DIR);

    pthread_mutex_lock(&g_face_engine_mutex);
    if (zh_face_prepare_engine_locked() != 0) {
        pthread_mutex_unlock(&g_face_engine_mutex);
        return -1;
    }
    if (zh_uds_send_cmd(g_face_uds_fd, cmd, compare_json, sizeof(compare_json)) != 0) {
        pthread_mutex_unlock(&g_face_engine_mutex);
        return -1;
    }
    pthread_mutex_unlock(&g_face_engine_mutex);

    s.buf = compare_json;
    s.len = strlen(compare_json);

    if (mg_json_get_num(s, "$.ok", &ok) && (int) ok == 1) {
        bithion_core_json_view_t results = mg_json_get_tok(s, "$.results");
        size_t ofs = 0;
        bithion_core_json_view_t item;
        double best_dist = 0;
        int best_has = 0;
        char best_name[128];
        char index_path[512];

        best_name[0] = '\0';
        if (results.buf == NULL || results.len == 0 || results.buf[0] != '[') {
            return 0;
        }

        snprintf(index_path, sizeof(index_path), "%sindex.json", ZH_FACE_EMB_DIR);

        while ((ofs = mg_json_next(results, ofs, NULL, &item)) > 0) {
            double item_ok = 0;
            double same = 0;
            double dist = 0;
            if (mg_json_get_num(item, "$.ok", &item_ok) && (int) item_ok == 1 &&
                mg_json_get_num(item, "$.dist", &dist)) {
                if (mg_json_get_num(item, "$.same", &same) && (int) same == 1) {
                    char *file = mg_json_get_str(item, "$.file");
                    if (file) {
                        char name_out[128];
                        if (zh_index_find_name(index_path, file, name_out, sizeof(name_out)) == 0) {
                            if (!best_has || dist < best_dist) {
                                snprintf(best_name, sizeof(best_name), "%s", name_out);
                                best_dist = dist;
                                best_has = 1;
                            }
                        }
                        free(file);
                    }
                }
            }
        }

        if (best_has) {
            return zh_ws_report_face("face_match", 1000, best_name);
        }
        return zh_ws_report_face("face_not_match", 1001, "");
    }

    if (zh_parse_err(compare_json, err_buf, sizeof(err_buf)) == 0) {
        if (strstr(err_buf, "no_face") != NULL) {
            return zh_ws_report_face("no_face", 1002, "");
        }
        if (strstr(err_buf, "open_dir_fail") != NULL) {
            if (zh_mkdir_recursive(ZH_FACE_EMB_DIR) == 0) {
                LOGW(__func__, "face emb dir recreated after engine error: %s",
                     ZH_FACE_EMB_DIR);
            } else {
                LOGE(__func__, "face emb dir unavailable: %s errno=%d",
                     ZH_FACE_EMB_DIR, errno);
            }
            // 特征库目录属于客户端数据状态，不应为此杀死并重启健康的引擎。
            return 0;
        }
        LOGE(__func__, "face_engine error: %s", err_buf);
    }

    return -1;
}

static int zh_face_detect_name_for_commit(char *out, size_t out_len) {
    char cmd[512];
    char compare_json[8192];
    bithion_core_json_view_t s;
    double ok = 0;
    char err_buf[128];

    if (!out || out_len == 0) return -1;
    snprintf(out, out_len, "error");
    snprintf(cmd, sizeof(cmd), "compare_face_emb_dir %s", ZH_FACE_EMB_DIR);

    pthread_mutex_lock(&g_face_engine_mutex);
    if (zh_face_prepare_engine_locked() != 0) {
        pthread_mutex_unlock(&g_face_engine_mutex);
        return -1;
    }
    if (zh_uds_send_cmd(g_face_uds_fd, cmd, compare_json, sizeof(compare_json)) != 0) {
        pthread_mutex_unlock(&g_face_engine_mutex);
        return -1;
    }
    pthread_mutex_unlock(&g_face_engine_mutex);

    s.buf = compare_json;
    s.len = strlen(compare_json);
    if (mg_json_get_num(s, "$.ok", &ok) && (int) ok == 1) {
        bithion_core_json_view_t results = mg_json_get_tok(s, "$.results");
        size_t ofs = 0;
        bithion_core_json_view_t item;
        double best_dist = 0;
        int best_has = 0;
        char best_name[128];
        char index_path[512];

        best_name[0] = '\0';
        if (results.buf == NULL || results.len == 0 || results.buf[0] != '[') {
            snprintf(out, out_len, "face_not_match");
            return 0;
        }

        snprintf(index_path, sizeof(index_path), "%sindex.json", ZH_FACE_EMB_DIR);
        while ((ofs = mg_json_next(results, ofs, NULL, &item)) > 0) {
            double item_ok = 0;
            double same = 0;
            double dist = 0;
            if (mg_json_get_num(item, "$.ok", &item_ok) && (int) item_ok == 1 &&
                mg_json_get_num(item, "$.dist", &dist)) {
                if (mg_json_get_num(item, "$.same", &same) && (int) same == 1) {
                    char *file = mg_json_get_str(item, "$.file");
                    if (file) {
                        char name_out[128];
                        if (zh_index_find_name(index_path, file, name_out, sizeof(name_out)) == 0) {
                            if (!best_has || dist < best_dist) {
                                snprintf(best_name, sizeof(best_name), "%s", name_out);
                                best_dist = dist;
                                best_has = 1;
                            }
                        }
                        free(file);
                    }
                }
            }
        }

        if (best_has) {
            snprintf(out, out_len, "%s", best_name);
        } else {
            snprintf(out, out_len, "face_not_match");
        }
        return 0;
    }

    if (zh_parse_err(compare_json, err_buf, sizeof(err_buf)) == 0 &&
        strstr(err_buf, "no_face") != NULL) {
        snprintf(out, out_len, "no_face");
        return 0;
    }

    return -1;
}

static void zh_face_cleanup_engine_locked(void) {
    if (g_face_uds_fd >= 0) {
        close(g_face_uds_fd);
        g_face_uds_fd = -1;
    }
    if (g_face_engine_pid > 0) {
        kill(g_face_engine_pid, SIGKILL);
        waitpid(g_face_engine_pid, NULL, 0);
        g_face_engine_pid = -1;
    }
}

static void zh_face_engine_check_child_locked(void) {
    int status;
    pid_t rc;

    if (g_face_engine_pid <= 0) {
        return;
    }

    rc = waitpid(g_face_engine_pid, &status, WNOHANG);
    if (rc == 0) {
        return;
    }
    if (rc < 0) {
        if (errno == ECHILD) {
            LOGW(__func__, "face_engine pid=%d no longer tracked by parent",
                 (int)g_face_engine_pid);
            g_face_engine_pid = -1;
        }
        return;
    }

    if (WIFEXITED(status)) {
        LOGE(__func__, "face_engine pid=%d exited rc=%d",
             (int)g_face_engine_pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        LOGE(__func__, "face_engine pid=%d killed by signal=%d",
             (int)g_face_engine_pid, WTERMSIG(status));
    } else {
        LOGW(__func__, "face_engine pid=%d changed state status=0x%x",
             (int)g_face_engine_pid, status);
    }
    g_face_engine_pid = -1;
}

static int zh_face_prepare_engine_locked(void) {
    char ready[256];

    zh_face_engine_check_child_locked();

    if (g_face_engine_pid <= 0) {
        g_face_engine_pid = zh_start_face_engine_uds();
        if (g_face_engine_pid <= 0) {
            LOGE(__func__, "start face_engine failed");
            return -1;
        }
    }

    if (g_face_uds_fd < 0) {
        g_face_uds_fd = zh_uds_connect(ZH_FACE_ENGINE_SOCK_PATH, 3000);
        if (g_face_uds_fd < 0) {
            LOGE(__func__, "connect uds failed: %s errno=%d pid=%d",
                 ZH_FACE_ENGINE_SOCK_PATH, errno, (int)g_face_engine_pid);
            // 连接失败时立即清掉坏状态，下一轮重试才能重新拉起 face_engine。
            zh_face_cleanup_engine_locked();
            return -1;
        }
        if (zh_uds_read_line(g_face_uds_fd, ready, sizeof(ready)) == 0) {
            LOGI(__func__, "face_engine ready: %s", ready);
        }
    }

    return 0;
}

static void zh_face_cleanup_engine(void) {
    pthread_mutex_lock(&g_face_engine_mutex);
    zh_face_cleanup_engine_locked();
    pthread_mutex_unlock(&g_face_engine_mutex);
}

static int zh_face_prepare_engine(void) {
    int rc;
    pthread_mutex_lock(&g_face_engine_mutex);
    rc = zh_face_prepare_engine_locked();
    pthread_mutex_unlock(&g_face_engine_mutex);
    return rc;
}

static int zh_face_should_report_locked(uint64_t now_ms) {
    return zh_face_report_should_run(g_face_active, g_face_report_until_ms, now_ms);
}

static uint64_t zh_face_report_interval_locked(uint64_t now_ms) {
    return zh_face_report_sleep_ms(g_face_active,
                                   g_face_report_until_ms,
                                   now_ms,
                                   ZH_FACE_COMPARE_INTERVAL_MS);
}

static void *zh_face_thread_main(void *arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&g_face_mutex);
        while (!zh_face_should_report_locked(zh_now_ms()) && g_face_running) {
            pthread_cond_wait(&g_face_cond, &g_face_mutex);
        }
        if (!g_face_running) {
            pthread_mutex_unlock(&g_face_mutex);
            break;
        }
        pthread_mutex_unlock(&g_face_mutex);

        if (zh_face_prepare_engine() != 0) {
            usleep(200 * 1000);
            continue;
        }

        while (1) {
            int should_report;
            uint64_t sleep_ms;
            uint64_t now_ms;

            pthread_mutex_lock(&g_face_mutex);
            now_ms = zh_now_ms();
            should_report = zh_face_should_report_locked(now_ms);
            sleep_ms = zh_face_report_interval_locked(now_ms);
            pthread_mutex_unlock(&g_face_mutex);
            if (!should_report || !g_face_running) {
                break;
            }

            uint64_t start_ms = zh_now_ms();
            if (zh_face_do_compare_and_report() != 0) {
                LOGE(__func__, "face compare/report failed");
                zh_face_cleanup_engine();
            }
            uint64_t cost_ms = zh_now_ms() - start_ms;
            if (cost_ms < sleep_ms) {
                usleep((sleep_ms - cost_ms) * 1000);
            }
        }
    }

    zh_face_cleanup_engine();
    return NULL;
}

static void *zh_face_capture_thread_main(void *arg) {
    (void)arg;

    while (g_face_capture_running) {
        if (!g_face_capture_running) {
            break;
        }

        uint64_t start_ms = zh_now_ms();
        uint8_t *jpeg = NULL;
        size_t jpeg_size = 0;
        int cap_w = 0;
        int cap_h = 0;
        int cap_q = 0;
        char err_buf[128];
        int rc;

        err_buf[0] = '\0';

        pthread_mutex_lock(&g_face_engine_mutex);
        rc = zh_capture_photo_bin_locked(&jpeg, &jpeg_size, &cap_w, &cap_h, &cap_q,
                                         err_buf, sizeof(err_buf));
        pthread_mutex_unlock(&g_face_engine_mutex);

        if (rc == 0) {
            LOGD(__func__, "capture_photo_bin ok bytes=%zu w=%d h=%d q=%d cost=%llums",
                    jpeg_size, cap_w, cap_h, cap_q,
                    (unsigned long long)(zh_now_ms() - start_ms));
            if (g_face_ws && zh_face_upload_one_frame(jpeg, jpeg_size) != 0) {
                LOGE(__func__, "vision pipeline failed bytes=%zu", jpeg_size);
            }
        } else {
            LOGE(__func__, "capture_photo_bin failed err=%s cost=%llums",
                 err_buf[0] ? err_buf : "unknown",
                 (unsigned long long)(zh_now_ms() - start_ms));
        }

        if (jpeg) {
            free(jpeg);
        }

        if (g_face_capture_running) {
            // 抓拍链路不再受 VAD active 门控，固定节奏持续推进 vision 往返。
            usleep(ZH_FACE_CAPTURE_INTERVAL_MS * 1000);
        }
    }

    return NULL;
}

int zh_face_recognition_start(void) {
    int err;

    if (g_face_thread_started) {
        return 0;
    }

    // 新装设备尚未录入人脸时目录也必须存在，否则 compare_face_emb_dir 会失败。
    if (zh_mkdir_recursive(ZH_FACE_EMB_DIR) != 0) {
        LOGE(__func__, "prepare face emb dir failed: %s errno=%d",
             ZH_FACE_EMB_DIR, errno);
        return -1;
    }

    g_face_running = 1;
    err = pthread_create(&g_face_thread, NULL, zh_face_thread_main, NULL);
    if (err != 0) {
        g_face_running = 0;
        errno = err;
        return -1;
    }
    g_face_thread_started = 1;

    g_face_capture_running = 1;
    err = pthread_create(&g_face_capture_thread, NULL, zh_face_capture_thread_main, NULL);
    if (err != 0) {
        g_face_capture_running = 0;
        LOGE(__func__, "face capture thread start failed err=%d", err);
    } else {
        g_face_capture_thread_started = 1;
    }
    return 0;
}

void zh_face_recognition_stop(void) {
    if (!g_face_thread_started) return;

    pthread_mutex_lock(&g_face_mutex);
    g_face_running = 0;
    g_face_active = 0;
    g_face_report_until_ms = 0;
    g_face_capture_running = 0;
    pthread_cond_broadcast(&g_face_cond);
    pthread_mutex_unlock(&g_face_mutex);

    pthread_join(g_face_thread, NULL);
    g_face_thread_started = 0;

    if (g_face_capture_thread_started) {
        pthread_join(g_face_capture_thread, NULL);
        g_face_capture_thread_started = 0;
    }

    // 统一走带锁清理，避免共享 UDS 在无锁场景被关闭。
    zh_face_cleanup_engine();
}

void zh_face_recognition_set_active(int active) {
    int was_active;

    pthread_mutex_lock(&g_face_mutex);
    was_active = g_face_active;
    g_face_active = active ? 1 : 0;
    if (g_face_active) {
        g_face_report_until_ms = 0;
        pthread_cond_broadcast(&g_face_cond);
    } else if (was_active) {
        g_face_report_until_ms = zh_face_report_deadline_ms(0, zh_now_ms());
        pthread_cond_broadcast(&g_face_cond);
    }
    pthread_mutex_unlock(&g_face_mutex);
}

void zh_face_recognition_set_ws(zh_ws_session_t *ws) {
    g_face_ws = ws;
}
