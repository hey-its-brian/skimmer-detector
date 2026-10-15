// Settings in NVS, logs on the microSD (with an NVS fallback for the allow list).
#pragma once
#include <Arduino.h>
#include "detector.h"
#include "touch.h"

struct Settings {
    bool     invert    = false;   // panel colour inversion
    bool     rgbSwap   = false;   // red/blue swapped
    uint8_t  rotation  = 1;       // 1 or 3 (landscape, either way up)
    uint8_t  brightness = 200;    // 0..255 backlight
    bool     sound     = true;
    bool     autoDim   = true;    // LDR dims the backlight in the dark
    uint8_t  sleepMin  = 3;       // deep sleep after N minutes idle, 0 = never
    bool     led       = true;    // RGB status LED
    bool     gpsOn     = true;    // UART GPS on CN1
    uint32_t gpsBaud   = 9600;
    TouchCal cal;
};

namespace storage {
    extern Settings  s;
    extern bool      sdOk;
    extern uint32_t  bootCount;
    extern uint32_t  logRows;      // rows written to SD this session

    void begin();                  // NVS + SD mount + allow list load
    void saveSettings();
    void saveAllow();
    void appendLog(const LogEntry& e);               // skimlog.csv
    void appendCheck(CheckState verdict, const String& who, size_t hitCount); // checks.csv
    void clearLogFile();
    bool remount();
    String sdSummary();            // "SD 1.2 MB used" or "no card"
    void flush();
}
