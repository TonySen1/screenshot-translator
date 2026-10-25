# -*- coding: utf-8 -*-
"""配置读取与校验。

配置文件 config.json 与程序放在同一个目录：
  - 直接运行源码时 = 项目目录
  - 打包成 exe 后   = exe 所在目录

配置项缺失、类型不对、数值越界都不会让程序崩溃：一律回退到默认值。
"""

from __future__ import annotations

import json
import logging
import os
import sys
from dataclasses import dataclass, fields

log = logging.getLogger(__name__)

# 程序版本号。发版时改这里，同时给 CHANGELOG.md 顶上补一节。
APP_VERSION = "1.2.1"

# 配置文件名
CONFIG_NAME = "config.json"

# 所有配置项的默认值（同时也是 config.json 的初始内容）
DEFAULTS: dict = {
    "api_base_url": "https://api.openai.com/v1",
    "api_key": "",
    "model": "gpt-4o-mini",
    "hotkey": "ctrl+alt+d",
    "exit_hotkey": "ctrl+alt+shift+q",
    "target_language": "中文",
    "mask_opacity": 0.35,
    "overlay_opacity": 0.88,
    "overlay_bg": "#1e1e1e",
    "overlay_fg": "#ffffff",
    "font_size": 12,
    "request_timeout": 30,
}

# 配置项的中文说明，写进自动生成的 config.json 里当注释用（JSON 不支持注释，用 _说明 字段）
HELP: dict = {
    "_说明": {
        "api_base_url": "OpenAI 兼容接口地址，末尾通常带 /v1",
        "api_key": "你的 API Key，必填；留空时按快捷键会提示填写",
        "model": "多模态（能看图）模型名，设置页可以从接口拉取模型列表再选",
        "hotkey": "截图翻译全局快捷键",
        "exit_hotkey": "退出程序全局快捷键",
        "target_language": "翻译成什么语言",
        "mask_opacity": "截图遮罩亮度，0.1~1.0，越小越暗",
        "overlay_opacity": "结果浮层不透明度，0.3~1.0",
        "overlay_bg": "浮层背景色",
        "overlay_fg": "浮层文字颜色",
        "font_size": "浮层字号（磅）",
        "request_timeout": "接口超时时间（秒）",
    }
}

# 用户双击 exe 时必须已经有配置；否则自动生成一份
_USER_WRITABLE = True


def app_dir() -> str:
    """程序所在目录（源码运行时是脚本目录，打包后是 exe 目录）。"""
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def config_path() -> str:
    return os.path.join(app_dir(), CONFIG_NAME)


@dataclass
class Config:
    api_base_url: str = DEFAULTS["api_base_url"]
    api_key: str = DEFAULTS["api_key"]
    model: str = DEFAULTS["model"]
    hotkey: str = DEFAULTS["hotkey"]
    exit_hotkey: str = DEFAULTS["exit_hotkey"]
    target_language: str = DEFAULTS["target_language"]
    mask_opacity: float = DEFAULTS["mask_opacity"]
    overlay_opacity: float = DEFAULTS["overlay_opacity"]
    overlay_bg: str = DEFAULTS["overlay_bg"]
    overlay_fg: str = DEFAULTS["overlay_fg"]
    font_size: int = DEFAULTS["font_size"]
    request_timeout: int = DEFAULTS["request_timeout"]

    @property
    def has_api_key(self) -> bool:
        return bool(self.api_key.strip())


def _as_str(value, default: str) -> str:
    if isinstance(value, str) and value.strip():
        return value.strip()
    return default


def _as_float(value, default: float, low: float, high: float) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    if number != number:  # NaN
        return default
    return min(high, max(low, number))


def _as_int(value, default: int, low: int, high: int) -> int:
    try:
        number = int(round(float(value)))
    except (TypeError, ValueError):
        return default
    return min(high, max(low, number))


def save_config(cfg: Config, path: str | None = None) -> bool:
    """把配置写回 config.json（设置窗口点“保存”时调用）。"""
    path = path or config_path()
    payload = {f.name: getattr(cfg, f.name) for f in fields(Config)}
    payload.update(HELP)
    try:
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
        os.replace(tmp, path)   # 原子替换，避免写一半被打断
        log.info("配置已保存：%s", path)
        return True
    except OSError as exc:
        log.error("保存配置失败：%s", exc)
        try:
            if os.path.exists(path + ".tmp"):
                os.remove(path + ".tmp")
        except OSError:
            pass
        return False


