#pragma once
#include <Arduino.h>
#include "touch.h"

enum Screen : uint8_t { SCR_HOME, SCR_DEVICES, SCR_LOG, SCR_SETUP, SCR_DETAIL, SCR_ALLOW, SCR_CALIB, SCR_ABOUT };

namespace ui {
    void begin();
    void service();                        // animation + redraws, call every loop
    void onTouch(const TouchPoint& p);     // a new tap (press-down event)
    void toast(const String& s, uint32_t ms);
    void progress(const char* what, int pct);   // -1 = indeterminate
    void verdict(uint8_t st);              // called when a check ends
    void setScreen(Screen s);
    Screen screen();
    void markDirty();
    bool capyAsleep();
    void boop();                           // poke the capybara
}
