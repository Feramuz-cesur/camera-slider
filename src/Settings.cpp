#include "Settings.h"
#include "Config.h"
#include <EEPROM.h>

#define EEPROM_SIZE     128
#define EEPROM_ADDR     0
#define SETTINGS_MAGIC  0xCA1A   // v6: added per-axis jog speeds

SliderSettings settings;

// Start/end "set" flags live next to SliderSettings, not inside it: growing the
// struct changes its layout, which the magic/checksum check would read as
// corruption and reset every calibration to defaults.
#define RANGE_FLAGS_ADDR    120
#define RANGE_FLAGS_MAGIC   0x5E
#define RANGE_START_BIT     0x01
#define RANGE_END_BIT       0x02
static_assert(sizeof(SliderSettings) <= RANGE_FLAGS_ADDR, "SliderSettings overlaps the range flags");
static_assert(RANGE_FLAGS_ADDR + 2 <= EEPROM_SIZE, "range flags past the end of EEPROM");

static uint8_t g_rangeFlags = 0;

static void saveRangeFlags() {
    EEPROM.write(RANGE_FLAGS_ADDR,     RANGE_FLAGS_MAGIC);
    EEPROM.write(RANGE_FLAGS_ADDR + 1, g_rangeFlags);
    EEPROM.commit();
}

static void loadRangeFlags() {
    if (EEPROM.read(RANGE_FLAGS_ADDR) == RANGE_FLAGS_MAGIC) {
        g_rangeFlags = EEPROM.read(RANGE_FLAGS_ADDR + 1) & (RANGE_START_BIT | RANGE_END_BIT);
        return;
    }
    // First boot on a firmware that tracks this: a pose that differs from the
    // factory default can only have come from the user.
    g_rangeFlags = 0;
    if (settings.startMm != DEFAULT_START_MM || settings.panStartDeg != DEFAULT_PAN_START_DEG)
        g_rangeFlags |= RANGE_START_BIT;
    if (settings.endMm != DEFAULT_END_MM || settings.panEndDeg != DEFAULT_PAN_END_DEG)
        g_rangeFlags |= RANGE_END_BIT;
    saveRangeFlags();
}

bool Settings_startSet() { return g_rangeFlags & RANGE_START_BIT; }
bool Settings_endSet()   { return g_rangeFlags & RANGE_END_BIT; }

void Settings_markRange(bool start, bool end) {
    uint8_t f = g_rangeFlags | (start ? RANGE_START_BIT : 0) | (end ? RANGE_END_BIT : 0);
    if (f == g_rangeFlags) return;   // spare the flash a no-op commit
    g_rangeFlags = f;
    saveRangeFlags();
}

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
    settings.jogSpeedMmS  = DEFAULT_JOG_SPEED_MMS;
    settings.homingSpeedMmS = DEFAULT_HOMING_SPEED_MMS;
    settings.useAccel     = DEFAULT_USE_ACCEL;
    settings.invertDir    = DEFAULT_INVERT_DIR;
    settings.startMm      = DEFAULT_START_MM;
    settings.endMm        = DEFAULT_END_MM;

    settings.panStepsPerRev = DEFAULT_PAN_STEPS_PER_REV;
    settings.panMaxSpeedDegS = DEFAULT_PAN_MAX_SPEED_DEGS;
    settings.panAccelDegS2   = DEFAULT_PAN_ACCEL_DEGS2;
    settings.panJogSpeedDegS = DEFAULT_PAN_JOG_SPEED_DEGS;
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
        g_rangeFlags = 0;          // factory range: nothing chosen yet
        saveRangeFlags();
    } else {
        loadRangeFlags();
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
