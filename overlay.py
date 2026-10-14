# -*- coding: utf-8 -*-
"""结果浮层：半透明、无边框、置顶，只显示译文。

浮层上只有：
  - 译文文本（自动换行；按住它可以把整个浮层拖到别处）
  - 右下角三个小图标：提取原文 / 复制译文 / 关闭
没有标题栏、边框、菜单、状态栏，也不显示原文。
"""

from __future__ import annotations

import logging
import tkinter as tk
import tkinter.font as tkfont

from icons import draw_icon, tint_icon
from screenshot import Rect
from winutil import set_round_corners

log = logging.getLogger(__name__)

# 浮层的尺寸约束
_MIN_WIDTH = 128          # 再窄三个图标就挤了
_MAX_WIDTH = 1000         # 不让它变成整屏
_PADDING_X = 12           # 文本左右内边距
_PADDING_TOP = 10
_PADDING_BOTTOM = 5
_GAP = 3                  # 正文和图标行之间的间距
_BUTTON_GAP = 8           # 图标按钮之间的间距
_BUTTON_PAD = 3           # 图标四周留白
_MIN_FONT_SIZE = 9        # 自动缩字号的下限

# 图标相对于一行文字的高度
_ICON_RATIO = 0.70

# 鼠标悬停多久才弹提示（毫秒）
_TOOLTIP_DELAY_MS = 380

# 悬停提示的配色
_TOOLTIP_BG = "#3a3a3c"
_TOOLTIP_FG = "#f0f0f0"
_TOOLTIP_BORDER = "#4a4a4d"

# 图标配色：低调的灰，悬停变亮；关闭按钮固定白色描边、悬停变红
_ICON_FG = "#8f8f8f"
_ICON_CLOSE_FG = "#ffffff"
_ICON_FG_HOVER = "#ffffff"
_ICON_FG_DISABLED = "#555555"
_ICON_CLOSE_HOVER = "#ff6b6b"

# 正文字体候选，按顺序挑第一个系统里有的
_FONT_CANDIDATES = ("Microsoft YaHei UI", "Microsoft YaHei", "微软雅黑", "Segoe UI", "Arial")

# 图标点下去之后反馈的停留时间
_FLASH_MS = 900
_LOADING_TEXT = "翻译中…"


def _pick_font_family(root: tk.Misc) -> str:
    """挑一个系统里存在的中文字体。"""
    try:
        available = set(tkfont.families(root))
    except tk.TclError:
        return "TkDefaultFont"
    for family in _FONT_CANDIDATES:
        if family in available:
            return family
    return "TkDefaultFont"


