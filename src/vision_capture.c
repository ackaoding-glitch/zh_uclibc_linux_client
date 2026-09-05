#include "vision_capture.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bithion_core.h"
#include "config.h"
#include "log.h"
#include "utils.h"

#define ZH_VISION_CONTENT_TYPE "image/jpeg"
#define ZH_VISION_PROBE_WIDTH 32
#define ZH_VISION_PROBE_HEIGHT 24
#define ZH_VISION_PROBE_SIZE (ZH_VISION_PROBE_WIDTH * ZH_VISION_PROBE_HEIGHT)
#define ZH_VISION_RESULT_UNCHANGED 1

static pthread_mutex_t g_vision_mutex = PTHREAD_MUTEX_INITIALIZER;
static zh_ws_session_t *g_vision_ws = NULL;
static int g_vision_busy = 0;
static int g_vision_running = 1;
static pthread_t g_vision_worker_thread;
static int g_vision_worker_started = 0;
static uint8_t g_vision_last_probe[ZH_VISION_PROBE_SIZE];
static int g_vision_last_probe_valid = 0;
static unsigned int g_vision_unchanged_count = 0;

static void zh_vision_sleep_ms_interruptible(int ms);

static int zh_env_int(const char *name, int fallback, int min_value, int max_value) {
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || value[0] == '\0') return fallback;
    parsed = strtol(value, &end, 10);
    if (end == value || parsed < min_value || parsed > max_value) return fallback;
    return (int)parsed;
}

static const char *zh_env_str(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : fallback;
}

static uint64_t zh_vision_wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static int zh_json_get_ll(const char *json, const char *key, long long *out) {
    char pattern[64];
    const char *p;
    char *end = NULL;

    if (!json || !key || !out) return -1;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (!p) return -1;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *out = strtoll(p, &end, 10);
    return (end != p) ? 0 : -1;
}

static int zh_read_exact_file(FILE *fp, uint8_t *buf, size_t len) {
    size_t used = 0;
    while (used < len) {
        size_t n = fread(buf + used, 1, len - used, fp);
        if (n == 0) return -1;
        used += n;
    }
    return 0;
}

static int zh_connect_ui_camera(FILE **out_io) {
    const char *sock_path = zh_env_str("ZH_UI_CAMERA_SOCKET", "/tmp/zh_ui_camera.sock");
    struct sockaddr_un addr;
    int fd;
    FILE *io;

    if (!out_io) return -1;
    *out_io = NULL;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    io = fdopen(fd, "r+");
    if (!io) {
        close(fd);
        return -1;
    }
    setvbuf(io, NULL, _IOLBF, 0);
    *out_io = io;
    return 0;
}

static int zh_get_ui_vision_probe(uint8_t probe[ZH_VISION_PROBE_SIZE]) {
    char header[512];
    long long size_ll = 0;
    long long width_ll = 0;
    long long height_ll = 0;
    FILE *io = NULL;

    if (!probe || zh_connect_ui_camera(&io) != 0) return -1;
    fprintf(io, "vision_probe\n");
    fflush(io);
    if (!fgets(header, sizeof(header), io) ||
        !strstr(header, "\"ok\":1") ||
        !strstr(header, "\"format\":\"gray\"") ||
        zh_json_get_ll(header, "size", &size_ll) != 0 ||
        zh_json_get_ll(header, "w", &width_ll) != 0 ||
        zh_json_get_ll(header, "h", &height_ll) != 0 ||
        size_ll != ZH_VISION_PROBE_SIZE ||
        width_ll != ZH_VISION_PROBE_WIDTH ||
        height_ll != ZH_VISION_PROBE_HEIGHT ||
        zh_read_exact_file(io, probe, ZH_VISION_PROBE_SIZE) != 0) {
        fclose(io);
        return -1;
    }
    fclose(io);
    return 0;
}

static int zh_int_compare(const void *lhs, const void *rhs) {
    int left = *(const int *)lhs;
    int right = *(const int *)rhs;
    return (left > right) - (left < right);
}

