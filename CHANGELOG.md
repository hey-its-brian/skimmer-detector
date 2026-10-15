# Changelog

## cyd-1.0.0 (2026-10-15)

CYD edition in `cyd/`: the same detector on an ESP32-2432S028R (2-USB) with a touch UI, an animated capybara, and the hit log on the microSD. See `cyd/README.md`. Merged image `firmware/skimmer-detector-cyd-1.0.0-cyd2usb-full.bin`, offset `0x0`, ESP32 4 MB. Untested on hardware at the time of this commit: display colours, touch calibration and the SD mount are the three things to check first.

## v3.0.0 (2026-10-15)

First release on the PCB v1 carrier board.

## Simple mode (default)

One screen, four states, no menus: READY, WAIT, SAFE, STOP. Size 3 status word, one size 2 instruction line.

- Any front button starts a check. A press registers on press-down after 120 ms; hold length is ignored.
- 10 second check window with a fill bar. STOP can fire from 2 s in on a strong signature. An ambiguous result runs one more window before committing.
- SAFE: one long buzz. STOP: inverted top band, three short buzzes, then quiet. Results hold 60 s, then READY. Deep sleep after 2 min idle; red or blue wakes it.
- No baseline step. Each completed check becomes the comparison for the next.
- STOP requires a module name, UART service, vendor OUI, or handshake, or a device that is both very close and got markedly stronger since the previous check. Proximity alone never trips it.
- Nothing can be allow-listed from this mode.

## Maintenance mode

Hold BOOT (back) for 3 s to toggle the previous full UI: All / BLE / WiFi / Log / Allow pages, manual baseline, probe, allow list. Exits after 60 s idle.

## Under the hood since v2

- BLE scan runs continuously via NimBLE callbacks; WiFi scan is async. The main loop never blocks on the radio.
- Button polling runs in its own FreeRTOS task.
- One-shot haptic patterns replace continuous buzzing.
- Panel contrast set to max; frame nudged 1 px every 30 s against burn-in.
- Wake press is confirmed by a 60 ms buzz before the display initialises.

## Flashing

`skimmer-detector-v3.0.0-xiao-c6-full.bin` is a merged image: write it at offset `0x0` with esptool or https://espressif.github.io/esptool-js/ (ESP32-C6, 4 MB). Or build from source with PlatformIO (`pio run -t upload`).

## Hardware

Seeed XIAO ESP32-C6 on the PCB v1 carrier (see `hardware/pcb-v1/`), 0.96" SSD1306 OLED, four panel-mount buttons, coin vibration motor through the on-board NPN driver (both alert jumpers at 2-3), LiPo on JST-PH.
