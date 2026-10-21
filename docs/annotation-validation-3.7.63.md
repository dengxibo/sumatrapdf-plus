# PDF annotation validation — 3.7.63

## Geometry and PDF properties

Run `tools/test_pdf_annotation_geometry.py --manifest <local-samples.json> --output <output-directory>` against the built executable. The manifest entries contain `name`, `source`, one-based `page`, and optional `force_ocr`, `crop`, `rotate`, `max_band_width`, `max_band_height` and source `sha256`. Original files are read only; all fixtures, private source paths and images remain in the output directory.

- Six independent scan originals: Chinese horizontal and vertical books, English mathematics, existing OCR layers and application OCR. Two digital/mixed-content controls and a cropped/rotated derivative are not counted as additional scan originals.
- Raw selection, displayed selection geometry, created QuadPoints and reopened QuadPoints are recorded. Round-trip tolerance is 0.001 point; transformed boundaries allow one pixel.
- 100/175/400% zoom, 0/90/180/270 rotation, light, dark Original, legacy Smart alias and Match theme: 432 renders. The legacy Smart entry currently maps to Auto.
- Regression bounds cover marginal-text/body merging and giant merged columns on cropped, rotated OCR pages. A 600-quad selection verifies dynamic quad storage.
- Opacity 0/25/50/100%, widths 0/0.25/0.5/0.75/1/12 pt, remembered-style and saved-PDF round trips. Zero retains the existing 0.15-point hairline representation.
- Independent PDFium renders check 48 rectangle/circle opacity-width combinations per fixture, with at most three RGB levels of interior pixel error.
- Original source SHA-256 values remain unchanged.

## Native inspector controls

Set `SUMATRA_TEST_ANNOTATION_UI` to an existing output directory and run `-appdata <isolated-directory> -render 1 <small-test.pdf>`.

- Warm, White, Dracula and Black; Chinese DPI sequence 96 → 120 → 144 → 192 → 240 → 96.
- Simplified/Traditional Chinese, English, German and Arabic RTL at 96 DPI.
- 240/360 DIP sidebar widths and rectangle/circle/line/ink controls: 40 cases, each checking eight layouts.
- Font sizes, translated label dimensions, horizontal bounds, visibility, trackbar ranges, quarter-point and opacity callbacks, and restoring default opacity pass. Native control screenshots are saved per theme and DPI.

These checks inject frame DPI and exercise the editor's close/recreate path without changing system scaling. Physical monitor moves and the OS-delivered mixed-DPI/nonclient behavior are not claimed as tested.

## Other checks and limits

The shared text-markup overlay test, valid EPUB render and 102,866 utility assertions passed. Arbitrarily damaged external OCR positions are not reconstructed automatically; low-confidence source geometry is retained. Screenshots were inspected, but no universal image-to-glyph coverage guarantee is implied.
