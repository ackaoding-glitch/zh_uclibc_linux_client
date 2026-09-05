// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/wait.h>
#include "config.h"
#include "log.h"

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"
#include "lib/sdp.h"
#include "lib/sdp_lib.h"
	
#include "lib/mgmt.h"
#include "uuid-helper.h"

#include "shared/mainloop.h"
#include "shared/mgmt.h"
#include "shared/util.h"
#include "shared/att.h"
#include "shared/gatt-helpers.h"
#include "shared/queue.h"
#include "shared/timeout.h"
#include "shared/gatt-db.h"
#include "shared/gatt-server.h"

#define UUID_GAP			0x1800
#define UUID_GATT			0x1801
#define UUID_HEART_RATE			0x180d
#define UUID_HEART_RATE_MSRMT		0x2a37
#define UUID_HEART_RATE_BODY		0x2a38
#define UUID_HEART_RATE_CTRL		0x2a39

#define UUID_WIFI			0xAF00
#define UUID_WIFI_WRITE_CHR		0xAE81
#define UUID_WIFI_NOTIFY_CHR		0xAE82

#define ATT_CID 4
#define WPA_SUPP_LOG_FILE ZH_LOG_PATH "zh_wpa_supplicant.log"
#define BLE_LOG_TAG "BLE_PROV"

#define PRLOG(...) \
	do { \
		printf(__VA_ARGS__); \
		print_prompt(); \
	} while (0)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define COLOR_OFF	"\x1B[0m"
#define COLOR_RED	"\x1B[0;91m"
#define COLOR_GREEN	"\x1B[0;92m"
#define COLOR_YELLOW	"\x1B[0;93m"
#define COLOR_BLUE	"\x1B[0;94m"
#define COLOR_MAGENTA	"\x1B[0;95m"
#define COLOR_BOLDGRAY	"\x1B[1;30m"
#define COLOR_BOLDWHITE	"\x1B[1;37m"

static const char default_device_name[] = "JLzh_unknown";
static bool verbose = false;
#define WIFI_RX_BUF_MAX 1024
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_provision_done = 0;
static volatile sig_atomic_t g_wait_disconnect_after_success = 0;

struct server {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_server *gatt;

	uint8_t *device_name;
	size_t name_len;

	uint16_t gatt_svc_chngd_handle;
	bool svc_chngd_enabled;

	uint16_t hr_handle;
	uint16_t hr_msrmt_handle;
	uint16_t hr_energy_expended;

	//wifi
	uint16_t wifi_handle;
	uint16_t wifi_write_handle;
	uint16_t wifi_notify_handle;
	bool wifi_chngd_enabled;
	bool wifi_list_phase_done;
	uint8_t wifi_rx_buf[WIFI_RX_BUF_MAX];
	size_t wifi_rx_len;

	bool hr_visible;
	bool hr_msrmt_enabled;
	int hr_ee_count;
	unsigned int hr_timeout_id;
};

static void print_prompt(void)
{
	printf(COLOR_BLUE "[GATT server]" COLOR_OFF "# ");
	fflush(stdout);
}

static void att_disconnect_cb(int err, void *user_data)
{
	LOGI(BLE_LOG_TAG, "device disconnected: %s", strerror(err));
	if (g_wait_disconnect_after_success) {
		g_provision_done = 1;
	}

	mainloop_quit();
}

static void att_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_BOLDGRAY "%s" COLOR_BOLDWHITE "%s\n" COLOR_OFF, prefix,
									str);
}

static void gatt_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_GREEN "%s%s\n" COLOR_OFF, prefix, str);
}

static void wifi_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;

	printf("wifi_read_cb called: offset: %d\n", offset);

	len = 5;
	value = "hello";

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static uint16_t crc16_xmodem(const uint8_t *data, size_t len)
{
	uint16_t crc = 0x0000;

	for (size_t i = 0; i < len; i++) {
		crc ^= ((uint16_t)data[i] << 8);
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc <<= 1;
		}
	}
	return crc;
}

static void print_hex_line(const char *tag, const uint8_t *buf, size_t len);

static int send_wifi_notify_packet(struct server *server, uint16_t cmd,
					const uint8_t *data, size_t data_len)
{
	uint8_t pkt[768];
	uint16_t payload_len;
	uint16_t crc;
	size_t frame_len;
	uint16_t mtu = 0;

	if (!server->wifi_chngd_enabled) {
		printf("ble_tx_skip: ccc notify disabled\n");
		return -1;
	}

	payload_len = (uint16_t)(2 + data_len);
	frame_len = (size_t)payload_len + 7;
	if (frame_len > sizeof(pkt)) {
		printf("ble_tx_err: frame too large len=%zu\n", frame_len);
		return -1;
	}

	pkt[0] = 0x4A;
	pkt[1] = 0x4C;
	pkt[2] = (payload_len >> 8) & 0xFF;
	pkt[3] = payload_len & 0xFF;
	pkt[4] = (cmd >> 8) & 0xFF;
	pkt[5] = cmd & 0xFF;
	if (data_len > 0)
		memcpy(pkt + 6, data, data_len);
	crc = crc16_xmodem(pkt + 2, payload_len + 2);
	pkt[6 + data_len] = (crc >> 8) & 0xFF;
	pkt[7 + data_len] = crc & 0xFF;
	pkt[8 + data_len] = 0xFF;

	print_hex_line("ble_tx_hex", pkt, frame_len);

	mtu = bt_gatt_server_get_mtu(server->gatt);
	if (mtu < 23)
		mtu = 23;
	LOGI(BLE_LOG_TAG, "ble tx: mtu=%u total=%zu", mtu, frame_len);
	if (frame_len > (size_t)(mtu - 3)) {
		LOGW(BLE_LOG_TAG, "ble tx frame too large: frame_len=%zu mtu_payload=%u",
		     frame_len, (uint16_t)(mtu - 3));
		return -1;
	}
	if (!bt_gatt_server_send_notification(server->gatt,
						server->wifi_notify_handle,
						pkt, frame_len, false)) {
		LOGW(BLE_LOG_TAG, "ble notify send failed: len=%zu", frame_len);
		return -1;
	}
	return 0;
}

static void mtu_exchange_cb(bool success, uint8_t att_ecode, void *user_data)
{
	struct server *server = user_data;
	uint16_t mtu = bt_att_get_mtu(server->att);
	LOGI(BLE_LOG_TAG, "ble mtu exchange: success=%d att_ecode=0x%02X mtu=%u",
	     success ? 1 : 0, att_ecode, mtu);
}

static void send_status_notify(struct server *server, int status)
{
	char json[32];
	int n = snprintf(json, sizeof(json), "{\"status\":%d}", status);
	if (n < 0)
		return;
	send_wifi_notify_packet(server, 0x1002, (const uint8_t *)json, (size_t)n);
}

static void send_status_notify_retry(struct server *server, int status, int times, int interval_ms)
{
	for (int i = 0; i < times; i++) {
		send_status_notify(server, status);
		if (i + 1 < times)
			usleep((useconds_t)interval_ms * 1000);
	}
}

