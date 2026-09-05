#!/usr/bin/env bash
#
# RV1106 完整编译脚本 (armv7 + uclibc)
# 编译产物: zh_client + bithion-core(.so) + face_engine
#
# 用法:
#   ./scripts/rv1106_build_all.sh              # 全量编译+推送
#   ./scripts/rv1106_build_all.sh --skip-push  # 只编译不推送
#   ./scripts/rv1106_build_all.sh --help       # 查看帮助
#
# 产物目录: /tmp/zh_client_rv1106_artifacts
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CLIENT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# ============================================================
# 固定路径（本脚本为内部开发脚本：从源码构建全部组件。
# 开源用户请使用 scripts/docker_build.sh（预构建 SDK 分发版）。
# 所有路径均可通过环境变量覆盖）
# ============================================================
BITHION_ROOT="${BITHION_ROOT:-}"
FACE_ROOT="${FACE_ROOT:-}"

# Rockchip RV1106 SDK
RV1106_SDK_ROOT="${RV1106_SDK_ROOT:-}"
RK_SDK_ROOT="${RK_SDK_ROOT:-$RV1106_SDK_ROOT}"

# RV1106 交叉编译工具链
RV1106_TOOLCHAIN_ROOT="${RV1106_TOOLCHAIN_ROOT:-${RV1106_SDK_ROOT:+$RV1106_SDK_ROOT/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf}}"
RV1106_GCC="${RV1106_GCC:-${RV1106_TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc-8.3.0}"
RV1106_GPP="${RV1106_GPP:-${RV1106_TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-g++}"

# bithion-core 从源码编译的默认 SDK 路径
BITHION_SDK="${BITHION_SDK:-/tmp/bithion_core_rv1106_sdk}"
# bithion-core 预编译 SDK (--skip-bithion 时使用)
BITHION_PREBUILT_SDK="${BITHION_PREBUILT_SDK:-${CLIENT_ROOT}/sdk/bithion-core-sdk}"

# 预编译依赖
RV1106_PREBUILT_ROOT="${RV1106_PREBUILT_ROOT:-${CLIENT_ROOT}/third_party/prebuilt/rv1106-uclibc-armv7}"

# 构建目录
BITHION_RV1106_BUILD="${BITHION_RV1106_BUILD:-/tmp/bithion_core_rv1106_sdk_build}"
RV1106_BUILD="${RV1106_BUILD:-/tmp/zh_client_rv1106_build}"
ARTIFACT_DIR="${ARTIFACT_DIR:-/tmp/zh_client_rv1106_artifacts}"

# 推送
PUSH_METHOD="${PUSH_METHOD:-adb}"
ADB_BIN="${ADB_BIN:-adb}"
ADB_SERIAL="${ADB_SERIAL:-}"
BOARD_DIR="${BOARD_DIR:-/data/zh_work}"
BOARD_HOST="${BOARD_HOST:-}"

# 构建选项
JOBS="${JOBS:-$(nproc)}"

# 阶段开关
SKIP_BITHION=0
SKIP_FACE=0
SKIP_MAIN=0
SKIP_PUSH=0
PUSH_ONLY=0
RUN_ONLY=0

# 默认推送项 (推送程序实际依赖的 bithion-core SONAME，可通过 --push-item 覆盖)
PUSH_ITEMS=()
DEFAULT_PUSH_ITEMS=("zh_client" "libbithion-core.so.1")

# ============================================================
# 帮助信息
# ============================================================
usage() {
    cat <<'EOF'
Usage:
  scripts/rv1106_build_all.sh [options]

Options:
  --skip-bithion       跳过 bithion-core 构建 (使用已有 SDK)
  --skip-face          跳过 face_engine 构建
  --skip-main          跳过 zh_client 主包构建
  --skip-push          只构建不推送
  --push-only          仅推送已构建产物并启动 (跳过编译)
  --run-only           仅运行板端已有 zh_client (跳过编译和推送)
  --push-item NAME     仅推送指定文件/目录 (可重复, 默认: zh_client libbithion-core.so.1)
  --adb-serial SERIAL  指定 adb 设备序列号
  --ssh USER@HOST      使用 ssh/scp 推送
  --board-dir PATH     板端目标路径 (默认 /data/zh_work)
  -j N                 并行编译任务数 (默认 nproc)
  -h, --help           显示帮助

环境变量:
  BITHION_ROOT, FACE_ROOT, ARTIFACT_DIR, BOARD_DIR, JOBS,
  RV1106_SDK_ROOT, RV1106_TOOLCHAIN_ROOT, RK_SDK_ROOT, BITHION_SDK,
  RV1106_PREBUILT_ROOT, ADB_BIN, ADB_SERIAL.
EOF
}

