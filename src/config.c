/* config.c —— config.json 读写 */
#include "config.h"
#include "util.h"

#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const wchar_t *DEFAULT_API_BASE   = L"https://api.openai.com/v1";
static const wchar_t *DEFAULT_MODEL      = L"gpt-4o-mini";
static const wchar_t *DEFAULT_HOTKEY     = L"ctrl+alt+d";
static const wchar_t *DEFAULT_EXIT_KEY   = L"ctrl+alt+shift+q";
static const wchar_t *DEFAULT_LANGUAGE   = L"中文";
static const wchar_t *DEFAULT_OVERLAY_BG = L"#1e1e1e";
static const wchar_t *DEFAULT_OVERLAY_FG = L"#ffffff";

void config_defaults(Config *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));

    wstr_copy(cfg->api_base_url, 512, DEFAULT_API_BASE);
    cfg->api_key[0] = 0;
    wstr_copy(cfg->model, 128, DEFAULT_MODEL);
    wstr_copy(cfg->hotkey, 64, DEFAULT_HOTKEY);
    wstr_copy(cfg->exit_hotkey, 64, DEFAULT_EXIT_KEY);
    wstr_copy(cfg->target_language, 64, DEFAULT_LANGUAGE);

    cfg->mask_opacity = 0.35f;
    cfg->overlay_opacity = 0.88f;
    wstr_copy(cfg->overlay_bg, 16, DEFAULT_OVERLAY_BG);
    wstr_copy(cfg->overlay_fg, 16, DEFAULT_OVERLAY_FG);

    cfg->font_size = 12;
    cfg->request_timeout = 30;
}

/* ---------- 读文件 ---------- */

static char *read_file_utf8(const wchar_t *path, DWORD *out_size)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return NULL;
    }

    DWORD n = (DWORD)size.QuadPart;
    char *buffer = (char *)heap_alloc(n + 1);
    if (buffer == NULL) { CloseHandle(file); return NULL; }

    DWORD read = 0;
    if (!ReadFile(file, buffer, n, &read, NULL)) {
        heap_free(buffer);
        CloseHandle(file);
        return NULL;
    }
    CloseHandle(file);

    buffer[read] = 0;

    /* 跳过 UTF-8 BOM（原 Python 版用 utf-8-sig 读） */
    if (read >= 3 && (unsigned char)buffer[0] == 0xEF &&
        (unsigned char)buffer[1] == 0xBB && (unsigned char)buffer[2] == 0xBF) {
        memmove(buffer, buffer + 3, read - 3 + 1);
        read -= 3;
    }

    if (out_size) *out_size = read;
    return buffer;
}

/* ---------- 取值辅助 ---------- */

static void get_string(cJSON *root, const char *key,
                       wchar_t *dst, size_t dst_count, const wchar_t *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        wchar_t *wide = u8_to_u16(item->valuestring);
        if (wide != NULL) {
            /* 空字符串也算合法（api_key 就允许空） */
            wstr_copy(dst, dst_count, wide);
            heap_free(wide);
            return;
        }
    }
    if (fallback) wstr_copy(dst, dst_count, fallback);
    else if (dst_count) dst[0] = 0;
}

static float clamp_float(double value, float low, float high)
{
    if (value != value) return low;           /* NaN */
    if (value < (double)low) return low;
    if (value > (double)high) return high;
    return (float)value;
}

static void get_float(cJSON *root, const char *key, float *dst, float fallback,
                      float low, float high)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        *dst = clamp_float(item->valuedouble, low, high);
        return;
    }
    *dst = fallback;
}

static void get_int(cJSON *root, const char *key, int *dst, int fallback, int low, int high)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        int value = (int)item->valuedouble;
        if (value < low) value = low;
        if (value > high) value = high;
        *dst = value;
        return;
    }
    *dst = fallback;
}

/* ---------- 对外 ---------- */

/* 把损坏的配置文件另存一份，免得用户的手工修改被直接覆盖掉 */
static void backup_broken(const wchar_t *path)
{
    wchar_t broken[MAX_PATH];
    _snwprintf_s(broken, MAX_PATH, _TRUNCATE, L"%s.broken", path);

    if (CopyFileW(path, broken, FALSE)) {
        LOG_WARN("config", L"损坏的配置已备份到 %ls", broken);
    } else {
        LOG_WARN("config", L"备份损坏配置失败（错误 %lu）", GetLastError());
    }
}

