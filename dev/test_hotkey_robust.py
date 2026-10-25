# -*- coding: utf-8 -*-
"""快捷键健壮性测试。只调内部方法，不接管鼠标键盘。

覆盖：
  - 连按快捷键不会让队列堆积
  - 正在截图 / 设置窗口开着时，按快捷键被忽略
  - Esc 兜底是常驻的，不随某次截图增删
  - 钩子自检失败两次会触发重装
"""

import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config as config_module  # noqa: E402
import main as main_module  # noqa: E402
import screenshot  # noqa: E402
import tray as tray_module  # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def main() -> int:
    screenshot.enable_dpi_awareness()
    tray_module.Tray.start = lambda self: False

    cfg = config_module.load_config(
        os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "config.json"))
    cfg.api_key = ''

    root = tk.Tk()
    root.withdraw()
    app = main_module.App(root, cfg)

    try:
        # ---- 注册了哪些热键 ----
        # 截图 + 退出 + 常驻 Esc
        check("注册了 3 个热键（截图/退出/Esc）",
              len(app._hotkey_handles) == 3, "%d 个" % len(app._hotkey_handles))
        check("Esc 兜底已常驻", app._uses_escape() is False)

        # ---- 队列去重 ----
        while not app.queue.empty():
            app.queue.get_nowait()
        app._capture_pending.clear()
        app.capturing = False
        app.settings_open = False

        for _ in range(5):
            app._dispatch(main_module.EVENT_CAPTURE)
        check("连按 5 次只入队 1 个截图请求", app.queue.qsize() == 1,
              "队列 %d" % app.queue.qsize())

        # 模拟主线程处理掉它
        app.queue.get_nowait()
        app._capture_pending.clear()
        app._dispatch(main_module.EVENT_CAPTURE)
        check("处理完之后可以再次入队", app.queue.qsize() == 1,
              "队列 %d" % app.queue.qsize())
        app.queue.get_nowait()
        app._capture_pending.clear()

        # ---- 正在截图时忽略 ----
        app.capturing = True
        app._dispatch(main_module.EVENT_CAPTURE)
        check("截图过程中按快捷键被忽略", app.queue.qsize() == 0,
              "队列 %d" % app.queue.qsize())
        app.capturing = False

        # ---- 设置窗口打开时忽略 ----
        app.settings_open = True
        app._dispatch(main_module.EVENT_CAPTURE)
        check("设置窗口开着时按快捷键被忽略", app.queue.qsize() == 0,
              "队列 %d" % app.queue.qsize())
        app.settings_open = False

        # ---- 退出热键不受去重影响 ----
        app._dispatch(main_module.EVENT_QUIT)
        app._dispatch(main_module.EVENT_QUIT)
        check("退出事件不去重（照常入队）", app.queue.qsize() == 2,
              "队列 %d" % app.queue.qsize())
        while not app.queue.empty():
            app.queue.get_nowait()

        # ---- Esc 常驻：反复截图不会增删 esc ----
        before = len(app._hotkey_handles)
        for _ in range(10):
            app.mask = object()
            app._on_mask_open(app.mask)
            app.mask = None
            app._on_mask_close(None)
        check("反复开关遮罩不改变热键数量", len(app._hotkey_handles) == before,
              "%d -> %d" % (before, len(app._hotkey_handles)))

        # ---- 监听线程健康检查 ----
        import keyboard
        recovered = []
        original_recover = app._recover_hooks
        app._recover_hooks = lambda: recovered.append(1)

        app._probe_failures = 0
        app._probe()                 # 真实 listener 是好的
        check("监听线程正常时不误判", app._probe_failures == 0,
              "failures=%d" % app._probe_failures)

        real_listener = keyboard._listener
        try:
            class DeadThread:
                @staticmethod
                def is_alive():
                    return False

            class DeadListener:
                listening = True
                listening_thread = DeadThread()
            keyboard._listener = DeadListener()
            app._probe_failures = 0
            app._probe()
            check("检测到线程异常先记一次", app._probe_failures == 1 and not recovered,
                  "failures=%d" % app._probe_failures)
            app._probe()
            check("连续两次异常触发重装", len(recovered) == 1, str(recovered))
        finally:
            keyboard._listener = real_listener

        app._recover_hooks = original_recover
        app._probe_job_cancel = None
        if app._probe_job is not None:
            root.after_cancel(app._probe_job)
            app._probe_job = None

        # ---- 自定义 Esc 热键时不重复注册 ----
        app.cfg.hotkey = "ctrl+esc"
        check("识别出自定义热键里用到了 Esc", app._uses_escape() is True)
        app.cfg.hotkey = "ctrl+alt+d"
        check("正常快捷键不会被误判", app._uses_escape() is False)
    finally:
        app.quitting = True
        if app._probe_job is not None:
            try:
                root.after_cancel(app._probe_job)
            except tk.TclError:
                pass
        app.quit()
        try:
            root.destroy()
        except tk.TclError:
            pass

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
