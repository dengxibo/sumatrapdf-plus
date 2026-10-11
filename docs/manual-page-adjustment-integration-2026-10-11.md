# Manual page adjustment — final integration

Rollback baseline: `9f756bbc8`. Existing settings-window edits in
AppDialogTheme.cpp, EbookFontMenu.cpp and SumatraDialogs.cpp are preserved and
excluded from this task's commit.

## Six requested changes

1. **Palette hierarchy.** Panel/control colors continue to use existing theme
   tokens. Canvas is derived from the control token: Warm −12, White −14,
   dark themes +8. Warm's beige and Dracula's purple are retained, rather than
   sharing a hardcoded gray/black. Page paper still comes from the reader's
   ThemePageRenderColors policy. Page outline and grid use distinct alpha levels
   of neutral theme ink. This does not change PDF color data or global themes.
2. **Deskew composition.** Heading on the left, a 32 DIP minus/90 DIP numeric
   region/32 DIP plus group on the right. The degree sign occupies the same
   visual input region, independently of editable numeric text. One outer
   border replaces three floating boxes. Buttons are owner drawn with focus,
   hover and pressed feedback; native edit/keyboard behavior stays intact.
   Native trackbar custom draw replaces only the channel with a thin neutral
   line, retaining the theme thumb and measured endpoint/zero coordinates.
   Existing 0.05-degree click, continuous elapsed-time hold and stop rules stay.
3. **180-degree geometry.** SVG arrowhead tails point outward; the two inward
   straight extensions are removed at the vector source. One geometry serves
   every theme and DPI. Left/right/180 labels and absolute direction semantics
   stay intact.
4. **Two transparent zoom tools.** Fit button, page/percentage label and toolbar
   background were removed, including their creation/layout/command code.
   Zoom buttons copy the current buffered preview beneath them and alpha-blend
   cached icons, leaving the page/grid visible in normal state. Hover/pressed
   add a subtle fill. Click targets remain 32 DIP, and zoom limits disable the
   corresponding button. Ctrl-wheel/pan remain. CreateSvgIconAlphaBitmap uses
   the existing SVG assets and MuPDF SVG renderer with an RGBA pixmap, preserving
   premultiplied alpha rather than keying a solid background color. Existing
   other toolbar callers are unchanged.
5. **Theme frame consistency.** The previous callback rendered the new preview
   before replacing old brushes and running native theme hooks. Those hooks
   could synchronously repaint children with mixed resources; the final repaint
   also requested background erasure. Now parent and visible children suspend
   redraw, brushes/icons/chrome and cached preview update, then redraw resumes
   with one complete buffered parent/child refresh without an erase pass.
   Intentionally hidden children stay hidden. All handled render-failure paths
   resume drawing; no Sleep, delayed transition or fake cover is involved.
   A successfully rendered bitmap replaces the old one; it is never cleared
   merely to await a new theme bitmap. No asynchronous render worker is added.
6. **Actual crop output preview.** See the geometry section below.

## Auto Crop semantics and shared geometry

Existing Apply call chain: PdfRotatePagesDoIt → EngineMupdfAdjustPageView →
PdfWrapContentsWithCm → EngineMupdfSaveUpdated. Auto Crop removes the rotated
page's empty corners using its largest axis-aligned inscribed rectangle. It is
not text detection, black-border removal or content-aware margin trimming.
The backend wraps content in a transform, updates MediaBox/CropBox and existing
auxiliary boxes, and clears /Rotate after baking. Clean quarter turns keep their
existing /Rotate fast path. Existing thresholds, including the <0.05-degree
no-crop interval, are unchanged.

The former preview independently duplicated crop math against bitmap pixel
dimensions, clipped the image and outlined its rotated source rectangle. It did
not show the final output page or its blank corners and ignored session deskew
when choosing the crop size.

The original inscribed-size function now lives in PdfAdjustmentGeometry.h,
called by BOTH Apply and preview. EngineMupdfGetPageAdjustmentSource reads the
same pdf_page_obj_transform source box used by Apply, including existing crop,
intrinsic rotation and UserUnit normalization. It is read-only. Preview renders
a theme-aware source bitmap with session deskew overridden to zero, then applies
the same ordered operations: stored session deskew → flips → pending clockwise
turn. Crop size uses the backend's existing pending+session angle convention.