static int json_get_string_field(const char *json, size_t len,
					const char *key, char *out, size_t out_len)
{
	char pattern[32];
	const char *p, *end, *q;
	size_t n;

	if (!json || !key || !out || out_len == 0)
		return -1;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p || p >= json + len)
		return -1;
	p += strlen(pattern);
	while (p < json + len && *p != ':')
		p++;
	if (p >= json + len || *p != ':')
		return -1;
	p++;
	while (p < json + len && isspace((unsigned char)*p))
		p++;
	if (p >= json + len || *p != '"')
		return -1;
	p++;
	end = json + len;
	q = p;
	while (q < end && *q != '"')
		q++;
	if (q >= end)
		return -1;
	n = (size_t)(q - p);
	if (n >= out_len)
		n = out_len - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 0;
}

static int parse_wifi_credential(const char *json, size_t len,
					char *ssid, size_t ssid_len,
					char *pass, size_t pass_len)
{
	if (json_get_string_field(json, len, "ssid", ssid, ssid_len) != 0 &&
	    json_get_string_field(json, len, "SSID", ssid, ssid_len) != 0)
		return -1;

	if (json_get_string_field(json, len, "pass", pass, pass_len) != 0 &&
	    json_get_string_field(json, len, "PSK", pass, pass_len) != 0) {
		pass[0] = '\0';
	}
	return 0;
}

static void escape_wpa_str(const char *in, char *out, size_t out_len)
{
	size_t j = 0;
	if (!in || !out || out_len == 0)
		return;
	for (size_t i = 0; in[i] != '\0' && j + 1 < out_len; i++) {
		if ((in[i] == '"' || in[i] == '\\') && j + 2 < out_len)
			out[j++] = '\\';
		out[j++] = in[i];
	}
	out[j] = '\0';
}

static int write_wpa_supplicant_conf(const char *ssid, const char *pass)
{
	FILE *fp;
	char esc_ssid[128];
	char esc_pass[160];
	int is_open_network;

	escape_wpa_str(ssid, esc_ssid, sizeof(esc_ssid));
	escape_wpa_str(pass, esc_pass, sizeof(esc_pass));
	is_open_network = !pass || pass[0] == '\0';

	fp = fopen("/etc/wpa_supplicant.conf", "w");
	if (!fp) {
		LOGE(BLE_LOG_TAG, "open /etc/wpa_supplicant.conf failed: %s", strerror(errno));
		return -1;
	}

	fprintf(fp,
		"ctrl_interface=/var/run/wpa_supplicant\n"
		"ap_scan=1\n"
		"update_config=1\n"
		"network={\n"
		"\tssid=\"%s\"\n",
		esc_ssid);
	if (is_open_network) {
		fprintf(fp, "\tkey_mgmt=NONE\n");
	} else {
		fprintf(fp,
			"\tpsk=\"%s\"\n"
			"\tkey_mgmt=WPA-PSK\n",
			esc_pass);
	}
	fprintf(fp, "}\n");
	fclose(fp);
	LOGI(BLE_LOG_TAG, "write wifi config: ssid=%s security=%s",
	     ssid, is_open_network ? "open" : "wpa-psk");
	return 0;
}

static int get_ipv4_of_iface(const char *ifname, char *ip, size_t ip_len)
{
	struct ifaddrs *ifaddr, *ifa;
	int ok = -1;

	if (getifaddrs(&ifaddr) < 0)
		return -1;
	for (ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (strcmp(ifa->ifa_name, ifname) != 0)
			continue;
		if (!inet_ntop(AF_INET,
				&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
				ip, ip_len))
			continue;
		ok = 0;
		break;
	}
	freeifaddrs(ifaddr);
	return ok;
}

static int get_wpa_state(char *state, size_t state_len)
{
	FILE *fp;
	char line[256];

	if (!state || state_len == 0)
		return -1;
	state[0] = '\0';

	fp = popen("wpa_cli -i wlan0 status 2>/dev/null", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "wpa_state=", 10) == 0) {
			strncpy(state, line + 10, state_len - 1);
			state[state_len - 1] = '\0';
			state[strcspn(state, "\r\n")] = '\0';
			break;
		}
	}
	pclose(fp);
	return state[0] ? 0 : -1;
}

static int wifi_link_ready(char *ip, size_t ip_len)
{
	char state[64];

	if (get_ipv4_of_iface("wlan0", ip, ip_len) != 0)
		return -1;
	if (get_wpa_state(state, sizeof(state)) != 0)
		return -1;
	if (strcmp(state, "COMPLETED") != 0) {
		LOGI(BLE_LOG_TAG, "wifi waiting: wpa_state=%s ip=%s", state, ip);
		return -1;
	}
	return 0;
}


static int run_wifi_connect_flow(const char *ssid, const char *pass)
{
	char ip[INET_ADDRSTRLEN] = {0};

	int rc;
	const char *cmds[] = {
		"wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf >" WPA_SUPP_LOG_FILE " 2>&1",
		"wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf >" WPA_SUPP_LOG_FILE " 2>&1",
		"wpa_supplicant -B -D wext -i wlan0 -c /etc/wpa_supplicant.conf >" WPA_SUPP_LOG_FILE " 2>&1",
	};
	size_t cmd_count = sizeof(cmds) / sizeof(cmds[0]);
	size_t i;

	if (write_wpa_supplicant_conf(ssid, pass) != 0)
		return 2;

	system("killall -9 wpa_supplicant >/dev/null 2>&1 || true");
	for (i = 0; i < cmd_count; i++) {
		rc = system(cmds[i]);
		if (rc == 0)
			break;
		if (rc == -1) {
			LOGW(BLE_LOG_TAG, "start wpa_supplicant try%zu failed: %s",
			     i + 1, strerror(errno));
		} else if (WIFEXITED(rc)) {
			LOGW(BLE_LOG_TAG, "start wpa_supplicant try%zu exit=%d",
			     i + 1, WEXITSTATUS(rc));
		} else if (WIFSIGNALED(rc)) {
			LOGW(BLE_LOG_TAG, "start wpa_supplicant try%zu signal=%d",
			     i + 1, WTERMSIG(rc));
		} else {
			LOGW(BLE_LOG_TAG, "start wpa_supplicant try%zu rc=0x%x", i + 1, rc);
		}
	}
	if (i == cmd_count) {
		LOGE(BLE_LOG_TAG, "start wpa_supplicant failed, see %s", WPA_SUPP_LOG_FILE);
		return 2;
	}

	for (int retry = 0; retry < 20; retry++) {
		if (wifi_link_ready(ip, sizeof(ip)) == 0) {
			LOGI(BLE_LOG_TAG, "wifi connected: wlan0 ip=%s", ip);
			return 0;
		}
		usleep(500 * 1000);
	}
	LOGE(BLE_LOG_TAG, "wifi connect timeout/auth fail, map status=4");
	return 4;
}


