# Issue 113: responsive toolbar validation

## Behavior

The toolbar remains one row. Page navigation and the existing search edit have priority; annotations, rotation/font tools, auxiliary tools, custom commands, file commands and other view controls overflow progressively. At very small widths page navigation can also overflow; the page-number slot is represented by Go to Page. Widening restores tools automatically. Explicitly hidden annotation tools do not reappear in the overflow menu.

The menu groups tools by function, preserves native enabled/checked states and shortcuts, includes custom toolbar commands, and reuses the existing OCR and read-aloud option builders. Secondary search icons collapse before the query; their commands remain available in the overflow menu. Resizing keeps the same edit HWND and selection. Rebuilding for DPI/language changes transfers the query and selection without restarting search. No advanced setting or persisted layout was added.

## Automated checks

Run the debug executable with an isolated -appdata directory, -render 1 and a sample PDF, setting SUMATRA_TEST_TOOLBAR_OVERFLOW to an absolute report filename. Exit code zero and Overall: PASS indicate success.

- Four themes and Simplified Chinese, Traditional Chinese, English, German and Arabic, using the real language-switch path.
- Native icon sizes 18 and 36, widths 3200 → 1280 → 960 → 640 → 500 → 640 → 960 → 3200 (Windows minimum tracking dimensions may clamp requested sizes).
- Visible search/edit bounds, non-overlapping native toolbar buttons, query and selection preservation, complete restoration on widening.
- Menu command availability and checked/disabled states; long custom captions and duplicate command IDs; hidden annotation preference.
- WM_DPICHANGED simulation 96 → 120 → 144 → 192 → 96, checking query transfer and layout coherence. This does not replace testing real monitor DPI settings.

## Desktop verification

The isolated visible application opens the grouped menu, exposes annotation tools and executes a rotation command. Search text remains visible in the narrow window. Keyboard navigation was checked separately and fixed so toolbar Enter/Space cannot be intercepted as document page-scroll accelerators.

## Remaining manual coverage

Physical monitor-to-monitor dragging, all four actual Windows DPI settings, IME composition during live resizing, every hover/pressed state, and exhaustive dropdown option execution still require manual verification. The automated test does not claim these checks.

## Delivery

Debug build: out/dbg64/SumatraPDF-Plus.exe. Build and native test reports are under out/issue-113. Included with the issue 112 follow-up adjustments in release 3.7.62. OCR has its own menu group, labeled 识别 / 識別 in Chinese and OCR in English.
