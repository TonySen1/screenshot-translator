/* settings.c —— 设置窗口
 *
 * 全部使用系统原生控件（EDIT / BUTTON / STATIC / msctls_trackbar32），
 * 不自绘：输入法、选区、光标、无障碍都由系统提供。
 *
 * 模态靠"禁用 owner + 内层消息循环"实现，Tab / Enter / ESC 交给
 * IsDialogMessage，行为与标准对话框一致。
 *
 * 网络操作（测试服务、拉取模型列表）放在工作线程里，界面不卡。
 */
#include "settings.h"
#include "app.h"
#include "hotkey.h"
#include "overlay_gdiplus.h"
#include "translator.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define SETTINGS_CLASS L"ScreenshotTranslatorSettings"

/* 控件 ID */
enum {
    IDC_LBL_MASK = 100,
    IDC_SLD_MASK,
    IDC_VAL_MASK,
    IDC_LBL_OVERLAY,
    IDC_SLD_OVERLAY,
    IDC_VAL_OVERLAY,
    IDC_LBL_FONT,
    IDC_SLD_FONT,
    IDC_VAL_FONT,

    IDC_LBL_BASE,
    IDC_EDIT_BASE,
    IDC_LBL_KEY,
    IDC_EDIT_KEY,
    IDC_LBL_MODEL,
    IDC_EDIT_MODEL,
    IDC_BTN_MODELS,
    IDC_LBL_LANG,
    IDC_EDIT_LANG,

    IDC_LBL_HOTKEY,
    IDC_EDIT_HOTKEY,
    IDC_LBL_EXITKEY,
    IDC_EDIT_EXITKEY,
    IDC_LBL_HINT,

    IDC_LBL_STATUS,
    IDC_BTN_TEST,
    IDC_BTN_SAVE,
    IDC_BTN_CANCEL
};

/* 工作线程 -> 窗口的私有消息 */
#define WM_SETTINGS_NET_DONE (WM_APP + 20)

#define NET_NONE     0
#define NET_TEST     1
#define NET_MODELS   2

typedef struct NetJob {
    int        kind;
    HWND       hwnd;        /* 结果投递目标 */
    wchar_t   *base_url;
    wchar_t   *api_key;
    wchar_t   *model;
    int        timeout_sec;
} NetJob;

typedef struct NetResult {
    int      kind;
    wchar_t *text;          /* 成功：模型列表（换行分隔） */
    wchar_t *error;         /* 失败：中文原因 */
} NetResult;

typedef struct SettingsState {
    HWND    hwnd;
    HWND    owner;
    HWND    picker;          /* 模型选择窗；非空表示它正开着 */
    UINT    dpi;
    HFONT   font;

    Config  edited;
    BOOL    saved;
    BOOL    done;

    int     client_w;
    int     client_h;

    /* 正在录制快捷键：0 = 没在录，否则是对应的编辑框 ID */
    int     recording_id;

    /* 网络请求进行中，防止重复点击 */
    BOOL    net_busy;
    HANDLE  net_thread;

    /* 深色主题的画刷，窗口销毁时释放 */
    HBRUSH  bg_brush;
    HBRUSH  field_brush;

    /* 底部 GitHub 图标尺寸（位置在 WM_PAINT 里按左下角算） */
    int     footer_icon;

    /* 图标和版本号的可点击区域（客户区坐标），由 WM_PAINT 更新 */
    RECT    footer_icon_rect;
    RECT    footer_text_rect;
    BOOL    footer_hover;
} SettingsState;

static SettingsState g_st;
static BOOL g_class_ready = FALSE;

/* ---------- 工具 ---------- */

static int scale(int value)
{
    UINT dpi = g_st.dpi ? g_st.dpi : 96;
    return MulDiv(value, (int)dpi, 96);
}

static HFONT make_ui_font(UINT dpi)
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);

    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        ncm.lfMessageFont.lfHeight = -MulDiv(9, (int)dpi, 72);
        return CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static HWND add_control(HWND parent, const wchar_t *cls, const wchar_t *text,
                        DWORD style, int x, int y, int w, int h, int id)
{
    HWND control = CreateWindowExW(
        0, cls, text,
        WS_CHILD | WS_VISIBLE | style,
        x, y, w, h,
        parent, (HMENU)(INT_PTR)id, g_app.instance, NULL);
    if (control != NULL) {
        if (g_st.font != NULL) {
            SendMessageW(control, WM_SETFONT, (WPARAM)g_st.font, TRUE);
        }
        dark_mode_apply_control(control);
    }
    return control;
}

static void get_edit_text(int id, wchar_t *out, size_t count)
{
    if (out == NULL || count == 0) return;
    out[0] = 0;
    HWND edit = GetDlgItem(g_st.hwnd, id);
    if (edit != NULL) GetWindowTextW(edit, out, (int)count);
}

/* 去掉首尾空白，就地修改；返回新起点 */
static wchar_t *trim_in_place(wchar_t *text)
{
    if (text == NULL) return NULL;
    while (*text == L' ' || *text == L'\t') text++;
    wchar_t *end = text + wcslen(text);
    while (end > text && (end[-1] == L' ' || end[-1] == L'\t')) *--end = 0;
    return text;
}

static void set_status(const wchar_t *text)
{
    SetDlgItemTextW(g_st.hwnd, IDC_LBL_STATUS, text ? text : L"");
}

/* 复制一份宽字符串（跨线程传参用，必须有自己的副本） */
static wchar_t *dup_wstr(const wchar_t *src)
{
    if (src == NULL) return NULL;
    size_t bytes = (wcslen(src) + 1) * sizeof(wchar_t);
    wchar_t *out = (wchar_t *)heap_alloc(bytes);
    if (out != NULL) memcpy(out, src, bytes);
    return out;
}

/* 把编辑框里的值读出来（去掉空白）。返回 FALSE 表示空。 */
static BOOL read_trimmed(int id, wchar_t *buffer, size_t count)
{
    get_edit_text(id, buffer, count);
    wchar_t *trimmed = trim_in_place(buffer);
    if (trimmed != buffer) {
        memmove(buffer, trimmed, (wcslen(trimmed) + 1) * sizeof(wchar_t));
    }
    return buffer[0] != 0;
}

/* ---------- 外观滑块 ---------- */

