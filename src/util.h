/* util.h —— UTF 转换、日志、路径、DPI、单实例 */
#ifndef UTIL_H
#define UTIL_H

#include <windows.h>

/* ---------- UTF-8 / UTF-16 ---------- */

/* 返回新分配的 UTF-16 字符串（HeapAlloc），调用方用 heap_free 释放。
   失败返回 NULL。 */
wchar_t *u8_to_u16(const char *src);

/* 返回新分配的 UTF-8 字符串（HeapAlloc），调用方用 heap_free 释放。 */
char *u16_to_u8(const wchar_t *src);

/* 安全拷贝，保证以 0 结尾 */
void wstr_copy(wchar_t *dst, size_t dst_count, const wchar_t *src);

/* 大小写不敏感比较（只用 ASCII 部分，够配置项和热键用了） */
BOOL wstr_ieq(const wchar_t *a, const wchar_t *b);

void *heap_alloc(size_t bytes);
void *heap_realloc(void *ptr, size_t bytes);
void heap_free(void *ptr);

/* ---------- 日志 ---------- */

/* 打开（或创建）日志文件，UTF-8 追加写。path 为空时用 g_app 里的默认路径。 */
BOOL log_init(const wchar_t *path);
void log_close(void);

/* 写一行：2026-10-26 17:00:00 [INFO] main: 文本
   格式串用宽字符 —— 中文日志占绝大多数，用窄串 + %ls 在 MSVC 上会丢内容。 */
void log_msg_wide(const char *level, const char *module, const wchar_t *fmt, ...);

#define LOG_INFO(mod, ...)  log_msg_wide("INFO",  mod, __VA_ARGS__)
#define LOG_WARN(mod, ...)  log_msg_wide("WARN",  mod, __VA_ARGS__)
#define LOG_ERR(mod, ...)   log_msg_wide("ERROR", mod, __VA_ARGS__)

/* ---------- 路径 ---------- */

/* 取 exe 所在目录（不含末尾反斜杠），失败返回 FALSE */
BOOL get_exe_dir(wchar_t *out, size_t out_count);

/* 拼路径：base + "\\" + name */
void path_join(wchar_t *out, size_t out_count, const wchar_t *base, const wchar_t *name);

/* 确保目录存在 */
BOOL ensure_dir(const wchar_t *path);

/* ---------- 系统 ---------- */

/* 进程级 DPI 感知。manifest 里已经声明了，这里是兜底。
   必须在任何窗口创建之前调用。 */
void set_dpi_awareness(void);

/* 单实例。成功返回互斥体句柄（进程存活期间不要关闭），已存在返回 NULL。 */
HANDLE acquire_single_instance(void);

/* 取主显示器的 DPI（用于字号换算） */
UINT get_dpi(HWND hwnd);

/* 颜色字符串 "#1e1e1e" -> COLORREF，失败返回 fallback */
COLORREF parse_color(const wchar_t *text, COLORREF fallback);

/* ---------- 深色模式 ---------- */

/* 让系统标准控件（编辑框、按钮、滑块、列表框、滚动条）按深色绘制。
   必须在创建任何窗口之前调用一次；Win10 1903+ / Win11 生效，
   更老的系统上静默失败（界面退回浅色）。 */
void dark_mode_init(void);

/* 给单个窗口套用深色标题栏，并递归处理它的子控件 */
void dark_mode_apply(HWND hwnd);

/* 只给某个控件套深色（新建控件后单独调用）。
   目前只对 EDIT / LISTBOX 生效，按钮和滑块保持系统外观。 */
void dark_mode_apply_control(HWND control);

/* 深色主题实际是否可用（老系统上可能是 FALSE） */
BOOL dark_mode_is_enabled(void);

/* 深色配色。WM_CTLCOLOR* 里用。 */
COLORREF dark_bg_color(void);
COLORREF dark_panel_color(void);
COLORREF dark_field_color(void);
COLORREF dark_border_color(void);
COLORREF dark_fg_color(void);
COLORREF dark_fg_dim_color(void);

#endif /* UTIL_H */
