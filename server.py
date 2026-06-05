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
# These two are seed defaults; the persisted values in SQLite (loaded just after
# db_init) override them on startup, and the /set* routes write changes back.
MAX_PLAYS = 3                 # global repeat ceiling (hard cap): no string plays more
                              # than this, regardless of its per-string limit
                              # (effective = min of the two). Live master throttle.
DEFAULT_PLAYS = 3            # preset repeat count inserted when a send doesn't specify
                              # an individual one. Independent of the ceiling.
MIN_MAX_PLAYS = 1             # lower bound for any play count
MAX_MAX_PLAYS = 99           # upper bound (sanity cap)
HEARTBEAT_TIMEOUT = 80        # seconds (HEARTBEAT_TIMEOUT)
CLEANUP_INTERVAL = 30         # seconds (CLEANUP_INTERVAL)
SCROLL_INTERVAL_MS = 120      # ms per pixel step (SCROLL_INTERVAL_MS)
DISPLAY_LEAD_MS = 2000        # lead time before a scheduled start (DISPLAY_LEAD_MS)
TILE_STAGGER_MS = 900         # 'tile' marquee per-panel offset. Empirically a hair under
                              # one full 8-px panel (8*120 = 960 ms): starting the next
                              # panel ~900 ms in lights it the instant the content drops off
                              # the previous one, so there is no seam at the join and the
                              # panels read as one long 16/24/32-wide display.
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

# Seed effect presets on first run. Each is a named, fully self-contained /effect config
# (incl. direction). data='' means "use the live text box" (a marquee of the current text).
# Direction baked in: empty order + reverse=1 = natural id order reversed = right-most first
# = correct left-ward flow when id 0 is the left-most panel.
EFFECT_PRESET_SEEDS = [
    ("Stern",    {"data": "*",            "stagger": "auto", "factor": 1,   "reverse": 1}),
    ("Komet",    {"data": "@cyan*",       "stagger": "auto", "factor": 0.5, "reverse": 1}),
    ("Strom",    {"data": "@yellow* * *", "stagger": "tile", "factor": 1,   "reverse": 1}),
    ("Pfeil",    {"data": "@green<<<",    "stagger": "auto", "factor": 0.6, "reverse": 1}),
    ("Lauftext", {"data": "", "stagger": "tile", "factor": 1, "lead": 800, "gap": 0, "reverse": 1}),
    ("Synchron", {"data": "@pink*",       "stagger": 0,      "factor": 1,   "reverse": 1}),
]

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
        # Persistent config key/value store (survives server restarts).
        conn.execute("""CREATE TABLE IF NOT EXISTS config (
                            key   TEXT PRIMARY KEY,
                            value INTEGER NOT NULL)""")
        # Named effect presets (full /effect config as JSON + an 'active' show-as-button flag).
        conn.execute("""CREATE TABLE IF NOT EXISTS effect_presets (
                            id       INTEGER PRIMARY KEY AUTOINCREMENT,
                            name     TEXT NOT NULL,
                            config   TEXT NOT NULL,
                            active   INTEGER NOT NULL DEFAULT 1,
                            position INTEGER NOT NULL DEFAULT 0)""")
        if conn.execute("SELECT COUNT(*) FROM effect_presets").fetchone()[0] == 0:
            conn.executemany(
                "INSERT INTO effect_presets (name, config, active, position) VALUES (?,?,1,?)",
                [(n, json.dumps(c), i) for i, (n, c) in enumerate(EFFECT_PRESET_SEEDS)])


def db_fragments():
    with db_connect() as conn:
        rows = conn.execute(
            "SELECT id, text FROM fragments ORDER BY position, id").fetchall()
    return [{"id": r["id"], "text": r["text"]} for r in rows]


def db_get_config(key, default):
    with db_connect() as conn:
        row = conn.execute("SELECT value FROM config WHERE key = ?", (key,)).fetchone()
    return row["value"] if row else default