static void update_slider_values(void)
{
    wchar_t text[32];

    int mask = (int)SendDlgItemMessageW(g_st.hwnd, IDC_SLD_MASK,
                                        TBM_GETPOS, 0, 0);
    _snwprintf_s(text, 32, _TRUNCATE, L"%d%%", mask);
    SetDlgItemTextW(g_st.hwnd, IDC_VAL_MASK, text);

    int overlay = (int)SendDlgItemMessageW(g_st.hwnd, IDC_SLD_OVERLAY,
                                           TBM_GETPOS, 0, 0);
    _snwprintf_s(text, 32, _TRUNCATE, L"%d%%", overlay);
    SetDlgItemTextW(g_st.hwnd, IDC_VAL_OVERLAY, text);

    int font = (int)SendDlgItemMessageW(g_st.hwnd, IDC_SLD_FONT,
                                        TBM_GETPOS, 0, 0);
    _snwprintf_s(text, 32, _TRUNCATE, L"%d", font);
    SetDlgItemTextW(g_st.hwnd, IDC_VAL_FONT, text);
}

/* ---------- 快捷键录制 ---------- */

static BOOL is_modifier_vk(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT ||
           vk == VK_LWIN || vk == VK_RWIN;
}

/* 编辑框子类化（始终安装）：
 *   - 拿到焦点 -> 通知父窗口进入录制
 *   - 录制期间吞掉字符，把按键转发给父窗口（否则 EDIT 自己吃掉 WM_KEYDOWN）
 * 不在 start_recording 里装子类，是为了让"获得焦点"这个事件本身能被看到。 */
static LRESULT CALLBACK hotkey_edit_proc(HWND hwnd, UINT msg,
                                         WPARAM wparam, LPARAM lparam,
                                         UINT_PTR id_subclass, DWORD_PTR ref)
{
    (void)id_subclass;
    (void)ref;

    switch (msg) {
    case WM_SETFOCUS:
        PostMessageW(g_st.hwnd, WM_APP + 21, (WPARAM)GetDlgCtrlID(hwnd), 0);
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_st.recording_id != 0) {
            SendMessageW(g_st.hwnd, msg, wparam, lparam);
            return 0;
        }
        break;

    case WM_CHAR:
    case WM_SYSCHAR:
        if (g_st.recording_id != 0) return 0;   /* 录制期间不要塞字符 */
        break;

    case WM_LBUTTONDOWN:
        /* 用户点进输入框就进入录制；不依赖 WM_SETFOCUS
           （从别的线程 SetFocus 不会产生焦点通知） */
        PostMessageW(g_st.hwnd, WM_APP + 21, (WPARAM)GetDlgCtrlID(hwnd), 0);
        break;

    default:
        break;
    }

    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

static void start_recording(int edit_id)
{
    if (g_st.recording_id == edit_id) return;

    g_st.recording_id = edit_id;

    /* 录制期间把编辑框设成只读，避免用户同时打字 */
    HWND edit = GetDlgItem(g_st.hwnd, edit_id);
    if (edit != NULL) {
        SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    }

    SetDlgItemTextW(g_st.hwnd, IDC_LBL_HINT,
                    L"请按下想要的组合键（需包含 Ctrl / Alt / Shift / Win），Esc 取消");
    SetDlgItemTextW(g_st.hwnd, IDC_LBL_STATUS, L"");
}

static void stop_recording(void)
{
    if (g_st.recording_id == 0) return;

    HWND edit = GetDlgItem(g_st.hwnd, g_st.recording_id);
    if (edit != NULL) {
        SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
    }

    g_st.recording_id = 0;
    SetDlgItemTextW(g_st.hwnd, IDC_LBL_HINT,
                    L"格式如 ctrl+alt+d。点击输入框后直接按组合键即可录制。");

    /* 录制过程中按下的组合也可能已入队，收尾时再清一次 */
    hotkey_drain_pending(g_app.hwnd);
}

/* 把当前按下的键转成 "ctrl+alt+d"；返回 FALSE 表示还不构成完整组合。
   用 GetAsyncKeyState 而不是 GetKeyState：后者只反映本线程队列里的按键，
   在输入框拿到焦点、按键消息被控件消费时读不到修饰键状态。 */
static BOOL capture_combination(UINT vk, wchar_t *out, size_t count)
{
    if (vk == VK_ESCAPE) return FALSE;

    UINT mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
        mods |= MOD_WIN;
    }

    /* 只按下修饰键时先不写入 */
    if (mods == 0) return FALSE;

    hotkey_format(mods, vk, out, count);
    return out[0] != 0;
}

/* ---------- 网络 ---------- */

static DWORD WINAPI net_thread(LPVOID param)
{
    NetJob *job = (NetJob *)param;

    NetResult *result = (NetResult *)heap_alloc(sizeof(*result));
    if (result != NULL) {
        result->kind = job->kind;
        if (job->kind == NET_MODELS) {
            translator_fetch_models(job->base_url, job->api_key,
                                    job->timeout_sec,
                                    &result->text, &result->error);
        } else {
            result->error = translator_test_connection(job->base_url, job->api_key,
                                                       job->model,
                                                       job->timeout_sec);
        }
    }

    HWND target = job->hwnd;
    heap_free(job->base_url);
    heap_free(job->api_key);
    heap_free(job->model);
    heap_free(job);

    PostMessageW(target, WM_SETTINGS_NET_DONE, 0, (LPARAM)result);
    return 0;
}

