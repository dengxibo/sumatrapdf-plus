# PDF / EPUB annotation sidebar sharing

## Implementation boundary

`src/AnnotationSidebar.h` contains the native visual shell extracted from the current PDF sidebar: header, list, row drawing, separators, inspector container, note editor sizing/right rail, color and icon dropdowns, footer action construction, secondary action painting, edit chrome, and size/layout handling.

`EditAnnotations.cpp` and `EditEbookAnnotations.cpp` supply backend callbacks, property visibility/value conversion, selection/navigation and persistence. They use the shared controls rather than an alternative EPUB shell. Property enumeration and some property-row assembly remain backend-local; this is not a complete unified inspector/model abstraction.

PDF persistence remains unchanged. EPUB retains its existing version-1 JSON store but now marks mutations dirty in memory and saves only on explicit Save (or a confirmed close-time save). Its footer offers Save and a JSON annotation copy; it does not write a new EPUB. Stored chapter/source anchors and quote fields are unchanged.

EPUB excerpts use cached quoted text, normalize whitespace, and are painted by the PDF row renderer. Notes are not substituted for source text, and there is no extraction or OCR during painting. Missing quote recovery is not added in this pass.

Both backends use the shared logical layout metrics and a scrolling inspector with a fixed footer. The footer exports on the left and places Save / Save as on the right. Row actions reserve their width even while hidden. The main-window splitter's TOC-edit minimum width applies only while the bookmark page is visible.

## Verification performed

- Debug x64 build: succeeded, zero warnings/errors.
- `test_util.exe`: all 102466 existing tests passed. These are utility tests, not a new end-to-end sidebar test suite.
- Actual native-app PDF/EPUB tab comparison at the current desktop DPI: White, Warm, Darcula, Dark.
- Existing PDF Square inspector and existing EPUB Underline/StrikeOut/Squiggly annotations were inspected without modifying their values.
- Shared header/list position, stripe, selection, quote rendering, note area, color value and fixed footer were inspected.
- Original Warm theme restored after inspection.

## Still requiring regression coverage

- 100/125/150/175/200 percent DPI matrix.
- Narrow/short-window overflow, long notes and many-property EPUB annotations.
- EPUB icon/line-end preview editing, save failure/retry and JSON-copy round trip.
- EPUB 2/3, fixed-layout, no-TOC and broken-anchor fixtures.
- New annotation creation, font/reflow/reopen anchor stability and both directions of document/list selection synchronization.
- Full PDF type/property/save regression matrix.

The manual theme comparisons were performed before the final small metadata-spacing and shared-footer-factory cleanup. The final source was rebuilt successfully afterward; those final changes have not received another complete visual matrix.
