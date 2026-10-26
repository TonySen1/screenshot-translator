/* overlay.c —— 译文浮层窗口
 *
 * 窗口本身只有一个空壳：内容是一张 32 位预乘 ARGB 位图，
 * 由 overlay_gdiplus.cpp 绘制，这里通过 UpdateLayeredWindow 送上去。
 *
 * 要点：
 *   - WS_EX_LAYERED + UpdateLayeredWindow（不能用 SetLayeredWindowAttributes，
 *     那样做不出逐像素 alpha，圆角边缘会发黑）
 *   - WS_EX_NOACTIVATE：出现时不抢前台、不切输入法，靠临时注册的 ESC 热键关闭
 *   - 不响应 WM_PAINT 绘制，重绘 = 重新 render + UpdateLayeredWindow
 *   - 分层窗口的命中测试按 alpha 走，圆角外和透明处点击会穿透
 */
#include "overlay.h"
#include "app.h"
#include "image.h"
#include "overlay_gdiplus.h"
#include "util.h"

#include <windowsx.h>
#include <string.h>
#include <wchar.h>

#define OVERLAY_CLASS       L"ScreenshotTranslatorOverlay"
#define TIMER_OUTSIDE       1
#define TIMER_AUTO_CLOSE    2
#define HOTKEY_ID_OVERLAY   3

typedef struct OverlayState {
    HWND        hwnd;
    HWND        notify;

    wchar_t    *text;              /* 当前显示的文字（指向下面两个之一） */
    wchar_t    *translated_copy;   /* 译文 */
    wchar_t    *original;          /* 原文，可为 NULL */

    int         x, y;              /* 屏幕坐标（虚拟屏） */
    int         width, height;
    UINT        dpi;

    OverlayTheme theme;

    HDC         mem_dc;
    HBITMAP     bitmap;
    HGDIOBJ     old_bitmap;
    void       *bits;

    int         hover_button;      /* OVERLAY_BTN_* */
    int         pressed_button;
    BOOL        dragging;
    BOOL        tracking_leave;
    BOOL        prev_lbutton;

    POINT       drag_cursor;       /* 按下时的屏幕光标 */
    RECT        drag_window;       /* 按下时的窗口矩形 */

    /* 悬浮提示气泡。浮层是 layered + noactivate，用不了系统 tooltip，
       所以自己画一块，在 UpdateLayeredWindow 之前叠上去。 */
    int         tip_w;
    int         tip_h;
    int         tip_x;
    int         tip_y;
    const wchar_t *tip_text;
} OverlayState;

static OverlayState g_ov;
static BOOL         g_class_ready = FALSE;

/* 框选区域（虚拟屏幕坐标）。有值时浮层贴着它摆，不再跟随鼠标。 */
static BOOL         g_has_anchor = FALSE;
static RECT         g_anchor;
static DWORD        g_auto_close_ms = 0;
static int          g_max_text_width = 600;

/* ================= 小工具 ================= */

static int scale_px(int value)
{
    UINT dpi = g_ov.dpi ? g_ov.dpi : 96;
    return MulDiv(value, (int)dpi, 96);
}

static unsigned int colorref_to_argb(COLORREF color, unsigned int alpha)
{
    return (alpha << 24) |
           ((unsigned int)GetRValue(color) << 16) |
           ((unsigned int)GetGValue(color) << 8) |
           (unsigned int)GetBValue(color);
}

static wchar_t *dup_wstr(const wchar_t *src)
{
    if (src == NULL) return NULL;
    size_t count = wcslen(src) + 1;
    wchar_t *out = (wchar_t *)heap_alloc(count * sizeof(wchar_t));
    if (out != NULL) memcpy(out, src, count * sizeof(wchar_t));
    return out;
}

/* 有没有可切换的原文 */
static BOOL has_original(void)
{
    return g_ov.original != NULL && g_ov.original[0] != 0;
}

