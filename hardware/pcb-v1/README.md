# Skimmer Detector PCB v1

Carrier board for the XIAO ESP32-C6 skimmer detector. 38 x 46 mm, 2 layers, rounded corners, 4x M2 corner holes.

- **Front:** 0.96" OLED on its header, wire pads for the four panel-mount buttons, "SKIMMER DETECTOR" branding with a radar icon, power switch labels.
- **Back:** XIAO on pin headers (USB-C flush with the top edge, parts facing out), JST-PH LiPo jack, slide power switch (lever hangs off the bottom edge), alert output pads, optional transistor driver, "SKIM-DET v1.0" branding.

Files: `SkimmerDetector_Gerbers_PCBWay_v1.zip` (upload this), `skimmer-detector.kicad_pcb` (opens in KiCad 7+), `SkimmerDetector_Render_v1.png`.

## Power

JST-PH (BAT_RAW) feeds the slide switch (BAT_SW), then the J4 pads, then short wires to the XIAO's underside BAT+ / BAT- pads. The XIAO's own regulator makes 3.3V for the OLED and everything else. The board has no 5V source: the XIAO's 5V pin is only its USB VBUS, so that net is dead on battery and nothing on the board uses it. USB-C charges the LiPo through the XIAO's onboard charger.

## PCBWay order settings

| Setting | Value |
|---|---|
| Size | 38 x 46 mm |
| Layers | 2 |
| Thickness | 1.6 mm |
| Min track / spacing | 6/6 mil (design uses 0.3 mm tracks, 0.2 mm clearance) |
| Min hole | 0.3 mm |
| Solder mask | Matte black recommended (renders show this). Green is cheapest. |
| Silkscreen | White |
| Surface finish | ENIG for the gold look, or HASL lead-free to save money |
| Quantity | 5 (minimum) |

Everything else stays at the defaults (no castellated holes, no V-score, no stencil needed).

## BOM

| Ref | Part | Notes |
|---|---|---|
| U1 | Seeed XIAO ESP32-C6 | Back side, on 2x 1x7 male headers |
| U2 | 0.96" SSD1306 I2C OLED, 4-pin GND VCC SCL SDA | Front side, on its own header |
| J1 | JST-PH 2-pin vertical (B2B-PH-K-S) | LiPo connector, back |
| SW5 | SS12D00G4 SPDT slide switch, 2.54 mm pitch | Back, lever out the bottom edge |
| J3 | 2-pin 2.54 mm header or two wires | Alert output (buzzer or motor) |
| J4 | Two short wires (26 to 28 AWG) | Switched battery to the XIAO BAT+ / BAT- pads underneath |
| J5 to J8 | Panel-mount buttons, wires to pads | Square pad = signal, round pad = GND |
| JP1, JP2 | Solder bridges | Alert mode select, see below |
| Q1 | MMBT2222A or S8050, SOT-23 | Optional, only for a magnetic buzzer or motor |
| R1 | 1k, 0805 | Optional, with Q1 |
| D1 | 1N4148W, SOD-123 | Optional, with Q1 (flyback) |
| H1 to H4 | M2 screws | Case mounting, 2.2 mm holes |
| H5 | M2 standoffs | Optional OLED support, 2.5 mm holes for slack |

## Button wiring

| Pads | Button | Color |
|---|---|---|
| MODE | D1 | Red |
| UP | D2 | Blue |
| DOWN | D3 | Green |
| OK | D6 | Yellow |

The front silk shows the function under each pad pair, and the back silk shows the color. Either button wire can go on either pad; polarity doesn't matter for a switch.

## Alert mode jumpers (back)

Bridge with a solder blob. Both jumpers ship open, so the alert does nothing until you choose a mode.

| Your part | JP1 | JP2 | Fit Q1/R1/D1? |
|---|---|---|---|
| Piezo (over ~100 ohms) | bridge 1-2 | bridge 1-2 | No |
| Magnetic buzzer or vibration motor | bridge 2-3 | bridge 2-3 | Yes |

The alert device wires to the ALERT pads (+ is the square pad).

## Assembly order

Order matters because the XIAO's header joints end up underneath the OLED.

1. **SMD parts first** (only if you're using the driver): R1, Q1, D1 on the back.
2. **XIAO headers:** push the short end of the two 1x7 male headers in from the **back**, plastic spacer on the back side. Solder them on the front, then **trim the front-side tails flush**. Those tails sit under the OLED.
3. **Battery wires:** solder two short wires to the XIAO's underside BAT+ / BAT- pads before mounting it, since you can't reach those pads afterwards.
4. **XIAO:** drop it onto the header pins, parts facing out, USB-C toward the top edge, and solder on top. Then solder the two battery wires to the J4 pads (square = BAT+).
5. **OLED:** fit it on the front with its header and solder from the back. Trim the back-side tails flush, because they're under the XIAO. A piece of kapton tape on the board under each module is cheap insurance.
6. **Switch, JST, alert header** on the back.
7. **Jumpers:** pick an alert mode (table above).
8. **Buttons:** wire each one to its pad pair once they're mounted in the case.

Check the LiPo polarity before plugging in. The silkscreen marks + and -, and JST-PH batteries don't all use the same wire order.

## Bring-up notes (first boards, 2026-10-14)

- The header pins' trimmed tails sit under the other module, and one of those joints was the cause of a dark OLED on the first assembled board. If the display is dark, reflow the OLED header from the back and the XIAO header from the front before suspecting anything else.
- Confirm the display is on the bus with a bit-banged I2C scan (the C6 hardware I2C driver gives false hits). The firmware does this at boot and prints `[OLED] found 0x3C`.
- Make sure the board is running the PCB firmware in `src/main.cpp`. The early v1 build (prints `[BLE]` lines on serial) predates the four front buttons and ignores them.

## Firmware changes for the PCB

- **OLED pins auto-detected.** The PCB follows the XIAO's standard I2C pins (SDA = D4, SCL = D5), while the hand-wired build ended up swapped. The firmware now probes both orientations at boot, so the same image runs on both.
- **BOOT is on the back now,** so its jobs moved to the front buttons:
  - Any button acknowledges an alert. OK still probes the alerting device instead.
  - Hold DOWN (green) to clear the list. On the Log page it clears the log.
  - BOOT still works for both.

## Design notes

- **Antenna clearance.** Copper pour is kept out from under the XIAO's antenna end on both layers.
- **Hole positions are best guesses.** The OLED support holes (H5) assume the common 27.3 x 27.8 mm module with holes 2 mm in from each edge. Measure yours before relying on them; they're drilled 2.5 mm to allow some slack. The board works without them, since the header holds the module.
- **Switch fit.** The switch footprint is for the SS12D00G4, 2.54 mm pitch, with 0.9 mm holes for some slack.
- **How it was made.** The board was generated with the KiCad 7 Python API and autorouted with Freerouting. DRC is clean: no clearance errors, no unconnected nets.
- **No schematic.** There's no separate schematic sheet; the nets are defined in the generator script and the tables above.
