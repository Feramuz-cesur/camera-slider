#include "WebInterface.h"
#include "SliderControl.h"
#include "SliderWifi.h"
#include "Settings.h"
#include "Config.h"

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static WebServer        server(80);   // serves the page + settings/wifi (request/response)
static WebSocketsServer ws(81);       // live status push + low-latency control commands
static uint32_t g_rebootAt = 0;       // 0 = no pending reboot

// Timelapse/layer mode (driven by the mobile app). The print's total layer count
// arrives once via "layerSetup"; each "layer" message maps the current layer to a
// position between the configured start and end on BOTH axes. 0 = no print set up.
static uint16_t g_layerTotal   = 0;
static uint16_t g_layerCurrent = 0;

// Setup wizard: the rail length step is confirmed once per boot, like homing.
// Not persisted - the wizard walks the user through all three steps on each
// power-up, and "keep current" accepts the stored value in one tap.
static bool g_travelSet = false;

// Detailed state token used by our web UI
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
        case STATE_LAYER_MOVE:      return "LAYER_MOVE";
        case STATE_CALIBRATE:       return "CALIBRATE";
        case STATE_FAULT:           return "FAULT";
    }
    return "?";
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

// ---------- Live status + control over WebSocket ----------
static void buildStatus(String& out) {
    JsonDocument doc;
    doc["stateRaw"]  = stateShortName(Slider_state());
    doc["stateText"] = Slider_stateText();
    doc["pos"]       = Slider_positionMm();       // slider mm
    doc["panPos"]    = Slider_position(AXIS_PAN);  // pan degrees
    doc["homed"]     = Slider_isHomed();       // linear axis referenced
    doc["panHomed"]  = Slider_isPanHomed();    // rotary zero accepted
    doc["travelSet"] = g_travelSet;            // rail length confirmed this boot
    doc["maxTravelMm"] = settings.maxTravelMm; // so the wizard can label its buttons
    doc["motors"]    = Slider_motorsEnabled();
    doc["panPower"]  = Slider_axisPowered(AXIS_PAN);
    doc["progress"]  = Slider_autoProgress();
    doc["layerTotal"]   = g_layerTotal;     // 0 = no print configured
    doc["layerCurrent"] = g_layerCurrent;   // last layer commanded
    serializeJson(doc, out);
}

static void broadcastStatus() {
    String out;
    buildStatus(out);
    ws.broadcastTXT(out);
}

// Reply to a single client with a rejection. Most commands are fire-and-forget
// (their result shows up in the next status push), but motion commands that we
// refuse — e.g. before homing — return an explicit error so the mobile app can
// react instead of silently doing nothing.
static void sendError(uint8_t num, const char* code) {
    JsonDocument doc;
    doc["t"]    = "error";
    doc["code"] = code;
    String out;
    serializeJson(doc, out);
    ws.sendTXT(num, out);
}

