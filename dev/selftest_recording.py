# -*- coding: utf-8 -*-
"""自测：录制状态下的快捷键框长什么样。"""
import os, sys, threading, time, tkinter as tk
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import mss
from PIL import Image
import config as cm, screenshot
from settings_window import SettingsWindow, install_styles

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

screenshot.enable_dpi_awareness()
root = tk.Tk(); root.withdraw()
cfg = cm.load_config(os.path.join(HERE, "config.json"))
cfg.api_key = "sk-demo-placeholder"
install_styles(root)
s = SettingsWindow(root, cfg)
s.win.deiconify(); s.win.update(); root.update()
time.sleep(0.4)

geom = {}
def start():
    s.win.update_idletasks()
    geom.update(x=s.win.winfo_x(), y=s.win.winfo_y(),
                w=s.win.winfo_width(), h=s.win.winfo_height())
    e = s._hotkey
    e.event_generate("<Button-1>", x=8, y=8)
    root.update()
    e.event_generate("<KeyPress>", keysym="Control_L")
    e.event_generate("<KeyPress>", keysym="Alt_L")
    root.update()

def shoot():
    time.sleep(1.2)
    with mss.MSS() as sct:
        img = sct.grab(sct.monitors[0])
        full = Image.frombytes("RGB", img.size, img.bgra, "raw", "BGRX")
    x, y = geom["x"], geom["y"] + 588
    w, h = geom["w"], geom["h"]
    full.crop((x-10, y+h-190, x+w+10, y+h-60)).resize((int((w+20)*1.6), int(130*1.6)), Image.LANCZOS).save(
        os.path.join(HERE, "dev", "shot_recording.png"))
    print("saved")

root.after(300, start)
threading.Thread(target=shoot, daemon=True).start()
root.after(2200, lambda: (s._close(), root.quit()))
root.mainloop()
root.destroy()
