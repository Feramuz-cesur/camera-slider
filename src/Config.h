#pragma once
#include <Arduino.h>

// ---------- Pin Definitions (ESP32-C3 0.42" OLED board) ----------
// ULN2003A driver inputs (28BYJ-48). All four are "safe" pins on this board
// (no strapping / flash / USB involvement).
#define PIN_IN1         0   // GPIO0  -> ULN2003 IN1
#define PIN_IN2         1   // GPIO1  -> ULN2003 IN2
#define PIN_IN3         3   // GPIO3  -> ULN2003 IN3
#define PIN_IN4         10  // GPIO10 -> ULN2003 IN4

#define PIN_LIMIT       4   // GPIO4  -> Limit Switch (to GND when triggered)

// Onboard 0.42" OLED (SSD1306 72x40, I2C addr 0x3C) — fixed by the board.
#define PIN_OLED_SDA    5   // GPIO5
#define PIN_OLED_SCL    6   // GPIO6

// Avoid: GPIO2 (strapping, WS2812 on some revisions), GPIO8/9 (strapping/BOOT
// button), GPIO20/21 (UART0).

// ---------- Homing Behavior ----------
// Fast approach speed toward the switch is user-configurable (settings.homingSpeedMmS);
// the values below tune the backoff/re-approach phase.
#define HOMING_BACKOFF_STEPS    200     // back off after touching limit (~2 mm)
#define HOMING_SLOW_SPEED_S     150     // slow re-approach for accuracy (steps/s)

// ---------- WiFi Access Point (fallback / provisioning) ----------
#define AP_SSID         "CameraSlider"
#define AP_PASSWORD     "slider1234"     // min 8 chars
#define AP_CHANNEL      6

// ---------- WiFi Station (connect to your network) ----------
#define WIFI_STA_TIMEOUT_MS  15000       // give up after this and fall back to AP
#define MDNS_HOST            "camera-slider"   // reachable as camera-slider.local
#define DEVICE_NAME          "Fergineer Slider"  // user-visible name (external apps, e.g. PrintLapse)
#define WIFI_CREDS_FILE      "/wifi.json"      // saved credentials on LittleFS

// ---------- Defaults (used when EEPROM empty) ----------
// 28BYJ-48 in half-step mode: ~4096 steps per output-shaft revolution.
// stepsPerMm is derived: stepsPerRev / mmPerRev  (4096 / 40 = 102.4)
#define DEFAULT_STEPS_PER_REV   4096     // 28BYJ-48 half-step (use calibration to fine-tune)
#define DEFAULT_MM_PER_REV      40.0f    // GT2 20T pulley travel per turn
#define DEFAULT_MAX_TRAVEL_MM   300.0f
#define DEFAULT_MAX_SPEED_MMS   6.0f     // 28BYJ-48 tops out around ~600-700 half-steps/s
#define DEFAULT_ACCEL_MMS2      20.0f
#define DEFAULT_HOMING_SPEED_MMS 4.0f    // approach speed toward the limit switch
#define DEFAULT_USE_ACCEL       true
#define DEFAULT_INVERT_DIR      false

// ---------- Misc ----------
#define STATUS_UPDATE_MS        150
#define DISPLAY_UPDATE_MS       500     // OLED refresh period (only while motor idle)
