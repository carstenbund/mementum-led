#include "ws_wifi.h"
//#include <esp_spiffs.h>
#include <SPIFFS.h>
#include <HTTPClient.h>

IPAddress apIP(10, 10, 10, 1);    // Set the IP address of the AP

char ipStr[16];
WebServer server(80);                               

bool isAPMode = true; // Set to false for client mode
String serverAddress = "http://10.10.10.1"; // Server address for registration and heartbeat
unsigned long lastHeartbeat = 0; // Tracks the last time the heartbeat was sent
const unsigned long heartbeatInterval = 30000; // Heartbeat interval in milliseconds (30 seconds)

int clientId = -1; // Client's assigned ID from the server

struct registeredClient {
    IPAddress ip;
    int id;
    uint32_t timestamp; // Last known activity time
};

std::vector<registeredClient> registeredClients;


struct BroadcastTaskParameters {
    String command;      // Command to be sent
    IPAddress clientIP;  // Client's IP address
    int clientId;        // Client's ID
    int repeat;          // How many times to run
    unsigned long timecode; // For scheduling or later use
    String route;         // Dynamic route for the request
};



const unsigned long HEARTBEAT_TIMEOUT = 120000; // 2 minutes in milliseconds
unsigned long lastCleanup = 0;                 // Last cleanup time
const unsigned long CLEANUP_INTERVAL = 30000;  // Cleanup interval (30 seconds)


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

void sendCommandToClient(void *parameters) {
    BroadcastTaskParameters *taskParams = (BroadcastTaskParameters *)parameters;

    for (int i = 0; i < taskParams->repeat; i++) {
        HTTPClient http;
        String url = "http://" + taskParams->clientIP.toString() + taskParams->route;
        String encodedCommand = urlEncode(taskParams->command);
        if (!taskParams->command.isEmpty() && taskParams->route == "/SendData") {
            url += "?data=" + encodedCommand;
        }

        printf("Broadcasting to client ID=%d, URL=%s, Attempt=%d/%d, Timecode=%lu\n",
               taskParams->clientId, url.c_str(), i + 1, taskParams->repeat, taskParams->timecode);

        http.begin(url); // Initialize the HTTP request

        int httpCode = http.GET(); // Send the GET request
        if (httpCode > 0) {
            printf("Command sent to client ID=%d. Response: %s\n",
                   taskParams->clientId, http.getString().c_str());
        } else {
            printf("Failed to send command to client ID=%d. Error: %s\n",
                   taskParams->clientId, http.errorToString(httpCode).c_str());
        }

        http.end(); // Close the connection

        if (i < taskParams->repeat - 1) {
            delay(1000); // Optional delay between repeats
        }
    }

    delete taskParams; // Clean up allocated memory
    vTaskDelete(NULL); // End the task
}


void broadcastCommand(const String &command, int repeat = 1, unsigned long timecode = millis(), const String &route = "/SendData") {
    for (const auto &client : registeredClients) {
        BroadcastTaskParameters *taskParams = new BroadcastTaskParameters{
            command,        // Command
            client.ip,      // Client IP
            client.id,      // Client ID
            repeat,         // Repeat count
            timecode,       // Timecode
            route           // Route for the request
        };

        xTaskCreate(
            sendCommandToClient,         // Task function
            "BroadcastTask",             // Name of the task
            4096,                        // Stack size (in bytes)
            taskParams,                  // Parameters passed to the task
            1,                           // Task priority
            NULL                         // Task handle
        );
    }
}


void registerWithServer() {
    HTTPClient http;
    String registerEndpoint = serverAddress + "/register";
    http.begin(registerEndpoint);

    int httpCode = http.GET();
    if (httpCode > 0) {
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
        // Assign a new ID based on the vector size
        int clientId = registeredClients.size();
        registeredClients.push_back({clientIP, clientId, currentTime});
        printf("New client registered: ID=%d, IP=%s\r\n", clientId, clientIP.toString().c_str());
        server.send(200, "text/plain", "Registered successfully. Your ID: " + String(clientId));
    } else {
        // Update timestamp for existing client
        it->timestamp = currentTime;
        printf("Updated client: ID=%d, IP=%s\r\n", it->id, clientIP.toString().c_str());
        server.send(200, "text/plain", "Already registered. Your ID: " + String(it->id));
    }
}


void handleHeartbeat() {
    if (server.hasArg("index")) {
        int clientId = server.arg("index").toInt();
        printf("Heartbeat received from client ID: %d\n", clientId);

        if (clientId >= 0 && clientId < registeredClients.size()) {
            registeredClients[clientId].timestamp = millis();
            server.send(200, "text/plain", "Heartbeat acknowledged.");
        } else {
            server.send(400, "text/plain", "Invalid client ID.");
        }
    } else {
        server.send(400, "text/plain", "No index provided.");
    }
}

