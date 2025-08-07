#!/bin/bash
set -e

# Ensure script is run as root
if [[ $EUID -ne 0 ]]; then
   echo "Run this script with sudo." 
   exit 1
fi

echo "Updating packages..."
apt update && apt upgrade -y

echo "Installing hostapd and dnsmasq..."
apt install -y hostapd dnsmasq iptables-persistent netfilter-persistent

echo "Enabling services..."
systemctl unmask hostapd
systemctl enable hostapd
systemctl enable dnsmasq

echo "Configuring static IP for wlan0..."
cat > /etc/dhcpcd.conf <<EOF
interface wlan0
    static ip_address=10.10.10.1/24
    nohook wpa_supplicant
EOF

echo "Restarting dhcpcd..."
systemctl restart dhcpcd

echo "Setting up dnsmasq..."
mv /etc/dnsmasq.conf /etc/dnsmasq.conf.orig || true
cat > /etc/dnsmasq.conf <<EOF
interface=wlan0
dhcp-range=10.10.10.2,10.10.10.20,255.255.255.0,24h
EOF

echo "Creating hostapd.conf..."
cat > /etc/hostapd/hostapd.conf <<EOF
interface=wlan0
driver=nl80211
ssid=ESP_AP
hw_mode=g
channel=7
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=esp12345678
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOF

echo "Pointing hostapd to config..."
sed -i 's|#DAEMON_CONF=""|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' /etc/default/hostapd

echo "Configuring IP forwarding..."
sed -i '/^#net.ipv4.ip_forward=1/c\net.ipv4.ip_forward=1' /etc/sysctl.conf
sysctl -w net.ipv4.ip_forward=1

echo "Setting up NAT with iptables..."
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
netfilter-persistent save

echo "Restarting services..."
systemctl restart hostapd
systemctl restart dnsmasq

echo "✅ Setup complete."
echo "SSID: ESP_AP"
echo "Password: esp12345678"
echo "Static AP IP: 10.10.10.1"

