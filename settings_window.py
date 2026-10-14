# -*- coding: utf-8 -*-
"""设置窗口：深色、无边框、自绘控件，和浮层保持同一套视觉。

入口只有一个 —— 右键托盘图标 → 设置。改动写回 config.json。
"""

from __future__ import annotations

import logging
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk

from config import Config
from icons import draw_icon, tint_icon
from winutil import set_dark_titlebar, set_round_corners

log = logging.getLogger(__name__)

# 配色
_BG = "#1e1e1e"
_CARD = "#252527"
_BORDER = "#343436"
_TEXT = "#e8e8e8"
_TEXT_DIM = "#8f8f8f"
_ACCENT = "#3d9bff"
_ACCENT_HOVER = "#5aabff"
_FIELD = "#2c2c2e"
_FIELD_BORDER = "#3a3a3c"
_BTN = "#3a3a3c"
_BTN_HOVER = "#47474a"
_CLOSE_HOVER = "#ff6b6b"

_WIN_W = 500
_LABEL_W = 14          # 左侧标签字符宽度
_SLIDER_W = 240

_FONT_CANDIDATES = ("Microsoft YaHei UI", "Microsoft YaHei", "微软雅黑", "Segoe UI", "Arial")


def _pick_font(root: tk.Misc) -> str:
    try:
        available = set(tkfont.families(root))
    except tk.TclError:
        return "TkDefaultFont"
    for name in _FONT_CANDIDATES:
        if name in available:
            return name
    return "TkDefaultFont"


class _Slider(tk.Canvas):
    """自绘滑块：深色轨道 + 蓝色已选段 + 圆形手柄。"""

    TRACK_H = 4
    KNOB_R = 7
    PAD = 12

    def __init__(self, parent, low: float, high: float, value: float,
                 on_change, step: float = 0.0, bg: str = _CARD):
        super().__init__(parent, width=_SLIDER_W, height=26, bg=bg,
                         highlightthickness=0, bd=0, cursor="hand2")
        self._low, self._high = low, high
        self._step = step
        self._value = min(high, max(low, value))
        self._on_change = on_change
        self._dragging = False

        self._track = self.create_line(0, 0, 0, 0, width=self.TRACK_H,
                                       capstyle="round", fill="#3a3a3c")
        self._fill = self.create_line(0, 0, 0, 0, width=self.TRACK_H,
                                      capstyle="round", fill=_ACCENT)
        self._knob = self.create_oval(0, 0, 0, 0, fill="#ffffff", outline="")

        self.bind("<Button-1>", self._on_press)
        self.bind("<B1-Motion>", self._on_move)
        self.bind("<ButtonRelease-1>", self._on_release)
        self.bind("<Configure>", lambda _e: self._redraw())
        self._redraw()

    # -- 值 --

    @property
    def value(self) -> float:
        return self._value

    def set_value(self, value: float) -> None:
        self._value = min(self._high, max(self._low, value))
        self._redraw()

    # -- 绘制 --

    def _x_for(self, value: float) -> float:
        span = max(1, self.winfo_width() - 2 * self.PAD)
        ratio = (value - self._low) / (self._high - self._low)
        return self.PAD + ratio * span

    def _value_for(self, x: float) -> float:
        span = max(1, self.winfo_width() - 2 * self.PAD)
        ratio = (x - self.PAD) / span
        ratio = min(1.0, max(0.0, ratio))
        value = self._low + ratio * (self._high - self._low)
        if self._step:
            value = round(value / self._step) * self._step
        return min(self._high, max(self._low, value))

    def _redraw(self) -> None:
        try:
            h = int(self.winfo_height()) or 26
            mid = h / 2
            x = self._x_for(self._value)
            self.coords(self._track, self.PAD, mid, self.winfo_width() - self.PAD, mid)
            self.coords(self._fill, self.PAD, mid, x, mid)
            self.coords(self._knob, x - self.KNOB_R, mid - self.KNOB_R,
                        x + self.KNOB_R, mid + self.KNOB_R)
        except tk.TclError:
            pass

    # -- 交互 --

    def _on_press(self, event) -> None:
        self._dragging = True
        self._apply(event.x)

    def _on_move(self, event) -> None:
        if self._dragging:
            self._apply(event.x)

    def _on_release(self, _event=None) -> None:
        self._dragging = False

    def _apply(self, x: float) -> None:
        value = self._value_for(x)
        if value != self._value:
            self._value = value
            self._redraw()
            self._on_change(value)


class _FlatButton(tk.Label):
    """扁平按钮。primary=True 时是蓝色实心。"""

    def __init__(self, parent, text: str, command, font, primary: bool = False):
        self._primary = primary
        self._normal = _ACCENT if primary else _BTN
        self._hover = _ACCENT_HOVER if primary else _BTN_HOVER
        super().__init__(parent, text=text, font=font, bg=self._normal,
                         fg="#ffffff" if primary else _TEXT,
                         padx=22, pady=7, cursor="hand2")
        self.bind("<Button-1>", lambda _e: command())
        self.bind("<Enter>", lambda _e: self.configure(bg=self._hover))
        self.bind("<Leave>", lambda _e: self.configure(bg=self._normal))


