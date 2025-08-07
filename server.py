from flask import Flask, request, jsonify, send_from_directory
from datetime import datetime
import time
import threading
import requests
import os

app = Flask(__name__)

# Configuration
MAX_SENT_STRINGS = 20
HEARTBEAT_TIMEOUT = 80  # seconds
CLEANUP_INTERVAL = 30   # seconds
PREMADE_STRINGS = ["Hallo", "Test", "LED", "Scroll", "Ping"]

# State
sent_strings = ["" for _ in range(MAX_SENT_STRINGS)]
play_counts = [0 for _ in range(MAX_SENT_STRINGS)]
sent_index = 0
sent_count = 0
registered_clients = {}  # ip -> {id, last_seen}

next_client_id = 0

@app.route("/")
def serve_index():
    return send_from_directory('.', 'index.html')

@app.route("/getData")
def get_data():
    ordered = []
    for i in range(sent_count):
        index = (sent_index - sent_count + i + MAX_SENT_STRINGS) % MAX_SENT_STRINGS
        ordered.append(sent_strings[index])
    return jsonify(ordered)

@app.route("/getPreMade")
def get_pre_made():
    return jsonify(PREMADE_STRINGS)

@app.route("/SendData")
def send_data():
    global sent_index, sent_count
    if 'data' not in request.args:
        return jsonify({"status": "error", "message": "No data provided."}), 400

    data = request.args['data']
    sent_strings[sent_index] = data
    play_counts[sent_index] = 0
    sent_index = (sent_index + 1) % MAX_SENT_STRINGS
    if sent_count < MAX_SENT_STRINGS:
        sent_count += 1
    return jsonify({"status": "ok", "message": "Command received and processed."})

@app.route("/clear")
def clear():
    global sent_index, sent_count
    for i in range(MAX_SENT_STRINGS):
        sent_strings[i] = ""
        play_counts[i] = 0
    sent_index = 0
    sent_count = 0
    return "Sent strings cleared.", 200

@app.route("/register")
def register():
    global next_client_id
    ip = request.remote_addr
    now = time.time()
    if ip not in registered_clients:
        registered_clients[ip] = {'id': next_client_id, 'last_seen': now}
        response = f"Registered successfully. Your ID: {next_client_id}"
        next_client_id += 1
    else:
        registered_clients[ip]['last_seen'] = now
        response = f"Already registered. Your ID: {registered_clients[ip]['id']}"
    return response

@app.route("/heartbeat")
def heartbeat():
    if 'index' not in request.args:
        return "No index provided.", 400
    ip = request.remote_addr
    idx = int(request.args['index'])
    if ip not in registered_clients or registered_clients[ip]['id'] != idx:
        return "Invalid client ID.", 400
    registered_clients[ip]['last_seen'] = time.time()
    return "Heartbeat acknowledged.", 200

@app.route("/resetPlayCount")
def reset_play_count():
    if 'indexes' in request.args:
        indexes = [int(i) for i in request.args['indexes'].split(',') if i.isdigit()]
    else:
        indexes = list(range(MAX_SENT_STRINGS))
    for idx in indexes:
        if 0 <= idx < MAX_SENT_STRINGS:
            play_counts[idx] = 0
    return "Play counts reset.", 200

@app.route("/deleteSelected")
def delete_selected():
    if 'indexes' not in request.args:
        return "Missing indexes parameter.", 400
    indexes = [int(i) for i in request.args['indexes'].split(',') if i.isdigit()]
    indexes.sort(reverse=True)
    global sent_count, sent_index
    for idx in indexes:
        if 0 <= idx < sent_count:
            real_idx = (sent_index - sent_count + idx + MAX_SENT_STRINGS) % MAX_SENT_STRINGS
            for j in range(real_idx, MAX_SENT_STRINGS - 1):
                sent_strings[j] = sent_strings[j + 1]
                play_counts[j] = play_counts[j + 1]
            sent_strings[MAX_SENT_STRINGS - 1] = ""
            play_counts[MAX_SENT_STRINGS - 1] = 0
            sent_count -= 1
            sent_index = (sent_index - 1 + MAX_SENT_STRINGS) % MAX_SENT_STRINGS
    return get_data()

def cleanup_stale_clients():
    while True:
        now = time.time()
        to_remove = [ip for ip, c in registered_clients.items()
                     if now - c['last_seen'] > HEARTBEAT_TIMEOUT]
        for ip in to_remove:
            print(f"Removing stale client: {ip}")
            del registered_clients[ip]
        time.sleep(CLEANUP_INTERVAL)

if __name__ == '__main__':
    threading.Thread(target=cleanup_stale_clients, daemon=True).start()
    app.run(host='0.0.0.0', port=80)
