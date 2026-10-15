// Detection core: BLE + WiFi scanning, scoring, baseline, handshake probe and
// the check state machine. Display-agnostic; the UI reads these structures.
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include <map>

enum Kind : uint8_t { KIND_BLE, KIND_WIFI };
enum : uint8_t { F_NAME = 1, F_UART = 2, F_OUI = 4, F_NEAR = 8, F_JUMP = 16, F_PROBE = 32 };

struct Hit {
    String   id;
    String   name;
    int      rssi;
    int      score;
    Kind     kind;
    uint32_t lastSeen;
    uint8_t  seen;
    bool     hasUart;
    int8_t   probe;        // -1 not probed, 0 no handshake, 1 handshake
    NimBLEAddress addr;
    uint8_t  factors;
};
struct LogEntry { uint32_t t; Kind kind; String id; String name; int score; int rssi; uint8_t factors; bool hasFix; float lat, lon; };

// check state machine (the Home screen)
enum CheckState : uint8_t { ST_READY, ST_WAIT, ST_SAFE, ST_STOP };

namespace det {
    // tuning
    constexpr int      ALERT_THRESHOLD   = 60;
    constexpr int      SUSPECT_THRESHOLD = 30;
    constexpr uint32_t CHECK_MS          = 10000;
    constexpr uint32_t CHECK_MIN_STOP_MS = 2000;
    constexpr uint32_t RESULT_HOLD_MS    = 60000;
    constexpr uint32_t BASELINE_MS       = 5000;
    constexpr size_t   LOG_MAX           = 60;

    extern std::vector<Hit>      hits;
    extern std::vector<LogEntry> logEntries;
    extern std::vector<String>   allowList;
    extern bool     baselineValid;
    extern size_t   baselineSize;
    extern uint32_t bleSeenCount, wifiSeenCount;
    extern CheckState state;
    extern uint32_t stateAt;
    extern uint8_t  checkWindows;
    extern String   lastVerdictName;   // the device that produced the last STOP

    // hooks the app fills in
    extern void (*onVerdict)(CheckState st);          // SAFE or STOP reached
    extern void (*onLog)(const LogEntry& e);          // new log row (for SD)
    extern void (*onToast)(const String& s, uint32_t ms);
    extern void (*onProgress)(const char* what, int pct); // long blocking ops

    void begin();
    void service();                 // call every loop: drains BLE, WiFi, expiry, state machine
    void startCheck();
    void captureBaseline();
    int  probe(Hit& h);             // blocking, a few seconds
    void autoProbe();
    void allow(const String& id);
    void unallow(size_t idx);
    bool isAllowed(const String& id);
    void clearHits();
    void clearLog();
    void sortHits();
    std::vector<Hit*> visibleHits(int kindFilter);   // -1 = all
    const char* factorText(uint8_t f);               // "name uart near"
    void shutdown();                // before deep sleep
}
