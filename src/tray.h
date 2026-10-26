/* tray.h —— 托盘图标与右键菜单 */
#ifndef TRAY_H
#define TRAY_H

#include <windows.h>
#include "config.h"

/* 添加托盘图标。hwnd 是接收回调的消息窗口。 */
BOOL tray_init(HWND hwnd, const Config *cfg);

/* 刷新悬停提示里的快捷键 */
void tray_update_tip(const wchar_t *hotkey);

/* 移除托盘图标（退出前必须调用，否则图标会残留） */
void tray_remove(void);

/* Explorer 重启后需要重新添加。返回值非 0 表示这条消息就是 TaskbarCreated。 */
UINT tray_taskbar_created_message(void);
void tray_re_add(void);

/* 在鼠标位置弹出右键菜单 */
void tray_show_menu(HWND hwnd);

#endif /* TRAY_H */
