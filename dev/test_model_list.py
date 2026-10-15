# -*- coding: utf-8 -*-
"""模型列表：解析、视觉模型判断、界面上的拉取与选择。

只做 OpenAI 兼容接口 —— 没有服务商预设那一层了。
界面部分需要能开窗口，拿不到显示器就跳过。
"""

import os
import shutil
import sys
import tempfile
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import config as config_module  # noqa: E402
import model_list  # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print("[%s] %s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def parse_checks() -> None:
    # OpenAI 标准形状
    ids = model_list._extract_ids({"object": "list", "data": [
        {"id": "gpt-4o", "object": "model"}, {"id": "gpt-3.5"}]})
    check("解析 OpenAI 标准的 data[].id", ids == ["gpt-4o", "gpt-3.5"], str(ids))

    # 有的服务直接返回数组，或 data 里放字符串
    check("兼容直接返回数组", model_list._extract_ids([{"id": "a"}, {"id": "b"}]) == ["a", "b"])
    check("兼容 data 里放字符串", model_list._extract_ids({"data": ["a", "b"]}) == ["a", "b"])
    check("兼容 models 字段", model_list._extract_ids({"models": [{"id": "x"}]}) == ["x"])
    check("兼容 name 字段", model_list._extract_ids({"data": [{"name": "n1"}]}) == ["n1"])
    check("去重", model_list._extract_ids({"data": [{"id": "a"}, {"id": "a"}]}) == ["a"])
    check("乱七八糟的形状不炸", model_list._extract_ids(None) == [])
    check("空对象不炸", model_list._extract_ids({}) == [])


def vision_checks() -> None:
    yes = ["Qwen/Qwen3-VL-8B-Instruct", "glm-4.6v-flash", "gpt-4o-mini",
           "claude-sonnet-4", "gemini-2.0-flash", "llava-1.5-7b",
           "Qwen/Qwen2.5-VL-72B-Instruct", "internvl2-8b"]
    no = ["Qwen/Qwen2.5-7B-Instruct", "deepseek-chat", "text-embedding-3-small",
          "bge-reranker-v2", "whisper-large-v3", "Qwen/Qwen2.5-Coder-7B"]
    for name in yes:
        check("认得视觉模型：%s" % name, model_list.looks_like_vision(name) is True)
    for name in no:
        check("不误判非视觉模型：%s" % name, model_list.looks_like_vision(name) is False)


def _stub_client(payload=None, exc=None):
    class FakeModels:
        def list(self):
            if exc is not None:
                raise exc
            return payload

    class FakeClient:
        models = FakeModels()

    return FakeClient()


def list_checks() -> None:
    cfg = config_module.Config()
    cfg.api_key = ""
    check("没填 Key 时不发请求", model_list.list_models(cfg).ok is False)

    cfg.api_key = "k"
    cfg.api_base_url = "not-a-url"
    result = model_list.list_models(cfg)
    check("地址不合法时报错", (not result.ok) and "地址" in result.message, result.message)

    cfg.api_base_url = "https://example.com/v1"

    # 正常：视觉模型排前面
    payload = {"data": [{"id": "deepseek-chat"}, {"id": "Qwen/Qwen3-VL-8B-Instruct"},
                        {"id": "text-embedding-3-small"}]}
    original = model_list.client_for
    model_list.client_for = lambda *a, **k: _stub_client(payload)
    try:
        result = model_list.list_models(cfg)
        check("拉取成功", result.ok, result.message)
        check("视觉模型排在前面", result.models[0] == "Qwen/Qwen3-VL-8B-Instruct",
              str(result.models))
        check("认出了视觉模型", result.vision == ["Qwen/Qwen3-VL-8B-Instruct"],
              str(result.vision))
        check("总数正确", len(result.models) == 3, str(len(result.models)))

        # 401
        class Unauthorized(Exception):
            status_code = 401

        model_list.client_for = lambda *a, **k: _stub_client(exc=Unauthorized("nope"))
        result = model_list.list_models(cfg)
        check("401 提示 Key 无效", (not result.ok) and "Key" in result.message, result.message)

        class NotFound(Exception):
            status_code = 404

        model_list.client_for = lambda *a, **k: _stub_client(exc=NotFound("nope"))
        result = model_list.list_models(cfg)
        check("404 提示地址不对", (not result.ok) and "地址" in result.message, result.message)

        # 空列表
        model_list.client_for = lambda *a, **k: _stub_client({"data": []})
        result = model_list.list_models(cfg)
        check("空列表算失败", (not result.ok) and "没有返回" in result.message, result.message)
    finally:
        model_list.client_for = original


