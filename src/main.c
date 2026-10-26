/* main.c —— 入口、隐藏消息窗口、消息循环 */
#include "app.h"
#include "capture.h"
#include "config.h"
#include "image.h"
#include "hotkey.h"
#include "overlay.h"
#include "overlay_gdiplus.h"
#include "resource.h"
#include "settings.h"
#include "tray.h"
#include "translator.h"
#include "util.h"

#include <string.h>
#include <wchar.h>

AppContext g_app;

/* 自测模式：显示一次浮层，关闭后直接退出进程 */
static BOOL g_overlay_test = FALSE;

/* 自测用的固定文本：只在命令行带 --overlay-test / --overlay-dump 时使用，
   正常运行时不会出现，避免和真实译文混淆。 */
/* 自测用的固定文本：只在命令行带 --overlay-test / --overlay-dump 时使用。
   故意写成一眼能看出是测试数据的样子，避免和真实译文混淆。 */
static const wchar_t *OVERLAY_TEST_CHINESE = L"[SELFTEST-中文] 测试 ABC 123";
static const wchar_t *OVERLAY_TEST_ENGLISH = L"[SELFTEST-EN] sample ABC 123";
static const wchar_t *OVERLAY_TEST_MIXED   = L"[SELFTEST-MIX] 中英 Mixed 123";
static const wchar_t *OVERLAY_TEST_EMOJI   = L"[SELFTEST-EMOJI] 😀🎉";

static const wchar_t *OVERLAY_BUSY_TEXT = L"正在识别并翻译…";

/* ---------- 翻译线程 ---------- */

typedef struct TranslateJob {
    HWND        notify;
    Config      cfg;          /* 复制一份，避免和主线程共享 */
    void       *png;
    size_t      png_size;
} TranslateJob;

static DWORD WINAPI translate_thread(LPVOID param)
{
    TranslateJob *job = (TranslateJob *)param;

    TranslationResult *result = (TranslationResult *)heap_alloc(sizeof(*result));
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        translator_run_image(&job->cfg, job->png, job->png_size, result);
    }

    HWND notify = job->notify;
    heap_free(job->png);
    heap_free(job);

    PostMessageW(notify, WM_APP_TRANSLATED, 0, (LPARAM)result);
    return 0;
}

static void start_translation(HWND hwnd, const ImageBuffer *image)
{
    if (g_app.translating) {
        LOG_INFO("main", L"上一次翻译还没结束，忽略本次请求");
        return;
    }

    void *data = NULL;
    size_t data_size = 0;
    if (!translator_prepare_image(image, &data, &data_size)) {
        overlay_show(hwnd, L"图片处理失败", &g_app.cfg);
        return;
    }

    TranslateJob *job = (TranslateJob *)heap_alloc(sizeof(*job));
    if (job == NULL) {
        heap_free(data);
        overlay_show(hwnd, L"内存不足，无法翻译", &g_app.cfg);
        return;
    }

    job->notify = hwnd;
    job->cfg = g_app.cfg;
    job->png = data;
    job->png_size = data_size;

    g_app.translating = TRUE;
    g_app.worker = CreateThread(NULL, 0, translate_thread, job, 0, NULL);
    if (g_app.worker == NULL) {
        LOG_ERR("main", L"创建翻译线程失败（错误 %lu）", GetLastError());
        g_app.translating = FALSE;
        heap_free(job->png);
        heap_free(job);
        overlay_show(hwnd, L"无法启动翻译线程", &g_app.cfg);
        return;
    }

    LOG_INFO("main", L"翻译线程已启动（图片 %zu 字节）", data_size);
}

static LRESULT CALLBACK msg_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

/* ---------- 退出清理 ---------- */

static void shutdown_app(HWND hwnd)
{
    if (g_app.quitting) return;
    g_app.quitting = TRUE;

    LOG_INFO("main", L"退出程序");

    overlay_hide();
    hotkey_unregister_all(hwnd);
    tray_remove();

    /* 翻译线程还在跑就先等它结束。窗口此时还没销毁，它 Post 的
       WM_APP_TRANSLATED 会被丢弃（消息循环已经退出），但那不影响退出。
       等不到也不能无限挂着，5 秒后强退。 */
    if (g_app.worker != NULL) {
        DWORD wait = WaitForSingleObject(g_app.worker, 5000);
        if (wait != WAIT_OBJECT_0) {
            LOG_WARN("main", L"翻译线程未在 5 秒内结束，强制退出");
        }
        CloseHandle(g_app.worker);
        g_app.worker = NULL;
    }

    if (g_app.mutex) {
        CloseHandle(g_app.mutex);
        g_app.mutex = NULL;
    }

    if (hwnd) DestroyWindow(hwnd);

    overlay_gdiplus_shutdown();
    log_close();
}

