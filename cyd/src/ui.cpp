// Touch UI: capybara scene (Home), device list, log, setup, calibration.
#include "ui.h"
#include "hw.h"
#include "detector.h"
#include "storage.h"
#include "capy_frames.h"
#include "gps.h"

extern Touch touch;

namespace ui {

// ---------------- layout ----------------
static const int TOP   = 18;          // status bar height
static const int TABY  = 198;         // tab bar top
static const int TABH  = SCREEN_H - TABY;
static const int PANEX = 200;         // right pane x on Home
static const int CONT_H = TABY - TOP;

// ---------------- theme ----------------
static uint16_t C_BG, C_PANEL, C_PANEL2, C_TEXT, C_MUTED, C_ACCENT, C_GREEN, C_RED, C_BLUE, C_YEL, C_LINE;
static void initTheme() {
    C_BG     = lcd.color565(16, 18, 26);
    C_PANEL  = lcd.color565(30, 34, 46);
    C_PANEL2 = lcd.color565(44, 48, 64);
    C_TEXT   = lcd.color565(236, 230, 218);
    C_MUTED  = lcd.color565(140, 142, 156);
    C_ACCENT = lcd.color565(246, 142, 32);
    C_GREEN  = lcd.color565(70, 200, 110);
    C_RED    = lcd.color565(232, 60, 48);
    C_BLUE   = lcd.color565(90, 160, 240);
    C_YEL    = lcd.color565(250, 222, 84);
    C_LINE   = lcd.color565(60, 64, 84);
}

// ---------------- state ----------------
static Screen cur = SCR_HOME;
static bool dirty = true;              // full redraw of the current screen
static String toastText; static uint32_t toastUntil = 0;
static int listTop = 0;                // scroll offset for lists
static int detailIdx = -1;             // selected hit in SCR_DETAIL
static String detailId;
static bool progActive = false; static String progText; static int progPct = -1;
static uint32_t lastStatus = 0;
static uint8_t  verdictFlash = 0;      // frames of full-screen flash left
static int calibStep = 0; static uint16_t calRaw[3][2];
static uint32_t sleepAt = 0;           // when the capybara nodded off (0 = awake)
static const uint32_t NAP_AFTER_MS = 45000;

// capybara animation
enum CapyState : uint8_t { CP_IDLE, CP_SNIFF, CP_SAFE, CP_STOP, CP_SLEEP };
static CapyState capy = CP_IDLE;
static uint8_t capyFrame = 0;
static uint32_t capyAt = 0, blinkAt = 0;
static uint8_t ringPhase = 0;
static String bubble; static uint8_t bubbleIdx = 0;

static LGFX_Sprite scene(&lcd);
static const int CAL_PTS[3][2] = { {20, 20}, {300, 20}, {20, 220} };

// ---------------- speech ----------------
static const char* LINES_READY[] = { "Hold me on the card slot", "Tap me and I'll sniff", "Ready when you are", "Cards go in, I listen" };
static const char* LINES_WAIT[]  = { "Sniff sniff...", "Hmm, let me listen", "Hold still a sec", "Checking the air..." };
static const char* LINES_SAFE[]  = { "All clear, go ahead", "Nothing fishy here", "Smells like oranges", "You're good" };
static const char* LINES_STOP[]  = { "NOPE. Not this one!", "Skimmer! Walk away", "Don't use this reader", "Find another reader" };
static const char* LINES_SLEEP[] = { "z z z", "Zzz...", "mmm... oranges", "five more minutes", "...hot springs..." };
// idle chatter while the screen is on and nothing is happening (max ~24 chars)
static const char* LINES_IDLE[]  = {
    "Is it lunch yet?", "I only trust ducks", "Beep boop. Sniff sniff.", "Any tiny radios about?",
    "Hot spring would be nice", "Orange is a mood", "Quiet on the BLE front", "I contain no skimmers",
    "Your card trusts me", "Smells like receipts", "Pet me. No, scan first.", "2.4 GHz in my dreams",
    "Long line today", "Chill. Visibly chill.", "100% fraud-free rodent", "Listening, so you don't",
    "Capy-barely awake", "Nothing fishy. Yet.", "Checking the vibes", "Here and sniffing"
};
static const uint8_t LINES_IDLE_N = sizeof(LINES_IDLE) / sizeof(LINES_IDLE[0]);
static uint32_t chatterAt = 0;
static uint8_t  lastIdleLine = 255;

static void pickLine(CapyState st) {
    const char** arr; int n;
    switch (st) {
        case CP_SNIFF: arr = LINES_WAIT;  n = 4; break;
        case CP_SAFE:  arr = LINES_SAFE;  n = 4; break;
        case CP_STOP:  arr = LINES_STOP;  n = 4; break;
        case CP_SLEEP: arr = LINES_SLEEP; n = 3; break;
        default:       arr = LINES_READY; n = 4; break;
    }
    bubbleIdx = (bubbleIdx + 1 + (millis() % n)) % n;
    bubble = arr[bubbleIdx];
    chatterAt = millis();
}

// ---------------- small widgets ----------------
static void button(int x, int y, int w, int h, const char* label, uint16_t fill, uint16_t text, bool big = false) {
    lcd.fillRoundRect(x, y, w, h, 8, fill);
    lcd.drawRoundRect(x, y, w, h, 8, C_LINE);
    lcd.setTextColor(text, fill);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setFont(big ? &fonts::Font4 : &fonts::Font2);
    lcd.drawString(label, x + w / 2, y + h / 2);
}
static void bars(int x, int y, int rssi, uint16_t col) {
    int n = rssi >= -50 ? 4 : rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -85 ? 1 : 0;
    for (int b = 0; b < 4; b++) {
        int h = 3 + b * 3;
        if (b < n) lcd.fillRect(x + b * 5, y + 12 - h, 4, h, col);
        else       lcd.drawRect(x + b * 5, y + 12 - h, 4, h, C_MUTED);
    }
}
static uint16_t scoreColor(int s) { return s >= det::ALERT_THRESHOLD ? C_RED : s >= det::SUSPECT_THRESHOLD ? C_YEL : C_GREEN; }

static void drawStatus() {
    lcd.fillRect(0, 0, SCREEN_W, TOP, C_PANEL);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(textdatum_t::middle_left);
    lcd.setTextColor(C_BLUE, C_PANEL);  lcd.drawString("B", 6, TOP / 2);
    lcd.setTextColor(C_TEXT, C_PANEL);  lcd.drawString(String(det::bleSeenCount), 18, TOP / 2);
    lcd.setTextColor(C_ACCENT, C_PANEL); lcd.drawString("W", 48, TOP / 2);
    lcd.setTextColor(C_TEXT, C_PANEL);  lcd.drawString(String(det::wifiSeenCount), 62, TOP / 2);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_MUTED, C_PANEL);
    lcd.drawString(det::baselineValid ? "SKIM-DET *" : "SKIM-DET", SCREEN_W / 2 - 16, TOP / 2);
    // SD + uptime
    lcd.setTextDatum(textdatum_t::middle_right);
    uint32_t s = millis() / 1000;
    char t[16]; snprintf(t, sizeof t, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    String u = gps::utc();
    if (u.length() >= 19) { u = u.substring(11, 16); u += "Z"; } else u = t;   // GPS clock when there is one
    lcd.setTextColor(C_MUTED, C_PANEL); lcd.drawString(u, SCREEN_W - 6, TOP / 2);
    lcd.setTextColor(storage::sdOk ? C_GREEN : C_RED, C_PANEL); lcd.drawString(storage::sdOk ? "SD" : "sd", SCREEN_W - 52, TOP / 2);
    if (gps::enabled) {
        lcd.setTextColor(gps::fix ? C_GREEN : gps::charsSeen ? C_YEL : C_RED, C_PANEL);
        lcd.drawString(gps::fix ? "G" + String(gps::sats) : "G", SCREEN_W - 76, TOP / 2);
    }
}

static void drawTabs() {
    static const char* names[] = { "HOME", "DEVICES", "LOG", "SETUP" };
    int w = SCREEN_W / 4;
    for (int i = 0; i < 4; i++) {
        bool on = (int)cur == i || (cur == SCR_DETAIL && i == 1) || (cur >= SCR_ALLOW && i == 3);
        lcd.fillRect(i * w, TABY, w, TABH, on ? C_PANEL2 : C_PANEL);
        if (on) lcd.fillRect(i * w, TABY, w, 3, C_ACCENT);
        lcd.setFont(&fonts::Font2);
        lcd.setTextDatum(textdatum_t::middle_center);
        lcd.setTextColor(on ? C_TEXT : C_MUTED, on ? C_PANEL2 : C_PANEL);
        lcd.drawString(names[i], i * w + w / 2, TABY + TABH / 2 + 1);
    }
    lcd.drawFastHLine(0, TABY, SCREEN_W, C_LINE);
}

static void drawToast() {
    if (millis() >= toastUntil) return;
    lcd.fillRoundRect(40, TABY - 30, 240, 24, 6, C_PANEL2);
    lcd.drawRoundRect(40, TABY - 30, 240, 24, 6, C_ACCENT);
    lcd.setFont(&fonts::Font2); lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_TEXT, C_PANEL2);
    lcd.drawString(toastText, 160, TABY - 18);
}

