# 瑞芯微实时语音/视觉交互系统

`rockchip_1106_voice_agent_system` 是上海字嗨科技的语音对话客户端开源仓库。产物是zh_client和zh_ble_gatt_server。

这是一个结合了实时语音，实时AI视觉，人脸识别的交互系统（real-time voice AI vision and face recognition: RTCV SYSTEM)。

本仓库公开的是客户端主程序，完整构建依赖配套提供的 `bithion-core` SDK 包以及rk-sdk环境。

如果想完整部署客户端客户端程序，需要配套的板子和rv1106_zh_clinet_installer仓库。


## 快速开始

我们提供了交付仓库[rv1106_zh_clinet_installer](https://github.com/ackaoding-glitch/rockchip_1106_ai_client_installer),可以快速烧写镜像，安装客户端。

如果要从头构建zh_client，可以按以下步骤操作：

```
## 构建环境 ubuntu:2204 x86_64
# 下载并解压配套提供的rk-sdk
mkdir sdk && cd sdk
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/rv110x_ipc_min_sdk_final_20260323.tar.gz
tar -zxvf rv110x_ipc_min_sdk_final_20260323.tar.gz
# 下载并解压配套提供的bithion-core-sdk
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/bithion-core-sdk-20260323.tar.gz
tar -zxvf bithion-core-sdk-20260323.tar.gz

# 返回项目根目录，编译
cd ..
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake \
  -DTOOLCHAIN_ROOT=sdk/rv110x_ipc_min_sdk_final_20260323/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf \
  -DRK_SDK_ROOT=sdk/rv110x_ipc_min_sdk_final_20260323 \
  -DBITHION_CORE_SDK_ROOT=sdk/bithion-core-sdk
cmake --build build -j"$(nproc)"
```

## 仓库范围

本仓库包含：

- 设备侧主程序 `zh_client`
- BLE 配网服务 `zh_ble_gatt_server`
- 人脸相关测试程序 `zh_face_test`
- 宿主侧业务逻辑、设备接入逻辑与部署脚本
- 构建本仓库时会配套提供 `bithion-core` SDK
- 构建本仓库时还会配套提供一个从原始板级 SDK 中提取出的最小编译依赖包`rk-sdk\rv110x_ipc_min_sdk_final_20260323.tar.gz`

## 功能概览

客户端宿主程序主要负责：

- 设备启动与联网流程
- BLE 配网
- 本地音频采集、播放与提示音控制
- 音乐、人脸、视觉等本地业务能力联动
- 对底层通信消息进行业务解释并驱动设备动作

底层设备绑定、会话管理以及 HTTP / WebSocket / UDP 基础通信能力由配套提供的 `bithion-core` SDK 提供。

## 目录说明

- `src/`：客户端宿主程序源码
- `include/`：宿主侧头文件与桥接头
- `docs/`：公开文档
- `scripts/`：辅助脚本
- `installer/`：安装与部署相关资源
- `third_party/`：随仓分发的第三方依赖与许可证文件

## 开源边界

读者可以通过本仓库了解：

- 宿主程序的整体结构
- 客户端如何接入私有通信 SDK
- 板端部署所需的脚本与资源组织方式

但本仓库仍不是一个可脱离配套 SDK 与目标设备环境、立即独立运行的完整设备交付物。

## 许可证

除另有说明的第三方组件外，本仓库中的源码与文档按 Apache License 2.0 分发，详见仓库根目录的 `LICENSE`。

第三方组件许可证见 `third_party/licenses/`。
