#!/usr/bin/env python3
"""Regenerate SCos's built-in application icons and Xcursor theme.

Only the Python standard library is used. The generated files are shipped in
the repository, so Python is not a build or runtime dependency.
"""
from pathlib import Path
import math
import os
import struct
import zlib

ROOT = Path(__file__).resolve().parent
ICON_ROOT = ROOT / "icons" / "hicolor"
CURSOR_ROOT = ROOT / "icons" / "SCos" / "cursors"
ACCENT = (57, 255, 20, 255)
DARK = (0, 21, 0, 255)
PANEL = (26, 26, 26, 255)
WHITE = (245, 245, 245, 255)
BLACK = (5, 8, 5, 255)


def rgba(c):
    return tuple(c) if len(c) == 4 else (*c, 255)


class Canvas:
    # All drawing coordinates use a 32x32 design grid. Supersampling and
    # premultiplied-alpha downsampling keep every output size sharp.
    def __init__(self, size, supersample=4):
        self.size = size
        self.n = size * supersample
        self.ss = supersample
        self.pixels = bytearray(self.n * self.n * 4)

    def xy(self, v):
        return v * self.n / 32.0

    def put(self, x, y, color):
        if 0 <= x < self.n and 0 <= y < self.n:
            i = (y * self.n + x) * 4
            self.pixels[i:i + 4] = bytes(color)

    def rect(self, x0, y0, x1, y1, color):
        color = rgba(color)
        xa, xb = sorted((self.xy(x0), self.xy(x1)))
        ya, yb = sorted((self.xy(y0), self.xy(y1)))
        for y in range(max(0, math.floor(ya)), min(self.n, math.ceil(yb))):
            for x in range(max(0, math.floor(xa)), min(self.n, math.ceil(xb))):
                if xa <= x + 0.5 < xb and ya <= y + 0.5 < yb:
                    self.put(x, y, color)

    def line(self, a, b, width, color):
        color = rgba(color)
        ax, ay, bx, by = self.xy(a[0]), self.xy(a[1]), self.xy(b[0]), self.xy(b[1])
        radius = self.xy(width) / 2.0
        dx, dy = bx - ax, by - ay
        length2 = dx * dx + dy * dy
        x0 = max(0, math.floor(min(ax, bx) - radius - 1))
        x1 = min(self.n, math.ceil(max(ax, bx) + radius + 1))
        y0 = max(0, math.floor(min(ay, by) - radius - 1))
        y1 = min(self.n, math.ceil(max(ay, by) + radius + 1))
        for y in range(y0, y1):
            for x in range(x0, x1):
                px, py = x + 0.5, y + 0.5
                if length2:
                    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length2))
                    qx, qy = ax + t * dx, ay + t * dy
                else:
                    qx, qy = ax, ay
                if (px - qx) ** 2 + (py - qy) ** 2 <= radius ** 2:
                    self.put(x, y, color)

    def polygon(self, points, fill, outline=None, width=0.8):
        pts = [(self.xy(x), self.xy(y)) for x, y in points]
        fill = rgba(fill)
        x0 = max(0, math.floor(min(x for x, _ in pts)))
        x1 = min(self.n, math.ceil(max(x for x, _ in pts) + 1))
        y0 = max(0, math.floor(min(y for _, y in pts)))
        y1 = min(self.n, math.ceil(max(y for _, y in pts) + 1))
        for y in range(y0, y1):
            for x in range(x0, x1):
                px, py = x + 0.5, y + 0.5
                inside = False
                j = len(pts) - 1
                for i, (xi, yi) in enumerate(pts):
                    xj, yj = pts[j]
                    if ((yi > py) != (yj > py)) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
                        inside = not inside
                    j = i
                if inside:
                    self.put(x, y, fill)
        if outline:
            for i, a in enumerate(points):
                self.line(a, points[(i + 1) % len(points)], width, outline)

    def circle(self, center, radius, fill, outline=None, width=0.8):
        cx, cy, r = self.xy(center[0]), self.xy(center[1]), self.xy(radius)
        fill = rgba(fill)
        for y in range(max(0, math.floor(cy - r)), min(self.n, math.ceil(cy + r + 1))):
            for x in range(max(0, math.floor(cx - r)), min(self.n, math.ceil(cx + r + 1))):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r ** 2:
                    self.put(x, y, fill)
        if outline:
            # A circle made from short segments uses the same line rasterizer.
            pts = [(center[0] + radius * math.cos(i * math.tau / 64),
                    center[1] + radius * math.sin(i * math.tau / 64)) for i in range(64)]
            for i, a in enumerate(pts):
                self.line(a, pts[(i + 1) % 64], width, outline)

    def image(self):
        out = bytearray(self.size * self.size * 4)
        samples = self.ss * self.ss
        for y in range(self.size):
            for x in range(self.size):
                alpha_sum = 0
                weighted = [0, 0, 0]
                for sy in range(self.ss):
                    for sx in range(self.ss):
                        i = ((y * self.ss + sy) * self.n + x * self.ss + sx) * 4
                        a = self.pixels[i + 3]
                        alpha_sum += a
                        for c in range(3):
                            weighted[c] += self.pixels[i + c] * a
                o = (y * self.size + x) * 4
                if alpha_sum:
                    for c in range(3):
                        out[o + c] = round(weighted[c] / alpha_sum)
                out[o + 3] = round(alpha_sum / samples)
        return self.size, self.size, out


