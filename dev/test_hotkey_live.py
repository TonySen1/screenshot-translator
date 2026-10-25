# -*- coding: utf-8 -*-
"""集成测试：全局热键注销后，真实按键能不能到达设置窗口的录制框。

这是这次改动的核心 —— 截图热键是 suppress=True 注册的，不注销的话
按键会被钩子吞掉，录制框永远收不到 KeyPress。
"""

import os
import sys
import time
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import keyboard  # noqa: E402

import config as config_module  # noqa: E402
import main as main_module  # noqa: E402
import screenshot  # noqa: E402
import tray as tray_module  # noqa: E402
from settings_window import SettingsWindow, install_styles  # noqa: E402
from winutil import bring_to_front  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(HERE, "config.json")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def main() -> int:
    screenshot.enable_dpi_awareness()

    backup = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as fh:
            backup = fh.read()

    original_start = tray_module.Tray.start
    tray_module.Tray.start = lambda self: False

    root = None
    try:
        cfg = config_module.load_config(CONFIG)
        cfg.api_key = ""
        root = tk.Tk()
        root.withdraw()

        app = main_module.App(root, cfg)
        check("启动时注册了 2 个全局热键", len(app._hotkey_handles) == 2,
              "%d 个" % len(app._hotkey_handles))

        # 模拟「打开设置窗口」做的事
        app._unregister_hotkeys()
        check("注销后没有全局热键", len(app._hotkey_handles) == 0,
              "%d 个" % len(app._hotkey_handles))

        install_styles(root)
        settings = SettingsWindow(root, cfg)
        settings.win.deiconify()
        settings.win.update()
        settings.win.focus_force()
        # 和 SettingsWindow.show() 里一样，把它提到系统前台，
        # 否则键盘消息根本到不了这个窗口
        brought = bring_to_front(settings.win)
        root.update()
        check("设置窗口已成为系统前台窗口", brought)

        entry = settings._hotkey
        entry.event_generate("<Button-1>", x=8, y=8)
        root.update()
        check("点击后进入录制", entry.recording is True)

        entry.focus_force()
        root.update()
        time.sleep(0.25)

        # 真实按键 —— 走全局键盘钩子，和用户手按一样
        keyboard.press_and_release("ctrl+alt+k")
        time.sleep(0.5)
        root.update()

        check("真实按键被录制到", entry.get() == "ctrl+alt+k", repr(entry.get()))
        check("录完退出录制状态", entry.recording is False)

        # 退出程序热键框也试一次，这次带 Shift
        entry2 = settings._exit_hotkey
        entry2.event_generate("<Button-1>", x=8, y=8)
        root.update()
        entry2.focus_force()
        root.update()
        time.sleep(0.25)
        keyboard.press_and_release("ctrl+shift+q")
        time.sleep(0.5)
        root.update()
        check("第二个框也能录", entry2.get() == "ctrl+shift+q", repr(entry2.get()))

        settings._close()
        root.update()

        # 关掉设置窗口后要把热键装回去
        app._register_hotkeys()
        check("恢复后又有 2 个全局热键", len(app._hotkey_handles) == 2,
              "%d 个" % len(app._hotkey_handles))

        app.quit()
    finally:
        tray_module.Tray.start = original_start
        try:
            keyboard.unhook_all()
        except Exception:
            pass
        if root is not None:
            try:
                root.destroy()
            except tk.TclError:
                pass
        if backup is not None:
            with open(CONFIG, "wb") as fh:
                fh.write(backup)
            print("config.json 已恢复", flush=True)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
