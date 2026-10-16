# -*- coding: utf-8 -*-
"""生成 README 用的效果图。

背景是用 Pillow 现画的一张假文档界面，不涉及真实桌面内容。
生成后图片放在 docs/ 下。

用法：python dev/make_screenshots.py
"""

import os
import sys
import threading
import time
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mss  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

import config as config_module  # noqa: E402
import screenshot  # noqa: E402
from overlay import show_overlay  # noqa: E402
from settings_window import SettingsWindow, install_styles  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(HERE, "docs")

W, H = 1000, 620
WIN_X, WIN_Y = 420, 260          # 假界面窗口在屏幕上的位置
# 四周留一圈纯色边距。裁图会带上这一圈，所以它必须是自己画出来的窗口 ——
# 让 pad 超出窗口边界的话，裁到的就是真实桌面（终端、游戏、别的窗口全漏进来）。
PAD = 16
MARGIN_BG = "#1e1e1e"            # 边距颜色，和浮层底色一致

# 文档正文（假的英文内容，用来演示"截英文翻中文"）
LINES = [
    ("h", "Getting Started"),
    ("p", "Before you begin, make sure the application is installed and the"),
    ("p", "configuration file has been created in the same folder."),
    ("s", ""),
    ("p", "Automatic updates are currently enabled. The application checks"),
    ("p", "for new versions every time it starts."),
    ("s", ""),
    ("h", "Installation"),
    ("p", "1. Download the latest release from the official website."),
    ("p", "2. Run the installer and follow the on-screen instructions."),
    ("p", "3. Restart your computer to apply all changes."),
    ("s", ""),
    ("h", "Troubleshooting"),
    ("p", "If the application fails to start, check the log file located"),
    ("p", "next to the executable. Most problems are caused by a missing"),
    ("p", "or incorrect configuration file."),
]

ORIGINAL = "Automatic updates are currently enabled."
TRANSLATED = "自动更新当前处于开启状态。"


