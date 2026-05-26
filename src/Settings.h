#pragma once
#include <Arduino.h>

struct SliderSettings {
    uint16_t magic;          // validates EEPROM data
    float    stepsPerMm;     // motor calibration
    float    maxTravelMm;    // usable rail length
    float    maxSpeedMmS;    // upper speed limit (mm/s)
    float    accelMmS2;      // acceleration (mm/s^2)
    bool     useAccel;       // smooth ramp on/off
    uint16_t checksum;
};

extern SliderSettings settings;

void   Settings_begin();
void   Settings_loadDefaults();
bool   Settings_load();
void   Settings_save();
