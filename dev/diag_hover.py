# -*- coding: utf-8 -*-
"""诊断：三个图标的悬停变红/变亮到底有没有生效。"""

import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config as config_module  # noqa: E402
import screenshot  # noqa: E402
from overlay import _ICON_CLOSE_HOVER, _ICON_FG, _ICON_FG_HOVER, show_overlay  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def color_of(button):
    """读按钮图标里所有 item 的颜色。"""
    out = []
    for item in button._items:
        kind = button.canvas.type(item)
        c = button.canvas.itemcget(item, "fill")
        if kind == "rectangle":
            c = button.canvas.itemcget(item, "outline") + "|" + c
        out.append("%s:%s" % (kind, c))
    return out


def main() -> int:
    screenshot.enable_dpi_awareness()
    cfg = config_module.load_config(os.path.join(HERE, "config.json"))
    root = tk.Tk()
    root.withdraw()
    screen = screenshot.Rect(0, -588, 4000, 2560)
    overlay = show_overlay(root, cfg, screenshot.Rect(500, 700, 400, 100), screen)
    overlay.show_result("Hello", "你好")
    root.update()

    for name, btn in (("提取", overlay.extract_button),
                      ("复制", overlay.copy_button),
                      ("关闭", overlay.close_button)):
        print("%s: enabled=%s color=%s items=%s"
              % (name, btn._enabled, btn._color, color_of(btn)), flush=True)

    print("\n--- 模拟鼠标移入 ---", flush=True)
    for name, btn, expect in (("提取", overlay.extract_button, _ICON_FG_HOVER),
                              ("复制", overlay.copy_button, _ICON_FG_HOVER),
                              ("关闭", overlay.close_button, _ICON_CLOSE_HOVER)):
        btn.canvas.event_generate("<Enter>")
        root.update()
        actual = color_of(btn)
        ok = any(expect in c for c in actual)
        print("[%s] %s 移入后期望 %s，实际 %s"
              % ("PASS" if ok else "FAIL", name, expect, actual), flush=True)
        btn.canvas.event_generate("<Leave>")
        root.update()

    print("\n--- 移出后 ---", flush=True)
    for name, btn in (("提取", overlay.extract_button),
                      ("复制", overlay.copy_button),
                      ("关闭", overlay.close_button)):
        print("%s: %s" % (name, color_of(btn)), flush=True)

    overlay.close()
    root.destroy()
    return 0


if __name__ == "__main__":
    sys.exit(main())
