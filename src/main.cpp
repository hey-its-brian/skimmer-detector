// Card skimmer detector v3
// Board:   Seeed XIAO ESP32-C6
// Display: 0.96" SSD1306 128x64 I2C OLED
//
// Wiring (XIAO C6 silkscreen):
//   3V3 -> OLED VCC      GND -> OLED GND
//   D4/D5 -> OLED SDA/SCL. Orientation is auto-detected at boot.
//   D0  -> haptic motor via NPN driver (+), or active buzzer      (GPIO0)
//   D1  -> RED button    -> GND   (GPIO1, deep-sleep wake pin)
//   D2  -> BLUE button   -> GND   (GPIO2, deep-sleep wake pin)
//   D3  -> GREEN button  -> GND   (GPIO21)
//   D6  -> YELLOW button -> GND   (GPIO16)
//   BOOT (onboard, GPIO9) on the back.
//
// SIMPLE MODE (default, what the end user sees)
//   One screen, four states, no pages:
//     READY  "On the slot"      press any front button to start a check
//     WAIT   "Hold still"       10 s fill bar; presses ignored
//     SAFE   "Go ahead"         one long buzz; holds 60 s then READY
//     STOP   "Pay inside"       inverted band, three short buzzes; holds 60 s
//   Every front button does the same thing. A press registers on press-down
//   after 120 ms of contact; hold length is ignored. Only RED and BLUE can
//   wake the C6 from deep sleep (GPIO0-7 only), so RED is the one to teach.
//   No baseline step: the readings from each completed check become the
//   comparison for the next check in the same session.
//   STOP needs a real signature (module name, UART service, vendor OUI,
//   handshake) or proximity plus a jump versus the last check. Proximity
//   alone never produces STOP. Nothing can be allow-listed from this mode.
//
// MAINTENANCE MODE (hold BOOT on the back for 3 s to toggle)
//   The full UI: All / BLE / WiFi / Log / Allow pages with scores and RSSI,
//   manual baseline (MODE hold), probe (OK), allow-list (OK hold), clear
//   (DOWN hold). Returns to simple mode after 60 s without a press.
//
// Detection:
//   BLE   known module names, FFE0/FFF0/FFE5 UART bridge service, module
//         vendor OUIs, unnamed non-phone devices very close by, RSSI jump
//         versus the previous check, and an active P/M handshake probe.
//   WiFi  default ESP/AI-Thinker SSIDs, open or hidden APs very close by,
//         RSSI jump versus the previous check.
//   Bluetooth Classic (HC-05/06) is NOT detectable on the C6.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_sleep.h>
#include <vector>
#include <map>
#include <algorithm>

// ---------------- pins ----------------
static int PIN_SDA          = 22;  // D4 (PCB); auto-detected at boot
static int PIN_SCL          = 23;  // D5
static const int PIN_HAPTIC  = 0;   // D0  vibration motor or active buzzer
static const int PIN_BTN_MODE= 1;   // D1  (must be GPIO0-7 for deep-sleep wake on C6)
static const int PIN_BTN_UP  = 2;   // D2
static const int PIN_BTN_DOWN= 21;  // D3
static const int PIN_BTN_OK  = 16;  // D6
static const int PIN_BTN_BOOT= 9;   // onboard
static const int PIN_LED     = 15;  // onboard, active LOW
static const int PIN_RF_EN   = 3;   // RF switch enable (LOW = on)
static const int PIN_RF_ANT  = 14;  // LOW = ceramic antenna

// ---------------- tuning ----------------
static const int      ALERT_THRESHOLD   = 60;
static const int      SUSPECT_THRESHOLD = 30;
static const int      NEAR_RSSI         = -60;
static const int      VERY_NEAR_RSSI    = -50;
static const int      BASELINE_JUMP_DB  = 15;     // RSSI rise vs baseline that counts
static const uint32_t BASELINE_MS       = 5000;
static const uint32_t STALE_MS          = 20000;
static const uint32_t LONG_PRESS_MS     = 1000;
static const uint32_t PRESS_MS          = 120;    // contact time before a press counts (simple mode)
static const uint32_t BOOT_HOLD_MS      = 3000;   // BOOT hold to toggle maintenance mode
static const uint32_t IDLE_SLEEP_MS     = 120000; // deep sleep after 2 min without a press
static const uint32_t CHECK_MS          = 10000;  // one check window (two WiFi scans fit)
static const uint32_t CHECK_MIN_STOP_MS = 2000;   // no STOP verdict before this much of the window
static const uint32_t CHECK_IGNORE_MS   = 1500;   // presses ignored at the start of a check
static const uint32_t RESULT_HOLD_MS    = 60000;  // SAFE/STOP shown this long, then READY
static const uint32_t RESULT_LOCK_MS    = 1000;   // presses ignored right after a verdict
static const uint32_t MAINT_IDLE_MS     = 60000;  // maintenance mode exits after this
static const uint32_t PROBE_TIMEOUT_MS  = 2500;
static const size_t   LOG_MAX           = 24;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Preferences prefs;

