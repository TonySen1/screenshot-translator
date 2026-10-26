/* util.c —— UTF 转换、日志、路径、DPI、单实例 */
#include "util.h"
#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ================= 内存 ================= */

void *heap_alloc(size_t bytes)
{
    if (bytes == 0) bytes = 1;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
}

void *heap_realloc(void *ptr, size_t bytes)
{
    if (ptr == NULL) return heap_alloc(bytes);
    if (bytes == 0) { HeapFree(GetProcessHeap(), 0, ptr); return NULL; }
    return HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ptr, bytes);
}

void heap_free(void *ptr)
{
    if (ptr) HeapFree(GetProcessHeap(), 0, ptr);
}

/* ================= UTF ================= */

wchar_t *u8_to_u16(const char *src)
{
    if (src == NULL) return NULL;

    int need = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (need <= 0) return NULL;

    wchar_t *out = (wchar_t *)heap_alloc((size_t)need * sizeof(wchar_t));
    if (out == NULL) return NULL;

    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, out, need) <= 0) {
        heap_free(out);
        return NULL;
    }
    return out;
}

char *u16_to_u8(const wchar_t *src)
{
    if (src == NULL) return NULL;

    int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return NULL;

    char *out = (char *)heap_alloc((size_t)need);
    if (out == NULL) return NULL;

    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, out, need, NULL, NULL) <= 0) {
        heap_free(out);
        return NULL;
    }
    return out;
}

void wstr_copy(wchar_t *dst, size_t dst_count, const wchar_t *src)
{
    if (dst == NULL || dst_count == 0) return;
    if (src == NULL) { dst[0] = 0; return; }
    wcsncpy_s(dst, dst_count, src, _TRUNCATE);
}

BOOL wstr_ieq(const wchar_t *a, const wchar_t *b)
{
    if (a == NULL || b == NULL) return a == b;
    return _wcsicmp(a, b) == 0;
}

/* ================= 日志 ================= */

static CRITICAL_SECTION g_log_lock;
static HANDLE           g_log_file = INVALID_HANDLE_VALUE;
static BOOL             g_log_lock_ready = FALSE;

BOOL log_init(const wchar_t *path)
{
    if (!g_log_lock_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_ready = TRUE;
    }
    if (g_log_file != INVALID_HANDLE_VALUE) return TRUE;

    if (path == NULL || path[0] == 0) return FALSE;

    g_log_file = CreateFileW(path, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return g_log_file != INVALID_HANDLE_VALUE;
}

void log_close(void)
{
    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
}

void log_msg_wide(const char *level, const char *module, const wchar_t *fmt, ...)
{
    if (g_log_file == INVALID_HANDLE_VALUE) return;

    wchar_t wmessage[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(wmessage, 2048, _TRUNCATE, fmt, args);
    va_end(args);

    char *message = u16_to_u8(wmessage);
    if (message == NULL) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[2400];
    int n = snprintf(line, sizeof(line),
                     "%04d-%02d-%02d %02d:%02d:%02d [%s] %s: %s\r\n",
                     st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond,
                     level, module ? module : "-", message);
    heap_free(message);
    if (n <= 0) return;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    EnterCriticalSection(&g_log_lock);
    DWORD written = 0;
    WriteFile(g_log_file, line, (DWORD)n, &written, NULL);
    LeaveCriticalSection(&g_log_lock);

#ifdef _DEBUG
    OutputDebugStringA(line);
#endif
}

/* ================= 路径 ================= */

BOOL get_exe_dir(wchar_t *out, size_t out_count)
{
    if (out == NULL || out_count == 0) return FALSE;

    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)out_count);
    if (n == 0 || n >= out_count) return FALSE;

    wchar_t *slash = wcsrchr(out, L'\\');
    if (slash == NULL) return FALSE;
    *slash = 0;
    return TRUE;
}

void path_join(wchar_t *out, size_t out_count, const wchar_t *base, const wchar_t *name)
{
    if (out == NULL || out_count == 0) return;
    if (name != NULL && name[0] == L'\\') {
        wstr_copy(out, out_count, name);
        return;
    }
    _snwprintf_s(out, out_count, _TRUNCATE, L"%s\\%s", base, name);
}

