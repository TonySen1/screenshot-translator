/* translator.h —— 调 OpenAI 兼容的多模态接口做 OCR + 翻译 */
#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <windows.h>
#include <stddef.h>
#include "config.h"
#include "image.h"

typedef struct TranslationResult {
    wchar_t *original;      /* 识别出的原文（可能为空串） */
    wchar_t *translated;    /* 译文（可能为空串） */
    wchar_t *error;         /* 非 NULL 表示失败，此时上面两项为空串 */
} TranslationResult;

/* 在主线程调用：把选区缩放到长边 <=1568，再编码成 JPEG q85。
   调用方用 heap_free 释放 *out_image。 */
BOOL translator_prepare_image(const ImageBuffer *image,
                              void **out_image, size_t *out_size);

/* 在工作线程调用：base64 + HTTP + 解析。不触碰 GDI。
   任何失败都写进 result->error，不会抛异常。
   调用方负责 translation_result_free。 */
void translator_run_image(const Config *cfg, const void *image, size_t image_size,
                          TranslationResult *result);

void translation_result_free(TranslationResult *result);

/* 调用 GET /models 拉模型列表。成功时 *out_text 是一行一个模型名的
   宽字符串（调用方 heap_free），失败时 *out_error 是中文原因。
   两者只需释放非 NULL 的那个。 */
void translator_fetch_models(const wchar_t *base_url, const wchar_t *api_key,
                             int timeout_sec,
                             wchar_t **out_text, wchar_t **out_error);

/* 用给定配置做一次最小可用性检查（不带图片，只验证鉴权和连通性）。
   成功返回 NULL，失败返回中文原因（调用方 heap_free）。 */
wchar_t *translator_test_connection(const wchar_t *base_url,
                                    const wchar_t *api_key,
                                    const wchar_t *model,
                                    int timeout_sec);

#endif /* TRANSLATOR_H */


