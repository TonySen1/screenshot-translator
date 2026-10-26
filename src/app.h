/* app.h —— 全局上下文与模块间消息定义 */
#ifndef APP_H
#define APP_H

#include <windows.h>
#include "config.h"

/* 自定义消息：一律用 PostMessage 投递到隐藏的消息窗口，由主线程处理 */
#define WM_APP_TRAY            (WM_APP + 1)   /* 托盘图标回调 */
#define WM_APP_CAPTURE         (WM_APP + 2)   /* 请求截图（热键/菜单） */
#define WM_APP_QUIT            (WM_APP + 3)   /* 请求退出 */
#define WM_APP_TRANSLATED      (WM_APP + 4)   /* 工作线程翻译完成，lParam = TranslationResult* */
#define WM_APP_OVERLAY_CLOSED  (WM_APP + 5)   /* 译文浮层已关闭 */

/* 托盘菜单命令 ID */
#define IDM_SETTINGS           1001
#define IDM_CAPTURE            1002
#define IDM_EXIT               1003

/* 托盘图标 ID */
#define TRAY_ICON_ID           1

#define APP_NAME               L"截图翻译"
#define APP_VERSION            L"2.0.0"
#define APP_CLASS              L"ScreenshotTranslatorMsgWnd"
#define APP_MUTEX              L"Local\\ScreenshotTranslatorC_SingleInstance"

/* 全局上下文。所有模块通过指针访问，避免散落全局变量。 */
typedef struct AppContext {
    HINSTANCE   instance;
    HWND        hwnd;                 /* 隐藏的消息窗口，接收热键/托盘/线程回调 */

    Config      cfg;

    wchar_t     exe_path[MAX_PATH];
    wchar_t     base_dir[MAX_PATH];
    wchar_t     config_path[MAX_PATH];
    wchar_t     log_path[MAX_PATH];

    HANDLE      mutex;

    BOOL        tray_added;
    BOOL        quitting;

    HANDLE      worker;               /* 翻译线程；同一时刻只有一个 */
    BOOL        translating;

    /* M1 只做骨架，后续里程碑会往这里加：
       capture/overlay/settings 的状态与窗口句柄 */
} AppContext;

extern AppContext g_app;

#endif /* APP_H */