# ============================================================
# 参数解析
# ============================================================
while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-bithion)   SKIP_BITHION=1 ;;
        --skip-face)      SKIP_FACE=1 ;;
        --skip-main)      SKIP_MAIN=1 ;;
        --skip-push)      SKIP_PUSH=1 ;;
        --push-only)      PUSH_ONLY=1 ;;
        --run-only)       RUN_ONLY=1 ;;
        --push-item)      shift; PUSH_ITEMS+=("${1:?missing --push-item value}") ;;
        --adb-serial)     shift; ADB_SERIAL="${1:?missing adb serial}" ;;
        --ssh)            shift; PUSH_METHOD=ssh; BOARD_HOST="${1:?missing ssh host}" ;;
        --board-dir)      shift; BOARD_DIR="${1:?missing board dir}" ;;
        -j)               shift; JOBS="${1:?missing job count}" ;;
        -h|--help)        usage; exit 0 ;;
        *)                echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ============================================================
# 工具函数
# ============================================================
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

log() {
    printf '\n[%s] %b%s%b\n' "$(date '+%H:%M:%S')" "${GREEN}" "$*" "${NC}"
}

warn() {
    printf '[%s] %bWARN:%b %s\n' "$(date '+%H:%M:%S')" "${YELLOW}" "${NC}" "$*"
}

die() {
    printf '[%s] %bERROR:%b %s\n' "$(date '+%H:%M:%S')" "${RED}" "${NC}" "$*" >&2
    exit 1
}

require_file() {
    [ -f "$1" ] || die "missing required file: $1"
}

