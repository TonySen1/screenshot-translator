# -*- coding: utf-8 -*-
"""对打包好的 dist/截图翻译.exe 做完整验收测试。

覆盖：启动时间、无控制台/无窗口、截图遮罩、框选、译文浮层、复制按钮、Esc、重复使用、退出。
用本地 mock 服务当翻译接口，测试期间临时改写 dist/config.json，结束恢复。
"""

import ctypes
import ctypes.wintypes
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import mss  # noqa: E402
import pyperclip  # noqa: E402
from PIL import Image  # noqa: E402

import screenshot  # noqa: E402

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(PROJECT, "dist")
EXE = os.path.join(DIST, "截图翻译.exe")
CONFIG = os.path.join(DIST, "config.json")
LOG = os.path.join(DIST, "screenshot-translator.log")

ORIGINAL = "Hello from a screenshot"
TRANSLATED = "来自截图的问候"

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


class MockHandler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        time.sleep(1.2)
        payload = {
            "id": "mock", "object": "chat.completion", "created": 0, "model": "mock-vl",
            "choices": [{"index": 0, "finish_reason": "stop", "message": {
                "role": "assistant",
                "content": json.dumps({"original_text": ORIGINAL,
                                       "translated_text": TRANSLATED},
                                      ensure_ascii=False)}}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        }
        data = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def tk_windows(pid=None):
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
        if not user32.IsWindowVisible(hwnd):
            return True
        rect = ctypes.wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        found.append({"hwnd": hwnd, "pid": owner.value, "class": buf.value,
                      "rect": (rect.left, rect.top, rect.right - rect.left,
                               rect.bottom - rect.top)})
        return True

    user32.EnumWindows(WNDENUMPROC(callback), None)
    return found


def any_visible_windows_for(pid):
    """该进程所有可见的顶层窗口（含控制台窗口，用来验证没有黑框）。"""
    found = []

    def callback(hwnd, _lparam):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, buf, 256)
            found.append(buf.value)
        return True

    user32.EnumWindows(WNDENUMPROC(callback), None)
    return found


def kill_stale():
    """清掉残留的 exe 实例和源码实例。"""
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-CimInstance Win32_Process | Where-Object { "
         "$_.Name -eq '截图翻译.exe' -or "
         "($_.Name -eq 'pythonw.exe' -and $_.CommandLine -like '*main.py*') } | "
         "ForEach-Object { $_.ProcessId }"],
        capture_output=True, text=True)
    for token in result.stdout.split():
        if token.strip().isdigit():
            subprocess.run(["taskkill", "/F", "/T", "/PID", token.strip()], capture_output=True)


def grab():
    with mss.MSS() as sct:
        shot = sct.grab(sct.monitors[0])
        return Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")


def brightness(img, box=None):
    region = img.crop(box) if box else img
    data = region.resize((80, 50)).tobytes()
    return sum(data) / len(data)


def move_mouse(x, y):
    ctypes.windll.user32.SetCursorPos(int(x), int(y))


def click(x, y):
    move_mouse(x, y)
    time.sleep(0.15)
    ctypes.windll.user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    ctypes.windll.user32.mouse_event(0x0004, 0, 0, 0, 0)


def cursor_pos():
    pt = ctypes.wintypes.POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
    return pt.x, pt.y


