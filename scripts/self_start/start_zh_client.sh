#!/bin/sh
# 东八区显示层时区（内核时钟保持 UTC，SNTP 校时写入 UTC 不变）。
# uclibc/busybox 支持 POSIX 时区字符串；若板子 /etc/TZ 已配置可删除此行。
export TZ=CST-8
BIN="/data/zh_work/zh_client"
SUP_PID_FILE="/var/run/zh_client_supervisor.pid"
SUP_LOCK_DIR="/var/run/zh_client_supervisor.lock"
SCRIPT_PATH="/data/zh_work/start_zh_client.sh"
LIB_DIR="/data/zh_work"
ONNX_LIB_REAL="$LIB_DIR/lib/libonnxruntime.so.1.17.3"
BOOT_DELAY_SEC=3
SYS_LD_LIBRARY_PATH="/oem/usr/lib:/oem/lib:/usr/lib:/lib"
LOG_DIR="$LIB_DIR/logs"
FACE_SAVE_DIR="$LIB_DIR/face/save"
DEBUG_LOG="$LOG_DIR/start_zh_client.log"
PROMPT_WAV_DIR="$LIB_DIR/prompt_wav"
BOOT_WAV="$PROMPT_WAV_DIR/boot.wav"
FACE_ENGINE_BIN="$LIB_DIR/face/face_engine"
FACE_ENGINE_SOCK="/tmp/face_engine.sock"
# 相机 ISP 3A（自动曝光/白平衡/对焦）由 rkaiq_3A_server 提供，视觉链路前置依赖。
# 最多尝试启动 3 次，失败则播报"视觉模块加载失败"提示音。
RKAIQ_BIN="/oem/usr/bin/rkaiq_3A_server"
RKAIQ_FLAG_FILE="/var/run/rkaiq_3a_server.started"
RKAIQ_MAX_RETRIES=3
VISION_LOAD_FAILED_WAV="$PROMPT_WAV_DIR/vision_load_failed.wav"
# 视觉链路使用摄像头采集路径；face_engine 为必选组件，默认开启视觉运行时
# 准备与清理（ZH_DISABLE_VISION=1 可显式关闭，一般不使用）。
ZH_DISABLE_VISION="${ZH_DISABLE_VISION:-0}"

if [ -n "$LD_LIBRARY_PATH" ]; then
    RKAIQ_LD_LIBRARY_PATH="$SYS_LD_LIBRARY_PATH:$LD_LIBRARY_PATH"
    # face_engine 由 zh_client fork/exec 启动，也会继承这里的库搜索路径。
    # 缺少 /oem/usr/lib 时，设备重启后常见的 librknnmrt.so 会加载失败，
    # 进而导致 /tmp/face_engine.sock 无法创建。
    CLIENT_LD_LIBRARY_PATH="$LIB_DIR/lib:$LIB_DIR:/usr/lib:/lib:$SYS_LD_LIBRARY_PATH:$LD_LIBRARY_PATH"
else
    RKAIQ_LD_LIBRARY_PATH="$SYS_LD_LIBRARY_PATH"
    CLIENT_LD_LIBRARY_PATH="$LIB_DIR/lib:$LIB_DIR:/usr/lib:/lib:$SYS_LD_LIBRARY_PATH"
fi

[ -x "$BIN" ] || chmod 755 "$BIN"

ensure_log_dir() {
    [ -d "$LOG_DIR" ] || mkdir -p "$LOG_DIR" 2>/dev/null || true
}

ensure_face_save_dir() {
    [ -d "$FACE_SAVE_DIR" ] || mkdir -p "$FACE_SAVE_DIR" 2>/dev/null
}

log_debug() {
    ensure_log_dir
    echo "$(date '+%Y-%m-%d %H:%M:%S') [start_zh_client] $*" >>"$DEBUG_LOG"
}

