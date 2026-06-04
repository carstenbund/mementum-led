#include "ws_wifi.h"
//#include <esp_spiffs.h>
#include <SPIFFS.h>
#include <HTTPClient.h>

IPAddress apIP(10, 10, 10, 1);    // Set the IP address of the AP

char ipStr[16];
WebServer server(80);                               

bool isAPMode = true; // Set to false for client mode
bool isConnected = false;
String serverAddress = "http://10.10.10.1"; // Server address for registration and heartbeat
unsigned long lastHeartbeat = 0; // Tracks the last time the heartbeat was sent
const unsigned long heartbeatInterval = 30000; // Heartbeat interval in milliseconds (30 seconds)

const unsigned long HEARTBEAT_TIMEOUT = 80000; // 2 minutes in milliseconds
unsigned long lastCleanup = 0;                 // Last cleanup time
const unsigned long CLEANUP_INTERVAL = 30000;  // Cleanup interval (30 seconds)

int clientId = -1; // Client's assigned ID from the server

// ---- Shared clock (Model A) ----
// serverNow() returns time in the server's millis() domain. On the server the offset is
// 0; clients estimate it from the server via syncClock(). All scheduling/rendering uses
// this single domain so devices stay phase-locked.
int32_t clockOffset = 0;
bool clockSynced = false;

uint32_t serverNow() {
    return millis() + (uint32_t)clockOffset;
}

uint32_t globalSeq = 0; // monotonic schedule sequence number (server side)

struct registeredClient {
    IPAddress ip;
    int id;
    uint32_t timestamp; // Last known activity time
};

std::vector<registeredClient> registeredClients;
int nextClientId = 0; // Monotonic ID counter; never reused, so IDs stay unique after cleanup

struct BroadcastTarget {
    IPAddress ip;
    int id;
};

struct BroadcastJob {
    String command;                      // Command/text to send (only used for /SendData)
    String route;                        // Dynamic route for the request
    int repeat;                          // How many times to run
    unsigned long timecode;              // For scheduling (reserved for time-sync feature)
    std::vector<BroadcastTarget> targets; // Snapshot of clients at enqueue time
};

String urlEncode(const String &str) {
    String encodedString = "";
    char c;
    char bufHex[4];
    for (size_t i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encodedString += c;
        } else {
            sprintf(bufHex, "%%%02X", c);
            encodedString += bufHex;
        }
    }
    return encodedString;
}

// Single broadcast task: iterates over the snapshot of clients sequentially.
// This avoids spawning one 4 KB task per client (heap exhaustion with ~20 clients)
// and serializes the radio access so failures are visible and bounded.
void broadcastTask(void *parameters) {
    BroadcastJob *job = (BroadcastJob *)parameters;
    String encodedCommand = urlEncode(job->command);

    for (int attempt = 0; attempt < job->repeat; attempt++) {
        for (const auto &target : job->targets) {
            HTTPClient http;
            String url = "http://" + target.ip.toString() + job->route;
            if (!job->command.isEmpty() && job->route == "/SendData") {
                url += "?data=" + encodedCommand;
            }

            http.begin(url);
            // Bound the stall caused by a client that has gone away but not yet been
            // cleaned up, so one dead client cannot block the whole broadcast.
            http.setConnectTimeout(2000); // ms to establish the connection
            http.setTimeout(2000);        // ms to wait for the response
            int httpCode = http.GET();
            if (httpCode >= 200 && httpCode < 300) {
                printf("Broadcast OK to client ID=%d, URL=%s, Attempt=%d/%d\n",
                       target.id, url.c_str(), attempt + 1, job->repeat);
            } else {
                printf("Broadcast FAILED to client ID=%d, URL=%s, code=%d (%s)\n",
                       target.id, url.c_str(), httpCode, http.errorToString(httpCode).c_str());
            }
            http.end();
        }
        if (attempt < job->repeat - 1) {
            delay(1000); // Optional delay between repeats
        }
    }

    delete job;        // Clean up allocated memory
    vTaskDelete(NULL); // End the task
}


