# EPUB illustration sizing checkpoint — 2026-11-08

Issue: https://github.com/dengxibo/sumatrapdf-plus/issues/121

## Intended behavior

Preserve publisher size distinctions instead of guessing small/medium/large from
image pixels. This change adds no automatic enlargement, setting, or UI control.
Unspecified sizes retain intrinsic sizing; only container/page overflow shrinks
an image. Explicit full-page covers and recognized comic panels retain their
dedicated sizing and page-break rules.

## Causes and implementation

- Ordinary Kindle illustration CSS replaced publisher percentage widths with
  `auto` and `max-width`, although this MuPDF build has no `max-width` support.
  Other rules forced page-derived heights and unconditional page breaks.
- Block images applied their style width both to the anonymous containing block
  and to the image flow node: a 50% image in a 400pt container became 100pt.
  The outer block now resolves percentage width, and the inner image fills it
  without applying the percentage again. Borders, padding and auto margins stay
  around the specified image box; only the image reserves its explicit height.
- Sidebar wrappers retain publisher widths after unsupported floats are removed.
  A global EPUB `table { width:100% !important }` override is removed. Percentage
  table cells now use the table's declared width, excluding border spacing.
- Image layout measures the final size before line breaking, caps against a full
  page rather than its current remainder, and accounts for enclosing vertical
  margins/borders/padding. The old fixed 40pt/88pt caption allowances are removed.
- A narrowly identified single-image/caption block gets an unpaginated measure
  using the real line layout. If the complete group fits a page but not the
  remainder, it moves together. Oversized captions paginate normally. Grouping
  recognizes `figure` + direct `img`/`figcaption`, and Kindle `alone100` wrappers
  with one direct image and one explicitly classified caption. It deliberately
  does not infer captions for arbitrary mixed-content blocks.

The existing theme/canvas resize sequence, chapter warm-up and reflow count lock
are unchanged. The MuPDF image box/layout changes are shared by HTML-based
formats, not just EPUB; this remains the principal compatibility risk.

## Reproduction and automated checks

Requires the normal debug build, Python and Pillow:

```powershell
python tools/test_epub_illustrations.py
python tools/test_epub_illustrations.py --pagination
python tools/test_epub_illustrations.py --book "path\to\book.epub" --prepare-only
python tools/test_epub_illustrations.py --samples
./cmd/test-epub-font-position.ps1 -Book "path\to\book.epub"
```

The illustration script generates its fixtures, isolated settings, measurement
JSON and raster outputs below `out/epub-illustrations/`. `--baseline --exe ...`
records measurements without asserting corrected dimensions. Original books are
read only; their extracted sample assets are local build artifacts, never added
to the repository.

Validation performed:

- `bun ./cmd/build.ts`: passed, zero warnings/errors; modified C/C++ was formatted
  with clang-format before building.
- 66 rendered dimension checks: 30/40/50/60/70/80% with/without captions, block and
  nested percentages, bordered and oversized explicit boxes, intrinsic size,
  inline/HTML dimensions, sidebar and two/
  three-column images at 600×800/11pt, 480×650/16pt and 900×900/11pt layouts.
- Five pagination scenarios, two rendered pages each: short-caption grouping,
  an otherwise identical image without caption that stays on page one, a long
  caption allowed to paginate, image-only overflow, and an oversized portrait
  that keeps its aspect ratio and appears only once.
- Complete Firefly book: 72 reading-position checks passed both before and after
  removing the global table-width override. The second run is recorded in
  `font-position-final.log` (metrics `epub-705004187.jsonl`). The later border-box
  refinement is covered by the dimension/pagination suite rather than another
  full 36-font-change run.
- UI checks on extracted Firefly sample: single/facing pages, narrow/maximized
  window, dark/light theme and document-color changes. Image and caption remain
  visible and aligned. Test windows use isolated settings and are closed afterward.

An additional existing `SUMATRA_EPUB_THEME_BENCH` run on a single-chapter sample
reported eight pages and selected an empty final page, making `same_layout`
false. The pre-change executable gives the same failure. This is not counted as
a passing check; theme behavior was checked in the actual window instead.

## Firefly sample evidence

Local source: 《英国未来出版社·萤火虫丛书第一辑（套装共10册）》,
206,036,004 bytes. Archive inspection found 2,733 image references in 19 classes,
including 337 references with inline styles. The source declares multiple
percentage-width tiers, em-sized inline images and two/three-column figures.

`sample-cases.json` records the chapter, resource, class, inline style and native
pixels of all 19 selected representatives. Examples:

| Case | Chapter | Image resource | Publisher role |
|---|---|---|---|
| sample-1 | text00002.html | Image00001.jpg | Cover, inline height:100% |
| sample-2 | text00004.html | Image00002.jpg | 80%, no caption |
| sample-3 | text00006.html | Image00004.jpg | 80%, caption |
| sample-4 | text00006.html | Image00006.jpg | 40%, caption |
| sample-7 | text00006.html | Image00011.jpg | 40%, no caption |
| sample-13 | text00014.html | Image00286.jpg | Sidebar |
| sample-14 | text00014.html | Image00405.jpg | Two-column figure |
| sample-17 | text00029.html | Image01419.jpg | Three-column figure |
| sample-18 | text00034.html | Image01593.jpg | Inline symbol, height:1.2em |
| sample-19 | text00081.html | Image02382.jpg | 30%, no caption |

Before comparison uses the actual image-bearing page (often page two); the old
unconditional breaks left page one empty for these figures. Corrected samples
start on page one. Do not compare only the old empty first-page raster.

No claim is made that every image in all ten volumes has been visually checked.
Manual enlargement and arbitrary unknown EPUB layouts remain future work.
