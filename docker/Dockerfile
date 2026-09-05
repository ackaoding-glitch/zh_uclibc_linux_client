# x86 构建镜像：在 x86 开发机上以容器方式交叉编译 RV1106 (armv7/uClibc) 语音客户端。
# 镜像内置全部编译环境（arm-rockchip830 uClibc 交叉工具链 + Rockchip 媒体库/
# 头文件 + BLE 静态库），用户零配置：
#
#   docker run --rm -v <仓库目录>:/repo \
#     -v <core-sdk.tar.gz>:/sdk/core-sdk.tar.gz \
#     -v <face-sdk.tar.gz>:/sdk/face-sdk.tar.gz \
#     zh_uclibc_linux_client_builder:YYYYMMDD \
#     bash -c "cd /repo && bash scripts/docker_build.sh"
#   产物：<仓库目录>/build/release.tar.gz（部署到 RV1106 板端）
#
# 构建镜像由分发方通过 docker/build_image.sh 产出（docker save tar.gz 分发），
# 用户无需自行构建。
#
# 环境说明：
#   - Ubuntu 20.04 + cmake/ninja，与 RV1106 官方 SDK 开发环境一致
#   - /rv1106-sdk：Rockchip RV1106 SDK 精简子集（交叉工具链 + rockit 媒体库/
#     头文件 + BLE 配网静态库），镜像构建时 ADD 解压
FROM ubuntu:20.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        git \
        pkg-config \
        file \
        rsync \
        wget \
        ca-certificates \
        tar \
        gzip \
        xz-utils \
        unzip \
        openssh-client \
    && rm -rf /var/lib/apt/lists/*

# Rockchip RV1106 SDK 精简子集（工具链 + rockit 媒体库 + BLE 静态库）
ADD rv1106-sdk.tar.gz /rv1106-sdk

WORKDIR /repo
