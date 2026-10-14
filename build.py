# -*- coding: utf-8 -*-
"""把项目打包成单文件 exe。

双击 build.bat 会调用本脚本；也可以直接跑：
    .venv\\Scripts\\python.exe build.py
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP_NAME = "截图翻译"
ICON = os.path.join(HERE, "assets", "icon.ico")
DIST = os.path.join(HERE, "dist")
BUILD = os.path.join(HERE, "build")

# 用不到的重量级库，显式排掉，exe 能小一圈
EXCLUDES = [
    "numpy", "scipy", "pandas", "matplotlib", "pytest", "setuptools",
    "pip", "wheel", "tkinter.test", "test", "unittest", "pydoc_data",
    "sqlite3", "PIL.ImageQt", "PIL.ImageShow",
]


def check_python_version() -> None:
    """PyInstaller 的字节码分析器在 Python 3.10.0 上会崩（dis 模块的老 bug）。

    这里提前拦一下，给一句人话提示，而不是丢一堆 traceback 给用户。
    """
    if sys.version_info < (3, 10):
        raise SystemExit("需要 Python 3.10 或更高版本，当前是 %s" % sys.version.split()[0])
    if sys.version_info[:3] < (3, 10, 2):
        raise SystemExit(
            "当前 Python 是 %s，这个早期版本会让 PyInstaller 打包失败。\n"
            "请升级到 Python 3.10.11 或更高版本（推荐 3.11+），再重新运行 build.bat。\n"
            "下载：https://www.python.org/downloads/" % sys.version.split()[0])


def ensure_pyinstaller() -> None:
    try:
        import PyInstaller  # noqa: F401
        return
    except ImportError:
        pass
    print("未安装 PyInstaller，正在安装依赖 ...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "-r",
                           os.path.join(HERE, "requirements.txt")])


def main() -> int:
    os.chdir(HERE)
    check_python_version()
    ensure_pyinstaller()

    if not os.path.exists(ICON):
        print("图标不存在，先生成 ...")
        subprocess.check_call([sys.executable, os.path.join("assets", "make_icon.py")])

    args = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm", "--clean",
        "--onefile",          # 打成单个 exe
        "--noconsole",        # 不要黑色命令行窗口
        "--name", APP_NAME,
        "--icon", ICON,
        "--distpath", DIST,
        "--workpath", BUILD,
        "--specpath", BUILD,
        # pystray 的后端是运行时按平台挑的，PyInstaller 静态分析看不到
        "--hidden-import", "pystray._win32",
    ]
    for module in EXCLUDES:
        args += ["--exclude-module", module]
    args.append(os.path.join(HERE, "main.py"))

    print("执行:", " ".join(args))
    result = subprocess.call(args)
    if result != 0:
        print("\n打包失败，请把上面的报错发给我。")
        return result

    exe = os.path.join(DIST, APP_NAME + ".exe")
    if os.path.exists(exe):
        size = os.path.getsize(exe) / 1024 / 1024
        print("\n打包完成：%s （%.1f MB）" % (exe, size))
        print("双击它就能用；首次运行请在它旁边放一份填好 api_key 的 config.json。")
        # 顺手把配置模板拷到 dist，省得用户自己建
        template = os.path.join(HERE, "config.json")
        target = os.path.join(DIST, "config.json")
        if os.path.exists(template) and not os.path.exists(target):
            try:
                shutil.copyfile(template, target)
                print("已复制配置模板到", target)
            except OSError:
                pass
        return 0

    print("打包结束但没找到 exe，请检查上面的输出。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
