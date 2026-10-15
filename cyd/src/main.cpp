// Skimmer Detector, CYD edition
// Board: ESP32-2432S028R (2-USB) with a capybara on the screen.
//
// Home: tap the capybara (or CHECK) with the board held on the card slot. It
// sniffs for ~10 s and answers SAFE or STOP. DEVICES lists everything scored,
// LOG shows alerts (also written to /skimlog.csv on the microSD), SETUP has
// display, sound, sleep, touch calibration and the allow list.
//
// Detection logic is identical to the XIAO build (see ../src/main.cpp):
// BLE module names, FFE0/FFF0/FFE5 UART service, vendor OUIs, proximity,
// RSSI jump versus the previous check, and the P/M handshake probe.
// Bluetooth Classic (HC-05/06) is not scanned: the WROOM could in theory, but
// BLE + BT Classic + WiFi together overflow IRAM on this chip.

#include <Arduino.h>
#include <esp_sleep.h>
#include "board.h"
#include "hw.h"
#include "touch.h"
#include "detector.h"
#include "storage.h"
#include "ui.h"
#include "gps.h"

LGFX lcd;
Touch touch;

// ---------------- sound ----------------
static const int SPK_CH = 0;
struct Note { uint16_t hz, ms; };
static const Note P_TAP[]   = { {1800, 18}, {0, 0} };
static const Note P_BOOP[]  = { {900, 40}, {1400, 60}, {0, 0} };
static const Note P_START[] = { {1200, 50}, {1600, 50}, {2000, 70}, {0, 0} };
static const Note P_SAFE[]  = { {1319, 90}, {1568, 90}, {2093, 160}, {0, 0} };
static const Note P_STOP[]  = { {400, 160}, {0, 80}, {400, 160}, {0, 80}, {400, 220}, {0, 0} };
static const Note P_WAKE[]  = { {1000, 40}, {0, 0} };
static const Note P_ERR[]   = { {300, 120}, {0, 0} };
static const Note* pat = nullptr; static uint8_t patIdx = 0; static uint32_t patAt = 0;

void sound(Sound s) {
    if (!storage::s.sound) return;
    switch (s) {
        case SND_TAP: pat = P_TAP; break;   case SND_BOOP: pat = P_BOOP; break;
        case SND_START: pat = P_START; break; case SND_SAFE: pat = P_SAFE; break;
        case SND_STOP: pat = P_STOP; break; case SND_WAKE: pat = P_WAKE; break;
        default: pat = P_ERR; break;
    }
    patIdx = 0; patAt = millis();
    ledcWriteTone(SPK_CH, pat[0].hz);
}
void soundService() {
    if (!pat) return;
    if (millis() - patAt < pat[patIdx].ms) return;
    patIdx++; patAt = millis();
    if (!pat[patIdx].ms) { pat = nullptr; ledcWriteTone(SPK_CH, 0); return; }
    ledcWriteTone(SPK_CH, pat[patIdx].hz);
}

// ---------------- LED ----------------
void led(bool r, bool g, bool b) {
    if (!storage::s.led) { r = g = b = false; }
    digitalWrite(PIN_LED_R, r ? LOW : HIGH);
    digitalWrite(PIN_LED_G, g ? LOW : HIGH);
    digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}
static void ledService() {
    uint32_t now = millis();
    switch (det::state) {
        case ST_WAIT: led(false, false, (now / 250) % 2 == 0); break;
        case ST_SAFE: led(false, now - det::stateAt < 5000, false); break;
        case ST_STOP: led((now / 300) % 2 == 0, false, false); break;
        default:      led(false, false, false); break;
    }
}

