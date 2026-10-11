"""Five deterministic content types for manual theme/geometry acceptance.

Run with the bundled Python runtime. No application settings or PDFs are edited.
Color samples are synthetic; real photographic scans need separate acceptance.
"""
from io import BytesIO
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader
from pypdf import PdfReader

output = Path("out/dbg64-page-adjust/page-adjustment-fixture.pdf")
output.parent.mkdir(parents=True, exist_ok=True)
font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 24)
scan = Image.new("RGB", (1000, 1300), "white")
draw = ImageDraw.Draw(scan)
for y in range(80, 1220, 50):
    draw.text((60, y), "Scanned baseline: precision 0.01 degrees 0123456789", font=font, fill="black")
table = scan.copy()
draw = ImageDraw.Draw(table)
for y in range(60, 1250, 100):
    draw.line((40, y, 960, y), fill="black", width=2)
for x in range(40, 1000, 230):
    draw.line((x, 60, x, 1260), fill="black", width=2)
color = Image.new("RGB", (1000, 650))
pixels = color.load()
for y in range(color.height):
    for x in range(color.width):
        pixels[x, y] = (x * 255 // 999, y * 255 // 649, 255 - x * 255 // 999)
draw = ImageDraw.Draw(color)
draw.ellipse((200, 100, 600, 500), fill=(210, 85, 45), outline="white", width=5)
pdf = canvas.Canvas(str(output), pagesize=(612, 792))

def title(text):
    pdf.setFont("Helvetica", 16)
    pdf.drawString(40, 750, text)

title("1. Native text and vector rules")
pdf.setFont("Helvetica", 12)
for y in range(100, 700, 25):
    pdf.drawString(45, y, "Native text baseline: precision 0.01 degrees 0123456789")
    pdf.line(40, y - 4, 570, y - 4)
pdf.showPage()
for label, image in [("2. Monochrome scan", scan), ("3. Scanned table", table)]:
    title(label)
    pdf.drawImage(ImageReader(image), 40, 40, width=530, height=689)
    pdf.showPage()
title("4. Synthetic color image / illustration protection")
pdf.drawImage(ImageReader(color), 40, 260, width=530, height=345)
pdf.showPage()
title("5. Native text plus scanned image")
pdf.setFont("Helvetica", 12)
pdf.drawString(40, 710, "This text remains a native PDF text object.")
pdf.drawImage(ImageReader(scan), 40, 80, width=300, height=390)
pdf.drawImage(ImageReader(color), 350, 250, width=220, height=143)
pdf.showPage()
pdf.save()
pages = PdfReader(output).pages
assert len(pages) == 5
assert len(pages[0].images) == 0
assert len(pages[1].images) == 1
assert len(pages[2].images) == 1
assert len(pages[3].images) == 1
assert len(pages[4].images) == 2 and "native PDF text" in pages[4].extract_text()
print(f"Verified five content types: {output}")