// ---------------- signatures ----------------
static const char* BLE_BAD_NAMES[] = {
    "HM-10", "HM-11", "HM-19", "HMSoft", "HC-08", "HC-02", "HC-10",
    "JDY-08", "JDY-10", "JDY-16", "JDY-18", "JDY-19", "JDY-23", "JDY",
    "BT05", "BT-05", "MLT-BT05", "CC41", "BLE-CC41", "AT-09", "SH-HC",
    "CC2541", "SPP-CA", "BLE_", "Skimmer", "MSR", "MagSpoof", "FaryLink",
    "HC-05", "HC-06"
};
static const char* WIFI_BAD_SSIDS[] = {
    "ESP_", "ESP-", "ESP32", "ESP8266", "AI-THINKER", "AI_THINKER",
    "FaryLink", "Skimmer", "HC-", "MSR", "WIFI_SKIM", "ESPap"
};
// OUIs of chips/modules that show up in skimmer kits. Lower weight than a
// name match because plenty of legitimate gear uses the same silicon.
static const char* SUSPECT_OUIS[] = {
    // Texas Instruments (CC2540/CC2541 = HM-10, CC41, AT-09, BT05)
    "D0:39:72", "B0:B4:48", "A0:E6:F8", "34:03:DE", "54:6C:0E", "68:C9:0B", "98:07:2D",
    // Espressif (ESP8266/ESP32 based skimmers and WiFi APs)
    "24:6F:28", "30:AE:A4", "A4:CF:12", "7C:9E:BD", "84:CC:A8", "C8:2B:96",
    "EC:FA:BC", "3C:71:BF", "48:3F:DA", "94:B9:7E", "DC:4F:22", "E8:DB:84",
    "AC:67:B2", "8C:AA:B5", "24:0A:C4", "5C:CF:7F", "60:01:94", "A0:20:A6",
    "BC:DD:C2", "2C:F4:32", "CC:50:E3", "D8:BF:C0", "24:62:AB", "08:3A:F2",
    "34:85:18", "F4:12:FA", "58:BF:25", "34:94:54", "10:06:1C"
};
static const uint16_t BENIGN_MFG[] = { 0x004C, 0x0006, 0x00E0, 0x0075, 0x0059, 0x038F };

// ---------------- state ----------------
enum Kind : uint8_t { KIND_BLE, KIND_WIFI };
struct Hit {
    String   id;
    String   name;
    int      rssi;
    int      score;
    Kind     kind;
    uint32_t lastSeen;
    uint8_t  seen;
    bool     hasUart;      // advertises FFE0/FFF0/FFE5
    int8_t   probe;        // -1 = not probed, 0 = no handshake, 1 = handshake!
    NimBLEAddress addr;
    uint8_t  factors;      // F_* bits, what the score is made of
};
enum : uint8_t { F_NAME = 1, F_UART = 2, F_OUI = 4, F_NEAR = 8, F_JUMP = 16, F_PROBE = 32 };
std::vector<Hit> hits;
std::map<String,int> baseline;      // id -> rssi captured away from the terminal
bool baselineValid = false;
std::vector<String> allowList;
struct LogEntry { uint32_t t; Kind kind; String id; String name; int score; };
std::vector<LogEntry> logEntries;

uint32_t bleSeenCount = 0, wifiSeenCount = 0;
uint32_t lastActivity = 0;
uint32_t lastLogWrite = 0;
bool     logDirty = false;

enum Page : uint8_t { PAGE_ALL, PAGE_BLE, PAGE_WIFI, PAGE_LOG, PAGE_ALLOW, PAGE_COUNT };
uint8_t page = PAGE_ALL;

// simple-mode state
enum Mode : uint8_t { MODE_SIMPLE, MODE_MAINT };
enum SState : uint8_t { ST_READY, ST_WAIT, ST_SAFE, ST_STOP };
Mode     mode = MODE_SIMPLE;
SState   sstate = ST_READY;
uint32_t stateAt = 0;          // when the current state began
uint8_t  checkWindows = 0;     // windows run in this check (max 2)
std::map<String,int> lastCheck;   // readings from the current window, becomes the next baseline
uint32_t lastMaintPress = 0;
int8_t   xShift = 0;           // burn-in: whole frame nudged by -1/0/+1 px
int     cursor = 0;
bool    alertActive = false, alertAcked = false;
String  alertId;
String  toast; uint32_t toastUntil = 0;

static void say(const String& s, uint32_t ms = 1500) { toast = s; toastUntil = millis() + ms; }

// ---------------- helpers ----------------
static bool startsWithCI(const String& s, const char* prefix) {
    size_t n = strlen(prefix);
    if (s.length() < n) return false;
    for (size_t i = 0; i < n; i++) if (tolower(s[i]) != tolower(prefix[i])) return false;
    return true;
}
static bool containsCI(const String& s, const char* needle) {
    String a = s; a.toLowerCase(); String b = needle; b.toLowerCase();
    return a.indexOf(b) >= 0;
}
static bool ouiSuspect(const String& mac) {
    for (auto o : SUSPECT_OUIS) if (startsWithCI(mac, o)) return true;
    return false;
}
static bool isAllowed(const String& id) {
    for (auto& a : allowList) if (a == id) return true;
    return false;
}

// ---------------- persistence ----------------
static void loadPrefs() {
    prefs.begin("skim", false);
    String al = prefs.getString("allow", "");
    int start = 0;
    while (start < (int)al.length()) {
        int e = al.indexOf('\n', start); if (e < 0) e = al.length();
        String s = al.substring(start, e); if (s.length()) allowList.push_back(s);
        start = e + 1;
    }
    String lg = prefs.getString("log", "");
    start = 0;
    while (start < (int)lg.length()) {
        int e = lg.indexOf('\n', start); if (e < 0) e = lg.length();
        String line = lg.substring(start, e); start = e + 1;
        int a = line.indexOf('|'), b = line.indexOf('|', a + 1), c = line.indexOf('|', b + 1), d = line.indexOf('|', c + 1);
        if (a < 0 || b < 0 || c < 0 || d < 0) continue;
        logEntries.push_back({ (uint32_t)line.substring(0, a).toInt(), (Kind)line.substring(a + 1, b).toInt(),
                               line.substring(b + 1, c), line.substring(c + 1, d), line.substring(d + 1).toInt() });
    }
}
static void saveAllow() {
    String s; for (auto& a : allowList) { s += a; s += '\n'; }
    prefs.putString("allow", s);
}
static void saveLog() {
    String s;
    for (auto& e : logEntries) s += String(e.t) + "|" + String((int)e.kind) + "|" + e.id + "|" + e.name + "|" + String(e.score) + "\n";
    prefs.putString("log", s);
    logDirty = false;
}
static void addLog(const Hit& h) {
    for (auto& e : logEntries) if (e.id == h.id && millis() - e.t < 600000) { e.score = max(e.score, h.score); logDirty = true; return; }
    if (logEntries.size() >= LOG_MAX) logEntries.erase(logEntries.begin());
    logEntries.push_back({ millis(), h.kind, h.id, h.name, h.score });
    logDirty = true;
    Serial.printf("[LOG] %s %s score=%d\n", h.id.c_str(), h.name.c_str(), h.score);
}

