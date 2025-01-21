#ifndef _WS_Flow_H_
#define _WS_Flow_H_

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#define RGB_Control_PIN   14    

extern char Text[100];
//extern uint8_t Flow_Flag;
extern volatile bool Flow_Flag; 
// Global variable to manage scrolling
extern int MatrixWidth;
extern Adafruit_NeoMatrix Matrix;


// Define color values
#define RED_COLOR Matrix.Color(0, 255, 0)
#define GREEN_COLOR Matrix.Color(255, 0, 0)
#define BLUE_COLOR Matrix.Color(0, 0, 255)
#define PINK_COLOR Matrix.Color(0, 255, 128)
#define YELLO_COLOR Matrix.Color(255, 255, 0)
#define CYAN_COLOR Matrix.Color(255, 0, 128)
#define WHITE_COLOR Matrix.Color(255, 255, 255) // Default color

// Define a structure for mapping prefixes to colors
struct ColorMap {
  const char *prefix;
  uint16_t color;
};

// Declare the color map array
const ColorMap colorMap[] = {
    {"@red", RED_COLOR},
    {"@green", GREEN_COLOR},
    {"@blue", BLUE_COLOR},
    {"@pink", PINK_COLOR},
    {"@yellow", YELLO_COLOR},
    {"@cyan", CYAN_COLOR},
    {nullptr, WHITE_COLOR} // Default color at the end
};

void getNextSegment(const char* inputText, char* outputBuffer);
void filterString(char input[], char output[]);
void colorWipe(uint32_t c, uint8_t wait);
int getCharWidth(char c);
int getStringWidth(const char* str);
void Text_Flow(char* Text);
void Matrix_Init();       
#endif
