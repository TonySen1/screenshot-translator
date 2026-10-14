# -*- coding: utf-8 -*-
"""图标绘制。

浮层上的按钮图标用 tkinter Canvas 现画（线条），这样悬停变色、跟随 DPI 缩放都很方便，
也不用额外带图片文件。托盘图标用 Pillow 生成，因为 pystray 要的是 PIL Image。
"""

from __future__ import annotations

import tkinter as tk

from PIL import Image, ImageDraw

# 所有图标都画在 0..20 的坐标系里，再按实际尺寸缩放
_BOX = 20.0

# 托盘图标配色（和浮层一致）
_TRAY_BG = "#1e1e1e"
_TRAY_ACCENT = "#3d9bff"
_TRAY_FG = "#ffffff"


def _s(value: float, size: int) -> float:
    return value * size / _BOX


def draw_icon(canvas: tk.Canvas, kind: str, color: str, size: int,
              bg: str = "", tags: str = "icon") -> list[int]:
    """在 canvas 上画一个图标，返回所有 item id（用来批量改颜色）。

    kind: "extract"（提取文本）/ "copy"（复制）/ "close"（关闭）
    """
    width = max(1, round(size / 11))
    items: list[int] = []
    canvas.delete(tags)

    if kind == "extract":
        # 一张纸 + 三条文字线
        items.append(canvas.create_rectangle(
            _s(3.5, size), _s(2, size), _s(16.5, size), _s(18, size),
            outline=color, width=width, tags=tags))
        for y, right in ((7.0, 13.5), (10.0, 13.5), (13.0, 10.0)):
            items.append(canvas.create_line(
                _s(6.5, size), _s(y, size), _s(right, size), _s(y, size),
                fill=color, width=width, capstyle="round", tags=tags))

    elif kind == "copy":
        # 两个错开的方块；前面那个填背景色，把后面的挡住一角
        items.append(canvas.create_rectangle(
            _s(7, size), _s(7, size), _s(18, size), _s(18, size),
            outline=color, width=width, tags=tags))
        items.append(canvas.create_rectangle(
            _s(2, size), _s(2, size), _s(13, size), _s(13, size),
            outline=color, width=width, fill=bg or "", tags=tags))

    elif kind == "close":
        # X 的两条线至少 2px：1px 的细线在深色底上带抗锯齿会发虚看不清。
        # 坐标取整是为了让线落在像素网格上，不然会糊成两像素的灰边。
        stroke = max(2, width)
        near = round(_s(5.5, size))
        far = round(_s(14.5, size))
        items.append(canvas.create_line(
            near, near, far, far,
            fill=color, width=stroke, capstyle="round", tags=tags))
        items.append(canvas.create_line(
            far, near, near, far,
            fill=color, width=stroke, capstyle="round", tags=tags))

    elif kind == "move":
        # 四向箭头，用作拖动提示（浮层上不显示，留给设置窗口用）
        cx, cy = _s(10, size), _s(10, size)
        items.append(canvas.create_line(
            _s(3, size), cy, _s(17, size), cy, fill=color, width=width, tags=tags))
        items.append(canvas.create_line(
            cx, _s(3, size), cx, _s(17, size), fill=color, width=width, tags=tags))

    return items


def tint_icon(canvas: tk.Canvas, items: list[int], color: str,
              fill_color: str = "") -> None:
    """给已经画好的图标换色。

    坑点：Canvas 的 line 图元**没有** -outline 选项，对它设 outline 会抛 TclError。
    如果把一圈图元套在同一个 try 里，第一条线抛异常就会让后面所有图元都更新不到，
    表现成"图标颜色永远不变"。所以这里逐个图元、按类型分开设置。
    """
    for item in items:
        try:
            if canvas.type(item) == "line":
                canvas.itemconfigure(item, fill=color)
            else:
                canvas.itemconfigure(item, outline=color, fill=fill_color)
        except tk.TclError:
            continue


def tray_image(size: int = 64) -> Image.Image:
    """生成托盘图标：深色圆角底 + 蓝色选框 + 白色文字线。"""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = size / 64.0

    def box(x0, y0, x1, y1):
        return [x0 * k, y0 * k, x1 * k, y1 * k]

    d.rounded_rectangle(box(2, 2, 62, 62), radius=int(14 * k), fill=_TRAY_BG)

    # 选框
    d.rectangle(box(10, 12, 54, 38), outline=_TRAY_ACCENT, width=max(1, int(4 * k)))
    d.rectangle(box(16, 18, 46, 23), fill=_TRAY_FG)
    d.rectangle(box(16, 28, 38, 33), fill=_TRAY_FG)

    # 译文线
    d.rectangle(box(10, 44, 54, 48), fill="#8a8a8a")
    d.rectangle(box(10, 53, 36, 57), fill="#8a8a8a")
    return img


def save_tray_ico(path: str) -> None:
    """把托盘图标存成 ico（调试用）。"""
    tray_image(64).save(path, format="ICO",
                        sizes=[(64, 64), (32, 32), (16, 16)])
