/* http.h —— 最小 WinHTTP 封装，只够发一个带 bearer token 的 JSON POST */
#ifndef HTTP_H
#define HTTP_H

#include <windows.h>
#include <stddef.h>

typedef struct HttpResponse {
    int    status;          /* HTTP 状态码，0 表示请求没发出去 */
    char  *body;            /* UTF-8，以 0 结尾；可能为 NULL */
    size_t body_size;
    wchar_t error[256];     /* 失败原因（人话），成功时为空 */
} HttpResponse;

/* 通用 JSON 请求。method 为 L"GET" / L"POST" 等。
   url        完整 URL，例如 https://api.openai.com/v1/chat/completions
   headers    额外的请求头，每项一行（不含 \r\n），以 NULL 结尾；可为 NULL
   body       请求体，body_size 字节；GET 传 NULL/0
   timeout_ms 整个请求的超时
   成功发出并收到响应时返回 TRUE（即使状态码是 4xx/5xx）。
   连不上、超时、内存不足时返回 FALSE，原因写进 out->error。 */
BOOL http_request_json(const wchar_t *method, const wchar_t *url,
                       const char *const *headers,
                       const char *body, size_t body_size,
                       int timeout_ms, HttpResponse *out);

/* POST JSON 的简写 */
BOOL http_post_json(const wchar_t *url, const char *const *headers,
                    const char *body, size_t body_size,
                    int timeout_ms, HttpResponse *out);

void http_response_free(HttpResponse *resp);

#endif /* HTTP_H */