// ---------------- hits ----------------
static Hit* findHit(const String& id) { for (auto& h : hits) if (h.id == id) return &h; return nullptr; }

static int baselineBonus(const String& id, int rssi) {
    if (!baselineValid) return 0;
    auto it = baseline.find(id);
    if (it == baseline.end()) return (rssi >= NEAR_RSSI) ? 20 : 0;   // new strong device that was not around before
    return (rssi - it->second >= BASELINE_JUMP_DB) ? 30 : 0;        // got a lot stronger at the terminal
}

static void upsertHit(const String& id, const String& name, int rssi, int score, Kind kind, bool hasUart, const NimBLEAddress* addr, uint8_t factors = 0) {
    if (isAllowed(id)) return;
    uint32_t now = millis();
    int bb = baselineBonus(id, rssi);
    score += bb;
    if (bb) factors |= F_JUMP;
    if (hasUart) factors |= F_UART;
    if (mode == MODE_SIMPLE && sstate == ST_WAIT) {   // remember what this check heard
        auto it = lastCheck.find(id);
        if (it == lastCheck.end() || rssi > it->second) lastCheck[id] = rssi;
    }
    if (Hit* h = findHit(id)) {
        h->rssi = rssi; h->name = name; h->hasUart |= hasUart; h->factors |= factors;
        h->seen = min<int>(h->seen + 1, 250);
        int s = score + (h->seen >= 3 ? 10 : 0) + (h->probe == 1 ? 50 : 0);
        h->score = min(s, 100);
        h->lastSeen = now;
        if (h->score >= ALERT_THRESHOLD) addLog(*h);
        return;
    }
    if (score < SUSPECT_THRESHOLD) return;
    if (hits.size() >= 24) {
        size_t worst = 0;
        for (size_t i = 1; i < hits.size(); i++) if (hits[i].score < hits[worst].score) worst = i;
        if (hits[worst].score >= score) return;
        hits.erase(hits.begin() + worst);
    }
    Hit h{ id, name, rssi, min(score, 100), kind, now, 1, hasUart, -1, addr ? *addr : NimBLEAddress(), factors };
    hits.push_back(h);
    lastActivity = now;
    if (h.score >= ALERT_THRESHOLD) addLog(h);
}
static void expireHits() {
    uint32_t now = millis();
    for (size_t i = 0; i < hits.size();)
        if (now - hits[i].lastSeen > STALE_MS) hits.erase(hits.begin() + i); else i++;
}
static void sortHits() {
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score; return a.rssi > b.rssi; });
}

// ---------------- BLE ----------------
static int scoreBle(const NimBLEAdvertisedDevice* d, String& outName, bool& hasUart, uint8_t& factors) {
    int score = 0; factors = 0;
    String name = d->haveName() ? String(d->getName().c_str()) : "";
    outName = name;
    int rssi = d->getRSSI();
    String mac = d->getAddress().toString().c_str(); mac.toUpperCase();

    if (name.length()) for (auto bad : BLE_BAD_NAMES) if (startsWithCI(name, bad)) { score += 80; factors |= F_NAME; break; }

    hasUart = d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFE0)) ||
              d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFF0)) ||
              d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFE5));
    if (hasUart) { score += 40; factors |= F_UART; }
    if (ouiSuspect(mac)) { score += 25; factors |= F_OUI; }

    bool benignMfg = false;
    if (d->haveManufacturerData()) {
        std::string m = d->getManufacturerData();
        if (m.size() >= 2) { uint16_t cid = (uint8_t)m[0] | ((uint8_t)m[1] << 8); for (auto b : BENIGN_MFG) if (cid == b) benignMfg = true; }
    }
    if (!name.length() && !benignMfg) {
        if (rssi >= VERY_NEAR_RSSI) { score += 45; factors |= F_NEAR; }
        else if (rssi >= NEAR_RSSI) { score += 30; factors |= F_NEAR; }
    } else if (name.length() && rssi >= VERY_NEAR_RSSI && score > 0) score += 10;
    return score;
}

// The BLE scan runs continuously in the NimBLE host task. Each advertisement
// is scored in the callback and pushed onto a small queue; loop() drains it.
// Nothing in loop() blocks on the radio any more, so buttons and the display
// stay responsive.
struct BleRec { String id; String name; int rssi; int score; bool uart; NimBLEAddress addr; uint8_t factors; };
static std::vector<BleRec> bleQueue;
static SemaphoreHandle_t bleMutex = nullptr;
static std::map<String,uint32_t> bleSeenMap;      // id -> last seen ms, for the header count
static bool bleScanning = false;

class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* d) override {
        String id = d->getAddress().toString().c_str(); id.toUpperCase();
        String name; bool uart = false; uint8_t f = 0;
        int score = scoreBle(d, name, uart, f);
        BleRec r{ id, name, d->getRSSI(), score, uart, d->getAddress(), f };
        if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (bleQueue.size() < 64) bleQueue.push_back(r);
            xSemaphoreGive(bleMutex);
        }
    }
    void onScanEnd(const NimBLEScanResults&, int) override {
        if (bleScanning) NimBLEDevice::getScan()->start(0, false, true);   // keep it rolling
    }
};
static ScanCB scanCb;

static void bleStart() {
    bleScanning = true;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan->isScanning()) scan->start(0, false, true);
}
static void bleStop() {
    bleScanning = false;
    NimBLEDevice::getScan()->stop();
}

