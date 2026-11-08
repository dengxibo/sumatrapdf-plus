"""Render EPUB illustration regressions through the application (requires Pillow).

python tools/test_epub_illustrations.py --baseline --exe out/dbg64/SumatraPDF-Plus-illustrations-before.exe
python tools/test_epub_illustrations.py
Optional --book creates local, single-chapter sample EPUBs; copyrighted assets stay in out/.
"""

import argparse
import html
import io
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile
import xml.etree.ElementTree as ET

from PIL import Image


def epub(path, body, css="", assets=None, head=""):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        assets = assets or {}
        items = ['<item id="text" href="OEBPS/text.xhtml" media-type="application/xhtml+xml"/>']
        for i, name in enumerate(assets):
            mime = "text/css" if name.endswith(".css") else "image/png" if name.endswith(".png") else "image/jpeg"
            items.append(f'<item id="a{i}" href="{html.escape(name)}" media-type="{mime}"/>')
            z.writestr(name, assets[name])
        z.writestr("content.opf", '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">illustrations-test</dc:identifier><dc:title>Illustrations regression</dc:title><dc:language>en</dc:language></metadata><manifest>' + ''.join(items) + '</manifest><spine><itemref idref="text"/></spine></package>')
        z.writestr("OEBPS/text.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Illustrations regression</title>' + head + '<style>' + css + '</style></head><body>' + body + '</body></html>')


def fixtures(root):
    red = io.BytesIO()
    Image.new("RGB", (400, 200), (220, 30, 40)).save(red, format="PNG")
    assets = {"OEBPS/red.png": red.getvalue()}
    cases = {}
    base = 'body{margin:0} .test{width:400pt;margin:0} img{margin:0 !important} '
    for pct in (30, 40, 50, 60, 70, 80):
        for note in (False, True):
            cls = f"kindle-cn-bodycontent-image-alone{pct}" + ("-withnote" if note else "")
            name = f"width-{pct}" + ("-caption" if note else "")
            body = f'<div class="test"><div class="kindle-cn-bodycontent-div-alone100"><img class="{cls}" src="red.png"/>'
            if note:
                body += '<p class="kindle-cn-picture-txt-withfewcharactors">Caption</p>'
            body += '</div></div>'
            css = base + f'img.{cls}{{width:{pct}%;height:auto}}'
            cases[name] = {"width": 400 * pct / 100, "height": 200 * pct / 100}
            epub(root / (name + ".epub"), body, css, assets)
    extra = {
        "block-percent": ('<div class="test"><img style="display:block;width:50%;height:auto;margin:0 auto !important" src="red.png"/></div>', 200, 100),
        "block-border": ('<div class="test"><img style="display:block;width:50%;height:auto;margin:0 auto !important;border:2pt solid #00aa00" src="red.png"/></div>', 200, 100),
        "oversized-box": ('<div class="test"><img style="display:block;width:800pt;height:400pt" src="red.png"/></div>', 400, 200),
        "nested-percent": ('<div class="test"><div style="width:50%"><img style="display:block;width:50%;height:auto" src="red.png"/></div></div>', 100, 50),
        "intrinsic": ('<div class="test"><img src="red.png"/></div>', 300, 150),
        "inline": ('<p class="test">A <img class="kindle-cn-inline-image" style="height:12pt;width:auto" src="red.png"/> B</p>', 24, 12),
        "explicit": ('<div class="test"><img width="160" height="80" src="red.png"/></div>', 160, 80),
        "sidebar": ('<div class="test"><div class="kindle-cn-bodycontent-div-left1"><img class="kindle-cn-bodycontent-divright-image-withnote" src="red.png"/></div></div>', 120, 60),
    }
    for name, (body, w, h) in extra.items():
        cases[name] = {"width": w, "height": h}
        epub(root / (name + ".epub"), body, base + 'div.kindle-cn-bodycontent-div-left1{width:30%;float:left} img.kindle-cn-bodycontent-divright-image-withnote{width:100%}', assets)
    for count in (2, 3):
        kind = "double" if count == 2 else "triple"
        cls = f"kindle-cn-tourism-{kind}_image_with_notes"
        cells = ''.join(f'<td><img class="{cls}-4" src="red.png"/></td>' for _ in range(count))
        css = base + f'table{{width:400pt;border-spacing:0}} td{{width:{100/count}%;padding:0}} img.{cls}-4{{width:90%;height:auto}}'
        name = f"table-{count}"
        cases[name] = {"width": 400 / count * .9, "height": 200 / count * .9, "count": count}
        epub(root / (name + ".epub"), f'<table class="{cls}-1"><tr>{cells}</tr></table>', css, assets)
    # At this viewport, image + margins fit after the spacer, but the caption
    # does not. The control proves this tests grouping, not just image fitting.
    for name, prefix, caption in (("caption-break", 740, "Caption " * 28),
                                   ("caption-break-control", 740, ""),
                                   ("caption-long", 700, "Caption " * 600),
                                   ("image-break", 900, "")):
        body = f'<div style="height:{prefix}pt"></div><div class="test"><figure><img src="red.png"/>'
        if caption:
            body += f'<figcaption>{caption}</figcaption>'
        body += '</figure></div><p>End marker</p>'
        epub(root / (name + ".epub"), body, base + 'figure{margin:0} figcaption{font-size:14pt}', assets)
    tall = io.BytesIO()
    Image.new("RGB", (200, 1600), (220, 30, 40)).save(tall, format="PNG")
    epub(root / "tall.epub", '<div class="test"><img src="tall.png"/></div><p>End marker</p>', base, {"OEBPS/tall.png": tall.getvalue()})
    (root / "cases.json").write_text(json.dumps(cases, indent=2), encoding="utf-8")
    return cases


def sample_book(book, root):
    """Keep original styles and the nearest figure/table for each image class."""
    records = []
    seen = set()
    ET.register_namespace("", "http://www.w3.org/1999/xhtml")
    with zipfile.ZipFile(book) as z:
        for name in z.namelist():
            if not name.endswith((".html", ".xhtml")):
                continue
            tree = ET.fromstring(z.read(name))
            parents = {c: p for p in tree.iter() for c in p}
            for image in tree.iter():
                if image.tag.rsplit('}', 1)[-1] != "img":
                    continue
                cls = image.get("class", "no-class")
                if cls in seen:
                    continue
                seen.add(cls)
                figure = parents[image]
                while figure in parents and parents[figure].tag.rsplit('}', 1)[-1] in ("td", "tr", "tbody", "table"):
                    figure = parents[figure]
                body = ET.tostring(figure, encoding="unicode")
                # Preserve the original namespace without generated ns0 selectors.
                body = re.sub(r'(<\/?)(?:ns\d+:)', r'\1', body)
                assets = {}
                for node in figure.iter():
                    src = node.get("src")
                    if src:
                        assets["OEBPS/" + src] = z.read("OEBPS/" + src)
                head = ""
                for node in tree.iter():
                    if node.tag.rsplit('}', 1)[-1] == "link" and node.get("href", "").endswith(".css"):
                        href = node.get("href")
                        assets["OEBPS/" + href] = z.read("OEBPS/" + href)
                        head += f'<link rel="stylesheet" type="text/css" href="{href}"/>'
                target = "sample-" + str(len(records) + 1)
                epub(root / (target + ".epub"), body, assets=assets, head=head)
                records.append({"case": target, "chapter": name, "class": cls, "src": image.get("src"), "style": image.get("style"), "pixels": Image.open(io.BytesIO(z.read("OEBPS/" + image.get("src")))).size})
    (root / "sample-cases.json").write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding="utf-8")


