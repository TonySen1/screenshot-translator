# -*- coding: utf-8 -*-
"""框选坐标换算的确定性测试。

不依赖真实鼠标：直接把 press/drag/release 事件喂给遮罩，
验证 canvas 坐标 -> 虚拟屏幕全局坐标的换算是精确的。
（端到端测试里的合成鼠标会有几像素滞后，那是测试环境的限制。）
"""

import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config as config_module  # noqa: E402
import screenshot  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


class FakeEvent:
    def __init__(self, x, y):
        self.x = x
        self.y = y


def main() -> int:
    screenshot.enable_dpi_awareness()
    cfg = config_module.load_config(os.path.join(HERE, "config.json"))

    root = tk.Tk()
    root.withdraw()
    image, area, monitors = screenshot.grab_virtual_screen()
    print("虚拟屏幕: %s @ (%d,%d)" % (area, area.left, area.top), flush=True)

    cases = []
    # 虚拟屏偏移 (0, -588)：canvas (0,0) 对应全局 (0,-588)
    cases.append(("主屏区域", (600, 1488), (1500, 1838), (600, 900, 900, 350)))
    cases.append(("左上角起点", (0, 588), (100, 688), (0, 0, 100, 100)))
    cases.append(("反向拖动", (1500, 1838), (600, 1488), (600, 900, 900, 350)))
    cases.append(("跨到虚拟屏顶部", (10, 0), (110, 50),
                  (10, area.top, 100, 50)))
    cases.append(("右屏区域", (3000, 1000), (3400, 1200),
                  (3000, 1000 + area.top, 400, 200)))

    for name, start, end, expect in cases:
        mask = screenshot.MaskWindow(root, image, area, cfg)
        mask._fade_out_enabled = False
        mask.win.deiconify()
        root.update()

        mask._on_press(FakeEvent(*start))
        mask._on_drag(FakeEvent(*end))
        mask._do_refresh()
        mask._on_release(FakeEvent(*end))
        root.update()

        got = mask._result
        actual = (got.left, got.top, got.width, got.height) if got else None
        check("%s 换算精确" % name, actual == expect,
              "%s（期望 %s）" % (actual, expect))
        mask._destroy()
        root.update()

    # 太小的选区应当被拒绝
    mask = screenshot.MaskWindow(root, image, area, cfg)
    mask._fade_out_enabled = False
    mask.win.deiconify()
    root.update()
    mask._on_press(FakeEvent(800, 1400))
    mask._on_drag(FakeEvent(803, 1403))
    mask._on_release(FakeEvent(803, 1403))
    root.update()
    check("3px 的误触被拒绝", mask._result is None, str(mask._result))
    mask._destroy()

    root.destroy()
    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
