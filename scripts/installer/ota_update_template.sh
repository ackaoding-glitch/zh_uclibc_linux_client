#!/bin/sh

set -eu

# 日志前缀，用于统一过滤 OTA 脚本输出。
LOG_PREFIX="[ota-script]"

log() {
    # 当上游已退出（例如 OTA 宿主进程被结束）时，stdout 可能断开。
    # 这里忽略写失败，避免 set -e 触发提前退出。
    echo "${LOG_PREFIX} $*" || true
}

# OTA 结果落盘文件路径（供下次启动上报）。
OTA_RESULT_FILE="${OTA_RESULT_FILE:-/zh_ota/ota_result.env}"
# 由 OTA 宿主注入的更新上下文（无注入时使用默认值）。
OTA_LOG_ID="${OTA_LOG_ID:-0}"
OTA_UPDATE_TYPE="${OTA_UPDATE_TYPE:-script}"
OTA_FROM_VERSION="${OTA_FROM_VERSION:-unknown}"
OTA_TO_VERSION="${OTA_TO_VERSION:-unknown}"

normalize_result_msg() {
    printf "%s" "$1" | tr '\r\n' ' ' | sed 's/[[:space:]]\+/ /g;s/^ //;s/ $//'
}

write_result_file() {
    status="$1"
    message="$2"
    safe_msg="$(normalize_result_msg "$message")"

    mkdir -p "$(dirname "$OTA_RESULT_FILE")" 2>/dev/null || true
    {
        printf "log_id=%s\n" "$OTA_LOG_ID"
        printf "update_type=%s\n" "$OTA_UPDATE_TYPE"
        printf "from_version=%s\n" "$OTA_FROM_VERSION"
        printf "to_version=%s\n" "$OTA_TO_VERSION"
        printf "status=%s\n" "$status"
        printf "error_message=%s\n" "$safe_msg"
    } > "$OTA_RESULT_FILE"
}

fail() {
    write_result_file "failed" "$*"
    echo "${LOG_PREFIX} ERROR: $*" >&2
    exit 1
}

# OTA 文件下载根路径（不含版本号）。
BASE_URL_ROOT="${BASE_URL_ROOT:-https://bithion.obs.cn-east-3.myhuaweicloud.com/ota_files/RV1106}"
# OTA 版本号，会拼接到下载路径中。
OTA_VERSION="${OTA_VERSION:-$OTA_TO_VERSION}"
# OTA 版本目录下的资源子目录名。
OTA_CHANNEL_DIR="${OTA_CHANNEL_DIR:-installer}"
# 客户端工作目录根路径。
ZH_WORK_BASE="${ZH_WORK_BASE:-/data/zh_work}"
# 可选外部清单，格式：远端文件名|目标路径|权限|开关（多行）。
OTA_ITEMS="${OTA_ITEMS:-}"
# 是否先自拉起为后台独立进程再执行升级（1:是, 0:否）。
AUTO_DETACH="${AUTO_DETACH:-0}"
# 后台独立进程日志路径。
DETACHED_LOG_PATH="${DETACHED_LOG_PATH:-/zh_ota/ota_installer_detached.log}"
# 内部标记：已处于后台独立进程。
OTA_DETACHED="${OTA_DETACHED:-0}"
# 是否在升级前停止 supervisor 与 zh_client（1:是, 0:否）。
STOP_BEFORE_UPDATE="${STOP_BEFORE_UPDATE:-1}"
# 升级前是否清理历史日志来额外释放空间（1:是, 0:否）。
PURGE_LOGS_BEFORE_UPDATE="${PURGE_LOGS_BEFORE_UPDATE:-0}"
# supervisor pid 文件路径。
SUP_PID_FILE="${SUP_PID_FILE:-/var/run/zh_client_supervisor.pid}"
# supervisor 进程匹配关键字。
SUP_DAEMON_MATCH="${SUP_DAEMON_MATCH:-/data/zh_work/start_zh_client.sh daemon}"
# zh_client 可执行路径，用于定位运行中的客户端进程。
ZH_CLIENT_BIN="${ZH_CLIENT_BIN:-/data/zh_work/zh_client}"
# 升级运行日志路径（切断父进程管道后继续写此日志）。
RUNTIME_LOG_PATH="${RUNTIME_LOG_PATH:-/zh_ota/ota_installer_runtime.log}"

