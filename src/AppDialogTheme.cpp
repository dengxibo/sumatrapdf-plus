/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "utils/WinDynCalls.h"

#include "Settings.h"
#include "AppSettings.h"
#include "Theme.h"
#include "AppDialogTheme.h"
#include "DarkModeSubclass.h"
#include "resource.h"

#include <uxtheme.h>
#include <commctrl.h>

// Windows 11 push buttons stay paper-white. On Light-Warm that reads as a bright
// chip on the beige dialog; Light-White is already near white, so the same
// system button does not glare. Only eye-care chrome replaces the fill.
constexpr UINT_PTR kWarmPushButtonSubclassId = 0xA11B;
constexpr const wchar_t* kWarmPushButtonProp = L"SumatraWarmPushBtn";

struct WarmPushButtonState {
    bool hot = false;
};

static bool IsPushButton(HWND hwnd) {
    WCHAR cls[32]{};
    if (GetClassNameW(hwnd, cls, dimof(cls)) <= 0 || !str::EqI(cls, L"Button")) {
        return false;
    }
    UINT type = (UINT)GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    return type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
}

static void PaintWarmPushButton(HWND hwnd, HDC hdc, bool hot) {
    RECT wr{};
    GetClientRect(hwnd, &wr);
    if (wr.right <= wr.left || wr.bottom <= wr.top) {
        return;
    }
    // Fill the button's client (including round-rect corners) with the parent
    // page color so system chrome cannot leave a white rim.
    // Dark: Options / annotations page (BackgroundColor). Light-Warm sidebar:
    // control color so the button does not sit in a darker window-color halo.
    COLORREF page = ThemeWindowBackgroundColor();
    if (!ThemeUsesDarkChrome()) {
        if (HWND parent = GetParent(hwnd)) {
            WCHAR cls[64]{};
            if (GetClassNameW(parent, cls, dimof(cls)) > 0) {
                // Wingui pages and the signature dialog are caption-colored.
                // The round-rect corners must use that same face, or the old
                // window color shows through as a darker halo.
                if (str::EqI(cls, L"SumatraWgDefaultWinClass") ||
                    (ThemeUsesEyeCareChrome() && str::EqI(cls, L"SumatraHandwrittenSignature"))) {
                    page = ThemeChromeBackgroundColor();
                }
            }
        }
    }
    {
        ScopedGdiObj<HBRUSH> pageBr(CreateSolidBrush(page));
        FillRect(hdc, &wr, pageBr);
    }

    DWORD bst = (DWORD)SendMessageW(hwnd, BM_GETSTATE, 0, 0);
    bool pushed = (bst & BST_PUSHED) != 0;
    bool enabled = IsWindowEnabled(hwnd) != FALSE;
    COLORREF fill = ThemeWindowControlBackgroundColor();
    if (ThemeUsesDarkChrome()) {
        // Options OK/Cancel face: chrome control surface, slightly lifted when hot.
        fill = ThemeChromeBackgroundColor();
    }
    if (!enabled) {
        fill = page;
    } else if (pushed) {
        fill = AccentColor(fill, 8);
    } else if (hot && ThemeUsesDarkChrome()) {
        fill = AccentColor(fill, 0, 8);
    }
    int borderDelta = 32;
    if (hot || pushed) {
        borderDelta = 48;
    }
    UINT type = (UINT)GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK;
    LRESULT defId = SendMessageW(GetParent(hwnd), DM_GETDEFID, 0, 0);
    bool isDefault = type == BS_DEFPUSHBUTTON || (HIWORD(defId) == DC_HASDEFID && LOWORD(defId) == GetDlgCtrlID(hwnd));
    if (isDefault && enabled) {
        borderDelta = 56;
    }
    // Dark: Options edge (link tint). Light: quiet accent from the page.
    COLORREF border = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowLinkColor(), (hot || pushed) ? 0 : -20)
                                            : AccentColor(page, borderDelta);

    RECT box = wr;
    InflateRect(&box, -1, -1);
    int rad = DpiScale(hwnd, 8);
    {
        ScopedGdiObj<HBRUSH> fillBr(CreateSolidBrush(fill));
        ScopedGdiObj<HPEN> pen(CreatePen(PS_SOLID, 1, border));
        ScopedSelectObject selBr(hdc, fillBr);
        ScopedSelectObject selPen(hdc, pen);
        RoundRect(hdc, box.left, box.top, box.right, box.bottom, rad, rad);
    }

    WCHAR text[256]{};
    GetWindowTextW(hwnd, text, dimof(text));
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, enabled ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor());
    DWORD flags =
        (GetPropW(hwnd, L"AnnotLeftAligned") ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
    DWORD uiState = (DWORD)SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0);
    if ((uiState & UISF_HIDEACCEL) != 0) {
        flags |= DT_HIDEPREFIX;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_MULTILINE) != 0) {
        flags &= ~DT_SINGLELINE;
        flags |= DT_WORDBREAK;
    }
    InflateRect(&box, -DpiScale(hwnd, 4), -DpiScale(hwnd, 2));
    DrawTextW(hdc, text, -1, &box, flags);

    if (enabled && (bst & BST_FOCUS) != 0 && (uiState & UISF_HIDEFOCUS) == 0) {
        InflateRect(&box, -DpiScale(hwnd, 1), -DpiScale(hwnd, 1));
        DrawFocusRect(hdc, &box);
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
}

