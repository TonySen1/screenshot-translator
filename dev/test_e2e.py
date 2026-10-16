# -*- coding: utf-8 -*-
"""端到端测试：真的启动 main.py，用合成按键/鼠标走完整个流程。

会短暂移动真实鼠标（测试结束恢复到原位）。

注意：venv 里的 pythonw.exe 是个 launcher，它会再起一个子进程跑真正的解释器，
所以 Popen 拿到的 pid 和 Tk 窗口所属的 pid 不是同一个。这里一律从 Tk 窗口反查真实 pid。
"""

import ctypes
import ctypes.wintypes
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mss  # noqa: E402
from PIL import Image  # noqa: E402

import screenshot  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PYTHONW = os.path.join(PROJECT, ".venv", "Scripts", "pythonw.exe")
LOG = os.path.join(PROJECT, "screenshot-translator.log")

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


def tk_windows(pid=None):
    """列出可见的 Tk 顶层窗口。"""
    found = []

    def callback(hwnd, _lparam):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if "Tk" not in buf.value:
            return True
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if pid is not None and owner.value != pid:
            return True
        rect = ctypes.wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        found.append({
            "hwnd": hwnd, "pid": owner.value, "visible": bool(user32.IsWindowVisible(hwnd)),
            "class": buf.value,
            "rect": (rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top),
        })
        return True

    user32.EnumWindows(WNDENUMPROC(callback), None)
    return [w for w in found if w["visible"]]


def app_pid():
    """当前正在显示的 Tk 窗口所属的 pid。"""
    windows = tk_windows()
    return windows[0]["pid"] if windows else None


def kill_stale_instances():
    """清掉之前测试残留的 main.py 进程，避免单实例互斥体把新实例挡掉。"""
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process -Filter \"Name='pythonw.exe'\" | "
         "Where-Object { $_.CommandLine -like '*main.py*' } | "
         "ForEach-Object { $_.ProcessId }"],
        capture_output=True, text=True)
    killed = []
    for token in result.stdout.split():
        if token.strip().isdigit():
            subprocess.run(["taskkill", "/F", "/PID", token.strip()], capture_output=True)
            killed.append(token.strip())
    return killed


def grab():
    with mss.MSS() as sct:
        shot = sct.grab(sct.monitors[0])
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def mean_brightness(img, box=None):
    region = img.crop(box) if box else img
    data = region.resize((80, 50)).tobytes()
    return sum(data) / len(data)


def move_mouse(x, y):
    ctypes.windll.user32.SetCursorPos(int(x), int(y))


def mouse_event(flag):
    ctypes.windll.user32.mouse_event(flag, 0, 0, 0, 0)


MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004


def send_hotkey(combo):
    import keyboard
    keyboard.press_and_release(combo)


