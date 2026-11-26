/* youdao.c —— 有道智云开放平台文本翻译（NMT）
 *
 * 凭据：「应用ID:应用密钥」，签名算法 v3 = SHA256(appKey + truncate(q) + salt + curtime + appSecret)。
 * truncate(q)：长度 <= 20 取全文，否则「前 10 字 + 长度 + 后 10 字」，长度按字符数（与官方 SDK 一致）。
 * 只向官方 HTTPS 端点发送用户主动提交的 OCR 文本，不读取浏览器会话。
 */
#include "youdao.h"
#include "http.h"
#include "util.h"

#include "cJSON.h"

#include <bcrypt.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define YOUDAO_ENDPOINT   L"https://openapi.youdao.com/api"
#define YOUDAO_KEY_LIMIT  255
#define YOUDAO_BODY_LIMIT (128u * 1024u)

static wchar_t *copy_error(const wchar_t *text)
{
    if (!text) text = L"有道翻译失败";
    size_t count = wcslen(text) + 1;
    wchar_t *out = (wchar_t *)heap_alloc(count * sizeof(*out));
    if (out) memcpy(out, text, count * sizeof(*out));
    return out;
}

BOOL youdao_key_is_valid(const wchar_t *key)
{
    if (!key || !*key) return FALSE;
    if (wcslen(key) > YOUDAO_KEY_LIMIT) return FALSE;

    const wchar_t *colon = wcschr(key, L':');
    if (!colon || colon == key || colon[1] == 0) return FALSE;
    if (wcschr(colon + 1, L':') != NULL) return FALSE;   /* 只允许一个分隔符 */

    for (const wchar_t *p = key; *p; p++) {
        if (*p <= L' ' || *p == 0x7F) return FALSE;      /* 不能有空白等控制字符 */
    }
    return TRUE;
}

BOOL youdao_target_language(const wchar_t *name, char out[16])
{
    if (!name || !*name) return FALSE;
    static const struct { const wchar_t *name; const char *code; } aliases[] = {
        {L"中文", "zh-CHS"}, {L"简体中文", "zh-CHS"}, {L"简体", "zh-CHS"},
        {L"Chinese", "zh-CHS"}, {L"ZH", "zh-CHS"}, {L"ZH-CN", "zh-CHS"},
        {L"繁體中文", "zh-CHT"}, {L"繁体中文", "zh-CHT"}, {L"繁体", "zh-CHT"},
        {L"ZH-TW", "zh-CHT"}, {L"ZH-HK", "zh-CHT"},
        {L"英语", "en"}, {L"英文", "en"}, {L"English", "en"}, {L"EN", "en"},
        {L"日语", "ja"}, {L"日文", "ja"}, {L"Japanese", "ja"},
        {L"韩语", "ko"}, {L"韩文", "ko"}, {L"Korean", "ko"},
        {L"法语", "fr"}, {L"French", "fr"}, {L"德语", "de"}, {L"German", "de"},
        {L"俄语", "ru"}, {L"Russian", "ru"}, {L"西班牙语", "es"}, {L"Spanish", "es"},
        {L"葡萄牙语", "pt"}, {L"Portuguese", "pt"},
        {L"意大利语", "it"}, {L"Italian", "it"},
        {L"越南语", "vi"}, {L"Thai", "th"}, {L"泰语", "th"},
        {L"阿拉伯语", "ar"}, {L"印尼语", "id"}
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (wstr_ieq(name, aliases[i].name)) {
            strcpy_s(out, 16, aliases[i].code);
            return TRUE;
        }
    }

    /* 其余按官方语言码原样使用，例如 zh-CHS；校验是否有服务端支持交给服务端。 */
    size_t length = wcslen(name);
    if (length < 2 || length >= 16) return FALSE;
    for (size_t i = 0; i < length; i++) {
        wchar_t c = name[i];
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-')) {
            return FALSE;
        }
        out[i] = (char)c;
    }
    out[length] = 0;
    return TRUE;
}

/* ---------- 签名 ---------- */