def db_set_config(key, value):
    with db_lock, db_connect() as conn:
        conn.execute("INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)",
                     (key, value))


def db_effect_presets():
    with db_connect() as conn:
        rows = conn.execute(
            "SELECT id, name, config, active FROM effect_presets ORDER BY position, id").fetchall()
    out = []
    for r in rows:
        try:
            cfg = json.loads(r["config"])
        except (ValueError, TypeError):
            cfg = {}
        out.append({"id": r["id"], "name": r["name"], "config": cfg,
                    "active": bool(r["active"])})
    return out


db_init()

# Load persisted config, falling back to the seed defaults defined above on first run.
MAX_PLAYS = db_get_config("max_plays", MAX_PLAYS)
DEFAULT_PLAYS = db_get_config("default_plays", DEFAULT_PLAYS)

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

# ---- Effects (experimental) ----
# A staggered, time-coded effect schedules the SAME glyph on each panel at a slightly
# different start time, so it appears to travel across the wall ("a thing moving through").
# While an effect is running the sequencer pauses so it does not overwrite the sweep.
effect_until = 0.0            # time.monotonic() until which the sequencer holds off
effect_generation = 0         # bumped on each /effect; a running worker stops when it sees
                              # a newer generation, so re-triggering supersedes cleanly
EFFECT_MAX_WAVES = 20         # safety cap on repeated sweeps per /effect call

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
    `limit` is this message's own repeat count; defaults to the preset DEFAULT_PLAYS."""
    global cur_index
    sent_strings.append(s)
    play_counts.append(0)
    play_limits.append(clamp_plays(DEFAULT_PLAYS if limit is None else limit))
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


def broadcast_play_each(items):
    """Push /play to each panel with its OWN params (its own start time and/or text).

    `items` is a list of (ip, params). Fans out concurrently and returns the number of
    panels that ACKed. This is the per-panel cousin of _broadcast(): effects and the
    identify diagnostic need each panel to receive something different.
    """
    if not items:
        return 0
    futures = {ip: _broadcast_pool.submit(_get_client, ip, "/play", params)
               for ip, params in items}
    ok = 0
    for ip, fut in futures.items():
        success, info = fut.result()  # bounded by BROADCAST_TIMEOUT
        if success:
            ok += 1
        else:
            log.emit("PLAY-EACH play FAILED IP=%s: %s" % (ip, info))
    return ok


def broadcast_play_staggered(seq, text, schedule):
    """Push the same glyph to each panel at its OWN start time.

    `schedule` is a list of (ip, cid, display_at). All start times are in the shared
    server clock domain, so each panel renders the same glyph at a different instant and
    it appears to sweep across the wall.
    """
    return broadcast_play_each(
        [(ip, {'seq': seq, 'at': at, 'data': text}) for ip, _cid, at in schedule])


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


# ---- Effect-preset store (named full /effect configs, like the fragment library) ----
def _clean_preset_name():
    return (request.args.get('name') or '').strip()[:40]


def _parse_preset_config():
    """Return the config dict from ?config=<json>, or None if missing/invalid."""
    try:
        cfg = json.loads(request.args.get('config', ''))
        return cfg if isinstance(cfg, dict) else None
    except (ValueError, TypeError):
        return None


@app.route("/effectPresets")
def list_effect_presets():
    return jsonify(db_effect_presets())


@app.route("/addEffectPreset")
def add_effect_preset():
    name = _clean_preset_name()
    cfg = _parse_preset_config()
    if not name:
        return jsonify({"status": "error", "message": "Empty name."}), 400
    if cfg is None:
        return jsonify({"status": "error", "message": "Invalid config JSON."}), 400
    active = 0 if request.args.get('active') in ('0', 'false', 'False') else 1
    with db_lock, db_connect() as conn:
        nextpos = conn.execute(
            "SELECT COALESCE(MAX(position), -1) + 1 FROM effect_presets").fetchone()[0]
        conn.execute(
            "INSERT INTO effect_presets (name, config, active, position) VALUES (?,?,?,?)",
            (name, json.dumps(cfg), active, nextpos))
    log.emit("PRESET    add %r" % name)
    return jsonify(db_effect_presets())


