/* hotkey.c —— 全局热键解析与注册 */
#include "hotkey.h"
#include "app.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static BOOL g_capture_registered = FALSE;
static BOOL g_exit_registered = FALSE;

/* 本进程已注册的组合，探测时用来区分"被别人占用"和"被自己占用" */
static UINT g_registered_mods[2];
static UINT g_registered_vk[2];
static BOOL g_registered_valid[2];

/* 暂停期间记住的组合，恢复时原样注册回去 */
static wchar_t g_saved_combo[2][64];
static BOOL    g_suspended = FALSE;

/* ---------- 键名 ---------- */

typedef struct KeyName {
    const wchar_t *name;
    UINT vk;
} KeyName;

static const KeyName SPECIAL_KEYS[] = {
    { L"esc",          VK_ESCAPE },
    { L"escape",       VK_ESCAPE },
    { L"enter",        VK_RETURN },
    { L"return",       VK_RETURN },
    { L"space",        VK_SPACE },
    { L"tab",          VK_TAB },
    { L"backspace",    VK_BACK },
    { L"delete",       VK_DELETE },
    { L"del",          VK_DELETE },
    { L"insert",       VK_INSERT },
    { L"ins",          VK_INSERT },
    { L"home",         VK_HOME },
    { L"end",          VK_END },
    { L"page up",      VK_PRIOR },
    { L"pageup",       VK_PRIOR },
    { L"pgup",         VK_PRIOR },
    { L"page down",    VK_NEXT },
    { L"pagedown",     VK_NEXT },
    { L"pgdn",         VK_NEXT },
    { L"up",           VK_UP },
    { L"down",         VK_DOWN },
    { L"left",         VK_LEFT },
    { L"right",        VK_RIGHT },
    { L"print screen", VK_SNAPSHOT },
    { L"printscreen",  VK_SNAPSHOT },
    { L"pause",        VK_PAUSE },
    { L"apps",         VK_APPS },
    { L"comma",        VK_OEM_COMMA },
    { L"period",       VK_OEM_PERIOD },
    { L"slash",        VK_OEM_2 },
    { L"semicolon",    VK_OEM_1 },
    { L"minus",        VK_OEM_MINUS },
    { L"equal",        VK_OEM_PLUS },
};

static BOOL parse_key_name(const wchar_t *name, UINT *out_vk)
{
    if (name == NULL || name[0] == 0) return FALSE;

    /* F1 - F24 */
    if ((name[0] == L'f' || name[0] == L'F') && name[1] >= L'0' && name[1] <= L'9') {
        wchar_t *end = NULL;
        long n = wcstol(name + 1, &end, 10);
        if (end && *end == 0 && n >= 1 && n <= 24) {
            *out_vk = (UINT)(VK_F1 + n - 1);
            return TRUE;
        }
    }

    if (name[1] == 0) {
        wchar_t c = name[0];
        if (c >= L'a' && c <= L'z') { *out_vk = (UINT)(c - L'a' + 'A'); return TRUE; }
        if (c >= L'A' && c <= L'Z') { *out_vk = (UINT)c; return TRUE; }
        if (c >= L'0' && c <= L'9') { *out_vk = (UINT)c; return TRUE; }
    }

    for (size_t i = 0; i < sizeof(SPECIAL_KEYS) / sizeof(SPECIAL_KEYS[0]); i++) {
        if (wstr_ieq(name, SPECIAL_KEYS[i].name)) {
            *out_vk = SPECIAL_KEYS[i].vk;
            return TRUE;
        }
    }
    return FALSE;
}

BOOL hotkey_parse(const wchar_t *combo, UINT *out_mods, UINT *out_vk)
{
    if (combo == NULL || out_mods == NULL || out_vk == NULL) return FALSE;

    wchar_t buffer[128];
    wstr_copy(buffer, 128, combo);

    UINT mods = 0;
    UINT vk = 0;

    wchar_t *context = NULL;
    wchar_t *token = wcstok_s(buffer, L"+", &context);
    while (token != NULL) {
        /* 跳过空白 */
        while (*token == L' ' || *token == L'\t') token++;
        wchar_t *end = token + wcslen(token);
        while (end > token && (end[-1] == L' ' || end[-1] == L'\t')) *--end = 0;

        if (token[0] == 0) {
            token = wcstok_s(NULL, L"+", &context);
            continue;
        }

        if (wstr_ieq(token, L"ctrl") || wstr_ieq(token, L"control")) {
            mods |= MOD_CONTROL;
        } else if (wstr_ieq(token, L"alt")) {
            mods |= MOD_ALT;
        } else if (wstr_ieq(token, L"shift")) {
            mods |= MOD_SHIFT;
        } else if (wstr_ieq(token, L"win") || wstr_ieq(token, L"windows")) {
            mods |= MOD_WIN;
        } else {
            UINT parsed = 0;
            if (!parse_key_name(token, &parsed)) return FALSE;
            if (vk != 0) return FALSE;      /* 只能有一个主键 */
            vk = parsed;
        }

        token = wcstok_s(NULL, L"+", &context);
    }

    if (vk == 0) return FALSE;              /* 只有修饰键 */
    *out_mods = mods | MOD_NOREPEAT;
    *out_vk = vk;
    return TRUE;
}