BOOL ensure_dir(const wchar_t *path)
{
    if (path == NULL || path[0] == 0) return FALSE;
    if (CreateDirectoryW(path, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* ================= 系统 ================= */

void set_dpi_awareness(void)
{
    /* manifest 里已经声明 permonitorv2；这里只做老系统兜底。
       SetProcessDpiAwarenessContext 需要 Win10 1703+，动态取符号避免硬依赖。 */
    typedef BOOL (WINAPI *PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        PFN_SetCtx fn = (PFN_SetCtx)(void *)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    typedef HRESULT (WINAPI *PFN_SetAware)(int);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        PFN_SetAware fn = (PFN_SetAware)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2 /* PROCESS_PER_MONITOR_DPI_AWARE */);
        FreeLibrary(shcore);
        return;
    }

    SetProcessDPIAware();
}

HANDLE acquire_single_instance(void)
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, APP_MUTEX);
    if (mutex == NULL) return NULL;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return NULL;
    }
    return mutex;
}

UINT get_dpi(HWND hwnd)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        PFN_GetDpiForWindow fn = (PFN_GetDpiForWindow)(void *)GetProcAddress(
            user32, "GetDpiForWindow");
        if (fn && hwnd) {
            UINT dpi = fn(hwnd);
            if (dpi) return dpi;
        }
    }
    HDC screen = GetDC(NULL);
    UINT dpi = screen ? (UINT)GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(NULL, screen);
    return dpi ? dpi : 96;
}

COLORREF parse_color(const wchar_t *text, COLORREF fallback)
{
    if (text == NULL) return fallback;

    while (*text == L' ' || *text == L'\t') text++;
    if (*text != L'#') {
        /* 也接受常见颜色名 */
        if (wstr_ieq(text, L"white")) return RGB(255, 255, 255);
        if (wstr_ieq(text, L"black")) return RGB(0, 0, 0);
        return fallback;
    }
    text++;

    size_t len = wcslen(text);
    if (len != 6 && len != 3) return fallback;

    wchar_t expanded[7];
    if (len == 3) {
        expanded[0] = text[0]; expanded[1] = text[0];
        expanded[2] = text[1]; expanded[3] = text[1];
        expanded[4] = text[2]; expanded[5] = text[2];
        expanded[6] = 0;
    } else {
        wstr_copy(expanded, 7, text);
    }

    unsigned int value = 0;
    for (int i = 0; i < 6; i++) {
        wchar_t c = expanded[i];
        unsigned int digit;
        if (c >= L'0' && c <= L'9') digit = (unsigned int)(c - L'0');
        else if (c >= L'a' && c <= L'f') digit = (unsigned int)(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') digit = (unsigned int)(c - L'A' + 10);
        else return fallback;
        value = (value << 4) | digit;
    }

    return RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

/* ================= 深色模式 =================
 *
 * 让系统标准控件（EDIT / BUTTON / TRACKBAR / LISTBOX）按深色绘制。
 * uxtheme 的 SetPreferredAppMode 只有序号导出，没有名字：
 *   序号 135 = SetPreferredAppMode，参数 2 = ForceDark
 * Win10 1903 起可用；更老的系统上取不到符号，静默退回浅色。
 */

#define DARK_BG     RGB(0x1E, 0x1E, 0x1E)
#define DARK_PANEL  RGB(0x25, 0x25, 0x25)
#define DARK_FIELD  RGB(0x2D, 0x2D, 0x2D)
#define DARK_BORDER RGB(0x3E, 0x3E, 0x3E)
#define DARK_FG     RGB(0xE8, 0xE8, 0xE8)
#define DARK_FG_DIM RGB(0x9A, 0x9A, 0x9A)

static BOOL g_dark_ready = FALSE;

/* uxtheme 的几个未文档化导出，只有序号没有名字 */
typedef int  (WINAPI *PFN_SetPreferredAppMode)(int);
typedef BOOL (WINAPI *PFN_AllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *PFN_FlushMenuThemes)(void);
typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);

static HMODULE                    g_uxtheme = NULL;
static PFN_AllowDarkModeForWindow g_allow_dark_window = NULL;
static PFN_SetWindowTheme         g_set_window_theme = NULL;

void dark_mode_init(void)
{
    g_uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (g_uxtheme == NULL) {
        LOG_INFO("util", L"uxtheme.dll 加载失败，设置窗口保持浅色");
        return;
    }

    PFN_SetPreferredAppMode set_mode = (PFN_SetPreferredAppMode)(void *)
        GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(135));
    if (set_mode == NULL) {
        LOG_INFO("util", L"系统不支持深色控件模式，设置窗口保持浅色");
        return;
    }

    /* 2 = ForceDark：标准控件的主题按深色绘制 */
    set_mode(2);

    /* 逐个窗口授权 + 刷新，否则 EDIT 这类带主题的控件仍会画白底 */
    g_allow_dark_window = (PFN_AllowDarkModeForWindow)(void *)
        GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(133));
    PFN_FlushMenuThemes flush_menu = (PFN_FlushMenuThemes)(void *)
        GetProcAddress(g_uxtheme, MAKEINTRESOURCEA(136));
    if (flush_menu != NULL) flush_menu();

    /* 动态取 SetWindowTheme：静态链接会多一个 uxtheme.lib 依赖，
       而它本来就在系统里，动态取更省事 */
    g_set_window_theme = (PFN_SetWindowTheme)(void *)
        GetProcAddress(g_uxtheme, "SetWindowTheme");

    g_dark_ready = (g_allow_dark_window != NULL);
    if (!g_dark_ready) {
        LOG_INFO("util", L"uxtheme 缺少 AllowDarkModeForWindow，退回浅色");
        return;
    }

    /* 不 FreeLibrary：主题状态与模块绑定，进程存续期间要一直有效 */
    LOG_INFO("util", L"深色控件模式已启用");
}

