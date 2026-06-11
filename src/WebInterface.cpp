#include "WebInterface.h"
#include "SliderControl.h"
#include "SliderWifi.h"
#include "Settings.h"
#include "Config.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static ESP8266WebServer server(80);
static uint32_t g_rebootAt = 0;   // 0 = no pending reboot

// External control lock (e.g. PrintLapse app takes over the slider)
static bool   g_locked   = false;
static String g_lockedBy = "";

// Detailed state token used by our own web UI
static const char* stateShortName(SliderState s) {
    switch (s) {
        case STATE_BOOT:            return "BOOT";
        case STATE_HOMING_FAST:
        case STATE_HOMING_BACKOFF:
        case STATE_HOMING_SLOW:     return "HOMING";
        case STATE_IDLE:            return "IDLE";
        case STATE_MANUAL:          return "MANUAL";
        case STATE_AUTO_REPOSITION: return "AUTO_REPOSITION";
        case STATE_AUTO_MOVE:       return "AUTO_MOVE";
        case STATE_AUTO_PAUSED:     return "AUTO_PAUSED";
        case STATE_CALIBRATE:       return "CALIBRATE";
        case STATE_FAULT:           return "FAULT";
    }
    return "?";
}

// Coarse state token for external apps (PrintLapse contract): idle|homing|moving|error|estop
static const char* stateApiName(SliderState s) {
    switch (s) {
        case STATE_BOOT:            return "idle";
        case STATE_HOMING_FAST:
        case STATE_HOMING_BACKOFF:
        case STATE_HOMING_SLOW:     return "homing";
        case STATE_IDLE:            return "idle";
        case STATE_AUTO_PAUSED:     return "idle";
        case STATE_MANUAL:
        case STATE_AUTO_REPOSITION:
        case STATE_AUTO_MOVE:
        case STATE_CALIBRATE:       return "moving";
        case STATE_FAULT:           return "error";
    }
    return "idle";
}

