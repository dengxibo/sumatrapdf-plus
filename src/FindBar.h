/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct FindBarWnd;

// Chrome-style floating search bar. Created on first Ctrl+F (or toolbar search);
// owns win->hwndFindEdit while open. Destroyed on tab switch / Esc / close.
FindBarWnd* CreateFindBar(MainWindow* win);
void DeleteFindBar(MainWindow* win);
void RecreateFindBar(MainWindow* win);
void ShowFindBar(MainWindow* win);
void HideFindBar(MainWindow* win, bool keepSearchState = false);
bool IsFindBarVisible(MainWindow* win);
// true if either the compact bar or the floating find window is visible
bool IsFindUIVisible(MainWindow* win);
// true if hwnd is the compact find bar, floating find window, or a child of either
bool IsFindUIHwnd(MainWindow* win, HWND hwnd);
// reposition over the search toolbar icon (no-op if not visible)
void FindBarReposition(MainWindow* win);
// show n/m or "No matches" style status in the bar
void FindBarSetStatus(MainWindow* win, const char* s);
TempStr FindUIGetStatusText(MainWindow* win);
void FindBarBeginStatusCompleteFlash(MainWindow* win);
void FindStatusCompleteFlashTimerFired(MainWindow* win);
// refresh the n/m (or in-progress n+) status on whichever find UI is visible
void RefreshFindUIStatus(MainWindow* win);
// reflect match-case toggle state on the bar's button
void FindBarSetMatchCaseChecked(MainWindow* win, bool checked);
// reflect match-whole-word toggle state on the bar's button
void FindBarSetMatchWholeWordChecked(MainWindow* win, bool checked);

// switch the find UI between the compact toolbar overlay and the floating
// window (persists the choice in gGlobalPrefs->searchUIFloating)
void ToggleFloatingFindUI(MainWindow* win);
// destroy find UI windows and drop HWND references (e.g. tab switch). Does not
// clear cached match data; pair with ResetFindUIForTabSwitch for a full reset.
void DestroyFindUI(MainWindow* win);
void FindBarResyncActiveEdit(MainWindow* win);
// move keyboard focus off hidden find UI (e.g. after tab switch during a scan)
void StealFocusFromFindUI(MainWindow* win);
// focus the find edit and select all text (Ctrl+F when find UI is already open)
void FocusFindEditSelectAll(MainWindow* win);
// handle Enter in the find edit (subclass proc; also swallows WM_CHAR to avoid beep)
void InstallFindEditKeyboardHandler(MainWindow* win, HWND hwndEdit);
