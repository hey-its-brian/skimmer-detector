#!/usr/bin/env python3
"""Draws approximate previews of the CYD screens (same layout as ui.cpp) to
cyd/art/_ui_preview.png so the look can be reviewed without hardware."""
import os
from PIL import Image, ImageDraw, ImageFont
from make_capy import capy, PAL

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "art", "_ui_preview.png")
W, H = 320, 240
TOP, TABY, PANEX = 18, 198, 200
BG, PANEL, PANEL2 = (16, 18, 26), (30, 34, 46), (44, 48, 64)
TEXT, MUTED, ACC = (236, 230, 218), (140, 142, 156), (246, 142, 32)
GREEN, RED, BLUE, YEL, LINE = (70, 200, 110), (232, 60, 48), (90, 160, 240), (250, 222, 84), (60, 64, 84)

def font(sz):
    for f in ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"):
        if os.path.exists(f): return ImageFont.truetype(f, sz)
    return ImageFont.load_default()
F12, F14, F26 = font(12), font(14), font(26)

def chrome(d, tab):
    d.rectangle((0, 0, W, TOP), fill=PANEL)
    d.text((6, 3), "B", fill=BLUE, font=F12); d.text((18, 3), "12", fill=TEXT, font=F12)
    d.text((48, 3), "W", fill=ACC, font=F12); d.text((62, 3), "5", fill=TEXT, font=F12)
    d.text((132, 3), "SKIM-DET", fill=MUTED, font=F12)
    d.text((W - 50, 3), "SD", fill=GREEN, font=F12); d.text((W - 36, 3), "2:14", fill=MUTED, font=F12)
    names = ["HOME", "DEVICES", "LOG", "SETUP"]
    for i, n in enumerate(names):
        on = i == tab
        d.rectangle((i * 80, TABY, i * 80 + 80, H), fill=PANEL2 if on else PANEL)
        if on: d.rectangle((i * 80, TABY, i * 80 + 80, TABY + 3), fill=ACC)
        tw = d.textlength(n, font=F12)
        d.text((i * 80 + 40 - tw / 2, TABY + 14), n, fill=TEXT if on else MUTED, font=F12)
    d.line((0, TABY, W, TABY), fill=LINE)

def button(d, x, y, w, h, label, fill, tc, f=F14):
    d.rounded_rectangle((x, y, x + w, y + h), 8, fill=fill, outline=LINE)
    tw = d.textlength(label, font=f)
    d.text((x + w / 2 - tw / 2, y + h / 2 - 8), label, fill=tc, font=f)

def home(state):
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    chrome(d, 0)
    # scene
    d.rectangle((0, TOP + 158, PANEX, TABY), fill=PAL[10]); d.rectangle((0, TOP + 158, PANEX, TOP + 160), fill=PAL[3])
    d.ellipse((4, TOP + 159, 64, TOP + 173), fill=PAL[13]); d.ellipse((12, TOP + 162, 56, TOP + 170), fill=PAL[7]); d.ellipse((20, TOP + 163, 48, TOP + 169), fill=PAL[13])
    cstate = {"READY": "idle", "SNIFF": "sniff", "SAFE": "safe", "STOP": "stop", "SLEEP": "sleep"}[state]
    if state == "SNIFF":
        cx, cy = 4 + 57 * 3, TOP + 26 + 23 * 3
        for r, c in ((24, YEL), (54, ACC), (84, ACC)): d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=c)
    if state == "STOP":
        d.text((26, TOP + 28), "!", fill=RED, font=F26); d.text((154, TOP + 18), "!", fill=RED, font=F26)
    fr = capy(cstate, 0).convert("RGBA")
    px = fr.load()
    for y in range(48):
        for x in range(64):
            r, g, b, a = px[x, y]
            if (r, g, b) != (255, 0, 255): d.rectangle((4 + x * 3, TOP + 26 + y * 3, 4 + x * 3 + 2, TOP + 26 + y * 3 + 2), fill=(r, g, b))
    lines = {"READY": "Hold me on the card slot", "SNIFF": "Sniff sniff...", "SAFE": "All clear, go ahead", "STOP": "NOPE. Pay inside!", "SLEEP": "z z z"}
    t = lines[state]; tw = d.textlength(t, font=F12) + 16
    bx = PANEX - tw - 4
    d.rounded_rectangle((bx, TOP + 2, bx + tw, TOP + 26), 8, fill=PAL[7])
    d.polygon([(bx + tw - 36, TOP + 25), (bx + tw - 22, TOP + 25), (bx + tw - 26, TOP + 33)], fill=PAL[7])
    d.text((bx + 8, TOP + 6), t, fill=PAL[6], font=F12)
    if state == "SLEEP":
        d.text((150, TOP + 50), "z", fill=PAL[13], font=F12); d.text((166, TOP + 36), "Z", fill=PAL[13], font=F14)
    # pane
    d.rectangle((PANEX, TOP, W, TABY), fill=PANEL); d.line((PANEX, TOP, PANEX, TABY), fill=LINE)
    word, col, sub, btn = {"READY": ("READY", TEXT, "On the slot", "CHECK"), "SNIFF": ("SNIFF", YEL, "Hold still", None),
                           "SAFE": ("SAFE", GREEN, "Go ahead", "AGAIN"), "STOP": ("STOP", RED, "Pay inside", "WHY?"),
                           "SLEEP": ("READY", TEXT, "On the slot", "CHECK")}[state]
    cx = PANEX + 60
    tw = d.textlength(word, font=F26); d.text((cx - tw / 2, TOP + 18), word, fill=col, font=F26)
    tw = d.textlength(sub, font=F12); d.text((cx - tw / 2, TOP + 56), sub, fill=MUTED, font=F12)
    third = {"STOP": ("HMSoft", RED), "SNIFF": ("window 1/2", MUTED)}.get(state, ("no suspects", MUTED))
    tw = d.textlength(third[0], font=F12); d.text((cx - tw / 2, TOP + 77), third[0], fill=third[1], font=F12)
    if btn: button(d, PANEX + 8, TOP + 112, 104, 56, btn, RED if state == "STOP" else ACC, TEXT if state == "STOP" else (30, 20, 10), F26)
    else:
        d.rounded_rectangle((PANEX + 8, TOP + 112, W - 8, TOP + 168), 8, outline=LINE)
        d.rounded_rectangle((PANEX + 16, TOP + 130, W - 16, TOP + 150), 4, outline=LINE); d.rectangle((PANEX + 18, TOP + 132, PANEX + 60, TOP + 148), fill=YEL)
    return im

