# -*- coding: utf-8 -*-
"""验证遮罩的淡入淡出：连续抓屏看亮度是不是平滑变化的，而不是一步到底。

测量线程只碰 mss/PIL；所有 Tk 操作都在主线程用 after 调度。
"""

import os
import sys
import threading
import time
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mss  # noqa: E402

import config as config_module  # noqa: E402
import screenshot  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def brightness():
    with mss.MSS() as sct:
        shot = sct.grab({"left": 0, "top": 0, "width": 400, "height": 300})
        data = shot.bgra
    return sum(data[0::4]) / (len(data) // 4)


class Sampler:
    def __init__(self):
        self.samples = []
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self.samples.clear()
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            self.samples.append(brightness())

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=1.0)


def main() -> int:
    screenshot.enable_dpi_awareness()
    cfg = config_module.load_config(os.path.join(HERE, "config.json"))
    cfg.mask_opacity = 0.35

    root = tk.Tk()
    root.withdraw()
    image, area, monitors = screenshot.grab_virtual_screen()

    mask = screenshot.MaskWindow(root, image, area, cfg)
    baseline = brightness()
    print("基线亮度: %.1f，期望最终约 %.1f" % (baseline, baseline * cfg.mask_opacity),
          flush=True)

    fade_in = Sampler()
    fade_out = Sampler()
    state = {}

    def phase_in():
        fade_in.start()
        root.after(40, lambda: (mask.win.deiconify(), mask._fade_in()))
        root.after(1200, phase_out)

    def phase_out():
        fade_in.stop()
        state["in"] = list(fade_in.samples)
        state["alpha_after_in"] = float(mask.win.attributes("-alpha"))
        fade_out.start()
        root.after(40, mask._fade_out)
        root.after(900, root.quit)

    root.after(60, phase_in)
    root.after(4000, root.quit)
    root.mainloop()
    fade_in.stop()
    fade_out.stop()
    time.sleep(0.15)
    state["out"] = list(fade_out.samples)

    # ---------- 淡入 ----------
    samples = state.get("in", [])
    print("淡入 %d 个采样: %s" % (len(samples),
                                 " ".join("%.0f" % v for v in samples[:26])), flush=True)
    check("淡入抓到了足够采样", len(samples) >= 5, "%d 个" % len(samples))
    if len(samples) >= 5:
        final = samples[-1]
        expected = baseline * cfg.mask_opacity
        check("最终亮度符合 mask_opacity", abs(final - expected) < baseline * 0.12,
              "%.1f（期望约 %.1f）" % (final, expected))
        mids = [v for v in samples
                if final + (baseline - final) * 0.15 < v < baseline - (baseline - final) * 0.15]
        check("淡入有中间亮度（不是一步到底）", len(mids) >= 2,
              "中间态 %d 个: %s" % (len(mids), " ".join("%.0f" % v for v in mids[:8])))
        drops = sum(1 for a, b in zip(samples, samples[1:]) if b <= a + 1.0)
        check("淡入亮度单调下降", drops >= len(samples) - 2,
              "%d/%d 递减" % (drops, len(samples) - 1))
    check("淡入结束后完全不透明", abs(state.get("alpha_after_in", 0) - 1.0) < 0.01,
          str(state.get("alpha_after_in")))

    # ---------- 淡出 ----------
    out = state.get("out", [])
    print("淡出 %d 个采样: %s" % (len(out), " ".join("%.0f" % v for v in out[:26])), flush=True)
    if len(out) >= 3:
        # 淡出是亮度往回升（暗 -> 亮），中间态要按最终/基线两端来卡
        bright_mids = [v for v in out
                       if out[0] * 1.15 < v < baseline * 0.92]
        check("淡出有中间亮度", len(bright_mids) >= 1, "中间态 %d 个" % len(bright_mids))
        check("淡出结束时屏幕已恢复", out[-1] > baseline * 0.85,
              "%.1f vs 基线 %.1f" % (out[-1], baseline))
    else:
        check("淡出抓到了采样", False, "%d 个" % len(out))

    alive = False
    try:
        alive = bool(mask.win.winfo_exists())
    except tk.TclError:
        alive = False
    check("淡出后窗口已销毁", not alive)

    try:
        root.destroy()
    except tk.TclError:
        pass

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
