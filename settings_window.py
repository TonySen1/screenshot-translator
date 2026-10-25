# -*- coding: utf-8 -*-
"""设置窗口：深色、无边框、自绘控件，和浮层保持同一套视觉。

入口只有一个 —— 右键托盘图标 → 设置。改动写回 config.json。
"""

from __future__ import annotations

import logging
import queue
import threading
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
from tkinter import ttk

import hotkey as hotkey_mod
import model_list
import service_test
from config import APP_VERSION, Config
from icons import draw_icon, tint_bitmap, tint_icon
from winutil import bring_to_front, set_round_corners

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
_ICON = "#8f8f8f"          # 图标常态：低调的灰（和 _TEXT_DIM 同色）
_ICON_HOVER = "#ffffff"

# 悬停提示的配色，跟浮层上那几个图标按钮保持一致
_TOOLTIP_BG = "#3a3a3c"
_TOOLTIP_FG = "#f0f0f0"
_TOOLTIP_BORDER = "#4a4a4d"
_TOOLTIP_DELAY_MS = 380

# 点左下角图标打开的项目仓库
_REPO_URL = "https://github.com/TonySen1/screenshot-translator"

_WIN_W = 500
_EMPTY_LABEL = "（先点「拉取列表」）"

_REPO_INSET = 44       # 仓库图标距窗口左边的距离
# 下边距单独一个值：footer 现在很矮（按钮底下就 16px），图标 28px 高，
# 想跟左边一样留 44px 会被窗口底边切掉。以实测能到的最舒服位置为准。
_REPO_INSET_BOTTOM = 22
# 实测标定（footer 收紧后重测）：pady=0 时图标离下边 22px，再加 pady 会压到 17px。
# 所以这里补 0，只靠 _REPO_INSET_BOTTOM 定位置。
_REPO_PADY_TRIM = 0
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


class _RepoButton:
    """左下角的仓库图标按钮：悬停变亮 + 悬停出提示，点一下用浏览器打开仓库。

    只有这一个图标，所以没做成通用控件；提示窗的逻辑是照浮层里那套抄的
    （见 overlay._IconButton），这里按设置窗口的排版简化了定位：
    提示开在图标正上方、左边缘对齐，越界再夹回屏幕里。
    """

    def __init__(self, parent: tk.Misc, url: str, tooltip: str, font, size: int = 20):
        self._url = url
        self._tooltip_text = tooltip
        self._font = font
        self._hovering = False
        self._size = size
        self._tooltip_win: tk.Toplevel | None = None
        self._tooltip_job: str | None = None

        side = size + 8
        self.canvas = tk.Canvas(parent, width=side, height=side, bg=_BG,
                                highlightthickness=0, bd=0, cursor="hand2")
        # 位图图标只有一个 canvas item，换色就是换整张图
        self._items = draw_icon(self.canvas, "repo", _ICON, size, bg=_BG)

        self.canvas.bind("<Button-1>", lambda _e: self._open())
        self.canvas.bind("<Enter>", lambda _e: self._set_hover(True))
        self.canvas.bind("<Leave>", lambda _e: self._set_hover(False))

    def pack(self, **kwargs) -> None:
        self.canvas.pack(**kwargs)

    def pack_configure(self, **kwargs) -> None:
        self.canvas.pack_configure(**kwargs)

    @property
    def widget(self) -> tk.Canvas:
        return self.canvas

    def _open(self) -> None:
        self._hide_tooltip()
        try:
            webbrowser.open(self._url)
        except Exception:
            # 打不开浏览器不算错误，别把设置窗口弄崩
            log.exception("打开仓库地址失败：%s", self._url)

    def _set_hover(self, hovering: bool) -> None:
        self._hovering = hovering
        tint_bitmap(self.canvas, self._items[0], "repo", self._size,
                    _ICON_HOVER if hovering else _ICON)
        if hovering:
            self._schedule_tooltip()
        else:
            self._hide_tooltip()

    # ---- 悬停提示 ----

    def destroy_tooltip(self) -> None:
        """窗口关闭前必须调一下，否则提示窗会变成孤儿窗口留在屏幕上。"""
        self._hide_tooltip()

    def _schedule_tooltip(self) -> None:
        if not self._tooltip_text or self._tooltip_job is not None:
            return
        try:
            self._tooltip_job = self.canvas.after(_TOOLTIP_DELAY_MS, self._show_tooltip)
        except tk.TclError:
            self._tooltip_job = None

    def _show_tooltip(self) -> None:
        self._tooltip_job = None
        if not self._tooltip_text or not self._hovering or self._tooltip_win is not None:
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
            x = self.canvas.winfo_rootx() - 6
            y = self.canvas.winfo_rooty() - h - 8
            try:
                screen_w = tip.winfo_screenwidth()
                screen_h = tip.winfo_screenheight()
                if x + w > screen_w - 6:
                    x = screen_w - w - 6
                if y < 6:
                    y = 6
                if x < 6:
                    x = 6
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