// ---------------- display ----------------
static uint8_t curBright = 0;
static bool panelRgb = false, panelInv = false;
static void configurePanel() {       // panel-level settings that need a (re)init
    auto& s = storage::s;
    auto p = (lgfx::Panel_Device*)lcd.getPanel();
    auto cfg = p->config(); cfg.rgb_order = s.rgbSwap; cfg.invert = s.invert; p->config(cfg);
    panelRgb = s.rgbSwap; panelInv = s.invert;
}
void applyDisplaySettings() {
    auto& s = storage::s;
    if (panelRgb != s.rgbSwap) {
        configurePanel();
        ((lgfx::Panel_Device*)lcd.getPanel())->init(false);
        ui::markDirty();
    }
    lcd.setRotation(s.rotation);
    lcd.invertDisplay(s.invert);
    curBright = s.brightness;
    lcd.setBrightness(curBright);
}
static void dimService() {
    static uint32_t last = 0;
    if (millis() - last < 500) return;
    last = millis();
    uint8_t target = storage::s.brightness;
    if (storage::s.autoDim) {
        int ldr = analogRead(PIN_LDR);          // ~0 bright room, ~4095 dark
        if (ldr > 3600) target = max<int>(30, target / 4);
        else if (ldr > 2800) target = max<int>(60, target / 2);
    }
    if (target != curBright) { curBright = curBright < target ? min<int>(target, curBright + 16) : max<int>(target, curBright - 16); lcd.setBrightness(curBright); }
}

// ---------------- touch / activity ----------------
static uint32_t lastTouch = 0;
uint32_t lastTouchMs() { return lastTouch; }
void touchActivity() { lastTouch = millis(); }

static void touchService() {
    static bool wasDown = false; static uint32_t downAt = 0;
    static uint32_t lastPoll = 0;
    if (millis() - lastPoll < 15) return;
    lastPoll = millis();
    TouchPoint p;
    bool down = touch.read(p);
    if (down && storage::s.rotation == 3) { p.x = SCREEN_W - 1 - p.x; p.y = SCREEN_H - 1 - p.y; }
    if (down && !wasDown) {
        if (millis() - downAt > 120) {              // debounce between taps
            downAt = millis();
            bool wasAsleep = ui::capyAsleep();
            lastTouch = millis();
            if (wasAsleep) { sound(SND_WAKE); ui::markDirty(); }
            ui::onTouch(p);
        }
    }
    wasDown = down;
}

// ---------------- sleep ----------------
void goToSleep() {
    Serial.println("[SLEEP]");
    lcd.fillScreen(TFT_BLACK);
    lcd.setBrightness(0);
    lcd.sleep();
    led(false, false, false);
    ledcWriteTone(SPK_CH, 0);
    det::shutdown();
    gps::end();
    storage::flush();
    // PENIRQ (GPIO36) goes low while the panel is pressed
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_TP_IRQ, 0);
    esp_deep_sleep_start();
}
static void sleepService() {
    if (!storage::s.sleepMin) return;
    if (det::state == ST_WAIT || det::state == ST_STOP) return;
    if (ui::screen() == SCR_CALIB) return;
    if (millis() - lastTouch > (uint32_t)storage::s.sleepMin * 60000UL) goToSleep();
}

// ---------------- hooks from the detector ----------------
static void onVerdict(CheckState st) {
    sound(st == ST_STOP ? SND_STOP : SND_SAFE);
    ui::verdict(st);
}
static void onLog(const LogEntry& e) { storage::appendLog(e); }
static void onToast(const String& s, uint32_t ms) { ui::toast(s, ms); }
static void onProgress(const char* what, int pct) { ui::progress(what, pct); }

// ---------------- setup / loop ----------------
void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
    led(false, false, false);
    ledcSetup(SPK_CH, 2000, 10);
    ledcAttachPin(PIN_SPK, SPK_CH);
    ledcWriteTone(SPK_CH, 0);
    analogReadResolution(12);

    storage::begin();
    configurePanel();
    lcd.init();
    applyDisplaySettings();
    touch.begin();
    touch.setScreen(SCREEN_W, SCREEN_H);
    touch.cal = storage::s.cal;

    ui::begin();
    det::onVerdict = onVerdict;
    det::onLog = onLog;
    det::onToast = onToast;
    det::onProgress = onProgress;
    det::begin();
    if (storage::s.gpsOn) gps::begin(storage::s.gpsBaud);

    bool wokeByTouch = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    if (wokeByTouch) sound(SND_WAKE);
    lastTouch = millis();
    Serial.printf("[BOOT] #%lu  heap %u  sd %s\n", (unsigned long)storage::bootCount, ESP.getFreeHeap(), storage::sdOk ? "ok" : "none");
}

void loop() {
    touchService();
    soundService();
    det::service();
    gps::service();
    ui::service();
    ledService();
    dimService();
    sleepService();
    delay(5);
}
