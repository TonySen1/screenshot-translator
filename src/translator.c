/* translator.c —— OpenAI 兼容的 /chat/completions 调用
 *
 * 只依赖 WinHTTP，不引第三方 HTTP 库。
 * 提示词与 Python 版保持一致：要求模型只回一个 JSON 对象。
 */
#include "translator.h"
#include "base64.h"
#include "http.h"
#include "util.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* 长边上限 1568：多数多模态模型在这个尺寸下识别效果和请求体大小最平衡。
   再大 base64 后会显著拉长请求，容易触发超时或被服务端拒绝。 */
#define IMAGE_MAX_SIDE 1568
#define IMAGE_JPEG_QUALITY 85

static const char *SYSTEM_PROMPT_TEMPLATE =
    "You are an OCR + translation engine. Identify every visible piece of text "
    "in the image and translate it into %s.\n"
    "\n"
    "Follow these rules strictly:\n"
    "1. Output only one JSON object. No Markdown code fences, no explanation, "
    "no prefix or suffix text.\n"
    "2. The JSON shape is exactly: "
    "{\"original_text\": \"recognised source text\", "
    "\"translated_text\": \"translation\"}\n"
    "3. Keep original_text in the source language. Preserve line breaks as \\n.\n"
    "4. translated_text must have the same paragraph structure as the source.\n"
    "5. If the image has no text at all, return empty strings for both fields.\n"
    "6. Do not translate proper nouns, code, variable names, URLs or numbers; "
    "keep them as they are.";

static const char *USER_PROMPT = "Recognise the text in this image and translate it.";

/* ---------- 释放 ---------- */

void translation_result_free(TranslationResult *result)
{
    if (result == NULL) return;
    heap_free(result->original);
    heap_free(result->translated);
    heap_free(result->error);
    memset(result, 0, sizeof(*result));
}

/* 复制一份 UTF-8 -> 宽字符串；失败返回空串而不是 NULL */
static wchar_t *dup_u8(const char *text)
{
    if (text == NULL) {
        wchar_t *empty = (wchar_t *)heap_alloc(sizeof(wchar_t));
        if (empty) empty[0] = 0;
        return empty;
    }
    wchar_t *wide = u8_to_u16(text);
    if (wide != NULL) return wide;

    wchar_t *empty = (wchar_t *)heap_alloc(sizeof(wchar_t));
    if (empty) empty[0] = 0;
    return empty;
}

static wchar_t *make_error(const wchar_t *text)
{
    if (text == NULL) text = L"翻译失败";
    size_t count = wcslen(text) + 1;
    wchar_t *out = (wchar_t *)heap_alloc(count * sizeof(wchar_t));
    if (out != NULL) memcpy(out, text, count * sizeof(wchar_t));
    return out;
}

/* ---------- 错误提示 ---------- */

/* 把状态码翻成一句中文；未知状态码返回 NULL。 */
static const wchar_t *friendly_status(int status)
{
    switch (status) {
    case 400: return L"请求格式被拒绝（400），请检查模型名";
    case 401: return L"API Key 无效，请在设置里重新填写";
    case 403: return L"API 拒绝访问（403），请检查 Key 权限";
    case 404: return L"接口地址或模型名不正确（404）";
    case 413: return L"图片太大被拒绝（413）";
    case 429: return L"请求过于频繁或额度不足（429）";
    default:
        break;
    }
    if (status >= 500 && status < 600) return L"接口服务端错误，请稍后再试";
    if (status >= 400) return L"接口返回错误，请检查配置";

    return NULL;
}

/* ---------- 响应解析 ---------- */

/* 从模型回复里尽量抠出一个 JSON 对象。
   顺序和 Python 版一致：先摘代码块，再原文，最后首尾花括号之间。 */
