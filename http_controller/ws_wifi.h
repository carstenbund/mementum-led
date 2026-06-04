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
extern char currentStringBuffer[100];

extern char Version[20];   // firmware version, reported to the server on /register

extern char apSSID[64];
extern char apPSK[64];

extern String serverAddress;
// Kept in sync with the elected role (true only while this node is the SoftAP server) so
// existing server-only branches (e.g. handleClearStrings broadcasting) keep working.
extern bool isAPMode;

// Role-agnostic failover: every node runs the same state machine and elects a single
// SoftAP "server" at runtime from its MAC-derived rank. See docs/failover-design.md.
enum NodeRole { ROLE_JOINING, ROLE_CLIENT, ROLE_CANDIDATE, ROLE_SERVER };
extern NodeRole nodeRole;

#define MAX_SENT_STRINGS 5
extern String sentStrings[MAX_SENT_STRINGS];
extern int sentCount;

#define MAX_TEXT_LENGTH 100
#define PREMADE_COUNT 8
extern char predefinedTexts[PREMADE_COUNT][MAX_TEXT_LENGTH];

// Compact FIFO queue state plus per-entry play counts
extern int playCount[MAX_SENT_STRINGS];
extern int max_plays;
extern int default_plays;
extern bool isDisplaying;

extern Adafruit_NeoMatrix Matrix;

// ---- Time-synchronized display (Model A) ----
extern bool clockSynced;   // client: true once the server clock offset is known
extern uint32_t globalSeq; // server: monotonic schedule sequence number

void syncClock();          // client: estimate the server-clock offset
void handleTime();         // route: return our millis() as the clock reference
void handlePlay();         // route: client receives a timed display command
void broadcastPlay(uint32_t seq, const String &text, uint32_t displayAt); // server -> clients

void clearSentStrings();
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