static void send_demo_wifi_list(struct server *server)
{
	/* AP 列表，先全部收集再排序 */
	struct ap_entry {
		char ssid[128];
		int  rssi;
	} aps[64];
	int total_aps = 0;

	char line[256];
	FILE *fp;
	int ready = 0;
	int i;
	const char *scan_conf = "/tmp/wpa_scan.conf";
	const char *cmds[] = {
		"wpa_supplicant -B -i wlan0 -c /tmp/wpa_scan.conf >/dev/null 2>&1",
		"wpa_supplicant -B -D nl80211 -i wlan0 -c /tmp/wpa_scan.conf >/dev/null 2>&1",
		"wpa_supplicant -B -D wext -i wlan0 -c /tmp/wpa_scan.conf >/dev/null 2>&1",
	};

	/* 写扫描专用最小配置，不含任何 network 块 */
	fp = fopen(scan_conf, "w");
	if (!fp) {
		printf("ble_wifi_err: write scan conf failed\n");
		goto fallback;
	}
	fprintf(fp, "ctrl_interface=/var/run/wpa_supplicant\nap_scan=1\n");
	fclose(fp);
	fp = NULL;

	/* 先杀掉可能残留的实例，再启动 */
	system("killall -9 wpa_supplicant >/dev/null 2>&1 || true");
	for (i = 0; i < 3; i++) {
		if (system(cmds[i]) == 0)
			break;
	}
	if (i == 3) {
		printf("ble_wifi_err: start wpa_supplicant for scan failed\n");
		goto fallback;
	}

	/* 等待 wpa_supplicant 就绪 */
	for (i = 0; i < 10 && !ready; i++) {
		usleep(300 * 1000);
		if (system("wpa_cli -i wlan0 ping >/dev/null 2>&1") == 0)
			ready = 1;
	}
	if (!ready) {
		printf("ble_wifi_err: wpa_supplicant not ready\n");
		goto fallback_kill;
	}

	/* 触发扫描，等待完成 */
	system("wpa_cli -i wlan0 scan >/dev/null 2>&1");
	usleep(3000 * 1000);

	fp = popen("wpa_cli -i wlan0 scan_results 2>/dev/null", "r");
	if (!fp) {
		printf("ble_wifi_err: scan_results popen failed\n");
		goto fallback_kill;
	}

	fgets(line, sizeof(line), fp); /* 跳过表头 */

	while (fgets(line, sizeof(line), fp) &&
	       total_aps < (int)(sizeof(aps) / sizeof(aps[0]))) {
		char bssid[32], flags[256], raw_ssid[128];
		int freq, signal;

		if (sscanf(line, "%31s %d %d %255s %127[^\n]",
			   bssid, &freq, &signal, flags, raw_ssid) != 5)
			continue;

		/* 过滤：ssid 中含非打印字符（如 \xNN）说明是中文或特殊字符，
		 * wpa_cli 会以 \xNN 形式输出，这类 ssid 跳过，避免 JSON 乱码。
		 * 也过滤空 ssid（隐藏 AP）。 */
		if (raw_ssid[0] == '\0')
			continue;
		int has_escape = 0;
		for (const char *p = raw_ssid; *p; p++) {
			if (*p == '\\' && *(p+1) == 'x') {
				has_escape = 1;
				break;
			}
		}
		if (has_escape)
			continue;

		strncpy(aps[total_aps].ssid, raw_ssid,
			sizeof(aps[total_aps].ssid) - 1);
		aps[total_aps].ssid[sizeof(aps[total_aps].ssid) - 1] = '\0';
		aps[total_aps].rssi = signal;
		total_aps++;
	}
	pclose(fp);
	fp = NULL;

	/* 扫描完毕，杀掉临时 wpa_supplicant；连接时会重新启动 */
	system("killall wpa_supplicant >/dev/null 2>&1 || true");

	if (total_aps == 0)
		goto fallback;

	/* 按信号强度降序排序（冒泡，数量不多） */
	for (i = 0; i < total_aps - 1; i++) {
		for (int j = i + 1; j < total_aps; j++) {
			if (aps[j].rssi > aps[i].rssi) {
				struct ap_entry tmp = aps[i];
				aps[i] = aps[j];
				aps[j] = tmp;
			}
		}
	}

	/* 计算本次允许的最大 JSON 字节数：
	 * BLE frame = magic(2)+len(2)+cmd(2)+json+crc(2)+0xFF(1) = json+9
	 * frame 须 <= mtu-3（ATT header）
	 * => max_json = mtu - 3 - 9 = mtu - 12 */
	{
		uint16_t mtu = bt_gatt_server_get_mtu(server->gatt);
		if (mtu < 23)
			mtu = 23;
		int max_json = (int)(mtu - 3) - 9;
		char json[2048];
		int json_len = snprintf(json, sizeof(json), "[");
		int ap_count = 0;

		for (i = 0; i < total_aps; i++) {
			/* 转义 ssid 中的双引号和反斜杠 */
			char safe[256];
			size_t si = 0, di = 0;
			const char *s = aps[i].ssid;
			while (s[si] && di + 2 < sizeof(safe)) {
				if (s[si] == '"' || s[si] == '\\')
					safe[di++] = '\\';
				safe[di++] = s[si++];
			}
			safe[di] = '\0';

			/* 先算这条 entry 加进去的长度，+1 是末尾 ']' */
			char entry[300];
			int elen = snprintf(entry, sizeof(entry),
					    "%s{\"ssid\":\"%s\",\"rssi\":%d}",
					    ap_count > 0 ? "," : "",
					    safe, aps[i].rssi);
			if (json_len + elen + 1 > max_json)
				break; /* 放不下了，停止 */

			json_len += snprintf(json + json_len,
					     sizeof(json) - json_len,
					     "%s", entry);
			ap_count++;
		}

		json_len += snprintf(json + json_len, sizeof(json) - json_len,
				     "]");

		if (ap_count == 0)
			goto fallback;

		LOGI(BLE_LOG_TAG, "send wifi list: %d/%d APs cmd=0x1001", ap_count, total_aps);
		send_wifi_notify_packet(server, 0x1001,
					(const uint8_t *)json, (size_t)json_len);
	}
	return;

fallback_kill:
	system("killall wpa_supplicant >/dev/null 2>&1 || true");
fallback:
	LOGW(BLE_LOG_TAG, "scan got 0 APs, send empty list");
	send_wifi_notify_packet(server, 0x1001,
				(const uint8_t *)"[]", 2);
}

