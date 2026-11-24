#include "deepl.h"
#include "cJSON.h"
#include "http.h"
#include "util.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define DEEPL_BODY_LIMIT (128u * 1024u)
#define DEEPL_KEY_LIMIT 511

static wchar_t *copy_error(const wchar_t *text)
{
    size_t length = wcslen(text) + 1;
    wchar_t *copy = (wchar_t *)heap_alloc(length * sizeof(*copy));
    if (copy) wstr_copy(copy, length, text);
    return copy;
}

BOOL deepl_key_is_valid(const wchar_t *key)
{
    if (!key || !*key || wcslen(key) > DEEPL_KEY_LIMIT) return FALSE;
    for (const wchar_t *p = key; *p; p++) {
        if (*p < L'!' || *p > L'~') return FALSE;
    }
    return TRUE;
}

BOOL deepl_target_language(const wchar_t *name, char out[DEEPL_TARGET_CAPACITY])
{
    if (!name || !*name) return FALSE;
    static const struct { const wchar_t *name; const char *code; } aliases[] = {
        {L"中文", "ZH-HANS"}, {L"简体中文", "ZH-HANS"}, {L"简体", "ZH-HANS"},
        {L"Chinese", "ZH-HANS"}, {L"ZH", "ZH-HANS"}, {L"ZH-CN", "ZH-HANS"},
        {L"繁體中文", "ZH-HANT"}, {L"繁体中文", "ZH-HANT"}, {L"繁体", "ZH-HANT"},
        {L"ZH-TW", "ZH-HANT"}, {L"ZH-HK", "ZH-HANT"},
        {L"英语", "EN-US"}, {L"英文", "EN-US"}, {L"English", "EN-US"}, {L"EN", "EN-US"},
        {L"日语", "JA"}, {L"日文", "JA"}, {L"Japanese", "JA"},
        {L"韩语", "KO"}, {L"韩文", "KO"}, {L"Korean", "KO"},
        {L"法语", "FR"}, {L"French", "FR"}, {L"德语", "DE"}, {L"German", "DE"},
        {L"俄语", "RU"}, {L"Russian", "RU"}, {L"西班牙语", "ES"}, {L"Spanish", "ES"},
        {L"葡萄牙语", "PT-BR"}, {L"Portuguese", "PT-BR"}, {L"PT", "PT-BR"}
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (wstr_ieq(name, aliases[i].name)) {
            strcpy_s(out, DEEPL_TARGET_CAPACITY, aliases[i].code);
            return TRUE;
        }
    }

    /* 其余可使用官方语言代码，如 DE、ES-419；服务验证实际支持范围。 */
    size_t length = wcslen(name), segment = 0;
    if (length < 2 || length >= DEEPL_TARGET_CAPACITY) return FALSE;
    BOOL suffix = FALSE;
    for (size_t i = 0; i < length; i++) {
        wchar_t c = name[i];
        if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        if (c == L'-') {
            if (segment < 2 || segment > (suffix ? 4u : 3u)) return FALSE;
            segment = 0;
            suffix = TRUE;
        } else {
            if (!(c >= L'A' && c <= L'Z') && !(suffix && c >= L'0' && c <= L'9')) return FALSE;
            segment++;
        }
        out[i] = (char)c;
    }
    if (segment < 2 || segment > (suffix ? 4u : 3u)) return FALSE;
    out[length] = 0;
    return TRUE;
}

static const wchar_t *friendly_status(int status)
{
    switch (status) {
    case 400: return L"DeepL 拒绝了请求（400），请检查目标语言";
    case 401:
    case 403: return L"DeepL 密钥无效或没有权限，请到设置里检查";
    case 413: return L"DeepL 请求过大，请缩小截图选区";
    case 429: return L"DeepL 请求过于频繁，请稍后再试";
    case 456: return L"DeepL 翻译额度已用完，请检查账号用量";
    default:  return status >= 500 && status < 600
                    ? L"DeepL 服务暂不可用，请稍后再试"
                    : L"DeepL 返回异常状态，请检查服务连接";
    }
}

