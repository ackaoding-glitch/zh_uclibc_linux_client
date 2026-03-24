#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>

#include "audio_capture.h"
#include "config.h"
#include "log.h"

#if ZH_AUDIO_BACKEND_ROCKIT

#include "rk_mpi_sys.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_amix.h"
#include "rk_mpi_mb.h"
#include "rk_comm_aio.h"

struct zh_audio_capture {
    AUDIO_DEV dev;
    AI_CHN chn;
    unsigned int dev_channels;
    unsigned int out_channels;
    int vqe_enabled;
    int first_frame_logged;
    int started;
    int16_t *stash;
    size_t stash_cap;
    size_t stash_len;
};

static void zh_rk_apply_ai_defaults(AUDIO_DEV dev) {
    AUDIO_FADE_S fade;
    memset(&fade, 0, sizeof(fade));
    if (RK_MPI_AI_SetMute(dev, RK_FALSE, &fade) != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetMute failed: dev=%d", dev);
    }
    if (RK_MPI_AI_SetVolume(dev, 100) != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetVolume failed: dev=%d volume=100", dev);
    } else {
        LOGI(__func__, "AI capture defaults applied: dev=%d mute=0 volume=100", dev);
    }
}

static void zh_rk_apply_ai_vqe_modules(AUDIO_DEV dev, AI_CHN chn) {
    AI_VQE_MOD_ENABLE_S mods;

    memset(&mods, 0, sizeof(mods));
    mods.bAec = RK_TRUE;
    mods.bBf = RK_TRUE;
    mods.bFastAec = RK_TRUE;
    mods.bAes = RK_TRUE;
    mods.bWakeup = RK_FALSE;
    mods.bGsc = RK_FALSE;
    mods.bAgc = RK_TRUE;
    mods.bAnr = RK_TRUE;
    mods.bNlp = RK_FALSE;
    mods.bDereverb = RK_TRUE;
    mods.bCng = RK_FALSE;
    mods.bDtd = RK_TRUE;
    mods.bEq = RK_FALSE;
    mods.bHowling = RK_TRUE;
    mods.bDoa = RK_FALSE;

    RK_S32 ret = RK_MPI_AI_SetVqeModuleEnable(dev, chn, &mods);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetVqeModuleEnable failed: dev=%d chn=%d ret=%d", dev, chn, ret);
    } else {
        LOGI(__func__, "AI VQE modules applied: aec=1 fast_aec=1 aes=1 agc=1 anr=1 dereverb=1 dtd=1 howl=1");
    }
}

static AUDIO_SAMPLE_RATE_E zh_rk_map_rate(unsigned int rate) {
    switch (rate) {
        case 8000: return AUDIO_SAMPLE_RATE_8000;
        case 16000: return AUDIO_SAMPLE_RATE_16000;
        case 32000: return AUDIO_SAMPLE_RATE_32000;
        case 44100: return AUDIO_SAMPLE_RATE_44100;
        case 48000: return AUDIO_SAMPLE_RATE_48000;
        default: return AUDIO_SAMPLE_RATE_16000;
    }
}

static void zh_rk_set_ld_library_path(void) {
    const char *path = getenv("LD_LIBRARY_PATH");
    if (path && strstr(path, "/oem/usr/lib")) {
        return;
    }
    if (!path || path[0] == '\0') {
        setenv("LD_LIBRARY_PATH", "/oem/usr/lib", 1);
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s:%s", path, "/oem/usr/lib");
        setenv("LD_LIBRARY_PATH", buf, 1);
    }
}

static void zh_rk_check_vqe_libs(void) {
    const char *libs[] = {
        "librkaudio_common.so",
        "libaec_bf_process.so",
    };
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); ++i) {
        dlerror();
        void *h = dlopen(libs[i], RTLD_LAZY);
        if (!h) {
            const char *err = dlerror();
            LOGE(__func__, "dlopen %s failed: %s", libs[i], err ? err : "unknown");
            continue;
        }
        LOGI(__func__, "dlopen %s ok", libs[i]);
        dlclose(h);
    }
}

