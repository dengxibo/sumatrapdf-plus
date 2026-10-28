"""Run native PDF annotation regressions against a local, read-only sample manifest.

Manifest: [{"name": "scan", "source": "book.pdf", "page": 15,
            "force_ocr": false}]. Requires pypdf, pypdfium2, Pillow.
All fixtures, private source paths, images and logs stay in --output.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from PIL import Image, ImageDraw
from pypdf import PdfReader, PdfWriter
from pypdf.generic import ArrayObject, NameObject, NumberObject
import pypdfium2 as pdfium


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_properties(path):
    reader = PdfReader(path)
    shapes = [a.get_object() for a in reader.pages[0].get("/Annots", [])
              if a.get_object().get("/Subtype") in ("/Square", "/Circle", "/Line", "/Ink")]
    assert len(shapes) == 96, len(shapes)
    for kind in ("/Square", "/Circle", "/Line", "/Ink"):
        marks = [a for a in shapes if a["/Subtype"] == kind]
        widths = [float(a.get("/BS", {}).get("/W", 1)) for a in marks]
        # Zero uses the existing 0.15-point hairline representation.
        for expected in (.15, .25, .5, .75, 1, 12):
            assert any(abs(w - expected) < .001 for w in widths), (kind, expected, widths)
        if kind in ("/Square", "/Circle"):
            values = {round(float(a.get("/CA", 1)) * 255) for a in marks}
            assert values == {0, 64, 128, 255}, (kind, values)


def independent_render(path, output):
    # PDFium is independent of the application's MuPDF appearance renderer.
    doc = pdfium.PdfDocument(str(path))
    page = doc[0]
    bitmap = page.render(scale=1.75, draw_annots=True)
    bitmap.to_pil().save(output)
    bitmap.close()
    page.close()
    doc.close()


def validate_shape_pixels(folder):
    reader = PdfReader(folder / "properties.pdf")
    refs = reader.pages[0]["/Annots"]
    chosen = [n for n, ref in enumerate(refs)
              if ref.get_object().get("/Subtype") in ("/Square", "/Circle")]
    chosen.sort(key=lambda n: (refs[n].get_object()["/Subtype"],
                              float(refs[n].get_object().get("/CA", 1)),
                              float(refs[n].get_object().get("/BS", {}).get("/W", 1))))
    writer = PdfWriter()
    for n in chosen:
        page = writer.add_page(reader.pages[0])
        page[NameObject("/Annots")] = ArrayObject([page["/Annots"][n]])
        page[NameObject("/Rotate")] = NumberObject(0)
        if "/Contents" in page:
            del page["/Contents"]
    target = folder / "isolated-shapes.pdf"
    with target.open("wb") as stream:
        writer.write(stream)
    doc = pdfium.PdfDocument(str(target))
    contact = Image.new("RGB", (6 * 160, 8 * 120), "white")
    draw = ImageDraw.Draw(contact)
    for n, index in enumerate(chosen):
        annotation = refs[index].get_object()
        alpha = float(annotation.get("/CA", 1))
        page = doc[n]
        bitmap = page.render(scale=2, draw_annots=True)
        image = bitmap.to_pil().convert("RGB")
        left, bottom, right, top = map(float, annotation["/Rect"])
        cropbox = reader.pages[0].cropbox
        x = round(((left + right) / 2 - float(cropbox.left)) * 2)
        y = round((float(cropbox.top) - (bottom + top) / 2) * 2)
        actual = image.getpixel((x, y))
        expected = (255, 255, round(255 * (1 - alpha)))
        assert max(abs(a - b) for a, b in zip(actual, expected)) <= 3, (annotation["/Subtype"], alpha, actual, expected)
        box = (round((left - float(cropbox.left)) * 2) - 8,
               round((float(cropbox.top) - top) * 2) - 8,
               round((right - float(cropbox.left)) * 2) + 8,
               round((float(cropbox.top) - bottom) * 2) + 8)
        preview = image.crop(box)
        preview.thumbnail((150, 90))
        cx, cy = (n % 6) * 160, (n // 6) * 120
        width = float(annotation.get("/BS", {}).get("/W", 1))
        draw.text((cx + 4, cy + 4), f'{annotation["/Subtype"]} {alpha:.0%} {width:g}pt', fill="black")
        contact.paste(preview, (cx + 4, cy + 25))
        bitmap.close()
        page.close()
    doc.close()
    contact.save(folder / "opacity-pdfium.png")
    return len(chosen)


def comparison(folder):
    original = Image.open(folder / "unmarked.png").convert("RGB")
    marked = Image.open(folder / "mode0-rot0-zoom100.png").convert("RGB")
    panels = [("Before annotation", original), ("Saved and reopened", marked)]
    width = max(im.width for _, im in panels)
    canvas = Image.new("RGB", (width * 2, max(im.height for _, im in panels) + 30), "white")
    draw = ImageDraw.Draw(canvas)
    for n, (label, im) in enumerate(panels):
        draw.text((n * width + 5, 7), label, fill="black")
        canvas.paste(im, (n * width, 30))
    canvas.save(folder / "comparison.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("out/annotation-regression"))
    parser.add_argument("--exe", type=Path, default=Path("out/dbg64/SumatraPDF-Plus.exe"))
    parser.add_argument("--only", help="Run one manifest entry")
    parser.add_argument("--markup", choices=("highlight", "underline", "squiggly"), default="highlight")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    samples = json.loads(args.manifest.read_text(encoding="utf-8"))
    results = []
    for sample in samples:
        if args.only and sample["name"] != args.only:
            continue
        source = Path(sample["source"])
        before = digest(source)
        if "sha256" in sample:
            assert sample["sha256"] == before, "Source changed since sample discovery"
        folder = output / sample["name"]
        folder.mkdir(exist_ok=True)
        reader = PdfReader(source)
        page = reader.pages[sample["page"] - 1]
        writer = PdfWriter()
        fixture_page = writer.add_page(page)
        if sample.get("crop"):
            left, bottom, right, top = map(float, fixture_page.cropbox)
            fixture_page.cropbox.lower_left = (left + 12, bottom + 12)
            fixture_page.cropbox.upper_right = (right - 12, top - 12)
        if sample.get("rotate"):
            fixture_page.rotate(sample["rotate"])
        with (folder / "input.pdf").open("wb") as stream:
            writer.write(stream)
        env = os.environ.copy()
        env["SUMATRA_TEST_PDF_ANNOTATIONS"] = str(folder)
        env["SUMATRA_TEST_PDF_MARKUP_TYPE"] = args.markup
        env.pop("SUMATRA_TEST_ANNOTATIONS_FORCE_OCR", None)
        if sample.get("force_ocr"):
            env["SUMATRA_TEST_ANNOTATIONS_FORCE_OCR"] = "1"
        command = [str(args.exe.resolve()), "-appdata", str(folder / "appdata"),
                   "-render", "1", str(folder / "input.pdf")]
        process = subprocess.run(command, env=env, capture_output=True, timeout=300)
        (folder / "run.log").write_bytes(process.stdout + process.stderr)
        report = (folder / "results.txt").read_text(encoding="utf-8")
        assert process.returncode == 0 and "Overall: PASS" in report, (sample["name"], process.returncode, report[-1500:])
        assert "FAIL" not in report, sample["name"]
        # Optional sample-specific bounds guard against merging neighboring
        # rows/columns. Round-trip equality alone cannot detect a giant box.
        for line in report.splitlines():
            fields = line.split()
            if fields and fields[0] == "raw":
                width, height = map(float, fields[4:6])
                if "max_band_width" in sample:
                    assert width <= sample["max_band_width"], (sample["name"], line)
                if "max_band_height" in sample:
                    assert height <= sample["max_band_height"], (sample["name"], line)
        validate_properties(folder / "properties.pdf")
        independent_render(folder / "marked.pdf", folder / "pdfium-highlight.png")
        independent_render(folder / "properties.pdf", folder / "pdfium-properties.png")
        opacity_cases = validate_shape_pixels(folder)
        comparison(folder)
        assert digest(source) == before, "Original PDF was modified"
        result = dict(name=sample["name"], original_unchanged=True,
                      native_checks=report.count(": PASS"), render_cases=48,
                      independent_renderer="PDFium", status="PASS")
        result["independent_opacity_cases"] = opacity_cases
        results.append(result)
        print(json.dumps(result), flush=True)
    (output / ("summary-" + args.only + ".json" if args.only else "summary.json")).write_text(
        json.dumps(results, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
