#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "utils.h"

// 配置读取：从设备与文件获取 device_id / key 等基础信息。

// 将MAC地址转换为12位小写十六进制字符串。
static void zh_get_mac_hex_str(const unsigned char *mac, char *out, size_t out_len) {
    static const char hex[] = "0123456789abcdef";
    int j;

    if (!mac || !out || out_len < (ZH_DEVICE_ID_LEN + 1)) {
        return;
    }

    for (j = 0; j < 6; j++) {
        out[j * 2] = hex[(mac[j] >> 4) & 0x0f];
        out[j * 2 + 1] = hex[mac[j] & 0x0f];
    }
    out[ZH_DEVICE_ID_LEN] = '\0';
}

/*
    获取客户端配置，包括 device_id 和 key
*/
void zh_get_config(zh_config_t *cfg) {
    unsigned char mac[6];

    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    // 获取device_id(MAC地址转成12位字母)
    if (zh_get_mac_addr(ZH_CARD_NAME, mac, sizeof(mac)) != 0) {
        cfg->device_id[0] = '\0';
    } else {
        // 获取mac_addr成功，转换成目标格式(12位小写字母)
        zh_get_mac_hex_str(mac, cfg->device_id, sizeof(cfg->device_id));
    }

    LOGI(__func__, "device_id=%s", cfg->device_id);

    // 读取key文件内容
    if (zh_read_file(ZH_KEY_PATH, cfg->key, sizeof(cfg->key)) != 0) {
        cfg->key[0] = '\0';
    }
}
