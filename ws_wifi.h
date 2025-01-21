#ifndef _WS_WIFI_H_
#define _WS_WIFI_H_

#include <WiFi.h>
#include <WebServer.h> 
#include <WiFiClient.h>
#include <WiFiAP.h>
#include "stdio.h"
#include "ws_flow.h"

// The name and password of the WiFi access point
#define APSSID       "ESP32-S3-Matrix"
#define APPSK        "waveshare"

extern volatile bool Flow_Flag;
extern char Text[100];


#define MAX_SENT_STRINGS 5
extern String sentStrings[MAX_SENT_STRINGS];
extern int sentCount;
extern int sentIndex;

#define PREMADE_COUNT 5
extern const char* preMadeStrings[PREMADE_COUNT];

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