static LRESULT CALLBACK WarmPushButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* st = (WarmPushButtonState*)data;
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintWarmPushButton(hwnd, hdc, st && st->hot);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (st && !st->hot) {
                st->hot = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (st && st->hot) {
                st->hot = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, WarmPushButtonProc, id);
            RemovePropW(hwnd, kWarmPushButtonProp);
            free(st);
            return DefSubclassProc(hwnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void InstallWarmPushButton(HWND hwnd) {
    if (!IsPushButton(hwnd) || GetPropW(hwnd, kWarmPushButtonProp)) {
        return;
    }
    auto* st = AllocStruct<WarmPushButtonState>();
    if (!SetWindowSubclass(hwnd, WarmPushButtonProc, kWarmPushButtonSubclassId, (DWORD_PTR)st)) {
        free(st);
        return;
    }
    SetPropW(hwnd, kWarmPushButtonProp, (HANDLE)st);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void RemoveWarmPushButton(HWND hwnd) {
    auto* st = (WarmPushButtonState*)RemovePropW(hwnd, kWarmPushButtonProp);
    if (!st) {
        return;
    }
    RemoveWindowSubclass(hwnd, WarmPushButtonProc, kWarmPushButtonSubclassId);
    free(st);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void InstallWarmCombo(HWND hwnd);
constexpr const wchar_t* kWarmComboProp = L"SumatraWarmCombo";

static BOOL CALLBACK SyncWarmPushButton(HWND child, LPARAM warm) {
    // Dropdown faces share one painter in all four themes, including White.
    // Theme tokens supply the colors; only push-button treatment is Warm-only.
    InstallWarmCombo(child);
    if (warm) {
        InstallWarmPushButton(child);
    } else {
        if (GetPropW(child, kWarmPushButtonProp)) {
            RemoveWarmPushButton(child);
        }
    }
    return TRUE;
}

// Win11 drop-down lists stay paper-white, same as the push buttons. The closed
// face is themed in WM_PAINT, so a parent CTLCOLOR brush never reaches it.
constexpr UINT_PTR kWarmComboSubclassId = 0xA11C;

struct WarmComboState {
    bool hot = false;
    COLORREF brushCol = CLR_INVALID;
    HBRUSH brush = nullptr;
};

static bool IsDropDownList(HWND hwnd) {
    WCHAR cls[32]{};
    if (GetClassNameW(hwnd, cls, dimof(cls)) <= 0 || !str::EqI(cls, L"ComboBox")) {
        return false;
    }
    UINT type = (UINT)GetWindowLongPtrW(hwnd, GWL_STYLE) & CBS_DROPDOWNLIST;
    return type == CBS_DROPDOWNLIST || type == CBS_DROPDOWN;
}

static HBRUSH WarmComboBrush(WarmComboState* st, COLORREF col) {
    if (!st) {
        return nullptr;
    }
    if (st->brush && st->brushCol == col) {
        return st->brush;
    }
    DeleteObject(st->brush);
    st->brush = CreateSolidBrush(col);
    st->brushCol = col;
    return st->brush;
}

static COLORREF WarmComboPageColor(HWND hwnd) {
    COLORREF page = ThemeWindowBackgroundColor();
    // Dark: keep the Options / annotations page color so the purple edge sits
    // on the same surface as Note. Light-Warm sidebar uses control color.
    if (ThemeUsesDarkChrome()) {
        return page;
    }
    HWND parent = GetParent(hwnd);
    if (!parent) {
        return page;
    }
    WCHAR cls[64]{};
    if (GetClassNameW(parent, cls, dimof(cls)) > 0 && str::EqI(cls, L"SumatraWgDefaultWinClass")) {
        page = ThemeWindowControlBackgroundColor();
    }
    return page;
}

static void PaintWarmCombo(HWND hwnd, HDC hdc, bool hot) {
    RECT wr{};
    GetClientRect(hwnd, &wr);
    if (wr.right <= wr.left || wr.bottom <= wr.top) {
        return;
    }
    bool editable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWN;
    int savedDc = SaveDC(hdc);
    if (editable) {
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(hwnd, &info)) {
            // Leave typing, caret and selection painting to the native edit.
            ExcludeClipRect(hdc, info.rcItem.left, info.rcItem.top, info.rcItem.right, info.rcItem.bottom);
        }
    }
    COLORREF page = WarmComboPageColor(hwnd);
    {
        ScopedGdiObj<HBRUSH> pageBr(CreateSolidBrush(page));
        FillRect(hdc, &wr, pageBr);
    }

    bool dropped = SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;
    bool enabled = IsWindowEnabled(hwnd) != FALSE;
    bool focused = enabled && GetFocus() == hwnd;
    COLORREF fill = ThemeUsesDarkChrome() ? ThemeChromeBackgroundColor() : ThemeWindowControlBackgroundColor();
    if (!enabled) {
        fill = page;
    } else if (dropped) {
        fill = AccentColor(fill, 8);
    } else if (hot && ThemeUsesDarkChrome()) {
        fill = AccentColor(fill, 0, 8);
    }
    int borderDelta = 32;
    if (hot || dropped) {
        borderDelta = 48;
    }
    if (focused) {
        borderDelta = 56;
    }
    // Dark: same purple edge as Note / Options (DarkMode setEdgeColor).
    COLORREF border = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowLinkColor(), (hot || dropped || focused) ? 0 : -20)
                                            : AccentColor(page, borderDelta);

    RECT box = wr;
    InflateRect(&box, -1, -1);
    int rad = DpiScale(hwnd, 8);
    {
        ScopedGdiObj<HBRUSH> fillBr(CreateSolidBrush(fill));
        ScopedGdiObj<HPEN> pen(CreatePen(PS_SOLID, 1, border));
        ScopedSelectObject selBr(hdc, fillBr);
        ScopedSelectObject selPen(hdc, pen);
        RoundRect(hdc, box.left, box.top, box.right, box.bottom, rad, rad);
    }

    COLORREF ink = enabled ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor();
    int halfArrow = std::max(1, DpiScale(hwnd, 7) / 2);
    int aw = halfArrow * 2;
    int ax = box.right - DpiScale(hwnd, 10) - aw;
    {
        // Same font-rendered chevron as DarkModeLib's combo face.
        ScopedSelectObject arrowFont(hdc, GetWindowFont(hwnd));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ink);
        RECT arrowRc{ax - DpiScale(hwnd, 3), box.top, ax + aw + DpiScale(hwnd, 3), box.bottom};
        DrawTextW(hdc, L"\u02c5", -1, &arrowRc, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    WCHAR text[256]{};
    GetWindowTextW(hwnd, text, dimof(text));
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ink);
    RECT textRc = box;
    textRc.left += DpiScale(hwnd, 8);
    textRc.right = ax - DpiScale(hwnd, 4);
    if (editable) {
        // The child edit owns the value, not the combo face painter.
    } else if (GetWindowLongW(hwnd, GWL_STYLE) & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
        // Preserve owner-drawn values (e.g. annotation icon previews) on the
        // closed face too, instead of replacing them with plain window text.
        DRAWITEMSTRUCT item{};
        item.CtlType = ODT_COMBOBOX;
        item.CtlID = GetDlgCtrlID(hwnd);
        item.itemID = (UINT)SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
        item.itemAction = ODA_DRAWENTIRE;
        item.itemState = ODS_COMBOBOXEDIT | (enabled ? 0 : ODS_DISABLED);
        item.hwndItem = hwnd;
        item.hDC = hdc;
        item.rcItem = textRc;
        item.rcItem.left = box.left + 2;
        // Owner drawing fills its rectangle: keep it off the rounded rim.
        InflateRect(&item.rcItem, 0, -DpiScale(hwnd, 2));
        item.itemData = SendMessageW(hwnd, CB_GETITEMDATA, item.itemID, 0);
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
        SendMessageW(GetParent(hwnd), WM_DRAWITEM, item.CtlID, (LPARAM)&item);
        RestoreDC(hdc, saved);
    } else {
        DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (oldFont) {
        SelectObject(hdc, oldFont);
    }
    RestoreDC(hdc, savedDc);
}

static LRESULT CALLBACK WarmComboProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    auto* st = (WarmComboState*)data;
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintWarmCombo(hwnd, hdc, st && st->hot);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintWarmCombo(hwnd, (HDC)wp, st && st->hot);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX: {
            COLORREF fill = ThemeWindowControlBackgroundColor();
            SetTextColor((HDC)wp, ThemeWindowTextColor());
            SetBkColor((HDC)wp, fill);
            HBRUSH br = WarmComboBrush(st, fill);
            return (LRESULT)br;
        }
        case WM_MOUSEMOVE:
            if (st && !st->hot) {
                st->hot = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            if (st && st->hot) {
                st->hot = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, WarmComboProc, id);
            RemovePropW(hwnd, kWarmComboProp);
            if (st) {
                DeleteObject(st->brush);
                free(st);
            }
            return DefSubclassProc(hwnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void AppDialogThemeScrollBar(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    // Same as InlineTranslate ApplyEditTheme(rich): theme only the scrollbar
    // class so the list/edit face can stay on CTLCOLOR / blank theme.
    if (DynSetWindowTheme) {
        const WCHAR* scroll = ThemeUsesDarkChrome() ? L"DarkMode_Explorer::ScrollBar" : L"Explorer::ScrollBar";
        DynSetWindowTheme(hwnd, nullptr, scroll);
    }
    if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
        DarkMode::setDarkScrollBar(hwnd);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void ThemeWarmComboListScrollBar(HWND combo) {
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(combo, &info) || !info.hwndList) {
        return;
    }
    // Blank the list face so Warm CTLCOLOR cream shows through, then put back
    // a themed scrollbar only (full Explorer theme would paint a white slab).
    SetWindowTheme(info.hwndList, L" ", L" ");
    AppDialogThemeScrollBar(info.hwndList);
    LONG_PTR style = GetWindowLongPtrW(info.hwndList, GWL_STYLE);
    if ((style & WS_VSCROLL) == 0) {
        SetWindowLongPtrW(info.hwndList, GWL_STYLE, style | WS_VSCROLL);
        SetWindowPos(info.hwndList, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

static void InstallWarmCombo(HWND hwnd) {
    if (!IsDropDownList(hwnd)) {
        return;
    }
    if (auto* existing = (WarmComboState*)GetPropW(hwnd, kWarmComboProp)) {
        // Theme refresh may have put another painter above us. Reinstall at
        // the top so all controls on the page keep the same final painter.
        RemoveWindowSubclass(hwnd, WarmComboProc, kWarmComboSubclassId);
        SetWindowSubclass(hwnd, WarmComboProc, kWarmComboSubclassId, (DWORD_PTR)existing);
        ThemeWarmComboListScrollBar(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }
    // Configure the separate list window once. Changing its theme from
    // WM_CTLCOLORLISTBOX triggers another repaint and makes hovered rows flicker.
    ThemeWarmComboListScrollBar(hwnd);
    auto* st = AllocStruct<WarmComboState>();
    if (!SetWindowSubclass(hwnd, WarmComboProc, kWarmComboSubclassId, (DWORD_PTR)st)) {
        free(st);
        return;
    }
    SetPropW(hwnd, kWarmComboProp, (HANDLE)st);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void AppDialogSyncWarmPushButtons(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    // Light-Warm: replace paper-white Win11 chips. Dark: replace the white
    // system push-button rim (Delete / Export / Save / Options OK).
    bool paintPush = ThemeUsesEyeCareChrome() || ThemeUsesDarkChrome();
    EnumChildWindows(hwnd, SyncWarmPushButton, paintPush ? 1 : 0);
}

void AppDialogFonts::CreateForHwnd(HWND hwnd) {
    CreateForDpi(DpiGetForHwnd(hwnd));
}

void AppDialogFonts::CreateForDpi(int dpi) {
    Destroy();
    if (dpi <= 0) {
        dpi = 96;
    }
    LOGFONTW lf{};
    if (GetObjectW(GetAppFontForDpi(dpi), sizeof(lf), &lf) != sizeof(lf)) {
        return;
    }
    lf.lfWeight = FW_NORMAL;
    body = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    semibold = CreateFontIndirectW(&lf);
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
    background = CreateSolidBrush(AppDialogPanelBackgroundColor());
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

COLORREF AppDialogPanelBackgroundColor() {
    return ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
}

struct StandardDialogControls {
    AppDialogFonts fonts;
    HBRUSH panel = nullptr;
};

static LRESULT CALLBACK StandardDialogControlsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                   DWORD_PTR refData) {
    auto data = (StandardDialogControls*)refData;
    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
        SetTextColor((HDC)wp, ThemeWindowTextColor());
        SetBkColor((HDC)wp, AppDialogPanelBackgroundColor());
        return (LRESULT)data->panel;
    }
    if (msg == WM_ERASEBKGND) {
        return AppDialogHandleEraseBkgnd(wp, hwnd, data->panel);
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, StandardDialogControlsProc, id);
        RemovePropW(hwnd, L"SumatraStandardDialogControls");
        UnregisterAppDialogForTheme(hwnd);
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        data->fonts.Destroy();
        DeleteObject(data->panel);
        delete data;
        return result;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void AppDialogUseStandardControls(HWND hwnd) {
    auto data = (StandardDialogControls*)GetPropW(hwnd, L"SumatraStandardDialogControls");
    if (!data) {
        data = new StandardDialogControls();
        data->fonts.CreateForHwnd(hwnd);
        SetPropW(hwnd, L"SumatraStandardDialogControls", (HANDLE)data);
        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM ctx) -> BOOL {
                auto fonts = (AppDialogFonts*)ctx;
                LOGFONTW lf{};
                HFONT current = GetWindowFont(child);
                GetObjectW(current, sizeof(lf), &lf);
                HFONT font = lf.lfWeight >= FW_SEMIBOLD ? fonts->semibold : fonts->body;
                if (font) SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
                return TRUE;
            },
            (LPARAM)&data->fonts);
        bool registered = false;
        for (auto& entry : gAppDialogThemeEntries) {
            if (entry.hwnd == hwnd) registered = true;
        }
        if (!registered) {
            RegisterAppDialogForTheme(
                hwnd,
                [](HWND dialog, void*) {
                    AppDialogApplyChrome(dialog);
                    RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
                },
                nullptr);
        }
    }
    DeleteObject(data->panel);
    data->panel = CreateSolidBrush(AppDialogPanelBackgroundColor());
    RemoveWindowSubclass(hwnd, StandardDialogControlsProc, 0xA121);
    SetWindowSubclass(hwnd, StandardDialogControlsProc, 0xA121, (DWORD_PTR)data);
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
    AppDialogSyncWarmPushButtons(hwnd);
    if (GetPropW(hwnd, L"SumatraStandardDialogControls")) {
        AppDialogUseStandardControls(hwnd);
    }
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
    bool recessed = (msg == WM_CTLCOLOREDIT && ctrlBrush) || (editHwnd && ctrl == editHwnd && ctrlBrush) ||
                    (msg == WM_CTLCOLORLISTBOX && ctrlBrush);
    SetTextColor(dc, ThemeWindowTextColor());
    SetBkColor(dc, recessed ? ThemeWindowControlBackgroundColor() : AppDialogPanelBackgroundColor());
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
