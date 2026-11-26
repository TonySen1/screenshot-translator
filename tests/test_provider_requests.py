"""Deterministic provider-adapter contracts; no network and no credentials."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROVIDERS = (ROOT / "src" / "providers.c").read_text(encoding="utf-8")
PROVIDER_H = (ROOT / "src" / "providers.h").read_text(encoding="utf-8")
CONFIG = (ROOT / "src" / "config.c").read_text(encoding="utf-8")
SETTINGS = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")
TRANSLATOR = (ROOT / "src" / "translator.c").read_text(encoding="utf-8")
YOUDAO_C = (ROOT / "src" / "youdao.c").read_text(encoding="utf-8")
YOUDAO_H = (ROOT / "src" / "youdao.h").read_text(encoding="utf-8")


class ProviderRequestTests(unittest.TestCase):
    def test_google_endpoint_is_fixed_and_auto_source(self):
        self.assertIn('https://translate.googleapis.com/translate_a/t', PROVIDERS)
        self.assertIn('&sl=auto&tl=%hs', PROVIDERS)
        self.assertIn('application/x-www-form-urlencoded', PROVIDERS)
        self.assertIn("(*body)[(*used)++] = 'q';", PROVIDERS)
        self.assertIn("body[used++] = '&';", PROVIDERS)

    def test_google_request_rejects_unordered_batch_shapes(self):
        self.assertIn('cJSON_GetArraySize(root) != count', PROVIDERS)
        self.assertIn('表格已拒绝对齐', PROVIDERS)
        self.assertIn('translations[i] = NULL', PROVIDERS)

    def test_provider_default_and_explicit_unsupported_ids(self):
        self.assertIn('translation_provider', CONFIG)
        self.assertIn('provider_default_for(cfg)', CONFIG)
        for provider in (
            'TRANSLATION_PROVIDER_BING', 'TRANSLATION_PROVIDER_MS',
            'TRANSLATION_PROVIDER_YOUDAO', 'TRANSLATION_PROVIDER_TENCENT',
            'TRANSLATION_PROVIDER_YANDEX',
        ):
            self.assertIn(provider, PROVIDER_H)
            self.assertIn(provider, PROVIDERS)

    def test_registry_carries_real_endpoints_and_key_pages(self):
        endpoints = (
            'https://api.deepl.com/v2',
            'https://api.openai.com/v1',
            'https://translate.googleapis.com/translate_a/t',
            'https://transmart.qq.com/api/imt',
            'https://edge.microsoft.com/translate/translatetext',
            'https://api-edge.cognitive.microsofttranslator.com',
            'https://openapi.youdao.com/api',
            'https://translate.yandex.net/api/v1.5/tr.json/translate',
        )
        for url in endpoints:
            self.assertIn(url, PROVIDERS)

        key_pages = (
            'https://www.deepl.com/pro-api',
            'https://platform.openai.com/api-keys',
            'https://console.cloud.tencent.com/tmt',
            'https://ai.youdao.com/console/',
            'https://translate.yandex.com/developers/keys',
        )
        for url in key_pages:
            self.assertIn(url, PROVIDERS)

    def test_registry_is_the_only_source_of_provider_facts(self):
        for symbol in ('ProviderInfo', 'provider_table', 'provider_at',
                       'provider_index_of', 'provider_info'):
            self.assertIn(symbol, PROVIDER_H)
            self.assertIn(symbol, PROVIDERS)
        # 下拉项由注册表的可见列表生成，不再手写一串 CB_ADDSTRING 字面量
        self.assertIn('provider_visible_count()', SETTINGS)

    def test_settings_tests_whichever_provider_is_selected(self):
        # 只留一个测试按钮：旧的双按钮会各自写死 DeepL / OpenAI 一条路径
        self.assertIn('IDC_BTN_TEST', SETTINGS)
        self.assertNotIn('IDC_BTN_DEEPL_TEST', SETTINGS)
        self.assertNotIn('IDC_EDIT_DEEPL_KEY', SETTINGS)
        self.assertNotIn('测试 DeepL', SETTINGS)

        # 工作线程按 job->provider 分派，三种已实现服务各有真实调用
        self.assertIn('start_test_job', SETTINGS)
        for call in ('deepl_test_connection', 'google_test_connection',
                     'translator_test_connection'):
            self.assertIn(call, SETTINGS)

    def test_settings_offers_key_page_for_selected_provider(self):
        self.assertIn('IDC_BTN_KEY_PAGE', SETTINGS)
        self.assertIn('open_provider_key_page', SETTINGS)
        self.assertIn('key_url', PROVIDER_H)

    def test_youdao_uses_open_platform_api_with_signature(self):
        # 开放平台端点，不是网页版 fanyi.youdao.com
        self.assertIn('https://openapi.youdao.com/api', YOUDAO_C)
        self.assertNotIn('fanyi.youdao.com', YOUDAO_C)

        # 签名 v3：系统 SHA-256 + 四个必需参数
        self.assertIn('BCRYPT_SHA256_ALGORITHM', YOUDAO_C)
        for field in ('"appKey"', '"salt"', '"curtime"', '"sign"',
                      '"signType"', '"v3"', '"from"', '"to"', '"q"'):
            self.assertIn(field, YOUDAO_C)

        # 凭据格式「应用ID:应用密钥」
        self.assertIn('youdao_key_is_valid', YOUDAO_H)
        self.assertIn("wcschr(key, L':')", YOUDAO_C)

        # 只向官方 HTTPS 端点、不跟随重定向
        self.assertIn('http_request_json_no_redirect', YOUDAO_C)

        # 接进了翻译分派、配置项与设置页测试
        self.assertIn('TRANSLATION_PROVIDER_YOUDAO', TRANSLATOR)
        self.assertIn('youdao_api_key', CONFIG)
        self.assertIn('youdao_test_connection', SETTINGS)

    @staticmethod
    def provider_block(provider_id):
        """返回注册表里某个 id 所在的那一条（条目以 '},' 结束，内部不含该串）。"""
        start = PROVIDERS.index(provider_id)
        end = PROVIDERS.index('},', start)
        return PROVIDERS[start:end]

    def test_only_tencent_microsoft_yandex_stay_hidden(self):
        for pid in ('TRANSLATION_PROVIDER_TENCENT', 'TRANSLATION_PROVIDER_MS',
                    'TRANSLATION_PROVIDER_YANDEX'):
            self.assertIn('PROVIDER_HIDDEN', self.provider_block(pid), pid)

        for pid in ('TRANSLATION_PROVIDER_DEEPL', 'TRANSLATION_PROVIDER_OPENAI',
                    'TRANSLATION_PROVIDER_YOUDAO', 'TRANSLATION_PROVIDER_BING',
                    'TRANSLATION_PROVIDER_GOOGLE'):
            self.assertIn('PROVIDER_VISIBLE', self.provider_block(pid), pid)

        # 设置页只按可见列表建下拉；全表索引不能再漏回界面
        self.assertIn('provider_visible_count()', SETTINGS)
        self.assertIn('provider_visible_at(', SETTINGS)
        self.assertNotIn('provider_at(', SETTINGS)
        self.assertNotIn('provider_table(', SETTINGS)

        # 配置里若还留着隐藏通道，加载时回退到默认项
        self.assertIn('provider_is_visible', CONFIG)

    def test_google_uses_gtx_client_not_webapp(self):
        # client=webapp 这类网页客户端会被拒（实测 403）；gtx 才是公开文本端点
        self.assertIn('client=gtx&dt=t', PROVIDERS)
        self.assertNotIn('client=webapp', PROVIDERS)
        self.assertIn('&sl=auto&tl=%hs', PROVIDERS)

    def test_bing_uses_keyless_edge_endpoint(self):
        self.assertIn('https://edge.microsoft.com/translate/translatetext', PROVIDERS)
        self.assertIn('isEnterpriseClient=false', PROVIDERS)
        self.assertIn('"zh-Hans"', PROVIDERS)
        self.assertIn('"zh-Hant"', PROVIDERS)
        # 免密钥：设置页测试走 bing_test_connection，翻译走同一适配器
        self.assertIn('bing_test_connection', SETTINGS)
        self.assertIn('TRANSLATION_PROVIDER_BING', TRANSLATOR)
        block = self.provider_block('TRANSLATION_PROVIDER_BING')
        self.assertIn('PROVIDER_VISIBLE', block)
        self.assertNotIn('PROVIDER_HIDDEN', block)

    def test_appearance_fields_remain_serialized_for_compatibility(self):
        for field in ('mask_opacity', 'overlay_opacity', 'font_size'):
            self.assertIn(field, CONFIG)

    def test_settings_does_not_expose_appearance_controls(self):
        for removed in ('IDC_SLD_', 'IDC_VAL_', 'mask_opacity',
                        'overlay_opacity', 'font_size', 'WM_HSCROLL'):
            self.assertNotIn(removed, SETTINGS)


if __name__ == '__main__':
    unittest.main(verbosity=2)
