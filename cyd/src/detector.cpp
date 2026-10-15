#include "detector.h"
#include <WiFi.h>
#include <algorithm>

namespace det {

std::vector<Hit>      hits;
std::vector<LogEntry> logEntries;
std::vector<String>   allowList;
bool     baselineValid = false;
size_t   baselineSize  = 0;
uint32_t bleSeenCount = 0, wifiSeenCount = 0;
CheckState state = ST_READY;
uint32_t stateAt = 0;
uint8_t  checkWindows = 0;
String   lastVerdictName;

void (*onVerdict)(CheckState) = nullptr;
void (*onLog)(const LogEntry&) = nullptr;
void (*onToast)(const String&, uint32_t) = nullptr;
void (*onProgress)(const char*, int) = nullptr;

static void say(const String& s, uint32_t ms = 1500) { if (onToast) onToast(s, ms); }

// ---------------- tuning ----------------
static const int      NEAR_RSSI        = -60;
static const int      VERY_NEAR_RSSI   = -50;
static const int      BASELINE_JUMP_DB = 15;
static const uint32_t STALE_MS         = 20000;
static const uint32_t PROBE_TIMEOUT_MS = 2500;
static const uint32_t WIFI_PERIOD_MS   = 4500;

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
static const char* SUSPECT_OUIS[] = {
    // Texas Instruments (CC2540/CC2541 = HM-10, CC41, AT-09, BT05)
    "D0:39:72", "B0:B4:48", "A0:E6:F8", "34:03:DE", "54:6C:0E", "68:C9:0B", "98:07:2D",
    // Espressif
    "24:6F:28", "30:AE:A4", "A4:CF:12", "7C:9E:BD", "84:CC:A8", "C8:2B:96",
    "EC:FA:BC", "3C:71:BF", "48:3F:DA", "94:B9:7E", "DC:4F:22", "E8:DB:84",
    "AC:67:B2", "8C:AA:B5", "24:0A:C4", "5C:CF:7F", "60:01:94", "A0:20:A6",
    "BC:DD:C2", "2C:F4:32", "CC:50:E3", "D8:BF:C0", "24:62:AB", "08:3A:F2",
    "34:85:18", "F4:12:FA", "58:BF:25", "34:94:54", "10:06:1C"
};
static const uint16_t BENIGN_MFG[] = { 0x004C, 0x0006, 0x00E0, 0x0075, 0x0059, 0x038F };

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
bool isAllowed(const String& id) {
    for (auto& a : allowList) if (a == id) return true;
    return false;
}
const char* factorText(uint8_t f) {
    static char buf[48];
    buf[0] = 0;
    if (f & F_NAME)  strcat(buf, "name ");
    if (f & F_UART)  strcat(buf, "uart ");
    if (f & F_OUI)   strcat(buf, "oui ");
    if (f & F_NEAR)  strcat(buf, "near ");
    if (f & F_JUMP)  strcat(buf, "jump ");
    if (f & F_PROBE) strcat(buf, "HANDSHAKE ");
    if (!buf[0]) strcpy(buf, "-");
    return buf;
}

// ---------------- log ----------------
static void addLog(const Hit& h) {
    for (auto& e : logEntries)
        if (e.id == h.id && millis() - e.t < 600000) { e.score = max(e.score, h.score); e.factors |= h.factors; return; }
    if (logEntries.size() >= LOG_MAX) logEntries.erase(logEntries.begin());
    LogEntry e{ millis(), h.kind, h.id, h.name, h.score, h.rssi, h.factors };
    logEntries.push_back(e);
    if (onLog) onLog(e);
    Serial.printf("[LOG] %s %s score=%d\n", h.id.c_str(), h.name.c_str(), h.score);
}
void clearLog() { logEntries.clear(); }

// ---------------- hits ----------------
static std::map<String,int> baseline;
static std::map<String,int> lastCheck;

static Hit* findHit(const String& id) { for (auto& h : hits) if (h.id == id) return &h; return nullptr; }

static int baselineBonus(const String& id, int rssi) {
    if (!baselineValid) return 0;
    auto it = baseline.find(id);
    if (it == baseline.end()) return (rssi >= NEAR_RSSI) ? 20 : 0;
    return (rssi - it->second >= BASELINE_JUMP_DB) ? 30 : 0;
}

static void upsertHit(const String& id, const String& name, int rssi, int score, Kind kind, bool hasUart, const NimBLEAddress* addr, uint8_t factors) {
    if (isAllowed(id)) return;
    uint32_t now = millis();
    int bb = baselineBonus(id, rssi);
    score += bb;
    if (bb) factors |= F_JUMP;
    if (hasUart) factors |= F_UART;
    if (state == ST_WAIT) {
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
    if (hits.size() >= 32) {
        size_t worst = 0;
        for (size_t i = 1; i < hits.size(); i++) if (hits[i].score < hits[worst].score) worst = i;
        if (hits[worst].score >= score) return;
        hits.erase(hits.begin() + worst);
    }
    Hit h{ id, name, rssi, min(score, 100), kind, now, 1, hasUart, -1, addr ? *addr : NimBLEAddress(), factors };
    hits.push_back(h);
    if (h.score >= ALERT_THRESHOLD) addLog(h);
}
static void expireHits() {
    uint32_t now = millis();
    for (size_t i = 0; i < hits.size();)
        if (now - hits[i].lastSeen > STALE_MS) hits.erase(hits.begin() + i); else i++;
}
void sortHits() {
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score; return a.rssi > b.rssi; });
}
std::vector<Hit*> visibleHits(int kindFilter) {
    std::vector<Hit*> v;
    for (auto& h : hits) { if (kindFilter >= 0 && h.kind != kindFilter) continue; if (h.score < SUSPECT_THRESHOLD) continue; v.push_back(&h); }
    return v;
}
void clearHits() { hits.clear(); }
void allow(const String& id) {
    if (!isAllowed(id)) allowList.push_back(id);
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const Hit& x){ return x.id == id; }), hits.end());
}
void unallow(size_t idx) { if (idx < allowList.size()) allowList.erase(allowList.begin() + idx); }

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

