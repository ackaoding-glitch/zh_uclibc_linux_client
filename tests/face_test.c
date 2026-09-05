#include "config.h"
#include "core_json_compat.h"
#include "log.h"
#include "utils.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <locale.h>

// 图片文件路径（jpg/png）
#define ZH_FACE_PNG_PATH "/data/luckfox_pico_retinaface_facenet_demo/my5.png"
// emb 获取方式开关：1=图片文件(get_file_emb)，0=摄像头采样平均(get_face_emb_avg)
#define ZH_FACE_EMB_FROM_FILE 1
// UDS socket 路径
#define ZH_FACE_ENGINE_SOCK_PATH "/tmp/face_engine.sock"

static pid_t zh_start_face_engine_uds(void) {
    pid_t pid = fork();
    if (pid == 0) {
        char uds_arg[256];
        snprintf(uds_arg, sizeof(uds_arg), "--uds_path=%s", ZH_FACE_ENGINE_SOCK_PATH);
        execl(ZH_FACE_ENGINE_PATH,
              ZH_FACE_ENGINE_PATH,
              "--uds",
              uds_arg,
              (char *)NULL);
        _exit(127);
    }
    return pid;
}

static int zh_uds_connect(const char *path, int timeout_ms) {
    int fd;
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
        close(fd);
        if ((int)(zh_now_ms() - start) >= timeout_ms) {
            break;
        }
        usleep(50 * 1000);
    }
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

// 从 JSON 中提取 err 字段文本（mongoose）
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

// 从 JSON 中解析 emb 数组为浮点数组（mongoose）
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
        emb[i] = (float) strtod(tmp, &end);
        if (end == tmp) return -1;
        i++;
    }
    return (i == emb_len) ? 0 : -1;
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
            case '\"': rep = "\\\""; break;
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

static int zh_append_index_json(const char *index_path,
                                const char *id,
                                const char *name,
                                const char *emb_path,
                                long ts) {
    FILE *in;
    FILE *out;
    char *buf = NULL;
    long len = 0;
    size_t end;
    char name_esc[256];
    char path_esc[512];
    char entry[1024];

    if (!index_path || !id || !name || !emb_path) {
        return -1;
    }

    zh_json_escape(name, name_esc, sizeof(name_esc));
    zh_json_escape(emb_path, path_esc, sizeof(path_esc));
    snprintf(entry, sizeof(entry),
             "{\"id\":\"%s\",\"name\":\"%s\",\"path\":\"%s\",\"ts\":%ld}",
             id, name_esc, path_esc, ts);

    in = fopen(index_path, "rb");
    if (in) {
        if (fseek(in, 0, SEEK_END) == 0) {
            len = ftell(in);
            if (len > 0 && fseek(in, 0, SEEK_SET) == 0) {
                buf = (char *)malloc((size_t)len + 1);
                if (buf && fread(buf, 1, (size_t)len, in) == (size_t)len) {
                    buf[len] = '\0';
                } else {
                    free(buf);
                    buf = NULL;
                }
            }
        }
        fclose(in);
    }

    out = fopen(index_path, "wb");
    if (!out) {
        free(buf);
        return -1;
    }

    if (!buf || len <= 0) {
        fprintf(out, "[%s]\n", entry);
        fclose(out);
        free(buf);
        return 0;
    }

    end = (size_t)len;
    while (end > 0 && isspace((unsigned char)buf[end - 1])) {
        end--;
    }
    if (end > 0 && buf[end - 1] == ']') {
        buf[end - 1] = '\0';
        end--;
        while (end > 0 && isspace((unsigned char)buf[end - 1])) {
            end--;
        }
        if (end > 0 && buf[end - 1] == '[') {
            fprintf(out, "%s%s]\n", buf, entry);
        } else {
            fprintf(out, "%s,%s]\n", buf, entry);
        }
    } else {
        fprintf(out, "[%s]\n", entry);
    }
    fclose(out);
    free(buf);
    return 0;
}

