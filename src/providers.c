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

/* 顺序即设置窗口下拉框的顺序；设置页、保存校验、连接测试都只查这张表，
   不会再出现"界面切到 A、测试还打 B"的分叉。
   PROVIDER_HIDDEN 的条目不会出现在下拉里，但适配器、历史配置与错误文案仍然认识它。 */
#define PROVIDER_VISIBLE FALSE
#define PROVIDER_HIDDEN  TRUE

static const ProviderInfo kProviders[] = {
    {
        TRANSLATION_PROVIDER_DEEPL, L"DeepL",
        L"https://api.deepl.com/v2", L"DeepL 密钥",
        L"https://www.deepl.com/pro-api",
        L"免费版密钥以 :fx 结尾，会自动改用 api-free.deepl.com。"
        L"密钥申请：www.deepl.com/pro-api",
        TRUE, FALSE, FALSE, TRUE, PROVIDER_VISIBLE
    },
    {
        TRANSLATION_PROVIDER_OPENAI, L"OpenAI 兼容",
        L"https://api.openai.com/v1", L"OpenAI 密钥",
        L"https://platform.openai.com/api-keys",
        L"地址可换成任意 OpenAI 格式端点（要带 /v1）。"
        L"密钥申请：platform.openai.com/api-keys",
        TRUE, TRUE, TRUE, TRUE, PROVIDER_VISIBLE
    },
    {
        TRANSLATION_PROVIDER_GOOGLE, L"Google Web",
        L"https://translate.googleapis.com/translate_a/t", L"免密钥",
        NULL,
        L"公开的无密钥文本端点（client=gtx），无需申请；只发送 OCR 文本，"
        L"不读取浏览器 Cookie 或登录会话。",
        FALSE, FALSE, FALSE, TRUE, PROVIDER_VISIBLE
    },
    {
        TRANSLATION_PROVIDER_TENCENT, L"腾讯 TranSmart",
        L"https://transmart.qq.com/api/imt", L"腾讯密钥",
        L"https://console.cloud.tencent.com/tmt",
        L"暂不可用（客户端签名协议未复现）。接口 transmart.qq.com/api/imt；"
        L"控制台 console.cloud.tencent.com/tmt",
        TRUE, FALSE, FALSE, FALSE, PROVIDER_HIDDEN
    },
    {
        TRANSLATION_PROVIDER_BING, L"微软翻译",
        L"https://edge.microsoft.com/translate/translatetext", L"免密钥",
        L"https://www.bing.com/translator",
        L"免密钥的公开端点（edge.microsoft.com/translate/translatetext），无需申请；"
        L"只发送 OCR 文本。入口：www.bing.com/translator",
        FALSE, FALSE, FALSE, TRUE, PROVIDER_VISIBLE
    },
    {
        TRANSLATION_PROVIDER_MS, L"Microsoft Android",
        L"https://api-edge.cognitive.microsofttranslator.com", L"Azure 密钥",
        L"https://portal.azure.com/#create/Microsoft.CognitiveServicesTextTranslation",
        L"暂不可用（签名端点需要运行时参数）。接口 api-edge.cognitive.microsofttranslator.com；"
        L"申请 portal.azure.com 的「文本翻译」资源",
        TRUE, FALSE, FALSE, FALSE, PROVIDER_HIDDEN
    },
    {
        TRANSLATION_PROVIDER_YOUDAO, L"有道智云",
        L"https://openapi.youdao.com/api", L"有道 ID:密钥",
        L"https://ai.youdao.com/console/",
        L"填「应用ID:应用密钥」（控制台里两段各复制一半，中间用冒号连起来）；"
        L"需开通「文本翻译」服务。申请：ai.youdao.com/console",
        TRUE, FALSE, FALSE, TRUE, PROVIDER_VISIBLE
    },
    {
        TRANSLATION_PROVIDER_YANDEX, L"Yandex 网页",
        L"https://translate.yandex.net/api/v1.5/tr.json/translate", L"Yandex 密钥",
        L"https://translate.yandex.com/developers/keys",
        L"暂不可用（公开端点需要密钥）。接口 translate.yandex.net/api/v1.5/tr.json/translate；"
        L"申请 translate.yandex.com/developers/keys",
        TRUE, FALSE, FALSE, FALSE, PROVIDER_HIDDEN
    }
};

