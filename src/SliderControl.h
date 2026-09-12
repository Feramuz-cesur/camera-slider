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
// Called when the carriage runs AWAY from the switch: aborts the run, flips the
// linear axis direction (caller persists settings.invertDir) and homes again.
bool Slider_restartHoming();
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

// Release/energize a SINGLE axis while the rig is at rest. Used by the rotary
// homing step, which asks the user to turn the platform to face forward by hand:
// the coils must be free for that. Any motion command re-energizes everything.
bool Slider_setAxisPower(AxisId axis, bool on);
bool Slider_axisPowered(AxisId axis);

// Per-axis manual jog. speed is in the axis's own units/s (mm/s or deg/s).
// unbounded lifts the axis's normal travel ceiling (linear axis only) so the
// setup wizard can drive past a stale maxTravelMm while measuring the real rail
// length. The limit switch still guards the zero end.
bool Slider_manualStart(AxisId axis, Direction d, float speed, bool unbounded = false);
void Slider_manualStop(AxisId axis);

// Define the axis's current physical position as posUnits (mm or deg) without
// moving it. Accepting the slider's reference also unlocks movement (counts as
// homed). The rotary axis homes to 180 deg, not 0, so the user keeps a half turn
// of travel on both sides of "forward".
bool Slider_setReference(AxisId axis, float posUnits);
bool Slider_setZero(AxisId axis);   // == Slider_setReference(axis, 0)

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
bool        Slider_isHomed();               // linear axis referenced (limit switch / skip)
bool        Slider_isPanHomed();            // rotary zero accepted by the user
float       Slider_autoProgress();          // 0..1 during auto move
const char* Slider_stateText();

// ---- Settings refresh (after EEPROM save) ----
void Slider_applySettings();
