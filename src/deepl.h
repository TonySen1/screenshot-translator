/* DeepL 文本翻译；联网调用均在后台线程执行。 */
#ifndef DEEPL_H
#define DEEPL_H

#include <windows.h>

#define DEEPL_TARGET_CAPACITY 16
#define DEEPL_MAX_TEXTS 50

BOOL deepl_key_is_valid(const wchar_t *key);
BOOL deepl_target_language(const wchar_t *name, char out[DEEPL_TARGET_CAPACITY]);

/* 真实端点主机：以 :fx 结尾的免费版密钥走 api-free.deepl.com，其余走 api.deepl.com。
   返回静态字符串，不含 /v2 路径。 */
const wchar_t *deepl_api_base(const wchar_t *key);

/* 输出由调用方使用 heap_free 释放。 */
void deepl_translate(const wchar_t *key, const wchar_t *target,
                     const wchar_t *source, int timeout_sec,
                     wchar_t **translated, wchar_t **error);
/* 按输入顺序返回同样数量的译文；失败时释放已解析的单元格。 */
BOOL deepl_translate_batch(const wchar_t *key, const wchar_t *target,
                           const wchar_t *const *sources, int count, int timeout_sec,
                           wchar_t **translations, wchar_t **error);
/* GET /v2/usage，不提交文本、不消耗翻译额度；NULL 表示成功。 */
wchar_t *deepl_test_connection(const wchar_t *key, int timeout_sec);

#endif