play_wav_prompt() {
    [ -f "$1" ] || {
        log_debug "prompt wav not found: $1"
        return 1
    }
    log_debug "play_wav_prompt start file=$1 path=$PATH"
    if command -v aplay >/dev/null 2>&1; then
        log_debug "play_wav_prompt use aplay: $(command -v aplay)"
        APLAY_ERR="$(LD_LIBRARY_PATH="/oem/usr/lib:/oem/lib:/usr/lib:/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" aplay "$1" 2>&1 >/dev/null; printf '__RC__%s' "$?")"
        APLAY_RC="${APLAY_ERR##*__RC__}"
        APLAY_MSG="${APLAY_ERR%__RC__*}"
        [ "$APLAY_MSG" = "$APLAY_ERR" ] && APLAY_MSG=""
        log_debug "play_wav_prompt aplay rc=$APLAY_RC err=${APLAY_MSG:-<empty>}"
        [ "$APLAY_RC" -eq 0 ] && return 0
    fi
    if command -v tinyplay >/dev/null 2>&1; then
        log_debug "play_wav_prompt use tinyplay: $(command -v tinyplay)"
        TINYPLAY_ERR="$(tinyplay "$1" 2>&1 >/dev/null; printf '__RC__%s' "$?")"
        TINYPLAY_RC="${TINYPLAY_ERR##*__RC__}"
        TINYPLAY_MSG="${TINYPLAY_ERR%__RC__*}"
        [ "$TINYPLAY_MSG" = "$TINYPLAY_ERR" ] && TINYPLAY_MSG=""
        log_debug "play_wav_prompt tinyplay rc=$TINYPLAY_RC err=${TINYPLAY_MSG:-<empty>}"
        [ "$TINYPLAY_RC" -eq 0 ] && return 0
    fi
    log_debug "no wav player found for $1"
    return 1
}

