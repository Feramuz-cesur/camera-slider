#include "SliderControl.h"
#include "Settings.h"
#include "Config.h"
#include <AccelStepper.h>

// NEMA 17 + A4988 step/direction driver. AccelStepper toggles STEP, DIR sets
// direction, and the ENABLE pin (active LOW) energizes the motor coils.
static AccelStepper stepper(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);
static SliderState  state = STATE_BOOT;
static bool         homed = false;

// auto-move bookkeeping
static long         autoStartStep   = 0;
static long         autoTargetStep  = 0;
static uint32_t     autoStartedMs   = 0;
static uint32_t     autoDurationMs  = 0;
static float        autoSpeedStepsS = 0; // remembered for pause/resume
static float        autoAccelStepsS2 = 0;

static inline long mmToSteps(float mm)   { return (long)(mm * settings.stepsPerMm + 0.5f); }
static inline float stepsToMm(long st)   { return (float)st / settings.stepsPerMm; }

// ---------- Setup ----------
void Slider_begin() {
    pinMode(PIN_LIMIT, INPUT_PULLUP);

    // A4988 ENABLE is active LOW -> mark the enable pin inverted so
    // enableOutputs()/disableOutputs() drive it the right way.
    stepper.setEnablePin(PIN_EN);
    stepper.setPinsInverted(settings.invertDir, false, true);
    stepper.setMaxSpeed(settings.maxSpeedMmS * settings.stepsPerMm);
    stepper.setAcceleration(settings.accelMmS2 * settings.stepsPerMm);
    stepper.setCurrentPosition(0);
    stepper.disableOutputs();   // de-energize coils until first move (A4988/motor stay cool)

    state = STATE_BOOT;
    homed = false;
}

void Slider_applySettings() {
    float maxSpeedSteps = settings.maxSpeedMmS * settings.stepsPerMm;
    float accelSteps    = settings.accelMmS2  * settings.stepsPerMm;
    stepper.setPinsInverted(settings.invertDir, false, true);
    stepper.setMaxSpeed(maxSpeedSteps);
    stepper.setAcceleration(settings.useAccel ? accelSteps : maxSpeedSteps * 50.0f);
}

// ---------- Helpers ----------
static bool limitTriggered() {
    // INPUT_PULLUP + switch to GND => LOW when pressed
    return digitalRead(PIN_LIMIT) == LOW;
}

// ---------- Commands ----------
bool Slider_startHoming() {
    if (state != STATE_BOOT && state != STATE_IDLE && state != STATE_FAULT) return false;

    float homingStepsS = settings.homingSpeedMmS * settings.stepsPerMm;
    stepper.setMaxSpeed(homingStepsS);
    stepper.setAcceleration(homingStepsS * 4);
    // Move a very large distance toward the limit; will stop on switch.
    long bigSteps = mmToSteps(settings.maxTravelMm + 200.0f);
    stepper.setCurrentPosition(bigSteps);          // pretend we're past max
    stepper.moveTo(0);                              // target 0 (toward switch)
    // Note: actual direction depends on coil wiring. If wrong, enable "invert direction" in settings.
    state = STATE_HOMING_FAST;
    homed = false;
    return true;
}

void Slider_skipHoming() {
    // Skip homing: assume the carriage is at 0 and unlock movement.
    stepper.setCurrentPosition(0);
    stepper.moveTo(0);
    Slider_applySettings();
    homed = true;
    state = STATE_IDLE;
}

bool Slider_calibrateSpin(long steps) {
    if (state != STATE_IDLE) return false;
    Slider_applySettings();
    stepper.move(steps);   // relative move
    state = STATE_CALIBRATE;
    return true;
}

void Slider_stop() {
    stepper.stop();   // decelerated stop
    if (state == STATE_MANUAL || state == STATE_AUTO_MOVE || state == STATE_AUTO_REPOSITION) {
        state = STATE_IDLE;
    }
}

void Slider_emergencyStop() {
    stepper.setSpeed(0);
    stepper.moveTo(stepper.currentPosition());
    state = STATE_IDLE;
}

