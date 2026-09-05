#!/bin/sh
#
# RV1106 语音对话客户端部署脚本（幂等，可重复执行）。
# 板端为 uClibc/busybox 系统（无 systemd），服务以 /etc/init.d/S22zh_client
# 管理（服务名 zh_client，支持 start/stop/restart，开机自启）。
#
# 功能：部署构建产物到板端 + 安装开机自启服务 + 启动。
#
# 三种用法：
#   开发机 adb 部署:    bash scripts/install.sh [--adb]
#   开发机 ssh 部署:    ZH_BOARD=root@<板端IP> bash scripts/install.sh
#   板端本地部署:       将 release.tar 拷到板端后: mkdir -p /data/zh_work && tar -xf release.tar -C /data/zh_work && rm -f release.tar && sh /data/zh_work/install.sh
#                       （需 /data 空闲 >45MB；不足请用 adb 模式）
#
# 说明：
#   - 远程部署直接把产物传到板端 $BOARD_DIR.new（闪存），再换目录执行安装，
#     不经 /tmp（tmpfs，板端内存仅 100MB，整包解压会 OOM）
#   - 不覆盖板端已有的鉴权 key（$BOARD_DIR/key 存在则保留；服务器鉴权 key 由服务方发放）
#   - 不覆盖已录入的人脸库（$BOARD_DIR/face/save）
#   - 无 key 时可用 --key /path/to/key 提供，否则客户端会提示缺少 key
#   - --no-start 只部署不启动（开机自启仍启用）；--board-dir 指定部署目录（默认 /data/zh_work）
#   - 默认部署完成后自动重启设备（远程/板端模式均生效；重启后服务由 init 开机自启），
#     --no-reboot 可关闭
#
set -e

BOARD_DIR="${BOARD_DIR:-/data/zh_work}"
SERVICE_FILE="/etc/init.d/S22zh_client"
RELEASE=""
KEY_FILE=""
AUTO_START=1
AUTO_REBOOT="${ZH_REBOOT:-1}"
ADB_MODE=0
BOARD="${ZH_BOARD:-}"

usage() {
    cat <<'EOF'
Usage: install.sh [release.tar] [options]

Options:
  --key FILE      服务器鉴权 key（板端已有 key 时不覆盖）
  --adb           开发机模式：通过 adb 推送到板端安装
  --no-start      部署后不启动服务（开机自启仍会启用）
  --no-reboot     部署完成后不重启设备（默认部署后自动重启）
  --board-dir DIR 部署目录（默认 /data/zh_work）

环境变量:
  ZH_BOARD        开发机 ssh 模式: ZH_BOARD=root@<板端IP> bash install.sh
  ZH_REBOOT       默认 1（自动重启），ZH_REBOOT=0 关闭（等同 --no-reboot）
  BOARD_DIR
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --key)        shift; KEY_FILE="${1:?missing value for --key}" ;;
        --adb)        ADB_MODE=1 ;;
        --no-start)   AUTO_START=0 ;;
        --no-reboot)  AUTO_REBOOT=0 ;;
        --board-dir)  shift; BOARD_DIR="${1:?missing value for --board-dir}" ;;
        -h|--help)    usage; exit 0 ;;
        -*)           echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)            RELEASE="$1" ;;
    esac
    shift
done

# 未显式指定 key 且脚本旁存在演示 key（demo.key，随仓库/release 分发）时自动使用；
# 板端已有 key 时后续安装逻辑仍以保留为主，不会覆盖。
if [ -z "$KEY_FILE" ] && [ -f "$(dirname -- "$0")/demo.key" ]; then
    KEY_FILE="$(dirname -- "$0")/demo.key"
    echo "[install] 未指定 key，使用内置演示 key（$(dirname -- "$0")/demo.key）"
fi

# 定位本地产物目录：优先 build/artifacts（docker_build.sh 产出），否则解包 release.tar
resolve_artifacts() {
    CLIENT_ROOT=$(cd "$(dirname -- "$0")/.." && pwd)
    ARTIFACT_SRC="$CLIENT_ROOT/build/artifacts"
    if [ ! -d "$ARTIFACT_SRC" ]; then
        RELEASE_FALLBACK="${RELEASE:-$CLIENT_ROOT/build/release.tar}"
        [ -f "$RELEASE_FALLBACK" ] || { echo "[install] 找不到发布产物: build/artifacts 或 $RELEASE_FALLBACK（先运行 scripts/docker_build.sh）" >&2; exit 1; }
        STAGE_LOCAL=$(mktemp -d)
        tar -xf "$RELEASE_FALLBACK" -C "$STAGE_LOCAL"
        ARTIFACT_SRC="$STAGE_LOCAL"
        trap 'rm -rf "$STAGE_LOCAL"' EXIT
    fi
}

