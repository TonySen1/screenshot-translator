# -*- coding: utf-8 -*-
"""截图遮罩、鼠标框选、获取选区图片。

屏幕"冻结"的实现方式：先用 mss 抓一张整个虚拟屏幕（含所有显示器）的图，
再弹一个无边框窗口把这张图铺满。窗口本身不透明，但显示的是**压暗后**的截图，
所以视觉上就是"屏幕暗下来一层 + 画面静止"。框选区域会恢复原始亮度，
方便用户确认截的是哪一块。
"""

from __future__ import annotations

import ctypes
import logging
import sys
import tkinter as tk
from dataclasses import dataclass

import mss
from PIL import Image, ImageEnhance, ImageTk

log = logging.getLogger(__name__)

# mss 10 把 mss.mss 改名成了 mss.MSS，这里兼容新旧版本
_MSS_FACTORY = getattr(mss, "MSS", None) or mss.mss

# 小于这个像素的选区视为误触，直接当作取消
MIN_SELECTION = 8

# 拖拽时刷新高亮区域的最小间隔（毫秒），防止大选区拖动卡顿
_REFRESH_INTERVAL_MS = 16

# 选区边框颜色
_BORDER_COLOR = "#3d9bff"

# 遮罩淡入淡出：目的是消除"啪一下变暗/变亮"的闪烁感
_FADE_IN_STEPS = 10
_FADE_IN_INTERVAL_MS = 13      # 合计约 130ms
_FADE_OUT_STEPS = 5
_FADE_OUT_INTERVAL_MS = 11     # 合计约 55ms，收手要快一点


@dataclass(frozen=True)
class Rect:
    """虚拟屏幕上的一个矩形，单位是物理像素，left/top 可以是负数。"""

    left: int
    top: int
    width: int
    height: int

    @property
    def right(self) -> int:
        return self.left + self.width

    @property
    def bottom(self) -> int:
        return self.top + self.height

    def contains(self, x: int, y: int) -> bool:
        return self.left <= x < self.right and self.top <= y < self.bottom


def enable_dpi_awareness() -> str:
    """开启 DPI 感知，保证截图坐标和窗口坐标一致（必须在建 Tk 窗口之前调用）。"""
    if sys.platform != "win32":
        return "non-windows"

    # 1) 每显示器 DPI 感知 v2（Win10 1703+）
    try:
        if ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return "per-monitor-v2"
    except Exception:
        pass
    # 2) 每显示器 DPI 感知（Win8.1+）
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
        return "per-monitor"
    except Exception:
        pass
    # 3) 系统 DPI 感知（Vista+）
    try:
        ctypes.windll.user32.SetProcessDPIAware()
        return "system"
    except Exception:
        pass
    return "none"


def _rect_from_monitor(monitor: dict) -> Rect:
    return Rect(int(monitor["left"]), int(monitor["top"]),
                int(monitor["width"]), int(monitor["height"]))


def grab_virtual_screen() -> tuple[Image.Image, Rect, list[Rect]]:
    """抓取整个虚拟屏幕（所有显示器拼起来的那块区域）。

    返回 (PIL 图片, 虚拟屏幕矩形, 各显示器矩形列表)。
    """
    with _MSS_FACTORY() as sct:
        monitor = sct.monitors[0]  # monitors[0] = 所有显示器合并后的虚拟屏幕
        shot = sct.grab(monitor)
        image = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
        area = _rect_from_monitor(monitor)
        monitors = [_rect_from_monitor(m) for m in sct.monitors[1:]]
    log.info("屏幕已抓取：%sx%s @ (%s,%s)，共 %d 个显示器",
             area.width, area.height, area.left, area.top, len(monitors))
    return image, area, monitors


def bounds_for(selection: Rect, monitors: list[Rect], fallback: Rect) -> Rect:
    """挑一个用来摆放浮层的边界框。

    选区完全落在某一块显示器里时就用那块显示器，否则用整个虚拟屏幕。
    """
    for monitor in monitors:
        if (selection.left >= monitor.left and selection.right <= monitor.right
                and selection.top >= monitor.top and selection.bottom <= monitor.bottom):
            return monitor
    return fallback