static cJSON *extract_json(const char *text)
{
    if (text == NULL || text[0] == 0) return NULL;

    cJSON *parsed = cJSON_Parse(text);
    if (parsed != NULL) return parsed;

    /* ```json ... ``` 或 ``` ... ``` */
    const char *fence = strstr(text, "```");
    if (fence != NULL) {
        const char *start = fence + 3;
        /* 跳过语言标记直到换行 */
        while (*start != 0 && *start != '\n') start++;
        while (*start == '\n' || *start == '\r') start++;

        const char *end = strstr(start, "```");
        if (end != NULL && end > start) {
            size_t len = (size_t)(end - start);
            char *chunk = (char *)heap_alloc(len + 1);
            if (chunk != NULL) {
                memcpy(chunk, start, len);
                chunk[len] = 0;
                parsed = cJSON_Parse(chunk);
                heap_free(chunk);
                if (parsed != NULL) return parsed;
            }
        }
    }

    /* 第一个 { 到最后一个 } */
    const char *open = strchr(text, '{');
    const char *close = strrchr(text, '}');
    if (open != NULL && close != NULL && close > open) {
        size_t len = (size_t)(close - open) + 1;
        char *chunk = (char *)heap_alloc(len + 1);
        if (chunk != NULL) {
            memcpy(chunk, open, len);
            chunk[len] = 0;
            parsed = cJSON_Parse(chunk);
            heap_free(chunk);
        }
    }

    return parsed;
}

/* 按候选键名取第一个非空字符串 */
static const char *pick_string(cJSON *root, const char *const *keys, int count)
{
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            item->valuestring[0] != 0) {
            return item->valuestring;
        }
    }
    return NULL;
}

/* 候选键名里只要有一个存在且是字符串就算出现了（空串也算） */
static BOOL has_string_key(cJSON *root, const char *const *keys, int count)
{
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsString(item)) return TRUE;
    }
    return FALSE;
}

static void parse_model_content(const char *content, TranslationResult *out)
{
    if (content == NULL || content[0] == 0) {
        out->error = make_error(L"模型没有返回内容");
        return;
    }

    cJSON *root = extract_json(content);
    if (root != NULL) {
        static const char *const ORIGINAL_KEYS[] = {
            "original_text", "originalText", "original",
            "source_text", "text"
        };
        static const char *const TRANSLATED_KEYS[] = {
            "translated_text", "translatedText", "translated",
            "translation", "target_text", "result"
        };

        /* 按"字段是否存在"判断，而不是"值是否非空" ——
           模型对无文字的图片会返回两个空字符串，那是合法结果。 */
        BOOL has_original = has_string_key(root, ORIGINAL_KEYS, 5);
        BOOL has_translated = has_string_key(root, TRANSLATED_KEYS, 6);

        if (has_original || has_translated) {
            const char *original = pick_string(root, ORIGINAL_KEYS, 5);
            const char *translated = pick_string(root, TRANSLATED_KEYS, 6);
            out->original = dup_u8(original ? original : "");
            out->translated = dup_u8(translated ? translated : "");
            cJSON_Delete(root);
            return;
        }

        /* JSON 合法但没有可用字段，模型没按格式来；落到下面的兜底 */
        cJSON_Delete(root);
    }

    /* 兜底：整段回复当译文，总比只说"失败"有用 */
    LOG_WARN("translator", L"模型回复不是预期的 JSON，按纯文本处理");
    out->original = dup_u8("");
    out->translated = dup_u8(content);
}

/* 从 /chat/completions 的响应里取出 choices[0].message.content */
static const char *extract_message_content(cJSON *root)
{
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices)) return NULL;

    cJSON *first = cJSON_GetArrayItem(choices, 0);
    if (first == NULL) return NULL;

    cJSON *message = cJSON_GetObjectItemCaseSensitive(first, "message");
    if (message == NULL) return NULL;

    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        return content->valuestring;
    }

    /* 部分服务把 content 返回成分段数组 */
    if (cJSON_IsArray(content)) {
        /* 交给调用方拼接 —— 这里用静态缓冲不合适，简单起见返回第一段 */
        cJSON *part = cJSON_GetArrayItem(content, 0);
        if (part != NULL) {
            cJSON *text = cJSON_GetObjectItemCaseSensitive(part, "text");
            if (cJSON_IsString(text) && text->valuestring != NULL) {
                return text->valuestring;
            }
        }
    }

    return NULL;
}