static int zh_rk_stash_reserve(zh_audio_capture_t *cap, size_t more_samples) {
    if (!cap) {
        return -1;
    }
    size_t need = cap->stash_len + more_samples;
    if (need <= cap->stash_cap) {
        return 0;
    }
    size_t new_cap = cap->stash_cap ? cap->stash_cap : (ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS * 4);
    while (new_cap < need) {
        new_cap *= 2;
    }
    int16_t *p = (int16_t *)realloc(cap->stash, new_cap * sizeof(int16_t));
    if (!p) {
        return -1;
    }
    cap->stash = p;
    cap->stash_cap = new_cap;
    return 0;
}

static void zh_rk_enable_loopback(AUDIO_DEV dev) {
#if ZH_RK_ENABLE_LOOPBACK
    char mode[] = ZH_RK_LOOPBACK_MODE;
    RK_S32 ret = RK_MPI_AMIX_SetControl(dev, "I2STDM Digital Loopback Mode", mode);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "AMIX set loopback failed: dev=%d mode=%s ret=%d", dev, mode, ret);
    } else {
        LOGI(__func__, "AMIX loopback enabled: dev=%d mode=%s", dev, mode);
    }
#else
    (void)dev;
#endif
}

int zh_audio_capture_init(zh_audio_capture_t **cap,
                          unsigned int *actual_rate,
                          unsigned int *actual_channels) {
    if (!cap) {
        errno = EINVAL;
        return -1;
    }

    zh_audio_capture_t *ctx = (zh_audio_capture_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return -1;
    }
    ctx->dev = ZH_RK_AI_DEV;
    ctx->chn = ZH_RK_AI_CHN;
    ctx->dev_channels = 2;
    ctx->out_channels = 1;

    zh_rk_set_ld_library_path();
    zh_rk_check_vqe_libs();

    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_SYS_Init failed");
        free(ctx);
        return -1;
    }

    zh_rk_enable_loopback(ctx->dev);

    const unsigned int ai_frame_samples = 1024;
    const unsigned int ai_frame_ms = (ai_frame_samples * 1000) / ZH_AUDIO_SAMPLE_RATE;
    const unsigned int vqe_frame_samples = (ZH_AUDIO_SAMPLE_RATE * 16) / 1000;

    AIO_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    attr.soundCard.channels = ctx->dev_channels;
    attr.soundCard.sampleRate = ZH_AUDIO_SAMPLE_RATE;
    attr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    attr.enSamplerate = zh_rk_map_rate(ZH_AUDIO_SAMPLE_RATE);
    attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    attr.enSoundmode = (ctx->out_channels == 1) ? AUDIO_SOUND_MODE_MONO : AUDIO_SOUND_MODE_STEREO;
    attr.u32FrmNum = 4;
    attr.u32PtNumPerFrm = ai_frame_samples;
    attr.u32ChnCnt = ctx->dev_channels;
    // Live capture: do NOT enable data_read (file_read). Keep EX flag disabled.
    attr.u32EXFlag = 0;
    if (ZH_RK_AI_CARD_NAME[0] != '\0') {
        strncpy((char *)attr.u8CardName, ZH_RK_AI_CARD_NAME, sizeof(attr.u8CardName) - 1);
    }

    RK_S32 ret = RK_MPI_AI_SetPubAttr(ctx->dev, &attr);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetPubAttr failed: %d", ret);
        RK_MPI_SYS_Exit();
        free(ctx);
        return -1;
    }

    ret = RK_MPI_AI_Enable(ctx->dev);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_Enable failed: %d", ret);
        RK_MPI_SYS_Exit();
        free(ctx);
        return -1;
    }

    LOGI(__func__, "rockit ai attr: card=%s dev=%d chn=%d rate=%u dev_ch=%u out_ch=%u bits=16 frame=%u (%ums)",
         ZH_RK_AI_CARD_NAME, ctx->dev, ctx->chn, ZH_AUDIO_SAMPLE_RATE, ctx->dev_channels,
         ctx->out_channels, attr.u32PtNumPerFrm, ai_frame_ms);

    AI_VQE_CONFIG_S vqe;
    memset(&vqe, 0, sizeof(vqe));
    vqe.enCfgMode = AIO_VQE_CONFIG_LOAD_FILE;
    vqe.s32WorkSampleRate = ZH_AUDIO_SAMPLE_RATE;
    vqe.s32FrameSample = (RK_S32)vqe_frame_samples;
    vqe.s64RefChannelType = ZH_RK_REF_LAYOUT;
    vqe.s64RecChannelType = ZH_RK_REC_LAYOUT;
    vqe.s64ChannelLayoutType = ZH_RK_CH_LAYOUT;
    strncpy(vqe.aCfgFile, ZH_RK_AIVQE_CONFIG_PATH, sizeof(vqe.aCfgFile) - 1);

    zh_rk_apply_ai_vqe_modules(ctx->dev, ctx->chn);

    RK_S32 vqe_ret = RK_MPI_AI_SetVqeAttr(ctx->dev, ctx->chn, ZH_RK_AO_DEV, ZH_RK_AO_CHN, &vqe);
    if (vqe_ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetVqeAttr failed: %d", vqe_ret);
        RK_MPI_AI_DisableChn(ctx->dev, ctx->chn);
        RK_MPI_AI_Disable(ctx->dev);
        RK_MPI_SYS_Exit();
        free(ctx);
        return -1;
    }
    RK_S32 vqe_enable_ret = RK_MPI_AI_EnableVqe(ctx->dev, ctx->chn);
    if (vqe_enable_ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_EnableVqe failed: %d", vqe_enable_ret);
        RK_MPI_AI_DisableChn(ctx->dev, ctx->chn);
        RK_MPI_AI_Disable(ctx->dev);
        RK_MPI_SYS_Exit();
        free(ctx);
        return -1;
    }
    ctx->vqe_enabled = 1;

    AI_CHN_PARAM_S chn_param;
    memset(&chn_param, 0, sizeof(chn_param));
    // Align with rk_mpi_ai_test default to avoid buffer starvation.
    chn_param.s32UsrFrmDepth = 4;
    chn_param.enLoopbackMode = AUDIO_LOOPBACK_NONE;
    ret = RK_MPI_AI_SetChnParam(ctx->dev, ctx->chn, &chn_param);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetChnParam failed: %d", ret);
    }

    // Do not call RK_MPI_AI_EnableDataRead for live capture.

    ret = RK_MPI_AI_EnableChn(ctx->dev, ctx->chn);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_EnableChn failed: %d", ret);
        RK_MPI_AI_Disable(ctx->dev);
        RK_MPI_SYS_Exit();
        free(ctx);
        return -1;
    } else {
        LOGI(__func__, "RK_MPI_AI_EnableChn ok");
    }

    AI_CHN_ATTR_S chn_attr;
    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.enChnAttr = AUDIO_CHN_ATTR_RATE;
    chn_attr.u32SampleRate = ZH_AUDIO_SAMPLE_RATE;
    ret = RK_MPI_AI_SetChnAttr(ctx->dev, ctx->chn, &chn_attr);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SetChnAttr failed: %d", ret);
    } else {
        LOGI(__func__, "RK_MPI_AI_SetChnAttr ok");
    }

    if (ctx->out_channels == 1) {
        RK_S32 tm_ret = RK_MPI_AI_SetTrackMode(ctx->dev, AUDIO_TRACK_FRONT_LEFT);
        if (tm_ret != RK_SUCCESS) {
            LOGE(__func__, "RK_MPI_AI_SetTrackMode failed: %d", tm_ret);
        } else {
            LOGI(__func__, "RK_MPI_AI_SetTrackMode ok: AUDIO_TRACK_FRONT_LEFT");
        }
    } else {
        LOGI(__func__, "RK_MPI_AI_SetTrackMode: skip (keep normal)");
    }
    zh_rk_apply_ai_defaults(ctx->dev);

    if (actual_rate) {
        *actual_rate = ZH_AUDIO_SAMPLE_RATE;
    }
    if (actual_channels) {
        *actual_channels = ZH_AUDIO_CHANNELS;
    }

    *cap = ctx;
    return 0;
}