bool Slider_manualStart(Direction d, float speedMmS) {
    if (!homed) return false;
    if (state != STATE_IDLE && state != STATE_MANUAL) return false;

    Slider_applySettings();

    // Jog at the requested speed, clamped to (0, maxSpeed]. Zero speed = no move.
    if (speedMmS <= 0) return false;
    if (speedMmS > settings.maxSpeedMmS) speedMmS = settings.maxSpeedMmS;
    stepper.setMaxSpeed(speedMmS * settings.stepsPerMm);

    long target;
    if (d == DIR_RIGHT) {
        target = mmToSteps(settings.maxTravelMm);
    } else {
        target = 0;
    }
    stepper.moveTo(target);
    state = STATE_MANUAL;
    return true;
}

void Slider_manualStop() {
    if (state == STATE_MANUAL) {
        stepper.stop();   // decelerate
    }
}

bool Slider_gotoMm(float mm) {
    if (!homed) return false;
    // Accept when idle or already doing a point-to-point move (retarget on the fly).
    if (state != STATE_IDLE && state != STATE_MANUAL) return false;
    if (mm < 0) mm = 0;
    if (mm > settings.maxTravelMm) mm = settings.maxTravelMm;
    Slider_applySettings();
    stepper.moveTo(mmToSteps(mm));   // STATE_MANUAL handler drives it to the target
    state = STATE_MANUAL;
    return true;
}

bool Slider_startAuto(bool leftToRight, float durationSec) {
    if (!homed) return false;
    if (state != STATE_IDLE) return false;
    if (durationSec <= 0.1f) return false;

    long startStep  = leftToRight ? 0L : mmToSteps(settings.maxTravelMm);
    long endStep    = leftToRight ? mmToSteps(settings.maxTravelMm) : 0L;

    autoStartStep   = startStep;
    autoTargetStep  = endStep;
    autoDurationMs  = (uint32_t)(durationSec * 1000.0f);

    // Compute required speed for the timed move (mm/s).
    float distMm  = settings.maxTravelMm;
    float reqMmS  = distMm / durationSec;
    if (reqMmS > settings.maxSpeedMmS) reqMmS = settings.maxSpeedMmS;
    float reqStepsS = reqMmS * settings.stepsPerMm;

    // First: reposition phase (fast travel back to start position).
    stepper.setMaxSpeed(settings.maxSpeedMmS * settings.stepsPerMm);
    stepper.setAcceleration(settings.useAccel
        ? settings.accelMmS2 * settings.stepsPerMm
        : settings.maxSpeedMmS * settings.stepsPerMm * 50.0f);
    stepper.moveTo(startStep);

    autoStartedMs = 0;
    (void)reqStepsS;   // actual move speed is recomputed in update() once reposition completes
    state = STATE_AUTO_REPOSITION;
    return true;
}

void Slider_stopAuto() {
    if (state == STATE_AUTO_REPOSITION || state == STATE_AUTO_MOVE || state == STATE_AUTO_PAUSED) {
        stepper.stop();
        state = STATE_IDLE;
    }
}

bool Slider_pauseAuto() {
    if (state != STATE_AUTO_MOVE) return false;
    // Decelerate to a stop at the current position
    stepper.stop();
    state = STATE_AUTO_PAUSED;
    return true;
}

bool Slider_resumeAuto() {
    if (state != STATE_AUTO_PAUSED) return false;
    // Re-issue the original move with the same speed/accel
    stepper.setMaxSpeed(autoSpeedStepsS);
    stepper.setAcceleration(autoAccelStepsS2);
    stepper.moveTo(autoTargetStep);
    state = STATE_AUTO_MOVE;
    return true;
}

