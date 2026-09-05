#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "wifi_bootstrap.h"

#define WPA_SUPP_LOG_FILE ZH_LOG_PATH "zh_wpa_supplicant.log"

static int zh_get_ipv4_of_iface(const char *ifname, char *ip, size_t ip_len) {
    struct ifaddrs *ifaddr = NULL;
    struct ifaddrs *ifa;
    int rc = -1;

    if (!ifname || !ip || ip_len == 0) {
        return -1;
    }

    if (getifaddrs(&ifaddr) != 0) {
        return -1;
    }

    for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (strcmp(ifa->ifa_name, ifname) != 0) {
            continue;
        }
        if (!inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, ip, ip_len)) {
            continue;
        }
        rc = 0;
        break;
    }

    freeifaddrs(ifaddr);
    return rc;
}

static int zh_get_wpa_state(char *state, size_t state_len) {
    FILE *fp;
    char line[256];

    if (!state || state_len == 0) {
        return -1;
    }
    state[0] = '\0';

    fp = popen("wpa_cli -i wlan0 status 2>/dev/null", "r");
    if (!fp) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "wpa_state=", 10) == 0) {
            strncpy(state, line + 10, state_len - 1);
            state[state_len - 1] = '\0';
            state[strcspn(state, "\r\n")] = '\0';
            break;
        }
    }
    pclose(fp);
    return (state[0] != '\0') ? 0 : -1;
}


static int zh_wifi_link_ready(char *ip, size_t ip_len) {
    char state[64] = {0};

    if (zh_get_ipv4_of_iface("wlan0", ip, ip_len) != 0) {
        return -1;
    }
    if (zh_get_wpa_state(state, sizeof(state)) != 0) {
        LOGW(__func__, "wpa_cli state unavailable, treat as not ready");
        return -1;
    }
    if (strcmp(state, "COMPLETED") != 0) {
        LOGW(__func__, "wpa_state=%s, treat as not ready", state);
        return -1;
    }
    return 0;
}

int zh_wifi_ensure_connected(void) {
    char ip[INET_ADDRSTRLEN] = {0};
    const char *cmds[] = {
        "wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf >" WPA_SUPP_LOG_FILE " 2>&1",
        "wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf >" WPA_SUPP_LOG_FILE " 2>&1",
        "wpa_supplicant -B -D wext -i wlan0 -c /etc/wpa_supplicant.conf >" WPA_SUPP_LOG_FILE " 2>&1",
    };
    int rc = -1;
    size_t cmd_count = sizeof(cmds) / sizeof(cmds[0]);
    size_t i;

    if (zh_wifi_link_ready(ip, sizeof(ip)) == 0) {
        LOGI(__func__, "wlan0 already connected, ip=%s", ip);
        return 0;
    }


    if (access("/etc/wpa_supplicant.conf", R_OK) != 0) {
        LOGW(__func__, "/etc/wpa_supplicant.conf missing, skip wifi bootstrap");
        return -1;
    }

    LOGI(__func__, "try wifi bootstrap via /etc/wpa_supplicant.conf");
    system("killall -9 wpa_supplicant >/dev/null 2>&1 || true");
    for (i = 0; i < cmd_count; ++i) {
        rc = system(cmds[i]);
        if (rc == 0) {
            break;
        }
        if (rc == -1) {
            LOGW(__func__, "start wpa_supplicant try%zu failed: %s", i + 1, strerror(errno));
        } else if (WIFEXITED(rc)) {
            LOGW(__func__, "start wpa_supplicant try%zu exit=%d", i + 1, WEXITSTATUS(rc));
        } else if (WIFSIGNALED(rc)) {
            LOGW(__func__, "start wpa_supplicant try%zu signal=%d", i + 1, WTERMSIG(rc));
        } else {
            LOGW(__func__, "start wpa_supplicant try%zu rc=0x%x", i + 1, rc);
        }
    }
    if (i == cmd_count) {
        LOGW(__func__, "start wpa_supplicant failed, see %s", WPA_SUPP_LOG_FILE);
        return -1;
    }

    for (int i = 0; i < 20; ++i) {
        if (zh_wifi_link_ready(ip, sizeof(ip)) == 0) {
            LOGI(__func__, "wifi bootstrap connected, ip=%s", ip);
            return 0;
        }
        usleep(500 * 1000);
    }

    LOGW(__func__, "wifi bootstrap timeout or auth failed, fallback to ble provision");
    return -1;
}