// One inbound command frame. Commands are fire-and-forget: the result shows up
// in the next status push, so we send no per-command reply (except errors).
static void handleWsText(uint8_t num, uint8_t* payload, size_t length) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, length)) return;
    const char* t = doc["t"] | "";

    auto axisOf = [&]() -> AxisId {
        const char* s = doc["axis"] | "slider";
        return (strcmp(s, "pan") == 0) ? AXIS_PAN : AXIS_SLIDER;
    };

    if (strcmp(t, "manual") == 0) {
        const char* action = doc["action"] | "";
        AxisId axis = axisOf();
        if (strcmp(action, "start") == 0) {
            const char* dir = doc["dir"] | "";
            float defSpeed = (axis == AXIS_PAN) ? settings.panMaxSpeedDegS : settings.maxSpeedMmS;
            float speed = doc["speed"] | defSpeed;
            // "free": the travel-calibration step drives past the stored rail length.
            bool free = doc["free"] | false;
            Slider_manualStart(axis, strcmp(dir, "right") == 0 ? DIR_RIGHT : DIR_LEFT, speed, free);
        } else if (strcmp(action, "stop") == 0) {
            Slider_manualStop(axis);
        }
    } else if (strcmp(t, "goto") == 0) {
        Slider_gotoPos(axisOf(), doc["pos"] | 0.0f);
    } else if (strcmp(t, "setzero") == 0) {
        // "pos" lets the caller name the angle/position this pose stands for;
        // the rotary homing step uses it to make "forward" 180 deg.
        Slider_setReference(axisOf(), doc["pos"] | 0.0f);
    } else if (strcmp(t, "home") == 0) {
        Slider_startHoming();
    } else if (strcmp(t, "homeflip") == 0) {
        // "The carriage is going the wrong way": flip the linear axis direction
        // for good (the same switch as Settings > Flip direction) and home again.
        settings.invertDir = !settings.invertDir;
        Settings_save();
        if (!Slider_restartHoming()) sendError(num, "BUSY");
    } else if (strcmp(t, "axispower") == 0) {
        // Release/hold one axis at rest — the rotary homing step frees the pan
        // so the platform can be turned to face forward by hand.
        if (!Slider_setAxisPower(axisOf(), doc["enabled"] | true)) sendError(num, "BUSY");
    } else if (strcmp(t, "settravel") == 0) {
        // Wizard step 2. With "mm" the current carriage position becomes the new
        // rail length; without it the stored value is simply accepted as-is.
        if (doc["mm"].is<float>()) {
            float mm = doc["mm"];
            if (mm < 10.0f || mm > MAX_TRAVEL_LIMIT_MM) { sendError(num, "BAD_TRAVEL"); return; }
            settings.maxTravelMm = mm;
            // Keep the stored shot range inside the rail we just measured.
            settings.startMm = constrain(settings.startMm, 0.0f, mm);
            settings.endMm   = constrain(settings.endMm,   0.0f, mm);
            Settings_save();
            Slider_applySettings();
        }
        g_travelSet = true;
    } else if (strcmp(t, "skiphome") == 0) {
        Slider_skipHoming();
    } else if (strcmp(t, "estop") == 0) {
        Slider_emergencyStop();
    } else if (strcmp(t, "motors") == 0) {
        Slider_setMotorsEnabled(doc["enabled"] | true);
    } else if (strcmp(t, "auto") == 0) {
        const char* action = doc["action"] | "";
        if (strcmp(action, "start") == 0) {
            float duration = doc["duration"] | 10.0f;
            // Persist any per-axis start/end overrides so the move uses what the UI shows.
            bool changed = false;
            if (doc["sStart"].is<float>() || doc["sEnd"].is<float>()) {
                float a = doc["sStart"] | settings.startMm;
                float b = doc["sEnd"]   | settings.endMm;
                settings.startMm = constrain(a, 0.0f, settings.maxTravelMm);
                settings.endMm   = constrain(b, 0.0f, settings.maxTravelMm);
                changed = true;
            }
            if (doc["pStart"].is<float>() || doc["pEnd"].is<float>()) {
                float a = doc["pStart"] | settings.panStartDeg;
                float b = doc["pEnd"]   | settings.panEndDeg;
                settings.panStartDeg = constrain(a, 0.0f, DEFAULT_PAN_MAX_DEG);
                settings.panEndDeg   = constrain(b, 0.0f, DEFAULT_PAN_MAX_DEG);
                changed = true;
            }
            if (changed) Settings_save();
            Slider_startAuto(duration, doc["enLin"] | true, doc["enRot"] | true);
        } else if (strcmp(action, "stop") == 0) {
            Slider_stopAuto();
        } else if (strcmp(action, "pause") == 0) {
            Slider_pauseAuto();
        } else if (strcmp(action, "resume") == 0) {
            Slider_resumeAuto();
        }
    } else if (strcmp(t, "layerSetup") == 0) {
        // Print started: remember the total layer count. No movement here.
        int total = doc["total"] | 0;
        if (total < 1) { sendError(num, "BAD_TOTAL"); return; }
        g_layerTotal   = (uint16_t)total;
        g_layerCurrent = 0;
    } else if (strcmp(t, "layer") == 0) {
        // Layer changed: map layer n -> position on both axes and move there.
        if (g_layerTotal < 1)       { sendError(num, "NO_SETUP");  return; }
        if (!Slider_isHomed())      { sendError(num, "NOT_HOMED"); return; }
        int n = doc["n"] | 0;
        if (n < 1) n = 1;
        if (n > g_layerTotal) n = g_layerTotal;
        // layer 1 -> start, layer total -> end (single layer stays at start).
        float frac = (g_layerTotal > 1) ? (float)(n - 1) / (float)(g_layerTotal - 1) : 0.0f;
        float sPos = settings.startMm     + (settings.endMm     - settings.startMm)     * frac;
        float pPos = settings.panStartDeg + (settings.panEndDeg - settings.panStartDeg) * frac;
        if (!Slider_gotoBoth(sPos, pPos)) { sendError(num, "BUSY"); return; }
        g_layerCurrent = (uint16_t)n;
    } else if (strcmp(t, "gostart") == 0) {
        // Send both axes to their configured start positions, together.
        if (!Slider_isHomed()) { sendError(num, "NOT_HOMED"); return; }
        if (!Slider_gotoBoth(settings.startMm, settings.panStartDeg)) { sendError(num, "BUSY"); return; }
    } else if (strcmp(t, "goend") == 0) {
        // ...and the same for the end pose.
        if (!Slider_isHomed()) { sendError(num, "NOT_HOMED"); return; }
        if (!Slider_gotoBoth(settings.endMm, settings.panEndDeg)) { sendError(num, "BUSY"); return; }
    } else if (strcmp(t, "simulate") == 0) {
        // Button 2: preview the move using the normal auto mode (default 25 s).
        if (!Slider_isHomed()) { sendError(num, "NOT_HOMED"); return; }
        float duration = doc["duration"] | 25.0f;
        if (!Slider_startAuto(duration, true, true)) { sendError(num, "BUSY"); return; }
    }
    broadcastStatus();   // reflect the command immediately
}