// ---------- Handlers ----------
static void handleIndex() {
    // AP mode -> WiFi provisioning page; STA mode -> the control UI.
    const char* path = Wifi_apActive() ? "/network.html" : "/index.html";
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(500, "text/plain",
            "Page not found on filesystem.\nUpload the filesystem image (PlatformIO: Upload Filesystem Image).");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

static void handlePing() {
    // Lightweight identity handshake for external apps (PrintLapse "Test Connection").
    // Stays fast and works even while locked (read-only, no lock check).
    server.send(200, "application/json",
        "{\"ok\":true,\"device\":\"slider\",\"name\":\"" DEVICE_NAME "\"}");
}

static void handleStatus() {
    JsonDocument doc;
    doc["state"]     = stateApiName(Slider_state());    // contract: idle|homing|moving|error|estop
    doc["stateRaw"]  = stateShortName(Slider_state());  // detailed token for our own UI
    doc["stateText"] = Slider_stateText();
    doc["pos"]       = Slider_positionMm();
    doc["homed"]     = Slider_isHomed();
    doc["progress"]  = Slider_autoProgress();
    doc["locked"]    = g_locked;
    doc["by"]        = g_lockedBy;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleHome() {
    bool ok = Slider_startHoming();
    server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleSkipHome() {
    Slider_skipHoming();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleManual() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    const char* action = doc["action"] | "";
    if (strcmp(action, "start") == 0) {
        if (g_locked) { server.send(409, "application/json", "{\"ok\":false}"); return; }
        const char* dir = doc["dir"] | "";
        float speed = doc["speed"] | settings.maxSpeedMmS;   // jog speed (mm/s)
        bool ok = Slider_manualStart(strcmp(dir, "right") == 0 ? DIR_RIGHT : DIR_LEFT, speed);
        server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (strcmp(action, "stop") == 0) {
        Slider_manualStop();
        server.send(200, "application/json", "{\"ok\":true}");
    } else {
        server.send(400, "application/json", "{\"ok\":false}");
    }
}

static void handleAuto() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    const char* action = doc["action"] | "";
    if (strcmp(action, "start") == 0) {
        if (g_locked) { server.send(409, "application/json", "{\"ok\":false}"); return; }
        const char* dir   = doc["dir"]      | "ltr";
        float duration    = doc["duration"] | 10.0f;
        bool ltr = (strcmp(dir, "ltr") == 0);
        bool ok = Slider_startAuto(ltr, duration);
        server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (strcmp(action, "stop") == 0) {
        Slider_stopAuto();
        server.send(200, "application/json", "{\"ok\":true}");
    } else if (strcmp(action, "pause") == 0) {
        bool ok = Slider_pauseAuto();
        server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (strcmp(action, "resume") == 0) {
        bool ok = Slider_resumeAuto();
        server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else {
        server.send(400, "application/json", "{\"ok\":false}");
    }
}

static void handleEstop() {
    Slider_emergencyStop();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleGoto() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    float pos = doc["pos"] | 0.0f;
    // Accept and return immediately; the move runs in the background (contract).
    bool ok = Slider_gotoMm(pos);
    server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleSettingsGet() {
    JsonDocument doc;
    doc["stepsPerMm"]  = settings.stepsPerMm;                         // canonical (external apps)
    doc["mmPerRev"]    = settings.stepsPerRev / settings.stepsPerMm;  // derived for display
    doc["stepsPerRev"] = settings.stepsPerRev;
    doc["maxTravelMm"] = settings.maxTravelMm;
    doc["maxSpeedMmS"] = settings.maxSpeedMmS;
    doc["accelMmS2"]   = settings.accelMmS2;
    doc["homingSpeedMmS"] = settings.homingSpeedMmS;
    doc["useAccel"]    = settings.useAccel;
    doc["invertDir"]   = settings.invertDir;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleSettingsPost() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }

    float    defMmPerRev = (float)settings.stepsPerRev / settings.stepsPerMm;
    float    mmPerRev    = doc["mmPerRev"]    | defMmPerRev;
    uint16_t stepsPerRev = doc["stepsPerRev"] | settings.stepsPerRev;
    float    maxTravelMm = doc["maxTravelMm"] | settings.maxTravelMm;
    float    maxSpeedMmS = doc["maxSpeedMmS"] | settings.maxSpeedMmS;
    float    accelMmS2   = doc["accelMmS2"]   | settings.accelMmS2;
    float    homingSpeed = doc["homingSpeedMmS"] | settings.homingSpeedMmS;
    bool     useAccel    = doc["useAccel"]    | settings.useAccel;
    bool     invertDir   = doc["invertDir"]   | settings.invertDir;

    if (mmPerRev    < 0.1f || mmPerRev    > 1000) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (stepsPerRev < 1    || stepsPerRev > 20000){ server.send(400, "application/json", "{\"ok\":false}"); return; }
    float stepsPerMm = stepsPerRev / mmPerRev;
    if (stepsPerMm  < 1 || stepsPerMm  > 3200)  { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (maxTravelMm < 10 || maxTravelMm > 2000) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (maxSpeedMmS < 1 || maxSpeedMmS > 500)   { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (accelMmS2   < 1 || accelMmS2   > 5000)  { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (homingSpeed < 1 || homingSpeed > 500)   { server.send(400, "application/json", "{\"ok\":false}"); return; }

    settings.stepsPerMm  = stepsPerMm;
    settings.stepsPerRev = stepsPerRev;
    settings.maxTravelMm = maxTravelMm;
    settings.maxSpeedMmS = maxSpeedMmS;
    settings.accelMmS2   = accelMmS2;
    settings.homingSpeedMmS = homingSpeed;
    settings.useAccel    = useAccel;
    settings.invertDir   = invertDir;
    Settings_save();
    Slider_applySettings();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleCalibrate() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    const char* action = doc["action"] | "";

    if (strcmp(action, "spin") == 0) {
        float turns = doc["turns"] | 0.0f;
        if (turns <= 0 || turns > 100) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        long steps = (long)(turns * settings.stepsPerRev + 0.5f);
        bool ok = Slider_calibrateSpin(steps);
        server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        return;
    }

    if (strcmp(action, "apply") == 0) {
        float turns    = doc["turns"]    | 0.0f;
        float measured = doc["measured"] | 0.0f;
        if (turns <= 0 || measured <= 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        float stepsPerMm = (turns * settings.stepsPerRev) / measured;
        if (stepsPerMm < 1 || stepsPerMm > 3200) { server.send(400, "application/json", "{\"ok\":false}"); return; }
        settings.stepsPerMm = stepsPerMm;
        Settings_save();
        Slider_applySettings();
        JsonDocument res;
        res["ok"]         = true;
        res["stepsPerMm"] = stepsPerMm;
        res["mmPerRev"]   = settings.stepsPerRev / stepsPerMm;
        String out;
        serializeJson(res, out);
        server.send(200, "application/json", out);
        return;
    }

    server.send(400, "application/json", "{\"ok\":false}");
}

static void handleLock() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    bool locked = doc["locked"] | false;
    g_locked   = locked;
    g_lockedBy = locked ? String((const char*)(doc["by"] | "")) : String("");
    server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- WiFi provisioning ----------
static void handleWifiStatus() {
    JsonDocument doc;
    doc["mode"]      = Wifi_isStation() ? "sta" : "ap";
    doc["connected"] = Wifi_isStation();
    doc["ssid"]      = Wifi_ssid();
    doc["ip"]        = Wifi_ip();
    doc["apSsid"]    = AP_SSID;
    doc["host"]      = MDNS_HOST;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleWifiScan() {
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n && i < 30; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["lock"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    }
    WiFi.scanDelete();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleWifiSave() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    String ssid = (const char*)(doc["ssid"] | "");
    String pass = (const char*)(doc["pass"] | "");
    if (ssid.length() == 0) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (!Wifi_saveCreds(ssid, pass)) { server.send(500, "application/json", "{\"ok\":false}"); return; }
    server.send(200, "application/json", "{\"ok\":true}");
    g_rebootAt = millis() + 800;   // flush response, then reboot to connect as STA
}

static void handleWifiForget() {
    Wifi_clearCreds();
    server.send(200, "application/json", "{\"ok\":true}");
    g_rebootAt = millis() + 800;   // reboot back into AP mode
}

static void handleNotFound() {
    // In AP mode, redirect any unknown host/URL to our portal root (absolute URL)
    // so the OS captive-portal check trips and pops the interface open.
    if (Wifi_apActive()) {
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    } else {
        server.sendHeader("Location", "/", true);
    }
    server.send(302, "text/plain", "");
}

// ---------- Public ----------
void Web_begin() {
    LittleFS.begin();   // WiFi mode is already configured by Wifi_begin()

    server.on("/",              HTTP_GET,  handleIndex);
    server.on("/api/ping",      HTTP_GET,  handlePing);
    server.on("/api/status",    HTTP_GET,  handleStatus);
    server.on("/api/home",      HTTP_POST, handleHome);
    server.on("/api/skiphome",  HTTP_POST, handleSkipHome);
    server.on("/api/calibrate", HTTP_POST, handleCalibrate);
    server.on("/api/manual",    HTTP_POST, handleManual);
    server.on("/api/auto",      HTTP_POST, handleAuto);
    server.on("/api/estop",     HTTP_POST, handleEstop);
    server.on("/api/goto",      HTTP_POST, handleGoto);
    server.on("/api/lock",      HTTP_POST, handleLock);
    server.on("/api/settings",  HTTP_GET,  handleSettingsGet);
    server.on("/api/settings",  HTTP_POST, handleSettingsPost);
    server.on("/api/wifi",        HTTP_GET,  handleWifiStatus);
    server.on("/api/wifi/scan",   HTTP_GET,  handleWifiScan);
    server.on("/api/wifi",        HTTP_POST, handleWifiSave);
    server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);

    // Serve any other files placed in /data (e.g. future favicon.ico, css splits)
    server.serveStatic("/", LittleFS, "/");

    server.onNotFound(handleNotFound);
    server.begin();
}

void Web_loop() {
    server.handleClient();
    if (g_rebootAt && (int32_t)(millis() - g_rebootAt) >= 0) {
        ESP.restart();
    }
}