is_running_pid() {
    [ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

find_supervisor_pids() {
    ps | grep "$SCRIPT_PATH daemon" | grep -v grep | awk '{print $1}'
}

is_supervisor_running() {
    [ -f "$SUP_PID_FILE" ] || return 1
    OLD_PID="$(cat "$SUP_PID_FILE" 2>/dev/null)"
    is_running_pid "$OLD_PID" || return 1
    CMDLINE="$(cat /proc/$OLD_PID/cmdline 2>/dev/null | tr '\000' ' ')"
    echo "$CMDLINE" | grep -Fq "$SCRIPT_PATH daemon"
}

is_rkaiq_running() {
    ps | grep "$RKAIQ_BIN" | grep -v grep >/dev/null 2>&1
}

ensure_rkaiq_running() {
    RETRY_COUNT=1
    if is_rkaiq_running; then
        log_debug "rkaiq already running"
        touch "$RKAIQ_FLAG_FILE"
        return 0
    fi
    while [ "$RETRY_COUNT" -le "$RKAIQ_MAX_RETRIES" ]; do
        log_debug "try start rkaiq_3A_server attempt=$RETRY_COUNT/$RKAIQ_MAX_RETRIES ld_library_path=$RKAIQ_LD_LIBRARY_PATH"
        ensure_log_dir
        LD_LIBRARY_PATH="$RKAIQ_LD_LIBRARY_PATH" "$RKAIQ_BIN" --silent >/dev/null 2>&1 &
        sleep 1
        if is_rkaiq_running; then
            log_debug "rkaiq started success"
            touch "$RKAIQ_FLAG_FILE"
            return 0
        fi
        RETRY_COUNT=$((RETRY_COUNT + 1))
        log_debug "rkaiq not ready, retry after 1s"
        sleep 1
    done
    log_debug "rkaiq start failed after $RKAIQ_MAX_RETRIES attempts, play failure prompt"
    play_wav_prompt "$VISION_LOAD_FAILED_WAV" || true
    return 1
}

prepare_runtime_libs() {
    if [ "$ZH_DISABLE_VISION" = "1" ]; then
        log_debug "vision disabled, skip onnxruntime symlink check"
        return 0
    fi
    if [ ! -f "$ONNX_LIB_REAL" ]; then
        return 1
    fi
    [ -e "$LIB_DIR/libonnxruntime.so.1" ] || ln -sf "$ONNX_LIB_REAL" "$LIB_DIR/libonnxruntime.so.1"
    [ -e "$LIB_DIR/libonnxruntime.so" ] || ln -sf "$ONNX_LIB_REAL" "$LIB_DIR/libonnxruntime.so"
    return 0
}

cleanup_face_engine() {
    if [ "$ZH_DISABLE_VISION" = "1" ]; then
        log_debug "vision disabled, skip face_engine cleanup"
        return 0
    fi
    if command -v killall >/dev/null 2>&1; then
        killall face_engine 2>/dev/null || true
    else
        for PID in $(ps | grep "$FACE_ENGINE_BIN" | grep -v grep | awk '{print $1}'); do
            kill "$PID" 2>/dev/null || true
        done
    fi
    [ -S "$FACE_ENGINE_SOCK" ] && rm -f "$FACE_ENGINE_SOCK"
}

daemon_loop() {
    log_debug "daemon enter, boot delay=${BOOT_DELAY_SEC}s"
    play_wav_prompt "$BOOT_WAV" || true
    sleep "$BOOT_DELAY_SEC"
    log_debug "daemon delay done"
    while true; do
        if ! prepare_runtime_libs; then
            log_debug "onnxruntime missing, retry after 2s"
            sleep 2
            continue
        fi
        ensure_rkaiq_running || log_debug "rkaiq start failed, continue launch zh_client"
        cleanup_face_engine
        log_debug "cleanup stale face_engine before launch"
        if ! ensure_face_save_dir; then
            log_debug "face save dir unavailable: $FACE_SAVE_DIR, retry after 2s"
            sleep 2
            continue
        fi
        log_debug "launch zh_client ld_library_path=$CLIENT_LD_LIBRARY_PATH"
        ensure_log_dir
        CLIENT_ERR_FILE="$LOG_DIR/zh_client.stderr"
        : >"$CLIENT_ERR_FILE"
        LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" "$BIN" >/dev/null 2>"$CLIENT_ERR_FILE"
        CLIENT_RC="$?"
        CLIENT_MSG="$(cat "$CLIENT_ERR_FILE" 2>/dev/null)"
        [ -n "$CLIENT_MSG" ] || CLIENT_MSG="<empty>"
        log_debug "zh_client exited rc=$CLIENT_RC err=$CLIENT_MSG, restart after 2s"
        sleep 2
    done
}

start_supervisor() {
    if ! mkdir "$SUP_LOCK_DIR" 2>/dev/null; then
        # 锁残留（进程被强杀/会话中断等导致 EXIT trap 未执行）时自愈：
        # supervisor 未在运行则视为残留，清理后重试一次
        if ! is_supervisor_running; then
            log_debug "stale supervisor lock detected, remove and retry"
            rm -rf "$SUP_LOCK_DIR"
            mkdir "$SUP_LOCK_DIR" 2>/dev/null || {
                log_debug "supervisor lock exists, skip duplicate start"
                exit 0
            }
        else
            log_debug "supervisor already running, skip duplicate start"
            exit 0
        fi
    fi
    trap 'rm -rf "$SUP_LOCK_DIR" 2>/dev/null || true' EXIT INT TERM

    if is_supervisor_running; then
        log_debug "supervisor already running, skip"
        exit 0
    fi
    rm -f "$SUP_PID_FILE"
    ensure_log_dir
    setsid /bin/sh "$SCRIPT_PATH" daemon >>"$DEBUG_LOG" 2>&1 < /dev/null &
    echo $! >"$SUP_PID_FILE"
    log_debug "supervisor started pid=$!"
}

stop_supervisor() {
    for PID in $(find_supervisor_pids); do
        if is_running_pid "$PID"; then
            kill "$PID" 2>/dev/null || true
        fi
    done
    if [ -f "$SUP_PID_FILE" ]; then
        OLD_PID="$(cat "$SUP_PID_FILE" 2>/dev/null)"
        if is_running_pid "$OLD_PID"; then
            kill "$OLD_PID" 2>/dev/null || true
        fi
        rm -f "$SUP_PID_FILE"
    fi
    if command -v killall >/dev/null 2>&1; then
        killall zh_client 2>/dev/null || true
    else
        for PID in $(ps | grep "$BIN" | grep -v grep | awk '{print $1}'); do
            kill "$PID" 2>/dev/null || true
        done
    fi
    # 停止即清理锁目录，避免残留导致下次 start 被跳过（restart 场景）
    rm -rf "$SUP_LOCK_DIR" 2>/dev/null || true
}

case "$1" in
    daemon)
        log_debug "argv=daemon"
        daemon_loop
        ;;
    start|"")
        log_debug "argv=start"
        start_supervisor
        ;;
    stop)
        log_debug "argv=stop"
        stop_supervisor
        ;;
    restart)
        log_debug "argv=restart"
        stop_supervisor
        start_supervisor
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
