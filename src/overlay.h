/* overlay.h —— 译文浮层窗口（无边框 / 分层 / 可拖动）
 *
 * 视觉部分全部由 overlay_gdiplus.cpp 画进一张 32 位预乘 ARGB 位图，
 * 这里只负责：窗口生命周期、定位、鼠标交互、把位图交给 UpdateLayeredWindow。
 */
#ifndef OVERLAY_H
#define OVERLAY_H

#include <windows.h>
#include "config.h"

/* 显示浮层。text 会被内部拷贝一份，调用后可以立刻释放。
   notify 非空时，浮层关闭后向它 PostMessage(WM_APP_OVERLAY_CLOSED)。
   定位在鼠标所在的显示器工作区内。
   返回 FALSE 表示创建失败。 */
BOOL overlay_show(HWND notify, const wchar_t *text, const Config *cfg);

/* 同上，但同时带上原文。文档按钮用于在原文/译文之间切换；
   original 为 NULL 或空串时文档按钮置灰。 */
BOOL overlay_show_pair(HWND notify, const wchar_t *original,
                       const wchar_t *text, const Config *cfg);

/* 文字太重时会撑满屏幕，这里限制最大宽度（逻辑像素，默认 600）。 */
void overlay_set_max_text_width(int logical_px);

/* 设置浮层的定位锚点（虚拟屏幕坐标）。框选完成后调用一次，
   之后每次显示都优先贴着这块区域摆，而不是跟着鼠标跑。
   传 NULL 表示改回"跟随鼠标"。 */
void overlay_set_anchor(const RECT *selection);

/* 配置改了（例如译文背景透明度）时调用：按新配置重建主题并立即重绘。
   浮层没显示时不做任何事。 */
void overlay_refresh_theme(const Config *cfg);

/* 主动关闭（未显示时无操作） */
void overlay_hide(void);

BOOL overlay_is_visible(void);

/* 自测用：显示后 delay_ms 毫秒自动关闭。0 = 取消自动关闭。 */
void overlay_set_auto_close(DWORD delay_ms);

/* 自测用：不开窗口，直接把浮层渲染结果叠到棋盘格背景上存成 PNG。
   用来在没有窗口管理器干扰的情况下逐像素检查圆角、抗锯齿和布局。 */
BOOL overlay_debug_dump(const wchar_t *text, const Config *cfg,
                        const wchar_t *png_path);

#endif /* OVERLAY_H */
