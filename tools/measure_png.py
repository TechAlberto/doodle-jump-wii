#!/usr/bin/env python3
"""Measure a Dolphin capture and normalise it back to game pixels.

Dolphin 2609 lays the 640x480 game image into a 640x395 render surface and
stretches it, so a window capture has the picture vertically squashed by
0.8229 with 43 px black bars.  This tool finds the picture, rescales it to
640x480, and reports geometry in *game* pixels.

usage: measure_png.py SHOT.png [cols rows] [--save norm.png] [--raw]
"""
import sys

import numpy as np
from PIL import Image

RAMP = " .:-=+*#%@"
GAME_W, GAME_H = 640, 480


def content_bbox(a):
    lum = a.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    mask = lum > 12.0
    ys, xs = np.nonzero(mask)
    if len(ys) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def normalise(im):
    """Crop to the picture and stretch it back to the 640x480 game grid."""
    a = np.asarray(im.convert("RGB"))
    bb = content_bbox(a)
    if not bb:
        return im.resize((GAME_W, GAME_H), Image.BILINEAR), None
    return im.crop(bb).resize((GAME_W, GAME_H), Image.BILINEAR), bb


def main():
    args = [a for a in sys.argv[1:]]
    save = None
    if "--save" in args:
        i = args.index("--save")
        save = args[i + 1]
        del args[i:i + 2]
    raw = "--raw" in args
    if raw:
        args.remove("--raw")
    cols = int(args[1]) if len(args) > 1 else 64
    rows = int(args[2]) if len(args) > 2 else 32

    path = args[0]
    im = Image.open(path)
    a0 = np.asarray(im.convert("RGB"))
    bb = content_bbox(a0)
    print("%s  %dx%d" % (path, im.size[0], im.size[1]))
    if bb:
        x0, y0, x1, y1 = bb
        print("picture      : x %d..%d  y %d..%d  (%dx%d, %.4f of full height)"
              % (x0, x1 - 1, y0, y1 - 1, x1 - x0, y1 - y0, (y1 - y0) / im.size[1]))
        print("bars         : top=%d bottom=%d left=%d right=%d"
              % (y0, im.size[1] - y1, x0, im.size[0] - x1))

    if raw:
        im = im.convert("RGB")
    else:
        im, _ = normalise(im)
        if save:
            im.save(save)
            print("normalised   : %s (%dx%d)" % (save, im.size[0], im.size[1]))

    g = np.asarray(im.convert("L").resize((cols, rows), Image.BOX), np.float32)
    print("view (%dx%d%s):" % (cols, rows, ", game pixels" if not raw else ""))
    for r in range(rows):
        print("  " + "".join(RAMP[min(9, int(g[r, c]) * 10 // 256)] for c in range(cols)))


if __name__ == "__main__":
    main()
