# -*- coding: utf-8 -*-
"""真实 API 验证：用一张本地生成的测试图调 DeepSeek 视觉模型。

不截屏、不上传任何本机屏幕内容。

用法：
    set DS_KEY=sk-xxx
    .venv\\Scripts\\python.exe dev/test_real_api.py [模型名]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PIL import Image, ImageDraw, ImageFont  # noqa: E402

import config as config_module  # noqa: E402
import screenshot  # noqa: E402
import translator  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def make_test_image() -> Image.Image:
    """造一张像软件设置窗口的英文截图。"""
    img = Image.new("RGB", (1100, 330), "#ffffff")
    d = ImageDraw.Draw(img)

    def font(size):
        for name in ("segoeui.ttf", "arial.ttf"):
            try:
                return ImageFont.truetype("C:/Windows/Fonts/" + name, size)
            except OSError:
                continue
        return ImageFont.load_default()

    f_big, f_small = font(28), font(23)

    d.rectangle([0, 0, 1100, 62], fill="#2b2b2b")
    d.text((22, 15), "TurboHUD  -  Settings", fill="#ffffff", font=f_big)

    d.text((44, 100), "Automatic updates are currently enabled.",
           fill="#111111", font=f_small)
    d.text((44, 148), "The application checks for new versions every time it starts.",
           fill="#111111", font=f_small)
    d.text((44, 196), "To turn this off, open the Settings panel and switch it off.",
           fill="#111111", font=f_small)
    d.text((44, 252), "Last checked: 3 minutes ago.", fill="#7a7a7a", font=f_small)
    return img


def main() -> int:
    key = os.environ.get("DS_KEY", "").strip()
    if not key:
        print("请先设置环境变量 DS_KEY")
        return 1
    model = sys.argv[1] if len(sys.argv) > 1 else "deepseek-flash"

    cfg = config_module.Config()
    cfg.api_base_url = "https://api.deepseek.com/v1"
    cfg.api_key = key
    cfg.model = model
    cfg.request_timeout = 90
    cfg.target_language = "中文"

    image = make_test_image()
    image.save(os.path.join(HERE, "test_image.png"))
    print("测试图已生成:", image.size, "（纯本地生成，不是屏幕截图）", flush=True)
    print("模型:", model, "| 接口:", cfg.api_base_url, flush=True)
    print("图片将作为 data:image/png;base64 发送，尺寸约 %d KB"
          % (len(translator.image_to_png_base64(translator.prepare_image(image))) * 3 // 4 // 1024),
          flush=True)

    started = time.time()
    result = translator.translate_image(image, cfg)
    elapsed = time.time() - started

    print("\n--- 耗时 %.1f 秒 ---" % elapsed, flush=True)
    if result.ok:
        print("原文:\n%s" % result.original_text, flush=True)
        print("\n译文:\n%s" % result.translated_text, flush=True)
        print("\n[PASS] 真实接口调用成功，JSON 解析正常", flush=True)
        return 0

    print("错误:", result.error, flush=True)
    print("\n[FAIL] 调用失败", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