/* 从错误响应里抠出 message 字段，供日志使用 */
static void log_api_error(int status, const char *body)
{
    if (body == NULL || body[0] == 0) {
        LOG_ERR("translator", L"接口返回 %d，响应体为空", status);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    if (root != NULL) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *message = cJSON_IsObject(error)
                         ? cJSON_GetObjectItemCaseSensitive(error, "message")
                         : NULL;
        if (cJSON_IsString(message) && message->valuestring != NULL) {
            wchar_t *wide = u8_to_u16(message->valuestring);
            LOG_ERR("translator", L"接口返回 %d：%ls", status,
                    wide ? wide : L"(无法解码)");
            heap_free(wide);
            cJSON_Delete(root);
            return;
        }
        cJSON_Delete(root);
    }

    /* 响应体不是 JSON，截一段原文记下来 */
    char snippet[201];
    size_t len = strlen(body);
    if (len > 200) len = 200;
    memcpy(snippet, body, len);
    snippet[len] = 0;

    wchar_t *wide = u8_to_u16(snippet);
    LOG_ERR("translator", L"接口返回 %d：%ls", status, wide ? wide : L"(无法解码)");
    heap_free(wide);
}

/* ---------- 请求构造 ---------- */

/* 拼 "https://base/v1" + "/chat/completions"，容忍结尾斜杠 */
static void build_endpoint(const wchar_t *base, wchar_t *out, size_t out_count)
{
    size_t len = wcslen(base);
    while (len > 0 && base[len - 1] == L'/') len--;

    /* base 里已经带 /chat/completions 就不再追加 */
    if (wcsstr(base, L"/chat/completions") != NULL) {
        _snwprintf_s(out, out_count, _TRUNCATE, L"%.*s", (int)len, base);
        return;
    }

    _snwprintf_s(out, out_count, _TRUNCATE, L"%.*s/chat/completions",
                 (int)len, base);
}

static char *build_request_body(const Config *cfg, const char *image_base64)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    char *model = u16_to_u8(cfg->model);
    char *language = u16_to_u8(cfg->target_language);

    cJSON_AddStringToObject(root, "model", model ? model : "");

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    {
        size_t need = strlen(SYSTEM_PROMPT_TEMPLATE) +
                      (language ? strlen(language) : 0) + 64;
        char *prompt = (char *)heap_alloc(need);
        if (prompt != NULL) {
            snprintf(prompt, need, SYSTEM_PROMPT_TEMPLATE,
                     (language && language[0]) ? language : "Chinese");
            cJSON_AddStringToObject(system, "content", prompt);
            heap_free(prompt);
        }
    }
    cJSON_AddItemToArray(messages, system);

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(user, "content");

    cJSON *text_part = cJSON_CreateObject();
    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text", USER_PROMPT);
    cJSON_AddItemToArray(content, text_part);

    cJSON *image_part = cJSON_CreateObject();
    cJSON_AddStringToObject(image_part, "type", "image_url");
    {
        size_t need = strlen(image_base64) + 32;
        char *data_url = (char *)heap_alloc(need);
        if (data_url != NULL) {
            snprintf(data_url, need, "data:image/jpeg;base64,%s", image_base64);
            cJSON *image_url = cJSON_AddObjectToObject(image_part, "image_url");
            cJSON_AddStringToObject(image_url, "url", data_url);
            heap_free(data_url);
        }
    }
    cJSON_AddItemToArray(content, image_part);

    cJSON_AddItemToArray(messages, user);

    heap_free(model);
    heap_free(language);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

/* ---------- 对外 ---------- */

