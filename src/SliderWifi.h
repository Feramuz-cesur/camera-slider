#pragma once
#include <Arduino.h>

// Connection lifecycle
void   Wifi_begin();    // load creds, try STA with timeout, fall back to AP
void   Wifi_loop();     // keep mDNS / captive-portal DNS alive (call from loop)

// Status
bool   Wifi_isStation();
bool   Wifi_apActive();
String Wifi_ip();
String Wifi_ssid();

// Boot-time connect progress, for the OLED (safe to read from any task).
bool    Wifi_connecting();   // Wifi_begin() is still trying the saved network
uint8_t Wifi_attempt();      // attempt in progress, 1-based
uint8_t Wifi_progress();     // share of the connect time budget used, 0..100

// Network scan for the provisioning page. Always asynchronous: Wifi_scanStart()
// only kicks a scan off, Wifi_loop() collects the result and Wifi_scanJson()
// returns the last completed list ("[]" until the first scan finishes). Nothing
// here ever blocks the main loop - see the note in Config.h.
void          Wifi_scanStart();
bool          Wifi_scanBusy();
const String& Wifi_scanJson();

// Credential store (LittleFS)
bool   Wifi_saveCreds(const String& ssid, const String& pass);
void   Wifi_clearCreds();