int zh_audio_capture_start(zh_audio_capture_t *cap) {
    if (!cap) {
        errno = EINVAL;
        return -1;
    }
    cap->started = 1;
    return 0;
}

int zh_audio_capture_read(zh_audio_capture_t *cap, int16_t *buffer, size_t samples) {
    if (!cap || !buffer || samples == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t need = samples;
    size_t loops = 0;
    while (cap->stash_len < need) {
        AUDIO_FRAME_S frame;
        AEC_FRAME_S aec;
        memset(&frame, 0, sizeof(frame));
        memset(&aec, 0, sizeof(aec));

        RK_S32 ret = RK_MPI_AI_GetFrame(cap->dev, cap->chn, &frame, &aec, -1);
        if (ret != RK_SUCCESS) {
            static int err_log_cnt = 0;
            if (++err_log_cnt % 50 == 0) {
                LOGE(__func__, "RK_MPI_AI_GetFrame failed: %d", ret);
            }
            if (ret == RK_ERR_AI_BUF_EMPTY || ret == RK_ERR_AI_NOBUF ||
                ret == RK_ERR_AI_SYS_NOTREADY || ret == RK_ERR_AI_BUSY) {
                usleep(1000);
                continue;
            }
            return -1;
        }

        int16_t *src = (int16_t *)RK_MPI_MB_Handle2VirAddr(frame.pMbBlk);
        if (!src) {
            LOGE(__func__, "RK_MPI_MB_Handle2VirAddr failed: mb=%p len=%u seq=%u",
                 frame.pMbBlk, frame.u32Len, frame.u32Seq);
            RK_MPI_AI_ReleaseFrame(cap->dev, cap->chn, &frame, &aec);
            return -1;
        }

        RK_U32 mb_bytes = RK_MPI_MB_GetSize(frame.pMbBlk);
        size_t in_samples_per_ch = frame.u32Len / sizeof(int16_t);
        if (in_samples_per_ch == 0) {
            LOGE(__func__, "empty frame: u32Len=%u mb_bytes=%u seq=%u soundmode=%d",
                 frame.u32Len, mb_bytes, frame.u32Seq, frame.enSoundMode);
            RK_MPI_AI_ReleaseFrame(cap->dev, cap->chn, &frame, &aec);
            return -1;
        }
        if (!cap->first_frame_logged) {
            RK_U32 ref_mb_bytes = 0;
            if (aec.bValid && aec.stRefFrame.pMbBlk) {
                ref_mb_bytes = RK_MPI_MB_GetSize(aec.stRefFrame.pMbBlk);
            }
            LOGI(__func__,
                 "first frame: u32Len=%u mb_bytes=%u seq=%u soundmode=%d out_ch=%u aec_valid=%d ref_len=%u ref_mb_bytes=%u ref_soundmode=%d",
                 frame.u32Len, mb_bytes, frame.u32Seq, frame.enSoundMode, cap->out_channels,
                 aec.bValid ? 1 : 0,
                 aec.bValid ? aec.stRefFrame.u32Len : 0,
                 ref_mb_bytes,
                 aec.bValid ? aec.stRefFrame.enSoundMode : -1);
            cap->first_frame_logged = 1;
        }

        size_t append_samples = in_samples_per_ch * ZH_AUDIO_CHANNELS;
        if (zh_rk_stash_reserve(cap, append_samples) != 0) {
            RK_MPI_AI_ReleaseFrame(cap->dev, cap->chn, &frame, &aec);
            return -1;
        }

        for (size_t i = 0; i < in_samples_per_ch; ++i) {
            int16_t v = src[i];
            cap->stash[cap->stash_len++] = v;
            cap->stash[cap->stash_len++] = v;
        }

        RK_MPI_AI_ReleaseFrame(cap->dev, cap->chn, &frame, &aec);
        if (++loops > 8) {
            break;
        }
    }

    if (cap->stash_len < need) {
        LOGE(__func__, "stash not enough: need=%zu have=%zu", need, cap->stash_len);
        return -1;
    }

    memcpy(buffer, cap->stash, need * sizeof(int16_t));
    cap->stash_len -= need;
    if (cap->stash_len > 0) {
        memmove(cap->stash, cap->stash + need, cap->stash_len * sizeof(int16_t));
    }
    return (int)need;
}

int zh_audio_capture_stop(zh_audio_capture_t *cap) {
    if (!cap) {
        return 0;
    }
    cap->started = 0;
    return 0;
}

void zh_audio_capture_deinit(zh_audio_capture_t *cap) {
    if (!cap) {
        return;
    }
    if (cap->vqe_enabled) {
        RK_MPI_AI_DisableVqe(cap->dev, cap->chn);
    }
    RK_MPI_AI_DisableDataRead(cap->dev, cap->chn);
    RK_MPI_AI_DisableChn(cap->dev, cap->chn);
    RK_MPI_AI_Disable(cap->dev);
    RK_MPI_SYS_Exit();
    free(cap->stash);
    free(cap);
}

int zh_audio_capture_dump(const char *path, int seconds) {
    if (!path || seconds <= 0) {
        errno = EINVAL;
        return -1;
    }

    zh_audio_capture_t *cap = NULL;
    unsigned int rate = 0;
    unsigned int ch = 0;
    if (zh_audio_capture_init(&cap, &rate, &ch) != 0) {
        return -1;
    }
    if (zh_audio_capture_start(cap) != 0) {
        zh_audio_capture_deinit(cap);
        return -1;
    }

    // 使用 RK_MPI_AI_SaveFile 直接由驱动写文件，避免 GetFrame 不出数据的问题
    AUDIO_SAVE_FILE_INFO_S save;
    memset(&save, 0, sizeof(save));
    save.bCfg = RK_TRUE;
    strncpy(save.aFilePath, "/tmp", sizeof(save.aFilePath) - 1);
    strncpy(save.aFileName, "aec.pcm", sizeof(save.aFileName) - 1);
    // 估算大小：16k * 2字节 * 1ch * seconds，单位KB，留出余量
    RK_U32 bytes = (RK_U32)(ZH_AUDIO_SAMPLE_RATE * 2 * seconds);
    save.u32FileSize = (bytes / 1024) + 128;
    RK_S32 sret = RK_MPI_AI_SaveFile(cap->dev, cap->chn, &save);
    if (sret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AI_SaveFile failed: %d", sret);
    } else {
        LOGI(__func__, "RK_MPI_AI_SaveFile started: /tmp/aec.pcm size_kb=%u", save.u32FileSize);
    }

    // 注意：rockit 的 SaveFile 只是开启“保存开关”，仍需要上层持续 GetFrame/ReleaseFrame 驱动管线跑起来。
    time_t end_ts = time(NULL) + seconds;
    LOGI(__func__, "dump loop start: seconds=%d end_ts=%ld", seconds, (long)end_ts);
    int got_frames = 0;
    int fail_frames = 0;
    while (time(NULL) < end_ts) {
        AUDIO_FRAME_S frame;
        AEC_FRAME_S aec;
        memset(&frame, 0, sizeof(frame));
        memset(&aec, 0, sizeof(aec));
        RK_S32 ret = RK_MPI_AI_GetFrame(cap->dev, cap->chn, &frame, &aec, 1000);
        if (ret != RK_SUCCESS) {
            LOGE(__func__, "dump GetFrame failed: %d", ret);
            if (++fail_frames % 50 == 0) {
                LOGE(__func__, "dump GetFrame still failing: %d", ret);
            }
            continue;
        }
        if (++got_frames == 1) {
            LOGI(__func__, "dump first frame: len=%u seq=%u",
                 frame.u32Len, frame.u32Seq);
        } else if (got_frames % 50 == 0) {
            LOGI(__func__, "dump frame ok: len=%u seq=%u",
                 frame.u32Len, frame.u32Seq);
        }
        RK_MPI_AI_ReleaseFrame(cap->dev, cap->chn, &frame, &aec);
    }
    // 停止保存
    save.bCfg = RK_FALSE;
    RK_MPI_AI_SaveFile(cap->dev, cap->chn, &save);
    zh_audio_capture_stop(cap);
    zh_audio_capture_deinit(cap);
    return 0;
}

#endif
