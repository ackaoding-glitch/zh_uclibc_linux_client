#ifndef ZH_CONFIG_H
#define ZH_CONFIG_H

#include "bithion_core_config.h"

// 客户端宿主配置：板级资源、本地路径与体验参数。
// 设备身份、默认路由与共享音频契约由 bithion-core SDK 公共头提供。

// 客户端配置（后续将迁移为配置文件）
#define ZH_WORK_BASE "/data/zh_work/"
#define ZH_KEY_PATH ZH_WORK_BASE"key" // 服务器鉴权 key，由服务方发放
#define ZH_LOG_PATH ZH_WORK_BASE"logs/" // 日志目录路径

// 人脸识别相关
#define ZH_FACE_ENGINE_PATH ZH_WORK_BASE "face/face_engine" // 人脸识别引擎路径
#define ZH_RETINA_FACE_MODEL_PATH ZH_WORK_BASE "face/model/RetinaFace.rknn" // retinaface模型路径
#define ZH_FACENET_MODEL_PATH ZH_WORK_BASE "face/model/w600k_mbf_conv_fixed.rknn" // facenet模型路径
#define ZH_FACE_EMB_DIR ZH_WORK_BASE "face/save/" // 本地人脸emb文件夹路径


#ifndef ZH_CARD_NAME
#define ZH_CARD_NAME "wlan0" // 客户端无线网卡名称
#endif

// websocket 重连
#define ZH_WS_RECONNECT_INTERVAL_MS 3000 // 重连超时
#define ZH_WS_RECONNECT_MAX 0 // 0 表示无限重连

// 音频后端选择：1=Rockit(RK_MPI), 0=ALSA
#ifndef ZH_AUDIO_BACKEND_ROCKIT
#define ZH_AUDIO_BACKEND_ROCKIT 1
#endif

// 录音/VAD 采集参数
#ifndef ZH_AUDIO_DEVICE
#define ZH_AUDIO_DEVICE "hw:0,0" // 采集设备
#endif
#ifndef ZH_AUDIO_CHANNELS
#define ZH_AUDIO_CHANNELS 2 // ALSA 需要双通道；Rockit 录音为单通道并在软件中上混到双通道
#endif
#ifndef ZH_AUDIO_MIC_CHANNEL_INDEX
#define ZH_AUDIO_MIC_CHANNEL_INDEX 0 // ALSA interleaved PCM 中用于 VAD/Opus 上行的麦克风通道，0-based
#endif
#ifndef ZH_AUDIO_REF_CHANNEL_INDEX
#define ZH_AUDIO_REF_CHANNEL_INDEX 1 // AEC 参考通道索引，0-based；仅 rkaudio backend 使用
#endif
#ifndef ZH_AUDIO_REF_CHANNELS
#define ZH_AUDIO_REF_CHANNELS 0 // AEC 参考通道数量；未知时为 0 并降级为 none
#endif
#ifndef ZH_AUDIO_AEC_FRAME_SAMPLES
#define ZH_AUDIO_AEC_FRAME_SAMPLES 256 // RK rkaudio 常用处理帧长
#endif
#ifndef ZH_ALSA_BUFFER_PERIODS
#define ZH_ALSA_BUFFER_PERIODS 4 // ALSA buffer 包含的 period 数，板端可按稳定性调整
#endif
#ifndef ZH_AUDIO_GAIN
#define ZH_AUDIO_GAIN 1.0f // 录音增益倍率，>1放大，<1衰减
#endif
#ifndef ZH_ENABLE_AEC
#define ZH_ENABLE_AEC 0
#endif
// 是否启用客户端高级 VAD 协作能力：开启时上报 CLIENT_ADVANCED_VAD 并由客户端发送 END；关闭时仍使用本地 VAD 起音和上行，真实判停交给服务端 VAD。
#ifndef ZH_CLIENT_ADVANCED_VAD_ENABLE
#define ZH_CLIENT_ADVANCED_VAD_ENABLE 1
#endif
#ifndef ZH_ENABLE_BAIDU_AUDIO_CAPABILITY
#define ZH_ENABLE_BAIDU_AUDIO_CAPABILITY 0
#endif
// VAD 前置音量门限：仅影响 VAD 输入，不改变上传音频主链路
#ifndef ZH_VAD_GATE_ENABLE
#define ZH_VAD_GATE_ENABLE 1
#endif
#ifndef ZH_VAD_GATE_OPEN_PEAK
#define ZH_VAD_GATE_OPEN_PEAK 8000 // 峰值达到该阈值后放行到 VAD
#endif
#ifndef ZH_VAD_GATE_CLOSE_PEAK
#define ZH_VAD_GATE_CLOSE_PEAK 800 // 峰值连续低于该阈值后关闭门限
#endif
#ifndef ZH_VAD_GATE_HOLD_FRAMES
#define ZH_VAD_GATE_HOLD_FRAMES 1 // 关闭前保持帧数，避免抖动
#endif

