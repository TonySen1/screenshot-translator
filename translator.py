# -*- coding: utf-8 -*-
"""调用多模态大模型识别图中文字并翻译成中文。

只依赖 OpenAI 兼容的 /chat/completions 接口，任何兼容该协议的服务都能用
（OpenAI、通义千问、智谱、DeepSeek、本地 vLLM / Ollama 等）。
"""

from __future__ import annotations

import base64
import json
import logging
import re
from dataclasses import dataclass

from PIL import Image

log = logging.getLogger(__name__)

# 送给模型的系统提示词。要求只回 JSON，避免各种客套话污染解析。
SYSTEM_PROMPT = """你是一个 OCR + 翻译引擎。请识别图片中所有可见的文字，并将其翻译成{target_language}。

严格遵守以下规则：
1. 只输出一个 JSON 对象，不要输出 Markdown 代码块，不要输出任何解释、前缀或后缀文字。
2. JSON 格式固定为：{{"original_text": "识别出的原文", "translated_text": "翻译后的中文"}}
3. original_text 保留识别到的原语言文字，尽量保持原有换行（用 \n 表示）。
4. translated_text 是翻译成{target_language}之后的结果，保持与原文相同的段落结构。
5. 如果图片中没有任何文字，两个字段都返回空字符串。
6. 不要翻译专有名词、代码、变量名、URL 和数字，原样保留。"""

USER_PROMPT = "请识别这张图片里的文字并翻译。"

# 图片预处理：太小的图放大有助于 OCR，太大的图压到模型友好尺寸
_MIN_SIDE = 96
_MAX_SIDE = 2048


@dataclass
class TranslationResult:
    """一次翻译的结果。error 非空表示失败，此时两个文本字段为空。"""

    original_text: str = ""
    translated_text: str = ""
    error: str = ""

    @property
    def ok(self) -> bool:
        return not self.error


def prepare_image(image: Image.Image) -> Image.Image:
    """按需缩放图片，返回新的 Image（不改动入参）。

    小图放大利于识别小字，大图压缩避免请求体过大。
    """
    width, height = image.size
    if width <= 0 or height <= 0:
        return image

    longest, shortest = max(width, height), min(width, height)
    scale = 1.0

    if longest > _MAX_SIDE:
        scale = _MAX_SIDE / longest
    elif shortest < _MIN_SIDE:
        # 最多放大 3 倍，避免把小图糊成马赛克
        scale = min(3.0, _MIN_SIDE / shortest)
        if longest * scale > _MAX_SIDE:
            scale = _MAX_SIDE / longest

    if abs(scale - 1.0) < 0.01:
        return image

    new_size = (max(1, round(width * scale)), max(1, round(height * scale)))
    resample = Image.LANCZOS if scale < 1 else Image.BICUBIC
    log.debug("图片缩放 %sx%s -> %sx%s (scale=%.2f)", width, height, *new_size, scale)
    return image.resize(new_size, resample)


def image_to_png_base64(image: Image.Image) -> str:
    """PIL 图片 -> PNG 的 base64 字符串。"""
    from io import BytesIO

    buffer = BytesIO()
    image.save(buffer, format="PNG", optimize=False)
    return base64.b64encode(buffer.getvalue()).decode("ascii")


def _build_messages(png_base64: str, target_language: str) -> list[dict]:
    return [
        {
            "role": "system",
            "content": SYSTEM_PROMPT.format(target_language=target_language),
        },
        {
            "role": "user",
            "content": [
                {"type": "text", "text": USER_PROMPT},
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:image/png;base64,{png_base64}"},
                },
            ],
        },
    ]


def _extract_json(text: str) -> dict | None:
    """从模型回复里尽量抠出一个 JSON 对象。"""
    if not text:
        return None

    candidates = []

    # 1) 去掉 ```json ... ``` 代码块围栏
    fenced = re.search(r"```(?:json)?\s*(.*?)```", text, re.DOTALL | re.IGNORECASE)
    if fenced:
        candidates.append(fenced.group(1).strip())

    # 2) 原文本身
    candidates.append(text.strip())

    # 3) 第一个 { 到最后一个 } 之间的内容
    start, end = text.find("{"), text.rfind("}")
    if start != -1 and end > start:
        candidates.append(text[start:end + 1])

    for candidate in candidates:
        try:
            parsed = json.loads(candidate)
        except (ValueError, TypeError):
            continue
        if isinstance(parsed, dict):
            return parsed
    return None


