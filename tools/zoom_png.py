#!/usr/bin/env python3
"""Zoom into a Dolphin capture at 1:1 game-pixel resolution.

The strip-based telemetry tells us *what* the game is doing; this tool is for
checking *shape* after a renderer change.  It normalises the capture back to
the 640x480 game grid and prints the requested crop as ASCII art, optionally
with the red/green/blue channels as separate views.

usage: zoom_png.py SHOT.png X Y W H [--channel l|r|g|b] [--scale N]
"""
import sys

import numpy as np
from PIL import Image

RAMP = " .:-=+*#%@"
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
    chan = "l"
    scale = 1
    if "--channel" in args:
        i = args.index("--channel")
        chan = args[i + 1]
        del args[i:i + 2]
    if "--scale" in args:
        i = args.index("--scale")
        scale = int(args[i + 1])
        del args[i:i + 2]

    path = args[0]
    x, y, w, h = (int(v) for v in args[1:5])
    im = normalise(Image.open(path))
    a = np.asarray(im).astype(np.float32)

    if chan == "l":
        g = a @ np.array([0.299, 0.587, 0.114], np.float32)
    else:
        g = a[:, :, "rgb".index(chan)]

    crop = g[max(0, y):y + h, max(0, x):x + w]
    rows, cols = crop.shape
    if scale > 1:
        crop = np.repeat(np.repeat(crop, scale, axis=0), scale, axis=1)

    print("%s  crop x=%d..%d y=%d..%d  channel=%s" % (path, x, x + w - 1,
                                                      y, y + h - 1, chan))
    for r in range(crop.shape[0]):
        print("  " + "".join(RAMP[min(9, int(v) * 10 // 256)] for v in crop[r]))


if __name__ == "__main__":
    main()
