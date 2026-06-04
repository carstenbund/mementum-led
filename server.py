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

from flask import Flask, request, jsonify, send_from_directory, Response
from concurrent.futures import ThreadPoolExecutor
import collections
import json
import os
import queue
import sqlite3
import sys
import time
import threading
import requests

# Stream logs live: under systemd, Python block-buffers stdout (pipe, not a TTY), so
# print() output would not reach journald until the buffer fills. Line buffering makes
# `journalctl -u mementum-server -f` show activity as it happens.
try:
    sys.stdout.reconfigure(line_buffering=True)
    sys.stderr.reconfigure(line_buffering=True)
except AttributeError:
    pass  # Python < 3.7

app = Flask(__name__)

# ---- Configuration (mirrors ws_wifi.h / ws_flow.h) ----
MAX_SENT_STRINGS = 5          # compact FIFO depth (matches firmware)
MAX_PLAYS = 3                 # max_plays in firmware (live-adjustable via /setMaxPlays)
MIN_MAX_PLAYS = 1             # lower bound for the live-adjustable play count
MAX_MAX_PLAYS = 99           # upper bound (sanity cap)
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

# Seed list for the fragment store on first run (table created empty otherwise).
PREMADE_STRINGS = ["Hello World", "Temperature: 25C", "Status: OK",
                   "Error: None", "Action: Start"]

MAX_TEXT_LENGTH = 100         # matches firmware MAX_TEXT_LENGTH (ws_wifi.h)

# @color prefixes recognized by the firmware (ws_flow.h colorMap)
COLOR_PREFIXES = ["@red", "@green", "@blue", "@pink", "@yellow", "@cyan"]

