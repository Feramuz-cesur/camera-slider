#pragma once
#include <Arduino.h>

struct SliderSettings {
    uint16_t magic;          // validates EEPROM data

    // ---- Axis 1: linear slider ----
    float    stepsPerMm;     // motor calibration (canonical; used by motion)
    uint16_t stepsPerRev;    // steps for one full motor turn (motor * microstep)
    float    maxTravelMm;    // usable rail length
    float    maxSpeedMmS;    // upper speed limit (mm/s)
    float    accelMmS2;      // acceleration (mm/s^2)
    float    jogSpeedMmS;    // manual jog speed (mm/s)
    float    homingSpeedMmS; // approach speed toward the limit switch (mm/s)
    bool     useAccel;       // smooth ramp on/off (applies to both axes, jog + auto)
    bool     invertDir;      // flip motor direction
    float    startMm;        // auto-move start position (mm)
    float    endMm;          // auto-move end position (mm)

    // ---- Axis 2: rotary pan ----
    uint16_t panStepsPerRev; // steps for one full 360° platform turn
    float    panMaxSpeedDegS;// upper speed limit (deg/s)
    float    panAccelDegS2;  // acceleration (deg/s^2)
    float    panJogSpeedDegS;// manual jog speed (deg/s)
    bool     panInvertDir;   // flip pan direction
    float    panStartDeg;    // auto-move start angle (deg)
    float    panEndDeg;      // auto-move end angle (deg)

    uint16_t checksum;
};

extern SliderSettings settings;

void   Settings_begin();
void   Settings_loadDefaults();
bool   Settings_load();
void   Settings_save();
