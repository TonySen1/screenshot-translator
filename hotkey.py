# -*- coding: utf-8 -*-
"""快捷键的解析、规范化和校验。

设置窗口里"按下组合键自动填入"靠这里把 Tk 的 keysym 翻成 keyboard 库认的名字；
手打输入也走同一套规范化，所以 `Ctrl + Alt + D` 和 `ctrl+alt+d` 是同一个东西。
"""

from __future__ import annotations

import re

# 修饰键的规范顺序（拼出来的字符串统一成这个顺序，方便显示和比较）
MODIFIER_ORDER = ("ctrl", "alt", "shift", "windows")

# Tk 的 keysym -> keyboard 库的键名
_MODIFIER_KEYSYMS = {
    "Control_L": "ctrl", "Control_R": "ctrl",
    "Alt_L": "alt", "Alt_R": "alt",
    "Shift_L": "shift", "Shift_R": "shift",
    "Super_L": "windows", "Super_R": "windows",
    "Win_L": "windows", "Win_R": "windows",
}

_SPECIAL_KEYSYMS = {
    "Return": "enter", "KP_Enter": "enter",
    "Escape": "escape", "Esc": "escape",
    "space": "space", "Tab": "tab",
    "BackSpace": "backspace", "Delete": "delete", "Insert": "insert",
    "Home": "home", "End": "end",
    "Prior": "page up", "Next": "page down",
    "Up": "up", "Down": "down", "Left": "left", "Right": "right",
    "Print": "print screen", "Sys_Req": "print screen",
    "Pause": "pause", "Menu": "apps",
    "comma": ",", "period": ".", "slash": "/", "backslash": "\\",
    "semicolon": ";", "apostrophe": "'",
    "bracketleft": "[", "bracketright": "]",
    "minus": "-", "equal": "=", "grave": "`",
    "KP_Add": "+", "KP_Subtract": "-", "KP_Multiply": "*", "KP_Divide": "/",
    "KP_Decimal": ".", "KP_Separator": ",",
}

# 手打输入时也认这些别名
_ALIASES = {
    "control": "ctrl", "ctl": "ctrl", "strg": "ctrl",
    "option": "alt", "opt": "alt",
    "win": "windows", "super": "windows", "meta": "windows", "cmd": "windows",
    "return": "enter", "esc": "escape", "del": "delete",
    "ins": "insert", "pgup": "page up", "pgdn": "page down",
    "pgdown": "page down", "spacebar": "space",
    "printscreen": "print screen", "prtsc": "print screen",
    "plus": "+", "comma": ",", "period": ".", "dot": ".",
}

_FUNCTION_KEY = re.compile(r"^f([1-9]|1[0-9]|2[0-4])$")


def keysym_to_name(keysym: str) -> str | None:
    """Tk 的 keysym 转成 keyboard 库的键名；不认识返回 None。"""
    if not keysym:
        return None

    if keysym in _SPECIAL_KEYSYMS:
        return _SPECIAL_KEYSYMS[keysym]

    # 小键盘数字 KP_0 .. KP_9
    if keysym.startswith("KP_") and len(keysym) == 4 and keysym[3].isdigit():
        return keysym[3]

    if len(keysym) == 1 and keysym.isprintable():
        return keysym.lower()

    low = keysym.lower()
    if _FUNCTION_KEY.match(low):
        return low
    return None


def keysym_to_modifier(keysym: str) -> str | None:
    """如果这个 keysym 是修饰键，返回它的规范名。"""
    return _MODIFIER_KEYSYMS.get(keysym)


def build_combo(modifiers: list[str], key: str) -> str:
    """按固定顺序把修饰键和主键拼成 `ctrl+alt+d` 这种形式。"""
    ordered = [m for m in MODIFIER_ORDER if m in modifiers]
    return "+".join(ordered + [key])


def normalize(text: str) -> str:
    """把用户手打的字符串规范成标准形式。解析不出来就原样返回（去掉首尾空白）。

    例：'Ctrl + Alt + D' -> 'ctrl+alt+d'；'ALT+F4' -> 'alt+f4'
    """
    if not text:
        return ""

    parts = [p.strip() for p in text.split("+") if p.strip()]
    # '+' 本身作为主键时会被 split 拆成空串，上面已经过滤，这里补回来
    if text.strip().endswith("+") and parts and parts[-1].lower() != "plus":
        parts.append("+")

    modifiers: list[str] = []
    key = ""
    for index, part in enumerate(parts):
        low = part.lower()
        low = _ALIASES.get(low, low)
        if low in MODIFIER_ORDER:
            if low not in modifiers:
                modifiers.append(low)
            continue
        # '+' 键：可能是 "+" 本身，也可能最后一段
        if index == len(parts) - 1:
            key = low
        elif not key:
            key = low

    if not key:
        return text.strip()
    return build_combo(modifiers, key)


def validate(combo: str) -> tuple[bool, str]:
    """检查一个快捷键是否可用。返回 (是否合法, 不合法时的原因)。"""
    text = (combo or "").strip()
    if not text:
        return False, "快捷键不能为空"

    parts = [p for p in text.split("+") if p]
    if not parts:
        return False, "快捷键不能为空"

    modifiers = [p for p in parts if p.lower() in MODIFIER_ORDER]
    keys = [p for p in parts if p.lower() not in MODIFIER_ORDER]

    if len(set(modifiers)) != len(modifiers):
        return False, "修饰键重复了"

    if not keys:
        # 只剩修饰键（比如 "ctrl+alt"），按下去没法触发
        return False, "还需要一个主键，例如 ctrl+alt+d 里的 d"

    if len(keys) > 1:
        return False, "只能有一个主键"

    key = keys[0].lower()
    if key in MODIFIER_ORDER:
        return False, "主键不能是修饰键"

    if not key.isascii():
        # keyboard 库只认英文键盘上的键名，中文输入法打出来的字符它解析不了
        return False, "只支持英文键盘上的按键"

    if len(key) == 1 and key.isalpha() and not modifiers:
        return True, "单个字母键容易误触，建议加上 Ctrl 或 Alt"
    if len(key) == 1 and key.isdigit() and not modifiers:
        return True, "单个数字键容易误触，建议加上 Ctrl 或 Alt"

    return True, ""


def same(a: str, b: str) -> bool:
    """两个快捷键是否等价（忽略大小写、空格和修饰键顺序）。"""
    return normalize(a).lower() == normalize(b).lower()


def display(combo: str) -> str:
    """给界面显示的写法：Ctrl+Alt+D（首字母大写，更好认）。"""
    text = normalize(combo)
    if not text:
        return ""
    parts = []
    for part in text.split("+"):
        if not part:
            continue
        parts.append(part.capitalize() if len(part) > 1 else part.upper())
    return "+".join(parts)