@app.route("/updateEffectPreset")
def update_effect_preset():
    """Patch any of name / config / active on a preset (only the params supplied)."""
    if not request.args.get('id', '').isdigit():
        return jsonify({"status": "error", "message": "Missing/invalid id."}), 400
    pid = int(request.args['id'])
    sets, vals = [], []
    if 'name' in request.args:
        name = _clean_preset_name()
        if not name:
            return jsonify({"status": "error", "message": "Empty name."}), 400
        sets.append("name = ?"); vals.append(name)
    if 'config' in request.args:
        cfg = _parse_preset_config()
        if cfg is None:
            return jsonify({"status": "error", "message": "Invalid config JSON."}), 400
        sets.append("config = ?"); vals.append(json.dumps(cfg))
    if 'active' in request.args:
        active = 1 if request.args.get('active') in ('1', 'true', 'True') else 0
        sets.append("active = ?"); vals.append(active)
    if not sets:
        return jsonify({"status": "error", "message": "Nothing to update."}), 400
    vals.append(pid)
    # `sets` holds only fixed column fragments; values are parameterised.
    with db_lock, db_connect() as conn:
        changed = conn.execute(
            "UPDATE effect_presets SET %s WHERE id = ?" % ", ".join(sets), vals).rowcount
    if not changed:
        return jsonify({"status": "error", "message": "No such preset."}), 404
    log.emit("PRESET    update id=%d" % pid)
    return jsonify(db_effect_presets())


@app.route("/deleteEffectPreset")
def delete_effect_preset():
    if not request.args.get('id', '').isdigit():
        return jsonify({"status": "error", "message": "Missing/invalid id."}), 400
    pid = int(request.args['id'])
    with db_lock, db_connect() as conn:
        conn.execute("DELETE FROM effect_presets WHERE id = ?", (pid,))
    log.emit("PRESET    delete id=%d" % pid)
    return jsonify(db_effect_presets())


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


def _int_arg(name, default, lo=None, hi=None):
    """Parse an integer query arg, falling back to `default`, clamped to [lo, hi]."""
    raw = request.args.get(name, '')
    value = int(raw) if raw.lstrip('-').isdigit() else default
    if lo is not None:
        value = max(lo, value)
    if hi is not None:
        value = min(hi, value)
    return value


def _float_arg(name, default, lo=None, hi=None):
    """Parse a float query arg, falling back to `default`, clamped to [lo, hi]."""
    raw = request.args.get(name, '')
    try:
        value = float(raw)
    except ValueError:
        value = default
    if lo is not None:
        value = max(lo, value)
    if hi is not None:
        value = min(hi, value)
    return value


def run_effect(gen, ordered, data, stagger, lead, waves, gap, full_go):
    """Pipelined sweeps (runs in its own thread).

    Each panel loops on its OWN period (full_go + gap), phase-offset by k*stagger, and we
    deliver each panel's next schedule ~lead ms before its start instant. So the first
    panel restarts the moment IT finishes -- while the last panel is still playing the
    previous pass -- and repeats have no whole-wave gap. Stops early if a newer effect
    (generation) supersedes this one.
    """
    global global_seq
    period = full_go + gap
    t0 = time.monotonic()
    base = (server_now() + lead) & 0xFFFFFFFF
    # Build every panel's fire events up front; deliver them in time order. Each panel k
    # plays at base + k*stagger + w*period, sent at real time t0 + that offset (which is
    # `lead` ms before the start instant, the headroom for the /play to arrive).
    events = []  # (send_offset_seconds, ip, at)
    for k, (ip, _cid) in enumerate(ordered):
        for w in range(waves):
            off_ms = k * stagger + w * period
            at = (base + off_ms) & 0xFFFFFFFF
            events.append((off_ms / 1000.0, ip, at))
    events.sort()
    for send_rel, ip, at in events:
        dt = t0 + send_rel - time.monotonic()
        if dt > 0:
            time.sleep(dt)
        with state_lock:
            if gen != effect_generation:
                return                       # superseded by a newer effect / stopped
            global_seq += 1
            seq = global_seq
        broadcast_play_each([(ip, {'seq': seq, 'at': at, 'data': data})])


