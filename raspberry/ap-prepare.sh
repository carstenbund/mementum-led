#!/bin/bash
#
# Release wlan0 from any client-mode network manager and put it into a clean
# state so hostapd can run it as an access point and dnsmasq can hand out leases.
#
# This is the step that makes the AP actually come up on a Pi that already had
# Wi-Fi "configured" (joined to a network). If wpa_supplicant or NetworkManager
# keeps holding wlan0 in station mode, hostapd cannot switch the radio to AP mode
# and clients never get DHCP offers.
#
# Safe and idempotent: only touches wlan0. eth0 / any wired admin link is left
# completely alone, and NetworkManager is NOT stopped globally (it keeps managing
# every other interface) -- we only tell it to leave wlan0 alone.
#
# Installed to /usr/local/sbin/mementum-ap-prepare by install.sh and run as an
# ExecStartPre of hostapd, so wlan0 is released on every AP start.
#
# Usage:  sudo mementum-ap-prepare

set -uo pipefail

IFACE="${MEMENTUM_AP_IFACE:-wlan0}"
AP_IP="${MEMENTUM_AP_IP:-10.10.10.1/24}"

log() { echo "[ap-prepare] $*"; }

# Make sure the radio is not soft/hard blocked.
rfkill unblock wlan 2>/dev/null || true
rfkill unblock wifi 2>/dev/null || true

# 1) NetworkManager (Raspberry Pi OS Bookworm default): stop managing wlan0.
#    'managed no' is the runtime release; the persistent unmanaged-devices file
#    installed at /etc/NetworkManager/conf.d/ keeps it released across reboots.
if command -v nmcli >/dev/null 2>&1 && systemctl is-active --quiet NetworkManager; then
    log "Releasing $IFACE from NetworkManager"
    nmcli dev set "$IFACE" managed no 2>/dev/null || true
    nmcli dev disconnect "$IFACE" 2>/dev/null || true
fi

# 2) wpa_supplicant client on wlan0 (Bullseye/older default, or a leftover):
#    it holds the radio in station mode, blocking AP mode. Stop the per-interface
#    unit and any stray process bound to this interface.
systemctl stop "wpa_supplicant@${IFACE}.service" 2>/dev/null || true
pkill -f "wpa_supplicant.*-i[ =]*${IFACE}\b" 2>/dev/null || true

# 3) Drop any client-mode addresses/routes left on the interface, then set the
#    static AP address. Doing this here (instead of relying only on dhcpcd) makes
#    it work regardless of whether the image uses dhcpcd, NetworkManager, or
#    systemd-networkd for IP configuration.
if ip link show "$IFACE" >/dev/null 2>&1; then
    log "Resetting $IFACE and assigning static AP address $AP_IP"
    ip link set "$IFACE" down 2>/dev/null || true
    ip addr flush dev "$IFACE" 2>/dev/null || true
    ip link set "$IFACE" up 2>/dev/null || true
    # '|| true': if the address is already present (e.g. dhcpcd also set it) this
    # is a harmless no-op rather than a failure.
    ip addr add "$AP_IP" dev "$IFACE" 2>/dev/null || true
else
    log "WARNING: interface $IFACE not found (is the Wi-Fi adapter present?)"
fi

have_ip="$(ip -4 addr show "$IFACE" 2>/dev/null | grep -oP 'inet \K[0-9.]+/[0-9]+' | head -n1)"
log "$IFACE ready${have_ip:+ ($have_ip)}"

# Never fail the dependent unit: hostapd should still try to start even if one of
# the release steps was a no-op on this particular image.
exit 0
