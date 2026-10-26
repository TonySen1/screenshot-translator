/* capture.c —— 全屏抓取 + 遮罩窗口 + 框选
 *
 * 遮罩是「普通窗口 + 不透明内容」，不是分层窗口：
 * 窗口里铺一张压暗过的截图，选区那块再从原图拷回来。
 * 这样边框和角标是实色的，不会被整窗 alpha 一起削弱。
 */
#include "capture.h"
#include "util.h"

#include <windowsx.h>
#include <stdio.h>
#include <string.h>

#define MASK_CLASS      L"ScreenshotTranslatorMask"
#define MIN_SELECTION   8          /* 小于这个像素视为误触 */
#define CORNER_LEN      14         /* 四角标记的长度 */
#define CORNER_WIDTH    2

#define BORDER_COLOR    RGB(0x3D, 0x9B, 0xFF)
#define SHADE_COLOR     RGB(0x00, 0x00, 0x00)

static BOOL g_mask_class_ready = FALSE;

/* 重入保护：截图有自己的内层消息循环。如果用户在这期间又按了截图热键，
   那个 WM_HOTKEY 会被内层循环取走并再开一次截图，遮罩层层叠加盖死屏幕。 */
static BOOL g_capture_active = FALSE;

/* 自动测试状态（只影响下一次 capture_select） */
static BOOL g_auto_enabled = FALSE;
static BOOL g_auto_cancel = FALSE;
static POINT g_auto_start = { 0, 0 };
static POINT g_auto_end = { 0, 0 };

typedef struct CaptureState {
    HWND         hwnd;
    HWND         owner;

    ImageBuffer  full;          /* 原始截图 */
    ImageBuffer  dimmed;        /* 压暗版 */
    ImageBuffer  back;          /* 双缓冲，窗口同尺寸 */

    Selection    area;          /* 虚拟屏幕矩形（物理像素） */

    BOOL         dragging;
    POINT        start;         /* 客户区坐标 */
    POINT        current;

    BOOL         finished;
    BOOL         cancelled;
    BOOL         closed;        /* 窗口已销毁；内层循环据此退出 */
    Selection    result;        /* 虚拟屏幕坐标 */
    POINT        anchor;        /* 鼠标按下点（虚拟屏幕坐标） */

    int          auto_step;

    /* 淡入：遮罩从透明逐渐压暗，避免直接糊一层黑 */
    float        fade_target;      /* 目标压暗系数 */
    float        fade_current;
    int          fade_step;
} CaptureState;

#define FADE_TIMER_ID   2
#define FADE_STEPS      10         /* 10 帧，约 160ms */
#define FADE_INTERVAL   16

/* ================= 抓屏 ================= */

/* 逐个枚举显示器，取所有 rcMonitor 的并集。
 *
 * 不能只靠 GetSystemMetrics(SM_*VIRTUALSCREEN)：在混合 DPI 的机器上
 * （一台 100%、一台 150%），这些值可能按主显示器的 DPI 折算，
 * 而 BitBlt 需要的是物理像素，两者一错位截图就整体偏移或缩放。
 * EnumDisplayMonitors + GetMonitorInfo 在 PerMonitorV2 进程里返回的是
 * 虚拟屏幕坐标系的物理像素，和 BitBlt 的源坐标一致。
 */
typedef struct MonitorBounds {
    RECT rect;
    int  count;
} MonitorBounds;

static BOOL CALLBACK enum_monitor_proc(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM lparam)
{
    MonitorBounds *mb = (MonitorBounds *)lparam;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return TRUE;

    if (mb->count == 0) {
        mb->rect = mi.rcMonitor;
    } else {
        if (mi.rcMonitor.left   < mb->rect.left)   mb->rect.left   = mi.rcMonitor.left;
        if (mi.rcMonitor.top    < mb->rect.top)    mb->rect.top    = mi.rcMonitor.top;
        if (mi.rcMonitor.right  > mb->rect.right)  mb->rect.right  = mi.rcMonitor.right;
        if (mi.rcMonitor.bottom > mb->rect.bottom) mb->rect.bottom = mi.rcMonitor.bottom;
    }
    mb->count++;
    return TRUE;
}

