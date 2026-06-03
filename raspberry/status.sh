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
echo "wlan0 network ownership (must be free of any client manager for AP mode):"
# Which radio mode is wlan0 actually in? 'AP' is what we want; 'managed'/station
# means something still holds it as a Wi-Fi client and hostapd could not take over.
mode=$(iw dev wlan0 info 2>/dev/null | grep -oP 'type \K\w+' || echo "unknown")
printf "  %-26s %s\n" "radio mode" "$mode"

if command -v nmcli >/dev/null 2>&1 && systemctl is-active --quiet NetworkManager; then
    nm_state=$(nmcli -t -f DEVICE,STATE dev status 2>/dev/null \
        | grep '^wlan0:' | cut -d: -f2 || true)
    printf "  %-26s %s\n" "NetworkManager (Bookworm)" "active, wlan0=${nm_state:-unknown (good if 'unmanaged')}"
else
    printf "  %-26s %s\n" "NetworkManager" "not active"
fi

wpa_state=$(systemctl is-active wpa_supplicant@wlan0.service 2>/dev/null || true)
printf "  %-26s %s\n" "wpa_supplicant@wlan0" "${wpa_state} (should be inactive/disabled)"

echo ""
echo "wlan0 address:"
ip -4 addr show wlan0 2>/dev/null | grep -oP 'inet \K[0-9.]+/[0-9]+' || echo "  (no IPv4 on wlan0)"

echo ""
echo "Server queue (/getData):"
curl -s --max-time 3 http://10.10.10.1/getData || echo "  (server not reachable)"
echo ""
