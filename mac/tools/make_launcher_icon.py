#!/usr/bin/env python3
"""Regenerate tools/AutoEDO.app/Contents/Resources/AppIcon.icns.

Pure stdlib (struct + zlib): rasterizes a rounded-rect + degree-lug design
(the pitch ruler's row of lugs — bone, one amber "correcting", gold root)
and packs the PNGs into an icns by hand. Runs anywhere, no Pillow, no
iconutil. Edit the colour constants / bar geometry below; nothing else is
AutoEDO-specific.
"""

import os
import struct
import sys
import zlib

# ---- design ----------------------------------------------------------------
BG     = (0x16, 0x16, 0x1C)   # card background
BONE   = (0xCF, 0xCC, 0xC0)   # lit degree lug
GOLD   = (0xC9, 0xA2, 0x27)   # root lug
AMBER  = (0xFF, 0xB4, 0x54)   # live correction target
# bar heights as fractions of the inner area, and per-bar colour
BARS = [(0.42, GOLD), (0.58, BONE), (0.78, BONE), (0.96, AMBER),
        (0.72, BONE), (0.52, BONE), (0.38, BONE)]

SS = 2  # supersampling factor (render at SS*size, box-downsample)


def render(size):
    """Return RGBA bytes (row-major) for one icon size."""
    S = size * SS
    margin = S * 0.075
    radius = S * 0.205
    inner_l, inner_t = S * 0.20, S * 0.24
    inner_r, inner_b = S * 0.80, S * 0.80
    n = len(BARS)
    slot = (inner_r - inner_l) / n
    bar_w = slot * 0.62

    def inside_rounded_rect(x, y):
        l, t, r, b = margin, margin, S - margin, S - margin
        if x < l or x > r or y < t or y > b:
            return False
        cx = min(max(x, l + radius), r - radius)
        cy = min(max(y, t + radius), b - radius)
        return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius

    def bar_color(x, y):
        for i, (h, col) in enumerate(BARS):
            bx = inner_l + slot * (i + 0.5)
            half_h = (inner_b - inner_t) * h / 2.0
            cy0 = (inner_t + inner_b) / 2.0
            l, r = bx - bar_w / 2, bx + bar_w / 2
            t, b = cy0 - half_h, cy0 + half_h
            if l <= x <= r and t <= y <= b:
                # rounded bar ends
                rr = bar_w / 2
                yy = min(max(y, t + rr), b - rr)
                if (x - bx) ** 2 + (y - yy) ** 2 <= rr * rr or t + rr <= y <= b - rr:
                    return col
        return None

    rows = bytearray()
    for py in range(size):
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(SS):
                for sx in range(SS):
                    x = px * SS + sx + 0.5
                    y = py * SS + sy + 0.5
                    if inside_rounded_rect(x, y):
                        c = bar_color(x, y) or BG
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]
                        acc[3] += 255
            k = SS * SS
            rows += bytes((acc[0] // k, acc[1] // k, acc[2] // k, acc[3] // k))
    return bytes(rows)


def png(size, rgba):
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        return c + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4]
                   for y in range(size))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "AutoEDO.app", "Contents", "Resources", "AppIcon.icns")
    os.makedirs(os.path.dirname(out), exist_ok=True)

    chunks = b""
    for typ, size in ((b"ic07", 128), (b"ic08", 256), (b"ic09", 512)):
        sys.stderr.write(f"rendering {size}px...\n")
        data = png(size, render(size))
        chunks += typ + struct.pack(">I", len(data) + 8) + data

    blob = b"icns" + struct.pack(">I", len(chunks) + 8) + chunks
    with open(out, "wb") as f:
        f.write(blob)
    print(f"wrote {out} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
