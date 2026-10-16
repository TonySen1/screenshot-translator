# -*- coding: utf-8 -*-
"""边界情况测试：自定义快捷键、遮罩透明度配置、误触框选、网络错误。

程序只在启动时读配置，所以分两轮跑：每轮写一份临时 config 再启动。
"""

import ctypes
import ctypes.wintypes
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mss  # noqa: E402
from PIL import Image  # noqa: E402

import config as config_module  # noqa: E402
import screenshot  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHONW = os.path.join(PROJECT, ".venv", "Scripts", "pythonw.exe")
CONFIG = os.path.join(PROJECT, "config.json")

RESULTS = []
user32 = ctypes.WinDLL("user32", use_last_error=True)
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
user32.EnumWindows.argtypes = [WNDENUMPROC, ctypes.c_void_p]
user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
user32.IsWindowVisible.argtypes = [ctypes.c_void_p]
user32.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowRect.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.wintypes.RECT)]


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def tk_windows():
    found = []

    def cb(hwnd, _l):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if "Tk" not in buf.value or not user32.IsWindowVisible(hwnd):
            return True
        rect = ctypes.wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        found.append((rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), None)
    return found


def kill_stale():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
        "Get-CimInstance Win32_Process | Where-Object { $_.Name -eq '截图翻译.exe' -or "
        "($_.Name -eq 'pythonw.exe' -and $_.CommandLine -like '*main.py*') } | "
        "ForEach-Object { $_.ProcessId }"], capture_output=True, text=True)
    for t in r.stdout.split():
        if t.strip().isdigit():
            subprocess.run(["taskkill", "/F", "/T", "/PID", t.strip()], capture_output=True)
    time.sleep(1.0)


def grab():
    with mss.MSS() as sct:
        shot = sct.grab(sct.monitors[0])
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def brightness(img):
    data = img.resize((80, 50)).tobytes()
    return sum(data) / len(data)


def send(combo):
    """合成一次按键。keyboard 的 press_and_release 偶尔会丢事件或者让修饰键粘住，
    所以按完主动把修饰键释放一遍。"""
    import keyboard
    keyboard.press_and_release(combo)
    time.sleep(0.08)
    for key in ("ctrl", "alt", "shift", "windows"):
        try:
            keyboard.release(key)
        except Exception:
            pass


def send_until(combo, predicate, tries=3, wait=1.5):
    """按热键直到条件成立。合成按键不可靠，多试几次再判定失败。"""
    for _ in range(tries):
        send(combo)
        deadline = time.time() + wait
        while time.time() < deadline:
            if predicate():
                return True
            time.sleep(0.15)
    return False


def move(x, y):
    ctypes.windll.user32.SetCursorPos(int(x), int(y))


def write_config(**overrides):
    data = {"api_base_url": "http://127.0.0.1:1/v1", "api_key": "sk-x",
            "model": "mock", "hotkey": "ctrl+alt+d", "exit_hotkey": "ctrl+alt+shift+q",
            "target_language": "中文", "mask_opacity": 0.35, "overlay_opacity": 0.88,
            "overlay_bg": "#1e1e1e", "overlay_fg": "#ffffff",
            "font_size": 12, "request_timeout": 5}
    data.update(overrides)
    with open(CONFIG, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)


def launch():
    p = subprocess.Popen([PYTHONW, os.path.join(PROJECT, "main.py")], cwd=PROJECT)
    time.sleep(3.5)
    return p