// ---------- Main update ----------
void Slider_update() {
    static SliderState prevState = STATE_BOOT;

    // Energize / de-energize the A4988 on state changes. With the DRIVER mode
    // enable pin, disableOutputs() cuts power and step() will NOT re-enable it,
    // so we must enableOutputs() before any motion. Resting states de-energize
    // to keep the driver and motor cool; STATE_AUTO_PAUSED stays energized on
    // purpose so resume continues without losing position.
    if (state != prevState) {
        bool resting = (state == STATE_IDLE || state == STATE_BOOT || state == STATE_FAULT);
        if (resting) stepper.disableOutputs();
        else         stepper.enableOutputs();
        prevState = state;
    }

    switch (state) {

        case STATE_BOOT:
        case STATE_IDLE:
        case STATE_FAULT:
            // Nothing to step; but call run() to keep state consistent.
            break;

        case STATE_HOMING_FAST: {
            stepper.run();
            if (limitTriggered()) {
                stepper.setCurrentPosition(0);
                stepper.moveTo(HOMING_BACKOFF_STEPS);
                stepper.setMaxSpeed(HOMING_SLOW_SPEED_S * 4);
                state = STATE_HOMING_BACKOFF;
            }
            break;
        }

        case STATE_HOMING_BACKOFF: {
            stepper.run();
            if (stepper.distanceToGo() == 0) {
                // Slow approach back toward switch for accuracy
                stepper.setMaxSpeed(HOMING_SLOW_SPEED_S);
                stepper.setAcceleration(HOMING_SLOW_SPEED_S * 4);
                stepper.moveTo(-HOMING_BACKOFF_STEPS - 50);  // toward switch
                state = STATE_HOMING_SLOW;
            }
            break;
        }

        case STATE_HOMING_SLOW: {
            stepper.run();
            if (limitTriggered()) {
                stepper.setCurrentPosition(0);
                stepper.moveTo(0);
                Slider_applySettings();
                homed = true;
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_MANUAL: {
            stepper.run();
            if (stepper.distanceToGo() == 0) {
                // Reached the rail end (or stopped) — return to idle
                state = STATE_IDLE;
            }
            // Safety: if limit triggered unexpectedly, abort
            if (limitTriggered() && stepper.targetPosition() < stepper.currentPosition()) {
                stepper.setCurrentPosition(0);
                stepper.moveTo(0);
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_AUTO_REPOSITION: {
            stepper.run();
            if (stepper.distanceToGo() == 0) {
                // Begin the timed move
                float distMm = settings.maxTravelMm;
                float reqMmS = distMm / (autoDurationMs / 1000.0f);
                if (reqMmS > settings.maxSpeedMmS) reqMmS = settings.maxSpeedMmS;
                float reqStepsS = reqMmS * settings.stepsPerMm;
                float accelStepsS2 = settings.useAccel
                    ? settings.accelMmS2 * settings.stepsPerMm
                    : reqStepsS * 50.0f;

                // Remember for pause/resume
                autoSpeedStepsS  = reqStepsS;
                autoAccelStepsS2 = accelStepsS2;

                stepper.setMaxSpeed(reqStepsS);
                stepper.setAcceleration(accelStepsS2);
                stepper.moveTo(autoTargetStep);
                autoStartedMs = millis();
                state = STATE_AUTO_MOVE;
            }
            break;
        }

        case STATE_AUTO_MOVE: {
            stepper.run();
            if (stepper.distanceToGo() == 0) {
                Slider_applySettings();
                state = STATE_IDLE;
            }
            // Safety: stop if limit unexpectedly triggered while moving toward 0
            if (limitTriggered() && stepper.targetPosition() < stepper.currentPosition()) {
                stepper.setCurrentPosition(0);
                stepper.moveTo(0);
                Slider_applySettings();
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_AUTO_PAUSED: {
            // Allow deceleration to complete (then stays put with holding torque)
            stepper.run();
            break;
        }

        case STATE_CALIBRATE: {
            stepper.run();
            if (stepper.distanceToGo() == 0) {
                Slider_applySettings();
                state = STATE_IDLE;
            }
            // Safety: stop if limit unexpectedly triggered while moving toward 0
            if (limitTriggered() && stepper.targetPosition() < stepper.currentPosition()) {
                stepper.setCurrentPosition(0);
                stepper.moveTo(0);
                Slider_applySettings();
                state = STATE_IDLE;
            }
            break;
        }
    }
}

// ---------- Status ----------
SliderState Slider_state()       { return state; }
float       Slider_positionMm()  { return stepsToMm(stepper.currentPosition()); }
bool        Slider_isHomed()     { return homed; }

float Slider_autoProgress() {
    if (state == STATE_AUTO_REPOSITION) return 0.0f;
    if (state != STATE_AUTO_MOVE && state != STATE_AUTO_PAUSED) return 0.0f;
    long total = autoTargetStep - autoStartStep;
    if (total == 0) return 1.0f;
    long done  = stepper.currentPosition() - autoStartStep;
    float p = (float)done / (float)total;
    if (p < 0) p = -p;
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
