/* overlay_gdiplus.h —— GDI+ 渲染的 C 接口
 *
 * GDI+ 的头文件是 C++ 的（namespace Gdiplus），纯 C 编译单元无法包含。
 * 所以测量和绘制放在一个 .cpp 里，这里只暴露 C 可见的入口。
 * 其余模块仍然是纯 C。
 */
#ifndef OVERLAY_GDIPLUS_H
#define OVERLAY_GDIPLUS_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 工具栏按钮编号 */
#define OVERLAY_BTN_NONE    0
#define OVERLAY_BTN_DOC     1
#define OVERLAY_BTN_COPY    2
#define OVERLAY_BTN_CLOSE   3

typedef struct OverlayTheme {
    unsigned int bg_argb;          /* 浮层背景（半透明） */
    unsigned int fg_argb;          /* 正文 */
    unsigned int border_argb;      /* 文本区边框 */
    unsigned int toolbar_bg_argb;  /* 胶囊工具栏底色 */
    unsigned int icon_argb;        /* 图标常态 */
    unsigned int icon_hover_argb;  /* 图标悬停 */
    unsigned int icon_bg_argb;     /* 按钮悬停底色 */
    unsigned int icon_press_argb;  /* 按钮按下底色 */
    unsigned int icon_disabled_argb; /* 不可用按钮的图标色 */
    unsigned int shadow_argb;      /* 文字框外发光色（0 = 不画） */

    int radius;                    /* 浮层圆角，像素 */
    int text_radius;               /* 文本区圆角，像素 */
    int font_px;                   /* 字号，像素 */
    int padding;                   /* 浮层外缘到内容的内边距 */
    int text_pad;                  /* 文本区左右留白 */
    int text_pad_y;                /* 文本区上下留白（一般比左右小） */
    int max_text_width;            /* 文本区最大宽度（不含 text_pad） */
    int gap;                       /* 文本区与工具栏之间的间距 */
    int toolbar_h;                 /* 胶囊工具栏高度 */
    int toolbar_radius;            /* 工具栏圆角（不会超过高度的一半） */
    int button_size;               /* 单个按钮的点击区边长 */
    int button_radius;             /* 按钮悬停底色的圆角 */
    int icon_size;                 /* 图标视觉尺寸 */
} OverlayTheme;

/* 进程内初始化一次 */
BOOL overlay_gdiplus_startup(void);
void overlay_gdiplus_shutdown(void);

/* 按文字内容算出窗口尺寸（含文本区边框、工具栏与各处内边距）。
   文字区宽度不会超过 theme->max_text_width。 */
BOOL overlay_measure(const wchar_t *text, const OverlayTheme *theme,
                     int *out_width, int *out_height);

/* 把整个浮层画进一段预乘 alpha 的 ARGB 缓冲（BGRA 字节序）。
   hover_button / pressed_button 取值 OVERLAY_BTN_*。
   doc_enabled 为 FALSE 时文档按钮画成置灰。 */
BOOL overlay_render(const wchar_t *text, const OverlayTheme *theme,
                    int width, int height, void *bits, int stride,
                    int hover_button, int pressed_button, int doc_enabled);

/* 算三个按钮在客户区里的矩形。overlay_measure 算出尺寸后调用。
   任一 out 参数可为 NULL。 */
void overlay_button_rects(const OverlayTheme *theme, int width, int height,
                          RECT *out_doc, RECT *out_copy, RECT *out_close);

/* 量一段提示文字所需的尺寸（含内边距）。用于给工具栏按钮画悬浮提示。 */
BOOL gdiplus_measure_tooltip(const wchar_t *text, int font_px,
                             int *out_width, int *out_height);

/* 在 dc 的 (x, y) 处画一个提示气泡，(x, y) 是气泡左上角。
   返回值同 measure。 */
BOOL gdiplus_draw_tooltip(HDC dc, const wchar_t *text, int font_px,
                          int x, int y, int width, int height,
                          unsigned int bg_argb, unsigned int fg_argb);

/* 在 dc 的 (x, y) 处画一个 size×size 的 GitHub 标志（矢量，任意 DPI 清晰）。
   argb 是填充色。失败返回 FALSE。 */
BOOL gdiplus_draw_github_mark(HDC dc, int x, int y, int size,
                              unsigned int argb);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_GDIPLUS_H */
