/* providers.c —— 安全的文本翻译适配器
 *
 * Google Web 是公开的无密钥文本端点。这里只发送用户主动提交的 OCR 文本，
 * 不读取浏览器 Cookie、令牌或其他隐藏凭据。网页会话/签名协议统一显式失败。
 */
#include "providers.h"
#include "http.h"
#include "util.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static wchar_t *copy_error(const wchar_t *text)
{
    if (!text) text = L"翻译失败";
    size_t count = wcslen(text) + 1;
    wchar_t *out = (wchar_t *)heap_alloc(count * sizeof(*out));
    if (out) memcpy(out, text, count * sizeof(*out));
    return out;
}

BOOL provider_id_is_known(const wchar_t *provider)
{
    if (!provider || !*provider) return FALSE;
    return wstr_ieq(provider, TRANSLATION_PROVIDER_DEEPL) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_OPENAI) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_GOOGLE) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_TENCENT) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_BING) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_MS) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_YOUDAO) ||
           wstr_ieq(provider, TRANSLATION_PROVIDER_YANDEX);
}

const wchar_t *provider_default_for(const Config *cfg)
{
    return cfg && cfg->deepl_api_key[0]
           ? TRANSLATION_PROVIDER_DEEPL
           : TRANSLATION_PROVIDER_OPENAI;
}

BOOL provider_is_openai(const Config *cfg)
{
    return cfg && wstr_ieq(cfg->translation_provider, TRANSLATION_PROVIDER_OPENAI);
}

BOOL provider_is_deepl(const Config *cfg)
{
    return cfg && wstr_ieq(cfg->translation_provider, TRANSLATION_PROVIDER_DEEPL);
}

wchar_t *provider_unavailable_error(const wchar_t *provider)
{
    if (provider && wstr_ieq(provider, TRANSLATION_PROVIDER_BING)) {
        return copy_error(L"Bing 网页翻译暂不可用：需要安全可复现的会话引导，程序不会抓取浏览器 Cookie");
    }
    if (provider && wstr_ieq(provider, TRANSLATION_PROVIDER_MS)) {
        return copy_error(L"Microsoft Android 翻译暂不可用：签名端点需要源项目未提供的运行时签名参数");
    }
    if (provider && wstr_ieq(provider, TRANSLATION_PROVIDER_YOUDAO)) {
        return copy_error(L"有道网页翻译暂不可用：会话与加密协议未安全复现");
    }
    if (provider && wstr_ieq(provider, TRANSLATION_PROVIDER_TENCENT)) {
        return copy_error(L"腾讯 TranSmart 暂不可用：当前未确认可公开复现的客户端密钥与主机回退协议");
    }
    if (provider && wstr_ieq(provider, TRANSLATION_PROVIDER_YANDEX)) {
        return copy_error(L"Yandex 网页翻译暂不可用：公开适配器需要密钥，不能伪装成无钥端点");
    }
    return copy_error(L"所选翻译服务暂未实现");
}

static BOOL google_target_code(const wchar_t *target, char out[16])
{
    if (!target || !*target) return FALSE;
    struct Alias { const wchar_t *name; const char *code; };
    static const struct Alias aliases[] = {
        {L"中文", "zh-CN"}, {L"简体中文", "zh-CN"}, {L"简体", "zh-CN"},
        {L"繁体中文", "zh-TW"}, {L"繁體中文", "zh-TW"}, {L"繁体", "zh-TW"},
        {L"英语", "en"}, {L"英文", "en"}, {L"English", "en"},
        {L"日语", "ja"}, {L"日文", "ja"}, {L"Japanese", "ja"},
        {L"韩语", "ko"}, {L"韩文", "ko"}, {L"Korean", "ko"},
        {L"法语", "fr"}, {L"德语", "de"}, {L"俄语", "ru"},
        {L"西班牙语", "es"}, {L"葡萄牙语", "pt"}
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (wstr_ieq(target, aliases[i].name)) {
            strcpy_s(out, 16, aliases[i].code);
            return TRUE;
        }
    }
    size_t length = wcslen(target);
    if (length < 2 || length >= 16) return FALSE;
    for (size_t i = 0; i < length; i++) {
        wchar_t c = target[i];
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (!((c >= L'a' && c <= L'z') || c == L'-' || c == L'_') &&
            !(c >= L'0' && c <= L'9')) return FALSE;
        out[i] = (char)c;
    }
    out[length] = 0;
    return TRUE;
}

static BOOL form_append(char **body, size_t *used, size_t *capacity,
                        const char *text, BOOL with_q_prefix)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t need = with_q_prefix ? 2 : 0;
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
            need += ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                     (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                     *p == '.' || *p == '~') ? 1 : 3;
        }
    }
    if (*used + need + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 256;
        while (next < *used + need + 1) next *= 2;
        char *grown = (char *)heap_realloc(*body, next);
        if (!grown) return FALSE;
        *body = grown;
        *capacity = next;
    }
    if (with_q_prefix) {
        (*body)[(*used)++] = 'q';
        (*body)[(*used)++] = '=';
    }
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            (*body)[(*used)++] = (char)c;
        } else {
            (*body)[(*used)++] = '%';
            (*body)[(*used)++] = hex[c >> 4];
            (*body)[(*used)++] = hex[c & 15];
        }
    }
    (*body)[*used] = 0;
    return TRUE;
}