// ---------------- the scene (Home, left pane) ----------------
static void sceneBegin() {
    scene.setColorDepth(4);
    scene.createSprite(PANEX, CONT_H);
    for (int i = 0; i < CAPY_COLORS; i++) {
        uint16_t c = pgm_read_word(&CAPY_PALETTE[i]);
        uint8_t r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
        if (i == 0) { r = 16; g = 18; b = 26; }        // index 0 is the scene background
        scene.setPaletteColor(i, r, g, b);
    }
}
static const int CAPY_X = 4, CAPY_Y = 26, ZOOM = 3;   // sprite coords of the capybara's top-left

static void drawCapyFrame(const uint8_t* frame, int ox, int oy) {
    for (int y = 0; y < CAPY_H; y++) {
        const uint8_t* row = frame + y * CAPY_W;
        int x = 0;
        while (x < CAPY_W) {
            uint8_t v = pgm_read_byte(row + x);
            if (!v) { x++; continue; }
            int run = 1;                                      // run-length the row: fewer fillRects
            while (x + run < CAPY_W && pgm_read_byte(row + x + run) == v) run++;
            scene.fillRect(ox + x * ZOOM, oy + y * ZOOM, run * ZOOM, ZOOM, v);
            x += run;
        }
    }
}

static const uint8_t* frameFor(CapyState st, uint8_t f) {
    switch (st) {
        case CP_SNIFF: return (const uint8_t*)pgm_read_ptr(&CAPY_SNIFF[f % CAPY_SNIFF_N]);
        case CP_SAFE:  return (const uint8_t*)pgm_read_ptr(&CAPY_SAFE[f % CAPY_SAFE_N]);
        case CP_STOP:  return (const uint8_t*)pgm_read_ptr(&CAPY_STOP[f % CAPY_STOP_N]);
        case CP_SLEEP: return (const uint8_t*)pgm_read_ptr(&CAPY_SLEEP[f % CAPY_SLEEP_N]);
        default:       return (const uint8_t*)pgm_read_ptr(&CAPY_IDLE[f % CAPY_IDLE_N]);
    }
}

