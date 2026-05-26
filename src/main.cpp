#include <Arduino.h>
#include "Config.h"
#include "Settings.h"
#include "SliderControl.h"
#include "WebInterface.h"

void setup() {
    // NOTE: GPIO1 (TX) and GPIO3 (RX) are used as GPIO. No Serial.begin().

    Settings_begin();
    Slider_begin();
    Web_begin();
}

void loop() {
    Slider_update();
    Web_loop();
    yield();
}
