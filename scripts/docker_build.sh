#!/usr/bin/env bash
#
# RV1106 (armv7/uClibc) 语音对话客户端交叉编译脚本。
# 在分发方提供的 x86 构建容器内执行，用户零配置。
#
# 输入（闭源组件，预构建分发，见 README「快速开始」）：
#   core-sdk —— 语音核心 SDK：libbithion-core.so + libonnxruntime.so + 头文件
#   face-sdk —— 必选。人脸识别引擎 face_engine（RetinaFace/Facenet 模型内嵌于二进制），
#               编入视觉能力
#
# 输出：build/release.tar（含 install.sh / uninstall.sh，部署到 RV1106 板端）。
# 注意：打包为无压缩 tar（板端 busybox tar 不支持 gzip）。
#
# 用法（推荐在分发方提供的构建容器内执行）：
#   docker run --rm \
#     -v <仓库目录>:/repo \
#     -v <core-sdk.tar.gz>:/sdk/core-sdk.tar.gz \
#     -v <face-sdk.tar.gz>:/sdk/face-sdk.tar.gz \
#     zh_uclibc_linux_client_builder:YYYYMMDD \
#     bash -c "cd /repo && bash scripts/docker_build.sh"
#
# 参数：
#   --core-sdk PATH      core-sdk tarball 或已解包目录（默认：CORE_SDK 环境变量，
#                        否则依次查找 /sdk/core-sdk.tar.gz、./core-sdk 目录、
#                        build/sdk/core-sdk.tar.gz）
#   --core-sdk-url URL   从 URL 下载 core-sdk tarball（wget）
#   --face-sdk PATH      face-sdk tarball 或已解包目录（必选）
#   --face-sdk-url URL   从 URL 下载 face-sdk tarball
#   -j N                 并行编译任务数（默认 nproc）
#   -h, --help           显示帮助
#
# 环境变量：
#   RV1106_SDK_ROOT      Rockchip RV1106 SDK 精简子集（容器内默认 /rv1106-sdk，
#                        含 uClibc 交叉工具链 + rockit 媒体库 + BLE 静态库）
#   CORE_SDK, CORE_SDK_URL, FACE_SDK, FACE_SDK_URL
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CLIENT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# ============================================================
# 默认值
# ============================================================
RV1106_SDK_ROOT="${RV1106_SDK_ROOT:-/rv1106-sdk}"
TOOLCHAIN_ROOT="${RV1106_SDK_ROOT}/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
RV1106_GCC="${TOOLCHAIN_ROOT}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc-8.3.0"

SDK_DIR="$CLIENT_ROOT/build/sdk"
MAIN_BUILD="${MAIN_BUILD:-$CLIENT_ROOT/build/zh_client_rv1106_build}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$CLIENT_ROOT/build/artifacts}"
RELEASE="$CLIENT_ROOT/build/release.tar"

CORE_SDK="${CORE_SDK:-}"
CORE_SDK_URL="${CORE_SDK_URL:-}"
FACE_SDK="${FACE_SDK:-}"
FACE_SDK_URL="${FACE_SDK_URL:-}"
JOBS="${JOBS:-$(nproc)}"

usage() {
    sed -n '/^#/p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

# ============================================================
# 工具函数
# ============================================================
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { printf '\n[%s] %b%s%b\n' "$(date '+%H:%M:%S')" "${GREEN}" "$*" "${NC}"; }
warn() { printf '[%s] %bWARN:%b %s\n' "$(date '+%H:%M:%S')" "${YELLOW}" "${NC}" "$*"; }
die()  { printf '[%s] %bERROR:%b %s\n' "$(date '+%H:%M:%S')" "${RED}" "${NC}" "$*" >&2; exit 1; }
require_cmd() { command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"; }
require_file() { [ -f "$1" ] || die "缺少必需文件: $1"; }
require_dir()  { [ -d "$1" ] || die "缺少必需目录: $1"; }

check_arch_arm32() {
    local f="$1"
    if file -L "$f" | grep -q "ELF 32-bit.*ARM"; then
        echo "  [OK] armv7: $f"
    else
        die "架构不对（期望 ARM 32-bit）: $f"
    fi
}

# ============================================================
# 参数解析
# ============================================================
while [ "$#" -gt 0 ]; do
    case "$1" in
        --core-sdk)      shift; CORE_SDK="${1:?missing value for --core-sdk}" ;;
        --core-sdk-url)  shift; CORE_SDK_URL="${1:?missing value for --core-sdk-url}" ;;
        --face-sdk)      shift; FACE_SDK="${1:?missing value for --face-sdk}" ;;
        --face-sdk-url)  shift; FACE_SDK_URL="${1:?missing value for --face-sdk-url}" ;;
        -j)              shift; JOBS="${1:?missing value for -j}" ;;
        -h|--help)       usage ;;
        *) die "未知参数: $1（用 --help 查看帮助）" ;;
    esac
    shift