def render(exe, root, name, page, label, width, height, font):
    settings = root / f"settings-{width}-{height}-{font}"
    settings.mkdir(exist_ok=True)
    (settings / "SumatraPDF-settings.txt").write_text(f'EBookUI [\n LayoutDx = {width}\n LayoutDy = {height}\n FontSize = {font}\n EpubProgressiveLoad = false\n]\n', encoding="utf-8")
    output = root / label
    output.mkdir(exist_ok=True)
    png = output / f"{name}-{width}-{height}-{font}-p{page}.png"
    env = os.environ.copy()
    env["SUMATRA_DUMP_RENDER_BMP"] = str(png)
    env["SUMATRA_RENDER_EXIT_AFTER_DUMP"] = "1"
    process = subprocess.run([str(exe), "-appdata", str(settings), "-render", str(page), "-zoom", "100", str(root / (name + ".epub"))], env=env, capture_output=True, timeout=90)
    png.with_suffix(".log").write_bytes(process.stdout + process.stderr)
    if process.returncode or not png.exists():
        raise RuntimeError(f"Render failed: {name} page {page}, exit {process.returncode}; see {png.with_suffix('.log')}")
    return png


def red_box(png):
    im = Image.open(png).convert("RGB")
    r, g, b = im.split()
    from PIL import ImageChops
    mask = ImageChops.multiply(r.point(lambda v: 255 if v > 160 else 0), g.point(lambda v: 255 if v < 90 else 0))
    return mask.getbbox()


