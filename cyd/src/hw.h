// Small hardware helpers implemented in main.cpp
#pragma once
#include <Arduino.h>
#include "board.h"

extern LGFX lcd;

enum Sound : uint8_t { SND_TAP, SND_BOOP, SND_START, SND_SAFE, SND_STOP, SND_WAKE, SND_ERR };
void sound(Sound s);                 // non-blocking, respects the sound setting
void soundService();
void led(bool r, bool g, bool b);    // RGB status LED, respects the led setting
void applyDisplaySettings();         // invert / rgb / rotation / brightness
void goToSleep();
uint32_t lastTouchMs();
void touchActivity();
#define FW_VERSION "cyd-1.0.0"
