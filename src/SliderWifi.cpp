#include "SliderWifi.h"
#include "Config.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static bool       g_sta = false;
static bool       g_ap  = false;
static DNSServer  dnsServer;   // captive portal: resolves every host to the AP IP

// Async scan state (see Config.h for why a blocking scan is not an option).
static String     g_scanJson = "[]";   // last completed result, ready to serve
static bool       g_scanBusy = false;
static uint32_t   g_scanDone = 0;      // millis() when the last scan finished (0 = never)

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
    // Modem power-save makes the AP slow to associate and laggy to respond
    // (clients, esp. Windows PCs, wait through the radio's sleep windows).
    // Keep the radio always on - this is the single biggest AP responsiveness win.
    WiFi.setSleep(false);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);
    // Must come after softAP() (radio has to be started first). See Config.h:
    // full TX power breaks association on this board's PCB antenna.
    WiFi.setTxPower(WIFI_TX_POWER);
    // Captive portal: answer every DNS query with our own IP so the phone's
    // "is there internet?" probe hits us and auto-opens the interface.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());
    g_ap  = true;
    g_sta = false;
}

void Wifi_begin() {
    LittleFS.begin(true);   // format on first boot
    WiFi.persistent(false);

    String ssid, pass;
    if (loadCreds(ssid, pass)) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);   // lower request latency, snappier web UI
        WiFi.begin(ssid.c_str(), pass.c_str());
        WiFi.setTxPower(WIFI_TX_POWER);   // after begin(); see Config.h note

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

// ---------- Async network scan ----------
void Wifi_scanStart() {
    if (g_scanBusy) return;
    // Rate-limit: every sweep still takes the radio off the AP channel for ~2 s,
    // so a page that asks for a refresh in a tight loop must not be able to
    // keep the AP permanently off-channel.
    if (g_scanDone && millis() - g_scanDone < WIFI_SCAN_MIN_PERIOD_MS) return;
    if (WiFi.scanNetworks(true /*async*/, false /*hidden*/, false /*active*/,
                          WIFI_SCAN_DWELL_MS) == WIFI_SCAN_RUNNING) {
        g_scanBusy = true;
    }
}

bool          Wifi_scanBusy() { return g_scanBusy; }
const String& Wifi_scanJson() { return g_scanJson; }

// Called from Wifi_loop() while a scan is in flight.
static void collectScan() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    g_scanBusy = false;
    g_scanDone = millis();
    if (n < 0) return;   // failed - keep whatever list we already had

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < WIFI_SCAN_MAX_RESULTS; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["lock"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    g_scanJson = "";
    serializeJson(doc, g_scanJson);
    WiFi.scanDelete();
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
    if (g_ap)       dnsServer.processNextRequest();
    if (g_scanBusy) collectScan();
    // ESP32 mDNS runs in its own task; no update() call needed.
}
