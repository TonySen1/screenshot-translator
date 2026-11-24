"""Table hints: build/ocr/python.exe -I -B tests/test_ocr_table.py.

Set OCR_TABLE_RUNTIME=the provisioned build/ocr path for the real-model test.
It loads the SOURCE worker against provisioned models, without changing build/ocr.
Rendered screenshots/results are saved only under PI_SCRATCH_DIR (or temp).
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

import numpy as np

SOURCE = Path(__file__).resolve().parents[1] / "ocr/worker.py"
spec = importlib.util.spec_from_file_location("ocr_table_under_test", SOURCE)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


def line(text, x, y, width, height=16):
    return {"text": text, "box": [[x, y], [x + width, y],
                                  [x + width, y + height], [x, y + height]]}


def grid(width=590, height=448, columns=(150,), rows=tuple(range(37, 445, 37))):
    image = np.full((height, width, 3), 35, dtype=np.uint8)
    for x in columns:
        image[:, x] = 110
    for y in rows:
        image[y, :] = 49  # Subtle, single-pixel horizontal rules.
    return image


class TableTests(unittest.TestCase):
    def assert_preserved(self, source, table):
        from collections import Counter
        self.assertEqual(Counter(item["text"] for item in source),
                         Counter(text for row in table["rows"] for cell in row
                                 for text in cell.split("\n") if text))
        self.assertTrue(all(type(value) is int and value > 0 for value in table["widths"]))

    def test_dark_tight_grid_centered_headers_long_cell(self):
        source = [line("Key", 61, 10, 28), line("Description", 338, 10, 84)]
        for row in range(1, 12):
            source += [line("Key " + str(row), 13, row * 37 + 10, 90),
                       line("A long cell with its final words" if row == 10 else "Description " + str(row),
                            162, row * 37 + 10, 420 if row == 10 else 160)]
        original = copy.deepcopy(source)
        # Input detector order is not assumed to be row-major.
        table = worker.group_table(source[::-1], 590, 448, grid())
        self.assertIsNotNone(table)
        self.assertEqual(table["rows"][0], ["Key", "Description"])
        self.assertEqual(table["rows"][10], ["Key 10", "A long cell with its final words"])
        self.assertEqual(len(table["rows"]), 12)
        self.assertEqual(table["widths"], [150, 440])
        self.assertEqual(table["heights"], [37] * 11 + [41])
        self.assertEqual(source, original)
        self.assert_preserved(source, table)

    def test_wrapped_cell_and_duplicate_text_never_dropped(self):
        source = [line("Header", 20, 10, 60), line("Details", 170, 10, 70),
                  line("Same", 13, 47, 40), line("First part", 162, 40, 95),
                  line("last part", 162, 58, 80), line("Same", 13, 84, 40),
                  line("Complete", 162, 84, 90)]
        table = worker.group_table(source, 590, 111, grid(height=111, rows=(37, 74)))
        self.assertIsNotNone(table)
        self.assertEqual(table["rows"][1], ["Same", "First part\nlast part"])
        self.assert_preserved(source, table)

    def test_borderless_two_to_four_columns_and_centered_header(self):
        for count in (2, 3, 4):
            with self.subTest(columns=count):
                source = []
                for row in range(4):
                    for col in range(count):
                        source.append(line(f"{row}:{col}", col * 160 + (50 if row == 0 else 10),
                                           row * 35 + 10, 45 if row == 0 else 80))
                table = worker.group_table(source, count * 160, 140)
                self.assertIsNotNone(table)
                self.assertEqual(len(table["widths"]), count)
                self.assertEqual(table["rows"][0], [f"0:{col}" for col in range(count)])
                self.assert_preserved(source, table)

    def test_plain_paragraph_and_word_fragments_are_not_tables(self):
        paragraph = [line("A normal paragraph line.", 13, row * 30 + 10, 460 - row * 9)
                     for row in range(8)]
        self.assertIsNone(worker.group_table(paragraph, 590, 260, grid(height=260, rows=())))
        fragments = []
        for row in range(6):
            fragments += [line("Some words", 13, row * 30 + 10, 100),
                          line("continue the same paragraph", 124, row * 30 + 10, 310)]
        self.assertIsNone(worker.group_table(fragments, 590, 200))

    def test_ambiguous_partial_rows_and_spanning_title_rejected(self):
        source = [line(str(row), 13, row * 35 + 10, 80) for row in range(4)]
        source += [line("Right", 180, row * 35 + 10, 200) for row in range(3)]
        self.assertIsNone(worker.group_table(source, 590, 160))
        source += [line("Right", 180, 115, 200), line("Spanning title", 20, 155, 500)]
        self.assertIsNone(worker.group_table(source, 590, 200))

    def test_at_least_three_rows_required(self):
        source = [line(str(col), 13 + col * 160, 10 + row * 35, 50)
                  for row in range(2) for col in range(2)]
        self.assertIsNone(worker.group_table(source, 320, 80))
    def test_non_table_lines_get_independent_regions(self):
        lines = [line("Build your own Agent workspace.", 80, 18, 420),
                 line("Your models · Your agents · Your plugins · Your workspace", 24, 60, 540),
                 line("Download · Documentation · Build a Plugin", 82, 120, 390)]
        table = worker.line_table(lines, 590, 150)
        self.assertEqual(len(table["rows"]), 3)
        self.assertEqual([r["bounds"] for r in table["regions"]], [[80,18,500,34],[24,60,564,76],[82,120,472,136]])
        self.assertTrue(all(r["centered"] for r in table["regions"]))

    def test_multicolumn_layout_uses_column_local_limits(self):
        lines = [line("Agent", 5, 10, 50), line("Give it a task.", 5, 45, 145),
                 line("Plan", 283, 10, 45), line("Review the approach.", 283, 45, 145),
                 line("Goal", 563, 10, 27), line("Define the outcome.", 563, 45, 27)]
        table = worker.line_table(lines, 590, 100)
        limits = [region["limit"] for region in table["regions"]]
        self.assertEqual(limits[0][0], 0); self.assertEqual(limits[0][2], 283)
        self.assertEqual(limits[2][0], 283); self.assertEqual(limits[2][2], 563)
        self.assertEqual(limits[4][0], 563); self.assertEqual(limits[4][2], 590)
        self.assertFalse(any(region["centered"] for region in table["regions"]))
    @unittest.skipUnless(os.environ.get("OCR_TABLE_RUNTIME"), "set OCR_TABLE_RUNTIME for real OCR regression")
    def test_real_ocr_rendered_screenshot_and_paragraph(self):
        from PIL import Image, ImageDraw, ImageFont
        runtime = Path(os.environ["OCR_TABLE_RUNTIME"]).resolve()
        self.assertTrue((runtime / "python.exe").is_file())
        self.assertTrue((runtime / "runtime-lock.json").is_file())
        font = ImageFont.truetype(str(Path(os.environ.get("SystemRoot", "C:/Windows")) /
                                       "Fonts/segoeui.ttf"), 16)
        image = Image.new("RGB", (590, 448), (35, 35, 35))
        draw = ImageDraw.Draw(image)
        draw.line((150, 0, 150, 447), fill=(110, 110, 110))
        for y in range(37, 445, 37):
            draw.line((0, y, 589, y), fill=(49, 49, 49))
        rows = [["Key", "Description"], ["Escape", "Close the current window."],
                ["Enter", "Confirm the selected action."], ["Space", "Pause or resume playback."],
                ["Tab", "Move focus to the next control."], ["Shift", "Extend the current selection."],
                ["Control", "Use an alternate command."], ["Home", "Jump to the start of the document."],
                ["End", "Jump to the end of the document."], ["Delete", "Remove the selected item."],
                ["Page Down", "Keep this long sentence intact, including the final words."],
                ["Page Up", "Scroll up to the previous page."]]
        for index, row in enumerate(rows):
            for col, text in enumerate(row):
                x = (75 if col == 0 else 380) - draw.textlength(text, font=font) / 2 if index == 0 else (13 if col == 0 else 162)
                draw.text((x, index * 37 + 7), text, font=font, fill=(230, 230, 230))
        artifacts = Path(os.environ.get("PI_SCRATCH_DIR", tempfile.gettempdir())) / "ocr-table-regression"
        artifacts.mkdir(parents=True, exist_ok=True)
        image.save(artifacts / "dark-table-590x448.png")
        previous_root = worker.ROOT
        worker.ROOT = runtime
        try:
            backend = worker.Backend()
            result = backend.recognize(image.convert("RGBA").tobytes("raw", "BGRA"), *image.size)
            (artifacts / "dark-table-result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
            self.assertEqual(result["text"], "\n".join(item["text"] for item in result["lines"]))
            self.assertIn("table", result, result)
            self.assertEqual(result["table"]["widths"], [150, 440])
            self.assertEqual(len(result["table"]["rows"]), 12)
            self.assertEqual(sum(result["table"]["heights"]), 448)
            self.assertEqual(result["table"]["rows"][0], rows[0])
            self.assertIn("final words", result["table"]["rows"][10][1])
            self.assert_preserved(result["lines"], result["table"])
            paragraph = Image.new("RGB", (590, 260), (35, 35, 35))
            draw = ImageDraw.Draw(paragraph)
            for row, text in enumerate([
                    "This is an ordinary paragraph, not a table.",
                    "The lines all start at the same left margin.",
                    "Some sentences are longer than other sentences.",
                    "No column structure should be invented here.",
                    "Preserve the original text and all its coordinates.",
                    "The final line ends the paragraph."]):
                draw.text((13, 12 + row * 36), text, font=font, fill=(230, 230, 230))
            paragraph.save(artifacts / "dark-paragraph.png")
            result = backend.recognize(paragraph.convert("RGBA").tobytes("raw", "BGRA"), *paragraph.size)
            (artifacts / "paragraph-result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
            self.assertIn("table", result)
            self.assertEqual(len(result["table"]["regions"]), len(result["lines"]))
            self.assertEqual(result["text"], "\n".join(item["text"] for item in result["lines"]))
        finally:
            worker.ROOT = previous_root


if __name__ == "__main__":
    unittest.main(verbosity=2)