/* BCrypt 的 SHA-256：比手写轮函数可靠，系统自带。 */
static BOOL sha256_hex(const char *data, size_t size, char out[65])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char *object = NULL;
    DWORD object_size = 0, produced = 0;
    unsigned char digest[32];
    BOOL ok = FALSE;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        return FALSE;
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &produced, 0) < 0) {
        goto done;
    }

    object = (unsigned char *)heap_alloc(object_size);
    if (object == NULL) goto done;
    if (BCryptCreateHash(alg, &hash, object, object_size, NULL, 0, 0) < 0) goto done;
    if (BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) < 0) goto done;
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) goto done;

    for (size_t i = 0; i < sizeof(digest); i++) {
        sprintf_s(out + i * 2, 3, "%02x", digest[i]);
    }
    out[64] = 0;
    ok = TRUE;

done:
    if (hash != NULL) BCryptDestroyHash(hash);
    if (object != NULL) heap_free(object);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* 每个请求一个随机 salt；只用 16 字节随机数的十六进制，够唯一。 */
static void make_salt(char out[33])
{
    unsigned char raw[16];
    if (BCryptGenRandom(NULL, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        ULONGLONG seed = GetTickCount64() ^ (ULONGLONG)GetCurrentProcessId();
        for (size_t i = 0; i < sizeof(raw); i++) {
            raw[i] = (unsigned char)(seed >> ((i % 8) * 8));
        }
    }
    for (size_t i = 0; i < sizeof(raw); i++) {
        sprintf_s(out + i * 2, 3, "%02x", raw[i]);
    }
    out[32] = 0;
}

/* truncate(q)：官方 SDK 按字符数算，这里用 UTF-16 长度，保持一致。 */
static char *truncate_q(const wchar_t *text)
{
    size_t length = text ? wcslen(text) : 0;
    if (length <= 20) return u16_to_u8(text ? text : L"");

    wchar_t head[11], tail[11];
    memcpy(head, text, 10 * sizeof(wchar_t));
    head[10] = 0;
    memcpy(tail, text + length - 10, 10 * sizeof(wchar_t));
    tail[10] = 0;

    char *head_u8 = u16_to_u8(head);
    char *tail_u8 = u16_to_u8(tail);
    if (head_u8 == NULL || tail_u8 == NULL) {
        heap_free(head_u8);
        heap_free(tail_u8);
        return NULL;
    }

    size_t need = strlen(head_u8) + 24 + strlen(tail_u8) + 1;
    char *out = (char *)heap_alloc(need);
    if (out != NULL) {
        _snprintf_s(out, need, _TRUNCATE, "%s%llu%s", head_u8,
                    (unsigned long long)length, tail_u8);
    }
    heap_free(head_u8);
    heap_free(tail_u8);
    return out;
}

static BOOL form_append(char **body, size_t *used, size_t *capacity,
                        const char *name, const char *value, BOOL first)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t need = (first ? 0 : 1) + strlen(name) + 1;
    for (const unsigned char *p = (const unsigned char *)value; p && *p; p++) {
        unsigned char c = *p;
        need += ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                 c == '.' || c == '~') ? 1 : 3;
    }

    if (*used + need + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 512;
        while (next < *used + need + 1) next *= 2;
        char *grown = (char *)heap_realloc(*body, next);
        if (grown == NULL) return FALSE;
        *body = grown;
        *capacity = next;
    }

    if (!first) (*body)[(*used)++] = '&';
    size_t name_len = strlen(name);
    memcpy(*body + *used, name, name_len);
    *used += name_len;
    (*body)[(*used)++] = '=';

    for (const unsigned char *p = (const unsigned char *)value; p && *p; p++) {
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

/* ---------- 响应 ---------- */

static const wchar_t *friendly_code(const char *code)
{
    if (code == NULL) return L"有道返回的错误码无法识别";
    if (strcmp(code, "101") == 0) return L"有道应用ID无效，请检查应用ID";
    if (strcmp(code, "102") == 0) return L"有道不支持该目标语言";
    if (strcmp(code, "103") == 0) return L"有道单次文本过长，请缩小选区";
    if (strcmp(code, "108") == 0) return L"有道应用ID无效，请检查应用ID";
    if (strcmp(code, "110") == 0) return L"有道无相关服务权限，请确认已开通「文本翻译」";
    if (strcmp(code, "111") == 0) return L"有道账号问题，请登录控制台查看";
    if (strcmp(code, "202") == 0) return L"有道签名校验失败，请检查应用密钥";
    if (strcmp(code, "206") == 0) return L"有道时间戳无效，请检查系统时间";
    if (strcmp(code, "207") == 0) return L"有道重放请求被拒绝，请重试";
    if (strcmp(code, "401") == 0) return L"有道账户余额不足，请先在控制台充值";
    if (strcmp(code, "411") == 0 || strcmp(code, "413") == 0) {
        return L"有道访问频率受限，请稍后再试";
    }
    return NULL;
}

static BOOL parse_response(const char *body, wchar_t **translated, wchar_t **error)
{
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *error = copy_error(L"有道返回的内容无法解析，请重试");
        return FALSE;
    }

    char code_buffer[16];
    const char *code = NULL;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "errorCode");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        code = item->valuestring;
    } else if (cJSON_IsNumber(item)) {
        _snprintf_s(code_buffer, sizeof(code_buffer), _TRUNCATE, "%d", item->valueint);
        code = code_buffer;
    }

    BOOL ok = FALSE;
    if (code != NULL && strcmp(code, "0") == 0) {
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "translation");
        const char *first = cJSON_IsArray(items)
                            ? cJSON_GetStringValue(cJSON_GetArrayItem(items, 0))
                            : NULL;
        if (first != NULL && *first != 0) {
            *translated = u8_to_u16(first);
            ok = *translated != NULL;
            if (!ok) *error = copy_error(L"有道返回的译文不是有效文本");
        } else {
            *error = copy_error(L"有道没有返回译文，请检查目标语言");
        }
    } else {
        const wchar_t *friendly = friendly_code(code);
        if (friendly != NULL) {
            *error = copy_error(friendly);
        } else {
            wchar_t message[160];
            _snwprintf_s(message, 160, _TRUNCATE, L"有道返回错误（错误码 %hs）",
                         code ? code : "?");
            *error = copy_error(message);
        }
    }

    cJSON_Delete(root);
    return ok;
}

