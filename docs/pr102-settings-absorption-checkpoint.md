# PR #102: settings viewport, first batch

The first batch keeps the existing settings page geometry and parent/notification
relationships. The category list and OK/Cancel buttons remain stationary while
the content controls scroll within a clipped viewport. A separate opaque footer
keeps content painting away from the buttons. Initial height is limited to the
monitor work area; vertical resizing is supported. Width remains fixed until the
second batch defines horizontal layout rules. Keyboard dialog navigation reveals
the focused content control. Switching categories resets the content scroll.

Validation performed:

- clang-format and git diff --check passed.
- C++ compilation succeeded without warnings. Normal linking was blocked by the
  running main executable; replaying the build's linker arguments with separate
  EXE/PDB/ILK output paths produced out/dbg64/SumatraPDF-Plus-settings.exe.
- Used a separate settings directory at out/settings-scroll-profile.
- Native GUI checks confirmed initial layout, category switching, vertical
  resizing and wheel scrolling with stationary category navigation and buttons.
- The first GUI pass exposed painting near the footer boundary. An opaque
  footer was added and the candidate rebuilt. Rechecking that final candidate
  was blocked by the computer-use helper reporting that the foreground window
  did not report a process id, including its one recovery attempt.

Remaining verification before calling batch one fully validated: final footer
boundary, Tab traversal across clipped controls, theme changes, Chinese/English
labels, and the planned 100/150/200 percent DPI matrix. No user preferences were
changed. No commits were made.

Later batches: compact group spacing and collapse hidden sections, followed by
configurable vertical wheel lines with consistent continuous/single-page behavior.

## Next step: compact visible settings sections (2026-10-10)

Implemented in src/SumatraDialogs.cpp:

- Recalculate each page from immutable original control rectangles on every
  layout pass, preventing cumulative movement when switching pages or resizing.
- Reduce excess space below controls and between visible groups, while preserving
  control sizes, row spacing, translated label widths and category width.
- Size the outer page frame to its actual visible content. Short pages no longer
  acquire scrolling solely from the original 414-dialog-unit page frame.
- Move the Advanced Options entry directly below the last visible group when
  inverse search is hidden.
- Move ordinary controls with SWP_NOSIZE. In particular, a combo's collapsed
  rectangle must not be used to resize its drop-down list during scrolling.

Validation: clang-format and git diff --check passed. C++ compilation completed
with zero warnings. The normal build could not overwrite the running main EXE
(LNK1168); a separate final candidate linked successfully at
out/dbg64/SumatraPDF-Plus-settings-compact.exe, including the combo-height fix.
The native GUI helper still reports that the foreground window has no process
id, so the final visual/keyboard/DPI checks remain pending, not passed.

Next: complete those GUI checks, then horizontal resizing rules. Wheel behavior
is a later independent batch. No commits were made.

## Horizontal resizing and live GUI follow-up (2026-10-10)

- Removed the fixed maximum width. Original width remains the minimum.
- Keep category and label columns fixed; stretch full-width fields and group
  frames; anchor Browse/Test/Add/Remove and sidebar size controls to the right.
- Preserve combo drop-down height using CB_GETDROPPEDCONTROLRECT when resizing.
- Recompute layout after ShowWindow: modeless creation initially has a hidden
  parent, so IsWindowVisible could not identify the first page's visible groups.
- Give the opaque footer WS_CLIPSIBLINGS so it cannot paint over OK/Cancel.

The Windows GUI helper recovered in this pass. Native screenshots verified the
final candidate's compact first-open General page and visible OK/Cancel buttons.
Keyboard system-menu sizing verified width growth, stretched author field and
frames, stationary category width and right-aligned footer buttons. Initial
mouse-border resize attempts did not resize the window and are not counted as
successful checks. Full keyboard traversal, scroll/footer clipping at reduced
height, theme/language and 100/150/200-percent DPI checks remain pending.