static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    if (type == WStype_TEXT) {
        handleWsText(num, payload, length);
    } else if (type == WStype_CONNECTED) {
        String out;
        buildStatus(out);
        ws.sendTXT(num, out);   // immediate snapshot for the new client
    }
}

static void handleSettingsGet() {
    JsonDocument doc;
    doc["stepsPerMm"]  = settings.stepsPerMm;                         // canonical (external apps)
    doc["mmPerRev"]    = settings.stepsPerRev / settings.stepsPerMm;  // derived for display
    doc["stepsPerRev"] = settings.stepsPerRev;
    doc["maxTravelMm"] = settings.maxTravelMm;
    doc["maxSpeedMmS"] = settings.maxSpeedMmS;
    doc["accelMmS2"]   = settings.accelMmS2;
    doc["jogSpeedMmS"] = settings.jogSpeedMmS;
    doc["homingSpeedMmS"] = settings.homingSpeedMmS;
    doc["useAccel"]    = settings.useAccel;
    doc["invertDir"]   = settings.invertDir;
    doc["startMm"]     = settings.startMm;
    doc["endMm"]       = settings.endMm;
    // Axis 2 (rotary pan)
    doc["panStepsPerRev"]  = settings.panStepsPerRev;
    doc["panMaxSpeedDegS"] = settings.panMaxSpeedDegS;
    doc["panAccelDegS2"]   = settings.panAccelDegS2;
    doc["panJogSpeedDegS"] = settings.panJogSpeedDegS;
    doc["panInvertDir"]    = settings.panInvertDir;
    doc["panStartDeg"]     = settings.panStartDeg;
    doc["panEndDeg"]       = settings.panEndDeg;
    doc["panMaxDeg"]       = DEFAULT_PAN_MAX_DEG;
    doc["panHomeDeg"]      = DEFAULT_PAN_HOME_DEG;   // angle the homed "forward" pose gets
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
    float    jogSpeedMmS = doc["jogSpeedMmS"] | settings.jogSpeedMmS;
    float    homingSpeed = doc["homingSpeedMmS"] | settings.homingSpeedMmS;
    bool     useAccel    = doc["useAccel"]    | settings.useAccel;
    bool     invertDir   = doc["invertDir"]   | settings.invertDir;
    float    startMm     = doc["startMm"]     | settings.startMm;
    float    endMm       = doc["endMm"]       | settings.endMm;

    uint16_t panStepsPerRev  = doc["panStepsPerRev"]  | settings.panStepsPerRev;
    float    panMaxSpeedDegS = doc["panMaxSpeedDegS"] | settings.panMaxSpeedDegS;
    float    panAccelDegS2   = doc["panAccelDegS2"]   | settings.panAccelDegS2;
    float    panJogSpeedDegS = doc["panJogSpeedDegS"] | settings.panJogSpeedDegS;
    bool     panInvertDir    = doc["panInvertDir"]    | settings.panInvertDir;
    float    panStartDeg     = doc["panStartDeg"]     | settings.panStartDeg;
    float    panEndDeg       = doc["panEndDeg"]       | settings.panEndDeg;

    if (mmPerRev    < 0.1f || mmPerRev    > 1000) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (stepsPerRev < 1    || stepsPerRev > 20000){ server.send(400, "application/json", "{\"ok\":false}"); return; }
    float stepsPerMm = stepsPerRev / mmPerRev;
    if (stepsPerMm  < 1 || stepsPerMm  > 3200)  { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (maxTravelMm < 10 || maxTravelMm > MAX_TRAVEL_LIMIT_MM) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (maxSpeedMmS < 1 || maxSpeedMmS > 500)   { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (accelMmS2   < 1 || accelMmS2   > 5000)  { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (homingSpeed < 1 || homingSpeed > 500)   { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (panStepsPerRev < 1 || panStepsPerRev > 60000) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (panMaxSpeedDegS < 1 || panMaxSpeedDegS > 2000) { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (panAccelDegS2   < 1 || panAccelDegS2   > 10000){ server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (jogSpeedMmS     < 1 || jogSpeedMmS     > 500)  { server.send(400, "application/json", "{\"ok\":false}"); return; }
    if (panJogSpeedDegS < 1 || panJogSpeedDegS > 2000) { server.send(400, "application/json", "{\"ok\":false}"); return; }

    settings.stepsPerMm  = stepsPerMm;
    settings.stepsPerRev = stepsPerRev;
    settings.maxTravelMm = maxTravelMm;
    settings.maxSpeedMmS = maxSpeedMmS;
    settings.accelMmS2   = accelMmS2;
    settings.jogSpeedMmS = jogSpeedMmS;
    settings.homingSpeedMmS = homingSpeed;
    settings.useAccel    = useAccel;
    settings.invertDir   = invertDir;
    settings.startMm     = constrain(startMm, 0.0f, maxTravelMm);
    settings.endMm       = constrain(endMm,   0.0f, maxTravelMm);
    settings.panStepsPerRev  = panStepsPerRev;
    settings.panMaxSpeedDegS = panMaxSpeedDegS;
    settings.panAccelDegS2   = panAccelDegS2;
    settings.panJogSpeedDegS = panJogSpeedDegS;
    settings.panInvertDir    = panInvertDir;
    settings.panStartDeg     = constrain(panStartDeg, 0.0f, DEFAULT_PAN_MAX_DEG);
    settings.panEndDeg       = constrain(panEndDeg,   0.0f, DEFAULT_PAN_MAX_DEG);
    Settings_save();
    Slider_applySettings();
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

// Never scans inline: a blocking scan holds the loop (and the radio) for
// seconds, which drops the very client that asked for the list. We answer
// instantly from the cache and let the scan finish in the background - the page
// polls until "scanning" goes false.
static void handleWifiScan() {
    if (server.hasArg("refresh") || Wifi_scanJson() == "[]") Wifi_scanStart();

    String out = "{\"scanning\":";
    out += Wifi_scanBusy() ? "true" : "false";
    out += ",\"nets\":";
    out += Wifi_scanJson();
    out += "}";
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

// ---------- Captive portal ----------
// Right after joining a network every OS fetches a known URL over plain HTTP and
// decides "this is a captive portal" when the answer is not the one it expects.
// Our DNS server maps every hostname to the AP, so those probes land here.
//
// The Location must be a literal IP: the client has no working DNS other than
// ours at this point. And it must not be cached - a cached redirect makes the OS
// skip the probe on the next join and just flag the network as broken.
static void redirectToPortal() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
}

// Registered on the probe URLs themselves so they are answered from RAM instead
// of going through the static handler's (failing) LittleFS lookups first.
static void handleProbe() {
    if (Wifi_apActive()) { redirectToPortal(); return; }
    server.send(404, "text/plain", "Not found");
}

static void handleNotFound() {
    // Only unknown *page* requests get bounced to the portal. API calls from a
    // stale tab and missing assets (favicon...) must 404 instead, otherwise the
    // browser chases the redirect back into a request it cannot satisfy.
    if (Wifi_apActive() && !server.uri().startsWith("/api/") && server.uri() != "/favicon.ico") {
        redirectToPortal();
        return;
    }
    server.send(404, "text/plain", "Not found");
}

// ---------- Public ----------
void Web_begin() {
    LittleFS.begin();   // WiFi mode is already configured by Wifi_begin()

    // HTTP: serve the page and the request/response config endpoints.
    server.on("/",                HTTP_GET,  handleIndex);
    server.on("/api/settings",    HTTP_GET,  handleSettingsGet);
    server.on("/api/settings",    HTTP_POST, handleSettingsPost);
    server.on("/api/wifi",        HTTP_GET,  handleWifiStatus);
    server.on("/api/wifi/scan",   HTTP_GET,  handleWifiScan);
    server.on("/api/wifi",        HTTP_POST, handleWifiSave);
    server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);

    // /index.html must go through handleIndex too, or a client that asks for it
    // by name in AP mode gets the control UI instead of the Wi-Fi setup page.
    server.on("/index.html", HTTP_GET, handleIndex);

    // OS connectivity probes -> the portal, so the phone opens our setup page
    // instead of reporting "connected, no internet" and falling back to mobile data.
    static const char* kProbeUrls[] = {
        "/generate_204", "/gen_204",                            // Android
        "/hotspot-detect.html", "/library/test/success.html",    // iOS / macOS
        "/connecttest.txt", "/ncsi.txt", "/redirect",            // Windows
        "/canonical.html", "/success.txt",                       // Firefox / others
    };
    for (const char* u : kProbeUrls) server.on(u, HTTP_GET, handleProbe);

    // Serve any other files placed in /data (e.g. future favicon.ico, css splits)
    server.serveStatic("/", LittleFS, "/");

    server.onNotFound(handleNotFound);
    server.begin();

    // WebSocket: live status push + low-latency control commands.
    ws.begin();
    ws.onEvent(onWsEvent);
}

void Web_loop() {
    server.handleClient();
    ws.loop();

    // Push the live status to all connected clients at ~10 Hz.
    static uint32_t lastPush = 0;
    uint32_t now = millis();
    if (now - lastPush >= 100) {
        lastPush = now;
        broadcastStatus();
    }

    if (g_rebootAt && (int32_t)(now - g_rebootAt) >= 0) {
        ESP.restart();
    }
}
