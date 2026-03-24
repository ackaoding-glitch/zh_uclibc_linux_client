#include <errno.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio_playback_rockit.h"
#include "bithion_core_bridge.h"
#include "config.h"
#include "log.h"
#include "music_player.h"
#include "udp_tts.h"
#include "utils.h"
#include "ws.h"

static volatile int g_tts_play_running = 0;
static volatile int g_tts_flush_pending = 0;
static volatile int g_tts_playing = 0;
static volatile int g_tts_drop_stale_pending = 0;
static volatile uint32_t g_tts_interrupt_generation = 0;
static pthread_t g_tts_play_thread;
static zh_ws_session_t *g_tts_ws = NULL;
static uint64_t g_tts_last_audio_ms = 0;
static uint64_t g_tts_open_fail_log_ms = 0;
static pthread_mutex_t g_tts_playback_mutex = PTHREAD_MUTEX_INITIALIZER;
static zh_ao_playback_t *g_tts_active_playback = NULL;

static void zh_tts_release_playback(zh_ao_playback_t **play_ref, int drain) {
    pthread_mutex_lock(&g_tts_playback_mutex);
    if (play_ref && *play_ref) {
        if (drain) {
            zh_ao_playback_drain(*play_ref);
        } else {
            zh_ao_playback_flush(*play_ref);
        }
        zh_ao_playback_close(*play_ref);
        if (g_tts_active_playback == *play_ref) {
            g_tts_active_playback = NULL;
        }
        *play_ref = NULL;
    }
    pthread_mutex_unlock(&g_tts_playback_mutex);
    g_tts_last_audio_ms = 0;
    g_tts_playing = 0;
}

static void zh_tts_reset_decoder(OpusDecoder *decoder) {
    if (!decoder) {
        return;
    }
    (void)opus_decoder_ctl(decoder, OPUS_RESET_STATE);
}

static void zh_tts_process_opus(OpusDecoder *decoder,
                                zh_ao_playback_t *playback,
                                const uint8_t *opus_data,
                                size_t opus_len) {
    int16_t pcm[ZH_TTS_MAX_SAMPLES * ZH_TTS_CHANNELS];

    if (!decoder || !playback || !opus_data || opus_len == 0) {
        return;
    }

    if (g_tts_flush_pending || !g_tts_play_running) {
        return;
    }

    int samples = opus_decode(decoder,
                              opus_data,
                              (opus_int32)opus_len,
                              pcm,
                              ZH_TTS_MAX_SAMPLES,
                              0);
    if (samples < 0) {
        LOGE(__func__, "opus decode failed: %d", samples);
        return;
    }
    if (ZH_TTS_GAIN != 1.0f) {
        for (int i = 0; i < samples * ZH_TTS_CHANNELS; ++i) {
            pcm[i] = zh_apply_gain(pcm[i], ZH_TTS_GAIN);
        }
    }
    if (g_tts_flush_pending || !g_tts_play_running) {
        return;
    }
    pthread_mutex_lock(&g_tts_playback_mutex);
    if (playback == g_tts_active_playback && !g_tts_flush_pending && g_tts_play_running) {
        g_tts_playing = 1;
        g_tts_last_audio_ms = zh_now_ms();
        (void)zh_ao_playback_write(playback, pcm, (size_t)samples);
    }
    pthread_mutex_unlock(&g_tts_playback_mutex);
}

