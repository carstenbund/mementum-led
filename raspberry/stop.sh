#!/bin/bash
#
# Stop the Mementum LED server and access point.
#
# Pass --release-wlan0 to also hand wlan0 back to NetworkManager so the Pi can
# rejoin a normal Wi-Fi network for maintenance (runtime only; the AP reclaims
# wlan0 again on the next start/reboot).
#
# Usage:  sudo ./raspberry/stop.sh [--release-wlan0]

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo." >&2
    exit 1
fi

systemctl stop mementum-server.service || true
systemctl stop dnsmasq || true
systemctl stop hostapd || true

echo "Stopped server, dnsmasq, and hostapd."

if [[ "${1:-}" == "--release-wlan0" ]]; then
    if command -v nmcli >/dev/null 2>&1 && systemctl is-active --quiet NetworkManager; then
        echo "Handing wlan0 back to NetworkManager (until next AP start)..."
        ip addr flush dev wlan0 2>/dev/null || true
        nmcli dev set wlan0 managed yes 2>/dev/null || true
    else
        echo "NetworkManager not active; re-enable wpa_supplicant@wlan0 manually if needed."
    fi
fi
