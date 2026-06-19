#include "SliderControl.h"
#include "Settings.h"
#include "Config.h"
#include <AccelStepper.h>

// Two A4988 step/direction drivers. Axis 0 = linear slider (mm), axis 1 = rotary
// pan (deg). Each driver has its own STEP/DIR/ENABLE; the ENABLE pins are active
// LOW. AccelStepper toggles STEP/DIR; enableOutputs()/disableOutputs() energize
// or release the coils so resting motors/drivers stay cool.

struct Axis {
    AccelStepper stepper;
    uint8_t      limitPin;
    bool         hasLimit;     // true when a homing switch is wired/used
    bool         energized;    // tracks enableOutputs/disableOutputs state
};

static Axis ax[AXIS_COUNT] = {
    { AccelStepper(AccelStepper::DRIVER, PIN_STEP,  PIN_DIR),  PIN_LIMIT,  true,                  false },
    { AccelStepper(AccelStepper::DRIVER, PIN_STEP2, PIN_DIR2), PIN_LIMIT2, (bool)PAN_HAS_LIMIT,   false },
};

static SliderState state = STATE_BOOT;
static bool        homed = false;
static AxisId      manualAxis = AXIS_SLIDER;   // which axis a manual jog / goto drives
static bool        holdEnabled = true;         // keep coils energized while idle (holding torque)

// auto-move bookkeeping. Both axes are driven from one shared clock: every loop
// each axis is commanded to start + (end-start)*s, where s is the eased fraction
// of elapsed time. Neither axis can reach its end before the full duration, so
// they always finish together no matter how their distances/speeds differ.
static long     autoStartStep[AXIS_COUNT]  = {0, 0};
static long     autoTargetStep[AXIS_COUNT] = {0, 0};
static bool     autoMoving[AXIS_COUNT]     = {false, false};  // axis has a real move this run
static bool     autoEase[AXIS_COUNT]       = {true, true};    // smooth-step vs linear per axis
static uint32_t autoStartedMs      = 0;
static uint32_t autoDurationMs     = 0;
static uint32_t autoElapsedAtPause = 0;   // frozen elapsed time while paused

// ---------- Unit conversion ----------
static inline float stepsPerUnit(AxisId a) {
    // slider: steps per mm; pan: steps per degree
    return (a == AXIS_SLIDER) ? settings.stepsPerMm
                              : (settings.panStepsPerRev / 360.0f);
}
static inline long  unitToSteps(AxisId a, float u) { return (long)(u * stepsPerUnit(a) + (u < 0 ? -0.5f : 0.5f)); }
static inline float stepsToUnit(AxisId a, long s)  { return (float)s / stepsPerUnit(a); }

static inline float axisMaxSpeedUnits(AxisId a) {
    return (a == AXIS_SLIDER) ? settings.maxSpeedMmS : settings.panMaxSpeedDegS;
}
static inline float axisAccelUnits(AxisId a) {
    return (a == AXIS_SLIDER) ? settings.accelMmS2 : settings.panAccelDegS2;
}
static inline bool axisInvert(AxisId a) {
    return (a == AXIS_SLIDER) ? settings.invertDir : settings.panInvertDir;
}

// A deliberately huge acceleration used to make a move start/stop effectively
// instantly (e.g. jog response, or the linear axis with "smooth ramping" off).
static inline float instantAccelSteps(AxisId a) {
    return axisMaxSpeedUnits(a) * stepsPerUnit(a) * 50.0f;
}

// Map a 0..1 time fraction to a 0..1 motion fraction. Smooth-step eases in/out
// (gentle starts and stops for video); linear is constant velocity.
static inline float easeFrac(float f, bool smooth) {
    if (f <= 0.0f) return 0.0f;
    if (f >= 1.0f) return 1.0f;
    return smooth ? (f * f * (3.0f - 2.0f * f)) : f;
}
// Peak speed reached relative to the average. Smooth-step peaks at 1.5x average
// (at the midpoint); a linear ramp holds the average the whole way.
static inline float peakSpeedFactor(bool smooth) { return smooth ? 1.5f : 1.0f; }

float Slider_axisMax(AxisId a) {
    return (a == AXIS_SLIDER) ? settings.maxTravelMm : DEFAULT_PAN_MAX_DEG;
}