BOOL capture_screen(ImageBuffer *img, Selection *area)
{
    if (img == NULL || area == NULL) return FALSE;

    MonitorBounds mb;
    memset(&mb, 0, sizeof(mb));
    EnumDisplayMonitors(NULL, NULL, enum_monitor_proc, (LPARAM)&mb);

    int x, y, w, h;
    if (mb.count > 0) {
        x = mb.rect.left;
        y = mb.rect.top;
        w = mb.rect.right - mb.rect.left;
        h = mb.rect.bottom - mb.rect.top;
        LOG_INFO("capture", L"枚举到 %d 个显示器，并集 %dx%d @ (%d,%d)",
                 mb.count, w, h, x, y);
    } else {
        /* 枚举失败才退回 GetSystemMetrics */
        x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        LOG_WARN("capture", L"EnumDisplayMonitors 没返回显示器，退回 GetSystemMetrics");
    }

    if (w <= 0 || h <= 0) {
        LOG_ERR("capture", L"虚拟屏幕尺寸无效：%dx%d", w, h);
        return FALSE;
    }

    if (!image_create(img, w, h)) {
        LOG_ERR("capture", L"创建截图缓冲区失败（%dx%d）", w, h);
        return FALSE;
    }

    HDC screen = GetDC(NULL);
    if (screen == NULL) {
        image_free(img);
        return FALSE;
    }

    /* 从虚拟屏幕左上角拷贝整个并集区域 */
    BOOL ok = BitBlt(img->dc, 0, 0, w, h, screen, x, y, SRCCOPY);
    ReleaseDC(NULL, screen);

    if (!ok) {
        LOG_ERR("capture", L"BitBlt 抓屏失败（错误 %lu）", GetLastError());
        image_free(img);
        return FALSE;
    }

    area->left = x;
    area->top = y;
    area->width = w;
    area->height = h;

    LOG_INFO("capture", L"屏幕已抓取：%dx%d @ (%d,%d)", w, h, x, y);
    return TRUE;
}

/* ================= 选区计算 ================= */

static void normalize_rect(POINT a, POINT b, RECT *out)
{
    out->left = a.x < b.x ? a.x : b.x;
    out->top = a.y < b.y ? a.y : b.y;
    out->right = a.x > b.x ? a.x : b.x;
    out->bottom = a.y > b.y ? a.y : b.y;
}

static BOOL has_selection(const CaptureState *st)
{
    if (!st->dragging && !st->finished) return FALSE;
    RECT r;
    normalize_rect(st->start, st->current, &r);
    return (r.right - r.left) > 0 && (r.bottom - r.top) > 0;
}

/* ================= 绘制 ================= */

/* 静态画刷/画笔，避免每次重绘都创建 GDI 对象 */
static HBRUSH g_border_brush = NULL;
static HPEN   g_corner_pen = NULL;

static HBRUSH capture_border_brush(void)
{
    if (g_border_brush == NULL) {
        g_border_brush = CreateSolidBrush(BORDER_COLOR);
    }
    return g_border_brush;
}

static HPEN capture_corner_pen(void)
{
    if (g_corner_pen == NULL) {
        g_corner_pen = CreatePen(PS_SOLID, CORNER_WIDTH, BORDER_COLOR);
    }
    return g_corner_pen;
}

static void draw_border(HDC dc, const RECT *r)
{
    RECT box = *r;
    if (box.right <= box.left || box.bottom <= box.top) return;
    FrameRect(dc, &box, capture_border_brush());
}

