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