static int zh_face_extract_and_save(int uds_fd, char *saved_path, size_t saved_len) {
    const int sample_count = 5;
    const int delay_ms = 100;
    char cmd[512];
    char json[8192];
    float emb[512];
    char err_buf[128];
    char name_buf[128];
    const char *name_env;

    if (!saved_path || saved_len == 0) return -1;
    saved_path[0] = '\0';

    // 获取人脸特征（图片文件 or 摄像头采样平均）
#if ZH_FACE_EMB_FROM_FILE
    snprintf(cmd, sizeof(cmd), "get_file_emb %s", ZH_FACE_PNG_PATH);
#else
    snprintf(cmd, sizeof(cmd), "get_face_emb_avg %d %d", sample_count, delay_ms);
#endif
    // 只读取 {"ok"...} 的 JSON 行，别的都是无效输出
    {
        uint64_t t0 = zh_now_ms();
        int rc = zh_uds_send_cmd(uds_fd, cmd, json, sizeof(json));
        uint64_t t1 = zh_now_ms();
        LOGI(__func__, "uds_send_cmd cost=%llums", (unsigned long long)(t1 - t0));
        if (rc != 0) {
            LOGE(__func__, "read face_engine json failed");
            return -1;
        }
    }
    LOGI(__func__, "face_engine json: %s", json);

    // ok=0 视为正常失败（如 bad_path/no_face_or_load_fail）
    {
        double ok = 0;
        bithion_core_json_view_t s;
        s.buf = json;
        s.len = strlen(json);
        if (!mg_json_get_num(s, "$.ok", &ok)) {
            LOGE(__func__, "parse ok failed, json=%s", json);
            return -1;
        }
        if ((int) ok != 1) {
            if (zh_parse_err(json, err_buf, sizeof(err_buf)) == 0) {
                LOGI(__func__, "face_engine returned ok=0 err=%s", err_buf);
            } else {
                LOGI(__func__, "face_engine returned ok=0: %s", json);
            }
            return -1;
        }
    }

    // 解析 emb 数组
    if (zh_parse_emb(json, emb, 512) != 0) {
        LOGE(__func__, "parse emb failed, json=%s", json);
        return -1;
    }

    // 确保保存目录存在
    if (mkdir(ZH_FACE_EMB_DIR, 0755) != 0 && errno != EEXIST) {
        LOGE(__func__, "mkdir %s failed errno=%d", ZH_FACE_EMB_DIR, errno);
        return -1;
    }

    {
        char path[512];
        char index_path[512];
        char id_buf[64];
        FILE *out;
        time_t now = time(NULL);

        name_env = getenv("ZH_FACE_NAME");
        if (name_env && name_env[0] != '\0') {
            snprintf(name_buf, sizeof(name_buf), "%s", name_env);
        } else {
            LOGI(__func__, "input name (UTF-8): ");
            if (!fgets(name_buf, sizeof(name_buf), stdin)) {
                snprintf(name_buf, sizeof(name_buf), "unknown");
            } else {
                size_t nlen = strlen(name_buf);
                while (nlen > 0 &&
                       (name_buf[nlen - 1] == '\n' || name_buf[nlen - 1] == '\r')) {
                    name_buf[nlen - 1] = '\0';
                    nlen--;
                }
                if (nlen == 0) {
                    snprintf(name_buf, sizeof(name_buf), "unknown");
                }
            }
        }

        // 保存二进制 emb（512 个 float）
        snprintf(id_buf, sizeof(id_buf), "%llu", (unsigned long long)zh_now_ms());
        snprintf(path, sizeof(path), "%s%s.emb", ZH_FACE_EMB_DIR, id_buf);
        out = fopen(path, "wb");
        if (!out) {
            LOGE(__func__, "open emb file failed: %s errno=%d", path, errno);
            return -1;
        }
        if (fwrite(emb, sizeof(float), 512, out) != 512) {
            LOGE(__func__, "write emb file failed: %s errno=%d", path, errno);
            fclose(out);
            return -1;
        }
        fclose(out);
        LOGI(__func__, "emb saved: %s", path);

        // 保存索引 JSON
        snprintf(index_path, sizeof(index_path), "%sindex.json", ZH_FACE_EMB_DIR);
        if (zh_append_index_json(index_path, id_buf, name_buf, path, (long)now) != 0) {
            LOGE(__func__, "append index json failed: %s errno=%d", index_path, errno);
            return -1;
        }

        snprintf(saved_path, saved_len, "%s", path);
    }
    return 0;
}

