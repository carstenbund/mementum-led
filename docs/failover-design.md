# Failover design — role-agnostic, self-healing server election

**Status:** design / RFC. No firmware behaviour changes yet — this document is for review
before implementation.

## 1. Goal

Remove the single point of failure. Today exactly one ESP32 is statically the server
(`isAPMode=true`) and *is* the WiFi access point; if it is lost, the whole network and the
sign go dark. We want every node to run identical firmware and the same state machine, so
that:

- exactly one node serves (is the AP) at a time;
- if the serving node disappears, the best surviving node promotes itself and the others
  rejoin it automatically;
- a node that comes back later rejoins as a plain client (no manual reset, no dual AP).

## 2. Topology (unchanged)

- One SoftAP network: SSID `APSSID`, PSK `APPSK`, fixed channel 1, subnet `10.10.10.0/24`,
  server always at `10.10.10.1` (`ws_wifi.cpp` SoftAP setup).
- Every non-serving node is an STA client on that AP and talks HTTP to `10.10.10.1`.
- **The 1.3 HTTP protocol is untouched.** `/register`, `/heartbeat`, `/time`, `/play`,
  `/SendData`, … are identical. Failover is purely about *which chip* is the AP at
  `10.10.10.1`. Clients already re-register and re-sync the clock on reconnect, so a
  promoted server with a fresh `millis()` clock domain is handled by existing logic.

## 3. Why this can't be a normal election

Because the server *is* the network, when it dies **every client drops at the same instant
and the nodes can no longer talk to each other.** There is no channel over which to run a
"vote for a leader" handshake. Each node must decide **alone**, using the only thing it can
still observe: whether `APSSID` is on the air (via a WiFi scan).

The danger is **split-brain**: two nodes deciding to promote at the same time → two SoftAPs
with the same SSID → clients flap between them. The whole design is about making promotion
*ordered* and *self-correcting* without any communication.

## 4. Identity and rank (MAC-derived)

- Each node has a globally-unique, stable **MAC**. We use it two ways:
  - `mac_hash = hash(STA MAC) % SPREAD_MS` → a stable, pseudo-random **backoff slot** that
    spreads nodes across a time window before promoting.
  - The full **SoftAP BSSID** (`WiFi.softAPmacAddress()`) is the **tiebreak key**: among
    nodes that end up serving simultaneously, the **lowest BSSID wins**; higher ones
    abdicate (see §7).
- A node knows only its *own* MAC while offline, so it cannot compute its global rank
  position. That is fine: the backoff slot orders promotions probabilistically, and the
  lowest-BSSID abdication rule deterministically cleans up any collision afterwards.

## 5. State machine

States (one per node):

| State | Meaning |
|---|---|
| `JOINING` | STA mode, trying to (re)connect to `APSSID`. Initial state for every node. |
| `CLIENT` | Connected, registered, heartbeating + clock-synced. Normal client. |
| `CANDIDATE` | Server lost; scanning for `APSSID` and counting down a promotion timer. |
| `SERVER` | Running SoftAP + HTTP server + sequencer. The AP at `10.10.10.1`. |

Transitions:

```
boot ───────────────► JOINING

JOINING:
  connect to APSSID ok ───────────────► CLIENT
  APSSID absent ≥ T_grace_cold ───────► CANDIDATE

CLIENT:
  WiFi lost, or heartbeat fails ≥ N ──► (drop STA) CANDIDATE

CANDIDATE:                              # scan every scan_interval
  APSSID appears ─────────────────────► JOINING        # someone serves; rejoin
  APSSID absent AND promote_timer fired:
       final guard scan:
          APSSID present ──────────────► JOINING        # lost the race; rejoin
          APSSID absent ───────────────► SERVER         # promote

SERVER:                                 # WIFI_AP_STA; passive scan every server_scan_interval
  sees another APSSID with lower BSSID ► (stop AP) JOINING   # abdicate to the better server
  otherwise ──────────────────────────► stay SERVER
```

`promote_timer = T_grace + (mac_hash) + jitter(0..J)` measured from when absence was first
confirmed. `T_grace` is `T_grace_cold` on first boot (no server ever seen) and
`T_grace_fail` after losing a known server.