// Pull everything the callback collected since the last call.
static void bleDrain(bool intoBaseline) {
    std::vector<BleRec> batch;
    if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        batch.swap(bleQueue);
        xSemaphoreGive(bleMutex);
    }
    uint32_t now = millis();
    for (auto& r : batch) {
        if (intoBaseline) { baseline[r.id] = max(baseline.count(r.id) ? baseline[r.id] : -127, r.rssi); continue; }
        bleSeenMap[r.id] = now;
        if (r.score > 0 || baselineValid) {
            String label = r.name.length() ? r.name : ("?" + r.id.substring(9));
            upsertHit(r.id, label, r.rssi, r.score, KIND_BLE, r.uart, &r.addr, r.factors);
        }
    }
    // header count = distinct devices heard in the last few seconds
    for (auto it = bleSeenMap.begin(); it != bleSeenMap.end();)
        if (now - it->second > 5000) it = bleSeenMap.erase(it); else ++it;
    bleSeenCount = bleSeenMap.size();
}

// Active probe: HM-10 style modules answer "P" with "M" on FFE1.
static int probeDevice(Hit& h) {
    if (h.kind != KIND_BLE) return -1;
    say("Probing " + h.name, 3000);
    bleStop();
    NimBLEClient* c = NimBLEDevice::createClient();
    c->setConnectTimeout(PROBE_TIMEOUT_MS / 1000 + 1);
    int result = 0;
    if (c->connect(h.addr, true, false, false)) {
        static volatile bool gotM = false; gotM = false;
        const NimBLEUUID svcIds[] = { NimBLEUUID((uint16_t)0xFFE0), NimBLEUUID((uint16_t)0xFFF0), NimBLEUUID((uint16_t)0xFFE5) };
        const NimBLEUUID chrIds[] = { NimBLEUUID((uint16_t)0xFFE1), NimBLEUUID((uint16_t)0xFFF1), NimBLEUUID((uint16_t)0xFFE9) };
        NimBLERemoteCharacteristic* chr = nullptr;
        for (int i = 0; i < 3 && !chr; i++) {
            NimBLERemoteService* s = c->getService(svcIds[i]);
            if (s) chr = s->getCharacteristic(chrIds[i]);
        }
        if (chr) {
            if (chr->canNotify()) chr->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
                for (size_t i = 0; i < n; i++) if (d[i] == 'M') gotM = true; });
            chr->writeValue("P", 1, false);
            uint32_t t0 = millis();
            while (millis() - t0 < 800 && !gotM) delay(20);
            if (!gotM && chr->canRead()) { std::string v = chr->readValue(); if (v.find('M') != std::string::npos) gotM = true; }
            result = gotM ? 1 : 0;
        }
        c->disconnect();
    }
    NimBLEDevice::deleteClient(c);
    bleStart();
    h.probe = result;
    if (result == 1) { h.score = 100; h.factors |= F_PROBE; addLog(h); say("HANDSHAKE! skimmer", 4000); }
    else say(result == 0 ? "No handshake" : "Probe failed", 1500);
    Serial.printf("[PROBE] %s -> %d\n", h.id.c_str(), result);
    return result;
}

static void autoProbe() {
    for (auto& h : hits)
        if (h.kind == KIND_BLE && h.hasUart && h.probe == -1 && h.score >= SUSPECT_THRESHOLD) { probeDevice(h); return; }
}

// ---------------- WiFi ----------------
static bool wifiBusy = false;
static uint32_t wifiLastStart = 0;

static void wifiCollect(bool intoBaseline, int n) {
    wifiSeenCount = n;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i); int rssi = WiFi.RSSI(i);
        String bssid = WiFi.BSSIDstr(i); bssid.toUpperCase();
        if (intoBaseline) { baseline[bssid] = max(baseline.count(bssid) ? baseline[bssid] : -127, rssi); continue; }
        bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN, hidden = ssid.length() == 0;
        int score = 0; uint8_t f = 0;
        for (auto bad : WIFI_BAD_SSIDS) if (startsWithCI(ssid, bad) || containsCI(ssid, bad)) { score += 80; f |= F_NAME; break; }
        if (ouiSuspect(bssid)) { score += 25; f |= F_OUI; }
        if (open && rssi >= VERY_NEAR_RSSI) { score += 40; f |= F_NEAR; } else if (open && rssi >= NEAR_RSSI) { score += 20; f |= F_NEAR; }
        if (hidden && rssi >= VERY_NEAR_RSSI) { score += 35; f |= F_NEAR; } else if (hidden && rssi >= NEAR_RSSI) { score += 15; f |= F_NEAR; }
        if (score > 0 || baselineValid) {
            String label = hidden ? ("<hidden>" + bssid.substring(9)) : ssid;
            upsertHit(bssid, label, rssi, score, KIND_WIFI, false, nullptr, f);
        }
    }
    WiFi.scanDelete();
}
// Non-blocking: starts a scan every WIFI_PERIOD_MS and harvests it when ready.
static const uint32_t WIFI_PERIOD_MS = 4500;
static void wifiService() {
    if (wifiBusy) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) return;
        wifiBusy = false;
        if (n >= 0) wifiCollect(false, n); else WiFi.scanDelete();
        return;
    }
    if (millis() - wifiLastStart >= WIFI_PERIOD_MS) {
        wifiLastStart = millis();
        WiFi.scanNetworks(true, true);
        wifiBusy = true;
    }
}
// Blocking version, used only inside baseline capture.
static void wifiScanWindow(bool intoBaseline) {
    if (wifiBusy) { WiFi.scanDelete(); wifiBusy = false; }
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) { WiFi.scanDelete(); return; }
    wifiCollect(intoBaseline, n);
}

// ---------------- baseline ----------------
static void captureBaseline() {
    baseline.clear(); baselineValid = false;
    oled.clearDisplay(); oled.setTextSize(1);
    oled.setCursor(0, 8);  oled.print("BASELINE");
    oled.setCursor(0, 24); oled.print("Hold away from the");
    oled.setCursor(0, 34); oled.print("terminal. 5s...");
    oled.display();
    digitalWrite(PIN_LED, LOW);
    bleDrain(false);                                   // discard whatever was pending
    uint32_t t0 = millis();
    while (millis() - t0 < BASELINE_MS) { delay(50); bleDrain(true); }
    wifiScanWindow(true);
    digitalWrite(PIN_LED, HIGH);
    baselineValid = true;
    hits.clear();
    say("Baseline: " + String(baseline.size()) + " devs", 2000);
}