done

# ============================================================
# 获取 SDK（本地路径 / URL 下载 / 容器挂载点）
# ============================================================
fetch_sdk() {
    local name="$1"       # 名称（core-sdk / face-sdk）
    local arg="${2:-}"    # 命令行指定的路径
    local url="${3:-}"    # URL
    local out="$SDK_DIR/$name"

    rm -rf "$out"
    mkdir -p "$SDK_DIR"

    # 1) 命令行路径 / 环境变量
    if [ -n "$arg" ]; then
        if [ -d "$arg" ]; then
            cp -a "$arg" "$out"; return 0
        elif [ -f "$arg" ]; then
            mkdir -p "$out"
            tar -xzf "$arg" -C "$out"
            return 0
        fi
        die "找不到 $name: $arg"
    fi

    # 2) URL 下载
    if [ -n "$url" ]; then
        require_cmd wget
        log "下载 $name: $url"
        wget -q -O "$SDK_DIR/$name.tar.gz" "$url"
        mkdir -p "$out"
        tar -xzf "$SDK_DIR/$name.tar.gz" -C "$out"
        return 0
    fi

    # 3) 容器挂载点 / 仓库内默认位置
    if [ -f "/sdk/$name.tar.gz" ]; then
        mkdir -p "$out"
        tar -xzf "/sdk/$name.tar.gz" -C "$out"
        return 0
    fi
    if [ -d "$CLIENT_ROOT/$name" ]; then
        cp -a "$CLIENT_ROOT/$name" "$out"; return 0
    fi
    if [ -f "$SDK_DIR/$name.tar.gz" ]; then
        mkdir -p "$out"
        tar -xzf "$SDK_DIR/$name.tar.gz" -C "$out"
        return 0
    fi
    return 1
}

