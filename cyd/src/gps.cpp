#include "gps.h"
#include <TinyGPSPlus.h>

namespace gps {

bool     enabled = false;
bool     fix = false;
double   lat = 0, lon = 0;
uint8_t  sats = 0;
float    hdop = 99;
uint32_t lastFixMs = 0;
uint32_t charsSeen = 0;

static TinyGPSPlus parser;
static HardwareSerial& port = Serial2;

void begin(uint32_t baud) {
    port.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    enabled = true;
}
void end() { port.end(); enabled = false; fix = false; }

void service() {
    if (!enabled) return;
    int n = 0;
    while (port.available() && n++ < 256) { parser.encode(port.read()); charsSeen++; }
    if (parser.location.isValid() && parser.location.age() < 5000) {
        fix = true;
        lat = parser.location.lat(); lon = parser.location.lng();
        lastFixMs = millis();
    } else if (fix && parser.location.age() > 15000) fix = false;
    if (parser.satellites.isValid()) sats = parser.satellites.value();
    if (parser.hdop.isValid()) hdop = parser.hdop.hdop();
}

String utc() {
    if (!parser.date.isValid() || !parser.time.isValid() || parser.date.year() < 2020) return "";
    char b[24];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", parser.date.year(), parser.date.month(), parser.date.day(),
             parser.time.hour(), parser.time.minute(), parser.time.second());
    return String(b);
}
String latStr() { return fix ? String(lat, 5) : ""; }
String lonStr() { return fix ? String(lon, 5) : ""; }
String summary() {
    if (!enabled) return "GPS off";
    if (!charsSeen) return "GPS no data";
    if (!fix) return "GPS no fix" + String(sats ? " (" + String(sats) + " sats)" : "");
    return "GPS fix " + String(sats) + " sats";
}

} // namespace gps
