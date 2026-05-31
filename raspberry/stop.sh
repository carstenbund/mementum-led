#!/bin/bash
#
# Stop the Mementum LED server and access point.
#
# Usage:  sudo ./raspberry/stop.sh

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo." >&2
    exit 1
fi

systemctl stop mementum-server.service || true
systemctl stop dnsmasq || true
systemctl stop hostapd || true

echo "Stopped server, dnsmasq, and hostapd."