# ---- Persistent text-fragment store (SQLite) ----
# Editable library of reusable text fragments ("Vorgaben"), persisted so they survive
# restarts. Path is overridable for tests; defaults next to this file.
DB_PATH = os.environ.get(
    "MEMENTUM_DB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "fragments.db"))
db_lock = threading.Lock()    # serialize writes to avoid "database is locked"


def db_connect():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def db_init():
    with db_lock, db_connect() as conn:
        conn.execute("""CREATE TABLE IF NOT EXISTS fragments (
                            id       INTEGER PRIMARY KEY AUTOINCREMENT,
                            text     TEXT NOT NULL,
                            position INTEGER NOT NULL DEFAULT 0)""")
        if conn.execute("SELECT COUNT(*) FROM fragments").fetchone()[0] == 0:
            conn.executemany("INSERT INTO fragments (text, position) VALUES (?, ?)",
                             [(t, i) for i, t in enumerate(PREMADE_STRINGS)])


def db_fragments():
    with db_connect() as conn:
        rows = conn.execute(
            "SELECT id, text FROM fragments ORDER BY position, id").fetchall()
    return [{"id": r["id"], "text": r["text"]} for r in rows]


db_init()

# ---- Shared state (guarded by state_lock) ----
state_lock = threading.Lock()

sent_strings = []             # compact FIFO of raw texts (may include @color prefix)
play_counts = []              # parallel to sent_strings: times each has been shown
play_limits = []              # parallel to sent_strings: per-string repeat cap (captured
                              # at send time so each message can repeat a different number
                              # of times — enables orchestrating patterns)
cur_index = 0                 # sequencer cursor

# ip -> {'id', 'version', 'first_seen', 'last_seen', 'active'}. Clients are kept
# after they go stale (active=False) so the status window can still show them with
# the time they were last seen; PURGE_INACTIVE bounds how long they linger.
registered_clients = {}
next_client_id = 0
global_seq = 0                # monotonic schedule sequence number
PURGE_INACTIVE = 3600         # seconds: drop a disappeared client from the table after this

_clock_start = time.monotonic()


# ---- Live event log (status window / debug stream) ----
class LogHub:
    """Fan server events out to any number of SSE subscribers and keep a short history.

    'info' events (register, play, broadcast, disappear) also go to journald via print();
    'debug' events (heartbeats, clock syncs) are streamed to the web debug view only, so
    journald is not flooded by per-client chatter.
    """
    def __init__(self, history=300):
        self._history = collections.deque(maxlen=history)
        self._subscribers = set()
        self._lock = threading.Lock()

    def emit(self, msg, level="info", journal=True):
        event = {"t": time.strftime("%H:%M:%S"), "level": level, "msg": msg}
        if journal:
            print(msg)
        with self._lock:
            self._history.append(event)
            for q in list(self._subscribers):
                try:
                    q.put_nowait(event)
                except queue.Full:
                    pass

    def subscribe(self):
        q = queue.Queue(maxsize=1000)
        with self._lock:
            for event in self._history:   # seed the new viewer with recent history
                try:
                    q.put_nowait(event)
                except queue.Full:
                    break
            self._subscribers.add(q)
        return q

    def unsubscribe(self, q):
        with self._lock:
            self._subscribers.discard(q)


log = LogHub()


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


def clamp_plays(value):
    """Clamp a repeat count into the allowed range."""
    return max(MIN_MAX_PLAYS, min(MAX_MAX_PLAYS, value))


def add_sent_string(s, limit=None):
    """Append to the compact FIFO, dropping the oldest when full and resetting the
    new slot's play count so the message is actually shown (mirrors addSentString).
    `limit` is this message's own repeat cap; defaults to the server default MAX_PLAYS."""
    global cur_index
    sent_strings.append(s)
    play_counts.append(0)
    play_limits.append(clamp_plays(MAX_PLAYS if limit is None else limit))
    if len(sent_strings) > MAX_SENT_STRINGS:
        sent_strings.pop(0)
        play_counts.pop(0)
        play_limits.pop(0)
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
            log.emit("BROADCAST %s FAILED ID=%s IP=%s: %s"
                     % (route, by_ip.get(ip), ip, info))
    log.emit("BROADCAST %s -> %d/%d clients OK" % (route, ok, len(targets)))


def broadcast_play(seq, text, display_at, targets):
    """Push a timed /play command to all clients concurrently."""
    _broadcast("/play", {'seq': seq, 'at': display_at, 'data': text}, targets)


def broadcast_simple(route, targets):
    """GET a parameterless route (e.g. /clear) on all clients concurrently."""
    _broadcast(route, None, targets)


def snapshot_targets():
    # Only push to clients we still believe are present; disappeared ones are kept
    # in the table for display but must not be broadcast to.
    return [(ip, c['id']) for ip, c in registered_clients.items() if c.get('active', True)]


# ---- HTTP routes (mirror the firmware route table) ----
@app.route("/")
def serve_index():
    return send_from_directory('data', 'index.html')


@app.route("/getData")
def get_data():
    with state_lock:
        # compact order == display order; include each string's own repeat cap and
        # how many times it has played so far so the UI can show/orchestrate them.
        return jsonify([{"text": t, "plays": play_counts[i], "limit": play_limits[i]}
                        for i, t in enumerate(sent_strings)])


@app.route("/getPreMade")
def get_pre_made():
    # Preset buttons are fed from the editable fragment store.
    return jsonify([f["text"] for f in db_fragments()])


# ---- Text-fragment store (editable "Vorgaben" library) ----
def _clean_fragment_text():
    text = (request.args.get('text') or '').strip()
    return text[:MAX_TEXT_LENGTH]


@app.route("/fragments")
def list_fragments():
    return jsonify(db_fragments())


@app.route("/addFragment")
def add_fragment():
    text = _clean_fragment_text()
    if not text:
        return jsonify({"status": "error", "message": "Empty fragment."}), 400
    with db_lock, db_connect() as conn:
        nextpos = conn.execute(
            "SELECT COALESCE(MAX(position), -1) + 1 FROM fragments").fetchone()[0]
        conn.execute("INSERT INTO fragments (text, position) VALUES (?, ?)", (text, nextpos))
    log.emit("FRAGMENT  add %r" % text)
    return jsonify(db_fragments())


@app.route("/updateFragment")
def update_fragment():
    text = _clean_fragment_text()
    if not request.args.get('id', '').isdigit():
        return jsonify({"status": "error", "message": "Missing/invalid id."}), 400
    if not text:
        return jsonify({"status": "error", "message": "Empty fragment."}), 400
    fid = int(request.args['id'])
    with db_lock, db_connect() as conn:
        cur = conn.execute("UPDATE fragments SET text = ? WHERE id = ?", (text, fid))
        changed = cur.rowcount
    if not changed:
        return jsonify({"status": "error", "message": "No such fragment."}), 404
    log.emit("FRAGMENT  update id=%d %r" % (fid, text))
    return jsonify(db_fragments())


@app.route("/deleteFragment")
def delete_fragment():
    if not request.args.get('id', '').isdigit():
        return jsonify({"status": "error", "message": "Missing/invalid id."}), 400
    fid = int(request.args['id'])
    with db_lock, db_connect() as conn:
        conn.execute("DELETE FROM fragments WHERE id = ?", (fid,))
    log.emit("FRAGMENT  delete id=%d" % fid)
    return jsonify(db_fragments())


@app.route("/SendData")
def send_data():
    if 'data' not in request.args:
        return jsonify({"status": "error", "message": "No data provided."}), 400
    # Optional per-string repeat count; falls back to the server default (MAX_PLAYS)
    # for callers that don't supply one (firmware, curl, older clients).
    raw_plays = request.args.get('plays', '')
    limit = clamp_plays(int(raw_plays)) if raw_plays.lstrip('-').isdigit() else None
    with state_lock:
        add_sent_string(request.args['data'], limit)
    # Model A: do not push raw text; the sequencer distributes it via /play.
    return jsonify({"status": "ok", "message": "Command received and processed."})


@app.route("/clear")
def clear():
    global cur_index
    with state_lock:
        sent_strings.clear()
        play_counts.clear()
        play_limits.clear()
        cur_index = 0
        targets = snapshot_targets()
    # Tell clients to blank immediately and drop any in-flight schedule.
    broadcast_simple("/clear", targets)
    return "Sent strings cleared and broadcasted.", 200


@app.route("/register")
def register():
    global next_client_id
    ip = request.remote_addr
    # Optional: new firmware (>=1.3) reports its version; old (pre-/play) firmware omits it.
    version = request.args.get('version', '?')
    now = time.time()
    with state_lock:
        if ip not in registered_clients:
            # Monotonic IDs, never reused after cleanup (mirrors firmware fix).
            cid = next_client_id
            registered_clients[ip] = {'id': cid, 'version': version,
                                      'first_seen': now, 'last_seen': now, 'active': True}
            next_client_id += 1
            response = "Registered successfully. Your ID: %d" % cid
            event = "REGISTER  new   ID=%d IP=%s version=%s" % (cid, ip, version)
        else:
            client = registered_clients[ip]
            reappeared = not client.get('active', True)
            client['last_seen'] = now
            client['version'] = version
            client['active'] = True
            cid = client['id']
            response = "Already registered. Your ID: %d" % cid
            event = "REGISTER  %s ID=%d IP=%s version=%s" % (
                "back " if reappeared else "again", cid, ip, version)
    log.emit(event)
    if version == '?':
        log.emit("  WARNING: client IP=%s reports no version -> likely OLD firmware that "
                 "does not support timed /play; reflash to >=1.3 or it will show no text." % ip)
    return response


@app.route("/heartbeat")
def heartbeat():
    # Identify by source IP, not the reported index (mirrors firmware fix).
    ip = request.remote_addr
    with state_lock:
        client = registered_clients.get(ip)
        if client is None:
            log.emit("HEARTBEAT unknown IP=%s -> asked to re-register" % ip,
                     level="debug", journal=False)
            return "Unknown client. Please re-register.", 400
        client['last_seen'] = time.time()
        reappeared = not client.get('active', True)
        client['active'] = True
        cid = client['id']
    if reappeared:
        log.emit("RECONNECT ID=%d IP=%s (heartbeat after going stale)" % (cid, ip))
    log.emit("HEARTBEAT ack    ID=%d IP=%s" % (cid, ip), level="debug", journal=False)
    return "Heartbeat acknowledged.", 200


@app.route("/time")
def get_time():
    # Reference clock for clients (Cristian's algorithm source).
    now = server_now()
    log.emit("CLOCK     /time -> %d  (from IP=%s)" % (now, request.remote_addr),
             level="debug", journal=False)
    return str(now)


@app.route("/clients")
def clients():
    """Status snapshot for the admin window: who is/was connected, and since/until when."""
    now = time.time()
    with state_lock:
        rows = []
        for ip, c in registered_clients.items():
            rows.append({
                "id": c['id'],
                "ip": ip,
                "version": c.get('version', '?'),
                "first_seen": c['first_seen'],
                "last_seen": c['last_seen'],
                "active": bool(c.get('active', True)),
                "age": round(now - c['last_seen'], 1),  # seconds since last contact
            })
    rows.sort(key=lambda r: r['id'])
    return jsonify({"now": now, "heartbeat_timeout": HEARTBEAT_TIMEOUT, "clients": rows})


@app.route("/logstream")
def logstream():
    """Server-Sent Events stream of server log events (status + debug) for the UI window."""
    def gen():
        q = log.subscribe()
        try:
            # An initial comment makes some proxies start streaming immediately.
            yield ": connected\n\n"
            while True:
                try:
                    event = q.get(timeout=15)
                    yield "data: %s\n\n" % json.dumps(event)
                except queue.Empty:
                    yield ": keep-alive\n\n"   # hold the connection open through idle gaps
        finally:
            log.unsubscribe(q)
    headers = {"Cache-Control": "no-cache", "X-Accel-Buffering": "no",
               "Connection": "keep-alive"}
    return Response(gen(), mimetype="text/event-stream", headers=headers)


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


@app.route("/getMaxPlays")
def get_max_plays():
    # Default repeat count applied to new strings that don't carry their own (the
    # "next string" value seeded into the UI; each string captures it at send time).
    return jsonify({"max_plays": MAX_PLAYS})


@app.route("/setMaxPlays")
def set_max_plays():
    # Set the default repeat count for newly sent strings. Existing strings keep the
    # per-string limit they were captured with; this only affects future sends that
    # don't pass an explicit ?plays= value.
    global MAX_PLAYS
    raw = request.args.get("value", "")
    if not raw.lstrip("-").isdigit():
        return "Missing or non-numeric value parameter.", 400
    value = int(raw)
    if value < MIN_MAX_PLAYS or value > MAX_MAX_PLAYS:
        return "Value out of range (%d..%d)." % (MIN_MAX_PLAYS, MAX_MAX_PLAYS), 400
    with state_lock:
        MAX_PLAYS = value
    log.emit("CONFIG    max_plays -> %d" % value)
    return jsonify({"max_plays": MAX_PLAYS})


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
                play_limits.pop(idx)
        if cur_index >= len(sent_strings):
            cur_index = 0
    return get_data()


# ---- Background workers ----
def cleanup_stale_clients():
    while True:
        now = time.time()
        disappeared, purged = [], []
        with state_lock:
            for ip, c in list(registered_clients.items()):
                idle = now - c['last_seen']
                if c.get('active', True) and idle > HEARTBEAT_TIMEOUT:
                    # Keep the record (so the status window can show "last seen"),
                    # just mark it gone so we stop broadcasting to it.
                    c['active'] = False
                    disappeared.append((c['id'], ip, idle))
                elif not c.get('active', True) and idle > PURGE_INACTIVE:
                    purged.append((c['id'], ip))
                    del registered_clients[ip]
        for cid, ip, idle in disappeared:
            log.emit("DISAPPEAR ID=%d IP=%s (no heartbeat for %ds)" % (cid, ip, int(idle)))
        for cid, ip in purged:
            log.emit("PURGE     ID=%d IP=%s (gone > %ds, dropped from table)"
                     % (cid, ip, PURGE_INACTIVE))
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
                    if play_counts[cur_index] < play_limits[cur_index]:
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

        log.emit("PLAY      seq=%d at=%d now=%d targets=%d text=%r"
                 % (seq, display_at, server_now(), len(targets), text))
        if not targets:
            log.emit("  (no active clients to receive this /play)")
        broadcast_play(seq, text, display_at, targets)
        # Wait the lead-in plus the scroll so the next /play arrives after this one ends.
        time.sleep((DISPLAY_LEAD_MS + scroll_duration_ms(text)) / 1000.0)


if __name__ == '__main__':
    threading.Thread(target=cleanup_stale_clients, daemon=True).start()
    threading.Thread(target=sequencer, daemon=True).start()
    app.run(host='0.0.0.0', port=80, threaded=True)