@app.route("/effect")
def effect():
    """Staggered, time-coded effect: schedule the same glyph on every panel at a slightly
    different start time so it appears to travel across the wall.

    Query params (all optional):
      data     glyph/text to sweep (default '*'). '* * *' = a stream of stars;
               '@cyan*' = a coloured star; '*..' = a star with a little tail.
      stagger  delay between adjacent panels:
                 'auto' (default) = one full display of the content on a node ('a full
                   go'), from scroll_duration_ms() which sums each letter's real width via
                   the getCharWidth hint, so narrow letters (i, l, !, .) stagger correctly;
                 'tile' = ~one panel width (TILE_STAGGER_MS, 900 ms), so the content tiles
                   seamlessly into one long continuous marquee across the whole wall
                   (leaves one node, enters the next);
                 a number = fixed ms.
      factor   scales the auto/tile stagger (default 1.0): for 'auto', 1 = clean hop, <1
               overlaps into a glide, >1 leaves a gap; for 'tile', <1 tightens the overlap,
               >1 opens it up to compensate for a physical bezel gap between panels.

    Note: panels scroll right->left, so reverse defaults to ON -- the natural id order
    (or the list from /identify) then travels the way you read the wall. Either leave
    `order` empty / natural with reverse on, or list it right-to-left with reverse=0.
      reverse  flip the sweep direction across the panel order. DEFAULT ON, because the
               panels scroll right->left; pass reverse=0 to disable.
      lead     ms before each (re)start -- the delay from trigger to first pixel, and the
               blank between repeated waves (default DISPLAY_LEAD_MS, 2000). It exists so
               every panel receives the /play before the start instant; for a handful of
               local panels a few hundred ms is plenty, so lower it for a snappier restart.
      waves    replay the whole sweep this many times (default 1).
      gap      extra ms of darkness between waves, on top of lead (default 0).
      order    explicit comma-separated client IDs giving the sweep order
               (default: ascending client id). Use this to match your physical layout.

    Examples: /effect                 -> one '*' hopping panel to panel (auto stagger)
              /effect?data=Hallo      -> the word travels the wall, timed to its letters
              /effect?factor=0.5      -> overlap into a continuous glide
              /effect?stagger=700&waves=3
    """
    global effect_until, effect_generation

    data = request.args.get('data', '*')[:MAX_TEXT_LENGTH]

    # Universal stagger: by default, delay each panel by one full display of the content
    # ('a full go'). scroll_duration_ms() already accounts for variable letter widths via
    # the getCharWidth hint, so this is the content-aware "universal stagger". `factor`
    # tunes it; a numeric `stagger` overrides with a fixed value in ms.
    factor = _float_arg('factor', 1.0, lo=0.05, hi=5.0)
    full_go = scroll_duration_ms(data)
    raw_stagger = request.args.get('stagger', 'auto').strip().lower()
    if raw_stagger.lstrip('-').isdigit():
        stagger = max(0, min(5000, int(raw_stagger)))
        stagger_mode = 'fixed'
    elif raw_stagger == 'tile':
        # Continuous "long display": offset each panel by ~one panel width so the content
        # tiles seamlessly across the wall -- what scrolls off one node enters the next,
        # turning the panels into one long marquee. factor < 1 tightens the overlap; > 1
        # opens it up to compensate for a physical gap/bezel between panels.
        stagger = max(1, min(5000, round(TILE_STAGGER_MS * factor)))
        stagger_mode = 'tile'
    else:  # 'auto' (default): one full display of the content per panel (a discrete hop)
        stagger = max(1, min(5000, round(full_go * factor)))
        stagger_mode = 'auto'

    lead = _int_arg('lead', DISPLAY_LEAD_MS, lo=0, hi=10000)
    waves = _int_arg('waves', 1, lo=1, hi=EFFECT_MAX_WAVES)
    gap = _int_arg('gap', 0, lo=0, hi=10000)
    # Default ON: panels scroll right->left, so reversing the natural id order (0,1,2,..)
    # makes the sweep/marquee travel the way you read the wall. Pass reverse=0 to disable.
    reverse = request.args.get('reverse', '1') not in ('0', '', 'false', 'False')

    with state_lock:
        targets = snapshot_targets()  # [(ip, cid), ...] active clients only
        if request.args.get('order'):
            wanted = [int(x) for x in request.args['order'].split(',')
                      if x.lstrip('-').isdigit()]
            by_id = {cid: ip for ip, cid in targets}
            ordered = [(by_id[c], c) for c in wanted if c in by_id]
        else:
            ordered = sorted(targets, key=lambda t: t[1])  # by client id
        if reverse:
            ordered = list(reversed(ordered))

        if not ordered:
            return jsonify({"status": "error", "message": "No active clients."}), 400

        n = len(ordered)
        period = full_go + gap
        # Pipelined: the last panel's last wave starts at lead + (n-1)*stagger +
        # (waves-1)*period and then scrolls for full_go.
        total_ms = lead + (n - 1) * stagger + (waves - 1) * period + full_go
        effect_generation += 1            # supersede any effect already running
        my_gen = effect_generation
        # Hold the sequencer off for the whole effect so it cannot overwrite the sweep.
        effect_until = time.monotonic() + total_ms / 1000.0

    log.emit("EFFECT    start data=%r panels=%d stagger=%dms(%s) waves=%d (~%.1fs)"
             % (data, n, stagger, stagger_mode, waves, total_ms / 1000.0))
    threading.Thread(target=run_effect,
                     args=(my_gen, ordered, data, stagger, lead, waves, gap, full_go),
                     daemon=True).start()

    return jsonify({"status": "ok", "panels": n, "order": [cid for _ip, cid in ordered],
                    "stagger_ms": stagger, "stagger_mode": stagger_mode,
                    "full_go_ms": full_go, "factor": factor,
                    "waves": waves, "data": data, "duration_ms": total_ms})