// ---------------- haptics ----------------
// Non-blocking pattern player: {on, off, on, off, ...} in ms, 0-terminated.
static const uint16_t HAP_TICK[]  = { 30, 0 };
static const uint16_t HAP_WAKE[]  = { 60, 0 };
static const uint16_t HAP_SAFE[]  = { 500, 0 };
static const uint16_t HAP_STOP[]  = { 150, 150, 150, 150, 150, 0 };
static const uint16_t* hapPat = nullptr;
static uint8_t  hapIdx = 0;
static uint32_t hapAt = 0;
static bool     hapOn = false;

static void haptic(const uint16_t* pat) { hapPat = pat; hapIdx = 0; hapAt = millis(); hapOn = true; digitalWrite(PIN_HAPTIC, HIGH); }
static void hapService() {
    if (!hapPat) return;
    uint16_t dur = hapPat[hapIdx];
    if (!dur) { hapPat = nullptr; hapOn = false; digitalWrite(PIN_HAPTIC, LOW); return; }
    if (millis() - hapAt < dur) return;
    hapIdx++; hapAt = millis();
    hapOn = (hapIdx % 2 == 0) && hapPat[hapIdx] != 0;
    digitalWrite(PIN_HAPTIC, hapOn ? HIGH : LOW);
}

// ---------------- UI ----------------
static void drawBars(int x, int y, int rssi) {
    int bars = rssi >= -50 ? 4 : rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -85 ? 1 : 0;
    for (int b = 0; b < 4; b++) { int h = 2 + b * 2;
        if (b < bars) oled.fillRect(x + b * 3, y + 8 - h, 2, h, SSD1306_WHITE);
        else          oled.drawRect(x + b * 3, y + 8 - h, 2, h, SSD1306_WHITE); }
}
static void drawHeader(const char* title) {
    oled.setTextSize(1); oled.setCursor(0, 0); oled.print(title);
    if (baselineValid) oled.print("*");
    oled.setCursor(70, 0); oled.printf("B%u W%u", (unsigned)bleSeenCount, (unsigned)wifiSeenCount);
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}
static std::vector<Hit*> visibleHits(int kindFilter) {
    std::vector<Hit*> v;
    for (auto& h : hits) { if (kindFilter >= 0 && h.kind != kindFilter) continue; if (h.score < SUSPECT_THRESHOLD) continue; v.push_back(&h); }
    return v;
}
static void drawList(const char* title, int kindFilter) {
    oled.clearDisplay(); drawHeader(title);
    auto v = visibleHits(kindFilter);
    if (cursor >= (int)v.size()) cursor = max(0, (int)v.size() - 1);
    int first = max(0, cursor - 3), y = 14, shown = 0;
    for (int i = first; i < (int)v.size() && shown < 4; i++, shown++) {
        Hit& h = *v[i];
        oled.setCursor(0, y); oled.print(i == cursor ? ">" : " ");
        oled.print(h.kind == KIND_BLE ? "b" : "w");
        String n = h.name; if (n.length() > 11) n = n.substring(0, 11);
        oled.print(n);
        if (h.probe == 1) oled.print("!");
        oled.setCursor(92, y); oled.printf("%3d", h.score);
        drawBars(114, y, h.rssi);
        y += 12;
    }
    if (v.empty()) { oled.setCursor(0, 30); oled.print(baselineValid ? "  Clear (vs baseline)" : "  Nothing suspicious"); }
}
static void drawLog() {
    oled.clearDisplay(); drawHeader("LOG");
    if (cursor >= (int)logEntries.size()) cursor = max(0, (int)logEntries.size() - 1);
    int n = logEntries.size(), y = 14, shown = 0;
    for (int i = n - 1 - cursor; i >= 0 && shown < 4; i--, shown++) {
        auto& e = logEntries[i];
        uint32_t m = e.t / 60000;
        oled.setCursor(0, y); oled.printf("%3lum %s ", (unsigned long)m, e.kind == KIND_BLE ? "b" : "w");
        String s = e.name; if (s.length() > 10) s = s.substring(0, 10); oled.print(s);
        oled.setCursor(110, y); oled.printf("%3d", e.score);
        y += 12;
    }
    if (!n) { oled.setCursor(0, 30); oled.print("  Log empty"); }
}
static void drawAllow() {
    oled.clearDisplay(); drawHeader("ALLOW");
    if (cursor >= (int)allowList.size()) cursor = max(0, (int)allowList.size() - 1);
    int y = 14, shown = 0;
    for (int i = max(0, cursor - 3); i < (int)allowList.size() && shown < 4; i++, shown++) {
        oled.setCursor(0, y); oled.print(i == cursor ? ">" : " "); oled.print(allowList[i]); y += 12;
    }
    if (allowList.empty()) { oled.setCursor(0, 30); oled.print("  Empty. OK-hold adds"); }
}
static void drawAlert(const Hit& h) {
    oled.clearDisplay(); oled.invertDisplay((millis() / 300) % 2 == 0);
    oled.setTextSize(2); oled.setCursor(10, 2); oled.print(h.probe == 1 ? "SKIMMER!" : "! ALERT !");
    oled.setTextSize(1);
    oled.setCursor(0, 24); oled.print(h.kind == KIND_BLE ? "BLE: " : "WiFi: "); oled.print(h.name);
    oled.setCursor(0, 36); oled.printf("RSSI %d  score %d", h.rssi, h.score);
    oled.setCursor(0, 48); oled.print(h.id);
    oled.setCursor(0, 56); oled.print("btn=ack  OK=probe");
    oled.display();
}
static void updateAlertState() {
    alertActive = false;
    for (auto& h : hits) if (h.score >= ALERT_THRESHOLD) {
        if (h.id != alertId) { alertId = h.id; alertAcked = false; lastActivity = millis(); }
        alertActive = true; break; }
    if (!alertActive) { alertId = ""; alertAcked = false; }
}
// Full-UI render (maintenance mode).
static void renderMaint() {
    sortHits(); updateAlertState();
    if (alertActive && !alertAcked) {
        drawAlert(hits[0]);
        bool on = (millis() / 150) % 2 == 0;
        digitalWrite(PIN_LED, on ? LOW : HIGH);
        return;
    }
    oled.invertDisplay(false);
    digitalWrite(PIN_LED, alertActive ? LOW : HIGH);
    switch (page) {
        case PAGE_ALL:   drawList("ALL", -1); break;
        case PAGE_BLE:   drawList("BLE", KIND_BLE); break;
        case PAGE_WIFI:  drawList("WiFi", KIND_WIFI); break;
        case PAGE_LOG:   drawLog(); break;
        default:         drawAllow(); break;
    }
    if (millis() < toastUntil) {
        oled.fillRect(0, 52, 128, 12, SSD1306_BLACK); oled.drawRect(0, 52, 128, 12, SSD1306_WHITE);
        oled.setCursor(3, 54); oled.print(toast);
    }
    oled.display();
}

