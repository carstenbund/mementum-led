#include <Adafruit_NeoPixel.h>
#include "ws_flow.h"
#include "ws_wifi.h"

char Text[100] ="UNANGEBRACHT";
//uint8_t Flow_Flag = 0;

// Define sentStrings, sentIndex, sentCount
String sentStrings[MAX_SENT_STRINGS];
int sentIndex = 0;
int sentCount = 0;

const char* preMadeStrings[PREMADE_COUNT] = {
    "Hello World",
    "Temperature: 25°C",
    "Status: OK",
    "Error: None",
    "Action: Start"
};

// Display-related global variables
int currentStringIndex = 0;             // Index of the current string being displayed
char currentStringBuffer[100] = {0};    // Buffer to hold the current string
bool isDisplaying = false;              // Flag to indicate if a string is being displayed

// Flow flag to indicate when a string has finished displaying
volatile bool Flow_Flag = false;

void setup()
{
// WIFI
  WIFI_Init();

// RGB
  Matrix_Init();
}

uint32_t Flag =0;
void loop(){
  WIFI_Loop();
 
 Display_Loop();
}


void Display_Loop() {
  if (sentCount == 0) {
    // No strings to display
    return;
  }

  if (!isDisplaying) {
    // Start displaying the current string
    String str = sentStrings[currentStringIndex];
    str.toCharArray(currentStringBuffer, sizeof(currentStringBuffer));
    MatrixWidth = Matrix.width();
    isDisplaying = true;
    Flow_Flag = false; // Reset the flag before starting
    Serial.printf("Displaying string: %s\r\n", currentStringBuffer);
  }

  if (isDisplaying) {
    Text_Flow(currentStringBuffer); // Display the current string

    // Check if the string has finished displaying
    if (Flow_Flag) {
      // Move to the next string
      currentStringIndex = (currentStringIndex + 1) % sentCount;
      isDisplaying = false; // Ready to display the next string
      Flow_Flag = false;    // Reset the flag
      Serial.println("Finished displaying string. Moving to next string.");
    }
  }
}
