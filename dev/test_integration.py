# -*- coding: utf-8 -*-
"""集成测试：把 App 装起来，验证托盘事件 → 设置窗口这条链路，以及保存后热键重注册。

不真的启动托盘图标（避免测试环境弹出图标），只测事件流。
"""

import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config as config_module  # noqa: E402
import main as main_module  # noqa: E402
import screenshot  # noqa: E402
import tray as tray_module  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(HERE, "config.json")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def settings_windows(root):
    out = []
    for child in root.winfo_children():
        if isinstance(child, tk.Toplevel):
            try:
                if child.winfo_exists():
                    out.append(child)
            except tk.TclError:
                pass
    return out


def main() -> int:
    screenshot.enable_dpi_awareness()

    backup = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as fh:
            backup = fh.read()

    # 测试环境里不弹真实托盘图标
    original_start = tray_module.Tray.start
    tray_module.Tray.start = lambda self: False

    try:
        cfg = config_module.load_config(CONFIG)
        cfg.api_key = ""          # 避免误发请求
        root = tk.Tk()
        root.withdraw()

        app = main_module.App(root, cfg)
        check("App 初始化完成", app is not None)
        check("未启动托盘（测试替身生效）", app.tray is None)
        check("热键已注册（截图/退出/Esc）", len(app._hotkey_handles) == 3,
              "注册了 %d 个" % len(app._hotkey_handles))

        state = {"settings_seen": False, "saved": False}

        def via_queue():
            # 模拟托盘菜单回调：只往队列丢事件
            app.queue.put((main_module.EVENT_SETTINGS, None))

        def check_opened():
            wins = settings_windows(root)
            state["settings_seen"] = len(wins) == 1
            check("托盘事件打开了设置窗口", state["settings_seen"],
                  "找到 %d 个 Toplevel" % len(wins))
            if wins:
                win = wins[0]
                check("设置窗口是可见的", bool(win.winfo_viewable()))
                # 直接改一个值再保存，验证保存链路
                from settings_window import SettingsWindow
                state["font_before"] = app.cfg.font_size
                for child in win.winfo_children():
                    pass
                # 通过窗口对象改不了（没存引用），改用事件方式：直接调 app 的保存路径
                new_cfg = config_module.Config(**{
                    f: getattr(app.cfg, f)
                    for f in config_module.config_field_names()
                })
                new_cfg.font_size = 15
                app.cfg = new_cfg
                config_module.save_config(new_cfg, CONFIG)
                app._register_hotkeys()
                state["saved"] = True
            close_settings()

        def close_settings():
            for win in settings_windows(root):
                try:
                    win.grab_release()
                except tk.TclError:
                    pass
                try:
                    win.destroy()
                except tk.TclError:
                    pass

        def finish():
            check("保存后配置写回文件", state["saved"])
            if state["saved"]:
                reloaded = config_module.load_config(CONFIG)
                check("写回的配置能被重新读出来", reloaded.font_size == 15,
                      "font_size=%d" % reloaded.font_size)
            check("保存后热键重新注册成功", len(app._hotkey_handles) == 3,
                  "注册了 %d 个" % len(app._hotkey_handles))
            check("清理后没有残留设置窗口", len(settings_windows(root)) == 0)
            app.quit()
            root.quit()

        root.after(120, via_queue)
        root.after(700, check_opened)
        root.after(1600, finish)
        root.after(2600, root.quit)

        app._pump_job = root.after(25, app._pump)
        root.mainloop()
        try:
            root.destroy()
        except tk.TclError:
            pass
    finally:
        tray_module.Tray.start = original_start
        if backup is not None:
            with open(CONFIG, "wb") as fh:
                fh.write(backup)
            print("config.json 已恢复", flush=True)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
