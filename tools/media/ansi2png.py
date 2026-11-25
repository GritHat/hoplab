#!/usr/bin/env python3
"""Render a terminal capture (text with ANSI SGR colour codes, as printed by
`tmux capture-pane -e -p`) to a PNG image.

    ansi2png.py capture.txt out.png [--cols 170 --rows 48 --size 18]
                [--caption "text"] [--width 1920 --height 1080]

Used to make the screenshots, the preview GIF and the video of the README:
see tools/media/make-media.sh.
"""
import argparse
import re
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
SANS_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
BG = (16, 17, 22)
FG = (208, 208, 208)

BASE16 = [(0, 0, 0), (205, 49, 49), (13, 188, 121), (229, 229, 16), (36, 114, 200), (188, 63, 188),
          (17, 168, 205), (229, 229, 229), (102, 102, 102), (241, 76, 76), (35, 209, 139), (245, 245, 67),
          (59, 142, 234), (214, 112, 214), (41, 184, 219), (255, 255, 255)]


def xterm(n):
    if n < 16:
        return BASE16[n]
    if n < 232:
        n -= 16
        lv = [0, 95, 135, 175, 215, 255]
        return (lv[n // 36], lv[n // 6 % 6], lv[n % 6])
    g = 8 + 10 * (n - 232)
    return (g, g, g)


def parse(text, cols, rows):
    """-> grid[row][col] = (char, fg, bg, bold)"""
    grid = [[(" ", FG, None, False) for _ in range(cols)] for _ in range(rows)]
    fg, bg, bold, dim, rev = FG, None, False, False, False
    for r, line in enumerate(text.split("\n")[:rows]):
        c = 0
        pos = 0
        for m in re.finditer(r"\x1b\[([0-9;]*)m", line):
            for ch in line[pos:m.start()]:
                if c < cols:
                    f, b = (bg or BG, fg) if rev else (fg, bg)
                    if dim:
                        f = tuple(int(v * 0.6) for v in f)
                    grid[r][c] = (ch, f, b, bold)
                c += 1
            pos = m.end()
            codes = [int(x) if x else 0 for x in m.group(1).split(";")]
            i = 0
            while i < len(codes):
                k = codes[i]
                if k == 0:
                    fg, bg, bold, dim, rev = FG, None, False, False, False
                elif k == 1:
                    bold = True
                elif k == 2:
                    dim = True
                elif k == 7:
                    rev = True
                elif k == 22:
                    bold = dim = False
                elif k == 27:
                    rev = False
                elif 30 <= k <= 37:
                    fg = BASE16[k - 30]
                elif 90 <= k <= 97:
                    fg = BASE16[k - 90 + 8]
                elif 40 <= k <= 47:
                    bg = BASE16[k - 40]
                elif 100 <= k <= 107:
                    bg = BASE16[k - 100 + 8]
                elif k == 39:
                    fg = FG
                elif k == 49:
                    bg = None
                elif k in (38, 48) and i + 2 < len(codes) and codes[i + 1] == 5:
                    if k == 38:
                        fg = xterm(codes[i + 2])
                    else:
                        bg = xterm(codes[i + 2])
                    i += 2
                elif k in (38, 48) and i + 4 < len(codes) and codes[i + 1] == 2:
                    col = tuple(codes[i + 2:i + 5])
                    if k == 38:
                        fg = col
                    else:
                        bg = col
                    i += 4
                i += 1
        for ch in line[pos:]:
            if c < cols:
                f, b = (bg or BG, fg) if rev else (fg, bg)
                if dim:
                    f = tuple(int(v * 0.6) for v in f)
                grid[r][c] = (ch, f, b, bold)
            c += 1
    return grid


def render(text, cols, rows, size, width=None, height=None, caption=None):
    font = ImageFont.truetype(FONT, size)
    fontb = ImageFont.truetype(FONT_BOLD, size)
    cw = font.getlength("M")
    asc, desc = font.getmetrics()
    ch = asc + desc
    gw, gh = int(cw * cols + 0.5), ch * rows
    cap_h = int(size * 3.2) if caption else 0
    W = width or gw + 2 * size
    H = height or gh + 2 * size + cap_h
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    ox, oy = (W - gw) // 2, (H - cap_h - gh) // 2
    grid = parse(text, cols, rows)
    for r in range(rows):
        for c in range(cols):
            chv, f, b, bold = grid[r][c]
            x0, x1 = ox + int(c * cw), ox + int((c + 1) * cw)
            if b:
                d.rectangle([x0, oy + r * ch, x1 - 1, oy + (r + 1) * ch - 1], fill=b)
    for r in range(rows):
        for c in range(cols):
            chv, f, b, bold = grid[r][c]
            if chv != " ":
                d.text((ox + c * cw, oy + r * ch), chv, font=fontb if bold else font, fill=f)
    if caption:
        capf = ImageFont.truetype(SANS_BOLD, int(size * 1.5))
        d.rectangle([0, H - cap_h, W, H], fill=(24, 28, 40))
        d.rectangle([0, H - cap_h, W, H - cap_h + 2], fill=(95, 175, 255))
        tw = d.textlength(caption, font=capf)
        d.text(((W - tw) / 2, H - cap_h + (cap_h - size * 1.5) / 2 - 2), caption, font=capf, fill=(235, 238, 245))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("out")
    ap.add_argument("--cols", type=int, default=170)
    ap.add_argument("--rows", type=int, default=48)
    ap.add_argument("--size", type=int, default=18)
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("--caption")
    a = ap.parse_args()
    text = open(a.capture, encoding="utf-8", errors="replace").read() if a.capture != "-" else sys.stdin.read()
    render(text, a.cols, a.rows, a.size, a.width, a.height, a.caption).save(a.out)


if __name__ == "__main__":
    main()