def write_default_config(path: str) -> bool:
    """在磁盘上生成一份带说明的默认 config.json，失败返回 False。"""
    payload = dict(DEFAULTS)
    payload.update(HELP)
    try:
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, ensure_ascii=False, indent=2)
        return True
    except OSError as exc:
        log.warning("无法写入默认配置文件 %s：%s", path, exc)
        return False


def load_config(path: str | None = None) -> Config:
    """读取配置。文件不存在时自动生成；内容有问题时逐项回退默认值。"""
    path = path or config_path()
    raw: dict = {}

    if not os.path.exists(path):
        log.info("未找到配置文件，生成默认配置：%s", path)
        write_default_config(path)
    else:
        try:
            with open(path, "r", encoding="utf-8-sig") as handle:
                loaded = json.load(handle)
            if isinstance(loaded, dict):
                raw = loaded
            else:
                log.warning("配置文件内容不是 JSON 对象，全部使用默认值")
        except (OSError, ValueError) as exc:
            log.warning("配置文件读取失败（%s），全部使用默认值", exc)

    cfg = Config(
        api_base_url=_as_str(raw.get("api_base_url"), DEFAULTS["api_base_url"]),
        api_key=raw.get("api_key").strip() if isinstance(raw.get("api_key"), str) else "",
        model=_as_str(raw.get("model"), DEFAULTS["model"]),
        hotkey=_as_str(raw.get("hotkey"), DEFAULTS["hotkey"]),
        exit_hotkey=_as_str(raw.get("exit_hotkey"), DEFAULTS["exit_hotkey"]),
        target_language=_as_str(raw.get("target_language"), DEFAULTS["target_language"]),
        mask_opacity=_as_float(raw.get("mask_opacity"), DEFAULTS["mask_opacity"], 0.05, 1.0),
        overlay_opacity=_as_float(raw.get("overlay_opacity"), DEFAULTS["overlay_opacity"], 0.3, 1.0),
        overlay_bg=_as_str(raw.get("overlay_bg"), DEFAULTS["overlay_bg"]),
        overlay_fg=_as_str(raw.get("overlay_fg"), DEFAULTS["overlay_fg"]),
        font_size=_as_int(raw.get("font_size"), DEFAULTS["font_size"], 8, 40),
        request_timeout=_as_int(raw.get("request_timeout"), DEFAULTS["request_timeout"], 5, 300),
    )

    _recover_from_services(raw, cfg)

    # 颜色值做个粗校验，避免 Tk 抛异常
    for name in ("overlay_bg", "overlay_fg"):
        value = getattr(cfg, name)
        if not _looks_like_color(value):
            log.warning("配置项 %s 不是合法颜色（%s），改用默认值", name, value)
            setattr(cfg, name, DEFAULTS[name])

    log.info("配置已加载：model=%s base_url=%s key=%s", cfg.model, cfg.api_base_url,
             "已填写" if cfg.has_api_key else "未填写")
    return cfg


def _recover_from_services(raw: dict, cfg: Config) -> None:
    """从上一版的「多服务商」结构里把用户填过的东西提回扁平字段。

    上一版把每家服务商的配置分开存（translation_service + services）。
    现在只留一份配置，但用户的 API Key 不能因为改结构就丢 —— 所以按当时
    选中的那家提回来；提不出来就保持顶层字段的值。
    """
    stored = raw.get("services")
    if not isinstance(stored, dict):
        return
    key = raw.get("translation_service")
    entry = stored.get(key) if isinstance(key, str) else None
    if not isinstance(entry, dict):
        # 选中的那家没找到，就挑第一个填过 Key 的
        entry = next((item for item in stored.values()
                      if isinstance(item, dict) and item.get("api_key")), None)
    if not isinstance(entry, dict):
        return
    if entry.get("api_key"):
        cfg.api_key = str(entry["api_key"]).strip()
    if entry.get("api_base_url"):
        cfg.api_base_url = str(entry["api_base_url"]).strip()
    if entry.get("model"):
        cfg.model = str(entry["model"]).strip()
    log.info("已从旧配置的多服务商结构里恢复出当前设置")


def _looks_like_color(value: str) -> bool:
    if value.startswith("#") and len(value) in (4, 7, 9):
        return all(ch in "0123456789abcdefABCDEF" for ch in value[1:])
    return value.isalpha() and len(value) >= 3


def config_field_names() -> list[str]:
    return [f.name for f in fields(Config)]
