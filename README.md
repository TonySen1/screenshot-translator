# 截图翻译

[![build](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml/badge.svg)](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml)

**中文** · [更新日志](CHANGELOG.md)

一个 Windows 截图翻译小工具。按下快捷键框选屏幕区域，译文直接浮现在原位置。

没有主窗口、没有登录、没有广告、没有历史记录 —— 启动后桌面干干净净，只在任务栏留一个小图标。**用完即走，不留痕迹。**

---

## 2.0.0：用 C 重写的原生版本

2.0.0 是一次彻底的重构。整体从 Python + Tkinter 改写为 **C11 + 纯 Win32 API**，只保留了 OpenAI 兼容接口这一项外部约定。

| | 1.x（Python） | 2.0.0（C） |
|---|---|---|
| 单文件体积 | 约 40 MB（PyInstaller 打包） | **390 KB** |
| 依赖的运行时 | 随包携带 Python 解释器 | **无**，只用 Windows 自带 DLL |
| 冷启动 | 需要解包、初始化解释器与 Tk | **约 30 ms** |
| 常驻内存 | 数十 MB | **约 30 MB** |
| 需要额外安装 | 无，但包体大 | **无**，双击即用 |

### 体积更小

整个程序只有一个 `screenshot-translator.exe`，**390 KB**。

用 `dumpbin /dependents` 看，它只依赖系统自带的动态库：

```
USER32.dll     GDI32.dll    MSIMG32.dll   gdiplus.dll
WINHTTP.dll    COMCTL32.dll SHELL32.dll  KERNEL32.dll
```

没有 `MSVCR*.dll`（C 运行时已静态链接），没有 Python 解释器，没有第三方库的 DLL。复制这一个文件到任何 Windows 10/11 上都能直接运行。

### 响应更快

- **启动**：静态链接 + 无解释器初始化，冷启动约 30 ms。
- **截图**：`BitBlt` 直接抓进 32 位 DIB，选区压暗、恢复原亮度、画边框全部在内存 DC 里完成，最后一次性 `BitBlt` 上屏，拖动框选时没有闪烁。
- **浮层渲染**：先把圆角背景、文字、工具栏全画进一张预乘 alpha 位图，再用 `UpdateLayeredWindow` 一次性提交。关掉窗口时不会看到逐帧绘制的过程。
- **翻译请求**：WinHTTP 在后台线程发起，主线程继续响应鼠标，浮层立刻显示「正在识别并翻译…」，结果回来再替换文字。

### 更稳定

- **热键用 `RegisterHotKey`，不用低级键盘钩子**。不拦截、不改写任何按键，`Ctrl+Alt+D` 按下后仍会传给前台程序。
- **截图有重入保护**。截图过程有自己的消息循环，重复触发会被直接忽略，不会叠出多层黑色遮罩。
- **热键状态自洽**。设置窗口整个生命周期内暂停全局热键，关闭时按最新配置重新注册，改来改去不会让快捷键失效。
- **配置文件损坏可恢复**。JSON 解析失败时自动备份为 `config.json.broken`，再写一份默认值并弹窗提示，程序照常启动。
- **翻译线程受控退出**。退出时等待后台线程收尾，不会留下悬空的消息投递。
- **便携**。配置和日志都写在 exe 同目录，不碰注册表、不碰 AppData。

---

## 一、最快上手

1. 从 [Releases](../../releases) 页面下载 `screenshot-translator.exe`，把它复制到一个**你自己新建的文件夹**里，比如 `D:\截图翻译\`。
   下载后可以自己改名成 `截图翻译.exe`，不影响运行。
   不要放在桌面或 C 盘的程序目录里，否则 Windows 可能不让它写配置文件。
2. **双击运行**。桌面上不会出现窗口，只在任务栏右下角的通知区域多一个小图标 —— 这是**正常的**。
3. **右键那个小图标 → 设置**，填好这几项，然后点「保存」：
   - **接口地址**：填服务商的 OpenAI 兼容地址，形如 `https://xxx/v1`
   - **API Key**：填你自己的 Key
   - **模型**：点「拉取列表」从接口拉下来选一个
   - 不确定能不能用就点「**测试服务**」—— 它会真发一个请求过去试
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
| `api_key` | 你的 API Key |
| `model` | 多模态模型名（要能看图） |
| `hotkey` / `exit_hotkey` | 全局快捷键，格式如 `ctrl+alt+d` |
| `target_language` | 翻译成什么语言 |
| `mask_opacity` | 截图遮罩的不透明度，0.05 ~ 1.0，越小越暗 |
| `overlay_opacity` | 译文浮层的不透明度，0.30 ~ 1.0 |
| `overlay_bg` / `overlay_fg` | 浮层的背景色 / 文字色 |
| `font_size` | 译文字号（磅） |
| `request_timeout` | 接口超时（秒） |

任何单项写错都只会回退该项的默认值，不影响其他设置。整个文件不是合法 JSON 时，会备份成 `config.json.broken` 再重建。

---

## 五、从源码构建

需要 Visual Studio 2022 Build Tools（MSVC 14.4x）和 Windows SDK 10。

```bat
build.bat
```

产物在 `build\screenshot-translator.exe`。

### 代码结构

| 文件 | 职责 |
|---|---|
| `src/main.c` | 入口、隐藏消息窗口、翻译线程调度 |
| `src/capture.c` | 全屏抓取、遮罩窗口、框选、淡入 |
| `src/overlay.c` | 译文浮层窗口、鼠标交互、剪贴板 |
| `src/overlay_gdiplus.cpp` | 唯一的 C++ 单元：GDI+ 文字排版与绘制 |
| `src/settings.c` | 设置窗口（原生控件 + 深色主题） |
| `src/translator.c` | 请求构造、响应解析 |
| `src/http.c` | WinHTTP 封装 |
| `src/hotkey.c` | 全局热键解析与注册 |
| `src/config.c` | config.json 读写 |
| `src/image.c` | DIB 缓冲、缩放、JPEG 编码 |
| `src/tray.c` | 托盘图标与菜单 |
| `src/base64.c` | Base64 编码 |

第三方只有两个单文件库：`third_party/cJSON.c`、`third_party/stb_image_write.h`。

---

## 六、常见问题

**双击后什么都没发生？**
正常。程序是托盘程序，看任务栏右下角的通知区域，可能要点开「显示隐藏的图标」。

**按快捷键没反应？**
先确认它没被别的程序占用：打开设置窗口，把热键重新保存一次，被占用会当场提示。某些游戏、录屏软件会独占全局热键。

**翻译报「API Key 无效」？**
检查 Key 有没有多余的空格，以及接口地址是否和 Key 所属的服务商一致。

**图片里的文字没识别出来？**
模型对图片的分辨率有要求。程序会先把选区缩放到长边 1568 再发出去；如果原文本身太小（比如整屏截图里的一个小角落），识别率会下降。

**配置文件不见了？**
程序会把配置写在 exe 同目录。如果 exe 放在 Program Files 这类只读目录，会退到 `%LOCALAPPDATA%\ScreenshotTranslatorC`。

---

## 许可

[MIT](LICENSE)
