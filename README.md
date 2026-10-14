# Skimmer Detector

Pocket BLE/WiFi sniffer that flags the radio modules found inside card skimmers and shimmers. Take a baseline a few feet away, then hold it next to the terminal for a few seconds before you insert your card.

Built on a Seeed XIAO ESP32-C6 with a 0.96" SSD1306 OLED, four front buttons, and a custom carrier PCB (see `hardware/pcb-v1/`).

## Parts

- Seeed XIAO ESP32-C6
- 0.96" SSD1306 128x64 I2C OLED (4 pin)
- 4 panel-mount buttons (red, blue, green, yellow)
- Coin vibration motor (discreet) or 3V active buzzer
- 3.7V LiPo on the BAT pads, slide switch in the battery + lead
- PCB v1 carrier board, or hand-wire it per the table below

## Wiring

The PCB routes everything for you. For a hand-wired build:

| Part | XIAO C6 pin | GPIO |
|------|-------------|------|
| OLED VCC | 3V3 | |
| OLED GND | GND | |
| OLED SDA | D4 | 22 |
| OLED SCL | D5 | 23 |
| Haptic / buzzer + | D0 | 0 |
| MODE button (red) | D1 to GND | 1 |
| UP button (blue) | D2 to GND | 2 |
| DOWN button (green) | D3 to GND | 21 |
| OK button (yellow) | D6 to GND | 16 |

SDA/SCL orientation is auto-detected at boot, so swapping D4 and D5 still works. Buttons need no resistors; internal pull-ups are used. MODE is on GPIO1 on purpose: the C6 can only wake from deep sleep on GPIO0 to 7. The onboard BOOT button and LED are used as-is.

Measure the alert device before wiring it. A piezo (over ~100 ohms) can go straight to D0. A magnetic buzzer or vibration motor (under ~100 ohms) needs an NPN driver (2N2222 or S8050, 1k base resistor, flyback diode); the PCB has pads for this. A low-resistance buzzer wired straight to a GPIO can drag the I2C lines low and blank the display.

## Controls

| Button | Short press | Long press (1s) |
|--------|-------------|-----------------|
| MODE (red) | Next page (All, BLE, WiFi, Log, Allow) | Capture baseline |
| UP (blue) | Cursor up | Jump to top |
| DOWN (green) | Cursor down | Clear list (on Log page: clear log) |
| OK (yellow) | Probe selected BLE device | Add selected to allow list (on Allow page: remove) |
| BOOT (on back) | Acknowledge alert (else next page) | Clear list (on Log page: clear log) |

Any front button acknowledges an alert (OK probes the alerting device instead). Any press keeps it awake. After 2 minutes idle with no alert it deep sleeps; press MODE to wake.

## Recommended routine

1. Power on. Standing a few feet from the register, hold MODE for a second. It scans for 5s and stores every device it can hear as the baseline. A `*` appears in the header.
2. Walk up and hold the unit within a few inches of the card slot and PIN pad for 5 to 10 seconds.
3. Anything that got 15 dBm stronger than baseline, or is new and strong, gets flagged. Anything that advertises the HM-10 style UART service is probed automatically: the unit connects, sends `P`, and if it gets `M` back the screen says SKIMMER and the score goes to 100.
4. ALERT flashes the screen, LED, and haptic. Any button acknowledges. OK probes the alerting device manually if you want confirmation.
5. If a store's own POS gear keeps tripping it, select it and hold OK. It is saved to flash and ignored from then on. Manage the list on the Allow page.
6. Hits at 60+ are logged with uptime, MAC, name, and score (last 24, saved to flash). Serial at 115200 prints full details.

## Build and flash

PlatformIO:

```
pio run -t upload
pio device monitor -b 115200
```

`platformio.ini` uses the pioarduino platform fork (the C6 needs Arduino core 3.x) and the `huge_app` partition table; BLE + WiFi + display does not fit the default app slot.

Boot output from a healthy unit starts with `[OLED] found 0x3C  SDA=GPIO22 SCL=GPIO23` and `[OLED] init OK`. If it says `not found`, the display is not on the bus: check power, header joints, and anything else sharing D4/D5.

## Scoring

BLE:
- Known module name (HM-10, HC-08, JDY-xx, CC41, AT-09, BT05, ...): +80
- FFE0 / FFF0 / FFE5 UART bridge service: +40
- TI CC254x or Espressif OUI: +25
- Unnamed, not a phone/watch/earbud vendor, RSSI at or above -50: +45 (-60: +30)
- New strong device not in baseline: +20; RSSI jumped 15 dBm over baseline: +30
- Seen 3+ windows in a row: +10
- P/M handshake succeeded: score forced to 100

WiFi:
- Default ESP/AI-Thinker SSID: +80
- Espressif OUI: +25
- Open AP at or above -50: +40; hidden AP at or above -50: +35
- Baseline bonuses as above

Alert at 60, listed at 30. All thresholds and lists are at the top of `src/main.cpp`.

## What it cannot detect

- Bluetooth Classic modules (HC-05, HC-06). The C6 is BLE only.
- Skimmers with no radio (pure storage, thief returns for it).
- Cellular skimmers.

## Hardware

`hardware/pcb-v1/` holds the carrier board: KiCad file, Gerbers for PCBWay, render, and the build brief with BOM, order settings, assembly order, and jumper settings.
