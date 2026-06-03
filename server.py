"""
Dedicated control server for Mementum LED.

This is a faithful port of the ESP32 AP-mode server (ws_wifi.cpp +
http_controller.ino). It owns the message queue and acts as the master
sequencer in the time-synchronized display model ("Model A"):

  - clients learn a shared clock from /time (Cristian's algorithm),
  - the server picks the next valid message, schedules it at
    displayAt = serverNow() + DISPLAY_LEAD_MS, and pushes a timed
    /play?seq&at&data command to every registered client,
  - every device renders the scroll column purely from
    (serverNow() - displayAt), so they stay phase-locked.

The protocol, queue semantics, scroll-width math, registration/heartbeat
behavior, and broadcast model intentionally mirror the firmware so this
server is a drop-in replacement that lets the swarm grow past the ~20
client limit of the ESP32 soft-AP.
"""

from flask import Flask, request, jsonify, send_from_directory
from concurrent.futures import ThreadPoolExecutor
import time
import threading
import requests

app = Flask(__name__)

# ---- Configuration (mirrors ws_wifi.h / ws_flow.h) ----
MAX_SENT_STRINGS = 5          # compact FIFO depth (matches firmware)
MAX_PLAYS = 3                 # max_plays in firmware
HEARTBEAT_TIMEOUT = 80        # seconds (HEARTBEAT_TIMEOUT)
CLEANUP_INTERVAL = 30         # seconds (CLEANUP_INTERVAL)
SCROLL_INTERVAL_MS = 120      # ms per pixel step (SCROLL_INTERVAL_MS)
DISPLAY_LEAD_MS = 2000        # lead time before a scheduled start (DISPLAY_LEAD_MS)
MATRIX_WIDTH = 8              # Matrix.width()
BROADCAST_TIMEOUT = 2.0       # seconds per /play request (firmware uses 2000 ms)

# Unlike the ESP32 (a single radio + sequential tasks, where fan-out time grows with the
# client count and capped practical use near ~20), the dedicated server pushes to all
# clients concurrently. Fan-out time then stays ~flat at the slowest single client
# instead of N * BROADCAST_TIMEOUT, so it comfortably fits inside DISPLAY_LEAD_MS even
# for many clients. Raise this with the client count; it bounds peak concurrent sockets.
BROADCAST_WORKERS = 64

PREMADE_STRINGS = ["Hello World", "Temperature: 25C", "Status: OK",
                   "Error: None", "Action: Start"]

# @color prefixes recognized by the firmware (ws_flow.h colorMap)
COLOR_PREFIXES = ["@red", "@green", "@blue", "@pink", "@yellow", "@cyan"]

# ---- Shared state (guarded by state_lock) ----
state_lock = threading.Lock()

sent_strings = []             # compact FIFO of raw texts (may include @color prefix)
play_counts = []              # parallel to sent_strings
cur_index = 0                 # sequencer cursor

registered_clients = {}       # ip -> {'id': int, 'last_seen': float}
next_client_id = 0
global_seq = 0                # monotonic schedule sequence number

_clock_start = time.monotonic()


def server_now():
    """Time in milliseconds, masked to 32 bits to mimic the ESP32 millis() domain."""
    return int((time.monotonic() - _clock_start) * 1000) & 0xFFFFFFFF


# ---- Scroll-width math (identical to ws_flow.cpp getCharWidth/getStringWidth) ----
def get_char_width(c):
    return 4 if c in ('i', 'l', '!', '.') else 5


def get_string_width(s):
    return sum(get_char_width(c) + 1 for c in s)


def apply_color_and_strip(raw):
    """Return text with any leading @color prefix removed (mirrors applyColorAndStrip)."""
    for prefix in COLOR_PREFIXES:
        if raw.startswith(prefix):
            return raw[len(prefix):]
    return raw


