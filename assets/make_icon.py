# -*- coding: utf-8 -*-
"""生成 assets/icon.ico（多尺寸）。改完图标跑一次：python assets/make_icon.py"""

import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "icon.ico")

SIZE = 256
BG = "#1e1e1e"
ACCENT = "#3d9bff"
FG = "#ffffff"
DIM = "#8a8a8a"


def draw_icon(size: int) -> Image.Image:
    scale = size / 256.0
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def box(x0, y0, x1, y1):
        return [x0 * scale, y0 * scale, x1 * scale, y1 * scale]

    def width(value):
        return max(1, round(value * scale))

    # 深色圆角底板
    d.rounded_rectangle(box(6, 6, 250, 250), radius=int(52 * scale),
                        fill=BG, outline=ACCENT, width=width(7))

    # 上：被框选住的原文（白线）
    d.rectangle(box(52, 54, 204, 142), outline=ACCENT, width=width(9))
    d.rectangle(box(74, 78, 182, 92), fill=FG)
    d.rectangle(box(74, 108, 140, 122), fill=FG)

    # 下：译文（灰线）
    d.rectangle(box(52, 168, 204, 182), fill=DIM)
    d.rectangle(box(52, 198, 152, 212), fill=DIM)

    return img


def main() -> int:
    base = draw_icon(SIZE)
    sizes = [(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)]
    base.save(OUT, format="ICO", sizes=sizes)
    print("已生成", OUT, os.path.getsize(OUT), "字节")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