static int zh_vision_probe_changed(const uint8_t probe[ZH_VISION_PROBE_SIZE],
                                   int *out_raw_mad_x100,
                                   int *out_mad_x100,
                                   int *out_changed_percent_x100,
                                   int *out_brightness_shift) {
    int pixel_threshold = zh_env_int("ZH_VISION_CHANGE_PIXEL_THRESHOLD", 12, 1, 255);
    int mad_threshold = zh_env_int("ZH_VISION_CHANGE_MAD_THRESHOLD", 6, 1, 255);
    int ratio_percent = zh_env_int("ZH_VISION_CHANGE_RATIO_PERCENT", 8, 1, 100);
    int signed_diffs[ZH_VISION_PROBE_SIZE];
    int sorted_diffs[ZH_VISION_PROBE_SIZE];
    unsigned int raw_diff_sum = 0;
    unsigned int diff_sum = 0;
    int brightness_shift;
    int changed_count = 0;
    int i;

    if (!g_vision_last_probe_valid) {
        if (out_raw_mad_x100) *out_raw_mad_x100 = 0;
        if (out_mad_x100) *out_mad_x100 = 0;
        if (out_changed_percent_x100) *out_changed_percent_x100 = 0;
        if (out_brightness_shift) *out_brightness_shift = 0;
        return 1;
    }
    for (i = 0; i < ZH_VISION_PROBE_SIZE; ++i) {
        signed_diffs[i] = (int)probe[i] - (int)g_vision_last_probe[i];
        sorted_diffs[i] = signed_diffs[i];
        raw_diff_sum += (unsigned int)abs(signed_diffs[i]);
    }
    qsort(sorted_diffs, ZH_VISION_PROBE_SIZE, sizeof(sorted_diffs[0]), zh_int_compare);
    brightness_shift = (sorted_diffs[ZH_VISION_PROBE_SIZE / 2 - 1] +
                        sorted_diffs[ZH_VISION_PROBE_SIZE / 2]) / 2;

    for (i = 0; i < ZH_VISION_PROBE_SIZE; ++i) {
        int diff = abs(signed_diffs[i] - brightness_shift);
        diff_sum += (unsigned int)diff;
        if (diff >= pixel_threshold) changed_count++;
    }
    if (out_raw_mad_x100) {
        *out_raw_mad_x100 = (int)(raw_diff_sum * 100U / ZH_VISION_PROBE_SIZE);
    }
    if (out_mad_x100) {
        *out_mad_x100 = (int)(diff_sum * 100U / ZH_VISION_PROBE_SIZE);
    }
    if (out_changed_percent_x100) {
        *out_changed_percent_x100 = changed_count * 10000 / ZH_VISION_PROBE_SIZE;
    }
    if (out_brightness_shift) {
        *out_brightness_shift = brightness_shift;
    }
    return diff_sum >= (unsigned int)(mad_threshold * ZH_VISION_PROBE_SIZE) ||
           changed_count * 100 >= ratio_percent * ZH_VISION_PROBE_SIZE;
}

static int zh_capture_from_ui_socket(uint8_t **out_jpeg, size_t *out_size) {
    int width = zh_env_int("ZH_VISION_CAPTURE_WIDTH", 640, 1, 4096);
    int height = zh_env_int("ZH_VISION_CAPTURE_HEIGHT", 480, 1, 4096);
    int quality = zh_env_int("ZH_VISION_CAPTURE_Q", 90, 1, 100);
    int warmup = zh_env_int("ZH_VISION_CAPTURE_WARMUP", 0, 0, 120);
    int inter_ms = zh_env_int("ZH_VISION_CAPTURE_INTER_FRAME_MS", 0, 0, 1000);
    FILE *io = NULL;
    char header[512];
    long long size_ll = 0;
    uint8_t *jpeg = NULL;

    if (!out_jpeg || !out_size) return -1;
    *out_jpeg = NULL;
    *out_size = 0;

    if (zh_connect_ui_camera(&io) != 0) return -1;
    fprintf(io, "capture_photo_bin %d %d %d %d %d\n", width, height, quality, warmup, inter_ms);
    fflush(io);
    if (!fgets(header, sizeof(header), io)) {
        fclose(io);
        return -1;
    }
    if (!strstr(header, "\"ok\":1") || zh_json_get_ll(header, "size", &size_ll) != 0 ||
        size_ll <= 0 || size_ll > (20LL * 1024LL * 1024LL)) {
        LOGE(__func__, "ui camera capture failed header=%s", header);
        fclose(io);
        return -1;
    }
    jpeg = (uint8_t *)malloc((size_t)size_ll);
    if (!jpeg) {
        fclose(io);
        return -1;
    }
    if (zh_read_exact_file(io, jpeg, (size_t)size_ll) != 0) {
        free(jpeg);
        fclose(io);
        return -1;
    }
    fclose(io);
    *out_jpeg = jpeg;
    *out_size = (size_t)size_ll;
    return 0;
}

