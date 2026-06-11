#pragma once
#include <Arduino.h>

enum SliderState {
    STATE_BOOT,             // just powered on, homing required
    STATE_HOMING_FAST,      // moving fast toward limit
    STATE_HOMING_BACKOFF,   // backing off after hitting limit
    STATE_HOMING_SLOW,      // slow re-approach
    STATE_IDLE,             // homed, ready
    STATE_MANUAL,           // manual jog active
    STATE_AUTO_REPOSITION,  // going to start of auto move
    STATE_AUTO_MOVE,        // executing the timed auto move
    STATE_AUTO_PAUSED,      // auto move paused (resumable)
    STATE_CALIBRATE,        // spinning a fixed number of steps for calibration
    STATE_FAULT
};

enum Direction {
    DIR_LEFT  = -1,
    DIR_RIGHT =  1
};

void Slider_begin();
void Slider_update();           // call frequently from loop()

// ---- Commands ----
bool Slider_startHoming();
void Slider_skipHoming();       // skip homing: treat current position as 0, unlock movement
void Slider_stop();             // soft stop (use after manual release)
void Slider_emergencyStop();

// Calibration: spin a fixed number of steps (signed), then return to idle
bool Slider_calibrateSpin(long steps);

bool Slider_manualStart(Direction d, float speedMmS);   // press (jog at speedMmS)
void Slider_manualStop();                               // release

// Absolute move to a millimetre position (0..maxTravel). Non-blocking; returns
// true if accepted. Used by /api/goto (external apps + "go to home").
bool Slider_gotoMm(float mm);

// Auto move: direction = "ltr" (0 -> max) or "rtl" (max -> 0), duration in seconds
bool Slider_startAuto(bool leftToRight, float durationSec);
void Slider_stopAuto();
bool Slider_pauseAuto();
bool Slider_resumeAuto();

// ---- Status getters ----
SliderState Slider_state();
float       Slider_positionMm();
bool        Slider_isHomed();
float       Slider_autoProgress();      // 0..1 during auto move
const char* Slider_stateText();

// ---- Settings refresh (after EEPROM save) ----
void Slider_applySettings();
