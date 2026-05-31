# Mementum LED — Raspberry Pi server

Turn a Raspberry Pi into a dedicated access point + control server that fully
replicates the ESP32 AP-mode server. This lets the swarm grow past the ~20-client
limit of the ESP32 soft-AP while speaking the exact same protocol, so unmodified
ESP32 clients connect and stay in sync.

The Pi runs `server.py` (the master sequencer: shared `/time` clock + timed `/play`
push), and hosts the Wi-Fi network the clients already look for.

## Parity with the ESP32 server

These values are fixed to match the firmware so existing clients just work:

| Setting   | Value             | Source in firmware            |
|-----------|-------------------|-------------------------------|
| SSID      | `mementumLED`     | `data/config.csv` `APSSID`    |
| Password  | `mementumLED`     | `data/config.csv` `APPSK`     |
| Server IP | `10.10.10.1`      | `ws_wifi.cpp` `serverAddress` |
| Channel   | `1` (2.4 GHz)     | `ws_wifi.cpp` `WiFi.softAP`   |
| Admin/API | port `80`         | `server.py` / firmware        |

## Install

On a fresh Raspberry Pi OS (Bookworm) with a `wlan0` interface:

```bash
git clone <this-repo>
cd mementum-led
sudo ./raspberry/install.sh
```

This installs `hostapd` + `dnsmasq`, writes the AP/DHCP config, pins `wlan0` to
`10.10.10.1`, creates a Python venv with Flask + requests, and installs a
`mementum-server` systemd service enabled at boot.

## Run

```bash
sudo ./raspberry/start.sh     # bring AP + server up now (no reboot)
sudo ./raspberry/status.sh    # services, wlan0 address, server queue
sudo ./raspberry/stop.sh      # stop server + AP
sudo reboot                   # or just reboot: it comes up as the AP server
```

After boot, ESP32 clients associate to `mementumLED` and reach the server at
`http://10.10.10.1`. The admin UI is the same `data/index.html` served at `/`.

## Files

| Path                              | Purpose                                          |
|-----------------------------------|--------------------------------------------------|
| `install.sh`                      | One-shot installer (idempotent)                  |
| `start.sh` / `stop.sh`            | Bring the AP + server up / down now              |
| `status.sh`                       | Service + network + queue status                 |
| `requirements.txt`                | Python deps for `server.py`                      |
| `config/hostapd.conf`             | Access point (SSID/PSK/channel)                  |
| `config/dnsmasq.conf`             | DHCP range + DNS for the AP subnet               |
| `config/dhcpcd.append.conf`       | Static `10.10.10.1` for `wlan0`                  |
| `systemd/mementum-server.service` | Boot-into-server unit                            |

## Notes

- `server.py` binds port 80, so the service runs as root.
- To change SSID/password, edit `config/hostapd.conf` **and** the clients'
  `data/config.csv` (`APSSID`/`APPSK`) so both ends still match, then re-run
  `install.sh`.
- Internet sharing (NAT to `eth0`) is intentionally not configured here — this
  builds an isolated display network, same as the ESP32. Add `iptables`
  MASQUERADE on `eth0` if you want uplink.
