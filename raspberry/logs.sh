#!/bin/bash
#
# Tail the live logs for the whole Mementum LED stack: the control server (registration,
# /play scheduling, broadcast results), the access point (hostapd), and DHCP (dnsmasq).
#
# Use this to watch what clients are doing in real time -- e.g. confirm each reflashed
# client registers as version 1.3 and that PLAY lines are broadcast OK.
#
# Usage:
#   sudo ./raspberry/logs.sh           # follow all three services live
#   sudo ./raspberry/logs.sh server    # just the control server
#   sudo ./raspberry/logs.sh ap        # just hostapd + dnsmasq

set -uo pipefail

case "${1:-all}" in
    server) units=(mementum-server.service) ;;
    ap)     units=(hostapd.service dnsmasq.service) ;;
    all|*)  units=(mementum-server.service hostapd.service dnsmasq.service) ;;
esac

args=()
for u in "${units[@]}"; do args+=(-u "$u"); done

echo "==> Following: ${units[*]}  (Ctrl-C to stop)"
exec journalctl "${args[@]}" -f -o cat --no-hostname