static void handle_wifi_cmd(struct server *server, uint16_t cmd,
				const uint8_t *payload, size_t payload_len)
{
	char json[512];

	if (cmd == 0x1002) {
		LOGI(BLE_LOG_TAG, "got cmd=0x1002, mark list phase done");
		server->wifi_list_phase_done = true;
		send_demo_wifi_list(server);
		return;
	}

	if (cmd == 0x1001) {
		char ssid[64];
		char pass[80];
		size_t n = payload_len;

		if (n >= sizeof(json))
			n = sizeof(json) - 1;
		memcpy(json, payload, n);
		json[n] = '\0';

		if (parse_wifi_credential(json, n, ssid, sizeof(ssid), pass, sizeof(pass)) != 0) {
			LOGW(BLE_LOG_TAG, "invalid wifi json payload");
			send_status_notify(server, 2);
			return;
		}

		LOGI(BLE_LOG_TAG, "received wifi credential: ssid=%s", ssid);
		if (!server->wifi_list_phase_done) {
			LOGW(BLE_LOG_TAG, "cmd=0x1001 before 0x1002, send list first");
			send_demo_wifi_list(server);
			server->wifi_list_phase_done = true;
		}
		send_status_notify(server, 1);
		{
			int wifi_result = run_wifi_connect_flow(ssid, pass);
			if (wifi_result == 0) {
			send_status_notify_retry(server, 0, 3, 200);
			LOGI(BLE_LOG_TAG, "wifi provision success, wait app disconnect");
			/*
			 * 不主动断开，等待手机端收到成功状态并主动断开；
			 * 断开后在 att_disconnect_cb 中置位 g_provision_done。
			 */
			g_wait_disconnect_after_success = 1;
			} else {
				send_status_notify(server, wifi_result);
			}
		}
		return;
	}

	LOGW(BLE_LOG_TAG, "unsupported cmd=0x%04X", cmd);
	send_status_notify(server, 2);
}

static void print_hex_line(const char *tag, const uint8_t *buf, size_t len)
{
	printf("%s[%zu]:", tag ? tag : "ble_hex", len);
	for (size_t i = 0; i < len; i++)
		printf(" %02X", buf[i]);
	printf("\n");
}

static void print_payload_json(const uint8_t *payload, size_t payload_len)
{
	char text[512];
	size_t n = payload_len;

	if (n >= sizeof(text))
		n = sizeof(text) - 1;
	memcpy(text, payload, n);
	text[n] = '\0';
	printf("ble_rx_json:%s\n", text);
}

static void wifi_process_frames(struct server *server)
{
	while (server->wifi_rx_len >= 2) {
		size_t frame_len;
		uint16_t payload_plus_cmd_len;
		uint16_t crc_expect, crc_actual;
		const uint8_t *frame = server->wifi_rx_buf;

		if (!(frame[0] == 0x4A && frame[1] == 0x4C)) {
			memmove(server->wifi_rx_buf, server->wifi_rx_buf + 1, --server->wifi_rx_len);
			continue;
		}

		if (server->wifi_rx_len < 4)
			return;

		payload_plus_cmd_len = ((uint16_t)frame[2] << 8) | frame[3];
		frame_len = (size_t)payload_plus_cmd_len + 7;
		if (frame_len > WIFI_RX_BUF_MAX) {
			printf("ble_rx_err: invalid frame_len=%zu\n", frame_len);
			server->wifi_rx_len = 0;
			return;
		}
		if (server->wifi_rx_len < frame_len)
			return;

		if (frame[frame_len - 1] != 0xFF) {
			printf("ble_rx_err: invalid eof=0x%02X\n", frame[frame_len - 1]);
			memmove(server->wifi_rx_buf, server->wifi_rx_buf + frame_len, server->wifi_rx_len - frame_len);
			server->wifi_rx_len -= frame_len;
			continue;
		}

		crc_expect = ((uint16_t)frame[frame_len - 3] << 8) | frame[frame_len - 2];
		crc_actual = crc16_xmodem(frame + 2, payload_plus_cmd_len + 2);
		if (crc_expect != crc_actual) {
			LOGW(BLE_LOG_TAG, "crc mismatch expect=0x%04X actual=0x%04X",
			     crc_expect, crc_actual);
			memmove(server->wifi_rx_buf, server->wifi_rx_buf + frame_len, server->wifi_rx_len - frame_len);
			server->wifi_rx_len -= frame_len;
			continue;
		}

		print_hex_line("ble_rx_hex", frame, frame_len);
		LOGI(BLE_LOG_TAG, "received cmd=0x%02X%02X", frame[4], frame[5]);
		if (payload_plus_cmd_len > 2) {
			size_t payload_len = payload_plus_cmd_len - 2;
			print_payload_json(frame + 6, payload_len);
			handle_wifi_cmd(server,
				(uint16_t)(((uint16_t)frame[4] << 8) | frame[5]),
				frame + 6, payload_len);
		} else {
			handle_wifi_cmd(server,
				(uint16_t)(((uint16_t)frame[4] << 8) | frame[5]),
				NULL, 0);
		}

		memmove(server->wifi_rx_buf, server->wifi_rx_buf + frame_len, server->wifi_rx_len - frame_len);
		server->wifi_rx_len -= frame_len;
	}
}

static void wifi_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;

	printf("wifi_write_cb called: offset: %u,len=%zu\n", offset, len);
	if (!value || len == 0)
		goto done;

	if (offset == 0 && (server->wifi_rx_len + len) <= WIFI_RX_BUF_MAX) {
		memcpy(server->wifi_rx_buf + server->wifi_rx_len, value, len);
		server->wifi_rx_len += len;
		wifi_process_frames(server);
	} else {
		printf("ble_rx_err: buffer overflow or unsupported offset, reset session\n");
		server->wifi_rx_len = 0;
	}
    
done:
	gatt_db_attribute_write_result(attrib, id, error);
}

static void gap_device_name_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;

	PRLOG("GAP Device Name Read called\n");

	len = server->name_len;

	if (offset > len) {
		error = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	len -= offset;
	value = len ? &server->device_name[offset] : NULL;

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static void gap_device_name_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;

	PRLOG("GAP Device Name Write called\n");

	/* If the value is being completely truncated, clean up and return */
	if (!(offset + len)) {
		free(server->device_name);
		server->device_name = NULL;
		server->name_len = 0;
		goto done;
	}

	/* Implement this as a variable length attribute value. */
	if (offset > server->name_len) {
		error = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (offset + len != server->name_len) {
		uint8_t *name;

		name = realloc(server->device_name, offset + len);
		if (!name) {
			error = BT_ATT_ERROR_INSUFFICIENT_RESOURCES;
			goto done;
		}

		server->device_name = name;
		server->name_len = offset + len;
	}

	if (value)
		memcpy(server->device_name + offset, value, len);

done:
	gatt_db_attribute_write_result(attrib, id, error);
}

static void gap_device_name_ext_prop_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	uint8_t value[2];

	PRLOG("Device Name Extended Properties Read called\n");

	value[0] = BT_GATT_CHRC_EXT_PROP_RELIABLE_WRITE;
	value[1] = 0;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void gatt_service_changed_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	PRLOG("Service Changed Read called\n");

	gatt_db_attribute_read_result(attrib, id, 0, NULL, 0);
}

static void gatt_svc_chngd_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	PRLOG("Service Changed CCC Read called\n");

	value[0] = server->svc_chngd_enabled ? 0x02 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void gatt_svc_chngd_ccc_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	PRLOG("Service Changed CCC Write called\n");

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00)
		server->svc_chngd_enabled = false;
	else if (value[0] == 0x02)
		server->svc_chngd_enabled = true;
	else
		ecode = 0x80;

	PRLOG("Service Changed Enabled: %s\n",
				server->svc_chngd_enabled ? "true" : "false");

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void hr_msrmt_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	value[0] = server->hr_msrmt_enabled ? 0x01 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, 2);
}