void hotkey_format(UINT mods, UINT vk, wchar_t *out, size_t out_count)
{
    if (out == NULL || out_count == 0) return;
    out[0] = 0;

    wchar_t key[32];
    if (vk >= VK_F1 && vk <= VK_F24) {
        _snwprintf_s(key, 32, _TRUNCATE, L"F%u", vk - VK_F1 + 1);
    } else if (vk >= 'A' && vk <= 'Z') {
        _snwprintf_s(key, 32, _TRUNCATE, L"%lc", (wchar_t)vk);
    } else if (vk >= '0' && vk <= '9') {
        _snwprintf_s(key, 32, _TRUNCATE, L"%lc", (wchar_t)vk);
    } else {
        switch (vk) {
        case VK_ESCAPE:   wstr_copy(key, 32, L"Esc"); break;
        case VK_RETURN:   wstr_copy(key, 32, L"Enter"); break;
        case VK_SPACE:    wstr_copy(key, 32, L"Space"); break;
        case VK_TAB:      wstr_copy(key, 32, L"Tab"); break;
        case VK_BACK:     wstr_copy(key, 32, L"Backspace"); break;
        case VK_DELETE:   wstr_copy(key, 32, L"Delete"); break;
        case VK_INSERT:   wstr_copy(key, 32, L"Insert"); break;
        case VK_HOME:     wstr_copy(key, 32, L"Home"); break;
        case VK_END:      wstr_copy(key, 32, L"End"); break;
        case VK_PRIOR:    wstr_copy(key, 32, L"PageUp"); break;
        case VK_NEXT:     wstr_copy(key, 32, L"PageDown"); break;
        case VK_UP:       wstr_copy(key, 32, L"Up"); break;
        case VK_DOWN:     wstr_copy(key, 32, L"Down"); break;
        case VK_LEFT:     wstr_copy(key, 32, L"Left"); break;
        case VK_RIGHT:    wstr_copy(key, 32, L"Right"); break;
        case VK_SNAPSHOT: wstr_copy(key, 32, L"PrintScreen"); break;
        default:          _snwprintf_s(key, 32, _TRUNCATE, L"0x%02X", vk); break;
        }
    }

    _snwprintf_s(out, out_count, _TRUNCATE, L"%s%s%s%s%s",
                 (mods & MOD_CONTROL) ? L"Ctrl+" : L"",
                 (mods & MOD_ALT)     ? L"Alt+"  : L"",
                 (mods & MOD_SHIFT)   ? L"Shift+": L"",
                 (mods & MOD_WIN)     ? L"Win+"  : L"",
                 key);
}

/* ---------- 注册 ---------- */

static BOOL register_one(HWND hwnd, int id, const wchar_t *combo, const wchar_t *default_combo)
{
    UINT mods = 0, vk = 0;
    if (!hotkey_parse(combo, &mods, &vk)) {
        LOG_WARN("hotkey", L"热键「%ls」格式无法识别，改用默认值", combo);
        if (!hotkey_parse(default_combo, &mods, &vk)) return FALSE;
    }

    if (!RegisterHotKey(hwnd, id, mods, vk)) {
        DWORD err = GetLastError();
        LOG_ERR("hotkey", L"注册热键 %ls 失败（错误 %lu，可能被其他程序占用）",
                combo, err);

        /* 用户自定义的组合失败时，退回默认值再试一次 */
        if (!wstr_ieq(combo, default_combo) &&
            hotkey_parse(default_combo, &mods, &vk)) {
            if (RegisterHotKey(hwnd, id, mods, vk)) {
                wchar_t text[64];
                hotkey_format(mods, vk, text, 64);
                LOG_WARN("hotkey", L"已回退到默认热键 %ls", text);
                return TRUE;
            }
        }
        return FALSE;
    }

    wchar_t text[64];
    hotkey_format(mods & ~MOD_NOREPEAT, vk, text, 64);
    LOG_INFO("hotkey", L"已注册热键：%ls (id=%d)", text, id);

    /* 记下来，hotkey_probe 用它区分"被别人占用"和"被自己占用" */
    if (id == HOTKEY_ID_CAPTURE) {
        g_registered_mods[0] = mods;
        g_registered_vk[0] = vk;
        g_registered_valid[0] = TRUE;
    } else if (id == HOTKEY_ID_EXIT) {
        g_registered_mods[1] = mods;
        g_registered_vk[1] = vk;
        g_registered_valid[1] = TRUE;
    }
    return TRUE;
}

/* 探测用的临时热键 ID，和正式注册的两个 ID 分开，避免互相干扰 */
#define HOTKEY_ID_PROBE 100

