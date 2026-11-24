"""Fast regression tests. Run with build/ocr/python.exe -I -B this_file.py."""
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

import numpy as np

SOURCE = Path(__file__).resolve().parents[1] / "ocr/worker.py"
spec = importlib.util.spec_from_file_location("ocr_worker_under_test", SOURCE)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerTests(unittest.TestCase):
    def test_fragmented_read(self):
        class Fragmented(io.BytesIO):
            def readinto(self, view):
                return super().readinto(view[:2])
        self.assertEqual(worker.read_exact(Fragmented(b"12345678"), 8), b"12345678")
        self.assertIsNone(worker.read_exact(io.BytesIO(), 8))
        with self.assertRaises(EOFError):
            worker.read_exact(Fragmented(b"12"), 8)

    def test_frame_unicode(self):
        output = io.BytesIO()
        worker.send_frame(output, {"text": "日本語\n안녕하세요"})
        data = output.getvalue()
        self.assertEqual(struct.unpack("<I", data[:4])[0], len(data) - 4)
        self.assertEqual(json.loads(data[4:]), {"text": "日本語\n안녕하세요"})

    def test_short_writes(self):
        class ShortWriter(io.BytesIO):
            def write(self, data):
                return super().write(data[:3])
        output = ShortWriter()
        worker.send_frame(output, {"ready": True})
        data = output.getvalue()
        self.assertEqual(struct.unpack("<I", data[:4])[0], len(data) - 4)
        self.assertEqual(json.loads(data[4:]), {"ready": True})

    def test_hangul_can_beat_higher_primary_confidence(self):
        self.assertTrue(worker.choose_korean("Settings", 0.98, "Settings 설정", 0.96))
        self.assertFalse(worker.choose_korean("設定", 0.99, "설정", 0.60))
        self.assertFalse(worker.choose_korean("Settings", 0.98, "Settings", 0.96))
        self.assertFalse(worker.choose_korean("", 0, "", 0))

    def test_one_detection_and_only_uncertain_crops_batched(self):
        backend = worker.Backend.__new__(worker.Backend)
        backend.np = np
        backend.rec_input = lambda imgs: SimpleNamespace(img=imgs)
        boxes = np.array([[[0, 0], [10, 0], [10, 5], [0, 5]]] * 3, dtype=np.float32)
        backend.detector = Mock(return_value=SimpleNamespace(boxes=boxes))
        backend.crop = Mock(return_value=np.zeros((5, 10, 3), dtype=np.uint8))
        backend.multi = Mock(return_value=SimpleNamespace(
            txts=("English", "Settings", ""), scores=(0.999, 0.971, 0.0)))
        backend.korean = Mock(return_value=SimpleNamespace(
            txts=("Settings 설정", "안녕하세요"), scores=(0.96, 0.99)))
        result = backend.recognize(bytes(16 * 16 * 4), 16, 16)
        self.assertEqual(result["text"], "English\nSettings 설정\n안녕하세요")
        backend.detector.assert_called_once()
        backend.multi.assert_called_once()
        backend.korean.assert_called_once()
        self.assertEqual(len(backend.multi.call_args.args[0].img), 3)
        self.assertEqual(len(backend.korean.call_args.args[0].img), 2)

    def test_confident_english_does_not_run_korean(self):
        backend = worker.Backend.__new__(worker.Backend)
        backend.np = np
        backend.rec_input = lambda imgs: SimpleNamespace(img=imgs)
        backend.detector = Mock(return_value=SimpleNamespace(
            boxes=np.array([[[0, 0], [10, 0], [10, 5], [0, 5]]], dtype=np.float32)))
        backend.crop = Mock(return_value=np.zeros((5, 10, 3), dtype=np.uint8))
        backend.multi = Mock(return_value=SimpleNamespace(txts=("English",), scores=(0.999,)))
        backend.korean = Mock()
        self.assertEqual(backend.recognize(bytes(16 * 16 * 4), 16, 16)["text"], "English")
        backend.korean.assert_not_called()

    def test_network_is_rejected(self):
        with self.assertRaises(RuntimeError):
            worker.deny_network("https://example.com")

    def test_native_stdout_redirect_and_request_error_recovery(self):
        # Deliberately inject Python print, CRT write AND Win32 WriteFile logs
        # during model initialization. None may corrupt the binary protocol.
        program = r'''
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location("worker", sys.argv[1])
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)
class FakeBackend:
    def __init__(self):
        print("python noise", flush=True)
        os.write(1, b"CRT noise\n")
        if os.name == "nt":
            import ctypes
            kernel = ctypes.windll.kernel32
            kernel.GetStdHandle.restype = ctypes.c_void_p
            kernel.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p]
            count = ctypes.c_uint32()
            data = ctypes.create_string_buffer(b"Win32 noise\n")
            assert kernel.WriteFile(kernel.GetStdHandle(-11), data, 12, ctypes.byref(count), None)
        self.calls = 0
    def recognize(self, pixels, width, height):
        self.calls += 1
        if self.calls == 1:
            raise ValueError("测试错误")
        return {"text":"ok", "lines":[]}
w.Backend = FakeBackend
raise SystemExit(w.main())
'''
        frame = struct.pack("<II", 1, 1) + bytes(4)
        result = subprocess.run([sys.executable, "-I", "-B", "-X", "utf8", "-c", program, str(SOURCE)],
                                input=frame + frame, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        stream = io.BytesIO(result.stdout)
        responses = []
        while header := stream.read(4):
            self.assertEqual(len(header), 4)
            size = struct.unpack("<I", header)[0]
            self.assertLess(size, 4096)
            responses.append(json.loads(stream.read(size)))
        self.assertEqual(len(responses), 3)
        self.assertEqual(responses[0], {"ready": True})
        self.assertIn("测试错误", responses[1]["error"])
        self.assertEqual(responses[2], {"text": "ok", "lines": []})
        self.assertIn(b"python noise", result.stderr)
        self.assertIn(b"CRT noise", result.stderr)
        if os.name == "nt":
            self.assertIn(b"Win32 noise", result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