clang-format and git diff --check passed. C++ compilation: zero warnings. Main
EXE linking remains blocked by the running program (LNK1168). Final independent
candidate linked successfully: out/dbg64/SumatraPDF-Plus-settings-resize2.exe.
Third batch (document wheel settings/behavior) has not been changed in this pass.
No commits were made.

## Third batch: vertical wheel lines and final footer/keyboard checks (2026-10-10)

Implemented WheelScrollLines (0 = Windows, 1-100 custom) through gen-settings.ts,
with Chinese inline comments and Chinese/traditional Chinese UI messages. Invalid
advanced-file values fall back to Windows; invalid dialog input is rejected before
saving. Each window clears its old wheel remainder when the setting changes.

Continuous distance scrolling and enlarged non-continuous single pages use the
same DPI-scaled 16-pixel line height. Explicit fit-content/whole-page navigation,
Ctrl zoom, and horizontal wheel behavior retain their existing branches. Custom
values use a 120-unit denominator and multiplier, so 7 lines remains exactly 7,
rather than rounding 120/7. WheelScrollPixels now uses a 64-bit intermediate.
Smooth continuous scrolling retains its existing scrollbar animation path, with
the custom line multiplier; the live checks below used SmoothScroll=false.

Regenerating Settings.h exposed pre-existing drift: FreeTextFont, FreeTextWidth,
FreeTextHeight and LastDrawStyle existed in the generated header but were absent
from the generator. Added their generator definitions and restored the recent-font
field's serialized order/comments. The round-trip test covers all five annotation
fields along with the new wheel setting, preventing silent loss on regeneration.

Final layout follow-up: all content controls now use WS_CLIPSIBLINGS. Section
frames sit behind fields, and outer page frames behind section frames. This fixes
the reduced-height footer overpainting exposed in this pass while preserving
section titles and ordinary dialog control parenting.

Verification:

- C++ compilation: zero warnings; main EXE link blocked by the running user app
  (LNK1168). Independent final link succeeded at
  out/dbg64/SumatraPDF-Plus-pr102-stage3b.exe.
- test_util.exe passed all 102466 tests, including wheel precision/coalesced-input
  overflow and generated settings round-trip checks.
- Diff whitespace checks passed with cr-at-eol enabled for existing CRLF resources.
- Isolated three-page PDF and Chinese settings: seven-line notch moved text 112
  screenshot pixels in continuous mode and in enlarged 150% single-page mode.
- Enlarged single page: reaching page-one bottom then another downward notch
  entered page two; upward input returned to page-one bottom.
- Fit-page single mode: a notch advanced exactly one page, despite seven-line
  customization.
- Chinese wheel input and warning text displayed correctly. Input 101 was blocked;
  cancelling discarded it (reopening showed 7).
- Final candidate: reduced the settings window from 754 to 454 screenshot pixels
  in height. Scroll top/bottom verified fixed navigation and footer, complete last
  item at bottom, and no content over OK/Cancel. Group titles remained visible.
- With content scrolled to the top, Shift+Tab traversed Cancel, OK, then the
  offscreen final checkbox; focus automatically scrolled that checkbox into view.

Remaining matrix: full 100/150/200-percent desktop DPI, theme changes, and live
custom-line scrolling with SmoothScroll=true. These are not marked as passed.
All three planned batches now have implementations; final matrix verification is
still outstanding. No user preferences were changed and no commits were made.

### Final verification follow-up (2026-10-10)

Found and fixed a per-monitor DPI omission in the new viewport: immutable control
rectangles, combo drop heights, category/footer geometry and scroll position now
scale on WM_DPICHANGED. Layout is suspended while the Windows per-monitor dialog
manager scales controls/fonts, then a posted message lays out and reveals focus.
This is a code-path fix; actual monitor transitions at 150/200 percent still need
physical desktop verification and are not claimed as passed.

