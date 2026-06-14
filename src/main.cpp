#include <Arduino.h>
#include "Config.h"
#include "Settings.h"
#include "SliderControl.h"
#include "SliderWifi.h"
#include "WebInterface.h"
#include "Display.h"

void setup() {
    Serial.begin(115200);   // ESP32-C3 native USB CDC

    Settings_begin();
    Display_begin();  // show splash while WiFi connects
    Slider_begin();
    Wifi_begin();     // STA (saved network) or AP fallback for provisioning
    Web_begin();

    Serial.printf("WiFi: %s  IP: %s\n", Wifi_ssid().c_str(), Wifi_ip().c_str());
}

void loop() {
    Slider_update();
    Web_loop();
    Wifi_loop();
    // Display now renders on its own FreeRTOS task (see Display_begin).
    yield();
}
