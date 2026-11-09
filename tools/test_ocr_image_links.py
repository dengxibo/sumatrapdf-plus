"""Exercise real canvas mouse handlers against linked-image PDF/EPUB fixtures.

OCR output is injected deterministically into the normal engine text cache;
this tests interaction without depending on the installed recognition model.
"""
import argparse
import io
import os
import subprocess
import zipfile
from pathlib import Path
import fitz
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exe', type=Path, default=Path('out/dbg64/SumatraPDF-Plus.exe'))
args = parser.parse_args()
root = Path('out/ocr-image-links').resolve()
root.mkdir(parents=True, exist_ok=True)
image = io.BytesIO()
Image.new('RGB', (240, 160), '#ddd8c9').save(image, format='PNG')
data = image.getvalue()
pdf = fitz.open()
pdf.new_page(width=500, height=650)
pdf.new_page(width=500, height=650)
page = pdf[0]
page.insert_image(fitz.Rect(40, 40, 280, 200), stream=data)
page.insert_link({'kind': fitz.LINK_GOTO, 'from': fitz.Rect(40, 40, 280, 200), 'page': 1})
page.insert_text((40, 260), 'LINK')
page.insert_link({'kind': fitz.LINK_GOTO, 'from': fitz.Rect(40, 245, 75, 265), 'page': 1})
pdf.save(root / 'linked-image.pdf')
with zipfile.ZipFile(root / 'linked-image.epub', 'w') as z:
    z.writestr('mimetype', 'application/epub+zip')
    z.writestr('META-INF/container.xml', '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
    z.writestr('book.opf', '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">ocr-image-links</dc:identifier><dc:title>Linked image test</dc:title><dc:language>en</dc:language></metadata><manifest><item id="a" href="a.xhtml" media-type="application/xhtml+xml"/><item id="b" href="b.xhtml" media-type="application/xhtml+xml"/><item id="img" href="image.png" media-type="image/png"/></manifest><spine><itemref idref="a"/><itemref idref="b"/></spine></package>')
    z.writestr('a.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>A</title></head><body style="font-size:12px"><p><a href="b.xhtml">LINK</a></p><p><a href="b.xhtml"><img src="image.png" style="width:120px;height:80px"/></a></p></body></html>')
    z.writestr('b.xhtml', '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>B</title></head><body><h1>Destination</h1></body></html>')
    z.writestr('image.png', data)
exe = args.exe.resolve()
for ext in ('pdf', 'epub'):
    settings = root / f'settings-{ext}'
    settings.mkdir(exist_ok=True)
    (settings / 'SumatraPDF-settings.txt').write_text('RestoreSession = false\nEBookUI [\n EpubProgressiveLoad = false\n]\n', encoding='utf-8')
    report = root / f'{ext}-results.txt'
    env = dict(os.environ, SUMATRA_TEST_OCR_IMAGE_LINKS=str(report))
    result = subprocess.run([str(exe), '-appdata', str(settings), '-render', '1', str(root / f'linked-image.{ext}')], env=env, capture_output=True, timeout=90)
    (root / f'{ext}-run.log').write_bytes(result.stdout + result.stderr)
    print(ext, result.returncode, report.read_text(encoding='utf-8') if report.exists() else 'No report', flush=True)
    assert result.returncode == 0