const ProviderInfo *provider_table(int *count)
{
    if (count) *count = (int)(sizeof(kProviders) / sizeof(kProviders[0]));
    return kProviders;
}

const ProviderInfo *provider_at(int index)
{
    int count = (int)(sizeof(kProviders) / sizeof(kProviders[0]));
    return (index >= 0 && index < count) ? &kProviders[index] : NULL;
}

int provider_index_of(const wchar_t *provider)
{
    if (provider == NULL || *provider == 0) return -1;
    for (size_t i = 0; i < sizeof(kProviders) / sizeof(kProviders[0]); i++) {
        if (wstr_ieq(provider, kProviders[i].id)) return (int)i;
    }
    return -1;
}

const ProviderInfo *provider_info(const wchar_t *provider)
{
    int index = provider_index_of(provider);
    return index < 0 ? NULL : &kProviders[index];
}

/* 设置页的下拉索引：只数可见项，隐藏项不占位。 */
int provider_visible_count(void)
{
    int count = 0;
    for (size_t i = 0; i < sizeof(kProviders) / sizeof(kProviders[0]); i++) {
        if (!kProviders[i].hidden) count++;
    }
    return count;
}

const ProviderInfo *provider_visible_at(int index)
{
    if (index < 0) return NULL;
    int seen = 0;
    for (size_t i = 0; i < sizeof(kProviders) / sizeof(kProviders[0]); i++) {
        if (kProviders[i].hidden) continue;
        if (seen == index) return &kProviders[i];
        seen++;
    }
    return NULL;
}

int provider_visible_index_of(const wchar_t *provider)
{
    if (provider == NULL || *provider == 0) return -1;
    int seen = 0;
    for (size_t i = 0; i < sizeof(kProviders) / sizeof(kProviders[0]); i++) {
        if (kProviders[i].hidden) continue;
        if (wstr_ieq(provider, kProviders[i].id)) return seen;
        seen++;
    }
    return -1;
}

BOOL provider_is_visible(const wchar_t *provider)
{
    return provider_visible_index_of(provider) >= 0;
}