While in `JOINING`/`CANDIDATE` the panel idles (holds last frame or blanks — TBD, see open
questions). `Display_Loop` switches on **role** instead of the old static `isAPMode`
(`http_controller.ino` `Display_Loop`): `SERVER → serverSequencerLoop()`,
`CLIENT → clientPlayerLoop()`, else idle.

## 6. Promotion safety (avoiding split-brain)

Three layers, in order of how often they act:

1. **Staggered backoff** — `mac_hash % SPREAD_MS` spreads promotion times, so the node with
   the smallest slot normally comes up alone; others detect its AP during their longer wait
   and rejoin as clients.
2. **Final guard scan** — immediately before `softAP()`, scan once more; if `APSSID` is now
   present, abort and `JOINING`. Catches most near-simultaneous cases.
3. **Lowest-BSSID abdication** — if two do come up together, the higher-BSSID server sees the
   lower one and steps down (see §7). Converges to a single server within one server scan.

The spacing math is the key tuning tension: to *fully* avoid collisions by backoff alone,
consecutive slots must exceed AP-bringup + client-detect (~3–4 s), which for N≤16 nodes
means `SPREAD_MS ≈ 60 s` (slow failover). Instead we keep `SPREAD_MS` modest (fast failover)
and rely on layer 3 to mop up the rare collision. Failover ends up ~10–20 s typical.

## 7. Lowest-BSSID abdication (the self-heal)

A `SERVER` runs in `WIFI_AP_STA` mode and every `server_scan_interval` does a short passive
scan:

- If it finds another AP advertising `APSSID` whose BSSID is **numerically lower** than its
  own `softAPmacAddress()`, it **abdicates**: stop SoftAP → `JOINING` → becomes a client of
  the lower-BSSID server.
- Otherwise it keeps serving.

This deterministically reduces any set of simultaneous servers to the single lowest-BSSID
one. A **cooldown** after abdication (and after promotion) prevents oscillation.

Caveat: `WIFI_AP_STA` scanning briefly pauses the AP; keep scans short/passive and
infrequent so connected clients are not disrupted.

## 8. Deliberate non-goals / simplifications

- **MAC rank only breaks ties among *simultaneous* promoters.** A node that returns later and
  finds the SSID already up stays a **client even if it has a lower MAC** — we prefer
  stability over "the lowest MAC must always lead," because reclaiming would mean tearing the
  network down again. So there is no pre-emptive reclaim.
- **Promoted server starts with an empty queue.** Messages that lived on the dead server are
  gone; the operator re-sends. State hand-off between nodes is explicitly out of scope for v1.
- **RF partitions are out of scope.** If a node genuinely cannot hear the AP (out of range),
  it may promote and create a real second network in its own RF zone. That is inherent to
  wireless; we assume all nodes are in range of each other.

## 9. Parameters (as implemented — `ws_wifi.cpp`, all compile-time / tunable)

| Name | Value | Meaning |
|---|---|---|
| `T_GRACE_COLD` | 3000 ms | absence before promoting on first boot (no server ever seen) |
| `T_GRACE_FAIL` | 8000 ms | absence before promoting after losing a known server (rides out quick reboots) |
| `SPREAD_MS` | 5000 ms | width of the MAC-hashed backoff window (small: speed over safety) |
| `JITTER_MS` | 0–500 ms | extra random spread on top of the slot |
| `JOIN_GRACE_COLD` / `JOIN_GRACE_WARM` | 3000 / 6000 ms | time a `JOINING` node tries to associate before scanning to decide whether to contest |
| `SCAN_INTERVAL` | 2000 ms | CANDIDATE scan cadence (scan itself ~1.5 s) |
| `SERVER_SCAN_INTERVAL` | 6000 ms | SERVER abdication-check cadence |
| `ABDICATION_WINDOW` | 60000 ms | only scan for competitors this long after promoting; afterwards a lone server never pauses its AP |
| `HEARTBEAT_FAIL_N` | 3 | consecutive heartbeat failures that declare the server lost (secondary to WiFi-link loss) |
| `PROMOTION_COOLDOWN` | 15000 ms | min time between abdication/promotion attempts (anti-flap) |