static bool hr_msrmt_cb(void *user_data)
{
	struct server *server = user_data;
	bool expended_present = !(server->hr_ee_count % 10);
	uint16_t len = 2;
	uint8_t pdu[4];
	uint32_t cur_ee;
	uint32_t val;

	if (util_getrandom(&val, sizeof(val), 0) < 0)
		return false;

	pdu[0] = 0x06;
	pdu[1] = 90 + (val % 40);

	if (expended_present) {
		pdu[0] |= 0x08;
		put_le16(server->hr_energy_expended, pdu + 2);
		len += 2;
	}

	bt_gatt_server_send_notification(server->gatt,
						server->hr_msrmt_handle,
						pdu, len, false);


	cur_ee = server->hr_energy_expended;
	server->hr_energy_expended = MIN(UINT16_MAX, cur_ee + 10);
	server->hr_ee_count++;

	return true;
}

static void update_hr_msrmt_simulation(struct server *server)
{
	if (!server->hr_msrmt_enabled || !server->hr_visible) {
		timeout_remove(server->hr_timeout_id);
		return;
	}

	server->hr_timeout_id = timeout_add(1000, hr_msrmt_cb, server, NULL);
}

static void hr_msrmt_ccc_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00)
		server->hr_msrmt_enabled = false;
	else if (value[0] == 0x01) {
		if (server->hr_msrmt_enabled) {
			PRLOG("HR Measurement Already Enabled\n");
			goto done;
		}

		server->hr_msrmt_enabled = true;
	} else
		ecode = 0x80;

	PRLOG("HR: Measurement Enabled: %s\n",
				server->hr_msrmt_enabled ? "true" : "false");

	update_hr_msrmt_simulation(server);

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void hr_control_point_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	if (!value || len != 1) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 1) {
		PRLOG("HR: Energy Expended value reset\n");
		server->hr_energy_expended = 0;
	}

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void confirm_write(struct gatt_db_attribute *attr, int err,
							void *user_data)
{
	if (!err)
		return;

	fprintf(stderr, "Error caching attribute %p - err: %d\n", attr, err);
	exit(1);
}

static void wifi_chngd_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	PRLOG("Wifi: Service Changed CCC Read called\n");

	value[0] = server->wifi_chngd_enabled ? 0x02 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void wifi_chngd_ccc_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	PRLOG("Wifi: Service Changed CCC Write called [%d:%d]\n", value[0], value[1]);

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00) {
		server->wifi_chngd_enabled = false;
		server->wifi_list_phase_done = false;
	} else if (value[0] == 0x01) {
		server->wifi_chngd_enabled = true;
	} else
		ecode = 0x80;

	PRLOG("Wifi: Service Changed Enabled: %s\n",
				server->wifi_chngd_enabled ? "true" : "false");
	if (server->wifi_chngd_enabled) {
		/* 兼容部分小程序状态机：订阅 notify 后主动下发一次列表。 */
		server->wifi_list_phase_done = true;
		send_demo_wifi_list(server);
	}

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void populate_gap_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *tmp;
	uint16_t appearance;

	/* Add the GAP service */
	bt_uuid16_create(&uuid, UUID_GAP);
	service = gatt_db_add_service(server->db, &uuid, true, 6);

	/*
	 * Device Name characteristic. Make the value dynamically read and
	 * written via callbacks.
	 */
	bt_uuid16_create(&uuid, GATT_CHARAC_DEVICE_NAME);
	gatt_db_service_add_characteristic(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_EXT_PROP,
					gap_device_name_read_cb,
					gap_device_name_write_cb,
					server);

	bt_uuid16_create(&uuid, GATT_CHARAC_EXT_PROPER_UUID);
	gatt_db_service_add_descriptor(service, &uuid, BT_ATT_PERM_READ,
					gap_device_name_ext_prop_read_cb,
					NULL, server);

	/*
	 * Appearance characteristic. Reads and writes should obtain the value
	 * from the database.
	 */
	bt_uuid16_create(&uuid, GATT_CHARAC_APPEARANCE);
	tmp = gatt_db_service_add_characteristic(service, &uuid,
							BT_ATT_PERM_READ,
							BT_GATT_CHRC_PROP_READ,
							NULL, NULL, server);

	/*
	 * Write the appearance value to the database, since we're not using a
	 * callback.
	 */
	put_le16(128, &appearance);
	gatt_db_attribute_write(tmp, 0, (void *) &appearance,
							sizeof(appearance),
							BT_ATT_OP_WRITE_REQ,
							NULL, confirm_write,
							NULL);

	gatt_db_service_set_active(service, true);
}

static void populate_gatt_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *svc_chngd;

	/* Add the GATT service */
	bt_uuid16_create(&uuid, UUID_GATT);
	service = gatt_db_add_service(server->db, &uuid, true, 4);

	bt_uuid16_create(&uuid, GATT_CHARAC_SERVICE_CHANGED);
	svc_chngd = gatt_db_service_add_characteristic(service, &uuid,
			BT_ATT_PERM_READ,
			BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_INDICATE,
			gatt_service_changed_cb,
			NULL, server);
	server->gatt_svc_chngd_handle = gatt_db_attribute_get_handle(svc_chngd);

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				gatt_svc_chngd_ccc_read_cb,
				gatt_svc_chngd_ccc_write_cb, server);

	gatt_db_service_set_active(service, true);
}

static void populate_hr_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *hr_msrmt, *body;
	uint8_t body_loc = 1;  /* "Chest" */

	/* Add Heart Rate Service */
	bt_uuid16_create(&uuid, UUID_HEART_RATE);
	service = gatt_db_add_service(server->db, &uuid, true, 8);
	server->hr_handle = gatt_db_attribute_get_handle(service);

	/* HR Measurement Characteristic */
	bt_uuid16_create(&uuid, UUID_HEART_RATE_MSRMT);
	hr_msrmt = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_NONE,
						BT_GATT_CHRC_PROP_NOTIFY,
						NULL, NULL, NULL);
	server->hr_msrmt_handle = gatt_db_attribute_get_handle(hr_msrmt);

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					hr_msrmt_ccc_read_cb,
					hr_msrmt_ccc_write_cb, server);

	/*
	 * Body Sensor Location Characteristic. Make reads obtain the value from
	 * the database.
	 */
	bt_uuid16_create(&uuid, UUID_HEART_RATE_BODY);
	body = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_READ,
						BT_GATT_CHRC_PROP_READ,
						NULL, NULL, server);
	gatt_db_attribute_write(body, 0, (void *) &body_loc, sizeof(body_loc),
							BT_ATT_OP_WRITE_REQ,
							NULL, confirm_write,
							NULL);

	/* HR Control Point Characteristic */
	bt_uuid16_create(&uuid, UUID_HEART_RATE_CTRL);
	gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_WRITE,
						BT_GATT_CHRC_PROP_WRITE,
						NULL, hr_control_point_write_cb,
						server);

	if (server->hr_visible)
		gatt_db_service_set_active(service, true);
}

