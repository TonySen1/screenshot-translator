"""Offline PaddleOCR worker. Binary stdin/stdout only; see tests/smoke_ocr.py.

Pinned RapidOCR 3.10.0 API: one PP-OCRv6 Tiny detector, batch PP-OCRv6
Small (English/Japanese), then batch PP-OCRv5 Korean for uncertain crops.
No orientation classifier or third recognition pass is used.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import sys
import traceback
TEXT_TABLE_MAX_COLS = 4
MAX_DIMENSION = 8192
MAX_PIXELS = 20_000_000
MAX_LINES = 512
# Rendered "Settings 설정" scored 0.971 in v6 despite dropping Hangul;
# 0.985 recovers it, while clean English/Japanese usually exceed 0.99.
RETRY_SCORE = 0.985
MIN_SCORE = 0.45
ROOT = Path(__file__).resolve().parent


def read_exact(stream, size):
    data = bytearray(size)
    view = memoryview(data)
    done = 0
    while done < size:
        count = stream.readinto(view[done:])
        if not count:
            if done == 0:
                return None
            raise EOFError("输入图像数据不完整")
        done += count
    return data


def send_frame(stream, value):
    payload = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    remaining = memoryview(struct.pack("<I", len(payload)) + payload)
    while remaining:
        written = stream.write(remaining)
        if not written:
            raise BrokenPipeError("OCR 响应管道已关闭")
        remaining = remaining[written:]
    stream.flush()


def deny_network(*args, **kwargs):
    raise RuntimeError("OCR 离线运行，禁止网络访问；请重新运行 setup-ocr.ps1")


def has_hangul(text):
    return any("\uac00" <= c <= "\ud7a3" or "\u3130" <= c <= "\u318f" for c in text)
def sample_foreground(region, image):
    if image is None: return [255, 255, 255]
    import numpy as np
    l, t, r, b = region["bounds"]
    l, t, r, b = max(0, l), max(0, t), min(image.shape[1], r), min(image.shape[0], b)
    if r <= l or b <= t: return [255, 255, 255]
    rgb = image[t:b, l:r, :3][:, :, ::-1].astype(np.float32)
    bg = np.array(region.get("background", [32, 32, 32]), dtype=np.float32)
    luminance = rgb[:, :, 0] * .299 + rgb[:, :, 1] * .587 + rgb[:, :, 2] * .114
    bg_luminance = bg[0] * .299 + bg[1] * .587 + bg[2] * .114
    distance = np.abs(luminance - bg_luminance)
    threshold = max(18.0, float(np.percentile(distance, 75)))
    pixels = rgb[distance >= threshold]
    if pixels.size == 0: return [255, 255, 255] if bg_luminance < 128 else [16, 16, 16]
    return [int(max(0, min(255, round(float(v))))) for v in pixels.mean(axis=0)]
def sample_background(region, image):
    if image is None: return [32, 32, 32]
    import numpy as np
    l, t, r, b = region["bounds"]
    h, w = image.shape[:2]
    l, t, r, b = max(0, l), max(0, t), min(w, r), min(h, b)
    pad = max(2, min(6, (b - t) // 3))
    strips = []
    if t > 0: strips.append(image[max(0, t - pad):t, l:r, :3])
    if b < h: strips.append(image[b:min(h, b + pad), l:r, :3])
    if l > 0: strips.append(image[t:b, max(0, l - pad):l, :3])
    if r < w: strips.append(image[t:b, r:min(w, r + pad), :3])
    if not strips: return [32, 32, 32]
    pixels = np.concatenate([strip.reshape(-1, 3)[:, ::-1] for strip in strips], axis=0)
    quant = (pixels // 8).astype(np.int32)
    keys, counts = np.unique(quant, axis=0, return_counts=True)
    chosen = keys[int(np.argmax(counts))]
    selected = pixels[np.all(quant == chosen, axis=1)]
    return [int(max(0, min(255, round(float(v))))) for v in selected.mean(axis=0)]


def choose_korean(primary, primary_score, korean, korean_score):
    # Scores from different dictionaries are not directly calibrated. Strong
    # Hangul evidence may win even when v6 confidently hallucinated CJK text.
    if not korean.strip():
        return False
    if has_hangul(korean):
        return korean_score >= 0.80 and korean_score >= primary_score - 0.12
    return korean_score >= 0.80 and korean_score > primary_score + 0.08


def group_table(lines, width, height, image=None):
    """Conservative, bounded table hint; never changes the original OCR lines.

    Prefer continuous grid rules. Otherwise require a shared whitespace gutter
    and repeated row/column alignment. Cell strings contain every source line
    verbatim, separated by newlines when OCR split a cell into several boxes.
    """
    from statistics import median

    if not 6 <= len(lines) <= MAX_LINES:
        return None
    items = []
    for line in lines:
        xs, ys = zip(*line["box"])
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        if right <= left or bottom <= top:
            return None
        items.append((left, top, right, bottom, line["text"]))
    text_height = median(item[3] - item[1] for item in items)

    def assemble(xcuts, ycuts):
        xedges, yedges = [0, *xcuts, width], [0, *ycuts, height]
        cells = [[[] for _ in range(len(xedges) - 1)] for _ in range(len(yedges) - 1)]
        first, last = 0, len(cells)
        for item in items:
            left, top, right, bottom, _ = item
            col = sum((left + right) / 2 > cut for cut in xcuts)
            row = sum((top + bottom) / 2 > cut for cut in ycuts)
            if (left < xedges[col] - 2 or right > xedges[col + 1] + 2 or
                    top < yedges[row] - 2 or bottom > yedges[row + 1] + 2): return None
            cells[row][col].append(item)
        while cells and not any(cells[0]): cells.pop(0); first += 1
        while cells and not any(cells[-1]): cells.pop(); last -= 1
        if len(cells) < 3 or sum(all(row) for row in cells) < 3 or not all(any(row) for row in cells): return None
        rows = [["\n".join(item[4] for item in sorted(cell, key=lambda a: (a[1], a[0]))) for cell in row] for row in cells]
        regions = []
        for row_index, row in enumerate(cells):
            for col, cell in enumerate(row):
                if not cell: return None
                bx = min(item[0] for item in cell); by = min(item[1] for item in cell)
                br = max(item[2] for item in cell); bb = max(item[3] for item in cell)
                regions.append({"bounds": [bx, by, br, bb],
                                "limit": [int(xedges[col]), int(yedges[first + row_index]),
                                          int(xedges[col + 1]), int(yedges[first + row_index + 1])],
                                "background": [32, 32, 32], "centered": row_index == 0})
                regions[-1]["foreground"] = sample_foreground(regions[-1], image)
        return {"rows": rows,
                "widths": [max(1, int(round(b - a))) for a, b in zip(xedges, xedges[1:])],
                "heights": [max(1, int(round(yedges[i + 1] - yedges[i]))) for i in range(first, last)],
                "regions": regions}
    def fill_background(table):
        if image is None or not table or "regions" not in table: return table
        for region in table["regions"]:
            region["background"] = sample_background(region, image)
            region["foreground"] = sample_foreground(region, image)
        return table

    if image is not None:
        import numpy as np

        def rules(axis):
            # Sample at most ~256 scanlines, retaining original rule positions.
            # Median edge contrast ignores glyphs and tolerates subtle 1px rules
            # on either light or dark backgrounds; no extra vision dependency.
            sample = (image[::max(1, height // 256), :, :3] if axis == 1
                      else image[:, ::max(1, width // 256), :3])
            gray = sample.mean(axis=2, dtype=np.float32)
            strength = np.median(np.abs(np.diff(gray, axis=axis)), axis=1 - axis)
            positions = np.flatnonzero(strength >= 6).tolist()
            clusters = []
            for pos in positions:
                if clusters and pos - clusters[-1][-1] <= 3:
                    clusters[-1].append(pos)
                else:
                    clusters.append([pos])
            extent = width if axis == 1 else height
            return [int(round((group[0] + group[-1] + 1) / 2)) for group in clusters
                    if text_height / 2 < (group[0] + group[-1] + 1) / 2 < extent - text_height / 2]

        xcuts, ycuts = rules(1), rules(0)
        if 1 <= len(xcuts) <= 3 and 2 <= len(ycuts) <= MAX_LINES:
            table = assemble(xcuts, ycuts)
            if table is not None:
                return fill_background(table)

    # Borderless fallback: intersect the horizontal whitespace of ALL boxes,
    # including centered headers. A fixed left-edge cluster would lose them.
    ordered = sorted(items, key=lambda a: (a[1] + a[3], a[0]))
    bands = []
    for item in ordered:
        center = (item[1] + item[3]) / 2
        if bands and abs(center - median((a[1] + a[3]) / 2 for a in bands[-1])) <= text_height * 0.5:
            bands[-1].append(item)
        else:
            bands.append([item])
    if len(bands) < 3:
        return None
    spans = sorted((item[0], item[2]) for item in items)
    merged = []
    for left, right in spans:
        if merged and left <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], right)
        else:
            merged.append([left, right])
    gutters = [(a[1], b[0]) for a, b in zip(merged, merged[1:])
               if b[0] - a[1] >= max(10, text_height * 0.8)]
    if not 1 <= len(gutters) <= 3:
        return None
    xcuts = [(left + right) / 2 for left, right in gutters]
    columns = [[] for _ in range(len(xcuts) + 1)]
    for band in bands:
        row = [[] for _ in columns]
        for item in band:
            row[sum((item[0] + item[2]) / 2 > cut for cut in xcuts)].append(item)
        if not all(row):
            return None  # Wrapped/partial rows need real grid evidence.
        for col, cell in zip(columns, row):
            col.append(min(item[0] for item in cell))
    for starts in columns:
        # Allow one centered header, but insist on stable body starts.
        anchor = median(starts)
        if sum(abs(start - anchor) <= max(3, text_height * 0.35) for start in starts) < max(3, len(starts) - 1):
            return None
    # Mere word gaps in a paragraph are not convincing column separators.
    # Require a conspicuously wide gutter without visible grid evidence.
    if any(right - left < text_height * 1.5 for left, right in gutters):
        return None
    ycuts = [(max(a[3] for a in before) + min(a[1] for a in after)) / 2
             for before, after in zip(bands, bands[1:])]
    table = assemble(xcuts, ycuts)
    return fill_background(table) if table is not None else None

def infer_columns(lines, width):
    if len(lines) < 4: return [(0, width)]
    lefts = sorted(min(p[0] for p in line["box"]) for line in lines)
    clusters = []
    tolerance = 12
    for left in lefts:
        if clusters and left - clusters[-1][-1] <= tolerance:
            clusters[-1].append(left)
        else:
            clusters.append([left])
    clusters = [cluster for cluster in clusters if len(cluster) >= 2]
    if len(clusters) < 2 or len(clusters) > TEXT_TABLE_MAX_COLS:
        return [(0, width)]
    anchors = [int(round(sum(cluster) / len(cluster))) for cluster in clusters]
    if any(b - a < max(80, width * 0.10) for a, b in zip(anchors, anchors[1:])):
        return [(0, width)]
    edges = [0] + anchors[1:] + [width]
    return list(zip(edges, edges[1:]))

def line_table(lines, width, height, image=None):
    if not lines: return None
    columns = infer_columns(lines, width)
    regions, heights = [], []
    for line in lines:
        xs, ys = zip(*line["box"])
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        heights.append(max(1, bottom - top))
        center = (left + right) / 2
        col = min(range(len(columns)), key=lambda i: abs(center - (columns[i][0] + columns[i][1]) / 2))
        col_left, col_right = columns[col]
        col_width = col_right - col_left
        centered = abs(center - (col_left + col_right) / 2) <= max(8, (col_width if len(columns) > 1 else width) * 0.10)
        if len(columns) > 1:
            centered = centered and left - col_left > col_width * 0.12 and right - left < col_width * 0.78
        region = {"bounds": [left, top, right, bottom],
                  "limit": [col_left, top, col_right, bottom],
                  "background": [32, 32, 32], "foreground": [255, 255, 255],
                  "centered": centered}
        region["background"] = sample_background(region, image)
        region["foreground"] = sample_foreground(region, image)
        regions.append(region)
    return {"rows": [[line["text"]] for line in lines], "widths": [width],
            "heights": heights, "regions": regions}


class Backend:
    def __init__(self):
        from importlib.metadata import version
        if version("rapidocr") != "3.10.0" or version("onnxruntime") != "1.23.2":
            raise RuntimeError("OCR 依赖版本不匹配，请重新运行 setup-ocr.ps1")
        manifest = json.loads((ROOT / "runtime-lock.json").read_text(encoding="utf-8"))
        for item in manifest["models"]:
            path = ROOT / "models" / item["name"]
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
                raise RuntimeError("OCR 模型缺失或校验失败，请重新运行 setup-ocr.ps1")

        # Block both the library downloader and network connections, including
        # accidental future fallback paths. All ONNX dictionaries are embedded.
        socket.create_connection = deny_network
        socket.socket.connect = deny_network
        socket.socket.connect_ex = deny_network
        import cv2
        import numpy as np
        from rapidocr import RapidOCR, ModelType, OCRVersion
        from rapidocr.ch_ppocr_rec import TextRecInput
        from rapidocr.utils.download_file import DownloadFile
        from rapidocr.utils.process_img import get_rotate_crop_image
        DownloadFile.run = deny_network
        cv2.setNumThreads(1)
        self.np = np
        self.rec_input = TextRecInput
        self.crop = get_rotate_crop_image
        common = {
            "Global.log_level": "error",
            "Global.use_cls": False,
            "Global.model_root_dir": str(ROOT / "models"),
            "EngineConfig.onnxruntime.intra_op_num_threads": min(4, os.cpu_count() or 1),
            "EngineConfig.onnxruntime.inter_op_num_threads": 1,
            "Det.model_path": str(ROOT / "models/PP-OCRv6_det_tiny.onnx"),
            "Det.model_type": ModelType.TINY,
            "Det.limit_type": "max",
            "Det.limit_side_len": 1280,
            "Rec.model_path": str(ROOT / "models/PP-OCRv6_rec_small.onnx"),
            "Rec.rec_batch_num": 8,
        }
        self.detector = RapidOCR(params={**common, "Global.use_rec": False})
        self.multi = RapidOCR(params=common)._load_rec_model()
        self.korean = RapidOCR(params={
            **common,
            "Rec.model_path": str(ROOT / "models/korean_PP-OCRv5_rec_mobile.onnx"),
            "Rec.ocr_version": OCRVersion.PPOCRV5,
            "Rec.model_type": ModelType.MOBILE,
            "Rec.lang_type": "korean",
        })._load_rec_model()
        # Lazy detector loading and both recognizers must finish before ready.
        blank = np.full((96, 320, 3), 255, dtype=np.uint8)
        self.detector(blank)
        self.multi(TextRecInput([blank]))
        self.korean(TextRecInput([blank]))

    def recognize(self, pixels, width, height):
        np = self.np
        image = np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 4)[:, :, :3].copy()
        detected = self.detector(image)
        if detected.boxes is None or len(detected.boxes) == 0:
            return {"text": "", "lines": []}
        boxes = detected.boxes
        if len(boxes) > MAX_LINES:
            raise ValueError("截图文字行过多，请缩小选区")
        crops = [self.crop(image, box.astype(np.float32)) for box in boxes]
        primary = self.multi(self.rec_input(crops))
        texts, scores = list(primary.txts), list(primary.scores)
        uncertain = [i for i, (text, score) in enumerate(zip(texts, scores))
                     if score < RETRY_SCORE or not text.strip() or text != text.strip()]
        if uncertain:
            retry = self.korean(self.rec_input([crops[i] for i in uncertain]))
            for i, text, score in zip(uncertain, retry.txts, retry.scores):
                if choose_korean(texts[i], scores[i], text, score):
                    texts[i], scores[i] = text, score
        lines = []
        for box, text, score in zip(boxes, texts, scores):
            text = text.strip()
            if not text or score < MIN_SCORE:
                continue
            coords = [[max(0, min(width - 1, int(round(float(x))))),
                       max(0, min(height - 1, int(round(float(y)))))] for x, y in box]
            lines.append({"text": text, "box": coords})
        result = {"text": "\n".join(line["text"] for line in lines), "lines": lines}
        table = group_table(lines, width, height, image)
        if table is None:
            table = line_table(lines, width, height, image)
        if table is not None:
            result["table"] = table
        return result



def main():
    # A Python-level redirect is not enough: native libraries may printf(1).
    # Duplicate the original pipe first, then redirect the underlying CRT fd.
    if os.name == "nt":
        import msvcrt
        msvcrt.setmode(0, os.O_BINARY)
        msvcrt.setmode(1, os.O_BINARY)
    protocol = os.fdopen(os.dup(1), "wb", buffering=0)
    os.dup2(2, 1)
    if os.name == "nt":
        import ctypes
        # Also protect Win32 WriteFile(GetStdHandle(STD_OUTPUT_HANDLE)).
        ctypes.windll.kernel32.SetStdHandle(-11, ctypes.c_void_p(msvcrt.get_osfhandle(2)))
    sys.stdout = sys.stderr
    sys.dont_write_bytecode = True
    source = sys.stdin.buffer
    try:
        backend = Backend()
    except Exception as exc:
        traceback.print_exc(file=sys.stderr)
        send_frame(protocol, {"error": "OCR 初始化失败：" + str(exc)})
        return 1
    send_frame(protocol, {"ready": True})
    while True:
        try:
            header = read_exact(source, 8)
            if header is None:
                return 0
            width, height = struct.unpack("<II", header)
            if not (1 <= width <= MAX_DIMENSION and 1 <= height <= MAX_DIMENSION
                    and width * height <= MAX_PIXELS):
                send_frame(protocol, {"error": "截图尺寸无效：最大边长 8192，最多 2000 万像素"})
                return 1  # Cannot safely resynchronize an invalid frame.
            pixels = read_exact(source, width * height * 4)
            if pixels is None:
                raise EOFError("输入图像数据不完整")
        except EOFError as exc:
            send_frame(protocol, {"error": str(exc)})
            return 1
        try:
            result = backend.recognize(pixels, width, height)
        except Exception as exc:
            traceback.print_exc(file=sys.stderr)
            result = {"error": "OCR 识别失败：" + str(exc)}
        send_frame(protocol, result)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(0)
