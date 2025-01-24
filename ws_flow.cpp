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
    return 3;
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

void Text_Flow(char* Text) {
  static unsigned long lastUpdate = 0;
  unsigned long currentMillis = millis();
  const unsigned long updateInterval = 120; // Adjust as needed for scroll speed

  if (currentMillis - lastUpdate >= updateInterval) {
    lastUpdate = currentMillis;
    filterString(Text, Text);
    int textWidth = getStringWidth(Text);
    Matrix.fillScreen(0);                       
    Matrix.setCursor(MatrixWidth, 0);
    Matrix.print(F(Text));                      
    Matrix.show();

    //printf("MatrixWidth: %d\r\n", MatrixWidth);
    if (--MatrixWidth < -textWidth) {      
      MatrixWidth = Matrix.width();
      Flow_Flag = true; // Indicate that the string has finished displaying
      //printf("Flow_Flag set to true\n");
    }
  }
}
