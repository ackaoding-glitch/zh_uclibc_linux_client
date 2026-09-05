#!/bin/sh
#
# RV1106 语音对话客户端卸载脚本。
# 默认保留服务器鉴权 key（$BOARD_DIR/key），重装时无需重新申请；
# --purge 连 key 与人脸库一起删除。
#
# 三种用法：
#   开发机 adb 卸载:    bash scripts/uninstall.sh [--adb]
#   开发机 ssh 卸载:    ZH_BOARD=root@<板端IP> bash scripts/uninstall.sh
#   板端本地卸载:      sh uninstall.sh
#
set -e

BOARD_DIR="${BOARD_DIR:-/data/zh_work}"
SERVICE_FILE="/etc/init.d/S22zh_client"
PURGE="${PURGE:-0}"
ADB_MODE=0
BOARD="${ZH_BOARD:-}"

usage() {
    cat <<'EOF'
Usage: uninstall.sh [options]

Options:
  --purge         连服务器鉴权 key 与人脸库一起删除（默认保留 $BOARD_DIR/key）
  --adb           开发机模式：通过 adb 在板端卸载
  --board-dir DIR 部署目录（默认 /data/zh_work）

环境变量:
  ZH_BOARD        开发机 ssh 模式: ZH_BOARD=root@<板端IP> bash uninstall.sh
  BOARD_DIR
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --purge)      PURGE=1 ;;
        --adb)        ADB_MODE=1 ;;
        --board-dir)  shift; BOARD_DIR="${1:?missing value for --board-dir}" ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------------- 开发机 ssh 远程卸载 ----------------
if [ -n "$BOARD" ]; then
    echo "[uninstall] ssh 卸载: $BOARD"
    scp -q "$0" "$BOARD:/tmp/zh_uninstall.sh"
    ssh "$BOARD" "BOARD_DIR='$BOARD_DIR' PURGE='$PURGE' sh /tmp/zh_uninstall.sh"
    ssh "$BOARD" "rm -f /tmp/zh_uninstall.sh"
    exit 0
fi

# ---------------- 开发机 adb 远程卸载 ----------------
if [ "$ADB_MODE" = "1" ]; then
    command -v adb >/dev/null 2>&1 || { echo "[uninstall] 缺少 adb 命令" >&2; exit 1; }
    DEVICE="${ADB_SERIAL:-}"
    [ -n "$DEVICE" ] || DEVICE=$(adb devices | awk 'NR > 1 && $2 == "device" {print $1; exit}')
    [ -n "$DEVICE" ] || { echo "[uninstall] 没有找到 adb 在线设备（可用 ADB_SERIAL 指定）" >&2; exit 1; }
    echo "[uninstall] adb 卸载: $DEVICE"
    adb -s "$DEVICE" push "$0" /tmp/zh_uninstall.sh
    adb -s "$DEVICE" shell "BOARD_DIR='$BOARD_DIR' PURGE='$PURGE' sh /tmp/zh_uninstall.sh"
    adb -s "$DEVICE" shell "rm -f /tmp/zh_uninstall.sh"
    exit 0
fi

# ================= 板端卸载 =================
[ "$(id -u)" = "0" ] || { echo "[uninstall] 板端卸载需要 root 权限" >&2; exit 1; }

echo "[uninstall] 停止并移除服务: $SERVICE_FILE"
if [ -f "$SERVICE_FILE" ]; then
    sh "$SERVICE_FILE" stop 2>/dev/null || true
    rm -f "$SERVICE_FILE"
fi
killall zh_client zh_ble_gatt_server face_engine 2>/dev/null || true
rm -f /var/run/zh_client_supervisor.pid 2>/dev/null || true
rm -rf /var/run/zh_client_supervisor.lock 2>/dev/null || true
rm -f /tmp/face_engine.sock 2>/dev/null || true

# 恢复 rkipc（--purge 时连带恢复系统相机服务）
if [ "$PURGE" = "1" ] && [ -f /oem/usr/bin/rkipc-bak ] && [ ! -f /oem/usr/bin/rkipc ]; then
    mv /oem/usr/bin/rkipc-bak /oem/usr/bin/rkipc
    echo "[uninstall] 已恢复 rkipc"
fi

if [ "$PURGE" = "1" ]; then
    echo "[uninstall] --purge: 删除部署目录（含 key 与人脸库）: $BOARD_DIR"
    rm -rf "$BOARD_DIR"
else
    echo "[uninstall] 保留服务器鉴权 key: $BOARD_DIR/key"
    if [ -f "$BOARD_DIR/key" ]; then
        KEY_TMP=$(mktemp)
        cp "$BOARD_DIR/key" "$KEY_TMP"
        rm -rf "$BOARD_DIR"
        mkdir -p "$BOARD_DIR"
        cp "$KEY_TMP" "$BOARD_DIR/key"
        chmod 600 "$BOARD_DIR/key"
        rm -f "$KEY_TMP"
    else
        rm -rf "$BOARD_DIR"
    fi
fi

echo "[uninstall] 完成"