# 板端安装步骤（产物已就位于 $BOARD_DIR.new，这里做换目录 + 执行 install.sh）。
# 通过 tar/ssh 或 adb push 传输到板端 $BOARD_DIR.new，避免经 /tmp(tmpfs) 中转
# 解包（板端内存仅 100MB、/tmp 仅 48MB，整包解压会 OOM）。
REMOTE_INSTALL=$(cat <<'REMOTE_EOF'
set -e
B="$BOARD_DIR"
N="$BOARD_DIR.new"
[ -d "$N" ] || { echo "[install] $N 不存在，文件传输未完成" >&2; exit 1; }

# 备份已有鉴权 key 与人脸库（重装/升级不丢失）
if [ -f "$B/key" ]; then
    cp "$B/key" /tmp/zh_key.bak 2>/dev/null || true
fi
if [ -d "$B/face/save" ] && [ -n "$(ls -A "$B/face/save" 2>/dev/null)" ]; then
    rm -rf /tmp/zh_save.bak
    mkdir -p /tmp/zh_save.bak
    cp -a "$B/face/save/." /tmp/zh_save.bak/ 2>/dev/null || true
fi

# 停止旧服务
if [ -f /etc/init.d/S22zh_client ]; then
    sh /etc/init.d/S22zh_client stop 2>/dev/null || true
fi
killall zh_client zh_ble_gatt_server face_engine 2>/dev/null || true
rm -f /var/run/zh_client_supervisor.pid 2>/dev/null || true
rm -rf /var/run/zh_client_supervisor.lock 2>/dev/null || true

rm -rf "$B"
mv "$N" "$B"
mkdir -p "$B/face/save"
if [ -f /tmp/zh_key.bak ]; then
    cp /tmp/zh_key.bak "$B/key"
    chmod 600 "$B/key"
    rm -f /tmp/zh_key.bak
fi
if [ -d /tmp/zh_save.bak ]; then
    cp -a /tmp/zh_save.bak/. "$B/face/save/"
    rm -rf /tmp/zh_save.bak
fi
# install.sh 会启动后台守护进程（supervisor），守护进程继承当前 stdout/stderr；
# 若让安装输出直连 adb shell 的 pty，会因守护进程不释放 fd 导致会话永不退出。
# 因此安装输出重定向到文件，执行完再回显。
if [ -f /tmp/zh_client_key ]; then
    sh "$B/install.sh" --board-dir "$B" --key /tmp/zh_client_key </dev/null >/tmp/zh_install.out 2>&1
    rm -f /tmp/zh_client_key
else
    sh "$B/install.sh" --board-dir "$B" </dev/null >/tmp/zh_install.out 2>&1
fi
cat /tmp/zh_install.out
rm -f /tmp/zh_install.out

# 服务 start 是异步的（S22zh_client 内部 sleep 3 后才拉起 supervisor），
# 而远程会话（尤其 adb shell 的 pty）结束时会对未脱离会话的进程发 SIGHUP，
# 打断启动链并残留锁目录。因此等待 supervisor 就绪后再结束会话。
# （自动重启时无需等待：reboot 会终止一切，重启后服务由 init 开机自启）
if [ "$AUTO_START" = "1" ] && [ "$AUTO_REBOOT" != "1" ]; then
    i=0
    while [ "$i" -lt 15 ]; do
        if [ -f /var/run/zh_client_supervisor.pid ] && \
           kill -0 "$(cat /var/run/zh_client_supervisor.pid 2>/dev/null)" 2>/dev/null; then
            break
        fi
        sleep 1
        i=$((i + 1))
    done
    if [ "$i" -ge 15 ]; then
        echo "[install] WARN: supervisor 未在 15 秒内就绪，请查看 $B/logs/start_zh_client.log" >&2
    else
        echo "[install] supervisor 已就绪 (pid=$(cat /var/run/zh_client_supervisor.pid))"
    fi
fi

# 自动重启：nohup 脱离会话延时执行，让 ssh/adb 会话先干净退出再重启
# （避免会话在重启瞬间被中断产生非零退出码），重启后服务由 init 自启。
if [ "$AUTO_REBOOT" = "1" ]; then
    echo "[install] 部署完成，设备 3 秒后自动重启（重启后服务开机自启）"
    nohup sh -c 'sleep 3; reboot' </dev/null >/dev/null 2>&1 &
    exit 0
fi
REMOTE_EOF
)