static void start_net_job(int kind)
{
    if (g_st.net_busy) {
        set_status(L"上一个请求还没结束，请稍候");
        return;
    }

    wchar_t base[512], key[512], model[128];

    if (!read_trimmed(IDC_EDIT_BASE, base, 512)) {
        set_status(L"请先填写 API 地址");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_BASE));
        return;
    }
    if (_wcsnicmp(base, L"http://", 7) != 0 &&
        _wcsnicmp(base, L"https://", 8) != 0) {
        set_status(L"API 地址必须以 http:// 或 https:// 开头");
        return;
    }

    read_trimmed(IDC_EDIT_KEY, key, 512);
    read_trimmed(IDC_EDIT_MODEL, model, 128);

    if (kind == NET_TEST && key[0] == 0) {
        set_status(L"请先填写 API Key");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_KEY));
        return;
    }
    if (kind == NET_TEST && model[0] == 0) {
        set_status(L"请先填写模型名");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_MODEL));
        return;
    }

    NetJob *job = (NetJob *)heap_alloc(sizeof(*job));
    if (job == NULL) {
        set_status(L"内存不足");
        return;
    }

    job->kind = kind;
    job->hwnd = g_st.hwnd;
    job->timeout_sec = 20;
    job->base_url = dup_wstr(base);
    job->api_key = dup_wstr(key);
    job->model = dup_wstr(model);

    if (job->base_url == NULL || job->api_key == NULL || job->model == NULL) {
        heap_free(job->base_url);
        heap_free(job->api_key);
        heap_free(job->model);
        heap_free(job);
        set_status(L"内存不足");
        return;
    }

    g_st.net_busy = TRUE;
    g_st.net_thread = CreateThread(NULL, 0, net_thread, job, 0, NULL);
    if (g_st.net_thread == NULL) {
        g_st.net_busy = FALSE;
        heap_free(job->base_url);
        heap_free(job->api_key);
        heap_free(job->model);
        heap_free(job);
        set_status(L"无法启动网络线程");
        return;
    }

    EnableWindow(GetDlgItem(g_st.hwnd, IDC_BTN_TEST), FALSE);
    EnableWindow(GetDlgItem(g_st.hwnd, IDC_BTN_MODELS), FALSE);
    set_status(kind == NET_TEST ? L"正在测试连接…" : L"正在拉取模型列表…");
}

/* 工作线程把结果送回来 */
static void on_net_done(NetResult *result)
{
    g_st.net_busy = FALSE;
    if (g_st.net_thread != NULL) {
        CloseHandle(g_st.net_thread);
        g_st.net_thread = NULL;
    }

    if (g_st.hwnd != NULL) {
        EnableWindow(GetDlgItem(g_st.hwnd, IDC_BTN_TEST), TRUE);
        EnableWindow(GetDlgItem(g_st.hwnd, IDC_BTN_MODELS), TRUE);
    }

    if (result == NULL) {
        set_status(L"请求失败：内部错误");
        return;
    }

    if (result->error != NULL) {
        set_status(result->error);
        LOG_WARN("settings", L"网络操作失败：%ls", result->error);
    } else if (result->kind == NET_TEST) {
        set_status(L"连接成功，配置可用");
    } else {
        /* 模型列表：拿第一条填进模型框，其余弹窗让用户选 */
        wchar_t *first = result->text;
        wchar_t *nl = wcschr(result->text, L'\n');
        if (nl != NULL) *nl = 0;

        SetDlgItemTextW(g_st.hwnd, IDC_EDIT_MODEL, first);
        set_status(L"已填入第一个模型，可在下拉里换");

        if (nl != NULL) {
            *nl = L'\n';
            /* 用简单列表让用户挑选 */
            wchar_t *choice = (wchar_t *)heap_alloc(
                (wcslen(result->text) + 1) * sizeof(wchar_t));
            if (choice != NULL) {
                wcscpy(choice, result->text);
                /* 把换行换成 \0，末尾再加一个 \0 结束列表 */
                int count = 0;
                for (wchar_t *p = choice; *p != 0; p++) {
                    if (*p == L'\n') { *p = 0; count++; }
                }
                int total = count + 1;      /* 换行数 + 1 = 条目数 */
                wchar_t **items = (wchar_t **)heap_alloc(
                    sizeof(wchar_t *) * (size_t)(total + 1));
                if (items != NULL) {
                    items[0] = choice;
                    for (int i = 1; i < total; i++) {
                        items[i] = items[i - 1] + wcslen(items[i - 1]) + 1;
                    }
                    items[total] = NULL;

                    int picked = settings_choose_model(g_st.hwnd,
                                                       (const wchar_t **)items);
                    if (picked >= 0 && picked < total) {
                        SetDlgItemTextW(g_st.hwnd, IDC_EDIT_MODEL, items[picked]);
                    }
                    heap_free(items);
                }
                heap_free(choice);
            }
        }
    }

    heap_free(result->text);
    heap_free(result->error);
    heap_free(result);
}

/* 录制到组合键后：先试注册，可用才写进输入框。
   不可用就保持录制状态并提示原因，用户可以直接重按。 */
static void commit_recorded_hotkey(const wchar_t *combo)
{
    int slot = (g_st.recording_id == IDC_EDIT_EXITKEY)
               ? HOTKEY_SLOT_EXIT : HOTKEY_SLOT_CAPTURE;

    const wchar_t *reason = NULL;
    if (!hotkey_probe(g_app.hwnd, combo, slot, &reason)) {
        SetDlgItemTextW(g_st.hwnd, IDC_LBL_STATUS,
                        reason ? reason : L"该组合不可用");
        /* 留在录制模式，让用户重新按 */
        return;
    }

    SetDlgItemTextW(g_st.hwnd, g_st.recording_id, combo);
    stop_recording();
}

/* ---------- 校验与保存 ---------- */

