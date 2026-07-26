#!/usr/bin/env python3
"""Regenerate tools/AutoEDO.app/Contents/Resources/AppIcon.icns.

Pure stdlib (struct + zlib): rasterizes the AutoEDO wordmark — "Auto" over
"EDO" in a simple geometric stroke type (bone over amber on the dark card)
— and packs the PNGs into an icns by hand. Runs anywhere, no Pillow, no
iconutil, no system fonts: the seven glyphs are defined below as stroke
paths. Edit the colour constants / glyph table; nothing else is
AutoEDO-specific.
"""

import math
import os
import struct
import sys
import zlib

# ---- design ----------------------------------------------------------------
BG    = (0x16, 0x16, 0x1C)   # card background
BONE  = (0xCF, 0xCC, 0xC0)   # "Auto"
AMBER = (0xFF, 0xB4, 0x54)   # "EDO"

SS = 2  # supersampling factor

# Stroke glyphs. Coordinates are in em units: y = 0 at cap top, 1 at the
# baseline; x-height letters start at y = 0.35. Shapes:
#   ('line', (x0,y0), (x1,y1), ...)          polyline
#   ('arc', cx, cy, r, a0, a1)               circle arc, degrees, y-down
#   ('ring', cx, cy, rx, ry)                 full ellipse outline
GLYPHS = {
    'A': (0.74, [('line', (0.0, 1.0), (0.37, 0.0), (0.74, 1.0)),
                 ('line', (0.155, 0.62), (0.585, 0.62))]),
    'u': (0.56, [('line', (0.0, 0.35), (0.0, 0.72)),
                 ('arc', 0.28, 0.72, 0.28, 0.0, 180.0),
                 ('line', (0.56, 0.35), (0.56, 1.0))]),
    't': (0.44, [('line', (0.22, 0.08), (0.22, 1.0)),
                 ('line', (0.0, 0.35), (0.44, 0.35))]),
    'o': (0.62, [('ring', 0.31, 0.675, 0.31, 0.325)]),
    'E': (0.60, [('line', (0.0, 0.0), (0.0, 1.0)),
                 ('line', (0.0, 0.0), (0.60, 0.0)),
                 ('line', (0.0, 0.5), (0.54, 0.5)),
                 ('line', (0.0, 1.0), (0.60, 1.0))]),
    'D': (0.78, [('line', (0.0, 0.0), (0.0, 1.0)),
                 ('line', (0.0, 0.0), (0.30, 0.0)),
                 ('line', (0.0, 1.0), (0.30, 1.0)),
                 ('arc', 0.30, 0.5, 0.48, -90.0, 90.0)]),
    'O': (0.92, [('ring', 0.46, 0.5, 0.46, 0.5)]),
}
TRACK = 0.20   # spacing between glyphs, em
STROKE = 0.15  # stroke width, em

LINES = [('Auto', BONE), ('EDO', AMBER)]


def line_width(word):
    return sum(GLYPHS[ch][0] for ch in word) + TRACK * (len(word) - 1)


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def stroke_dist(shape, x, y):
    """Distance (em) from point to the shape's centerline."""
    if shape[0] == 'line':
        pts = shape[1:]
        return min(seg_dist(x, y, *pts[i], *pts[i + 1]) for i in range(len(pts) - 1))
    if shape[0] == 'arc':
        _, cx, cy, r, a0, a1 = shape
        dx, dy = x - cx, y - cy
        ang = math.degrees(math.atan2(dy, dx)) % 360.0
        lo, hi = a0 % 360.0, a1 % 360.0
        inside = lo <= ang <= hi if lo <= hi else (ang >= lo or ang <= hi)
        if inside:
            return abs(math.hypot(dx, dy) - r)
        ends = []
        for a in (a0, a1):
            ends.append(math.hypot(x - (cx + r * math.cos(math.radians(a))),
                                   y - (cy + r * math.sin(math.radians(a)))))
        return min(ends)
    if shape[0] == 'ring':
        _, cx, cy, rx, ry = shape
        return abs(math.hypot((x - cx) / rx, (y - cy) / ry) - 1.0) * min(rx, ry)
    return 1e9


def render(size):
    """Return RGBA bytes (row-major) for one icon size."""
    S = size * SS
    margin = S * 0.075
    radius = S * 0.205

    # Wordmark layout: two centred lines filling ~78% of the width.
    widest = max(line_width(w) for w, _ in LINES)
    em = S * 0.78 / widest
    gap = 0.50 * em
    block_h = 2 * em + gap
    top = (S - block_h) / 2.0

    layout = []  # (x0, y0, word, colour) per line, plus per-glyph x offsets
    for i, (word, col) in enumerate(LINES):
        x0 = (S - line_width(word) * em) / 2.0
        y0 = top + i * (em + gap)
        xs, x = [], 0.0
        for ch in word:
            xs.append(x)
            x += GLYPHS[ch][0] + TRACK
        layout.append((x0, y0, word, col, xs))

    hw = STROKE / 2.0

    def inside_rounded_rect(x, y):
        l, t, r, b = margin, margin, S - margin, S - margin
        if x < l or x > r or y < t or y > b:
            return False
        cx = min(max(x, l + radius), r - radius)
        cy = min(max(y, t + radius), b - radius)
        return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius

    def glyph_color(x, y):
        for x0, y0, word, col, xs in layout:
            gy = (y - y0) / em
            if gy < -0.2 or gy > 1.2:
                continue
            gx_all = (x - x0) / em
            for ch, gx0 in zip(word, xs):
                w = GLYPHS[ch][0]
                gx = gx_all - gx0
                if gx < -0.2 or gx > w + 0.2:
                    continue
                for shape in GLYPHS[ch][1]:
                    if stroke_dist(shape, gx, gy) <= hw:
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
                        c = glyph_color(x, y) or BG
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
