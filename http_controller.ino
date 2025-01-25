#include <Adafruit_NeoPixel.h>
#include "ws_flow.h"
#include "ws_wifi.h"
#include <FS.h>
#include <SPIFFS.h>

char apSSID[64] = "ESP32-S3-Matrix";
char apPSK[64]  = "waveshare";

char Text[100] = "NEWNOTNEW";

// Display-related global variables
int currentStringIndex = 0;
char currentStringBuffer[100] = {0};
bool isDisplaying = false;
volatile bool Flow_Flag = false; // Flow flag for display logic


// Define sentStrings, sentIndex, sentCount
String sentStrings[MAX_SENT_STRINGS];
int sentIndex = 0;
int sentCount = 0;

int max_plays = 3;

char predefinedTexts[PREMADE_COUNT][MAX_TEXT_LENGTH] = {
    "Hello World",
    "Temperature: 25C",
    "Status: OK",
    "Error: None",
    "Action: Start"
};

// Define sentStrings, sentIndex, sentCount, and playCount
int playCount[MAX_SENT_STRINGS] = {0}; // Tracks how many times each string has been displayed

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
            } else {
                for (size_t i = 0; i < PREMADE_COUNT; i++) {
                    if (key == "predefText" + String(i + 1)) {
                        value.toCharArray(predefinedTexts[i], MAX_TEXT_LENGTH);
                        break;
                    }
                }
            }
        }
    }

    file.close();
    printf("Configuration loaded from SPIFFS:");
    printf("APSSID: %s\n", apSSID);
    printf("APPSK: %s\n", apPSK);
    printf("Text: %s\n", Text);
    for (size_t i = 0; i < PREMADE_COUNT; i++) {
        printf("predefinedTexts[%d]: %s\n", i, predefinedTexts[i]);
    }
}

// Setup function
void setup() {
    delay(1000);
    //Serial.begin(115200);
    
    // Load configuration from SPIFFS
    loadConfigFromCSV();
    delay(1000);
    
    // Initialize WiFi
    WIFI_Init();
    delay(1000);
    // Initialize RGB Matrix
    Matrix_Init();
}

// Main loop
void loop() {
    WIFI_Loop();
    Display_Loop();
}
unsigned long lastExpiredMessageTime = 0; // Tracks the last time the expired message was shown
const unsigned long expiredMessageInterval = 5000; // Minimum interval for showing the expired message (5 seconds)

void Display_Loop() {
    if (sentCount == 0) {
        // Reset flags if no strings are available
        isDisplaying = false;
        Flow_Flag = false;
        return;
    }

    if (!isDisplaying) {
        // Find the next valid string to display
        int startIndex = currentStringIndex;
        bool foundValidString = false;

        do {
            if (playCount[currentStringIndex] < max_plays) {
                // Start displaying the current string
                String str = sentStrings[currentStringIndex];
                str.toCharArray(currentStringBuffer, sizeof(currentStringBuffer));
                isDisplaying = true;
                Flow_Flag = false;
                foundValidString = true;
                break;
            } else {
                // Skip expired strings
                currentStringIndex = (currentStringIndex + 1) % sentCount;
            }
        } while (currentStringIndex != startIndex);

        if (!foundValidString) {
            return; // All strings are expired
        }
    }

    if (isDisplaying) {
        Text_Flow(currentStringBuffer); // Display the current string

        if (Flow_Flag) {
            playCount[currentStringIndex]++;
            currentStringIndex = (currentStringIndex + 1) % sentCount;
            isDisplaying = false;
            Flow_Flag = false;
        }
    }
}