static BOOL validate_and_save(void)
{
    /* 保存前必须先退出录制，否则热键还处于暂停状态 */
    stop_recording();

    wchar_t base[512], key[512], model[128], hotkey[64], exitkey[64], lang[64];

    if (!read_trimmed(IDC_EDIT_BASE, base, 512)) {
        set_status(L"API 地址不能为空");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_BASE));
        return FALSE;
    }
    if (_wcsnicmp(base, L"http://", 7) != 0 &&
        _wcsnicmp(base, L"https://", 8) != 0) {
        set_status(L"API 地址必须以 http:// 或 https:// 开头");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_BASE));
        return FALSE;
    }

    read_trimmed(IDC_EDIT_KEY, key, 512);

    if (!read_trimmed(IDC_EDIT_MODEL, model, 128)) {
        set_status(L"模型名不能为空");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_MODEL));
        return FALSE;
    }

    read_trimmed(IDC_EDIT_LANG, lang, 64);
    if (lang[0] == 0) {
        set_status(L"目标语言不能为空");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_LANG));
        return FALSE;
    }

    if (!read_trimmed(IDC_EDIT_HOTKEY, hotkey, 64)) {
        set_status(L"截图热键不能为空");
        SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_HOTKEY));
        return FALSE;
    }

    /* 退出热键不是必填项：用户只改截图热键时，这里空着就用配置里的原值 */
    if (!read_trimmed(IDC_EDIT_EXITKEY, exitkey, 64)) {
        wstr_copy(exitkey, 64,
                  g_st.edited.exit_hotkey[0] ? g_st.edited.exit_hotkey
                                             : L"ctrl+alt+shift+q");
        SetDlgItemTextW(g_st.hwnd, IDC_EDIT_EXITKEY, exitkey);
    }

    {
        UINT mods_a = 0, vk_a = 0;
        UINT mods_b = 0, vk_b = 0;
        if (!hotkey_parse(hotkey, &mods_a, &vk_a)) {
            set_status(L"截图热键格式无法识别，例如 ctrl+alt+d");
            SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_HOTKEY));
            return FALSE;
        }
        if (!hotkey_parse(exitkey, &mods_b, &vk_b)) {
            set_status(L"退出热键格式无法识别，例如 ctrl+alt+shift+q");
            SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_EXITKEY));
            return FALSE;
        }
        if (mods_a == mods_b && vk_a == vk_b) {
            set_status(L"两个热键不能相同");
            SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_EXITKEY));
            return FALSE;
        }
    }

    /* 真正试着注册一次，提前发现"被其他程序占用"。
       注意要在校验顺序上放在格式检查之后，否则会给出误导性的原因。 */
    {
        const wchar_t *reason = NULL;
        if (!hotkey_probe(g_app.hwnd, hotkey, HOTKEY_SLOT_CAPTURE, &reason)) {
            wchar_t text[256];
            _snwprintf_s(text, 256, _TRUNCATE, L"截图热键不可用：%ls",
                         reason ? reason : L"未知原因");
            set_status(text);
            LOG_INFO("settings", L"截图热键校验失败：%ls", text);
            SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_HOTKEY));
            return FALSE;
        }
        if (!hotkey_probe(g_app.hwnd, exitkey, HOTKEY_SLOT_EXIT, &reason)) {
            wchar_t text[256];
            _snwprintf_s(text, 256, _TRUNCATE, L"退出热键不可用：%ls",
                         reason ? reason : L"未知原因");
            set_status(text);
            SetFocus(GetDlgItem(g_st.hwnd, IDC_EDIT_EXITKEY));
            return FALSE;
        }
    }

    /* 写进编辑副本 */
    wstr_copy(g_st.edited.api_base_url, 512, base);
    wstr_copy(g_st.edited.api_key,      512, key);
    wstr_copy(g_st.edited.model,        128, model);
    wstr_copy(g_st.edited.target_language, 64, lang);
    wstr_copy(g_st.edited.hotkey,        64, hotkey);
    wstr_copy(g_st.edited.exit_hotkey,   64, exitkey);

    g_st.edited.mask_opacity =
        (float)SendDlgItemMessageW(g_st.hwnd, IDC_SLD_MASK, TBM_GETPOS, 0, 0) / 100.0f;
    g_st.edited.overlay_opacity =
        (float)SendDlgItemMessageW(g_st.hwnd, IDC_SLD_OVERLAY, TBM_GETPOS, 0, 0) / 100.0f;
    g_st.edited.font_size =
        (int)SendDlgItemMessageW(g_st.hwnd, IDC_SLD_FONT, TBM_GETPOS, 0, 0);

    /* 兜住范围，和 config_load 的钳制保持一致 */
    if (g_st.edited.mask_opacity < 0.05f) g_st.edited.mask_opacity = 0.05f;
    if (g_st.edited.overlay_opacity < 0.30f) g_st.edited.overlay_opacity = 0.30f;
    if (g_st.edited.font_size < 8)  g_st.edited.font_size = 8;
    if (g_st.edited.font_size > 40) g_st.edited.font_size = 40;

    if (!config_save(&g_st.edited, g_app.config_path)) {
        set_status(L"保存失败，详情见日志文件");
        return FALSE;
    }

    g_st.saved = TRUE;
    return TRUE;
}

/* ---------- 布局 ---------- */