/* 三个工具栏按钮在客户区里的矩形；返回命中哪个（OVERLAY_BTN_*）。
   没有原文时文档按钮不可点。 */
static int hit_button(POINT client)
{
    RECT doc, copy, close;
    overlay_button_rects(&g_ov.theme, g_ov.width, g_ov.height,
                         &doc, &copy, &close);

    if (PtInRect(&close, client)) return OVERLAY_BTN_CLOSE;
    if (PtInRect(&copy, client))  return OVERLAY_BTN_COPY;
    if (has_original() && PtInRect(&doc, client)) return OVERLAY_BTN_DOC;
    return OVERLAY_BTN_NONE;
}

/* 提示气泡用比正文小一号的字，视觉上更像个附属标注 */
static int tooltip_font_px(void)
{
    int px = g_ov.theme.font_px - 3;
    int minimum = scale_px(9);
    if (px < minimum) px = minimum;
    return px;
}

/* 某个按钮的悬浮提示文字；没有就返回 NULL */
static const wchar_t *button_tip(int id)
{
    switch (id) {
    case OVERLAY_BTN_DOC:
        return L"复制原文";
    case OVERLAY_BTN_COPY:
        return L"复制译文";
    /* 关闭按钮不加提示：X 的含义已经足够明确，
       而且它是离指针最近的按钮，气泡会频繁闪现，反而干扰。 */
    default:
        return NULL;
    }
}

static void build_theme(const Config *cfg)
{
    int alpha = (int)(cfg->overlay_opacity * 255.0f + 0.5f);
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;

    COLORREF bg = parse_color(cfg->overlay_bg, RGB(0x1E, 0x1E, 0x1E));
    COLORREF fg = parse_color(cfg->overlay_fg, RGB(0xFF, 0xFF, 0xFF));

    memset(&g_ov.theme, 0, sizeof(g_ov.theme));

    /* 背景用整窗 alpha（由 UpdateLayeredWindow 的像素 alpha 实现），
       边框/图标都用 ARGB 常量，不随 overlay_opacity 走，免得半透明后看不清。 */
    g_ov.theme.bg_argb          = colorref_to_argb(bg, (unsigned int)alpha);
    g_ov.theme.fg_argb          = colorref_to_argb(fg, 255);
    g_ov.theme.border_argb      = 0xFF4A4A4A;
    /* 工具栏底色跟文字框一致，两个元素看起来是一套东西；
       略亮一点点以便在同色区域上还能分辨轮廓 */
    g_ov.theme.toolbar_bg_argb  = 0xFF303030;

    /* 图标：常态 72% 白，悬停 100% 白 + 悬停/按下各一层底色。
       文档按钮在没有原文时会置灰，单独给一个更暗的常态色。 */
    g_ov.theme.icon_argb        = 0xB8FFFFFF;
    g_ov.theme.icon_hover_argb  = 0xFFFFFFFF;
    g_ov.theme.icon_bg_argb     = 0x1FFFFFFF;
    g_ov.theme.icon_press_argb  = 0x33FFFFFF;
    g_ov.theme.icon_disabled_argb = 0x3DFFFFFF;

    /* 不画外阴影：0 表示关闭。保留字段是因为
       overlay_gdiplus.cpp 里还有对应的绘制代码，将来想加回来改这里就行。 */
    g_ov.theme.shadow_argb      = 0;

    /* 间距整体收紧：文字更贴边，工具栏也矮一些 */
    g_ov.theme.radius          = scale_px(9);
    g_ov.theme.text_radius     = scale_px(9);
    g_ov.theme.font_px         = MulDiv(cfg->font_size, (int)g_ov.dpi, 72);
    if (g_ov.theme.font_px < 9) g_ov.theme.font_px = 9;

    g_ov.theme.padding         = scale_px(10);
    /* 上下内边距和左右分开：文字行的上下本来就有行高留白，
       再叠一层 text_pad 会显得很空 */
    g_ov.theme.text_pad        = scale_px(10);
    g_ov.theme.text_pad_y      = scale_px(5);
    g_ov.theme.max_text_width  = scale_px(g_max_text_width);
    g_ov.theme.gap             = scale_px(6);
    g_ov.theme.toolbar_h       = scale_px(28);
    g_ov.theme.toolbar_radius  = scale_px(7);
    g_ov.theme.button_size     = scale_px(24);
    g_ov.theme.button_radius   = scale_px(5);
    g_ov.theme.icon_size       = scale_px(14);
}