Without crop, preview fills/outlines the transformed output bounding page,
including the empty corners that are part of the saved page. With crop, it
clips/fills/outlines the shared actual crop rectangle. Both use the same original
source fit scale and pan center: changing crop never calls Fit or recenters.
The grid resets to screen coordinates afterward. This makes the boundary and
removed area visible without inventing a crop mask. Preview uses source point
dimensions rather than rounded bitmap dimensions. Output bounding size follows
the same ordered rotation/flip composition; raster interpolation can differ
slightly from reopened vector output at pixel edges.

Each page in a multi-page Apply still computes geometry from its own source box;
the current preview rectangle is never passed to other pages. Existing session
deskew also participates in Apply availability, initialized before that check.
Auto Crop remains OFF by the user's earlier explicit choice.

## Performance and unchanged behavior

Angle/crop/flip/grid updates reuse the source bitmap. Per-frame crop work is
constant-cost trigonometry, not image analysis, PDF parsing or theme conversion.
The stored double precision deskew and elapsed-time hold integral are unchanged.
Release, leave, capture loss, focus loss, deactivation and destruction still stop
the hold. Theme changes stop the hold before refreshing resources.

Page color modes and image protection still use BuildViewDarkModeProfile and
ApplyRenderThemePostColors. Source bitmap/grid/borders/icons are display-only;
Apply receives only geometric choices and page numbers. Specified-page disabled
state, stored range text and independent scrolling/footer remain.

## Tests and honest acceptance status

- Full debug build: passed, zero warnings/errors.
- Existing utility suite: passed.
- PdfPageAdjustmentTests: passed (precision, parsing, composition, hold travel).
- PdfAdjustmentTrackbarTests: passed (four layout scales, native thumb centers).
- New PdfAdjustmentGeometryTests: **17,980** signed angle/source-size cases
  passed, checking crop corners, symmetry, non-destructive false results and
  existing near-quarter thresholds across portrait/landscape/square/tall pages.
- Localization audit: 36 current labels/messages covered after toolbar removal.
- Diff checked; no unrelated settings edits included in the commit.

No application UI automation was performed, following the user's existing
instruction to receive builds for self-verification. Therefore actual Warm,
White, Dracula and Dark screenshots, rapid-switch frame recordings, physical
100/125/150/200% DPI, mouse hold frame time and Apply/reopen comparisons are NOT
claimed as passed. Shared math tests do not establish those end-to-end results.

Manual acceptance: use native text, monochrome/table/color scans and mixed PDFs;
rapidly cycle Warm→White→Dracula→Dark→Warm and reverse; inspect the transition,
not only the final frame. Hold/release ±, enter 0.01 and ±1.25 degrees, use the
slider, test 90/180 and flips. Toggle crop at a fixed zoom/pan: the page border
must shrink without changing the text scale. Test each range scope and mixed
page dimensions, existing CropBox, /Rotate and session deskew. Apply to a copy,
reopen and compare page boxes/content with the final preview; Cancel must leave
the file unchanged. Confirm only two transparent zoom tools remain.

Known inherited limits: the reader's display-enhancement filter is not integrated
here; image darkening/protection follows the reader's existing classifier.
Subsequent external content edits that do not replace the source engine or bump
the color epoch require reopening the dialog. Existing backend crop conventions
and thresholds are deliberately preserved; this task does not redefine them.

## Files/functions

PdfTools.cpp: layout, native custom draw, cached preview, theme refresh and tool
painting. EngineMupdf.cpp/EngineAll.h: read-only source box query, shared crop
call. PdfAdjustmentGeometry.h: extracted existing crop algorithm. Toolbar.cpp/.h:
premultiplied-alpha SVG cache creation. SvgIcons.cpp: simplified 180° paths.
tests/PdfAdjustmentGeometryTests.cpp; Version-history.md; this report.
