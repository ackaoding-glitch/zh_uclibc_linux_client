#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "music_player.h"
#include "prompt_tone.h"
#include "utils.h"

// 保护初始化状态和断网提示时间戳。
static pthread_mutex_t g_prompt_mutex = PTHREAD_MUTEX_INITIALIZER;
// music_player 是否已初始化，避免重复启动。
static int g_prompt_inited = 0;
// 最近一次播放“网络断开”提示音的时间戳（毫秒）。
static uint64_t g_last_disconnected_ms = 0;
static pthread_t g_web_search_wait_thread;
static int g_web_search_wait_active = 0;
static int g_web_search_wait_thread_running = 0;

static const char *zh_prompt_tone_event_name(zh_prompt_tone_event_t event) {
    switch (event) {
        case ZH_PROMPT_TONE_BOOT:
            return "boot";
        case ZH_PROMPT_TONE_PROVISION:
            return "provision";
        case ZH_PROMPT_TONE_NET_CONNECTED:
            return "net_connected";
        case ZH_PROMPT_TONE_NET_DISCONNECTED:
            return "net_disconnected";
        case ZH_PROMPT_TONE_READY:
            return "ready";
        case ZH_PROMPT_TONE_WEB_SEARCH_WAIT:
            return "web_search_wait";
        default:
            return "unknown";
    }
}

// 将事件映射为配置中的 MP3 URL。
static const char *zh_prompt_tone_url(zh_prompt_tone_event_t event) {
    switch (event) {
        case ZH_PROMPT_TONE_BOOT:
            return ZH_PROMPT_BOOT_MP3_URL;
        case ZH_PROMPT_TONE_PROVISION:
            return ZH_PROMPT_PROVISION_MP3_URL;
        case ZH_PROMPT_TONE_NET_CONNECTED:
            return ZH_PROMPT_NET_CONNECTED_MP3_URL;
        case ZH_PROMPT_TONE_NET_DISCONNECTED:
            return ZH_PROMPT_NET_DISCONNECTED_MP3_URL;
        case ZH_PROMPT_TONE_READY:
            return ZH_PROMPT_CHAT_MODE_MP3_URL;
        case ZH_PROMPT_TONE_WEB_SEARCH_WAIT:
            return ZH_PROMPT_WEB_SEARCH_WAIT_MP3_URL;
        default:
            return "";
    }
}

int zh_prompt_tone_init(void) {
    int rc = 0;
    pthread_mutex_lock(&g_prompt_mutex);
    if (!g_prompt_inited) {
        rc = zh_music_player_start();
        if (rc == 0) {
            g_prompt_inited = 1;
            LOGI(__func__, "prompt tone init ok");
        }
    }
    pthread_mutex_unlock(&g_prompt_mutex);
    return rc;
}

void zh_prompt_tone_play(zh_prompt_tone_event_t event) {
    const char *url = zh_prompt_tone_url(event);
    const char *urls[1] = {url};

    if (!url || url[0] == '\0') {
        LOGW(__func__, "prompt tone skipped: event=%s url is empty",
             zh_prompt_tone_event_name(event));
        return;
    }

    if (zh_prompt_tone_init() != 0) {
        LOGE(__func__, "prompt tone init failed");
        return;
    }

    if (event == ZH_PROMPT_TONE_NET_DISCONNECTED) {
        uint64_t now_ms = zh_now_ms();
        pthread_mutex_lock(&g_prompt_mutex);
        // 断网事件可能由 WS ERROR/CLOSE 连续触发，5 秒内只播一次。
        if (g_last_disconnected_ms != 0 && now_ms - g_last_disconnected_ms < 5000) {
            pthread_mutex_unlock(&g_prompt_mutex);
            LOGI(__func__, "prompt tone debounced: event=%s", zh_prompt_tone_event_name(event));
            return;
        }
        g_last_disconnected_ms = now_ms;
        pthread_mutex_unlock(&g_prompt_mutex);
    }

    LOGI(__func__, "prompt tone play: event=%s src=%s",
         zh_prompt_tone_event_name(event), url);
    zh_music_player_play_urls_with_gain(urls, 1, ZH_PROMPT_TONE_GAIN);
}

static int zh_prompt_web_search_wait_is_active(void) {
    int active = 0;
    pthread_mutex_lock(&g_prompt_mutex);
    active = g_web_search_wait_active;
    pthread_mutex_unlock(&g_prompt_mutex);
    return active;
}

static void *zh_prompt_web_search_wait_thread_main(void *arg) {
    (void)arg;
    const char *urls[1] = {ZH_PROMPT_WEB_SEARCH_WAIT_MP3_URL};

    while (zh_prompt_web_search_wait_is_active()) {
        if (zh_prompt_tone_init() != 0 || !urls[0] || urls[0][0] == '\0') {
            usleep(200 * 1000);
            continue;
        }

        if (!zh_music_player_is_active()) {
            zh_music_player_play_urls_with_gain(urls, 1, ZH_PROMPT_WEB_SEARCH_WAIT_GAIN);
        }

        while (zh_prompt_web_search_wait_is_active() && zh_music_player_is_active()) {
            usleep(50 * 1000);
        }
    }

    pthread_mutex_lock(&g_prompt_mutex);
    g_web_search_wait_thread_running = 0;
    pthread_mutex_unlock(&g_prompt_mutex);
    return NULL;
}

void zh_prompt_tone_start_web_search_wait(void) {
    int create_thread = 0;

    pthread_mutex_lock(&g_prompt_mutex);
    g_web_search_wait_active = 1;
    if (!g_web_search_wait_thread_running) {
        g_web_search_wait_thread_running = 1;
        create_thread = 1;
    }
    pthread_mutex_unlock(&g_prompt_mutex);

    if (!create_thread) {
        LOGI(__func__, "web search wait tone already active");
        return;
    }

    int err = pthread_create(&g_web_search_wait_thread, NULL,
                             zh_prompt_web_search_wait_thread_main, NULL);
    if (err != 0) {
        pthread_mutex_lock(&g_prompt_mutex);
        g_web_search_wait_active = 0;
        g_web_search_wait_thread_running = 0;
        pthread_mutex_unlock(&g_prompt_mutex);
        LOGE(__func__, "web search wait tone thread create failed: err=%d", err);
        return;
    }
    pthread_detach(g_web_search_wait_thread);
    LOGI(__func__, "web search wait tone start");
}

void zh_prompt_tone_stop_web_search_wait(void) {
    int was_active = 0;

    pthread_mutex_lock(&g_prompt_mutex);
    was_active = g_web_search_wait_active;
    g_web_search_wait_active = 0;
    pthread_mutex_unlock(&g_prompt_mutex);

    if (was_active) {
        zh_music_player_interrupt();
        LOGI(__func__, "web search wait tone stop");
    }
}
