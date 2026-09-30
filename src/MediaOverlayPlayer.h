/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// EPUB 3 Media Overlays playback: narration audio synchronized with text highlighting,
// auto-follow and continuous playback across spine documents. One session at a time.

struct MainWindow;
struct WindowTab;

// true once background detection found media overlays in the tab's EPUB
bool MediaOverlayTabHasNarration(WindowTab* tab);
bool MediaOverlayIsPlayingInTab(WindowTab* tab);
bool MediaOverlayIsPlaying();
// narration was started in the tab and not stopped (it may be paused)
bool MediaOverlayHasSessionInTab(WindowTab* tab);

// Read Aloud command: toggles narration when this location has an overlay. Returns false when
// the tab has none here, so the caller falls back to text-to-speech.
bool MediaOverlayHandleReadAloud(WindowTab* tab);
// start (or jump) narration at the viewport top, or at the phrase under a canvas point.
// false when this location has no overlay (caller should use text-to-speech)
bool MediaOverlayStartFromViewport(WindowTab* tab);
bool MediaOverlayStartAtPoint(WindowTab* tab, Point screenPt);
bool MediaOverlayPause();
void MediaOverlayStop();
// narration playback speed, saved in prefs
void MediaOverlaySetRate(double rate);

// the tab is closing
void MediaOverlayOnTabDocumentGone(WindowTab* tab);
// the tab's document is unloaded; when the tab shows the same unchanged file again (reload after
// a font size change, for example) narration resumes where it was
void MediaOverlayOnTabDocumentUnloaded(WindowTab* tab);
void MediaOverlayOnWindowClosing(MainWindow* win);

// canvas paint: draws the active fragment highlight, starts detection for a newly shown
// EPUB and keeps the narration bar placed
void MediaOverlayOnCanvasPaint(MainWindow* win, HDC hdc);
void MediaOverlayOnUserViewChanged(MainWindow* win);