void broadcastCommand(const String &command, int repeat = 1, unsigned long timecode = 0, const String &route = "/SendData") {
    if (registeredClients.empty()) {
        return; // Nothing to broadcast to
    }

    BroadcastJob *job = new BroadcastJob();
    job->command = command;
    job->route = route;
    job->repeat = repeat;
    job->timecode = timecode;
    for (const auto &client : registeredClients) {
        job->targets.push_back({client.ip, client.id});
    }

    xTaskCreate(
        broadcastTask,   // Task function
        "BroadcastTask", // Name of the task
        8192,            // Stack size (in bytes) - HTTPClient + vector iteration
        job,             // Parameters passed to the task
        1,               // Task priority
        NULL             // Task handle
    );
}


// Server -> clients: schedule a message to start scrolling at displayAt (server clock).
// The whole query (including url-encoded text) is prebuilt into the route so it rides the
// generic broadcast path.
void broadcastPlay(uint32_t seq, const String &text, uint32_t displayAt) {
    String route = "/play?seq=" + String((unsigned long)seq) +
                   "&at=" + String((unsigned long)displayAt) +
                   "&data=" + urlEncode(text);
    broadcastCommand("", 1, displayAt, route);
}


void registerWithServer() {
    HTTPClient http;
    // Report the firmware version so the server log shows which protocol each client
    // speaks. Old (pre-/play) firmware omits this param; the server treats it as optional.
    String registerEndpoint = serverAddress + "/register?version=" + urlEncode(String(Version));
    http.begin(registerEndpoint);

    int httpCode = http.GET();
    if (httpCode >= 200 && httpCode < 300) {
        String response = http.getString();
        if (response.startsWith("Registered successfully. Your ID: ")) {
            clientId = response.substring(response.lastIndexOf(":") + 2).toInt();
            printf("Registered successfully. Assigned ID: %d\n", clientId);
        } else if (response.startsWith("Already registered. Your ID: ")) {
            clientId = response.substring(response.lastIndexOf(":") + 2).toInt();
            printf("Already registered. Assigned ID: %d\n", clientId);
        } else {
            printf("Unexpected response: %s\n", response.c_str());
        }
    } else {
        printf("Failed to register with server. Error: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
}

// Estimate the server-clock offset using Cristian's algorithm. Takes a few samples and
// keeps the one with the smallest round-trip time (least uncertainty).
void syncClock() {
    if (serverAddress.isEmpty() || !isConnected) {
        return;
    }

    int32_t bestOffset = 0;
    uint32_t bestRtt = UINT32_MAX;

    for (int i = 0; i < 3; i++) {
        HTTPClient http;
        http.setConnectTimeout(2000);
        http.setTimeout(2000);
        uint32_t t0 = millis();
        http.begin(serverAddress + "/time");
        int httpCode = http.GET();
        if (httpCode >= 200 && httpCode < 300) {
            uint32_t serverMs = (uint32_t)strtoul(http.getString().c_str(), nullptr, 10);
            uint32_t t1 = millis();
            uint32_t rtt = t1 - t0;
            if (rtt < bestRtt) {
                bestRtt = rtt;
                // Estimated server time at t1 is serverMs + rtt/2; offset = that - local t1.
                bestOffset = (int32_t)(serverMs + rtt / 2 - t1);
            }
        }
        http.end();
    }

    if (bestRtt != UINT32_MAX) {
        clockOffset = bestOffset;
        clockSynced = true;
        printf("Clock synced. offset=%ld ms, rtt=%lu ms\n", (long)clockOffset, (unsigned long)bestRtt);
    } else {
        printf("Clock sync failed (no /time response).\n");
    }
}


void handleRoot() {
    // Open the HTML file stored in SPIFFS
    File file = SPIFFS.open("/index.html", "r");
    if (!file || file.isDirectory()) {
        // If the file doesn't exist or is a directory, send a 404 error
        server.send(404, "text/plain", "File not found");
        printf("HTML file not found!\n");
        return;
    }

    // Read the file content and send it to the client
    String htmlPage;
    while (file.available()) {
        htmlPage += (char)file.read();
    }
    file.close();

    server.send(200, "text/html", htmlPage); 
    printf("Home page served from SPIFFS\n");
}

void handleRegister() {
    IPAddress clientIP = server.client().remoteIP();
    uint32_t currentTime = millis();

    // Check if the client is already registered
    auto it = std::find_if(registeredClients.begin(), registeredClients.end(),
        [&](const registeredClient &client) { return client.ip == clientIP; });

    if (it == registeredClients.end()) {
        // Assign a new monotonic ID. Using the vector size would reuse IDs after a
        // cleanup erases an entry, causing ID collisions and heartbeat mismatches.
        int newId = nextClientId++;
        registeredClients.push_back({clientIP, newId, currentTime});
        printf("New client registered: ID=%d, IP=%s\r\n", newId, clientIP.toString().c_str());
        server.send(200, "text/plain", "Registered successfully. Your ID: " + String(newId));
    } else {
        // Update timestamp for existing client
        it->timestamp = currentTime;
        printf("Updated client: ID=%d, IP=%s\r\n", it->id, clientIP.toString().c_str());
        server.send(200, "text/plain", "Already registered. Your ID: " + String(it->id));
    }
}

void handleHeartbeat() {
    // Identify the client by source IP, not by the reported index. IDs are no longer
    // equal to the vector position (cleanup can erase middle entries), so indexing the
    // vector by ID would refresh the wrong client or read out of bounds.
    IPAddress clientIP = server.client().remoteIP();
    auto it = std::find_if(registeredClients.begin(), registeredClients.end(),
        [&](const registeredClient &client) { return client.ip == clientIP; });

    if (it != registeredClients.end()) {
        it->timestamp = millis();
        printf("Heartbeat from client ID=%d, IP=%s\n", it->id, clientIP.toString().c_str());
        server.send(200, "text/plain", "Heartbeat acknowledged.");
    } else {
        // Unknown client (e.g. it was cleaned up). Tell it to re-register.
        printf("Heartbeat from unknown IP=%s; requesting re-register\n", clientIP.toString().c_str());
        server.send(400, "text/plain", "Unknown client. Please re-register.");
    }
}

unsigned long dynamicHeartbeatInterval = heartbeatInterval;

void sendHeartbeat() {
    if (serverAddress.isEmpty() || clientId == -1 || !isConnected) {
        printf("Cannot send heartbeat. Server not set or not connected.\n");
        return;
    }

    HTTPClient http;
    String heartbeatEndpoint = serverAddress + "/heartbeat?index=" + String(clientId);

    printf("Sending heartbeat to server: %s\n", heartbeatEndpoint.c_str());
    http.begin(heartbeatEndpoint);
    int httpCode = http.GET();

    // Only a 2xx response is a real acknowledgement. An HTTP error status (e.g. 400
    // "Unknown client") is a positive httpCode but means the server dropped us, so we
    // must drop the connection flag and re-register on the next loop.
    if (httpCode >= 200 && httpCode < 300) {
        printf("Heartbeat acknowledged: %s\n", http.getString().c_str());
        dynamicHeartbeatInterval = heartbeatInterval; // Reset interval on success
    } else {
        printf("Heartbeat failed. code=%d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        dynamicHeartbeatInterval = min(dynamicHeartbeatInterval * 2, HEARTBEAT_TIMEOUT); // Increase interval
        clientId = -1;        // Force re-registration to get a fresh ID
        // Do NOT clear isConnected here — WiFi is still up, only the HTTP server
        // went away. The server-reachability retry in WIFI_Loop() handles recovery.
    }
    http.end();
}

void cleanupStaleClients() {
    // Interval gating is handled by the caller (WIFI_Loop). Doing it again here with
    // the lastCleanup the caller just updated would make this return immediately and
    // never actually remove anyone.
    unsigned long currentMillis = millis();

    // Iterate through the registered clients and remove stale ones
    for (auto it = registeredClients.begin(); it != registeredClients.end();) {
        if (currentMillis - it->timestamp > HEARTBEAT_TIMEOUT) {
            printf("Removing stale client: ID=%d, IP=%s\n", it->id, it->ip.toString().c_str());
            it = registeredClients.erase(it); // Remove the stale client
        } else {
            ++it; // Move to the next client
        }
    }
}

void handleDeleteSelected() {
    if (server.hasArg("indexes")) {
        String indexesArg = server.arg("indexes");

        // Model A: the queue is server-owned. Clients are stateless players that only
        // show scheduled /play items, so no client broadcast is needed here — deleting
        // an entry simply means the sequencer won't schedule it again.

        int indexes[MAX_SENT_STRINGS];
        int count = 0;

        // Parse the indexes from the query parameter (e.g., "0,2,4")
        char* token = strtok(indexesArg.begin(), ",");
        while (token != NULL && count < MAX_SENT_STRINGS) {
            indexes[count++] = atoi(token);
            token = strtok(NULL, ",");
        }

        // Sort indexes in descending order so shifting earlier entries does not
        // invalidate the positions of entries still to be deleted.
        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (indexes[i] < indexes[j]) {
                    int temp = indexes[i];
                    indexes[i] = indexes[j];
                    indexes[j] = temp;
                }
            }
        }

        // Delete the strings at the specified indexes (compact array, index == display order)
        for (int i = 0; i < count; i++) {
            int idx = indexes[i];
            if (idx >= 0 && idx < sentCount) {
                for (int j = idx; j < sentCount - 1; j++) {
                    sentStrings[j] = sentStrings[j + 1];
                    playCount[j] = playCount[j + 1];
                    playLimit[j] = playLimit[j + 1];
                }
                sentStrings[sentCount - 1] = "";
                playCount[sentCount - 1] = 0;
                playLimit[sentCount - 1] = 0;
                sentCount--;
            }
        }

        // Send the updated list
        handleGetData();
    } else {
        server.send(400, "text/plain", "Missing indexes parameter.");
    }
}

void handleResetPlayCount() {
    bool anyPlayCountReset = false; // Track if any play count is reset
    if (server.hasArg("indexes")) {
        // Model A: play counts gate the server-side sequencer only; no client broadcast.
        // Parse the "indexes" query parameter (e.g., "0,2,4")
        String indexesArg = server.arg("indexes");
        std::vector<int> indexes;
        char *token = strtok(indexesArg.begin(), ",");
        while (token != NULL) {
            int index = atoi(token);
            if (index >= 0 && index < MAX_SENT_STRINGS) {
                indexes.push_back(index);
            }
            token = strtok(NULL, ",");
        }

        // Reset play counts for the specified indices
        for (int index : indexes) {
            if (playCount[index] > 0) { // Only reset if it's greater than 0
                playCount[index] = 0;
                anyPlayCountReset = true;
                printf("Play count for index %d has been reset.\n", index);
            }
        }

        server.send(200, "text/plain", "Play counts reset for specified indices.");
    } else {
        // If no indices are provided, reset all play counts
        for (int i = 0; i < MAX_SENT_STRINGS; i++) {
            if (playCount[i] > 0) { // Only reset if it's greater than 0
                playCount[i] = 0;
                anyPlayCountReset = true;
            }
        }

        printf("All play counts have been reset.\n");
        server.send(200, "text/plain", "All play counts reset.");
    }

    // Resume displaying if any play count was reset
    if (anyPlayCountReset) {
        isDisplaying = false; // Ensure the display loop starts fresh
        printf("Display loop reset after play count changes.\n");
    }
}



// Append a string to the compact FIFO queue. When full, drop the oldest entry.
// Crucially, the new slot's play count is reset to 0 so the message is actually shown
// (a reused slot would otherwise keep the previous occupant's expired play count).
void addSentString(const String &s, int limit = -1) {
    int effectiveLimit = (limit > 0) ? limit : default_plays;
    if (sentCount < MAX_SENT_STRINGS) {
        sentStrings[sentCount] = s;
        playCount[sentCount] = 0;
        playLimit[sentCount] = effectiveLimit;
        sentCount++;
    } else {
        // Full: shift everything left by one (drop oldest) and append at the end.
        for (int i = 0; i < MAX_SENT_STRINGS - 1; i++) {
            sentStrings[i] = sentStrings[i + 1];
            playCount[i] = playCount[i + 1];
            playLimit[i] = playLimit[i + 1];
        }
        sentStrings[MAX_SENT_STRINGS - 1] = s;
        playCount[MAX_SENT_STRINGS - 1] = 0;
        playLimit[MAX_SENT_STRINGS - 1] = effectiveLimit;
    }
}

void clearSentStrings() {
    for (int i = 0; i < MAX_SENT_STRINGS; i++) {
        sentStrings[i] = "";
        playCount[i] = 0;
        playLimit[i] = 0;
    }
    sentCount = 0;

    // Debug log
    printf("All sent strings cleared.\n");
}

void handleClearStrings() {
    if (isAPMode) {
            // Tell clients to stop immediately so they blank instead of finishing the
            // currently-scheduled scroll.
            printf("Broadcasting command: clear\n");
            broadcastCommand("", 1, millis(), "/clear");
    }
    else { printf("clear local\n"); }
    clearSentStrings(); // Clear the local buffer
    Text[0] = '\0';
    isDisplaying = false;
    Flow_Flag = false;
    currentSchedule.active = false; // drop any in-flight scheduled message
    currentStringBuffer[0] = '\0';  // (was an out-of-bounds write at index 100)
    Matrix.fillScreen(0);
    Matrix.show();
    server.send(200, "text/plain", "Sent strings cleared and broadcasted.");
}


void handleSendData() {
    if (!server.hasArg("data")) {
        server.send(400, "text/plain", "No data provided.");
        return;
    }

    // Add to the (server-owned) queue. handleSwitch(1) stores the text and sends the
    // single HTTP response ("OK"); do not send again here or the WebServer logs a
    // "response already sent" error on every message.
    //
    // Model A: we do NOT broadcast the raw text to clients here. The server's sequencer
    // distributes each message as a timed /play command (broadcastPlay) so all devices
    // start the same message at the same instant.
    handleSwitch(1);
}


// Reference clock for clients: just our current millis().
void handleTime() {
    server.send(200, "text/plain", String((unsigned long)millis()));
}

// Client-side: receive a timed display command and (re)schedule it. Idempotent on seq.
void handlePlay() {
    if (!server.hasArg("seq") || !server.hasArg("data") || !server.hasArg("at")) {
        server.send(400, "text/plain", "Missing play parameters.");
        return;
    }
    uint32_t seq = (uint32_t)strtoul(server.arg("seq").c_str(), nullptr, 10);
    uint32_t at  = (uint32_t)strtoul(server.arg("at").c_str(), nullptr, 10);

    if (!currentSchedule.active || seq != currentSchedule.seq) {
        setSchedule(seq, server.arg("data").c_str(), at);
        printf("Scheduled seq=%lu at=%lu now=%lu\n",
               (unsigned long)seq, (unsigned long)at, (unsigned long)serverNow());
    }
    server.send(200, "text/plain", "OK");
}

void handleGetData() {
  // Return rich objects matching the Pi server format so the UI can show play badges.
  String json = "[";
  for (int i = 0; i < sentCount; i++) {
    if (i > 0) json += ",";
    json += "{\"text\":\"" + sentStrings[i] + "\""
            ",\"plays\":" + String(playCount[i]) +
            ",\"limit\":" + String(playLimit[i]) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleSwitch(uint8_t ledNumber) {
  switch(ledNumber){
    case 1:
      if (server.hasArg("data")) {
        String newData = server.arg("data");
        newData.toCharArray(Text, sizeof(Text));
        int limit = -1;
        if (server.hasArg("plays")) {
            int v = server.arg("plays").toInt();
            if (v >= 1 && v <= 99) limit = v;
        }
        addSentString(newData, limit);
      }
      Flow_Flag = true;
      isDisplaying = false;
      printf("Text=%s.\r\n",Text);
      break;
    case 2:
      colorWipe(Matrix.Color(0, 255, 0), 0);
      printf("RGB On.\r\n");
      Flow_Flag = false;
      break;
    case 3:
      colorWipe(Matrix.Color(0, 0, 0), 0);
      printf("RGB Off.\r\n");
      Flow_Flag = true;
      break;
  }
  server.send(200, "text/plain", "OK");
}
//void handleSendData()  { handleSwitch(1); }
void handleRGBOn()     { handleSwitch(2); }
void handleRGBOff()    { handleSwitch(3); }
unsigned long retryDelay = 1000; // Start with 1 second
const unsigned long maxRetryDelay = 32000; // Cap at 32 seconds


bool connectToServer() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(apSSID, apPSK);

    printf("Connecting to WiFi...\n");
    unsigned long startAttemptTime = millis();

    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < retryDelay) {
        delay(1000);
        printf(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        printf("\nConnected to WiFi. IP Address: %s\n", WiFi.localIP().toString().c_str());
        retryDelay = 1000; // Reset delay after successful WiFi connection
        isConnected = true;
        return true;
    } else {
        retryDelay = min(retryDelay * 2, maxRetryDelay); // Exponential backoff
        printf("\nFailed to connect to WiFi. Retrying in %lu ms...\n", retryDelay);
        return false;
    }
}

void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case WIFI_EVENT_STA_DISCONNECTED:
            isConnected = false;
            printf("WiFi disconnected. Attempting to reconnect...\n");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            printf("WiFi connected. Waiting for IP...\n");
            break;
        case IP_EVENT_STA_GOT_IP:
            printf("WiFi connected with IP: %s\n", WiFi.localIP().toString().c_str());
            isConnected = true;
            retryDelay = 1000; // Reset retry delay
            registerWithServer();
            syncClock();
            break;
        default:
            break;
    }
}