SmoothScroll=true was enabled only in the isolated stage3b profile. One 120-unit
notch with WheelScrollLines=7 moved fixture line 3 from y=241 to y=129 after the
animation settled (112 screenshot pixels), matching the non-smooth result.
Applied Light-Warm -> Dark-Dracula in that profile and reopened settings: section
frames, labels, inputs, fixed navigation and footer rendered in the dark theme.
The initial warm-theme dialog was also visually checked. This covers theme
application/reopening, not every live-refresh and reduced-height combination.

The DPI follow-up compiled with zero warnings; the main output remains locked by
the user's running reader (LNK1168). Independent stage3b linking succeeded.
Current test_util run passed all 102337 tests; the runner's reported count differs
from the earlier run, so both counts are retained as recorded rather than treated
as a fixed expected count. Diff whitespace verification passed.

Remaining acceptance: real desktop 100/150/200-percent and cross-monitor moves,
plus live theme refresh while settings are open at reduced height. No commits.

### Single-screen follow-up: restrict scrolling repaint (2026-10-10)

The user has one monitor; cross-monitor acceptance is deferred and does not block
further work. Reviewed the remaining PR areas against local implementations.
Absorbed the fixed-navigation repaint detail: when only scroll position changes
and viewport geometry is unchanged, invalidate/erase only the right content and
scrollbar. Resizing, category changes and theme refresh retain full repaint.
The fixed category/footer are excluded from ordinary scroll erase requests.

clang-format and CRLF-aware diff checks passed. Compilation reported zero
warnings; the main EXE is still locked by the user reader. Independently linked
out/dbg64/SumatraPDF-Plus-pr102-final.exe and launched it with its own copied
profile at out/pr102-final-profile. In Dark-Dracula, reduced settings height from
754 to 454 screenshot pixels, switched to Reading, and scrolled to the bottom.
Navigation/footer stayed stationary; the last checkbox and frame were fully
visible, without content over the buttons. These screenshots verify rendering
correctness, not a quantified flicker/performance measurement.
No new unit tests were added for this repaint-only change. No commits.

### Previous-page destination semantics (2026-10-10)

Absorbed the remaining DisplayModel::GoToPrevPage distinction from PR #102.
Continuous navigation with a nonnegative offset still reveals the current page's
hidden top first. An explicit negative offset (GoToPrevPage(true) -> -1) now skips
that intermediate step and reaches the previous-page destination logic. Existing
fit-content handling and previous-page bottom-offset calculation remain intact.

Formatted DisplayModel.cpp; C++ compilation completed with zero warnings. Main
output remained locked by the user's reader; independent link succeeded at
out/dbg64/SumatraPDF-Plus-pr102-prev-page.exe. Current test_util run reported
102637 passing assertions; it does not directly exercise this DisplayModel path.

Native isolated GUI check: continuous single-page layout was confirmed checked
in View menu. With fit-content zoom, dragged the vertical scrollbar until page 2
line 7 appeared at the viewport top. Up (CmdScrollUp, which requests previous-page
bottom at fit-content zoom) navigated to page 1; fit-content placement showed its
content top. Restored page 2 line 7 at viewport top and clicked toolbar Previous:
page 2 line 1 became visible and page number remained 2, confirming ordinary
previous-page navigation still reveals the current page top first.

Cross-monitor DPI remains deferred per the user's single-screen setup. No commit.

### Remove redundant outer page frames (2026-10-10)

Per user approval, removed all seven outer category groupboxes from the dialog
resource and their visibility/translation references. Inner section frames remain.
Viewport geometry comes from the first section and category navigation; compaction
starts at the first visible section with a small top inset and computes trailing
controls without any outer-frame dependency. Removed obsolete outer-frame Z-order
handling. Right-side scrolling still computes its range from visible controls.

Formatted C++; compilation zero warnings. Main EXE locked by running reader;
independent link succeeded: out/dbg64/SumatraPDF-Plus-settings-clean.exe.
Chinese Dark-Dracula GUI checked General, Advanced and AI: no duplicate outer
heading/frame; first section titles are unobstructed and advanced trailing button
remains placed below the visible PDF section. Independent copied test profile.
CRLF-aware diff check passed. No commits.