BOOL translator_prepare_image(const ImageBuffer *image,
                              void **out_image, size_t *out_size)
{
    if (image == NULL || out_image == NULL || out_size == NULL) return FALSE;
    *out_image = NULL;
    *out_size = 0;

    ImageBuffer scaled;
    memset(&scaled, 0, sizeof(scaled));

    const ImageBuffer *source = image;
    if (image->width > IMAGE_MAX_SIDE || image->height > IMAGE_MAX_SIDE) {
        if (!image_resize_max_side(image, &scaled, IMAGE_MAX_SIDE)) {
            LOG_ERR("translator", L"图片缩放失败");
            return FALSE;
        }
        source = &scaled;
        LOG_INFO("translator", L"图片已缩放：%dx%d -> %dx%d",
                 image->width, image->height, scaled.width, scaled.height);
    }

    /* JPEG q85：截图内容在 q85 下 OCR 效果与 PNG 无明显差异，
       体积通常只有 PNG 的三分之一到五分之一。 */
    BOOL ok = image_encode_jpeg(source, IMAGE_JPEG_QUALITY,
                                out_image, out_size);
    image_free(&scaled);
    return ok;
}

void translator_run_image(const Config *cfg, const void *image, size_t image_size,
                          TranslationResult *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));

    if (cfg == NULL || image == NULL || image_size == 0) {
        result->error = make_error(L"内部错误：参数为空");
        return;
    }
    if (cfg->api_key[0] == 0) {
        result->error = make_error(L"请先填写 API Key（右键托盘图标 → 设置）");
        return;
    }
    if (cfg->api_base_url[0] == 0) {
        result->error = make_error(L"请先填写 api_base_url");
        return;
    }

    size_t b64_size = base64_encoded_size(image_size);
    char *b64 = (char *)heap_alloc(b64_size + 1);
    if (b64 == NULL) {
        result->error = make_error(L"内存不足");
        return;
    }
    base64_encode((const unsigned char *)image, image_size, b64);

    LOG_INFO("translator", L"请求体：JPEG %zu 字节，base64 %zu 字符",
             image_size, b64_size);

    /* 3. 构造请求 */
    char *body = build_request_body(cfg, b64);
    heap_free(b64);
    if (body == NULL) {
        result->error = make_error(L"构造请求失败");
        return;
    }

    char *key = u16_to_u8(cfg->api_key);
    size_t auth_len = (key ? strlen(key) : 0) + 32;
    char *auth = (char *)heap_alloc(auth_len);
    if (auth == NULL) {
        cJSON_free(body);      /* cJSON_Print* 走的是 malloc/free */
        heap_free(key);
        result->error = make_error(L"内存不足");
        return;
    }
    snprintf(auth, auth_len, "Authorization: Bearer %s", key ? key : "");
    heap_free(key);

    const char *headers[] = {
        "Content-Type: application/json",
        auth,
        NULL
    };

    wchar_t endpoint[1024];
    build_endpoint(cfg->api_base_url, endpoint, 1024);

    LOG_INFO("translator", L"POST %ls（model=%ls，超时 %d 秒）",
             endpoint, cfg->model, cfg->request_timeout);

    /* 4. 发请求 */
    HttpResponse response;
    int timeout_ms = cfg->request_timeout * 1000;
    if (timeout_ms < 5000) timeout_ms = 5000;

    BOOL sent = http_post_json(endpoint, headers, body, strlen(body),
                               timeout_ms, &response);
    cJSON_free(body);
    heap_free(auth);

    if (!sent) {
        result->error = make_error(response.error[0] ? response.error
                                                     : L"网络请求失败");
        http_response_free(&response);
        return;
    }

    LOG_INFO("translator", L"响应 %d，%zu 字节",
             response.status, response.body_size);

    if (response.status < 200 || response.status >= 300) {
        log_api_error(response.status, response.body);
        const wchar_t *friendly = friendly_status(response.status);
        if (friendly != NULL) {
            result->error = make_error(friendly);
        } else {
            wchar_t text[128];
            _snwprintf_s(text, 128, _TRUNCATE, L"接口返回错误（%d）",
                         response.status);
            result->error = make_error(text);
        }
        http_response_free(&response);
        return;
    }

    /* 5. 解析 */
    cJSON *root = response.body ? cJSON_Parse(response.body) : NULL;
    if (root == NULL) {
        LOG_ERR("translator", L"响应不是合法 JSON");
        result->error = make_error(L"接口返回的内容无法解析");
        http_response_free(&response);
        return;
    }

    const char *content = extract_message_content(root);
    if (content == NULL) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (error != NULL) {
            LOG_ERR("translator", L"响应里带 error 字段");
            result->error = make_error(L"接口返回错误，详情见日志");
        } else {
            LOG_ERR("translator", L"响应里没有 choices[0].message.content");
            result->error = make_error(L"接口返回格式不符合预期");
        }
        cJSON_Delete(root);
        http_response_free(&response);
        return;
    }

    parse_model_content(content, result);
    cJSON_Delete(root);
    http_response_free(&response);

    if (result->error == NULL) {
        LOG_INFO("translator", L"翻译完成：原文 %zu 字，译文 %zu 字",
                 result->original ? wcslen(result->original) : 0,
                 result->translated ? wcslen(result->translated) : 0);
    }
}

