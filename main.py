# -*- coding: utf-8 -*-
"""极简截图翻译浮层 —— 程序入口。

启动后没有任何窗口，只在后台等全局快捷键：
  - 截图翻译（默认 Ctrl+Alt+D）
  - 退出程序（默认 Ctrl+Alt+Shift+Q）

热键回调跑在 keyboard 的钩子线程里，所以它们只往队列里丢事件，
真正的活儿全部由 Tk 主线程从队列里取出来执行（Tk 不是线程安全的）。
"""

from __future__ import annotations

import ctypes
import logging
import logging.handlers
import os
import queue
import sys
import tempfile
import threading
import time
import tkinter as tk
import traceback

import config as config_module
import screenshot
import translator
from overlay import Overlay, show_overlay
from settings_window import open_settings
from translator import TranslationResult
from tray import Tray

log = logging.getLogger("main")

APP_NAME = "截图翻译"
MUTEX_NAME = "ScreenshotTranslator_SingleInstance_Mutex"
LOG_NAME = "screenshot-translator.log"

# 事件类型
EVENT_CAPTURE = "capture"
EVENT_QUIT = "quit"
EVENT_TRANSLATED = "translated"
EVENT_ESCAPE = "escape"
EVENT_SETTINGS = "settings"


# --------------------------------------------------------------------------
# 基础设施
# --------------------------------------------------------------------------

def _patch_stdio() -> None:
    """--noconsole 打包后 stdout/stderr 是 None，先兜住再做别的事。"""
    for name in ("stdout", "stderr"):
        if getattr(sys, name, None) is None:
            try:
                setattr(sys, name, open(os.devnull, "w", encoding="utf-8"))
            except OSError:
                pass


def _log_dir() -> str:
    """优先写在程序旁边，不行就退到用户目录、临时目录。"""
    candidates = [
        config_module.app_dir(),
        os.path.join(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir(),
                     "ScreenshotTranslator"),
        tempfile.gettempdir(),
    ]
    for candidate in candidates:
        try:
            os.makedirs(candidate, exist_ok=True)
            probe = os.path.join(candidate, ".write_test")
            with open(probe, "w", encoding="utf-8") as handle:
                handle.write("ok")
            os.remove(probe)
            return candidate
        except OSError:
            continue
    return tempfile.gettempdir()


def setup_logging() -> str:
    """把日志写进文件。程序没有控制台，出问题只能靠这个排查。"""
    path = os.path.join(_log_dir(), LOG_NAME)
    handler = logging.handlers.RotatingFileHandler(
        path, maxBytes=512 * 1024, backupCount=2, encoding="utf-8")
    handler.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s"))
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    root.handlers.clear()
    root.addHandler(handler)
    return path


def acquire_single_instance() -> bool:
    """防止重复双击启动多个实例（否则热键会互相打架）。

    注意必须用 use_last_error=True 的 WinDLL：ctypes.windll 拿到的
    GetLastError 可能已被 ctypes 内部调用覆盖，判断会失效。
    """
    if sys.platform != "win32":
        return True
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_wchar_p]
        kernel32.CreateMutexW.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

        handle = kernel32.CreateMutexW(None, False, MUTEX_NAME)
        last_error = ctypes.get_last_error()
        if not handle:
            log.warning("CreateMutexW 失败（错误码 %s），跳过单实例检查", last_error)
            return True
        if last_error == 183:  # ERROR_ALREADY_EXISTS
            kernel32.CloseHandle(handle)
            return False
        # 句柄故意不释放：进程活着，互斥体就活着
        acquire_single_instance._handle = handle  # type: ignore[attr-defined]
        return True
    except Exception:
        log.exception("单实例检查失败，按新实例继续运行")
        return True


