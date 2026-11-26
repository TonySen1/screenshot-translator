/* youdao.h —— 有道智云开放平台文本翻译（NMT）
 *
 * 与网页版 fanyi.youdao.com 是两套东西：这里走 openapi.youdao.com，
 * 凭据是「应用ID:应用密钥」，签名 v3 = SHA256(appKey + truncate(q) + salt + curtime + appSecret)。
 */
#ifndef YOUDAO_H
#define YOUDAO_H

#include <windows.h>

#define YOUDAO_MAX_TEXTS 50

/* 凭据格式：appKey:appSecret，两段都非空、不含空白。 */
BOOL youdao_key_is_valid(const wchar_t *key);

/* 目标语言名/代码 -> 有道语言码（zh-CHS、zh-CHT、en、ja ...） */
BOOL youdao_target_language(const wchar_t *name, char out[16]);

/* 逐条翻译，按输入顺序返回同样数量的译文。
   有道把多行文本按行拆开返回，容易错位，这里一条一请求保证对齐。
   失败时释放已产出的项。 */
BOOL youdao_translate_batch(const wchar_t *key, const wchar_t *target,
                            const wchar_t *const *sources, int count,
                            int timeout_sec, wchar_t **translations,
                            wchar_t **error);

void youdao_translate(const wchar_t *key, const wchar_t *target,
                      const wchar_t *source, int timeout_sec,
                      wchar_t **translated, wchar_t **error);

/* 用一条最小文本验证凭据、签名与额度；成功返回 NULL，失败返回中文原因。 */
wchar_t *youdao_test_connection(const wchar_t *key, int timeout_sec);

#endif /* YOUDAO_H */