// ---------- Power management ----------
static void axisPower(AxisId a, bool on) {
    if (on == ax[a].energized) return;
    if (on) ax[a].stepper.enableOutputs();
    else    ax[a].stepper.disableOutputs();
    ax[a].energized = on;
}

// Apply coil power for a given state. Motors are kept energized whenever
// holdEnabled is set (the default), so they hold position even when idle and
// stay on after a move completes. Starting any motion re-arms the hold flag, so
// a one-shot "disable" only lasts until the next command. The user can release
// the coils on demand via Slider_setMotorsEnabled(false).
static void applyPowerFor(SliderState s) {
    bool moving = !(s == STATE_IDLE || s == STATE_BOOT || s == STATE_FAULT);
    if (moving) holdEnabled = true;
    for (int i = 0; i < AXIS_COUNT; i++) axisPower((AxisId)i, holdEnabled);
}

// ---------- Setup ----------
static void configureAxis(AxisId a) {
    pinMode(ax[a].limitPin, INPUT_PULLUP);
    ax[a].stepper.setEnablePin(a == AXIS_SLIDER ? PIN_EN : PIN_EN2);
    // A4988 ENABLE is active LOW -> mark the enable pin inverted.
    ax[a].stepper.setPinsInverted(axisInvert(a), false, true);
    ax[a].stepper.setMaxSpeed(axisMaxSpeedUnits(a) * stepsPerUnit(a));
    ax[a].stepper.setAcceleration(axisAccelUnits(a) * stepsPerUnit(a));
    ax[a].stepper.setCurrentPosition(0);
    ax[a].stepper.disableOutputs();
    ax[a].energized = false;
}

void Slider_begin() {
    for (int i = 0; i < AXIS_COUNT; i++) configureAxis((AxisId)i);
    state = STATE_BOOT;
    homed = false;
    applyPowerFor(state);   // energize and hold from power-on (holdEnabled default true)
}

bool Slider_setMotorsEnabled(bool enabled) {
    if (!enabled) {
        // Releasing the coils mid-move would drop steps; only allow it at rest.
        if (state != STATE_IDLE && state != STATE_BOOT && state != STATE_FAULT) return false;
        holdEnabled = false;
    } else {
        holdEnabled = true;
    }
    for (int i = 0; i < AXIS_COUNT; i++) axisPower((AxisId)i, holdEnabled);
    return true;
}

bool Slider_motorsEnabled() { return holdEnabled; }

void Slider_applySettings() {
    for (int i = 0; i < AXIS_COUNT; i++) {
        AxisId a = (AxisId)i;
        float maxSpeedSteps = axisMaxSpeedUnits(a) * stepsPerUnit(a);
        float accelSteps    = axisAccelUnits(a)    * stepsPerUnit(a);
        ax[a].stepper.setPinsInverted(axisInvert(a), false, true);
        ax[a].stepper.setMaxSpeed(maxSpeedSteps);
        // The linear axis honours the "smooth ramping" toggle; pan always ramps.
        bool useAccel = (a == AXIS_SLIDER) ? settings.useAccel : true;
        ax[a].stepper.setAcceleration(useAccel ? accelSteps : maxSpeedSteps * 50.0f);
    }
}

// ---------- Helpers ----------
static bool limitTriggered(AxisId a) {
    if (!ax[a].hasLimit) return false;
    // INPUT_PULLUP + switch to GND => LOW when pressed
    return digitalRead(ax[a].limitPin) == LOW;
}

// ---------- Commands ----------
bool Slider_startHoming() {
    if (state != STATE_BOOT && state != STATE_IDLE && state != STATE_FAULT) return false;

    AccelStepper& s = ax[AXIS_SLIDER].stepper;
    float homingStepsS = settings.homingSpeedMmS * settings.stepsPerMm;
    s.setMaxSpeed(homingStepsS);
    s.setAcceleration(homingStepsS * 4);
    long bigSteps = unitToSteps(AXIS_SLIDER, settings.maxTravelMm + 200.0f);
    s.setCurrentPosition(bigSteps);          // pretend we're past max
    s.moveTo(0);                             // target 0 (toward switch)
    state = STATE_HOMING_FAST;
    homed = false;
    return true;
}

void Slider_skipHoming() {
    // Skip homing: assume both axes are at 0 and unlock movement.
    for (int i = 0; i < AXIS_COUNT; i++) {
        ax[i].stepper.setCurrentPosition(0);
        ax[i].stepper.moveTo(0);
    }
    Slider_applySettings();
    homed = true;
    state = STATE_IDLE;
}

