#!/usr/bin/env python3
"""Print a compact text view of a PNG plus colour statistics.

Used to verify emulator output without an image viewer: downsamples to an
ASCII luminance map and reports the dominant colours per region.
"""
import sys
from collections import Counter

from PIL import Image

RAMP = " .:-=+*#%@"


def ascii_view(im, cols=64, rows=32):
    g = im.convert("L").resize((cols, rows), Image.BOX)
    px = list(g.getdata())
    out = []
    for r in range(rows):
        line = "".join(RAMP[min(len(RAMP) - 1, px[r * cols + c] * len(RAMP) // 256)]
                       for c in range(cols))
        out.append(line)
    return "\n".join(out)


def main(path, cols=64, rows=32):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    print("%s  %dx%d" % (path, w, h))
    print(ascii_view(im, cols, rows))
    c = Counter(im.getdata())
    print("distinct colours: %d" % len(c))
    for col, n in c.most_common(8):
        print("  #%02x%02x%02x  %5.1f%%" % (col[0], col[1], col[2], 100.0 * n / (w * h)))
    # 3x3 region medians, to spot letterboxing or a misplaced image
    print("region medians:")
    for ry in range(3):
        row = []
        for rx in range(3):
            box = (rx * w // 3, ry * h // 3, (rx + 1) * w // 3, (ry + 1) * h // 3)
            reg = im.crop(box).resize((1, 1), Image.BOX).getpixel((0, 0))
            row.append("#%02x%02x%02x" % reg)
        print("  " + "  ".join(row))


if __name__ == "__main__":
    main(sys.argv[1],
         int(sys.argv[2]) if len(sys.argv) > 2 else 64,
         int(sys.argv[3]) if len(sys.argv) > 3 else 32)