static void zh_make_frame_id(char *out, size_t out_len) {
    static unsigned int seq = 0;
    seq++;
    snprintf(out, out_len, "f_%llu_%u", (unsigned long long)zh_vision_wall_ms(), seq);
}

static int zh_write_all_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t used = 0;

    while (used < len) {
        ssize_t n = write(fd, p + used, len - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}

static int zh_read_small_file(const char *path, char *out, size_t out_len) {
    int fd;
    ssize_t n;

    if (!path || !out || out_len == 0) return -1;
    out[0] = '\0';
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = read(fd, out, out_len - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
}

static int zh_http_put_binary_via_curl(const char *url,
                                       const char *content_type,
                                       const uint8_t *body,
                                       size_t body_len,
                                       int with_content_type,
                                       int *out_status,
                                       char *out_resp,
                                       size_t out_resp_len) {
    char body_template[] = "/tmp/zh_vision_curl_body_XXXXXX";
    char resp_template[] = "/tmp/zh_vision_curl_resp_XXXXXX";
    char status_template[] = "/tmp/zh_vision_curl_status_XXXXXX";
    char ct_header[160];
    char status_buf[32];
    int body_fd = -1;
    int resp_fd = -1;
    int status_fd = -1;
    int wait_status = 0;
    int status_code = 0;
    int max_time = zh_env_int("ZH_VISION_PUT_MAX_TIME", 45, 5, 300);
    char max_time_arg[16];
    pid_t pid;

    if (out_status) *out_status = 0;
    if (out_resp && out_resp_len > 0) out_resp[0] = '\0';
    if (!url || !body || body_len == 0) return -1;

    body_fd = mkstemp(body_template);
    if (body_fd < 0) return -1;
    resp_fd = mkstemp(resp_template);
    if (resp_fd < 0) {
        close(body_fd);
        unlink(body_template);
        return -1;
    }
    status_fd = mkstemp(status_template);
    if (status_fd < 0) {
        close(body_fd);
        close(resp_fd);
        unlink(body_template);
        unlink(resp_template);
        return -1;
    }

    if (zh_write_all_fd(body_fd, body, body_len) != 0) {
        if (out_resp && out_resp_len > 0) snprintf(out_resp, out_resp_len, "write tmp failed errno=%d", errno);
        close(body_fd);
        close(resp_fd);
        close(status_fd);
        unlink(body_template);
        unlink(resp_template);
        unlink(status_template);
        return -1;
    }
    close(body_fd);
    body_fd = -1;
    close(resp_fd);
    resp_fd = -1;
    close(status_fd);
    status_fd = -1;

    snprintf(max_time_arg, sizeof(max_time_arg), "%d", max_time);
    snprintf(ct_header, sizeof(ct_header), "Content-Type: %s",
             content_type && content_type[0] ? content_type : "application/octet-stream");

    pid = fork();
    if (pid < 0) {
        if (out_resp && out_resp_len > 0) snprintf(out_resp, out_resp_len, "fork failed errno=%d", errno);
        unlink(body_template);
        unlink(resp_template);
        unlink(status_template);
        return -1;
    }
    if (pid == 0) {
        int out_fd = open(status_template, O_WRONLY | O_TRUNC);
        int err_fd = open(resp_template, O_WRONLY | O_TRUNC);
        if (out_fd < 0 || err_fd < 0) _exit(127);
        if (dup2(out_fd, STDOUT_FILENO) < 0) _exit(127);
        if (dup2(err_fd, STDERR_FILENO) < 0) _exit(127);
        close(out_fd);
        close(err_fd);

        if (with_content_type) {
            execlp("curl", "curl",
                   "--http1.1",
                   "--connect-timeout", "10",
                   "--max-time", max_time_arg,
                   "-sS",
                   "-T", body_template,
                   "-H", ct_header,
                   "-H", "Expect:",
                   "-o", resp_template,
                   "-w", "%{http_code}",
                   url,
                   (char *)NULL);
        } else {
            execlp("curl", "curl",
                   "--http1.1",
                   "--connect-timeout", "10",
                   "--max-time", max_time_arg,
                   "-sS",
                   "-T", body_template,
                   "-H", "Expect:",
                   "-o", resp_template,
                   "-w", "%{http_code}",
                   url,
                   (char *)NULL);
        }
        _exit(127);
    }

    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
    }

    if (zh_read_small_file(status_template, status_buf, sizeof(status_buf)) == 0) {
        status_code = atoi(status_buf);
        if (out_status) *out_status = status_code;
    }
    if (out_resp && out_resp_len > 0) {
        (void)zh_read_small_file(resp_template, out_resp, out_resp_len);
    }

    unlink(body_template);
    unlink(resp_template);
    unlink(status_template);

    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        if (out_resp && out_resp_len > 0 && out_resp[0] == '\0') {
            snprintf(out_resp, out_resp_len, "curl exit=%d", WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 255);
        }
        return -1;
    }
    return (status_code >= 200 && status_code < 300) ? 0 : -1;
}