static void drawBubble() {
    if (!bubble.length()) return;
    scene.setFont(&fonts::Font2);
    int tw = scene.textWidth(bubble) + 16;
    if (tw > PANEX - 6) { scene.setFont(&fonts::Font0); tw = scene.textWidth(bubble) + 16; }   // long line: small font
    int bx = PANEX - tw - 4, by = 2, bh = 24;
    if (bx < 2) bx = 2;
    scene.fillRoundRect(bx, by, tw, bh, 8, 7);
    // tail toward the head
    scene.fillTriangle(bx + tw - 36, by + bh - 1, bx + tw - 22, by + bh - 1, bx + tw - 26, by + bh + 7, 7);
    scene.setTextDatum(textdatum_t::middle_center);
    scene.setTextColor(6);
    scene.drawString(bubble, bx + tw / 2, by + bh / 2);
}

static void drawScene() {
    scene.fillSprite(0);
    // ground and pond
    scene.fillRect(0, 158, PANEX, CONT_H - 158, 10);                 // grass
    scene.fillRect(0, 158, PANEX, 2, 3);                             // grass edge highlight
    scene.fillEllipse(34, 166, 30, 7, 13);                           // pond
    scene.fillEllipse(34, 166, 22, 4, 7);                            // sparkle
    scene.fillEllipse(34, 166, 14, 3, 13);
    // radar rings while sniffing: expand from the nose
    if (capy == CP_SNIFF) {
        int cx = CAPY_X + 57 * ZOOM, cy = CAPY_Y + 23 * ZOOM;
        for (int i = 0; i < 3; i++) {
            int r = ((ringPhase + i * 10) % 30) * 3 + 6;
            scene.drawCircle(cx, cy, r, i == 0 ? 14 : 8);
        }
    }
    if (capy == CP_STOP) {
        // alarm: exclamation marks around
        scene.setFont(&fonts::Font4); scene.setTextColor(12); scene.setTextDatum(textdatum_t::middle_center);
        scene.drawString("!", 30 + (capyFrame & 1) * 4, 40); scene.drawString("!", 160 - (capyFrame & 1) * 4, 30);
    }
    drawCapyFrame(frameFor(capy, capyFrame), CAPY_X, CAPY_Y);
    if (capy == CP_SLEEP) {
        scene.setFont(&fonts::Font2); scene.setTextColor(13); scene.setTextDatum(textdatum_t::middle_center);
        int ph = (millis() / 400) % 4;
        scene.drawString("z", 150, 56 - ph * 6);
        scene.drawString("Z", 166, 44 - ((ph + 2) % 4) * 6);
    }
    if (capy == CP_SAFE) {
        scene.setFont(&fonts::Font2); scene.setTextColor(14); scene.setTextDatum(textdatum_t::middle_center);
        int ph = (millis() / 300) % 2;
        scene.drawString("+", 18 + ph * 2, 60); scene.drawString("+", 184, 90 - ph * 3);
    }
    drawBubble();
    scene.pushSprite(0, TOP);
    drawToast();
}

static void drawHomePane() {
    int x = PANEX, w = SCREEN_W - PANEX;
    lcd.fillRect(x, TOP, w, CONT_H, C_PANEL);
    lcd.drawFastVLine(x, TOP, CONT_H, C_LINE);
    lcd.setTextDatum(textdatum_t::middle_center);
    int cx = x + w / 2;
    const char* word; uint16_t col; const char* sub; const char* btn;
    switch (det::state) {
        case ST_WAIT: word = "SNIFF"; col = C_YEL;   sub = "Hold still";  btn = nullptr; break;
        case ST_SAFE: word = "SAFE";  col = C_GREEN; sub = "Go ahead";    btn = "AGAIN"; break;
        case ST_STOP: word = "STOP";  col = C_RED;   sub = "Not this one";  btn = "WHY?"; break;
        default:      word = "READY"; col = C_TEXT;  sub = "On the slot"; btn = "CHECK"; break;
    }
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.setTextColor(col, C_PANEL);
    lcd.drawString(word, cx, TOP + 34);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(C_MUTED, C_PANEL);
    lcd.drawString(sub, cx, TOP + 64);
    if (det::state == ST_STOP && det::lastVerdictName.length()) {
        String n = det::lastVerdictName; if (n.length() > 14) n = n.substring(0, 14);
        lcd.setTextColor(C_RED, C_PANEL); lcd.drawString(n, cx, TOP + 84);
    } else if (det::state == ST_WAIT) {
        lcd.drawString(det::checkWindows > 1 ? "window 2/2" : "window 1/2", cx, TOP + 84);
    } else {
        int susp = det::visibleHits(-1).size();
        lcd.setTextColor(susp ? C_YEL : C_MUTED, C_PANEL);
        lcd.drawString(susp ? String(susp) + " suspect" + (susp > 1 ? "s" : "") : "no suspects", cx, TOP + 84);
    }
    if (btn) button(x + 8, TOP + 112, w - 16, 56, btn, det::state == ST_STOP ? C_RED : C_ACCENT, det::state == ST_STOP ? C_TEXT : lcd.color565(30, 20, 10), true);
    else { lcd.drawRoundRect(x + 8, TOP + 112, w - 16, 56, 8, C_LINE); }
}
static void drawHomeProgress() {
    if (det::state != ST_WAIT) return;
    int x = PANEX + 16, y = TOP + 130, w = SCREEN_W - PANEX - 32, h = 20;
    uint32_t el = millis() - det::stateAt; if (el > det::CHECK_MS) el = det::CHECK_MS;
    int fill = (int)((uint64_t)(w - 4) * el / det::CHECK_MS);
    lcd.drawRoundRect(x, y, w, h, 4, C_LINE);
    lcd.fillRect(x + 2, y + 2, fill, h - 4, C_YEL);
    lcd.fillRect(x + 2 + fill, y + 2, w - 4 - fill, h - 4, C_PANEL);
}

