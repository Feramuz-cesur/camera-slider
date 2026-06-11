#include "SliderWifi.h"
#include "Config.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static bool       g_sta = false;
static bool       g_ap  = false;
static DNSServer  dnsServer;   // captive portal: resolves every host to the AP IP

static bool loadCreds(String& ssid, String& pass) {
    File f = LittleFS.open(WIFI_CREDS_FILE, "r");
    if (!f) return false;
    JsonDocument d;
    DeserializationError err = deserializeJson(d, f);
    f.close();
    if (err) return false;
    ssid = (const char*)(d["ssid"] | "");
    pass = (const char*)(d["pass"] | "");
    return ssid.length() > 0;
}

static void startAP() {
    // AP_STA so the radio can still scan for networks while serving the portal.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);
    // Captive portal: answer every DNS query with our own IP so the phone's
    // "is there internet?" probe hits us and auto-opens the interface.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());
    g_ap  = true;
    g_sta = false;
}

void Wifi_begin() {
    LittleFS.begin();
    WiFi.persistent(false);

    String ssid, pass;
    if (loadCreds(ssid, pass)) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_STA_TIMEOUT_MS) {
            delay(200);
            yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
            g_sta = true;
            g_ap  = false;
            if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);
            return;
        }

        // Timed out -> drop the failed STA attempt and fall back to AP.
        WiFi.disconnect(true);
    }

    startAP();
}

bool Wifi_saveCreds(const String& ssid, const String& pass) {
    JsonDocument d;
    d["ssid"] = ssid;
    d["pass"] = pass;
    File f = LittleFS.open(WIFI_CREDS_FILE, "w");
    if (!f) return false;
    serializeJson(d, f);
    f.close();
    return true;
}

void Wifi_clearCreds() {
    LittleFS.remove(WIFI_CREDS_FILE);
}

bool   Wifi_isStation() { return g_sta; }
bool   Wifi_apActive()  { return g_ap; }
String Wifi_ip()        { return g_sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
String Wifi_ssid()      { return g_sta ? WiFi.SSID() : String(AP_SSID); }

void Wifi_loop() {
    if (g_ap)  dnsServer.processNextRequest();
    if (g_sta) MDNS.update();
}
