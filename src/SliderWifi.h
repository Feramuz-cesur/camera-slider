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

// Credential store (LittleFS)
bool   Wifi_saveCreds(const String& ssid, const String& pass);
void   Wifi_clearCreds();