def font(size: int, bold: bool = False):
    names = ["segoeui.ttf", "arial.ttf"]
    if bold:
        names = ["segoeuib.ttf", "arialbd.ttf"] + names
    for name in names:
        try:
            return ImageFont.truetype("C:/Windows/Fonts/" + name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def make_fake_screen() -> Image.Image:
    """画一张看起来像文档窗口的图，当截图背景用。"""
    img = Image.new("RGB", (W, H), "#ffffff")
    d = ImageDraw.Draw(img)

    # 窗口标题栏
    d.rectangle([0, 0, W, 46], fill="#2f2f31")
    for i, color in enumerate(("#ff5f56", "#ffbd2e", "#27c93f")):
        cx = 24 + i * 22
        d.ellipse([cx - 6, 17, cx + 6, 29], fill=color)
    d.text((104, 15), "Product Documentation", fill="#dcdcdc", font=font(17))

    # 正文
    y = 86
    for kind, text in LINES:
        if kind == "h":
            d.text((56, y), text, fill="#1b1b1b", font=font(20, bold=True))
            y += 26
            d.line([56, y, W - 56, y], fill="#e2e2e2", width=1)
            y += 18
        elif kind == "s":
            y += 14
        else:
            d.text((56, y), text, fill="#444444", font=font(16))
            y += 27
    return img


def grab(full: bool = True) -> Image.Image:
    with mss.MSS() as sct:
        shot = sct.grab(sct.monitors[0])
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def crop_window(full: Image.Image, x: int, y: int, w: int, h: int,
                pad: int = 0) -> Image.Image:
    """按虚拟屏坐标裁剪。

    x/y 是虚拟屏坐标；抓屏图的 y=0 对应虚拟屏的顶部，
    而虚拟屏顶部在绝对坐标里是负数（多显示器时），所以要补回这个偏移。
    """
    with mss.MSS() as sct:
        offset_y = -int(sct.monitors[0]["top"])
    return full.crop((x - pad, y + offset_y - pad, x + w + pad, y + h + offset_y + pad))


def main() -> int:
    os.makedirs(DOCS, exist_ok=True)
    screenshot.enable_dpi_awareness()

    fake = make_fake_screen()
    fake.save(os.path.join(DOCS, "_bg.png"))

    cfg = config_module.load_config(os.path.join(HERE, "config.json"))
    # 演示图会进仓库，绝不能带上真实 Key —— 这里强制换成占位符
    cfg.api_key = "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    cfg.font_size = 12
    cfg.overlay_opacity = 0.94      # 演示图里让浮层实一点，看得清
    cfg.mask_opacity = 0.35

    root = tk.Tk()
    root.withdraw()

    # 用一个无边框窗口把假界面铺出来当底
    backdrop = tk.Toplevel(root)
    backdrop.overrideredirect(True)
    backdrop.attributes("-topmost", True)
    backdrop.configure(bg=MARGIN_BG)
    # 铺满整屏，不只是为了那圈边距：屏幕上随时可能有别的 topmost 窗口
    # （游戏 HUD、输入法候选框）盖在假界面上方，铺满 + 置顶才能压住它们。
    backdrop.geometry("%dx%d+0+0" % (root.winfo_screenwidth(),
                                     root.winfo_screenheight()))
    photo = None
    try:
        from PIL import ImageTk
        photo = ImageTk.PhotoImage(fake, master=backdrop)
        tk.Label(backdrop, image=photo, bd=0,
                 bg=MARGIN_BG).place(x=WIN_X, y=WIN_Y)
    except Exception as exc:
        print("贴背景失败:", exc)
    backdrop.update()
    time.sleep(0.6)

    # ---------- 1. 遮罩 + 框选（这张用 PIL 合成，避免拍到真实桌面）----------
    dimmed = Image.blend(fake, Image.new("RGB", fake.size, "#000000"), 1 - cfg.mask_opacity)
    sel = (150, 300, 690, 420)      # 框选区域（相对背景图）
    highlight = fake.crop(sel)
    dimmed.paste(highlight, (sel[0], sel[1]))
    d = ImageDraw.Draw(dimmed)
    d.rectangle([sel[0], sel[1], sel[2] - 1, sel[3] - 1], outline="#3d9bff", width=2)
    dimmed.save(os.path.join(DOCS, "screenshot-mask.png"))
    print("saved docs/screenshot-mask.png", flush=True)

    # ---------- 2. 译文浮层（真实浮层叠在假背景上）----------
    sel_rect = screenshot.Rect(WIN_X + 150, WIN_Y + 300, 540, 120)
    overlay = show_overlay(root, cfg, sel_rect,
                           screenshot.Rect(0, -588, 4000, 2560))
    overlay.show_result(ORIGINAL, TRANSLATED)
    root.update()
    time.sleep(0.9)
    # 从下到上重新抬一遍。只保证「浮层在最上」不够 —— 屏幕上随时可能有
    # 第三方 topmost 窗口（游戏 HUD 之类）插进 backdrop 和浮层之间。
    backdrop.lift()
    overlay.win.lift()
    overlay.button_win.lift()
    root.update()
    time.sleep(0.5)
    shot = grab()
    crop_window(shot, WIN_X, WIN_Y, W, H, pad=PAD).save(
        os.path.join(DOCS, "screenshot-overlay.png"))
    print("saved docs/screenshot-overlay.png", flush=True)
    overlay.close()
    root.update()
    time.sleep(0.3)

    # ---------- 3. 设置窗口 ----------
    install_styles(root)
    sx, sy = WIN_X + 230, WIN_Y - 40
    win = SettingsWindow(root, cfg)
    win.win.geometry("+%d+%d" % (sx, sy))
    win.win.deiconify()
    root.update()
    gw = win.win.winfo_width()
    gh = win.win.winfo_height()

    # 同样铺满整屏，理由同 backdrop
    margin = tk.Toplevel(root)
    margin.overrideredirect(True)
    margin.attributes("-topmost", True)
    margin.configure(bg=MARGIN_BG)
    margin.geometry("%dx%d+0+0" % (root.winfo_screenwidth(),
                                   root.winfo_screenheight()))
    margin.update()

    # 同样从下到上抬一遍
    backdrop.lift()
    margin.lift()
    win.win.lift()
    root.update()
    time.sleep(0.8)
    shot2 = grab()
    crop_window(shot2, sx, sy, gw, gh, pad=PAD).save(
        os.path.join(DOCS, "screenshot-settings.png"))
    print("saved docs/screenshot-settings.png", flush=True)
    margin.destroy()
    win._close()

    backdrop.destroy()
    root.destroy()
    os.remove(os.path.join(DOCS, "_bg.png"))

    for name in ("screenshot-mask.png", "screenshot-overlay.png",
                 "screenshot-settings.png"):
        path = os.path.join(DOCS, name)
        print("  %-28s %5d KB  %s" % (name, os.path.getsize(path) // 1024,
                                      Image.open(path).size), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
