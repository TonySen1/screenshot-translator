/* http.c —— WinHTTP 的最小封装 */
#include "http.h"
#include "util.h"

#include <winhttp.h>

#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define RESPONSE_LIMIT (16 * 1024 * 1024)   /* 16 MB，足够任何模型回复 */

static void set_error(HttpResponse *out, const wchar_t *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(out->error, 256, _TRUNCATE, fmt, args);
    va_end(args);
}

/* "https://host:port/path" -> 组件。返回 FALSE 表示 URL 无法解析。 */
static BOOL crack_url(const wchar_t *url, BOOL *secure,
                      wchar_t *host, size_t host_count,
                      wchar_t *path, size_t path_count,
                      INTERNET_PORT *port)
{
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)host_count;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)path_count;

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return FALSE;

    *secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    *port = uc.nPort;

    if (host[0] == 0 || path[0] == 0) return FALSE;
    return TRUE;
}

BOOL http_request_json(const wchar_t *method, const wchar_t *url,
                       const char *const *headers,
                       const char *body, size_t body_size,
                       int timeout_ms, HttpResponse *out)
{
    if (method == NULL) method = L"GET";
    if (url == NULL || out == NULL) return FALSE;

    memset(out, 0, sizeof(*out));
    if (body_size > 0x7FFFFFFF) {
        set_error(out, L"请求体过大");
        return FALSE;
    }
    if (timeout_ms < 1000) timeout_ms = 1000;

    BOOL secure = TRUE;
    wchar_t host[256];
    wchar_t path[2048];
    INTERNET_PORT port = 0;

    if (!crack_url(url, &secure, host, 256, path, 2048, &port)) {
        set_error(out, L"api_base_url 格式不正确：%s", url);
        return FALSE;
    }

    HINTERNET session = WinHttpOpen(L"ScreenshotTranslator/0.1",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == NULL) {
        set_error(out, L"WinHttpOpen 失败（错误 %lu）", GetLastError());
        return FALSE;
    }

    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    BOOL ok = FALSE;
    HINTERNET connect = NULL;
    HINTERNET request = NULL;
    char *buffer = NULL;

    connect = WinHttpConnect(session, host, port, 0);
    if (connect == NULL) {
        set_error(out, L"连接 %s 失败（错误 %lu）", host, GetLastError());
        goto cleanup;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    request = WinHttpOpenRequest(connect, method, path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (request == NULL) {
        set_error(out, L"创建请求失败（错误 %lu）", GetLastError());
        goto cleanup;
    }

    for (int i = 0; headers != NULL && headers[i] != NULL; i++) {
        wchar_t *wide = u8_to_u16(headers[i]);
        if (wide == NULL) continue;
        WinHttpAddRequestHeaders(request, wide,
                                 (DWORD)-1L,
                                 WINHTTP_ADDREQ_FLAG_ADD |
                                 WINHTTP_ADDREQ_FLAG_REPLACE);
        heap_free(wide);
    }

    if (!WinHttpSendRequest(request,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            (LPVOID)body,
                            (DWORD)body_size, (DWORD)body_size, 0)) {
        set_error(out, L"发送请求失败（错误 %lu）", GetLastError());
        goto cleanup;
    }

    if (!WinHttpReceiveResponse(request, NULL)) {
        set_error(out, L"等待响应失败（错误 %lu）", GetLastError());
        goto cleanup;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &status_size, WINHTTP_NO_HEADER_INDEX)) {
        set_error(out, L"读取状态码失败（错误 %lu）", GetLastError());
        goto cleanup;
    }
    out->status = (int)status;

    size_t capacity = 8192;
    size_t used = 0;
    buffer = (char *)heap_alloc(capacity + 1);
    if (buffer == NULL) {
        set_error(out, L"内存不足");
        goto cleanup;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            set_error(out, L"读取响应长度失败（错误 %lu）", GetLastError());
            goto cleanup;
        }
        if (available == 0) break;

        if (used + available > RESPONSE_LIMIT) {
            set_error(out, L"响应超过 %d MB，已放弃", RESPONSE_LIMIT / (1024 * 1024));
            goto cleanup;
        }

        if (used + available + 1 > capacity) {
            size_t next = capacity;
            while (next < used + available + 1) next *= 2;
            char *grown = (char *)heap_realloc(buffer, next + 1);
            if (grown == NULL) {
                set_error(out, L"内存不足");
                goto cleanup;
            }
            buffer = grown;
            capacity = next;
        }

        DWORD read = 0;
        if (!WinHttpReadData(request, buffer + used, available, &read)) {
            set_error(out, L"读取响应失败（错误 %lu）", GetLastError());
            goto cleanup;
        }
        used += read;
        if (read == 0) break;
    }

    buffer[used] = 0;
    out->body = buffer;
    out->body_size = used;
    buffer = NULL;
    ok = TRUE;

cleanup:
    if (buffer != NULL) heap_free(buffer);
    if (request != NULL) WinHttpCloseHandle(request);
    if (connect != NULL) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

BOOL http_post_json(const wchar_t *url, const char *const *headers,
                    const char *body, size_t body_size,
                    int timeout_ms, HttpResponse *out)
{
    return http_request_json(L"POST", url, headers, body, body_size,
                             timeout_ms, out);
}

void http_response_free(HttpResponse *resp)
{
    if (resp == NULL) return;
    heap_free(resp->body);
    memset(resp, 0, sizeof(*resp));
}
