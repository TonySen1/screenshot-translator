# Screenshot Translator

[![build](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml/badge.svg)](https://github.com/TonySen1/screenshot-translator/actions/workflows/build.yml)

[中文说明](README.md) · **English**

Press a hotkey, drag a box around anything on your screen, and the Chinese translation appears floating right where you selected it.

No main window, no login, no ads, no history. After launch there is nothing on your desktop except a small tray icon — it only shows itself the moment you press the hotkey.

> **Note:** the OCR step asks the model to translate into Chinese (`target_language` in `config.json`). Point it at any language you like; the rest of the tool is language-agnostic.

## Screenshots

The whole screen dims while you drag; the selected region keeps its original brightness:

![Selection mask](docs/screenshot-mask.png)

Release the mouse and the translation floats right over the selection. The three icons in the bottom-right corner are copy-original, copy-translation, and close:

![Translation overlay](docs/screenshot-overlay.png)

Everything is configured from the tray icon's settings window — just drag the sliders:

![Settings window](docs/screenshot-settings.png)

---

## Quick start (prebuilt exe)

1. Grab `截图翻译.exe` from the [Releases](../../releases) page and put it in **a folder you created**, e.g. `D:\ScreenshotTranslator\`.
   Avoid the Desktop or `C:\Program Files` — Windows may block the app from writing its config there.
2. **Run it once**, then quit it. It will create a `config.json` next to itself.
3. Open `config.json` in Notepad and fill in your API key:

   ```json
   "api_key": "sk-your-key-here",
   ```
   Also check `api_base_url` and `model` (see [Configuration](#configuration) for examples).
4. Save, then run `截图翻译.exe` again. This time it just sits quietly in the background.
5. Press **Ctrl + Alt + D**, drag a box around the text you want translated, release the mouse.
6. A dark translucent panel appears at the selection with the translation. Press **Esc** when you're done.
7. To quit for good, press **Ctrl + Alt + Shift + Q**.

> Nothing happening after you double-click the exe is **expected** — it has no window.
> Just press the hotkey. The tray icon sits in the notification area (you may need to click the `^` arrow to see it).

### Hotkeys

| Key | Action |
| --- | --- |
| `Ctrl + Alt + D` | Start screenshot translation |
| `Ctrl + Alt + Shift + Q` | Quit |
| `Esc` | Close the translation panel; during selection, cancel the capture |
| Right-click on the panel | Close the translation panel |
| Right-click on the mask | Cancel the capture |
| Drag the panel | Move it somewhere else |

### The translation panel

It is a small dark translucent card with rounded corners whose **size fits the translated text itself**, not the size of the box you dragged:

- Short translation → small card, tucked at the top-left of your selection
- Long translation → grows wider first, then wraps and grows taller
- Very long text → font size shrinks automatically so everything stays visible — no scrolling, never full screen
- Only the translation is shown; the original text is available via the copy icon

**Hold anywhere on the panel to drag it elsewhere.** Once you've moved it, it stays put (later text changes won't snap it back).

### The three icons

| Icon | Tooltip | Action |
| --- | --- | --- |
| 📄 a sheet | Copy text | Copy the recognized original text to the clipboard |
| ⧉ two squares | Copy translation | Copy the translation to the clipboard |
| ✕ cross (white) | Close | Close the panel |

Clicking the first two makes the icon flash to confirm the copy — no popups, ever. Hovering brightens an icon; hovering for ~0.4s shows a tooltip *outside* the panel so it never covers the translation. `Esc`, right-click, or the ✕ all close it.

### Mask fade

The screen doesn't slam to black — it darkens over ~0.13s with an ease-out curve, and brightens back over ~0.05s when you release or cancel. This keeps repeated captures from feeling flickery.

### Tray icon

A small icon appears in the notification area (click the `^` arrow if it's hidden):

- **Double-click** → open settings
- **Right-click** → menu: Open settings / Screenshot translate / Quit

### Settings window

Everything here is written back to `config.json` and applies **immediately** — no restart needed:

| Section | What you can change |
| --- | --- |
| Appearance | Mask opacity, panel opacity, font size (sliders) |
| API | Base URL, API key, model, target language |
| Hotkeys | Capture hotkey, quit hotkey |

This window and the `config.json` file are equivalent — editing either one is fine.

---

## Running from source

Requires **Python 3.10+** (3.11 or newer recommended). Get it from <https://www.python.org/downloads/> and
**tick "Add python.exe to PATH"** during installation.

In this folder, open a terminal (type `cmd` in the File Explorer address bar) and run:

```bat
pip install -r requirements.txt
python main.py
```

Again, there's no window — press `Ctrl + Alt + D` to use it.

Using a virtual environment (recommended, keeps your system Python clean):

```bat
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\python main.py
```

---

## Building the exe yourself

Double-click **`build.bat`**. It installs PyInstaller if needed and produces:

```text
dist\截图翻译.exe
```

Building takes 1–3 minutes. Or do it manually:

```bat
pip install pyinstaller
pyinstaller --noconfirm --clean --onefile --noconsole --name 截图翻译 --icon assets\icon.ico main.py
```

The exe creates `config.json` next to itself on first run, so you can change settings without rebuilding.

> If you're on Python 3.10.0, the build will fail — that specific early 3.10 release breaks PyInstaller's
> bytecode analyzer. Use 3.10.11+ or 3.11+. `build.py` checks this and tells you.

---

## Configuration

`config.json` must sit next to the exe. If it doesn't exist, the program creates one with defaults.

| Key | Default | Meaning |
| --- | --- | --- |
| `api_base_url` | `https://api.openai.com/v1` | API endpoint, usually ending in `/v1` |
| `api_key` | *(empty)* | **Required.** If empty, the panel tells you to fill it in |
| `model` | `gpt-4o-mini` | Vision-capable model name |
| `hotkey` | `ctrl+alt+d` | Capture hotkey |
| `exit_hotkey` | `ctrl+alt+shift+q` | Quit hotkey |
| `target_language` | `中文` | Language to translate into |
| `mask_opacity` | `0.35` | Mask brightness; lower = darker screen |
| `overlay_opacity` | `0.88` | Panel opacity; raise to `0.95` for a more solid look |
| `overlay_bg` | `#1e1e1e` | Panel background |
| `overlay_fg` | `#ffffff` | Panel text color |
| `font_size` | `12` | Panel font size (points) |
| `request_timeout` | `30` | API timeout in seconds |

Bad values never crash the program — it falls back to the default and notes it in the log.

### Using a different provider

Any OpenAI-compatible `/chat/completions` endpoint works. Some examples:

```jsonc
// OpenAI
{ "api_base_url": "https://api.openai.com/v1", "model": "gpt-4o-mini" }

// DeepSeek (must use a vision-capable tier; text-only tiers reject images)
{ "api_base_url": "https://api.deepseek.com/v1", "model": "deepseek-flash" }

// Alibaba Qwen (DashScope)
{ "api_base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1", "model": "qwen-vl-max" }

// Zhipu GLM
{ "api_base_url": "https://open.bigmodel.cn/api/paas/v4", "model": "glm-4v-plus" }

// Local Ollama (pull a vision model first)
{ "api_base_url": "http://127.0.0.1:11434/v1", "api_key": "ollama", "model": "llava" }
```

The model **must be able to see images**:

| Provider | Model | Note |
| --- | --- | --- |
| DeepSeek | `deepseek-flash` | Vision tier added Aug 2026. `deepseek-reasoner` etc. do **not** accept images |
| OpenAI | `gpt-4o-mini` | |
| Alibaba | `qwen-vl-max` | |
| Zhipu | `glm-4v-plus` | |
| Local | `llava` / `qwen2-vl` | Via Ollama, fully offline |

---

## Verifying it works

Go through this list; each step should behave as described in parentheses.

1. **Background** — double-click the exe. No window appears; a small icon shows up in the notification area. (Correct)
2. **Tray menu** — right-click the icon → "Open settings / Screenshot translate / Quit". (Correct)
3. **Settings** — click "Open settings". A dark window appears with sliders and input fields. (Correct)
4. **Capture** — press `Ctrl + Alt + D`. The screen dims smoothly and the cursor becomes a crosshair. (Correct)
5. **Drag** — hold the left button and drag a rectangle. The selected area keeps its original brightness. (Correct)
6. **Release** — the dimmed mask fades away. (Correct)
7. **Translation** — a dark translucent panel appears at the selection, showing "翻译中…" first, then the Chinese translation after a second or two. (Correct)
8. **Copy original** — click the first icon (a sheet). Paste elsewhere with `Ctrl + V`; you should get the original text. (Correct)
9. **Copy translation** — click the second icon (two squares); you get the Chinese translation. (Correct)
10. **Drag the panel** — hold the text and move it. It follows the cursor and stays where you drop it. (Correct)
11. **Close** — press `Esc`, or click the third icon (✕). The panel disappears but the app keeps running. (Correct)
12. **Repeat** — press `Ctrl + Alt + D` again; everything works again. (Correct)
13. **Quit** — press `Ctrl + Alt + Shift + Q`, or tray → Quit. The process ends and the tray icon disappears. (Correct)

### Troubleshooting

The app has no console, so everything goes into a log file next to the exe:

```text
screenshot-translator.log
```

It records startup, hotkey registration, every translation, and full stack traces.

| Symptom | Cause |
| --- | --- |
| Panel says "请先在 config.json 中填写 api_key" | `api_key` is empty |
| Panel says "API Key 无效" | Wrong key, or a stray space when pasting |
| Panel says "模型名或 api_base_url 不正确" | Model name typo, or the URL is missing `/v1` |
| Panel says "网络连接失败" | No network, wrong URL, or you need a proxy for that provider |
| Panel says "请求超时" | Slow network — increase `request_timeout` |
| Hotkey does nothing | Another app grabbed it (dictionary tools, QQ, screenshot utilities all love `Ctrl+Alt+D`). Change `hotkey` in `config.json` and restart |
| Won't start / nothing happens | Check the log. Or: an instance is already running — only one is allowed at a time |

---

## How it works

```text
screenshot-translator\
├─ main.py             Entry point: global hotkeys, event loop, quit
├─ config.py           Load, validate, and save config.json
├─ screenshot.py       Capture mask, mouse selection, crop
├─ translator.py       Multimodal API call, JSON parsing, friendly errors
├─ overlay.py          Translation panel: text, three icon buttons, dragging
├─ settings_window.py  Settings window (dark, borderless, custom sliders)
├─ tray.py             Tray icon and context menu
├─ icons.py            Icon drawing (panel buttons + tray icon)
├─ winutil.py          Windows helpers (top-level HWND, Win11 rounded corners)
├─ config.json         Configuration
├─ requirements.txt    Dependencies
├─ build.bat           One-click build
├─ build.py            Build script
├─ assets\icon.ico     App icon
└─ dev\                Self-test scripts (not needed for normal use)
```

The flow:

1. `keyboard` listens for the global hotkey in the background. Its callback only pushes an event into a queue — Tk is not thread-safe, so no UI work happens there.
2. The main thread picks up the event and grabs the entire virtual screen (all monitors) with `mss`.
3. A borderless window covers the whole virtual screen showing that screenshot **darkened** — which is why the screen looks frozen. The selected region restores its original brightness.
4. On release, the selection is cropped, encoded to PNG, then base64, and sent to an OpenAI-compatible endpoint.
5. The request runs in a worker thread; the JSON response is parsed into original/translated text and handed back to the main thread to update the panel.
6. The panel sizes itself to fit the text and can be dragged. Closing it leaves the app waiting for the next hotkey.
7. The tray icon runs in `pystray`'s own thread and its menu callbacks also just push events. Saving settings re-registers the hotkeys immediately.

---

## What this deliberately doesn't do

To keep it from turning into bloat:

- No main window (the desktop stays empty apart from a tray icon)
- No login, accounts, subscriptions, ads, telemetry, or auto-update
- No history, no engine picker list
- No "Copied!" popups
- No extra elements on the panel — just the translation and three icons

---

## License

[MIT](LICENSE) — use it, modify it, ship it commercially. Just keep the copyright notice; no warranty.