static void populate_wifi_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *wifi_write, *wifi_notify;
	uint16_t decl_handle = 0, value_handle = 0;
	uint8_t props = 0;
	uint16_t ext_props = 0;
	bt_uuid_t char_uuid;

	printf("populate_wifi_service\n");

	/* Add Wifi Service */
	bt_uuid16_create(&uuid, UUID_WIFI);
	service = gatt_db_add_service(server->db, &uuid, true, 8);
	server->wifi_handle = gatt_db_attribute_get_handle(service);

	/* Add Wifi Write Characteristic (AE81) */
	bt_uuid16_create(&uuid, UUID_WIFI_WRITE_CHR);
	wifi_write = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_WRITE,
						BT_GATT_CHRC_PROP_WRITE |
						BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
						NULL, wifi_write_cb, server);
	if (gatt_db_attribute_get_char_data(wifi_write, &decl_handle, &value_handle,
						&props, &ext_props, &char_uuid)) {
		server->wifi_write_handle = value_handle;
	} else {
		server->wifi_write_handle = gatt_db_attribute_get_handle(wifi_write);
	}

	/* Add Wifi Notify Characteristic (AE82) */
	bt_uuid16_create(&uuid, UUID_WIFI_NOTIFY_CHR);
	wifi_notify = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_READ,
						BT_GATT_CHRC_PROP_NOTIFY,
						wifi_read_cb, NULL, server);
	if (gatt_db_attribute_get_char_data(wifi_notify, &decl_handle, &value_handle,
						&props, &ext_props, &char_uuid)) {
		server->wifi_notify_handle = value_handle;
	} else {
		server->wifi_notify_handle = gatt_db_attribute_get_handle(wifi_notify);
	}

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					wifi_chngd_ccc_read_cb,
					wifi_chngd_ccc_write_cb, server);
	printf("wifi service handles: service=0x%04x write=0x%04x notify=0x%04x\n",
		server->wifi_handle, server->wifi_write_handle, server->wifi_notify_handle);

	gatt_db_service_set_active(service, true);
}

static void populate_db(struct server *server)
{
	populate_gap_service(server);
	populate_gatt_service(server);
	populate_wifi_service(server);
}

static const char *resolve_gap_device_name(void)
{
	const char *name = getenv("ZH_BLE_DEVICE_NAME");

	if (!name || name[0] == '\0')
		return default_device_name;

	return name;
}

static struct server *server_create(int fd, uint16_t mtu, bool hr_visible)
{
	struct server *server;
	const char *gap_device_name = resolve_gap_device_name();
	size_t name_len = strlen(gap_device_name);

	server = new0(struct server, 1);
	if (!server) {
		fprintf(stderr, "Failed to allocate memory for server\n");
		return NULL;
	}

	server->att = bt_att_new(fd, false);
	if (!server->att) {
		fprintf(stderr, "Failed to initialze ATT transport layer\n");
		goto fail;
	}

	if (!bt_att_set_close_on_unref(server->att, true)) {
		fprintf(stderr, "Failed to set up ATT transport layer\n");
		goto fail;
	}

	if (!bt_att_register_disconnect(server->att, att_disconnect_cb, NULL,
									NULL)) {
		fprintf(stderr, "Failed to set ATT disconnect handler\n");
		goto fail;
	}

	server->name_len = name_len + 1;
	server->device_name = malloc(name_len + 1);
	if (!server->device_name) {
		fprintf(stderr, "Failed to allocate memory for device name\n");
		goto fail;
	}

	memcpy(server->device_name, gap_device_name, name_len);
	server->device_name[name_len] = '\0';
	LOGI(BLE_LOG_TAG, "using GAP device name: %s", gap_device_name);

	server->fd = fd;
	server->db = gatt_db_new();
	if (!server->db) {
		fprintf(stderr, "Failed to create GATT database\n");
		goto fail;
	}

	server->gatt = bt_gatt_server_new(server->db, server->att, mtu, 0);
	if (!server->gatt) {
		fprintf(stderr, "Failed to create GATT server\n");
		goto fail;
	}
	printf("GATT negotiated mtu=%u\n", bt_gatt_server_get_mtu(server->gatt));
	/* 主动请求更大 MTU，避免 23 下业务帧超长导致小程序无法解析。 */
	bt_gatt_exchange_mtu(server->att, 247, mtu_exchange_cb, server, NULL);

	server->hr_visible = hr_visible;

	if (verbose) {
		bt_att_set_debug(server->att, BT_ATT_DEBUG_VERBOSE,
						att_debug_cb, "att: ", NULL);
		bt_gatt_server_set_debug(server->gatt, gatt_debug_cb,
							"server: ", NULL);
	}

	/* Random seed for generating fake Heart Rate measurements */
	srand(time(NULL));

	/* bt_gatt_server already holds a reference */
	populate_db(server);

	return server;

fail:
	gatt_db_unref(server->db);
	free(server->device_name);
	bt_att_unref(server->att);
	free(server);

	return NULL;
}

static void server_destroy(struct server *server)
{
	timeout_remove(server->hr_timeout_id);
	bt_gatt_server_unref(server->gatt);
	gatt_db_unref(server->db);
}

static void usage(void)
{
	printf("btgatt-server\n");
	printf("Usage:\n\tbtgatt-server [options]\n");

	printf("Options:\n"
		"\t-i, --index <id>\t\tSpecify adapter index, e.g. hci0\n"
		"\t-m, --mtu <mtu>\t\t\tThe ATT MTU to use\n"
		"\t-s, --security-level <sec>\tSet security level (low|"
								"medium|high)\n"
		"\t-t, --type [random|public] \t The source address type\n"
		"\t-v, --verbose\t\t\tEnable extra logging\n"
		"\t-r, --heart-rate\t\tEnable Heart Rate service\n"
		"\t-h, --help\t\t\tDisplay help\n");
}

static struct option main_options[] = {
	{ "index",		1, 0, 'i' },
	{ "mtu",		1, 0, 'm' },
	{ "security-level",	1, 0, 's' },
	{ "type",		1, 0, 't' },
	{ "verbose",		0, 0, 'v' },
	{ "heart-rate",		0, 0, 'r' },
	{ "help",		0, 0, 'h' },
	{ }
};

static int l2cap_le_att_listen_and_accept(bdaddr_t *src, int sec,
							uint8_t src_type)
{
	int sk, nsk;
	struct sockaddr_l2 srcaddr, addr;
	socklen_t optlen;
	struct bt_security btsec;
	char ba[18];

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	/* Set up source address */
	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = src_type;
	bacpy(&srcaddr.l2_bdaddr, src);

	if (bind(sk, (struct sockaddr *) &srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		goto fail;
	}

	/* Set the security level */
	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		fprintf(stderr, "Failed to set L2CAP security level\n");
		goto fail;
	}

	if (listen(sk, 10) < 0) {
		perror("Listening on socket failed");
		goto fail;
	}

	printf("Started listening on ATT channel. Waiting for connections\n");

	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);
	nsk = accept(sk, (struct sockaddr *) &addr, &optlen);
	if (nsk < 0) {
		perror("Accept failed");
		goto fail;
	}

	ba2str(&addr.l2_bdaddr, ba);
	printf("Connect from %s\n", ba);
	close(sk);

	return nsk;

