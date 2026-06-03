#!/bin/bash
#
# Bring the Mementum LED access point + server up now (without rebooting).
# Starts the services in the order the ESP32 boot does: radio/AP first, then DHCP,
# then the control server that binds 10.10.10.1:80.
#
# IMPORTANT: switching wlan0 into AP mode drops any Wi-Fi *client* connection on
# wlan0 -- including an SSH session you opened over the Pi's Wi-Fi. To keep the
# bring-up from dying with that session, this script re-launches itself detached
# (its own session, output to a log) so the full sequence completes even after
# your link drops. Reconnect to SSID 'mementumLED' and SSH to 10.10.10.1.
#
# Usage:  sudo ./raspberry/start.sh

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo." >&2
    exit 1
fi

LOG=/var/log/mementum-start.log

# First invocation: detach and return, so closing/losing this shell (or the wlan0
# SSH session) cannot interrupt the bring-up. The detached copy runs the body.
if [[ "${MEMENTUM_DETACHED:-}" != "1" ]]; then
    echo "==> Starting AP + server in the background."
    echo "    If you are connected over the Pi's Wi-Fi (wlan0), this session will"
    echo "    drop when the AP comes up. Reconnect to SSID 'mementumLED' and"
    echo "    SSH to 10.10.10.1.  Progress log: $LOG"
    MEMENTUM_DETACHED=1 setsid "$0" "$@" </dev/null >>"$LOG" 2>&1 &
    exit 0
fi

echo "===== $(date '+%F %T') bringing up Mementum LED AP ====="

echo "==> Bringing wlan0 to the static AP address..."
rfkill unblock wlan || true
systemctl restart dhcpcd 2>/dev/null || true
sleep 2

# hostapd's ExecStartPre (mementum-ap-prepare) releases wlan0 from NetworkManager /
# wpa_supplicant and assigns the static AP IP before the radio switches to AP mode.
echo "==> Starting access point (hostapd)..."
systemctl restart hostapd
sleep 2

echo "==> Starting DHCP/DNS (dnsmasq)..."
systemctl restart dnsmasq

echo "==> Starting control server..."
systemctl restart mementum-server.service

echo "Up. Clients should associate to SSID 'mementumLED' and reach http://10.10.10.1"
