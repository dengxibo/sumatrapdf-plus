# Manual page adjustment: themed preview and final polish

Rollback baseline: `45dbeb759`. Unrelated settings/combobox edits are preserved.

## Root cause and minimal repair

The reader's normal view path is RenderCache::RenderCacheThread →
BuildViewDarkModeProfile → EngineBase::RenderPage with args.darkProfile →
conditional display bitmap postprocessing. MuPDF performs object/image-aware
processing internally; the legacy postprocessing path also supports protected
image regions. Original document colors and custom colors follow existing
profile/theme precedence, rather than an unconditional inversion.

The adjustment dialog previously called RenderPage with a View target but no
darkProfile and no postprocessing. Thus it bypassed both the object-aware and
legacy color stages while its controls followed the theme.

PdfRotateLoadPreview now uses BuildViewDarkModeProfile and the existing public
ApplyRenderThemePostColors helper in RenderCache.cpp. That helper already exists
for offscreen previews and reuses the reader's color decisions, bitmap remapping
and GetBitmapRecolorSkipRects protection. No copied classifier, new thresholds,
or separate inversion algorithm was added. Dark/Dracula use their own existing
theme/configuration parameters; Warm/White and Original mode use the same policy.

The resulting display bitmap is cached. Angle/flip/crop/grid changes transform
or overlay this cache without re-running color classification. Render size and
the render cache's darkModeEpoch control invalidation; theme callbacks and a
250 ms settings observer refresh it. Old bitmap replacement happens only after
the new bitmap succeeds. Rendering remains synchronous; there are no async jobs
that can outlive the window or apply an outdated angle. Source document changes
invalidate the preview and Apply eligibility, requiring reopening the dialog.

Geometry/save functions were untouched. PdfRotatePagesDoIt still submits only
angle, flips, crop and page range to the engine; it never consumes the display
bitmap. Grid is drawn after the image transform and before the toolbar; neither
grid nor theme processing is submitted to PDF output.

## UI changes

- Toolbar width follows translated page/zoom text plus buttons and padding;
  zoom/fit and DPI layout recompute the width.
- Rotation icons grow to 22 DIP; 180° gets a distinct double-arrow SVG in the
  shared icon set with the same 24-unit viewbox and 0.9 stroke style.
- Grid uses theme neutral disabled-text ink instead of selection blue, while
  retaining major/minor distinction and weaker vertical lines. Dark workspace
  derives from the theme control background, not a forced pure black.
- Thin page outline follows the image transform and remains display-only.
- Mode/density tooltips use the existing localization catalogs (cn/tw added).
- Existing actual-content scrolling and specified-page disabled/edit state are
  retained. Input values survive scope changes. Auto Crop stays off by request.

## Validation and limits

| Check | Result |
|---|---|
| Full debug build, independent out/dbg64-page-adjust | Passed, zero warnings/errors |
| Existing utility suite | Passed |
| Angle parsing/0.01 precision/composition/continuous hold curve | Passed |
| Native slider geometry at four scaled layout sizes | Passed |
| Chinese dialog labels/messages | 38 covered |
| Five-page content fixture structure | Passed: native text, monochrome image, table image, color image, mixed |
| Fixture rendered with Poppler and visually inspected | All five pages rendered; expected content visible |
| Warm/White/Dracula/Dark application appearance | Not UI tested; user acceptance required |
| Actual mouse hold/release/capture, four monitor DPIs | Not UI tested; previous tests establish numerical/geometry contracts only |
| Theme preview versus saved/reopened PDF | Data flow reviewed, end-to-end acceptance still required |
| Long-hold frame time | Not measured; expensive color work is outside the angle-update/paint path |

The fixture generator is tests/make_page_adjustment_fixture.py; it produces
out/dbg64-page-adjust/page-adjustment-fixture.pdf. The color page is synthetic,
not a photographic scan. Poppler reported unavailable Symbol/ArialUnicode font
aliases; the actual fixture uses Helvetica/embedded raster text and all pages
rendered correctly.

The reader's existing per-image decisions remain authoritative: some colorful
or photographic scans may intentionally retain original image colors. This
patch cannot improve unsupported scans independently of the reader. Reader
display enhancement filters are NOT included in this dialog; the shared theme
profile/post-color path is reused, not the enhancement-from-original filter
pipeline. No full color-equivalence claim is made with enhancement enabled.

Per the user's existing instruction to deliver builds for their own validation,
no application UI automation was performed. Compare the fixture and real photos
in both reader and preview under four themes with enhancement off; exercise
theme changes while open, hold/release, zoom/pan, rotation/flip/crop, page ranges,
Cancel, Apply/reopen, and verify the original PDF colors remain intact.

## Modified files

src/PdfTools.cpp; src/SvgIcons.cpp/.h; cmd/migrate-page-adjustment-translations.ts;
translations/translations-good.txt and translations.txt;
tests/make_page_adjustment_fixture.py; docs/md/Version-history.md; this report.
The generated translation archive is a build resource and is not tracked.