/* ---------- 请求 ---------- */

/* 一条文本一次请求。返回 FALSE 时写 *error。 */
static BOOL translate_one(const wchar_t *key, const wchar_t *target,
                          const wchar_t *source, int timeout_sec,
                          wchar_t **translated, wchar_t **error)
{
    const wchar_t *colon = wcschr(key, L':');
    if (colon == NULL) {
        *error = copy_error(L"有道凭据格式应为「应用ID:应用密钥」");
        return FALSE;
    }

    wchar_t *head = (wchar_t *)heap_alloc(((size_t)(colon - key) + 1) * sizeof(wchar_t));
    if (head == NULL) {
        *error = copy_error(L"内存不足");
        return FALSE;
    }
    memcpy(head, key, (size_t)(colon - key) * sizeof(wchar_t));
    head[colon - key] = 0;

    char *app_key = u16_to_u8(head);
    char *app_secret = u16_to_u8(colon + 1);
    heap_free(head);
    if (app_key == NULL || app_secret == NULL) {
        heap_free(app_key);
        heap_free(app_secret);
        *error = copy_error(L"有道凭据无法转换为 UTF-8");
        return FALSE;
    }

    char language[16];
    if (!youdao_target_language(target, language)) {
        heap_free(app_key);
        heap_free(app_secret);
        *error = copy_error(L"有道目标语言无法识别，请填中文或语言代码（如 zh-CHS）");
        return FALSE;
    }

    char *query = u16_to_u8(source ? source : L"");
    char *truncated = truncate_q(source);
    BOOL ok = FALSE;

    if (query == NULL || truncated == NULL) {
        *error = copy_error(L"有道请求文本转换失败");
        goto done;
    }
    if (strlen(query) > YOUDAO_BODY_LIMIT) {
        *error = copy_error(L"有道单次文本过长，请缩小选区");
        goto done;
    }

    char salt[33];
    make_salt(salt);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG ticks = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ULONGLONG unix_seconds = ticks / 10000000ULL - 11644473600ULL;
    char curtime[24];
    _snprintf_s(curtime, sizeof(curtime), _TRUNCATE, "%llu",
                (unsigned long long)unix_seconds);

    /* sign = SHA256(appKey + truncate(q) + salt + curtime + appSecret) */
    size_t sign_input_size = strlen(app_key) + strlen(truncated) + strlen(salt) +
                             strlen(curtime) + strlen(app_secret) + 1;
    char *sign_input = (char *)heap_alloc(sign_input_size);
    if (sign_input == NULL) {
        *error = copy_error(L"内存不足");
        goto done;
    }
    _snprintf_s(sign_input, sign_input_size, _TRUNCATE, "%s%s%s%s%s",
                app_key, truncated, salt, curtime, app_secret);

    char sign[65];
    BOOL signed_ok = sha256_hex(sign_input, strlen(sign_input), sign);
    SecureZeroMemory(sign_input, sign_input_size);
    heap_free(sign_input);
    if (!signed_ok) {
        *error = copy_error(L"有道签名计算失败");
        goto done;
    }

    char *body = NULL;
    size_t used = 0, capacity = 0;
    if (!form_append(&body, &used, &capacity, "q", query, TRUE) ||
        !form_append(&body, &used, &capacity, "from", "auto", FALSE) ||
        !form_append(&body, &used, &capacity, "to", language, FALSE) ||
        !form_append(&body, &used, &capacity, "appKey", app_key, FALSE) ||
        !form_append(&body, &used, &capacity, "salt", salt, FALSE) ||
        !form_append(&body, &used, &capacity, "curtime", curtime, FALSE) ||
        !form_append(&body, &used, &capacity, "sign", sign, FALSE) ||
        !form_append(&body, &used, &capacity, "signType", "v3", FALSE)) {
        heap_free(body);
        *error = copy_error(L"构造有道请求失败");
        goto done;
    }

    const char *headers[] = {
        "Content-Type: application/x-www-form-urlencoded",
        NULL
    };
    HttpResponse response = {0};
    int timeout_ms = timeout_sec < 5 ? 5000 : (timeout_sec > 300 ? 300000 : timeout_sec * 1000);
    BOOL sent = http_request_json_no_redirect(L"POST", YOUDAO_ENDPOINT, headers, body,
                                              strlen(body), timeout_ms, &response);
    heap_free(body);

    if (!sent) {
        wchar_t message[320];
        _snwprintf_s(message, 320, _TRUNCATE, L"连接有道失败：%ls",
                     response.error[0] ? response.error : L"请检查网络");
        *error = copy_error(message);
        http_response_free(&response);
        goto done;
    }
    if (response.status < 200 || response.status >= 300) {
        wchar_t message[160];
        _snwprintf_s(message, 160, _TRUNCATE, L"有道返回 HTTP 错误（%d）", response.status);
        *error = copy_error(message);
        http_response_free(&response);
        goto done;
    }

    LOG_INFO("youdao", L"响应 %d，%zu 字节", response.status, response.body_size);
    ok = parse_response(response.body, translated, error);
    http_response_free(&response);

done:
    heap_free(query);
    heap_free(truncated);
    heap_free(app_key);
    heap_free(app_secret);
    return ok;
}