// ---- simple mode screens ----
// Layout: big word (size 3, 18x24 px/char) on top, one size 2 line (12x16) below.
static void bigWord(const char* w, int y, uint16_t color = SSD1306_WHITE) {
    int wpx = strlen(w) * 18 - 3;
    oled.setTextSize(3); oled.setTextColor(color);
    oled.setCursor((128 - wpx) / 2 + xShift, y); oled.print(w);
    oled.setTextColor(SSD1306_WHITE);
}
static void line2(const char* t, int y, uint16_t color = SSD1306_WHITE) {
    int wpx = strlen(t) * 12 - 2;
    oled.setTextSize(2); oled.setTextColor(color);
    oled.setCursor((128 - wpx) / 2 + xShift, y); oled.print(t);
    oled.setTextColor(SSD1306_WHITE);
}
static void line1(const char* t, int y) {
    int wpx = strlen(t) * 6 - 1;
    oled.setTextSize(1);
    oled.setCursor((128 - wpx) / 2 + xShift, y); oled.print(t);
}
static void drawReady() {
    oled.clearDisplay();
    bigWord("READY", 10);
    line2("On the slot", 42);
    oled.display();
}
static void drawWait() {
    oled.clearDisplay();
    bigWord("WAIT", 2);
    // fill bar: 120 x 8, proportional to elapsed time in this window
    uint32_t el = millis() - stateAt; if (el > CHECK_MS) el = CHECK_MS;
    int fill = (int)(118UL * el / CHECK_MS);
    oled.drawRect(4 + xShift, 32, 120, 8, SSD1306_WHITE);
    if (fill > 0) oled.fillRect(5 + xShift, 33, fill, 6, SSD1306_WHITE);
    line2("Hold still", 46);
    oled.display();
}
static void drawSafe() {
    oled.clearDisplay();
    bigWord("SAFE", 10);
    line2("Go ahead", 42);
    oled.display();
}
static void drawStop() {
    oled.clearDisplay();
    oled.fillRect(0, 0, 128, 32, SSD1306_WHITE);           // inverted band: bright means stop
    bigWord("STOP", 4, SSD1306_BLACK);
    line2("Pay inside", 35);
    line1("Tap, cash, call Brian", 55);
    oled.display();
}
static void renderSimple() {
    switch (sstate) {
        case ST_READY: drawReady(); break;
        case ST_WAIT:  drawWait();  break;
        case ST_SAFE:  drawSafe();  break;
        case ST_STOP:  drawStop();  break;
    }
    // LED: blink while checking, solid on STOP, off otherwise
    bool led = sstate == ST_STOP || (sstate == ST_WAIT && (millis() / 500) % 2 == 0);
    digitalWrite(PIN_LED, led ? LOW : HIGH);
}
static void render() { if (mode == MODE_MAINT) renderMaint(); else renderSimple(); }

// ---- simple mode state machine ----
static bool stopWorthy(const Hit& h) {
    if (h.score < ALERT_THRESHOLD) return false;
    if (h.factors & (F_NAME | F_UART | F_OUI | F_PROBE)) return true;
    return (h.factors & F_NEAR) && (h.factors & F_JUMP);   // close AND got stronger since last check
}
static void enterState(SState st) {
    sstate = st; stateAt = millis();
    switch (st) {
        case ST_SAFE: haptic(HAP_SAFE); Serial.println("[CHECK] SAFE"); break;
        case ST_STOP: haptic(HAP_STOP); Serial.println("[CHECK] STOP"); break;
        default: break;
    }
    render();
}
static void startCheck() {
    // the previous check's readings are the comparison for this one
    if (!lastCheck.empty()) { baseline = lastCheck; baselineValid = true; }
    lastCheck.clear();
    hits.clear(); alertId = ""; alertAcked = false;
    checkWindows = 1;
    Serial.printf("[CHECK] start (baseline %s, %u ids)\n", baselineValid ? "yes" : "no", (unsigned)baseline.size());
    enterState(ST_WAIT);
}
static void simpleService() {
    uint32_t now = millis(), el = now - stateAt;
    switch (sstate) {
        case ST_WAIT: {
            if (el >= CHECK_MIN_STOP_MS)
                for (auto& h : hits) if (stopWorthy(h)) { addLog(h); enterState(ST_STOP); return; }
            if (el < CHECK_MS) return;
            bool suspect = false;
            for (auto& h : hits) if (h.score >= SUSPECT_THRESHOLD) suspect = true;
            if (suspect && checkWindows < 2) {        // ambiguous: run one more window, bar refills
                checkWindows++; stateAt = now;
                Serial.println("[CHECK] extending window");
                return;
            }
            enterState(ST_SAFE);
            break;
        }
        case ST_SAFE: case ST_STOP:
            if (el >= RESULT_HOLD_MS) enterState(ST_READY);
            break;
        default: break;
    }
}
static void simplePress() {
    lastActivity = millis();
    uint32_t el = millis() - stateAt;
    switch (sstate) {
        case ST_WAIT:
            if (el >= CHECK_IGNORE_MS) haptic(HAP_TICK);     // "I heard you, keep holding"
            return;
        case ST_SAFE: case ST_STOP:
            if (el < RESULT_LOCK_MS) return;                  // bounce right after the verdict
            // fall through
        default:
            haptic(HAP_TICK);
            startCheck();
    }
}
static void setMode(Mode m) {
    mode = m;
    if (m == MODE_MAINT) { lastMaintPress = millis(); page = PAGE_ALL; cursor = 0; Serial.println("[MODE] maintenance"); }
    else { enterState(ST_READY); Serial.println("[MODE] simple"); }
    render();
}