struct BleRec { String id; String name; int rssi; int score; bool uart; NimBLEAddress addr; uint8_t factors; };
static std::vector<BleRec> bleQueue;
static SemaphoreHandle_t bleMutex = nullptr;
static std::map<String,uint32_t> bleSeenMap;
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
        if (bleScanning) NimBLEDevice::getScan()->start(0, false, true);
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
    for (auto it = bleSeenMap.begin(); it != bleSeenMap.end();)
        if (now - it->second > 5000) it = bleSeenMap.erase(it); else ++it;
    bleSeenCount = bleSeenMap.size();
}

// Active probe: HM-10 style modules answer "P" with "M" on FFE1.
static volatile bool gotM = false;
int probe(Hit& h) {
    if (h.kind != KIND_BLE) return -1;
    say("Probing " + h.name, 3000);
    if (onProgress) onProgress("Probing", -1);
    bleStop();
    NimBLEClient* c = NimBLEDevice::createClient();
    c->setConnectTimeout(PROBE_TIMEOUT_MS / 1000 + 1);
    int result = -1;
    if (c->connect(h.addr, true, false, false)) {
        result = 0; gotM = false;
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
void autoProbe() {
    for (auto& h : hits)
        if (h.kind == KIND_BLE && h.hasUart && h.probe == -1 && h.score >= SUSPECT_THRESHOLD) { probe(h); return; }
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
static void wifiScanWindow(bool intoBaseline) {
    if (wifiBusy) { WiFi.scanDelete(); wifiBusy = false; }
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) { WiFi.scanDelete(); return; }
    wifiCollect(intoBaseline, n);
}

// ---------------- baseline ----------------
void captureBaseline() {
    baseline.clear(); baselineValid = false;
    bleDrain(false);
    uint32_t t0 = millis();
    while (millis() - t0 < BASELINE_MS) {
        delay(50); bleDrain(true);
        if (onProgress) onProgress("Baseline", (millis() - t0) * 100 / BASELINE_MS);
    }
    if (onProgress) onProgress("Baseline", 100);
    wifiScanWindow(true);
    baselineValid = true;
    baselineSize = baseline.size();
    hits.clear();
    say("Baseline: " + String(baseline.size()) + " devs", 2000);
}

// ---------------- check state machine ----------------
static bool stopWorthy(const Hit& h) {
    if (h.score < ALERT_THRESHOLD) return false;
    if (h.factors & (F_NAME | F_UART | F_OUI | F_PROBE)) return true;
    return (h.factors & F_NEAR) && (h.factors & F_JUMP);
}
static void enterState(CheckState st) {
    state = st; stateAt = millis();
    if (st == ST_SAFE || st == ST_STOP) { Serial.printf("[CHECK] %s\n", st == ST_SAFE ? "SAFE" : "STOP"); if (onVerdict) onVerdict(st); }
}
void startCheck() {
    if (!lastCheck.empty()) { baseline = lastCheck; baselineValid = true; baselineSize = baseline.size(); }
    lastCheck.clear();
    hits.clear();
    checkWindows = 1;
    lastVerdictName = "";
    Serial.printf("[CHECK] start (baseline %s, %u ids)\n", baselineValid ? "yes" : "no", (unsigned)baseline.size());
    enterState(ST_WAIT);
}
static void checkService() {
    uint32_t now = millis(), el = now - stateAt;
    switch (state) {
        case ST_WAIT: {
            if (el >= CHECK_MIN_STOP_MS)
                for (auto& h : hits) if (stopWorthy(h)) { addLog(h); lastVerdictName = h.name; enterState(ST_STOP); return; }
            if (el < CHECK_MS) return;
            bool suspect = false;
            for (auto& h : hits) if (h.score >= SUSPECT_THRESHOLD) suspect = true;
            if (suspect && checkWindows < 2) { checkWindows++; stateAt = now; Serial.println("[CHECK] extending window"); return; }
            enterState(ST_SAFE);
            break;
        }
        case ST_SAFE: case ST_STOP:
            if (el >= RESULT_HOLD_MS) enterState(ST_READY);
            break;
        default: break;
    }
}

// ---------------- lifecycle ----------------
void begin() {
    WiFi.mode(WIFI_STA); WiFi.disconnect(true);
    NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true); scan->setInterval(100); scan->setWindow(99);
    scan->setDuplicateFilter(false); scan->setMaxResults(0);
    scan->setScanCallbacks(&scanCb, false);
    bleMutex = xSemaphoreCreateMutex();
    bleStart();
    stateAt = millis();
}
void service() {
    bleDrain(false);
    wifiService();
    expireHits();
    if (state == ST_WAIT) autoProbe();
    checkService();
}
void shutdown() {
    bleStop();
    NimBLEDevice::deinit(true);
    WiFi.mode(WIFI_OFF);
}

} // namespace det
