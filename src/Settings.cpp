#include "Settings.h"
#include "Config.h"
#include <EEPROM.h>

#define EEPROM_SIZE     128
#define EEPROM_ADDR     0
#define SETTINGS_MAGIC  0xCA19   // v5: added rotary pan axis + start/end positions

SliderSettings settings;

static uint16_t calcChecksum(const SliderSettings& s) {
    const uint8_t* p = (const uint8_t*)&s;
    uint16_t c = 0;
    // hash everything except the checksum field itself (last 2 bytes)
    size_t n = sizeof(SliderSettings) - sizeof(uint16_t);
    for (size_t i = 0; i < n; i++) {
        c = (c << 1) ^ p[i] ^ (c >> 15);
    }
    return c ? c : 1;
}

void Settings_loadDefaults() {
    settings.magic        = SETTINGS_MAGIC;
    settings.stepsPerRev  = DEFAULT_STEPS_PER_REV;
    settings.stepsPerMm   = DEFAULT_STEPS_PER_REV / DEFAULT_MM_PER_REV;
    settings.maxTravelMm  = DEFAULT_MAX_TRAVEL_MM;
    settings.maxSpeedMmS  = DEFAULT_MAX_SPEED_MMS;
    settings.accelMmS2    = DEFAULT_ACCEL_MMS2;
    settings.homingSpeedMmS = DEFAULT_HOMING_SPEED_MMS;
    settings.useAccel     = DEFAULT_USE_ACCEL;
    settings.invertDir    = DEFAULT_INVERT_DIR;
    settings.startMm      = DEFAULT_START_MM;
    settings.endMm        = DEFAULT_END_MM;

    settings.panStepsPerRev = DEFAULT_PAN_STEPS_PER_REV;
    settings.panMaxSpeedDegS = DEFAULT_PAN_MAX_SPEED_DEGS;
    settings.panAccelDegS2   = DEFAULT_PAN_ACCEL_DEGS2;
    settings.panInvertDir    = DEFAULT_PAN_INVERT_DIR;
    settings.panStartDeg     = DEFAULT_PAN_START_DEG;
    settings.panEndDeg       = DEFAULT_PAN_END_DEG;

    settings.checksum     = calcChecksum(settings);
}

void Settings_begin() {
    EEPROM.begin(EEPROM_SIZE);
    if (!Settings_load()) {
        Settings_loadDefaults();
        Settings_save();
    }
}

bool Settings_load() {
    SliderSettings tmp;
    uint8_t* p = (uint8_t*)&tmp;
    for (size_t i = 0; i < sizeof(SliderSettings); i++) {
        p[i] = EEPROM.read(EEPROM_ADDR + i);
    }
    if (tmp.magic != SETTINGS_MAGIC) return false;
    if (tmp.checksum != calcChecksum(tmp)) return false;
    settings = tmp;
    return true;
}

void Settings_save() {
    settings.magic    = SETTINGS_MAGIC;
    settings.checksum = calcChecksum(settings);
    const uint8_t* p = (const uint8_t*)&settings;
    for (size_t i = 0; i < sizeof(SliderSettings); i++) {
        EEPROM.write(EEPROM_ADDR + i, p[i]);
    }
    EEPROM.commit();
}
