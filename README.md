# 截图翻译

[![build](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml/badge.svg)](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml)

**中文** · [更新日志](CHANGELOG.md)

一个 Windows 截图翻译小工具。按下快捷键框选屏幕区域，译文直接浮现在原位置。

没有主窗口、没有登录、没有广告、没有历史记录 —— 启动后桌面干干净净，只在任务栏留一个小图标。**用完即走，不留痕迹。**

---

## 本地识别，框选直接翻译

- **原位覆盖译文**：表格和文字会直接覆盖原文位置，不再在旁边生成第二张表；原网格、列宽、行高保留，背景色取自原图对应区域。
- **表格保留行列**：清晰、对齐的表格按单元格翻译和显示，空间足够时放在原图旁边。长译文在格内换行；复制后可按原行列粘贴到表格软件。
- **不等模型启动再截图**：Win32 主程序启动后在后台预热模型。松手立即显示进度，识别和翻译都在后台执行。
- **保留看图翻译**：如遇识别困难的排版，可从托盘主动选择「看图翻译（上传截图）」。默认模式不会自动上传截图。

界面、截图和全局热键仍用 C11 / Win32 实现。发布包自带 OCR 运行时，不需要安装 Python 或 Windows OCR 语言包。

---

## 一、最快上手

1. 从 [Releases](../../releases) 下载 `ScreenshotTranslator-windows-x64.zip`，完整解压到可写目录。保持 `ScreenshotTranslator.exe` 和 `ocr` 文件夹在同一目录。
2. **双击运行**。桌面上不会出现窗口，只在任务栏右下角的通知区域多一个小图标 —— 这是**正常的**。
3. **右键那个小图标 → 设置**，选择下面一种文本翻译方式，测试通过后点「保存」：
   - **DeepL**：只填「DeepL 密钥」，点「测试 DeepL」即可，不用填接口地址或选模型。程序自动使用 API Free 或 Pro 端点。
   - **OpenAI 兼容**：留空「DeepL 密钥」，填写 OpenAI 地址（如 `https://xxx/v1`）、密钥和模型，再点「测试 OpenAI」。
   - 同时配置时，默认截图文字用 DeepL；托盘「看图翻译」仍使用 OpenAI 兼容配置，模型必须支持图片。
4. 关掉设置窗口。现在它就安静地在后台待命了。
5. 按下 **Ctrl + Alt + D**，屏幕变暗，拖鼠标框住要翻译的内容，松手。
6. 译文浮层出现在**鼠标按下的位置**附近。看完按 **Esc**、点右上角的 ✕，或点浮层外面的任何地方，它就消失了。

---

## 二、浮层上的三个按钮

| 图标 | 作用 |
|---|---|
| 文档 | **复制原文**（识别出来的那段文字） |
| 复制 | **复制译文** |
| ✕ | 关闭浮层 |

悬停在按钮上会显示提示。按住译文区域可以拖动整个浮层。

---

## 三、快捷键

| 快捷键 | 作用 |
|---|---|
| `Ctrl + Alt + D` | 截图翻译 |
| `Ctrl + Alt + Shift + Q` | 退出程序 |
| `Esc` | 关闭译文浮层 |

全部可以在设置窗口里改：点进输入框，直接按下你想要的组合键即可，不用手动输入文字。热键格式在保存前会先试注册一次，被别的程序占用会当场提示。

---

## 四、配置文件

配置保存在 exe 同目录的 `config.json`，纯 JSON，可以直接用文本编辑器改。程序首次运行会自动生成一份默认值。

