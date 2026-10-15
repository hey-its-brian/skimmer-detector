// Bit-banged XPT2046 resistive touch for the CYD. The touch controller sits on
// its own pins (25/32/39/33) that are not a hardware SPI host once the display
// has HSPI and the SD card has VSPI, so we clock it by hand. ~1 kHz is plenty.
#pragma once
#include <Arduino.h>

struct TouchCal {
    // raw value at screen x=0 / x=max, y=0 / y=max (either order: inverted ranges mirror)
    int16_t x0 = 200, x1 = 3700, y0 = 240, y1 = 3800;
    bool    swapXY = false;
};

struct TouchPoint { bool down; int16_t x, y; uint16_t rawX, rawY, z; };

class Touch {
public:
    void begin();
    // Reads the panel. Returns true when pressed; x/y are screen coords (0..319, 0..239).
    bool read(TouchPoint& p);
    bool rawRead(uint16_t& x, uint16_t& y, uint16_t& z);
    TouchCal cal;
    void setScreen(int w, int h) { _w = w; _h = h; }
private:
    uint16_t xfer16(uint8_t cmd);
    int _w = 320, _h = 240;
};
