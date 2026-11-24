"""Deterministic provider-adapter contracts; no network and no credentials."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROVIDERS = (ROOT / "src" / "providers.c").read_text(encoding="utf-8")
PROVIDER_H = (ROOT / "src" / "providers.h").read_text(encoding="utf-8")
CONFIG = (ROOT / "src" / "config.c").read_text(encoding="utf-8")
SETTINGS = (ROOT / "src" / "settings.c").read_text(encoding="utf-8")


class ProviderRequestTests(unittest.TestCase):
    def test_google_endpoint_is_fixed_and_auto_source(self):
        self.assertIn('https://translate.googleapis.com/translate_a/t', PROVIDERS)
        self.assertIn('&sl=auto&', PROVIDERS)
        self.assertIn('&tl=%hs&', PROVIDERS)
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

    def test_appearance_fields_remain_serialized_for_compatibility(self):
        for field in ('mask_opacity', 'overlay_opacity', 'font_size'):
            self.assertIn(field, CONFIG)

    def test_settings_does_not_expose_appearance_controls(self):
        for removed in ('IDC_SLD_', 'IDC_VAL_', 'mask_opacity',
                        'overlay_opacity', 'font_size', 'WM_HSCROLL'):
            self.assertNotIn(removed, SETTINGS)


if __name__ == '__main__':
    unittest.main(verbosity=2)
