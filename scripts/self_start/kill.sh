#!/bin/sh

SYSTEMD_UNIT="${SYSTEMD_UNIT:-zh_client}"
SUP_PID_FILE="/var/run/zh_client_supervisor.pid"
SUP_LOCK_DIR="/var/run/zh_client_supervisor.lock"
DEBUG_LOG="/tmp/zh_startup_debug.log"
SCRIPT_PATH="/data/zh_work/start_zh_client.sh"
FACE_ENGINE_SOCKET="/tmp/face_engine.sock"

is_running_pid() {
    [ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

find_related_pids() {
    ps | grep -E "start_zh_client\.sh|/data/zh_work/(zh_client|zh_ble_gatt_server|face/face_engine)" \
        | grep -v grep | awk '{print $1}'
}

stop_pid_list() {
    SIGNAL="$1"
    shift
    for PID in "$@"; do
        if is_running_pid "$PID"; then
            kill "$SIGNAL" "$PID" 2>/dev/null || true
        fi
    done
}

# Stop the transient service first so systemd cannot keep the launcher alive.
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop "$SYSTEMD_UNIT.service" 2>/dev/null || true
    systemctl reset-failed "$SYSTEMD_UNIT.service" 2>/dev/null || true
fi

# Stop the supervisor script before sweeping child processes.
if [ -x "$SCRIPT_PATH" ]; then
    "$SCRIPT_PATH" stop 2>/dev/null || true
fi

if [ -f "$SUP_PID_FILE" ]; then
    OLD_PID="$(cat "$SUP_PID_FILE" 2>/dev/null)"
    if is_running_pid "$OLD_PID"; then
        kill "$OLD_PID" 2>/dev/null || true
    fi
    rm -f "$SUP_PID_FILE"
fi

if command -v killall >/dev/null 2>&1; then
    killall zh_client zh_ble_gatt_server face_engine 2>/dev/null || true
fi

# Path-based sweeps also cover long process names that BusyBox killall may truncate.
RELATED_PIDS="$(find_related_pids)"
[ -z "$RELATED_PIDS" ] || stop_pid_list -TERM $RELATED_PIDS
sleep 1
RELATED_PIDS="$(find_related_pids)"
[ -z "$RELATED_PIDS" ] || stop_pid_list -KILL $RELATED_PIDS

rm -f "$SUP_PID_FILE" "$FACE_ENGINE_SOCKET"
rmdir "$SUP_LOCK_DIR" 2>/dev/null || true

echo "$(date '+%Y-%m-%d %H:%M:%S') [kill.sh] all related processes stopped" >>"$DEBUG_LOG"

exit 0
