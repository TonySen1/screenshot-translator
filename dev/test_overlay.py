# -*- coding: utf-8 -*-
"""浮层交互测试：图标按钮、剪贴板、图标反馈、拖动移动、三种关闭方式。

会临时改写剪贴板，测完把原内容还回去。
"""

import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pyperclip  # noqa: E402

import config as config_module  # noqa: E402
import screenshot  # noqa: E402
from overlay import _ICON_FG, show_overlay  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def icon_color(button) -> str:
    """读按钮图标当前的颜色。"""
    item = button._items[0]
    return button.canvas.itemcget(item, "outline") or button.canvas.itemcget(item, "fill")


def click(button) -> None:
    # 只发一次按下事件：<Button-1> 本身就是 ButtonPress-1。
    # 不能补发 release —— 点 X 之后窗口已经销毁了，再发会报 “bad window path name”。
    button.canvas.event_generate("<Button-1>")


def main() -> int:
    screenshot.enable_dpi_awareness()
    try:
        saved_clipboard = pyperclip.paste()
    except Exception:
        saved_clipboard = ""

    cfg = config_module.load_config(os.path.join(HERE, "dev", "_selftest_config.json"))
    cfg.overlay_bg = "#1e1e1e"
    cfg.overlay_fg = "#ffffff"

    root = tk.Tk()
    root.withdraw()
    screen = screenshot.Rect(0, -588, 4000, 2560)

    closed_calls = []
    overlay = show_overlay(root, cfg, screenshot.Rect(500, 700, 600, 100), screen,
                           on_close=lambda o: closed_calls.append(o))
    overlay.show_result("Hello world\nSecond line", "你好，世界\n第二行")
    root.update()

    # --- 1. 图标按钮存在且初始是灰的 ---
    check("三个图标按钮都已创建",
          all(b is not None for b in (overlay.extract_button, overlay.copy_button,
                                      overlay.close_button)))
    check("未启用时图标是暗灰", icon_color(overlay.extract_button) != "#ffffff",
          icon_color(overlay.extract_button))
    overlay._set_buttons_enabled(True)
    root.update()
    check("启用后图标恢复常规灰", icon_color(overlay.extract_button) == _ICON_FG,
          icon_color(overlay.extract_button))

    # --- 2. 点“提取原文” ---
    pyperclip.copy("__before__")
    click(overlay.extract_button)
    root.update()
    check("点[提取原文]写入剪贴板", pyperclip.paste() == "Hello world\nSecond line",
          repr(pyperclip.paste()))
    check("点完图标闪成高亮色", icon_color(overlay.extract_button) == "#ffffff",
          icon_color(overlay.extract_button))

    # --- 3. 点“复制译文” ---
    pyperclip.copy("__before__")
    click(overlay.copy_button)
    root.update()
    check("点[复制译文]写入剪贴板", pyperclip.paste() == "你好，世界\n第二行",
          repr(pyperclip.paste()))
    check("复制图标也闪成高亮色", icon_color(overlay.copy_button) == "#ffffff")

    # --- 4. 反馈会自动恢复 ---
    def after_flash():
        check("图标自动恢复常规灰", icon_color(overlay.extract_button) == _ICON_FG,
              icon_color(overlay.extract_button))

    root.after(1400, after_flash)

    # --- 5. 空内容不写剪贴板 ---
    def test_empty():
        overlay.show_result("", "")
        root.update()
        overlay._set_buttons_enabled(True)
        before = pyperclip.paste()
        click(overlay.extract_button)
        root.update()
        check("没有内容时不写剪贴板", pyperclip.paste() == before)

    root.after(1600, test_empty)

    # --- 6. 错误状态下按钮禁用 ---
    def test_error():
        overlay.show_error("请先在 config.json 中填写 api_key")
        root.update()
        check("错误提示显示在浮层正文", "api_key" in overlay.text_label.cget("text"))
        check("错误状态下按钮禁用", overlay._buttons_enabled is False)
        before = pyperclip.paste()
        click(overlay.extract_button)
        root.update()
        check("禁用时点击不写剪贴板", pyperclip.paste() == before)
        check("禁用时关闭按钮仍可用", overlay.close_button._enabled is True)

    root.after(1800, test_error)

    # --- 7. 拖动移动浮层 ---
    def test_drag():
        overlay.show_result("x", "y")
        root.update()
        before = (overlay.win.winfo_x(), overlay.win.winfo_y())
        overlay._on_drag_start(type("E", (), {"x_root": 800, "y_root": 800})())
        overlay._on_drag_move(type("E", (), {"x_root": 900, "y_root": 860})())
        root.update()
        after = (overlay.win.winfo_x(), overlay.win.winfo_y())
        check("拖动改变了浮层位置", after != before,
              "%s -> %s" % (before, after))
        check("位移量正确", after == (before[0] + 100, before[1] + 60),
              "%s" % (after,))
        overlay._on_drag_end()
        check("标记了用户已手动移动", overlay._user_moved is True)

    root.after(2000, test_drag)

    # --- 8. 拖动之后更新内容不会再跳回原位 ---
    def test_keep_position():
        pos = (overlay.win.winfo_x(), overlay.win.winfo_y())
        overlay.show_result("x" * 3, "一段更长的译文，用来触发重新排版")
        root.update()
        after = (overlay.win.winfo_x(), overlay.win.winfo_y())
        check("重新排版后位置保持不动", after == pos, "%s -> %s" % (pos, after))

    root.after(2200, test_keep_position)

    # --- 9. 点 X 关闭 ---
    def test_close_button():
        click(overlay.close_button)
        root.update()
        check("点 X 关闭了浮层", overlay.closed is True)
        check("关闭时通知了主程序", len(closed_calls) == 1)

    root.after(2400, test_close_button)

    # --- 10. Esc 关闭（新开一个浮层）---
    overlay2 = None

    def test_escape():
        global overlay2
        overlay2 = show_overlay(root, cfg, screenshot.Rect(500, 700, 600, 100), screen)
        overlay2.show_result("a", "b")
        root.update()
        overlay2.win.event_generate("<Escape>")
        root.update()
        check("Esc 关闭浮层", overlay2.closed is True)

    root.after(2600, test_escape)

    def finish():
        try:
            pyperclip.copy(saved_clipboard)
        except Exception:
            pass
        root.quit()

    root.after(3000, finish)
    root.mainloop()
    root.destroy()

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
