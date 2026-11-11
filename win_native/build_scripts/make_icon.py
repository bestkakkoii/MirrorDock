# SPDX-License-Identifier: MIT
"""Generate the UxPlay native application icon (monitor + phone glyph).

Pure Python (zlib only): draws at 4x supersampling, writes a multi-size ICO
whose entries are PNG streams.  usage: python -I make_icon.py <output.ico>
"""
import struct
import sys
import zlib


def rounded_rect_coverage(x, y, left, top, right, bottom, radius):
    """1.0 inside the rounded rectangle, 0.0 outside (point sample)."""
    if x < left or x > right or y < top or y > bottom:
        return 0.0
    cx = min(max(x, left + radius), right - radius)
    cy = min(max(y, top + radius), bottom - radius)
    return 1.0 if (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2 else 0.0


def render(size):
    scale = 4
    big = size * scale
    unit = big / 256.0
    pixels = bytearray(big * big * 4)
    for py in range(big):
        for px in range(big):
            x = px / unit
            y = py / unit
            r = g = b = a = 0.0
            # background tile with vertical gradient
            if rounded_rect_coverage(x, y, 8, 8, 248, 248, 52):
                t = y / 256.0
                r, g, b, a = 0.07 + 0.05 * t, 0.42 - 0.18 * t, 0.86 - 0.30 * t, 1.0
            # monitor outline (white ring) and dark screen
            outer = rounded_rect_coverage(x, y, 36, 50, 220, 170, 16)
            inner = rounded_rect_coverage(x, y, 50, 64, 206, 156, 8)
            if outer and not inner:
                r, g, b = 1.0, 1.0, 1.0
            elif inner:
                r, g, b = 0.05, 0.10, 0.22
            # monitor stand
            if rounded_rect_coverage(x, y, 104, 170, 152, 196, 4) or rounded_rect_coverage(x, y, 80, 194, 176, 206, 6):
                r, g, b = 1.0, 1.0, 1.0
            # phone in front of the screen (white body, blue display)
            phone_outer = rounded_rect_coverage(x, y, 150, 104, 210, 214, 14)
            phone_inner = rounded_rect_coverage(x, y, 158, 116, 202, 200, 6)
            if phone_outer and not phone_inner:
                r, g, b = 1.0, 1.0, 1.0
            elif phone_inner:
                r, g, b = 0.20, 0.72, 1.0
            # mirrored picture on the monitor: a light bar
            if rounded_rect_coverage(x, y, 64, 78, 140, 142, 6):
                r, g, b = 0.20, 0.72, 1.0
            offset = (py * big + px) * 4
            pixels[offset:offset + 4] = bytes(int(round(v * 255)) for v in (r, g, b, a))
    # box filter down to the target size
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(scale):
                for sx in range(scale):
                    offset = (((y * scale + sy) * big) + (x * scale + sx)) * 4
                    alpha = pixels[offset + 3]
                    acc[0] += pixels[offset] * alpha
                    acc[1] += pixels[offset + 1] * alpha
                    acc[2] += pixels[offset + 2] * alpha
                    acc[3] += alpha
            o = (y * size + x) * 4
            if acc[3]:
                out[o] = acc[0] // acc[3]
                out[o + 1] = acc[1] // acc[3]
                out[o + 2] = acc[2] // acc[3]
            out[o + 3] = acc[3] // (scale * scale)
    return out


def png(size, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * size * 4:(y + 1) * size * 4]) for y in range(size))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)

    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [png(size, render(size)) for size in sizes]
    directory = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b""
    for size, image in zip(sizes, images):
        dimension = 0 if size == 256 else size
        entries += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    with open(sys.argv[1], "wb") as handle:
        handle.write(directory + entries + b"".join(images))


if __name__ == "__main__":
    main()