// ---------------- device list ----------------
static const int ROW_H = 28, ROWS = (CONT_H - 4) / ROW_H;   // 6 rows
static void drawListChrome(const char* title, int count) {
    lcd.fillRect(0, TOP, SCREEN_W, CONT_H, C_BG);
    // scroll arrows
    button(SCREEN_W - 34, TOP + 2, 32, CONT_H / 2 - 3, "^", C_PANEL, C_TEXT);
    button(SCREEN_W - 34, TOP + CONT_H / 2 + 1, 32, CONT_H / 2 - 3, "v", C_PANEL, C_TEXT);
    (void)title; (void)count;
}
static void drawDevices() {
    det::sortHits();
    auto v = det::visibleHits(-1);
    drawListChrome("DEVICES", v.size());
    if (listTop > (int)v.size() - ROWS) listTop = max(0, (int)v.size() - ROWS);
    lcd.setFont(&fonts::Font2);
    if (v.empty()) {
        lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_MUTED, C_BG);
        lcd.drawString(det::baselineValid ? "Clear (vs baseline)" : "Nothing suspicious nearby", 140, TOP + CONT_H / 2 - 10);
        lcd.drawString("Hits appear here as they are heard", 140, TOP + CONT_H / 2 + 10);
        return;
    }
    for (int i = 0; i < ROWS && listTop + i < (int)v.size(); i++) {
        Hit& h = *v[listTop + i];
        int y = TOP + 2 + i * ROW_H;
        lcd.fillRoundRect(2, y, SCREEN_W - 40, ROW_H - 2, 5, C_PANEL);
        uint16_t kc = h.kind == KIND_BLE ? C_BLUE : C_ACCENT;
        lcd.fillRoundRect(6, y + 5, 22, ROW_H - 12, 4, kc);
        lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_BG, kc);
        lcd.drawString(h.kind == KIND_BLE ? "B" : "W", 17, y + ROW_H / 2 - 1);
        lcd.setTextDatum(textdatum_t::middle_left); lcd.setTextColor(C_TEXT, C_PANEL);
        String n = h.name; if (n.length() > 17) n = n.substring(0, 17);
        if (h.probe == 1) n += " !";
        lcd.drawString(n, 34, y + ROW_H / 2 - 1);
        // score pill
        uint16_t sc = scoreColor(h.score);
        lcd.fillRoundRect(SCREEN_W - 110, y + 5, 36, ROW_H - 12, 4, sc);
        lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_BG, sc);
        lcd.drawString(String(h.score), SCREEN_W - 92, y + ROW_H / 2 - 1);
        bars(SCREEN_W - 68, y + 6, h.rssi, C_TEXT);
    }
}
static void drawDetail() {
    Hit* h = nullptr;
    for (auto& x : det::hits) if (x.id == detailId) h = &x;
    lcd.fillRect(0, TOP, SCREEN_W, CONT_H, C_BG);
    if (!h) { lcd.setFont(&fonts::Font2); lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_MUTED, C_BG);
              lcd.drawString("Device went quiet", 160, TOP + 60); button(110, TOP + 110, 100, 40, "BACK", C_PANEL, C_TEXT); return; }
    lcd.setFont(&fonts::Font4); lcd.setTextDatum(textdatum_t::top_left); lcd.setTextColor(C_TEXT, C_BG);
    String n = h->name; if (n.length() > 18) n = n.substring(0, 18);
    lcd.drawString(n, 10, TOP + 6);
    lcd.setFont(&fonts::Font2); lcd.setTextColor(C_MUTED, C_BG);
    lcd.drawString(String(h->kind == KIND_BLE ? "BLE  " : "WiFi  ") + h->id, 10, TOP + 36);
    lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString("Score " + String(h->score) + "   RSSI " + String(h->rssi) + " dBm   seen x" + String(h->seen), 10, TOP + 56);
    lcd.setTextColor(scoreColor(h->score), C_BG);
    lcd.drawString(String("Why: ") + det::factorText(h->factors), 10, TOP + 76);
    lcd.setTextColor(h->probe == 1 ? C_RED : C_MUTED, C_BG);
    lcd.drawString(h->probe == 1 ? "Handshake: YES, this is a skimmer module" : h->probe == 0 ? "Handshake: no reply" : h->hasUart ? "Handshake: not tried yet" : "Handshake: n/a (no UART service)", 10, TOP + 96);
    button(8, TOP + 126, 96, 44, "PROBE", h->kind == KIND_BLE ? C_BLUE : C_PANEL, h->kind == KIND_BLE ? C_BG : C_MUTED);
    button(112, TOP + 126, 96, 44, "ALLOW", C_PANEL2, C_TEXT);
    button(216, TOP + 126, 96, 44, "BACK", C_PANEL, C_TEXT);
}