static int zh_put_jpeg_with_retry(const char *url, const uint8_t *jpeg, size_t jpeg_size) {
    int try_idx;
    int status_code = 0;
    char resp[256];

    for (try_idx = 1; try_idx <= 30; ++try_idx) {
        resp[0] = '\0';
        status_code = 0;
        if (zh_http_put_binary_via_curl(url, ZH_VISION_CONTENT_TYPE,
                                        jpeg, jpeg_size, 1,
                                        &status_code, resp, sizeof(resp)) == 0) {
            return 0;
        }
        if (status_code == 400 &&
            zh_http_put_binary_via_curl(url, ZH_VISION_CONTENT_TYPE,
                                        jpeg, jpeg_size, 0,
                                        &status_code, resp, sizeof(resp)) == 0) {
            return 0;
        }
        LOGW(__func__, "vision put retry %d/30 status=%d resp=%s", try_idx, status_code, resp);
        usleep(100 * 1000);
    }
    return -1;
}

static int zh_vision_upload_one(zh_ws_session_t *ws,
                                const uint8_t *jpeg,
                                size_t jpeg_size,
                                int *out_committed) {
    const char *device_id;
    char frame_id[128];
    zh_ws_vision_upload_response_t upload_resp;
    zh_ws_vision_done_t done;

    if (out_committed) *out_committed = 0;
    if (!ws || !jpeg || jpeg_size == 0) return -1;
    device_id = zh_ws_get_device_id(ws);
    if (!device_id || device_id[0] == '\0') return -1;
    zh_make_frame_id(frame_id, sizeof(frame_id));

    if (zh_ws_send_vision_upload_request(ws, device_id, frame_id, ZH_VISION_CONTENT_TYPE) != 0) {
        LOGE(__func__, "send vision_upload_request failed frame_id=%s", frame_id);
        return -1;
    }
    if (zh_ws_wait_vision_upload_response(ws, &upload_resp, &g_vision_running) != 0) {
        LOGE(__func__, "wait vision_upload_response failed frame_id=%s", frame_id);
        return -1;
    }
    if (strcmp(upload_resp.status, "ok") != 0 || upload_resp.upload_url[0] == '\0' ||
        upload_resp.upload_id[0] == '\0' || upload_resp.object_key[0] == '\0') {
        if (strcmp(upload_resp.error_code, "VISION_BUSY") == 0) {
            int wait_ms = (int)upload_resp.next_capture_ms;
            if (wait_ms <= 0) wait_ms = 500;
            LOGI(__func__,
                 "vision server busy frame_id=%s active wait_ms=%d",
                 frame_id,
                 wait_ms);
            zh_vision_sleep_ms_interruptible(wait_ms);
            return 0;
        }
        LOGE(__func__,
             "invalid vision_upload_response frame_id=%s status=%s code=%s",
             frame_id,
             upload_resp.status,
             upload_resp.error_code);
        return -1;
    }
    if (zh_put_jpeg_with_retry(upload_resp.upload_url, jpeg, jpeg_size) != 0) {
        LOGE(__func__, "put jpeg failed frame_id=%s upload_id=%s", frame_id, upload_resp.upload_id);
        return -1;
    }
    if (zh_ws_send_vision_upload_commit(ws, device_id, frame_id,
                                        upload_resp.upload_id,
                                        upload_resp.object_key,
                                        "") != 0) {
        LOGE(__func__, "send vision_upload_commit failed frame_id=%s", frame_id);
        return -1;
    }
    if (out_committed) *out_committed = 1;
    if (zh_ws_wait_vision_done(ws, frame_id, &done, &g_vision_running) != 0) {
        LOGE(__func__, "wait vision_done failed frame_id=%s", frame_id);
        return -1;
    }
    if (strcmp(done.result, "ok") != 0) {
        LOGE(__func__, "vision_done error frame_id=%s code=%s", frame_id, done.error_code);
        return -1;
    }
    LOGI(__func__, "vision capture uploaded frame_id=%s upload_id=%s", frame_id, upload_resp.upload_id);
    return 0;
}

