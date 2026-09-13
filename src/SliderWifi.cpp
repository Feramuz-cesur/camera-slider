#include "SliderWifi.h"
#include "Config.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static bool       g_sta = false;
static bool       g_ap  = false;
static DNSServer  dnsServer;   // captive portal: resolves every host to the AP IP

// Async scan state (see Config.h for why the sweep is split per channel).
struct ScanEntry { String ssid; int32_t rssi; bool lock; };

static String     g_scanJson = "[]";   // result so far, ready to serve as-is
static bool       g_scanBusy = false;  // a sweep is in progress
static uint32_t   g_scanDone = 0;      // millis() when the last sweep ended (0 = never)
static ScanEntry  g_scanAcc[WIFI_SCAN_MAX_RESULTS];
static uint8_t    g_scanAccCount = 0;  // APs collected so far this sweep
static uint8_t    g_scanChan   = 0;    // channel being scanned right now (0 = between channels)
static uint8_t    g_scanNextCh = 1;    // next channel to visit
static uint32_t   g_scanNextAt = 0;    // millis() the next channel may start at
static uint32_t   g_scanChanAt = 0;    // millis() the in-flight channel scan was started

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
    // ...but the STA side must stay passive. If saved credentials just failed
    // (wrong password, router out of range) the driver would otherwise keep
    // retrying that network on its own for as long as the AP is up, and every
    // attempt takes the radio off the AP channel for seconds - with no client
    // action involved at all. That looks exactly like "the phone cannot even
    // associate". Kill the auto-reconnect and cancel any pending attempt.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false /*keep the radio on, we still need it for scans*/);
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
bool          Wifi_scanBusy() { return g_scanBusy; }
const String& Wifi_scanJson() { return g_scanJson; }

// One AP into the sweep's accumulator. The same network often answers on
// several channels, so keep the strongest sighting of each SSID.
static void mergeAp(const String& ssid, int32_t rssi, bool lock) {
    if (ssid.length() == 0) return;   // hidden AP, nothing to pick from a list
    for (uint8_t i = 0; i < g_scanAccCount; i++) {
        if (g_scanAcc[i].ssid == ssid) {
            if (rssi > g_scanAcc[i].rssi) { g_scanAcc[i].rssi = rssi; g_scanAcc[i].lock = lock; }
            return;
        }
    }
    if (g_scanAccCount >= WIFI_SCAN_MAX_RESULTS) return;
    g_scanAcc[g_scanAccCount++] = { ssid, rssi, lock };
}

// Re-serialise the accumulator, strongest first. Called after every channel so
// the provisioning page fills in as the sweep progresses instead of waiting.
static void publishScan() {
    for (int i = 1; i < g_scanAccCount; i++) {          // insertion sort by RSSI
        ScanEntry e = g_scanAcc[i];
        int j = i - 1;
        while (j >= 0 && g_scanAcc[j].rssi < e.rssi) { g_scanAcc[j + 1] = g_scanAcc[j]; j--; }
        g_scanAcc[j + 1] = e;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < g_scanAccCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = g_scanAcc[i].ssid;
        o["rssi"] = g_scanAcc[i].rssi;
        o["lock"] = g_scanAcc[i].lock;
    }
    g_scanJson = "";
    serializeJson(doc, g_scanJson);
}

void Wifi_scanStart() {
    if (g_scanBusy) return;
    // Every sweep still costs the AP some time off-channel, so a page asking for
    // refreshes in a tight loop must not be able to keep it there.
    if (g_scanDone && millis() - g_scanDone < WIFI_SCAN_MIN_PERIOD_MS) return;
    WiFi.scanDelete();   // drop a stale result buffer from an earlier sweep
    g_scanAccCount = 0;
    g_scanChan     = 0;
    g_scanNextCh   = 1;
    g_scanNextAt   = millis();
    g_scanBusy     = true;
}

// Drives the sweep; called from Wifi_loop() while one is in progress.
static void scanStep() {
    uint32_t now = millis();

    if (g_scanChan) {                       // a channel scan is in flight
        int16_t n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            // A scan that never reports back is not just a missing network list:
            // the radio stays parked on that channel, so the AP is gone for good
            // and clients cannot even complete DHCP. Nothing clears this by
            // itself, so abandon the channel and force the radio back home.
            if ((int32_t)(now - g_scanChanAt) < WIFI_SCAN_CHANNEL_TIMEOUT_MS) return;
            esp_wifi_scan_stop();
            WiFi.scanDelete();
            g_scanChan   = 0;
            g_scanNextAt = now + WIFI_SCAN_CHANNEL_GAP_MS;
            return;
        }
        for (int i = 0; i < n; i++)         // n < 0 on failure -> loop does not run
            mergeAp(WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        WiFi.scanDelete();
        publishScan();
        g_scanChan   = 0;
        g_scanNextAt = now + WIFI_SCAN_CHANNEL_GAP_MS;   // let the AP breathe
        return;
    }

    if ((int32_t)(now - g_scanNextAt) < 0) return;

    if (g_scanNextCh > WIFI_SCAN_LAST_CHANNEL) {         // sweep complete
        g_scanBusy = false;
        g_scanDone = now;
        return;
    }

    if (WiFi.scanNetworks(true /*async*/, false /*hidden*/, false /*active*/,
                          WIFI_SCAN_DWELL_MS, g_scanNextCh) == WIFI_SCAN_RUNNING) {
        g_scanChan   = g_scanNextCh;
        g_scanChanAt = now;
    } else {
        g_scanNextAt = now + WIFI_SCAN_CHANNEL_GAP_MS;   // could not start; skip on
    }
    g_scanNextCh++;
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
    if (g_scanBusy) scanStep();
    // ESP32 mDNS runs in its own task; no update() call needed.
}
