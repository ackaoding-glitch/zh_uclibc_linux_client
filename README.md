# ZH Voice Client · RV1106

> 面向 **RV1106 板卡**（armv7 / uClibc / busybox）的端侧语音对话客户端——
> 本地语音唤醒、全双工云端对话、TTS 播报，纯 C 实现，Docker 一键交叉编译，
> 在 **44MB 闪存 / 100MB 内存**的板卡上也能跑起来的完整语音交互方案。

[![License](https://img.shields.io/badge/license-Apache--2.0-brightgreen)](LICENSE)
[![Language](https://img.shields.io/badge/language-C-8892bf)](src/)
[![Build](https://img.shields.io/badge/build-Docker%20one--click-2496ed)](README.md#3-容器内构建)
[![Platform](https://img.shields.io/badge/platform-RV1106%20armv7-orange)](README.md#板端环境准备)
[![OS](https://img.shields.io/badge/OS-uClibc%20busybox-4e9a51)](README.md#服务管理)

---

## 目录

- [简介](#简介)
- [技术亮点](#技术亮点)
- [架构](#架构)
- [实测性能与架构优势](#实测性能与架构优势)
- [音频管线](#音频管线)
- [快速开始](#快速开始)
- [板端环境准备](#板端环境准备)
- [开发指南](#开发指南)
- [常见问题](#常见问题)
- [第三方组件](#第三方组件)
- [许可证](#许可证)

---

## 简介

ZH Voice Client 是一个**面向资源受限嵌入式设备的完整语音对话方案**：本地
VAD 唤醒检测 + 低延迟全双工云端对话 + TTS 播报，内置 Rockchip AI+VQE 音频
预处理（回声消除 / 降噪），摄像头人脸识别（RKNN NPU），支持 BLE 配网与
Wi-Fi 引导。无 systemd、无重框架依赖——用 busybox init 守护，一条命令部署
上板。

能力清单：

| 能力 | 说明 |
|---|---|
| 🎙️ 语音对话 | 本地 VAD 唤醒 + 全双工云端对话（语音算法与云端协议由 core-sdk 实现） |
| 🔊 AI+VQE 音频处理 | Rockchip Rockit（RK_MPI）AEC 回声消除 / 降噪，远场拾音 |
| 😀 人脸识别 | RetinaFace + Facenet，RKNN NPU 推理，本地人脸库（face-sdk，预构建分发） |
| 🎵 云端音频资源 | 对接百度云音频内容（音乐 / 音频直链），语音可随时打断播放 |
| 📶 联网能力 | BLE 配网（GATT provisioning）、Wi-Fi 引导、SNTP 开机自动校时 |
| 📦 一键部署 | 单 tar 包 + 幂等 install.sh，busybox init 开机自启，uninstall.sh 干净卸载 |

## 技术亮点

- **纯 C，零重框架**：无 systemd、无 C++、无运行时依赖链，静态链接核心组件，
  产物 + 运行库仅约 **20MB**；
- **容器化交叉编译**：官方 Docker 构建镜像内置完整工具链（arm-rockchip830
  uClibc 工具链 + Rockchip 媒体库 + BLE 静态库），**开发机零配置**，一条命令
  产出可部署安装包；
- **分层 SDK 架构**：语音算法与云端协议封装在预构建 core-sdk 中，本仓库提供
  全部客户端源码，职责清晰、可独立演进；
- **幂等部署**：`install.sh` 可重复执行，重装/升级不丢鉴权 key 与人脸库；
- **资源自适应**：针对 44MB 闪存 / 100MB 内存优化——部署走闪存换目录（不经
  tmpfs），`/tmp` 仅承载解包暂存。

## 架构

客户端分为三层，源码与构建方式各不相同：

```
┌──────────────────────────────────────────────────────────┐
│  zh_uclibc_linux_client（开源 · 本仓库 · 交叉编译）        │
│  ┌──────────────┐   ┌──────────────────────────────────┐  │
│  │ zh_client    │   │ zh_ble_gatt_server               │  │
│  │ 主客户端      │   │ BLE 配网服务（GATT provisioning）│  │
│  │ 采集/对话/状态机│   └────────────────────────────────┘  │
│  └──────────────┘                                         │
├───────────────────────────────────────────────────────────┤
│  core-sdk（预构建分发 · 官方发布渠道提供）                   │
│  ├── libbithion-core.so                                    │
│  └── libonnxruntime.so                                     │
├───────────────────────────────────────────────────────────┤
│  face-sdk（预构建分发 · 官方发布渠道提供）                  |
│  └── face_engine                                          │
└───────────────────────────────────────────────────────────┘
```

- **core-sdk / face-sdk**：预构建分发，由官方发布渠道提供（armv7 库 + 头文件；
  人脸模型内嵌于 `face_engine` 二进制），不随本仓库分发；
- **本仓库**：全部可运行源码（客户端 + BLE 配网 + 部署脚本）。

## 实测性能与架构优势

### 端侧资源占用

> 
> 以下数据为RV1106 完成启动、视觉模块加载和云端鉴权后读取的
> 进程 `VmRSS` 之和，用于给出工程选型量级，不代表峰值或独占物理内存（PSS）。

| 平台与场景 | 实测 RSS | 统计范围 |
|---|---:|---|
| RV1106 / armv7 / uClibc | **约 29.6 MiB** | supervisor + `zh_client` + `face_engine` |

### 交互时延与服务端容量

| 指标 | 实测结果 | 口径 |
|---|---:|---|
| 首个下行音频包 | **600–700 ms** | 从端侧 VAD 判定本轮语音结束，到客户端收到首个 TTS 音频包。全真实链路，包含 ASR、LLM/GPT、TTS 模型计算时间|
| 服务端并发 | **100 路并发，响应保持在 300 ms 内** | 2 vCPU / 2 GiB 服务端；统计接入、鉴权、会话调度与消息转发链路，不包含 ASR、LLM/GPT、TTS 模型计算时间 |

ASR、LLM/GPT 与 TTS 均可作为独立计算服务横向扩容；接入层与会话层保持轻量，
无需随模型实例做等比例扩容。实际时延仍会受到公网质量、模型规格、上下文长度和
音频内容影响，上表用于描述当前测试环境下的稳定观测值。

### 架构为什么适合资源受限设备

- **纯 C 数据面**：主客户端基于 C11 与 pthread 构建，没有大型语言运行时；在
  uClibC / busybox 环境中仍能保留完整的 VAD、双向流式音频与设备状态机；
- **硬件能力就地释放**：音频采集与 AEC 使用 Rockit / RK_MPI，人脸检测与特征提取
  交给 RKNN NPU，CPU 主要承担会话编排和轻量数据搬运；
- **并行但不过度抽象**：采集上行、TTS、音乐与视觉任务使用独立工作线程，
  `face_engine` 和 BLE 配网使用独立进程，慢设备或慢网络不会串行阻塞主状态机；
- **算法与产品逻辑分层**：VAD、云端协议和推理由 core-sdk 提供，设备接入、音频后端、
  BLE 配网与部署逻辑保留在开源客户端层，二者可以独立升级；
- **面向小内存的发布与守护**：发布包直接在闪存目录间切换，避免在 tmpfs 重复解包；
  busybox supervisor 负责异常自拉起，不依赖 systemd 或容器运行时。

## 音频管线

端侧音频处理全链路：

```
麦克风 ──▶ Rockit AIVQE（AEC 回声消除 / 降噪 / 增益）──▶ VAD 唤醒检测
                                                          │
                                                  语音分段 ──▶ 云端全双工对话（WS）
                                                          │
扬声器 ◀── TTS 播放（ALSA，可随时打断）◀── TTS 音频流 ◀──┘
```

- **AEC 前置**：播放信号经 Rockit 回采进参考通道，采集进麦，消除自身播报；
- **VAD 门控**：峰值门控过滤环境噪声，语音段才上行，省流量、降误唤醒；
- **打断优先**：播放中检测到唤醒词立即打断 TTS/音乐播放，交互低延迟。

## 快速开始

以下流程从源码构建并部署到板端，共 5 步，全程在 x86 开发机上完成。

| 项目 | 要求 |
|---|---|
| 开发机 | x86_64 Linux，已装 **Docker** 与 **adb**（见「板端环境准备」） |
| 目标板 | RV1106（armv7 / uClibc / busybox），已开启 adb |
| 构建镜像 | 官方发布渠道获取（见第 1 步） |
| core-sdk | 官方发布渠道获取（见第 2 步） |
| face-sdk | 官方发布渠道获取（见第 2 步） |


### 0. 开发板烧录镜像

**目标板RV1106需要先烧录系统镜像**，官方提供预构建镜像（`update.img`）及烧录所需的工具与驱动：

| 文件 | 说明 |
|---|---|
| `update.img` | 系统镜像（烧录目标） |
| `RKDevTool_Release_v2.96.zip` | 烧录工具 RKDevTool（Windows 运行） |
| `DriverAssitant_v5.0.zip` | Rockchip USB 驱动（Windows，烧录前安装） |

```bash
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/rv1106_burn_tools/update.img
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/rv1106_burn_tools/RKDevTool_Release_v2.96.zip
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/rv1106_burn_tools/DriverAssitant_v5.0.zip
```

烧录步骤（在 Windows 电脑上操作）：

1. **安装驱动**：解压 `DriverAssitant_v5.0.zip`，运行 `DriverInstall.exe`
   （首次烧录前安装一次即可）；
2. **启动工具**：解压 `RKDevTool_Release_v2.96.zip`，以管理员身份运行
   `RKDevTool.exe`；
3. **进入烧录模式**：板端断电，按住 **BOOT键**上电并通过
   USB 连接电脑，工具识别到设备（Maskrom）。
   或点击烧录工具「升级固件」页的「切换」按钮从adb模式切换到maskrom模式。
4. **烧录**：切到「升级固件」页，点击「固件」选择 `update.img`，点击「执行」
   开始烧录；
5. **完成重启**：烧录进度 100% 后板端自动重启，`adb devices` 能看到设备即就绪。

### 1. 获取构建镜像

镜像内置全部编译依赖（uClibc 交叉工具链 + Rockchip 媒体库/头文件 + BLE 静态库），
**无需配置任何环境**：

```bash
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/zh_uclibc_linux_client_builder.tar.gz
docker load -i zh_uclibc_linux_client_builder.tar.gz
```

### 2. 获取预构建组件（core-sdk / face-sdk）

```bash
cd <本仓库目录>
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/bithion-core-sdk-rv1106-uclibc-armv7.tar.gz
mkdir -p core-sdk && tar -xzf bithion-core-sdk-rv1106-uclibc-armv7.tar.gz -C core-sdk
wget https://bithion.obs.cn-east-3.myhuaweicloud.com/%E5%AD%97%E5%97%A8%E5%BC%80%E6%BA%90sdk%E5%8C%85/face-engine-rv1106-uclibc-armv7.tar.gz
```

仓库根目录出现 `core-sdk/`，应包含 `lib/`（libbithion-core.so + libonnxruntime.so）
与 `include/`（头文件）。注意必须解压到 `core-sdk/` 子目录（tar 内为平铺结构，
直接解压会与仓库自身的 `include/` 冲突）。

face-sdk 为单文件分发（人脸识别引擎 face_engine，模型内嵌），只需下载、无需解压
（构建时直接把 tarball 挂载进容器，见第 3 步）。人脸识别为**必选能力**，构建缺少
face-sdk 会直接报错。

### 3. 容器内构建

```bash
docker run --rm \
  -v "$PWD":/repo \
  -v <face-sdk.tar.gz路径>:/sdk/face-sdk.tar.gz \
  zh_uclibc_linux_client_builder:20260813 \
  bash -c "cd /repo && bash scripts/docker_build.sh"
```

- core-sdk 已在第 2 步解压到仓库 `core-sdk/`，构建脚本自动找到，无需挂载
  （也可不解压，改挂载 `-v <core-sdk.tar.gz路径>:/sdk/core-sdk.tar.gz`）；
- face-sdk 为必选能力：把第 2 步下载的 tarball 挂载到 `/sdk/face-sdk.tar.gz`
  （也可用 `--face-sdk` 参数直接指定路径）。

产物：`build/release.tar`（armv7 可执行程序 + 运行库 + 人脸识别引擎 + 证书/提示音
+ AIVQE 配置 + install.sh / uninstall.sh）。人脸模型已内嵌于 face_engine，无单独
模型文件。

### 4. 部署到板端

```bash
# 开发机 adb 一键部署（自动推送到板端并执行安装）：
bash scripts/install.sh
```

部署脚本把产物部署到 `/data/zh_work/`，安装开机自启服务（busybox init，
`/etc/init.d/S22zh_client`），并**保留板端已有的鉴权 key**（`/data/zh_work/key`，
服务器鉴权，由服务提供方签发）。板端无 key 时，`install.sh` 自动放置随发布包
分发的演示 key（`demo.key`），开箱即可对话。安装完成后设备默认在 3 秒后自动重启，
重启后由 busybox init 拉起客户端；部署调试时可追加 `--no-reboot` 跳过自动重启。


### 5. 验证

```bash
adb shell 'ps | grep zh_client'                                # 客户端进程
adb shell 'tail -f /data/zh_work/logs/start_zh_client.log'     # 守护/启动日志
adb shell 'tail -f /data/zh_work/logs/zh_client.stderr'        # 客户端日志
```

部署后首次启动应听到启动提示音（`prompt_wav/boot.wav`）。**鉴权 key 无需手动准备**：
首次部署时 `install.sh` 检测到板端无 key，自动放置随发布包分发的演示 key
（`/data/zh_work/key`）。需更换为正式 key（由服务提供方签发）时，覆盖
`/data/zh_work/key` 后重启服务即可（重装/升级不会覆盖已有 key）。


## 开发指南

日常开发与运维。

### 部署内容

解包到 `/data/zh_work/`：

```
/data/zh_work/
├── zh_client / zh_ble_gatt_server
├── lib/                  # 运行库（libbithion-core.so.1 + libonnxruntime.so.1.17.3）
│                         # 其余系统库（rockit/alsa/rknn 等）由板端系统提供
├── face/                 # 人脸识别引擎（模型内嵌）+ 人脸库 save/
├── certs/ prompt_mp3/    # CA 证书与提示音
├── config_aivqe.json     # Rockit AIVQE 配置（安装时同步到 /oem/usr/share/vqefiles）
└── key                   # 服务器鉴权 key（首次部署自动放置演示 key，重装/升级不覆盖）
```

注意：**不覆盖已有的 `/data/zh_work/key`**（服务器鉴权，由服务提供方签发；
首次部署无 key 时自动放置随包分发的 `demo.key`）。

### 服务管理

板端为 uClibc/busybox 系统（无 systemd），服务以 `/etc/init.d/S22zh_client` 管理
（服务名 `zh_client`，busybox init 开机自启）：

```bash
# 开发机远程操作
adb shell '/etc/init.d/S22zh_client restart'
adb shell 'tail -f /data/zh_work/logs/start_zh_client.log'
adb shell 'ps | grep zh_client'      # 进程存活检查

# 板端直接执行
/etc/init.d/S22zh_client {start|stop|restart}
```

| 部署文件 | 说明 |
|---|---|
| `/etc/init.d/S22zh_client` | 开机自启入口（sleep 3 后拉起 supervisor 守护） |
| `/data/zh_work/start_zh_client.sh` | 守护脚本：supervisor 监控 zh_client，异常退出自动拉起 |
| `/data/zh_work/logs/` | 客户端运行日志 |
| `/data/zh_work/kill.sh` | 一键停止全部相关进程 |

### 卸载

卸载脚本随构建产物分发：

```bash
# 开发机远程卸载
bash scripts/uninstall.sh --adb

# 板端直接执行
sh /data/zh_work/uninstall.sh
```

默认保留 `/data/zh_work/key`（重装可复用）；彻底清除（连 key 一起删）：
`sh /data/zh_work/uninstall.sh --purge`。

### 手工运行与调试

```bash
# 停止服务后前台手动运行（Ctrl-C 退出，日志直出）
adb shell 'sh /etc/init.d/S22zh_client stop'
adb shell 'cd /data/zh_work && export LD_LIBRARY_PATH=/data/zh_work/lib:/data/zh_work:/usr/lib:/lib:/oem/usr/lib && ./zh_client'
```

**日志速查**（`/data/zh_work/logs/`）：

| 日志片段 | 含义 |
|---|---|
| `ws connect/auth failed` | 连不上服务器（网络 / key 问题） |
| `[capture] level: ...` | 采集线程在跑（麦克风工作） |
| `[prompt] prompt tone ...` | 提示音触发（net_connect / net_disconnect 等） |
| `core event WS_CLOSED` | 与服务器的连接断开（自动重连） |
| `sntp sync ok` | 时间同步成功 |

## 常见问题

### 构建相关

| 问题 | 处理 |
|---|---|
| 构建报缺 core-sdk | 仓库根目录 `core-sdk/lib/libbithion-core.so.1` 不存在，先完成快速开始第 2 步 |
| `docker: command not found` | 开发机未装 Docker（docs.docker.com） |
| 镜像加载失败 | 确认 tar 包完整（可重新下载）；`docker images` 看到 `zh_uclibc_linux_client_builder` 即成功 |

### 运行相关

| 问题 | 处理 |
|---|---|
| 服务一直在重启 | `tail -f /data/zh_work/logs/start_zh_client.log` 看报错；确认 `/data/zh_work/lib/` 下库齐全（部署用 install.sh 全量解包） |
| 没有语音回应 | 确认 `/data/zh_work/key` 已放好；声卡设备空闲（`fuser /dev/snd/*`）；服务器可达 |
| 麦克风无电平 | `arecord -l` 确认设备；`[capture]` 日志无输出则设备被占用（rkipc 等） |
| 时间不对连不上服务器 | 等待客户端 SNTP 自动对时（`[sntp]` 日志），或手动 `date -s` 校准 |

## 第三方组件

| 组件 | 版本 | 许可 | 说明 |
|---|---|---|---|
| [Opus](https://opus-codec.org/) | 1.6.1 | BSD | 音频编解码（`third_party/opus_prebuilt/`，静态链接） |
| [minimp3](https://github.com/lieff/minimp3) | 单头文件 | CC0 | MP3 解码（`third_party/minimp3/`） |
| alsa-lib | 1.1.5 | LGPL-2.1 | 音频采集/播放（`third_party/alsa_prebuilt_rk/` 预编译库） |
| [onnxruntime](https://github.com/microsoft/onnxruntime) | 1.17.3 | MIT | 模型推理（随 core-sdk 分发） |
| MbedTLS | - | Apache-2.0 | TLS（bithion-core 内置，静态链接） |
| libfvad | - | BSD-3-Clause | 传统 VAD（bithion-core 内置，静态链接） |
| Rockchip rockit（RK_MPI） | - | 专有 | AI+VQE 音频处理（板端运行库 + SDK 库） |
| Rockchip RKNN（librknnmrt） | - | 专有 | 人脸识别 NPU 推理（板端系统库） |

各组件许可文本见 `third_party/licenses/`。

## 许可证

Apache-2.0。详见 [LICENSE](LICENSE)。

预构建组件（core-sdk / face-sdk）另行授权，不在本仓库许可证范围内。
