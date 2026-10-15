#include "storage.h"
#include "board.h"
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>

namespace storage {

Settings s;
bool     sdOk = false;
uint32_t bootCount = 0;
uint32_t logRows = 0;

static Preferences prefs;
static SPIClass sdSpi(VSPI);
static const char* LOG_FILE   = "/skimlog.csv";
static const char* CHECK_FILE = "/checks.csv";
static const char* ALLOW_FILE = "/allow.txt";

static void loadAllowNvs() {
    String al = prefs.getString("allow", "");
    int start = 0;
    while (start < (int)al.length()) {
        int e = al.indexOf('\n', start); if (e < 0) e = al.length();
        String v = al.substring(start, e); if (v.length()) det::allowList.push_back(v);
        start = e + 1;
    }
}

static void ensureHeader(const char* path, const char* header) {
    if (!sdOk || SD.exists(path)) return;
    File f = SD.open(path, FILE_WRITE);
    if (f) { f.println(header); f.close(); }
}

bool remount() {
    SD.end();
    sdOk = SD.begin(PIN_SD_CS, sdSpi, 20000000);
    if (sdOk) {
        ensureHeader(LOG_FILE,   "boot,uptime_s,kind,id,name,score,rssi,factors");
        ensureHeader(CHECK_FILE, "boot,uptime_s,verdict,device,hits");
        // allow list on the card wins over NVS if present
        if (SD.exists(ALLOW_FILE)) {
            File f = SD.open(ALLOW_FILE, FILE_READ);
            if (f) {
                det::allowList.clear();
                while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l.length()) det::allowList.push_back(l); }
                f.close();
            }
        }
    }
    Serial.printf("[SD] %s\n", sdOk ? "mounted" : "no card");
    return sdOk;
}

void begin() {
    prefs.begin("skimcyd", false);
    bootCount = prefs.getUInt("boots", 0) + 1;
    prefs.putUInt("boots", bootCount);
    s.invert     = prefs.getBool("inv", s.invert);
    s.rgbSwap    = prefs.getBool("rgb", s.rgbSwap);
    s.rotation   = prefs.getUChar("rot", s.rotation);
    s.brightness = prefs.getUChar("bri", s.brightness);
    s.sound      = prefs.getBool("snd", s.sound);
    s.autoDim    = prefs.getBool("dim", s.autoDim);
    s.sleepMin   = prefs.getUChar("slp", s.sleepMin);
    s.led        = prefs.getBool("led", s.led);
    s.cal.x0     = prefs.getShort("cx0", s.cal.x0);
    s.cal.x1     = prefs.getShort("cx1", s.cal.x1);
    s.cal.y0     = prefs.getShort("cy0", s.cal.y0);
    s.cal.y1     = prefs.getShort("cy1", s.cal.y1);
    s.cal.swapXY = prefs.getBool("cswap", s.cal.swapXY);
    loadAllowNvs();
    sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    remount();
}

void saveSettings() {
    prefs.putBool("inv", s.invert);
    prefs.putBool("rgb", s.rgbSwap);
    prefs.putUChar("rot", s.rotation);
    prefs.putUChar("bri", s.brightness);
    prefs.putBool("snd", s.sound);
    prefs.putBool("dim", s.autoDim);
    prefs.putUChar("slp", s.sleepMin);
    prefs.putBool("led", s.led);
    prefs.putShort("cx0", s.cal.x0); prefs.putShort("cx1", s.cal.x1);
    prefs.putShort("cy0", s.cal.y0); prefs.putShort("cy1", s.cal.y1);
    prefs.putBool("cswap", s.cal.swapXY);
}

void saveAllow() {
    String all; for (auto& a : det::allowList) { all += a; all += '\n'; }
    prefs.putString("allow", all);
    if (sdOk) {
        File f = SD.open(ALLOW_FILE, FILE_WRITE);
        if (f) { f.print(all); f.close(); }
    }
}

static String csvSafe(const String& in) {
    String o = in; o.replace("\"", "'"); o.replace(",", ";"); o.replace("\n", " ");
    return o;
}

void appendLog(const LogEntry& e) {
    if (!sdOk) return;
    File f = SD.open(LOG_FILE, FILE_APPEND);
    if (!f) { sdOk = false; return; }
    f.printf("%lu,%lu,%s,%s,%s,%d,%d,%s\n", (unsigned long)bootCount, (unsigned long)(e.t / 1000),
             e.kind == KIND_BLE ? "ble" : "wifi", e.id.c_str(), csvSafe(e.name).c_str(), e.score, e.rssi,
             det::factorText(e.factors));
    f.close();
    logRows++;
}

void appendCheck(CheckState verdict, const String& who, size_t hitCount) {
    if (!sdOk) return;
    File f = SD.open(CHECK_FILE, FILE_APPEND);
    if (!f) { sdOk = false; return; }
    f.printf("%lu,%lu,%s,%s,%u\n", (unsigned long)bootCount, (unsigned long)(millis() / 1000),
             verdict == ST_STOP ? "STOP" : "SAFE", csvSafe(who).c_str(), (unsigned)hitCount);
    f.close();
}

void clearLogFile() {
    if (!sdOk) return;
    SD.remove(LOG_FILE);
    ensureHeader(LOG_FILE, "boot,uptime_s,kind,id,name,score,rssi,factors");
    logRows = 0;
}

String sdSummary() {
    if (!sdOk) return "no card";
    uint64_t used = SD.usedBytes(), total = SD.totalBytes();
    char b[40];
    snprintf(b, sizeof b, "SD %.1f/%.0f MB", used / 1048576.0, total / 1048576.0);
    return String(b);
}

void flush() { /* files are closed after every write */ }

} // namespace storage