// ---------------- log ----------------
static void drawLog() {
    auto& L = det::logEntries;
    drawListChrome("LOG", L.size());
    int n = L.size();
    lcd.setFont(&fonts::Font2);
    if (!n) {
        lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_MUTED, C_BG);
        lcd.drawString("Log empty", 140, TOP + CONT_H / 2 - 10);
        lcd.drawString(storage::sdOk ? "Alerts also go to /skimlog.csv" : "No SD card: log is RAM only", 140, TOP + CONT_H / 2 + 10);
        return;
    }
    if (listTop > n - (ROWS - 1)) listTop = max(0, n - (ROWS - 1));
    for (int i = 0; i < ROWS - 1 && n - 1 - listTop - i >= 0; i++) {
        auto& e = L[n - 1 - listTop - i];
        int y = TOP + 2 + i * ROW_H;
        lcd.fillRoundRect(2, y, SCREEN_W - 40, ROW_H - 2, 5, C_PANEL);
        lcd.setTextDatum(textdatum_t::middle_left); lcd.setTextColor(C_MUTED, C_PANEL);
        uint32_t s = e.t / 1000; char t[12]; snprintf(t, sizeof t, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        lcd.drawString(t, 8, y + ROW_H / 2 - 1);
        lcd.setTextColor(e.kind == KIND_BLE ? C_BLUE : C_ACCENT, C_PANEL);
        lcd.drawString(e.kind == KIND_BLE ? "B" : "W", 56, y + ROW_H / 2 - 1);
        lcd.setTextColor(C_TEXT, C_PANEL);
        String nm = e.name; if (nm.length() > 16) nm = nm.substring(0, 16);
        lcd.drawString(nm, 72, y + ROW_H / 2 - 1);
        if (e.hasFix) { lcd.setTextColor(C_GREEN, C_PANEL); lcd.drawString("@", SCREEN_W - 100, y + ROW_H / 2 - 1); }
        uint16_t sc = scoreColor(e.score);
        lcd.fillRoundRect(SCREEN_W - 84, y + 5, 36, ROW_H - 12, 4, sc);
        lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_BG, sc);
        lcd.drawString(String(e.score), SCREEN_W - 66, y + ROW_H / 2 - 1);
    }
    int y = TOP + 2 + (ROWS - 1) * ROW_H;
    button(2, y, 90, ROW_H - 2, "CLEAR", C_PANEL2, C_TEXT);
    lcd.setFont(&fonts::Font2); lcd.setTextDatum(textdatum_t::middle_left); lcd.setTextColor(C_MUTED, C_BG);
    lcd.drawString(storage::sdSummary() + (storage::sdOk ? "  rows " + String(storage::logRows) : ""), 100, y + ROW_H / 2 - 1);
}

// ---------------- setup ----------------
struct SetupBtn { const char* label; };
static const int SB_W = 100, SB_H = 32, SB_COLS = 3, SB_ROWS = 5;
static String setupLabel(int i) {
    auto& s = storage::s;
    switch (i) {
        case 0: return String("Sound: ") + (s.sound ? "on" : "off");
        case 1: return String("LED: ") + (s.led ? "on" : "off");
        case 2: return String("Auto dim: ") + (s.autoDim ? "on" : "off");
        case 3: return s.sleepMin ? "Sleep: " + String(s.sleepMin) + " min" : "Sleep: never";
        case 4: return String("Invert: ") + (s.invert ? "on" : "off");
        case 5: return String("R/B swap: ") + (s.rgbSwap ? "on" : "off");
        case 6: return "Flip screen";
        case 7: return "Bright: " + String(s.brightness * 100 / 255) + "%";
        case 8: return "Calibrate touch";
        case 9: return "Baseline now";
        case 10: return "Allowed (" + String(det::allowList.size()) + ")";
        case 11: return "About";
        case 12: return String("GPS: ") + (s.gpsOn ? "on" : "off");
        case 13: return "GPS baud: " + String(s.gpsBaud / 1000) + "k";
        default: return "Clear log";
    }
}
static void drawSetup() {
    lcd.fillRect(0, TOP, SCREEN_W, CONT_H, C_BG);
    int x0 = (SCREEN_W - SB_COLS * (SB_W + 6)) / 2 + 3, y0 = TOP + 6;
    for (int i = 0; i < SB_COLS * SB_ROWS; i++) {
        int c = i % SB_COLS, r = i / SB_COLS;
        uint16_t fill = (i == 9) ? C_ACCENT : (i == 8 || i == 11 || i == 10) ? C_PANEL2 : C_PANEL;
        button(x0 + c * (SB_W + 6), y0 + r * (SB_H + 3), SB_W, SB_H, setupLabel(i).c_str(), fill, i == 9 ? C_BG : C_TEXT);
    }
}
static int setupHit(int x, int y) {
    int x0 = (SCREEN_W - SB_COLS * (SB_W + 6)) / 2 + 3, y0 = TOP + 6;
    for (int i = 0; i < SB_COLS * SB_ROWS; i++) {
        int c = i % SB_COLS, r = i / SB_COLS;
        int bx = x0 + c * (SB_W + 6), by = y0 + r * (SB_H + 3);
        if (x >= bx && x < bx + SB_W && y >= by && y < by + SB_H) return i;
    }
    return -1;
}
static void drawAllow() {
    drawListChrome("ALLOW", det::allowList.size());
    lcd.setFont(&fonts::Font2);
    auto& A = det::allowList;
    if (A.empty()) { lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_MUTED, C_BG);
                     lcd.drawString("Allow list empty", 140, TOP + CONT_H / 2 - 10);
                     lcd.drawString("Devices > tap one > ALLOW", 140, TOP + CONT_H / 2 + 10); }
    for (int i = 0; i < ROWS - 1 && listTop + i < (int)A.size(); i++) {
        int y = TOP + 2 + i * ROW_H;
        lcd.fillRoundRect(2, y, SCREEN_W - 40, ROW_H - 2, 5, C_PANEL);
        lcd.setTextDatum(textdatum_t::middle_left); lcd.setTextColor(C_TEXT, C_PANEL);
        lcd.drawString(A[listTop + i], 10, y + ROW_H / 2 - 1);
        lcd.setTextDatum(textdatum_t::middle_right); lcd.setTextColor(C_RED, C_PANEL);
        lcd.drawString("remove", SCREEN_W - 48, y + ROW_H / 2 - 1);
    }
    button(2, TOP + 2 + (ROWS - 1) * ROW_H, 90, ROW_H - 2, "BACK", C_PANEL2, C_TEXT);
}
static void drawAbout() {
    lcd.fillRect(0, TOP, SCREEN_W, CONT_H, C_BG);
    lcd.setFont(&fonts::Font4); lcd.setTextDatum(textdatum_t::top_left); lcd.setTextColor(C_ACCENT, C_BG);
    lcd.drawString("Skimmer Detector", 10, TOP + 6);
    lcd.setFont(&fonts::Font2); lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString("Firmware " FW_VERSION "  CYD 2-USB", 10, TOP + 36);
    lcd.drawString("Free heap " + String(ESP.getFreeHeap() / 1024) + " KB   boot #" + String(storage::bootCount), 10, TOP + 56);
    lcd.drawString(storage::sdSummary() + "   " + gps::summary(), 10, TOP + 76);
    if (gps::fix) lcd.drawString(gps::latStr() + ", " + gps::lonStr() + "  " + gps::utc(), 10, TOP + 96);
    lcd.setTextColor(C_MUTED, C_BG);
    lcd.drawString("Finds HM-10/HC-08/JDY/CC41 bridges,", 10, TOP + 116);
    lcd.drawString("ESP APs, OUIs, UART svc, P/M probe.", 10, TOP + 132);
    lcd.drawString("GPS on CN1: 3V3 GND RX=22 TX=27", 10, TOP + 148);
    button(216, TOP + 128, 96, 44, "BACK", C_PANEL, C_TEXT);
}
static void drawCalib() {
    lcd.fillRect(0, TOP, SCREEN_W, CONT_H, C_BG);
    int x = CAL_PTS[calibStep][0], y = CAL_PTS[calibStep][1];
    lcd.drawCircle(x, y, 10, C_ACCENT); lcd.drawFastHLine(x - 14, y, 28, C_ACCENT); lcd.drawFastVLine(x, y - 14, 28, C_ACCENT);
    lcd.setFont(&fonts::Font2); lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_TEXT, C_BG);
    lcd.drawString("Tap the crosshair (" + String(calibStep + 1) + "/3)", 160, 120);
}