# 说明：
# 1) 每行格式：远端文件名|本地目标绝对路径|chmod权限(八进制)|开关(1/0)
# 2) 远端文件名支持子目录，例如 certs/ca-certificates.pem
# 3) 版本号通过 OTA_VERSION 变量控制
# 4) 可通过 OTA_ITEMS 变量覆盖默认清单（多行，格式同上）
build_default_manifest() {
    cat <<EOF
face_engine|${ZH_WORK_BASE}/face/face_engine|755|1
libonnxruntime.so.1.17.3|${ZH_WORK_BASE}/libonnxruntime.so.1.17.3|644|1
libbithion-core.so.1|${ZH_WORK_BASE}/libbithion-core.so.1|644|1
certs/ca-certificates.pem|${ZH_WORK_BASE}/certs/ca-certificates.pem|644|1
zh_client|${ZH_WORK_BASE}/zh_client|755|1
start_zh_client.sh|${ZH_WORK_BASE}/start_zh_client.sh|755|1
kill.sh|${ZH_WORK_BASE}/kill.sh|755|1
start_ble_provision.sh|${ZH_WORK_BASE}/start_ble_provision.sh|755|1
zh_ble_gatt_server|${ZH_WORK_BASE}/zh_ble_gatt_server|755|1
prompt_mp3/boot.mp3|${ZH_WORK_BASE}/prompt_mp3/boot.mp3|644|1
prompt_mp3/chat_mode.mp3|${ZH_WORK_BASE}/prompt_mp3/chat_mode.mp3|644|1
prompt_mp3/ciallo.mp3|${ZH_WORK_BASE}/prompt_mp3/ciallo.mp3|644|1
prompt_mp3/net_connect.mp3|${ZH_WORK_BASE}/prompt_mp3/net_connect.mp3|644|1
prompt_mp3/net_disconnect.mp3|${ZH_WORK_BASE}/prompt_mp3/net_disconnect.mp3|644|1
prompt_mp3/provision.mp3|${ZH_WORK_BASE}/prompt_mp3/provision.mp3|644|1
prompt_mp3/web_search_wait.mp3|${ZH_WORK_BASE}/prompt_mp3/web_search_wait.mp3|644|1
prompt_wav/boot.wav|${ZH_WORK_BASE}/prompt_wav/boot.wav|644|1
prompt_wav/vision_load_failed.wav|${ZH_WORK_BASE}/prompt_wav/vision_load_failed.wav|644|1
S22zh_client|/etc/init.d/S22zh_client|755|1
EOF
}