ConfigLoadStatus config_load(Config *cfg, const wchar_t *path)
{
    if (cfg == NULL || path == NULL) return CONFIG_LOAD_UNREADABLE;

    config_defaults(cfg);

    /* 先看文件在不在，用来区分"首次运行"和"读不了" */
    DWORD attrs = GetFileAttributesW(path);
    BOOL exists = (attrs != INVALID_FILE_ATTRIBUTES) &&
                  !(attrs & FILE_ATTRIBUTE_DIRECTORY);

    DWORD size = 0;
    char *text = read_file_utf8(path, &size);
    if (text == NULL) {
        if (!exists) {
            LOG_INFO("config", L"config.json 不存在，生成默认配置");
            config_save(cfg, path);
            return CONFIG_LOAD_CREATED;
        }
        LOG_WARN("config", L"config.json 存在但无法读取，本次使用默认值");
        return CONFIG_LOAD_UNREADABLE;
    }

    cJSON *root = cJSON_Parse(text);
    heap_free(text);

    if (root == NULL) {
        LOG_WARN("config", L"config.json 不是合法 JSON，改用默认值");
        backup_broken(path);
        config_save(cfg, path);
        return CONFIG_LOAD_CORRUPTED;
    }

    cJSON *item;

    get_string(root, "api_base_url", cfg->api_base_url, 512, DEFAULT_API_BASE);
    get_string(root, "api_key",      cfg->api_key,      512, L"");
    get_string(root, "model",        cfg->model,        128, DEFAULT_MODEL);
    get_string(root, "hotkey",       cfg->hotkey,        64, DEFAULT_HOTKEY);
    get_string(root, "exit_hotkey",  cfg->exit_hotkey,   64, DEFAULT_EXIT_KEY);
    get_string(root, "target_language", cfg->target_language, 64, DEFAULT_LANGUAGE);
    get_string(root, "overlay_bg",   cfg->overlay_bg,    16, DEFAULT_OVERLAY_BG);
    get_string(root, "overlay_fg",   cfg->overlay_fg,    16, DEFAULT_OVERLAY_FG);

    /* 字符串项兜底：空值退回默认（api_key 例外，允许为空） */
    if (cfg->api_base_url[0] == 0) wstr_copy(cfg->api_base_url, 512, DEFAULT_API_BASE);
    if (cfg->model[0] == 0)        wstr_copy(cfg->model, 128, DEFAULT_MODEL);
    if (cfg->hotkey[0] == 0)       wstr_copy(cfg->hotkey, 64, DEFAULT_HOTKEY);
    if (cfg->exit_hotkey[0] == 0)  wstr_copy(cfg->exit_hotkey, 64, DEFAULT_EXIT_KEY);
    if (cfg->target_language[0] == 0) wstr_copy(cfg->target_language, 64, DEFAULT_LANGUAGE);
    if (cfg->overlay_bg[0] == 0)   wstr_copy(cfg->overlay_bg, 16, DEFAULT_OVERLAY_BG);
    if (cfg->overlay_fg[0] == 0)   wstr_copy(cfg->overlay_fg, 16, DEFAULT_OVERLAY_FG);

    get_float(root, "mask_opacity",    &cfg->mask_opacity,    0.35f, 0.05f, 1.0f);
    get_float(root, "overlay_opacity", &cfg->overlay_opacity, 0.88f, 0.30f, 1.0f);
    get_int(root, "font_size",         &cfg->font_size,       12, 8, 40);
    get_int(root, "request_timeout",   &cfg->request_timeout, 30, 5, 300);

    /* 顺手校验颜色字符串 */
    (void)item;
    cJSON_Delete(root);

    LOG_INFO("config", L"配置已加载：model=%ls base_url=%ls key=%ls",
             cfg->model, cfg->api_base_url,
             cfg->api_key[0] ? L"已填写" : L"未填写");
    return CONFIG_LOAD_OK;
}

BOOL config_save(const Config *cfg, const wchar_t *path)
{
    if (cfg == NULL || path == NULL) return FALSE;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return FALSE;

    char *u8;

    u8 = u16_to_u8(cfg->api_base_url); cJSON_AddStringToObject(root, "api_base_url", u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->api_key);      cJSON_AddStringToObject(root, "api_key",      u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->model);        cJSON_AddStringToObject(root, "model",        u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->hotkey);       cJSON_AddStringToObject(root, "hotkey",       u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->exit_hotkey);  cJSON_AddStringToObject(root, "exit_hotkey",  u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->target_language); cJSON_AddStringToObject(root, "target_language", u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->overlay_bg);   cJSON_AddStringToObject(root, "overlay_bg",   u8 ? u8 : ""); heap_free(u8);
    u8 = u16_to_u8(cfg->overlay_fg);   cJSON_AddStringToObject(root, "overlay_fg",   u8 ? u8 : ""); heap_free(u8);

    /* float 直接转 double 会写成 0.34999999403953552，取三位小数就够 */
    cJSON_AddNumberToObject(root, "mask_opacity",
                            floor(cfg->mask_opacity * 1000.0f + 0.5f) / 1000.0);
    cJSON_AddNumberToObject(root, "overlay_opacity",
                            floor(cfg->overlay_opacity * 1000.0f + 0.5f) / 1000.0);
    cJSON_AddNumberToObject(root, "font_size",       cfg->font_size);
    cJSON_AddNumberToObject(root, "request_timeout", cfg->request_timeout);

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL) return FALSE;

    /* 写临时文件再替换，避免写到一半断电留下半个文件 */
    wchar_t temp_path[MAX_PATH];
    _snwprintf_s(temp_path, MAX_PATH, _TRUNCATE, L"%s.tmp", path);

    HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        LOG_ERR("config", L"无法写入临时配置文件（错误 %lu）", GetLastError());
        cJSON_free(text);
        return FALSE;
    }

    size_t len = strlen(text);
    DWORD written = 0;
    BOOL ok = WriteFile(file, text, (DWORD)len, &written, NULL) && written == len;
    CloseHandle(file);
    cJSON_free(text);

    if (!ok) {
        LOG_ERR("config", L"写配置文件失败（错误 %lu）", GetLastError());
        DeleteFileW(temp_path);
        return FALSE;
    }

    if (!MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOG_ERR("config", L"替换配置文件失败（错误 %lu）", GetLastError());
        DeleteFileW(temp_path);
        return FALSE;
    }

    LOG_INFO("config", L"配置已保存");
    return TRUE;
}
