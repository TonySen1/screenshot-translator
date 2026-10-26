/* settings.h —— 设置对话框（原生控件，纯代码创建） */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <windows.h>
#include "config.h"

/* 以模态方式打开设置窗口。
   用户点"保存"并写入成功时返回 TRUE，同时 *cfg 已被更新；
   取消、关闭或保存失败返回 FALSE，*cfg 保持不变。 */
BOOL settings_show(HWND owner, Config *cfg);

/* 模型列表选择框。items 是以 NULL 结尾的字符串数组。
   返回选中下标；取消返回 -1。 */
int settings_choose_model(HWND owner, const wchar_t **items);

/* 需要在启动时调用一次：注册工具栏等公共控件类。 */
void settings_init(void);

#endif /* SETTINGS_H */