/* ================= 位图与呈现 ================= */

static void destroy_surface(void)
{
    if (g_ov.mem_dc != NULL) {
        if (g_ov.old_bitmap != NULL) SelectObject(g_ov.mem_dc, g_ov.old_bitmap);
        DeleteDC(g_ov.mem_dc);
        g_ov.mem_dc = NULL;
    }
    if (g_ov.bitmap != NULL) {
        DeleteObject(g_ov.bitmap);
        g_ov.bitmap = NULL;
    }
    g_ov.old_bitmap = NULL;
    g_ov.bits = NULL;
}

static BOOL create_surface(int width, int height)
{
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;      /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS,
                                      &bits, NULL, 0);
    if (bitmap == NULL || bits == NULL) {
        if (bitmap != NULL) DeleteObject(bitmap);
        return FALSE;
    }

    HDC dc = CreateCompatibleDC(NULL);
    if (dc == NULL) {
        DeleteObject(bitmap);
        return FALSE;
    }

    g_ov.old_bitmap = SelectObject(dc, bitmap);
    g_ov.bitmap = bitmap;
    g_ov.mem_dc = dc;
    g_ov.bits = bits;
    return TRUE;
}

static void present(void)
{
    POINT src = { 0, 0 };
    POINT dst = { g_ov.x, g_ov.y };
    SIZE  size = { g_ov.width, g_ov.height };
    BLENDFUNCTION blend;
    blend.BlendOp             = AC_SRC_OVER;
    blend.BlendFlags          = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat         = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_ov.hwnd, NULL, &dst, &size,
                        g_ov.mem_dc, &src, 0, &blend, ULW_ALPHA);
}

static BOOL render_now(void)
{
    if (g_ov.hwnd == NULL || g_ov.bits == NULL) return FALSE;

    if (!overlay_render(g_ov.text, &g_ov.theme, g_ov.width, g_ov.height,
                        g_ov.bits, g_ov.width * 4,
                        g_ov.hover_button, g_ov.pressed_button,
                        has_original())) {
        LOG_ERR("overlay", L"GDI+ 绘制浮层失败");
        return FALSE;
    }

    /* 悬停在按钮上时，把提示气泡直接画进同一张位图里 */
    if (g_ov.tip_text != NULL && g_ov.tip_w > 0) {
        gdiplus_draw_tooltip(g_ov.mem_dc, g_ov.tip_text, tooltip_font_px(),
                             g_ov.tip_x, g_ov.tip_y,
                             g_ov.tip_w, g_ov.tip_h,
                             0xF2202020u,      /* 深色背景 */
                             0xFFFFFFFFu);     /* 白字 */
    }

    present();
    return TRUE;
}

/* ---------- 剪贴板 ---------- */

static BOOL copy_to_clipboard(const wchar_t *text)
{
    if (text == NULL) return FALSE;

    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    if (bytes > 16 * 1024 * 1024) return FALSE;

    /* 浮层不激活，但剪贴板是全进程共享的，不需要赶在打开剪贴板前激活窗口 */
    if (!OpenClipboard(g_ov.hwnd)) {
        LOG_WARN("overlay", L"打开剪贴板失败（错误 %lu）", GetLastError());
        return FALSE;
    }

    BOOL ok = FALSE;
    if (EmptyClipboard()) {
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem != NULL) {
            void *dest = GlobalLock(mem);
            if (dest != NULL) {
                memcpy(dest, text, bytes);
                GlobalUnlock(mem);
                if (SetClipboardData(CF_UNICODETEXT, mem) != NULL) {
                    ok = TRUE;
                    mem = NULL;      /* 所有权交给剪贴板 */
                }
            }
            if (mem != NULL) GlobalFree(mem);
        }
    }

    if (!ok) {
        LOG_WARN("overlay", L"写剪贴板失败（错误 %lu）", GetLastError());
    }
    CloseClipboard();
    return ok;
}