def drag(x0, y0, x1, y1):
    """在遮罩上拖出一个选区。

    合成鼠标有竞争：SetCursorPos 之后立刻按下时，Tk 收到的按下坐标可能还是
    旧位置，选区左上角就会偏几像素。先确认光标真的到位再按。
    """
    target = (int(x0), int(y0))
    for _ in range(10):
        move_mouse(x0, y0)
        time.sleep(0.05)
        if cursor_pos() == target:
            break
    time.sleep(0.15)
    ctypes.windll.user32.mouse_event(0x0002, 0, 0, 0, 0)
    for step in range(1, 11):
        move_mouse(x0 + (x1 - x0) * step // 10, y0 + (y1 - y0) * step // 10)
        time.sleep(0.03)
    ctypes.windll.user32.mouse_event(0x0004, 0, 0, 0, 0)


def send_hotkey(combo):
    import keyboard
    keyboard.press_and_release(combo)


def main() -> int:
    screenshot.enable_dpi_awareness()

    if not os.path.exists(EXE):
        print("找不到 exe：", EXE)
        return 1

    kill_stale()
    time.sleep(1.0)
    check("测试前无残留实例", len(tk_windows()) == 0)

    server = HTTPServer(("127.0.0.1", 0), MockHandler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()

    backup = None
    if os.path.exists(CONFIG):
        with open(CONFIG, "rb") as handle:
            backup = handle.read()
    with open(CONFIG, "w", encoding="utf-8") as handle:
        json.dump({"api_base_url": "http://127.0.0.1:%d/v1" % port,
                   "api_key": "sk-mock", "model": "mock-vl",
                   "hotkey": "ctrl+alt+d", "exit_hotkey": "ctrl+alt+shift+q",
                   "target_language": "中文", "mask_opacity": 0.35,
                   "overlay_opacity": 0.88, "overlay_bg": "#1e1e1e",
                   "overlay_fg": "#ffffff", "font_size": 12,
                   "request_timeout": 30},
                  handle, ensure_ascii=False, indent=2)

    if os.path.exists(LOG):
        try:
            os.remove(LOG)
        except OSError:
            pass

    start_cursor = ctypes.wintypes.POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(start_cursor))
    try:
        saved_clipboard = pyperclip.paste()
    except Exception:
        saved_clipboard = ""

    proc = None
    try:
        # ---- 启动 ----
        baseline = grab()
        base_bright = brightness(baseline)

        t0 = time.time()
        proc = subprocess.Popen([EXE], cwd=DIST)
        time.sleep(2.0)
        check("exe 进程存活", proc.poll() is None)
        check("启动后桌面没有可见窗口", len(tk_windows()) == 0)

        # 反复按热键直到有反应，顺便量冷启动到可用的耗时
        deadline = time.time() + 25
        ready = False
        while time.time() < deadline:
            send_hotkey("ctrl+alt+d")
            for _ in range(12):
                time.sleep(0.15)
                if tk_windows():
                    ready = True
                    break
            if ready:
                break
        startup = time.time() - t0
        check("冷启动后热键可用", ready, "耗时 %.1f 秒" % startup)

        # 遮罩有约 0.13 秒的淡入动画，等它走完再测亮度，否则量到的是中间态
        time.sleep(0.5)
        mask_bright = brightness(grab())
        check("按快捷键出现半透明遮罩", mask_bright < base_bright * 0.75,
              "%.1f -> %.1f" % (base_bright, mask_bright))

        mask = tk_windows()[0]
        check("遮罩覆盖整个虚拟屏幕",
              mask["rect"][2] >= 4000 and mask["rect"][3] >= 2560, str(mask["rect"]))

        # onefile 的 bootloader 父进程 + 应用子进程都不能有控制台黑框
        app_pid = mask["pid"]
        app_visible = any_visible_windows_for(app_pid)
        console = [c for c in app_visible if "Console" in c or c == "#32770"]
        check("应用进程没有控制台窗口 / 弹窗", not console, str(app_visible))
        check("bootloader 父进程也没有可见窗口",
              len(any_visible_windows_for(proc.pid)) == 0,
              str(any_visible_windows_for(proc.pid)))
        check("应用进程确实是 exe 拉起的子进程", app_pid != proc.pid,
              "launcher=%d app=%d" % (proc.pid, app_pid))

        # ---- 框选 ----
        x0, y0, x1, y1 = 600, 600, 1500, 850
        move_mouse(x0, y0)
        time.sleep(0.25)
        ctypes.windll.user32.mouse_event(0x0002, 0, 0, 0, 0)
        for step in range(1, 11):
            move_mouse(x0 + (x1 - x0) * step // 10, y0 + (y1 - y0) * step // 10)
            time.sleep(0.03)
        ctypes.windll.user32.mouse_event(0x0004, 0, 0, 0, 0)

        time.sleep(0.6)
        # v1.2.0 起译文框和按钮框是两个独立窗口，所以是 2 个
        check("松开后遮罩消失、浮层出现",
              len(tk_windows()) == 2 and brightness(grab()) > base_bright * 0.9,
              str(len(tk_windows())))

        time.sleep(2.0)
        # 取面积最大的那个 = 译文框（按钮框只有 88x30）
        overlay = max(tk_windows(), key=lambda w: w["rect"][2] * w["rect"][3])
        rect = overlay["rect"]
        print("浮层:", rect, flush=True)
        # 合成鼠标在系统事件队列里会有几像素到几十像素的滞后，
        # 真实鼠标不受影响。坐标换算本身有 test_mask_rect.py 做精确验证。
        check("浮层贴在框选位置附近",
              abs(rect[0] - x0) <= 30 and abs(rect[1] - y0) <= 30,
              "选区(%d,%d) 浮层(%d,%d)" % (x0, y0, rect[0], rect[1]))

        shot = grab()
        shot.crop((max(0, rect[0] - 15), rect[1] + 588 - 15,
                   rect[0] + rect[2] + 15, rect[1] + rect[3] + 588 + 15)).save(
            os.path.join(PROJECT, "dev", "shot_exe_result.png"))

        # ---- 右下角三个图标按钮：提取 / 复制 / 关闭 ----
        # v1.2.0 起按钮住在独立的按钮浮层里，贴在译文框右下角外侧，
        # 所以坐标要以那个浮层为基准。常量见 overlay.py：
        # 图标画布 20（图标 14 + 两侧 padding 3），按钮间距 8，浮层内边距 左右 6 / 上下 5。
        btn_rect = next(w["rect"] for w in tk_windows() if w["rect"] != rect)
        print("按钮浮层:", btn_rect, flush=True)
        side, gap, pad_x, pad_y = 20, 8, 6, 5
        extract_x = btn_rect[0] + pad_x + side // 2
        copy_x = extract_x + (side + gap)
        close_x = copy_x + (side + gap)
        btn_y = btn_rect[1] + pad_y + side // 2

        pyperclip.copy("__before__")
        click(extract_x, btn_y)
        time.sleep(0.7)
        check("点[提取原文]图标后剪贴板得到原文", pyperclip.paste() == ORIGINAL,
              repr(pyperclip.paste()))

        pyperclip.copy("__before__")
        click(copy_x, btn_y)
        time.sleep(0.7)
        check("点[复制译文]图标后剪贴板得到译文", pyperclip.paste() == TRANSLATED,
              repr(pyperclip.paste()))

        # ---- 点 X 关闭（用户要求的新关闭方式）----
        click(close_x, btn_y)
        time.sleep(1.0)
        check("点右下角 X 关闭浮层", len(tk_windows()) == 0, str(tk_windows()))
        check("X 关闭后进程仍在后台", proc.poll() is None)

        # ---- 再截一次，这次用 Esc 关 ----
        # 合成按键偶尔会丢，多按几次再判定
        appeared = False
        for _ in range(3):
            send_hotkey("ctrl+alt+d")
            for _ in range(15):
                time.sleep(0.15)
                if tk_windows():
                    appeared = True
                    break
            if appeared:
                break
        check("可以再次截图", appeared and len(tk_windows()) == 1, str(tk_windows()))
        drag(600, 600, 1500, 850)
        time.sleep(2.5)
        send_hotkey("esc")
        time.sleep(1.2)
        check("Esc 关闭浮层", len(tk_windows()) == 0, str(tk_windows()))
        check("关闭后进程仍在后台", proc.poll() is None)

        # ---- 托盘图标 ----
        if os.path.exists(LOG):
            with open(LOG, encoding="utf-8", errors="replace") as fh:
                log_text = fh.read()
            check("托盘图标已启动", "托盘图标已启动" in log_text)
        else:
            check("托盘图标已启动", False, "没有日志文件")

        # ---- 退出 ----
        send_hotkey("ctrl+alt+shift+q")
        time.sleep(2.5)
        check("退出热键结束进程", proc.poll() is not None, "code=%s" % proc.poll())
    finally:
        if proc is not None and proc.poll() is None:
            proc.kill()
        kill_stale()
        move_mouse(start_cursor.x, start_cursor.y)
        try:
            pyperclip.copy(saved_clipboard)
        except Exception:
            pass
        if backup is not None:
            with open(CONFIG, "wb") as handle:
                handle.write(backup)
            print("dist/config.json 已恢复", flush=True)

    if os.path.exists(LOG):
        print("--- exe 日志 ---", flush=True)
        with open(LOG, encoding="utf-8", errors="replace") as handle:
            for line in handle.readlines()[-12:]:
                print("   " + line.rstrip(), flush=True)

    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
