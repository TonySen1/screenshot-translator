# -*- coding: utf-8 -*-
"""开发自测：把设置窗口画出来抓屏，看排版。"""

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
from settings_window import SettingsWindow, install_styles  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    screenshot.enable_dpi_awareness()
    cfg = config_module.load_config(os.path.join(HERE, "..", "config.json"))

    root = tk.Tk()
    root.withdraw()
    install_styles(root)

    win = SettingsWindow(root, cfg)
    win.win.deiconify()
    root.update_idletasks()

    geom = {}

    def report():
        root.update_idletasks()
        geom["x"] = win.win.winfo_x()
        geom["y"] = win.win.winfo_y()
        geom["w"] = win.win.winfo_width()
        geom["h"] = win.win.winfo_height()
        print("geometry:", win.win.winfo_geometry(), flush=True)

    def shoot():
        time.sleep(1.4)
        with mss.MSS() as sct:
            img = sct.grab(sct.monitors[0])
            full = Image.frombytes("RGB", img.size, img.bgra, "raw", "BGRX")
        x, y = geom.get("x", 0), geom.get("y", 0) + 588
        w, h = geom.get("w", 500), geom.get("h", 600)
        pad = 20
        full.crop((max(0, x - pad), max(0, y - pad),
                   min(full.width, x + w + pad), min(full.height, y + h + pad))).save(
            os.path.join(HERE, "shot_settings.png"))
        print("saved shot_settings.png", flush=True)

    root.after(400, report)
    threading.Thread(target=shoot, daemon=True).start()
    root.after(2600, win._close)
    win.show()
    root.destroy()
    print("done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