/* ================= 定位 ================= */

static void place_near_cursor(void)
{
    POINT cursor;
    GetCursorPos(&cursor);

    /* 优先用"鼠标按下点"：用户按下左键的位置就是视线的落点，
       把译文放在它右下方最符合直觉。 */
    int pref_x, pref_y;
    if (g_has_anchor) {
        pref_x = g_anchor.left + scale_px(12);
        pref_y = g_anchor.bottom - scale_px(2);
    } else {
        pref_x = cursor.x + scale_px(16);
        pref_y = cursor.y + scale_px(16);
    }

    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (g_has_anchor) {
        POINT anchor_pt = { g_anchor.left, g_anchor.bottom };
        monitor = MonitorFromPoint(anchor_pt, MONITOR_DEFAULTTONEAREST);
    }

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        g_ov.x = pref_x;
        g_ov.y = pref_y;
        return;
    }

    int margin = scale_px(8);
    int x = pref_x;
    int y = pref_y;

    /* 右边放不下就左移贴住工作区右边 */
    if (x + g_ov.width > mi.rcWork.right - margin) {
        x = mi.rcWork.right - margin - g_ov.width;
    }
    /* 下方放不下就翻到选区上方 */
    if (g_has_anchor && y + g_ov.height > mi.rcWork.bottom - margin) {
        int above = g_anchor.bottom - scale_px(10) - g_ov.height;
        if (above >= mi.rcWork.top + margin) {
            y = above;
        } else {
            y = mi.rcWork.bottom - margin - g_ov.height;
        }
    }

    if (x < mi.rcWork.left + margin) x = mi.rcWork.left + margin;
    if (y < mi.rcWork.top + margin)  y = mi.rcWork.top + margin;
    if (x + g_ov.width > mi.rcWork.right - margin) {
        x = mi.rcWork.right - margin - g_ov.width;
    }
    if (y + g_ov.height > mi.rcWork.bottom - margin) {
        y = mi.rcWork.bottom - margin - g_ov.height;
    }

    g_ov.x = x;
    g_ov.y = y;
}

void overlay_set_anchor(const RECT *selection)
{
    if (selection == NULL) {
        g_has_anchor = FALSE;
        return;
    }
    g_anchor = *selection;
    g_has_anchor = TRUE;
}


/* ================= 交互 ================= */

/* 根据当前悬停的按钮，算出提示气泡的尺寸和位置。
   气泡优先放在按钮上方；上方放不下就放下面。 */
static void update_tooltip(void)
{
    const wchar_t *tip = button_tip(g_ov.hover_button);
    if (tip == NULL) {
        g_ov.tip_text = NULL;
        g_ov.tip_w = 0;
        g_ov.tip_h = 0;
        return;
    }

    int w = 0, h = 0;
    int font_px = tooltip_font_px();
    if (!gdiplus_measure_tooltip(tip, font_px, &w, &h)) {
        g_ov.tip_text = NULL;
        return;
    }

    /* 按钮在客户区里的位置 */
    RECT doc, copy, close;
    overlay_button_rects(&g_ov.theme, g_ov.width, g_ov.height,
                         &doc, &copy, &close);
    RECT box;
    switch (g_ov.hover_button) {
    case OVERLAY_BTN_DOC:   box = doc;   break;
    case OVERLAY_BTN_COPY:  box = copy;  break;
    case OVERLAY_BTN_CLOSE: box = close; break;
    default:                box = copy;  break;
    }

    /* 放在按钮左侧、与工具栏同一行：
       放在上方会压住译文，放在下方又超出窗口高度。 */
    int x = box.left - w - scale_px(6);
    int y = box.top + (box.bottom - box.top) / 2 - h / 2;

    /* 左边放不下就放右边 */
    if (x < 0) {
        int right_x = box.right + scale_px(6);
        if (right_x + w <= g_ov.width) {
            x = right_x;
        } else {
            x = 0;
        }
    }
    if (x + w > g_ov.width) x = g_ov.width - w;
    if (x < 0) x = 0;

    /* 垂直方向夹在窗口内 */
    if (y < 0) y = 0;
    if (y + h > g_ov.height) y = g_ov.height - h;

    g_ov.tip_text = tip;
    g_ov.tip_w = w;
    g_ov.tip_h = h;
    g_ov.tip_x = x;
    g_ov.tip_y = y;
}