static void *zh_udp_tts_play_thread_main(void *arg) {
    uint8_t opus_frame[4000];
    OpusDecoder *decoder = NULL;
    zh_ao_playback_t *playback = NULL;
    int opus_err = 0;

    (void)arg;
    decoder = opus_decoder_create(ZH_TTS_SAMPLE_RATE, ZH_TTS_CHANNELS, &opus_err);
    if (!decoder || opus_err != OPUS_OK) {
        LOGE(__func__, "opus decoder create failed: %d", opus_err);
        g_tts_play_running = 0;
        g_tts_playing = 0;
        return NULL;
    }

    LOGI(__func__, "client tts playback thread started");
    while (g_tts_play_running) {
        size_t opus_len = 0;
        int ret = 0;
        uint32_t read_generation = 0;

        if (g_tts_flush_pending) {
            zh_tts_release_playback(&playback, 0);
            zh_tts_reset_decoder(decoder);
            g_tts_flush_pending = 0;
        }

        read_generation = g_tts_interrupt_generation;
        ret = zh_core_tts_read_opus(opus_frame, sizeof(opus_frame), &opus_len, 100);
        if (!g_tts_play_running) {
            break;
        }
        if (read_generation != g_tts_interrupt_generation) {
            continue;
        }

        if (ret < 0) {
            usleep(10000);
            continue;
        }

        if (ret == 0) {
            int has_pending = zh_core_tts_transport_has_pending_data();
            uint64_t now_ms = zh_now_ms();

            if (g_tts_drop_stale_pending && !has_pending) {
                LOGI(__func__, "interrupt drain finished, accept new round audio");
                zh_tts_reset_decoder(decoder);
                g_tts_drop_stale_pending = 0;
            }

            if (playback && !has_pending && g_tts_last_audio_ms != 0 &&
                now_ms - g_tts_last_audio_ms >= ZH_TTS_IDLE_TIMEOUT_MS) {
                LOGI(__func__, "tts idle timeout, release playback");
                zh_tts_release_playback(&playback, 1);
                zh_tts_reset_decoder(decoder);
            }

            if (g_tts_ws && zh_udp_tts_is_round_done()) {
                zh_core_ws_on_tts_round_done();
            }

            if (!zh_core_tts_transport_is_running() && !has_pending) {
                break;
            }
            continue;
        }

        if (opus_len == 0) {
            continue;
        }

        if (g_tts_flush_pending) {
            zh_tts_release_playback(&playback, 0);
            zh_tts_reset_decoder(decoder);
            g_tts_flush_pending = 0;
            continue;
        }

        if (g_tts_drop_stale_pending) {
            continue;
        }

        if (zh_music_player_is_active()) {
            LOGI(__func__, "recv tts while music active, interrupt music");
            zh_music_player_interrupt();
        }

        if (!playback) {
            if (zh_ao_playback_open(&playback, ZH_TTS_SAMPLE_RATE, ZH_TTS_CHANNELS) != 0) {
                uint64_t now_ms = zh_now_ms();
                if (g_tts_open_fail_log_ms == 0 || now_ms - g_tts_open_fail_log_ms >= 1000) {
                    LOGE(__func__, "tts playback open failed, retry later");
                    g_tts_open_fail_log_ms = now_ms;
                }
                usleep(100000);
                continue;
            }
            pthread_mutex_lock(&g_tts_playback_mutex);
            g_tts_active_playback = playback;
            pthread_mutex_unlock(&g_tts_playback_mutex);
        }

        zh_tts_process_opus(decoder, playback, opus_frame, opus_len);

        while (g_tts_play_running) {
            if (g_tts_flush_pending) {
                zh_tts_release_playback(&playback, 0);
                zh_tts_reset_decoder(decoder);
                g_tts_flush_pending = 0;
                break;
            }

            opus_len = 0;
            read_generation = g_tts_interrupt_generation;
            ret = zh_core_tts_read_opus(opus_frame, sizeof(opus_frame), &opus_len, 0);
            if (read_generation != g_tts_interrupt_generation) {
                continue;
            }
            if (ret <= 0) {
                break;
            }
            if (g_tts_drop_stale_pending) {
                continue;
            }
            if (opus_len > 0) {
                zh_tts_process_opus(decoder, playback, opus_frame, opus_len);
            }
        }

        if (playback && g_tts_ws && zh_ws_is_tts_completed(g_tts_ws) &&
            !zh_core_tts_transport_has_pending_data()) {
            LOGI(__func__, "tts round done, release playback");
            zh_tts_release_playback(&playback, 1);
            zh_tts_reset_decoder(decoder);
            if (zh_udp_tts_is_round_done()) {
                zh_core_ws_on_tts_round_done();
            }
        }
    }

    zh_tts_release_playback(&playback, 0);
    if (decoder) {
        opus_decoder_destroy(decoder);
    }
    g_tts_play_running = 0;
    g_tts_playing = 0;
    LOGI(__func__, "client tts playback thread exit");
    return NULL;
}

int zh_udp_tts_start(const zh_config_t *cfg, zh_ws_session_t *ws) {
    int err = 0;

    g_tts_ws = ws;
    if (zh_core_tts_transport_start(cfg, ws) != 0) {
        return -1;
    }

    if (g_tts_play_running) {
        return 0;
    }

    g_tts_flush_pending = 0;
    g_tts_playing = 0;
    g_tts_drop_stale_pending = 0;
    g_tts_interrupt_generation = 0;
    g_tts_last_audio_ms = 0;
    g_tts_open_fail_log_ms = 0;
    g_tts_play_running = 1;
    err = pthread_create(&g_tts_play_thread, NULL, zh_udp_tts_play_thread_main, NULL);
    if (err != 0) {
        g_tts_play_running = 0;
        zh_core_tts_transport_stop();
        errno = err;
        return -1;
    }
    return 0;
}

void zh_udp_tts_stop(void) {
    g_tts_play_running = 0;
    g_tts_flush_pending = 1;
    g_tts_playing = 0;
    g_tts_drop_stale_pending = 0;
    zh_core_tts_transport_stop();
}

int zh_udp_tts_wait(int timeout_ms) {
    int waited = 0;

    while (g_tts_play_running) {
        usleep(50000);
        waited += 50;
        if (timeout_ms > 0 && waited >= timeout_ms) {
            return -1;
        }
    }

    if (timeout_ms > 0) {
        int remain = timeout_ms - waited;
        return zh_core_tts_transport_wait(remain > 0 ? remain : 1);
    }
    return zh_core_tts_transport_wait(timeout_ms);
}

void zh_udp_tts_interrupt(void) {
    g_tts_flush_pending = 1;
    g_tts_playing = 0;
    g_tts_drop_stale_pending = 1;
    g_tts_interrupt_generation++;
    g_tts_last_audio_ms = 0;

    pthread_mutex_lock(&g_tts_playback_mutex);
    if (g_tts_active_playback) {
        zh_ao_playback_flush(g_tts_active_playback);
    }
    pthread_mutex_unlock(&g_tts_playback_mutex);

    zh_core_tts_transport_interrupt();
}

void zh_udp_tts_set_playing(int playing) {
    g_tts_playing = playing ? 1 : 0;
}

int zh_udp_tts_is_playing(void) {
    return g_tts_playing;
}

int zh_udp_tts_is_busy(void) {
    return (g_tts_playing || zh_core_tts_transport_has_pending_data()) ? 1 : 0;
}

int zh_udp_tts_is_round_done(void) {
    if (!g_tts_ws || !zh_ws_is_tts_completed(g_tts_ws)) {
        return 0;
    }
    if (g_tts_playing) {
        return 0;
    }
    return zh_core_tts_transport_has_pending_data() ? 0 : 1;
}
