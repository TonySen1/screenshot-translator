# -*- coding: utf-8 -*-
"""设置窗口左下角仓库图标的测试。

图标是位图（assets/github-light.ico）：这里既检查资源本身，
也开窗口检查它在 footer 里的位置、悬停换色和点击行为 ——
所以需要能开出窗口的环境，拿不到显示器就整体跳过。
"""

import os
import shutil
import sys
import tempfile
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import icons  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def asset_checks() -> None:
    """资源本身：文件在、尺寸够大、有透明通道（换色靠它当遮罩）。"""
    from PIL import Image

    path = os.path.join(HERE, "assets", "github-light.ico")
    check("图标文件存在", os.path.exists(path), path)

    with Image.open(path) as src:
        src.load()
        size = src.size
        alpha = src.convert("RGBA").getchannel("A")
    check("图标是正方形且足够大（缩到 20px 才不糊）",
          size[0] == size[1] and size[0] >= 128, "%dx%d" % size)
    lo, hi = alpha.getextrema()
    check("图标有透明通道（背景透明、前景不透明）", lo == 0 and hi == 255,
          "alpha %d..%d" % (lo, hi))


def ui_checks() -> None:
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        print("跳过窗口相关检查：%s" % exc, flush=True)
        return
    root.withdraw()

    import config as config_module  # noqa: E402
    from icons import draw_icon, tint_bitmap  # noqa: E402
    import settings_window as sw  # noqa: E402
    from settings_window import SettingsWindow, install_styles, _REPO_URL  # noqa: E402
    install_styles(root)

    # 用临时配置：load_config 在文件不存在时会往仓库目录里写一份
    temp_dir = tempfile.mkdtemp()
    temp_cfg = os.path.join(temp_dir, "config.json")
    source = os.path.join(HERE, "config.json")
    try:
        if os.path.exists(source):
            shutil.copyfile(source, temp_cfg)
        cfg = config_module.load_config(temp_cfg)
        win = SettingsWindow(root, cfg, on_save=lambda _c: None)
        win.win.deiconify()
        win.win.lift()
        win.win.update()
        win._align_footer_icon()
        win.win.update()

        cv = win._repo.canvas
        items = cv.find_all()
        check("图标已画进画布", len(items) == 1 and cv.type(items[0]) == "image",
              str([cv.type(i) for i in items]))

        # 位图图标是整张图换色，不是逐个图元改 outline
        check("换色走位图那条路", tint_bitmap(cv, items[0], "repo", 20, "#ffffff") is True)

        widget, frame = win._repo.widget, win.win
        left = widget.winfo_rootx() - frame.winfo_rootx()
        bottom = (frame.winfo_rooty() + frame.winfo_height()) - (
            widget.winfo_rooty() + widget.winfo_height())
        # 期望值从代码里的常量推出来，不要在测试里另写一份 ——
        # 调布局时只需要改常量，这里自动跟着走
        check("距左边符合 _REPO_INSET（±2px）",
              abs(left - sw._REPO_INSET) <= 2, "距左 %d，期望 %d" % (left, sw._REPO_INSET))
        check("距下边符合 _REPO_INSET_BOTTOM（±2px）",
              abs(bottom - sw._REPO_INSET_BOTTOM) <= 2,
              "距下 %d，期望 %d" % (bottom, sw._REPO_INSET_BOTTOM))
        check("图标没被窗口底边切掉",
              bottom >= 0 and left >= 0, "距左 %d 距下 %d" % (left, bottom))
        check("位置可重复调用而不漂移", _stable(win, (left, bottom)),
              "再算一次仍是 %d/%d" % (left, bottom))

        # 悬停：位图不换图元，但提示要出来
        win._repo._set_hover(True)
        root.after(600, root.quit)
        root.mainloop()
        tip = win._repo._tooltip_win
        check("悬停弹出提示窗", tip is not None)
        if tip:
            labels = [c for c in tip.winfo_children() if isinstance(c, tk.Label)]
            check("提示文字正确", bool(labels) and labels[0].cget("text") == "打开项目仓库",
                  labels[0].cget("text") if labels else "无")
        win._repo._set_hover(False)
        check("移开后提示消失", win._repo._tooltip_win is None)

        opened = []
        import webbrowser  # noqa: E402
        original = webbrowser.open
        webbrowser.open = lambda url, *a, **k: opened.append(url) or True
        try:
            win._repo._open()
        finally:
            webbrowser.open = original
        check("点击打开的是仓库地址", opened == [_REPO_URL], str(opened))

        win._close()
        root.update()
        check("关闭窗口后提示窗被收掉", win._repo._tooltip_win is None)
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)
        root.destroy()


def _stable(win, before) -> bool:
    """重复对齐不该让图标一点点漂走。"""
    for _ in range(2):
        win._align_footer_icon()
        win.win.update()
    widget, frame = win._repo.widget, win.win
    now = (widget.winfo_rootx() - frame.winfo_rootx(),
           (frame.winfo_rooty() + frame.winfo_height())
           - (widget.winfo_rooty() + widget.winfo_height()))
    return now == before


def main() -> int:
    asset_checks()
    ui_checks()
    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
