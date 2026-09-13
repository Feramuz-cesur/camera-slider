#pragma once
#include <Arduino.h>

// ---------- Pin Definitions (ESP32-C3 0.42" OLED board) ----------
// A4988 stepper driver (NEMA 17). STEP/DIR/EN are "safe" pins on this board
// (no strapping / flash / USB involvement). EN on the A4988 is active-LOW.
// Wire A4988: STEP, DIR, ENABLE, plus MS1/MS2/MS3 (set by jumpers for the
// microstep mode — see DRIVER_MICROSTEP below), RESET tied to SLEEP (both HIGH).
// ---- Axis 1: Linear slider (A4988 #1, NEMA 17 + GT2 belt) ----
#define PIN_STEP        0   // GPIO0  -> A4988 #1 STEP
#define PIN_DIR         1   // GPIO1  -> A4988 #1 DIR
#define PIN_EN          3   // GPIO3  -> A4988 #1 ENABLE (active LOW)
#define PIN_LIMIT       4   // GPIO4  -> Slider limit switch (to GND when triggered)

// ---- Axis 2: Rotary pan (A4988 #2). 360° platform on top of the slider. ----
// GPIO20/21 are UART0 by default, but Serial runs over native USB-CDC here
// (ARDUINO_USB_CDC_ON_BOOT=1), so they are free for general use.
#define PIN_STEP2       7   // GPIO7  -> A4988 #2 STEP
#define PIN_DIR2        10  // GPIO10 -> A4988 #2 DIR  (was ULN2003 IN4 on the old setup)
#define PIN_EN2         20  // GPIO20 -> A4988 #2 ENABLE (active LOW)
#define PIN_LIMIT2      21  // GPIO21 -> Pan limit switch (optional/reserved; INPUT_PULLUP)
// Set PAN_HAS_LIMIT to 1 once a switch is wired to PIN_LIMIT2 and you want
// the pan axis to home against it. Left 0 = pan treats power-on as 0°.
#define PAN_HAS_LIMIT   0

// Onboard 0.42" OLED (SSD1306 72x40, I2C addr 0x3C) — fixed by the board.
#define PIN_OLED_SDA    5   // GPIO5
#define PIN_OLED_SCL    6   // GPIO6

// Avoid: GPIO2 (strapping, WS2812 on some revisions), GPIO8/9 (strapping/BOOT
// button), GPIO18/19 (native USB D-/D+), GPIO11-17 (internal flash).

// ---------- Homing Behavior ----------
// Fast approach speed toward the switch is user-configurable (settings.homingSpeedMmS);
// the values below tune the backoff/re-approach phase.
#define HOMING_BACKOFF_STEPS    200     // back off after touching limit (~2.5 mm @ 80 steps/mm)
#define HOMING_SLOW_SPEED_S     400     // slow re-approach for accuracy (steps/s)

// ---------- WiFi Access Point (fallback / provisioning) ----------
#define AP_SSID         "CameraSlider"
#define AP_PASSWORD     "12345678"     // min 8 chars
#define AP_CHANNEL      6
// ESP32-C3 0.42" OLED boards have a badly matched PCB antenna: at the default
// ~20 dBm the RF output distorts and clients cannot complete association with
// the AP (and STA connects get flaky). ~8.5 dBm is the widely used sweet spot.
#define WIFI_TX_POWER   WIFI_POWER_8_5dBm

// ---------- WiFi scan (provisioning page) ----------
// The scan is ASYNCHRONOUS and swept ONE CHANNEL AT A TIME, for two reasons:
//
// 1. A scan parks the radio on each channel in turn, so an all-channels sweep
//    takes the AP off its own channel for seconds on end: associated clients
//    stop hearing beacons, new clients cannot complete association, and (if the
//    call also blocks) neither the captive-portal DNS nor the web server answer.
//    Scanning a single channel per step keeps every absence short.
// 2. Arduino derives its scan deadline from this very value
//    (WiFiScan.cpp: _scanTimeout = max_ms_per_chan * 20). For an all-channels
//    sweep that deadline expires mid-scan while the softAP keeps beaconing, and
//    scanComplete() reports WIFI_SCAN_FAILED even though the scan was fine - the
//    result is an always-empty network list. Asking for one channel makes the
//    derived deadline (dwell x 20) enormously generous for the work requested.
//
// What actually matters for clients is not how long the whole sweep takes, but
// (a) how long the AP is missing in one stretch and (b) what fraction of the
// time it is missing. A phone tolerates losing a beacon or two; it gives up when
// the AP vanishes for hundreds of milliseconds at a time. So keep the dwell
// short and the gap between channels long: 150 ms off / 250 ms on means the AP
// is present 60% of the sweep and never absent for more than ~150 ms, versus
// 300/120 (74% absent, in 300 ms blocks) which was enough to break association
// and DHCP on phones that had just joined.
#define WIFI_SCAN_DWELL_MS       150    // per-channel dwell -> 3 s deadline for that channel
#define WIFI_SCAN_CHANNEL_GAP_MS 250    // AP breathing room between channels
#define WIFI_SCAN_LAST_CHANNEL   13     // 2.4 GHz channels 1..13
#define WIFI_SCAN_MIN_PERIOD_MS  6000   // ignore refresh requests fired faster than this
#define WIFI_SCAN_MAX_RESULTS    30
// A channel scan that never reports back leaves the radio parked off the AP
// channel indefinitely - the AP simply vanishes and clients cannot even get a
// DHCP lease, with nothing to recover it short of a power cycle. Abandon a
// channel that takes longer than this (dwell is 150 ms, so this is ~13x slack).
#define WIFI_SCAN_CHANNEL_TIMEOUT_MS 2000
// The portal page also holds its first automatic sweep back a few seconds (see
// network.html), so a phone that has just joined can finish DHCP and its
// captive-portal probe before the radio starts hopping channels.

