// Card skimmer detector v2
// Board:   Seeed XIAO ESP32-C6
// Display: 0.96" SSD1306 128x64 I2C OLED
//
// Wiring (XIAO C6 silkscreen):
//   3V3 -> OLED VCC      GND -> OLED GND
//   D4/D5 -> OLED SDA/SCL. Orientation is auto-detected at boot, so the PCB
//            (SDA=D4, SCL=D5) and the hand-wired build (SDA=D5, SCL=D4) both work.
//   D0  -> haptic motor / active buzzer (+), other leg to GND   (GPIO0)
//   D1  -> MODE button (red)    -> GND                                 (GPIO1, deep-sleep wake pin)
//   D2  -> UP button (blue)     -> GND                                 (GPIO2)
//   D3  -> DOWN button (green)  -> GND                                 (GPIO21)
//   D6  -> OK button (yellow)   -> GND                                 (GPIO16)
//   BOOT (onboard, GPIO9) is also used.
//
// Buttons:
//   MODE (red)    short: next page (All / BLE / WiFi / Log / Allow list)
//         long : capture BASELINE (hold device a few feet from the terminal)
//   UP (blue) / DOWN (green): move cursor in the current list
//   OK (yellow)   short: probe the selected BLE device (FFE0 "P" -> "M" handshake)
//         long : add selected device to the persistent allow list
//               (on the Allow-list page: remove it)
//   Any front button acknowledges an alert (OK probes the alerting device instead).
//   DOWN (green) long: clear tracked devices; on Log page: clear the log
//   BOOT  short: acknowledge alert   long: same as DOWN long (BOOT is on the back on the PCB)
//   Any button wakes the unit from deep sleep (MODE is the wake pin).
//
// Detection:
//   BLE   known module names, FFE0/FFF0/FFE5 UART bridge service, module
//         vendor OUIs, unnamed non-phone devices very close by, RSSI jump
//         versus baseline, and an active P/M handshake probe.
//   WiFi  default ESP/AI-Thinker SSIDs, open or hidden APs very close by,
//         RSSI jump versus baseline.
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
static const uint32_t BLE_WINDOW_MS     = 1500;
static const uint32_t BASELINE_MS       = 5000;
static const uint8_t  WIFI_EVERY_N      = 3;
static const uint32_t STALE_MS          = 20000;
static const uint32_t LONG_PRESS_MS     = 1000;
static const uint32_t IDLE_SLEEP_MS     = 120000; // deep sleep after 2 min without buttons or hits
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
};
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

static void upsertHit(const String& id, const String& name, int rssi, int score, Kind kind, bool hasUart, const NimBLEAddress* addr) {
    if (isAllowed(id)) return;
    uint32_t now = millis();
    score += baselineBonus(id, rssi);
    if (Hit* h = findHit(id)) {
        h->rssi = rssi; h->name = name; h->hasUart |= hasUart;
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
    Hit h{ id, name, rssi, min(score, 100), kind, now, 1, hasUart, -1, addr ? *addr : NimBLEAddress() };
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
static int scoreBle(const NimBLEAdvertisedDevice* d, String& outName, bool& hasUart) {
    int score = 0;
    String name = d->haveName() ? String(d->getName().c_str()) : "";
    outName = name;
    int rssi = d->getRSSI();
    String mac = d->getAddress().toString().c_str(); mac.toUpperCase();

    if (name.length()) for (auto bad : BLE_BAD_NAMES) if (startsWithCI(name, bad)) { score += 80; break; }

    hasUart = d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFE0)) ||
              d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFF0)) ||
              d->isAdvertisingService(NimBLEUUID((uint16_t)0xFFE5));
    if (hasUart) score += 40;
    if (ouiSuspect(mac)) score += 25;

    bool benignMfg = false;
    if (d->haveManufacturerData()) {
        std::string m = d->getManufacturerData();
        if (m.size() >= 2) { uint16_t cid = (uint8_t)m[0] | ((uint8_t)m[1] << 8); for (auto b : BENIGN_MFG) if (cid == b) benignMfg = true; }
    }
    if (!name.length() && !benignMfg) {
        if (rssi >= VERY_NEAR_RSSI) score += 45; else if (rssi >= NEAR_RSSI) score += 30;
    } else if (name.length() && rssi >= VERY_NEAR_RSSI && score > 0) score += 10;
    return score;
}

static void bleScanWindow(uint32_t ms, bool intoBaseline) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    NimBLEScanResults res = scan->getResults(ms, false);
    bleSeenCount = res.getCount();
    for (int i = 0; i < res.getCount(); i++) {
        const NimBLEAdvertisedDevice* d = res.getDevice(i);
        String id = d->getAddress().toString().c_str(); id.toUpperCase();
        if (intoBaseline) { baseline[id] = max(baseline.count(id) ? baseline[id] : -127, (int)d->getRSSI()); continue; }
        String name; bool uart = false;
        int score = scoreBle(d, name, uart);
        if (score > 0 || baselineValid) {
            String label = name.length() ? name : ("?" + id.substring(9));
            NimBLEAddress a = d->getAddress();
            upsertHit(id, label, d->getRSSI(), score, KIND_BLE, uart, &a);
        }
    }
    scan->clearResults();
}

// Active probe: HM-10 style modules answer "P" with "M" on FFE1.
static int probeDevice(Hit& h) {
    if (h.kind != KIND_BLE) return -1;
    say("Probing " + h.name, 3000);
    NimBLEDevice::getScan()->stop();
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
    h.probe = result;
    if (result == 1) { h.score = 100; addLog(h); say("HANDSHAKE! skimmer", 4000); }
    else say(result == 0 ? "No handshake" : "Probe failed", 1500);
    Serial.printf("[PROBE] %s -> %d\n", h.id.c_str(), result);
    return result;
}