BOOL provider_id_is_known(const wchar_t *provider)
{
    return provider_index_of(provider) >= 0;
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
    if (provider && wstr_ieq(provider, TRANSLATION_PROVIDER_MS)) {
        return copy_error(L"Microsoft Android 翻译暂不可用：签名端点需要源项目未提供的运行时签名参数");
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
                 L"https://translate.googleapis.com/translate_a/t?client=gtx&dt=t&sl=auto&tl=%hs",
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

wchar_t *google_test_connection(const wchar_t *target, int timeout_sec)
{
    static const wchar_t *const probe[] = {L"hello"};
    wchar_t *translated[1] = {NULL};
    wchar_t *error = NULL;

    if (google_translate_batch(target, probe, 1, timeout_sec, translated, &error)) {
        heap_free(translated[0]);
        return NULL;
    }
    heap_free(translated[0]);
    return error ? error : copy_error(L"Google 连接测试失败");
}

/* ---------- 微软翻译（edge.microsoft.com 免费端点） ----------
 *
 * 与需要 Azure 密钥的 api.cognitive.microsofttranslator.com 不同，这个端点
 * 用 Edge 浏览器自带的翻译接口，不需要任何密钥：
 *   POST https://edge.microsoft.com/translate/translatetext?to=<code>&isEnterpriseClient=false
 *   body: ["text1","text2"]      -> [{"translations":[{"text":"..."}]}, ...]
 */

/* 中文按微软的 Hant/Hans 分法；其余沿用 BCP-47。 */
static BOOL bing_target_code(const wchar_t *target, char out[16])
{
    char base[16];
    if (!google_target_code(target, base)) return FALSE;
    if (strcmp(base, "zh-CN") == 0)      { strcpy_s(out, 16, "zh-Hans"); return TRUE; }
    if (strcmp(base, "zh-TW") == 0)      { strcpy_s(out, 16, "zh-Hant"); return TRUE; }
    if (strcmp(base, "pt") == 0)         { strcpy_s(out, 16, "pt-PT"); return TRUE; }
    strcpy_s(out, 16, base);
    return TRUE;
}

BOOL bing_translate_batch(const wchar_t *target,
                          const wchar_t *const *sources, int count,
                          int timeout_sec, wchar_t **translations,
                          wchar_t **error)
{
    if (error) *error = NULL;
    if (!sources || !translations || count < 1 || count > 50) {
        if (error) *error = copy_error(L"微软翻译请求的文本条数无效");
        return FALSE;
    }
    for (int i = 0; i < count; i++) translations[i] = NULL;

    char code[16];
    if (!bing_target_code(target, code)) {
        if (error) *error = copy_error(L"微软翻译的目标语言无法识别，请填写语言名称或代码");
        return FALSE;
    }

    cJSON *array = cJSON_CreateArray();
    BOOL built = array != NULL;
    for (int i = 0; built && i < count; i++) {
        char *text = u16_to_u8(sources[i] ? sources[i] : L"");
        cJSON *item = text ? cJSON_CreateString(text) : NULL;
        if (item == NULL) {
            built = FALSE;
        } else if (!cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            built = FALSE;
        }
        heap_free(text);
    }
    char *body = built ? cJSON_PrintUnformatted(array) : NULL;
    cJSON_Delete(array);
    if (body == NULL) {
        if (error) *error = copy_error(L"构造微软翻译请求失败");
        return FALSE;
    }

    wchar_t url[192];
    _snwprintf_s(url, 192, _TRUNCATE,
                 L"https://edge.microsoft.com/translate/translatetext"
                 L"?to=%hs&isEnterpriseClient=false", code);

    const char *headers[] = {
        "Content-Type: application/json",
        "Accept: */*",
        NULL
    };
    HttpResponse response = {0};
    int timeout_ms = timeout_sec < 5 ? 5000 : (timeout_sec > 300 ? 300000 : timeout_sec * 1000);
    BOOL sent = http_request_json_no_redirect(L"POST", url, headers, body,
                                               strlen(body), timeout_ms, &response);
    cJSON_free(body);

    if (!sent) {
        if (error) *error = copy_error(response.error[0] ? response.error
                                                         : L"微软翻译网络请求失败");
        http_response_free(&response);
        return FALSE;
    }
    if (response.status < 200 || response.status >= 300) {
        wchar_t message[128];
        _snwprintf_s(message, 128, _TRUNCATE,
                     L"微软翻译返回错误（%d），不会改用其他服务", response.status);
        if (error) *error = copy_error(message);
        http_response_free(&response);
        return FALSE;
    }

    cJSON *root = response.body ? cJSON_Parse(response.body) : NULL;
    BOOL ok = cJSON_IsArray(root) && cJSON_GetArraySize(root) == count;
    for (int i = 0; ok && i < count; i++) {
        const cJSON *item = cJSON_GetArrayItem(root, i);
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(item, "translations");
        const cJSON *first = cJSON_GetArrayItem(items, 0);
        const char *text = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(first, "text"));
        ok = text != NULL && *text != 0 && (translations[i] = u8_to_u16(text)) != NULL;
    }
    cJSON_Delete(root);

    if (!ok) {
        for (int i = 0; i < count; i++) {
            heap_free(translations[i]);
            translations[i] = NULL;
        }
        if (error) *error = copy_error(L"微软翻译返回的译文数量或格式不正确，表格已拒绝对齐");
    }
    http_response_free(&response);
    return ok;
}

wchar_t *bing_test_connection(const wchar_t *target, int timeout_sec)
{
    static const wchar_t *const probe[] = {L"hello"};
    wchar_t *translated[1] = {NULL};
    wchar_t *error = NULL;

    if (bing_translate_batch(target, probe, 1, timeout_sec, translated, &error)) {
        heap_free(translated[0]);
        return NULL;
    }
    heap_free(translated[0]);
    return error ? error : copy_error(L"微软翻译连接测试失败");
}
