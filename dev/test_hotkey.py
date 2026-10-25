# -*- coding: utf-8 -*-
"""快捷键录制测试：纯解析 + 输入框里的真实按键序列。"""

import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import hotkey  # noqa: E402
import screenshot  # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def press(entry, keysym):
    entry.event_generate("<KeyPress>", keysym=keysym)
    entry.update()


def release(entry, keysym):
    entry.event_generate("<KeyRelease>", keysym=keysym)
    entry.update()


def test_parsing():
    print("--- 解析与规范化 ---", flush=True)
    cases = [
        ("Ctrl + Alt + D", "ctrl+alt+d"),
        ("ALT+F4", "alt+f4"),
        ("shift+ctrl+x", "ctrl+shift+x"),
        ("win+space", "windows+space"),
        ("ctrl+pgup", "ctrl+page up"),
        ("ctrl+alt+d", "ctrl+alt+d"),
    ]
    for raw, expect in cases:
        got = hotkey.normalize(raw)
        check("规范化 %r" % raw, got == expect, "-> %r" % got)

    check("显示大写修饰键", hotkey.display("ctrl+alt+d") == "Ctrl+Alt+D",
          hotkey.display("ctrl+alt+d"))
    check("等价判断忽略顺序和大小写",
          hotkey.same("Ctrl+Alt+D", "alt+ctrl+d"))

    print("--- 校验 ---", flush=True)
    bad = [
        ("", "空"),
        ("ctrl+alt", "只有修饰键"),
        ("ctrl+ctrl+d", "修饰键重复"),
        ("ctrl+alt+d+e", "多个主键"),
        ("ctrl+alt+é", "非 ASCII"),
    ]
    for combo, why in bad:
        ok, reason = hotkey.validate(combo)
        check("拒绝 %s（%s）" % (combo or "空", why), not ok, reason)

    ok, reason = hotkey.validate("ctrl+alt+d")
    check("接受 ctrl+alt+d", ok, reason)

    print("--- keysym 翻译 ---", flush=True)
    pairs = [
        ("D", "d"), ("F5", "f5"), ("Return", "enter"), ("Escape", "escape"),
        ("Prior", "page up"), ("KP_3", "3"), ("space", "space"),
    ]
    for keysym, expect in pairs:
        got = hotkey.keysym_to_name(keysym)
        check("keysym %s -> %s" % (keysym, expect), got == expect, "-> %r" % got)

    # 修饰键由另一条路径处理，keysym_to_name 对它返回 None 是对的
    mods = [("Control_L", "ctrl"), ("Control_R", "ctrl"), ("Alt_L", "alt"),
            ("Shift_R", "shift"), ("Super_L", "windows")]
    for keysym, expect in mods:
        got = hotkey.keysym_to_modifier(keysym)
        check("修饰键 %s -> %s" % (keysym, expect), got == expect, "-> %r" % got)
        check("%s 不走普通键路径" % keysym, hotkey.keysym_to_name(keysym) is None)

    check("普通键不算修饰键", hotkey.keysym_to_modifier("d") is None)


def test_widget():
    print("--- 录制控件 ---", flush=True)
    from settings_window import _HotkeyEntry, install_styles

    screenshot.enable_dpi_awareness()
    root = tk.Tk()
    root.withdraw()
    install_styles(root)

    holder = tk.Toplevel(root)
    holder.withdraw()
    entry = _HotkeyEntry(holder, "ctrl+alt+d", None)
    entry.pack()
    holder.deiconify()
    entry.update()

    check("初始值已规范化", entry.get() == "ctrl+alt+d", repr(entry.get()))
    check("初始显示成 Ctrl+Alt+D", entry._shown.get() == "Ctrl+Alt+D",
          repr(entry._shown.get()))
    check("初始不在录制", entry.recording is False)

    # 点击进入录制
    entry.event_generate("<Button-1>", x=5, y=5)
    entry.update()
    check("点击后进入录制", entry.recording is True)
    check("录制时显示占位提示", entry._shown.get() == _HotkeyEntry.PLACEHOLDER,
          repr(entry._shown.get()))

    # 逐级按下 Ctrl -> Alt -> D
    press(entry, "Control_L")
    check("按住 Ctrl 时预览", entry._shown.get() == "Ctrl+…", repr(entry._shown.get()))
    press(entry, "Alt_L")
    check("再加 Alt 时预览", entry._shown.get() == "Ctrl+Alt+…",
          repr(entry._shown.get()))
    press(entry, "D")
    check("按下 D 后录制完成", entry.recording is False)
    check("得到 ctrl+alt+d", entry.get() == "ctrl+alt+d", repr(entry.get()))
    check("显示成 Ctrl+Alt+D", entry._shown.get() == "Ctrl+Alt+D",
          repr(entry._shown.get()))

    # 松开修饰键不应影响结果
    release(entry, "Alt_L")
    release(entry, "Control_L")
    check("松开修饰键不影响结果", entry.get() == "ctrl+alt+d", repr(entry.get()))

    # 录一个带 Shift 的
    entry.event_generate("<Button-1>", x=5, y=5)
    entry.update()
    press(entry, "Shift_L")
    press(entry, "Control_L")
    press(entry, "F5")
    check("可以录制 Ctrl+Shift+F5", entry.get() == "ctrl+shift+f5", repr(entry.get()))

    # Esc 取消：值保持原样
    entry.event_generate("<Button-1>", x=5, y=5)
    entry.update()
    press(entry, "Control_L")
    press(entry, "Escape")
    check("Esc 取消录制", entry.recording is False)
    check("Esc 取消后值不变", entry.get() == "ctrl+shift+f5", repr(entry.get()))
    check("Esc 取消后显示恢复", entry._shown.get() == "Ctrl+Shift+F5",
          repr(entry._shown.get()))

    # 失焦取消
    entry.event_generate("<Button-1>", x=5, y=5)
    entry.update()
    press(entry, "Alt_L")
    entry.event_generate("<FocusOut>")
    entry.update()
    check("失焦自动取消录制", entry.recording is False)
    check("失焦后值不变", entry.get() == "ctrl+shift+f5", repr(entry.get()))

    # 未录制时按键不改变内容（防止手打和录制打架）
    entry.event_generate("<Button-1>", x=5, y=5)
    entry.update()
    press(entry, "Control_L")
    press(entry, "Escape")
    before = entry.get()
    press(entry, "z")
    check("未录制时按键被忽略", entry.get() == before, repr(entry.get()))

    # set_value 外部赋值
    entry.set_value("Alt+F4")
    check("set_value 规范化", entry.get() == "alt+f4", repr(entry.get()))
    check("set_value 刷新显示", entry._shown.get() == "Alt+F4",
          repr(entry._shown.get()))

    holder.destroy()
    root.destroy()


def main() -> int:
    # CI 的 Windows runner 没有交互式桌面，建不了 Tk 窗口，
    # 所以只跑纯解析那一半：python dev/test_hotkey.py --parsing-only
    parsing_only = "--parsing-only" in sys.argv
    test_parsing()
    if not parsing_only:
        test_widget()
    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
