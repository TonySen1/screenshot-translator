/* capture.h —— 全屏抓取、遮罩、框选 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <windows.h>
#include "config.h"
#include "image.h"

/* 虚拟屏幕上的一个矩形，坐标为物理像素，left/top 可能为负 */
typedef struct Selection {
    int left;
    int top;
    int width;
    int height;
} Selection;

/* 抓取整个虚拟屏幕（所有显示器）。
   成功时 img 已创建，area 是虚拟屏矩形。 */
BOOL capture_screen(ImageBuffer *img, Selection *area);

/* 上一次成功框选时，鼠标按下的那个点（虚拟屏幕坐标）。
   capture_select 返回 TRUE 后有效。 */
void capture_last_anchor(POINT *out);

/* 显示遮罩并让用户框选。
   返回 TRUE  -> out_sel 有效（虚拟屏幕坐标），out_image 是裁剪出的原图
   返回 FALSE -> 用户取消（ESC / 右键 / 选区过小），out_image 未被创建
   调用方负责 image_free(out_image)。 */
BOOL capture_select(HWND owner, const Config *cfg,
                    Selection *out_sel, ImageBuffer *out_image);

/* 自动测试：设定后，下一次 capture_select 会用 PostMessage 注入鼠标事件，
   不移动真实光标。坐标是虚拟屏幕坐标。传 FALSE 关闭。 */
void capture_set_auto_select(BOOL enabled, int x0, int y0, int x1, int y1);

/* 自动测试：进入截图后立即模拟 ESC */
void capture_set_auto_cancel(BOOL enabled);

#endif /* CAPTURE_H */
