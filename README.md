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

## Using it (simple mode, the default)

One screen, four states, no menus. Built for someone who will never read this file.

| Screen | What it means | What to do |
|---|---|---|
| **READY** / On the slot | Awake and listening | Hold it on the card slot, press the red button |
| **WAIT** / Hold still | Checking, 10 second fill bar | Keep it on the slot |
| **SAFE** / Go ahead | Nothing suspicious heard. One long buzz. | Use the card |
| **STOP** / Pay inside | Skimmer signature heard. Bright inverted band, three short buzzes. | Tap, pay inside, or cash. Call Brian. |

- Every front button does the same thing: start a check. A press counts after 120 ms of contact; hold length is ignored.
- Only the **red** and **blue** buttons can wake it from sleep (the C6 only wakes on GPIO 0 to 7). Teach the red one.
- A result stays on screen for 60 seconds, then it goes back to READY. It sleeps after 2 minutes without a press.
- There is no baseline step. Each completed check becomes the comparison for the next one, so pressing once in the car and again at the pump gives the "got stronger" bonus for free. A single check at the slot is signature-only, which is what catches an HM-10 anyway.
- STOP requires a real signature (module name, UART service, vendor OUI, or a successful handshake), or a device that is both very close and got markedly stronger since the previous check. Proximity alone never produces STOP.
- Nothing can be allow-listed from this mode. The owner does that in maintenance mode.

Back label: `Hold on card slot. Press RED. 1 buzz = SAFE. 3 = STOP, pay another way.`

What to say when handing it over: "Hold it on the slot where the card goes in. Press the red button. One long buzz and SAFE means go ahead. Three short buzzes and STOP means tap, pay inside, or cash, and call me. It sometimes says STOP at a store that's fine. It can't catch every kind of skimmer, so still cover the keypad and check your statement. If it doesn't light up when you press red, it needs charging, and no answer means pay another way."

## Maintenance mode (hold BOOT on the back for 3 seconds)

The full UI with pages, scores, and RSSI. Hold BOOT again to leave; it also exits on its own after 60 seconds without a press.

| Button | Short press | Long press (1s) |
|--------|-------------|-----------------|
| RED (MODE) | Next page (All, BLE, WiFi, Log, Allow) | Capture a manual baseline |
| BLUE (UP) | Cursor up | Jump to top |
| GREEN (DOWN) | Cursor down | Clear list (on Log page: clear log) |
| YELLOW (OK) | Probe selected BLE device | Add selected to allow list (on Allow page: remove) |
| BOOT | Acknowledge alert (else next page) | Clear list (on Log page: clear log) |

To tune a unit for someone's regular stores: enter maintenance mode at each store, select the POS gear that shows up, hold yellow to allow-list it. The allow list and the hit log (last 24 hits at 60+) are kept in flash. Serial at 115200 prints `[CHECK]`, `[LOG]`, `[PROBE]` and `[MODE]` lines.

## Build and flash

PlatformIO:

```
pio run -t upload
pio device monitor -b 115200
```

`platformio.ini` uses the pioarduino platform fork (the C6 needs Arduino core 3.x) and the `huge_app` partition table; BLE + WiFi + display does not fit the default app slot.

Boot output from a healthy unit starts with `[OLED] found 0x3C  SDA=GPIO22 SCL=GPIO23` and `[OLED] init OK`. If it says `not found`, the display is not on the bus: check power, header joints, and anything else sharing D4/D5.

## Scoring (shared by both modes)

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