// ---------------- buttons ----------------
struct Btn { int pin; bool wasDown; uint32_t downAt; bool longFired; };
Btn btns[] = { {PIN_BTN_MODE,false,0,false}, {PIN_BTN_UP,false,0,false}, {PIN_BTN_DOWN,false,0,false},
               {PIN_BTN_OK,false,0,false}, {PIN_BTN_BOOT,false,0,false} };
enum { B_MODE, B_UP, B_DOWN, B_OK, B_BOOT };

static Hit* selectedHit() {
    int f = page == PAGE_BLE ? KIND_BLE : page == PAGE_WIFI ? KIND_WIFI : -1;
    if (page == PAGE_LOG || page == PAGE_ALLOW) return nullptr;
    if (alertActive && !alertAcked) return &hits[0];
    auto v = visibleHits(f);
    if (v.empty()) return nullptr;
    return v[min(cursor, (int)v.size() - 1)];
}

static void clearListOrLog() {
    if (page == PAGE_LOG) { logEntries.clear(); saveLog(); say("Log cleared"); }
    else { hits.clear(); alertId = ""; alertAcked = false; say("Cleared"); }
}
static void onShort(int b) {
    lastActivity = millis();
    if (alertActive && !alertAcked && b != B_OK) { alertAcked = true; return; }
    switch (b) {
        case B_MODE: page = (page + 1) % PAGE_COUNT; cursor = 0; break;
        case B_UP:   if (cursor > 0) cursor--; break;
        case B_DOWN: cursor++; break;
        case B_OK:   { Hit* h = selectedHit(); if (h) { if (h->kind == KIND_BLE) probeDevice(*h); else say("WiFi: no probe"); } else say("Nothing selected"); } break;
        case B_BOOT: if (alertActive && !alertAcked) alertAcked = true; else { page = (page + 1) % PAGE_COUNT; cursor = 0; } break;
    }
}
static void onLong(int b) {
    lastActivity = millis();
    switch (b) {
        case B_MODE: captureBaseline(); break;
        case B_OK:
            if (page == PAGE_ALLOW) {
                if (cursor < (int)allowList.size()) { allowList.erase(allowList.begin() + cursor); saveAllow(); say("Removed"); }
            } else if (Hit* h = selectedHit()) {
                if (!isAllowed(h->id)) { allowList.push_back(h->id); saveAllow(); }
                String id = h->id;
                hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const Hit& x){ return x.id == id; }), hits.end());
                say("Allowed " + id.substring(9));
            }
            break;
        case B_BOOT: clearListOrLog(); break;
        case B_UP:   cursor = 0; break;
        case B_DOWN: clearListOrLog(); break;
    }
}
// Button polling runs in its own FreeRTOS task (10 ms) so a press is never
// missed while loop() is busy. Events:
//   EV_DOWN   front button held PRESS_MS: the simple-mode action (fires once per press)
//   EV_SHORT  released before LONG_PRESS_MS (maintenance mode)
//   EV_LONG   held LONG_PRESS_MS (maintenance mode)
//   EV_HOLD3  BOOT held BOOT_HOLD_MS: toggle maintenance mode
enum BtnEvent : uint8_t { EV_NONE, EV_DOWN, EV_SHORT, EV_LONG, EV_HOLD3 };
struct BtnMsg { uint8_t btn; uint8_t ev; };
static QueueHandle_t btnQueue = nullptr;
struct BtnTrack { bool wasDown; uint32_t downAt; bool downFired, longFired, hold3Fired; };
static BtnTrack trk[5] = {};

