#!/bin/sh

set -eu

IFACE="${ZH_BLE_IFACE:-wlan0}"
BLE_NAME="${1:-}"
BT_INIT_BIN="${ZH_BT_INIT_BIN:-/usr/bin/bt_init.sh}"
HCI_TOOL_BIN="${ZH_HCITOOL_BIN:-/usr/bin/hcitool}"
HCI_CONFIG_BIN="${ZH_HCICONFIG_BIN:-/usr/bin/hciconfig}"
BLE_GATT_SERVER_BIN="${ZH_BLE_GATT_SERVER_BIN:-/data/zh_work/zh_ble_gatt_server}"

if [ -z "$BLE_NAME" ]; then
    if [ -f "/sys/class/net/$IFACE/address" ]; then
        MAC_HEX="$(tr -d ':\n' < "/sys/class/net/$IFACE/address")"
        BLE_NAME="zh_${MAC_HEX}"
    else
        BLE_NAME="zh_unknown"
    fi
fi

if [ -z "${MAC_HEX:-}" ]; then
    if [ -f "/sys/class/net/$IFACE/address" ]; then
        MAC_HEX="$(tr -d ':\n' < "/sys/class/net/$IFACE/address")"
    else
        MAC_HEX="000000000000"
    fi
fi

case "$BLE_NAME" in
    JL*) ;;
    *) BLE_NAME="JL_${BLE_NAME}" ;;
esac

if [ ! -x "$BLE_GATT_SERVER_BIN" ]; then
    if [ -x "./build/zh_ble_gatt_server" ]; then
        BLE_GATT_SERVER_BIN="./build/zh_ble_gatt_server"
    fi
fi

if [ ! -x "$BLE_GATT_SERVER_BIN" ]; then
    BLE_GATT_SERVER_BIN="/usr/bin/btgatt_server"
fi

if [ ! -x "$BT_INIT_BIN" ] || [ ! -x "$HCI_TOOL_BIN" ] || [ ! -x "$HCI_CONFIG_BIN" ] || [ ! -x "$BLE_GATT_SERVER_BIN" ]; then
    echo "ble provision prerequisites missing"
    exit 1
fi

cleanup_stale_ble() {
    killall zh_ble_gatt_server >/dev/null 2>&1 || true
    killall btgatt_server >/dev/null 2>&1 || true
    "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x000a 0 >/dev/null 2>&1 || true
    "$HCI_CONFIG_BIN" hci0 down >/dev/null 2>&1 || true
    sleep 1
}

to_hex_bytes() {
    if command -v od >/dev/null 2>&1; then
        printf '%s' "$1" | od -An -tx1 -v | tr '\n' ' ' | sed 's/^ *//;s/  */ /g'
        return 0
    fi
    if command -v hexdump >/dev/null 2>&1; then
        printf '%s' "$1" | hexdump -v -e '/1 "%02x "' | sed 's/^ *//;s/  */ /g'
        return 0
    fi
    if command -v xxd >/dev/null 2>&1; then
        printf '%s' "$1" | xxd -p -c 256 | sed 's/../& /g;s/ $//'
        return 0
    fi
    return 1
}

cleanup() {
    "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x000a 0 >/dev/null 2>&1 || true
    if [ -n "${ADV_KEEPALIVE_PID:-}" ]; then
        kill "$ADV_KEEPALIVE_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

cleanup_stale_ble
"$BT_INIT_BIN"

wait_hci_ready() {
    i=0
    while [ "$i" -lt 40 ]; do
        if "$HCI_CONFIG_BIN" hci0 up >/dev/null 2>&1; then
            if "$HCI_TOOL_BIN" -i hci0 cmd 0x04 0x0001 >/dev/null 2>&1; then
                return 0
            fi
        fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

if ! wait_hci_ready; then
    echo "ble provision failed: hci0 not ready"
    exit 1
fi

run_hci_with_retry() {
    retry=0
    while [ "$retry" -lt 30 ]; do
        if "$@" >/dev/null 2>&1; then
            return 0
        fi
        retry=$((retry + 1))
        sleep 1
    done
    return 1
}

if ! NAME_BYTES="$(to_hex_bytes "$BLE_NAME")"; then
    echo "ble provision failed: no od/hexdump/xxd available"
    exit 1
fi
NAME_LEN=$(printf '%s' "$BLE_NAME" | wc -c | tr -d ' ')
NAME_FIELD_LEN=$(printf '%02X' $((NAME_LEN + 1)))
MANUF_BYTES="$(printf '%s' "$MAC_HEX" | sed 's/../& /g;s/ $//')"
ADV_LEN=$(printf '%02X' $((3 + 4 + 8)))
ADV_BYTES="02 01 06 03 03 00 AF 07 FF $MANUF_BYTES"
SCAN_RSP_LEN=$(printf '%02X' $((2 + NAME_LEN)))
SCAN_RSP_BYTES="$NAME_FIELD_LEN 09 $NAME_BYTES"

if ! run_hci_with_retry "$HCI_CONFIG_BIN" hci0 name "$BLE_NAME"; then
    echo "ble provision failed: set name failed"
    exit 1
fi
if ! run_hci_with_retry "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x0005 05 1b c1 72 52 5a; then
    echo "ble provision failed: set random addr failed"
    exit 1
fi
if ! run_hci_with_retry "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x0006 A0 00 A0 00 00 01 00 00 00 00 00 00 00 07 00; then
    echo "ble provision failed: set adv params failed"
    exit 1
fi
if ! run_hci_with_retry "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x0008 "$ADV_LEN" $ADV_BYTES; then
    echo "ble provision failed: set adv data failed"
    exit 1
fi
if ! run_hci_with_retry "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x0009 "$SCAN_RSP_LEN" $SCAN_RSP_BYTES; then
    echo "ble provision failed: set scan rsp failed"
    exit 1
fi
if ! run_hci_with_retry "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x000a 1; then
    echo "ble provision failed: enable adv failed"
    exit 1
fi

(
    while true; do
        "$HCI_TOOL_BIN" -i hci0 cmd 0x08 0x000a 1 >/dev/null 2>&1 || true
        sleep 2
    done
) &
ADV_KEEPALIVE_PID=$!

echo "ble provision started, name=$BLE_NAME, gatt_server=$BLE_GATT_SERVER_BIN, mtu=247"
ZH_BLE_DEVICE_NAME="$BLE_NAME" "$BLE_GATT_SERVER_BIN" -t public -r -m 247
rc=$?
cleanup
exit "$rc"
