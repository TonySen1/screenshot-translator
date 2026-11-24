"""Exercise the actual provisioned child, never an in-process OCR substitute.

From the repository root (PowerShell):
  .\build\ocr\python.exe -I -B -X utf8 .\tests\smoke_ocr.py
Japanese/Korean optional Windows fonts must be installed for rendered tests.
Missing fonts FAIL with instructions rather than silently skipping languages.
Images, stderr and timing JSON go to --artifacts / PI_SCRATCH_DIR / user temp.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time

from PIL import Image, ImageDraw, ImageFont


def exact(stream, size):
    data = bytearray()
    while len(data) < size:
        part = stream.read(size - len(data))
        if not part:
            if not data:
                return None
            raise EOFError("truncated response")
        data.extend(part)
    return bytes(data)


class Child:
    def __init__(self, runtime, artifacts, worker=None):
        self.events = queue.Queue()
        self.stderr = tempfile.TemporaryFile(dir=artifacts)
        env = dict(os.environ)
        for key in ("PYTHONPATH", "PYTHONHOME"):
            env.pop(key, None)
        env["PATH"] = str(Path(env.get("SystemRoot", "C:/Windows")) / "System32")
        self.process = subprocess.Popen(
            [str(runtime / "python.exe"), "-I", "-B", "-X", "utf8", str(worker or runtime / "worker.py")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            cwd=artifacts, env=env)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                header = exact(self.process.stdout, 4)
                if header is None:
                    self.events.put(None)
                    return
                length = struct.unpack("<I", header)[0]
                assert 0 < length <= 4_000_000, "stdout log pollution / invalid frame length"
                payload = exact(self.process.stdout, length)
                assert payload is not None
                self.events.put(json.loads(payload.decode("utf-8")))
        except BaseException as exc:
            self.events.put(exc)

    def response(self, timeout=120):
        result = self.events.get(timeout=timeout)
        if isinstance(result, BaseException):
            raise result
        assert result is not None, "worker exited without a response"
        return result

    def send(self, image, fragmented=False):
        pixels = image.convert("RGBA").tobytes("raw", "BGRA")
        data = struct.pack("<II", *image.size) + pixels
        if fragmented:
            for start, stop in ((0, 3), (3, 8), (8, 25), (25, len(data))):
                self.process.stdin.write(data[start:stop])
                self.process.stdin.flush()
        else:
            self.process.stdin.write(data)
            self.process.stdin.flush()
        return self.response()

    def finish(self, code=0):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        assert self.process.wait(timeout=30) == code
        self.reader.join(timeout=5)
        assert self.events.get(timeout=5) is None, "extra response for one request"
        assert self.events.empty()
        self.stderr.seek(0)
        logs = self.stderr.read().decode("utf-8", "replace")
        self.stderr.close()
        self.process.stdout.close()
        return logs

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)
        if not self.process.stdin.closed:
            self.process.stdin.close()
        self.reader.join(timeout=5)
        self.process.stdout.close()
        if not self.stderr.closed:
            self.stderr.close()


def font(name, size):
    path = Path(os.environ.get("SystemRoot", "C:/Windows")) / "Fonts" / name
    if not path.is_file():
        raise RuntimeError("Install Windows Japanese/Korean supplemental fonts; missing: " + str(path))
    return ImageFont.truetype(str(path), size)


def render(rows, size=(1100, 160), dark=False):
    image = Image.new("RGB", size, (24, 24, 24) if dark else "white")
    draw = ImageDraw.Draw(image)
    for text, face, points, x, y in rows:
        draw.text((x, y), text, font=font(face, points), fill=(240, 240, 240) if dark else "black")
    return image


def normalize(text):
    return "".join(text.split())


def check_result(result, expected, width, height):
    assert "error" not in result, result
    assert result["text"] == "\n".join(line["text"] for line in result["lines"])
    for text in expected:
        assert normalize(text) in normalize(result["text"]), (text, result)
    for line in result["lines"]:
        assert line["text"] and len(line["box"]) == 4
        for x, y in line["box"]:
            assert isinstance(x, int) and isinstance(y, int)
            assert 0 <= x < width and 0 <= y < height


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, default=Path(__file__).resolve().parents[1] / "build/ocr")
    parser.add_argument("--artifacts", type=Path, default=Path(os.environ.get("PI_SCRATCH_DIR", tempfile.gettempdir())) / "ocr-smoke")
    args = parser.parse_args()
    runtime, artifacts = args.runtime.resolve(), args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    samples = [
        ("english", render([("The quick brown fox jumps over the lazy dog.", "segoeui.ttf", 32, 24, 24)]), ["The quick brown fox jumps over the lazy dog."]),
        ("english_dark_18px", render([("Settings Save Cancel Continue", "segoeui.ttf", 18, 24, 24)], dark=True), ["Settings Save Cancel Continue"]),
        ("japanese", render([("日本語のスクリーンショットを翻訳します。", "YuGothM.ttc", 32, 24, 24)]), ["日本語のスクリーンショットを翻訳します。"]),
        ("korean", render([("안녕하세요. 스크린샷을 번역합니다.", "malgun.ttf", 32, 24, 24)]), ["안녕하세요.", "스크린샷을 번역합니다."]),
        ("english_korean_same_line", render([("Settings 설정", "malgun.ttf", 32, 24, 24)]), ["Settings 설정"]),
        ("korean_ui_18px", render([("파일 편집 보기 도움말", "malgun.ttf", 18, 24, 24)]), ["파일 편집 보기 도움말"]),
        ("mixed", render([
            ("Hello world! Capture and translate.", "segoeui.ttf", 32, 24, 24),
            ("日本語のスクリーンショットを翻訳します。", "YuGothM.ttc", 32, 24, 94),
            ("안녕하세요. 스크린샷을 번역합니다.", "malgun.ttf", 32, 24, 164),
        ], size=(1100, 250)), ["Hello world!", "日本語のスクリーンショットを翻訳します。", "안녕하세요.", "스크린샷을 번역합니다."]),
        ("scaled_geometry", render([("Original selection coordinates", "segoeui.ttf", 32, 1200, 800)], size=(2000, 1000)), ["Original selection coordinates"]),
        ("blank", Image.new("RGB", (64, 64), "white"), []),
    ]
    records = []
    child = None
    try:
        start = time.perf_counter()
        child = Child(runtime, artifacts)
        assert child.response() == {"ready": True}
        ready_ms = (time.perf_counter() - start) * 1000
        print(f"Ready (models loaded and warmed): {ready_ms:.1f} ms", flush=True)
        for name, image, expected in samples:
            image.save(artifacts / (name + ".png"))
            elapsed = []
            for iteration in range(3):
                start = time.perf_counter()
                result = child.send(image, fragmented=iteration == 0)
                elapsed.append(round((time.perf_counter() - start) * 1000, 2))
                check_result(result, expected, *image.size)
                if name == "scaled_geometry":
                    assert min(p[0] for line in result["lines"] for p in line["box"]) > 1100
                    assert min(p[1] for line in result["lines"] for p in line["box"]) > 700
                if name == "blank":
                    assert result == {"text": "", "lines": []}
            records.append({"sample": name, "milliseconds": elapsed, "result": result})
            print(json.dumps(records[-1], ensure_ascii=False), flush=True)
        (artifacts / "worker-stderr.txt").write_text(child.finish(), encoding="utf-8")
        child = None
        # Reject malformed frames without allocating the advertised pixel data.
        for label, payload in [
            ("zero_dimension", struct.pack("<II", 0, 10)),
            ("oversize_dimension", struct.pack("<II", 8193, 1)),
            ("too_many_pixels", struct.pack("<II", 5000, 5000)),
            ("partial_header", b"\x01\x00"),
            ("partial_pixels", struct.pack("<II", 2, 2) + b"abc"),
        ]:
            child = Child(runtime, artifacts)
            assert child.response() == {"ready": True}
            child.process.stdin.write(payload)
            child.process.stdin.close()
            response = child.response()
            assert isinstance(response.get("error"), str) and response["error"]
            child.finish(code=1)
            child = None
            print(label + ": passed", flush=True)
        # Source worker deliberately has no models; initialization must be framed,
        # Chinese, and must not try downloading models to repair the installation.
        child = Child(runtime, artifacts, worker=Path(__file__).resolve().parents[1] / "ocr/worker.py")
        error = child.response()
        assert "初始化失败" in error.get("error", ""), error
        child.finish(code=1)
        child = None
        report = {"ready_ms": ready_ms, "samples": records, "protocol_errors": "passed"}
        (artifacts / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print("PASS: 27 image requests, five malformed frames, EOF, offline initialization failure, geometry and binary stdout.")
        print("Artifacts: " + str(artifacts))
    finally:
        if child is not None:
            child.kill()


if __name__ == "__main__":
    main()