# ---------------- 开发机 ssh 远程部署 ----------------
if [ -n "$BOARD" ]; then
    resolve_artifacts
    echo "[install] ssh 部署到 $BOARD"
    if [ -n "$KEY_FILE" ]; then
        scp -q "$KEY_FILE" "$BOARD:/tmp/zh_client_key"
    fi
    ssh "$BOARD" "rm -rf '$BOARD_DIR.new' && mkdir -p '$BOARD_DIR.new'"
    tar -cf - -C "$ARTIFACT_SRC" . | ssh "$BOARD" "tar -xf - -C '$BOARD_DIR.new'"
    ssh "$BOARD" "BOARD_DIR='$BOARD_DIR' AUTO_START='$AUTO_START' AUTO_REBOOT='$AUTO_REBOOT' sh -s" <<REMOTE_EOF
$REMOTE_INSTALL
REMOTE_EOF
    echo "[install] 完成: $BOARD"
    exit 0
fi

# ---------------- 开发机 adb 远程部署 ----------------
if [ "$ADB_MODE" = "1" ] || [ ! -f "$(dirname "$0")/zh_client" ]; then
    # 无板端上下文（脚本不在产物解包目录内）且未指定 ssh 主机 → adb 模式
    resolve_artifacts
    command -v adb >/dev/null 2>&1 || { echo "[install] 缺少 adb 命令" >&2; exit 1; }
    DEVICE="${ADB_SERIAL:-}"
    [ -n "$DEVICE" ] || DEVICE=$(adb devices | awk 'NR > 1 && $2 == "device" {print $1; exit}')
    [ -n "$DEVICE" ] || { echo "[install] 没有找到 adb 在线设备（可用 ADB_SERIAL 指定）" >&2; exit 1; }
    echo "[install] adb 部署到设备 $DEVICE"
    if [ -n "$KEY_FILE" ]; then
        adb -s "$DEVICE" push "$KEY_FILE" /tmp/zh_client_key
    fi
    # adb shell 的 pty 会改写二进制流（CR→NL）且不传递 stdin EOF（sh -s 会阻塞），
    # 因此：整目录 push 传输文件；远程脚本先 push 为文件再执行。
    adb -s "$DEVICE" shell "rm -rf '$BOARD_DIR.new'"
    adb -s "$DEVICE" push "$ARTIFACT_SRC" "$BOARD_DIR.new" | tail -1
    REMOTE_FILE=$(mktemp)
    printf '%s\n' "$REMOTE_INSTALL" > "$REMOTE_FILE"
    adb -s "$DEVICE" push "$REMOTE_FILE" /tmp/zh_remote_install.sh >/dev/null
    rm -f "$REMOTE_FILE"
    adb -s "$DEVICE" shell "BOARD_DIR='$BOARD_DIR' AUTO_START='$AUTO_START' AUTO_REBOOT='$AUTO_REBOOT' sh /tmp/zh_remote_install.sh"
    adb -s "$DEVICE" shell "rm -f /tmp/zh_remote_install.sh"
    echo "[install] 完成: $DEVICE"
    exit 0
fi

# ================= 板端部署 =================
[ "$(id -u)" = "0" ] || { echo "[install] 板端部署需要 root 权限" >&2; exit 1; }

# 定位产物根目录：本脚本所在目录即产物根（install.sh 随发布包分发）
BUNDLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
[ -f "$BUNDLE_DIR/zh_client" ] || { echo "[install] 产物不完整: $BUNDLE_DIR（缺少 zh_client）" >&2; exit 1; }

echo "[install] 停止旧服务..."
if [ -f "$SERVICE_FILE" ]; then
    sh "$SERVICE_FILE" stop 2>/dev/null || true
fi
killall zh_client zh_ble_gatt_server face_engine 2>/dev/null || true
rm -f /var/run/zh_client_supervisor.pid 2>/dev/null || true
rm -rf /var/run/zh_client_supervisor.lock 2>/dev/null || true

# 备份已有鉴权 key 与人脸库（重装/升级不丢失）
KEY_BACKUP=""
FACE_SAVE_BACKUP=""
if [ -f "$BOARD_DIR/key" ]; then
    echo "[install] 保留已有 key: $BOARD_DIR/key"
    KEY_BACKUP=$(mktemp)
    cp "$BOARD_DIR/key" "$KEY_BACKUP"
