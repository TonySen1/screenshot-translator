/* providers.h —— 文本翻译适配器与提供商注册表 */
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
#define TRANSLATION_PROVIDER_YOUDAO  L"youdao"
#define TRANSLATION_PROVIDER_YANDEX  L"yandex-web"

/* 一个提供商的全部静态事实：真实接口地址、密钥字段叫什么、密钥在哪申请。
   设置窗口只按这张表渲染，不再为某个提供商单开一行控件。 */
typedef struct ProviderInfo {
    const wchar_t *id;          /* TRANSLATION_PROVIDER_* */
    const wchar_t *name;        /* 界面显示名 */
    const wchar_t *base_url;    /* 真实接口地址（只读回显，或 OpenAI 兼容的默认值） */
    const wchar_t *key_label;   /* 密钥输入框的标签 */
    const wchar_t *key_url;     /* 密钥/接口申请页；NULL 表示无需申请 */
    const wchar_t *hint;        /* 一行补充说明，地址也写在这里方便复制 */
    BOOL needs_key;
    BOOL needs_model;
    BOOL editable_base;         /* 地址是否允许用户改（只有 OpenAI 兼容允许） */
    BOOL implemented;           /* 翻译与测试是否已实现 */
    BOOL hidden;                /* 暂时不在设置页出现；适配器与历史配置仍认得它 */
} ProviderInfo;

/* 注册表按界面下拉的顺序排列；隐藏项只是过滤掉，不改变其余项的先后。 */
const ProviderInfo *provider_table(int *count);
const ProviderInfo *provider_at(int index);
const ProviderInfo *provider_info(const wchar_t *provider);
int provider_index_of(const wchar_t *provider);

/* 设置页只用这三个：下拉索引就是可见列表索引。 */
int provider_visible_count(void);
const ProviderInfo *provider_visible_at(int index);
int provider_visible_index_of(const wchar_t *provider);
BOOL provider_is_visible(const wchar_t *provider);

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

/* 无密钥可用性检查：发一条最小文本，成功返回 NULL，失败返回中文原因。 */
wchar_t *google_test_connection(const wchar_t *target, int timeout_sec);

/* 微软翻译的免费端点（edge.microsoft.com/translate/translatetext）：无需密钥，
   按输入顺序返回；失败或数量不符时为 FALSE。 */
BOOL bing_translate_batch(const wchar_t *target,
                          const wchar_t *const *sources, int count,
                          int timeout_sec, wchar_t **translations,
                          wchar_t **error);
wchar_t *bing_test_connection(const wchar_t *target, int timeout_sec);

/* 明确拒绝尚未安全实现的网页/签名服务，避免隐式回退或收集 Cookie。 */
wchar_t *provider_unavailable_error(const wchar_t *provider);

#endif /* PROVIDERS_H */