static char *build_google_form(const wchar_t *const *sources, int count)
{
    char *body = NULL;
    size_t used = 0, capacity = 0;
    for (int i = 0; i < count; i++) {
        char *source = u16_to_u8(sources[i] ? sources[i] : L"");
        BOOL ok = source && form_append(&body, &used, &capacity, source, TRUE);
        heap_free(source);
        if (!ok) {
            heap_free(body);
            return NULL;
        }
        if (i + 1 < count) {
            if (used + 2 > capacity) {
                size_t next = capacity ? capacity * 2 : 256;
                char *grown = (char *)heap_realloc(body, next);
                if (!grown) { heap_free(body); return NULL; }
                body = grown;
                capacity = next;
            }
            body[used++] = '&';
            body[used] = 0;
        }
    }
    return body;
}

static BOOL append_json_string(char **text, size_t *used, size_t *capacity,
                               const char *value)
{
    size_t length = value ? strlen(value) : 0;
    if (*used + length + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 256;
        while (next < *used + length + 1) next *= 2;
        char *grown = (char *)heap_realloc(*text, next);
        if (!grown) return FALSE;
        *text = grown;
        *capacity = next;
    }
    if (length) memcpy(*text + *used, value, length);
    *used += length;
    (*text)[*used] = 0;
    return TRUE;
}

/* Google 返回的每个分段是 [译文, 原文, ...]；只取第一个字符串，
   不把原文和置信信息误拼进译文。多条输入时每个顶层元素对应一个输入。 */
static BOOL collect_google_item(const cJSON *item, char **text,
                                size_t *used, size_t *capacity,
                                BOOL concatenate_segments)
{
    if (cJSON_IsString(item) && item->valuestring) {
        return append_json_string(text, used, capacity, item->valuestring);
    }
    if (!cJSON_IsArray(item)) return FALSE;
    const cJSON *first = cJSON_GetArrayItem(item, 0);
    if (cJSON_IsString(first) && first->valuestring) {
        return append_json_string(text, used, capacity, first->valuestring);
    }
    if (!concatenate_segments) return FALSE;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, item) {
        if (!collect_google_item(child, text, used, capacity, TRUE)) return FALSE;
    }
    return TRUE;
}

static BOOL parse_google_response(const char *body, int count, wchar_t **translations)
{
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return FALSE;
    }
    BOOL ok = TRUE;
    if (count == 1) {
        char *text = NULL;
        size_t used = 0, capacity = 0;
        ok = collect_google_item(cJSON_GetArrayItem(root, 0), &text, &used, &capacity, TRUE) && used > 0;
        if (ok) translations[0] = u8_to_u16(text);
        if (!translations[0]) ok = FALSE;
        heap_free(text);
    } else {
        if (cJSON_GetArraySize(root) != count) ok = FALSE;
        for (int i = 0; ok && i < count; i++) {
            char *text = NULL;
            size_t used = 0, capacity = 0;
            ok = collect_google_item(cJSON_GetArrayItem(root, i),
                                     &text, &used, &capacity, TRUE) && used > 0;
            if (ok) translations[i] = u8_to_u16(text);
            if (!translations[i]) ok = FALSE;
            heap_free(text);
        }
    }
    cJSON_Delete(root);
    if (!ok) {
        for (int i = 0; i < count; i++) {
            heap_free(translations[i]);
            translations[i] = NULL;
        }
    }
    return ok;
}

BOOL google_translate_batch(const wchar_t *target,
                            const wchar_t *const *sources, int count,
                            int timeout_sec, wchar_t **translations,
                            wchar_t **error)
{
    if (error) *error = NULL;
    if (!sources || !translations || count < 1 || count > 50) {
        if (error) *error = copy_error(L"Google 请求的文本条数无效");
        return FALSE;
    }
    for (int i = 0; i < count; i++) translations[i] = NULL;

    char target_code[16];
    if (!google_target_code(target, target_code)) {
        if (error) *error = copy_error(L"Google 目标语言无法识别，请填写语言名称或代码");
        return FALSE;
    }
    wchar_t url[256];
    _snwprintf_s(url, 256, _TRUNCATE,
                 L"https://translate.googleapis.com/translate_a/t?client=webapp&sl=auto&tl=%hs&hl=en&v=1.0&source=is",
                 target_code);
    char *body = build_google_form(sources, count);
    if (!body) {
        if (error) *error = copy_error(L"构造 Google 请求失败");
        return FALSE;
    }
    const char *headers[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
    HttpResponse response = {0};
    int timeout_ms = timeout_sec < 5 ? 5000 : (timeout_sec > 300 ? 300000 : timeout_sec * 1000);
    BOOL sent = http_request_json_no_redirect(L"POST", url, headers, body,
                                               strlen(body), timeout_ms, &response);
    heap_free(body);
    if (!sent) {
        if (error) *error = copy_error(response.error[0] ? response.error : L"Google 网络请求失败");
        http_response_free(&response);
        return FALSE;
    }
    if (response.status < 200 || response.status >= 300) {
        wchar_t message[128];
        _snwprintf_s(message, 128, _TRUNCATE, L"Google 返回错误（%d），不会改用其他服务", response.status);
        if (error) *error = copy_error(message);
        http_response_free(&response);
        return FALSE;
    }
    BOOL ok = parse_google_response(response.body, count, translations);
    if (!ok && error) *error = copy_error(L"Google 返回的译文数量或格式不正确，表格已拒绝对齐");
    http_response_free(&response);
    return ok;
}