void sendHeartbeat() {
    if (serverAddress.isEmpty() || clientId == -1) {
        printf("Server address not set or client not registered. Cannot send heartbeat.\n");
        printf("ClientID: %n, Server Address: %s\n", clientId, serverAddress.c_str());
        return;
    }

    HTTPClient http;
    String heartbeatEndpoint = serverAddress + "/heartbeat?index=" + String(clientId);

    printf("Sending heartbeat to server: %s\n", heartbeatEndpoint.c_str());
    http.begin(heartbeatEndpoint);
    // Send GET request
    int httpCode = http.GET();
    if (httpCode > 0) {
        printf("Heartbeat acknowledged by server. Response: %s\n", http.getString().c_str());
    } else {
        printf("Failed to send heartbeat. Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}


void cleanupStaleClients() {
    unsigned long currentMillis = millis();
    
    // Only perform cleanup at intervals
    if (currentMillis - lastCleanup < CLEANUP_INTERVAL) {
        return;
    }
    lastCleanup = currentMillis;

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
        int indexes[MAX_SENT_STRINGS];
        int count = 0;

        // Parse the indexes from the query parameter (e.g., "0,2,4")
        char* token = strtok(indexesArg.begin(), ",");
        while (token != NULL && count < MAX_SENT_STRINGS) {
            indexes[count++] = atoi(token);
            token = strtok(NULL, ",");
        }

        // Sort indexes in descending order to avoid shifting issues
        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (indexes[i] < indexes[j]) {
                    int temp = indexes[i];
                    indexes[i] = indexes[j];
                    indexes[j] = temp;
                }
            }
        }

        // Delete the strings at the specified indexes
        for (int i = 0; i < count; i++) {
            int idx = indexes[i];
            if (idx >= 0 && idx < sentCount) {
                for (int j = idx; j < sentCount - 1; j++) {
                    sentStrings[j] = sentStrings[j + 1];
                }
                sentStrings[sentCount - 1] = ""; // Clear the last entry
                sentCount--;
                sentIndex = (sentIndex - 1 + MAX_SENT_STRINGS) % MAX_SENT_STRINGS;
            }
        }

        // Send the updated list
        handleGetData();
    } else {
        server.send(400, "text/plain", "Missing indexes parameter.");
    }
}



void clearSentStrings() {
    // Reset all strings in the array
    for (int i = 0; i < MAX_SENT_STRINGS; i++) {
        sentStrings[i] = ""; // Clear the string
    }

    // Reset the index and count
    sentIndex = 0;
    sentCount = 0;

    // Debug log
    printf("All sent strings cleared.\n");
}

void handleClearStrings() {
    if (isAPMode) {
            // Only broadcast in server mode
            printf("Broadcasting command: clear\n");
            broadcastCommand("", 1, millis(), "/clear");
    }
    else { printf("clear local\n"); }
    clearSentStrings(); // Clear the local buffer
    Text[0] = '\0';
    Matrix.fillScreen(0);
    Matrix.show();
    server.send(200, "text/plain", "Sent strings cleared and broadcasted.");
}


void handleSendData() {
    if (server.hasArg("data")) {
        String newData = server.arg("data");

        // Process the command locally
        handleSwitch(1); 
        if (isAPMode) {
            // Only broadcast in server mode
            printf("Broadcasting command: %s\n", newData.c_str());
            broadcastCommand(newData);
        } else {
            printf("Skipping broadcast; device is in client mode.\n");
        }
        server.send(200, "text/plain", "Command received and processed.");
    } else {
        server.send(400, "text/plain", "No data provided.");
    }
}


void handleGetData() {
  String json = "[";
  for (int i = 0; i < sentCount; i++) {
    int index = (sentIndex - sentCount + i + MAX_SENT_STRINGS) % MAX_SENT_STRINGS;
    json += "\"" + sentStrings[index] + "\"";
    if (i < sentCount - 1) {
      json += ",";
    }
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

        // Store the newData in the sentStrings buffer
        sentStrings[sentIndex] = newData;
        sentIndex = (sentIndex + 1) % MAX_SENT_STRINGS;
        if (sentCount < MAX_SENT_STRINGS) {
          sentCount++;
        }
      }
      Flow_Flag = true;
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
        WiFi.mode(WIFI_STA);
        WiFi.begin(apSSID, apPSK); // Replace with network credentials

        printf("Connecting to WiFi...\n");
        while (WiFi.status() != WL_CONNECTED) {
            delay(1000);
            printf(".");
        }

        printf("\nConnected to WiFi. IP Address: %s\n", WiFi.localIP().toString().c_str());

        // Register with the server
        registerWithServer();
    }

  server.on("/"           ,handleRoot);        
  server.on("/getData"    , handleGetData);
  server.on("/SendData"   , handleSendData);
  server.on("/register"   , handleRegister);
  server.on("/RGBOn"      , handleRGBOn);
  server.on("/RGBOff"     , handleRGBOff);
  server.on("/heartbeat"  , handleHeartbeat);
  server.on("/clear", handleClearStrings);
  server.on("/deleteSelected", HTTP_POST, handleDeleteSelected);


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
}

void WIFI_Loop() {
    server.handleClient(); // Process incoming HTTP requests

    unsigned long currentMillis = millis();

    if (!isAPMode) {
        // Client Mode: Send heartbeat at intervals
        if (currentMillis - lastHeartbeat >= heartbeatInterval) {
            lastHeartbeat = currentMillis;
            sendHeartbeat();
        }
    } else {
        // AP Mode: Cleanup stale clients at intervals
        if (currentMillis - lastCleanup >= CLEANUP_INTERVAL) {
            lastCleanup = currentMillis;
            cleanupStaleClients();
        }
    }
}
