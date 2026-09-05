#!/usr/bin/env bash
#
# 构建 x86 交叉编译镜像（面向分发方，普通用户无需执行）。
# 产物：仓库根目录下 <镜像名>_<日期>.tar.gz，即 docker save 的完整镜像包。
#
# 用法:
#   ./docker/build_image.sh <rv1106-sdk.tar.gz 或目录> [tag]
#
# 参数:
#   rv1106-sdk    Rockchip RV1106 SDK 精简子集（gzip 压缩包或已解压目录），
#                 需包含 tools/linux/toolchain（uClibc 工具链）、media/out
#                 （rockit 媒体库与头文件）、sysdrv/packages/ble_test（BLE 静态库）
#   tag           镜像版本号（默认 YYYYMMDD）
#
# 环境变量:
#   IMAGE_NAME   镜像名（默认 zh_uclibc_linux_client_builder）
#   CONTEXT_DIR  构建上下文目录（默认 <仓库>/build/docker-context，自动清理重建）
#
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE_NAME="${IMAGE_NAME:-zh_uclibc_linux_client_builder}"
TAG="${2:-$(date +%Y%m%d)}"
IMAGE="$IMAGE_NAME:$TAG"

SDK_SRC="${1:-}"
[ -n "$SDK_SRC" ] || { echo "usage: $0 <rv1106-sdk.tar.gz|dir> [tag]" >&2; exit 1; }
[ -e "$SDK_SRC" ] || { echo "missing sdk: $SDK_SRC" >&2; exit 1; }

CONTEXT_DIR="${CONTEXT_DIR:-$ROOT_DIR/build/docker-context}"
rm -rf "$CONTEXT_DIR"
mkdir -p "$CONTEXT_DIR"

echo "组装构建上下文: $CONTEXT_DIR"
if [ -d "$SDK_SRC" ]; then
    # 目录输入：剥离顶层目录重新打包，保证容器内固定落在 /rv1106-sdk/ 下
    ( cd "$SDK_SRC" && tar -czf "$CONTEXT_DIR/rv1106-sdk.tar.gz" . )
else
    # tar.gz 输入：解压后同样剥离顶层目录
    TMP=$(mktemp -d)
    tar -xzf "$SDK_SRC" -C "$TMP"
    ( cd "$TMP" && tar -czf "$CONTEXT_DIR/rv1106-sdk.tar.gz" . )
    rm -rf "$TMP"
fi

# 校验必需内容
TMP=$(mktemp -d)
tar -xzf "$CONTEXT_DIR/rv1106-sdk.tar.gz" -C "$TMP" 2>/dev/null || {
    echo "SDK 压缩包解压失败: $CONTEXT_DIR/rv1106-sdk.tar.gz" >&2
    rm -rf "$TMP"
    exit 1
}
[ -d "$TMP/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin" ] || {
    echo "SDK 内容不完整：需要 tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf" >&2
    rm -rf "$TMP"
    exit 1
}
[ -d "$TMP/media/out/lib" ] || {
    echo "SDK 内容不完整：需要 media/out/lib（rockit 媒体库）" >&2
    rm -rf "$TMP"
    exit 1
}
[ -d "$TMP/sysdrv/packages/ble_test/libs" ] || {
    echo "SDK 内容不完整：需要 sysdrv/packages/ble_test/libs（BLE 静态库）" >&2
    rm -rf "$TMP"
    exit 1
}
[ -x "$TMP/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc-8.3.0" ] || {
    echo "未找到 RV1106 交叉编译器 gcc-8.3.0" >&2
    rm -rf "$TMP"
    exit 1
}
rm -rf "$TMP"
echo "SDK 校验通过（工具链 / media / ble_test）"

echo "构建镜像: $IMAGE"
docker build -f "$ROOT_DIR/docker/Dockerfile" -t "$IMAGE" "$CONTEXT_DIR"

OUT="$ROOT_DIR/${IMAGE_NAME}_${TAG}.tar.gz"
echo "导出镜像包: $OUT"
docker save "$IMAGE" | gzip -1 > "$OUT"
ls -lh "$OUT"
echo "完成。分发方式: docker load -i ${OUT##*/}"
