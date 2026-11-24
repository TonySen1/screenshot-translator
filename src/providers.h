/* providers.h —— 无密钥文本翻译适配器与提供商标识 */
#ifndef PROVIDERS_H
#define PROVIDERS_H

#include <windows.h>

#include "config.h"

#define TRANSLATION_PROVIDER_DEEPL   L"deepl"
#define TRANSLATION_PROVIDER_OPENAI  L"openai-compatible"
#define TRANSLATION_PROVIDER_GOOGLE  L"google-web"
#define TRANSLATION_PROVIDER_TENCENT L"tencent-transmart"
#define TRANSLATION_PROVIDER_BING    L"bing-web"
#define TRANSLATION_PROVIDER_MS      L"microsoft-android"
#define TRANSLATION_PROVIDER_YOUDAO  L"youdao-web"
#define TRANSLATION_PROVIDER_YANDEX  L"yandex-web"

/* 配置缺省/兼容性校验。未知值不会静默改成另一个已配置服务。 */
BOOL provider_id_is_known(const wchar_t *provider);
const wchar_t *provider_default_for(const Config *cfg);
BOOL provider_is_openai(const Config *cfg);
BOOL provider_is_deepl(const Config *cfg);

/* Google Web endpoint：无密钥，按输入顺序返回；失败或数量不符时为 FALSE。 */
BOOL google_translate_batch(const wchar_t *target,
                            const wchar_t *const *sources, int count,
                            int timeout_sec, wchar_t **translations,
                            wchar_t **error);

/* 明确拒绝尚未安全实现的网页/签名服务，避免隐式回退或收集 Cookie。 */
wchar_t *provider_unavailable_error(const wchar_t *provider);

#endif /* PROVIDERS_H */