class _IconButton:
    """一个画在 Canvas 上的图标按钮：悬停变色、悬停出提示、点下去闪一下。"""

    def __init__(self, parent: tk.Misc, kind: str, size: int, command,
                 bg: str, hover_color: str = _ICON_FG_HOVER,
                 tooltip: str = "", font=None, color: str = _ICON_FG):
        self._kind = kind
        self._size = size
        self._command = command
        self._bg = bg
        self._hover_color = hover_color
        self._color = color
        self._tooltip_text = tooltip
        self._font = font
        self._enabled = True
        self._hovering = False
        self._tooltip_win: tk.Toplevel | None = None
        self._tooltip_job: str | None = None

        side = size + 2 * _BUTTON_PAD
        self.canvas = tk.Canvas(parent, width=side, height=side, bg=bg,
                                highlightthickness=0, bd=0, cursor="hand2")
        self._items = draw_icon(self.canvas, kind, _ICON_FG_DISABLED, size,
                                bg=bg, tags="icon")
        # 立刻按自身状态重画一次：关闭按钮不走 set_enabled 那条路，
        # 不重画的话它会一直停留在上面的禁用色（暗灰）。
        self._repaint()

        self.canvas.bind("<Button-1>", self._on_click)
        self.canvas.bind("<Enter>", lambda _e: self._set_hover(True))
        self.canvas.bind("<Leave>", lambda _e: self._set_hover(False))

    def pack(self, **kwargs) -> None:
        self.canvas.pack(**kwargs)

    @property
    def widget(self) -> tk.Canvas:
        return self.canvas

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = enabled
        self.canvas.configure(cursor="hand2" if enabled else "arrow")
        if not enabled:
            self._hide_tooltip()
        self._repaint()

    def flash(self, seconds: float = _FLASH_MS / 1000.0) -> None:
        """图标短暂变成高亮色再恢复，用来表示"按下去了"。"""
        if not self._enabled:
            return
        self._paint(_ICON_FG_HOVER)
        try:
            self.canvas.after(int(seconds * 1000), self._repaint)
        except tk.TclError:
            pass

    def destroy_tooltip(self) -> None:
        """浮层关闭前必须调一下，否则提示窗会变成孤儿窗口留在屏幕上。"""
        self._hide_tooltip()

    # ---- 提示 ----

    def _schedule_tooltip(self) -> None:
        if not self._tooltip_text or self._tooltip_job is not None:
            return
        try:
            self._tooltip_job = self.canvas.after(_TOOLTIP_DELAY_MS, self._show_tooltip)
        except tk.TclError:
            self._tooltip_job = None

    def _show_tooltip(self) -> None:
        self._tooltip_job = None
        if not self._tooltip_text or not self._hovering or not self._enabled:
            return
        if self._tooltip_win is not None:
            return
        try:
            tip = tk.Toplevel(self.canvas)
            tip.overrideredirect(True)
            tip.attributes("-topmost", True)
            tip.configure(bg=_TOOLTIP_BORDER)
            tk.Label(tip, text=self._tooltip_text, bg=_TOOLTIP_BG, fg=_TOOLTIP_FG,
                     font=self._font, padx=9, pady=3).pack(padx=1, pady=1)
            tip.update_idletasks()
            w, h = tip.winfo_reqwidth(), tip.winfo_reqheight()
            x = self.canvas.winfo_rootx() + (self.canvas.winfo_width() - w) // 2
            # 默认吊在浮层外面（下方）—— 浮层很小，放里面会压住译文
            panel = self.canvas.winfo_toplevel()
            y = panel.winfo_rooty() + panel.winfo_height() + 6
            try:
                screen_w = tip.winfo_screenwidth()
                screen_h = tip.winfo_screenheight()
                if y + h > screen_h - 6:
                    # 下方放不下就翻到浮层上方
                    y = panel.winfo_rooty() - h - 6
                if x + w > screen_w - 6:
                    x = screen_w - w - 6
                if x < 6:
                    x = 6
                if y < 6:
                    y = 6
            except tk.TclError:
                pass
            tip.geometry("{}x{}+{}+{}".format(w, h, int(x), int(y)))
            self._tooltip_win = tip
            set_round_corners(tip, small=True)
        except tk.TclError:
            self._tooltip_win = None

    def _hide_tooltip(self) -> None:
        if self._tooltip_job is not None:
            try:
                self.canvas.after_cancel(self._tooltip_job)
            except (tk.TclError, ValueError):
                pass
            self._tooltip_job = None
        if self._tooltip_win is not None:
            try:
                self._tooltip_win.destroy()
            except tk.TclError:
                pass
            self._tooltip_win = None

    # ---- 交互 ----

    def _on_click(self, _event=None) -> None:
        if self._enabled:
            self._command()

    def _set_hover(self, hovering: bool) -> None:
        self._hovering = hovering
        self._repaint()
        if hovering:
            self._schedule_tooltip()
        else:
            self._hide_tooltip()

    def _repaint(self) -> None:
        if not self._enabled:
            self._paint(_ICON_FG_DISABLED)
            return
        if self._hovering:
            self._paint(self._hover_color)
        else:
            self._paint(self._color)

    def _paint(self, color: str) -> None:
        """按图元类型上色。

        注意：Canvas 的 line 没有 -outline 选项，对它设 outline 会抛 TclError。
        早先的写法把整轮循环包在一个 try 里，结果线条一抛异常，后面所有图元
        （包括它自己的 fill）都不再更新 —— X 和文字线因此一直是禁用色。
        现在逐个图元分开处理。
        """
        tint_icon(self.canvas, self._items, color, self._fill_for(color))

    def _fill_for(self, color: str) -> str:
        # “复制”图标前面那个方块要填底色，才能把后面那个挡住一角
        if self._kind == "copy":
            return self._bg
        # 其余图标只描边
        return ""