// ---------------- redraw dispatcher ----------------
static void redraw() {
    switch (cur) {
        case SCR_HOME:    drawHomePane(); drawScene(); break;
        case SCR_DEVICES: drawDevices(); break;
        case SCR_LOG:     drawLog(); break;
        case SCR_SETUP:   drawSetup(); break;
        case SCR_DETAIL:  drawDetail(); break;
        case SCR_ALLOW:   drawAllow(); break;
        case SCR_ABOUT:   drawAbout(); break;
        case SCR_CALIB:   drawCalib(); break;
    }
    drawTabs();
    drawToast();
    dirty = false;
}

// ---------------- public ----------------
void begin() {
    initTheme();
    sceneBegin();
    lcd.fillScreen(C_BG);
    pickLine(CP_IDLE);
    drawStatus();
    dirty = true;
}
void setScreen(Screen s) { cur = s; listTop = 0; dirty = true; if (s == SCR_CALIB) calibStep = 0; }
Screen screen() { return cur; }
void markDirty() { dirty = true; }
void toast(const String& s, uint32_t ms) { toastText = s; toastUntil = millis() + ms; dirty = true; }
bool capyAsleep() { return sleepAt != 0; }
void boop() { sound(SND_BOOP); pickLine(capy); dirty = true; }

void progress(const char* what, int pct) {
    // blocking operations call this from inside their loop: draw a modal bar
    lcd.fillRoundRect(60, 90, 200, 60, 8, C_PANEL2);
    lcd.drawRoundRect(60, 90, 200, 60, 8, C_ACCENT);
    lcd.setFont(&fonts::Font2); lcd.setTextDatum(textdatum_t::middle_center); lcd.setTextColor(C_TEXT, C_PANEL2);
    lcd.drawString(what, 160, 108);
    lcd.drawRoundRect(76, 124, 168, 14, 4, C_LINE);
    if (pct >= 0) lcd.fillRect(78, 126, (164 * pct) / 100, 10, C_ACCENT);
    else { int ph = (millis() / 100) % 140; lcd.fillRect(78, 126, 164, 10, C_PANEL2); lcd.fillRect(78 + ph, 126, 24, 10, C_ACCENT); }
    dirty = true;
}

void verdict(uint8_t st) {
    verdictFlash = st == ST_STOP ? 6 : 3;
    sleepAt = 0;
    capy = st == ST_STOP ? CP_STOP : CP_SAFE; capyFrame = 0;
    pickLine(capy);
    if (cur != SCR_HOME) setScreen(SCR_HOME);
    storage::appendCheck((CheckState)st, det::lastVerdictName, det::hits.size());
    dirty = true;
}

static void syncCapy() {
    CapyState want;
    switch (det::state) {
        case ST_WAIT: want = CP_SNIFF; break;
        case ST_SAFE: want = CP_SAFE; break;
        case ST_STOP: want = CP_STOP; break;
        default:      want = sleepAt ? CP_SLEEP : CP_IDLE; break;
    }
    if (want != capy) { capy = want; capyFrame = 0; pickLine(capy); if (cur == SCR_HOME) dirty = true; }
}

