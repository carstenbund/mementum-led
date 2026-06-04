#include <Adafruit_NeoPixel.h>
#include "ws_flow.h"
#include "ws_wifi.h"
#include <FS.h>
#include <SPIFFS.h>

char Project[20] = "mementumLED";
char Version[20] = "1.3";

char apSSID[64] = "ESP32-S3-Matrix";
char apPSK[64]  = "waveshare";

char Text[100] = "mementum";

// Display-related global variables
int currentStringIndex = 0;
char currentStringBuffer[100] = {0};

bool isDisplaying = false;
volatile bool Flow_Flag = false; // Flow flag for display logic


// Compact FIFO message queue: valid entries live at [0, sentCount)
String sentStrings[MAX_SENT_STRINGS];
int sentCount = 0;

int max_plays    = 3; // global ceiling: no string ever plays more than this
int default_plays = 3; // preset for new strings when no per-string count is supplied

std::vector<String> fragments; // editable preset library; loaded from /fragments.csv at boot

// Per-entry play counts and per-string repeat caps, parallel to sentStrings
int playCount[MAX_SENT_STRINGS] = {0};
int playLimit[MAX_SENT_STRINGS] = {0}; // set at send time; effective = min(playLimit, max_plays)

// Function to load configuration from SPIFFS (config.csv)
void loadConfigFromCSV() {
    if (!SPIFFS.begin(true)) { // Initialize SPIFFS
        printf("Failed to mount SPIFFS\n");
        return;
    }

    File file = SPIFFS.open("/config.csv", "r");
    if (!file) {
        printf("Failed to open config.csv\n");
        return;
    }

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim(); // Remove whitespace and newlines

        int separatorIndex = line.indexOf(',');
        if (separatorIndex > 0) {
            String key = line.substring(0, separatorIndex);
            String value = line.substring(separatorIndex + 1);
            printf("%s: %s\n", key.c_str(), value.c_str());

           // Map keys to variables
            if (key == "APSSID") {
                value.toCharArray(apSSID, sizeof(apSSID));
            } else if (key == "APPSK") {
                value.toCharArray(apPSK, sizeof(apPSK));
            } else if (key == "isAPMode") {
                isAPMode = (value == "true");
            } else if (key == "matrix_rotation") {
                matrix_rotation = value.toInt();
            } else if (key == "max_plays") {
                 max_plays = value.toInt();
            } else if (key == "initialText") {
                value.toCharArray(Text, sizeof(Text));
            }
        }
    }

    file.close();
    printf("Config loaded: APSSID=%s Text=%s\n", apSSID, Text);
}

void loadFragmentsFromCSV() {
    // SPIFFS is already mounted by loadConfigFromCSV(); open the fragment store.
    File file = SPIFFS.open("/fragments.csv", "r");
    if (!file) {
        // First boot: seed with defaults and persist them so future edits are saved.
        fragments = {"Hello World", "Temperature: 25C", "Status: OK",
                     "Error: None", "Action: Start"};
        saveFragmentsToCSV();
        printf("fragments.csv not found, seeded %d defaults\n", fragments.size());
        return;
    }
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) fragments.push_back(line);
    }
    file.close();
    printf("Loaded %d fragments from SPIFFS\n", fragments.size());
}

void displayTextTask(void *parameters) {
    char *text = (char *)parameters;

    // Loop until the text scrolling is complete
    while (Text_Flow(text) != 0) {
        // Allow other tasks to run
         vTaskDelay(pdMS_TO_TICKS(10));  // Allow lower-priority tasks to run and avoid CPU starvation
    }

    // Clear the display once scrolling is complete
    Matrix.fillScreen(0);
    Matrix.show();

    // Clean up and delete the task
    delete[] text;  // Free the allocated memory for the text
    vTaskDelete(NULL);
}


void displayText(const char *text) {
    // Allocate memory for the text to pass it to the task
    char *taskText = new char[strlen(text) + 1];
    strcpy(taskText, text);

    // Create the FreeRTOS task
    xTaskCreate(
        displayTextTask,  // Task function
        "DisplayTextTask",// Name of the task
        4096,             // Stack size
        taskText,         // Task parameters (text to display)
        1,                // Priority
        NULL              // Task handle
    );
}


void xdisplayText(char* text){
  while(Text_Flow(text) !=0 ){
      yield();
    }
    Matrix.fillScreen(0);
    Matrix.show();
}


// Setup function
void setup() {
    delay(1000);
    //Serial.begin(115200);
    
    // Load configuration and fragment library from SPIFFS
    loadConfigFromCSV();
    loadFragmentsFromCSV();
    delay(1000);
    
    // Initialize WiFi
    WIFI_Init();
    delay(1000);
    // Initialize RGB Matrix
    Matrix_Init();
    clearSentStrings();
    // Build the banner from Version so the displayed/flashed version can never drift
    // from the protocol again (old 1.2 spoke a different /play protocol than the server).
    String banner = String("  ") + Project + (isAPMode ? " server " : " client ") + Version;
    displayText(banner.c_str());
}

// Main loop
void loop() {
    WIFI_Loop();
    Display_Loop();
}
unsigned long lastExpiredMessageTime = 0; // Tracks the last time the expired message was shown
const unsigned long expiredMessageInterval = 5000; // Minimum interval for showing the expired message (5 seconds)

// Server (AP) is the master sequencer: it owns the queue, picks the next valid message,
// schedules a start time, broadcasts a timed /play to all clients, and renders the very
// same schedule locally so it stays in lockstep with them.
void serverSequencerLoop() {
    // A message is currently scheduled/playing: render it until it finishes.
    if (currentSchedule.active) {
        renderScheduled(); // sets active=false when the scroll completes
        yield();
        delay(5);
        return;
    }

    if (sentCount == 0) {
        yield();
        delay(50);
        return;
    }

    if (currentStringIndex >= sentCount) {
        currentStringIndex = 0;
    }

    // Find the next message that still has plays left.
    int startIndex = currentStringIndex;
    bool found = false;
    do {
        if (playCount[currentStringIndex] < min(playLimit[currentStringIndex], max_plays)) {
            found = true;
            break;
        }
        currentStringIndex = (currentStringIndex + 1) % sentCount;
    } while (currentStringIndex != startIndex);

    if (!found) {
        // All messages exhausted their plays; wait for new text or a play-count reset.
        yield();
        delay(20);
        return;
    }

    // Schedule this message to start a fixed lead-time in the future so every client has
    // time to receive the /play before the shared start instant.
    uint32_t seq = ++globalSeq;
    uint32_t displayAt = millis() + DISPLAY_LEAD_MS; // server clock domain
    String text = sentStrings[currentStringIndex];

    broadcastPlay(seq, text, displayAt);             // tell the clients
    setSchedule(seq, text.c_str(), displayAt);       // and schedule it locally

    playCount[currentStringIndex]++;
    currentStringIndex = (currentStringIndex + 1) % sentCount;

    yield();
    delay(5);
}

// Client (STA) is a stateless player: it just renders whatever /play scheduled, keyed off
// the shared clock. No queue logic, so it can never drift out of step with its peers.
void clientPlayerLoop() {
    if (!clockSynced) {
        // Without the shared clock we cannot place the scroll in time; idle until synced.
        yield();
        delay(20);
        return;
    }
    renderScheduled();
    yield();
    delay(5);
}

void Display_Loop() {
    if (isAPMode) {
        serverSequencerLoop();
    } else {
        clientPlayerLoop();
    }
}
