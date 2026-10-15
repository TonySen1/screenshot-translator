# -*- coding: utf-8 -*-
"""设置页的「测试服务」：真的发一次请求，验证这套配置能不能用。

做法是端到端跑一遍完整链路 —— 现画一张写着 "42" 的小图当截图，
走 translate_image()，再看译文里有没有 42。
只发一个字符，成本可以忽略，而且能同时验证：地址对不对、Key 有没有效、
模型名字对不对、这个模型到底能不能看图。

界面上只显示「连接成功」或「连接失败」两种结果，具体原因写日志。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass

from PIL import Image, ImageDraw, ImageFont

log = logging.getLogger(__name__)

# 测试图里写的字，以及界面上的两种结论
TEST_TOKEN = "42"
OK_TEXT = "连接成功"
FAIL_TEXT = "连接失败"

_FONT_CANDIDATES = ("consola.ttf", "cour.ttf", "arial.ttf", "msyh.ttc")

_IMAGE_W, _IMAGE_H = 320, 140
_BG = "#ffffff"
_FG = "#111111"


@dataclass
class ServiceTestResult:
    ok: bool
    message: str = ""
    detail: str = ""          # 只写日志，不上界面


def make_test_image() -> Image.Image:
    """画一张带文字的小图，当作假截图。"""
    image = Image.new("RGB", (_IMAGE_W, _IMAGE_H), _BG)
    draw = ImageDraw.Draw(image)

    font = None
    for name in _FONT_CANDIDATES:
        try:
            font = ImageFont.truetype(name, 72)
            break
        except OSError:
            continue
    if font is None:
        # 找不到字体文件就用 Pillow 自带的位图字体，字小但照样能认
        font = ImageFont.load_default()

    text = TEST_TOKEN
    try:
        box = draw.textbbox((0, 0), text, font=font)
    except (AttributeError, ValueError):
        box = (0, 0, 40, 40)
    x = (_IMAGE_W - (box[2] - box[0])) // 2 - box[0]
    y = (_IMAGE_H - (box[3] - box[1])) // 2 - box[1]
    draw.text((x, y), text, font=font, fill=_FG)
    return image


def test_service(cfg, timeout: int | None = None) -> ServiceTestResult:
    """用当前配置真发一次翻译请求。

    返回的只有成功/失败两种结论；失败原因记进日志，界面上不展开
    （用户要的是"能不能用"这一个判断）。
    """
    import translator

    image = make_test_image()
    try:
        png = translator.image_to_png_base64(image)
    except Exception as exc:                   # 图片编码失败基本不可能发生
        log.exception("测试图编码失败")
        return ServiceTestResult(False, FAIL_TEXT, "测试图生成失败：%s" % exc)

    log.info("测试服务：base_url=%s model=%s", cfg.api_base_url, cfg.model)
    result = translator.translate_image(
        image, cfg, png_base64=png,
        timeout=timeout or min(20, int(getattr(cfg, "request_timeout", 30))))

    if not result.ok:
        log.warning("测试服务失败：%s", result.error)
        return ServiceTestResult(False, FAIL_TEXT, result.error or "")

    seen = (result.original_text or "") + (result.translated_text or "")
    if TEST_TOKEN not in seen:
        # 最常见的情况是选了个不看图的模型（比如 DeepSeek 的 v4-pro / chat
        # 这类纯文本档位），它们会把图片块丢掉、回一句"没看到图"。
        # 这和"连接不上"是两回事，日志里要分清楚，不然没法排查。
        detail = "模型没有读出测试图（多半是这个模型不支持图片输入）"
        if not (result.original_text or result.translated_text):
            detail += "；模型返回了空内容"
        log.warning("测试服务失败：%s。模型返回=%r",
                    detail, (result.translated_text or result.original_text)[:200])
        return ServiceTestResult(False, FAIL_TEXT, detail)

    log.info("测试服务成功")
    return ServiceTestResult(True, OK_TEXT)
