#ifndef _WS_Flow_H_
#define _WS_Flow_H_

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#define RGB_Control_PIN   14    

extern char Text[100];
//extern uint8_t Flow_Flag;
extern bool isDisplaying;
extern volatile bool Flow_Flag; 
// Global variable to manage scrolling
extern int MatrixWidth;
extern Adafruit_NeoMatrix Matrix;
extern int matrix_rotation; 

// Define color values
#define RED_COLOR Matrix.Color(0, 255, 0)
#define GREEN_COLOR Matrix.Color(255, 0, 0)
#define BLUE_COLOR Matrix.Color(0, 0, 255)
#define PINK_COLOR Matrix.Color(0, 255, 128)
#define YELLO_COLOR Matrix.Color(128, 192, 0)
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
//void Text_Flow(char* Text);
int Text_Flow(char* Text);
void Matrix_Init();

// ---- Time-synchronized display (Model A: master sequencer) ----
#define SCROLL_INTERVAL_MS 120   // ms per 1-pixel scroll step (same on every device)
#define DISPLAY_LEAD_MS    2000  // how far ahead the server schedules a message start

// A single scheduled message. Every device (server + clients) renders the scroll
// position as a pure function of the shared clock, so they stay phase-locked and a
// dropped frame self-corrects on the next one.
struct DisplaySchedule {
    uint32_t seq;        // monotonic id; supersedes older items, dedups duplicates
    uint32_t displayAt;  // server-clock timecode at which scrolling begins
    char text[100];      // color-stripped text to render
    int textWidth;       // precomputed pixel width
    uint16_t color;      // resolved text color
    int lastStep;        // last scroll step drawn (redraw only on change)
    bool active;         // false = nothing scheduled / finished
};
extern DisplaySchedule currentSchedule;

// serverNow() lives in ws_wifi.cpp (it owns the clock offset); declared here so the
// renderer can read the shared clock without pulling in the WiFi header.
uint32_t serverNow();

void applyColorAndStrip(const char* raw, char* out, size_t outSize, uint16_t* colorOut);
void setSchedule(uint32_t seq, const char* raw, uint32_t displayAt);
bool renderScheduled();  // returns true while showing/pending, false when idle/finished
#endif
