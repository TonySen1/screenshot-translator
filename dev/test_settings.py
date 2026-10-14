# -*- coding: utf-8 -*-
"""设置窗口功能测试：滑块拖动、输入框、保存回写 config.json。

会临时改写 config.json，测完恢复。
"""

import io
import json
import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config as config_module  # noqa: E402
import screenshot  # noqa: E402
from settings_window import SettingsWindow, install_styles  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(HERE, "config.json")

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def main() -> int:
    screenshot.enable_dpi_awareness()

    backup = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as fh:
            backup = fh.read()

    saved_calls = []

    try:
        cfg = config_module.load_config(CONFIG)
        root = tk.Tk()
        root.withdraw()
        install_styles(root)

        win = SettingsWindow(root, cfg, on_save=lambda c: saved_calls.append(c))
        win.win.deiconify()
        win.win.update()

        # --- 1. 滑块初值来自配置 ---
        check("遮罩滑块初值来自配置", abs(win._mask.value - cfg.mask_opacity) < 0.001,
              "%.3f" % win._mask.value)
        check("浮层滑块初值来自配置", abs(win._overlay.value - cfg.overlay_opacity) < 0.001)
        check("字号滑块初值来自配置", int(win._fontsize.value) == cfg.font_size)

        # --- 2. 拖动滑块 ---
        before = win._mask.value
        win._mask.event_generate("<Button-1>", x=40, y=13)
        win._mask.event_generate("<B1-Motion>", x=90, y=13)
        win._mask.event_generate("<ButtonRelease-1>", x=90, y=13)
        win.win.update()
        check("拖动滑块改变了数值", win._mask.value != before,
              "%.3f -> %.3f" % (before, win._mask.value))
        check("滑块数值实时显示在右侧",
              win._mask._value_label.cget("text") == "%d%%" % round(win._mask.value * 100),
              win._mask._value_label.cget("text"))

        # --- 3. 滑块值被夹在范围内 ---
        win._mask.set_value(5.0)
        check("滑块数值被限制在上限内", win._mask.value <= 1.0, "%.2f" % win._mask.value)
        win._mask.set_value(-3.0)
        check("滑块数值被限制在下限内", win._mask.value >= 0.05, "%.2f" % win._mask.value)

        # --- 4. 输入框 ---
        win._api_key.delete(0, "end")
        win._api_key.insert(0, "sk-test-12345")
        win._model.delete(0, "end")
        win._model.insert(0, "my-model-v2")
        win._base_url.delete(0, "end")
        win._base_url.insert(0, "https://example.com/v1")

        win._mask.set_value(0.42)
        win._fontsize.set_value(17)

        # --- 5. 点保存 ---
        win._save()
        root.update()

        check("保存时回调被触发", len(saved_calls) == 1)
        check("保存后窗口关闭", win._closed is True)

        new_cfg = saved_calls[0] if saved_calls else cfg
        check("遮罩透明度已更新", abs(new_cfg.mask_opacity - 0.42) < 0.001,
              "%.2f" % new_cfg.mask_opacity)
        check("字号已更新", new_cfg.font_size == 17, str(new_cfg.font_size))
        check("API Key 已更新", new_cfg.api_key == "sk-test-12345", new_cfg.api_key)
        check("模型已更新", new_cfg.model == "my-model-v2", new_cfg.model)
        check("接口地址已更新", new_cfg.api_base_url == "https://example.com/v1",
              new_cfg.api_base_url)

        # --- 6. config.json 真的写盘了 ---
        # （这里手动调 save_config，模拟主程序的 on_save）
        config_module.save_config(new_cfg, CONFIG)
        with io.open(CONFIG, encoding="utf-8") as fh:
            on_disk = json.load(fh)
        check("config.json 已写盘", on_disk["api_key"] == "sk-test-12345",
              on_disk.get("api_key"))
        check("写盘时保留了 _说明 字段", "_说明" in on_disk)
        check("写盘时保留了其他字段", on_disk["hotkey"] == cfg.hotkey, on_disk.get("hotkey"))

        # --- 7. 重新读回来 ---
        reloaded = config_module.load_config(CONFIG)
        check("重新加载后数值一致",
              abs(reloaded.mask_opacity - 0.42) < 0.001 and reloaded.font_size == 17,
              "mask=%.2f font=%d" % (reloaded.mask_opacity, reloaded.font_size))

        # --- 8. 空值不会把配置清没 ---
        win2 = SettingsWindow(root, reloaded)
        win2.win.deiconify()
        win2.win.update()
        win2._base_url.delete(0, "end")
        win2._model.delete(0, "end")
        win2._api_key.delete(0, "end")
        captured = {}
        win2._on_save = lambda c: captured.update(cfg=c)
        win2._save()
        root.update()
        check("接口地址清空后保留原值", captured["cfg"].api_base_url == "https://example.com/v1",
              captured["cfg"].api_base_url)
        check("API Key 允许清空", captured["cfg"].api_key == "", repr(captured["cfg"].api_key))

        root.destroy()
    finally:
        if backup is not None:
            with open(CONFIG, "wb") as fh:
                fh.write(backup)
            print("config.json 已恢复", flush=True)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
