#include <Arduino.h>
#include "Config.h"
#include "Settings.h"
#include "SliderControl.h"
#include "SliderWifi.h"
#include "WebInterface.h"

void setup() {
    // NOTE: GPIO1 (TX) and GPIO3 (RX) are used as GPIO. No Serial.begin().

    Settings_begin();
    Slider_begin();
    Wifi_begin();     // STA (saved network) or AP fallback for provisioning
    Web_begin();
}

void loop() {
    Slider_update();
    Web_loop();
    Wifi_loop();
    yield();
}
