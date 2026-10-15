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
    DY = 4  # headroom for the orange and the hackles
    breath = 1 if (state == "idle" and frame == 1) else 0
    shake = (1 if frame % 2 else -1) if state == "stop" else 0
    headdy = {"sniff": [-2, -1, 1][frame % 3], "sleep": 3, "safe": 0}.get(state, 0)
    ox = shake

    # legs (behind body)
    for lx in (12, 21, 33, 42):
        d.rectangle((lx + ox, 35 + DY, lx + 5 + ox, 43 + DY), fill=SHADE)
    # body
    d.ellipse((5 + ox, 15 - breath + DY, 47 + ox, 41 + DY), fill=BODY)
    d.ellipse((12 + ox, 24 + DY, 42 + ox, 40 + DY), fill=CREAM)          # belly
    d.ellipse((8 + ox, 15 - breath + DY, 44 + ox, 32 + DY), fill=BODY)   # cover top of belly
    d.ellipse((10 + ox, 16 - breath + DY, 30 + ox, 24 + DY), fill=LITE)  # back highlight
    if state == "stop":  # hackles up
        for sx in (14, 20, 26, 32):
            d.polygon([(sx + ox, 16 + DY), (sx + 2 + ox, 11 + DY), (sx + 4 + ox, 16 + DY)], fill=BODY)
    # head
    hy = 7 + headdy + DY
    d.rounded_rectangle((29 + ox, hy, 61 + ox, hy + 26), radius=9, fill=BODY)
    d.rounded_rectangle((45 + ox, hy + 9, 61 + ox, hy + 25), radius=5, fill=CREAM)  # snout
    d.rounded_rectangle((33 + ox, hy + 2, 50 + ox, hy + 10), radius=4, fill=LITE)    # forehead
    # ears
    for ex in (34, 46):
        d.ellipse((ex + ox, hy - 4, ex + 6 + ox, hy + 3), fill=BODY)
        d.ellipse((ex + 2 + ox, hy - 2, ex + 4 + ox, hy + 1), fill=PINK)
    # nostrils
    nx = 55 + ox
    if state == "sniff" and frame % 2:
        d.rectangle((nx, hy + 12, nx + 1, hy + 13), fill=BLK)
        d.rectangle((nx + 3, hy + 12, nx + 4, hy + 13), fill=BLK)
    else:
        d.point((nx, hy + 12), fill=BLK); d.point((nx + 3, hy + 12), fill=BLK)
    # mouth
    if state == "stop":
        d.ellipse((50 + ox, hy + 18, 56 + ox, hy + 23), fill=BLK)
    elif state == "safe":
        d.arc((48 + ox, hy + 15, 57 + ox, hy + 22), 10, 170, fill=OUT)
    else:
        d.line((50 + ox, hy + 19, 55 + ox, hy + 19), fill=OUT)
    # eye
    ex, ey = 42 + ox, hy + 10
    if state == "sleep" or (state == "idle" and frame == 2):
        d.line((ex - 1, ey + 1, ex + 2, ey + 1), fill=BLK)
    elif state == "safe":
        d.arc((ex - 2, ey - 1, ex + 3, ey + 3), 180, 360, fill=BLK)
        d.point((ex + 7, ey + 5), fill=PINK); d.point((ex + 8, ey + 5), fill=PINK)
    elif state == "stop":
        d.ellipse((ex - 2, ey - 2, ex + 3, ey + 3), fill=WHT)
        d.rectangle((ex, ey, ex + 1, ey + 1), fill=BLK)
    else:
        d.rectangle((ex - 1, ey, ex + 1, ey + 2), fill=BLK)
        d.point((ex - 1, ey), fill=WHT)
    # orange on the head (safe only)
    if state == "safe":
        d.ellipse((36 + ox, hy - 9, 44 + ox, hy - 1), fill=ORG)
        d.point((38 + ox, hy - 7), fill=WHT)
        d.rectangle((40 + ox, hy - 11, 40 + ox, hy - 9), fill=ORGD)
        d.ellipse((41 + ox, hy - 12, 44 + ox, hy - 10), fill=LEAF)
    # feet toes
    for lx in (12, 21, 33, 42):
        d.line((lx + 1 + ox, 43 + DY, lx + 4 + ox, 43 + DY), fill=OUT)
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