static void buttonTask(void*) {
    for (;;) {
        uint32_t now = millis();
        for (int i = 0; i < 5; i++) {
            BtnTrack& t = trk[i];
            bool down = digitalRead(btns[i].pin) == LOW;
            if (down && !t.wasDown) { t.downAt = now; t.downFired = t.longFired = t.hold3Fired = false; }
            if (down && t.wasDown) {
                uint32_t held = now - t.downAt;
                if (!t.downFired && held >= PRESS_MS && i != B_BOOT) { t.downFired = true; BtnMsg m{ (uint8_t)i, EV_DOWN }; xQueueSend(btnQueue, &m, 0); }
                if (!t.longFired && held >= LONG_PRESS_MS) { t.longFired = true; BtnMsg m{ (uint8_t)i, EV_LONG }; xQueueSend(btnQueue, &m, 0); }
                if (!t.hold3Fired && held >= BOOT_HOLD_MS && i == B_BOOT) { t.hold3Fired = true; BtnMsg m{ (uint8_t)i, EV_HOLD3 }; xQueueSend(btnQueue, &m, 0); }
            }
            if (!down && t.wasDown && !t.longFired && now - t.downAt > 30) { BtnMsg m{ (uint8_t)i, EV_SHORT }; xQueueSend(btnQueue, &m, 0); }
            t.wasDown = down;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
static void startButtonTask() {
    btnQueue = xQueueCreate(8, sizeof(BtnMsg));
    xTaskCreate(buttonTask, "btns", 2048, nullptr, 2, nullptr);
}
// Drain queued presses. Called from loop() only.
static void handleButtons() {
    BtnMsg m;
    bool any = false;
    while (btnQueue && xQueueReceive(btnQueue, &m, 0) == pdTRUE) {
        if (m.ev == EV_HOLD3) { setMode(mode == MODE_SIMPLE ? MODE_MAINT : MODE_SIMPLE); any = true; continue; }
        if (mode == MODE_SIMPLE) {
            if (m.ev == EV_DOWN) { simplePress(); any = true; }
        } else {
            lastMaintPress = millis();
            if (m.ev == EV_SHORT) { onShort(m.btn); any = true; }
            else if (m.ev == EV_LONG && m.btn != B_BOOT) { onLong(m.btn); any = true; }
        }
    }
    if (any) render();
}

// ---------------- sleep ----------------
static void maybeSleep() {
    if (millis() - lastActivity < IDLE_SLEEP_MS) return;
    if (mode == MODE_MAINT) return;
    if (sstate == ST_STOP) return;                      // keep a STOP on screen while he is holding it
    if (logDirty) saveLog();
    oled.clearDisplay(); oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
    digitalWrite(PIN_LED, HIGH); digitalWrite(PIN_HAPTIC, LOW);
    bleStop();
    NimBLEDevice::deinit(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[SLEEP]");
    // Only GPIO0-7 can wake the C6: RED (GPIO1) and BLUE (GPIO2). RED is the one to teach.
    esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_BTN_MODE) | (1ULL << PIN_BTN_UP), ESP_GPIO_WAKEUP_GPIO_LOW);
    esp_deep_sleep_start();
}


static void i2cReport() {
    int found = 0;
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) { Serial.printf("[I2C] device at 0x%02X\n", a); found++; }
    }
    if (!found) Serial.println("[I2C] nothing found. Check D4/D5, VCC, GND.");
}

// Bit-banged probe: reports a real ACK only (the C6 hardware driver gave ghost hits).
static bool bbProbe(int sda, int scl, uint8_t addr) {
    auto rel = [](int p) { pinMode(p, INPUT_PULLUP); };
    auto low = [](int p) { pinMode(p, OUTPUT); digitalWrite(p, LOW); };
    auto d = []() { delayMicroseconds(10); };
    rel(sda); rel(scl); delay(2);
    if (!digitalRead(sda) || !digitalRead(scl)) return false;
    low(sda); d(); low(scl); d();                                  // START
    uint8_t b = addr << 1;
    for (int i = 7; i >= 0; i--) { if (b & (1 << i)) rel(sda); else low(sda); d(); rel(scl); d(); low(scl); }
    rel(sda); d(); rel(scl); d();
    bool ack = digitalRead(sda) == LOW;
    low(scl); d(); low(sda); d(); rel(scl); d(); rel(sda); d();   // STOP
    return ack;
}
static uint8_t detectOled() {
    const int pairs[2][2] = { {22, 23}, {23, 22} };               // {SDA, SCL}: PCB first, then swapped
    for (auto& pr : pairs)
        for (uint8_t a : {0x3C, 0x3D})
            if (bbProbe(pr[0], pr[1], a)) {
                PIN_SDA = pr[0]; PIN_SCL = pr[1];
                Serial.printf("[OLED] found 0x%02X  SDA=GPIO%d SCL=GPIO%d\n", a, PIN_SDA, PIN_SCL);
                return a;
            }
    Serial.println("[OLED] not found on D4/D5 either way");
    return 0x3C;
}

// ---------------- setup / loop ----------------
void setup() {
    // Confirm the wake press by touch before anything else is initialised.
    pinMode(PIN_HAPTIC, OUTPUT); digitalWrite(PIN_HAPTIC, HIGH); delay(60); digitalWrite(PIN_HAPTIC, LOW);
    Serial.begin(115200);
    for (auto& b : btns) pinMode(b.pin, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);    digitalWrite(PIN_LED, HIGH);
    pinMode(PIN_RF_EN, OUTPUT);  digitalWrite(PIN_RF_EN, LOW);
    pinMode(PIN_RF_ANT, OUTPUT); digitalWrite(PIN_RF_ANT, LOW);

    delay(300);
    uint8_t oledAddr = detectOled();
    Wire.begin(PIN_SDA, PIN_SCL);
    bool ok = oled.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    Serial.printf("[OLED] init %s\n", ok ? "OK" : "FAILED");
    oled.ssd1306_command(SSD1306_SETCONTRAST); oled.ssd1306_command(0xFF);   // full contrast for daylight
    oled.setTextColor(SSD1306_WHITE); oled.setTextWrap(false);
    drawReady();

    loadPrefs();
    WiFi.mode(WIFI_STA); WiFi.disconnect(true);
    NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
    scan->setDuplicateFilter(false); scan->setMaxResults(0);   // callbacks only, keep no result list
    scan->setScanCallbacks(&scanCb, false);
    bleMutex = xSemaphoreCreateMutex();
    bleStart();

    lastActivity = millis();
    stateAt = millis();
    startButtonTask();
}

void loop() {
    static uint32_t lastRender = 0, lastShift = 0;
    handleButtons();
    hapService();
    bleDrain(false);
    wifiService();
    expireHits();
    if (mode == MODE_MAINT) {
        autoProbe();
        if (millis() - lastMaintPress > MAINT_IDLE_MS) setMode(MODE_SIMPLE);
    } else {
        if (sstate == ST_WAIT) autoProbe();
        simpleService();
    }
    if (millis() - lastShift > 30000) {                    // burn-in: nudge the frame 1 px
        lastShift = millis(); static int8_t seq[] = { 0, 1, 0, -1 }; static uint8_t k = 0;
        xShift = seq[k++ & 3];
    }
    bool fast = (mode == MODE_MAINT && alertActive && !alertAcked) || sstate == ST_WAIT;
    if (millis() - lastRender >= (fast ? 150 : 500)) { render(); lastRender = millis(); }
    if (logDirty && millis() - lastLogWrite > 10000) { saveLog(); lastLogWrite = millis(); }
    maybeSleep();
    delay(10);
}