bool Slider_calibrateSpin(long steps) {
    if (state != STATE_IDLE) return false;
    Slider_applySettings();
    ax[AXIS_SLIDER].stepper.move(steps);   // relative move on the linear axis
    state = STATE_CALIBRATE;
    return true;
}

void Slider_stop() {
    for (int i = 0; i < AXIS_COUNT; i++) ax[i].stepper.stop();
    if (state == STATE_MANUAL || state == STATE_AUTO_MOVE || state == STATE_AUTO_REPOSITION) {
        state = STATE_IDLE;
    }
}

void Slider_emergencyStop() {
    for (int i = 0; i < AXIS_COUNT; i++) {
        ax[i].stepper.setSpeed(0);
        ax[i].stepper.moveTo(ax[i].stepper.currentPosition());
    }
    state = STATE_IDLE;
}

bool Slider_manualStart(AxisId axis, Direction d, float speed) {
    if (!homed) return false;
    if (state != STATE_IDLE && state != STATE_MANUAL) return false;
    if (speed <= 0) return false;

    Slider_applySettings();
    manualAxis = axis;

    float maxU = axisMaxSpeedUnits(axis);
    if (speed > maxU) speed = maxU;
    ax[axis].stepper.setMaxSpeed(speed * stepsPerUnit(axis));
    // Smooth ramping governs jog too: ramp at the configured acceleration when
    // on, or snap instantly to/from speed when off.
    ax[axis].stepper.setAcceleration(settings.useAccel ? axisAccelUnits(axis) * stepsPerUnit(axis)
                                                       : instantAccelSteps(axis));

    long target = (d == DIR_RIGHT) ? unitToSteps(axis, Slider_axisMax(axis)) : 0;
    ax[axis].stepper.moveTo(target);
    state = STATE_MANUAL;
    return true;
}

void Slider_manualStop(AxisId axis) {
    if (state == STATE_MANUAL && manualAxis == axis) {
        // Ramp down when smooth ramping is on, else stop the instant it's released.
        ax[axis].stepper.setAcceleration(settings.useAccel ? axisAccelUnits(axis) * stepsPerUnit(axis)
                                                           : instantAccelSteps(axis));
        ax[axis].stepper.stop();
    }
}

// Treat the axis's current physical position as its new zero (origin), without
// moving. Accepting the slider's zero also unlocks movement (counts as homed).
bool Slider_setZero(AxisId axis) {
    if (state != STATE_IDLE && state != STATE_BOOT && state != STATE_FAULT) return false;
    ax[axis].stepper.setCurrentPosition(0);
    ax[axis].stepper.moveTo(0);
    if (axis == AXIS_SLIDER) homed = true;
    state = STATE_IDLE;
    return true;
}

bool Slider_gotoPos(AxisId axis, float pos) {
    if (!homed) return false;
    if (state != STATE_IDLE && state != STATE_MANUAL) return false;
    if (pos < 0) pos = 0;
    if (pos > Slider_axisMax(axis)) pos = Slider_axisMax(axis);
    Slider_applySettings();
    manualAxis = axis;
    ax[axis].stepper.moveTo(unitToSteps(axis, pos));
    state = STATE_MANUAL;
    return true;
}

bool Slider_gotoMm(float mm) {
    return Slider_gotoPos(AXIS_SLIDER, mm);
}

