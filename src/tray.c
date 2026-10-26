/* tray.c —— 托盘图标、右键菜单、Explorer 重启恢复 */
#include "tray.h"
#include "app.h"
#include "resource.h"
#include "util.h"

#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static NOTIFYICONDATAW g_nid;
static HWND             g_hwnd = NULL;
static HICON            g_icon = NULL;
static UINT             g_taskbar_created = 0;

static BOOL fill_nid(void)
{
    if (g_hwnd == NULL) return FALSE;

    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    return TRUE;
}

UINT tray_taskbar_created_message(void)
{
    if (g_taskbar_created == 0) {
        g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    }
    return g_taskbar_created;
}

BOOL tray_init(HWND hwnd, const Config *cfg)
{
    if (hwnd == NULL) return FALSE;
    g_hwnd = hwnd;

    if (g_icon == NULL) {
        g_icon = (HICON)LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_APP),
                                   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if (g_icon == NULL) {
            /* 资源缺失时用一个系统图标兜底，总比没有强 */
            g_icon = LoadIconW(NULL, IDI_APPLICATION);
            LOG_WARN("tray", L"加载应用图标失败，改用系统默认图标");
        }
    }

    if (!fill_nid()) return FALSE;

    wchar_t tip[128];
    _snwprintf_s(tip, 128, _TRUNCATE, L"%s（%ls）", APP_NAME,
                 cfg ? cfg->hotkey : L"ctrl+alt+d");
    wstr_copy(g_nid.szTip, 128, tip);

    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        LOG_ERR("tray", L"添加托盘图标失败（错误 %lu）", GetLastError());
        return FALSE;
    }

    g_app.tray_added = TRUE;
    LOG_INFO("tray", L"托盘图标已启动");
    return TRUE;
}

void tray_re_add(void)
{
    if (g_hwnd == NULL) return;
    if (!fill_nid()) return;

    /* 图标丢失后 NIM_ADD 可能仍返回成功，所以先删再加 */
    Shell_NotifyIconW(NIM_DELETE, &g_nid);

    wchar_t tip[128];
    _snwprintf_s(tip, 128, _TRUNCATE, L"%s（%ls）", APP_NAME,
                 g_app.cfg.hotkey[0] ? g_app.cfg.hotkey : L"ctrl+alt+d");
    wstr_copy(g_nid.szTip, 128, tip);

    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_app.tray_added = TRUE;
        LOG_INFO("tray", L"Explorer 重启，托盘图标已重新添加");
    } else {
        LOG_ERR("tray", L"重新添加托盘图标失败");
    }
}

void tray_update_tip(const wchar_t *hotkey)
{
    if (g_hwnd == NULL) return;
    if (!fill_nid()) return;

    wchar_t tip[128];
    _snwprintf_s(tip, 128, _TRUNCATE, L"%s（%ls）", APP_NAME,
                 (hotkey && hotkey[0]) ? hotkey : L"ctrl+alt+d");
    wstr_copy(g_nid.szTip, 128, tip);

    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void tray_remove(void)
{
    if (g_hwnd == NULL || !g_app.tray_added) return;

    if (!fill_nid()) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_app.tray_added = FALSE;
    LOG_INFO("tray", L"托盘图标已移除");

    /* 图标句柄是 LoadImage 拿到的，需要释放；系统图标不能 DestroyIcon */
    if (g_icon != NULL) {
        HICON system_icon = LoadIconW(NULL, IDI_APPLICATION);
        if (g_icon != system_icon) DestroyIcon(g_icon);
        g_icon = NULL;
    }
}

void tray_show_menu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (menu == NULL) return;

    /* MNS_NOCHECK 去掉左侧那条留给勾选标记的空白栏。
       这几项都没有勾选状态，留着它只会让菜单白白宽出 ~28px
       （实测 146px -> 118px）。 */
    MENUINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_NOCHECK;
    SetMenuInfo(menu, &mi);

    AppendMenuW(menu, MF_STRING, IDM_CAPTURE,  L"截图翻译");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT,     L"退出");

    /* 微软文档要求：TrackPopupMenu 前先 SetForegroundWindow，
       否则点击菜单外部时菜单不会消失 */
    SetForegroundWindow(hwnd);

    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);

    /* 文档同样要求：菜单结束后补一条空消息 */
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}