def red_components(png):
    im = Image.open(png).convert("RGB")
    columns = []
    for x in range(im.width):
        column = im.crop((x, 0, x + 1, im.height))
        pixels = getattr(column, "get_flattened_data", column.getdata)()
        if any(r > 160 and g < 90 for r, g, b in pixels):
            columns.append(x)
    groups = []
    for x in columns:
        if not groups or x != groups[-1][-1] + 1:
            groups.append([])
        groups[-1].append(x)
    return [len(g) for g in groups]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path("out/dbg64/SumatraPDF-Plus.exe"))
    parser.add_argument("--output", type=Path, default=Path("out/epub-illustrations"))
    parser.add_argument("--baseline", action="store_true")
    parser.add_argument("--book", type=Path)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--samples", action="store_true")
    parser.add_argument("--case", help="Run just this dimension case")
    parser.add_argument("--pagination", action="store_true", help="Run pagination cases only, at the reference viewport")
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    cases = fixtures(root)
    if args.book:
        sample_book(args.book, root)
    if args.prepare_only:
        return
    results = {}
    failed = []
    label = "baseline" if args.baseline else "after"
    if args.pagination:
        for name in ("caption-break", "caption-break-control", "caption-long", "image-break", "tall"):
            boxes = []
            for page in (1, 2):
                png = render(args.exe.resolve(), root, name, page, label, 600, 800, 11)
                boxes.append(red_box(png))
            print(name, boxes)
            if not args.baseline:
                if name in ("caption-break", "image-break"):
                    assert boxes[0] is None and boxes[1] is not None, (name, boxes)
                    assert abs(boxes[1][2] - boxes[1][0] - 300) <= 2, (name, boxes)
                    assert abs(boxes[1][3] - boxes[1][1] - 150) <= 2, (name, boxes)
                if name in ("caption-long", "caption-break-control"):
                    assert boxes[0] is not None and boxes[1] is None, (name, boxes)
                if name == "tall":
                    assert boxes[0] is not None and boxes[1] is None, (name, boxes)
                    w, h = boxes[0][2] - boxes[0][0], boxes[0][3] - boxes[0][1]
                    assert abs(h / w - 8) < .1, (name, boxes)
        print("Pagination checks complete")
        return
    for width, height, font in ((600, 800, 11), (480, 650, 16), (900, 900, 11)):
        for name, expected in cases.items():
            if args.case and name != args.case:
                continue
            png = render(args.exe.resolve(), root, name, 1, label, width, height, font)
            key = f"{name}-{width}-{height}-{font}"
            box = red_box(png)
            actual = (box[2] - box[0], box[3] - box[1]) if box else (0, 0)
            if expected.get("count"):
                widths = red_components(png)
                actual = (widths[0] if widths else 0, actual[1])
                if len(widths) != expected["count"]:
                    failed.append(key)
            results[key] = {"expected": expected, "box": box, "size": actual}
            if name == "block-border" and not args.baseline:
                im = Image.open(png).convert("RGB")
                r, g, b = im.split()
                from PIL import ImageChops
                border = ImageChops.multiply(g.point(lambda v: 255 if v > 120 else 0), r.point(lambda v: 255 if v < 90 else 0)).getbbox()
                assert border and box and abs(border[2] - border[0] - 204) <= 2, (name, border, box)
            if not args.baseline and any(abs(actual[i] - expected[k]) > 2 for i, k in enumerate(("width", "height"))):
                failed.append(key)
            print(key, actual)
    if args.samples:
        for record in json.loads((root / "sample-cases.json").read_text(encoding="utf-8")):
            render(args.exe.resolve(), root, record["case"], 1, label, 600, 800, 11)
    (root / (label + ".json")).write_text(json.dumps(results, indent=2), encoding="utf-8")
    if failed:
        raise AssertionError(f"{len(failed)} illustration regressions: {', '.join(failed)}")
    print("Baseline recorded" if args.baseline else f"PASS: {len(results)} illustration measurements")


if __name__ == "__main__":
    main()