BOOL youdao_translate_batch(const wchar_t *key, const wchar_t *target,
                            const wchar_t *const *sources, int count,
                            int timeout_sec, wchar_t **translations,
                            wchar_t **error)
{
    if (error) *error = NULL;
    if (!sources || !translations || count < 1 || count > YOUDAO_MAX_TEXTS) {
        if (error) *error = copy_error(L"有道请求的文本条数无效");
        return FALSE;
    }
    for (int i = 0; i < count; i++) translations[i] = NULL;
    if (!youdao_key_is_valid(key)) {
        if (error) *error = copy_error(L"有道凭据应为「应用ID:应用密钥」，请到设置里重新填写");
        return FALSE;
    }

    for (int i = 0; i < count; i++) {
        if (!translate_one(key, target, sources[i], timeout_sec,
                           &translations[i], error)) {
            for (int j = 0; j < count; j++) {
                heap_free(translations[j]);
                translations[j] = NULL;
            }
            return FALSE;
        }
    }
    return TRUE;
}

void youdao_translate(const wchar_t *key, const wchar_t *target,
                      const wchar_t *source, int timeout_sec,
                      wchar_t **translated, wchar_t **error)
{
    const wchar_t *sources[] = {source};
    youdao_translate_batch(key, target, sources, 1, timeout_sec, translated, error);
}

wchar_t *youdao_test_connection(const wchar_t *key, int timeout_sec)
{
    wchar_t *translated = NULL;
    wchar_t *error = NULL;
    const wchar_t *sources[] = {L"hello"};

    if (youdao_translate_batch(key, L"中文", sources, 1, timeout_sec,
                               &translated, &error)) {
        heap_free(translated);
        return NULL;
    }
    heap_free(translated);
    return error ? error : copy_error(L"有道连接测试失败");
}