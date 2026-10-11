# Manual page adjustment — second pass, 2026-10-11

Rollback baseline: `d6855458a`. Existing unrelated edits in AppDialogTheme.cpp,
EbookFontMenu.cpp and SumatraDialogs.cpp remain untouched and outside this commit.

## Findings and fixes

- SVG resources and the shared DrawSvgIcon renderer already support alpha.
  The new cache passed an uninitialized compatible bitmap as the alpha blend
  destination, so transparent areas blended against undefined/black pixels.
  Initialize the cache surface with its actual state/theme background before
  calling the shared renderer. Keep separate hover, pressed, selected and
  disabled cache entries, invalidated on theme epoch/DPI changes. This fixes the
  compositing destination rather than covering the result with a rectangle.
- Derive the sidebar scroll extent from the last laid-out hint's bottom.
  Every scrolling child retains a region clipped above the fixed footer;
  scrollbar space is reserved separately. Row height and font size are kept,
  while spacing decreases from 8 to 5 DIP.
- Replace whitespace-separated angle labels with custom drawing aligned to
  native thumb centers. The common control can retain a stale thumb rectangle
  after a non-redrawing position/range change. The helper suspends redraw,
  forces position changes, measures each center and restores the selection.
  Also replace automatic tick frequency with an explicit zero tick: the native
  test exposed an automatic tick at −499 rather than zero with this setup.
  [Native control messages](https://learn.microsoft.com/en-us/windows/win32/controls/trackbar-controls)
  describe the native tick/rectangle queries used here.
- Add missing Simplified/Traditional Chinese entries to the existing translation
  catalogs and rebuild the resource archive. Existing translations in every
  language are preserved; English remains the normal fallback.
- Default grid mode is horizontal lines. Cross grid adds the same screen-fixed
  vertical lines, at weaker contrast. Both use the existing spacing and major
  line calculation. Show Grid disables both mode and density without resetting
  either selection. Neither mode participates in PDF processing.
- Zoom icons, page number and zoom percentage share a compact toolbar background
  drawn after the grid. Separators use a subtle shade of the theme panel color.
- Fine controls stay centered with a 90 DIP input and at least 32 DIP buttons.
  The redundant Deskew radio becomes a heading. Its old restore-direction
  action remains reachable by clicking the selected direction shortcut again;
  shortcuts continue selecting an absolute pending orientation and retain fine
  correction. Obsolete radio handling is removed.
- Auto Crop remains OFF by the user's explicit earlier instruction; this was
  intentional, not a restoration bug. Hold timing, transform/save/range
  algorithms are unchanged in this pass.

## Verification

- Debug build succeeded with zero warnings and errors after the fixes.
- Existing utility suite and PdfPageAdjustment precision/hold tests passed.
- New native hidden-control test passed for 100/125/150/200% layout sizes,
  five current positions per size: endpoints ordered, zero thumb center matches
  label coordinate, zero tick within native pixel rounding, selection restored.
  These are scaled geometry tests, not actual cross-monitor DPI acceptance.
- Chinese localization audit passed for all 36 labels/messages in the dialog.
- Diff checked with `git -c core.whitespace=cr-at-eol diff --check` because the
  translation catalogs intentionally retain their existing CRLF encoding.

Per the user's previous request, no application UI automation was performed.
Warm/White/Dracula/Dark appearance, real mouse hold/release/capture loss,
cross-monitor DPI transitions, footer scrolling and saved PDF appearance still
require user acceptance of the delivered build. The numerical/coordinate tests
do not establish those visual or end-to-end results.