static void update_hover(POINT client, BOOL inside)
{
    int id = inside ? hit_button(client) : OVERLAY_BTN_NONE;

    if (!g_ov.tracking_leave) {
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = g_ov.hwnd;
        tme.dwHoverTime = 0;
        if (TrackMouseEvent(&tme)) g_ov.tracking_leave = TRUE;
    }

    if (id != g_ov.hover_button) {
        g_ov.hover_button = id;
        update_tooltip();
        render_now();
    }
    SetCursor(LoadCursorW(NULL, id != OVERLAY_BTN_NONE ? IDC_HAND : IDC_ARROW));
}

static void begin_drag(void)
{
    g_ov.dragging = TRUE;
    GetCursorPos(&g_ov.drag_cursor);
    GetWindowRect(g_ov.hwnd, &g_ov.drag_window);
}

/* ================= 窗口过程 ================= */

static LRESULT CALLBACK overlay_wnd_proc(HWND hwnd, UINT msg,
                                         WPARAM wparam, LPARAM lparam)
{
    if (g_ov.hwnd != hwnd) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        /* 内容是 UpdateLayeredWindow 推上去的，这里只把无效区域吃掉 */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEACTIVATE:
        /* 点击浮层不要激活它，前台程序保持原样 */
        return MA_NOACTIVATE;

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL,
                g_ov.hover_button != OVERLAY_BTN_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        POINT client = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        int id = hit_button(client);
        SetCapture(hwnd);

        if (id != OVERLAY_BTN_NONE) {
            g_ov.pressed_button = id;
            g_ov.hover_button = id;
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            render_now();
            return 0;
        }

        /* 只有文本区能拖动；工具栏空白处按住不动，避免误拖 */
        RECT text_box;
        int pad = g_ov.theme.padding;
        text_box.left   = pad;
        text_box.top    = pad;
        text_box.right  = g_ov.width - pad;
        text_box.bottom = g_ov.height - pad - g_ov.theme.toolbar_h - g_ov.theme.gap;

        if (PtInRect(&text_box, client)) {
            begin_drag();
        } else {
            ReleaseCapture();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT client = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };

        if (g_ov.dragging) {
            POINT cursor;
            GetCursorPos(&cursor);
            int nx = g_ov.drag_window.left + (cursor.x - g_ov.drag_cursor.x);
            int ny = g_ov.drag_window.top  + (cursor.y - g_ov.drag_cursor.y);
            SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            g_ov.x = nx;
            g_ov.y = ny;
            return 0;
        }

        update_hover(client, TRUE);
        return 0;
    }

    case WM_MOUSELEAVE:
        g_ov.tracking_leave = FALSE;
        if (g_ov.hover_button != OVERLAY_BTN_NONE && !g_ov.dragging) {
            g_ov.hover_button = OVERLAY_BTN_NONE;
            render_now();
        }
        return 0;

    case WM_LBUTTONUP: {
        POINT client = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        int pressed = g_ov.pressed_button;
        int over = hit_button(client);

        g_ov.pressed_button = OVERLAY_BTN_NONE;
        g_ov.dragging = FALSE;
        ReleaseCapture();

        if (pressed == OVERLAY_BTN_NONE) return 0;

        /* 按下和松开必须在同一个按钮上才算点击 */
        if (pressed == over) {
            if (pressed == OVERLAY_BTN_CLOSE) {
                LOG_INFO("overlay", L"点击关闭按钮，隐藏浮层");
                overlay_hide();
                return 0;
            }
            /* 文档图标 = 复制原文，复制图标 = 复制译文。
               两个按钮各管一份，不用先切换再复制。 */
            if (pressed == OVERLAY_BTN_DOC) {
                const wchar_t *src = has_original() ? g_ov.original : NULL;
                if (src != NULL) {
                    if (copy_to_clipboard(src)) {
                        LOG_INFO("overlay", L"已复制原文（%zu 字）", wcslen(src));
                    }
                } else {
                    LOG_INFO("overlay", L"没有原文可复制");
                }
            } else if (pressed == OVERLAY_BTN_COPY) {
                const wchar_t *src = g_ov.translated_copy;
                if (src != NULL) {
                    if (copy_to_clipboard(src)) {
                        LOG_INFO("overlay", L"已复制译文（%zu 字）", wcslen(src));
                    }
                } else {
                    LOG_INFO("overlay", L"没有译文可复制");
                }
            }
        }

        g_ov.hover_button = over;
        render_now();
        return 0;
    }

    case WM_HOTKEY:
        if (wparam == HOTKEY_ID_OVERLAY) {
            LOG_INFO("overlay", L"ESC 关闭浮层");
            overlay_hide();
        }
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_OUTSIDE) {
            POINT cursor;
            GetCursorPos(&cursor);
            RECT win;
            GetWindowRect(hwnd, &win);

            BOOL down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            BOOL rising = down && !g_ov.prev_lbutton;
            g_ov.prev_lbutton = down;

            if (rising && !g_ov.dragging && g_ov.pressed_button == OVERLAY_BTN_NONE &&
                !PtInRect(&win, cursor)) {
                LOG_INFO("overlay", L"点击浮层外部，隐藏");
                overlay_hide();
            }
            return 0;
        }
        if (wparam == TIMER_AUTO_CLOSE) {
            KillTimer(hwnd, TIMER_AUTO_CLOSE);
            LOG_INFO("overlay", L"自动测试：到时关闭浮层");
            overlay_hide();
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        HWND notify = g_ov.notify;

        KillTimer(hwnd, TIMER_OUTSIDE);
        KillTimer(hwnd, TIMER_AUTO_CLOSE);
        UnregisterHotKey(hwnd, HOTKEY_ID_OVERLAY);

        destroy_surface();
        heap_free(g_ov.translated_copy);
        heap_free(g_ov.original);

        memset(&g_ov, 0, sizeof(g_ov));

        if (notify != NULL) {
            PostMessageW(notify, WM_APP_OVERLAY_CLOSED, 0, 0);
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL ensure_class(HINSTANCE instance)
{
    if (g_class_ready) return TRUE;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = overlay_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = OVERLAY_CLASS;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERR("overlay", L"注册浮层窗口类失败（错误 %lu）", err);
            return FALSE;
        }
    }
    g_class_ready = TRUE;
    return TRUE;
}