static void draw_corners(HDC dc, const RECT *r)
{
    int left = r->left, top = r->top;
    int right = r->right - 1, bottom = r->bottom - 1;
    int len = CORNER_LEN;

    /* 选区太小时缩短角标，别糊在一起 */
    int max_len = (r->right - r->left) / 2;
    if ((r->bottom - r->top) / 2 < max_len) max_len = (r->bottom - r->top) / 2;
    if (max_len < len) len = max_len;
    if (len <= 0) return;

    HPEN pen = capture_corner_pen();
    HGDIOBJ old_pen = SelectObject(dc, pen);

    /* 左上 */
    MoveToEx(dc, left, top + len, NULL);  LineTo(dc, left, top);
    LineTo(dc, left + len, top);
    /* 右上 */
    MoveToEx(dc, right - len, top, NULL); LineTo(dc, right, top);
    LineTo(dc, right, top + len);
    /* 右下 */
    MoveToEx(dc, right, bottom - len, NULL); LineTo(dc, right, bottom);
    LineTo(dc, right - len, bottom);
    /* 左下 */
    MoveToEx(dc, left + len, bottom, NULL); LineTo(dc, left, bottom);
    LineTo(dc, left, bottom - len);

    SelectObject(dc, old_pen);
}

static void compose(CaptureState *st)
{
    int w = st->area.width;
    int h = st->area.height;

    /* 1. 整窗铺压暗图。淡入阶段把压暗图按当前进度混合到原图上，
          等价于"遮罩从透明渐变到目标浓度"。 */
    if (st->fade_current >= 1.0f) {
        BitBlt(st->back.dc, 0, 0, w, h, st->dimmed.dc, 0, 0, SRCCOPY);
    } else if (st->fade_current <= 0.0f) {
        BitBlt(st->back.dc, 0, 0, w, h, st->full.dc, 0, 0, SRCCOPY);
    } else {
        /* 先铺原图，再把压暗图按 alpha 叠上去 */
        BitBlt(st->back.dc, 0, 0, w, h, st->full.dc, 0, 0, SRCCOPY);
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = (BYTE)(st->fade_current * 255.0f + 0.5f);
        bf.AlphaFormat = 0;
        AlphaBlend(st->back.dc, 0, 0, w, h,
                   st->dimmed.dc, 0, 0, w, h, bf);
    }

    /* 2. 选区贴回原图 */
    if (has_selection(st)) {
        RECT r;
        normalize_rect(st->start, st->current, &r);

        int cw = r.right - r.left;
        int ch = r.bottom - r.top;
        if (cw > 0 && ch > 0) {
            BitBlt(st->back.dc, r.left, r.top, cw, ch,
                   st->full.dc, r.left, r.top, SRCCOPY);
            draw_border(st->back.dc, &r);
            draw_corners(st->back.dc, &r);
        }
    }
}

static void present(CaptureState *st)
{
    HDC dc = GetDC(st->hwnd);
    if (dc == NULL) return;
    BitBlt(dc, 0, 0, st->area.width, st->area.height,
           st->back.dc, 0, 0, SRCCOPY);
    ReleaseDC(st->hwnd, dc);
}

/* ================= 完成/取消 ================= */

static void finish_selection(CaptureState *st)
{
    RECT r;
    normalize_rect(st->start, st->current, &r);

    if ((r.right - r.left) < MIN_SELECTION || (r.bottom - r.top) < MIN_SELECTION) {
        LOG_INFO("capture", L"选区太小（%dx%d），按取消处理",
                 r.right - r.left, r.bottom - r.top);
        st->cancelled = TRUE;
    } else {
        st->result.left = r.left + st->area.left;
        st->result.top = r.top + st->area.top;
        /* 记住鼠标按下的那个点（虚拟屏幕坐标），浮层以它为锚 */
        st->anchor.x = st->start.x + st->area.left;
        st->anchor.y = st->start.y + st->area.top;
        st->result.width = r.right - r.left;
        st->result.height = r.bottom - r.top;
        st->finished = TRUE;
        LOG_INFO("capture", L"选区确定：%dx%d @ 虚拟屏 (%d,%d)",
                 st->result.width, st->result.height,
                 st->result.left, st->result.top);
    }
    DestroyWindow(st->hwnd);
}

/* ================= 自动测试 ================= */