IDENTIFY_LEAD_MS = 500   # small lead: ids loop continuously, so keep the blank gap short


def run_identify(seconds, repeat, fmt):
    """Keep each panel showing its OWN id for `seconds` (runs in its own thread).

    Every cycle re-schedules each panel with its id string (a fresh seq, the same start
    instant for all) so the ids stay on screen continuously while you walk the wall.
    Re-reads the client list each cycle so a panel that (re)joins mid-run gets labelled too.
    """
    global global_seq
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        with state_lock:
            targets = snapshot_targets()
            global_seq += 1
            seq = global_seq
            at = (server_now() + IDENTIFY_LEAD_MS) & 0xFFFFFFFF
            items = [(ip, {'seq': seq, 'at': at, 'data': fmt.format(id=cid) * repeat})
                     for ip, cid in targets]
        if not items:
            time.sleep(0.2)
            continue
        broadcast_play_each(items)
        # Pace to the longest id string so every panel finishes before the next cycle.
        cycle = max(scroll_duration_ms(p['data']) for _ip, p in items)
        time.sleep((IDENTIFY_LEAD_MS + cycle) / 1000.0)
    log.emit("IDENTIFY  done")


@app.route("/identify")
def identify():
    """Diagnostic: show every panel its OWN client id so you can read the physical layout
    off the wall and build the /effect order= list.

    Query params (all optional):
      seconds  how long to keep the ids on screen (default 20)
      repeat   how many times the id string repeats, e.g. 3 -> '0 0 0 ' (default 3)
      fmt      Python format for one label; {id} is the client id (default '{id} ').
               e.g. fmt=ID{id}%20 for 'ID0 ', or fmt=%23{id}%20 for '#0 '

    Walk the wall, note each panel's number left-to-right, then drive the sweep with
    /effect?order=<those ids>. Holds the text sequencer off while it runs.
    """
    global effect_until
    seconds = _int_arg('seconds', 20, lo=1, hi=600)
    repeat = _int_arg('repeat', 3, lo=1, hi=20)
    fmt = request.args.get('fmt', '{id} ')
    # Validate the format string against an injection / bad-field mistake before we loop.
    try:
        fmt.format(id=0)
    except (KeyError, IndexError, ValueError):
        return jsonify({"status": "error",
                        "message": "Invalid fmt; use {id}, e.g. '{id} '."}), 400

    with state_lock:
        targets = snapshot_targets()
        if not targets:
            return jsonify({"status": "error", "message": "No active clients."}), 400
        effect_until = time.monotonic() + seconds  # reuse the sequencer-hold mechanism

    panels = sorted(cid for _ip, cid in targets)
    log.emit("IDENTIFY  start panels=%d seconds=%d ids=%s" % (len(targets), seconds, panels))
    threading.Thread(target=run_identify, args=(seconds, repeat, fmt), daemon=True).start()
    return jsonify({"status": "ok", "panels": panels, "seconds": seconds})


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