def main() -> int:
    screenshot.enable_dpi_awareness()

    killed = kill_stale_instances()
    if killed:
        print("清理残留实例:", killed, flush=True)
        time.sleep(1.2)
    check("测试开始前没有残留窗口", len(tk_windows()) == 0, str(tk_windows()))

    if os.path.exists(LOG):
        try:
            os.remove(LOG)
        except OSError:
            pass

    start_cursor = ctypes.wintypes.POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(start_cursor))

    launcher = subprocess.Popen([PYTHONW, os.path.join(PROJECT, "main.py")], cwd=PROJECT)
    time.sleep(4)
    check("进程存活", launcher.poll() is None, "launcher pid=%d" % launcher.pid)
    check("启动后桌面没有可见窗口", len(tk_windows()) == 0, str(tk_windows()))

    baseline = grab()
    base_bright = mean_brightness(baseline)

    # ---- 触发截图快捷键 ----
    send_hotkey("ctrl+alt+d")
    time.sleep(1.5)
    masked_bright = mean_brightness(grab())
    check("按快捷键后桌面出现半透明遮罩", masked_bright < base_bright * 0.75,
          "%.1f -> %.1f" % (base_bright, masked_bright))

    pid = app_pid()
    check("能定位到实际进程", pid is not None, "pid=%s" % pid)
    if pid is None:
        launcher.kill()
        return 1

    mask = tk_windows(pid)[0]
    check("遮罩是一个覆盖整个虚拟屏幕的窗口",
          mask["rect"][2] >= 4000 and mask["rect"][3] >= 2560, str(mask["rect"]))

    # ---- 模拟拖拽框选 ----
    x0, y0, x1, y1 = 500, 500, 1400, 750
    move_mouse(x0, y0)
    time.sleep(0.25)
    mouse_event(MOUSEEVENTF_LEFTDOWN)
    for step in range(1, 11):
        move_mouse(x0 + (x1 - x0) * step // 10, y0 + (y1 - y0) * step // 10)
        time.sleep(0.03)
    mouse_event(MOUSEEVENTF_LEFTUP)
    time.sleep(1.3)

    after_release = grab()
    released_bright = mean_brightness(after_release)
    check("松开鼠标后遮罩消失", released_bright > base_bright * 0.9,
          "基线 %.1f -> %.1f" % (base_bright, released_bright))

    overlay_windows = tk_windows(pid)
    # v1.2.0 起译文框和按钮框是两个独立窗口
    check("只剩下结果浮层两个窗口", len(overlay_windows) == 2, str(overlay_windows))
    if overlay_windows:
        # 取面积最大的那个 = 译文框（按钮框只有 88x30）
        rect = max(overlay_windows,
                   key=lambda w: w["rect"][2] * w["rect"][3])["rect"]
        print("浮层位置尺寸:", rect, flush=True)
        check("浮层出现在框选位置",
              abs(rect[0] - x0) <= 4 and abs(rect[1] - y0) <= 4,
              "选区(%d,%d) 浮层(%d,%d)" % (x0, y0, rect[0], rect[1]))
        check("浮层宽度贴合文字（不超过上限、不窄于下限）",
              180 <= rect[2] <= 1000, "浮层宽 %d" % rect[2])
        # 裁一块浮层附近的画面存档（图片坐标 y 要加 -area.top = 588）
        after_release.crop((max(0, rect[0] - 20), rect[1] + 588 - 20,
                            rect[0] + rect[2] + 20, rect[1] + rect[3] + 588 + 20)) \
            .save(os.path.join(PROJECT, "dev", "shot_e2e_overlay.png"))

    # ---- Esc 关闭浮层 ----
    send_hotkey("esc")
    time.sleep(1.3)
    left = tk_windows(pid)
    check("Esc 关闭浮层（窗口已销毁）", len(left) == 0, str(left))
    check("关闭后进程仍在后台运行", launcher.poll() is None)

    # ---- 再次截图，验证可以反复用 ----
    send_hotkey("ctrl+alt+d")
    time.sleep(1.5)
    again_bright = mean_brightness(grab())
    check("再次按快捷键仍能进入截图模式", again_bright < base_bright * 0.75,
          "%.1f -> %.1f" % (base_bright, again_bright))
    send_hotkey("esc")
    time.sleep(1.0)
    check("取消截图后回到后台", len(tk_windows(pid)) == 0, str(tk_windows(pid)))

    # ---- 退出热键 ----
    send_hotkey("ctrl+alt+shift+q")
    time.sleep(2.0)
    check("退出热键后进程结束", launcher.poll() is not None,
          "returncode=%s" % launcher.poll())

    if launcher.poll() is None:
        launcher.kill()

    move_mouse(start_cursor.x, start_cursor.y)
    print("鼠标已恢复", flush=True)

    if os.path.exists(LOG):
        print("--- 日志尾部 ---", flush=True)
        with open(LOG, encoding="utf-8", errors="replace") as handle:
            for line in handle.readlines()[-14:]:
                print("   " + line.rstrip(), flush=True)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
