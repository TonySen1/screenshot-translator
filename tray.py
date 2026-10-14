# -*- coding: utf-8 -*-
"""托盘图标。

pystray 有自己的消息循环，跑在单独的线程里；菜单回调也在这个线程里执行，
所以回调只做一件事：往主线程的队列丢事件（Tk 不是线程安全的）。
"""

from __future__ import annotations

import logging
import threading

from icons import tray_image

log = logging.getLogger(__name__)

APP_TITLE = "截图翻译"


class Tray:
    """系统托盘图标。右键出菜单，双击打开设置。"""

    def __init__(self, on_settings, on_capture, on_quit, hotkey_hint: str = ""):
        self._on_settings = on_settings
        self._on_capture = on_capture
        self._on_quit = on_quit
        self._hotkey_hint = hotkey_hint
        self._icon = None
        self._thread: threading.Thread | None = None

    def start(self) -> bool:
        try:
            import pystray
        except ImportError:
            log.warning("没有安装 pystray，跳过托盘图标")
            return False

        text = "截图翻译"
        if self._hotkey_hint:
            text += "（%s）" % self._hotkey_hint

        menu = pystray.Menu(
            pystray.MenuItem("打开设置", self._settings_clicked, default=True),
            pystray.MenuItem("截图翻译", self._capture_clicked),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("退出", self._quit_clicked),
        )

        try:
            self._icon = pystray.Icon("screenshot-translator", tray_image(64),
                                      text, menu)
        except Exception:
            log.exception("创建托盘图标失败")
            return False

        self._thread = threading.Thread(target=self._run, name="tray", daemon=True)
        self._thread.start()
        log.info("托盘图标已启动")
        return True

    def _run(self) -> None:
        try:
            self._icon.run()
        except Exception:
            log.exception("托盘图标运行异常")

    def stop(self) -> None:
        if self._icon is None:
            return
        try:
            self._icon.stop()
        except Exception:
            pass
        self._icon = None

    def update_title(self, hotkey: str) -> None:
        """快捷键改了之后刷新悬停提示。"""
        if self._icon is None:
            return
        text = "截图翻译"
        if hotkey:
            text += "（%s）" % hotkey
        try:
            self._icon.title = text
        except Exception:
            pass

    def notify(self, message: str) -> None:
        """弹一条气泡提示（不常用，出错时才提示一下）。"""
        if self._icon is None:
            return
        try:
            self._icon.notify(message, APP_TITLE)
        except Exception:
            pass

    # ---- 菜单回调（跑在托盘线程里，只丢事件） ----

    def _settings_clicked(self, *_args) -> None:
        self._on_settings()

    def _capture_clicked(self, *_args) -> None:
        self._on_capture()

    def _quit_clicked(self, *_args) -> None:
        self._on_quit()