BOOL hotkey_probe(HWND hwnd, const wchar_t *combo,
                  int slot, const wchar_t **out_reason)
{
    if (out_reason != NULL) *out_reason = NULL;

    if (hwnd == NULL || combo == NULL || combo[0] == 0) {
        if (out_reason) *out_reason = L"热键不能为空";
        return FALSE;
    }

    UINT mods = 0, vk = 0;
    if (!hotkey_parse(combo, &mods, &vk)) {
        if (out_reason) *out_reason = L"格式无法识别，例如 ctrl+alt+d";
        return FALSE;
    }

    /* 只按了修饰键的情况 hotkey_parse 已经拒掉；
       这里再拦一下"没有修饰键的单独按键"——那会抢占全局输入，
       几乎肯定是误操作。 */
    if ((mods & ~MOD_NOREPEAT) == 0) {
        if (out_reason) *out_reason = L"必须包含 Ctrl / Alt / Shift / Win";
        return FALSE;
    }

    if (RegisterHotKey(hwnd, HOTKEY_ID_PROBE, mods, vk)) {
        UnregisterHotKey(hwnd, HOTKEY_ID_PROBE);
        return TRUE;
    }

    DWORD err = GetLastError();

    /* 探测前刚用临时 ID 试过，失败说明组合被占。
       现在分辨是被谁占的：
         - 本程序同一个槽位已经注册了它 -> 等于没变，放行
         - 本程序另一个槽位占着它 -> "已被本程序的其他功能占用"
         - 都不是 -> 被别的程序占用 */
    BOOL self_same_slot = FALSE;
    BOOL self_other_slot = FALSE;

    for (int i = 0; i < 2; i++) {
        if (!g_registered_valid[i]) continue;
        if (g_registered_mods[i] != mods || g_registered_vk[i] != vk) continue;

        if (slot == HOTKEY_SLOT_ANY || i == slot) self_same_slot = TRUE;
        else self_other_slot = TRUE;
    }

    if (self_same_slot) return TRUE;

    if (out_reason != NULL) {
        if (self_other_slot) {
            *out_reason = L"该组合已被本程序的其他功能占用";
        } else if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            *out_reason = L"该组合已被其他程序占用";
        } else {
            *out_reason = L"系统拒绝注册该组合";
        }
    }
    LOG_WARN("hotkey", L"探测热键 %ls 失败（错误 %lu，self_other=%d）",
             combo, err, self_other_slot);
    return FALSE;
}

BOOL hotkey_register_all(HWND hwnd, const Config *cfg)
{
    if (hwnd == NULL || cfg == NULL) return FALSE;

    hotkey_unregister_all(hwnd);

    g_capture_registered = register_one(hwnd, HOTKEY_ID_CAPTURE,
                                        cfg->hotkey, L"ctrl+alt+d");
    g_exit_registered = register_one(hwnd, HOTKEY_ID_EXIT,
                                     cfg->exit_hotkey, L"ctrl+alt+shift+q");
    return g_capture_registered;
}

void hotkey_unregister_all(HWND hwnd)
{
    if (hwnd == NULL) return;
    if (g_capture_registered) { UnregisterHotKey(hwnd, HOTKEY_ID_CAPTURE); g_capture_registered = FALSE; }
    if (g_exit_registered)    { UnregisterHotKey(hwnd, HOTKEY_ID_EXIT);    g_exit_registered = FALSE; }
    g_registered_valid[0] = FALSE;
    g_registered_valid[1] = FALSE;
}

/* ---------- 暂停 / 恢复 ---------- */

void hotkey_suspend(HWND hwnd)
{
    if (hwnd == NULL || g_suspended) return;

    /* 记住当前组合：优先用已注册的解析结果，退回全局配置 */
    if (g_registered_valid[0]) {
        hotkey_format(g_registered_mods[0] & ~MOD_NOREPEAT,
                      g_registered_vk[0], g_saved_combo[0], 64);
    } else {
        wstr_copy(g_saved_combo[0], 64, g_app.cfg.hotkey);
    }

    if (g_registered_valid[1]) {
        hotkey_format(g_registered_mods[1] & ~MOD_NOREPEAT,
                      g_registered_vk[1], g_saved_combo[1], 64);
    } else {
        wstr_copy(g_saved_combo[1], 64, g_app.cfg.exit_hotkey);
    }

    hotkey_unregister_all(hwnd);
    g_suspended = TRUE;

    LOG_INFO("hotkey", L"热键已暂停（录制中）：%ls / %ls",
             g_saved_combo[0], g_saved_combo[1]);
}

void hotkey_abandon_suspend(void)
{
    g_suspended = FALSE;
}

void hotkey_resume(HWND hwnd)
{
    if (hwnd == NULL || !g_suspended) return;

    g_suspended = FALSE;

    register_one(hwnd, HOTKEY_ID_CAPTURE, g_saved_combo[0], L"ctrl+alt+d");
    register_one(hwnd, HOTKEY_ID_EXIT,    g_saved_combo[1], L"ctrl+alt+shift+q");

    LOG_INFO("hotkey", L"热键已恢复");
}
