# -*- coding: utf-8 -*-
"""Windows 窗口小工具：拿到真正的顶层 HWND、给无边框窗口加 Win11 圆角。

Tk 的 `winfo_id()` 对 Toplevel 返回的不一定是最终那个顶层窗口，
要再往上找一层（GA_ROOT）才能拿到 DWM 认的那个。

圆角只在 Windows 11 上有效；老系统上这些调用会失败，调用方静默忽略即可。
"""

from __future__ import annotations

import ctypes
import logging
import sys

log = logging.getLogger(__name__)

_DWMWA_WINDOW_CORNER_PREFERENCE = 33
_DWMWCP_ROUND = 2
_DWMWCP_ROUNDSMALL = 3

_GA_ROOT = 2


def top_hwnd(widget) -> int:
    """取控件所属的顶层窗口句柄，失败返回 0。"""
    if sys.platform != "win32":
        return 0
    try:
        user32 = ctypes.windll.user32
        hwnd = widget.winfo_id()
        return user32.GetAncestor(hwnd, _GA_ROOT) or hwnd
    except Exception:
        return 0


def set_round_corners(widget, small: bool = True) -> bool:
    """给窗口加 Win11 圆角。small=True 用系统的小圆角档。"""
    if sys.platform != "win32":
        return False
    hwnd = top_hwnd(widget)
    if not hwnd:
        return False
    try:
        dwm = ctypes.WinDLL("dwmapi")
        preference = ctypes.c_int(_DWMWCP_ROUNDSMALL if small else _DWMWCP_ROUND)
        result = dwm.DwmSetWindowAttribute(
            ctypes.c_void_p(hwnd),
            ctypes.c_uint(_DWMWA_WINDOW_CORNER_PREFERENCE),
            ctypes.byref(preference),
            ctypes.sizeof(preference),
        )
        if result != 0:
            log.debug("DWM 圆角设置返回 %s（系统可能不支持）", result)
        return result == 0
    except Exception:
        log.debug("DWM 圆角不可用", exc_info=True)
        return False


def set_dark_titlebar(widget) -> bool:
    """把标题栏（如果有）设成深色，避免亮色边框破坏深色界面。"""
    if sys.platform != "win32":
        return False
    hwnd = top_hwnd(widget)
    if not hwnd:
        return False
    try:
        dwm = ctypes.WinDLL("dwmapi")
        value = ctypes.c_int(1)   # DWMWA_USE_IMMERSIVE_DARK_MODE = 20
        result = dwm.DwmSetWindowAttribute(ctypes.c_void_p(hwnd), ctypes.c_uint(20),
                                           ctypes.byref(value), ctypes.sizeof(value))
        return result == 0
    except Exception:
        return False
