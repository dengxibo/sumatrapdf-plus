"""Exercise Locate Current Chapter against real application windows.

Requires pypdf. Writes synthetic PDF/EPUB fixtures and reports under out/toc-locate.
Optional --book also checks an existing, read-only EPUB (for example Firefly).
"""

import argparse
import html
import os
from pathlib import Path
import subprocess
import zipfile

from pypdf import PdfWriter


def make_pdf(path, kind):
    writer = PdfWriter()
    for _ in range(16):
        writer.add_blank_page(width=600, height=1000)
    if kind == "single":
        writer.add_outline_item("Only chapter", 0)
    elif kind == "late":
        writer.add_outline_item("First chapter starts later", 5)
    elif kind == "samepage":
        parent = writer.add_outline_item("Part", 0)
        writer.add_outline_item("Earlier same-page heading", 8, parent=parent)
        writer.add_outline_item("Later same-page heading", 8, parent=parent)
        writer.add_outline_item("Next page", 15, parent=parent)
    elif kind != "none":
        first = writer.add_outline_item("Part one", 0)
        second = writer.add_outline_item("Part two", 5)
        writer.add_outline_item("Unrelated branch", 1, parent=first)
        parent = writer.add_outline_item("Chapter two", 7, parent=second)
        writer.add_outline_item("Target section", 8, parent=parent)
        writer.add_outline_item("Later section", 13, parent=parent)
        writer.add_outline_item("Part three", 15)
    writer.write(path)


def make_epub(path):
    chapters = []
    nav = []
    for i in range(16):
        name = f"chapter-{i}.xhtml"
        title = f"Chapter {i + 1}"
        text = ''.join(f'<p>Unique paragraph {i}-{j}. This content tests the reading anchor while the bookmark sidebar opens and filters are cleared.</p>' for j in range(18))
        chapters.append((name, '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>' + title + '</title></head><body><h1>' + title + '</h1>' + text + '</body></html>'))
        nav.append(f'<navPoint id="c{i}" playOrder="{i + 1}"><navLabel><text>{html.escape(title)}</text></navLabel><content src="{name}"/></navPoint>')
    nested = '<navPoint id="part" playOrder="0"><navLabel><text>Collected chapters</text></navLabel><content src="chapter-0.xhtml"/>' + ''.join(nav) + '</navPoint>'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
        z.writestr('META-INF/container.xml', '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0"><rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        items = ''.join(f'<item id="c{i}" href="{name}" media-type="application/xhtml+xml"/>' for i, (name, _) in enumerate(chapters))
        spine = ''.join(f'<itemref idref="c{i}"/>' for i in range(len(chapters)))
        z.writestr('content.opf', '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">toc-locate-regression</dc:identifier><dc:title>TOC locate regression</dc:title><dc:language>en</dc:language></metadata><manifest>' + items + '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx">' + spine + '</spine></package>')
        z.writestr('toc.ncx', '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="toc-locate-regression"/></head><docTitle><text>TOC locate regression</text></docTitle><navMap>' + nested + '</navMap></ncx>')
        for name, content in chapters:
            z.writestr(name, content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('out/dbg64/SumatraPDF-Plus.exe'))
    parser.add_argument('--output', type=Path, default=Path('out/toc-locate'))
    parser.add_argument('--book', type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    cases = []
    for kind in ('nested', 'single', 'none', 'late', 'samepage'):
        path = root / f'{kind}.pdf'
        make_pdf(path, kind)
        cases.append(path)
    path = root / 'chapters.epub'
    make_epub(path)
    cases.append(path)
    if args.book:
        cases.append(args.book.resolve())
    for i, path in enumerate(cases):
        settings = root / f'settings-{i}'
        settings.mkdir(exist_ok=True)
        (settings / 'SumatraPDF-settings.txt').write_text('RestoreSession = false\nEnableAskAI = true\nEBookUI [\n FontSize = 14\n EpubProgressiveLoad = true\n]\n', encoding='utf-8')
        report = root / f'results-{i}.txt'
        env = os.environ.copy()
        env['SUMATRA_TEST_TOC_LOCATE'] = str(report)
        process = subprocess.run([str(args.exe.resolve()), '-appdata', str(settings), '-render', '1', str(path)], env=env, capture_output=True, timeout=240)
        (root / f'run-{i}.log').write_bytes(process.stdout + process.stderr)
        result = report.read_text(encoding='utf-8') if report.exists() else 'No report'
        print(f'{path.name}: exit={process.returncode}\n{result}', flush=True)
        if process.returncode:
            raise AssertionError(f'TOC locate regression failed: {path}; see {report}')


if __name__ == '__main__':
    main()