static void auto_select_step(CaptureState *st)
{
    if (g_auto_cancel) {
        if (st->auto_step == 0) {
            PostMessageW(st->hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        } else {
            KillTimer(st->hwnd, 1);
        }
        st->auto_step++;
        return;
    }

    /* 用 PostMessage 注入客户区坐标的鼠标事件，不碰真实光标 */
    POINT a = { g_auto_start.x - st->area.left, g_auto_start.y - st->area.top };
    POINT b = { g_auto_end.x - st->area.left, g_auto_end.y - st->area.top };

    switch (st->auto_step) {
    case 0:
        PostMessageW(st->hwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                     MAKELPARAM(a.x, a.y));
        break;
    case 1:
        PostMessageW(st->hwnd, WM_MOUSEMOVE, MK_LBUTTON,
                     MAKELPARAM(b.x, b.y));
        break;
    case 2:
        PostMessageW(st->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(b.x, b.y));
        break;
    default:
        KillTimer(st->hwnd, 1);
        break;
    }
    st->auto_step++;
}

/* ================= 窗口过程 ================= */

static LRESULT CALLBACK mask_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    CaptureState *st = (CaptureState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        st = (CaptureState *)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;                       /* 全部由 WM_PAINT 双缓冲绘制 */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (st) {
            compose(st);
            BitBlt(dc, 0, 0, st->area.width, st->area.height,
                   st->back.dc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_CROSS));
        return TRUE;

    case WM_LBUTTONDOWN:
        if (st && !st->dragging) {
            st->dragging = TRUE;
            st->finished = FALSE;
            st->start.x = GET_X_LPARAM(lparam);
            st->start.y = GET_Y_LPARAM(lparam);
            st->current = st->start;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (st && st->dragging) {
            POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
            if (pt.x != st->current.x || pt.y != st->current.y) {
                st->current = pt;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (st && st->dragging) {
            st->dragging = FALSE;
            st->current.x = GET_X_LPARAM(lparam);
            st->current.y = GET_Y_LPARAM(lparam);
            ReleaseCapture();
            finish_selection(st);
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (st) {
            LOG_INFO("capture", L"右键取消");
            st->cancelled = TRUE;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && st) {
            LOG_INFO("capture", L"ESC 取消");
            st->cancelled = TRUE;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_TIMER:
        if (wparam == 1 && st) {
            auto_select_step(st);
            return 0;
        }
        if (wparam == FADE_TIMER_ID && st) {
            st->fade_step++;
            float t = (float)st->fade_step / (float)FADE_STEPS;
            if (t >= 1.0f) {
                t = 1.0f;
                KillTimer(hwnd, FADE_TIMER_ID);
            }
            /* ease-out：开头快、结尾慢，观感更自然 */
            float eased = 1.0f - (1.0f - t) * (1.0f - t);
            st->fade_current = eased;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        /* 只置标志，绝对不能 PostQuitMessage —— 那会把外层主循环也退掉 */
        KillTimer(hwnd, FADE_TIMER_ID);
        if (st) st->closed = TRUE;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL ensure_mask_class(HINSTANCE instance)
{
    if (g_mask_class_ready) return TRUE;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = mask_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    wc.lpszClassName = MASK_CLASS;
    wc.hbrBackground = NULL;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERR("capture", L"注册遮罩窗口类失败（错误 %lu）", err);
            return FALSE;
        }
    }
    g_mask_class_ready = TRUE;
    return TRUE;
}

/* ================= 对外 ================= */

static POINT g_last_anchor;

void capture_last_anchor(POINT *out)
{
    if (out != NULL) *out = g_last_anchor;
}

void capture_set_auto_select(BOOL enabled, int x0, int y0, int x1, int y1)
{
    g_auto_enabled = enabled;
    g_auto_cancel = FALSE;
    g_auto_start.x = x0;
    g_auto_start.y = y0;
    g_auto_end.x = x1;
    g_auto_end.y = y1;
}

void capture_set_auto_cancel(BOOL enabled)
{
    g_auto_enabled = enabled;
    g_auto_cancel = TRUE;
}

BOOL capture_select(HWND owner, const Config *cfg,
                    Selection *out_sel, ImageBuffer *out_image)
{
    if (out_sel == NULL || out_image == NULL) return FALSE;
    memset(out_image, 0, sizeof(*out_image));

    /* 已经在截图就不重入：内层消息循环会把第二次的 WM_HOTKEY 也取走，
       再开一个遮罩窗口，反复按就会叠成一片黑。 */
    if (g_capture_active) {
        LOG_INFO("capture", L"截图已在进行中，忽略重复触发");
        return FALSE;
    }

    HINSTANCE instance = owner ? (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE)
                               : GetModuleHandleW(NULL);
    if (!ensure_mask_class(instance)) return FALSE;

    g_capture_active = TRUE;

    CaptureState st;
    memset(&st, 0, sizeof(st));
    st.owner = owner;
    st.cancelled = TRUE;

    /* 1. 抓屏 */
    if (!capture_screen(&st.full, &st.area)) {
        g_capture_active = FALSE;
        return FALSE;
    }

    /* 2. 压暗 */
    if (!image_create(&st.dimmed, st.area.width, st.area.height) ||
        !image_create(&st.back, st.area.width, st.area.height)) {
        LOG_ERR("capture", L"创建遮罩缓冲区失败");
        image_free(&st.full);
        image_free(&st.dimmed);
        image_free(&st.back);
        g_capture_active = FALSE;
        return FALSE;
    }

    float factor = (cfg != NULL) ? cfg->mask_opacity : 0.35f;
    if (factor < 0.05f) factor = 0.05f;
    if (factor > 1.0f)  factor = 1.0f;

    /* 先按最终强度生成压暗图；淡入时通过拷贝多少行来控制观感 */
    st.fade_target = factor;
    st.fade_current = factor;
    st.fade_step = 0;
    image_darken(&st.full, &st.dimmed, factor);

    /* 3. 建遮罩窗口（覆盖整个虚拟屏） */
    st.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        MASK_CLASS, L"",
        WS_POPUP,
        st.area.left, st.area.top, st.area.width, st.area.height,
        NULL, NULL, instance, &st);

    if (st.hwnd == NULL) {
        LOG_ERR("capture", L"创建遮罩窗口失败（错误 %lu）", GetLastError());
        image_free(&st.full);
        image_free(&st.dimmed);
        image_free(&st.back);
        g_capture_active = FALSE;
        return FALSE;
    }

    /* 淡入：先以 0 浓度显示，再由定时器逐步加到目标值 */
    st.fade_current = 0.0f;
    ShowWindow(st.hwnd, SW_SHOW);
    SetForegroundWindow(st.hwnd);
    SetFocus(st.hwnd);
    SetTimer(st.hwnd, FADE_TIMER_ID, FADE_INTERVAL, NULL);

    if (g_auto_enabled) {
        st.auto_step = 0;
        SetTimer(st.hwnd, 1, 80, NULL);
        LOG_INFO("capture", L"自动测试模式：(%d,%d) -> (%d,%d)",
                 g_auto_start.x, g_auto_start.y, g_auto_end.x, g_auto_end.y);
    }

    /* 4. 内层消息循环，直到遮罩窗口销毁 */
    MSG msg;
    while (!st.closed && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* 5. 收拾 */
    image_free(&st.dimmed);
    image_free(&st.back);

    if (!st.finished) {
        image_free(&st.full);
        g_capture_active = FALSE;
        LOG_INFO("capture", L"截图已取消");
        return FALSE;
    }

    *out_sel = st.result;

    /* 6. 裁剪原图（客户区坐标 == 原图坐标，因为窗口正好铺满虚拟屏） */
    int cx = st.result.left - st.area.left;
    int cy = st.result.top - st.area.top;
    if (!image_create(out_image, st.result.width, st.result.height) ||
        !image_crop(&st.full, out_image, cx, cy, st.result.width, st.result.height)) {
        LOG_ERR("capture", L"裁剪选区失败");
        image_free(out_image);
        image_free(&st.full);
        g_capture_active = FALSE;
        return FALSE;
    }

    image_free(&st.full);
    g_capture_active = FALSE;
    g_last_anchor = st.anchor;
    LOG_INFO("capture", L"选区图像已裁剪：%dx%d", out_image->width, out_image->height);
    return TRUE;
}