def _pick(data: dict, *keys: str) -> str:
    """从解析出的字典里按候选键名取值，全部缺失时返回空串。"""
    for key in keys:
        value = data.get(key)
        if isinstance(value, str):
            return value.strip()
        if isinstance(value, (list, tuple)):
            joined = "".join(str(item) for item in value).strip()
            if joined:
                return joined
    return ""


def parse_response(content: str) -> TranslationResult:
    """解析模型返回的文本。解析不出 JSON 时退而求其次，把整段当作译文。"""
    content = (content or "").strip()
    if not content:
        return TranslationResult(error="翻译失败：模型没有返回内容")

    data = _extract_json(content)
    if data is not None:
        original = _pick(data, "original_text", "originalText", "original", "source_text", "text")
        translated = _pick(data, "translated_text", "translatedText", "translated", "translation",
                           "target_text", "result")
        if original or translated:
            return TranslationResult(original_text=original, translated_text=translated)
        # JSON 合法但没有可用字段，说明模型没按格式来
        log.warning("模型返回的 JSON 缺少有效字段：%s", content[:300])
    else:
        log.warning("模型返回的不是合法 JSON，按纯文本处理：%s", content[:300])

    # 兜底：把整段回复当译文显示，总比甩一句"翻译失败"有用
    return TranslationResult(translated_text=content)


def _friendly_error(exc: Exception, timeout: int) -> str:
    """把各种异常翻译成一句人话。"""
    name = type(exc).__name__
    text = str(exc)

    status = getattr(exc, "status_code", None)
    if status == 401 or "401" in text[:120]:
        return "API Key 无效，请检查 config.json"
    if status == 403:
        return "API 拒绝访问（403），请检查 Key 权限"
    if status == 404 or "model_not_found" in text:
        return "模型名或 api_base_url 不正确"
    if status == 429:
        return "请求过于频繁或额度不足，请稍后再试"
    if status is not None and 500 <= int(status) < 600:
        return "接口服务端错误（{}），请稍后再试".format(status)

    if "timed out" in text.lower() or "timeout" in text.lower() or name.endswith("Timeout"):
        return f"请求超时（{timeout} 秒），请检查网络"
    if name in ("APIConnectionError", "ConnectError", "ConnectionError",
                "NewConnectionError", "MaxRetryError", "SSLError"):
        return "网络连接失败，请检查网络或 api_base_url"

    short = text.strip().splitlines()[0] if text.strip() else name
    if len(short) > 80:
        short = short[:80] + "…"
    return f"翻译失败：{short}"


def translate_image(image: Image.Image, cfg, png_base64: str | None = None,
                    timeout: float | None = None) -> TranslationResult:
    """把选中的图片送去识别 + 翻译。

    timeout 用于覆盖配置里的超时（设置页的"测试服务"希望快点返回，
    不想让用户干等 30 秒）。任何异常都会被吃掉并转换成
    TranslationResult.error，绝不向上抛。
    """
    if not cfg.has_api_key:
        return TranslationResult(error="请先填 api_key（右键托盘图标 → 打开设置）")

    if png_base64 is None:
        try:
            png_base64 = image_to_png_base64(prepare_image(image))
        except Exception as exc:  # 图片处理失败
            log.exception("图片编码失败")
            return TranslationResult(error=f"图片处理失败：{exc}")

    try:
        from openai import OpenAI

        client = OpenAI(
            api_key=cfg.api_key,
            base_url=cfg.api_base_url,
            timeout=float(timeout if timeout else cfg.request_timeout),
            max_retries=1,
        )
        response = client.chat.completions.create(
            model=cfg.model,
            messages=_build_messages(png_base64, cfg.target_language),
        )
        content = response.choices[0].message.content or ""
        result = parse_response(content)
        if result.ok:
            log.info("翻译完成：原文 %d 字，译文 %d 字",
                     len(result.original_text), len(result.translated_text))
        return result
    except Exception as exc:
        log.exception("翻译请求失败")
        return TranslationResult(error=_friendly_error(exc, int(timeout or cfg.request_timeout)))
