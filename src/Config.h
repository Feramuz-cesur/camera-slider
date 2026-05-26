#pragma once
#include <Arduino.h>

// ---------- Pin Definitions (ESP-01) ----------
#define PIN_STEP        0   // GPIO0 -> A4988 STEP
#define PIN_DIR         2   // GPIO2 -> A4988 DIR
#define PIN_ENABLE      1   // GPIO1 (TX) -> A4988 EN (active LOW)
#define PIN_LIMIT       3   // GPIO3 (RX) -> Limit Switch (to GND when triggered)

// ---------- A4988 / Motor ----------
#define MOTOR_ENABLED   LOW
#define MOTOR_DISABLED  HIGH

// Homing direction: which DIR level moves carriage TOWARD the limit switch.
// Flip this if your wiring/mechanics go the wrong way during homing.
#define HOMING_DIR_LEVEL    LOW     // LOW = move toward limit switch
#define AWAY_FROM_LIMIT     HIGH    // opposite

// ---------- Homing Behavior ----------
#define HOMING_SPEED_STEPS_S    800     // fast approach
#define HOMING_BACKOFF_STEPS    200     // back off after touching limit
#define HOMING_SLOW_SPEED_S     200     // slow re-approach for accuracy

// ---------- WiFi Access Point ----------
#define AP_SSID         "CameraSlider"
#define AP_PASSWORD     "slider1234"     // min 8 chars
#define AP_CHANNEL      6

// ---------- Defaults (used when EEPROM empty) ----------
#define DEFAULT_STEPS_PER_MM    80.0f    // GT2 20T pulley + 1/16 microstep
#define DEFAULT_MAX_TRAVEL_MM   300.0f
#define DEFAULT_MAX_SPEED_MMS   60.0f
#define DEFAULT_ACCEL_MMS2      80.0f
#define DEFAULT_USE_ACCEL       true

// ---------- Misc ----------
#define STATUS_UPDATE_MS        150