def ui_checks() -> None:
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        print("跳过界面检查：%s" % exc, flush=True)
        return
    root.withdraw()

    import settings_window as sw  # noqa: E402
    import service_test as st  # noqa: E402
    sw.install_styles(root)
    tmp = tempfile.mkdtemp()
    try:
        cfg = config_module.load_config(os.path.join(tmp, "config.json"))
        cfg.api_key = "sk-test"
        cfg.model = "gpt-4o-mini"
        win = sw.SettingsWindow(root, cfg, on_save=lambda _c: None)
        win.win.deiconify()
        win.win.update()

        check("有接口地址输入框", win._base_url.get() == cfg.api_base_url, win._base_url.get())
        check("有 API Key 输入框", win._api_key.get() == "sk-test")
        check("模型下拉带当前值",
              any(item == "gpt-4o-mini" for _l, item in win._model._pairs),
              str([l for l, _ in win._model._pairs]))
        check("模型下拉没有「手动输入」了",
              all("手动输入" not in label for label, _ in win._model._pairs),
              str([l for l, _ in win._model._pairs]))
        check("没有服务商下拉了", not hasattr(win, "_service_select"))
        check("没有「申请 API」按钮了", not hasattr(win, "_apply_btn"))

        # 关窗图标的悬停换色：这里曾经因为漏 import 抛 NameError，
        # 而且只有鼠标划过 ✕ 才会触发，测试不点就发现不了
        try:
            win._close_button.event_generate("<Enter>", x=2, y=2)
            win.win.update()
            win._close_button.event_generate("<Leave>", x=2, y=2)
            win.win.update()
            check("鼠标划过关窗图标不报错", True)
        except tk.TclError as exc:
            check("鼠标划过关窗图标不报错", False, str(exc))

        # 拉取模型列表：走队列回传
        original_list = model_list.list_models
        model_list.list_models = lambda cfg_, timeout=None: model_list.ModelList(
            True, "ok", ["Qwen/Qwen3-VL-8B-Instruct", "deepseek-chat"],
            ["Qwen/Qwen3-VL-8B-Instruct"])
        try:
            win._fetch_models()
            for _ in range(40):
                root.update()
                if not win._fetching:
                    break
            names = [item for _l, item in win._model._pairs]
            check("拉到的模型进了下拉框",
                  "Qwen/Qwen3-VL-8B-Instruct" in names and "deepseek-chat" in names,
                  str(names))
            check("自动选中了视觉模型", win._model.value == "Qwen/Qwen3-VL-8B-Instruct",
                  win._model.value)
            check("状态栏不再显示译文",
                  "译文" not in win._status.cget("text"), win._status.cget("text"))
        finally:
            model_list.list_models = original_list

        # 测试服务：只有两种结论，且不显示译文
        original_test = st.test_service
        st.test_service = lambda cfg_, timeout=None: st.ServiceTestResult(
            True, st.OK_TEXT, "译文：42")
        try:
            win._test_service()
            for _ in range(40):
                root.update()
                if not win._testing:
                    break
            shown = win._status.cget("text")
            check("测试服务显示连接成功", shown == st.OK_TEXT, shown)
            check("界面上不出现译文内容", "译文" not in shown and "42" not in shown, shown)

            st.test_service = lambda cfg_, timeout=None: st.ServiceTestResult(
                False, st.FAIL_TEXT, "API Key 无效")
            win._test_service()
            for _ in range(40):
                root.update()
                if not win._testing:
                    break
            shown = win._status.cget("text")
            check("测试服务显示连接失败", shown == st.FAIL_TEXT, shown)
            check("失败原因不上界面", "Key" not in shown, shown)
        finally:
            st.test_service = original_test

        # 保存
        saved = []
        win._on_save = lambda c: saved.append(c)
        win._save()
        root.update()
        check("保存回调被触发", len(saved) == 1)
        if saved:
            kept = saved[0]
            check("保存了模型名", kept.model == "Qwen/Qwen3-VL-8B-Instruct", kept.model)
            check("保存了接口地址", kept.api_base_url == cfg.api_base_url, kept.api_base_url)
            check("不再有服务商字段", not hasattr(kept, "translation_service"))
            check("不再有 services 分段", not hasattr(kept, "services"))

        # 窗口销毁后：迟到结果不能炸、误点按钮直接返回
        win._test_queue.put(("test", st.ServiceTestResult(True, st.OK_TEXT)))
        win._poll_result()
        check("窗口销毁后收到迟到结果不抛异常", True)
        check("窗口销毁后不再排轮询", win._test_poll is None, str(win._test_poll))
        win._test_service()
        check("窗口销毁后点测试服务直接返回", win._testing is False)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        root.destroy()


def main() -> int:
    parse_checks()
    vision_checks()
    list_checks()
    ui_checks()
    passed = sum(RESULTS)
    print("\n%d/%d 通过" % (passed, len(RESULTS)), flush=True)
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
