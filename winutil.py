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


def bring_to_front(widget) -> bool:
    """把窗口提到前台，让它真正能收到键盘消息。

    Tk 的 focus_force() 只改 Tk 内部的焦点，不等于 Windows 的前台窗口。
    不是前台窗口就收不到键盘消息，快捷键录制会"按了没反应"。
    SetForegroundWindow 平时会被系统拒绝（返回 0），得先把自己的线程
    和当前前台窗口的线程 AttachThreadInput 挂到一起才行。
    """
    if sys.platform != "win32":
        return False
    hwnd = top_hwnd(widget)
    if not hwnd:
        return False
    try:
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

        user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        user32.GetWindowThreadProcessId.restype = ctypes.c_ulong
        user32.AttachThreadInput.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_bool]
        user32.AttachThreadInput.restype = ctypes.c_bool
        user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
        user32.SetForegroundWindow.restype = ctypes.c_bool
        user32.BringWindowToTop.argtypes = [ctypes.c_void_p]
        user32.BringWindowToTop.restype = ctypes.c_bool
        user32.GetForegroundWindow.restype = ctypes.c_void_p
        kernel32.GetCurrentThreadId.restype = ctypes.c_ulong

        foreground = user32.GetForegroundWindow()
        if foreground == hwnd:
            return True

        fg_thread = user32.GetWindowThreadProcessId(foreground, None) if foreground else 0
        my_thread = kernel32.GetCurrentThreadId()
        attached = False
        if fg_thread and fg_thread != my_thread:
            attached = bool(user32.AttachThreadInput(fg_thread, my_thread, True))
        try:
            user32.BringWindowToTop(ctypes.c_void_p(hwnd))
            user32.SetForegroundWindow(ctypes.c_void_p(hwnd))
        finally:
            if attached:
                user32.AttachThreadInput(fg_thread, my_thread, False)

        return user32.GetForegroundWindow() == hwnd
    except Exception:
        log.debug("提到前台失败", exc_info=True)
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