class MaskWindow:
    """覆盖整个虚拟屏幕的框选遮罩。"""

    def __init__(self, parent: tk.Misc, image: Image.Image, area: Rect, cfg):
        self._parent = parent
        self._image = image
        self._area = area
        self._cfg = cfg
        self._result: Rect | None = None
        self._dragging = False
        self._start = (0, 0)
        self._current = (0, 0)
        self._refresh_job: str | None = None
        self._last_highlight: Rect | None = None
        self._closed = False
        self._fade_job: str | None = None
        self._fade_out_enabled = True
        self._bright_photo = None

        # 压暗后的整屏图（作为遮罩背景）
        brightness = max(0.05, min(1.0, float(cfg.mask_opacity)))
        self._dimmed = ImageEnhance.Brightness(image).enhance(brightness)

        self.win = tk.Toplevel(parent)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg="#000000")
        self.win.geometry("{}x{}+{}+{}".format(area.width, area.height, area.left, area.top))
        # 先透明着，等窗口显示出来再淡入 —— 直接弹一块暗屏会很闪
        self._set_alpha(0.0)

        self.canvas = tk.Canvas(self.win, highlightthickness=0, bd=0,
                                bg="#000000", cursor="crosshair")
        self.canvas.pack(fill="both", expand=True)

        # 用实例属性引用住 PhotoImage，否则会被 GC 回收变成空白
        self._dim_photo = ImageTk.PhotoImage(self._dimmed, master=self.win)
        self.canvas.create_image(0, 0, anchor="nw", image=self._dim_photo, tags="bg")

        self._highlight_id = self.canvas.create_image(0, 0, anchor="nw", tags="highlight")
        self._border_id = self.canvas.create_rectangle(
            0, 0, 0, 0, outline=_BORDER_COLOR, width=2, tags="border")
        self.canvas.itemconfigure("highlight", state="hidden")
        self.canvas.itemconfigure("border", state="hidden")

        self.canvas.bind("<Button-1>", self._on_press)
        self.canvas.bind("<B1-Motion>", self._on_drag)
        self.canvas.bind("<ButtonRelease-1>", self._on_release)
        for sequence in ("<Escape>", "<Button-3>", "<Button-2>"):
            self.win.bind(sequence, self._on_cancel)
            self.canvas.bind(sequence, self._on_cancel)

    # ---------- 对外接口 ----------

    def run(self) -> Rect | None:
        """显示遮罩并阻塞，直到用户完成框选或取消。返回全局坐标的选区。"""
        self.win.deiconify()
        # 再兜一次：确保第一帧就是全透明的，不然会闪一下全亮的暗屏
        self._set_alpha(0.0)
        self.win.lift()
        self.win.attributes("-topmost", True)
        self.win.focus_force()
        try:
            self.win.grab_set()  # 独占输入，避免误操作到下面的窗口
        except tk.TclError:
            pass
        self._fade_in()
        self.win.wait_window()
        return self._result

    def cancel(self) -> None:
        """外部取消（例如全局 Esc 兜底热键）。"""
        self._close(None)

    # ---------- 淡入淡出 ----------

    def _set_alpha(self, value: float) -> None:
        try:
            self.win.attributes("-alpha", max(0.0, min(1.0, value)))
        except tk.TclError:
            pass

    def _fade_in(self, step: int = 0) -> None:
        """屏幕从原样平滑暗下来，而不是"啪"一下变黑。"""
        if self._closed:
            return
        total = _FADE_IN_STEPS
        # 二次缓出：开头快、收尾慢，看起来更自然
        t = (step + 1) / total
        self._set_alpha(1.0 - (1.0 - t) ** 2)
        if step + 1 < total:
            try:
                self._fade_job = self.win.after(
                    _FADE_IN_INTERVAL_MS, lambda: self._fade_in(step + 1))
            except tk.TclError:
                self._fade_job = None

    def _finish_fade_in(self) -> None:
        """用户已经开始操作了，直接跳到最终状态。"""
        if self._fade_job is not None:
            try:
                self.win.after_cancel(self._fade_job)
            except tk.TclError:
                pass
            self._fade_job = None
        self._set_alpha(1.0)

    def _fade_out(self, step: int = _FADE_OUT_STEPS) -> None:
        """淡出之后再销毁，免得松手瞬间屏幕猛地一亮。"""
        if step <= 0:
            self._destroy()
            return
        self._set_alpha(step / _FADE_OUT_STEPS)
        try:
            self.win.after(_FADE_OUT_INTERVAL_MS, lambda: self._fade_out(step - 1))
        except tk.TclError:
            self._destroy()

    # ---------- 内部实现 ----------

    def _close(self, result: Rect | None) -> None:
        if self._closed:
            return
        self._closed = True
        self._result = result

        if self._refresh_job is not None:
            try:
                self.win.after_cancel(self._refresh_job)
            except tk.TclError:
                pass
            self._refresh_job = None
        if self._fade_job is not None:
            try:
                self.win.after_cancel(self._fade_job)
            except tk.TclError:
                pass
            self._fade_job = None

        try:
            self.win.grab_release()
        except tk.TclError:
            pass

        if self._fade_out_enabled:
            self._fade_out()
        else:
            self._destroy()

    def _destroy(self) -> None:
        try:
            self.win.destroy()
        except tk.TclError:
            pass

    def _on_cancel(self, _event=None) -> None:
        log.debug("用户取消了截图")
        self._close(None)

    def _on_press(self, event) -> None:
        self._finish_fade_in()
        self._dragging = True
        self._start = (event.x, event.y)
        self._current = (event.x, event.y)
        self._last_highlight = None
        self.canvas.itemconfigure("highlight", state="hidden")
        self.canvas.itemconfigure("border", state="hidden")

    def _on_drag(self, event) -> None:
        if not self._dragging:
            return
        self._current = (event.x, event.y)
        self._schedule_refresh()

    def _on_release(self, event) -> None:
        if not self._dragging:
            return
        self._dragging = False
        self._current = (event.x, event.y)
        rect = self._canvas_rect()
        if rect is None or rect.width < MIN_SELECTION or rect.height < MIN_SELECTION:
            log.debug("选区太小或没有拖拽，按取消处理")
            self._close(None)
            return
        # 换算成虚拟屏幕的全局坐标
        self._close(Rect(rect.left + self._area.left, rect.top + self._area.top,
                         rect.width, rect.height))

    def _canvas_rect(self) -> Rect | None:
        """当前选区在 canvas 坐标系里的矩形（已做边界裁剪）。"""
        x0, y0 = self._start
        x1, y1 = self._current
        left, right = sorted((x0, x1))
        top, bottom = sorted((y0, y1))
        left = max(0, min(left, self._area.width))
        right = max(0, min(right, self._area.width))
        top = max(0, min(top, self._area.height))
        bottom = max(0, min(bottom, self._area.height))
        if right <= left or bottom <= top:
            return None
        return Rect(left, top, right - left, bottom - top)

    def _schedule_refresh(self) -> None:
        """节流刷新：同一时刻最多有一个待执行的刷新任务排队。"""
        if self._refresh_job is not None:
            return
        try:
            self._refresh_job = self.win.after(_REFRESH_INTERVAL_MS, self._do_refresh)
        except tk.TclError:
            self._refresh_job = None

    def _do_refresh(self) -> None:
        self._refresh_job = None
        if self._closed:
            return
        rect = self._canvas_rect()
        if rect is None or rect == self._last_highlight:
            return
        self._last_highlight = rect

        # 选区内部恢复原始亮度
        try:
            crop = self._image.crop((rect.left, rect.top, rect.right, rect.bottom))
            self._bright_photo = ImageTk.PhotoImage(crop, master=self.win)
        except Exception:
            log.exception("绘制选区高亮失败")
            return
        try:
            self.canvas.itemconfigure(self._highlight_id, image=self._bright_photo, state="normal")
            self.canvas.coords(self._highlight_id, rect.left, rect.top)
            self.canvas.coords(self._border_id, rect.left, rect.top, rect.right, rect.bottom)
            self.canvas.itemconfigure("border", state="normal")
            self.canvas.tag_raise("highlight")
            self.canvas.tag_raise("border")
        except tk.TclError:
            pass


def select_region(parent: tk.Misc, image: Image.Image, area: Rect, cfg,
                  on_open=None, on_close=None) -> Rect | None:
    """弹出遮罩让用户框选。返回全局坐标的选区，取消或误触返回 None。

    on_open / on_close 用来让主程序知道何时该挂上、何时该摘掉全局 Esc 兜底热键。
    """
    mask = MaskWindow(parent, image, area, cfg)
    if on_open:
        on_open(mask)
    try:
        return mask.run()
    except Exception:
        log.exception("截图遮罩异常")
        try:
            mask.cancel()
        except Exception:
            pass
        return None
    finally:
        if on_close:
            on_close(mask)