class SettingsWindow:
    """设置窗口。用 show() 打开，模态。"""

    def __init__(self, root: tk.Misc, cfg: Config, on_save=None):
        self._root = root
        self._cfg = cfg
        self._on_save = on_save
        self._drag_offset = None
        self._saved = False
        self._closed = False

        family = _pick_font(root)
        self._f_label = tkfont.Font(root=root, family=family, size=10)
        self._f_section = tkfont.Font(root=root, family=family, size=10, weight="bold")
        self._f_value = tkfont.Font(root=root, family=family, size=10)
        self._f_title = tkfont.Font(root=root, family=family, size=11)

        self.win = tk.Toplevel(root)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg=_BORDER)   # 外圈 1px 描边

        self._build()
        self._center()

    # ---------- 构建 ----------

    def _build(self) -> None:
        inner = tk.Frame(self.win, bg=_BG)
        inner.pack(fill="both", expand=True, padx=1, pady=1)

        self._build_titlebar(inner)

        body = tk.Frame(inner, bg=_BG)
        body.pack(fill="both", expand=True, padx=18, pady=(4, 0))

        # ---- 外观 ----
        self._section(body, "外观")
        self._mask = self._slider_row(body, "截图遮罩透明度",
                                      self._cfg.mask_opacity, 0.05, 1.0,
                                      self._fmt_percent, step=0.01)
        self._overlay = self._slider_row(body, "译文背景透明度",
                                         self._cfg.overlay_opacity, 0.3, 1.0,
                                         self._fmt_percent, step=0.01)
        self._fontsize = self._slider_row(body, "译文字号",
                                          self._cfg.font_size, 8, 28,
                                          self._fmt_int, step=1)

        self._spacer(body)

        # ---- 接口 ----
        self._section(body, "接口")
        self._base_url = self._field_row(body, "接口地址", self._cfg.api_base_url)
        self._api_key = self._field_row(body, "API Key", self._cfg.api_key)
        self._model = self._field_row(body, "模型", self._cfg.model)
        self._target = self._field_row(body, "目标语言", self._cfg.target_language)

        self._spacer(body)

        # ---- 快捷键 ----
        self._section(body, "快捷键")
        self._hotkey = self._field_row(body, "截图翻译", self._cfg.hotkey)
        self._exit_hotkey = self._field_row(body, "退出程序", self._cfg.exit_hotkey)

        # ---- 底部按钮 ----
        footer = tk.Frame(inner, bg=_BG)
        footer.pack(fill="x", padx=18, pady=14)
        _FlatButton(footer, "保存", self._save, self._f_title, primary=True).pack(side="right")
        _FlatButton(footer, "取消", self._close, self._f_label).pack(side="right", padx=(0, 10))

        self._hint = tk.Label(inner, text="", bg=_BG, fg=_TEXT_DIM, font=self._f_label)
        self._hint.pack(pady=(0, 10))

    def _build_titlebar(self, parent: tk.Misc) -> None:
        bar = tk.Frame(parent, bg=_BG, height=42)
        bar.pack(fill="x")
        bar.pack_propagate(False)

        title = tk.Label(bar, text="设置", bg=_BG, fg=_TEXT, font=self._f_title)
        title.pack(side="left", padx=18)

        close = tk.Canvas(bar, width=26, height=26, bg=_BG, highlightthickness=0,
                          bd=0, cursor="hand2")
        close.pack(side="right", padx=12)
        items = draw_icon(close, "close", _TEXT_DIM, 16)
        close.bind("<Button-1>", lambda _e: self._close())
        # 之前这里直接对每个图元设 outline，但关闭图标是 line 图元，
        # 没有 -outline 选项，异常被吞掉后颜色其实一直没变过
        close.bind("<Enter>", lambda _e: tint_icon(close, items, _CLOSE_HOVER))
        close.bind("<Leave>", lambda _e: tint_icon(close, items, _TEXT_DIM))

        # 拖动窗口
        for widget in (bar, title):
            widget.bind("<Button-1>", self._drag_start)
            widget.bind("<B1-Motion>", self._drag_move)
        self.win.bind("<Escape>", lambda _e: self._close())

    def _section(self, parent: tk.Misc, text: str) -> None:
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=(10, 4))
        tk.Label(row, text=text, bg=_BG, fg=_TEXT_DIM,
                 font=self._f_section).pack(side="left")

    def _spacer(self, parent: tk.Misc) -> None:
        tk.Frame(parent, bg=_BORDER, height=1).pack(fill="x", pady=(14, 0))

    def _slider_row(self, parent: tk.Misc, label: str, value, low, high,
                    formatter, step: float = 0.0) -> _Slider:
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=4)

        tk.Label(row, text=label, bg=_BG, fg=_TEXT, font=self._f_label,
                 width=_LABEL_W, anchor="w").pack(side="left")

        value_label = tk.Label(row, text=formatter(value), bg=_BG, fg=_ACCENT,
                               font=self._f_value, width=5, anchor="e")
        value_label.pack(side="right")

        def changed(new_value):
            value_label.configure(text=formatter(new_value))

        slider = _Slider(row, low, high, value, changed, step=step, bg=_BG)
        slider.pack(side="left")
        slider._value_label = value_label  # type: ignore[attr-defined]
        return slider

    def _field_row(self, parent: tk.Misc, label: str, value: str) -> ttk.Entry:
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=4)

        tk.Label(row, text=label, bg=_BG, fg=_TEXT, font=self._f_label,
                 width=_LABEL_W, anchor="w").pack(side="left")

        entry = ttk.Entry(row, style="Dark.TEntry", font=self._f_value)
        entry.insert(0, value or "")
        entry.pack(side="left", fill="x", expand=True, ipady=4)
        return entry

    # ---------- 窗口行为 ----------

    def _center(self) -> None:
        self.win.update_idletasks()
        w = max(_WIN_W, self.win.winfo_reqwidth())
        h = self.win.winfo_reqheight()
        try:
            screen_w = self.win.winfo_screenwidth()
            screen_h = self.win.winfo_screenheight()
        except tk.TclError:
            screen_w, screen_h = 1920, 1080
        x = (screen_w - w) // 2
        y = max(20, (screen_h - h) // 2)
        self.win.geometry("{}x{}+{}+{}".format(w, h, x, y))

    def _drag_start(self, event) -> None:
        try:
            self._drag_offset = (event.x_root - self.win.winfo_x(),
                                 event.y_root - self.win.winfo_y())
        except tk.TclError:
            self._drag_offset = None

    def _drag_move(self, event) -> None:
        if self._drag_offset is None:
            return
        try:
            self.win.geometry("+{}+{}".format(
                int(event.x_root - self._drag_offset[0]),
                int(event.y_root - self._drag_offset[1])))
        except tk.TclError:
            pass

    def show(self) -> bool:
        """显示窗口并阻塞，返回是否点了保存。"""
        self.win.deiconify()
        self.win.lift()
        self.win.focus_force()
        set_round_corners(self.win, small=True)
        try:
            self.win.grab_set()
        except tk.TclError:
            pass
        self.win.wait_window()
        return self._saved

    def _close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self.win.grab_release()
        except tk.TclError:
            pass
        try:
            self.win.destroy()
        except tk.TclError:
            pass

    # ---------- 保存 ----------

    @staticmethod
    def _fmt_percent(value) -> str:
        return "%d%%" % round(float(value) * 100)

    @staticmethod
    def _fmt_int(value) -> str:
        return str(int(round(float(value))))

    def _save(self) -> None:
        cfg = self._cfg
        cfg.mask_opacity = round(float(self._mask.value), 3)
        cfg.overlay_opacity = round(float(self._overlay.value), 3)
        cfg.font_size = int(round(float(self._fontsize.value)))
        cfg.api_base_url = self._base_url.get().strip() or cfg.api_base_url
        cfg.api_key = self._api_key.get().strip()
        cfg.model = self._model.get().strip() or cfg.model
        cfg.target_language = self._target.get().strip() or cfg.target_language
        cfg.hotkey = self._hotkey.get().strip() or cfg.hotkey
        cfg.exit_hotkey = self._exit_hotkey.get().strip() or cfg.exit_hotkey

        self._saved = True
        if self._on_save:
            try:
                self._on_save(cfg)
            except Exception:
                log.exception("保存配置失败")
                try:
                    self._hint.configure(text="保存失败，详见日志")
                except tk.TclError:
                    pass
                self._saved = False
                return
        self._close()


def install_styles(root: tk.Misc) -> None:
    """给 ttk 控件套一层深色样式（进程内只需装一次）。"""
    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except tk.TclError:
        pass
    style.configure(
        "Dark.TEntry",
        fieldbackground=_FIELD, background=_FIELD, foreground=_TEXT,
        insertcolor=_TEXT, bordercolor=_FIELD_BORDER, lightcolor=_FIELD,
        darkcolor=_FIELD, borderwidth=1, relief="flat", padding=4,
    )
    style.map("Dark.TEntry",
              bordercolor=[("focus", _ACCENT)],
              lightcolor=[("focus", _ACCENT)],
              darkcolor=[("focus", _ACCENT)])


def open_settings(root: tk.Misc, cfg: Config, on_save=None) -> bool:
    """打开设置窗口（模态）。返回是否保存了改动。"""
    install_styles(root)
    window = SettingsWindow(root, cfg, on_save)
    return window.show()