static void build_layout(HWND hwnd)
{
    const int pad     = scale(10);
    const int group   = scale(16);      /* 分组标题行高 */
    const int row     = scale(22);
    const int gap     = scale(4);
    const int line_h  = scale(15);
    const int label_w = scale(76);
    const int edit_w  = scale(320);
    const int btn_w   = scale(76);
    const int btn_h   = scale(24);
    const int val_w   = scale(42);

    int x_left  = pad;
    int x_field = pad + label_w + scale(8);
    int y       = pad;

    /* 外观分组 */
    add_control(hwnd, L"STATIC", L"外观", SS_LEFT,
                x_left, y, scale(200), line_h, IDC_LBL_MASK);
    y += group;

    {
        int sld_x = x_field;
        int sld_w = edit_w - val_w - gap;

        add_control(hwnd, L"STATIC", L"截图遮罩透明度", SS_LEFT,
                    x_left, y + scale(4), label_w, line_h, 0);
        add_control(hwnd, L"msctls_trackbar32", L"",
                    WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS | TBS_NOTHUMB * 0,
                    sld_x, y, sld_w, scale(20), IDC_SLD_MASK);
        SendDlgItemMessageW(hwnd, IDC_SLD_MASK, TBM_SETRANGE, TRUE,
                            MAKELPARAM(5, 100));
        SendDlgItemMessageW(hwnd, IDC_SLD_MASK, TBM_SETTICFREQ, 25, 0);
        add_control(hwnd, L"STATIC", L"", SS_RIGHT,
                    sld_x + sld_w + gap, y + scale(4), val_w, line_h, IDC_VAL_MASK);
        y += scale(20) + gap;
    }

    {
        int sld_x = x_field;
        int sld_w = edit_w - val_w - gap;

        add_control(hwnd, L"STATIC", L"译文背景透明度", SS_LEFT,
                    x_left, y + scale(4), label_w, line_h, 0);
        add_control(hwnd, L"msctls_trackbar32", L"",
                    WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                    sld_x, y, sld_w, scale(20), IDC_SLD_OVERLAY);
        SendDlgItemMessageW(hwnd, IDC_SLD_OVERLAY, TBM_SETRANGE, TRUE,
                            MAKELPARAM(30, 100));
        SendDlgItemMessageW(hwnd, IDC_SLD_OVERLAY, TBM_SETTICFREQ, 25, 0);
        add_control(hwnd, L"STATIC", L"", SS_RIGHT,
                    sld_x + sld_w + gap, y + scale(4), val_w, line_h, IDC_VAL_OVERLAY);
        y += scale(20) + gap;
    }

    {
        int sld_x = x_field;
        int sld_w = edit_w - val_w - gap;

        add_control(hwnd, L"STATIC", L"译文字号", SS_LEFT,
                    x_left, y + scale(4), label_w, line_h, 0);
        add_control(hwnd, L"msctls_trackbar32", L"",
                    WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                    sld_x, y, sld_w, scale(20), IDC_SLD_FONT);
        SendDlgItemMessageW(hwnd, IDC_SLD_FONT, TBM_SETRANGE, TRUE,
                            MAKELPARAM(8, 40));
        SendDlgItemMessageW(hwnd, IDC_SLD_FONT, TBM_SETTICFREQ, 4, 0);
        add_control(hwnd, L"STATIC", L"", SS_RIGHT,
                    sld_x + sld_w + gap, y + scale(4), val_w, line_h, IDC_VAL_FONT);
        y += scale(20) + gap + scale(2);
    }

    /* 翻译服务分组 */
    add_control(hwnd, L"STATIC", L"翻译服务", SS_LEFT,
                x_left, y, scale(200), line_h, 0);

    add_control(hwnd, L"BUTTON", L"测试服务",
                WS_TABSTOP | BS_PUSHBUTTON,
                x_field + edit_w - btn_w, y - scale(3), btn_w, btn_h, IDC_BTN_TEST);
    y += group + scale(2);

    add_control(hwnd, L"STATIC", L"接口地址", SS_LEFT,
                x_left, y + scale(4), label_w, line_h, IDC_LBL_BASE);
    add_control(hwnd, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                x_field, y, edit_w, row, IDC_EDIT_BASE);
    y += row + gap;

    add_control(hwnd, L"STATIC", L"API Key", SS_LEFT,
                x_left, y + scale(4), label_w, line_h, IDC_LBL_KEY);
    add_control(hwnd, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
                x_field, y, edit_w, row, IDC_EDIT_KEY);
    y += row + gap;

    add_control(hwnd, L"STATIC", L"模型", SS_LEFT,
                x_left, y + scale(4), label_w, line_h, IDC_LBL_MODEL);
    add_control(hwnd, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                x_field, y, edit_w - btn_w - gap, row, IDC_EDIT_MODEL);
    add_control(hwnd, L"BUTTON", L"拉取列表",
                WS_TABSTOP | BS_PUSHBUTTON,
                x_field + edit_w - btn_w, y, btn_w, row, IDC_BTN_MODELS);
    y += row + gap;

    add_control(hwnd, L"STATIC", L"目标语言", SS_LEFT,
                x_left, y + scale(4), label_w, line_h, IDC_LBL_LANG);
    add_control(hwnd, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                x_field, y, edit_w, row, IDC_EDIT_LANG);
    y += row + gap + scale(4);

    /* 快捷键分组 */
    add_control(hwnd, L"STATIC", L"快捷键", SS_LEFT,
                x_left, y, scale(200), line_h, 0);
    y += group;

    add_control(hwnd, L"STATIC", L"截图翻译", SS_LEFT,
                x_left, y + scale(4), label_w, line_h, IDC_LBL_HOTKEY);
    add_control(hwnd, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                x_field, y, edit_w, row, IDC_EDIT_HOTKEY);
    y += row + gap;

    add_control(hwnd, L"STATIC", L"退出程序", SS_LEFT,
                x_left, y + scale(4), label_w, line_h, IDC_LBL_EXITKEY);
    add_control(hwnd, L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                x_field, y, edit_w, row, IDC_EDIT_EXITKEY);
    y += row + scale(4);

    add_control(hwnd, L"STATIC", L"点击输入框后直接按组合键录制，Esc 取消。",
                SS_LEFT, x_field, y, edit_w, line_h, IDC_LBL_HINT);
    y += line_h + scale(10);

    /* 状态行：两行高，避免文字变化时窗口跳动 */
    add_control(hwnd, L"STATIC", L"",
                SS_LEFT | SS_ENDELLIPSIS,
                x_left, y, edit_w + label_w, line_h * 2, IDC_LBL_STATUS);
    y += line_h * 2 + scale(8);

    /* 底部一行：右边按钮；GitHub 图标和版本号贴在右下角，由 WM_PAINT 绘制 */
    {
        int row_h = btn_h > scale(20) ? btn_h : scale(20);

        /* 图标尺寸；实际位置在 WM_PAINT 里按右下角计算 */
        g_st.footer_icon = scale(16);

        int btn_x = x_field + edit_w - btn_w * 2 - gap;
        add_control(hwnd, L"BUTTON", L"取消",
                    WS_TABSTOP | BS_PUSHBUTTON,
                    btn_x, y, btn_w, btn_h, IDC_BTN_CANCEL);
        add_control(hwnd, L"BUTTON", L"保存",
                    WS_TABSTOP | BS_DEFPUSHBUTTON,
                    btn_x + btn_w + gap, y, btn_w, btn_h, IDC_BTN_SAVE);
        y += row_h + pad;
    }

    g_st.client_w = x_field + edit_w + pad;
    g_st.client_h = y;

    /* 客户区固定成布局尺寸（算进标题栏与边框） */
    {
        RECT wr, cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        int frame_w = (wr.right - wr.left) - (cr.right - cr.left);
        int frame_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);
        SetWindowPos(hwnd, NULL, 0, 0,
                     g_st.client_w + frame_w,
                     g_st.client_h + frame_h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static void center_window(HWND hwnd, HWND owner)
{
    RECT area;
    if (owner != NULL && IsWindowVisible(owner)) {
        GetWindowRect(owner, &area);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi)) {
            area = mi.rcWork;
        } else {
            area.left = 0;
            area.top = 0;
            area.right = GetSystemMetrics(SM_CXSCREEN);
            area.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
    }

    RECT wr;
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    int x = area.left + ((area.right - area.left) - w) / 2;
    int y = area.top + ((area.bottom - area.top) - h) / 2;

    SetWindowPos(hwnd, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 从当前配置填充控件 */
static void fill_controls(const Config *cfg)
{
    SetDlgItemTextW(g_st.hwnd, IDC_EDIT_BASE,    cfg->api_base_url);
    SetDlgItemTextW(g_st.hwnd, IDC_EDIT_KEY,     cfg->api_key);
    SetDlgItemTextW(g_st.hwnd, IDC_EDIT_MODEL,   cfg->model);
    SetDlgItemTextW(g_st.hwnd, IDC_EDIT_LANG,    cfg->target_language);
    SetDlgItemTextW(g_st.hwnd, IDC_EDIT_HOTKEY,  cfg->hotkey);
    SetDlgItemTextW(g_st.hwnd, IDC_EDIT_EXITKEY, cfg->exit_hotkey);

    SendDlgItemMessageW(g_st.hwnd, IDC_SLD_MASK, TBM_SETPOS, TRUE,
                        (LPARAM)(int)(cfg->mask_opacity * 100.0f + 0.5f));
    SendDlgItemMessageW(g_st.hwnd, IDC_SLD_OVERLAY, TBM_SETPOS, TRUE,
                        (LPARAM)(int)(cfg->overlay_opacity * 100.0f + 0.5f));
    SendDlgItemMessageW(g_st.hwnd, IDC_SLD_FONT, TBM_SETPOS, TRUE,
                        (LPARAM)cfg->font_size);

    update_slider_values();
}

/* ---------- 窗口过程 ---------- */

/* 打开项目主页。失败时静默：这只是个附带入口。 */
static void open_repo_page(void)
{
    ShellExecuteW(NULL, L"open",
                  L"https://github.com/TonySen1/screenshot-translator",
                  NULL, NULL, SW_SHOWNORMAL);
}

/* 鼠标是否落在页脚的可点击区域（图标或版本号） */
static BOOL footer_hit_test(POINT client)
{
    if (g_st.footer_icon_rect.right > g_st.footer_icon_rect.left &&
        PtInRect(&g_st.footer_icon_rect, client)) {
        return TRUE;
    }
    if (g_st.footer_text_rect.right > g_st.footer_text_rect.left &&
        PtInRect(&g_st.footer_text_rect, client)) {
        return TRUE;
    }
    return FALSE;
}

static LRESULT CALLBACK settings_wnd_proc(HWND hwnd, UINT msg,
                                          WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        SettingsState *st = (SettingsState *)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        if (dark_mode_is_enabled()) {
            st->bg_brush = CreateSolidBrush(dark_bg_color());
            st->field_brush = CreateSolidBrush(dark_field_color());
        }
        return 0;
    }

    case WM_ERASEBKGND: {
        if (!dark_mode_is_enabled() || g_st.bg_brush == NULL) break;

        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wparam, &rc, g_st.bg_brush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (dc != NULL && g_st.footer_icon > 0) {
            /* 右下角对齐：图标 + 版本号贴着客户端右下角 */
            RECT rc;
            GetClientRect(hwnd, &rc);

            wchar_t version[64];
            _snwprintf_s(version, 64, _TRUNCATE, L"v%ls", APP_VERSION);

            HFONT old_font = NULL;
            if (g_st.font != NULL) {
                old_font = (HFONT)SelectObject(dc, g_st.font);
            }

            /* 先量版本号宽度，再整体右对齐 */
            SIZE text_size;
            GetTextExtentPoint32W(dc, version, (int)wcslen(version), &text_size);

            int margin = g_st.footer_icon;
            int icon = g_st.footer_icon;
            int gap_px = icon / 3;

            /* 放左下角：右下角被保存/取消按钮占着 */
            int block_x = margin;
            int block_y = rc.bottom - margin - icon;
            if (block_x < 0) block_x = 0;
            if (block_y < 0) block_y = 0;

            /* GitHub 标志用当前文字色，和版本号保持同一视觉层级 */
            unsigned int icon_color = dark_mode_is_enabled()
                                      ? 0xFF9A9A9A : 0xFF707070;
            gdiplus_draw_github_mark(dc, block_x, block_y, icon, icon_color);

            /* 记下两块热区，供点击时判断 */
            g_st.footer_icon_rect.left   = block_x;
            g_st.footer_icon_rect.top    = block_y;
            g_st.footer_icon_rect.right  = block_x + icon;
            g_st.footer_icon_rect.bottom = block_y + icon;

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, dark_mode_is_enabled()
                             ? dark_fg_dim_color()
                             : GetSysColor(COLOR_GRAYTEXT));

            RECT tr;
            tr.left = block_x + icon + gap_px;
            tr.top = block_y + (icon - (int)text_size.cy) / 2;
            tr.right = rc.right;
            tr.bottom = tr.top + text_size.cy;
            DrawTextW(dc, version, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

            g_st.footer_text_rect.left   = tr.left;
            g_st.footer_text_rect.top    = tr.top;
            g_st.footer_text_rect.right  = tr.left + (int)text_size.cx;
            g_st.footer_text_rect.bottom = tr.bottom;

            if (old_font != NULL) SelectObject(dc, old_font);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT client = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        if (footer_hit_test(client)) {
            LOG_INFO("settings", L"打开项目主页");
            open_repo_page();
            return 0;
        }
        break;
    }

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(hwnd, &cursor);
            if (footer_hit_test(cursor)) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wparam;

        if (!dark_mode_is_enabled()) {
            SetBkMode(dc, TRANSPARENT);
            if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
            }
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

        if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
            /* 输入框：深色底 + 浅色字 */
            SetTextColor(dc, dark_fg_color());
            SetBkColor(dc, dark_field_color());
            if (g_st.field_brush == NULL) {
                g_st.field_brush = CreateSolidBrush(dark_field_color());
            }
            return (LRESULT)g_st.field_brush;
        }

        /* 静态文本 / 分组标题：跟窗口底色一致，文字透明 */
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark_fg_color());
        return (LRESULT)g_st.bg_brush;
    }

    case WM_HSCROLL: {
        /* 滑块变化时刷新右侧数值 */
        HWND slider = (HWND)lparam;
        if (slider == GetDlgItem(hwnd, IDC_SLD_MASK) ||
            slider == GetDlgItem(hwnd, IDC_SLD_OVERLAY) ||
            slider == GetDlgItem(hwnd, IDC_SLD_FONT)) {
            update_slider_values();
        }
        return 0;
    }

    case WM_SETTINGS_NET_DONE:
        on_net_done((NetResult *)lparam);
        return 0;

    case WM_APP + 21:
        /* 快捷键输入框拿到焦点，进入录制模式 */
        if ((int)wparam == IDC_EDIT_HOTKEY ||
            (int)wparam == IDC_EDIT_EXITKEY) {
            start_recording((int)wparam);
        }
        return 0;

    case WM_KEYDOWN: {
        /* 录制模式：拦截按键，不让编辑框处理 */
        if (g_st.recording_id != 0) {
            if (wparam == VK_ESCAPE) {
                stop_recording();
                return 0;
            }
            if (is_modifier_vk((UINT)wparam)) return 0;

            wchar_t combo[64];
            if (capture_combination((UINT)wparam, combo, 64)) {
                commit_recorded_hotkey(combo);
            }
            return 0;
        }
        break;
    }

    case WM_SYSKEYDOWN: {
        /* Alt 组合会走 WM_SYSKEYDOWN */
        if (g_st.recording_id != 0) {
            if (is_modifier_vk((UINT)wparam)) return 0;

            wchar_t combo[64];
            if (capture_combination((UINT)wparam, combo, 64)) {
                commit_recorded_hotkey(combo);
            }
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_BTN_SAVE:
            if (validate_and_save()) {
                LOG_INFO("settings", L"配置已保存");
                g_st.done = TRUE;
                DestroyWindow(hwnd);
            }
            return 0;

        case IDC_BTN_CANCEL:
            stop_recording();
            g_st.done = TRUE;
            DestroyWindow(hwnd);
            return 0;

        case IDC_BTN_TEST:
            start_net_job(NET_TEST);
            return 0;

        case IDC_BTN_MODELS:
            start_net_job(NET_MODELS);
            return 0;

        /* 通知码 EN_SETFOCUS=0x0100，EN_KILLFOCUS=0x0200 */
        case IDC_EDIT_HOTKEY:
            if (HIWORD(wparam) == 0x0100) start_recording(IDC_EDIT_HOTKEY);
            else if (HIWORD(wparam) == 0x0200) stop_recording();
            return 0;

        case IDC_EDIT_EXITKEY:
            if (HIWORD(wparam) == 0x0100) start_recording(IDC_EDIT_EXITKEY);
            else if (HIWORD(wparam) == 0x0200) stop_recording();
            return 0;

        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        stop_recording();
        g_st.done = TRUE;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        /* 这里不恢复热键：恢复要等调用方拿到"是否保存"之后再做，
           否则会先按旧值注册、再被新值覆盖，两个组合撞在一起。 */
        g_st.recording_id = 0;

        if (g_st.bg_brush != NULL) {
            DeleteObject(g_st.bg_brush);
            g_st.bg_brush = NULL;
        }
        if (g_st.field_brush != NULL) {
            DeleteObject(g_st.field_brush);
            g_st.field_brush = NULL;
        }
        g_st.hwnd = NULL;
        return 0;

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
    wc.lpfnWndProc = settings_wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    /* 深色模式下不设类背景刷，改由 WM_ERASEBKGND 自己填，
       否则会先被系统刷成浅灰再被覆盖，出现闪烁 */
    wc.hbrBackground = dark_mode_is_enabled() ? NULL
                                              : (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = SETTINGS_CLASS;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERR("settings", L"注册设置窗口类失败（错误 %lu）", err);
            return FALSE;
        }
    }
    g_class_ready = TRUE;
    return TRUE;
}

/* ---------- 对外 ---------- */

void settings_init(void)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;    /* msctls_trackbar32 */
    InitCommonControlsEx(&icc);
}

