#!/bin/bash
#
# Show the state of the Mementum LED AP + server: service status, the AP address
# on wlan0, and the currently registered clients (queried from the server itself).
#
# Usage:  sudo ./raspberry/status.sh

set -uo pipefail

for svc in hostapd dnsmasq mementum-server.service; do
    state=$(systemctl is-active "$svc" 2>/dev/null || true)
    printf "  %-26s %s\n" "$svc" "$state"
done

echo ""
echo "wlan0 address:"
ip -4 addr show wlan0 2>/dev/null | grep -oP 'inet \K[0-9.]+/[0-9]+' || echo "  (no IPv4 on wlan0)"

echo ""
echo "Server queue (/getData):"
curl -s --max-time 3 http://10.10.10.1/getData || echo "  (server not reachable)"
echo ""
