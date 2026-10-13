/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "utils/WinDynCalls.h"

#include "Settings.h"
#include "Translations.h"
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
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            if (st && !st->hot) {
                st->hot = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
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

constexpr UINT_PTR kWarmEditSubclassId = 0xA11D;

static COLORREF WarmComboPageColor(HWND hwnd);

static bool IsSingleLineEdit(HWND hwnd) {
    WCHAR cls[32]{};
    if (GetClassNameW(hwnd, cls, dimof(cls)) <= 0 || !str::EqI(cls, L"Edit")) {
        return false;
    }
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_MULTILINE) {
        return false;
    }
    HWND parent = GetParent(hwnd);
    WCHAR parentCls[32]{};
    if (parent && GetClassNameW(parent, parentCls, dimof(parentCls)) > 0 && str::EqI(parentCls, L"ComboBox")) {
        return false;
    }
    return true;
}

static void PaintWarmEditChrome(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    COLORREF page = WarmComboPageColor(hwnd);
    COLORREF border = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowLinkColor(), GetFocus() == hwnd ? 0 : -20)
                                            : AccentColor(page, GetFocus() == hwnd ? 56 : 32);
    COLORREF fill = ThemeUsesDarkChrome() ? ThemeChromeBackgroundColor() : ThemeWindowControlBackgroundColor();
    if (!IsWindowEnabled(hwnd)) {
        fill = page;
    }
    {
        ScopedGdiObj<HBRUSH> pageBr(CreateSolidBrush(page));
        FillRect(dc, &rc, pageBr);
    }
    int rad = DpiScale(hwnd, 8);
    int h = rc.bottom - rc.top;
    if (rad > h - 2) {
        rad = std::max(2, h - 2);
    }
    ScopedGdiObj<HBRUSH> fillBr(CreateSolidBrush(fill));
    ScopedGdiObj<HPEN> pen(CreatePen(PS_SOLID, 1, border));
    ScopedSelectObject selBr(dc, fillBr);
    ScopedSelectObject selPen(dc, pen);
    // A 1px non-client ring clips the corner arc, so light themes looked square.
    // Paint the same rounded face as the combos in the client.
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, rad, rad);
}