```json
{
    "api_base_url": "https://api.openai.com/v1",
    "api_key": "",
    "deepl_api_key": "",
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
| `api_base_url` | OpenAI 兼容接口地址，末尾通常带 `/v1` |
| `api_key` | OpenAI 兼容接口密钥 |
| `deepl_api_key` | DeepL 密钥；非空时用于 OCR 文本翻译，留空用 OpenAI 兼容接口 |
| `model` | OpenAI 兼容的模型名；看图翻译需要支持图片 |
| `hotkey` / `exit_hotkey` | 全局快捷键，格式如 `ctrl+alt+d` |
| `target_language` | 译文语言；DeepL 下「中文」等同 `ZH-HANS`，也可填 `ZH-HANT`、`EN-US` 等语言代码 |
| `mask_opacity` | 截图遮罩的不透明度，0.05 ~ 1.0，越小越暗 |
| `overlay_opacity` | 译文浮层的不透明度，0.30 ~ 1.0 |
| `overlay_bg` / `overlay_fg` | 浮层的背景色 / 文字色 |
| `font_size` | 译文字号（磅） |
| `request_timeout` | 接口超时（秒） |

任何单项写错都只会回退该项的默认值，不影响其他设置。整个文件不是合法 JSON 时，会备份成 `config.json.broken` 再重建。

---

## 五、从源码构建

需要 Visual Studio 2022 Build Tools（MSVC 14.4x）、Windows SDK 10，以及首次准备 OCR 运行时的网络连接。

```powershell
.\build.bat
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup-ocr.ps1
```

运行 `build\screenshot-translator.exe`，不要移走同级 `build\ocr`。只有准备运行时时才下载 OCR 依赖；运行时不会联网下载模型。

本地运行 OCR 冒烟测试（需要 Windows 已安装英日韩字体，用于渲染测试图，与 OCR 语言包无关）：

```powershell
.\build\ocr\python.exe -I -B -X utf8 .\tests\smoke_ocr.py --artifacts .\build\ocr-smoke
```

### 代码结构

| 文件 | 职责 |
|---|---|
| `src/main.c` | 入口、隐藏消息窗口、翻译线程调度 |
| `src/capture.c` | 全屏抓取、遮罩窗口、框选、淡入 |
| `src/overlay.c` | 译文浮层窗口、鼠标交互、剪贴板 |
| `src/overlay_gdiplus.cpp` | 唯一的 C++ 单元：GDI+ 文字排版与绘制 |
| `src/settings.c` | 设置窗口（原生控件 + 深色主题） |
| `src/translator.c` | 请求构造、响应解析 |
| `src/ocr_process.c` | 管理常驻 OCR 子进程、限时管道通信 |
| `ocr/worker.py` | 本地 PaddleOCR 推理与英日韩识别分流 |
| `src/http.c` | WinHTTP 封装 |
| `src/hotkey.c` | 全局热键解析与注册 |
| `src/config.c` | config.json 读写 |
| `src/image.c` | DIB 缓冲、缩放、JPEG 编码 |
| `src/tray.c` | 托盘图标与菜单 |
| `src/base64.c` | Base64 编码 |

OCR 采用 PaddleOCR 模型，经 RapidOCR 和 ONNX Runtime 在 CPU 上运行。主程序内嵌 cJSON 与 stb_image_write。

---

## 六、常见问题

**双击后什么都没发生？**
正常。程序是托盘程序，看任务栏右下角的通知区域，可能要点开「显示隐藏的图标」。

**按快捷键没反应？**
先确认它没被别的程序占用：打开设置窗口，把热键重新保存一次，被占用会当场提示。某些游戏、录屏软件会独占全局热键。

**翻译报「API Key 无效」？**
检查 Key 有没有多余的空格，以及接口地址是否和 Key 所属的服务商一致。

**图片里的文字没识别出来？**
尽量只框住文字，避免大面积背景；很小的字可以先在原窗口放大。对复杂排版可以从托盘选择「看图翻译（上传截图）」。

**提示未找到 PaddleOCR？**
从 ZIP 完整解压并保留 `ocr` 目录。若从源码构建，运行 `setup-ocr.ps1` 后重试。

**配置文件不见了？**
程序会把配置写在 exe 同目录。如果 exe 放在 Program Files 这类只读目录，会退到 `%LOCALAPPDATA%\ScreenshotTranslatorC`。

---

## 许可

[MIT](LICENSE)