static int zh_vision_capture_upload_once(zh_ws_session_t *ws) {
    uint8_t probe[ZH_VISION_PROBE_SIZE];
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    int probe_available = 0;
    int raw_mad_x100 = 0;
    int mad_x100 = 0;
    int changed_percent_x100 = 0;
    int brightness_shift = 0;
    int committed = 0;
    int rc = -1;

    if (zh_env_int("ZH_VISION_CHANGE_ENABLE", 1, 0, 1) != 0 &&
        zh_get_ui_vision_probe(probe) == 0) {
        probe_available = 1;
        if (!zh_vision_probe_changed(probe,
                                     &raw_mad_x100,
                                     &mad_x100,
                                     &changed_percent_x100,
                                     &brightness_shift)) {
            g_vision_unchanged_count++;
            if (g_vision_unchanged_count == 1 || g_vision_unchanged_count % 60 == 0) {
                LOGI(__func__,
                     "vision unchanged, skip upload raw_mad=%.2f mad=%.2f brightness_shift=%d "
                     "changed=%.2f%% skipped=%u retry_ms=%d",
                     raw_mad_x100 / 100.0,
                     mad_x100 / 100.0,
                     brightness_shift,
                     changed_percent_x100 / 100.0,
                     g_vision_unchanged_count,
                     zh_env_int("ZH_VISION_UNCHANGED_RETRY_MS", 1000, 100, 60000));
            }
            return ZH_VISION_RESULT_UNCHANGED;
        }
        LOGI(__func__,
             "vision changed, continue upload raw_mad=%.2f mad=%.2f brightness_shift=%d "
             "changed=%.2f%% baseline=%s",
             raw_mad_x100 / 100.0,
             mad_x100 / 100.0,
             brightness_shift,
             changed_percent_x100 / 100.0,
             g_vision_last_probe_valid ? "ready" : "initial");
        g_vision_unchanged_count = 0;
    } else if (zh_env_int("ZH_VISION_CHANGE_ENABLE", 1, 0, 1) != 0) {
        LOGW(__func__, "vision change probe unavailable, continue upload");
        g_vision_unchanged_count = 0;
    }

    if (zh_capture_from_ui_socket(&jpeg, &jpeg_size) == 0) {
        rc = zh_vision_upload_one(ws, jpeg, jpeg_size, &committed);
        if (committed && probe_available) {
            memcpy(g_vision_last_probe, probe, sizeof(g_vision_last_probe));
            g_vision_last_probe_valid = 1;
        }
        if (rc != 0 && !committed) {
            LOGE(__func__, "vision upload failed bytes=%zu", jpeg_size);
        }
    } else {
        LOGE(__func__, "capture from UI camera socket failed errno=%d", errno);
    }
    free(jpeg);
    return rc;
}

static void *zh_vision_capture_thread(void *arg) {
    zh_ws_session_t *ws = (zh_ws_session_t *)arg;

    (void)zh_vision_capture_upload_once(ws);

    pthread_mutex_lock(&g_vision_mutex);
    g_vision_busy = 0;
    pthread_mutex_unlock(&g_vision_mutex);
    return NULL;
}

static void zh_vision_sleep_ms_interruptible(int ms) {
    int slept = 0;

    while (slept < ms) {
        int step = ms - slept;
        if (step > 100) step = 100;
        if (!g_vision_running) return;
        usleep(step * 1000);
        slept += step;
    }
}