/* ================= 对外 ================= */

BOOL overlay_show(HWND notify, const wchar_t *text, const Config *cfg)
{
    return overlay_show_pair(notify, NULL, text, cfg);
}

BOOL overlay_show_pair(HWND notify, const wchar_t *original,
                       const wchar_t *text, const Config *cfg)
{
    if (text == NULL || text[0] == 0 || cfg == NULL) return FALSE;

    if (!overlay_gdiplus_startup()) {
        LOG_ERR("overlay", L"GDI+ 初始化失败");
        return FALSE;
    }

    /* 已经有浮层就换掉；先摘掉 notify，避免旧窗口的销毁通知误伤调用方 */
    g_ov.notify = NULL;
    overlay_hide();

    HINSTANCE instance = g_app.instance;
    if (instance == NULL) instance = GetModuleHandleW(NULL);
    if (!ensure_class(instance)) return FALSE;

    POINT cursor;
    GetCursorPos(&cursor);

    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        mi.rcWork.left = 0;
        mi.rcWork.top = 0;
        mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    /* 先建一个不可见的 1x1 窗口落在这块显示器上，才能拿到正确的 DPI */
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS, L"",
        WS_POPUP,
        mi.rcWork.left, mi.rcWork.top, 1, 1,
        NULL, NULL, instance, NULL);

    if (hwnd == NULL) {
        LOG_ERR("overlay", L"创建浮层窗口失败（错误 %lu）", GetLastError());
        return FALSE;
    }

    memset(&g_ov, 0, sizeof(g_ov));
    g_ov.hwnd = hwnd;
    g_ov.dpi = get_dpi(hwnd);
    build_theme(cfg);

    /* 屏幕工作区放不下 600px 文字区时收窄 */
    int work_width = mi.rcWork.right - mi.rcWork.left;
    int limit = work_width - scale_px(60);
    if (limit < scale_px(160)) limit = scale_px(160);
    if (g_ov.theme.max_text_width > limit) {
        g_ov.theme.max_text_width = limit;
    }

    int measured_w = 0, measured_h = 0;
    if (!overlay_measure(text, &g_ov.theme, &measured_w, &measured_h)) {
        LOG_ERR("overlay", L"测量文字尺寸失败");
        DestroyWindow(hwnd);
        return FALSE;
    }
    g_ov.width = measured_w;
    g_ov.height = measured_h;

    g_ov.translated_copy = dup_wstr(text);
    if (g_ov.translated_copy == NULL) {
        LOG_ERR("overlay", L"复制译文失败");
        DestroyWindow(hwnd);
        return FALSE;
    }
    g_ov.text = g_ov.translated_copy;

    if (original != NULL && original[0] != 0) {
        g_ov.original = dup_wstr(original);
        if (g_ov.original == NULL) {
            LOG_WARN("overlay", L"复制原文失败，文档按钮将不可用");
        }
    }

    if (!create_surface(g_ov.width, g_ov.height)) {
        LOG_ERR("overlay", L"创建 ARGB 位图失败（%dx%d）", g_ov.width, g_ov.height);
        DestroyWindow(hwnd);
        return FALSE;
    }

    place_near_cursor();

    if (!render_now()) {
        DestroyWindow(hwnd);
        return FALSE;
    }

    /* 显示但不激活。UpdateLayeredWindow 已定好位置和尺寸。 */
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    /* 走到这里才算真正显示成功，此后关闭才需要通知调用方 */
    g_ov.notify = notify;

    /* 不激活的窗口收不到 WM_KEYDOWN，临时注册一个裸 ESC 热键 */
    if (!RegisterHotKey(hwnd, HOTKEY_ID_OVERLAY, 0, VK_ESCAPE)) {
        LOG_WARN("overlay", L"注册 ESC 热键失败（错误 %lu），只能点关闭按钮或点外部",
                 GetLastError());
    }

    g_ov.prev_lbutton = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? TRUE : FALSE;
    SetTimer(hwnd, TIMER_OUTSIDE, 60, NULL);
    if (g_auto_close_ms > 0) {
        SetTimer(hwnd, TIMER_AUTO_CLOSE, g_auto_close_ms, NULL);
    }

    LOG_INFO("overlay", L"浮层显示：%dx%d @ (%d,%d)，DPI %u，字号 %dpx",
             g_ov.width, g_ov.height, g_ov.x, g_ov.y, g_ov.dpi,
             g_ov.theme.font_px);
    return TRUE;
}

