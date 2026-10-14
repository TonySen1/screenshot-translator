# -*- coding: utf-8 -*-
"""托盘图标测试：图标能创建、菜单项齐全、回调能通到事件队列。"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from icons import tray_image  # noqa: E402
from tray import Tray  # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def main() -> int:
    img = tray_image(64)
    check("托盘图标是 RGBA 且尺寸正确", img.size == (64, 64) and img.mode == "RGBA",
          "%s %s" % (img.size, img.mode))
    check("托盘图标有非透明像素",
          any(p[3] > 0 for p in list(img.getdata())))  # noqa

    events = []
    tray = Tray(
        on_settings=lambda: events.append("settings"),
        on_capture=lambda: events.append("capture"),
        on_quit=lambda: events.append("quit"),
        hotkey_hint="ctrl+alt+d",
    )

    started = tray.start()
    check("托盘启动成功", started)
    if not started:
        return 1

    time.sleep(1.5)
    check("图标对象存在", tray._icon is not None)
    check("图标已显示在状态栏", bool(getattr(tray._icon, "visible", False)),
          str(getattr(tray._icon, "visible", None)))
    check("悬停提示包含快捷键", "ctrl+alt+d" in (tray._icon.title or ""),
          repr(tray._icon.title))

    try:
        items = [item for item in tray._icon.menu.items]
    except Exception as exc:
        items = []
        print("读取菜单失败:", exc)
    texts = [getattr(i, "text", "?") for i in items]
    check("菜单有 4 项", len(items) == 4, str(texts))
    check("菜单文字正确",
          texts[:2] == ["打开设置", "截图翻译"] and texts[-1] == "退出", str(texts))
    check("双击默认项是打开设置",
          any(getattr(i, "default", False) and i.text == "打开设置" for i in items))

    # 触发菜单回调
    tray._settings_clicked()
    tray._capture_clicked()
    tray._quit_clicked()
    check("三个菜单回调都能触发", events == ["settings", "capture", "quit"], str(events))

    # 改快捷键后提示会更新
    tray.update_title("ctrl+alt+j")
    check("改快捷键后提示同步更新", "ctrl+alt+j" in (tray._icon.title or ""),
          repr(tray._icon.title))

    tray.stop()
    time.sleep(0.5)
    check("停止后托盘对象已释放", tray._icon is None)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
