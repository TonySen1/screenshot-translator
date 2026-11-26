# 截图翻译

[![build](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml/badge.svg)](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml)

**中文** · [更新日志](CHANGELOG.md)

Windows 截图翻译小工具：按快捷键框选屏幕区域，译文直接浮现在原位置。没有主窗口，只在托盘留一个图标。

## 功能

- 译文覆盖原文位置，保留原图配色与对齐。
- 表格按单元格翻译，行列与列宽不变，复制后可按原行列粘贴回表格软件。
- 本地 OCR，默认只发送识别出的文本，不上传截图。
- 排版复杂时可用托盘「看图翻译（上传截图）」，显式选择才上传。
- 发布包自带 OCR 运行时，无需安装 Python 或 Windows OCR 语言包。

## 使用

1. 下载 `ScreenshotTranslator-windows-x64.zip`，解压到可写目录，`ScreenshotTranslator.exe` 与 `ocr` 目录保持同级。
2. 双击运行。不出现窗口，托盘区多一个图标。
3. 右键该图标 → 设置 → 选择提供商并填写对应字段 → 「测试连接」通过 → 保存。
4. 按 `Ctrl + Alt + D`，拖动框选，松手查看译文。

## 提供商

| 提供商 | 需要密钥 | 填写内容 |
|---|---|---|
| DeepL | 是 | 密钥；免费版以 `:fx` 结尾，自动使用免费端点 |
| OpenAI 兼容 | 是 | 接口地址、密钥、模型名 |
| Google Web | 否 | 无需填写 |
| 微软翻译 | 否 | 无需填写 |
| 有道智云 | 是 | `应用ID:应用密钥`，需在控制台开通文本翻译 |
| 腾讯 TranSmart、Microsoft Android、Yandex | — | 暂不提供 |

「接口地址」显示当前服务实际使用的地址，只有 OpenAI 兼容可以修改；「申请地址」打开对应控制台。同时配置多个服务时，截图文字翻译优先用 DeepL，托盘「看图翻译」固定用 OpenAI 兼容（模型需支持图片）。测试连接只测当前选中的服务，失败不会自动切换。

## 快捷键

| 快捷键 | 作用 |
|---|---|
| `Ctrl + Alt + D` | 截图翻译 |
| `Ctrl + Alt + Shift + Q` | 退出程序 |
| `Esc` | 关闭译文浮层 |

在设置窗口里点进输入框后直接按组合键即可修改，保存前会试注册，被占用会当场提示。

## 浮层按钮

| 图标 | 作用 |
|---|---|
| 文档 | 复制原文 |
| 复制 | 复制译文 |
| ✕ | 关闭浮层 |

按住译文区域可拖动整个浮层。

## 配置文件

配置保存在 exe 同目录的 `config.json`，首次运行自动生成。

```json
{
    "api_base_url": "https://api.openai.com/v1",
    "api_key": "",
    "deepl_api_key": "",
    "youdao_api_key": "",
    "model": "gpt-4o-mini",
    "hotkey": "ctrl+alt+d",
    "exit_hotkey": "ctrl+alt+shift+q",
    "target_language": "中文",
    "mask_opacity": 0.35,
    "overlay_opacity": 0.88,
    "overlay_bg": "#1e1e1e",
    "overlay_fg": "#ffffff",
    "font_size": 12,
    "request_timeout": 30
}
```

| 字段 | 说明 |
|---|---|
| `api_base_url`、`api_key`、`model` | OpenAI 兼容的地址、密钥、模型名 |
| `deepl_api_key` | DeepL 密钥 |
| `youdao_api_key` | 有道智云凭据，格式 `应用ID:应用密钥` |
| `hotkey`、`exit_hotkey` | 全局快捷键，如 `ctrl+alt+d` |
| `target_language` | 译文语言，可填语言名或代码 |
| `mask_opacity`、`overlay_opacity` | 截图遮罩与译文浮层的不透明度 |
| `overlay_bg`、`overlay_fg`、`font_size` | 浮层的背景色、文字色、字号 |
| `request_timeout` | 接口超时（秒） |

单项写错只回退该项；整份不是合法 JSON 时会备份成 `config.json.broken` 再重建。

## 构建

需要 Visual Studio 2022 Build Tools 与 Windows SDK 10。

```powershell
.\build.bat
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-ocr.ps1
```

产物在 `build\` 下，`build\ocr` 不要移走。界面与截图用 C11 / Win32，`src/overlay_gdiplus.cpp` 是唯一的 C++ 单元，OCR 由 `ocr/worker.py` 在常驻子进程里运行。

## 常见问题

**双击后什么都没发生？** 正常，程序是托盘程序，可能要点开「显示隐藏的图标」。

**按快捷键没反应？** 打开设置把热键重新保存一次，被其他程序占用会当场提示。

**提示未找到 PaddleOCR？** 从 ZIP 完整解压并保留 `ocr` 目录；从源码构建先运行 `setup-ocr.ps1`。

**配置文件在哪？** exe 同目录；该目录只读时退到 `%LOCALAPPDATA%\ScreenshotTranslatorC`。

## 许可

[MIT](LICENSE)