is_enabled() {
    # 开关原始值，支持 1/true/yes/on 等写法。
    value="$1"
    case "$value" in
        ""|1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

list_pids_by_pattern() {
    pattern="$1"
    ps | grep "$pattern" | grep -v grep | awk '{print $1}' || true
}

switch_to_local_runtime_log() {
    mkdir -p "$(dirname "$RUNTIME_LOG_PATH")" 2>/dev/null || true
    exec >>"$RUNTIME_LOG_PATH" 2>&1
    log "switched runtime log to ${RUNTIME_LOG_PATH}"
}

stop_supervisor_only() {
    sup_pid=""
    pid=""

    log "stop supervisor before update"

    if [ -f "$SUP_PID_FILE" ]; then
        sup_pid="$(cat "$SUP_PID_FILE" 2>/dev/null || true)"
        if [ -n "$sup_pid" ]; then
            kill "$sup_pid" 2>/dev/null || true
        fi
        rm -f "$SUP_PID_FILE" || true
    fi

    for pid in $(list_pids_by_pattern "$SUP_DAEMON_MATCH"); do
        kill "$pid" 2>/dev/null || true
    done

    sleep 1

    for pid in $(list_pids_by_pattern "$SUP_DAEMON_MATCH"); do
        kill -9 "$pid" 2>/dev/null || true
    done

    log "stop supervisor done"
}

stop_client_only() {
    pid=""
    FACE_ENGINE_BIN="${ZH_WORK_BASE}/face/face_engine"

    log "stop zh_client before critical replace"

    for pid in $(list_pids_by_pattern "$ZH_CLIENT_BIN"); do
        kill "$pid" 2>/dev/null || true
    done
    for pid in $(list_pids_by_pattern "$FACE_ENGINE_BIN"); do
        kill "$pid" 2>/dev/null || true
    done

    sleep 1

    for pid in $(list_pids_by_pattern "$ZH_CLIENT_BIN"); do
        kill -9 "$pid" 2>/dev/null || true
    done
    for pid in $(list_pids_by_pattern "$FACE_ENGINE_BIN"); do
        kill -9 "$pid" 2>/dev/null || true
    done

    log "stop zh_client done"
}

get_manifest_content() {
    if [ -n "${OTA_ITEMS}" ]; then
        printf "%s\n" "${OTA_ITEMS}"
    else
        build_default_manifest
    fi
}

purge_logs_before_update() {
    if [ "${PURGE_LOGS_BEFORE_UPDATE}" != "1" ]; then
        return 0
    fi
    log "purge logs before update"
    rm -rf "${ZH_WORK_BASE}/logs" 2>/dev/null || true
    rm -f /zh_ota/ota_exec_*.log 2>/dev/null || true
    rm -f /zh_ota/ota_installer_detached.log 2>/dev/null || true
    sync || true
}

delete_targets_before_update() {
    log "delete old targets before update"
    get_manifest_content | while IFS='|' read -r remote_name target_path mode enabled; do
        case "$remote_name" in
            ""|\#*) continue ;;
        esac
        if ! is_enabled "${enabled:-1}"; then
            continue
        fi
        rm -f "$target_path" 2>/dev/null || true
    done
    sync || true
}

spawn_detached_if_needed() {
    if [ "$AUTO_DETACH" != "1" ] || [ "$OTA_DETACHED" = "1" ]; then
        return 0
    fi

    mkdir -p "$(dirname "$DETACHED_LOG_PATH")" 2>/dev/null || true
    log "spawn detached updater log=${DETACHED_LOG_PATH}"
    if command -v setsid >/dev/null 2>&1; then
        OTA_DETACHED=1 setsid /bin/sh "$0" "$@" >>"$DETACHED_LOG_PATH" 2>&1 < /dev/null &
    else
        OTA_DETACHED=1 /bin/sh "$0" "$@" >>"$DETACHED_LOG_PATH" 2>&1 < /dev/null &
    fi
    log "detached updater pid=$!"
    exit 0
}

parse_https_url() {
    # 原始下载 URL。
    url="$1"
    # 去掉协议头后的部分，如 host/path。
    rest=""
    # host:port 组合段。
    host_port=""
    # 解析后的主机名。
    host=""
    # 解析后的端口，默认 443。
    port="443"
    # 解析后的请求路径，默认 /。
    req_path="/"

    case "$url" in
        https://*) ;;
        *) return 1 ;;
    esac

    rest="${url#https://}"
    case "$rest" in
        */*)
            host_port="${rest%%/*}"
            req_path="/${rest#*/}"
            ;;
        *)
            host_port="$rest"
            req_path="/"
            ;;
    esac

    [ -n "$host_port" ] || return 1
    case "$host_port" in
        *:*)
            host="${host_port%%:*}"
            port="${host_port##*:}"
            ;;
        *)
            host="$host_port"
            ;;
    esac

    [ -n "$host" ] || return 1
    case "$port" in
        ""|*[!0-9]*) return 1 ;;
    esac

    # 导出给下载流程使用的解析结果。
    DOWNLOAD_HOST="$host"
    DOWNLOAD_PORT="$port"
    DOWNLOAD_PATH="$req_path"
    return 0
}

download_file() {
    # 远端下载 URL。
    url="$1"
    # 本地输出文件路径。
    out="$2"
    # 响应头解析结果文本。
    header_info=""
    # HTTP 状态码。
    status_code=""
    # 响应头字节长度。
    header_bytes=""
    # Content-Length 数值（可为空）。
    content_len=""
    # 下载后落盘文件大小（字节）。
    out_size=0

    command -v ssl_client >/dev/null 2>&1 || fail "ssl_client 不可用，无法下载：$url"
    parse_https_url "$url" || fail "下载链接不是有效 https 地址：$url"

    # 第一次请求：只解析响应头，不缓存完整文件内容。
    header_info="$(printf "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" \
        "$DOWNLOAD_PATH" "$DOWNLOAD_HOST" \
        | ssl_client "$DOWNLOAD_HOST" "$DOWNLOAD_PORT" 2>/dev/null \
        | awk '
            BEGIN { bytes = 0; code = ""; clen = ""; done = 0; }
            {
                raw = $0;
                bytes += length(raw) + 1;
                line = raw;
                sub(/\r$/, "", line);
                if (NR == 1) {
                    n = split(line, arr, /[[:space:]]+/);
                    if (n >= 2) code = arr[2];
                }
                if (tolower(line) ~ /^content-length:[[:space:]]*[0-9]+$/) {
                    tmp = line;
                    sub(/^[^:]*:[[:space:]]*/, "", tmp);
                    clen = tmp;
                }
                if (line == "") {
                    done = 1;
                    printf("status_code=%s\nheader_bytes=%d\ncontent_len=%s\n", code, bytes, clen);
                    exit 0;
                }
            }
            END {
                if (!done) exit 2;
            }
        ')" || return 1

    status_code=""
    header_bytes=""
    content_len=""
    while IFS='=' read -r field value; do
        case "$field" in
            status_code) status_code="$value" ;;
            header_bytes) header_bytes="$value" ;;
            content_len) content_len="$value" ;;
        esac
    done <<EOF
