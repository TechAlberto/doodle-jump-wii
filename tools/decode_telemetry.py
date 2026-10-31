#!/usr/bin/env python3
"""Decode the TELEMETRY strip from a Dolphin capture.

The strip is drawn by source/dbg.c: 10 rows of 32 cells, 6x6 px cell, 7 px
pitch, starting at (6, 6).  The capture is first normalised to the 640x480 game
grid (Dolphin's window render surface squashes it), then each cell centre is
sampled and the bits are reassembled into the values main.c packed.

usage: decode_telemetry.py SHOT.png [--save norm.png]
"""
import sys

import numpy as np
from PIL import Image

CELL, GAPX, GAPY = 6, 1, 4
X0, Y0 = 6, 6

NAMES = ["state", "score", "best", "y(+10000)", "cam(+10000)",
         "max_height", "vy(+4000)", "platforms", "monsters", "frame",
         "update_us", "draw_us", "present_us",
         "bg_us", "plat_us", "actors_us", "doodler_us", "hud_us", "overlay_us",
         "fill_px", "aa_px", "line_px", "text_px", "poly_px",
         "fill_us", "shape_us", "line_us", "text_us", "poly_us",
         "x(+10000)", "vx(+4000)", "want_x(+10000)", "want_y(+10000)",
         "n_above", "n_reach", "apex(+10000)", "top_x(+10000)",
         "top_y(+10000)"]


def normalise(im):
    a = np.asarray(im.convert("RGB"))
    lum = a.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    ys, xs = np.nonzero(lum > 12.0)
    if len(ys) == 0:
        return im.convert("RGB"), None
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    return im.crop(box).resize((640, 480), Image.BILINEAR).convert("RGB"), box


def main():
    path = sys.argv[1]
    save = None
    if "--save" in sys.argv:
        save = sys.argv[sys.argv.index("--save") + 1]

    im, box = normalise(Image.open(path))
    if save:
        im.save(save)
    a = np.asarray(im).astype(np.int32)

    print("%s  content-box=%s" % (path, box))
    values = []
    for r in range(len(NAMES)):
        bits = 0
        cells = []
        for b in range(32):
            x = X0 + b * (CELL + GAPX) + CELL // 2
            y = Y0 + r * (CELL + GAPY) + CELL // 2
            on = a[y, x].mean() > 128
            cells.append("1" if on else "0")
            if on:
                bits |= 1 << b
        values.append(bits)

    for name, v in zip(NAMES, values):
        signed = v - (1 << 32) if v >= (1 << 31) else v
        print("  %-12s %10d  (signed %d)" % (name, v, signed))

    state, score, best = values[0], values[1], values[2]
    y = values[3] - 10000
    cam = values[4] - 10000
    vy = values[6] - 4000
    print("decoded: state=%s score=%d best=%d y=%d cam=%d vy=%d platforms=%d "
          "monsters=%d frame=%d x=%d vx=%d want=%d/%d nup=%d nok=%d apex=%d "
          "top=%d/%d"
          % ({0: "TITLE", 1: "PLAY", 2: "PAUSE", 3: "DEAD"}.get(state, state),
             score, best, y, cam, vy, values[7], values[8], values[9],
             values[29] - 10000, values[30] - 4000, values[31] - 10000,
             values[32] - 10000, values[33], values[34], values[35] - 10000,
             values[36] - 10000, values[37] - 10000))
    print("  timing us: update=%d draw=%d present=%d | bg=%d plat=%d actors=%d "
          "doodler=%d hud=%d overlay=%d"
          % (values[10], values[11], values[12], values[13], values[14],
             values[15], values[16], values[17], values[18]))
    print("  prim us: fill=%d shape=%d line=%d text=%d poly=%d"
          % (values[24], values[25], values[26], values[27], values[28]))


if __name__ == "__main__":
    main()