def badge(c):
    c.polygon([(5, 2), (27, 2), (30, 5), (30, 27), (27, 30), (5, 30),
               (2, 27), (2, 5)], PANEL, ACCENT, 0.75)


def app_icon(kind, size):
    c = Canvas(size)
    badge(c)
    if kind == "terminal":
        c.line((8, 10), (14, 16), 2.2, ACCENT)
        c.line((14, 16), (8, 22), 2.2, ACCENT)
        c.line((16, 23), (25, 23), 2.2, ACCENT)
    elif kind == "files":
        c.polygon([(6, 9), (13, 9), (16, 12), (26, 12), (26, 24), (6, 24)],
                  ACCENT, DARK, 0.6)
        c.polygon([(6, 14), (26, 14), (23, 24), (6, 24)], ACCENT)
        c.line((7, 15), (25, 15), 0.9, DARK)
    elif kind == "sysmon":
        c.line((7, 24), (26, 24), 1.1, ACCENT)
        for x, top in [(8, 17), (13, 11), (18, 14), (23, 7)]:
            c.rect(x, top, x + 3, 23, ACCENT)
    elif kind == "settings":
        for y, knob in [(9, 11), (16, 21), (23, 14)]:
            c.line((7, y), (25, y), 1.25, ACCENT)
            c.rect(knob - 2, y - 2.5, knob + 2, y + 2.5, ACCENT)
            c.rect(knob - 0.7, y - 1.2, knob + 0.7, y + 1.2, DARK)
    return c.image()


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def write_png(path, image):
    w, h, pixels = image
    raw = b"".join(b"\0" + bytes(pixels[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    data = b"\x89PNG\r\n\x1a\n"
    data += png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += png_chunk(b"IDAT", zlib.compress(raw, 9))
    data += png_chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def cursor_image(kind, size):
    c = Canvas(size)
    arrow = [(4, 2), (4, 24), (10, 18), (14, 28), (19, 26), (15, 16), (23, 16)]
    if kind in ("left_ptr", "right_ptr"):
        pts = arrow if kind == "left_ptr" else [(32 - x, y) for x, y in arrow]
        c.polygon(pts, WHITE, BLACK, 1.25)
        # A restrained SCos accent in the arrow's heel distinguishes it
        # from the similar-looking desktop-theme defaults.
        if kind == "left_ptr":
            c.line((7, 18), (9, 22), 1.1, ACCENT)
        else:
            c.line((25, 18), (23, 22), 1.1, ACCENT)
    elif kind == "xterm":
        for width, color in [(4.5, WHITE), (1.8, BLACK)]:
            c.line((16, 5), (16, 27), width, color)
            c.line((10, 5), (22, 5), width, color)
            c.line((10, 27), (22, 27), width, color)
    elif kind == "hand2":
        c.polygon([(10, 28), (6, 22), (4, 14), (6, 12), (9, 17), (9, 4),
                   (12, 3), (14, 14), (15, 7), (18, 7), (18, 14), (20, 9),
                   (23, 10), (22, 17), (25, 13), (27, 15), (24, 24), (19, 29)],
                  WHITE, BLACK, 1.0)
    elif kind == "move":
        c.polygon([(14, 2), (18, 2), (18, 10), (23, 7), (30, 14), (30, 18),
                   (22, 18), (25, 23), (18, 30), (14, 30), (14, 22), (9, 25),
                   (2, 18), (2, 14), (10, 14), (7, 9)], WHITE, BLACK, 0.9)
    elif kind == "crosshair":
        for width, color in [(5.5, BLACK), (2.1, WHITE)]:
            c.line((16, 2), (16, 30), width, color)
            c.line((2, 16), (30, 16), width, color)
        c.circle((16, 16), 2.2, ACCENT)
    elif kind == "watch":
        c.circle((16, 16), 13, WHITE, BLACK, 1.4)
        c.circle((16, 16), 2, ACCENT)
        c.line((16, 16), (16, 7), 1.8, BLACK)
        c.line((16, 16), (22, 20), 1.8, BLACK)
    elif kind in ("hresize", "vresize", "bd_double_arrow", "fd_double_arrow"):
        if kind == "hresize":
            a, b = (3, 16), (29, 16)
            heads = [([(3, 16), (10, 10), (10, 22)],), ([(29, 16), (22, 10), (22, 22)],)]
            head_points = [head[0] for head in heads]
        elif kind == "vresize":
            a, b = (16, 3), (16, 29)
            head_points = [[(16, 3), (10, 10), (22, 10)], [(16, 29), (10, 22), (22, 22)]]
        elif kind == "bd_double_arrow":
            a, b = (5, 27), (27, 5)
            head_points = [[(5, 27), (6, 18), (14, 26)], [(27, 5), (18, 6), (26, 14)]]
        else:
            a, b = (5, 5), (27, 27)
            head_points = [[(5, 5), (14, 6), (6, 14)], [(27, 27), (18, 26), (26, 18)]]
        c.line(a, b, 7, BLACK)
        c.line(a, b, 3.2, WHITE)
        for points in head_points:
            c.polygon(points, WHITE, BLACK, 0.8)
    elif kind == "not-allowed":
        c.circle((16, 16), 13, WHITE, BLACK, 1.4)
        c.line((8, 24), (24, 8), 4.2, BLACK)
        c.line((9, 23), (23, 9), 1.5, ACCENT)
    return c.image()


def write_xcursor(path, kind):
    sizes = (24, 32, 48)
    images = [(size, *cursor_image(kind, size)) for size in sizes]
    positions = []
    chunks = []
    position = 16 + 12 * len(images)
    for nominal, w, h, pixels in images:
        if kind == "left_ptr":
            hx, hy = 4, 2
        elif kind == "right_ptr":
            hx, hy = 28, 2
        elif kind == "hand2":
            hx, hy = 11, 4
        elif kind == "xterm":
            hx, hy = 16, 16
        else:
            hx, hy = 16, 16
        xhot = min(w - 1, round(hx * w / 32))
        yhot = min(h - 1, round(hy * h / 32))
        positions.append(position)
        argb = [struct.pack("<I", (pixels[i + 3] << 24) | (pixels[i] << 16) |
                            (pixels[i + 1] << 8) | pixels[i + 2])
                for i in range(0, len(pixels), 4)]
        chunk = struct.pack("<9I", 36, 0xFFFD0002, nominal, 1, w, h, xhot, yhot, 0)
        chunk += b"".join(argb)
        chunks.append(chunk)
        position += len(chunk)
    header = struct.pack("<4I", 0x72756358, 16, 0x00010002, len(images))
    toc = b"".join(struct.pack("<3I", 0xFFFD0002, size, pos)
                   for size, pos in zip(sizes, positions))
    path.write_bytes(header + toc + b"".join(chunks))


def main():
    for kind in ("terminal", "files", "sysmon", "settings"):
        for size in (16, 22, 24, 32, 48, 64, 128, 256):
            write_png(ICON_ROOT / f"{size}x{size}" / "apps" / f"scos-{kind}.png", app_icon(kind, size))

    CURSOR_ROOT.mkdir(parents=True, exist_ok=True)
    kinds = ("left_ptr", "right_ptr", "xterm", "hand2", "move", "crosshair", "watch",
             "hresize", "vresize", "bd_double_arrow", "fd_double_arrow", "not-allowed")
    for kind in kinds:
        write_xcursor(CURSOR_ROOT / kind, kind)

    aliases = {
        "left_ptr": "default arrow top_left_arrow center_ptr context-menu copy dnd-copy dnd-move dnd-ask alias dnd-link help question_arrow pencil",
        "right_ptr": "top_right_arrow",
        "xterm": "text ibeam vertical-text",
        "hand2": "hand hand1 pointer pointing_hand link grab openhand grabbing closedhand",
        "move": "fleur size_all all-scroll",
        "crosshair": "cross plus cell zoom-in zoom-out X_cursor tcross draft_large draft_small",
        "watch": "wait left_ptr_watch progress",
        "hresize": "sb_h_double_arrow h_double_arrow ew-resize e-resize w-resize col-resize left_side right_side size_hor split_h",
        "vresize": "sb_v_double_arrow v_double_arrow ns-resize n-resize s-resize row-resize top_side bottom_side size_ver split_v",
        "bd_double_arrow": "nesw-resize ne-resize sw-resize bottom_left_corner top_right_corner size_bdiag",
        "fd_double_arrow": "nwse-resize nw-resize se-resize top_left_corner bottom_right_corner size_fdiag",
        "not-allowed": "forbidden crossed_circle no-drop dnd-no-drop dnd-none",
    }
    for target, names in aliases.items():
        for name in names.split():
            link = CURSOR_ROOT / name
            if link.exists() or link.is_symlink():
                link.unlink()
            link.symlink_to(target)

    (CURSOR_ROOT.parent / "index.theme").write_text(
        "[Icon Theme]\nName=SCos\nComment=SCos white-arrow and desktop cursors\n", encoding="utf-8")
    (CURSOR_ROOT.parent / "cursor.theme").write_text(
        "[Icon Theme]\nInherits=SCos\n", encoding="utf-8")
    print("generated four SCos app icons at eight sizes and the SCos Xcursor theme")


if __name__ == "__main__":
    main()
