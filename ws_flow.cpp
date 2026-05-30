#include "ws_flow.h"
#include <stdio.h>

// Rotation index variable (0: 0°, 1: 90°, 2: 180°, 3: 270°)
int matrix_rotation = 1; // Example: 1 corresponds to 90° rotation

Adafruit_NeoMatrix Matrix = Adafruit_NeoMatrix(8, 8, RGB_Control_PIN,    
  //NEO_MATRIX_TOP     + NEO_MATRIX_LEFT +   
  NEO_MATRIX_BOTTOM  + NEO_MATRIX_RIGHT +            
  NEO_MATRIX_ROWS    + NEO_MATRIX_PROGRESSIVE,
  NEO_GRB            + NEO_KHZ800);      


int MatrixWidth = 0;

// Currently scheduled message, shared by the sequencer (server) and the player (client).
DisplaySchedule currentSchedule = { 0, 0, "", 0, 0, -1000000, false };

// Resolve the @color prefix once and copy the remaining text out. Unlike the older
// filterString(), this always writes the output, even when no prefix matches.
void applyColorAndStrip(const char* raw, char* out, size_t outSize, uint16_t* colorOut) {
    String s(raw);
    for (int i = 0; colorMap[i].prefix != nullptr; i++) {
        if (s.startsWith(colorMap[i].prefix)) {
            *colorOut = colorMap[i].color;
            s.substring(strlen(colorMap[i].prefix)).toCharArray(out, outSize);
            return;
        }
    }
    *colorOut = WHITE_COLOR; // no prefix -> default color, full text
    s.toCharArray(out, outSize);
}

// Publish a new schedule. active is cleared first and set last so a concurrent
// render (webServerTask vs loop) cannot observe a half-written schedule.
void setSchedule(uint32_t seq, const char* raw, uint32_t displayAt) {
    currentSchedule.active = false;
    currentSchedule.seq = seq;
    currentSchedule.displayAt = displayAt;
    applyColorAndStrip(raw, currentSchedule.text, sizeof(currentSchedule.text), &currentSchedule.color);
    currentSchedule.textWidth = getStringWidth(currentSchedule.text) + 2;
    currentSchedule.lastStep = -1000000; // force the first frame to draw
    currentSchedule.active = true;
}

// Render the scheduled message at the column implied by the shared clock. Because the
// column is derived from time (not a per-device counter), every device shows the same
// column at the same instant and recovers from dropped frames automatically.
bool renderScheduled() {
    if (!currentSchedule.active) {
        return false;
    }

    int32_t elapsed = (int32_t)(serverNow() - currentSchedule.displayAt);
    int step = (elapsed < 0) ? -1 : (elapsed / SCROLL_INTERVAL_MS);

    if (step == currentSchedule.lastStep) {
        return true; // same column as last frame; nothing to redraw
    }
    currentSchedule.lastStep = step;

    if (elapsed < 0) {
        Matrix.fillScreen(0); // pre-start lead-in: keep the panel blank
        Matrix.show();
        return true;
    }

    int cursorX = Matrix.width() - step;
    if (cursorX < -currentSchedule.textWidth) {
        Matrix.fillScreen(0); // scrolled fully off; finished
        Matrix.show();
        currentSchedule.active = false;
        return false;
    }

    Matrix.fillScreen(0);
    Matrix.setTextColor(currentSchedule.color);
    Matrix.setCursor(cursorX, 0);
    Matrix.print(currentSchedule.text);
    Matrix.show();
    return true;
}

// Function to set color based on prefix
void setColor(const String &prefix) {
  for (int i = 0; colorMap[i].prefix != nullptr; i++) {
    if (prefix == colorMap[i].prefix) {
      Matrix.setTextColor(colorMap[i].color);
      return;
    }
  }
  Matrix.setTextColor(WHITE_COLOR); // Default color if no match
}