def devices():
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    chrome(d, 1)
    button(d, W - 34, TOP + 2, 32, 87, "^", PANEL, TEXT); button(d, W - 34, TOP + 91, 32, 87, "v", PANEL, TEXT)
    rows = [("B", "HMSoft !", 100, 4, RED), ("W", "ESP_3A2F1C", 80, 3, RED), ("B", "?A1:B2:C3", 45, 3, YEL), ("W", "<hidden>:9E", 35, 2, YEL)]
    for i, (k, n, sc, b, c) in enumerate(rows):
        y = TOP + 2 + i * 28
        d.rounded_rectangle((2, y, W - 38, y + 26), 5, fill=PANEL)
        kc = BLUE if k == "B" else ACC
        d.rounded_rectangle((6, y + 5, 28, y + 21), 4, fill=kc); d.text((13, y + 5), k, fill=BG, font=F12)
        d.text((34, y + 6), n, fill=TEXT, font=F12)
        d.rounded_rectangle((W - 110, y + 5, W - 74, y + 21), 4, fill=c); tw = d.textlength(str(sc), font=F12); d.text((W - 92 - tw / 2, y + 5), str(sc), fill=BG, font=F12)
        for j in range(4):
            h = 3 + j * 3
            if j < b: d.rectangle((W - 68 + j * 5, y + 18 - h, W - 65 + j * 5, y + 18), fill=TEXT)
            else: d.rectangle((W - 68 + j * 5, y + 18 - h, W - 65 + j * 5, y + 18), outline=MUTED)
    return im

def setup():
    im = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(im)
    chrome(d, 3)
    labels = ["Sound: on", "LED: on", "Auto dim: on", "Sleep: 3 min", "Invert: off", "R/B swap: off", "Flip screen", "Bright: 78%", "Calibrate touch", "Baseline now", "Allowed (2)", "About"]
    x0 = (W - 3 * 106) // 2 + 3
    for i, l in enumerate(labels):
        c, r = i % 3, i // 3
        fill = ACC if i == 9 else PANEL2 if i in (8, 10, 11) else PANEL
        button(d, x0 + c * 106, TOP + 6 + r * 44, 100, 40, l, fill, BG if i == 9 else TEXT, F12)
    return im

if __name__ == "__main__":
    shots = [home("READY"), home("SNIFF"), home("SAFE"), home("STOP"), home("SLEEP"), devices(), setup()]
    cols = 2
    sheet = Image.new("RGB", (W * cols + 10 * (cols + 1), (H + 10) * ((len(shots) + 1) // 2) + 10), (60, 60, 70))
    for i, s in enumerate(shots):
        sheet.paste(s, (10 + (i % cols) * (W + 10), 10 + (i // cols) * (H + 10)))
    sheet.save(OUT)
    print("wrote", OUT)
