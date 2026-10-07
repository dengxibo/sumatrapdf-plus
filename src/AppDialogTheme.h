/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// Shared chrome for long-lived / themed dialogs: fonts, brushes, CTLCOLOR,
// DarkModeLib apply, and live RefreshAll when the toolbar moon toggles theme.

struct AppDialogFonts {
    HFONT body = nullptr;
    HFONT semibold = nullptr;

    void CreateForHwnd(HWND hwnd);
    // `dpi` is the window DPI to build fonts for. Prefer this over CreateForHwnd
    // during WM_DPICHANGED: GetDpiForWindow can still report the previous monitor.
    void CreateForDpi(int dpi);
    void Destroy();
};

struct AppDialogBrushes {
    HBRUSH background = nullptr;
    HBRUSH control = nullptr;

    void Create();
    void Destroy();
    void Recreate();
};

using AppDialogThemeRefreshFn = void (*)(HWND hwnd, void* ctx);

void RegisterAppDialogForTheme(HWND hwnd, AppDialogThemeRefreshFn fn, void* ctx);
void UnregisterAppDialogForTheme(HWND hwnd);
void RefreshAllAppDialogsTheme();

// DarkModeLib child theming + caption colors (safe when DarkModeLib is off).
void AppDialogApplyChrome(HWND hwnd);
// Shared annotation-sidebar panel layer and application font for native dialogs.
COLORREF AppDialogPanelBackgroundColor();
void AppDialogUseStandardControls(HWND hwnd);

// Warm chrome replaces paper-white Win11 push buttons and drop-down lists.
// Safe on other themes: it removes the custom paint when eye-care chrome is off.
void AppDialogSyncWarmPushButtons(HWND hwnd);

// Theme only the vertical scrollbar (Ask AI / InlineTranslate pattern). Keeps a
// blank or custom list face for Warm cream CTLCOLOR while adapting White /
// Warm / Dracula / Dark bars via Explorer::ScrollBar / DarkMode_Explorer::ScrollBar.
void AppDialogThemeScrollBar(HWND hwnd);

// For WM_CTLCOLOR*: sets text/bk colors and returns the brush to use, or null.
HBRUSH AppDialogCtlColorBrush(UINT msg, WPARAM wp, LPARAM lp, HBRUSH bgBrush, HBRUSH ctrlBrush = nullptr,
                              HWND editHwnd = nullptr);
bool AppDialogHandleEraseBkgnd(WPARAM wp, HWND hwnd, HBRUSH bgBrush);

void AppDialogApplyFontToChildren(HWND hwnd, HFONT font);