// ---------- WiFi Station (connect to your network) ----------
#define WIFI_STA_TIMEOUT_MS  15000       // give up after this and fall back to AP
#define MDNS_HOST            "camera-slider"   // reachable as camera-slider.local
#define DEVICE_NAME          "Fergineer Slider"  // user-visible device name
#define WIFI_CREDS_FILE      "/wifi.json"      // saved credentials on LittleFS

// ---------- Defaults (used when EEPROM empty) ----------
// NEMA 17 (1.8°/step = 200 full steps/rev) driven by an A4988 at 1/16 microstep.
// steps/rev = 200 * 16 = 3200; with a GT2 20T pulley (40 mm/rev) that is
// 3200 / 40 = 80 steps/mm. Set DRIVER_MICROSTEP to match your A4988 jumpers.
#define MOTOR_FULL_STEPS_PER_REV 200     // NEMA 17 = 1.8° per full step
#define DRIVER_MICROSTEP         16      // A4988 MS1/MS2/MS3 jumper setting (1,2,4,8,16)
#define DEFAULT_STEPS_PER_REV   (MOTOR_FULL_STEPS_PER_REV * DRIVER_MICROSTEP)  // 3200
#define DEFAULT_MM_PER_REV      40.0f    // GT2 20T pulley travel per turn
#define DEFAULT_MAX_TRAVEL_MM   300.0f
// Hard ceiling for the rail length. It bounds what the setup wizard's travel
// calibration may jog to (the stored maxTravelMm is unknown/too small at that
// point, so the normal jog ceiling cannot be used) and what /api/settings accepts.
#define MAX_TRAVEL_LIMIT_MM     2000.0f
#define DEFAULT_MAX_SPEED_MMS   50.0f    // NEMA 17 + A4988 handles much higher rates
#define DEFAULT_ACCEL_MMS2      80.0f
#define DEFAULT_JOG_SPEED_MMS   30.0f    // manual jog speed (mm/s)
#define DEFAULT_HOMING_SPEED_MMS 10.0f   // approach speed toward the limit switch
#define DEFAULT_USE_ACCEL       false
#define DEFAULT_INVERT_DIR      false

// Auto-move start/end for the linear axis (mm). Defaults span the full rail.
#define DEFAULT_START_MM        0.0f
#define DEFAULT_END_MM          DEFAULT_MAX_TRAVEL_MM

// ---------- Axis 2: Rotary pan defaults ----------
// Steps for one full 360° turn of the camera platform. Direct drive on a
// 1.8° NEMA 17 at 1/16 microstep = 3200; increase for a geared head and
// calibrate from the web UI. steps/deg = panStepsPerRev / 360.
#define DEFAULT_PAN_STEPS_PER_REV (MOTOR_FULL_STEPS_PER_REV * DRIVER_MICROSTEP)  // 3200
#define DEFAULT_PAN_MAX_DEG       360.0f   // usable rotation range
#define DEFAULT_PAN_MAX_SPEED_DEGS 90.0f   // deg/s upper limit
#define DEFAULT_PAN_ACCEL_DEGS2    180.0f  // deg/s^2
#define DEFAULT_PAN_JOG_SPEED_DEGS 45.0f   // manual jog speed (deg/s)
#define DEFAULT_PAN_INVERT_DIR     false
// Angle the rotary axis is given when the user accepts its home position. The
// platform has no limit switch: the user turns the phone holder to face forward
// and that pose becomes this angle. 180 puts "forward" in the middle of the
// 0..360 range, so there is a full half turn of travel available either way.
#define DEFAULT_PAN_HOME_DEG       180.0f
#define DEFAULT_PAN_START_DEG      0.0f
#define DEFAULT_PAN_END_DEG        360.0f

// ---------- Misc ----------
#define STATUS_UPDATE_MS        150
#define DISPLAY_UPDATE_MS       500     // OLED refresh period (only while motor idle)