BOOL settings_show(HWND owner, Config *cfg)
{
    if (cfg == NULL) return FALSE;

    if (g_st.hwnd != NULL && IsWindow(g_st.hwnd)) {
        SetForegroundWindow(g_st.hwnd);
        return FALSE;
    }

    HINSTANCE instance = g_app.instance ? g_app.instance : GetModuleHandleW(NULL);
    if (!ensure_class(instance)) return FALSE;

    memset(&g_st, 0, sizeof(g_st));
    g_st.owner = owner;
    g_st.edited = *cfg;
    g_st.dpi = get_dpi(owner);
    if (g_st.dpi == 0) g_st.dpi = 96;
    g_st.font = make_ui_font(g_st.dpi);

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        SETTINGS_CLASS, L"设置",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, scale(560), scale(720),
        owner, NULL, instance, &g_st);

    g_st.hwnd = hwnd;

    if (hwnd == NULL) {
        LOG_ERR("settings", L"创建设置窗口失败（错误 %lu）", GetLastError());
        if (g_st.font != NULL) DeleteObject(g_st.font);
        memset(&g_st, 0, sizeof(g_st));
        return FALSE;
    }

    dark_mode_apply(hwnd);
    /* 设置窗口整个生命周期内暂停全局热键：
       否则用户在随便哪个框里按 Ctrl+Alt+D 都会触发截图，
       甚至叠出多层遮罩。关闭窗口时统一恢复。 */
    hotkey_suspend(g_app.hwnd);

    build_layout(hwnd);

    /* 两个快捷键输入框装上子类：负责焦点通知和录制期间的按键转发 */
    SetWindowSubclass(GetDlgItem(hwnd, IDC_EDIT_HOTKEY),
                      hotkey_edit_proc, 1, 0);
    SetWindowSubclass(GetDlgItem(hwnd, IDC_EDIT_EXITKEY),
                      hotkey_edit_proc, 1, 0);

    fill_controls(cfg);
    center_window(hwnd, owner);

    if (owner != NULL) EnableWindow(owner, FALSE);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(GetDlgItem(hwnd, IDC_EDIT_BASE));

    LOG_INFO("settings", L"设置窗口已打开");

    MSG msg;
    while (!g_st.done && g_st.hwnd != NULL && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    /* 选择窗还开着（比如用户在它上面按了 Esc 之外的方式退出），先关掉它，
       否则它会把 owner 重新启用/禁用弄乱，也可能往已销毁的父窗口投递 */
    if (g_st.picker != NULL && IsWindow(g_st.picker)) {
        DestroyWindow(g_st.picker);
        g_st.picker = NULL;
    }

    /* 等网络线程收尾，避免它往已销毁的窗口投递 */
    if (g_st.net_thread != NULL) {
        WaitForSingleObject(g_st.net_thread, 5000);
        CloseHandle(g_st.net_thread);
        g_st.net_thread = NULL;
        g_st.net_busy = FALSE;
    }

    if (owner != NULL) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    BOOL saved = g_st.saved;
    if (saved) *cfg = g_st.edited;

    /* 恢复注册。保存时直接按新配置注册（hotkey_register_all 内部会
       先注销旧的），取消时按暂停前记住的组合恢复。
       两条路径都只注册一次，不会有旧组合残留。 */
    if (saved) {
        hotkey_abandon_suspend();
        hotkey_register_all(g_app.hwnd, cfg);
    } else {
        hotkey_resume(g_app.hwnd);
    }

    if (g_st.font != NULL) DeleteObject(g_st.font);
    memset(&g_st, 0, sizeof(g_st));

    LOG_INFO("settings", L"设置窗口已关闭（%ls）", saved ? L"已保存" : L"未修改");
    return saved;
}

