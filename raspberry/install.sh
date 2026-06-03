#!/bin/bash
#
# Install the Mementum LED server on a Raspberry Pi as a dedicated, self-booting
# access point + control server that fully replicates the ESP32 AP-mode server.
#
# What it does:
#   - installs hostapd + dnsmasq and the AP/DHCP config (SSID/PSK/IP that the
#     ESP32 clients expect: mementumLED / mementumLED on 10.10.10.1, channel 1),
#   - gives wlan0 a static 10.10.10.1,
#   - creates a Python venv and installs Flask + requests (server.py deps),
#   - installs a systemd service so the Pi boots straight into the server.
#
# Re-runnable: safe to run again to update config or the service.
#
# Usage:  sudo ./raspberry/install.sh

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run this script with sudo." >&2
    exit 1
fi

# Resolve paths. REPO_DIR is the repo root (parent of this raspberry/ folder),
# which is the WorkingDirectory for server.py (it serves data/index.html).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"

echo "==> Repo dir:  $REPO_DIR"
echo "==> Config:    SSID=mementumLED  IP=10.10.10.1  channel=1"

echo "==> Installing packages (hostapd, dnsmasq, python venv)..."
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y hostapd dnsmasq python3 python3-venv python3-pip

# hostapd and dnsmasq ship masked/auto-started; manage them ourselves.
systemctl unmask hostapd || true
systemctl stop hostapd || true
systemctl stop dnsmasq || true

echo "==> Installing AP configuration..."
install -m 0644 "$SCRIPT_DIR/config/hostapd.conf" /etc/hostapd/hostapd.conf
# Point the hostapd daemon at our config.
if grep -q '^#\?DAEMON_CONF=' /etc/default/hostapd; then
    sed -i 's|^#\?DAEMON_CONF=.*|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' /etc/default/hostapd
else
    echo 'DAEMON_CONF="/etc/hostapd/hostapd.conf"' >> /etc/default/hostapd
fi

install -d /etc/dnsmasq.d
install -m 0644 "$SCRIPT_DIR/config/dnsmasq.conf" /etc/dnsmasq.d/mementum.conf

echo "==> Releasing wlan0 from the existing network configuration..."
# This is what lets the AP actually come up on a Pi that is already joined to a
# Wi-Fi network. Both common Raspberry Pi OS stacks are handled:
#   - NetworkManager (Bookworm): told to leave wlan0 unmanaged, permanently.
#   - wpa_supplicant client (Bullseye/older): the per-interface unit is disabled.
# Only wlan0 is affected; eth0 / wired admin stays managed.
install -m 0755 "$SCRIPT_DIR/ap-prepare.sh" /usr/local/sbin/mementum-ap-prepare

install -d /etc/NetworkManager/conf.d
install -m 0644 "$SCRIPT_DIR/config/networkmanager-unmanaged.conf" \
    /etc/NetworkManager/conf.d/99-mementum-unmanaged.conf
if systemctl is-active --quiet NetworkManager; then
    systemctl reload NetworkManager || true
fi

# Stop wlan0 ever being brought up as a Wi-Fi client by wpa_supplicant.
systemctl disable --now wpa_supplicant@wlan0.service 2>/dev/null || true

# Run the release+static-IP step right before hostapd, and gate dnsmasq on the AP
# being up, via systemd drop-ins (kept across package upgrades).
install -d /etc/systemd/system/hostapd.service.d
install -m 0644 "$SCRIPT_DIR/systemd/hostapd.dropin.conf" \
    /etc/systemd/system/hostapd.service.d/10-mementum.conf
install -d /etc/systemd/system/dnsmasq.service.d
install -m 0644 "$SCRIPT_DIR/systemd/dnsmasq.dropin.conf" \
    /etc/systemd/system/dnsmasq.service.d/10-mementum.conf

echo "==> Configuring static IP for wlan0..."
# Append our wlan0 stanza to dhcpcd.conf once (idempotent).
if ! grep -q "# Mementum LED AP" /etc/dhcpcd.conf 2>/dev/null; then
    {
        echo ""
        echo "# Mementum LED AP"
        cat "$SCRIPT_DIR/config/dhcpcd.append.conf"
    } >> /etc/dhcpcd.conf
fi

echo "==> Unblocking wifi radio..."
rfkill unblock wlan || true

echo "==> Creating Python venv and installing server dependencies..."
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$SCRIPT_DIR/requirements.txt"

echo "==> Installing systemd service..."
# Fill the template placeholders with the resolved repo dir and venv python.
sed -e "s|__INSTALL_DIR__|$REPO_DIR|g" \
    -e "s|__PYTHON__|$VENV_DIR/bin/python|g" \
    "$SCRIPT_DIR/systemd/mementum-server.service" \
    > /etc/systemd/system/mementum-server.service

systemctl daemon-reload
systemctl unmask hostapd || true
systemctl enable hostapd dnsmasq mementum-server.service

echo ""
echo "Install complete."
echo "  SSID:     mementumLED"
echo "  Password: mementumLED"
echo "  Server:   http://10.10.10.1  (admin UI on port 80)"
echo ""
echo "Start now without rebooting:   sudo $SCRIPT_DIR/start.sh"
echo "Or reboot to come up as the AP server:   sudo reboot"