void overlay_hide(void)
{
    if (g_ov.hwnd != NULL) {
        DestroyWindow(g_ov.hwnd);
    }
}

BOOL overlay_is_visible(void)
{
    return g_ov.hwnd != NULL;
}

void overlay_set_auto_close(DWORD delay_ms)
{
    g_auto_close_ms = delay_ms;
}

/* 配置变了（比如改了译文背景透明度）：按新配置重建主题并立刻重绘。
   浮层没显示时什么都不做，下次 overlay_show 自然会用新配置。 */
void overlay_refresh_theme(const Config *cfg)
{
    if (g_ov.hwnd == NULL || cfg == NULL) return;

    build_theme(cfg);
    render_now();
}

void overlay_set_max_text_width(int logical_px)
{
    if (logical_px < 160) logical_px = 160;
    if (logical_px > 2000) logical_px = 2000;
    g_max_text_width = logical_px;
}

/* ================= 自测：离屏渲染成 PNG ================= */

BOOL overlay_debug_dump(const wchar_t *text, const Config *cfg,
                        const wchar_t *png_path)
{
    if (text == NULL || cfg == NULL || png_path == NULL) return FALSE;
    if (!overlay_gdiplus_startup()) return FALSE;

    UINT dpi = 96;
    HDC screen = GetDC(NULL);
    if (screen != NULL) {
        dpi = (UINT)GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(NULL, screen);
    }
    if (dpi == 0) dpi = 96;

    memset(&g_ov, 0, sizeof(g_ov));
    g_ov.dpi = dpi;
    build_theme(cfg);

    int w = 0, h = 0;
    if (!overlay_measure(text, &g_ov.theme, &w, &h)) return FALSE;

    if (!create_surface(w, h)) return FALSE;
    if (!overlay_render(text, &g_ov.theme, w, h, g_ov.bits, w * 4, 0, 0, 1)) {
        destroy_surface();
        return FALSE;
    }

    /* 把预乘 ARGB 叠到棋盘格上，反算回直通 alpha，这样才能看出
       半透明背景和抗锯齿边缘的真实观感（纯色背景看不出 alpha 问题）。 */
    int pad = 24;
    int ow = w + pad * 2;
    int oh = h + pad * 2;

    ImageBuffer out;
    if (!image_create(&out, ow, oh)) {
        destroy_surface();
        return FALSE;
    }

    const unsigned char *src = (const unsigned char *)g_ov.bits;
    unsigned char *dst = (unsigned char *)out.bits;

    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int o = (y * ow + x) * 4;
            int cx = (x / 12 + y / 12) & 1;
            unsigned char bg = cx ? 0x3A : 0x2A;

            int sx = x - pad;
            int sy = y - pad;
            unsigned char br, bgg, bb, ba;

            if (sx >= 0 && sy >= 0 && sx < w && sy < h) {
                int so = (sy * w + sx) * 4;
                ba = src[so + 3];
                if (ba == 0) {
                    br = bgg = bb = bg;
                } else {
                    /* 预乘值先还原成直通颜色，再按 alpha 混合到背景 */
                    int r = (src[so + 2] * 255) / ba;
                    int g = (src[so + 1] * 255) / ba;
                    int b = (src[so + 0] * 255) / ba;
                    if (r > 255) r = 255;
                    if (g > 255) g = 255;
                    if (b > 255) b = 255;
                    br  = (unsigned char)((r * ba + bg * (255 - ba)) / 255);
                    bgg = (unsigned char)((g * ba + bg * (255 - ba)) / 255);
                    bb  = (unsigned char)((b * ba + bg * (255 - ba)) / 255);
                }
                ba = 255;
            } else {
                br = bgg = bb = bg;
            }
            /* DIB 是 BGRA */
            dst[o + 0] = bb;
            dst[o + 1] = bgg;
            dst[o + 2] = br;
            dst[o + 3] = 255;
        }
    }

    BOOL ok = image_save_png(&out, png_path);
    image_free(&out);
    destroy_surface();

    if (ok) {
        LOG_INFO("overlay", L"自测渲染已保存：%ls（%dx%d，DPI %u）",
                 png_path, w, h, dpi);
    }
    return ok;
}