static void *zh_vision_worker_thread_main(void *arg) {
    (void)arg;

    LOGI(__func__, "vision auto capture worker started");
    while (g_vision_running) {
        zh_ws_session_t *ws = NULL;
        int retry_ms;
        int idle_ms;
        int unchanged_ms;
        int rc;

        pthread_mutex_lock(&g_vision_mutex);
        if (!g_vision_running) {
            pthread_mutex_unlock(&g_vision_mutex);
            break;
        }
        if (!g_vision_ws || g_vision_busy) {
            pthread_mutex_unlock(&g_vision_mutex);
            zh_vision_sleep_ms_interruptible(200);
            continue;
        }
        ws = g_vision_ws;
        g_vision_busy = 1;
        pthread_mutex_unlock(&g_vision_mutex);

        rc = zh_vision_capture_upload_once(ws);

        pthread_mutex_lock(&g_vision_mutex);
        g_vision_busy = 0;
        pthread_mutex_unlock(&g_vision_mutex);

        if (!g_vision_running) {
            break;
        }
        retry_ms = zh_env_int("ZH_VISION_CAPTURE_RETRY_MS", 1000, 100, 60000);
        idle_ms = zh_env_int("ZH_VISION_CAPTURE_IDLE_MS", 10, 0, 60000);
        unchanged_ms = zh_env_int("ZH_VISION_UNCHANGED_RETRY_MS", 1000, 100, 60000);
        zh_vision_sleep_ms_interruptible(
            rc == ZH_VISION_RESULT_UNCHANGED ? unchanged_ms : (rc == 0 ? idle_ms : retry_ms));
    }
    LOGI(__func__, "vision auto capture worker stopped");
    return NULL;
}

void zh_vision_capture_set_ws(zh_ws_session_t *ws) {
    pthread_mutex_lock(&g_vision_mutex);
    g_vision_ws = ws;
    pthread_mutex_unlock(&g_vision_mutex);
}

int zh_vision_capture_start(void) {
    int err;

    if (!ZH_ENABLE_VISION_CAPTURE) {
        return 0;
    }
    if (ZH_ENABLE_FACE) {
        LOGI(__func__, "skip vision auto capture worker because face module owns vision upload");
        return 0;
    }
    if (zh_env_int("ZH_VISION_AUTO_CAPTURE", 1, 0, 1) == 0) {
        LOGI(__func__, "vision auto capture disabled by env");
        return 0;
    }

    pthread_mutex_lock(&g_vision_mutex);
    if (g_vision_worker_started) {
        pthread_mutex_unlock(&g_vision_mutex);
        return 0;
    }
    g_vision_running = 1;
    pthread_mutex_unlock(&g_vision_mutex);

    err = pthread_create(&g_vision_worker_thread, NULL, zh_vision_worker_thread_main, NULL);
    if (err != 0) {
        errno = err;
        return -1;
    }

    pthread_mutex_lock(&g_vision_mutex);
    g_vision_worker_started = 1;
    pthread_mutex_unlock(&g_vision_mutex);
    return 0;
}

void zh_vision_capture_stop(void) {
    int should_join = 0;
    pthread_t tid;

    if (!ZH_ENABLE_VISION_CAPTURE || ZH_ENABLE_FACE) {
        return;
    }

    pthread_mutex_lock(&g_vision_mutex);
    g_vision_running = 0;
    if (g_vision_worker_started) {
        should_join = 1;
        tid = g_vision_worker_thread;
        g_vision_worker_started = 0;
    }
    pthread_mutex_unlock(&g_vision_mutex);

    if (should_join) {
        pthread_join(tid, NULL);
    }
}

int zh_vision_capture_request_async(const char *reason) {
    pthread_t tid;
    zh_ws_session_t *ws;
    int err;

    pthread_mutex_lock(&g_vision_mutex);
    if (g_vision_busy) {
        pthread_mutex_unlock(&g_vision_mutex);
        LOGW(__func__, "vision capture already running, ignore reason=%s", reason ? reason : "");
        return 0;
    }
    ws = g_vision_ws;
    if (!ws) {
        pthread_mutex_unlock(&g_vision_mutex);
        LOGE(__func__, "vision capture requested without ws reason=%s", reason ? reason : "");
        return -1;
    }
    g_vision_busy = 1;
    pthread_mutex_unlock(&g_vision_mutex);

    err = pthread_create(&tid, NULL, zh_vision_capture_thread, ws);
    if (err != 0) {
        pthread_mutex_lock(&g_vision_mutex);
        g_vision_busy = 0;
        pthread_mutex_unlock(&g_vision_mutex);
        errno = err;
        return -1;
    }
    pthread_detach(tid);
    LOGI(__func__, "vision capture requested reason=%s", reason ? reason : "");
    return 0;
}
