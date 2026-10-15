# -*- coding: utf-8 -*-
"""从 OpenAI 兼容接口拉模型列表，供设置页选择。

只依赖协议里唯一有保证的那部分：GET {base_url}/models 返回
{"data": [{"id": "模型名"}, ...]}。各家额外塞的字段（定价、上下文长度等）
一律不解析 —— 那些字段名各不一样，认了就等着踩坑。

关于「哪些模型是视觉模型」：接口里没有统一的标识位。硅基流动的 sub_type
只分 chat/embedding/reranker 这类任务类别，并没有 vision 这个值。
所以这里用模型名做启发式判断（见 vision_models），结果仅供参考 ——
真正的判据是设置页的「测试服务」，它会发一张真图过去试。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field

log = logging.getLogger(__name__)

# 模型名里出现这些，基本可以断定不是能看图的对话模型
_NOT_CHAT = (
    "embed", "embedding", "rerank", "reranker", "moderation",
    "tts", "whisper", "speech", "audio", "voice",
    "stable-diffusion", "flux", "kolors", "dall-e", "sd3", "sd-xl",
    "text-to-image", "image-to-image", "video", "wan-", "cogvideo",
)

# 出现这些的一般是能看图的多模态模型
_VISION_HINTS = (
    "vl", "vision", "-v-", "multimodal", "omni", "4v", "5v", "6v",
    "gpt-4o", "gpt-4.1", "gpt-5", "o3", "o4",
    "gemini", "claude-3", "claude-4", "claude-5", "claude-sonnet", "claude-opus",
    "glm-4v", "glm-4.1v", "glm-4.5v", "glm-4.6v", "glm-5v",
    "qwen-vl", "qwen2-vl", "qwen2.5-vl", "qwen3-vl", "internvl", "minicpm-v",
    "llava", "pixtral", "phi-3-vision", "phi-4-vision", "smolvlm", "moondream",
    "step-1v", "ernie-vl", "doubao-vision", "seed-vl", "hunyuan-vision",
)


@dataclass
class ModelList:
    ok: bool
    message: str = ""
    models: list[str] = field(default_factory=list)      # 全部（已排序）
    vision: list[str] = field(default_factory=list)      # 猜出来的视觉模型
    detail: str = ""


def looks_like_vision(model_id: str) -> bool:
    """按模型名猜是不是能看图。猜错也不要紧，用户可以用测试服务验证。"""
    name = (model_id or "").lower()
    if any(bad in name for bad in _NOT_CHAT):
        return False
    return any(hint in name for hint in _VISION_HINTS)


def client_for(cfg, timeout: float | None = None):
    """按配置建一个 openai 客户端（只有这一处拼参数）。"""
    from openai import OpenAI

    return OpenAI(
        api_key=cfg.api_key,
        base_url=cfg.api_base_url,
        timeout=float(timeout or cfg.request_timeout),
        max_retries=1,
    )


def _status_of(exc: Exception):
    status = getattr(exc, "status_code", None)
    if status is not None:
        return status
    text = str(exc)
    for code in (401, 403, 404, 429, 500, 502, 503):
        if "Error code: %d" % code in text:
            return code
    return None


def _extract_ids(payload) -> list[str]:
    """从 /models 的返回里抠出模型名。

    兼容几种常见形状：OpenAI 的 {"data": [{"id": ...}]}、个别服务直接返回
    数组、以及 data 里直接放字符串的情况。
    """
    items = payload
    if isinstance(payload, dict):
        items = payload.get("data")
        if items is None:
            items = payload.get("models")
    if not isinstance(items, (list, tuple)):
        return []

    ids: list[str] = []
    for item in items:
        if isinstance(item, str):
            name = item
        elif isinstance(item, dict):
            name = item.get("id") or item.get("model") or item.get("name") or ""
        else:
            name = ""
        name = str(name).strip()
        if name and name not in ids:
            ids.append(name)
    return ids


def list_models(cfg, timeout: float | None = None) -> ModelList:
    """拉取模型列表。任何异常都转成 ModelList(ok=False)，不向上抛。"""
    if not cfg.has_api_key:
        return ModelList(False, "请先填 API Key")
    if not (cfg.api_base_url or "").startswith("http"):
        return ModelList(False, "接口地址不对", cfg.api_base_url)

    try:
        client = client_for(cfg, timeout or 20)
    except Exception as exc:
        log.exception("创建客户端失败")
        return ModelList(False, "无法初始化请求：%s" % exc)

    try:
        response = client.models.list()
        payload = response.model_dump() if hasattr(response, "model_dump") else response
        ids = _extract_ids(payload)
    except Exception as exc:
        log.warning("拉取模型列表失败：%s", exc)
        status = _status_of(exc)
        if status == 401:
            return ModelList(False, "API Key 无效")
        if status == 403:
            return ModelList(False, "接口拒绝访问（403）")
        if status == 404:
            return ModelList(False, "接口地址不对（404）",
                             "确认地址形如 https://xxx/v1")
        if status == 429:
            return ModelList(False, "请求过于频繁（429），稍后再试")
        text = str(exc).lower()
        if "timed out" in text or "timeout" in text:
            return ModelList(False, "请求超时，检查网络")
        return ModelList(False, "拉取失败：%s" % str(exc).strip().splitlines()[0][:80])

    if not ids:
        return ModelList(False, "接口没有返回任何模型",
                         "确认地址是 OpenAI 兼容的 /v1 端点")

    vision = [name for name in ids if looks_like_vision(name)]
    # 猜出来的视觉模型排前面，其余按名字排，列表好找一些
    ordered = vision + sorted(name for name in ids if name not in set(vision))
    return ModelList(True, "共 %d 个模型，其中 %d 个像是能看图的" % (len(ids), len(vision)),
                     ordered, vision)