def scroll_duration_ms(raw):
    """How long the firmware spends scrolling one message once.

    setSchedule uses textWidth = getStringWidth(stripped) + 2; renderScheduled
    finishes when cursorX = MATRIX_WIDTH - step < -textWidth, i.e. after
    (MATRIX_WIDTH + textWidth + 1) steps of SCROLL_INTERVAL_MS each.
    """
    text_width = get_string_width(apply_color_and_strip(raw)) + 2
    return (MATRIX_WIDTH + text_width + 1) * SCROLL_INTERVAL_MS


def add_sent_string(s):
    """Append to the compact FIFO, dropping the oldest when full and resetting the
    new slot's play count so the message is actually shown (mirrors addSentString)."""
    global cur_index
    sent_strings.append(s)
    play_counts.append(0)
    if len(sent_strings) > MAX_SENT_STRINGS:
        sent_strings.pop(0)
        play_counts.pop(0)
        cur_index = max(0, cur_index - 1)  # keep the cursor on the same logical item


# ---- Outbound broadcasts to clients ----
# Shared pool so all clients are contacted concurrently; total fan-out time stays close
# to the slowest single client rather than the sum over every client (see BROADCAST_WORKERS).
_broadcast_pool = ThreadPoolExecutor(max_workers=BROADCAST_WORKERS,
                                     thread_name_prefix="broadcast")


def _get_client(ip, route, params):
    try:
        r = requests.get("http://%s%s" % (ip, route), params=params, timeout=BROADCAST_TIMEOUT)
        return 200 <= r.status_code < 300, r.status_code
    except requests.RequestException as e:
        return False, str(e)


def _broadcast(route, params, targets):
    """Fan a request out to every client in parallel and wait for all to finish/time out."""
    if not targets:
        return
    futures = {ip: _broadcast_pool.submit(_get_client, ip, route, params)
               for ip, _cid in targets}
    by_ip = dict(targets)
    ok = 0
    for ip, fut in futures.items():
        success, info = fut.result()  # bounded by BROADCAST_TIMEOUT
        if success:
            ok += 1
        else:
            print("Broadcast %s FAILED to client ID=%s, IP=%s: %s"
                  % (route, by_ip.get(ip), ip, info))
    print("Broadcast %s -> %d/%d clients OK" % (route, ok, len(targets)))


def broadcast_play(seq, text, display_at, targets):
    """Push a timed /play command to all clients concurrently."""
    _broadcast("/play", {'seq': seq, 'at': display_at, 'data': text}, targets)


def broadcast_simple(route, targets):
    """GET a parameterless route (e.g. /clear) on all clients concurrently."""
    _broadcast(route, None, targets)


def snapshot_targets():
    return [(ip, c['id']) for ip, c in registered_clients.items()]


# ---- HTTP routes (mirror the firmware route table) ----
@app.route("/")
def serve_index():
    return send_from_directory('data', 'index.html')


@app.route("/getData")
def get_data():
    with state_lock:
        return jsonify(list(sent_strings))  # compact order == display order


@app.route("/getPreMade")
def get_pre_made():
    return jsonify(PREMADE_STRINGS)


@app.route("/SendData")
def send_data():
    if 'data' not in request.args:
        return jsonify({"status": "error", "message": "No data provided."}), 400
    with state_lock:
        add_sent_string(request.args['data'])
    # Model A: do not push raw text; the sequencer distributes it via /play.
    return jsonify({"status": "ok", "message": "Command received and processed."})


@app.route("/clear")
def clear():
    global cur_index
    with state_lock:
        sent_strings.clear()
        play_counts.clear()
        cur_index = 0
        targets = snapshot_targets()
    # Tell clients to blank immediately and drop any in-flight schedule.
    broadcast_simple("/clear", targets)
    return "Sent strings cleared and broadcasted.", 200


@app.route("/register")
def register():
    global next_client_id
    ip = request.remote_addr
    now = time.time()
    with state_lock:
        if ip not in registered_clients:
            # Monotonic IDs, never reused after cleanup (mirrors firmware fix).
            registered_clients[ip] = {'id': next_client_id, 'last_seen': now}
            response = "Registered successfully. Your ID: %d" % next_client_id
            next_client_id += 1
        else:
            registered_clients[ip]['last_seen'] = now
            response = "Already registered. Your ID: %d" % registered_clients[ip]['id']
    return response


