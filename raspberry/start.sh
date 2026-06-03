#!/bin/bash
#
# Bring the Mementum LED access point + server up now (without rebooting).
# Starts the services in the order the ESP32 boot does: radio/AP first, then DHCP,
# then the control server that binds 10.10.10.1:80.
#
# Usage:  sudo ./raspberry/start.sh

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo." >&2
    exit 1
fi

echo "==> Bringing wlan0 to the static AP address..."
rfkill unblock wlan || true
systemctl restart dhcpcd || true
sleep 2

echo "==> Starting access point (hostapd)..."
systemctl restart hostapd
sleep 2

echo "==> Starting DHCP/DNS (dnsmasq)..."
systemctl restart dnsmasq

echo "==> Starting control server..."
systemctl restart mementum-server.service

echo ""
echo "Up. Clients should associate to SSID 'mementumLED' and reach http://10.10.10.1"
echo "Status:  sudo ./raspberry/status.sh"