// Function to filter the string and set the color
void filterString(char input[], char output[]) {
  // Convert the char array to a String for manipulation
  String inputString = String(input);
  for (int i = 0; colorMap[i].prefix != nullptr; i++) {
    if (inputString.startsWith(colorMap[i].prefix)) {
      setColor(colorMap[i].prefix); // Set the color
      // Remove the prefix
      String filteredString = inputString.substring(strlen(colorMap[i].prefix));
      // Convert the filtered String back to char array
      filteredString.toCharArray(output, filteredString.length() + 1);
      //printf("Filtered: %s\r\n", output);
      return;
    }
  }
}

// Set color for LEDs in array with delay between setting each LED
void colorWipe(uint32_t c, uint8_t wait) {
  for(uint16_t i=0; i<Matrix.numPixels(); i++) {
    Matrix.setPixelColor(i, c);
    Matrix.show();
    delay(wait);
  }
}

int getCharWidth(char c) {
  if (c == 'i' || c == 'l' || c == '!' || c == '.') {
    return 4;
  // } else if (c == 'm' || c == 'w') {
  //   return 7;
  } else {
    return 5;
  }
}

int getStringWidth(const char* str) {
  int width = 0;
  int length = strlen(str);
  // printf("%d\r\n",length);
  for (int i = 0; i < length; i++) {
    width += getCharWidth(str[i]);
    width += 1;                           
  }
  // printf("%d\r\n",width);
  return width;
}
void Matrix_Init() {
  Matrix.begin(); 
  Matrix.setTextWrap(false);    
  
  Matrix.setBrightness(20);                             // set brightness
  Matrix.setTextColor( Matrix.Color(0, 255, 128)); 
  MatrixWidth   = Matrix.width();
   // Apply the rotation index (0-3)
  Matrix.setRotation(matrix_rotation);
}

int Text_Flow(char* Text) {
    static unsigned long lastUpdate = 0;
    static int MatrixWidth = 0; // Track scrolling position
    unsigned long currentMillis = millis();
    const unsigned long updateInterval = 120; // Adjust as needed for scroll speed

    if (Text == nullptr || strlen(Text) == 0) {
        printf("Empty or null Text passed to Text_Flow.\n");
        yield();
        delay(100);
        return 0; // No action needed, indicate completion
    }

    if (currentMillis - lastUpdate >= updateInterval) {
        lastUpdate = currentMillis;

        // Apply any necessary filtering to the text
        filterString(Text, Text);
        int textWidth = getStringWidth(Text) +2;

        // Clear the screen and prepare to display the text
        Matrix.fillScreen(0);
        Matrix.setCursor(MatrixWidth, 0);
        Matrix.print(F(Text));
        Matrix.show();

        // Check if the text has finished scrolling
        if (--MatrixWidth < -textWidth) {
            MatrixWidth = Matrix.width(); // Reset position
            Flow_Flag = true; // Indicate completion
            printf("Flow_Flag set to true. Scrolling complete for text: %s\n", Text);
            return 0;
        }
    }

    return 1; // Indicate scrolling in progress
}


void xText_Flow(char* Text) {
    static unsigned long lastUpdate = 0;
    unsigned long currentMillis = millis();
    const unsigned long updateInterval = 120; // Adjust as needed for scroll speed

    if (Text == nullptr || strlen(Text) == 0) {
        printf("Empty or null Text passed to Text_Flow.\n");
        isDisplaying = false;
        Flow_Flag = false;
        // Yield and delay at the end to allow other tasks to run
        yield();
        delay(100);
        return; // No action needed for empty text
    }

    if (currentMillis - lastUpdate >= updateInterval) {
        lastUpdate = currentMillis;

        // Apply any necessary filtering to the text
        filterString(Text, Text);
        int textWidth = getStringWidth(Text);

        // Clear the screen and prepare to display the text
        Matrix.fillScreen(0);
        Matrix.setCursor(MatrixWidth, 0);
        Matrix.print(F(Text));
        Matrix.show();

        // Check if the text has finished scrolling
        if (--MatrixWidth < -textWidth) {
            MatrixWidth = Matrix.width(); // Reset position
            Flow_Flag = true; // Indicate completion
            printf("Flow_Flag set to true. Scrolling complete for text: %s\n", Text);
        }
    }
}