@app.route("/getConfig")
def get_config():
    # All live-adjustable config values (extend this dict as more are added).
    return jsonify({"max_plays": MAX_PLAYS, "default_plays": DEFAULT_PLAYS})


def _parse_plays_value():
    """Validate the ?value= arg shared by the play-count config setters.
    Returns (value, None) on success or (None, (msg, status)) on error."""
    raw = request.args.get("value", "")
    if not raw.lstrip("-").isdigit():
        return None, ("Missing or non-numeric value parameter.", 400)
    value = int(raw)
    if value < MIN_MAX_PLAYS or value > MAX_MAX_PLAYS:
        return None, ("Value out of range (%d..%d)." % (MIN_MAX_PLAYS, MAX_MAX_PLAYS), 400)
    return value, None


@app.route("/setMaxPlays")
def set_max_plays():
    # Hard ceiling: no string ever plays more than this (effective = min(per-string,
    # ceiling)). Takes effect on the next sequencer pass -- lowering it throttles every
    # string down to the new cap, raising it lets higher per-string limits resume.
    global MAX_PLAYS
    value, err = _parse_plays_value()
    if err:
        return err
    with state_lock:
        MAX_PLAYS = value
    db_set_config("max_plays", value)
    log.emit("CONFIG    max_plays (ceiling) -> %d" % value)
    return jsonify({"max_plays": MAX_PLAYS})


@app.route("/setDefaultPlays")
def set_default_plays():
    # Preset repeat count inserted into a new string when the send omits one. Does not
    # touch existing strings or the ceiling.
    global DEFAULT_PLAYS
    value, err = _parse_plays_value()
    if err:
        return err
    with state_lock:
        DEFAULT_PLAYS = value
    db_set_config("default_plays", value)
    log.emit("CONFIG    default_plays -> %d" % value)
    return jsonify({"default_plays": DEFAULT_PLAYS})


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
            # An effect is sweeping the wall: hold off so we don't overwrite it. Leaving
            # text=None routes us to the idle sleep below, same as an empty queue.
            n = 0 if time.monotonic() < effect_until else len(sent_strings)
            if n:
                if cur_index >= n:
                    cur_index = 0
                start = cur_index
                found = False
                for _ in range(n):
                    # Effective cap = per-string limit, but never above the global
                    # ceiling MAX_PLAYS (a live master throttle over the whole wall).
                    if play_counts[cur_index] < min(play_limits[cur_index], MAX_PLAYS):
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
