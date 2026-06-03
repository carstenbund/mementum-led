# Mementum LED — Raspberry Pi server

Turn a Raspberry Pi into a dedicated access point + control server that fully
replicates the ESP32 AP-mode server. This lets the swarm grow past the ~20-client
limit of the ESP32 soft-AP while speaking the exact same protocol, so unmodified
ESP32 clients connect and stay in sync.

The Pi runs `server.py` (the master sequencer: shared `/time` clock + timed `/play`
push), and hosts the Wi-Fi network the clients already look for.

## Scaling past 20 clients

The ESP32's ~20-client ceiling came from three stacked limits: the soft-AP radio cap
(`max_connection=16`), sequential per-client broadcast whose fan-out time grew with the
client count, and a small queue. On the Pi:

- **Broadcast is concurrent** — `server.py` pushes `/play` to all clients in parallel
  (`BROADCAST_WORKERS`), so total fan-out stays ~flat (≈ slowest single client, well
  under the 2 s display lead) instead of `N × timeout`. Measured: 50 clients in ~0.5 s.
- **Caps raised** — `max_num_sta=64` and a near-full /24 DHCP pool (`.2–.250`).

The remaining real limit is the **access point hardware**: the built-in Raspberry Pi
radio reliably serves only **~8–16 AP clients** no matter the config. For ~50 real
clients use an external USB Wi-Fi adapter in AP mode or a dedicated AP/router; for 250+
also widen the subnet to /23 (`dnsmasq.conf` + `dhcpcd.append.conf`).

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

On Raspberry Pi OS (Bookworm or Bullseye) with a `wlan0` interface:

```bash
git clone <this-repo>
cd mementum-led
sudo ./raspberry/install.sh
```

This installs `hostapd` + `dnsmasq`, writes the AP/DHCP config, pins `wlan0` to
`10.10.10.1`, creates a Python venv with Flask + requests, and installs a
`mementum-server` systemd service enabled at boot.

### Taking wlan0 over from an existing network

If the Pi is already joined to a Wi-Fi network, that connection must be released
or hostapd cannot switch the radio into AP mode (the symptom: AP never appears /
no DHCP offers). The installer handles this automatically for both Raspberry Pi
OS network stacks, touching **only `wlan0`** (wired `eth0`/admin is left alone):

- **NetworkManager** (Bookworm): `wlan0` is marked permanently *unmanaged* via
  `/etc/NetworkManager/conf.d/99-mementum-unmanaged.conf`.
- **wpa_supplicant** (Bullseye/older): `wpa_supplicant@wlan0.service` is disabled.
- On every AP start, `mementum-ap-prepare` (a hostapd `ExecStartPre`) re-releases
  `wlan0`, flushes any client-mode address, and re-asserts the static AP IP — so
  it is self-healing across reboots and `systemctl restart hostapd`.

> ⚠️ **If you administer the Pi over its Wi-Fi (`wlan0`), bringing up the AP will
> drop your SSH session** — the radio can't be a client and an AP at once. After
> it comes up, reconnect to SSID `mementumLED` and SSH to `10.10.10.1`. Prefer a
> wired/console session for the first bring-up. `start.sh` deliberately detaches
> itself (logging to `/var/log/mementum-start.log`) so the bring-up finishes even
> after your wlan0 session drops. To temporarily get normal Wi-Fi back for
> maintenance: `sudo ./raspberry/stop.sh --release-wlan0`.

## Run

```bash
sudo ./raspberry/start.sh     # bring AP + server up now (no reboot)
sudo ./raspberry/status.sh    # services, wlan0 address, server queue
sudo ./raspberry/stop.sh      # stop server + AP
sudo reboot                   # or just reboot: it comes up as the AP server
```

After boot, ESP32 clients associate to `mementumLED` and reach the server at
`http://10.10.10.1`. The admin UI is the same `data/index.html` served at `/`.

## Live logs / "clients connect but show no text"

```bash
sudo ./raspberry/logs.sh          # follow server + hostapd + dnsmasq live
sudo ./raspberry/logs.sh server   # just the control server
```

What to look for in the server log:

- `REGISTER new ID=.. IP=.. version=1.3` — a client checked in. `version=?` with a
  **WARNING** means that client runs **old firmware** that predates the timed `/play`
  protocol: it will heartbeat fine but **never display text**. Reflash it to **≥1.3**.
- `PLAY seq=.. at=.. now=.. targets=N text=..` — the sequencer scheduled a message.
  `targets=0` means no clients are registered. Then `Broadcast /play -> N/N OK`
  confirms each client accepted it.

This is the usual cause of "heartbeats acknowledged but no text": the server now
distributes messages only via timed `/play` (with shared-clock sync), which old 1.2
firmware doesn't implement. The fix is to reflash the clients, not to change the server.

## Files

| Path                              | Purpose                                          |
|-----------------------------------|--------------------------------------------------|
| `install.sh`                      | One-shot installer (idempotent)                  |
| `start.sh` / `stop.sh`            | Bring the AP + server up / down now              |
| `status.sh`                       | Service + network + queue status                 |
| `logs.sh`                         | Follow live server / hostapd / dnsmasq logs      |
| `ap-prepare.sh`                   | Release `wlan0` from NM/wpa_supplicant + set IP  |
| `requirements.txt`                | Python deps for `server.py`                      |
| `config/hostapd.conf`             | Access point (SSID/PSK/channel)                  |
| `config/dnsmasq.conf`             | DHCP range + DNS for the AP subnet               |
| `config/dhcpcd.append.conf`       | Static `10.10.10.1` for `wlan0`                  |
| `config/networkmanager-unmanaged.conf` | Marks `wlan0` unmanaged (Bookworm)          |
| `systemd/hostapd.dropin.conf`     | Runs `ap-prepare` before hostapd                 |
| `systemd/dnsmasq.dropin.conf`     | Gates DHCP on the AP being up                    |
| `systemd/mementum-server.service` | Boot-into-server unit                            |

## Notes

- `server.py` binds port 80, so the service runs as root.
- To change SSID/password, edit `config/hostapd.conf` **and** the clients'
  `data/config.csv` (`APSSID`/`APPSK`) so both ends still match, then re-run
  `install.sh`.
- Internet sharing (NAT to `eth0`) is intentionally not configured here — this
  builds an isolated display network, same as the ESP32. Add `iptables`
  MASQUERADE on `eth0` if you want uplink.