def _install_exception_hooks() -> None:
    def handle(exc_type, exc_value, exc_tb):
        if issubclass(exc_type, KeyboardInterrupt):
            return
        log.critical("未捕获异常:\n%s",
                     "".join(traceback.format_exception(exc_type, exc_value, exc_tb)))

    sys.excepthook = handle

    def thread_handle(args):
        if issubclass(args.exc_type, SystemExit):
            return
        log.critical("子线程未捕获异常 (%s):\n%s", args.thread.name,
                     "".join(traceback.format_exception(
                         args.exc_type, args.exc_value, args.exc_traceback)))

    threading.excepthook = thread_handle


# --------------------------------------------------------------------------
# 主程序
# --------------------------------------------------------------------------

class App:
    def __init__(self, root: tk.Tk, cfg: config_module.Config):
        self.root = root
        self.cfg = cfg
        self.queue: queue.Queue = queue.Queue()
        self.mask: screenshot.MaskWindow | None = None
        self.overlay: Overlay | None = None
        self.capturing = False
        self.quitting = False
        self._hotkey_handles: list = []
        self._esc_handle = None
        self._esc_hooked = False
        self._pump_job = None
        self._screen: screenshot.Rect | None = None
        self.settings_open = False
        self.tray: Tray | None = None

        root.withdraw()
        # 让 Tk 自己的回调异常也进日志，而不是弹一个错误框/静默
        root.report_callback_exception = self._report_callback_exception

        self._register_hotkeys()
        self._start_tray()

    # ---------- 快捷键 ----------

    def _unregister_hotkeys(self) -> None:
        """摘掉所有全局热键。

        设置窗口里录制快捷键时必须先摘：截图热键是 suppress=True 注册的，
        会把按键从系统事件里吞掉，Tk 的输入框根本收不到 KeyPress。
        """
        if not self._hotkey_handles:
            return
        import keyboard

        for handle in self._hotkey_handles:
            try:
                keyboard.remove_hotkey(handle)
            except Exception:
                pass
        self._hotkey_handles.clear()

    def _register_hotkeys(self) -> None:
        import keyboard

        # 重新注册前先摘掉旧的（设置窗口改完快捷键会再走一次这里）
        self._unregister_hotkeys()

        def add(combo: str, event: str, suppress: bool) -> bool:
            try:
                handle = keyboard.add_hotkey(
                    combo, lambda: self.queue.put((event, None)), suppress=suppress)
                self._hotkey_handles.append(handle)
                log.info("已注册热键：%s", combo)
                return True
            except Exception as exc:
                log.error("热键 %s 注册失败：%s", combo, exc)
                return False

        if not add(self.cfg.hotkey, EVENT_CAPTURE, suppress=True):
            # 用户自定的快捷键不可用时，退回默认值，别让程序变成哑巴
            if self.cfg.hotkey.lower() != config_module.DEFAULTS["hotkey"]:
                add(config_module.DEFAULTS["hotkey"], EVENT_CAPTURE, suppress=True)

        if not add(self.cfg.exit_hotkey, EVENT_QUIT, suppress=False):
            if self.cfg.exit_hotkey.lower() != config_module.DEFAULTS["exit_hotkey"]:
                add(config_module.DEFAULTS["exit_hotkey"], EVENT_QUIT, suppress=False)

    def _start_tray(self) -> None:
        """托盘图标。右键出菜单，双击打开设置。

        pystray 没装或者启动失败都不影响主功能，只是少一个入口。
        """
        tray = Tray(
            on_settings=lambda: self.queue.put((EVENT_SETTINGS, None)),
            on_capture=lambda: self.queue.put((EVENT_CAPTURE, None)),
            on_quit=lambda: self.queue.put((EVENT_QUIT, None)),
            hotkey_hint=self.cfg.hotkey,
        )
        if tray.start():
            self.tray = tray

    def open_settings(self) -> None:
        """打开设置窗口（模态）。改完立刻生效，不用重启。"""
        if self.settings_open:
            return
        self.settings_open = True
        try:
            self._close_overlay()
            if self.mask is not None:
                self.mask.cancel()
                self.mask = None
            self._sync_escape_hook()

            # 让设置窗口里的快捷键录制能收到真实按键
            self._unregister_hotkeys()

            def on_save(new_cfg) -> None:
                config_module.save_config(new_cfg)
                self.cfg = new_cfg
                if self.tray is not None:
                    self.tray.update_title(self.cfg.hotkey)
                log.info("设置已更新：model=%s 遮罩=%.2f 浮层=%.2f 字号=%d",
                         new_cfg.model, new_cfg.mask_opacity,
                         new_cfg.overlay_opacity, new_cfg.font_size)

            open_settings(self.root, self.cfg, on_save)
        except Exception:
            log.exception("设置窗口异常")
        finally:
            self.settings_open = False
            # 不管有没有保存，都按当前配置把全局热键装回去
            self._register_hotkeys()
            self._sync_escape_hook()

    def _sync_escape_hook(self) -> None:
        """遮罩/浮层显示期间挂一个全局 Esc 兜底，保证 Esc 一定关得掉。

        窗口自身也绑了 Esc，这里是防止焦点被别的窗口抢走时按 Esc 没反应。
        不吞按键（suppress=False），所以不会影响别的程序。
        """
        wanted = bool(self.mask is not None or self.overlay is not None)
        if wanted == self._esc_hooked:
            return
        import keyboard

        if wanted:
            try:
                self._esc_handle = keyboard.add_hotkey(
                    "esc", lambda: self.queue.put((EVENT_ESCAPE, None)), suppress=False)
                self._esc_hooked = True
            except Exception:
                log.exception("注册 Esc 兜底热键失败（可以点浮层再按 Esc）")
        else:
            if self._esc_handle is not None:
                try:
                    keyboard.remove_hotkey(self._esc_handle)
                except Exception:
                    pass
                self._esc_handle = None
            self._esc_hooked = False

    # ---------- 事件泵 ----------

    def _report_callback_exception(self, exc_type, exc_value, exc_tb) -> None:
        log.error("Tk 回调异常:\n%s",
                  "".join(traceback.format_exception(exc_type, exc_value, exc_tb)))

    def run(self) -> None:
        self._pump_job = self.root.after(25, self._pump)
        self.root.mainloop()

    def _pump(self) -> None:
        if self.quitting:
            return
        try:
            while True:
                event, payload = self.queue.get_nowait()
                self._handle(event, payload)
                if self.quitting:
                    return
        except queue.Empty:
            pass
        except Exception:
            log.exception("事件处理出错")
        try:
            self._pump_job = self.root.after(25, self._pump)
        except tk.TclError:
            pass

    def _handle(self, event: str, payload) -> None:
        if event == EVENT_CAPTURE:
            self.start_capture()
        elif event == EVENT_QUIT:
            self.quit()
        elif event == EVENT_ESCAPE:
            self._on_escape()
        elif event == EVENT_TRANSLATED:
            self._on_translated(payload)
        elif event == EVENT_SETTINGS:
            self.open_settings()
        else:
            log.warning("未知事件：%s", event)

    def _on_escape(self) -> None:
        """Esc：优先取消截图遮罩，否则关闭浮层。"""
        if self.mask is not None:
            self.mask.cancel()
        elif self.overlay is not None and not self.overlay.closed:
            self.overlay.close()
            self.overlay = None
            self._sync_escape_hook()

    # ---------- 截图流程 ----------

    def start_capture(self) -> None:
        if self.settings_open:
            log.debug("设置窗口开着，忽略这次快捷键")
            return
        if self.capturing:
            log.debug("正在截图中，忽略这次快捷键")
            return
        self.capturing = True
        try:
            self._capture_once()
        except Exception:
            log.exception("截图流程异常")
        finally:
            self.capturing = False
            self._sync_escape_hook()

    def _capture_once(self) -> None:
        # 上一次的浮层要先消失，否则会被拍进新截图里
        self._close_overlay()
        self.root.update()
        time.sleep(0.05)

        try:
            image, area, monitors = screenshot.grab_virtual_screen()
        except Exception:
            log.exception("抓屏失败，静默退出截图模式")
            return
        self._screen = area

        selection = screenshot.select_region(
            self.root, image, area, self.cfg,
            on_open=self._on_mask_open, on_close=self._on_mask_close)

        if selection is None:
            log.debug("未选择区域")
            return

        # 从整屏图里裁出选区
        box = (selection.left - area.left, selection.top - area.top,
               selection.right - area.left, selection.bottom - area.top)
        try:
            crop = image.crop(box)
        except Exception:
            log.exception("裁剪选区失败")
            return
        if crop.width <= 0 or crop.height <= 0:
            log.debug("选区为空")
            return

        try:
            overlay = show_overlay(self.root, self.cfg, selection,
                                   screenshot.bounds_for(selection, monitors, area),
                                   on_close=self._on_overlay_closed)
        except Exception:
            log.exception("浮层创建失败")
            return
        self.overlay = overlay
        self._sync_escape_hook()

        if not self.cfg.has_api_key:
            log.warning("未填写 api_key")
            overlay.show_error("请先填 api_key（右键托盘图标 → 打开设置）")
            return

        threading.Thread(target=self._translate_worker, args=(crop, overlay),
                         name="translate", daemon=True).start()

    def _on_overlay_closed(self, overlay) -> None:
        """浮层被关掉了（Esc / 点 X / 右键），把引用和 Esc 兜底热键一起收掉。"""
        if self.overlay is overlay:
            self.overlay = None
        self._sync_escape_hook()

    def _on_mask_open(self, mask) -> None:
        self.mask = mask
        self._sync_escape_hook()

    def _on_mask_close(self, mask) -> None:
        if self.mask is mask:
            self.mask = None
        self._sync_escape_hook()

    def _translate_worker(self, crop, overlay: Overlay) -> None:
        result = TranslationResult(error="翻译失败")
        try:
            prepared = translator.prepare_image(crop)
            png_base64 = translator.image_to_png_base64(prepared)
            result = translator.translate_image(crop, self.cfg, png_base64)
        except Exception as exc:
            log.exception("翻译线程异常")
            result = TranslationResult(error="翻译失败，请检查 API 配置或网络")
        self.queue.put((EVENT_TRANSLATED, (overlay, result)))

    def _on_translated(self, payload) -> None:
        overlay, result = payload
        if overlay is not self.overlay or overlay.closed:
            log.debug("浮层已被关闭，丢弃这次翻译结果")
            return
        if result.ok:
            overlay.show_result(result.original_text, result.translated_text)
        else:
            overlay.show_error(result.error)

    def _close_overlay(self) -> None:
        if self.overlay is not None:
            try:
                self.overlay.close()
            except Exception:
                log.exception("关闭浮层失败")
            self.overlay = None
        self._sync_escape_hook()

    # ---------- 退出 ----------

    def quit(self) -> None:
        if self.quitting:
            return
        self.quitting = True
        log.info("退出程序")
        import keyboard

        if self.tray is not None:
            try:
                self.tray.stop()
            except Exception:
                pass
            self.tray = None

        try:
            keyboard.unhook_all()
        except Exception:
            pass
        try:
            if self.mask is not None:
                self.mask.cancel()
            self._close_overlay()
        except Exception:
            pass
        try:
            self.root.quit()
        except tk.TclError:
            pass
        try:
            self.root.destroy()
        except tk.TclError:
            pass


# --------------------------------------------------------------------------

def main() -> int:
    _patch_stdio()
    screenshot.enable_dpi_awareness()
    log_path = setup_logging()
    _install_exception_hooks()

    log.info("=" * 60)
    log.info("%s 启动，日志：%s", APP_NAME, log_path)

    if not acquire_single_instance():
        log.info("已有实例在运行，本次启动退出")
        return 0

    cfg = config_module.load_config()

    try:
        root = tk.Tk()
    except Exception:
        log.exception("无法创建 Tk 窗口，程序退出")
        return 1
    root.title(APP_NAME)

    try:
        app = App(root, cfg)
        app.run()
    except Exception:
        log.exception("主循环异常退出")
        return 1
    log.info("程序已退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