## 10. Firmware changes (sketch — for the eventual implementation)

- `ws_wifi.cpp`
  - Add `enum Role { JOINING, CLIENT, CANDIDATE, SERVER }` + current-role accessor.
  - `uint32_t computeBackoff()` from `WiFi.macAddress()`.
  - `bool scanForServer()` via `WiFi.scanNetworks()` filtered to `SSID == APSSID` (returns
    presence + lowest competing BSSID for abdication).
  - `promote()` / `abdicate()` that switch `WiFi.mode()`, start/stop `softAP`, start/stop the
    HTTP `server`, and reset timers.
  - Rewrite `WIFI_Loop()` as the state-machine tick; replace the static `isAPMode` branches
    in setup and loop.
- `http_controller.ino`
  - `Display_Loop()` switches on role instead of `isAPMode`; idle when not CLIENT/SERVER.
- `config.csv`
  - `isAPMode` becomes irrelevant to role (kept ignored, or repurposed). `APSSID`/`APPSK`
    still define the one network. Optional `failoverEnabled` (default true) and an optional
    `preferServer` hint that zeroes a chosen node's backoff so known-good hardware leads on
    cold boot — open question below.

## 11. Failure-case walkthrough

- **Cold boot, no server:** all `JOINING` → none found → all `CANDIDATE` → smallest-slot node
  promotes after `T_grace_cold + slot`; rest see the SSID and become `CLIENT`. ✓
- **Server dies:** clients lose WiFi/heartbeat → `CANDIDATE` → smallest-slot survivor promotes;
  others rejoin. ✓
- **Original returns after a backup promoted:** original boots `JOINING`, finds SSID up,
  becomes `CLIENT` — even if its MAC is lower. No dual AP, no reclaim. ✓
- **Two promote at once (slot collision):** both `SERVER` briefly → `AP_STA` scan → higher-BSSID
  abdicates within one `server_scan_interval`; clients converge on the survivor. ✓ (brief
  disruption, self-heals)
- **Flapping server:** `T_grace_fail` + "join existing SSID beats promoting" + cooldown damp it.

## 12. Risks & testing

- Pure-firmware WiFi role-switching; **must be tested on hardware with ≥3 nodes** — cannot be
  validated in this environment.
- SoftAP bring-up timing and `WIFI_AP_STA` scan behaviour vary by ESP32 core version; the
  parameters in §9 will need field tuning.
- Brief client disruption during abdication scans and during promotion windows.

## 13. Suggested phasing

1. **Phase 1** — state machine (`JOINING/CLIENT/CANDIDATE/SERVER`), MAC-hashed backoff, final
   guard scan. No abdication yet; use a larger `SPREAD_MS` to keep collisions rare. Goal: basic
   failover working end to end.
2. **Phase 2** — `AP_STA` periodic scan + lowest-BSSID abdication + cooldown; shrink `SPREAD_MS`
   for faster failover now that collisions self-heal.
3. **Phase 3 (optional)** — queue/state hand-off; `preferServer` hint; `config.csv` toggles.

## 14. Resolved decisions

1. **Speed over safety.** Use a modest `SPREAD_MS` for fast failover and rely on lowest-BSSID
   abdication (§7) to clean up the rare case where a lower-MAC node appears at nearly the same
   time. We do **not** trade failover speed for collision-free promotion.
2. **Pure MAC.** No `preferServer` hint; role order is derived entirely from the MAC. Zero
   config.
3. **Spinner while electing.** During `JOINING`/`CANDIDATE` the panel shows a small rotating
   glyph cycling `| / - \` to indicate "configuring / waiting for network," rather than holding
   the last frame or blanking.
4. **Empty queue on promotion is fine.** No state hand-off; each client keeps its own presets,
   so a freshly promoted server starting with an empty queue is acceptable.
5. **Phase 1 + 2 together.** Ship the state machine, MAC-hashed backoff, final guard scan, **and**
   lowest-BSSID abdication + cooldown in the first cut, so dual-AP self-heals from day one. This
   also lets `SPREAD_MS` stay small (decision 1).
