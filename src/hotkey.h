/* hotkey.h —— 全局热键（RegisterHotKey） */
#ifndef HOTKEY_H
#define HOTKEY_H

#include <windows.h>
#include "config.h"

/* 热键 ID，对应 WM_HOTKEY 的 wParam */
#define HOTKEY_ID_CAPTURE   1
#define HOTKEY_ID_EXIT      2

/* 把 "ctrl+alt+d" 解析成 Win32 的 modifier 与虚拟键码。
   成功返回 TRUE；失败（空串、只有修饰键、无法识别的键名）返回 FALSE。 */
BOOL hotkey_parse(const wchar_t *combo, UINT *out_mods, UINT *out_vk);

/* 反过来：把 mods + vk 拼成 "ctrl+alt+d"，用于界面显示。
   out 需要至少 64 个 wchar_t。 */
void hotkey_format(UINT mods, UINT vk, wchar_t *out, size_t out_count);

/* 槽位：探测时告诉函数"这个组合是给谁用的"，
   用来区分"和本程序其他功能冲突"和"被别的程序占用"。 */
#define HOTKEY_SLOT_CAPTURE  0
#define HOTKEY_SLOT_EXIT     1
#define HOTKEY_SLOT_ANY     (-1)   /* 不知道哪个槽位 */

/* 试探某个组合能否注册。用 hwnd 申请一个临时 ID，立刻注销。
   slot 传 HOTKEY_SLOT_*，表示这个组合准备用在哪个功能上。
   *out_reason 为空表示可用；否则是一句中文说明（调用方不需要释放）。 */
BOOL hotkey_probe(HWND hwnd, const wchar_t *combo,
                  int slot, const wchar_t **out_reason);

/* 按配置注册全部热键到 hwnd。注册失败会自动回退到默认值并记日志。 */
BOOL hotkey_register_all(HWND hwnd, const Config *cfg);

/* 注销本模块注册过的全部热键 */
void hotkey_unregister_all(HWND hwnd);

/* 暂停/恢复：录制快捷键期间用，避免按下的组合触发动作。
   hotkey_suspend 会注销本程序的热键并记住原组合；
   hotkey_resume 按记住的组合重新注册。两者可重复调用。 */
void hotkey_suspend(HWND hwnd);
void hotkey_resume(HWND hwnd);

/* 丢弃暂停时记住的组合（不清除暂停标志）。
   用于"保存了新配置、准备按新配置重新注册"的场景，
   避免 resume 先把旧组合注册回去。 */
void hotkey_abandon_suspend(void);

#endif /* HOTKEY_H */
