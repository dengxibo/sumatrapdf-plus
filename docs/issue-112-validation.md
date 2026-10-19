# Issue 112 validation — 3.7.61

The native integration test is enabled by SUMATRA_TEST_IMAGE_EDIT=1 with SUMATRA_TEST_IMAGE_EDIT_DIR set to an existing output directory. Run the executable with an isolated -appdata directory and -render 1. Exit code 0 indicates success; results.txt in the output directory records each case.

## Automated checks

- Warm, White, Dracula and Dark; Simplified Chinese, Traditional Chinese, English, German and Arabic.
- Native WM_DPICHANGED sequence 96 → 120 → 144 → 192 → 96 DPI, translated control bounds and initial layout.
- Four drawing directions, Esc and capture cancellation preserving the previous crop, 90° rotation and reverse rotation.
- Saved crop dimensions and original red/blue pixels across all themes, without overlay pixels or extra DPI scaling.
- Default closed-after-save behavior, signature save/apply callback, filename collision avoidance, invalid remembered directory fallback.
- Failed write keeps the editor, crop and remembered directory; subsequent Chinese-path save succeeds and records the open preference. The failure test suppresses only the warning dialog, using the same save implementation.

## Desktop checks

- Open an image through the normal application UI, redraw the initial full-image selection by dragging, inspect all eight handles and the overlay.
- Checkbox receives focus and Space toggles it. Esc returns from crop mode to save mode; closing then follows the existing EscToExit preference.
- A manually exported 300×200 crop matches the corresponding original pixels.

## Remaining manual coverage

The automated DPI round trip is a native message simulation, not a physical two-monitor drag. Physical monitor round trips, Alt+Shift mouse modifier input, Space+drag, every hover/pressed state and the full OCR/pan/annotation interaction matrix still require manual verification. Those cases are not claimed as tested by the native suite.

Testing found and fixed German button-label wrapping and the original path/browse controls missing Tab stops.

## Format coverage after 3.7.61

Removed the PDF/image type gate. Native clipped exports passed for PDF, PNG, DOCX, FB2, HTML, TXT, CBZ and EPUB at 0/90/180/270 degrees and 50/100/200 percent zoom. This covers the shared fixed-page path; formats rendered through external browser controls are outside that path.
