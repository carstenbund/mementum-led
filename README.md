# Mementum LED
Group of ESP32-Matrix share text to display

This project aims to build a networked swarm of wearable devices, displaying syncronized messages. 

Implemented with Arduino IDE, for ESP32-S3 with integrated display

This codebase includes both client and server, defined by config choice at runtime.

A Raspberry Pi can also act as a dedicated server that fully replicates the ESP32
AP-mode server (same SSID/PSK, IP, and protocol) to grow the swarm past the ~20
client soft-AP limit. See `raspberry/` for a one-shot installer and boot-into-server
services. Note: the legacy top-level `setup.sh` advertises a different SSID/password
than the firmware clients expect — use `raspberry/install.sh` instead.

Maximum devices are about 20 in this configuration.

The system boots, evaluated configuration to set mode and network specifics.
When configures as server the system will create a softAP to allow clients to connct and register.

Clients search for the configured network and register with the server, to signal readines for display.

The server exposes an admin interface to allow an operator to send text for display in to a queue, which is the broadcasted in parallel using threads to the redisteted clients.

## Synchronized display (timecode)

Clients show the same message at the same instant using a master-sequencer model:

- **Shared clock:** the server's `millis()` is the reference. Each client estimates an
  offset to it via `/time` (Cristian's algorithm, best of three samples), refreshed on
  register and on every heartbeat. `serverNow()` reports time in that shared domain.
- **Scheduled start:** the server is the sequencer. It picks the next message from its
  queue, chooses `displayAt = serverNow() + DISPLAY_LEAD_MS`, and broadcasts a timed
  `/play?seq&at&data` command to every client. The server renders the same schedule
  locally, so it stays in lockstep.
- **Time-derived scroll:** every device computes the scroll column purely from
  `(serverNow() - displayAt)`, so they render the same column at the same moment and a
  dropped frame self-corrects on the next one. Clients are stateless players — they hold
  no queue and cannot drift out of step.

Tunables live in `ws_flow.h`: `SCROLL_INTERVAL_MS` (scroll speed) and `DISPLAY_LEAD_MS`
(lead time before a scheduled start).

Todo:
automated text, time based text, repetitions,
dedicated server to increase client count.
Use of multicast protocol.
Late-join schedule pull (currently a late client syncs on the next scheduled message).
Port the Python server (server.py) to drive clients with the same /time and /play protocol.

