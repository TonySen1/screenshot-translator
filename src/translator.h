/* translator.h —— OpenAI 兼容的文本翻译与可选看图翻译 */
#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <windows.h>
#include <stddef.h>
#include "config.h"
#include "image.h"
#include "text_table.h"

typedef struct TranslationResult {
    wchar_t *original;      /* 识别出的原文（可能为空串） */
    wchar_t *translated;    /* 译文（可能为空串） */
    wchar_t *error;         /* 非 NULL 表示失败 */
    TextTable table;        /* 非空时以表格绘制；原文/译文用 TSV 复制 */
} TranslationResult;

/* 把选区缩放到长边 <=1568，再编码成 JPEG q85。
   调用方用 heap_free 释放 *out_image。 */
BOOL translator_prepare_image(const ImageBuffer *image,
                              void **out_image, size_t *out_size);

/* 在工作线程调用：base64 + HTTP + 解析。不触碰 GDI。
   任何失败都写进 result->error，不会抛异常。
   调用方负责 translation_result_free。 */
void translator_run_image(const Config *cfg, const void *image, size_t image_size,
                          TranslationResult *result);

/* 本地 OCR 文本；接管 source，借用可为空的 table_json。 */
void translator_run_text(const Config *cfg, wchar_t *source, const struct cJSON *table_json,
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