class _HotkeyEntry(ttk.Entry):
    """快捷键输入框：点一下进入录制，直接按下组合键就填进去。

    没有用 readonly 状态 —— ttk 在 readonly 下对按键的处理依赖主题，
    这里改成自己拦下所有按键（返回 "break"），内容只由代码改，
    行为在各主题下都一样。
    """

    PLACEHOLDER = "按下快捷键…"
    HINT_CANCEL = "按 Esc 取消"

    def __init__(self, parent: tk.Misc, value: str, font):
        self._shown = tk.StringVar(master=parent, value=hotkey_mod.display(value))
        # 用专用样式（insertwidth=0）：这个框永远不接受直接打字，
        # 光标闪来闪去反而误导。ttk 没有 insertwidth 选项，只能走 style。
        super().__init__(parent, style="Hotkey.TEntry", font=font,
                         textvariable=self._shown, justify="left")
        self._value = hotkey_mod.normalize(value)
        self._recording = False
        self._mods: list[str] = []
        self._on_change = None

        self.bind("<Button-1>", self._start_recording)
        self.bind("<KeyPress>", self._on_key_press)
        self.bind("<KeyRelease>", self._on_key_release)
        self.bind("<FocusOut>", self._on_focus_out)

    # ---- 和 ttk.Entry 保持同样的接口，_save() 那边不用改 ----

    def get(self) -> str:
        return self._value

    def set_value(self, combo: str) -> None:
        self._value = hotkey_mod.normalize(combo)
        if not self._recording:
            self._shown.set(hotkey_mod.display(self._value))

    @property
    def recording(self) -> bool:
        return self._recording

    # ---- 录制 ----

    def _start_recording(self, _event=None) -> str:
        self._recording = True
        self._mods = []
        self.focus_set()
        self._render_preview()
        try:
            self.configure(foreground=_ACCENT)
        except tk.TclError:
            pass
        return "break"          # 别让 Entry 去定位光标

    def _on_key_press(self, event) -> str:
        if not self._recording:
            # 没在录制时也不允许手打，避免和录制逻辑打架
            return "break"

        if event.keysym in ("Escape", "Esc"):
            self._cancel()
            return "break"

        mod = hotkey_mod.keysym_to_modifier(event.keysym)
        if mod:
            if mod not in self._mods:
                self._mods.append(mod)
            self._render_preview()
            return "break"

        key = hotkey_mod.keysym_to_name(event.keysym)
        if not key:
            return "break"

        combo = hotkey_mod.build_combo(self._mods, key)
        ok, _reason = hotkey_mod.validate(combo)
        if not ok:
            return "break"

        self._value = combo
        self._finish()
        if self._on_change is not None:
            self._on_change(combo)
        return "break"

    def _on_key_release(self, event) -> str:
        if not self._recording:
            return "break"
        # 松开修饰键时把预览收回去，不然按一下 Ctrl 再松手会一直停在 "Ctrl+…"
        mod = hotkey_mod.keysym_to_modifier(event.keysym)
        if mod and mod in self._mods:
            self._mods.remove(mod)
            self._render_preview()
        return "break"

    def _on_focus_out(self, _event=None) -> None:
        if self._recording:
            self._cancel()

    def _render_preview(self) -> None:
        if not self._mods:
            self._shown.set(self.PLACEHOLDER)
            return
        mods = [m.capitalize() for m in hotkey_mod.MODIFIER_ORDER if m in self._mods]
        self._shown.set("+".join(mods) + "+…")

    def _finish(self) -> None:
        self._recording = False
        self._mods = []
        self._shown.set(hotkey_mod.display(self._value))
        try:
            self.configure(foreground=_TEXT)
        except tk.TclError:
            pass

    def _cancel(self) -> None:
        self._finish()          # _value 没被改过，等于恢复原值