/* ---------- 窗口过程 ---------- */

static LRESULT CALLBACK msg_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    /* Explorer 重启后会广播 TaskbarCreated，收到就重新添加托盘图标 */
    UINT taskbar_created = tray_taskbar_created_message();
    if (taskbar_created != 0 && msg == taskbar_created) {
        tray_re_add();
        return 0;
    }

    switch (msg) {
    case WM_HOTKEY:
        if (wparam == HOTKEY_ID_CAPTURE) {
            LOG_INFO("main", L"热键触发：截图翻译");
            PostMessageW(hwnd, WM_APP_CAPTURE, 0, 0);
        } else if (wparam == HOTKEY_ID_EXIT) {
            LOG_INFO("main", L"热键触发：退出");
            PostMessageW(hwnd, WM_APP_QUIT, 0, 0);
        }
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lparam)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_show_menu(hwnd);
            break;
        case WM_LBUTTONDBLCLK:
            PostMessageW(hwnd, WM_COMMAND, IDM_SETTINGS, 0);
            break;
        default:
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDM_SETTINGS: {
            LOG_INFO("main", L"打开设置窗口");
            if (settings_show(g_app.hwnd, &g_app.cfg)) {
                /* 热键已由设置窗口按新配置注册好了，这里只刷新托盘提示 */
                tray_update_tip(g_app.cfg.hotkey);
                /* 透明度/字号等外观项立即生效：浮层开着也要重绘 */
                overlay_refresh_theme(&g_app.cfg);
                LOG_INFO("main", L"配置已应用：hotkey=%ls model=%ls",
                         g_app.cfg.hotkey, g_app.cfg.model);
            }
            break;
        }
        case IDM_CAPTURE:
            PostMessageW(hwnd, WM_APP_CAPTURE, 0, 0);
            break;
        case IDM_EXIT:
            PostMessageW(hwnd, WM_APP_QUIT, 0, 0);
            break;
        default:
            break;
        }
        return 0;

    case WM_APP_CAPTURE: {
        Selection sel;
        ImageBuffer shot;
        if (capture_select(hwnd, &g_app.cfg, &sel, &shot)) {
            LOG_INFO("main", L"选区（虚拟屏坐标）：left=%d top=%d w=%d h=%d",
                     sel.left, sel.top, sel.width, sel.height);

            /* 用"鼠标按下的那个点"当锚：译文出现在按下位置的右下方，
               比贴在选区边缘更符合直觉。 */
            {
                POINT pt;
                capture_last_anchor(&pt);

                RECT anchor;
                anchor.left   = pt.x;
                anchor.top    = pt.y;
                anchor.right  = pt.x;
                anchor.bottom = pt.y;
                overlay_set_anchor(&anchor);
            }

            /* 先把浮层立起来当"正在翻译"提示，结果回来再换文字。
               先显示再发请求，用户能立刻看到反馈。 */
            overlay_show(hwnd, OVERLAY_BUSY_TEXT, &g_app.cfg);
            start_translation(hwnd, &shot);
            image_free(&shot);
        } else {
            LOG_INFO("main", L"截图被取消");
        }
        return 0;
    }

    case WM_APP_TRANSLATED: {
        TranslationResult *result = (TranslationResult *)lparam;

        g_app.translating = FALSE;
        if (g_app.worker != NULL) {
            CloseHandle(g_app.worker);
            g_app.worker = NULL;
        }

        if (result == NULL) {
            overlay_show(hwnd, L"翻译失败：内部错误", &g_app.cfg);
            return 0;
        }

        if (result->error != NULL) {
            LOG_ERR("main", L"翻译失败：%ls", result->error);
            overlay_show(hwnd, result->error, &g_app.cfg);
        } else if (result->translated != NULL && result->translated[0] != 0) {
            /* 同时带上原文，文档按钮可以在两者之间切换 */
            overlay_show_pair(hwnd, result->original, result->translated,
                              &g_app.cfg);
        } else {
            overlay_show(hwnd, L"图片里没有识别到文字", &g_app.cfg);
        }

        translation_result_free(result);
        heap_free(result);
        return 0;
    }

    case WM_APP_OVERLAY_CLOSED:
        LOG_INFO("main", L"浮层已关闭");
        if (g_overlay_test) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;

    case WM_APP_QUIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_CLOSE:
        shutdown_app(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ---------- 入口 ---------- */

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance,
                    PWSTR command_line, int show_command)
{
    (void)prev_instance;
    (void)show_command;

    /* 隐藏的自测参数：--auto-select x0,y0,x1,y1
       用 PostMessage 注入鼠标事件，不移动真实光标，方便验证坐标换算。 */
    BOOL auto_select = FALSE;
    BOOL auto_cancel = FALSE;
    const wchar_t *overlay_test_text = NULL;
    int ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;

    BOOL overlay_dump = FALSE;
    if (command_line != NULL && wcsstr(command_line, L"--overlay-dump") != NULL) {
        overlay_dump = TRUE;
    }

    if (overlay_dump ||
        (command_line != NULL && wcsstr(command_line, L"--overlay-test") != NULL)) {
        g_overlay_test = TRUE;
        overlay_test_text = OVERLAY_TEST_MIXED;
        if (command_line != NULL && wcsstr(command_line, L"chinese") != NULL) {
            overlay_test_text = OVERLAY_TEST_CHINESE;
        } else if (command_line != NULL && wcsstr(command_line, L"english") != NULL) {
            overlay_test_text = OVERLAY_TEST_ENGLISH;
        } else if (command_line != NULL && wcsstr(command_line, L"emoji") != NULL) {
            overlay_test_text = OVERLAY_TEST_EMOJI;
        }
    }

    if (command_line != NULL && wcsstr(command_line, L"--auto-cancel") != NULL) {
        auto_cancel = TRUE;
    } else if (command_line != NULL && wcsstr(command_line, L"--auto-select") != NULL) {
        const wchar_t *p = wcsstr(command_line, L"--auto-select") + 13;
        while (*p == L' ') p++;
        if (swscanf_s(p, L"%d,%d,%d,%d", &ax0, &ay0, &ax1, &ay1) == 4) {
            auto_select = TRUE;
        }
    }

    /* DPI 感知必须在任何窗口创建之前 */
    set_dpi_awareness();

    /* 注册工具栏等公共控件类（设置窗口的滑块要用） */
    settings_init();

    /* 单实例：第二个实例直接安静退出 */
    g_app.mutex = acquire_single_instance();
    if (g_app.mutex == NULL) return 0;

    g_app.instance = instance;

    if (!get_exe_dir(g_app.base_dir, MAX_PATH)) {
        wstr_copy(g_app.base_dir, MAX_PATH, L".");
    }
    /* 如果 exe 所在目录不可写，退到 %LOCALAPPDATA% */
    {
        wchar_t probe[MAX_PATH];
        path_join(probe, MAX_PATH, g_app.base_dir, L".write_test");
        HANDLE f = CreateFileW(probe, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
            DeleteFileW(probe);
        } else {
            const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
            if (local && local[0]) {
                wchar_t fallback[MAX_PATH];
                path_join(fallback, MAX_PATH, local, L"ScreenshotTranslatorC");
                if (ensure_dir(fallback)) {
                    wstr_copy(g_app.base_dir, MAX_PATH, fallback);
                    LOG_WARN("main", L"exe 目录不可写，配置与日志改存到 %ls", fallback);
                }
            }
        }
    }

    path_join(g_app.config_path, MAX_PATH, g_app.base_dir, L"config.json");
    path_join(g_app.log_path, MAX_PATH, g_app.base_dir, L"screenshot-translator.log");

    log_init(g_app.log_path);
    LOG_INFO("main", L"============================================================");
    LOG_INFO("main", L"%ls %ls 启动，日志：%ls", APP_NAME, APP_VERSION, g_app.log_path);

    /* 让系统标准控件按深色绘制。必须在创建任何窗口之前，
       放在日志初始化之后才能记下失败原因。 */
    dark_mode_init();

    /* GDI+ 在入口初始化一次，退出前 Shutdown。
       不要放到每个绘制函数里反复 Startup/Shutdown。 */
    if (!overlay_gdiplus_startup()) {
        LOG_WARN("main", L"GDI+ 初始化失败，译文浮层将无法显示");
    }

    ConfigLoadStatus load_status = config_load(&g_app.cfg, g_app.config_path);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = msg_wnd_proc;
    wc.hInstance = instance;
    wc.lpszClassName = APP_CLASS;
    wc.hIcon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP),
                                 IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

    if (!RegisterClassExW(&wc)) {
        LOG_ERR("main", L"注册窗口类失败（错误 %lu）", GetLastError());
        log_close();
        CloseHandle(g_app.mutex);
        return 1;
    }

    /* 注意：不能用 HWND_MESSAGE —— 那种消息窗口收不到 TaskbarCreated 广播。
       这里用普通窗口，但不加 WS_VISIBLE，所以永远不会显示。 */
    g_app.hwnd = CreateWindowExW(0, APP_CLASS, APP_NAME, WS_OVERLAPPED,
                                 0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (g_app.hwnd == NULL) {
        LOG_ERR("main", L"创建消息窗口失败（错误 %lu）", GetLastError());
        log_close();
        CloseHandle(g_app.mutex);
        return 1;
    }

    hotkey_register_all(g_app.hwnd, &g_app.cfg);
    tray_init(g_app.hwnd, &g_app.cfg);

    /* 配置损坏时提示用户，而不是静默用默认值 —— 否则用户会以为设置丢了 */
    if (load_status == CONFIG_LOAD_CORRUPTED ||
        load_status == CONFIG_LOAD_UNREADABLE) {
        wchar_t message[512];
        _snwprintf_s(message, 512, _TRUNCATE,
                     L"配置文件读取失败，本次已使用默认设置。\n\n%ls\n\n%ls",
                     g_app.config_path,
                     load_status == CONFIG_LOAD_CORRUPTED
                         ? L"原文件已备份为 config.json.broken，可在设置窗口重新填写。"
                         : L"请检查文件是否被其他程序占用。");
        MessageBoxW(g_app.hwnd, message, L"截图翻译",
                    MB_OK | MB_ICONWARNING);
    }

    if (overlay_dump && overlay_test_text != NULL) {
        /* 离屏渲染四种文本，各存一张 PNG，然后直接退出 */
        const wchar_t *names[4] = { L"overlay-chinese.png", L"overlay-english.png",
                                    L"overlay-mixed.png",   L"overlay-emoji.png" };
        const wchar_t *texts[4] = { OVERLAY_TEST_CHINESE, OVERLAY_TEST_ENGLISH,
                                    OVERLAY_TEST_MIXED,   OVERLAY_TEST_EMOJI };

        for (int i = 0; i < 4; i++) {
            wchar_t path[MAX_PATH];
            path_join(path, MAX_PATH, g_app.base_dir, names[i]);
            if (!overlay_debug_dump(texts[i], &g_app.cfg, path)) {
                LOG_ERR("main", L"离屏渲染失败：%ls", names[i]);
            }
        }
        PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
    } else if (g_overlay_test && overlay_test_text != NULL) {
        overlay_set_auto_close(90000);
        LOG_INFO("main", L"自动测试：显示译文浮层（90 秒后自动关闭）");
        overlay_show(g_app.hwnd, overlay_test_text, &g_app.cfg);
    }

    if (auto_cancel) {
        capture_set_auto_cancel(TRUE);
        LOG_INFO("main", L"自动测试：ESC 取消");
        PostMessageW(g_app.hwnd, WM_APP_CAPTURE, 0, 0);
    } else if (auto_select) {
        capture_set_auto_select(TRUE, ax0, ay0, ax1, ay1);
        LOG_INFO("main", L"自动测试：(%d,%d) -> (%d,%d)", ax0, ay0, ax1, ay1);
        PostMessageW(g_app.hwnd, WM_APP_CAPTURE, 0, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* 正常走 WM_CLOSE 时已经清理过，这里兜底 */
    if (!g_app.quitting) {
        hotkey_unregister_all(g_app.hwnd);
        tray_remove();
        if (g_app.mutex) { CloseHandle(g_app.mutex); g_app.mutex = NULL; }
        LOG_INFO("main", L"程序已退出");
        overlay_gdiplus_shutdown();
        log_close();
    }

    return 0;
}