bool Slider_startAuto(float durationSec, bool moveLin, bool moveRot) {
    if (!homed) return false;
    if (state != STATE_IDLE) return false;
    if (durationSec <= 0.1f) return false;

    float startU[AXIS_COUNT] = { settings.startMm, settings.panStartDeg };
    float endU[AXIS_COUNT]   = { settings.endMm,   settings.panEndDeg   };
    bool  axisEnabled[AXIS_COUNT] = { moveLin, moveRot };  // user can exclude an axis

    // The linear axis honours "smooth ramping"; pan always ramps. Stretch the
    // shared duration if any axis's required speed would exceed its limit (the
    // eased profile peaks at 1.5x its average speed at the midpoint).
    float effDuration = durationSec;
    for (int i = 0; i < AXIS_COUNT; i++) {
        AxisId a = (AxisId)i;
        float dist = fabsf(endU[i] - startU[i]);
        autoMoving[i] = (dist > 0.0001f) && axisEnabled[i];
        autoEase[i]   = settings.useAccel;   // both axes ramp only when enabled
        if (!autoMoving[i]) continue;
        float minT = peakSpeedFactor(autoEase[i]) * dist / axisMaxSpeedUnits(a);
        if (minT > effDuration) effDuration = minT;
    }

    for (int i = 0; i < AXIS_COUNT; i++) {
        AxisId a = (AxisId)i;
        autoStartStep[i]  = unitToSteps(a, startU[i]);
        autoTargetStep[i] = unitToSteps(a, endU[i]);
        // Reposition phase: drive each moving axis to its start at full speed.
        if (autoMoving[i]) {
            ax[a].stepper.setMaxSpeed(axisMaxSpeedUnits(a) * stepsPerUnit(a));
            ax[a].stepper.setAcceleration(axisAccelUnits(a) * stepsPerUnit(a));
            ax[a].stepper.moveTo(autoStartStep[i]);
        }
    }

    autoDurationMs     = (uint32_t)(effDuration * 1000.0f);
    autoStartedMs      = 0;
    autoElapsedAtPause = 0;
    state = STATE_AUTO_REPOSITION;
    return true;
}

void Slider_stopAuto() {
    if (state == STATE_AUTO_REPOSITION || state == STATE_AUTO_MOVE || state == STATE_AUTO_PAUSED) {
        for (int i = 0; i < AXIS_COUNT; i++) ax[i].stepper.stop();
        state = STATE_IDLE;
    }
}

bool Slider_pauseAuto() {
    if (state != STATE_AUTO_MOVE) return false;
    autoElapsedAtPause = millis() - autoStartedMs;   // freeze the clock here
    for (int i = 0; i < AXIS_COUNT; i++) ax[i].stepper.stop();
    state = STATE_AUTO_PAUSED;
    return true;
}

bool Slider_resumeAuto() {
    if (state != STATE_AUTO_PAUSED) return false;
    // Shift the start time so elapsed continues from where it was paused.
    autoStartedMs = millis() - autoElapsedAtPause;
    for (int i = 0; i < AXIS_COUNT; i++) {
        if (!autoMoving[i]) continue;
        AxisId a = (AxisId)i;
        ax[a].stepper.setMaxSpeed(axisMaxSpeedUnits(a) * stepsPerUnit(a));
        ax[a].stepper.setAcceleration(instantAccelSteps(a));
    }
    state = STATE_AUTO_MOVE;
    return true;
}

