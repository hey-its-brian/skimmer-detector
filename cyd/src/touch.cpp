#include "touch.h"
#include "board.h"

static inline void clk(bool v) { digitalWrite(PIN_TP_CLK, v); }

void Touch::begin() {
    pinMode(PIN_TP_CLK, OUTPUT);  clk(LOW);
    pinMode(PIN_TP_MOSI, OUTPUT); digitalWrite(PIN_TP_MOSI, LOW);
    pinMode(PIN_TP_MISO, INPUT);
    pinMode(PIN_TP_CS, OUTPUT);   digitalWrite(PIN_TP_CS, HIGH);
    pinMode(PIN_TP_IRQ, INPUT);
    // One throwaway conversion so PENIRQ is enabled (power-down bits 00)
    uint16_t x, y, z; rawRead(x, y, z);
}

// Sends an 8-bit command, reads the 12-bit result (first bit is a busy bit).
uint16_t Touch::xfer16(uint8_t cmd) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(PIN_TP_MOSI, (cmd >> i) & 1);
        clk(HIGH); delayMicroseconds(2); clk(LOW); delayMicroseconds(2);
    }
    digitalWrite(PIN_TP_MOSI, LOW);
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        clk(HIGH); delayMicroseconds(2);
        v = (v << 1) | digitalRead(PIN_TP_MISO);
        clk(LOW);  delayMicroseconds(2);
    }
    return (v >> 3) & 0x0FFF;   // skip busy bit, take 12 bits, drop padding
}

bool Touch::rawRead(uint16_t& x, uint16_t& y, uint16_t& z) {
    digitalWrite(PIN_TP_CS, LOW);
    delayMicroseconds(2);
    // Z1/Z2 for pressure (differential, PD=01 keeps the reference on between reads)
    uint16_t z1 = xfer16(0xB1);
    uint16_t z2 = xfer16(0xC1);
    z = z1 + 4095 - z2;
    // 3 samples each, keep the median
    uint16_t xs[3], ys[3];
    for (int i = 0; i < 3; i++) { xs[i] = xfer16(0x91); ys[i] = xfer16(0xD1); }
    xfer16(0x90);   // final read with PD=00: re-enable PENIRQ
    digitalWrite(PIN_TP_CS, HIGH);
    auto med = [](uint16_t* a) { if (a[0] > a[1]) std::swap(a[0], a[1]); if (a[1] > a[2]) std::swap(a[1], a[2]); if (a[0] > a[1]) std::swap(a[0], a[1]); return a[1]; };
    x = med(xs); y = med(ys);
    return z > 400 && x > 50 && x < 4050 && y > 50 && y < 4050;
}

bool Touch::read(TouchPoint& p) {
    p.down = false;
    if (digitalRead(PIN_TP_IRQ) == HIGH) return false;   // nothing on the panel
    uint16_t x, y, z;
    if (!rawRead(x, y, z)) return false;
    p.rawX = x; p.rawY = y; p.z = z;
    int32_t rx = cal.swapXY ? y : x, ry = cal.swapXY ? x : y;
    int32_t sx = (rx - cal.x0) * (_w - 1) / (cal.x1 - cal.x0);
    int32_t sy = (ry - cal.y0) * (_h - 1) / (cal.y1 - cal.y0);
    p.x = constrain(sx, 0, _w - 1);
    p.y = constrain(sy, 0, _h - 1);
    p.down = true;
    return true;
}