/* 只向官方 HTTPS 地址发送密钥，且不跟随重定向。 */
static cJSON *request(const wchar_t *key, BOOL usage, const char *body, int timeout_sec,
                      wchar_t **error)
{
    if (!deepl_key_is_valid(key)) {
        *error = copy_error(L"DeepL 密钥为空或格式不正确，请到设置里检查");
        return NULL;
    }
    size_t length = wcslen(key);
    BOOL free_plan = length >= 3 && wstr_ieq(key + length - 3, L":fx");
    const wchar_t *host = free_plan ? L"https://api-free.deepl.com" : L"https://api.deepl.com";
    wchar_t url[96];
    swprintf_s(url, 96, L"%ls/v2/%ls", host, usage ? L"usage" : L"translate");
    const char prefix[] = "Authorization: DeepL-Auth-Key ";
    char auth[sizeof(prefix) + DEEPL_KEY_LIMIT];
    memcpy(auth, prefix, sizeof(prefix) - 1);
    for (size_t i = 0; i <= length; i++) auth[sizeof(prefix) - 1 + i] = (char)key[i];
    const char *headers[] = {auth, usage ? NULL : "Content-Type: application/json", NULL};
    HttpResponse response = {0};
    ULONGLONG started = GetTickCount64();
    int timeout_ms = timeout_sec < 5 ? 5000 : (timeout_sec > 300 ? 300000 : timeout_sec * 1000);
    BOOL ok = http_request_json_no_redirect(usage ? L"GET" : L"POST", url, headers,
                                           body, body ? strlen(body) : 0, timeout_ms, &response);
    SecureZeroMemory(auth, sizeof(auth));
    LOG_INFO("deepl", L"%ls响应 %d，%llu ms", usage ? L"连接测试" : L"文本翻译",
             response.status, GetTickCount64() - started);
    cJSON *root = NULL;
    if (!ok) {
        wchar_t message[320];
        swprintf_s(message, 320, L"连接 DeepL 失败：%ls", *response.error ? response.error : L"请检查网络");
        *error = copy_error(message);
    } else if (response.status < 200 || response.status >= 300) {
        *error = copy_error(friendly_status(response.status));
    } else {
        root = cJSON_Parse(response.body);
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            root = NULL;
            *error = copy_error(L"DeepL 返回的内容无法解析，请重试");
        }
    }
    http_response_free(&response);
    return root;
}

BOOL deepl_translate_batch(const wchar_t *key, const wchar_t *target,
                           const wchar_t *const *sources, int count, int timeout_sec,
                           wchar_t **translations, wchar_t **error)
{
    *error = NULL;
    if (count < 1 || count > DEEPL_MAX_TEXTS) {
        *error = copy_error(L"DeepL 请求的文本条数无效");
        return FALSE;
    }
    for (int i = 0; i < count; i++) translations[i] = NULL;
    char language[DEEPL_TARGET_CAPACITY];
    if (!deepl_target_language(target, language)) {
        *error = copy_error(L"DeepL 目标语言无法识别，请在设置中填中文或语言代码（如 ZH-HANS）");
        return FALSE;
    }
    cJSON *payload = cJSON_CreateObject();
    cJSON *texts = cJSON_AddStringToObject(payload, "target_lang", language)
                 ? cJSON_AddArrayToObject(payload, "text") : NULL;
    BOOL complete = texts != NULL;
    for (int i = 0; complete && i < count; i++) {
        char *text = u16_to_u8(sources[i]);
        cJSON *item = text ? cJSON_CreateString(text) : NULL;
        complete = cJSON_AddItemToArray(texts, item);
        if (!complete) cJSON_Delete(item);
        heap_free(text);
    }
    char *body = complete ? cJSON_PrintUnformatted(payload) : NULL;
    cJSON_Delete(payload);
    if (!body) { *error = copy_error(L"构造 DeepL 请求失败"); return FALSE; }
    if (strlen(body) > DEEPL_BODY_LIMIT) {
        cJSON_free(body);
        *error = copy_error(L"DeepL 请求过大，请缩小截图选区");
        return FALSE;
    }
    /* 不传 source_lang，由 DeepL 检测；不发送截图或 OCR 坐标。 */
    cJSON *root = request(key, FALSE, body, timeout_sec, error);
    cJSON_free(body);
    if (!root) return FALSE;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "translations");
    complete = cJSON_IsArray(items) && cJSON_GetArraySize(items) == count;
    for (int i = 0; complete && i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const char *output = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "text"));
        complete = output && *output && (translations[i] = u8_to_u16(output));
    }
    cJSON_Delete(root);
    if (!complete) {
        for (int i = 0; i < count; i++) { heap_free(translations[i]); translations[i] = NULL; }
        *error = copy_error(L"DeepL 返回的译文不完整，请重试");
    }
    return complete;
}

void deepl_translate(const wchar_t *key, const wchar_t *target,
                     const wchar_t *source, int timeout_sec,
                     wchar_t **translated, wchar_t **error)
{
    const wchar_t *sources[] = {source};
    deepl_translate_batch(key, target, sources, 1, timeout_sec, translated, error);
}

wchar_t *deepl_test_connection(const wchar_t *key, int timeout_sec)
{
    wchar_t *error = NULL;
    cJSON *root = request(key, TRUE, NULL, timeout_sec, &error);
    if (root && !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "character_count"))) {
        error = copy_error(L"DeepL 用量响应格式异常，请重试");
    }
    cJSON_Delete(root);
    return error;
}
