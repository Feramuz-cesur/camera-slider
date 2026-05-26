#include "WebInterface.h"
#include "SliderControl.h"
#include "Settings.h"
#include "Config.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static ESP8266WebServer server(80);

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
        case STATE_FAULT:           return "FAULT";
    }
    return "?";
}

// ---------- Handlers ----------
static void handleIndex() {
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
        server.send(500, "text/plain",
            "index.html not found.\nUpload the filesystem image (PlatformIO: Upload Filesystem Image).");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

static void handleStatus() {
    JsonDocument doc;
    doc["state"]     = stateShortName(Slider_state());
    doc["stateText"] = Slider_stateText();
    doc["pos"]       = Slider_positionMm();
    doc["homed"]     = Slider_isHomed();
    doc["progress"]  = Slider_autoProgress();
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleHome() {
    bool ok = Slider_startHoming();
    server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleManual() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    const char* action = doc["action"] | "";
    if (strcmp(action, "start") == 0) {
        const char* dir = doc["dir"] | "";
        bool ok = Slider_manualStart(strcmp(dir, "right") == 0 ? DIR_RIGHT : DIR_LEFT);
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
        const char* dir   = doc["dir"]      | "ltr";
        float duration    = doc["duration"] | 10.0f;
        bool ltr = (strcmp(dir, "ltr") == 0);
        bool ok = Slider_startAuto(ltr, duration);
        server.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    } else if (strcmp(action, "stop") == 0) {
        Slider_stopAuto();
        server.send(200, "application/json", "{\"ok\":true}");
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
    if (!Slider_isHomed()) { server.send(409, "application/json", "{\"ok\":false}"); return; }
    if (pos < 0) pos = 0;
    if (pos > settings.maxTravelMm) pos = settings.maxTravelMm;
    if (pos > Slider_positionMm())  Slider_manualStart(DIR_RIGHT);
    else                            Slider_manualStart(DIR_LEFT);
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSettingsGet() {
    JsonDocument doc;
    doc["stepsPerMm"]  = settings.stepsPerMm;
    doc["maxTravelMm"] = settings.maxTravelMm;
    doc["maxSpeedMmS"] = settings.maxSpeedMmS;
    doc["accelMmS2"]   = settings.accelMmS2;
    doc["useAccel"]    = settings.useAccel;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

static void handleSettingsPost() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", "{\"ok\":false}"); return; }

    float stepsPerMm  = doc["stepsPerMm"]  | settings.stepsPerMm;
    float maxTravelMm = doc["maxTravelMm"] | settings.maxTravelMm;
    float maxSpeedMmS = doc["maxSpeedMmS"] | settings.maxSpeedMmS;
    float accelMmS2   = doc["accelMmS2"]   | settings.accelMmS2;
    bool  useAccel    = doc["useAccel"]    | settings.useAccel;

    if (stepsPerMm  < 1 || stepsPerMm  > 3200)  { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (maxTravelMm < 10 || maxTravelMm > 2000) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (maxSpeedMmS < 1 || maxSpeedMmS > 500)   { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (accelMmS2   < 1 || accelMmS2   > 5000)  { server.send(400, "application/json", "{\"ok\":false}"); return; }

    settings.stepsPerMm  = stepsPerMm;
    settings.maxTravelMm = maxTravelMm;
    settings.maxSpeedMmS = maxSpeedMmS;
    settings.accelMmS2   = accelMmS2;
    settings.useAccel    = useAccel;
    Settings_save();
    Slider_applySettings();
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotFound() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
}

// ---------- Public ----------
void Web_begin() {
    LittleFS.begin();

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);

    server.on("/",              HTTP_GET,  handleIndex);
    server.on("/api/status",    HTTP_GET,  handleStatus);
    server.on("/api/home",      HTTP_POST, handleHome);
    server.on("/api/manual",    HTTP_POST, handleManual);
    server.on("/api/auto",      HTTP_POST, handleAuto);
    server.on("/api/estop",     HTTP_POST, handleEstop);
    server.on("/api/goto",      HTTP_POST, handleGoto);
    server.on("/api/settings",  HTTP_GET,  handleSettingsGet);
    server.on("/api/settings",  HTTP_POST, handleSettingsPost);

    // Serve any other files placed in /data (e.g. future favicon.ico, css splits)
    server.serveStatic("/", LittleFS, "/");

    server.onNotFound(handleNotFound);
    server.begin();
}

void Web_loop() {
    server.handleClient();
}
