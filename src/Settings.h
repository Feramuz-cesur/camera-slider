#pragma once
#include <Arduino.h>

struct SliderSettings {
    uint16_t magic;          // validates EEPROM data
    float    stepsPerMm;     // motor calibration (canonical; used by motion)
    uint16_t stepsPerRev;    // steps for one full motor turn (motor * microstep)
    float    maxTravelMm;    // usable rail length
    float    maxSpeedMmS;    // upper speed limit (mm/s)
    float    accelMmS2;      // acceleration (mm/s^2)
    float    homingSpeedMmS; // approach speed toward the limit switch (mm/s)
    bool     useAccel;       // smooth ramp on/off
    bool     invertDir;      // flip motor direction
    uint16_t checksum;
};

extern SliderSettings settings;

void   Settings_begin();
void   Settings_loadDefaults();
bool   Settings_load();
void   Settings_save();
