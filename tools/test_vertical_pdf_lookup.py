"""Check native CJK glyph picking against independent PDFium outline positions.

Requires pypdfium2. Uses the native SUMATRA_TEST_GLYPH_COORDS render harness.
Private PDFs, point lists and reports stay in the requested output directory.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess

import pypdfium2 as pdfium


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--pdf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    doc = pdfium.PdfDocument(str(args.pdf.resolve()))
    page = doc[0]
    text = page.get_textpage()
    points = []
    characters = 0
    crop_left, crop_bottom, crop_right, crop_top = page.get_cropbox()
    rotation = page.get_rotation()
    for index in range(text.count_chars()):
        char = text.get_text_range(index, 1)
        if len(char) != 1 or not 0x3400 <= ord(char) <= 0x9FFF:
            continue
        left, bottom, right, top = text.get_charbox(index)
        if right - left < 1 or top - bottom < 1:
            continue
        characters += 1
        for dx, dy in ((0, 0), (-.2, 0), (.2, 0), (0, -.2), (0, .2)):
            x = (left + right) / 2 + dx * (right - left)
            y = (bottom + top) / 2 + dy * (top - bottom)
            # PDFium char boxes use unrotated PDF coordinates, while the
            # application's page text uses the rotated, top-left page space.
            if rotation == 90:
                x, y = y - crop_bottom, x - crop_left
            elif rotation == 180:
                x, y = crop_right - x, y - crop_bottom
            elif rotation == 270:
                x, y = crop_top - y, crop_right - x
            else:
                x, y = x - crop_left, crop_top - y
            points.append(f"{ord(char):04x} {x:.4f} {y:.4f}")
    text.close()
    page.close()
    doc.close()
    assert characters, "No native CJK glyphs found on page 1"
    points_path = output / "points.txt"
    points_path.write_text("\n".join(points), encoding="utf-8")
    report_path = output / "glyphs.txt"
    env = os.environ.copy()
    env["SUMATRA_TEST_GLYPH_COORDS"] = str(report_path)
    env["SUMATRA_TEST_GLYPH_POINTS"] = str(points_path)
    process = subprocess.run(
        [str(args.exe.resolve()), "-appdata", str(output / "appdata"),
         "-render", "1", str(args.pdf.resolve())],
        env=env, capture_output=True, timeout=120)
    (output / "run.log").write_bytes(process.stdout + process.stderr)
    rows = [line.split() for line in report_path.read_text().splitlines()]
    hits = [row for row in rows if row[0] == "P"]
    failures = [row for row in hits if row[1] != row[2]]
    wide = [(int(row[1], 16), tuple(map(int, row[2:])))
            for row in rows if row[0] not in ("P", "U")]
    utf8 = [row for row in rows if row[0] == "U"]
    data = bytes(int(row[2], 16) for row in utf8)
    decoded, offset = [], 0
    for char in data.decode("utf-8"):
        count = len(char.encode("utf-8"))
        rect = tuple(map(int, utf8[offset][3:]))
        assert all(tuple(map(int, row[3:])) == rect for row in utf8[offset:offset + count])
        decoded.append((ord(char), rect))
        offset += count
    # UTF-16 deliberately narrows whitespace hit boxes; compare actual glyphs.
    wide_glyphs = [item for item in wide if not chr(item[0]).isspace()]
    utf8_glyphs = [item for item in decoded if not chr(item[0]).isspace()]
    summary = dict(characters=characters, points=len(hits), failures=len(failures),
                   utf16_utf8_glyphs_equal=wide_glyphs == utf8_glyphs, exit_code=process.returncode)
    (output / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary), flush=True)
    assert len(hits) == len(points), "Native harness did not check every point"
    assert not failures, f"Incorrect glyph picks: {failures[:10]}"
    assert wide_glyphs == utf8_glyphs, "UTF-16 and UTF-8 glyph coordinates differ"
    assert process.returncode == 0, "Native harness failed"


if __name__ == "__main__":
    main()
