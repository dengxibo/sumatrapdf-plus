/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "Settings.h"
#include "AppSettings.h"
#include "Theme.h"
#include "AppDialogTheme.h"
#include "DarkModeSubclass.h"

void AppDialogFonts::CreateForHwnd(HWND hwnd) {
    CreateForDpi(DpiGetForHwnd(hwnd));
}

void AppDialogFonts::CreateForDpi(int dpi) {
    Destroy();
    if (dpi <= 0) {
        dpi = 96;
    }
    NONCLIENTMETRICS ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!GetNonClientMetricsForDpi(dpi, &ncm)) {
        return;
    }
    int pt = IsAppFontSizeDefault() ? 8 : (GetAppFontSize() * 72) / 96;
    ncm.lfMessageFont.lfHeight = -MulDiv(pt, dpi, 72);
    wcscpy_s(ncm.lfMessageFont.lfFaceName, L"MS Shell Dlg");
    ncm.lfMessageFont.lfWeight = FW_NORMAL;
    body = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    semibold = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!body || !semibold) {
        Destroy();
    }
}

void AppDialogFonts::Destroy() {
    DeleteObject(body);
    DeleteObject(semibold);
    body = semibold = nullptr;
}

void AppDialogBrushes::Create() {
    Destroy();
    background = CreateSolidBrush(ThemeWindowBackgroundColor());
    control = CreateSolidBrush(ThemeWindowControlBackgroundColor());
}

void AppDialogBrushes::Destroy() {
    DeleteObject(background);
    DeleteObject(control);
    background = control = nullptr;
}

void AppDialogBrushes::Recreate() {
    Create();
}

struct AppDialogThemeEntry {
    HWND hwnd = nullptr;
    AppDialogThemeRefreshFn fn = nullptr;
    void* ctx = nullptr;
};

static Vec<AppDialogThemeEntry> gAppDialogThemeEntries;

void RegisterAppDialogForTheme(HWND hwnd, AppDialogThemeRefreshFn fn, void* ctx) {
    if (!hwnd || !fn) {
        return;
    }
    for (int i = 0; i < gAppDialogThemeEntries.Size(); i++) {
        if (gAppDialogThemeEntries[i].hwnd == hwnd) {
            gAppDialogThemeEntries[i].fn = fn;
            gAppDialogThemeEntries[i].ctx = ctx;
            return;
        }
    }
    AppDialogThemeEntry e;
    e.hwnd = hwnd;
    e.fn = fn;
    e.ctx = ctx;
    gAppDialogThemeEntries.Append(e);
}

void UnregisterAppDialogForTheme(HWND hwnd) {
    for (int i = gAppDialogThemeEntries.Size() - 1; i >= 0; i--) {
        if (gAppDialogThemeEntries[i].hwnd == hwnd) {
            gAppDialogThemeEntries.RemoveAt(i);
        }
    }
}

void RefreshAllAppDialogsTheme() {
    Vec<AppDialogThemeEntry> snap = gAppDialogThemeEntries;
    for (int i = 0; i < snap.Size(); i++) {
        HWND hwnd = snap[i].hwnd;
        if (!hwnd || !IsWindow(hwnd)) {
            UnregisterAppDialogForTheme(hwnd);
            continue;
        }
        snap[i].fn(hwnd, snap[i].ctx);
    }
}

void AppDialogApplyChrome(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    if (UseDarkModeLib()) {
        // setDarkWndSafe installs WindowCtlColorSubclass — required for
        // WM_CTLCOLORLISTBOX (Settings category list stays white without it).
        if (ThemeUsesDarkChrome()) {
            DarkMode::setDarkWndSafe(hwnd);
        } else {
            DarkMode::setChildCtrlsSubclassAndTheme(hwnd);
            DarkMode::setWindowEraseBgSubclass(hwnd);
        }
    }
    UpdateWindowCaptionTheme(hwnd);
}

HBRUSH AppDialogCtlColorBrush(UINT msg, WPARAM wp, LPARAM lp, HBRUSH bgBrush, HBRUSH ctrlBrush, HWND editHwnd) {
    if (msg != WM_CTLCOLORDLG && msg != WM_CTLCOLORSTATIC && msg != WM_CTLCOLORBTN && msg != WM_CTLCOLOREDIT &&
        msg != WM_CTLCOLORLISTBOX) {
        return nullptr;
    }
    if (!bgBrush) {
        return nullptr;
    }
    // Dark chrome: DarkModeLib's WindowCtlColorSubclass owns CTLCOLOR*; returning
    // our brushes here would fight listbox/combo painting.
    if (ThemeUsesDarkChrome() && UseDarkModeLib()) {
        return nullptr;
    }
    HDC dc = (HDC)wp;
    HWND ctrl = (HWND)lp;
    bool recessed = (editHwnd && ctrl == editHwnd && ctrlBrush) || (msg == WM_CTLCOLORLISTBOX && ctrlBrush);
    SetTextColor(dc, ThemeWindowTextColor());
    SetBkColor(dc, recessed ? ThemeWindowControlBackgroundColor() : ThemeWindowBackgroundColor());
    return recessed ? ctrlBrush : bgBrush;
}

bool AppDialogHandleEraseBkgnd(WPARAM wp, HWND hwnd, HBRUSH bgBrush) {
    if (!bgBrush || !hwnd) {
        return false;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect((HDC)wp, &rc, bgBrush);
    return true;
}

void AppDialogApplyFontToChildren(HWND hwnd, HFONT font) {
    if (!hwnd || !font) {
        return;
    }
    EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM fontLp) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)fontLp, TRUE);
            return TRUE;
        },
        (LPARAM)font);
}
