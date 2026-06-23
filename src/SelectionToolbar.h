/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

// Show the floating selection toolbar for the current text selection.
// Does nothing if the feature is disabled, the engine doesn't support
// annotations, or there is no real text selection.
void ShowSelectionToolbar(MainWindow* win);

// Reposition the toolbar so it keeps following the selection (called from
// the canvas paint routine). Hides it if the selection scrolled out of view
// or the current tab changed.
void UpdateSelectionToolbarPosition(MainWindow* win);

// Hide the toolbar but keep it around for reuse.
void HideSelectionToolbar(MainWindow* win);

// Destroy the toolbar window and free its state.
void DeleteSelectionToolbar(MainWindow* win);