/* ---------- 辅助接口（设置窗口用） ---------- */

/* 拼 "base/models" 或 "base/chat/completions"，容忍结尾斜杠 */
static void build_sub_url(const wchar_t *base, const wchar_t *suffix,
                          wchar_t *out, size_t out_count)
{
    wchar_t trimmed[1024];
    wstr_copy(trimmed, 1024, base);

    size_t len = wcslen(trimmed);
    while (len > 0 && trimmed[len - 1] == L'/') trimmed[--len] = 0;

    _snwprintf_s(out, out_count, _TRUNCATE, L"%ls%ls", trimmed, suffix);
}

static wchar_t *dup_or_null(const wchar_t *text)
{
    if (text == NULL) return NULL;
    return make_error(text);
}

/* 把 UTF-8 字符串转宽字符，失败返回 NULL */
static wchar_t *try_u16(const char *text)
{
    if (text == NULL) return NULL;
    return u8_to_u16(text);
}

void translator_fetch_models(const wchar_t *base_url, const wchar_t *api_key,
                             int timeout_sec,
                             wchar_t **out_text, wchar_t **out_error)
{
    if (out_text == NULL || out_error == NULL) return;
    *out_text = NULL;
    *out_error = NULL;

    if (base_url == NULL || base_url[0] == 0) {
        *out_error = make_error(L"请先填写 API 地址");
        return;
    }

    wchar_t url[1024];
    build_sub_url(base_url, L"/models", url, 1024);

    char *key = u16_to_u8(api_key ? api_key : L"");
    size_t auth_len = (key ? strlen(key) : 0) + 32;
    char *auth = (char *)heap_alloc(auth_len);
    if (auth == NULL) {
        heap_free(key);
        *out_error = make_error(L"内存不足");
        return;
    }
    snprintf(auth, auth_len, "Authorization: Bearer %s", key ? key : "");
    heap_free(key);

    const char *headers[] = { auth, NULL };

    int timeout_ms = (timeout_sec > 0 ? timeout_sec : 15) * 1000;
    HttpResponse response;
    BOOL sent = http_request_json(L"GET", url, headers, NULL, 0,
                                  timeout_ms, &response);
    heap_free(auth);

    if (!sent) {
        *out_error = dup_or_null(response.error[0] ? response.error
                                                   : L"网络请求失败");
        http_response_free(&response);
        return;
    }

    if (response.status < 200 || response.status >= 300) {
        log_api_error(response.status, response.body);
        const wchar_t *friendly = friendly_status(response.status);
        if (friendly != NULL) {
            *out_error = make_error(friendly);
        } else {
            wchar_t text[128];
            _snwprintf_s(text, 128, _TRUNCATE, L"接口返回错误（%d）",
                         response.status);
            *out_error = make_error(text);
        }
        http_response_free(&response);
        return;
    }

    cJSON *root = response.body ? cJSON_Parse(response.body) : NULL;
    if (root == NULL) {
        *out_error = make_error(L"接口返回的内容无法解析");
        http_response_free(&response);
        return;
    }

    /* 标准形状：{"data":[{"id":"gpt-4o-mini",...}, ...]} */
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        http_response_free(&response);
        *out_error = make_error(L"接口没有返回模型列表");
        return;
    }

    /* 先用 UTF-8 拼，最后一次性转宽字符 */
    size_t capacity = 1024;
    size_t used = 0;
    char *buffer = (char *)heap_alloc(capacity);
    if (buffer == NULL) {
        cJSON_Delete(root);
        http_response_free(&response);
        *out_error = make_error(L"内存不足");
        return;
    }
    buffer[0] = 0;

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, data) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!cJSON_IsString(id) || id->valuestring == NULL ||
            id->valuestring[0] == 0) {
            continue;
        }
        size_t need = used + strlen(id->valuestring) + 2;
        if (need > capacity) {
            size_t next = capacity;
            while (next < need) next *= 2;
            char *grown = (char *)heap_realloc(buffer, next);
            if (grown == NULL) break;
            buffer = grown;
            capacity = next;
        }
        if (used > 0) buffer[used++] = '\n';
        size_t n = strlen(id->valuestring);
        memcpy(buffer + used, id->valuestring, n);
        used += n;
        buffer[used] = 0;
        count++;
    }

    cJSON_Delete(root);
    http_response_free(&response);

    if (count == 0) {
        heap_free(buffer);
        *out_error = make_error(L"模型列表为空");
        return;
    }

    *out_text = u8_to_u16(buffer);
    heap_free(buffer);

    if (*out_text == NULL) {
        *out_error = make_error(L"模型名无法转换为文本");
        return;
    }

    LOG_INFO("translator", L"拉到 %d 个模型", count);
}

