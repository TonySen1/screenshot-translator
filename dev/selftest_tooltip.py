# -*- coding: utf-8 -*-
"""自测：新浮层的小图标 + 圆角 + 悬停提示。"""

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


def main() -> int:
    screenshot.enable_dpi_awareness()
    cfg = config_module.load_config(os.path.join(HERE, "..", "config.json"))
    root = tk.Tk()
    root.withdraw()

    image, area, monitors = screenshot.grab_virtual_screen()
    sel = screenshot.Rect(300, 200, 700, 120)
    overlay = show_overlay(root, cfg, sel, area)
    overlay.show_result("The quick brown fox", "敏捷的棕色狐狸")
    root.update()

    geom = {}

    def report():
        root.update_idletasks()
        geom.update(x=overlay.win.winfo_x(), y=overlay.win.winfo_y(),
                    w=overlay.win.winfo_width(), h=overlay.win.winfo_height())
        print("浮层:", geom, "图标尺寸:", overlay._icon_size, flush=True)
        # 模拟鼠标悬到“复制翻译”图标上
        overlay.copy_button.canvas.event_generate("<Enter>")

    root.after(400, report)

    def shoot():
        time.sleep(1.2)
        with mss.MSS() as sct:
            img = sct.grab(sct.monitors[0])
            full = Image.frombytes("RGB", img.size, img.bgra, "raw", "BGRX")
        x, y = geom.get("x", 0), geom.get("y", 0) + 588
        w, h = geom.get("w", 300), geom.get("h", 90)
        crop = full.crop((max(0, x - 20), max(0, y - 60),
                          min(full.width, x + w + 20), min(full.height, y + h + 80)))
        crop = crop.resize((crop.width * 2, crop.height * 2), Image.LANCZOS)
        crop.save(os.path.join(HERE, "shot_tooltip.png"))
        print("saved shot_tooltip.png", flush=True)

    threading.Thread(target=shoot, daemon=True).start()
    root.after(2600, lambda: (overlay.close(), root.quit()))
    root.mainloop()
    root.destroy()
    print("done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
