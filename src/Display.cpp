#include "Display.h"
#include "Config.h"
#include "SliderWifi.h"
#include "SliderControl.h"
#include <U8g2lib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Onboard 0.42" SSD1306, 72x40 visible area, I2C addr 0x3C.
// The 72X40_ER constructor handles the panel's column/row offset for us.
static U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_OLED_SCL, PIN_OLED_SDA);

static const uint8_t  SCREEN_W = 72;
static const uint8_t  SCREEN_H = 40;
static const uint16_t SCROLL_STEP_MS = 40;   // marquee frame period (smooth while idle)
static const uint8_t  SCROLL_PX      = 2;    // pixels moved per frame
static const uint8_t  SCROLL_GAP     = 16;   // blank gap between repeats of the IP

static int16_t scrollX = SCREEN_W;           // marquee position; start off the right edge

// Render one frame. Runs only on the display task (sole owner of the u8g2 buffer
// and the I2C bus), so no locking is needed against the main loop.
static void drawFrame() {
    // sendBuffer() blocks ~8 ms on I2C. The C3 is single-core, so even from a
    // separate task that transfer time-shares the CPU; refresh only while the
    // motor is at rest so we never disturb step timing mid-move.
    switch (Slider_state()) {
        case STATE_BOOT:
        case STATE_IDLE:
        case STATE_AUTO_PAUSED:
        case STATE_FAULT:
            break;
        default:
            return;
    }
    // Until the radio is up, leave the boot splash in place.
    if (!Wifi_isStation() && !Wifi_apActive()) return;

    // Once on the network, the only thing worth showing is the address to type
    // into the browser. Big font (~2/3 of the 40px height), vertically centred.
    String ip = Wifi_ip();

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_logisoso26_tr);   // ~26px tall glyphs
    u8g2.setFontPosCenter();                 // y now refers to the glyph's vertical centre

    int16_t w  = u8g2.getUTF8Width(ip.c_str());
    int16_t cy = SCREEN_H / 2;               // vertically centred

    if (w <= SCREEN_W) {
        // Fits on screen: just centre it horizontally, no scrolling needed.
        u8g2.drawUTF8((SCREEN_W - w) / 2, cy, ip.c_str());
        scrollX = SCREEN_W;
    } else {
        // Too wide: marquee scroll right -> left, looping seamlessly.
        int16_t span = w + SCROLL_GAP;
        u8g2.drawUTF8(scrollX, cy, ip.c_str());
        u8g2.drawUTF8(scrollX + span, cy, ip.c_str());   // trailing copy for a gapless loop
        scrollX -= SCROLL_PX;
        if (scrollX <= -span) scrollX += span;
    }

    u8g2.setFontPosBaseline();   // restore default reference
    u8g2.sendBuffer();
}

static void displayTask(void* arg) {
    for (;;) {
        drawFrame();
        vTaskDelay(pdMS_TO_TICKS(SCROLL_STEP_MS));
    }
}

void Display_begin() {
    u8g2.begin();
    u8g2.setBusClock(400000);   // shorter I2C transfers = shorter stalls
    u8g2.setContrast(255);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 12, "Camera");
    u8g2.drawStr(0, 22, "Slider");
    u8g2.drawStr(0, 36, "WiFi...");
    u8g2.sendBuffer();

    // Hand the OLED off to its own task so the ~8ms I2C refresh no longer stalls
    // the main loop (web + stepper). Priority 1 = same as the Arduino loop; the
    // "draw only while idle" guard keeps it off the bus during moves.
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);
}