fail:
	close(sk);
	return -1;
}

static void notify_usage(void)
{
	printf("Usage: notify [options] <value_handle> <value>\n"
					"Options:\n"
					"\t -i, --indicate\tSend indication\n"
					"e.g.:\n"
					"\tnotify 0x0001 00 01 00\n");
}

static struct option notify_options[] = {
	{ "indicate",	0, 0, 'i' },
	{ }
};

static bool parse_args(char *str, int expected_argc,  char **argv, int *argc)
{
	char **ap;

	for (ap = argv; (*ap = strsep(&str, " \t")) != NULL;) {
		if (**ap == '\0')
			continue;

		(*argc)++;
		ap++;

		if (*argc > expected_argc)
			return false;
	}

	return true;
}

static void conf_cb(void *user_data)
{
	PRLOG("Received confirmation\n");
}

static void cmd_notify(struct server *server, char *cmd_str)
{
	int opt, i;
	char *argvbuf[516];
	char **argv = argvbuf;
	int argc = 1;
	uint16_t handle;
	char *endptr = NULL;
	int length;
	uint8_t *value = NULL;
	bool indicate = false;

	if (!parse_args(cmd_str, 514, argv + 1, &argc)) {
		printf("Too many arguments\n");
		notify_usage();
		return;
	}

	optind = 0;
	argv[0] = "notify";
	while ((opt = getopt_long(argc, argv, "+i", notify_options,
								NULL)) != -1) {
		switch (opt) {
		case 'i':
			indicate = true;
			break;
		default:
			notify_usage();
			return;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		notify_usage();
		return;
	}

	handle = strtol(argv[0], &endptr, 16);
	if (!endptr || *endptr != '\0' || !handle) {
		printf("Invalid handle: %s\n", argv[0]);
		return;
	}

	length = argc - 1;

	if (length > 0) {
		if (length > UINT16_MAX) {
			printf("Value too long\n");
			return;
		}

		value = malloc(length);
		if (!value) {
			printf("Failed to construct value\n");
			return;
		}

		for (i = 1; i < argc; i++) {
			if (strlen(argv[i]) != 2) {
				printf("Invalid value byte: %s\n",
								argv[i]);
				goto done;
			}

			value[i-1] = strtol(argv[i], &endptr, 16);
			if (endptr == argv[i] || *endptr != '\0'
							|| errno == ERANGE) {
				printf("Invalid value byte: %s\n",
								argv[i]);
				goto done;
			}
		}
	}

	printf("notify: %x:%x\n", server->wifi_handle, server->wifi_notify_handle);

	if (indicate) {
		if (!bt_gatt_server_send_indication(server->gatt, handle,
							value, length,
							conf_cb, NULL, NULL))
			printf("Failed to initiate indication\n");
	} else if (!bt_gatt_server_send_notification(server->gatt, handle,
							value, length, false))
		printf("Failed to initiate notification\n");

done:
	free(value);
}

static void heart_rate_usage(void)
{
	printf("Usage: heart-rate on|off\n");
}

static void cmd_heart_rate(struct server *server, char *cmd_str)
{
	bool enable;
	uint8_t pdu[4];
	struct gatt_db_attribute *attr;

	if (!cmd_str) {
		heart_rate_usage();
		return;
	}

	if (strcmp(cmd_str, "on") == 0)
		enable = true;
	else if (strcmp(cmd_str, "off") == 0)
		enable = false;
	else {
		heart_rate_usage();
		return;
	}

	if (enable == server->hr_visible) {
		printf("Heart Rate Service already %s\n",
						enable ? "visible" : "hidden");
		return;
	}

	server->hr_visible = enable;
	attr = gatt_db_get_attribute(server->db, server->hr_handle);
	gatt_db_service_set_active(attr, server->hr_visible);
	update_hr_msrmt_simulation(server);

	if (!server->svc_chngd_enabled)
		return;

	put_le16(server->hr_handle, pdu);
	put_le16(server->hr_handle + 7, pdu + 2);

	server->hr_msrmt_enabled = false;
	update_hr_msrmt_simulation(server);

	bt_gatt_server_send_indication(server->gatt,
						server->gatt_svc_chngd_handle,
						pdu, 4, conf_cb, NULL, NULL);
}

static void print_uuid(const bt_uuid_t *uuid)
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t uuid128;

	bt_uuid_to_uuid128(uuid, &uuid128);
	bt_uuid_to_string(&uuid128, uuid_str, sizeof(uuid_str));

	printf("%s\n", uuid_str);
}

static void print_incl(struct gatt_db_attribute *attr, void *user_data)
{
	struct server *server = user_data;
	uint16_t handle, start, end;
	struct gatt_db_attribute *service;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_incl_data(attr, &handle, &start, &end))
		return;

	service = gatt_db_get_attribute(server->db, start);
	if (!service)
		return;

	gatt_db_attribute_get_service_uuid(service, &uuid);

	printf("\t  " COLOR_GREEN "include" COLOR_OFF " - handle: "
					"0x%04x, - start: 0x%04x, end: 0x%04x,"
					"uuid: ", handle, start, end);
	print_uuid(&uuid);
}

static void print_desc(struct gatt_db_attribute *attr, void *user_data)
{
	printf("\t\t  " COLOR_MAGENTA "descr" COLOR_OFF
					" - handle: 0x%04x, uuid: ",
					gatt_db_attribute_get_handle(attr));
	print_uuid(gatt_db_attribute_get_type(attr));
}

static void print_chrc(struct gatt_db_attribute *attr, void *user_data)
{
	uint16_t handle, value_handle;
	uint8_t properties;
	uint16_t ext_prop;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, &handle,
								&value_handle,
								&properties,
								&ext_prop,
								&uuid))
		return;

	printf("\t  " COLOR_YELLOW "charac" COLOR_OFF
				" - start: 0x%04x, value: 0x%04x, "
				"props: 0x%02x, ext_prop: 0x%04x, uuid: ",
				handle, value_handle, properties, ext_prop);
	print_uuid(&uuid);

	gatt_db_service_foreach_desc(attr, print_desc, NULL);
}

static void print_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct server *server = user_data;
	uint16_t start, end;
	bool primary;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
									&uuid))
		return;

	printf(COLOR_RED "service" COLOR_OFF " - start: 0x%04x, "
				"end: 0x%04x, type: %s, uuid: ",
				start, end, primary ? "primary" : "secondary");
	print_uuid(&uuid);

	gatt_db_service_foreach_incl(attr, print_incl, server);
	gatt_db_service_foreach_char(attr, print_chrc, NULL);

	printf("\n");
}

