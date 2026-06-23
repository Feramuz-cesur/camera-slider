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
    STATE_LAYER_MOVE,       // timelapse/layer mode: both axes seeking a target
    STATE_CALIBRATE,        // spinning a fixed number of steps for calibration
    STATE_FAULT
};

enum Direction {
    DIR_LEFT  = -1,         // toward 0 (slider: left, pan: CCW)
    DIR_RIGHT =  1          // toward max (slider: right, pan: CW)
};

// Motion axes. AXIS_SLIDER is the linear rail (units = mm); AXIS_PAN is the
// rotary platform on top of it (units = degrees, 0..360).
enum AxisId {
    AXIS_SLIDER = 0,
    AXIS_PAN    = 1,
    AXIS_COUNT  = 2
};

void Slider_begin();
void Slider_update();           // call frequently from loop()

// ---- Commands ----
bool Slider_startHoming();       // homes the linear axis (pan unaffected)
void Slider_skipHoming();        // skip homing: treat current positions as 0, unlock movement
void Slider_stop();              // soft stop (use after manual release)
void Slider_emergencyStop();     // stop all motion immediately

// Calibration: spin the linear axis a fixed number of steps (signed), then idle
bool Slider_calibrateSpin(long steps);

// Motor power. Motors are held energized by default (even when idle) so they
// resist being back-driven. Disable releases the coils (only allowed when not
// moving); any new motion command re-enables and re-arms the "hold" behaviour.
bool Slider_setMotorsEnabled(bool enabled);
bool Slider_motorsEnabled();

// Per-axis manual jog. speed is in the axis's own units/s (mm/s or deg/s).
bool Slider_manualStart(AxisId axis, Direction d, float speed);
void Slider_manualStop(AxisId axis);

// Define the axis's current physical position as its new zero (origin) without
// moving. Accepting the slider's zero also unlocks movement (counts as homed).
bool Slider_setZero(AxisId axis);

// Absolute move of one axis to a position in its own units (mm or deg).
// Non-blocking; returns true if accepted.
bool Slider_gotoPos(AxisId axis, float pos);
// Convenience wrapper kept for external apps (PrintLapse): linear axis only.
bool Slider_gotoMm(float mm);

// Move BOTH axes simultaneously to absolute targets (slider mm, pan deg), each at
// its own max speed/accel. Used by the mobile timelapse/layer mode and the
// "go to start" command. Non-blocking; returns true if accepted.
bool Slider_gotoBoth(float sliderMm, float panDeg);

// Auto move: both axes travel from their configured start->end position
// (settings.startMm/endMm and settings.panStartDeg/panEndDeg) simultaneously,
// completing together in roughly durationSec seconds.
bool Slider_startAuto(float durationSec, bool moveLin = true, bool moveRot = true);
void Slider_stopAuto();
bool Slider_pauseAuto();
bool Slider_resumeAuto();

// ---- Status getters ----
SliderState Slider_state();
float       Slider_position(AxisId axis);   // current position in axis units
float       Slider_positionMm();            // == Slider_position(AXIS_SLIDER)
float       Slider_axisMax(AxisId axis);    // usable range max (maxTravelMm / 360)
bool        Slider_isHomed();
float       Slider_autoProgress();          // 0..1 during auto move
const char* Slider_stateText();

// ---- Settings refresh (after EEPROM save) ----
void Slider_applySettings();
