/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct FindWindowWnd;

// The floating, movable/resizable variant of the find UI (see SearchUIFloating).
// Phase 1: search controls only; a results list is added in a later phase.
FindWindowWnd* CreateFindWindow(MainWindow* win);
void DeleteFindWindow(MainWindow* win);
void ShowFindWindow(MainWindow* win);
// Ctrl+F / toolbar search: show the find UI or raise the existing window without restarting search.
void FindWindowActivateForShortcut(MainWindow* win);
void HideFindWindow(MainWindow* win, bool keepSearchState = false);
bool IsFindWindowVisible(MainWindow* win);
bool IsFindWindowDocked(MainWindow* win);
HWND FindWindowHwnd(MainWindow* win);
void FindWindowSetStatus(MainWindow* win, const char* s);
void FindWindowSetStatusText(MainWindow* win, const char* s);
void FindWindowFlashStatusText(MainWindow* win, bool flash);
TempStr FindWindowGetStatusText(MainWindow* win);
void FindWindowSetSuppressTextChanged(MainWindow* win, bool suppress);
void FindWindowClearEditText(MainWindow* win);
void FindWindowResyncActiveEdit(MainWindow* win);
void FindWindowSetMatchCaseChecked(MainWindow* win, bool checked);
void FindWindowSetMatchWholeWordChecked(MainWindow* win, bool checked);
// repopulate the results list from win->findMatches (no-op if not visible).
// allowNavigation=false for streamed partial updates: don't navigate the
// document (navigation would cancel the in-flight count scan)
void FindWindowRefreshResults(MainWindow* win, bool allowNavigation = true);
void FindWindowApplyPendingNavigation(MainWindow* win);
int FindWindowPendingNavigationIndex(MainWindow* win);
// glyph budget for a one-line find-result snippet, based on results list width
int FindWindowSnippetGlyphBudget(MainWindow* win);
// re-apply theme colors/icons to the floating window after a theme change
void UpdateFindWindowTheme(MainWindow* win);
// keep the floating find window DPI/layout in sync when the frame moves or DPI changes
void FindWindowReposition(MainWindow* win);
// Switch the existing find window between its movable form and the compact
// top-right form. Both forms keep the same controls and search state.
void FindWindowSetDocked(MainWindow* win, bool docked);
// after RecreateFindBar() repoints hwndFindEdit at the hidden compact bar
void ResyncFloatingFindEdit(MainWindow* win);

// Headless draw test for issue #5736: match highlights must not bleed into the page column.
char* TestFindResultPageColumnClipResult(int* exitCodeOut = nullptr);