void service() {
    uint32_t now = millis();
    // nap when idle on READY
    if (det::state == ST_READY && !sleepAt && now - lastTouchMs() > NAP_AFTER_MS) sleepAt = now;
    syncCapy();
    // idle chatter: a new musing every 12 to 20 s while READY and awake, with an
    // instruction line mixed back in every few lines so the job stays obvious
    if (capy == CP_IDLE && cur == SCR_HOME && now - chatterAt > 12000 + (now % 8000)) {
        chatterAt = now;
        static uint8_t sinceHint = 0;
        if (++sinceHint >= 4) { sinceHint = 0; pickLine(CP_IDLE); }
        else {
            uint8_t i; do { i = (uint8_t)(esp_random() % LINES_IDLE_N); } while (i == lastIdleLine);
            lastIdleLine = i; bubble = LINES_IDLE[i];
        }
    } else if (capy == CP_SLEEP && cur == SCR_HOME && now - chatterAt > 20000) { chatterAt = now; pickLine(CP_SLEEP); }
    if (now - lastStatus > 1000) {
        lastStatus = now; drawStatus();
        // the Home pane's suspect count and the device list follow the radios
        static int lastSuspects = -1; int susp = det::visibleHits(-1).size();
        if (susp != lastSuspects) { lastSuspects = susp; if (cur == SCR_HOME || cur == SCR_DEVICES) dirty = true; }
        else if (cur == SCR_DEVICES && (now / 1000) % 3 == 0) dirty = true;
    }
    if (toastUntil && now >= toastUntil) { toastUntil = 0; dirty = true; }
    if (dirty) redraw();

    if (cur == SCR_HOME) {
        uint32_t period = capy == CP_SNIFF ? 220 : capy == CP_STOP ? 140 : capy == CP_SLEEP ? 900 : 600;
        bool blink = (capy == CP_IDLE) && (now - blinkAt > 3500);
        if (now - capyAt > period || blink) {
            capyAt = now;
            if (blink) { capyFrame = 2; blinkAt = now; }
            else if (capy == CP_IDLE) capyFrame = capyFrame == 1 ? 0 : 1;
            else capyFrame++;
            ringPhase += 3;
            if (verdictFlash) {
                verdictFlash--;
                lcd.fillRect(0, TOP, SCREEN_W, CONT_H, (verdictFlash & 1) ? (capy == CP_STOP ? C_RED : C_GREEN) : C_BG);
                if (!verdictFlash) { dirty = true; }
            } else {
                drawScene();
                drawHomeProgress();
            }
        }
    }
}

// ---------------- touch ----------------
static bool inRect(const TouchPoint& p, int x, int y, int w, int h) { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }

static void finishCalib() {
    // Targets in the touch controller's own frame (rotation 1). In rotation 3 the
    // screen is upside down, so the drawn crosshairs sit at mirrored positions.
    bool flip = storage::s.rotation == 3;
    auto T = [&](int i, int axis) { int v = CAL_PTS[i][axis]; return flip ? ((axis ? SCREEN_H : SCREEN_W) - 1 - v) : v; };
    int dxx = abs((int)calRaw[1][0] - (int)calRaw[0][0]), dxy = abs((int)calRaw[1][1] - (int)calRaw[0][1]);
    bool swap = dxy > dxx;
    float ax1 = swap ? calRaw[0][1] : calRaw[0][0], ax2 = swap ? calRaw[1][1] : calRaw[1][0];   // raw along x at targets 0 and 1
    float ay1 = swap ? calRaw[0][0] : calRaw[0][1], ay2 = swap ? calRaw[2][0] : calRaw[2][1];   // raw along y at targets 0 and 2
    float X1 = T(0, 0), X2 = T(1, 0), Y1 = T(0, 1), Y2 = T(2, 1);
    float kx = (ax2 - ax1) / (X2 - X1), ky = (ay2 - ay1) / (Y2 - Y1);
    TouchCal c;
    c.swapXY = swap;
    c.x0 = ax1 - kx * X1;  c.x1 = ax1 + kx * (SCREEN_W - 1 - X1);
    c.y0 = ay1 - ky * Y1;  c.y1 = ay1 + ky * (SCREEN_H - 1 - Y1);
    if (abs(c.x1 - c.x0) < 500 || abs(c.y1 - c.y0) < 500) { toast("Calibration failed, keep old", 2000); }
    else { storage::s.cal = c; storage::saveSettings(); toast("Touch calibrated", 1500); }
    touch.cal = storage::s.cal;
}