require_dir() {
    [ -d "$1" ] || die "missing required directory: $1"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

check_arch_arm32() {
    local f="$1"
    local arch
    arch=$(file -L "$f" 2>/dev/null || true)

    # ELF 32-bit ARM 动态库/可执行文件
    if echo "$arch" | grep -q "ELF 32-bit.*ARM"; then
        echo "  [OK] armv7: $f"
        return 0
    fi

    # .a 静态库 (ar archive) — 用 objdump 检查
    if echo "$arch" | grep -q "ar archive"; then
        if "${RV1106_TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-objdump" -f "$f" >/dev/null 2>&1; then
            echo "  [OK] armv7 (ar): $f"
            return 0
        fi
    fi

    die "架构不对 (期望 ARM 32-bit): $f -- $arch"
}

# ============================================================
# Step 0: 环境检查
# ============================================================
env_check() {
    log "=== Step 0: 环境检查 ==="

    [ -n "$RV1106_SDK_ROOT" ] || die "未设置 RV1106_SDK_ROOT（Rockchip RV1106 SDK 根目录，含工具链/media 库）"
    [ -n "$BITHION_ROOT" ] || die "未设置 BITHION_ROOT（bithion-core 源码目录）"
    [ -n "$FACE_ROOT" ] || die "未设置 FACE_ROOT（face_engine 源码目录）"

    # 交叉工具链
    require_file "$RV1106_GCC"
    require_file "$RV1106_GPP"
    echo ""
    echo "RV1106 工具链:"
    echo "  [OK] gcc: $("$RV1106_GCC" --version | head -1)"
    echo "  [OK] g++: $("$RV1106_GPP" --version | head -1)"

    # CMake toolchain 文件会从 gcc-8.3.0 推导 g++-8.3.0，确保存在
    local gpp_ver="${RV1106_TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-g++-8.3.0"
    if [ ! -e "$gpp_ver" ]; then
        ln -sf arm-rockchip830-linux-uclibcgnueabihf-g++ "$gpp_ver" 2>/dev/null || true
    fi

    export TOOLCHAIN_ROOT="$RV1106_TOOLCHAIN_ROOT"

    # bithion-core 预编译依赖
    echo ""
    echo "bithion-core RV1106 预编译依赖:"
    local mbedtls_lib="$BITHION_ROOT/third_party/mbedtls_prebuilt/lib/libmbedcrypto.a"
    local fvad_lib="$BITHION_ROOT/third_party/libfvad_prebuilt/lib/libfvad.a"
    local onnx_lib="$BITHION_ROOT/third_party/onnxruntime_prebuilt/lib/libonnxruntime.so.1.17.3"
    require_file "$mbedtls_lib"
    require_file "$fvad_lib"
    require_file "$onnx_lib"
    check_arch_arm32 "$onnx_lib"

    require_file "$BITHION_ROOT/cmake/toolchain-rv1106.cmake"

    # RV1106 onnxruntime 头文件嵌套在 onnxruntime/core/session/ 下，创建扁平 symlink
    local onnx_inc="$BITHION_ROOT/third_party/onnxruntime_prebuilt/include"
    if [ -d "$onnx_inc/onnxruntime/core/session" ] && [ ! -f "$onnx_inc/onnxruntime_c_api.h" ]; then
        for f in "$onnx_inc"/onnxruntime/core/session/*.h "$onnx_inc"/onnxruntime/core/providers/cpu/*.h; do
            [ -f "$f" ] && ln -sf "$(echo "$f" | sed "s|^$onnx_inc/||")" "$onnx_inc/$(basename "$f")" 2>/dev/null
        done
    fi

    # Rockchip SDK
    echo ""
    echo "Rockchip SDK:"
    if [ -d "$RK_SDK_ROOT" ]; then
        echo "  [OK] RK_SDK_ROOT: $RK_SDK_ROOT"
    else
        warn "RK_SDK_ROOT 不存在: $RK_SDK_ROOT (rockit 库可能无法链接)"
    fi

    # face_engine
    require_file "$FACE_ROOT/build.sh"
    echo ""
    echo "face_engine: $FACE_ROOT"

    # ADB
    if [ "$SKIP_PUSH" -eq 0 ]; then
        require_cmd adb
        if [ -z "$ADB_SERIAL" ]; then
            ADB_SERIAL=$(adb devices | awk 'NR > 1 && $2 == "device" {print $1; exit}')
            [ -n "$ADB_SERIAL" ] || die "没有找到 adb 在线设备"
        fi
        echo ""
        echo "ADB 设备: $ADB_SERIAL"
    fi

    echo ""
    log "环境检查通过"
}

# ============================================================
# Step 1: 构建 bithion-core (RV1106 通常用预编译 SDK，可选从源码构建)
# ============================================================
build_bithion_core() {
    log "=== Step 1: 构建 bithion-core (RV1106 / armv7 / uclibc) ==="

    require_dir "$BITHION_ROOT"
    require_file "$BITHION_ROOT/cmake/toolchain-rv1106.cmake"

    echo "  Source:       $BITHION_ROOT"
    echo "  Toolchain:    $RV1106_TOOLCHAIN_ROOT"
    echo "  Build dir:    $BITHION_RV1106_BUILD"
    echo "  Install dir:  $BITHION_SDK"

    rm -rf "$BITHION_RV1106_BUILD" "$BITHION_SDK"
    cmake -S "$BITHION_ROOT" \
        -B "$BITHION_RV1106_BUILD" \
        -DBITHION_TARGET_PLATFORM=rv1106 \
        -DCMAKE_TOOLCHAIN_FILE="$BITHION_ROOT/cmake/toolchain-rv1106.cmake" \
        -DTOOLCHAIN_ROOT="$RV1106_TOOLCHAIN_ROOT" \
        -DBITHION_CORE_ENABLE_AUDIO=ON \
        -DBITHION_CORE_ENABLE_SILERO_VAD=ON \
        -DBITHION_CORE_ENABLE_TLS=ON \
        -DCMAKE_INSTALL_PREFIX="$BITHION_SDK"

    cmake --build "$BITHION_RV1106_BUILD" -j"$JOBS"
    cmake --install "$BITHION_RV1106_BUILD"

    # 验证产物
    echo ""
    log "验证 bithion-core 产物:"
    require_file "$BITHION_SDK/include/bithion_core.h"
    require_file "$BITHION_SDK/lib/libbithion-core.so.1"
    check_arch_arm32 "$BITHION_SDK/lib/libbithion-core.so.1"
    if [ -f "$BITHION_SDK/lib/libonnxruntime.so" ]; then
        check_arch_arm32 "$BITHION_SDK/lib/libonnxruntime.so"
    fi

    echo ""
    log "bithion-core RV1106 构建完成 -> $BITHION_SDK"
}

# ============================================================
# Step 2: 构建 face_engine
# ============================================================
build_face_engine() {
    log "=== Step 2: 构建 face_engine (RV1106) ==="

    require_file "$FACE_ROOT/build.sh"

    echo "  Source:  $FACE_ROOT"
    echo "  Target:  rv1106 (uclibc)"

    # face_engine 的 CMake 通过 LUCKFOX_SDK_PATH 找工具链
    export LUCKFOX_SDK_PATH="$RK_SDK_ROOT"
    (cd "$FACE_ROOT" && ./build.sh rv1106)

    local face_bin="$FACE_ROOT/install/uclibc/luckfox_pico_retinaface_facenet_demo/face_engine"
    if [ ! -f "$face_bin" ]; then
        # 尝试 glibc 路径 (RV1106 有时也用 glibc 工具链)
        face_bin="$FACE_ROOT/install/glibc/luckfox_pico_retinaface_facenet_demo/face_engine"
    fi
    require_file "$face_bin"
    check_arch_arm32 "$face_bin"

    echo ""
    log "face_engine 构建完成 -> $face_bin"
}

# ============================================================
# Step 3: 构建 zh_client 主包
# ============================================================
build_main_client() {
    log "=== Step 3: 构建 zh_client (RV1106 / armv7 / uclibc) ==="

    require_file "$RV1106_GCC"
    require_dir "$BITHION_SDK"

    echo "  Toolchain:    $RV1106_TOOLCHAIN_ROOT"
    echo "  Bithion SDK:  $BITHION_SDK"
    echo "  Prebuilt:     $RV1106_PREBUILT_ROOT"
    echo "  RK SDK:       $RK_SDK_ROOT"
    echo "  Build dir:    $RV1106_BUILD"

    # 清理旧的 cmake 缓存，避免 BITHION_CORE_SDK_LIB 等路径指向旧 SDK
    rm -rf "$RV1106_BUILD"

    cmake -S "$CLIENT_ROOT" \
        -B "$RV1106_BUILD" \
        -DZH_TARGET_PLATFORM=rv1106 \
        -DCMAKE_TOOLCHAIN_FILE="$CLIENT_ROOT/cmake/toolchain.cmake" \
        -DTOOLCHAIN_ROOT="$RV1106_TOOLCHAIN_ROOT" \
        -DBITHION_CORE_SDK_ROOT="$BITHION_SDK" \
        -DZH_PREBUILT_ROOT="$RV1106_PREBUILT_ROOT" \
        -DRK_SDK_ROOT="$RK_SDK_ROOT" \
        -DZH_ENABLE_FACE=ON \
        -DZH_ENABLE_VISION_CAPTURE=OFF \
        -DZH_BUILD_BLE_GATT_SERVER=ON \
        -DZH_BUILD_FACE_TEST=OFF

    cmake --build "$RV1106_BUILD" -j"$JOBS"

    # 验证产物
    echo ""
    log "验证 zh_client 产物:"
    require_file "$RV1106_BUILD/zh_client"
    check_arch_arm32 "$RV1106_BUILD/zh_client"

    # BLE GATT helper
    if [ -f "$RV1106_BUILD/zh_ble_gatt_server" ]; then
        check_arch_arm32 "$RV1106_BUILD/zh_ble_gatt_server"
    fi

    echo ""
    log "zh_client RV1106 构建完成"
}

# ============================================================
# Step 4: 合并产物
# ============================================================
restore_artifacts() {
    log "=== Step 4: 合并产物到 $ARTIFACT_DIR ==="

    require_file "$RV1106_BUILD/zh_client"
    require_file "$BITHION_SDK/lib/libbithion-core.so.1"

    local face_install_dir="$FACE_ROOT/install/uclibc/luckfox_pico_retinaface_facenet_demo"
    if [ ! -d "$face_install_dir" ]; then
        face_install_dir="$FACE_ROOT/install/glibc/luckfox_pico_retinaface_facenet_demo"
    fi
    local face_model_dir="$FACE_ROOT/scripts/luckfox_onnx_to_rknn/model"

    rm -rf "$ARTIFACT_DIR"
    mkdir -p "$ARTIFACT_DIR/lib" \
             "$ARTIFACT_DIR/face/lib" \
             "$ARTIFACT_DIR/face/model" \
             "$ARTIFACT_DIR/face/save" \
             "$ARTIFACT_DIR/logs"

    # zh_client
    cp -a "$RV1106_BUILD/zh_client" "$ARTIFACT_DIR/zh_client"
    chmod +x "$ARTIFACT_DIR/zh_client"

    # BLE GATT
    if [ -f "$RV1106_BUILD/zh_ble_gatt_server" ]; then
        cp -a "$RV1106_BUILD/zh_ble_gatt_server" "$ARTIFACT_DIR/zh_ble_gatt_server"
        chmod +x "$ARTIFACT_DIR/zh_ble_gatt_server"
    fi

    # bithion-core 库
    cp -a "$BITHION_SDK/lib/libbithion-core.so"* "$ARTIFACT_DIR/lib/" 2>/dev/null || true
    if [ -f "$BITHION_SDK/lib/libonnxruntime.so" ]; then
        cp -a "$BITHION_SDK/lib/libonnxruntime.so"* "$ARTIFACT_DIR/lib/" 2>/dev/null || true
    fi

    # 预编译依赖库 (opus, alsa 等)
    if [ -d "$RV1106_PREBUILT_ROOT/opus/lib" ]; then
        cp -a "$RV1106_PREBUILT_ROOT/opus/lib/"*.so* "$ARTIFACT_DIR/lib/" 2>/dev/null || true
    fi
    if [ -d "$RV1106_PREBUILT_ROOT/alsa/lib" ]; then
        cp -a "$RV1106_PREBUILT_ROOT/alsa/lib/"*.so* "$ARTIFACT_DIR/lib/" 2>/dev/null || true
    fi

    # face_engine
    if [ -d "$face_install_dir" ]; then
        cp -a "$face_install_dir/face_engine" "$ARTIFACT_DIR/face/face_engine"
        chmod +x "$ARTIFACT_DIR/face/face_engine"
        if [ -d "$face_install_dir/lib" ]; then
            cp -a "$face_install_dir/lib/"*.so* "$ARTIFACT_DIR/face/lib/" 2>/dev/null || true
        fi
        if [ -f "$face_install_dir/libluckfox_pico_retinaface_facenet_api.so" ]; then
            cp -a "$face_install_dir/libluckfox_pico_retinaface_facenet_api.so" "$ARTIFACT_DIR/face/lib/"
        fi
    else
        warn "face_engine 安装目录不存在，跳过 face 产物"
    fi

    if [ -d "$face_model_dir" ]; then
        cp -a "$face_model_dir/." "$ARTIFACT_DIR/face/model/"
    fi

    # 启动脚本
    if [ -f "$CLIENT_ROOT/scripts/self_start/start_zh_client.sh" ]; then
        cp -a "$CLIENT_ROOT/scripts/self_start/start_zh_client.sh" "$ARTIFACT_DIR/start_zh_client.sh"
        chmod +x "$ARTIFACT_DIR/start_zh_client.sh"
    fi
    if [ -f "$CLIENT_ROOT/scripts/self_start/kill.sh" ]; then
        cp -a "$CLIENT_ROOT/scripts/self_start/kill.sh" "$ARTIFACT_DIR/kill.sh"
        chmod +x "$ARTIFACT_DIR/kill.sh"
    fi

    # certs / prompt (可选)
    for d in certs prompt_mp3 prompt_wav; do
        if [ -d "$CLIENT_ROOT/scripts/installer/$d" ]; then
            cp -a "$CLIENT_ROOT/scripts/installer/$d" "$ARTIFACT_DIR/$d"
        fi
    done

    # 汇总
    echo ""
    log "产物清单:"
    echo "============================================"
    echo "  $ARTIFACT_DIR/"
    find "$ARTIFACT_DIR" -mindepth 1 -maxdepth 1 | sort | while read -r f; do
        local name; name=$(basename "$f")
        if [ -f "$f" ]; then
            printf "    %-35s %s\n" "$name" "$(stat -c%s "$f" 2>/dev/null || echo '?') bytes"
        elif [ -d "$f" ]; then
            printf "    %-35s [dir]\n" "$name/"
        fi
    done
    echo "============================================"

    # 架构验证
    echo ""
    log "产物架构验证:"
    for bin in zh_client zh_ble_gatt_server; do
        if [ -f "$ARTIFACT_DIR/$bin" ]; then
            check_arch_arm32 "$ARTIFACT_DIR/$bin"
        fi
    done
    for lib in lib/libbithion-core.so.1 lib/libonnxruntime.so; do
        if [ -f "$ARTIFACT_DIR/$lib" ]; then
            check_arch_arm32 "$ARTIFACT_DIR/$lib"
        fi
    done
    if [ -f "$ARTIFACT_DIR/face/face_engine" ]; then
        check_arch_arm32 "$ARTIFACT_DIR/face/face_engine"
    fi

    local total_size
    total_size=$(du -sh "$ARTIFACT_DIR" 2>/dev/null | cut -f1)
    echo ""
    log "产物总大小: $total_size"
    log "产物合并完成 -> $ARTIFACT_DIR"
}

# ============================================================
# ============================================================
# run_zh_client: 在板端启动 zh_client (不推送)
# ============================================================
run_zh_client() {
    log "启动 zh_client..."

    if [ "$PUSH_METHOD" = "adb" ]; then
        require_cmd "$ADB_BIN"
        [ -n "$ADB_SERIAL" ] || ADB_SERIAL=$(adb devices | awk 'NR > 1 && $2 == "device" {print $1; exit}')
        [ -n "$ADB_SERIAL" ] || die "没有找到 adb 在线设备"
        echo "  设备: $ADB_SERIAL"
        echo "  命令: cd $BOARD_DIR && ./zh_client"
        "$ADB_BIN" -s "$ADB_SERIAL" shell "cd '$BOARD_DIR' && export LD_LIBRARY_PATH='$BOARD_DIR:/usr/lib:\$LD_LIBRARY_PATH' && ./zh_client"
    else
        require_cmd ssh
        [ -n "$BOARD_HOST" ] || die "ssh 模式下需要 BOARD_HOST"
        echo "  主机: $BOARD_HOST"
        echo "  命令: cd $BOARD_DIR && ./zh_client"
        ssh "$BOARD_HOST" "cd '$BOARD_DIR' && export LD_LIBRARY_PATH='$BOARD_DIR:/usr/lib:\$LD_LIBRARY_PATH' && ./zh_client"
    fi
}

# ============================================================
# Step 5: 推送到板端
# ============================================================
push_artifacts() {
    log "=== Step 5: 推送到板端 ==="

    require_dir "$ARTIFACT_DIR"

    # 未指定推送项时使用默认列表
    if [ ${#PUSH_ITEMS[@]} -eq 0 ]; then
        PUSH_ITEMS=("${DEFAULT_PUSH_ITEMS[@]}")
    fi

    # 解析推送项路径 (支持直接路径或按名称搜索)
    resolve_push_src() {
        local name="$1"
        if [ -e "$ARTIFACT_DIR/$name" ]; then
            echo "$ARTIFACT_DIR/$name"
        else
            find "$ARTIFACT_DIR" -name "$name" -not -path '*/.*' 2>/dev/null | head -1
        fi
    }

    if [ "$PUSH_METHOD" = "adb" ]; then
        require_cmd "$ADB_BIN"
        [ -n "$ADB_SERIAL" ] || ADB_SERIAL=$(adb devices | awk 'NR > 1 && $2 == "device" {print $1; exit}')
        [ -n "$ADB_SERIAL" ] || die "没有找到 adb 在线设备"

        echo "  方式: adb"
        echo "  设备: $ADB_SERIAL"
        echo "  目标: $BOARD_DIR"
        echo "  推送项: ${PUSH_ITEMS[*]}"

        "$ADB_BIN" -s "$ADB_SERIAL" shell "mkdir -p '$BOARD_DIR'"

        for item in "${PUSH_ITEMS[@]}"; do
            src=$(resolve_push_src "$item")
            if [ -z "$src" ] || [ ! -e "$src" ]; then
                warn "推送项不存在，跳过: $item"
                continue
            fi
            echo "  推送: $item"
            "$ADB_BIN" -s "$ADB_SERIAL" push "$src" "$BOARD_DIR/"
        done

        # 设置可执行权限
        "$ADB_BIN" -s "$ADB_SERIAL" shell "chmod +x '$BOARD_DIR'/zh_client '$BOARD_DIR'/zh_ble_gatt_server '$BOARD_DIR'/*.sh '$BOARD_DIR'/face/face_engine 2>/dev/null || true"

    elif [ "$PUSH_METHOD" = "ssh" ]; then
        require_cmd ssh
        require_cmd scp
        [ -n "$BOARD_HOST" ] || die "ssh 模式下需要 BOARD_HOST"

        echo "  方式: ssh"
        echo "  主机: $BOARD_HOST"
        echo "  目标: $BOARD_DIR"
        echo "  推送项: ${PUSH_ITEMS[*]}"

        ssh "$BOARD_HOST" "mkdir -p '$BOARD_DIR'"

        for item in "${PUSH_ITEMS[@]}"; do
            src=$(resolve_push_src "$item")
            if [ -z "$src" ] || [ ! -e "$src" ]; then
                warn "推送项不存在，跳过: $item"
                continue
            fi
            echo "  推送: $item"
            scp -r "$src" "$BOARD_HOST:$BOARD_DIR/"
        done

        ssh "$BOARD_HOST" "chmod +x '$BOARD_DIR'/zh_client '$BOARD_DIR'/zh_ble_gatt_server '$BOARD_DIR'/*.sh '$BOARD_DIR'/face/face_engine 2>/dev/null || true"
    else
        die "不支持的推送方式: $PUSH_METHOD"
    fi

    log "推送完成"

    # 推送后自动启动
    run_zh_client
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║     RV1106 完整编译脚本 (armv7 + uclibc)            ║"
    echo "║     产物: zh_client + bithion-core + face_engine     ║"
    echo "╚══════════════════════════════════════════════════════╝"
    echo ""
    echo "配置:"
    echo "  工具链:         $RV1106_TOOLCHAIN_ROOT"
    echo "  bithion-core:   $BITHION_SDK (skip-bithion 时用预编译: $BITHION_PREBUILT_SDK)"
    echo "  face_engine:    $FACE_ROOT"
    echo "  RK SDK:         $RK_SDK_ROOT"
    echo "  并行编译:       $JOBS jobs"
    echo "  产物目录:       $ARTIFACT_DIR"

    # --run-only: 跳过编译和推送，直接运行板端已有程序
    if [ "$RUN_ONLY" -eq 1 ]; then
        log "=== 仅运行模式 (跳过编译和推送) ==="
        run_zh_client
        exit 0
    fi

    # --push-only: 跳过编译，直接推送并启动
    if [ "$PUSH_ONLY" -eq 1 ]; then
        log "=== 仅推送模式 (跳过编译) ==="
        require_dir "$ARTIFACT_DIR"
        push_artifacts
        exit 0
    fi

    # Step 0
    env_check

    # Step 1: bithion-core
    if [ "$SKIP_BITHION" -eq 0 ]; then
        build_bithion_core
    else
        log "=== Step 1: 跳过 bithion-core 源码编译 (使用预编译 SDK: $BITHION_PREBUILT_SDK) ==="
        require_file "$BITHION_PREBUILT_SDK/lib/libbithion-core.so.1"
        BITHION_SDK="$BITHION_PREBUILT_SDK"
    fi

    # Step 2: face_engine
    if [ "$SKIP_FACE" -eq 0 ]; then
        build_face_engine
    else
        log "=== Step 2: 跳过 face_engine ==="
    fi

    # Step 3: zh_client 主包
    if [ "$SKIP_MAIN" -eq 0 ]; then
        build_main_client
    else
        log "=== Step 3: 跳过 zh_client 主包 ==="
    fi

    # Step 4: 合并产物
    restore_artifacts

    # Step 5: 推送
    if [ "$SKIP_PUSH" -eq 0 ]; then
        push_artifacts
    else
        log "=== Step 5: 跳过推送 ==="
    fi

    # 完成
    echo ""
    echo "╔══════════════════════════════════════════════════════╗"
    echo "║  编译完成!                                          ║"
    echo "║  产物目录: $ARTIFACT_DIR"
    if [ "$SKIP_PUSH" -eq 0 ]; then
        echo "║  板端目录: $BOARD_DIR"
    fi
    echo "╚══════════════════════════════════════════════════════╝"
}

main "$@"