static void cmd_services(struct server *server, char *cmd_str)
{
	gatt_db_foreach_service(server->db, NULL, print_service, server);
}

static bool convert_sign_key(char *optarg, uint8_t key[16])
{
	int i;

	if (strlen(optarg) != 32) {
		printf("sign-key length is invalid\n");
		return false;
	}

	for (i = 0; i < 16; i++) {
		if (sscanf(optarg + (i * 2), "%2hhx", &key[i]) != 1)
			return false;
	}

	return true;
}

static void set_sign_key_usage(void)
{
	printf("Usage: set-sign-key [options]\nOptions:\n"
		"\t -c, --sign-key <remote csrk>\tRemote CSRK\n"
		"e.g.:\n"
		"\tset-sign-key -c D8515948451FEA320DC05A2E88308188\n");
}

static bool remote_counter(uint32_t *sign_cnt, void *user_data)
{
	static uint32_t cnt = 0;

	if (*sign_cnt < cnt)
		return false;

	cnt = *sign_cnt;

	return true;
}

static void cmd_set_sign_key(struct server *server, char *cmd_str)
{
	char *argv[3];
	int argc = 0;
	uint8_t key[16];

	memset(key, 0, 16);

	if (!parse_args(cmd_str, 2, argv, &argc)) {
		set_sign_key_usage();
		return;
	}

	if (argc != 2) {
		set_sign_key_usage();
		return;
	}

	if (!strcmp(argv[0], "-c") || !strcmp(argv[0], "--sign-key")) {
		if (convert_sign_key(argv[1], key))
			bt_att_set_remote_key(server->att, key, remote_counter,
									server);
	} else
		set_sign_key_usage();
}

static void cmd_help(struct server *server, char *cmd_str);

typedef void (*command_func_t)(struct server *server, char *cmd_str);

static struct {
	char *cmd;
	command_func_t func;
	char *doc;
} command[] = {
	{ "help", cmd_help, "\tDisplay help message" },
	{ "notify", cmd_notify, "\tSend handle-value notification" },
	{ "heart-rate", cmd_heart_rate, "\tHide/Unhide Heart Rate Service" },
	{ "services", cmd_services, "\tEnumerate all services" },
	{ "set-sign-key", cmd_set_sign_key,
			"\tSet remote signing key for signed write command"},
	{ }
};

static void cmd_help(struct server *server, char *cmd_str)
{
	int i;

	printf("Commands:\n");
	for (i = 0; command[i].cmd; i++)
		printf("\t%-15s\t%s\n", command[i].cmd, command[i].doc);
}

static void prompt_read_cb(int fd, uint32_t events, void *user_data)
{
	ssize_t read;
	size_t len = 0;
	char *line = NULL;
	char *cmd = NULL, *args;
	struct server *server = user_data;
	int i;

	if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
		mainloop_quit();
		return;
	}

	read = getline(&line, &len, stdin);
	if (read < 0) {
		free(line);
		return;
	}

	if (read <= 1) {
		cmd_help(server, NULL);
		print_prompt();
		free(line);
		return;
	}

	line[read-1] = '\0';
	args = line;

	while ((cmd = strsep(&args, " \t")))
		if (*cmd != '\0')
			break;

	if (!cmd)
		goto failed;

	for (i = 0; command[i].cmd; i++) {
		if (strcmp(command[i].cmd, cmd) == 0)
			break;
	}

	if (command[i].cmd)
		command[i].func(server, args);
	else
		fprintf(stderr, "Unknown command: %s\n", line);

failed:
	print_prompt();

	free(line);
}

static void signal_cb(int signum, void *user_data)
{
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		g_stop_requested = 1;
		mainloop_quit();
		break;
	default:
		break;
	}
}

int main(int argc, char *argv[])
{
	int opt;
	bdaddr_t src_addr;
	int dev_id = -1;
	int sec = BT_SECURITY_LOW;
	uint8_t src_type = BDADDR_LE_PUBLIC;
	uint16_t mtu = 0;
	bool hr_visible = false;
	struct server *server;

	while ((opt = getopt_long(argc, argv, "+hvrs:t:m:i:",
						main_options, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'v':
			verbose = true;
			break;
		case 'r':
			hr_visible = true;
			break;
		case 's':
			if (strcmp(optarg, "low") == 0)
				sec = BT_SECURITY_LOW;
			else if (strcmp(optarg, "medium") == 0)
				sec = BT_SECURITY_MEDIUM;
			else if (strcmp(optarg, "high") == 0)
				sec = BT_SECURITY_HIGH;
			else {
				fprintf(stderr, "Invalid security level\n");
				return EXIT_FAILURE;
			}
			break;
		case 't':
			if (strcmp(optarg, "random") == 0)
				src_type = BDADDR_LE_RANDOM;
			else if (strcmp(optarg, "public") == 0)
				src_type = BDADDR_LE_PUBLIC;
			else {
				fprintf(stderr,
					"Allowed types: random, public\n");
				return EXIT_FAILURE;
			}
			break;
		case 'm': {
			int arg;

			arg = atoi(optarg);
			if (arg <= 0) {
				fprintf(stderr, "Invalid MTU: %d\n", arg);
				return EXIT_FAILURE;
			}

			if (arg > UINT16_MAX) {
				fprintf(stderr, "MTU too large: %d\n", arg);
				return EXIT_FAILURE;
			}

			mtu = (uint16_t) arg;
			break;
		}
		case 'i':
			dev_id = hci_devid(optarg);
			if (dev_id < 0) {
				perror("Invalid adapter");
				return EXIT_FAILURE;
			}

			break;
		default:
			fprintf(stderr, "Invalid option: %c\n", opt);
			return EXIT_FAILURE;
		}
	}

	argc -= optind;
	argv -= optind;
	optind = 0;

	if (argc) {
		usage();
		return EXIT_SUCCESS;
	}

	if (dev_id == -1)
		bacpy(&src_addr, BDADDR_ANY);
	else if (hci_devba(dev_id, &src_addr) < 0) {
		perror("Adapter not available");
		return EXIT_FAILURE;
	}

	while (!g_stop_requested) {
		int fd = l2cap_le_att_listen_and_accept(&src_addr, sec, src_type);
		if (fd < 0) {
			if (g_stop_requested)
				break;
			fprintf(stderr, "Failed to accept L2CAP ATT connection, retry\n");
			sleep(1);
			continue;
		}

		mainloop_init();
		server = server_create(fd, mtu, hr_visible);
		if (!server) {
			close(fd);
			if (g_stop_requested)
				break;
			sleep(1);
			continue;
		}

		printf("Running GATT server session\n");
		mainloop_run_with_signal(signal_cb, NULL);
		printf("GATT session ended, wait for next connection\n");
		server_destroy(server);
		if (g_provision_done)
			break;
	}

	if (g_provision_done)
		return EXIT_SUCCESS;
	printf("\n\nShutting down...\n");

	return EXIT_SUCCESS;
}
