#!/usr/bin/env python3
"""Draws the default capybara frames as 64x48 indexed PNGs into cyd/art/.

These are the stock frames. Replace any of them with your own 64x48 PNG
(Blender render, ArtCraft, Aseprite...) and run tools/frames2header.py to
rebuild src/capy_frames.h. Transparent = alpha 0 or pure magenta (255,0,255).

Frame naming: <state>_<n>.png   states: idle, sniff, safe, stop, sleep
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "..", "art")
os.makedirs(ART, exist_ok=True)

W, H = 64, 48
PAL = [
    (255, 0, 255),   # 0 transparent
    (46, 26, 16),    # 1 outline
    (156, 100, 54),  # 2 body
    (190, 134, 80),  # 3 body light
    (118, 72, 38),   # 4 body shade
    (222, 186, 136), # 5 snout / belly cream
    (22, 18, 18),    # 6 black
    (255, 255, 255), # 7 white
    (246, 142, 32),  # 8 orange
    (204, 104, 22),  # 9 orange dark
    (76, 156, 62),   # 10 leaf green
    (232, 148, 148), # 11 pink
    (226, 52, 44),   # 12 red
    (124, 184, 240), # 13 sky blue
    (250, 222, 84),  # 14 yellow
    (96, 96, 96),    # 15 grey
]
T, OUT, BODY, LITE, SHADE, CREAM, BLK, WHT, ORG, ORGD, LEAF, PINK, RED, SKY, YEL, GRY = range(16)


def new():
    im = Image.new("P", (W, H), T)
    flat = []
    for c in PAL:
        flat += c
    im.putpalette(flat + [0, 0, 0] * (256 - len(PAL)))
    return im


def outline(im):
    """Any body pixel touching transparency becomes outline."""
    px = im.load()
    src = {(x, y): px[x, y] for x in range(W) for y in range(H)}
    for (x, y), v in src.items():
        if v == T or v in (OUT,):
            continue
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if nx < 0 or ny < 0 or nx >= W or ny >= H or src[(nx, ny)] == T:
                px[x, y] = OUT
                break


def capy(state="idle", frame=0):
    im = new()
    d = ImageDraw.Draw(im)
    breath = 1 if (state == "idle" and frame == 1) else 0
    ox = (1 if frame % 2 else -1) if state == "stop" else 0
    headdy = {"sniff": [-2, -1, 1][frame % 3], "sleep": 3}.get(state, 0)
    hy = 10 + headdy                         # head top
    by = 17 - breath                         # body top

    # legs first, the body overlaps their tops
    for lx in (9, 18, 33, 42):
        d.rectangle((lx + ox, 37, lx + 5 + ox, 45), fill=SHADE)
        d.rectangle((lx + ox, 44, lx + 5 + ox, 45), fill=CREAM)      # toes
    # ears before the head so the head hides their roots
    for ex in (35, 47):
        d.ellipse((ex + ox, hy - 4, ex + 6 + ox, hy + 4), fill=BODY)
        d.ellipse((ex + 2 + ox, hy - 2, ex + 4 + ox, hy + 1), fill=PINK)
    # body
    d.ellipse((3 + ox, by, 49 + ox, 41), fill=BODY)
    d.ellipse((12 + ox, 27, 44 + ox, 40), fill=CREAM)                # belly
    d.ellipse((9 + ox, by + 2, 31 + ox, by + 9), fill=LITE)          # back highlight
    if state == "stop":                                              # hackles up
        for sx in (12, 18, 24, 30):
            d.polygon([(sx + ox, by + 1), (sx + 2 + ox, by - 4), (sx + 4 + ox, by + 1)], fill=BODY)
    # head: one rounded block that merges into the body, boxy muzzle in front
    d.rounded_rectangle((30 + ox, hy, 60 + ox, hy + 25), radius=10, fill=BODY)
    d.rounded_rectangle((46 + ox, hy + 9, 60 + ox, hy + 24), radius=4, fill=CREAM)   # muzzle
    d.rounded_rectangle((34 + ox, hy + 3, 47 + ox, hy + 8), radius=3, fill=LITE)     # forehead
    # nose and mouth
    nx = 55 + ox
    wide = state == "sniff" and frame % 2 == 1
    for dx in (0, 3):
        d.rectangle((nx + dx, hy + 11, nx + dx + (1 if wide else 0), hy + 11 + (1 if wide else 0)), fill=BLK)
    if state == "stop":
        d.ellipse((50 + ox, hy + 16, 56 + ox, hy + 21), fill=BLK)
    elif state == "safe":
        d.arc((49 + ox, hy + 12, 58 + ox, hy + 20), 20, 160, fill=OUT)
    else:
        d.line((51 + ox, hy + 17, 56 + ox, hy + 17), fill=OUT)
    # eye
    ex, ey = 42 + ox, hy + 11
    if state == "sleep" or (state == "idle" and frame == 2):
        d.line((ex - 1, ey + 1, ex + 2, ey + 1), fill=BLK)
    elif state == "safe":
        d.arc((ex - 2, ey - 1, ex + 3, ey + 3), 180, 360, fill=BLK)
        d.rectangle((ex + 5, ey + 4, ex + 7, ey + 4), fill=PINK)      # blush
    elif state == "stop":
        d.ellipse((ex - 2, ey - 2, ex + 3, ey + 3), fill=WHT)
        d.rectangle((ex, ey, ex + 1, ey + 1), fill=BLK)
    else:
        d.rectangle((ex - 1, ey, ex + 1, ey + 2), fill=BLK)
        d.point((ex - 1, ey), fill=WHT)
    # orange on the head (safe only)
    if state == "safe":
        d.ellipse((37 + ox, 1, 48 + ox, 12), fill=ORG)
        d.rectangle((39 + ox, 3, 40 + ox, 4), fill=WHT)
        d.rectangle((43 + ox, 0, 43 + ox, 1), fill=ORGD)
        d.rounded_rectangle((44 + ox, 0, 48 + ox, 2), radius=1, fill=LEAF)
    outline(im)
    return im


FRAMES = {"idle": 3, "sniff": 3, "safe": 2, "stop": 2, "sleep": 2}

if __name__ == "__main__":
    sheet = Image.new("RGB", (W * 4 * 3, H * 4 * 5), (30, 30, 40))
    row = 0
    for state, n in FRAMES.items():
        for f in range(n):
            im = capy(state, f)
            im.save(os.path.join(ART, f"{state}_{f}.png"))
            big = im.convert("RGB").resize((W * 4, H * 4), Image.NEAREST)
            sheet.paste(big, (f * W * 4, row * H * 4))
        row += 1
    sheet.save(os.path.join(ART, "_sheet_preview.png"))
    print("wrote", sum(FRAMES.values()), "frames to", os.path.abspath(ART))