wchar_t *translator_test_connection(const wchar_t *base_url,
                                    const wchar_t *api_key,
                                    const wchar_t *model,
                                    int timeout_sec)
{
    if (base_url == NULL || base_url[0] == 0) {
        return make_error(L"请先填写 API 地址");
    }
    if (api_key == NULL || api_key[0] == 0) {
        return make_error(L"请先填写 API Key");
    }
    if (model == NULL || model[0] == 0) {
        return make_error(L"请先填写模型名");
    }

    wchar_t url[1024];
    build_sub_url(base_url, L"/chat/completions", url, 1024);

    /* 一个最小的文本请求：只要服务端能回 200 就说明地址、Key、模型都对 */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return make_error(L"内存不足");

    char *model_u8 = u16_to_u8(model);
    cJSON_AddStringToObject(root, "model", model_u8 ? model_u8 : "");
    heap_free(model_u8);
    /* 不要带 max_tokens：新版推理模型会拒绝这个参数，
       这里只要服务端能回 200 就说明配置可用。 */

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", "hi");
    cJSON_AddItemToArray(messages, msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return make_error(L"构造请求失败");

    char *key = u16_to_u8(api_key);
    size_t auth_len = (key ? strlen(key) : 0) + 32;
    char *auth = (char *)heap_alloc(auth_len);
    if (auth == NULL) {
        heap_free(key);
        cJSON_free(body);
        return make_error(L"内存不足");
    }
    snprintf(auth, auth_len, "Authorization: Bearer %s", key ? key : "");
    heap_free(key);

    const char *headers[] = {
        "Content-Type: application/json",
        auth,
        NULL
    };

    int timeout_ms = (timeout_sec > 0 ? timeout_sec : 15) * 1000;
    HttpResponse response;
    BOOL sent = http_post_json(url, headers, body, strlen(body),
                               timeout_ms, &response);

    heap_free(auth);
    cJSON_free(body);

    wchar_t *result = NULL;

    if (!sent) {
        result = dup_or_null(response.error[0] ? response.error
                                               : L"网络请求失败");
        http_response_free(&response);
        return result;
    }

    if (response.status < 200 || response.status >= 300) {
        log_api_error(response.status, response.body);
        const wchar_t *friendly = friendly_status(response.status);
        if (friendly != NULL) {
            result = make_error(friendly);
        } else {
            wchar_t text[128];
            _snwprintf_s(text, 128, _TRUNCATE, L"接口返回错误（%d）",
                         response.status);
            result = make_error(text);
        }
    } else {
        LOG_INFO("translator", L"连接测试通过（状态 %d）", response.status);
    }

    http_response_free(&response);
    return result;
}
