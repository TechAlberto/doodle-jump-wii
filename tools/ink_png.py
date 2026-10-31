#!/usr/bin/env python3
"""Binary "ink" view of a Dolphin capture for checking text and layout.

The game is dark ink on a near-white paper background, so a plain luminance
ramp saturates and hides everything.  This tool normalises the capture to the
640x480 game grid and prints which cells contain dark pixels, which makes
glyph shapes and layout readable as ASCII.

usage: ink_png.py SHOT.png [COLS ROWS] [--thresh N] [--invert]
"""
import sys

import numpy as np
from PIL import Image

GAME_W, GAME_H = 640, 480


def normalise(im):
    a = np.asarray(im.convert("RGB"))
    lum = a.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    ys, xs = np.nonzero(lum > 12.0)
    if len(ys) == 0:
        return im.convert("RGB").resize((GAME_W, GAME_H), Image.BILINEAR)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    return im.crop(box).resize((GAME_W, GAME_H), Image.BILINEAR).convert("RGB")


def main():
    args = sys.argv[1:]
    thresh = 190.0
    invert = False
    if "--thresh" in args:
        i = args.index("--thresh")
        thresh = float(args[i + 1])
        del args[i:i + 2]
    if "--invert" in args:
        invert = True
        args.remove("--invert")
    cols = int(args[1]) if len(args) > 1 else 106
    rows = int(args[2]) if len(args) > 2 else 40

    im = normalise(Image.open(args[0]))
    a = np.asarray(im).astype(np.float32)
    lum = a @ np.array([0.299, 0.587, 0.114], np.float32)

    # Fraction of dark pixels in each cell, mapped to a ramp: this shows both
    # thin strokes and solid blocks.
    cw, ch = GAME_W / cols, GAME_H / rows
    ramp = " .:-=+*#%@"
    print("%s  ink<thresh=%.0f  cols=%d rows=%d  (%.1fx%.1f px/cell)"
          % (args[0], thresh, cols, rows, cw, ch))
    for r in range(rows):
        line = []
        for c in range(cols):
            y0, y1 = int(r * ch), max(int(r * ch) + 1, int((r + 1) * ch))
            x0, x1 = int(c * cw), max(int(c * cw) + 1, int((c + 1) * cw))
            cell = lum[y0:y1, x0:x1]
            frac = float((cell < thresh).mean())
            if invert:
                frac = 1.0 - frac
            line.append(ramp[min(9, int(frac * 10))])
        print("  " + "".join(line))


if __name__ == "__main__":
    main()
