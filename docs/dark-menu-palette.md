# Dark menu floating surfaces

`ThemeMenuColors()` centralizes the dark-only menu palette. White and Warm
continue to use their existing native renderer / warm tint path.

| Token | Dracula | Dark |
| --- | --- | --- |
| Surface | #343746 | #171717 |
| Border | #424450 | #333333 |
| Text | #F8F8F2 | #F2F2F2 |
| Shortcut | #C8CAD3 | #B8B8B8 |
| Disabled | #7A8098 | #6F6F6F |
| Hover | #3B3E4D | #242424 |
| Selected | #44475A | #2C2C2C |
| Pressed | #4B4E62 | #343434 |
| Separator | #464957 | #303030 |
| Arrow | #D7D8DE | #C8C8C8 |
| Check / radio | #BD93F9 | #E0E0E0 |

Native HMENU retains measurement, placement, keyboard handling and flyouts.
The existing popup-window hook paints the native item rectangles with these
tokens, after native drawing completes. Submenus use the same surface. The
Windows shadow is retained; no custom shadow or popup geometry is added.
The application menu bar has a dark-only UAH paint subclass using the same
state tokens. Normal menu-bar background remains application chrome.

## Verification, 2026-10-07

- Debug x64 compilation/link succeeded, zero warnings/errors.
- Inspected screenshots of White, Warm, Dracula and Dark main popup menus.
- Inspected settings/theme nested flyouts, parent selection, keyboard arrows,
  translated access keys and radio indicators.
- Inspected Dark PDF context menu with disabled items, shortcuts and separators.
- White/Warm drawing branches and metrics were not replaced.

Not yet exhaustively verified: native menu-bar mode, all PDF/EPUB/annotation/tab
context menus, long scrolling font flyouts, RTL, multiple DPI values, pressed
states, disabled submenus and exact pixel comparison against pre-change
White/Warm screenshots. These remain acceptance checks, not claimed passes.

## Flicker correction

The first dark renderer erased and drew rows directly into the window DC and
queued repaint work after almost every native menu message. This could expose
blank/incomplete frames and alternate native/theme frames even while idle.
Dark rendering now composes into a compatible bitmap and transfers once.
WM_PAINT and animation WM_PRINT use the themed composition directly; background
erase is suppressed. Query/idle messages no longer enqueue repaint work.
Native GDI hover transfers are replaced synchronously with the composed frame,
before system-colored pixels reach the window; no deferred hover repaint is
needed. Window/client DC origins are matched to avoid moving rows during hover.
White/Warm paths are unchanged. The corrected version builds successfully.
Repeated runtime snapshots check open/flyout behavior, but are not a high-frame-
rate recording and cannot certify the absence of every transient frame.
