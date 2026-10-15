---
type: project
area: Electronics
topic: skimmer-detector
tags:
  - esp32
  - cyd
  - skimmer-detector
  - touchscreen
  - lovyangfx
created: 2026-10-15
github: https://github.com/hey-its-brian/skimmer-detector/tree/cyd/cyd
---

# Skimmer Detector: CYD edition

The [[Skimmer Detector]] detector on the 2-USB [[Cheap Yellow Display]] (ESP32-2432S028R). Touch UI, an animated capybara that sniffs for you, and the hit log on the microSD. Nothing to wire: display, touch, SD, RGB LED, speaker and LDR are all on the board.

**Status:** firmware written and compiled 2026-10-15 (branch `cyd` in the repo, folder `cyd/`). Not yet flashed to hardware. First things to check on the bench: panel colours (Invert and R/B swap buttons on Setup), touch calibration (Setup > Calibrate touch), SD mount (status bar shows `SD` in green when mounted).

## Hardware

- ESP32-2432S028R, 2-USB revision (USB-C + micro-USB). ESP32-WROOM-32, 4 MB flash, no PSRAM. Panel is ST7789V on most units, ILI9342 on recent AliExpress batches (build with `-DPANEL_ILI9342=1` for those).
- microSD, FAT32, optional. Log, checks and allow list go on it.
- USB-A to C or micro-USB for power and flashing. The USB-C port has no CC resistors, C-to-C cables do nothing.
- Free headers: CN1 (I2C) and P3 are unused, so sensors or a haptic driver could still go there later.

## Pins the firmware uses

All fixed by the board (`cyd/src/board.h`):

| Function | Pins |
|---|---|
| Display SPI (HSPI) | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, backlight 21 (PWM) |
| Touch XPT2046 (bit-banged) | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| microSD (VSPI) | SCK 18, MISO 19, MOSI 23, CS 5 |
| RGB LED (active low) | R 4, G 16, B 17 |
| Speaker amp | 26 (LEDC tone) |
| Light sensor | 34 (ADC) |

Touch is bit-banged on purpose: LovyanGFX's touch driver wants a hardware SPI host, and both hosts are taken (display on HSPI, SD on VSPI).

## Build and flash

```
cd skimmer-detector/cyd
pio run -t upload
```

Or write `firmware/skimmer-detector-cyd-1.0.0-cyd2usb-full.bin` at offset `0x0` (ESP32, 4 MB) with esptool or esptool-js. Partition table `huge_app` (3 MB app, no OTA). Build was 56 KB static RAM, 1.24 MB flash.

Libraries: LovyanGFX 1.2.x (display only), NimBLE-Arduino 2.5.x, Arduino SD. Platform `espressif32@6.9` (Arduino core 2.0.17).

## Controls

| Where | Action |
|---|---|
| HOME | Tap the capybara or CHECK to start a 10 s check. SAFE shows AGAIN, STOP shows WHY? (opens Devices) |
| DEVICES | Scored devices sorted by score. Tap a row for detail: id, RSSI, score reasons, PROBE (BLE handshake), ALLOW, BACK. Arrows on the right scroll |
| LOG | Alerts (score 60+), newest first. CLEAR wipes RAM and `/skimlog.csv` |
| SETUP | Sound, LED, auto-dim, sleep timer (never/1/3/10 min), invert, R/B swap, flip screen, brightness, calibrate touch (3 crosshairs), baseline now, allow list, about |

Capybara states mirror the detector: idle (breathes, blinks), sniff (nose wiggles, radar rings), safe (orange on the head, sparkles), stop (hackles up, wide eyes, exclamation marks), sleep after 45 s idle (z z z). Speech bubble lines are at the top of `cyd/src/ui.cpp`.

Deep sleep after the sleep timer (not during a check or while STOP is showing); any touch wakes it via PENIRQ (GPIO36, ext0). Verdict feedback: SAFE chime + green LED 5 s + green flash; STOP three low beeps + red blink + red flash.

## SD files

- `/skimlog.csv`: `boot,uptime_s,kind,id,name,score,rssi,factors` per alert
- `/checks.csv`: `boot,uptime_s,verdict,device,hits` per completed check
- `/allow.txt`: one id per line, wins over the NVS copy when the card is in

`boot` is a counter in NVS so rows from different power cycles can be separated. No RTC, so times are uptime.

## Art pipeline

Frames are 64x48 indexed PNGs in `cyd/art/`, drawn at 3x on screen. `tools/make_capy.py` draws the stock set with PIL; `tools/frames2header.py` packs `art/<state>_<n>.png` into `src/capy_frames.h`; `tools/mockup.py` renders `art/_ui_preview.png`.

To drop in a Blender or ArtCraft character: render each frame to 64x48 PNG with transparent background (alpha or pure magenta), name them `idle_0.png`, `idle_1.png`, `idle_2.png`, `sniff_0..2`, `safe_0..1`, `stop_0..1`, `sleep_0..1`, run `frames2header.py`, rebuild. Colours snap to a 16-entry palette (entry 0 transparent) defined in both scripts; a smooth-shaded render will posterise, so paint flat or edit the palette.

Rendering: the left pane is a 200x180 4-bit palette sprite (18 KB) so the scene redraws without flicker; frame pixels are run-length filled as 3x3 blocks. The right pane and the other tabs draw straight to the panel and only on change.

## What is different from the XIAO build

- Touch instead of four buttons; no simple/maintenance split, the full device list is one tab away.
- RAM log holds 60 entries (XIAO: 24 in NVS) plus the CSV on the card.
- Still no Bluetooth Classic (HC-05/06). The WROOM has the radio but BLE + BT Classic + WiFi overflowed IRAM in the earlier M5StickC attempt; separate experiment if wanted.
- Detection logic (names, FFE0/FFF0/FFE5 UART service, OUIs, proximity, RSSI jump vs previous check, P/M handshake probe, STOP rules) is a straight port of `src/main.cpp` v3.

## Ideas for later

- Bluetooth Classic scan with the WiFi stack stopped during the BT window (time-slice the radios instead of running all three).
- Haptic motor on P3 or CN1 for a silent STOP.
- Swipe scrolling on the lists once touch calibration is confirmed solid.
- A 3D-printed bumper case with a cut-out that puts the WROOM antenna edge against the card slot.