sdk_root() {
    # 找到 SDK 实际内容目录（兼容 tarball 内带一层顶层目录）
    local out="$SDK_DIR/$1" candidate
    for candidate in "$out" "$out"/*/; do
        case "$1" in
            core-sdk) [ -f "$candidate/include/bithion_core.h" ] && [ -d "$candidate/lib" ] && { echo "$candidate"; return; } ;;
            face-sdk) [ -f "$candidate/face_engine" ] && { echo "$candidate"; return; } ;;
        esac
    done
    echo ""
}

# ============================================================
# Step 1: 环境检查
# ============================================================
env_check() {
    log "=== Step 1: 环境检查 ==="
    require_cmd cmake
    require_cmd tar
    require_cmd file
    require_file "$RV1106_GCC"
    echo "  工具链: $("$RV1106_GCC" --version | head -1)"
    require_dir "$RV1106_SDK_ROOT/media/out/lib"
    echo "  Rockchip SDK: $RV1106_SDK_ROOT"
    log "环境检查通过"
}

# ============================================================
# Step 2: 获取闭源 SDK
# ============================================================
fetch_core_sdk() {
    log "=== Step 2: 获取 core-sdk ==="
    if ! fetch_sdk core-sdk "$CORE_SDK" "$CORE_SDK_URL"; then
        die "找不到 core-sdk。请通过 --core-sdk / --core-sdk-url 提供，或挂载到 /sdk/core-sdk.tar.gz（见 README「快速开始」）"
    fi
    CORE_SDK_DIR=$(sdk_root core-sdk)
    [ -n "$CORE_SDK_DIR" ] || die "core-sdk 内容不完整（需要 include/bithion_core.h + lib/）: $SDK_DIR/core-sdk"
    require_file "$CORE_SDK_DIR/lib/libbithion-core.so.1"
    require_file "$CORE_SDK_DIR/lib/libonnxruntime.so"
    echo "  core-sdk: $CORE_SDK_DIR"
    check_arch_arm32 "$CORE_SDK_DIR/lib/libbithion-core.so.1"
}

fetch_face_sdk() {
    if ! fetch_sdk face-sdk "$FACE_SDK" "$FACE_SDK_URL"; then
        die "找不到 face-sdk。人脸识别为必选能力：请通过 --face-sdk / --face-sdk-url 提供，或挂载到 /sdk/face-sdk.tar.gz（见 README「快速开始」第 2、3 步）"
    fi
    FACE_SDK_DIR=$(sdk_root face-sdk)
    [ -n "$FACE_SDK_DIR" ] || die "face-sdk 内容不完整（缺少 face_engine）: $SDK_DIR/face-sdk"
    require_file "$FACE_SDK_DIR/face_engine"
    check_arch_arm32 "$FACE_SDK_DIR/face_engine"
    echo "  face-sdk: $FACE_SDK_DIR"
}

# ============================================================
# Step 3: 构建主客户端
# ============================================================
build_main_client() {
    log "=== Step 3: 构建 zh_client (RV1106 / armv7 / uclibc) ==="
    echo "  Bithion SDK: $CORE_SDK_DIR"
    echo "  Face SDK:    $FACE_SDK_DIR"
    echo "  Build dir:   $MAIN_BUILD"

    # CMake 不支持在同一构建目录切换工具链/SDK，必须重建
    rm -rf "$MAIN_BUILD"

    cmake -S "$CLIENT_ROOT" \
        -B "$MAIN_BUILD" \
        -DZH_TARGET_PLATFORM=rv1106 \
        -DCMAKE_TOOLCHAIN_FILE="$CLIENT_ROOT/cmake/toolchain.cmake" \
        -DTOOLCHAIN_ROOT="$TOOLCHAIN_ROOT" \
        -DBITHION_CORE_SDK_ROOT="$CORE_SDK_DIR" \
        -DRK_SDK_ROOT="$RV1106_SDK_ROOT" \
        -DZH_ENABLE_FACE=ON \
        -DZH_ENABLE_VISION_CAPTURE=OFF \
        -DZH_BUILD_BLE_GATT_SERVER=ON \
        -DZH_BUILD_FACE_TEST=OFF

    cmake --build "$MAIN_BUILD" -j"$JOBS"

    echo ""
    log "验证主客户端产物:"
    require_file "$MAIN_BUILD/zh_client"
    check_arch_arm32 "$MAIN_BUILD/zh_client"
    if [ -f "$MAIN_BUILD/zh_ble_gatt_server" ]; then
        check_arch_arm32 "$MAIN_BUILD/zh_ble_gatt_server"
    fi
    log "主客户端构建完成"
}

# ============================================================
# Step 4: 合并产物
# ============================================================
merge_artifacts() {
    log "=== Step 4: 合并产物到 $ARTIFACT_DIR ==="

    rm -rf "$ARTIFACT_DIR"
    mkdir -p "$ARTIFACT_DIR/lib" \
             "$ARTIFACT_DIR/logs"

    # 主客户端
    cp -a "$MAIN_BUILD/zh_client" "$ARTIFACT_DIR/zh_client"
    chmod +x "$ARTIFACT_DIR/zh_client"
    if [ -f "$MAIN_BUILD/zh_ble_gatt_server" ]; then
        cp -a "$MAIN_BUILD/zh_ble_gatt_server" "$ARTIFACT_DIR/zh_ble_gatt_server"
        chmod +x "$ARTIFACT_DIR/zh_ble_gatt_server"
    fi

    # bithion-core 运行库。展平为实体文件（无符号链接，便于 adb push 直接传输）：
    #   libbithion-core.so.1         SONAME，zh_client 直接依赖
    #   libonnxruntime.so.1.17.3     bithion-core 依赖（$ORIGIN 同目录查找）
    # 其余系统库（rockit/mpp/drm/rga/aec/alsa/rknn 等）由板端系统提供。
    cp -aL "$CORE_SDK_DIR/lib/libbithion-core.so.1"   "$ARTIFACT_DIR/lib/libbithion-core.so.1"
    cp -aL "$CORE_SDK_DIR/lib/libonnxruntime.so.1.17.3" "$ARTIFACT_DIR/lib/libonnxruntime.so.1.17.3"

    # face_engine（必选）。模型已内嵌于 face_engine 二进制（RetinaFace/Facenet），
    # 无需外部模型文件与辅助库（face/save 为人脸库目录，运行时写入）。
    mkdir -p "$ARTIFACT_DIR/face/save"
    cp -a "$FACE_SDK_DIR/face_engine" "$ARTIFACT_DIR/face/face_engine"
    chmod +x "$ARTIFACT_DIR/face/face_engine"

    # 板端资源：证书 / 提示音 / AIVQE 配置
    for d in certs prompt_mp3 prompt_wav; do
        [ -d "$CLIENT_ROOT/scripts/installer/$d" ] && cp -a "$CLIENT_ROOT/scripts/installer/$d" "$ARTIFACT_DIR/$d"
    done
    [ -f "$CLIENT_ROOT/scripts/installer/config_aivqe.json" ] && \
        cp -a "$CLIENT_ROOT/scripts/installer/config_aivqe.json" "$ARTIFACT_DIR/config_aivqe.json"

    # 板端服务脚本
    for f in start_zh_client.sh kill.sh S22zh_client; do
        [ -f "$CLIENT_ROOT/scripts/self_start/$f" ] && {
            cp -a "$CLIENT_ROOT/scripts/self_start/$f" "$ARTIFACT_DIR/$f"
            chmod +x "$ARTIFACT_DIR/$f"
        }
    done

    # BLE 配网引导脚本
    [ -f "$CLIENT_ROOT/scripts/ble/start_ble_provision.sh" ] && {
        cp -a "$CLIENT_ROOT/scripts/ble/start_ble_provision.sh" "$ARTIFACT_DIR/start_ble_provision.sh"
        chmod +x "$ARTIFACT_DIR/start_ble_provision.sh"
    }

    # 安装/卸载脚本（随发布包分发）
    cp -a "$SCRIPT_DIR/install.sh"   "$ARTIFACT_DIR/install.sh"
    cp -a "$SCRIPT_DIR/uninstall.sh" "$ARTIFACT_DIR/uninstall.sh"
    chmod +x "$ARTIFACT_DIR/install.sh" "$ARTIFACT_DIR/uninstall.sh"
    # 演示鉴权 key（随发布包分发，install.sh 在板端无 key 时自动使用）
    cp -a "$SCRIPT_DIR/demo.key" "$ARTIFACT_DIR/demo.key"

    # 架构验证
    echo ""
    log "产物架构验证:"
    for bin in zh_client zh_ble_gatt_server; do
        [ -f "$ARTIFACT_DIR/$bin" ] && check_arch_arm32 "$ARTIFACT_DIR/$bin"
    done
    for lib in lib/libbithion-core.so.1 lib/libonnxruntime.so; do
        [ -f "$ARTIFACT_DIR/$lib" ] && check_arch_arm32 "$ARTIFACT_DIR/$lib"
    done
    check_arch_arm32 "$ARTIFACT_DIR/face/face_engine"

    echo ""
    log "产物清单:"
    echo "--------------------------------------------"
    (cd "$ARTIFACT_DIR" && du -sh . && ls -1)
    echo "--------------------------------------------"
}

# ============================================================
# Step 5: 打包发布
# ============================================================
package_release() {
    log "=== Step 5: 打包发布 $RELEASE ==="
    (cd "$ARTIFACT_DIR" && find . -mindepth 1 -printf '%P|%s|%m\n' | sort > manifest.txt)
    rm -f "$RELEASE"
    tar -cf "$RELEASE" -C "$ARTIFACT_DIR" .
    ls -lh "$RELEASE"
    log "发布包完成。部署方法（见 README「部署到板端」）："
    echo "  开发机: bash scripts/install.sh"
    echo "  板端:   拷贝 $RELEASE 到板端后: mkdir -p /data/zh_work && tar -xf release.tar -C /data/zh_work && rm -f release.tar && sh /data/zh_work/install.sh"
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo "╔══════════════════════════════════════════════════╗"
    echo "║  RV1106 语音对话客户端交叉编译                    ║"
    echo "║  产物: build/release.tar（含 install.sh）         ║"
    echo "╚══════════════════════════════════════════════════╝"
    echo ""
    echo "配置:"
    echo "  并行编译: $JOBS jobs"

    env_check
    fetch_core_sdk
    fetch_face_sdk
    build_main_client
    merge_artifacts
    package_release

    echo ""
    echo "╔══════════════════════════════════════════════════╗"
    echo "║  编译完成! 发布包: $RELEASE"
    echo "╚══════════════════════════════════════════════════╝"
}

main "$@"
