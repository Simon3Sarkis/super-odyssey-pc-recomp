"""The app icon (docs/icon/): a green warp pipe standing on brick ground under a gold star, on a sky-blue rounded square -
our own drawing, no game art (the same pipe as runtime/ui/theme.h theme_logo and the installer's background).
Geometry: a 16 x 16 grid over the rounded square; the pipe's body and its wider lip with a light stripe and a dark
shade, a five-pointed star above. Colours: the palette of runtime/ui/theme.h. Pure Python (no Pillow): rasterised with
4x4 supersampling, written as PNG; icon.ico holds PNG images of the Windows sizes, icon.svg is the same drawing as a
vector file.
Run: python tools/make_icon.py"""
import math
import os
import struct
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'icon')
SIZE = 1024
SQUARE_MARGIN, SQUARE_RADIUS = 32, 210
U = (SIZE - 2 * SQUARE_MARGIN) / 16  # one grid unit

SKY_TOP, SKY_LOW = (0x6F, 0xB4, 0xFF), (0x2F, 0x74, 0xD8)
PIPE, PIPE_LIGHT, PIPE_DARK, PIPE_EDGE = (0x3B, 0xC4, 0x5A), (0x96, 0xEC, 0x82), (0x10, 0x60, 0x26), (0x08, 0x30, 0x12)
BRICK, BRICK_DARK, BRICK_LIGHT = (0xC4, 0x5C, 0x28), (0x60, 0x28, 0x10), (0xEC, 0x94, 0x5C)
GOLD, GOLD_EDGE = (0xF5, 0xC5, 0x42), (0x9A, 0x60, 0x10)

# rectangles in grid units: (x0, y0, x1, y1, colour), drawn in order (later on top)
RECTS = [
    # the brick ground
    (0, 13, 16, 16, BRICK),
    (0, 13, 16, 13.25, BRICK_LIGHT),
    (0, 14.4, 16, 14.6, BRICK_DARK), (0, 15.4, 16, 15.6, BRICK_DARK),
    (3, 13.25, 3.2, 14.4, BRICK_DARK), (8, 13.25, 8.2, 14.4, BRICK_DARK), (13, 13.25, 13.2, 14.4, BRICK_DARK),
    (5.5, 14.6, 5.7, 15.4, BRICK_DARK), (10.5, 14.6, 10.7, 15.4, BRICK_DARK),
    # the pipe's body (outline, fill, light stripe, shade)
    (4.6, 8, 11.4, 13, PIPE_EDGE),
    (5, 8, 11, 13, PIPE),
    (5.8, 8, 6.8, 13, PIPE_LIGHT), (7.3, 8, 7.6, 13, PIPE_LIGHT),
    (9.2, 8, 10.4, 13, PIPE_DARK),
    # the lip
    (3.1, 5.4, 12.9, 8.6, PIPE_EDGE),
    (3.5, 5.8, 12.5, 8.2, PIPE),
    (4.3, 6.2, 5.3, 7.8, PIPE_LIGHT), (5.8, 6.2, 6.1, 7.8, PIPE_LIGHT),
    (10.4, 6.2, 11.8, 7.8, PIPE_DARK),
    (3.5, 5.8, 12.5, 6.1, PIPE_LIGHT),
]
STAR_C, STAR_R, STAR_r = (8, 2.9), 2.35, 1.0  # centre, outer and inner radius (grid units)


def star_points(c, r_out, r_in):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = r_out if i % 2 == 0 else r_in
        pts.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a)))
    return pts


STAR = star_points(STAR_C, STAR_R, STAR_r)
STAR_OUTLINE = star_points(STAR_C, STAR_R + 0.35, STAR_r + 0.2)


def in_poly(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        (xi, yi), (xj, yj) = pts[i], pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def in_rrect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def color_at(x, y):
    """Colour of the drawing at (x, y) on the 0..SIZE canvas, or None (transparent)."""
    if not in_rrect(x, y, SQUARE_MARGIN, SQUARE_MARGIN, SIZE - SQUARE_MARGIN, SIZE - SQUARE_MARGIN, SQUARE_RADIUS):
        return None
    gx, gy = (x - SQUARE_MARGIN) / U, (y - SQUARE_MARGIN) / U
    t = min(max(gy / 13, 0), 1)
    c = tuple(round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_LOW))
    for x0, y0, x1, y1, col in RECTS:
        if x0 <= gx < x1 and y0 <= gy < y1:
            c = col
    if in_poly(gx, gy, STAR_OUTLINE):
        c = GOLD if in_poly(gx, gy, STAR) else GOLD_EDGE
    return c


def render(size):
    s = 4  # supersampling
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(s):
                for sx in range(s):
                    c = color_at((px + (sx + 0.5) / s) * SIZE / size, (py + (sy + 0.5) / s) * SIZE / size)
                    if c:
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += 255
            n = acc[3] // 255
            row += bytes((acc[0] // n, acc[1] // n, acc[2] // n, acc[3] // (s * s)) if n else (0, 0, 0, 0))
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def svg():
    hexc = lambda c: '#%02X%02X%02X' % c
    w = SIZE - 2 * SQUARE_MARGIN
    g = lambda v: SQUARE_MARGIN + v * U
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {SIZE} {SIZE}">',
             f'  <defs><linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{hexc(SKY_TOP)}"/>'
             f'<stop offset="0.8125" stop-color="{hexc(SKY_LOW)}"/></linearGradient>',
             f'  <clipPath id="sq"><rect x="{SQUARE_MARGIN}" y="{SQUARE_MARGIN}" width="{w}" height="{w}" rx="{SQUARE_RADIUS}"/></clipPath></defs>',
             '  <g clip-path="url(#sq)">',
             f'  <rect x="{SQUARE_MARGIN}" y="{SQUARE_MARGIN}" width="{w}" height="{w}" fill="url(#sky)"/>']
    for x0, y0, x1, y1, col in RECTS:
        parts.append(f'  <rect x="{g(x0):.1f}" y="{g(y0):.1f}" width="{(x1 - x0) * U:.1f}" height="{(y1 - y0) * U:.1f}" fill="{hexc(col)}"/>')
    for pts, col in ((STAR_OUTLINE, GOLD_EDGE), (STAR, GOLD)):
        parts.append('  <polygon points="' + ' '.join(f'{g(px):.1f},{g(py):.1f}' for px, py in pts) + f'" fill="{hexc(col)}"/>')
    parts += ['  </g>', '</svg>', '']
    return '\n'.join(parts)


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = {n: png(n, render(n)) for n in sizes}
    # ICO: header, one directory entry per size (0 = 256), then the PNG images
    ico = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    for n in sizes:
        ico += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(images[n]), off)
        off += len(images[n])
    ico += b''.join(images[n] for n in sizes)
    open(os.path.join(OUT, 'icon.ico'), 'wb').write(ico)
    open(os.path.join(OUT, 'icon_512.png'), 'wb').write(png(512, render(512)))
    open(os.path.join(OUT, 'icon.svg'), 'w', newline='\n').write(svg())
    print('wrote', OUT)