BOOL dark_mode_is_enabled(void)
{
    return g_dark_ready;
}

/* 递归给子控件也打上深色主题。
   只靠 WM_CTLCOLOR 不够：EDIT 这类带主题的控件会用主题画背景，
   把我们设的颜色盖掉，所以必须让它自己走深色主题。 */
static void apply_to_children(HWND parent)
{
    HWND child = GetWindow(parent, GW_CHILD);
    while (child != NULL) {
        dark_mode_apply_control(child);
        apply_to_children(child);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

void dark_mode_apply_control(HWND control)
{
    if (!g_dark_ready || control == NULL) return;

    wchar_t cls[64];
    GetClassNameW(control, cls, 64);

    /* 只处理会画自己白底的控件：EDIT 和 LISTBOX。
       Button / msctls_trackbar32 保持系统当前外观（用户明确要求别动）。
       这里用"关掉主题"而不是换深色主题名：DarkMode_CFD 在不同版本上
       表现不一致，关掉主题后 WM_CTLCOLOR* 设的颜色才稳定生效。 */
    BOOL is_edit = (_wcsicmp(cls, L"Edit") == 0);
    BOOL is_list = (_wcsicmp(cls, L"ListBox") == 0);

    if (!is_edit && !is_list) return;

    if (g_allow_dark_window != NULL) {
        g_allow_dark_window(control, TRUE);
    }

    if (g_set_window_theme != NULL) {
        g_set_window_theme(control, L"", L"");
    }
}

void dark_mode_apply(HWND hwnd)
{
    if (!g_dark_ready || hwnd == NULL) return;

    if (g_allow_dark_window != NULL) {
        g_allow_dark_window(hwnd, TRUE);
    }
    if (g_set_window_theme != NULL) {
        g_set_window_theme(hwnd, L"DarkMode_Explorer", NULL);
    }

    /* 深色标题栏。Win10 20H1 用属性 19，Win11 用 20，都试一遍。 */
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm != NULL) {
        typedef HRESULT (WINAPI *PFN_SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        PFN_SetAttr set_attr = (PFN_SetAttr)(void *)
            GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (set_attr != NULL) {
            BOOL dark = TRUE;
            if (FAILED(set_attr(hwnd, 20, &dark, sizeof(dark)))) {
                set_attr(hwnd, 19, &dark, sizeof(dark));
            }
        }
        FreeLibrary(dwm);
    }

    apply_to_children(hwnd);
}

COLORREF dark_bg_color(void)     { return DARK_BG; }
COLORREF dark_panel_color(void)  { return DARK_PANEL; }
COLORREF dark_field_color(void)  { return DARK_FIELD; }
COLORREF dark_border_color(void) { return DARK_BORDER; }
COLORREF dark_fg_color(void)     { return DARK_FG; }
COLORREF dark_fg_dim_color(void) { return DARK_FG_DIM; }