// Rockit/RK_MPI AI+VQE(AEC) 配置
#define ZH_RK_AIVQE_CONFIG_PATH "/oem/usr/share/vqefiles/config_aivqe.json"
#define ZH_RK_AI_CARD_NAME "hw:0,0" // Rockit SDK 示例使用 hw:0,0
#define ZH_RK_AI_DEV 0
#define ZH_RK_AI_CHN 0
#define ZH_RK_AO_DEV 0
#define ZH_RK_AO_CHN 0
#define ZH_RK_AO_CARD_NAME "hw:0,0"
#define ZH_RK_ENABLE_LOOPBACK 1
#define ZH_RK_LOOPBACK_MODE "Mode2"
// VQE 声道布局：左声道为录音(rec=0x1)，右声道为参考(ref=0x2)
#define ZH_RK_REC_LAYOUT 0x1
#define ZH_RK_REF_LAYOUT 0x2
#define ZH_RK_CH_LAYOUT (ZH_RK_REC_LAYOUT | ZH_RK_REF_LAYOUT)

// TTS 播放参数
#define ZH_TTS_SAMPLE_RATE 16000 // 播放采样率
#define ZH_TTS_CHANNELS 1 // 播放通道数
#define ZH_TTS_MAX_SAMPLES 5760 // 单帧解码最大采样点
#ifndef ZH_TTS_PLAY_DEVICE
#define ZH_TTS_PLAY_DEVICE "plughw:0,0" // ALSA 播放设备
#endif
#define ZH_TTS_IDLE_TIMEOUT_MS 2000 // 长时间无可播放的包后,清理播放状态，避免一直卡在播放中
#define ZH_TTS_GAIN 0.5f // 播放增益倍率
// 音乐播放增益倍率（普通 URL、本地音乐及百度音乐均适用）
#ifndef ZH_MUSIC_GAIN
#define ZH_MUSIC_GAIN 0.5f
#endif

// 系统提示音（支持本地文件路径或 HTTP/HTTPS URL；留空表示禁用）
#define ZH_PROMPT_MP3_BASE ZH_WORK_BASE "prompt_mp3/"
#define ZH_PROMPT_BOOT_MP3_URL ZH_PROMPT_MP3_BASE "boot.mp3"
// #define ZH_PROMPT_BOOT_MP3_URL ZH_PROMPT_MP3_BASE "ciallo.mp3"
#define ZH_PROMPT_PROVISION_MP3_URL ZH_PROMPT_MP3_BASE "provision.mp3"
#define ZH_PROMPT_NET_CONNECTED_MP3_URL ZH_PROMPT_MP3_BASE "net_connect.mp3"
#define ZH_PROMPT_NET_DISCONNECTED_MP3_URL ZH_PROMPT_MP3_BASE "net_disconnect.mp3"
#define ZH_PROMPT_CHAT_MODE_MP3_URL ZH_PROMPT_MP3_BASE "chat_mode.mp3"
#define ZH_PROMPT_WEB_SEARCH_WAIT_MP3_URL ZH_PROMPT_MP3_BASE "web_search_wait.mp3"
// 系统提示音增益（0~1更小声，1为原始音量）
#define ZH_PROMPT_TONE_GAIN 1.0f
#define ZH_PROMPT_WEB_SEARCH_WAIT_GAIN 0.85f

// 调试相关
// 是否存储 VAD 录音数据到文件
// 该文件保存的是“Opus编码前”的单声道PCM（与服务器收到前一致）
// 保存后，播放：ffplay -f s16le -ar 16000 -ch_layout mono record_pcm_dump.pcm
// 转换wav：ffmpeg -f s16le -ar 16000 -ch_layout mono -i record_pcm_dump.pcm ./test.wav

#ifndef ZH_VAD_RECORD_DUMP_ENABLE
#define ZH_VAD_RECORD_DUMP_ENABLE 0
#endif
#ifndef ZH_VAD_RECORD_DUMP_PATH
#define ZH_VAD_RECORD_DUMP_PATH "/tmp/record_pcm_dump.pcm"
#endif
#ifndef ZH_FORCE_LOCAL_ROUTE_CONFIG
#define ZH_FORCE_LOCAL_ROUTE_CONFIG 0
#endif
#ifndef ZH_ENABLE_VISION_CAPTURE
#define ZH_ENABLE_VISION_CAPTURE 0
#endif
#ifndef ZH_ENABLE_UI_TEXT_CAPABILITY
#define ZH_ENABLE_UI_TEXT_CAPABILITY 0
#endif
#ifndef ZH_ENABLE_LIVE2D_ACTION_CAPABILITY
#define ZH_ENABLE_LIVE2D_ACTION_CAPABILITY 0
#endif

// SNTP 时间同步：空格分隔的服务器域名/IP 列表，逐个尝试
#ifndef ZH_NTP_SERVERS
#define ZH_NTP_SERVERS "ntp.aliyun.com ntp1.aliyun.com cn.pool.ntp.org" // NTP 服务器
#endif
#ifndef ZH_NTP_TIMEOUT_MS
#define ZH_NTP_TIMEOUT_MS 2000 // 单服务器接收超时（毫秒）
#endif

void zh_get_config(zh_config_t *cfg);

#endif
