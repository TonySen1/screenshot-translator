# -*- coding: utf-8 -*-
"""用本地 mock 服务测 translator：正常/围栏/纯文本/401/超时/非 JSON。"""

import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PIL import Image  # noqa: E402

import config as config_module  # noqa: E402
import translator  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 服务端行为由这个变量控制，测试按顺序切换
MODE = {"value": "ok"}
LAST_REQUEST = {}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            LAST_REQUEST.update(json.loads(body))
        except ValueError:
            LAST_REQUEST.clear()

        mode = MODE["value"]
        if mode == "401":
            payload = {"error": {"message": "Incorrect API key provided", "type": "invalid_request_error"}}
        elif mode == "429":
            payload = {"error": {"message": "Rate limit reached"}}
        elif mode == "500":
            payload = {"error": {"message": "server exploded"}}
        elif mode == "hang":
            import time
            time.sleep(4)
            payload = self._completion("太慢了")
        else:
            if mode == "ok":
                content = json.dumps({"original_text": "Hello world", "translated_text": "你好，世界"})
            elif mode == "fenced":
                content = "```json\n{\"original_text\": \"Hi\", \"translated_text\": \"嗨\"}\n```"
            elif mode == "chatty":
                content = "Sure! Here is the JSON:\n{\"original_text\": \"Cat\", \"translated_text\": \"猫\"}\nHope that helps!"
            elif mode == "plain":
                content = "这是一段没有 JSON 的纯文本译文"
            elif mode == "empty":
                content = ""
            elif mode == "altkeys":
                content = "<original>TEXT</original><translated>译文</translated>"
            else:
                content = mode
            payload = self._completion(content)

        data = json.dumps(payload).encode("utf-8")
        self.send_response(self._code())
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _code(self):
        return {"401": 401, "429": 429, "500": 500}.get(MODE["value"], 200)

    @staticmethod
    def _completion(content):
        return {
            "id": "chatcmpl-mock",
            "object": "chat.completion",
            "created": 0,
            "model": "mock-model",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": content},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        }


def make_cfg(port, key="sk-test"):
    cfg = config_module.Config()
    cfg.api_base_url = "http://127.0.0.1:%d/v1" % port
    cfg.api_key = key
    cfg.model = "mock-model"
    cfg.request_timeout = 2
    return cfg


def run_case(server_port, mode, expect_error, expect_original=None, expect_translated=None):
    MODE["value"] = mode
    cfg = make_cfg(server_port)
    image = Image.new("RGB", (200, 60), (255, 255, 255))
    result = translator.translate_image(image, cfg)
    ok = True
    detail = []
    if expect_error:
        if result.ok:
            ok = False
            detail.append("期望报错但成功了")
        else:
            detail.append("错误信息=%r" % result.error)
    else:
        if not result.ok:
            ok = False
            detail.append("期望成功但报错: %s" % result.error)
        else:
            detail.append("原文=%r 译文=%r" % (result.original_text, result.translated_text))
            if expect_original is not None and result.original_text != expect_original:
                ok = False
                detail.append("原文不符 (期望 %r)" % expect_original)
            if expect_translated is not None and result.translated_text != expect_translated:
                ok = False
                detail.append("译文不符 (期望 %r)" % expect_translated)
    print("[%s] %s  %s" % ("PASS" if ok else "FAIL", mode, " | ".join(detail)), flush=True)
    return ok


def main() -> int:
    server = HTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print("mock 服务端口:", port, flush=True)

    results = []
    results.append(run_case(port, "ok", False, "Hello world", "你好，世界"))
    results.append(run_case(port, "fenced", False, "Hi", "嗨"))
    results.append(run_case(port, "chatty", False, "Cat", "猫"))
    results.append(run_case(port, "plain", False, None, "这是一段没有 JSON 的纯文本译文"))
    results.append(run_case(port, "empty", True))
    results.append(run_case(port, "401", True))
    results.append(run_case(port, "429", True))
    results.append(run_case(port, "500", True))
    results.append(run_case(port, "hang", True))

    # 未填 api_key
    MODE["value"] = "ok"
    cfg = make_cfg(port, key="")
    r = translator.translate_image(Image.new("RGB", (10, 10)), cfg)
    ok = (not r.ok) and ("api_key" in r.error)
    results.append(ok)
    print("[%s] 未填 api_key -> %r" % ("PASS" if ok else "FAIL", r.error), flush=True)

    # 检查请求体结构
    print("--- 请求体检查 ---", flush=True)
    req_ok = True
    msgs = LAST_REQUEST.get("messages", [])
    if len(msgs) != 2:
        req_ok = False
    else:
        content = msgs[1]["content"]
        url = content[1]["image_url"]["url"]
        checks = {
            "model": LAST_REQUEST.get("model") == "mock-model",
            "system 提示词": msgs[0]["role"] == "system" and "JSON" in msgs[0]["content"],
            "user 含图片": url.startswith("data:image/png;base64,"),
            "base64 非空": len(url) > 200,
        }
        for name, cond in checks.items():
            print("   %s: %s" % (name, "OK" if cond else "FAIL"), flush=True)
            req_ok = req_ok and cond
    results.append(req_ok)

    # 图片缩放
    print("--- 图片预处理 ---", flush=True)
    for size, name in [((40, 20), "小图放大"), ((3000, 2000), "大图压缩"), ((800, 300), "不变")]:
        out = translator.prepare_image(Image.new("RGB", size))
        print("   %s %s -> %s" % (name, size, out.size), flush=True)

    if all(results):
        print("\n全部通过 (%d/%d)" % (sum(results), len(results)), flush=True)
        return 0
    print("\n有失败 (%d/%d)" % (sum(results), len(results)), flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