static LRESULT CALLBACK WarmEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    constexpr const wchar_t* kPrinting = L"SumatraWarmEditPrinting";
    switch (msg) {
        case WM_NCCALCSIZE:
            // Frame is drawn in the client. A 1px non-client inset made the
            // corner arc fall outside the visible border in light themes.
            return DefSubclassProc(hwnd, msg, wp, lp);
        case WM_NCPAINT:
            return 0;
        case WM_ERASEBKGND:
            PaintWarmEditChrome(hwnd, (HDC)wp);
            return 1;
        case WM_PAINT: {
            if (GetPropW(hwnd, kPrinting)) {
                break;
            }
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintWarmEditChrome(hwnd, hdc);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int rad = DpiScale(hwnd, 8);
            int h = rc.bottom - rc.top;
            if (rad > h - 2) {
                rad = std::max(2, h - 2);
            }
            RECT inner = rc;
            InflateRect(&inner, -2, -2);
            int innerRad = std::max(2, rad - 2);
            if (inner.right > inner.left && inner.bottom > inner.top) {
                HRGN rgn =
                    CreateRoundRectRgn(inner.left, inner.top, inner.right + 1, inner.bottom + 1, innerRad, innerRad);
                int saved = SaveDC(hdc);
                SelectClipRgn(hdc, rgn);
                SetPropW(hwnd, kPrinting, (HANDLE)1);
                SendMessageW(hwnd, WM_PRINTCLIENT, (WPARAM)hdc, PRF_CLIENT);
                RemovePropW(hwnd, kPrinting);
                RestoreDC(hdc, saved);
                DeleteObject(rgn);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, WarmEditProc, id);
            return DefSubclassProc(hwnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void InstallWarmEdit(HWND hwnd) {
    if (!IsSingleLineEdit(hwnd)) {
        return;
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~WS_BORDER;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!GetPropW(hwnd, L"SumatraWarmEdit")) {
        if (SetWindowSubclass(hwnd, WarmEditProc, kWarmEditSubclassId, 0)) {
            SetPropW(hwnd, L"SumatraWarmEdit", (HANDLE)1);
        }
    }
    int pad = DpiScale(hwnd, 6);
    SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad, pad));
}

static BOOL CALLBACK SyncWarmPushButton(HWND child, LPARAM warm) {
    // Dropdown faces share one painter in all four themes, including White.
    // Theme tokens supply the colors; only push-button treatment is Warm-only.
    InstallWarmCombo(child);
    InstallWarmEdit(child);
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

static void WarmComboClosedFaceRect(HWND hwnd, RECT* out);

// Closed-face chevron, shared by the painter and the editable inset. The list
// combos have no child edit, so this rect is the whole arrow. An editable
// combo's edit must end at textRight or it paints over the left of the same v.
struct WarmComboFace {
    RECT box{};
    RECT arrow{};
    int textRight = 0;
};

static bool WarmComboFaceMetrics(HWND hwnd, WarmComboFace* out) {
    RECT face{};
    WarmComboClosedFaceRect(hwnd, &face);
    if (face.right <= face.left + DpiScale(hwnd, 24) || face.bottom <= face.top) {
        return false;
    }
    // A not-yet-shown combo reports the dropped-list height. Don't lock the
    // edit to that, or the closed face never gets a real chevron gap.
    if (face.bottom - face.top > DpiScale(hwnd, 48)) {
        return false;
    }
    RECT box = face;
    InflateRect(&box, -1, -1);
    int halfArrow = std::max(1, DpiScale(hwnd, 7) / 2);
    int aw = halfArrow * 2;
    int ax = box.right - DpiScale(hwnd, 10) - aw;
    out->box = box;
    out->arrow = RECT{ax - DpiScale(hwnd, 3), box.top, ax + aw + DpiScale(hwnd, 3), box.bottom};
    out->textRight = ax - DpiScale(hwnd, 4);
    return out->textRight > box.left + DpiScale(hwnd, 12);
}

static bool DesiredComboEditRect(HWND combo, RECT* out) {
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(combo, &info) || info.rcButton.bottom <= 0) {
        return false;
    }
    WarmComboFace face{};
    if (!WarmComboFaceMetrics(combo, &face)) {
        return false;
    }
    // Same closed face as a dropdown list. The edit fills only the text, and
    // stops where that list's text stops, so it cannot cover the chevron.
    int inset = std::max(2, DpiScale(combo, 2));
    out->left = inset;
    out->top = inset;
    out->right = face.textRight;
    out->bottom = (int)info.rcButton.bottom - inset;
    return out->right > out->left + DpiScale(combo, 12) && out->bottom > out->top + DpiScale(combo, 8);
}

static LRESULT CALLBACK WarmComboEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    // The combo face already draws the rounded rim. The inner edit's own
    // non-client border is the broken left edge on editable combos.
    if (msg == WM_NCPAINT) {
        return 0;
    }
    // The combo puts the edit back over the stroke and the chevron after we
    // move it. Clamp every position change.
    if (msg == WM_WINDOWPOSCHANGING) {
        auto* pos = (WINDOWPOS*)lp;
        RECT want{};
        if (pos && !(pos->flags & SWP_HIDEWINDOW) && DesiredComboEditRect(GetParent(hwnd), &want)) {
            pos->x = want.left;
            pos->y = want.top;
            pos->cx = want.right - want.left;
            pos->cy = want.bottom - want.top;
            pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void AppDialogInsetComboEdit(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & CBS_DROPDOWNLIST) != CBS_DROPDOWN) {
        return;
    }
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(hwnd, &info) || !info.hwndItem || info.rcButton.right <= info.rcButton.left) {
        return;
    }
    HWND edit = info.hwndItem;
    // The native edit covers the rounded stroke. Dropdown-list faces have no
    // child edit, which is why only these combos lose the left edge.
    RECT want{};
    if (!DesiredComboEditRect(hwnd, &want)) {
        return;
    }
    int left = want.left;
    int top = want.top;
    int right = want.right;
    int bottom = want.bottom;
    RECT cur{};
    GetWindowRect(edit, &cur);
    MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&cur, 2);
    bool placed = cur.left == left && cur.top == top && cur.right == right && cur.bottom == bottom;
    DWORD_PTR unused = 0;
    BOOL subclassed = GetWindowSubclass(edit, WarmComboEditProc, kWarmComboSubclassId, &unused);
    if (placed && subclassed) {
        return;
    }
    if (!subclassed) {
        if (DynSetWindowTheme) {
            DynSetWindowTheme(edit, L"", L"");
        }
        LONG_PTR ex = GetWindowLongPtrW(edit, GWL_EXSTYLE);
        ex &= ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
        SetWindowLongPtrW(edit, GWL_EXSTYLE, ex);
        SetWindowSubclass(edit, WarmComboEditProc, kWarmComboSubclassId, 0);
        // Closed list text starts DpiScale(8) from the face. The edit is already inset.
        int pad = std::max(0, DpiScale(hwnd, 8) - left);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad, DpiScale(hwnd, 4)));
    }
    if (!placed) {
        SetWindowPos(edit, nullptr, left, top, right - left, bottom - top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

static void AlignComboEditText(HWND hwnd) {
    AppDialogInsetComboEdit(hwnd);
}

static void WarmComboClosedFaceRect(HWND hwnd, RECT* out) {
    GetClientRect(hwnd, out);
    // Settings layout sizes ComboBox windows to their dropped height and clips
    // the closed face with SetWindowRgn. Paint only that face, not the tall
    // client, or hover redraws look like the control is shaking.
    // Use the button's outer edge. rcItem follows an inset edit and would
    // pull the rounded face inward, which clips the left corner.
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(hwnd, &info) || info.rcButton.right <= 0 || info.rcButton.bottom <= 0) {
        return;
    }
    out->left = 0;
    out->top = 0;
    out->right = info.rcButton.right;
    out->bottom = info.rcButton.bottom;
}

static void PaintWarmCombo(HWND hwnd, HDC hdc, bool hot) {
    RECT wr{};
    WarmComboClosedFaceRect(hwnd, &wr);
    if (wr.right <= wr.left || wr.bottom <= wr.top) {
        return;
    }
    bool editable = (GetWindowLongPtrW(hwnd, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWN;
    int savedDc = SaveDC(hdc);
    if (editable) {
        COMBOBOXINFO info{};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(hwnd, &info) && info.hwndItem) {
            // Clip the edit window, not rcItem. rcItem includes the left stroke.
            RECT er{};
            GetWindowRect(info.hwndItem, &er);
            MapWindowPoints(HWND_DESKTOP, hwnd, (POINT*)&er, 2);
            ExcludeClipRect(hdc, er.left, er.top, er.right, er.bottom);
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
    // Light themes: ignore hover for the closed face. Border-delta changes on
    // enter/leave were the remaining flash after the leave-loop fix.
    bool hotVisual = hot && ThemeUsesDarkChrome();
    COLORREF fill = ThemeUsesDarkChrome() ? ThemeChromeBackgroundColor() : ThemeWindowControlBackgroundColor();
    if (!enabled) {
        fill = page;
    } else if (dropped) {
        fill = AccentColor(fill, 8);
    } else if (hotVisual) {
        fill = AccentColor(fill, 0, 8);
    }
    int borderDelta = 32;
    if (hotVisual || dropped) {
        borderDelta = 48;
    }
    if (focused) {
        borderDelta = 56;
    }
    // Dark: same purple edge as Note / Options (DarkMode setEdgeColor).
    COLORREF border = ThemeUsesDarkChrome()
                          ? AccentColor(ThemeWindowLinkColor(), (hotVisual || dropped || focused) ? 0 : -20)
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
    WarmComboFace face{};
    if (!WarmComboFaceMetrics(hwnd, &face)) {
        face.box = box;
        face.arrow = box;
        face.textRight = box.right - DpiScale(hwnd, 22);
    }
    {
        // Same chevron, same rect, as the dropdown-list faces.
        ScopedSelectObject arrowFont(hdc, GetWindowFont(hwnd));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, ink);
        DrawTextW(hdc, L"\u02c5", -1, &face.arrow, DT_NOPREFIX | DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    WCHAR text[256]{};
    GetWindowTextW(hwnd, text, dimof(text));
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ink);
    RECT textRc = box;
    textRc.left += DpiScale(hwnd, 8);
    textRc.right = face.textRight;
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
        case WM_SIZE:
        case WM_SETFONT:
        case WM_THEMECHANGED:
        case CB_SETITEMHEIGHT: {
            LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
            AlignComboEditText(hwnd);
            return result;
        }
        case WM_PARENTNOTIFY:
            if (LOWORD(wp) == WM_CREATE) {
                // SetWindowTheme recreates the edit after the first inset.
                AlignComboEditText(hwnd);
            }
            break;
        case WM_PAINT: {
            // First show lays the edit out before the arrow button exists, so
            // the edit covers the chevron. Theme switches redo this after the
            // button is real; do the same before the first paint.
            AppDialogInsetComboEdit(hwnd);
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT face{};
            WarmComboClosedFaceRect(hwnd, &face);
            int fw = face.right - face.left;
            int fh = face.bottom - face.top;
            if (fw > 0 && fh > 0) {
                HDC mem = CreateCompatibleDC(hdc);
                HBITMAP bmp = CreateCompatibleBitmap(hdc, fw, fh);
                HGDIOBJ oldBmp = SelectObject(mem, bmp);
                SetWindowOrgEx(mem, face.left, face.top, nullptr);
                PaintWarmCombo(hwnd, mem, st && st->hot);
                BitBlt(hdc, face.left, face.top, fw, fh, mem, face.left, face.top, SRCCOPY);
                SelectObject(mem, oldBmp);
                DeleteObject(bmp);
                DeleteDC(mem);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintWarmCombo(hwnd, (HDC)wp, st && st->hot);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCPAINT:
            // Our WM_PAINT face is the border. The unthemed combo frame is a
            // second, square rim and is what the screenshots mark as broken.
            return 0;
        case WM_CTLCOLOREDIT: {
            COLORREF fill = ThemeUsesDarkChrome() ? ThemeChromeBackgroundColor() : ThemeWindowControlBackgroundColor();
            SetTextColor((HDC)wp, IsWindowEnabled(hwnd) ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor());
            SetBkColor((HDC)wp, fill);
            HBRUSH br = WarmComboBrush(st, fill);
            return (LRESULT)br;
        }
        case WM_CTLCOLORLISTBOX: {
            COLORREF fill = ThemeWindowControlBackgroundColor();
            SetTextColor((HDC)wp, ThemeWindowTextColor());
            SetBkColor((HDC)wp, fill);
            HBRUSH br = WarmComboBrush(st, fill);
            return (LRESULT)br;
        }
        case WM_MOUSEMOVE: {
            // TrackMouseEvent cancels on the next mouse message. Re-arm every
            // move or we get leave→enter loops and the closed face shakes.
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            if (st && !st->hot) {
                st->hot = true;
                // Light themes keep a constant closed face; only dark paints hover.
                if (ThemeUsesDarkChrome()) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            // Native combo paints its own hot face from mouse-move; skip it so
            // our closed face is the only paint.
            return 0;
        }
        case WM_MOUSELEAVE:
            if (st && st->hot) {
                st->hot = false;
                if (ThemeUsesDarkChrome()) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
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

static void PaintComboList(HWND list, HWND combo, HDC dc) {
    RECT client{};
    GetClientRect(list, &client);
    int w = client.right - client.left;
    int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) {
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    ScopedGdiObj<HBRUSH> background(CreateSolidBrush(ThemeWindowControlBackgroundColor()));
    FillRect(mem, &client, background);
    ScopedSelectObject font(mem, GetWindowFont(combo));
    SetBkMode(mem, TRANSPARENT);
    int count = (int)SendMessageW(list, LB_GETCOUNT, 0, 0);
    int selected = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    int top = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    int pad = DpiScale(combo, 8);
    for (int i = std::max(0, top); i < count; i++) {
        RECT row{};
        if (SendMessageW(list, LB_GETITEMRECT, i, (LPARAM)&row) == LB_ERR || row.top >= client.bottom) {
            break;
        }
        if (row.bottom <= client.top) {
            continue;
        }
        bool highlight = i == selected;
        if (highlight) {
            FillRect(mem, &row, GetSysColorBrush(COLOR_HIGHLIGHT));
        }
        int length = (int)SendMessageW(list, LB_GETTEXTLEN, i, 0);
        if (length < 0) {
            continue;
        }
        ScopedMem<WCHAR> text(AllocArray<WCHAR>((size_t)length + 1));
        if (SendMessageW(list, LB_GETTEXT, i, (LPARAM)text.Get()) == LB_ERR) {
            continue;
        }
        SetTextColor(mem, highlight ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ThemeWindowTextColor());
        RECT textRc = row;
        textRc.left += pad;
        textRc.right -= pad;
        DrawTextW(mem, text.Get(), length, &textRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK ComboListProc(HWND list, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    HWND combo = (HWND)data;
    bool ownerDraw = (GetWindowLongPtrW(combo, GWL_STYLE) & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0;
    if (!ownerDraw && msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(list, &ps);
        PaintComboList(list, combo, dc);
        EndPaint(list, &ps);
        return 0;
    }
    if (!ownerDraw && msg == WM_PRINTCLIENT) {
        PaintComboList(list, combo, (HDC)wp);
        return 0;
    }
    if (!ownerDraw && msg == WM_ERASEBKGND) {
        return 1;
    }
    // Hover/keyboard tracking: freeze native painting, then paint our face only
    // when the selection or scroll position actually changed.
    if (!ownerDraw && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_KEYDOWN ||
                       msg == WM_MOUSEWHEEL || msg == WM_VSCROLL || msg == LB_SETCURSEL || msg == LB_SETTOPINDEX)) {
        int selBefore = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
        int topBefore = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        LRESULT result = DefSubclassProc(list, msg, wp, lp);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        int selAfter = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
        int topAfter = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        if (topAfter != topBefore) {
            InvalidateRect(list, nullptr, FALSE);
            UpdateWindow(list);
        } else if (selAfter != selBefore) {
            // Only the old and new rows change; avoid flashing the whole popup.
            RECT dirty{};
            bool have = false;
            RECT row{};
            if (selBefore >= 0 && SendMessageW(list, LB_GETITEMRECT, selBefore, (LPARAM)&row) != LB_ERR) {
                dirty = row;
                have = true;
            }
            if (selAfter >= 0 && SendMessageW(list, LB_GETITEMRECT, selAfter, (LPARAM)&row) != LB_ERR) {
                if (have) {
                    UnionRect(&dirty, &dirty, &row);
                } else {
                    dirty = row;
                    have = true;
                }
            }
            if (have) {
                InvalidateRect(list, &dirty, FALSE);
                UpdateWindow(list);
            }
        }
        return result;
    }
    LRESULT result = DefSubclassProc(list, msg, wp, lp);
    if (msg == WM_NCPAINT) {
        // Draw every edge explicitly; the unthemed popup's native left rim
        // otherwise blends into the page behind it in the light themes.
        HDC dc = GetWindowDC(list);
        RECT border{};
        GetWindowRect(list, &border);
        OffsetRect(&border, -border.left, -border.top);
        COLORREF color = ThemeUsesDarkChrome() ? AccentColor(ThemeWindowLinkColor(), -20)
                                               : AccentColor(ThemeWindowControlBackgroundColor(), -48);
        ScopedGdiObj<HBRUSH> brush(CreateSolidBrush(color));
        FrameRect(dc, &border, brush);
        ReleaseDC(list, dc);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(list, ComboListProc, id);
    }
    return result;
}

static void ThemeWarmComboListScrollBar(HWND combo) {
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(combo, &info) || !info.hwndList) {
        return;
    }
    RemoveWindowSubclass(info.hwndList, ComboListProc, kWarmComboSubclassId);
    SetWindowSubclass(info.hwndList, ComboListProc, kWarmComboSubclassId, (DWORD_PTR)combo);
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
    // Short lists (OCR detail, voice mode) do not need a bar. Leaving WS_VSCROLL
    // visible reserves gutter space and the popup width jumps while hovering.
    int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    int minVis = (int)SendMessageW(combo, CB_GETMINVISIBLE, 0, 0);
    if (minVis <= 0) {
        minVis = 30;
    }
    ShowScrollBar(info.hwndList, SB_VERT, count > minVis);
}

static void InstallWarmCombo(HWND hwnd) {
    if (!IsDropDownList(hwnd)) {
        return;
    }
    AlignComboEditText(hwnd);
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
    bool registerForTheme = true;
    AppDialogFonts fonts;
    HBRUSH panel = nullptr;
    HBRUSH control = nullptr;
};

static LRESULT CALLBACK StandardDialogControlsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                                   DWORD_PTR refData) {
    auto data = (StandardDialogControls*)refData;
    if ((msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) && !ThemeUsesDarkChrome()) {
        SetTextColor((HDC)wp, ThemeWindowTextColor());
        SetBkColor((HDC)wp, ThemeWindowControlBackgroundColor());
        // Warm edits paint their own rounded fill. An opaque text background
        // would stamp a square over the corners.
        if (msg == WM_CTLCOLOREDIT && GetPropW((HWND)lp, L"SumatraWarmEdit")) {
            SetBkMode((HDC)wp, TRANSPARENT);
        }
        return (LRESULT)data->control;
    }
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
        if (data->registerForTheme) UnregisterAppDialogForTheme(hwnd);
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        data->fonts.Destroy();
        DeleteObject(data->panel);
        DeleteObject(data->control);
        delete data;
        return result;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void AppDialogUseStandardControls(HWND hwnd, bool registerForTheme) {
    auto data = (StandardDialogControls*)GetPropW(hwnd, L"SumatraStandardDialogControls");
    if (!data) {
        data = new StandardDialogControls();
        data->registerForTheme = registerForTheme;
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
        if (data->registerForTheme) {
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
    }
    DeleteObject(data->panel);
    data->panel = CreateSolidBrush(AppDialogPanelBackgroundColor());
    DeleteObject(data->control);
    data->control = CreateSolidBrush(ThemeWindowControlBackgroundColor());
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
    if (msg == WM_CTLCOLOREDIT && GetPropW(ctrl, L"SumatraWarmEdit")) {
        SetBkMode(dc, TRANSPARENT);
    }
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

Size AppDialogButtonSize(HWND button, int dpi) {
    Size text = HwndMeasureText(button, HwndGetTextTemp(button), GetWindowFont(button));
    return {std::max(MulDiv(72, dpi, 96), text.dx + MulDiv(24, dpi, 96)),
            std::max(MulDiv(28, dpi, 96), text.dy + MulDiv(10, dpi, 96))};
}

// Native MessageBox buttons otherwise follow the Windows display language,
// which can differ from the language selected in the application.
static void TranslateMessageBoxButtons(HWND hwnd) {
    struct ButtonLabel {
        int id;
        const WCHAR* text;
    };
    ButtonLabel labels[] = {
        {IDOK, _TRW("OK")},         {IDCANCEL, _TRW("Cancel")},      {IDYES, _TRW("Yes")},
        {IDNO, _TRW("No")},         {IDRETRY, _TRW("&Retry")},       {IDABORT, _TRW("Abort")},
        {IDIGNORE, _TRW("Ignore")}, {IDTRYAGAIN, _TRW("Try again")}, {IDCONTINUE, _TRW("Continue")}};
    for (auto& label : labels) {
        HWND button = GetDlgItem(hwnd, label.id);
        if (button && IsPushButton(button)) SetWindowTextW(button, label.text);
    }
}

static void LayoutMessageBoxButtons(HWND hwnd) {
    Vec<HWND> buttons;
    int dpi = DpiGet(hwnd);
    int gap = MulDiv(8, dpi, 96), width = 0, height = 0;
    Rect oldRow;
    bool first = true;
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsPushButton(child) || !IsWindowVisible(child)) continue;
        buttons.Append(child);
        Size size = AppDialogButtonSize(child, dpi);
        width += size.dx;
        height = std::max(height, size.dy);
        Rect rect = MapRectToWindow(WindowRect(child), HWND_DESKTOP, hwnd);
        oldRow = first ? rect : oldRow.Union(rect);
        first = false;
    }
    if (buttons.IsEmpty()) return;
    width += gap * (buttons.Size() - 1);
    Rect client = ClientRect(hwnd), window = WindowRect(hwnd);
    int extraW = std::max(0, width + MulDiv(32, dpi, 96) - client.dx);
    int extraH = std::max(0, height - oldRow.dy);
    SetWindowPos(hwnd, nullptr, window.x - extraW / 2, window.y - extraH / 2, window.dx + extraW, window.dy + extraH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    int x = (client.dx + extraW - width) / 2;
    for (HWND button : buttons) {
        Size size = AppDialogButtonSize(button, dpi);
        MoveWindow(button, x, oldRow.y, size.dx, height, TRUE);
        x += size.dx + gap;
    }
}

static volatile LONG gNativeDialogThemeReady = 0;

void InitAppMessageBoxTheme() {
    InterlockedExchange(&gNativeDialogThemeReady, 1);
    SetNativeMessageBoxHandler(AppMessageBoxW);
}

static void AppThemeNativeMessageWindow(HWND hwnd) {
    // Modal windows can be opened on worker threads; their resources stay with
    // the window instead of entering the UI thread's live-dialog registry.
    AppDialogUseStandardControls(hwnd, false);
    AppDialogApplyChrome(hwnd);
}

static LRESULT CALLBACK AppMessageBoxCbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE) {
        HWND hwnd = (HWND)wp;
        WCHAR cls[32]{};
        GetClassNameW(hwnd, cls, dimof(cls));
        if (wcscmp(cls, L"#32770") == 0 && !GetPropW(hwnd, L"SumatraStandardDialogControls")) {
            AppThemeNativeMessageWindow(hwnd);
            TranslateMessageBoxButtons(hwnd);
            LayoutMessageBoxButtons(hwnd);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static int AppMessageBoxFallbackW(HWND owner, const WCHAR* text, const WCHAR* caption, UINT flags) {
    struct ScopedHook {
        HHOOK hook = SetWindowsHookExW(WH_CBT, AppMessageBoxCbtProc, nullptr, GetCurrentThreadId());
        ~ScopedHook() {
            if (hook) {
                UnhookWindowsHookEx(hook);
            }
        }
    } hook;
    return MessageBoxW(owner, text, caption, flags);
}

// Classic MessageBox leaves a paper-white button strip under dark chrome and
// clips long lines after we swap fonts. Task Dialog sizes to content and takes
// the same AppTaskDialogIndirect theme path as the rest of the app.
static void AppMessageBoxFillTaskDialog(TASKDIALOGCONFIG* tdc, HWND owner, const WCHAR* text, const WCHAR* caption,
                                        UINT flags) {
    *tdc = {};
    tdc->cbSize = sizeof(*tdc);
    tdc->hwndParent = owner;
    tdc->dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    tdc->pszWindowTitle = caption;
    tdc->pszContent = text;
    tdc->lpCallbackData = (LONG_PTR)flags;

    switch (flags & MB_TYPEMASK) {
        case MB_OKCANCEL:
            tdc->dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            tdc->nDefaultButton = ((flags & MB_DEFMASK) == MB_DEFBUTTON2) ? IDCANCEL : IDOK;
            break;
        case MB_YESNOCANCEL:
            tdc->dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON | TDCBF_CANCEL_BUTTON;
            if ((flags & MB_DEFMASK) == MB_DEFBUTTON2) {
                tdc->nDefaultButton = IDNO;
            } else if ((flags & MB_DEFMASK) == MB_DEFBUTTON3) {
                tdc->nDefaultButton = IDCANCEL;
            } else {
                tdc->nDefaultButton = IDYES;
            }
            break;
        case MB_YESNO:
            tdc->dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
            tdc->nDefaultButton = ((flags & MB_DEFMASK) == MB_DEFBUTTON2) ? IDNO : IDYES;
            break;
        case MB_RETRYCANCEL:
            tdc->dwCommonButtons = TDCBF_RETRY_BUTTON | TDCBF_CANCEL_BUTTON;
            tdc->nDefaultButton = ((flags & MB_DEFMASK) == MB_DEFBUTTON2) ? IDCANCEL : IDRETRY;
            break;
        case MB_OK:
        default:
            tdc->dwCommonButtons = TDCBF_OK_BUTTON;
            break;
    }

    switch (flags & MB_ICONMASK) {
        case MB_ICONERROR:
            tdc->pszMainIcon = TD_ERROR_ICON;
            break;
        case MB_ICONWARNING:
            tdc->pszMainIcon = TD_WARNING_ICON;
            break;
        case MB_ICONINFORMATION:
            tdc->pszMainIcon = TD_INFORMATION_ICON;
            break;
        case MB_ICONQUESTION:
            tdc->dwFlags |= TDF_USE_HICON_MAIN;
            tdc->hMainIcon = (HICON)LoadImageW(nullptr, IDI_QUESTION, IMAGE_ICON, 0, 0, LR_SHARED);
            break;
        default:
            break;
    }

    if (flags & MB_RTLREADING) {
        tdc->dwFlags |= TDF_RTL_LAYOUT;
    }
}

static HRESULT CALLBACK AppMessageBoxTaskDialogExtra(HWND hwnd, UINT notification, WPARAM, LPARAM, LONG_PTR data) {
    if (notification == TDN_DIALOG_CONSTRUCTED) {
        UINT flags = (UINT)data;
        if (flags & (MB_SYSTEMMODAL | MB_TOPMOST)) {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        }
        if (flags & MB_SETFOREGROUND) {
            SetForegroundWindow(hwnd);
        }
        TranslateMessageBoxButtons(hwnd);
    }
    return S_OK;
}

int AppMessageBoxW(HWND owner, const WCHAR* text, const WCHAR* caption, UINT flags) {
    if (!InterlockedCompareExchange(&gNativeDialogThemeReady, 0, 0)) {
        return MessageBoxW(owner, text, caption, flags);
    }
    // Custom button sets (Abort/Retry/Ignore, Cancel/Try/Continue) stay on the
    // classic MessageBox path; everything Options uses is OK / YesNo / etc.
    UINT type = flags & MB_TYPEMASK;
    if (type == MB_ABORTRETRYIGNORE || type == MB_CANCELTRYCONTINUE) {
        return AppMessageBoxFallbackW(owner, text, caption, flags);
    }
    TASKDIALOGCONFIG tdc{};
    AppMessageBoxFillTaskDialog(&tdc, owner, text, caption, flags);
    tdc.pfCallback = AppMessageBoxTaskDialogExtra;
    tdc.lpCallbackData = (LONG_PTR)flags;
    int button = 0;
    if (SUCCEEDED(AppTaskDialogIndirect(&tdc, &button, nullptr, nullptr)) && button != 0) {
        return button;
    }
    return AppMessageBoxFallbackW(owner, text, caption, flags);
}

struct AppTaskDialogCallbackContext {
    PFTASKDIALOGCALLBACK original = nullptr;
    LONG_PTR originalData = 0;
};

static HRESULT CALLBACK AppTaskDialogCallback(HWND hwnd, UINT notification, WPARAM wp, LPARAM lp, LONG_PTR data) {
    auto* ctx = (AppTaskDialogCallbackContext*)data;
    if (notification == TDN_DIALOG_CONSTRUCTED || notification == TDN_NAVIGATED) {
        if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
            DarkMode::setDarkTaskDlg(hwnd);
        }
        AppThemeNativeMessageWindow(hwnd);
        // Task dialogs route text and link colors through nested notify sinks.
        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM) -> BOOL {
                WCHAR cls[64]{};
                GetClassNameW(child, cls, dimof(cls));
                if (wcscmp(cls, L"CtrlNotifySink") == 0 || wcscmp(cls, L"DirectUIHWND") == 0) {
                    AppDialogUseStandardControls(child, false);
                    AppDialogApplyChrome(child);
                }
                return TRUE;
            },
            0);
        // DirectUI often paints a system button strip; force our panel color
        // after construction so Warm / dark themes do not keep a white band.
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    return ctx->original ? ctx->original(hwnd, notification, wp, lp, ctx->originalData) : S_OK;
}

HRESULT AppTaskDialogIndirect(const TASKDIALOGCONFIG* config, int* button, int* radio, BOOL* verification) {
    if (!config || !InterlockedCompareExchange(&gNativeDialogThemeReady, 0, 0))
        return TaskDialogIndirect(config, button, radio, verification);
    TASKDIALOGCONFIG themed = *config;
    AppTaskDialogCallbackContext ctx{config->pfCallback, config->lpCallbackData};
    themed.pfCallback = AppTaskDialogCallback;
    themed.lpCallbackData = (LONG_PTR)&ctx;
    if (UseDarkModeLib() && ThemeUsesDarkChrome())
        return DarkMode::darkTaskDialogIndirect(&themed, button, radio, verification);
    return TaskDialogIndirect(&themed, button, radio, verification);
}