def drag(x0, y0, x1, y1):
    move(x0, y0)
    time.sleep(0.35)
    move(x0, y0)   # 再发一次，确保按下时鼠标坐标已经生效
    time.sleep(0.12)
    ctypes.windll.user32.mouse_event(0x0002, 0, 0, 0, 0)
    for i in range(1, 9):
        move(x0 + (x1 - x0) * i // 8, y0 + (y1 - y0) * i // 8)
        time.sleep(0.03)
    ctypes.windll.user32.mouse_event(0x0004, 0, 0, 0, 0)


def main() -> int:
    screenshot.enable_dpi_awareness()
    kill_stale()
    backup = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as fh:
            backup = fh.read()

    try:
        # ================= 第一轮：自定义快捷键 + 遮罩透明度 =================
        print("--- 第一轮：自定义快捷键 ctrl+alt+j，mask_opacity=0.15 ---", flush=True)
        write_config(hotkey="ctrl+alt+j", mask_opacity=0.15)
        proc = launch()
        baseline = brightness(grab())

        send("ctrl+alt+d")           # 旧快捷键，应当没反应
        time.sleep(1.2)
        check("旧快捷键 ctrl+alt+d 已失效", len(tk_windows()) == 0, str(tk_windows()))

        ok = send_until("ctrl+alt+j", lambda: len(tk_windows()) == 1)
        check("新快捷键 ctrl+alt+j 生效", ok, str(tk_windows()))

        # 窗口映射出来 ≠ 画面已经合成到屏幕上，等一拍再测亮度
        time.sleep(1.0)
        masked = brightness(grab())
        ratio = masked / baseline if baseline else 0
        check("mask_opacity=0.15 生效（亮度比≈0.15）", 0.08 <= ratio <= 0.24,
              "%.1f -> %.1f，比值 %.2f" % (baseline, masked, ratio))

        # 误触：只拖 3 像素
        time.sleep(0.5)
        drag(800, 800, 803, 803)
        time.sleep(1.2)
        check("框选太小视为取消（无浮层）", len(tk_windows()) == 0, str(tk_windows()))
        check("误触后进程仍在运行", proc.poll() is None)

        # 截图过程中按 Esc 取消
        send("ctrl+alt+j")
        time.sleep(1.2)
        check("能再次进入截图", len(tk_windows()) == 1)
        send("esc")
        time.sleep(1.0)
        check("截图过程中 Esc 可取消", len(tk_windows()) == 0)

        send("ctrl+alt+shift+q")
        time.sleep(1.8)
        if proc.poll() is None:
            proc.kill()
        kill_stale()

        # ================= 第二轮：网络错误 + 字号配置 =================
        print("--- 第二轮：接口指到不存在的端口，font_size=20 ---", flush=True)
        write_config(hotkey="ctrl+alt+d", api_base_url="http://127.0.0.1:1/v1",
                     font_size=20, request_timeout=5)
        proc = launch()

        check("遮罩出现", send_until("ctrl+alt+d", lambda: len(tk_windows()) == 1),
              str(tk_windows()))

        drag(600, 600, 1400, 800)
        time.sleep(1.5)
        wins = tk_windows()
        # v1.2.0 起译文框和按钮框是两个独立窗口，所以是 2 个
        check("框选后浮层出现", len(wins) == 2, str(wins))

        # 等网络错误返回
        time.sleep(4.0)
        check("网络错误后程序没崩溃", proc.poll() is None)
        check("错误时浮层仍在显示", len(tk_windows()) == 2, str(tk_windows()))

        if wins:
            shot = grab()
            # 取面积最大的那个 = 译文框（按钮框只有 88x30）
            x, y, w, h = max(wins, key=lambda r: r[2] * r[3])
            shot.crop((max(0, x - 10), y + 588 - 10, x + w + 10, y + h + 588 + 10)).save(
                os.path.join(PROJECT, "dev", "shot_edge_error.png"))
            print("浮层尺寸（font_size=20）:", (w, h), flush=True)

        send("esc")
        time.sleep(0.8)
        send("ctrl+alt+shift+q")
        time.sleep(1.8)
        if proc.poll() is None:
            proc.kill()
        kill_stale()
    finally:
        kill_stale()
        if backup is not None:
            with open(CONFIG, "wb") as fh:
                fh.write(backup)
            print("config.json 已恢复", flush=True)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