class Overlay:
    """一次翻译结果的浮层。用完即毁，不重复利用。"""

    def __init__(self, root: tk.Misc, cfg, selection: Rect, screen: Rect | None = None,
                 on_close=None):
        self._root = root
        self._cfg = cfg
        self._selection = selection
        self._screen = screen
        self._on_close = on_close
        self._closed = False
        self._original_text = ""
        self._translated_text = ""
        self._buttons_enabled = False

        # 拖动
        self._drag_offset: tuple[int, int] | None = None
        self._user_moved = False

        family = _pick_font_family(root)
        self._body_font = tkfont.Font(root=root, family=family, size=cfg.font_size)
        self._line_height = max(14, int(self._body_font.metrics("linespace")))
        self._icon_size = max(11, int(self._line_height * _ICON_RATIO))
        self._max_width = self._compute_max_width(screen)
        self._width = _MIN_WIDTH

        bg = cfg.overlay_bg
        fg = cfg.overlay_fg

        self.win = tk.Toplevel(root)
        self.win.withdraw()
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        self.win.configure(bg=bg)
        try:
            self.win.attributes("-alpha", float(cfg.overlay_opacity))
        except tk.TclError:
            log.warning("当前系统不支持窗口透明度，浮层将不透明显示")

        self.win.columnconfigure(0, weight=1)

        self.text_label = tk.Label(
            self.win, text=_LOADING_TEXT, bg=bg, fg=fg, font=self._body_font,
            justify="left", anchor="nw", wraplength=_MIN_WIDTH - 2 * _PADDING_X,
            cursor="fleur",
        )
        self.text_label.grid(row=0, column=0, sticky="nsew",
                             padx=_PADDING_X, pady=(_PADDING_TOP, 0))

        self._button_bar = tk.Frame(self.win, bg=bg, cursor="fleur")
        self._button_bar.grid(row=1, column=0, sticky="e",
                              padx=_PADDING_X, pady=(_GAP, _PADDING_BOTTOM))

        self._tip_font = tkfont.Font(root=root, family=family,
                                     size=max(_MIN_FONT_SIZE, cfg.font_size - 3))
        self.extract_button = _IconButton(
            self._button_bar, "extract", self._icon_size, self._copy_original, bg,
            tooltip="复制文字", font=self._tip_font)
        self.extract_button.pack(side="left", padx=(0, _BUTTON_GAP))
        self.copy_button = _IconButton(
            self._button_bar, "copy", self._icon_size, self._copy_translated, bg,
            tooltip="复制翻译", font=self._tip_font)
        self.copy_button.pack(side="left", padx=(0, _BUTTON_GAP))
        self.close_button = _IconButton(
            self._button_bar, "close", self._icon_size, self.close, bg,
            hover_color=_ICON_CLOSE_HOVER, tooltip="关闭", font=self._tip_font,
            color=_ICON_CLOSE_FG)
        self.close_button.pack(side="left")

        # 兼容旧名字（测试脚本还在用）
        self.copy_original_label = self.extract_button.canvas
        self.copy_translated_label = self.copy_button.canvas

        self._set_buttons_enabled(False)

        # 拖动整个浮层
        for widget in (self.win, self.text_label, self._button_bar):
            widget.bind("<Button-1>", self._on_drag_start)
            widget.bind("<B1-Motion>", self._on_drag_move)
            widget.bind("<ButtonRelease-1>", self._on_drag_end)

        # Esc / 右键关闭
        for sequence in ("<Escape>", "<Button-3>"):
            self.win.bind(sequence, self._on_escape)

        self.win.update_idletasks()
        self._apply_layout(_LOADING_TEXT)

    # ---------- 对外接口 ----------

    @property
    def closed(self) -> bool:
        return self._closed

    def show_result(self, original_text: str, translated_text: str) -> None:
        """显示译文。原文只存起来供“提取原文”用，不显示。"""
        if self._closed:
            return
        self._original_text = original_text or ""
        self._translated_text = translated_text or ""
        text = self._translated_text or self._original_text or "（没有识别到文字）"
        self._set_text(text)
        self._set_buttons_enabled(True)

    def show_error(self, message: str) -> None:
        """显示一条错误提示。图标按钮保持不可用。"""
        if self._closed:
            return
        self._set_text(message)
        self._set_buttons_enabled(False)

    def close(self) -> None:
        """关闭浮层。程序继续在后台运行。"""
        if self._closed:
            return
        self._closed = True
        for button in (self.extract_button, self.copy_button, self.close_button):
            button.destroy_tooltip()
        try:
            self.win.unbind_all("<Escape>")
        except tk.TclError:
            pass
        try:
            self.win.destroy()
        except tk.TclError:
            pass
        log.debug("浮层已关闭")
        # 通知主程序：浮层没了，Esc 兜底热键可以摘了
        if self._on_close is not None:
            try:
                self._on_close(self)
            except Exception:
                log.exception("浮层关闭回调出错")

    def focus(self) -> None:
        """显示浮层并抢到焦点，让 Esc 生效。"""
        if self._closed:
            return
        try:
            self.win.deiconify()
            self.win.lift()
            self.win.attributes("-topmost", True)
            self.win.focus_force()
            # Win11 原生小圆角；老系统上这个调用会失败，不影响使用
            set_round_corners(self.win, small=True)
        except tk.TclError:
            log.exception("显示浮层失败")

    # ---------- 拖动 ----------

    def _on_drag_start(self, event) -> None:
        try:
            self._drag_offset = (event.x_root - self.win.winfo_x(),
                                 event.y_root - self.win.winfo_y())
        except tk.TclError:
            self._drag_offset = None

    def _on_drag_move(self, event) -> None:
        if self._drag_offset is None or self._closed:
            return
        try:
            x = event.x_root - self._drag_offset[0]
            y = event.y_root - self._drag_offset[1]
            self.win.geometry("+{}+{}".format(int(x), int(y)))
            self._user_moved = True
        except tk.TclError:
            pass

    def _on_drag_end(self, _event=None) -> None:
        self._drag_offset = None

    # ---------- 尺寸与位置 ----------

    def _compute_max_width(self, screen: Rect | None) -> int:
        limit = _MAX_WIDTH
        if screen is not None:
            limit = min(limit, screen.width - 40)
        return max(_MIN_WIDTH, int(limit))

    def _set_text(self, text: str) -> None:
        try:
            self.text_label.configure(text=text)
            self.win.update_idletasks()
        except tk.TclError:
            return
        self._apply_layout(text)

    def _natural_text_width(self, text: str) -> int:
        """文本不换行时需要多宽（取最长的一行）。"""
        lines = (text or "").split("\n") or [""]
        return max((self._body_font.measure(line) for line in lines), default=0)

    def _layout_size(self, text: str, buttons_width: int) -> tuple[int, int]:
        """按当前字号算一次浮层的 (宽, 高)。

        宽度优先贴合文字：短译文就给一小块底板，只有文字太宽时才拉宽并换行。
        """
        text_width = self._natural_text_width(text)
        width = max(text_width + 2 * _PADDING_X,
                    buttons_width + 2 * _PADDING_X,
                    _MIN_WIDTH)
        width = min(width, self._max_width)
        try:
            self.text_label.configure(wraplength=max(40, width - 2 * _PADDING_X))
            self.win.update_idletasks()
            height = (int(self.text_label.winfo_reqheight())
                      + _PADDING_TOP + _GAP
                      + int(self._button_bar.winfo_reqheight())
                      + _PADDING_BOTTOM)
        except tk.TclError:
            return int(width), 0
        return int(width), int(height)

    def _apply_layout(self, text: str) -> None:
        """重算尺寸并摆放窗口。文字太长就逐步缩小字号。"""
        try:
            buttons_width = int(self._button_bar.winfo_reqwidth())
        except tk.TclError:
            buttons_width = _MIN_WIDTH

        available = self._available_height()
        size = int(self._cfg.font_size)
        width, height = self._layout_size(text, buttons_width)

        while height > available and size > _MIN_FONT_SIZE:
            size -= 1
            self._body_font.configure(size=size)
            width, height = self._layout_size(text, buttons_width)

        self._width = width
        self._place(min(height, available))

    def _available_height(self) -> int:
        if self._screen is None:
            return 100000
        return max(120, int(self._screen.height * 0.92))

    def _place(self, height: int) -> None:
        """摆放窗口。用户手动拖过之后，就只改大小、不再挪位置。"""
        width = self._width
        if self._user_moved:
            try:
                x, y = self.win.winfo_x(), self.win.winfo_y()
            except tk.TclError:
                return
        else:
            x, y = self._default_position(width, height)

        try:
            self.win.geometry("{}x{}+{}+{}".format(width, height, x, y))
        except tk.TclError:
            log.exception("设置浮层位置失败")

    def _default_position(self, width: int, height: int) -> tuple[int, int]:
        """默认贴在框选区域的左上角，并保证整个浮层落在屏幕内。"""
        x, y = self._selection.left, self._selection.top
        if self._screen is None:
            return x, y

        margin = 8
        left, top = self._screen.left, self._screen.top
        right, bottom = self._screen.right, self._screen.bottom

        if x + width > right - margin:
            x = right - margin - width
        if x < left + margin:
            x = left + margin
        if y + height > bottom - margin:
            y = min(self._selection.bottom - height, bottom - margin - height)
        if y < top + margin:
            y = top + margin
        return int(x), int(y)

    # ---------- 按钮 ----------

    def _set_buttons_enabled(self, enabled: bool) -> None:
        self._buttons_enabled = enabled
        for button in (self.extract_button, self.copy_button):
            button.set_enabled(enabled)

    def _on_escape(self, _event=None) -> str:
        self.close()
        return "break"

    def _copy_original(self) -> None:
        self._copy_to_clipboard(self._original_text, self.extract_button)

    def _copy_translated(self) -> None:
        self._copy_to_clipboard(self._translated_text, self.copy_button)

    def _copy_to_clipboard(self, text: str, button: _IconButton) -> None:
        """写剪贴板并让图标闪一下，绝不弹窗。"""
        if not text:
            log.debug("没有可复制的内容")
            return
        try:
            import pyperclip

            pyperclip.copy(text)
            button.flash()
            log.info("已复制到剪贴板：%d 字", len(text))
        except Exception:
            log.exception("写剪贴板失败")


def show_overlay(root: tk.Misc, cfg, selection: Rect, screen: Rect | None = None,
                 on_close=None) -> Overlay:
    """创建并显示一个"翻译中…"状态的浮层。"""
    overlay = Overlay(root, cfg, selection, screen, on_close=on_close)
    overlay.focus()
    return overlay