class SettingsWindow:
    """设置窗口。用 show() 打开，模态。"""

    def __init__(self, root: tk.Misc, cfg: Config, on_save=None):
        self._root = root
        self._cfg = cfg
        self._on_save = on_save
        self._drag_offset = None
        self._saved = False
        self._closed = False
        # 图标在"还没补间距"时离窗口左边/下边的距离，窗口映射到屏幕上后测一次
        self._icon_base_inset = None
        self._testing = False
        self._fetching = False
        self._model_cache: list[str] = []               # 从接口拉到的模型名
        self._test_queue: queue.Queue = queue.Queue()   # 后台线程 -> 主线程
        self._test_poll = None

        family = _pick_font(root)
        self._f_label = tkfont.Font(root=root, family=family, size=10)
        self._f_section = tkfont.Font(root=root, family=family, size=10, weight="bold")
        self._f_value = tkfont.Font(root=root, family=family, size=10)
        self._f_title = tkfont.Font(root=root, family=family, size=11)
        self._f_hint = tkfont.Font(root=root, family=family, size=9)

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
        body.pack(fill="both", expand=True, padx=16, pady=(0, 0))

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

        # ---- 翻译服务 ----
        # 标题行右侧直接放「测试服务」，结果就显示在按钮右边 —— 这样这一块
        # 少占两行，而且分区块之间本来就有分割线，不需要那么大的间隙
        self._section(body, "翻译服务", self._build_test_controls)
        self._build_api_block(body)
        self._target = self._field_row(body, "目标语言", self._cfg.target_language)

        self._spacer(body)

        # ---- 快捷键 ----
        self._section(body, "快捷键")
        self._hotkey = self._hotkey_row(body, "截图翻译", self._cfg.hotkey)
        self._exit_hotkey = self._hotkey_row(body, "退出程序", self._cfg.exit_hotkey)
        self._hotkey_tip = tk.Label(
            body, text="点击输入框，然后直接按下想要的组合键；按 Esc 取消",
            bg=_BG, fg=_TEXT_DIM, font=self._f_hint, anchor="w")
        self._hotkey_tip.pack(fill="x", pady=(1, 0))

        # ---- 底部 ----
        footer = tk.Frame(inner, bg=_BG)
        footer.pack(fill="x", padx=18, pady=(12, 16))
        save = _FlatButton(footer, "保存", self._save, self._f_title, primary=True)
        save.pack(side="right")
        cancel = _FlatButton(footer, "取消", self._close, self._f_label)
        cancel.pack(side="right", padx=(0, 10))
        # 仓库图标放左下角。pack 是按顶边对齐的，而图标画布比按钮矮，
        # 所以量完两边的实际高度再补一点上边距，让它们垂直居中在同一条线上。
        self._repo = _RepoButton(footer, _REPO_URL, "打开项目仓库", self._f_label)
        self._repo.pack(side="left")
        # 版本号跟在图标右边，和图标一样能点开仓库。pack 会把它排在图标之后，
        # 所以 _align_footer_icon 给图标补的左内边距会把它一起推到位。
        self._version = tk.Label(footer, text="v" + APP_VERSION, bg=_BG,
                                 fg=_TEXT_DIM, font=self._f_label, cursor="hand2")
        self._version.pack(side="left", padx=(7, 0))
        self._version.bind("<Button-1>", lambda _e: self._open_repo())
        self._version.bind("<Enter>",
                           lambda _e: self._version.configure(fg=_ICON_HOVER))
        self._version.bind("<Leave>",
                           lambda _e: self._version.configure(fg=_TEXT_DIM))
        self._footer = footer
        self._cancel_button = cancel

        # 这行只在保存失败时才有字。空着的时候不 pack —— 否则它是一个空标签，
        # 照样占一行高度，按钮下面就会空出一块
        self._hint = tk.Label(inner, text="", bg=_BG, fg=_TEXT_DIM, font=self._f_label)

    def _align_footer_icon(self) -> None:
        """把仓库图标摆到距窗口左边/下边各 _REPO_INSET 的位置。

        基准（还没补间距时图标离窗口边多远）在窗口真正显示到屏幕上之后测一次
        并缓存：测之前先把内边距清零，否则量到的是上一次补完的位置，间距会
        越加越大。首次调用必须发生在窗口已映射之后 —— 构造阶段即使
        update_idletasks 也拿不到坐标，Tk 对没映射的窗口一律返回 0。

        为什么不写解析式：图标画布比按钮矮，pack 在槽位里按中心分配空余高度，
        pady 最终换算成多少像素和 footer 内部排布耦合，推导过几轮都对不上。
        改成"实测基准 + 实测标定的偏移"，换布局之后重新量一次就准。
        """
        widget = self._repo.widget
        self.win.update_idletasks()
        if self._icon_base_inset is None:
            self._repo.pack_configure(padx=(0, 0), pady=(0, 0))
            self.win.update()
            base = (
                widget.winfo_rootx() - self.win.winfo_rootx(),
                (self.win.winfo_rooty() + self.win.winfo_height())
                - (widget.winfo_rooty() + widget.winfo_height()),
            )
            if base[0] <= 0 or base[1] <= 0:
                # 窗口还没映射到屏幕上，坐标是假的，等下次再测
                return
            self._icon_base_inset = base

        base_x, base_y = self._icon_base_inset
        # 实测的 base 和 _REPO_INSET 是同一口径（图标外沿离窗口边），直接补差额；
        # _REPO_PADY_TRIM 是标定出来的修正量，见常量处的注释
        padx = max(0, _REPO_INSET - base_x)
        pady = max(0, _REPO_INSET_BOTTOM - base_y) + _REPO_PADY_TRIM
        self._repo.pack_configure(padx=(padx, 0), pady=(max(0, pady), 0))

    def _open_repo(self) -> None:
        """用浏览器打开项目仓库。左下角的图标和版本号都走这里。"""
        try:
            webbrowser.open(_REPO_URL)
        except Exception:
            # 打不开浏览器不算错误，别把设置窗口弄崩
            log.exception("打开仓库地址失败：%s", _REPO_URL)

    def _build_titlebar(self, parent: tk.Misc) -> None:
        bar = tk.Frame(parent, bg=_BG, height=36)
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
        self._close_button = close          # 留个引用，测试要模拟悬停

        # 拖动窗口
        for widget in (bar, title):
            widget.bind("<Button-1>", self._drag_start)
            widget.bind("<B1-Motion>", self._drag_move)
        self.win.bind("<Escape>", lambda _e: self._close())

    def _build_api_block(self, parent) -> None:
        """接口配置：地址 + Key + 从接口拉取的模型列表。

        只支持 OpenAI 兼容接口 —— 不预设服务商，地址自己填。
        """
        rows = tk.Frame(parent, bg=_BG)
        rows.pack(fill="x")

        self._base_url = self._field_row(rows, "接口地址", self._cfg.api_base_url)
        self._api_key = self._field_row(rows, "API Key", self._cfg.api_key)
        self._model_row(rows, self._cfg.model)

        self._model_hint = tk.Label(rows, text="", bg=_BG, fg=_TEXT_DIM,
                                    font=self._f_label, anchor="w", justify="left",
                                    wraplength=_WIN_W - 80)
        self._model_hint.pack(fill="x", pady=(2, 0))

    def _model_row(self, parent, value: str) -> None:
        """模型：从接口拉列表来选，也能手输（下拉里没有的名字）。"""
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=1)
        tk.Label(row, text="模型", bg=_BG, fg=_TEXT, font=self._f_label,
                 width=_LABEL_W, anchor="w").pack(side="left")

        self._fetch_btn = tk.Label(row, text="拉取列表", bg=_BTN, fg=_TEXT,
                                   font=self._f_label, padx=10, pady=5,
                                   cursor="hand2")
        self._fetch_btn.pack(side="right", padx=(6, 0))
        self._fetch_btn.bind("<Button-1>", lambda _e: self._fetch_models())
        self._fetch_btn.bind("<Enter>", lambda _e: self._fetch_btn.configure(bg=_BTN_HOVER))
        self._fetch_btn.bind("<Leave>", lambda _e: self._fetch_btn.configure(bg=_BTN))

        choices = self._model_choices(value)
        self._model = _Select(row, choices, self._f_label, bg=_FIELD,
                              on_change=lambda _v: self._refresh_model_hint(),
                              text_font=self._f_value)
        self._model.pack(side="left", fill="x", expand=True, ipady=1)
        self._model.set_value(value or choices[0][1])

    def _model_choices(self, value: str = "") -> list[tuple[str, str]]:
        """下拉选项：已经拉到的模型 + 当前配置里的那个。

        没有「手动输入」这一项 —— 模型名一律来自接口，避免手打出不存在的名字。
        """
        choices: list[tuple[str, str]] = []
        for name in self._model_cache:
            choices.append((name, name))
        for name in (value, self._cfg.model):
            if name and all(existing != name for _label, existing in choices):
                choices.insert(0, (name, name))
        if not choices:
            # 还没拉过列表，给个占位项，免得下拉是空的
            choices.append((_EMPTY_LABEL, _EMPTY_LABEL))
        return choices

    def _refresh_model_hint(self) -> None:
        """这行只提示"选的是不是能看图的模型"，顺带说明接口类型。"""
        try:
            name = self._model.value
            if name in ("", _EMPTY_LABEL) or model_list.looks_like_vision(name):
                text = "只支持 OpenAI 兼容接口（/chat/completions）"
            else:
                text = "这个名字看不出是视觉模型；识别不了截图的话换一个"
            self._model_hint.configure(text=text, fg=_TEXT_DIM)
        except (tk.TclError, AttributeError):
            pass

    # ---- 拉取模型列表 ----

    def _fetch_models(self) -> None:
        if self._fetching or self._closed:
            return
        self._fetching = True
        self._fetch_btn.configure(text="拉取中…", cursor="arrow")
        self._status.configure(text="", fg=_TEXT_DIM)
        threading.Thread(target=self._run_fetch, args=(self._cfg_from_ui(),),
                         daemon=True).start()
        self._start_result_poll()

    def _run_fetch(self, cfg: Config) -> None:
        """后台线程：只把结果放进队列，不碰任何控件。"""
        try:
            result = model_list.list_models(cfg)
        except Exception as exc:                # 兜底，别让线程静默死掉
            log.exception("拉取模型列表异常")
            result = model_list.ModelList(False, "拉取失败", detail=str(exc))
        self._test_queue.put(("models", result))

    def _apply_models(self, result) -> None:
        self._fetching = False
        try:
            self._fetch_btn.configure(text="拉取列表", cursor="hand2")
            if not result.ok:
                log.warning("拉取模型列表失败：%s %s", result.message, result.detail)
                self._status.configure(text="拉取失败", fg=_CLOSE_HOVER)
                return
            self._model_cache = list(result.models)
            keep = self._model.value
            self._model._pairs = []             # 换一批选项，保留当前选中值
            for label, name in self._model_choices(keep):
                self._model.add_value(name, label)
            self._model.set_value(keep if keep in result.models else
                                  (result.vision[0] if result.vision else result.models[0]))
            self._refresh_model_hint()
            self._status.configure(
                text="拉到 %d 个模型（猜测 %d 个能看图），下拉里选" % (
                    len(result.models), len(result.vision)),
                fg=_TEXT_DIM)
        except tk.TclError:
            pass

    def _collect_fields(self) -> None:
        """把界面上填的值收回配置对象（保存、测试、拉取前都要用）。

        接口地址和模型名留空时保留原值 —— 空着等于没填，不该把配置弄没
        （API Key 例外，它本来就应该允许清空）。
        """
        cfg = self._cfg
        cfg.api_key = self._api_key.get().strip()
        url = self._base_url.get().strip()
        if url:
            cfg.api_base_url = url
        model = self._model.value
        if model and model != _EMPTY_LABEL:
            cfg.model = model

    def _cfg_from_ui(self) -> Config:
        """用界面上当前填的值拼一份配置副本，不动真正的 cfg。"""
        import copy

        cfg = copy.copy(self._cfg)
        cfg.api_key = self._api_key.get().strip()
        cfg.api_base_url = self._base_url.get().strip() or self._cfg.api_base_url
        model = self._model.value
        cfg.model = model if model and model != _EMPTY_LABEL else self._cfg.model
        return cfg

    def _open_url(self, url: str) -> None:
        try:
            webbrowser.open(url)
        except Exception:
            log.exception("打开链接失败：%s", url)

    # ---- 测试服务 ----

    def _test_service(self) -> None:
        """后台线程里真发一次请求，结果回主线程显示（Tk 不是线程安全的）。"""
        if self._testing or self._closed:
            return
        self._testing = True
        try:
            self._status.configure(text="测试中…", fg=_ACCENT)
        except tk.TclError:              # 窗口已经销毁
            self._testing = False
            return
        self._test_btn.configure(cursor="arrow")
        threading.Thread(target=self._run_test, args=(self._cfg_from_ui(),),
                         daemon=True).start()
        self._start_result_poll()

    def _run_test(self, cfg: Config) -> None:
        """后台线程：发请求，把结果放进队列就完事。

        这里绝对不能碰界面（连 self.win.after 都不行）—— Tk 只在主线程安全。
        """
        try:
            result = service_test.test_service(cfg)
        except Exception as exc:                 # 兜底，别让线程静默死掉
            log.exception("测试服务异常")
            result = service_test.ServiceTestResult(False, service_test.FAIL_TEXT, str(exc))
        self._test_queue.put(("test", result))

    def _start_result_poll(self) -> None:
        """主线程轮询队列，把结果取出来显示。"""
        if self._test_poll is not None:
            return
        self._poll_result()

    def _poll_result(self) -> None:
        self._test_poll = None
        if self._closed:
            # 窗口已经销毁了：控件都没了，结果直接丢掉，别再排下一轮
            return
        try:
            kind, result = self._test_queue.get_nowait()
        except queue.Empty:
            try:
                self._test_poll = self.win.after(120, self._poll_result)
            except (tk.TclError, RuntimeError):
                self._test_poll = None
            return
        if kind == "models":
            self._apply_models(result)
        else:
            self._show_test_result(result)

    def _show_test_result(self, result) -> None:
        """界面上只给「连接成功」或「连接失败」两个结论，原因看日志。"""
        self._testing = False
        try:
            if self._closed:
                return
            self._status.configure(text=result.message,
                                   fg=_ACCENT if result.ok else _CLOSE_HOVER)
            self._test_btn.configure(cursor="hand2")
        except tk.TclError:
            pass

    def _section(self, parent, text: str, trailing=None) -> None:
        """区块标题。trailing 是个回调，用来在标题行右侧挂控件。

        分块之间已经有分割线，所以这里留白很小就够 —— 靠线分区，不靠空白。
        """
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=(4, 2))
        tk.Label(row, text=text, bg=_BG, fg=_TEXT_DIM,
                 font=self._f_section).pack(side="left")
        if trailing is not None:
            trailing(row)

    def _build_test_controls(self, row) -> None:
        """标题行右侧的「测试服务」按钮，结果文字贴在它左边。"""
        self._test_btn = _FlatButton(row, "测试服务", self._test_service,
                                     self._f_label, primary=True)
        self._test_btn.pack(side="right")
        # 先 pack 按钮再 pack 结果：side="right" 是从右往左依次排
        self._status = tk.Label(row, text="", bg=_BG, fg=_TEXT_DIM,
                                font=self._f_label, anchor="e")
        self._status.pack(side="right", padx=(0, 8))

    def _spacer(self, parent: tk.Misc) -> None:
        tk.Frame(parent, bg=_BORDER, height=1).pack(fill="x", pady=(6, 0))

    def _slider_row(self, parent: tk.Misc, label: str, value, low, high,
                    formatter, step: float = 0.0) -> _Slider:
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=1)

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

    def _hotkey_row(self, parent: tk.Misc, label: str, value: str) -> "_HotkeyEntry":
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=1)

        tk.Label(row, text=label, bg=_BG, fg=_TEXT, font=self._f_label,
                 width=_LABEL_W, anchor="w").pack(side="left")

        entry = _HotkeyEntry(row, value, self._f_value)
        entry.pack(side="left", fill="x", expand=True, ipady=2)
        return entry

    def _field_row(self, parent: tk.Misc, label: str, value: str) -> ttk.Entry:
        row = tk.Frame(parent, bg=_BG)
        row.pack(fill="x", pady=1)

        tk.Label(row, text=label, bg=_BG, fg=_TEXT, font=self._f_label,
                 width=_LABEL_W, anchor="w").pack(side="left")

        entry = ttk.Entry(row, style="Dark.TEntry", font=self._f_value)
        entry.insert(0, value or "")
        entry.pack(side="left", fill="x", expand=True, ipady=2)
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
        # 光 focus_force 只改 Tk 内部焦点；不是系统前台窗口就收不到键盘消息，
        # 快捷键录制会按了没反应
        bring_to_front(self.win)
        # 先跑完一轮完整布局（update 而不是 update_idletasks），
        # 否则量到的还是映射前的 0 坐标（见 _align_footer_icon）
        self.win.update()
        self._align_footer_icon()
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
        # 先收提示窗：它是个独立的 Toplevel，不跟着设置窗口一起销毁
        self._repo.destroy_tooltip()
        if self._test_poll is not None:
            try:
                self.win.after_cancel(self._test_poll)
            except (tk.TclError, ValueError):
                pass
            self._test_poll = None
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

    def _show_hint(self, text: str) -> None:
        """显示底部那行提示。空字符串就把整行收掉，不留空位。"""
        if not text:
            self._hint.pack_forget()
            return
        self._hint.configure(text=text)
        if not self._hint.winfo_ismapped():
            self._hint.pack(pady=(0, 6))

    def _check_hotkeys(self, capture: str, exit_combo: str) -> str:
        """保存前校验两个快捷键。返回空串表示没问题，否则是给用户看的原因。"""
        for name, combo in (("截图翻译", capture), ("退出程序", exit_combo)):
            ok, reason = hotkey_mod.validate(combo)
            if not ok:
                return "%s的快捷键不可用：%s" % (name, reason)
            try:
                import keyboard

                keyboard.parse_hotkey(combo)
            except Exception:
                return "%s的快捷键「%s」无法识别，换一个试试" % (name, combo)

        if hotkey_mod.same(capture, exit_combo):
            return "两个快捷键不能相同"
        return ""

    def _save(self) -> None:
        capture = self._hotkey.get().strip()
        exit_combo = self._exit_hotkey.get().strip()
        problem = self._check_hotkeys(capture, exit_combo)
        if problem:
            self._show_hint(problem)
            return

        cfg = self._cfg
        cfg.mask_opacity = round(float(self._mask.value), 3)
        cfg.overlay_opacity = round(float(self._overlay.value), 3)
        cfg.font_size = int(round(float(self._fontsize.value)))
        self._collect_fields()                 # 接口地址 / Key / 模型收回配置
        cfg.target_language = self._target.get().strip() or cfg.target_language
        cfg.hotkey = capture
        cfg.exit_hotkey = exit_combo

        self._saved = True
        if self._on_save:
            try:
                self._on_save(cfg)
            except Exception:
                log.exception("保存配置失败")
                try:
                    self._show_hint("保存失败，详见日志")
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
    for name in ("Dark.TEntry", "Hotkey.TEntry"):
        style.configure(
            name,
            fieldbackground=_FIELD, background=_FIELD, foreground=_TEXT,
            insertcolor=_TEXT, bordercolor=_FIELD_BORDER, lightcolor=_FIELD,
            darkcolor=_FIELD, borderwidth=1, relief="flat", padding=4,
        )
        style.map(name,
                  bordercolor=[("focus", _ACCENT)],
                  lightcolor=[("focus", _ACCENT)],
                  darkcolor=[("focus", _ACCENT)])

    # 快捷键录制框不给文本光标：它不接受直接输入
    style.configure("Hotkey.TEntry", insertwidth=0)