$header_info
EOF

    [ "$status_code" = "200" ] || return 1
    [ -n "$header_bytes" ] || return 1
    case "$header_bytes" in
        ""|*[!0-9]*) return 1 ;;
    esac

    # 第二次请求：直接写入目标路径，不经过中间缓存文件。
    if [ -n "$content_len" ]; then
        case "$content_len" in
            ""|*[!0-9]*) return 1 ;;
        esac
        [ "$content_len" -gt 0 ] || return 1
        printf "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" \
            "$DOWNLOAD_PATH" "$DOWNLOAD_HOST" \
            | ssl_client "$DOWNLOAD_HOST" "$DOWNLOAD_PORT" 2>/dev/null \
            | dd bs=1 skip="$header_bytes" count="$content_len" of="$out" 2>/dev/null || return 1
    else
        printf "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" \
            "$DOWNLOAD_PATH" "$DOWNLOAD_HOST" \
            | ssl_client "$DOWNLOAD_HOST" "$DOWNLOAD_PORT" 2>/dev/null \
            | dd bs=1 skip="$header_bytes" of="$out" 2>/dev/null || return 1
    fi

    out_size="$(wc -c < "$out" 2>/dev/null || echo 0)"
    [ "$out_size" -gt 0 ] || return 1
    if [ -n "$content_len" ] && [ "$out_size" -ne "$content_len" ]; then
        return 1
    fi
    return 0
}

apply_one() {
    # 远端文件名（含可选子目录）。
    remote_name="$1"
    # 本地覆盖目标绝对路径。
    target_path="$2"
    # 目标文件权限（八进制）。
    mode="$3"
    # 当前文件是否启用更新。
    enabled="$4"

    [ -n "$remote_name" ] || fail "remote_name 为空"
    [ -n "$target_path" ] || fail "target_path 为空"
    [ -n "$mode" ] || fail "mode 为空"
    [ -n "$enabled" ] || enabled="1"

    if ! is_enabled "$enabled"; then
        log "skip $target_path (switch=${enabled})"
        return 0
    fi

    # 当前文件完整下载地址。
    url="${BASE_URL_ROOT}/${OTA_VERSION}/${OTA_CHANNEL_DIR}/${remote_name}"
    # 先删旧文件，再下载到目标路径，避免额外缓存占用空间。
    rm -f "$target_path"
    mkdir -p "$(dirname "$target_path")"

    log "download $url"
    download_file "$url" "$target_path" || {
        rm -f "$target_path"
        fail "下载失败：$url"
    }
    chmod "$mode" "$target_path" || fail "chmod失败：$target_path mode=$mode"

    log "updated $target_path"
}

run_manifest_all() {
    get_manifest_content | while IFS='|' read -r remote_name target_path mode enabled; do
        case "$remote_name" in
            ""|\#*) continue ;;
        esac
        apply_one "$remote_name" "$target_path" "$mode" "$enabled"
    done
}

main() {
    spawn_detached_if_needed "$@"

    log "start ota update version=${OTA_VERSION} base=${BASE_URL_ROOT}"

    if [ "${STOP_BEFORE_UPDATE}" = "1" ]; then
        # 在杀宿主进程前先切换到本地日志文件，避免 Broken pipe 影响执行。
        switch_to_local_runtime_log
        stop_supervisor_only
        stop_client_only
    fi

    mkdir -p "${ZH_WORK_BASE}" || fail "创建工作目录失败：${ZH_WORK_BASE}"
    mkdir -p "${ZH_WORK_BASE}/certs" "${ZH_WORK_BASE}/face/save" || fail "创建子目录失败"

    purge_logs_before_update
    delete_targets_before_update
    run_manifest_all

    write_result_file "success" "ota script finished"
    log "done"
}

main "$@"

reboot