// ---------- Main update ----------
void Slider_update() {
    static SliderState prevState = STATE_BOOT;
    if (state != prevState) {
        applyPowerFor(state);
        prevState = state;
    }

    AccelStepper& sl = ax[AXIS_SLIDER].stepper;

    switch (state) {

        case STATE_BOOT:
        case STATE_IDLE:
        case STATE_FAULT:
            break;

        case STATE_HOMING_FAST: {
            sl.run();
            if (limitTriggered(AXIS_SLIDER)) {
                sl.setCurrentPosition(0);
                sl.moveTo(HOMING_BACKOFF_STEPS);
                sl.setMaxSpeed(HOMING_SLOW_SPEED_S * 4);
                state = STATE_HOMING_BACKOFF;
            }
            break;
        }

        case STATE_HOMING_BACKOFF: {
            sl.run();
            if (sl.distanceToGo() == 0) {
                sl.setMaxSpeed(HOMING_SLOW_SPEED_S);
                sl.setAcceleration(HOMING_SLOW_SPEED_S * 4);
                sl.moveTo(-HOMING_BACKOFF_STEPS - 50);  // toward switch
                state = STATE_HOMING_SLOW;
            }
            break;
        }

        case STATE_HOMING_SLOW: {
            sl.run();
            if (limitTriggered(AXIS_SLIDER)) {
                sl.setCurrentPosition(0);
                sl.moveTo(0);
                Slider_applySettings();
                homed = true;
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_MANUAL: {
            AccelStepper& m = ax[manualAxis].stepper;
            m.run();
            if (m.distanceToGo() == 0) {
                state = STATE_IDLE;
            }
            // Safety: slider limit hit while moving toward 0 -> abort & re-zero.
            if (manualAxis == AXIS_SLIDER && limitTriggered(AXIS_SLIDER)
                && m.targetPosition() < m.currentPosition()) {
                m.setCurrentPosition(0);
                m.moveTo(0);
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_AUTO_REPOSITION: {
            bool allArrived = true;
            for (int i = 0; i < AXIS_COUNT; i++) {
                if (!autoMoving[i]) continue;
                ax[i].stepper.run();
                if (ax[i].stepper.distanceToGo() != 0) allArrived = false;
            }
            if (allArrived) {
                // Begin the synchronised timed move. A high acceleration ceiling
                // lets each axis track its time-based target tightly; the eased
                // trajectory (not AccelStepper) shapes the actual motion.
                for (int i = 0; i < AXIS_COUNT; i++) {
                    if (!autoMoving[i]) continue;
                    AxisId a = (AxisId)i;
                    ax[a].stepper.setMaxSpeed(axisMaxSpeedUnits(a) * stepsPerUnit(a));
                    ax[a].stepper.setAcceleration(instantAccelSteps(a));
                }
                autoStartedMs = millis();
                state = STATE_AUTO_MOVE;
            }
            break;
        }

        case STATE_AUTO_MOVE: {
            // Shared clock: command every axis to start + (end-start)*ease(frac).
            uint32_t elapsed = millis() - autoStartedMs;
            float frac = (autoDurationMs > 0) ? (float)elapsed / (float)autoDurationMs : 1.0f;
            if (frac > 1.0f) frac = 1.0f;
            for (int i = 0; i < AXIS_COUNT; i++) {
                if (!autoMoving[i]) continue;
                float s = easeFrac(frac, autoEase[i]);
                long desired = autoStartStep[i] +
                               (long)lroundf((float)(autoTargetStep[i] - autoStartStep[i]) * s);
                ax[i].stepper.moveTo(desired);
                ax[i].stepper.run();
            }
            // Safety: slider limit hit while moving toward 0.
            if (limitTriggered(AXIS_SLIDER) && sl.targetPosition() < sl.currentPosition()) {
                sl.setCurrentPosition(0);
                sl.moveTo(0);
            }
            // Done only once the clock is full and both axes have caught up.
            bool done = (frac >= 1.0f);
            for (int i = 0; i < AXIS_COUNT && done; i++) {
                if (autoMoving[i] && ax[i].stepper.distanceToGo() != 0) done = false;
            }
            if (done) {
                Slider_applySettings();
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_AUTO_PAUSED: {
            for (int i = 0; i < AXIS_COUNT; i++) {
                if (autoMoving[i]) ax[i].stepper.run();
            }
            break;
        }

        case STATE_CALIBRATE: {
            sl.run();
            if (sl.distanceToGo() == 0) {
                Slider_applySettings();
                state = STATE_IDLE;
            }
            if (limitTriggered(AXIS_SLIDER) && sl.targetPosition() < sl.currentPosition()) {
                sl.setCurrentPosition(0);
                sl.moveTo(0);
                Slider_applySettings();
                state = STATE_IDLE;
            }
            break;
        }
    }
}

// ---------- Status ----------
SliderState Slider_state()       { return state; }
float       Slider_position(AxisId a) { return stepsToUnit(a, ax[a].stepper.currentPosition()); }
float       Slider_positionMm()  { return Slider_position(AXIS_SLIDER); }
bool        Slider_isHomed()     { return homed; }

float Slider_autoProgress() {
    if (state == STATE_AUTO_REPOSITION) return 0.0f;
    if (state != STATE_AUTO_MOVE && state != STATE_AUTO_PAUSED) return 0.0f;
    if (autoDurationMs == 0) return 1.0f;
    uint32_t elapsed = (state == STATE_AUTO_PAUSED) ? autoElapsedAtPause
                                                    : (millis() - autoStartedMs);
    float p = (float)elapsed / (float)autoDurationMs;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    return p;
}

const char* Slider_stateText() {
    switch (state) {
        case STATE_BOOT:             return "Homing Required";
        case STATE_HOMING_FAST:      return "Homing...";
        case STATE_HOMING_BACKOFF:   return "Homing...";
        case STATE_HOMING_SLOW:      return "Homing...";
        case STATE_IDLE:             return "Idle";
        case STATE_MANUAL:           return "Manual";
        case STATE_AUTO_REPOSITION:  return "Repositioning";
        case STATE_AUTO_MOVE:        return "Moving";
        case STATE_AUTO_PAUSED:      return "Paused";
        case STATE_CALIBRATE:        return "Calibrating";
        case STATE_FAULT:            return "Fault";
    }
    return "?";
}