@app.route("/heartbeat")
def heartbeat():
    # Identify by source IP, not the reported index (mirrors firmware fix).
    ip = request.remote_addr
    with state_lock:
        client = registered_clients.get(ip)
        if client is None:
            return "Unknown client. Please re-register.", 400
        client['last_seen'] = time.time()
    return "Heartbeat acknowledged.", 200


@app.route("/time")
def get_time():
    # Reference clock for clients (Cristian's algorithm source).
    return str(server_now())


@app.route("/play")
def play():
    # Present so a client could in principle be pointed at this server; the dedicated
    # server has no panel of its own, so it just acknowledges.
    return "OK", 200


@app.route("/RGBOn")
def rgb_on():
    # Firmware handles RGB locally and does not broadcast it; no panel here.
    return "OK", 200


@app.route("/RGBOff")
def rgb_off():
    return "OK", 200


@app.route("/resetPlayCount")
def reset_play_count():
    # Server-only in Model A (gates the sequencer); no client broadcast.
    with state_lock:
        n = len(play_counts)
        if 'indexes' in request.args:
            indexes = [int(i) for i in request.args['indexes'].split(',') if i.isdigit()]
        else:
            indexes = list(range(n))
        for idx in indexes:
            if 0 <= idx < n:
                play_counts[idx] = 0
    return "Play counts reset.", 200


@app.route("/deleteSelected")
def delete_selected():
    global cur_index
    if 'indexes' not in request.args:
        return "Missing indexes parameter.", 400
    indexes = [int(i) for i in request.args['indexes'].split(',') if i.isdigit()]
    indexes.sort(reverse=True)  # delete high-to-low so positions stay valid
    with state_lock:
        for idx in indexes:
            if 0 <= idx < len(sent_strings):
                sent_strings.pop(idx)
                play_counts.pop(idx)
        if cur_index >= len(sent_strings):
            cur_index = 0
    return get_data()


# ---- Background workers ----
def cleanup_stale_clients():
    while True:
        now = time.time()
        with state_lock:
            stale = [ip for ip, c in registered_clients.items()
                     if now - c['last_seen'] > HEARTBEAT_TIMEOUT]
            for ip in stale:
                print("Removing stale client: ID=%d, IP=%s"
                      % (registered_clients[ip]['id'], ip))
                del registered_clients[ip]
        time.sleep(CLEANUP_INTERVAL)


def sequencer():
    """Master sequencer: schedule one message at a time and push it to all clients.

    Mirrors serverSequencerLoop(): pick the next message with plays remaining,
    schedule it DISPLAY_LEAD_MS ahead, broadcast /play, then wait the lead-in plus
    one scroll duration before scheduling the next so clients are never overwritten
    mid-scroll.
    """
    global cur_index, global_seq
    while True:
        text = None
        with state_lock:
            n = len(sent_strings)
            if n:
                if cur_index >= n:
                    cur_index = 0
                start = cur_index
                found = False
                for _ in range(n):
                    if play_counts[cur_index] < MAX_PLAYS:
                        found = True
                        break
                    cur_index = (cur_index + 1) % n
                if found:
                    text = sent_strings[cur_index]
                    global_seq += 1
                    seq = global_seq
                    display_at = (server_now() + DISPLAY_LEAD_MS) & 0xFFFFFFFF
                    play_counts[cur_index] += 1
                    cur_index = (cur_index + 1) % n
                    targets = snapshot_targets()

        if text is None:
            time.sleep(0.05)  # idle: nothing to play or all plays exhausted
            continue

        broadcast_play(seq, text, display_at, targets)
        # Wait the lead-in plus the scroll so the next /play arrives after this one ends.
        time.sleep((DISPLAY_LEAD_MS + scroll_duration_ms(text)) / 1000.0)


if __name__ == '__main__':
    threading.Thread(target=cleanup_stale_clients, daemon=True).start()
    threading.Thread(target=sequencer, daemon=True).start()
    app.run(host='0.0.0.0', port=80, threaded=True)