void onTouch(const TouchPoint& p) {
    sleepAt = 0;
    if (cur == SCR_CALIB) {
        calRaw[calibStep][0] = p.rawX; calRaw[calibStep][1] = p.rawY;
        sound(SND_TAP);
        if (++calibStep >= 3) { finishCalib(); setScreen(SCR_SETUP); } else dirty = true;
        return;
    }
    if (p.y >= TABY) {
        int t = p.x / (SCREEN_W / 4);
        Screen s = t == 0 ? SCR_HOME : t == 1 ? SCR_DEVICES : t == 2 ? SCR_LOG : SCR_SETUP;
        if (s != cur) { sound(SND_TAP); setScreen(s); }
        return;
    }
    auto& v = det::hits;
    switch (cur) {
        case SCR_HOME: {
            bool onBtn = inRect(p, PANEX + 8, TOP + 112, SCREEN_W - PANEX - 16, 56);
            if (det::state == ST_STOP && onBtn) { sound(SND_TAP); setScreen(SCR_DEVICES); return; }
            if (det::state == ST_WAIT) { sound(SND_TAP); toast("Hold still, sniffing", 1000); return; }
            if (onBtn || p.x < PANEX) {
                if ((det::state == ST_SAFE || det::state == ST_STOP) && millis() - det::stateAt < 1000) return;
                sound(SND_START); det::startCheck(); dirty = true;
            }
            break;
        }
        case SCR_DEVICES: {
            if (p.x >= SCREEN_W - 36) { sound(SND_TAP); if (p.y < TOP + CONT_H / 2) listTop = max(0, listTop - ROWS); else listTop += ROWS; dirty = true; return; }
            auto vis = det::visibleHits(-1);
            int row = (p.y - TOP - 2) / ROW_H;
            if (row >= 0 && listTop + row < (int)vis.size()) { sound(SND_TAP); detailId = vis[listTop + row]->id; setScreen(SCR_DETAIL); }
            break;
        }
        case SCR_DETAIL: {
            Hit* h = nullptr; for (auto& x : v) if (x.id == detailId) h = &x;
            if (inRect(p, 216, TOP + 126, 96, 44) || !h || inRect(p, 110, TOP + 110, 100, 40) ) { sound(SND_TAP); setScreen(SCR_DEVICES); return; }
            if (inRect(p, 8, TOP + 126, 96, 44)) { if (h->kind == KIND_BLE) { sound(SND_TAP); det::probe(*h); } else toast("WiFi: no probe", 1200); dirty = true; return; }
            if (inRect(p, 112, TOP + 126, 96, 44)) { sound(SND_TAP); det::allow(h->id); storage::saveAllow(); toast("Allowed " + h->id.substring(9), 1500); setScreen(SCR_DEVICES); return; }
            break;
        }
        case SCR_LOG: {
            if (p.x >= SCREEN_W - 36) { sound(SND_TAP); if (p.y < TOP + CONT_H / 2) listTop = max(0, listTop - (ROWS - 1)); else listTop += ROWS - 1; dirty = true; return; }
            if (inRect(p, 2, TOP + 2 + (ROWS - 1) * ROW_H, 90, ROW_H - 2)) { sound(SND_TAP); det::clearLog(); storage::clearLogFile(); toast("Log cleared", 1200); dirty = true; return; }
            {   // tap a row: where was it?
                auto& L = det::logEntries; int n = L.size();
                int row = (p.y - TOP - 2) / ROW_H;
                if (row >= 0 && row < ROWS - 1 && n - 1 - listTop - row >= 0) {
                    auto& e = L[n - 1 - listTop - row];
                    sound(SND_TAP);
                    toast(e.hasFix ? String(e.lat, 5) + ", " + String(e.lon, 5) : "No GPS fix for this one", 2500);
                }
            }
            break;
        }
        case SCR_SETUP: {
            int i = setupHit(p.x, p.y);
            if (i < 0) return;
            sound(SND_TAP);
            auto& s = storage::s;
            switch (i) {
                case 0: s.sound = !s.sound; break;
                case 1: s.led = !s.led; if (!s.led) led(false, false, false); break;
                case 2: s.autoDim = !s.autoDim; break;
                case 3: s.sleepMin = s.sleepMin == 0 ? 1 : s.sleepMin == 1 ? 3 : s.sleepMin == 3 ? 10 : 0; break;
                case 4: s.invert = !s.invert; applyDisplaySettings(); break;
                case 5: s.rgbSwap = !s.rgbSwap; applyDisplaySettings(); break;
                case 6: s.rotation = s.rotation == 1 ? 3 : 1; applyDisplaySettings(); lcd.fillScreen(C_BG); drawStatus(); break;
                case 7: s.brightness = s.brightness >= 255 ? 60 : min(255, s.brightness + 65); applyDisplaySettings(); break;
                case 8: setScreen(SCR_CALIB); return;
                case 9: det::captureBaseline(); break;
                case 10: setScreen(SCR_ALLOW); return;
                case 11: setScreen(SCR_ABOUT); return;
                case 12: s.gpsOn = !s.gpsOn; if (s.gpsOn) gps::begin(s.gpsBaud); else gps::end(); break;
                case 13: s.gpsBaud = s.gpsBaud == 9600 ? 38400 : s.gpsBaud == 38400 ? 115200 : 9600;
                         if (s.gpsOn) { gps::end(); gps::begin(s.gpsBaud); } break;
                default: det::clearLog(); storage::clearLogFile(); toast("Log cleared", 1200); break;
            }
            storage::saveSettings();
            dirty = true;
            break;
        }
        case SCR_ALLOW: {
            if (inRect(p, 2, TOP + 2 + (ROWS - 1) * ROW_H, 90, ROW_H - 2)) { sound(SND_TAP); setScreen(SCR_SETUP); return; }
            if (p.x >= SCREEN_W - 36) { sound(SND_TAP); if (p.y < TOP + CONT_H / 2) listTop = max(0, listTop - (ROWS - 1)); else listTop += ROWS - 1; dirty = true; return; }
            int row = (p.y - TOP - 2) / ROW_H;
            if (row >= 0 && row < ROWS - 1 && listTop + row < (int)det::allowList.size()) {
                sound(SND_TAP); det::unallow(listTop + row); storage::saveAllow(); toast("Removed", 1000); dirty = true;
            }
            break;
        }
        case SCR_ABOUT: sound(SND_TAP); setScreen(SCR_SETUP); break;
        default: break;
    }
}

} // namespace ui
