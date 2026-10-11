# Manual page adjustment checkpoint — 2026-10-11

Baseline: `e54445025` (shared caption, zoom/pan, buffered preview, sidebar).

## Implementation

The same layout is used at normal and maximized sizes. Deskew is pinned above
the auxiliary settings; the settings scroll and the Apply/Cancel footer stays
fixed. The three direction shortcuts share one row, and the two flips share
another. Direction selects an absolute pending quarter turn and preserves the
fine correction. Selecting Deskew restores the original direction.

Canonical double precision state is independent of edit formatting. Single
click adds 0.05 degrees. Holding waits 250 ms after the initial step, then moves
at 0.3 degrees/second, smoothly accelerating to 1.8 over 1.5 seconds. Travel is
the analytic integral of elapsed time, so timer delays do not alter the result.
Release, leaving the button, capture loss, focus loss, deactivation, destruction,
disable, DPI and theme changes stop the hold. Invalid partial input remains
editable and disables Apply. Slider covers ±5 degrees; input covers ±180.

Grid remains screen aligned and is never included in saved content. Sparse,
normal and dense spacing is 32, 20 and 12 DIP, with a major line every five.
Colors follow theme tokens. Cached SVG icons follow theme and DPI changes.
Angle updates reuse the preview bitmap and preserve zoom and pan; a larger
render after zoom replaces the old bitmap only when ready. Auto Crop defaults
off. Crop geometry, transform order and page range/save behavior are retained.

Investigated call chain: PdfRotateSetAngle → PdfRotatePaintPreview, and
PdfRotatePagesDoIt → EngineMupdfAdjustPageView → EngineMupdfSaveUpdated → reload.
Backend quarter-turn fast path formerly swallowed changes within 0.05 degrees.
Its tolerance is now 0.00001, retaining real 0.01-degree corrections without
changing the geometry algorithm.

## Automated verification

- Debug build with `bun ./cmd/build.ts`: succeeded, zero warnings/errors.
- Existing `out/dbg64/test_util.exe`: all 102,780 tests passed.
- Standalone `tests/PdfPageAdjustmentTests.cpp`: passed with MSVC /W4.
  Covers strict parsing and invalid input, 0.01 precision, accumulated clicks,
  idempotent direction selection, angle composition/wrapping/clamps, hold delay,
  monotonic travel, elapsed-time independence and continuous speed at ramp end.

## User acceptance still required

Per the user's request, deliver the build for their own UI verification.
Automated state tests do not validate native mouse capture, display performance,
theme appearance, DPI transitions or saved PDF rendering.

Check Warm/White/Dracula/Dark at 100/150/200% DPI, normal/maximized windows;
hold and release/leave/deactivate while observing text against the grid;
zoom/pan while deskewing; direction plus flip plus crop combinations;
current/all/specified page ranges; Cancel; Apply and reopen the output. Compare
0.01-degree and 90.01-degree outputs against the final preview. Verify no crop
retains all original content and crop only removes the intended outer area.