void handleGetConfig() {
    String json = "{\"max_plays\":" + String(max_plays) +
                  ",\"default_plays\":" + String(default_plays) + "}";
    server.send(200, "application/json", json);
}

void handleSetMaxPlays() {
    if (!server.hasArg("value")) { server.send(400, "text/plain", "Missing value."); return; }
    int v = server.arg("value").toInt();
    if (v < 1 || v > 99) { server.send(400, "text/plain", "Value out of range (1..99)."); return; }
    max_plays = v;
    server.send(200, "application/json", "{\"max_plays\":" + String(max_plays) + "}");
}

void handleSetDefaultPlays() {
    if (!server.hasArg("value")) { server.send(400, "text/plain", "Missing value."); return; }
    int v = server.arg("value").toInt();
    if (v < 1 || v > 99) { server.send(400, "text/plain", "Value out of range (1..99)."); return; }
    default_plays = v;
    server.send(200, "application/json", "{\"default_plays\":" + String(default_plays) + "}");
}

void handleClients() {
    unsigned long now = millis();
    String json = "{\"heartbeat_timeout\":" + String(HEARTBEAT_TIMEOUT / 1000) + ",\"clients\":[";
    for (size_t i = 0; i < registeredClients.size(); i++) {
        const auto &c = registeredClients[i];
        if (i > 0) json += ",";
        json += "{\"id\":" + String(c.id) +
                ",\"ip\":\"" + c.ip.toString() + "\"" +
                ",\"active\":true" +
                ",\"age\":" + String((now - c.timestamp) / 1000.0f, 1) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void webServerTask(void *params) {
    while (true) {
        server.handleClient(); // Process HTTP requests
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield to other tasks (adjust delay as needed)
    }
}


void WIFI_Init()
{
    // The name and password of the WiFi access point
    String ssid = apSSID;            
    String password = apPSK;            

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) { // Use true for automatic format if SPIFFS is unformatted
        printf("Failed to mount SPIFFS\n");
        return;
    }
    printf("SPIFFS mounted successfully\n");

  if (isAPMode) {
        // AP Mode
        WiFi.mode(WIFI_AP);
        String ssid = apSSID;
        String password = apPSK;

        if (!WiFi.softAP(ssid.c_str(), password.c_str())) {
            printf("Soft AP creation failed.\n");
            return;
        }

        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        IPAddress myIP = WiFi.softAP(ssid, password, 1, 0, 16);
        delay(100);
        printf("AP Mode - IP Address: %s\n", myIP.toString().c_str());
    } else {
      // Client Mode
      WiFi.onEvent(WiFiEvent); // Attach the event handler
      connectToServer();
  }

  server.on("/"           ,handleRoot);        
  server.on("/getData"    , handleGetData);
  server.on("/SendData"   , handleSendData);
  server.on("/register"   , handleRegister);
  server.on("/RGBOn"      , handleRGBOn);
  server.on("/RGBOff"     , handleRGBOff);
  server.on("/heartbeat"  , handleHeartbeat);
  server.on("/clear", handleClearStrings);
  server.on("/deleteSelected", handleDeleteSelected);
  server.on("/resetPlayCount", handleResetPlayCount);
  server.on("/time", handleTime);
  server.on("/play", handlePlay);
  server.on("/getConfig",        handleGetConfig);
  server.on("/setMaxPlays",      handleSetMaxPlays);
  server.on("/setDefaultPlays",  handleSetDefaultPlays);
  server.on("/clients",          handleClients);


  // Endpoint to retrieve pre-made strings
  server.on("/getPreMade", HTTP_GET, [](){
      String json = "[";
      for (int i = 0; i < PREMADE_COUNT; i++) {
          json += "\"" + String(predefinedTexts[i]) + "\"";
          if (i < PREMADE_COUNT - 1) {
              json += ",";
          }
      }
      json += "]";
      server.send(200, "application/json", json);
  });

  server.begin(); 
  printf("Web server started\r\n");

   // Create the web server task
    xTaskCreate(
        webServerTask,        // Task function
        "WebServerTask",      // Task name
        4096,                 // Stack size
        NULL,                 // Parameters
        1,                    // Priority
        NULL                  // Task handle
    );
    
}

void WIFI_Loop() {
    //server.handleClient(); // Process incoming HTTP requests

    unsigned long currentMillis = millis();

    if (!isAPMode) {
        if (!isConnected) {
            // WiFi layer is down — retry the physical connection with backoff.
            static unsigned long lastWiFiRetry = 0;
            if (currentMillis - lastWiFiRetry >= retryDelay) {
                lastWiFiRetry = currentMillis;
                if (connectToServer()) {
                    printf("WiFi reconnected.\n");
                }
            }
        } else if (clientId == -1) {
            // WiFi is up but we lost the server registration (server restarted,
            // heartbeat rejected, etc.). Retry registration independently with
            // its own backoff so WiFi state is not touched.
            static unsigned long lastServerRetry = 0;
            static unsigned long serverRetryDelay = 1000;
            if (currentMillis - lastServerRetry >= serverRetryDelay) {
                lastServerRetry = currentMillis;
                registerWithServer();
                if (clientId != -1) {
                    serverRetryDelay = 1000; // reset on success
                    syncClock();
                    printf("Re-registered with server.\n");
                } else {
                    serverRetryDelay = min(serverRetryDelay * 2, maxRetryDelay);
                    printf("Server still unreachable. Retrying in %lu ms...\n", serverRetryDelay);
                }
            }
        }
      
        // Client Mode: Send heartbeat at intervals and refresh the clock offset
        if (currentMillis - lastHeartbeat >= heartbeatInterval) {
            lastHeartbeat = currentMillis;
            sendHeartbeat();
            if (isConnected) {
                syncClock(); // keep the shared clock from drifting between reconnects
            }
        }
    } else {
        // AP Mode: Cleanup stale clients at intervals
        if (currentMillis - lastCleanup >= CLEANUP_INTERVAL) {
            lastCleanup = currentMillis;
            cleanupStaleClients();
        }
    }
}
