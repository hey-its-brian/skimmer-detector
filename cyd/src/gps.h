// GPS on the CN1 header: 3V3, GND, GPIO22 (ESP RX <- GPS TX), GPIO27 (ESP TX -> GPS RX).
// Any NMEA module works (ATGM336H, NEO-6M/7M/8M, BN-220...). Default 9600 baud.
#pragma once
#include <Arduino.h>

#define PIN_GPS_RX 22
#define PIN_GPS_TX 27

namespace gps {
    extern bool     enabled;
    extern bool     fix;
    extern double   lat, lon;
    extern uint8_t  sats;
    extern float    hdop;
    extern uint32_t lastFixMs;      // millis() of the last valid position
    extern uint32_t charsSeen;      // anything at all arriving on the UART?

    void begin(uint32_t baud);
    void end();
    void service();                 // feed the parser, call every loop
    String utc();                   // "2026-10-15T15:24:07Z" or "" without a time fix
    String latStr();                // "36.85210" or ""
    String lonStr();
    String summary();               // "fix 8 sats" / "no fix" / "GPS off" / "no data"
}