static void zh_face_compare_loop(int uds_fd) {
    char cmd[512];
    char compare_json[8192];

    // 对比当前摄像头人脸和目录内所有 emb
    snprintf(cmd, sizeof(cmd), "compare_face_emb_dir %s", ZH_FACE_EMB_DIR);

    // 只读取 {"ok"...} 的 JSON 行
    while (1) {
        double ok = 0;
        bithion_core_json_view_t s;
        char index_path[512];
        char name_out[128];
        bithion_core_json_view_t results;
        size_t ofs = 0;
        bithion_core_json_view_t item;

        {
            uint64_t t0 = zh_now_ms();
            int rc = zh_uds_send_cmd(uds_fd, cmd, compare_json, sizeof(compare_json));
            uint64_t t1 = zh_now_ms();
            LOGI(__func__, "uds_send_cmd cost=%llums", (unsigned long long)(t1 - t0));
            if (rc != 0) {
                LOGE(__func__, "read compare json failed");
                close(uds_fd);
                return;
            }
        }
        LOGI(__func__, "face_engine compare json: %s", compare_json);

        s.buf = compare_json;
        s.len = strlen(compare_json);
        if (mg_json_get_num(s, "$.ok", &ok) && (int) ok == 1) {
            results = mg_json_get_tok(s, "$.results");
            if (results.buf == NULL || results.len == 0 || results.buf[0] != '[') {
                continue;
            }
            snprintf(index_path, sizeof(index_path), "%sindex.json", ZH_FACE_EMB_DIR);
            {
                double best_dist = 0;
                int best_has = 0;
                char best_name[128];

                best_name[0] = '\0';
                while ((ofs = mg_json_next(results, ofs, NULL, &item)) > 0) {
                    double item_ok = 0;
                    double same = 0;
                    double dist = 0;
                    if (mg_json_get_num(item, "$.ok", &item_ok) &&
                        (int) item_ok == 1 &&
                        mg_json_get_num(item, "$.dist", &dist)) {
                        if (mg_json_get_num(item, "$.same", &same) && (int) same == 1) {
                            char *file = mg_json_get_str(item, "$.file");
                            if (file) {
                                if (zh_index_find_name(index_path, file, name_out,
                                                       sizeof(name_out)) == 0) {
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
                    LOGI(__func__, "face matched best: %s", best_name);
                }
            }
        }
        usleep(1000 * 500);
    }
}

static void face_test(void) {
    char json[8192];
    pid_t fe_pid;
    int uds_fd;
    char saved_path[512];

    fe_pid = zh_start_face_engine_uds();
    uds_fd = zh_uds_connect(ZH_FACE_ENGINE_SOCK_PATH, 3000);
    if (uds_fd < 0) {
        LOGE(__func__, "connect uds failed: %s", ZH_FACE_ENGINE_SOCK_PATH);
        if (fe_pid > 0) {
            kill(fe_pid, SIGKILL);
            waitpid(fe_pid, NULL, 0);
        }
        return;
    }
    // 读取并丢弃 UDS 首行 ready
    if (zh_uds_read_line(uds_fd, json, sizeof(json)) == 0) {
        LOGI(__func__, "face_engine ready: %s", json);
    }

    // if (zh_face_extract_and_save(uds_fd, saved_path, sizeof(saved_path)) != 0) {
    //     return;
    // }
    zh_face_compare_loop(uds_fd);
}

int main() {
    setlocale(LC_ALL, "");
    face_test();
    return 0;
}
