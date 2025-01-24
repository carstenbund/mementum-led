#ifndef _WS_WIFI_H_
#define _WS_WIFI_H_

#include <Adafruit_NeoMatrix.h>

#include <WiFi.h>
#include <esp_event.h>
//#include <esp_wifi.h>
#include <WebServer.h> 
#include <WiFiClient.h>
#include <WiFiAP.h>
#include "stdio.h"
#include "ws_flow.h"

extern volatile bool Flow_Flag;
extern char Text[100];

extern char apSSID[64];
extern char apPSK[64];

extern String serverAddress;
extern bool isAPMode;

#define MAX_SENT_STRINGS 5
extern String sentStrings[MAX_SENT_STRINGS];
extern int sentCount;
extern int sentIndex;

#define MAX_TEXT_LENGTH 50
#define PREMADE_COUNT 8
extern char predefinedTexts[PREMADE_COUNT][MAX_TEXT_LENGTH];

// Define sentStrings, sentIndex, sentCount, and playCount
extern int playCount[MAX_SENT_STRINGS];
extern int max_plays;
extern bool isDisplaying;

extern Adafruit_NeoMatrix Matrix;

void handleRoot();
void handleGetData();
void handleSwitch(uint8_t ledNumber);

void handleSwitch1();
void handleSwitch2();
void handleSwitch3();
void handleSwitch4();
void handleSwitch5();
void handleSwitch6();
void handleSwitch7();
void handleSwitch8();
void WIFI_Init();
void WIFI_Loop();

#endif
