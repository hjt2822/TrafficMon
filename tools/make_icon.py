#!/usr/bin/env python3
"""Generate the Signal Paper app icon (16/32/48 px) without third-party packages.

The mark is an instrument tile: paper field, ink hairline, and three measured bars
(ink-green, copper, muted grey). Output is a Windows .ico with 32-bit BGRA DIBs.
"""
from __future__ import annotations

import math
import struct
from pathlib import Path

BG = (0xF2, 0xF4, 0xF1, 255)
BORDER = (0xB9, 0xC6, 0xC0, 255)
GREEN = (0x0E, 0x5E, 0x56, 255)
COPPER = (0xA8, 0x5F, 0x2A, 255)
GREY = (0xB9, 0xC6, 0xC0, 255)
SUPERSAMPLE = 4


def rounded_rect_coverage(px: float, py: float, x0: float, y0: float,
                          x1: float, y1: float, radius: float) -> bool:
    if px < x0 or px > x1 or py < y0 or py > y1:
        return False
    rx = min(radius, (x1 - x0) / 2)
    ry = min(radius, (y1 - y0) / 2)
    cx = min(max(px, x0 + rx), x1 - rx)
    cy = min(max(py, y0 + ry), y1 - ry)
    if cx == px or cy == py:
        return True
    return (px - cx) ** 2 + (py - cy) ** 2 <= rx ** 2


def render(size: int) -> bytes:
    """Return size*size RGBA rows, top-down."""
    s = size * SUPERSAMPLE
    # Geometry in supersampled pixels.
    def r(v: float) -> float:
        return v * s

    bg = (r(0.03), r(0.03), r(0.97), r(0.97), r(0.20))
    inset = max(float(SUPERSAMPLE), r(0.025))
    inner = (bg[0] + inset, bg[1] + inset, bg[2] - inset, bg[3] - inset,
             max(0.0, bg[4] - inset))
    bars = [
        (r(0.25), r(0.44), r(0.39), r(0.77), r(0.035), GREEN),
        (r(0.43), r(0.21), r(0.57), r(0.77), r(0.035), COPPER),
        (r(0.61), r(0.55), r(0.75), r(0.77), r(0.035), GREY),
    ]

    high = []
    for y in range(s):
        row = []
        for x in range(s):
            px = x + 0.5
            py = y + 0.5
            if not rounded_rect_coverage(px, py, *bg):
                row.append((0, 0, 0, 0))
                continue
            if not rounded_rect_coverage(px, py, *inner):
                row.append(BORDER)
                continue
            color = BG
            for bx0, by0, bx1, by1, br, bc in bars:
                if rounded_rect_coverage(px, py, bx0, by0, bx1, by1, br):
                    color = bc
                    break
            row.append(color)
        high.append(row)

    # Box-downsample to the target size.
    out = bytearray()
    n = SUPERSAMPLE * SUPERSAMPLE
    for y in range(size):
        for x in range(size):
            rsum = gsum = bsum = asum = 0
            for dy in range(SUPERSAMPLE):
                for dx in range(SUPERSAMPLE):
                    rr, gg, bb, aa = high[y * SUPERSAMPLE + dy][x * SUPERSAMPLE + dx]
                    rsum += rr * aa
                    gsum += gg * aa
                    bsum += bb * aa
                    asum += aa
            if asum == 0:
                out += b"\x00\x00\x00\x00"
            else:
                out += bytes((rsum // asum, gsum // asum, bsum // asum, asum // n))
    return bytes(out)


def dib(rgba: bytes, size: int) -> bytes:
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    pixels = bytearray()
    stride = size * 4
    for y in range(size - 1, -1, -1):
        row = rgba[y * stride:(y + 1) * stride]
        for x in range(0, len(row), 4):
            r, g, b, a = row[x], row[x + 1], row[x + 2], row[x + 3]
            pixels += bytes((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    mask = b"\x00" * (mask_stride * size)
    return header + bytes(pixels) + mask


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    target = root / "ui" / "app.ico"
    sizes = [16, 32, 48]
    images = [(size, dib(render(size), size)) for size in sizes]
    offset = 6 + 16 * len(images)
    directory = struct.pack("<HHH", 0, 1, len(images))
    entries = b""
    for size, data in images:
        entries += struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    target.write_bytes(directory + entries + b"".join(data for _, data in images))
    print(f"wrote {target} ({target.stat().st_size} bytes)")


if __name__ == "__main__":
    main()