fi
if [ -d "$BOARD_DIR/face/save" ] && [ -n "$(ls -A "$BOARD_DIR/face/save" 2>/dev/null)" ]; then
    echo "[install] 保留已录入人脸库: $BOARD_DIR/face/save"
    FACE_SAVE_BACKUP=$(mktemp -d)
    cp -a "$BOARD_DIR/face/save/." "$FACE_SAVE_BACKUP/"
fi

if [ "$BUNDLE_DIR" != "$BOARD_DIR" ]; then
    echo "[install] 部署产物到 $BOARD_DIR"
    rm -rf "$BOARD_DIR"
    mkdir -p "$BOARD_DIR"
    cp -a "$BUNDLE_DIR/." "$BOARD_DIR/"
else
    echo "[install] 产物已在部署目录（$BOARD_DIR，跳过拷贝）"
fi

if [ -n "$KEY_BACKUP" ]; then
    cp "$KEY_BACKUP" "$BOARD_DIR/key"
    chmod 600 "$BOARD_DIR/key"
    rm -f "$KEY_BACKUP"
elif [ -n "$KEY_FILE" ]; then
    [ -f "$KEY_FILE" ] || { echo "[install] 找不到 key 文件: $KEY_FILE" >&2; exit 1; }
    cp "$KEY_FILE" "$BOARD_DIR/key"
    chmod 600 "$BOARD_DIR/key"
fi
if [ ! -f "$BOARD_DIR/key" ]; then
    echo "[install] WARN: 缺少服务器鉴权 key（$BOARD_DIR/key）。请与服务方联系获取，放入后重启服务。" >&2
fi
mkdir -p "$BOARD_DIR/face/save"
if [ -n "$FACE_SAVE_BACKUP" ]; then
    cp -a "$FACE_SAVE_BACKUP/." "$BOARD_DIR/face/save/"
    rm -rf "$FACE_SAVE_BACKUP"
fi

# AIVQE 配置（Rockit 音频预处理）；/oem 可写才安装
if [ -f "$BOARD_DIR/config_aivqe.json" ]; then
    if [ -w /oem/usr/share/vqefiles ] || mkdir -p /oem/usr/share/vqefiles 2>/dev/null; then
        cp -f "$BOARD_DIR/config_aivqe.json" /oem/usr/share/vqefiles/config_aivqe.json
        echo "[install] AIVQE 配置已安装: /oem/usr/share/vqefiles/config_aivqe.json"
    else
        echo "[install] WARN: /oem 不可写，跳过 AIVQE 配置安装（语音预处理将不可用）" >&2
    fi
fi

# rkipc（相机服务器）会抢占声卡/资源，改名禁其自启动（幂等）
if [ -f /oem/usr/bin/rkipc ] && [ ! -f /oem/usr/bin/rkipc-bak ]; then
    mv /oem/usr/bin/rkipc /oem/usr/bin/rkipc-bak
    echo "[install] 已禁用 rkipc 自启动（/oem/usr/bin/rkipc → rkipc-bak）"
elif [ -f /oem/usr/bin/rkipc ]; then
    echo "[install] rkipc 已禁用（rkipc-bak 存在）"
fi

echo "[install] 安装开机自启服务: $SERVICE_FILE"
cp -f "$BOARD_DIR/S22zh_client" "$SERVICE_FILE"
chmod +x "$SERVICE_FILE"
chmod +x "$BOARD_DIR/start_zh_client.sh" "$BOARD_DIR/kill.sh" 2>/dev/null || true
chmod +x "$BOARD_DIR/zh_client" "$BOARD_DIR/zh_ble_gatt_server" 2>/dev/null || true
[ ! -f "$BOARD_DIR/face/face_engine" ] || chmod +x "$BOARD_DIR/face/face_engine"

if [ "$AUTO_START" = "1" ]; then
    echo "[install] 启动服务..."
    sh "$SERVICE_FILE" start
    sleep 2
    if [ -f "$BOARD_DIR/logs/start_zh_client.log" ]; then
        echo "[install] 启动日志尾部:"
        tail -n 5 "$BOARD_DIR/logs/start_zh_client.log" 2>/dev/null || true
    fi
    echo "[install] 完成。服务: $SERVICE_FILE {start|stop|restart}，日志: $BOARD_DIR/logs"
else
    echo "[install] 部署完成（未启动，--no-start）。启动: sh $SERVICE_FILE start"
fi

if [ "$AUTO_REBOOT" = "1" ]; then
    echo "[install] 部署完成，3 秒后自动重启设备（重启后服务开机自启）"
    sleep 3
    reboot
fi
