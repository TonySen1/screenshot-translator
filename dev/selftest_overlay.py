# -*- coding: utf-8 -*-
"""开发自测：把结果浮层画出来，抓屏存成 PNG，方便肉眼检查排版。

用法：python dev/selftest_overlay.py [场景名]
场景：normal（默认）/ long（超长文本）/ error（错误提示）/ loading（加载中）
"""

import os
import sys
import threading
import time
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mss  # noqa: E402
from PIL import Image  # noqa: E402

import config as config_module  # noqa: E402
import screenshot  # noqa: E402
from overlay import show_overlay  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

SCENARIOS = {
    "normal": (
        "The quick brown fox jumps over the lazy dog.\n"
        "Screenshot translation should feel like a temporary layer on your screen.",
        "敏捷的棕色狐狸跳过了那只懒狗。\n"
        "截图翻译应该像屏幕上临时贴出来的一层译文。",
    ),
    "long": (
        "Artificial intelligence is transforming the way we work. " * 12,
        "人工智能正在改变我们的工作方式。它让机器能够理解图像、语音和自然语言，"
        "并在此基础上完成翻译、摘要、问答等任务。对于普通用户来说，最直观的体验"
        "就是：对着屏幕框一块区域，立刻看到中文。\n\n"
        "这段话故意写得很长，用来检查浮层在文本超长时会不会撑出屏幕、"
        "字号会不会自动缩小、以及按钮会不会被挤出可视区域。"
        "如果这段话你全部能看见，说明高度自适应是正常的。" * 3,
    ),
    "error": ("", ""),
    "loading": ("", ""),
}


def capture_after(delay: float, out_path: str, region: dict) -> None:
    """延时抓屏，只裁出目标窗口周围一小块，避免把整个桌面存下来。"""
    def work():
        time.sleep(delay)
        with mss.MSS() as sct:
            shot = sct.grab(sct.monitors[0])
            full = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
        pad = 30
        box = (max(0, region["x"] - pad), max(0, region["y"] - pad),
               min(full.width, region["x"] + region["w"] + pad),
               min(full.height, region["y"] + region["h"] + pad))
        full.crop(box).save(out_path)
        print("saved:", out_path, "crop:", box, flush=True)

    threading.Thread(target=work, daemon=True).start()


def main() -> int:
    scenario = sys.argv[1] if len(sys.argv) > 1 else "normal"
    original, translated = SCENARIOS.get(scenario, SCENARIOS["normal"])

    print("DPI:", screenshot.enable_dpi_awareness(), flush=True)
    cfg = config_module.load_config(os.path.join(HERE, "_selftest_config.json"))
    cfg.overlay_bg = "#1e1e1e"
    cfg.overlay_fg = "#ffffff"

    root = tk.Tk()
    root.withdraw()

    image, area, monitors = screenshot.grab_virtual_screen()
    print("虚拟屏幕:", area, flush=True)

    # 在主屏中间放一个假的选区
    sel = screenshot.Rect(300, 200, 700, 120)
    overlay = show_overlay(root, cfg, sel, area)

    if scenario == "error":
        overlay.show_error("请先在 config.json 中填写 api_key")
    elif scenario == "loading":
        pass
    else:
        overlay.show_result(original, translated)

    geom = {"x": 0, "y": 0, "w": 700, "h": 200}

    def report():
        root.update_idletasks()
        print("geometry:", overlay.win.winfo_geometry(), flush=True)
        print("text reqheight:", overlay.text_label.winfo_reqheight(), flush=True)
        print("font size:", overlay._body_font.cget("size"), flush=True)
        geom.update(x=overlay.win.winfo_x() - area.left,
                    y=overlay.win.winfo_y() - area.top,
                    w=overlay.win.winfo_width(), h=overlay.win.winfo_height())

    root.after(300, report)

    out = os.path.join(HERE, "shot_overlay_%s.png" % scenario)
    capture_after(0.9, out, geom)
    root.after(2200, lambda: (overlay.close(), root.quit()))
    root.mainloop()
    print("done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