def open_settings(root: tk.Misc, cfg: Config, on_save=None) -> bool:
    """打开设置窗口（模态）。返回是否保存了改动。"""
    install_styles(root)
    window = SettingsWindow(root, cfg, on_save)
    return window.show()


class _Select(tk.Label):
    """自绘下拉框：深色、无系统边框，点一下弹出选项列表。

    用 ttk.Combobox 的话弹窗是系统原生浅色列表，跟这套深色界面凑不到一起，
    所以这里自己做一个（选项少、结构固定，实现很短）。
    """

    def __init__(self, parent: tk.Misc, values: list[tuple[str, str]], font,
                 bg: str = _CARD, on_change=None, text_font=None):
        self._pairs = list(values)          # [(显示名, 值), ...]
        self._value = self._pairs[0][1] if self._pairs else ""
        self._on_change = on_change
        self._bg = bg
        self._popup: tk.Toplevel | None = None
        self._font = text_font or font
        self._drop_font = font
        super().__init__(parent, text=self._label_for(self._value), font=self._font,
                         bg=bg, fg=_TEXT, anchor="w", padx=10, pady=6,
                         cursor="hand2", highlightthickness=0, bd=0)
        self.bind("<Button-1>", self._toggle)
        self.bind("<Enter>", lambda _e: self._paint(_FIELD if not self._popup else _BTN))
        self.bind("<Leave>", lambda _e: self._paint(bg if not self._popup else _BTN))

    # ---- 对外 ----

    @property
    def value(self) -> str:
        return self._value

    def get(self) -> str:
        """和 Entry.get() 保持一样的接口，调用方不用区分是哪种控件。"""
        return self._value

    def set_value(self, value: str, notify: bool = False) -> None:
        for _label, key in self._pairs:
            if key == value:
                self._value = key
                self.configure(text=self._label_for(key))
                break
        if notify and self._on_change:
            self._on_change(self._value)

    def add_value(self, value: str, label: str = "") -> None:
        """把一个不在预设里的值加进选项（比如用户自己填的模型名）。"""
        if any(key == value for _label, key in self._pairs):
            return
        self._pairs.append((label or value, value))

    def _label_for(self, value: str) -> str:
        for label, key in self._pairs:
            if key == value:
                return label
        return value

    def _paint(self, color: str) -> None:
        try:
            self.configure(bg=color)
        except tk.TclError:
            pass

    # ---- 弹出 ----

    def _toggle(self, _event=None) -> None:
        if self._popup is not None:
            self._hide()
            return
        self._show()

    def _show(self) -> None:
        self._paint(_BTN)
        try:
            top = self.winfo_toplevel()
            popup = tk.Toplevel(top)
            popup.overrideredirect(True)
            popup.attributes("-topmost", True)
            popup.configure(bg=_BORDER)
            inner = tk.Frame(popup, bg=_CARD)
            inner.pack(padx=1, pady=1, fill="both", expand=True)
            for text, key in self._pairs:
                row = tk.Label(inner, text=text, font=self._drop_font, bg=_CARD,
                               fg=_TEXT if key != self._value else _ACCENT,
                               anchor="w", padx=10, pady=6, cursor="hand2")
                row.pack(fill="x")
                row.bind("<Enter>", lambda _e, r=row: r.configure(bg=_BTN_HOVER))
                row.bind("<Leave>", lambda _e, r=row: r.configure(bg=_CARD))
                row.bind("<Button-1>", lambda _e, k=key: self._choose(k))
            popup.update_idletasks()
            width = max(self.winfo_width(), popup.winfo_reqwidth())
            x = self.winfo_rootx()
            y = self.winfo_rooty() + self.winfo_height()
            popup.geometry("{}x{}+{}+{}".format(
                width, popup.winfo_reqheight(), x, y))
            self._popup = popup
            set_round_corners(popup, small=True)
            # 点到别处就收起来
            popup.bind("<FocusOut>", lambda _e: self._hide())
            popup.focus_force()
        except tk.TclError:
            self._popup = None

    def _choose(self, key: str) -> None:
        self.set_value(key)
        self._hide()
        if self._on_change:
            self._on_change(key)

    def _hide(self) -> None:
        if self._popup is not None:
            try:
                self._popup.destroy()
            except tk.TclError:
                pass
            self._popup = None
        self._paint(self._bg)