static void autoProbe() {
    for (auto& h : hits)
        if (h.kind == KIND_BLE && h.hasUart && h.probe == -1 && h.score >= SUSPECT_THRESHOLD) { probeDevice(h); return; }
}

// ---------------- WiFi ----------------
static void wifiScanWindow(bool intoBaseline) {
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) { WiFi.scanDelete(); return; }
    wifiSeenCount = n;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i); int rssi = WiFi.RSSI(i);
        String bssid = WiFi.BSSIDstr(i); bssid.toUpperCase();
        if (intoBaseline) { baseline[bssid] = max(baseline.count(bssid) ? baseline[bssid] : -127, rssi); continue; }
        bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN, hidden = ssid.length() == 0;
        int score = 0;
        for (auto bad : WIFI_BAD_SSIDS) if (startsWithCI(ssid, bad) || containsCI(ssid, bad)) { score += 80; break; }
        if (ouiSuspect(bssid)) score += 25;
        if (open && rssi >= VERY_NEAR_RSSI) score += 40; else if (open && rssi >= NEAR_RSSI) score += 20;
        if (hidden && rssi >= VERY_NEAR_RSSI) score += 35; else if (hidden && rssi >= NEAR_RSSI) score += 15;
        if (score > 0 || baselineValid) {
            String label = hidden ? ("<hidden>" + bssid.substring(9)) : ssid;
            upsertHit(bssid, label, rssi, score, KIND_WIFI, false, nullptr);
        }
    }
    WiFi.scanDelete();
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
    uint32_t t0 = millis();
    while (millis() - t0 < BASELINE_MS) bleScanWindow(1000, true);
    wifiScanWindow(true);
    digitalWrite(PIN_LED, HIGH);
    baselineValid = true;
    hits.clear();
    say("Baseline: " + String(baseline.size()) + " devs", 2000);
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
static void render() {
    sortHits(); updateAlertState();
    if (alertActive && !alertAcked) {
        drawAlert(hits[0]);
        bool on = (millis() / 150) % 2 == 0;
        digitalWrite(PIN_LED, on ? LOW : HIGH); digitalWrite(PIN_HAPTIC, on ? HIGH : LOW);
        return;
    }
    oled.invertDisplay(false);
    digitalWrite(PIN_LED, alertActive ? LOW : HIGH); digitalWrite(PIN_HAPTIC, LOW);
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
static void handleButtons() {
    uint32_t now = millis();
    for (int i = 0; i < 5; i++) {
        Btn& b = btns[i];
        bool down = digitalRead(b.pin) == LOW;
        if (down && !b.wasDown) { b.downAt = now; b.longFired = false; }
        if (down && b.wasDown && !b.longFired && now - b.downAt >= LONG_PRESS_MS) { b.longFired = true; onLong(i); }
        if (!down && b.wasDown && !b.longFired && now - b.downAt > 30) onShort(i);
        b.wasDown = down;
    }
}

// ---------------- sleep ----------------
static void maybeSleep() {
    if (millis() - lastActivity < IDLE_SLEEP_MS) return;
    if (alertActive) return;
    if (logDirty) saveLog();
    oled.clearDisplay(); oled.setTextSize(1); oled.setCursor(0, 28); oled.print("  Sleeping. MODE wakes"); oled.display();
    delay(800);
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
    digitalWrite(PIN_LED, HIGH); digitalWrite(PIN_HAPTIC, LOW);
    NimBLEDevice::deinit(true);
    WiFi.mode(WIFI_OFF);
    esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_BTN_MODE, ESP_GPIO_WAKEUP_GPIO_LOW);
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
    Serial.begin(115200);
    for (auto& b : btns) pinMode(b.pin, INPUT_PULLUP);
    pinMode(PIN_LED, OUTPUT);    digitalWrite(PIN_LED, HIGH);
    pinMode(PIN_HAPTIC, OUTPUT); digitalWrite(PIN_HAPTIC, LOW);
    pinMode(PIN_RF_EN, OUTPUT);  digitalWrite(PIN_RF_EN, LOW);
    pinMode(PIN_RF_ANT, OUTPUT); digitalWrite(PIN_RF_ANT, LOW);

    delay(300);
    uint8_t oledAddr = detectOled();
    Wire.begin(PIN_SDA, PIN_SCL);
    bool ok = oled.begin(SSD1306_SWITCHCAPVCC, oledAddr);
    Serial.printf("[OLED] init %s\n", ok ? "OK" : "FAILED");
    oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2); oled.setCursor(4, 8);  oled.print("SKIM-DET");
    oled.setTextSize(1); oled.setCursor(0, 32); oled.print("MODE-hold = baseline");
    oled.setCursor(0, 44); oled.print("OK = probe  OK-hold=allow");
    oled.display();

    loadPrefs();
    WiFi.mode(WIFI_STA); WiFi.disconnect(true);
    NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
    scan->setDuplicateFilter(false); scan->setMaxResults(64);

    lastActivity = millis();
    delay(800);
}

void loop() {
    static uint8_t cycle = 0;
    handleButtons();
    bleScanWindow(BLE_WINDOW_MS, false);
    handleButtons();
    if (++cycle >= WIFI_EVERY_N) { cycle = 0; wifiScanWindow(false); }
    expireHits();
    autoProbe();
    render();
    if (logDirty && millis() - lastLogWrite > 10000) { saveLog(); lastLogWrite = millis(); }
    maybeSleep();

    uint32_t t = millis();
    while (millis() - t < 150) { handleButtons(); if (alertActive && !alertAcked) render(); delay(25); }
}
