#!/usr/bin/env python3
"""Generates assets/wc_paper_noise.png (512x512 RGBA, tileable).

RGB = periodic value noise (wobble field for the watercolor composite's UV
distortion); A = paper grain (finer periodic noise, remapped to mid-gray
contrast). Pure stdlib (zlib+struct) so it runs anywhere; deterministic
(fixed LCG seed), so the committed PNG is reproducible bit-for-bit.
"""
import struct
import zlib

N = 512


def lcg(seed):
    state = seed & 0xFFFFFFFF
    while True:
        state = (1103515245 * state + 12345) & 0xFFFFFFFF
        yield state >> 16


def make_grid(size, seed):
    g = lcg(seed)
    return [[next(g) % 1000 / 999.0 for _ in range(size)] for _ in range(size)]


def smooth(t):
    return t * t * (3.0 - 2.0 * t)


def periodic_value_noise(x, y, grid, size):
    # x, y in cells; wraps at `size` so the result tiles.
    xi = int(x) % size
    yi = int(y) % size
    xf = x - int(x)
    yf = y - int(y)
    u = smooth(xf)
    v = smooth(yf)
    a = grid[yi][xi]
    b = grid[yi][(xi + 1) % size]
    c = grid[(yi + 1) % size][xi]
    d = grid[(yi + 1) % size][(xi + 1) % size]
    return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v


def fbm(px, py, grids, base_cells):
    total = 0.0
    weight = 0.0
    amp = 1.0
    cells = base_cells
    for grid in grids:
        total += amp * periodic_value_noise(
            px * cells / N, py * cells / N, grid, cells)
        weight += amp
        amp *= 0.5
        cells *= 2
    return total / weight


def main():
    rgb_grids = [(make_grid(8 << o, 101 + o), 8 << o) for o in range(4)]
    grn_grids = [(make_grid(64 << o, 707 + o), 64 << o) for o in range(3)]

    def fbm2(px, py, layers):
        total = 0.0
        weight = 0.0
        amp = 1.0
        for grid, cells in layers:
            total += amp * periodic_value_noise(
                px * cells / N, py * cells / N, grid, cells)
            weight += amp
            amp *= 0.5
        return total / weight

    raw = bytearray()
    for y in range(N):
        raw.append(0)  # filter: none
        for x in range(N):
            r = fbm2(x, y, rgb_grids)
            g = fbm2(x + 137.0, y + 291.0, rgb_grids)
            b = fbm2(x + 401.0, y + 53.0, rgb_grids)
            paper = fbm2(x, y, grn_grids)
            paper = 0.5 + (paper - 0.5) * 0.9
            raw.append(int(r * 255 + 0.5))
            raw.append(int(g * 255 + 0.5))
            raw.append(int(b * 255 + 0.5))
            raw.append(int(max(0.0, min(1.0, paper)) * 255 + 0.5))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        out += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return out

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open("assets/wc_paper_noise.png", "wb") as f:
        f.write(png)
    print("wrote assets/wc_paper_noise.png")


if __name__ == "__main__":
    main()
