#include <Adafruit_NeoPixel.h>
#include "ws_flow.h"
#include "ws_wifi.h"
#include <FS.h>
#include <SPIFFS.h>

char apSSID[64] = "ESP32-S3-Matrix";
char apPSK[64]  = "waveshare";

char Text[100] = "UNANGEBRACHT";

// Define sentStrings, sentIndex, sentCount
String sentStrings[MAX_SENT_STRINGS];
int sentIndex = 0;
int sentCount = 0;

char predefinedTexts[PREMADE_COUNT][MAX_TEXT_LENGTH] = {
    "Hello World",
    "Temperature: 25C",
    "Status: OK",
    "Error: None",
    "Action: Start"
};

// Display-related global variables
int currentStringIndex = 0;
char currentStringBuffer[100] = {0};
bool isDisplaying = false;
volatile bool Flow_Flag = false; // Flow flag for display logic

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

// Display loop function
void Display_Loop() {
    if (sentCount == 0) {
        // No strings to display
        return;
    }

    if (!isDisplaying) {
        // Start displaying the current string
        String str = sentStrings[currentStringIndex];
        str.toCharArray(currentStringBuffer, sizeof(currentStringBuffer));
        isDisplaying = true;
        Flow_Flag = false; // Reset the flag
        //printf("Displaying string: %s\r\n", currentStringBuffer);
    }

    if (isDisplaying) {
        Text_Flow(currentStringBuffer); // Display the current string

        // Check if the string has finished displaying
        if (Flow_Flag) {
            // Move to the next string
            currentStringIndex = (currentStringIndex + 1) % sentCount;
            isDisplaying = false; // Ready to display the next string
            Flow_Flag = false;    // Reset the flag
            //printf("Finished displaying string. Moving to next string.\n");
        }
    }
}
