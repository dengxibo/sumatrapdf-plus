"""Compare opaque yellow PDF highlights with native light rendering.

Uses the application's -render path, including warm cache replay. Requires
pypdf and Pillow. Example:
  python tools/test_dark_highlight_rendering.py --pdf book.pdf --page 114
The source PDF is read only; a single-page fixture and renders go under tmp/.
"""

import argparse
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageChops
from pypdf import PdfReader, PdfWriter


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pdf", required=True, type=Path)
    parser.add_argument("--page", required=True, type=int)
    parser.add_argument("--exe", type=Path, default=Path("out/dbg64/SumatraPDF-Plus.exe"))
    parser.add_argument("--output", type=Path, default=Path("tmp/dark-highlight-regression"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    page = PdfReader(args.pdf).pages[args.page - 1]
    if page.rotation:
        raise ValueError("Use an unrotated page for pixel-coordinate comparisons")
    quads = []
    for ref in page.get("/Annots", []):
        annot = ref.get_object()
        if (annot.get("/Subtype") != "/Highlight"
                or list(annot.get("/C", [])) != [1, 1, 0]
                or float(annot.get("/CA", 1)) != 1):
            continue
        points = list(map(float, annot.get("/QuadPoints", [])))
        for i in range(0, len(points), 8):
            q = points[i:i + 8]
            # Compare axis-aligned interiors; appearance edges blend with the
            # different page backgrounds and are inspected visually instead.
            if len(q) == 8 and q[1] == q[3] and q[5] == q[7] and q[0] == q[4] and q[2] == q[6]:
                quads.append((min(q[::2]), min(q[1::2]), max(q[::2]), max(q[1::2])))
    if not quads:
        raise ValueError("No opaque, axis-aligned yellow highlights to compare")
    writer = PdfWriter()
    writer.add_page(page)
    fixture = output / "page.pdf"
    writer.write(fixture)

    def render(name, zoom, mode=None):
        env = os.environ.copy()
        for key in ("SUMATRA_DUMP_RENDER_BMP", "SUMATRA_DUMP_RENDER_TGA",
                    "SUMATRA_DUMP_DARK_BMP", "SUMATRA_RENDER_EXIT_AFTER_DUMP"):
            env.pop(key, None)
        path = output / (name + ".png")
        if mode:
            env["SUMATRA_DUMP_DARK_BMP"] = str(path)
            env["SUMATRA_DUMP_DARK_THEME"] = "Dark-Black"
            env["SUMATRA_DUMP_IMAGE_DARK"] = mode
        else:
            env["SUMATRA_DUMP_RENDER_BMP"] = str(path)
            env["SUMATRA_RENDER_EXIT_AFTER_DUMP"] = "1"
        process = subprocess.run(
            [str(args.exe.resolve()), "-render", "1", "-zoom", str(zoom), str(fixture)],
            env=env, capture_output=True, timeout=120,
        )
        (output / (name + ".log")).write_bytes(process.stdout + process.stderr)
        process.check_returncode()
        with Image.open(path) as image:
            return image.convert("RGB")

    left = float(page.cropbox.left)
    top = float(page.cropbox.top)
    for zoom in (100, 175, 400):
        light = render(f"light-{zoom}", zoom)
        scale = zoom / 100
        for mode in ("auto", "tone", "original"):
            dark = render(f"{mode}-{zoom}", zoom, mode)
            assert dark.size == light.size
            for x0, y0, x1, y1 in quads:
                box = (round((x0 - left) * scale) + 2, round((top - y1) * scale) + 2,
                       round((x1 - left) * scale) - 2, round((top - y0) * scale) - 2)
                if box[0] >= box[2] or box[1] >= box[3]:
                    raise ValueError(f"Highlight is too small to compare at {zoom}%")
                difference = ImageChops.difference(light.crop(box), dark.crop(box))
                error = max(high for low, high in difference.getextrema())
                assert error <= 1, (zoom, mode, box, error)
            print(f"PASS {zoom}% {mode}: {len(quads)} highlights retain native glyph AA and yellow")


if __name__ == "__main__":
    main()