/* 模型选择窗的状态。按钮点击产生的是同步 WM_COMMAND，
   不会进消息队列，所以必须由窗口过程处理，不能靠读队列。 */
typedef struct PickerState {
    HWND    hwnd;
    HWND    list;
    int     picked;
    BOOL    done;
    HBRUSH  bg_brush;
    HBRUSH  field_brush;
} PickerState;

static LRESULT CALLBACK picker_wnd_proc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam)
{
    PickerState *st = (PickerState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        st = (PickerState *)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

        if (dark_mode_is_enabled()) {
            st->bg_brush = CreateSolidBrush(dark_bg_color());
            st->field_brush = CreateSolidBrush(dark_field_color());
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        if (!dark_mode_is_enabled() || st == NULL) break;

        HDC dc = (HDC)wparam;
        if (msg == WM_CTLCOLORLISTBOX) {
            SetTextColor(dc, dark_fg_color());
            SetBkColor(dc, dark_field_color());
            return (LRESULT)st->field_brush;
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark_fg_color());
        return (LRESULT)st->bg_brush;
    }

    case WM_ERASEBKGND:
        if (dark_mode_is_enabled() && st != NULL && st->bg_brush != NULL) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wparam, &rc, st->bg_brush);
            return 1;
        }
        break;

    case WM_COMMAND:
        if (st == NULL) break;
        switch (LOWORD(wparam)) {
        case IDOK:
            if (st->list != NULL) {
                st->picked = (int)SendMessageW(st->list, LB_GETCURSEL, 0, 0);
            }
            st->done = TRUE;
            DestroyWindow(hwnd);
            return 0;

        case IDCANCEL:
            st->picked = -1;
            st->done = TRUE;
            DestroyWindow(hwnd);
            return 0;

        default:
            break;
        }
        break;

    case WM_CLOSE:
        if (st != NULL) {
            st->picked = -1;
            st->done = TRUE;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (st != NULL) {
            if (st->bg_brush != NULL) {
                DeleteObject(st->bg_brush);
                st->bg_brush = NULL;
            }
            if (st->field_brush != NULL) {
                DeleteObject(st->field_brush);
                st->field_brush = NULL;
            }
            st->hwnd = NULL;
            st->list = NULL;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* 模型列表选择框。返回选中下标，取消返回 -1。 */
int settings_choose_model(HWND owner, const wchar_t **items)
{
    if (items == NULL) return -1;

    /* 自建一个最小对话框：一个列表框 + 确定/取消 */
    enum { ID_LIST = 200 };

    int count = 0;
    while (items[count] != NULL) count++;
    if (count == 0) return -1;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = picker_wnd_proc;
    wc.hInstance = g_app.instance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = dark_mode_is_enabled() ? NULL
                                              : (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ScreenshotTranslatorModelPicker";
    RegisterClassExW(&wc);

    int dpi = (int)get_dpi(owner);
    int w = MulDiv(360, dpi, 96);
    int h = MulDiv(420, dpi, 96);

    PickerState st;
    memset(&st, 0, sizeof(st));
    st.picked = -1;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"ScreenshotTranslatorModelPicker", L"选择模型",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        owner, NULL, g_app.instance, &st);
    if (dlg == NULL) return -1;

    HFONT font = make_ui_font((UINT)dpi);
    int pad = MulDiv(12, dpi, 96);
    int btn_h = MulDiv(28, dpi, 96);

    HWND list = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY,
        pad, pad, w - MulDiv(24, dpi, 96) - MulDiv(16, dpi, 96),
        h - pad * 2 - btn_h - pad - MulDiv(32, dpi, 96),
        dlg, (HMENU)(INT_PTR)ID_LIST, g_app.instance, NULL);
    st.list = list;

    for (int i = 0; i < count; i++) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)items[i]);
    }
    SendMessageW(list, LB_SETCURSEL, 0, 0);

    int by = h - pad - btn_h - MulDiv(32, dpi, 96);
    int bw = MulDiv(84, dpi, 96);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"确定",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        w - pad - bw * 2 - MulDiv(8, dpi, 96), by, bw, btn_h,
        dlg, (HMENU)(INT_PTR)IDOK, g_app.instance, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"取消",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        w - pad - bw, by, bw, btn_h,
        dlg, (HMENU)(INT_PTR)IDCANCEL, g_app.instance, NULL);

    if (font != NULL) {
        SendMessageW(list, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
    }

    dark_mode_apply(dlg);
    center_window(dlg, owner);

    if (owner != NULL) EnableWindow(owner, FALSE);

    /* 记在设置窗口的状态里，方便设置窗关闭时兜底 */
    if (owner != NULL) {
        SettingsState *owner_st =
            (SettingsState *)GetWindowLongPtrW(owner, GWLP_USERDATA);
        if (owner_st != NULL) owner_st->picker = dlg;
    }

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(list);

    /* 内层循环：退出条件由窗口过程设置 */
    MSG msg;
    while (!st.done && IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    /* 循环被外部打断（比如消息循环结束）时窗口可能还在 */
    if (IsWindow(dlg)) DestroyWindow(dlg);

    if (font != NULL) DeleteObject(font);

    if (owner != NULL) {
        SettingsState *owner_st =
            (SettingsState *)GetWindowLongPtrW(owner, GWLP_USERDATA);
        if (owner_st != NULL) owner_st->picker = NULL;

        /* 无论怎么退出，都必须把 owner 恢复回来，否则整页点了会响 */
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    return st.picked;
}
