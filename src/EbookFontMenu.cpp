/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/WinDynCalls.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "WindowTab.h"

#include "Commands.h"
#include "EbookFontConfig.h"
#include "Theme.h"
#include "AppDialogTheme.h"
#include "DarkModeSubclass.h"
#include "Translations.h"
#include "EbookFontMenu.h"
#include "Annotation.h"
#include "EbookInstalledFonts.h"
#include "resource.h"

static constexpr UINT_PTR kEbookFontComboSubclass = 0xEF01;
static const WCHAR* kFontComboState = L"SumatraEbookFontCombo";
static const WCHAR* kPreviewNone = L"";
static const WCHAR* kPreviewWestern = L"Quick fox";
static const WCHAR* kPreviewCjk = L"\u6c38\u548c\u6625\u98ce\u660e\u6708"; // 永和春风明月

struct EbookFontComboState {
    StrVec families;
    // Parallel to families. nullptr = unresolved; kPreviewNone = no sample; else static sample.
    Vec<const WCHAR*> previews;
    char* selected = nullptr;
    int editItemDy = 0;
    int listItemDy = 0;
    bool isCjk = false;
    bool filtering = false;
    bool composing = false;
    bool editing = false;
    ~EbookFontComboState() { str::Free(selected); }
};

static EbookFontComboState* FontComboState(HWND combo) {
    return (EbookFontComboState*)GetPropW(combo, kFontComboState);
}

static bool FontFaceCanDraw(HDC hdc, HFONT font, const WCHAR* text) {
    if (!font || !text || !text[0]) {
        return false;
    }
    HFONT old = (HFONT)SelectObject(hdc, font);
    bool ok = true;
    const WCHAR* p = text;
    while (ok && *p) {
        WORD glyphs[32];
        int n = 0;
        while (p[n] && n < dimof(glyphs)) {
            n++;
        }
        DWORD got = GetGlyphIndicesW(hdc, p, n, glyphs, GGI_MARK_NONEXISTING_GLYPHS);
        if (got == GDI_ERROR) {
            ok = false;
            break;
        }
        for (int i = 0; i < n; i++) {
            if (glyphs[i] == 0xFFFF) {
                ok = false;
                break;
            }
        }
        p += n;
    }
    SelectObject(hdc, old);
    return ok;
}

static HFONT CreateFamilyFontForDpi(const char* family, int dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(11, dpi, 96);
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    TempWStr face = ToWStrTemp(family);
    wcsncpy_s(lf.lfFaceName, face ? face : L"", _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

static const WCHAR* ResolveFontPreviewSample(HWND hwnd, bool isCjk, const char* family) {
    if (!family) {
        return kPreviewNone;
    }
    const WCHAR* sample = isCjk ? kPreviewCjk : kPreviewWestern;
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return kPreviewNone;
    }
    int dpi = DpiGetForHwnd(hwnd);
    HFONT font = CreateFamilyFontForDpi(family, dpi);
    const WCHAR* out = kPreviewNone;
    if (font) {
        if (FontFaceCanDraw(hdc, font, sample)) {
            out = sample;
        }
        DeleteObject(font);
    }
    ReleaseDC(hwnd, hdc);
    return out;
}

static const WCHAR* FontComboPreviewAt(EbookFontComboState* state, HWND combo, int familyIdx) {
    if (!state || familyIdx < 0 || familyIdx >= state->families.Size()) {
        return kPreviewNone;
    }
    while (state->previews.Size() < state->families.Size()) {
        state->previews.Append(nullptr);
    }
    if (!state->previews[familyIdx]) {
        state->previews[familyIdx] =
            ResolveFontPreviewSample(combo, state->isCjk, state->families.At(familyIdx));
    }
    return state->previews[familyIdx] ? state->previews[familyIdx] : kPreviewNone;
}

static int FontComboFindFamilyIndex(EbookFontComboState* state, const char* family) {
    if (!state || !family) {
        return -1;
    }
    for (int i = 0; i < state->families.Size(); i++) {
        if (str::EqI(state->families.At(i), family)) {
            return i;
        }
    }
    return -1;
}

static void FontComboApplyItemHeights(HWND combo, EbookFontComboState* state) {
    if (!combo || !state) {
        return;
    }
    if (state->editItemDy > 0) {
        SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, state->editItemDy);
    }
    if (state->listItemDy > 0) {
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, state->listItemDy);
    }
}

static void FontComboClearEditSelection(HWND combo) {
    if (combo) {
        SendMessageW(combo, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
    }
}

static void FontComboThemeListScrollbar(HWND combo) {
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(combo, &info) || !info.hwndList) {
        return;
    }
    // Reuse AppDialogThemeScrollBar (Ask AI / InlineTranslate scrollbar-only).
    if (DynSetWindowTheme) {
        DynSetWindowTheme(info.hwndList, L" ", L" ");
    }
    AppDialogThemeScrollBar(info.hwndList);
    LONG_PTR style = GetWindowLongPtrW(info.hwndList, GWL_STYLE);
    if ((style & WS_VSCROLL) == 0) {
        SetWindowLongPtrW(info.hwndList, GWL_STYLE, style | WS_VSCROLL);
        SetWindowPos(info.hwndList, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    int minVis = (int)SendMessageW(combo, CB_GETMINVISIBLE, 0, 0);
    if (minVis <= 0) {
        minVis = 10;
    }
    ShowScrollBar(info.hwndList, SB_VERT, count > minVis);
}

static void FontComboFill(HWND combo, EbookFontComboState* state, const char* query) {
    if (!combo || !state) {
        return;
    }
    state->filtering = true;
    SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < state->families.Size(); i++) {
        const char* family = state->families.At(i);
        if (!str::IsEmptyOrWhiteSpace(query) && !str::ContainsI(family, query)) {
            continue;
        }
        int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)ToWStrTemp(family));
        if (idx >= 0) {
            SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM)i);
        }
    }
    FontComboApplyItemHeights(combo, state);
    SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
    state->filtering = false;
}

static void FontComboRestoreCommitted(HWND combo, EbookFontComboState* state) {
    if (!combo || !state || !state->selected) {
        return;
    }
    FontComboFill(combo, state, nullptr);
    state->filtering = true;
    HwndSetText(combo, state->selected);
    int idx = FontComboFindFamilyIndex(state, state->selected);
    if (idx >= 0) {
        for (int i = 0; i < (int)SendMessageW(combo, CB_GETCOUNT, 0, 0); i++) {
            if ((int)SendMessageW(combo, CB_GETITEMDATA, i, 0) == idx) {
                SendMessageW(combo, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }
    state->filtering = false;
    state->editing = false;
    FontComboClearEditSelection(combo);
    InvalidateRect(combo, nullptr, FALSE);
}

static bool FontComboCommitExactText(HWND combo, EbookFontComboState* state) {
    if (!combo || !state) {
        return false;
    }
    const char* text = HwndGetTextTemp(combo);
    int idx = FontComboFindFamilyIndex(state, text);
    if (idx < 0) {
        return false;
    }
    str::ReplaceWithCopy(&state->selected, state->families.At(idx));
    state->editing = false;
    FontComboFill(combo, state, nullptr);
    state->filtering = true;
    HwndSetText(combo, state->selected);
    for (int i = 0; i < (int)SendMessageW(combo, CB_GETCOUNT, 0, 0); i++) {
        if ((int)SendMessageW(combo, CB_GETITEMDATA, i, 0) == idx) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            break;
        }
    }
    state->filtering = false;
    FontComboClearEditSelection(combo);
    return true;
}

static void FontComboSelectCurrentInList(HWND combo, EbookFontComboState* state) {
    if (!combo || !state || !state->selected) {
        return;
    }
    int famIdx = FontComboFindFamilyIndex(state, state->selected);
    if (famIdx < 0) {
        return;
    }
    int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        if ((int)SendMessageW(combo, CB_GETITEMDATA, i, 0) == famIdx) {
            SendMessageW(combo, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

static LRESULT CALLBACK FontComboProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
    if (msg == WM_NCDESTROY) {
        RemovePropW(hwnd, kFontComboState);
        RemoveWindowSubclass(hwnd, FontComboProc, id);
        delete (EbookFontComboState*)data;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK FontComboEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    HWND combo = GetParent(hwnd);
    auto state = FontComboState(combo);
    if (state && msg == WM_IME_STARTCOMPOSITION) {
        state->composing = true;
    }
    if (state && msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            FontComboRestoreCommitted(combo, state);
            SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (FontComboCommitExactText(combo, state)) {
                SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
                return 0;
            }
            int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
            if (sel >= 0) {
                int famIdx = (int)SendMessageW(combo, CB_GETITEMDATA, sel, 0);
                if (famIdx >= 0 && famIdx < state->families.Size()) {
                    str::ReplaceWithCopy(&state->selected, state->families.At(famIdx));
                    FontComboRestoreCommitted(combo, state);
                    SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
                    return 0;
                }
            }
            FontComboRestoreCommitted(combo, state);
            SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
            return 0;
        }
    }
    LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
    if (state && msg == WM_IME_ENDCOMPOSITION) {
        state->composing = false;
        PostMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_EDITCHANGE), (LPARAM)combo);
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, FontComboEditProc, id);
    }
    return result;
}

void LayoutEbookFontCombo(HWND combo, HWND heightRef) {
    if (!combo || !IsWindow(combo)) {
        return;
    }
    // Closed height is CB_SETITEMHEIGHT(-1). Match the reference Options combo
    // face exactly — no extra padding (that made these chips taller than peers).
    HFONT font = GetWindowFont(combo);
    int editDy = HwndMeasureText(combo, "Ag", font).dy + DpiScale(combo, 6);

    HWND ref = heightRef && IsWindow(heightRef) ? heightRef : nullptr;
    if (ref) {
        COMBOBOXINFO refInfo{sizeof(refInfo)};
        if (GetComboBoxInfo(ref, &refInfo)) {
            int refFace = std::max(RectDy(refInfo.rcItem), RectDy(refInfo.rcButton));
            if (refFace > 0) {
                editDy = refFace;
            }
        } else {
            int refItem = (int)SendMessageW(ref, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
            if (refItem > 0) {
                editDy = refItem;
            }
        }
    }

    // Popup rows may be taller for font preview; closed face stays with peers.
    int listDy = DpiScale(combo, 28);
    if (listDy < editDy + DpiScale(combo, 6)) {
        listDy = editDy + DpiScale(combo, 6);
    }
    SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, editDy);
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, listDy);
    SendMessageW(combo, CB_SETMINVISIBLE, 10, 0);

    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info) && info.hwndItem) {
        RECT rc = info.rcItem;
        SetWindowPos(info.hwndItem, nullptr, rc.left, rc.top, RectDx(rc), RectDy(rc),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    FontComboClearEditSelection(combo);
    InvalidateRect(combo, nullptr, TRUE);

    auto state = FontComboState(combo);
    if (state) {
        state->editItemDy = editDy;
        state->listItemDy = listDy;
    } else {
        SetPropW(combo, L"SumatraEbookFontComboEditDy", (HANDLE)(INT_PTR)editDy);
        SetPropW(combo, L"SumatraEbookFontComboListDy", (HANDLE)(INT_PTR)listDy);
    }
}

void InitEbookFontCombo(HWND combo) {
    auto state = new EbookFontComboState();
    state->isCjk = GetDlgCtrlID(combo) == IDC_EBOOK_CJK_FONT;
    state->editItemDy = (int)(INT_PTR)GetPropW(combo, L"SumatraEbookFontComboEditDy");
    state->listItemDy = (int)(INT_PTR)GetPropW(combo, L"SumatraEbookFontComboListDy");
    RemovePropW(combo, L"SumatraEbookFontComboEditDy");
    RemovePropW(combo, L"SumatraEbookFontComboListDy");
    if (state->editItemDy <= 0) {
        state->editItemDy = (int)SendMessageW(combo, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
    }
    if (state->listItemDy <= 0) {
        state->listItemDy = DpiScale(combo, 28);
    }
    int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        int length = (int)SendMessageW(combo, CB_GETLBTEXTLEN, i, 0);
        if (length < 0) {
            continue;
        }
        WCHAR* name = AllocArray<WCHAR>(length + 1);
        SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)name);
        state->families.Append(ToUtf8Temp(name));
        state->previews.Append(nullptr);
        SendMessageW(combo, CB_SETITEMDATA, i, (LPARAM)(state->families.Size() - 1));
        free(name);
    }
    state->selected = str::Dup(HwndGetTextTemp(combo));
    FontComboApplyItemHeights(combo, state);
    SetPropW(combo, kFontComboState, state);
    SetWindowSubclass(combo, FontComboProc, kEbookFontComboSubclass, (DWORD_PTR)state);
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info) && info.hwndItem) {
        SetWindowSubclass(info.hwndItem, FontComboEditProc, kEbookFontComboSubclass, 0);
    }
    FontComboThemeListScrollbar(combo);
    FontComboClearEditSelection(combo);
}

bool DrawEbookFontComboItem(DRAWITEMSTRUCT* item) {
    if (!item || item->CtlType != ODT_COMBOBOX ||
        (item->CtlID != IDC_EBOOK_LATIN_FONT && item->CtlID != IDC_EBOOK_CJK_FONT)) {
        return false;
    }
    bool selected = (item->itemState & ODS_SELECTED) != 0;
    bool editFace = (item->itemState & ODS_COMBOBOXEDIT) != 0;
    COLORREF bg = selected ? ThemeInspectorSelectedBackgroundColor() : ThemeWindowControlBackgroundColor();
    COLORREF fg = selected ? ThemeInspectorSelectedTextColor() : ThemeWindowTextColor();
    AutoDeleteBrush brush(CreateSolidBrush(bg));
    FillRect(item->hDC, &item->rcItem, brush);
    if (item->itemID == (UINT)-1) {
        return true;
    }
    int length = (int)SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
    if (length == CB_ERR) {
        return true;
    }
    WCHAR* family = AllocArray<WCHAR>(length + 1);
    defer {
        free(family);
    };
    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, (LPARAM)family);
    auto state = FontComboState(item->hwndItem);
    int famIdx = (int)item->itemData;
    if (famIdx < 0 || !state || famIdx >= state->families.Size()) {
        famIdx = state ? FontComboFindFamilyIndex(state, ToUtf8Temp(family)) : -1;
    }
    const WCHAR* preview = (!editFace && state) ? FontComboPreviewAt(state, item->hwndItem, famIdx) : kPreviewNone;

    int saved = SaveDC(item->hDC);
    IntersectClipRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, fg);
    SelectObject(item->hDC, GetWindowFont(item->hwndItem));
    RECT label = item->rcItem;
    int pad = DpiScale(item->hwndItem, 6);
    label.left += pad;
    label.right -= pad;

    Gdiplus::Graphics graphics(item->hDC);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::Font* font = TryCreateBundledFont(family, 11.0f, Gdiplus::FontStyleRegular);
    if (!font) {
        font = new Gdiplus::Font(family, 11.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    }
    // Two columns: [ name … ][gap][ preview ]. Never shrink preview type size.
    int gap = DpiScale(item->hwndItem, 14);
    int minNameDx = DpiScale(item->hwndItem, 64);
    int previewDx = 0;
    if (preview && preview[0] && font->GetLastStatus() == Gdiplus::Ok) {
        int avail = RectDx(item->rcItem) - pad * 2;
        int maxPreview = avail - minNameDx - gap;
        if (maxPreview >= DpiScale(item->hwndItem, 40)) {
            Gdiplus::RectF bounds;
            graphics.MeasureString(preview, -1, font, Gdiplus::PointF(0, 0), &bounds);
            int natural = (int)(bounds.Width + 0.99f);
            // Prefer full preview; long names ellipsis first. Wide faces may still
            // clip the preview after the name's minimum width is reserved.
            previewDx = natural <= maxPreview ? natural : maxPreview;
            label.right = item->rcItem.right - pad - previewDx - gap;
        } else {
            preview = kPreviewNone;
        }
    }
    if (font->GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, GetRValue(fg), GetGValue(fg), GetBValue(fg)));
        Gdiplus::StringFormat format;
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        Gdiplus::RectF rect((float)label.left, (float)label.top, (float)RectDx(label), (float)RectDy(label));
        graphics.DrawString(family, -1, font, rect, &format, &ink);
        if (preview && preview[0] && previewDx > 0) {
            COLORREF previewFg = selected ? fg : AccentColor(fg, 32);
            Gdiplus::SolidBrush previewInk(
                Gdiplus::Color(255, GetRValue(previewFg), GetGValue(previewFg), GetBValue(previewFg)));
            Gdiplus::StringFormat previewFmt;
            previewFmt.SetAlignment(Gdiplus::StringAlignmentFar);
            previewFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            previewFmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            previewFmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
            Gdiplus::RectF previewRect((float)(item->rcItem.right - pad - previewDx), (float)item->rcItem.top,
                                       (float)previewDx, (float)RectDy(item->rcItem));
            graphics.DrawString(preview, -1, font, previewRect, &previewFmt, &previewInk);
        }
    } else {
        DrawTextW(item->hDC, family, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    delete font;
    if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) {
        DrawFocusRect(item->hDC, &item->rcItem);
    }
    RestoreDC(item->hDC, saved);
    return true;
}

void EbookFontComboCommand(HWND combo, int notification) {
    auto state = FontComboState(combo);
    if (!state || state->filtering || state->composing) {
        return;
    }
    if (notification == CBN_SETFOCUS) {
        state->editing = true;
        return;
    }
    if (notification == CBN_KILLFOCUS) {
        // List clicks / scrollbar drags can briefly move focus; ignore while open.
        if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) {
            return;
        }
        COMBOBOXINFO info{sizeof(info)};
        HWND focus = GetFocus();
        if (GetComboBoxInfo(combo, &info) &&
            (focus == combo || focus == info.hwndItem || focus == info.hwndList)) {
            return;
        }
        if (!FontComboCommitExactText(combo, state)) {
            FontComboRestoreCommitted(combo, state);
        }
        return;
    }
    if (notification == CBN_CLOSEUP) {
        const char* text = HwndGetTextTemp(combo);
        if (FontComboFindFamilyIndex(state, text) < 0) {
            state->filtering = true;
            HwndSetText(combo, state->selected);
            state->filtering = false;
            FontComboFill(combo, state, nullptr);
            FontComboSelectCurrentInList(combo, state);
        }
        FontComboClearEditSelection(combo);
        return;
    }
    if (notification == CBN_DROPDOWN) {
        FontComboThemeListScrollbar(combo);
        const char* text = HwndGetTextTemp(combo);
        if (str::IsEmptyOrWhiteSpace(text) || str::EqI(text, state->selected)) {
            FontComboFill(combo, state, nullptr);
            state->filtering = true;
            HwndSetText(combo, state->selected);
            state->filtering = false;
            FontComboSelectCurrentInList(combo, state);
        } else {
            FontComboFill(combo, state, text);
            state->filtering = true;
            HwndSetText(combo, text);
            state->filtering = false;
        }
        return;
    }
    if (notification == CBN_SELCHANGE || notification == CBN_SELENDOK) {
        int index = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
        int famIdx = (int)SendMessageW(combo, CB_GETITEMDATA, index, 0);
        if (famIdx >= 0 && famIdx < state->families.Size()) {
            str::ReplaceWithCopy(&state->selected, state->families.At(famIdx));
            state->editing = false;
        } else {
            int length = (int)SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
            if (length >= 0) {
                WCHAR* name = AllocArray<WCHAR>(length + 1);
                SendMessageW(combo, CB_GETLBTEXT, index, (LPARAM)name);
                str::ReplaceWithCopy(&state->selected, ToUtf8Temp(name));
                free(name);
                state->editing = false;
            }
        }
        FontComboClearEditSelection(combo);
        return;
    }
    if (notification != CBN_EDITCHANGE) {
        return;
    }
    state->editing = true;
    AutoFreeStr query(str::Dup(HwndGetTextTemp(combo)));
    DWORD selection = (DWORD)SendMessageW(combo, CB_GETEDITSEL, 0, 0);
    FontComboFill(combo, state, query);
    state->filtering = true;
    HwndSetText(combo, query);
    SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
    SendMessageW(combo, CB_SETEDITSEL, 0, selection);
    state->filtering = false;
    FontComboThemeListScrollbar(combo);
    InvalidateRect(combo, nullptr, FALSE);
}

const char* EbookFontComboSelection(HWND combo) {
    auto state = FontComboState(combo);
    const char* text = HwndGetTextTemp(combo);
    if (state) {
        int idx = FontComboFindFamilyIndex(state, text);
        if (idx >= 0) {
            return state->families.At(idx);
        }
        return state->selected;
    }
    return text;
}

int gFirstEbookLatinFontCmdId = 0;
int gLastEbookLatinFontCmdId = 0;
int gFirstEbookCjkFontCmdId = 0;
int gLastEbookCjkFontCmdId = 0;

static int gEbookLatinBundledMenuCount = 0;
static int gEbookCjkBundledMenuCount = 0;

bool IsReflowableEbookTabForFontMenu(WindowTab* tab) {
    if (!tab || !tab->IsDocLoaded() || tab->IsAboutTab()) {
        return false;
    }
    EngineBase* engine = tab->GetEngine();
    if (!engine) {
        return false;
    }
    if (engine->kind == kindEngineMupdf) {
        return !str::EqI(engine->defaultExt, ".pdf");
    }
    return engine->kind == kindEngineMobi || engine->kind == kindEngineEpub || engine->kind == kindEngineFb2 ||
           engine->kind == kindEnginePdb || engine->kind == kindEngineHtml || engine->kind == kindEngineTxt;
}

static bool IsExtWithoutFontSize(const char* ext) {
    // Office keeps author sizes; Markdown layout ignores reader font-size CSS.
    return ext &&
           (str::EqI(ext, ".docx") || str::EqI(ext, ".doc") || str::EqI(ext, ".xlsx") || str::EqI(ext, ".pptx") ||
            str::EqI(ext, ".hwpx") || str::EqI(ext, ".md") || str::EqI(ext, ".markdown"));
}

bool SupportsEbookFontSizeChange(WindowTab* tab) {
    if (!IsReflowableEbookTabForFontMenu(tab)) {
        return false;
    }
    EngineBase* engine = tab->GetEngine();
    if (!engine) {
        return false;
    }
    // Prefer path extension so classic .doc converted to a temp .docx still
    // counts as Word (no reader font-size override).
    const char* ext = engine->defaultExt;
    if (engine->FilePath()) {
        TempStr pathExt = path::GetExtTemp(engine->FilePath());
        if (pathExt && pathExt[0]) {
            ext = pathExt;
        }
    }
    if (IsExtWithoutFontSize(ext) || EngineMupdfIsWordDocument(engine)) {
        return false;
    }
    return true;
}

static void SortFamilyNames(Vec<char*>* families) {
    if (!families || families->size() < 2) {
        return;
    }
    for (size_t i = 0; i + 1 < families->size(); i++) {
        for (size_t j = i + 1; j < families->size(); j++) {
            if (_stricmp(families->at(i), families->at(j)) > 0) {
                char* tmp = families->at(i);
                families->at(i) = families->at(j);
                families->at(j) = tmp;
            }
        }
    }
}

static bool FamilyListContainsCanonical(Vec<char*>* families, const char* family, bool cjk) {
    if (!families || !family) {
        return false;
    }
    for (char* existing : *families) {
        bool same =
            cjk ? EbookCjkFontFamiliesEquivalent(existing, family) : EbookLatinFontFamiliesEquivalent(existing, family);
        if (same) {
            return true;
        }
    }
    return false;
}

static void AppendUniqueFamily(Vec<char*>* families, char* family, bool cjk) {
    if (!families || !family || !family[0]) {
        str::Free(family);
        return;
    }
    if (FamilyListContainsCanonical(families, family, cjk)) {
        str::Free(family);
        return;
    }
    families->Append(family);
}

static void CollectBundledFontFamilies(Vec<char*>* latinFamilies, Vec<char*>* cjkFamilies) {
    if (!latinFamilies || !cjkFamilies) {
        return;
    }
    Vec<char*> all;
    CollectBundledFontFamilyNames(&all);
    for (char* family : all) {
        if (!family || !family[0]) {
            str::Free(family);
            continue;
        }
        if (IsBundledLatinFontFamily(family)) {
            const char* canonical = NormalizeEbookLatinFontFamily(family);
            AppendUniqueFamily(latinFamilies, str::Dup(canonical), false);
        } else {
            const char* canonical = NormalizeEbookCjkFontFamily(family);
            AppendUniqueFamily(cjkFamilies, str::Dup(canonical), true);
        }
        str::Free(family);
    }
    SortFamilyNames(latinFamilies);
    SortFamilyNames(cjkFamilies);
}

static void MergeInstalledFontFamilies(Vec<char*>* bundledFamilies, Vec<char*>* installedFamilies, bool cjk) {
    if (!bundledFamilies || !installedFamilies) {
        return;
    }
    SortFamilyNames(installedFamilies);
    for (char* family : *installedFamilies) {
        AppendUniqueFamily(bundledFamilies, family, cjk);
    }
    installedFamilies->Clear();
}

static int FindFontMenuCmdId(int origCmdId, const char* family) {
    if (!family || !family[0]) {
        return 0;
    }
    bool cjk = origCmdId == CmdSetEbookCjkFont;
    Vec<CustomCommand*> cmds;
    GetCommandsWithOrigId(cmds, origCmdId);
    for (CustomCommand* cmd : cmds) {
        const char* cmdFamily = GetCommandStringArg(cmd, kCmdArgFontFamily, nullptr);
        if (!cmdFamily) {
            continue;
        }
        bool same = cjk ? EbookCjkFontFamiliesEquivalent(cmdFamily, family)
                        : EbookLatinFontFamiliesEquivalent(cmdFamily, family);
        if (same) {
            return cmd->id;
        }
    }
    return 0;
}

static void CreateFontFamilyCommands(int origCmdId, Vec<char*>* families, int* firstCmdId, int* lastCmdId) {
    *firstCmdId = 0;
    *lastCmdId = 0;
    bool cjk = origCmdId == CmdSetEbookCjkFont;
    for (char* family : *families) {
        const char* label = cjk ? GetEbookCjkFontMenuLabel(family) : GetEbookLatinFontMenuLabel(family);
        auto args = NewStringArg(kCmdArgFontFamily, family);
        CustomCommand* cmd = CreateCustomCommand(label, origCmdId, args);
        cmd->name = str::Dup(label);
        if (*firstCmdId == 0) {
            *firstCmdId = cmd->id;
        }
        *lastCmdId = cmd->id;
    }
}

void CreateEbookFontMenuCommands() {
    gFirstEbookLatinFontCmdId = 0;
    gLastEbookLatinFontCmdId = 0;
    gFirstEbookCjkFontCmdId = 0;
    gLastEbookCjkFontCmdId = 0;
    gEbookLatinBundledMenuCount = 0;
    gEbookCjkBundledMenuCount = 0;

    Vec<char*> latinFamilies;
    Vec<char*> cjkFamilies;
    CollectBundledFontFamilies(&latinFamilies, &cjkFamilies);
    gEbookLatinBundledMenuCount = (int)latinFamilies.size();
    gEbookCjkBundledMenuCount = (int)cjkFamilies.size();

    Vec<char*> latinInstalled;
    Vec<char*> cjkInstalled;
    CollectInstalledLatinFontFamilies(&latinInstalled);
    CollectInstalledCjkFontFamilies(&cjkInstalled);
    MergeInstalledFontFamilies(&latinFamilies, &latinInstalled, false);
    MergeInstalledFontFamilies(&cjkFamilies, &cjkInstalled, true);

    CreateFontFamilyCommands(CmdSetEbookLatinFont, &latinFamilies, &gFirstEbookLatinFontCmdId,
                             &gLastEbookLatinFontCmdId);
    CreateFontFamilyCommands(CmdSetEbookCjkFont, &cjkFamilies, &gFirstEbookCjkFontCmdId, &gLastEbookCjkFontCmdId);
    for (char* family : latinFamilies) {
        str::Free(family);
    }
    for (char* family : cjkFamilies) {
        str::Free(family);
    }
}

void CollectEbookFontFamilies(Vec<char*>* latinFamilies, Vec<char*>* cjkFamilies) {
    if (!latinFamilies || !cjkFamilies) {
        return;
    }
    CollectBundledFontFamilies(latinFamilies, cjkFamilies);
    Vec<char*> latinInstalled;
    Vec<char*> cjkInstalled;
    CollectInstalledLatinFontFamilies(&latinInstalled);
    CollectInstalledCjkFontFamilies(&cjkInstalled);
    MergeInstalledFontFamilies(latinFamilies, &latinInstalled, false);
    MergeInstalledFontFamilies(cjkFamilies, &cjkInstalled, true);
}

static void CheckFontMenuRadio(HMENU menu, int origCmdId, const char* currentFamily, int firstCmdId, int lastCmdId) {
    if (!menu || firstCmdId <= 0 || lastCmdId < firstCmdId) {
        return;
    }
    int currCmdId = FindFontMenuCmdId(origCmdId, currentFamily);
    if (currCmdId >= firstCmdId && currCmdId <= lastCmdId) {
        CheckMenuRadioItem(menu, firstCmdId, lastCmdId, currCmdId, MF_BYCOMMAND);
    }
}

static void AppendFontCommandsToMenu(HMENU menu, int origCmdId, int bundledCount, int firstCmdId, int lastCmdId,
                                     const char* currentFamily) {
    Vec<CustomCommand*> cmds;
    GetCommandsWithOrigId(cmds, origCmdId);
    int idx = 0;
    for (CustomCommand* cmd : cmds) {
        if (!cmd->name) {
            continue;
        }
        if (bundledCount > 0 && idx == bundledCount) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        TempWStr ws = ToWStrTemp(cmd->name);
        AppendMenuW(menu, MF_STRING, (UINT_PTR)cmd->id, ws);
        idx++;
    }
    CheckFontMenuRadio(menu, origCmdId, currentFamily, firstCmdId, lastCmdId);
}

void AppendEbookLatinFontsToMenu(HMENU menu) {
    AppendFontCommandsToMenu(menu, CmdSetEbookLatinFont, gEbookLatinBundledMenuCount, gFirstEbookLatinFontCmdId,
                             gLastEbookLatinFontCmdId, GetEbookLatinFontFamily());
}

void AppendEbookCjkFontsToMenu(HMENU menu) {
    AppendFontCommandsToMenu(menu, CmdSetEbookCjkFont, gEbookCjkBundledMenuCount, gFirstEbookCjkFontCmdId,
                             gLastEbookCjkFontCmdId, GetEbookCjkFontFamily());
}

void UpdateEbookFontMenuRadioState(HMENU menu) {
    CheckFontMenuRadio(menu, CmdSetEbookLatinFont, GetEbookLatinFontFamily(), gFirstEbookLatinFontCmdId,
                       gLastEbookLatinFontCmdId);
    CheckFontMenuRadio(menu, CmdSetEbookCjkFont, GetEbookCjkFontFamily(), gFirstEbookCjkFontCmdId,
                       gLastEbookCjkFontCmdId);
}

HWND GetCurrentModelessDialog();
void SetCurrentModelessDialog(HWND);

constexpr const WCHAR* kFontPickerClass = L"SUMATRA_FONT_PICKER";
constexpr const WCHAR* kFontPickerListClass = L"SUMATRA_FONT_PICKER_LIST";

struct FontPickerWnd {
    HWND hwnd = nullptr;
    HWND hwndSearch = nullptr;
    HWND hwndList = nullptr;
    HWND hwndOk = nullptr;
    HWND hwndCancel = nullptr;
    WNDPROC prevSearchProc = nullptr;
    bool cjk = false;
    int dpi = 96;
    int bundledCount = 0;
    int recentCount = 0;
    Vec<char*> names;
    Vec<int> visible; // index into names, or -1 for the separator
    int sel = 0;
    int rowDy = 22;
    HFONT uiFont = nullptr;
    HFONT uiFontBold = nullptr;
    HBRUSH bgBrush = nullptr;
    HBRUSH ctrlBrush = nullptr;
    FontFamilyPickedFn onPick = nullptr;
    void* pickCtx = nullptr;
};

static FontPickerWnd* gFontPicker = nullptr;
static ATOM gFontPickerAtom = 0;
static ATOM gFontPickerListAtom = 0;

static void FontPickerFree(FontPickerWnd* p) {
    if (!p) {
        return;
    }
    for (char* name : p->names) {
        str::Free(name);
    }
    p->names.Reset();
    DeleteObject(p->uiFont);
    DeleteObject(p->uiFontBold);
    DeleteObject(p->bgBrush);
    DeleteObject(p->ctrlBrush);
    p->uiFont = p->uiFontBold = nullptr;
    p->bgBrush = p->ctrlBrush = nullptr;
}

static void FontPickerRebuildVisible(FontPickerWnd* p) {
    p->visible.Reset();
    char* query = HwndGetTextTemp(p->hwndSearch);
    auto matches = [&](int i) { return str::IsEmpty(query) || str::ContainsI(p->names[i], query); };
    int shownRecent = 0;
    for (int i = 0; i < p->recentCount && i < p->names.Size(); i++) {
        if (!matches(i)) {
            continue;
        }
        if (shownRecent == 0 && str::IsEmpty(query)) {
            p->visible.Append(-2);
        }
        p->visible.Append(i);
        shownRecent++;
    }
    int bundledStart = p->recentCount;
    int shownBundled = 0;
    for (int i = bundledStart; i < p->bundledCount && i < p->names.Size(); i++) {
        if (matches(i)) {
            p->visible.Append(i);
            shownBundled++;
        }
    }
    bool anyInstalled = false;
    for (int i = p->bundledCount; i < p->names.Size(); i++) {
        if (matches(i)) {
            anyInstalled = true;
            break;
        }
    }
    if ((shownRecent > 0 || shownBundled > 0) && anyInstalled) {
        p->visible.Append(-1);
    }
    for (int i = p->bundledCount; i < p->names.Size(); i++) {
        if (matches(i)) {
            p->visible.Append(i);
        }
    }
    if (p->sel >= p->visible.Size()) {
        p->sel = p->visible.Size() - 1;
    }
    if (p->sel < 0) {
        p->sel = 0;
    }
    if (p->sel < p->visible.Size() && p->visible[p->sel] < 0 && p->sel + 1 < p->visible.Size()) {
        p->sel++;
    }
}

static int FontPickerRowsPerPage(FontPickerWnd* p) {
    RECT rc{};
    if (p->hwndList) {
        GetClientRect(p->hwndList, &rc);
    }
    int listDy = rc.bottom - rc.top;
    if (listDy < p->rowDy) {
        return 1;
    }
    return listDy / p->rowDy;
}

static void FontPickerEnsureSelVisible(FontPickerWnd* p) {
    if (!p->hwndList) {
        return;
    }
    int rows = FontPickerRowsPerPage(p);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    GetScrollInfo(p->hwndList, SB_VERT, &si);
    int pos = si.nPos;
    if (p->sel < pos) {
        pos = p->sel;
    } else if (p->sel >= pos + rows) {
        pos = p->sel - rows + 1;
    }
    if (pos < 0) {
        pos = 0;
    }
    si.fMask = SIF_POS | SIF_RANGE | SIF_PAGE;
    si.nMin = 0;
    si.nMax = std::max(0, p->visible.Size() - 1);
    si.nPage = rows;
    si.nPos = pos;
    SetScrollInfo(p->hwndList, SB_VERT, &si, TRUE);
}

static void FontPickerApplySelection(FontPickerWnd* p) {
    if (!p || p->sel < 0 || p->sel >= p->visible.Size()) {
        return;
    }
    int nameIdx = p->visible[p->sel];
    if (nameIdx < 0 || nameIdx >= p->names.Size()) {
        return;
    }
    char* picked = str::Dup(p->names[nameIdx]);
    FontFamilyPickedFn onPick = p->onPick;
    void* pickCtx = p->pickCtx;
    int orig = p->cjk ? CmdSetEbookCjkFont : CmdSetEbookLatinFont;
    int cmdId = onPick ? 0 : FindFontMenuCmdId(orig, p->names[nameIdx]);
    HWND owner = GetWindow(p->hwnd, GW_OWNER);
    DestroyWindow(p->hwnd);
    if (onPick) {
        NoteFreeTextFontUsed(picked);
        onPick(picked, pickCtx);
        str::Free(picked);
        return;
    }
    str::Free(picked);
    if (cmdId > 0 && owner) {
        HwndSendCommand(owner, cmdId);
    }
}

static void FontPickerMoveSel(FontPickerWnd* p, int delta) {
    if (p->visible.Size() == 0) {
        return;
    }
    int next = p->sel;
    for (int step = 0; step < p->visible.Size(); step++) {
        next += delta;
        if (next < 0) {
            next = 0;
        }
        if (next >= p->visible.Size()) {
            next = p->visible.Size() - 1;
        }
        if (p->visible[next] >= 0) {
            break;
        }
    }
    p->sel = next;
    FontPickerEnsureSelVisible(p);
    if (p->hwndList) {
        InvalidateRect(p->hwndList, nullptr, FALSE);
    }
}

// A step away from the window color. ControlBackgroundColor is a near-white
// card, which reads as a white slab on Warm parchment and on Light-White.
static COLORREF FontPickerFieldColor() {
    // Must match the CTLCOLOR background, not a separate accented brush:
    // single-line EDIT paints its text band with SetBkColor and the remaining
    // client height with the returned brush. Different colors leave a stripe.
    return ThemeWindowControlBackgroundColor();
}

static COLORREF FontPickerSelColor() {
    COLORREF bg = AppDialogPanelBackgroundColor();
    // Negative AccentColor on a light color walks toward white.
    if (ThemeUsesDarkChrome()) {
        return AccentColor(bg, 22);
    }
    return AccentColor(bg, 12);
}

static void FontPickerApplyControlTheme(FontPickerWnd* p) {
    if (!p) {
        return;
    }
    if (p->hwndSearch) {
        if (UseDarkModeLib()) {
            // The search proc owns the outer frame and centered text band;
            // don't let the generic edit border outline that inner band.
            DarkMode::removeCustomBorderForListBoxOrEditCtrlSubclass(p->hwndSearch);
        }
        // A themed EDIT ignores WM_CTLCOLOREDIT and paints COLOR_WINDOW, which
        // is white on Warm. An empty theme lets the field brush show through.
        // Dark chrome keeps the theme so the box matches the other dialogs.
        if (ThemeUsesDarkChrome()) {
            if (DynSetWindowTheme) {
                DynSetWindowTheme(p->hwndSearch, nullptr, nullptr);
            }
        } else if (DynSetWindowTheme) {
            DynSetWindowTheme(p->hwndSearch, L"", L"");
        }
    }
    if (!p->hwndList) {
        return;
    }
    // The list is our own class, so setDarkWndSafe never themes its bar.
    // Ask AI recolors only the scrollbar class and leaves the rest alone.
    if (UseDarkModeLib() && ThemeUsesDarkChrome() && DarkMode::isExperimentalActive()) {
        if (DynSetWindowTheme) {
            DynSetWindowTheme(p->hwndList, nullptr, L"DarkMode_Explorer::ScrollBar");
        }
    } else if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
        DarkMode::setDarkScrollBar(p->hwndList);
    } else if (DynSetWindowTheme) {
        DynSetWindowTheme(p->hwndList, L"Explorer", nullptr);
    }
    SetWindowPos(p->hwndList, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void FontPickerRecreateThemeBrushes(FontPickerWnd* p) {
    DeleteObject(p->bgBrush);
    DeleteObject(p->ctrlBrush);
    p->bgBrush = CreateSolidBrush(AppDialogPanelBackgroundColor());
    p->ctrlBrush = CreateSolidBrush(FontPickerFieldColor());
}

static void FontPickerThemeRefreshCb(HWND hwnd, void* ctx) {
    auto* p = (FontPickerWnd*)ctx;
    if (!p) {
        return;
    }
    FontPickerRecreateThemeBrushes(p);
    AppDialogApplyChrome(hwnd);
    FontPickerApplyControlTheme(p);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static Size FontPickerOuterSize(HWND hwnd, int dpi, int clientDx, int clientDy) {
    RECT rc{0, 0, clientDx, clientDy};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    if (hwnd) {
        style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
        exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_DLGMODALFRAME;
    }
    using SigAdjustWindowRectExForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static SigAdjustWindowRectExForDpi pfnAdjust = nullptr;
    static bool triedAdjust = false;
    if (!triedAdjust) {
        triedAdjust = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            pfnAdjust = (SigAdjustWindowRectExForDpi)GetProcAddress(user32, "AdjustWindowRectExForDpi");
        }
    }
    BOOL ok = FALSE;
    if (pfnAdjust && dpi > 0) {
        ok = pfnAdjust(&rc, style, FALSE, exStyle, (UINT)dpi);
    }
    if (!ok) {
        AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    }
    return Size(rc.right - rc.left, rc.bottom - rc.top);
}

static Size FontPickerLayout(FontPickerWnd* p, int dpi) {
    if (!p || !p->hwnd) {
        return Size();
    }
    if (dpi <= 0) {
        dpi = 96;
    }
    p->dpi = dpi;
    int pad = MulDiv(12, dpi, 96);
    int gap = MulDiv(8, dpi, 96);
    int searchH = MulDiv(24, dpi, 96);
    Size okSize = ButtonGetIdealSize(p->hwndOk);
    Size cancelSize = ButtonGetIdealSize(p->hwndCancel);
    int btnH = std::max(okSize.dy, cancelSize.dy);
    int btnW = std::max(MulDiv(75, dpi, 96), std::max(okSize.dx, cancelSize.dx));
    p->rowDy = MulDiv(24, dpi, 96);
    int clientW = MulDiv(320, dpi, 96);
    int listH = p->rowDy * 12;
    int y = pad;
    if (p->hwndSearch) {
        SetWindowPos(p->hwndSearch, nullptr, pad, y, clientW - pad * 2, searchH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    y += searchH + gap;
    if (p->hwndList) {
        SetWindowPos(p->hwndList, nullptr, pad, y, clientW - pad * 2, listH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    y += listH + pad;
    int okX = clientW - pad - btnW;
    int cancelX = okX - gap - btnW;
    if (p->hwndCancel) {
        SetWindowPos(p->hwndCancel, nullptr, cancelX, y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (p->hwndOk) {
        SetWindowPos(p->hwndOk, nullptr, okX, y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    y += btnH + pad;
    FontPickerEnsureSelVisible(p);
    return FontPickerOuterSize(p->hwnd, dpi, clientW, y);
}

static void FontPickerPaintList(FontPickerWnd* p, HDC hdc) {
    RECT rc{};
    GetClientRect(p->hwndList, &rc);
    COLORREF text = ThemeWindowTextColor();
    HBRUSH fill = p->bgBrush ? p->bgBrush : (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(hdc, &rc, fill);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    GetScrollInfo(p->hwndList, SB_VERT, &si);
    int first = si.nPos;
    int rows = FontPickerRowsPerPage(p);
    int padX = MulDiv(6, p->dpi, 96);
    SetBkMode(hdc, TRANSPARENT);
    int y = 0;
    for (int row = 0; row < rows; row++) {
        int vis = first + row;
        if (vis >= p->visible.Size()) {
            break;
        }
        RECT rr{rc.left + padX, y, rc.right - padX, y + p->rowDy};
        int nameIdx = p->visible[vis];
        if (nameIdx < 0) {
            if (nameIdx == -2) {
                SetTextColor(hdc, ThemeWindowTextDisabledColor());
                HFONT old = (HFONT)SelectObject(hdc, p->uiFont);
                DrawTextW(hdc, ToWStrTemp(_TRA("Recent")), -1, &rr,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(hdc, old);
                y += p->rowDy;
                continue;
            }
            int mid = (rr.top + rr.bottom) / 2;
            HPEN pen = CreatePen(PS_SOLID, 1, ThemeWindowTextDisabledColor());
            HGDIOBJ old = SelectObject(hdc, pen);
            MoveToEx(hdc, rr.left, mid, nullptr);
            LineTo(hdc, rr.right, mid);
            SelectObject(hdc, old);
            DeleteObject(pen);
        } else {
            if (vis == p->sel) {
                AutoDeleteBrush sel(CreateSolidBrush(FontPickerSelColor()));
                RECT bar = rr;
                bar.left = rc.left;
                bar.right = rc.right;
                FillRect(hdc, &bar, sel);
            }
            SetTextColor(hdc, text);
            const char* family = p->names[nameIdx];
            HFONT face = CreateFamilyFontForDpi(family, p->dpi);
            TempWStr ws = ToWStrTemp(family);
            HFONT use = FontFaceCanDraw(hdc, face, ws) ? face : p->uiFont;
            HFONT old = (HFONT)SelectObject(hdc, use);
            DrawTextW(hdc, ws, -1, &rr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, old);
            DeleteObject(face);
        }
        y += p->rowDy;
    }
}

static void FontPickerScrollList(FontPickerWnd* p, int lines) {
    if (!p || !p->hwndList || lines == 0) {
        return;
    }
    WPARAM code = lines < 0 ? SB_LINEUP : SB_LINEDOWN;
    int n = lines < 0 ? -lines : lines;
    for (int i = 0; i < n; i++) {
        SendMessageW(p->hwndList, WM_VSCROLL, code, 0);
    }
}

static LRESULT CALLBACK FontPickerSearchProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = (FontPickerWnd*)GetWindowLongPtr(GetParent(hwnd), GWLP_USERDATA);
    if (p && p->prevSearchProc && msg == WM_NCCALCSIZE) {
        LRESULT result = CallWindowProcW(p->prevSearchProc, hwnd, msg, wp, lp);
        RECT* client = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
        int textH = HwndMeasureText(hwnd, "Ag", p->uiFont).dy;
        int available = client->bottom - client->top;
        if (textH > 0 && available > textH) {
            client->top += (available - textH) / 2;
            client->bottom = client->top + textH;
        }
        return result;
    }
    if (p && msg == WM_NCPAINT) {
        HDC dc = GetWindowDC(hwnd);
        if (dc) {
            RECT window{};
            GetWindowRect(hwnd, &window);
            RECT client{};
            GetClientRect(hwnd, &client);
            POINT origin{};
            ClientToScreen(hwnd, &origin);
            OffsetRect(&client, origin.x - window.left, origin.y - window.top);
            ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
            RECT frame{0, 0, window.right - window.left, window.bottom - window.top};
            FillRect(dc, &frame, p->ctrlBrush);
            ScopedGdiObj<HBRUSH> border(CreateSolidBrush(ThemeInspectorSeparatorColor()));
            FrameRect(dc, &frame, border);
            ReleaseDC(hwnd, dc);
        }
        return 0;
    }
    if (p && msg == WM_KEYDOWN) {
        if (wp == VK_DOWN) {
            FontPickerMoveSel(p, 1);
            return 0;
        }
        if (wp == VK_UP) {
            FontPickerMoveSel(p, -1);
            return 0;
        }
    }
    if (p && msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        FontPickerScrollList(p, delta > 0 ? -3 : 3);
        return 0;
    }
    if (!p || !p->prevSearchProc) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return CallWindowProcW(p->prevSearchProc, hwnd, msg, wp, lp);
}

static FontPickerWnd* FontPickerFromList(HWND hwnd) {
    HWND parent = GetParent(hwnd);
    if (!parent) {
        return nullptr;
    }
    return (FontPickerWnd*)GetWindowLongPtr(parent, GWLP_USERDATA);
}

static void FontPickerSelectRow(FontPickerWnd* p, int y, bool apply) {
    if (!p) {
        return;
    }
    int row = y / p->rowDy;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    GetScrollInfo(p->hwndList, SB_VERT, &si);
    int vis = si.nPos + row;
    if (vis < 0 || vis >= p->visible.Size() || p->visible[vis] < 0) {
        return;
    }
    p->sel = vis;
    if (apply) {
        FontPickerApplySelection(p);
        return;
    }
    FontPickerEnsureSelVisible(p);
    InvalidateRect(p->hwndList, nullptr, FALSE);
}

static LRESULT CALLBACK WndProcFontPickerList(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = FontPickerFromList(hwnd);
    if (msg == WM_NCCREATE) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!p) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            FontPickerPaintList(p, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            FontPickerEnsureSelVisible(p);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            FontPickerScrollList(p, delta > 0 ? -3 : 3);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP:
                    pos--;
                    break;
                case SB_LINEDOWN:
                    pos++;
                    break;
                case SB_PAGEUP:
                    pos -= (int)si.nPage;
                    break;
                case SB_PAGEDOWN:
                    pos += (int)si.nPage;
                    break;
                case SB_THUMBTRACK:
                    pos = si.nTrackPos;
                    break;
            }
            if (pos < 0) {
                pos = 0;
            }
            int maxPos = std::max(0, p->visible.Size() - (int)si.nPage);
            if (pos > maxPos) {
                pos = maxPos;
            }
            si.fMask = SIF_POS;
            si.nPos = pos;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            FontPickerSelectRow(p, GET_Y_LPARAM(lp), false);
            return 0;
        case WM_LBUTTONDBLCLK:
            FontPickerSelectRow(p, GET_Y_LPARAM(lp), true);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_DOWN) {
                FontPickerMoveSel(p, 1);
                return 0;
            }
            if (wp == VK_UP) {
                FontPickerMoveSel(p, -1);
                return 0;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProcFontPicker(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = (FontPickerWnd*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        p = (FontPickerWnd*)cs->lpCreateParams;
        p->hwnd = hwnd;
        FontPickerRecreateThemeBrushes(p);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)p);
    }
    if (!p) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (msg == WM_ERASEBKGND) {
        if (AppDialogHandleEraseBkgnd(wp, hwnd, p->bgBrush)) {
            return 1;
        }
    }
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORBTN || msg == WM_CTLCOLORDLG) {
        HBRUSH br = AppDialogCtlColorBrush(msg, wp, lp, p->bgBrush, p->ctrlBrush, p->hwndSearch);
        if (br) {
            return (LRESULT)br;
        }
    }
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (code == EN_CHANGE && (HWND)lp == p->hwndSearch) {
                FontPickerRebuildVisible(p);
                FontPickerEnsureSelVisible(p);
                if (p->hwndList) {
                    InvalidateRect(p->hwndList, nullptr, FALSE);
                }
                return 0;
            }
            if (id == IDOK) {
                FontPickerApplySelection(p);
                return 0;
            }
            if (id == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_ACTIVATE:
            SetCurrentModelessDialog(LOWORD(wp) == WA_INACTIVE ? nullptr : hwnd);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            FontPickerScrollList(p, delta > 0 ? -3 : 3);
            return 0;
        }
        case WM_DPICHANGED: {
            int dpi = (int)LOWORD(wp);
            if (dpi <= 0) {
                dpi = 96;
            }
            RECT* suggested = (RECT*)lp;
            AppDialogFonts fonts;
            fonts.CreateForDpi(dpi);
            if (fonts.body) {
                DeleteObject(p->uiFont);
                DeleteObject(p->uiFontBold);
                p->uiFont = fonts.body;
                p->uiFontBold = fonts.semibold;
                fonts.body = fonts.semibold = nullptr;
                AppDialogApplyFontToChildren(hwnd, p->uiFont);
            }
            Size outer = FontPickerLayout(p, dpi);
            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int x = 0;
            int y = 0;
            if (suggested) {
                x = suggested->left;
                y = suggested->top;
            } else {
                flags |= SWP_NOMOVE;
            }
            if (!outer.IsEmpty()) {
                SetWindowPos(hwnd, nullptr, x, y, outer.dx, outer.dy, flags);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            if (gFontPicker == p) {
                gFontPicker = nullptr;
            }
            UnregisterAppDialogForTheme(hwnd);
            if (GetCurrentModelessDialog() == hwnd) {
                SetCurrentModelessDialog(nullptr);
            }
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            FontPickerFree(p);
            delete p;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct FontPickExtra {
    bool both = false;
    const char* current = nullptr;
    FontFamilyPickedFn onPick = nullptr;
    void* ctx = nullptr;
};

static FontPickExtra gFontPickExtra;

static void AppendPickerFontFamilies(FontPickerWnd* p, int origCmd) {
    Vec<CustomCommand*> cmds;
    GetCommandsWithOrigId(cmds, origCmd);
    for (CustomCommand* cmd : cmds) {
        const char* family = GetCommandStringArg(cmd, kCmdArgFontFamily, nullptr);
        if (family && family[0]) {
            p->names.Append(str::Dup(family));
        }
    }
}

bool HwndBelongsToFontPicker(HWND hwnd) {
    if (!hwnd || !gFontPicker || !gFontPicker->hwnd) {
        return false;
    }
    return hwnd == gFontPicker->hwnd || IsChild(gFontPicker->hwnd, hwnd);
}

void ShowFreeTextFontPicker(HWND owner, const char* currentFamily, FontFamilyPickedFn onPick, void* ctx) {
    const char* shown = currentFamily;
    if (!currentFamily || str::Eq(currentFamily, "Helv")) {
        shown = "Arial";
    } else if (str::Eq(currentFamily, "Cour")) {
        shown = "Courier New";
    } else if (str::Eq(currentFamily, "TiRo")) {
        shown = "Times New Roman";
    }
    gFontPickExtra.both = true;
    gFontPickExtra.current = shown;
    gFontPickExtra.onPick = onPick;
    gFontPickExtra.ctx = ctx;
    ShowEbookFontPicker(owner, false);
}

void ShowEbookFontPicker(HWND owner, bool cjk) {
    if (gFontPicker && IsWindow(gFontPicker->hwnd)) {
        DestroyWindow(gFontPicker->hwnd);
        gFontPicker = nullptr;
    }
    HINSTANCE inst = GetModuleHandle(nullptr);
    if (!gFontPickerAtom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProcFontPicker;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kFontPickerClass;
        gFontPickerAtom = RegisterClassExW(&wc);
    }
    if (!gFontPickerListAtom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProcFontPickerList;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kFontPickerListClass;
        gFontPickerListAtom = RegisterClassExW(&wc);
    }
    auto* p = new FontPickerWnd();
    p->cjk = cjk;
    p->bundledCount = cjk ? gEbookCjkBundledMenuCount : gEbookLatinBundledMenuCount;
    bool both = gFontPickExtra.both;
    if (both) {
        const char* recent[3]{};
        int nRecent = GetFreeTextRecentFonts(recent, 3);
        for (int i = 0; i < nRecent; i++) {
            if (recent[i] && recent[i][0]) {
                p->names.Append(str::Dup(recent[i]));
            }
        }
        p->recentCount = p->names.Size();
    }
    char pickedCurrent[128]{};
    if (both) {
        if (gFontPickExtra.current) {
            str::BufSet(pickedCurrent, dimof(pickedCurrent), gFontPickExtra.current);
        }
        p->onPick = gFontPickExtra.onPick;
        p->pickCtx = gFontPickExtra.ctx;
        AppendPickerFontFamilies(p, CmdSetEbookLatinFont);
        p->bundledCount = p->names.Size();
        AppendPickerFontFamilies(p, CmdSetEbookCjkFont);
        gFontPickExtra = {};
    } else {
        AppendPickerFontFamilies(p, cjk ? CmdSetEbookCjkFont : CmdSetEbookLatinFont);
    }

    const WCHAR* title = both ? _TRW("Choose Font") : cjk ? _TRW("CJK Body Font") : _TRW("Western Body Font");
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kFontPickerClass, title,
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, inst, p);
    if (!hwnd) {
        FontPickerFree(p);
        delete p;
        return;
    }
    int dpi = DpiGet(hwnd);
    if (dpi <= 0 && owner) {
        dpi = DpiGet(owner);
    }
    if (dpi <= 0) {
        dpi = 96;
    }
    p->dpi = dpi;
    AppDialogFonts fonts;
    fonts.CreateForDpi(dpi);
    if (!fonts.body) {
        DestroyWindow(hwnd);
        return;
    }
    p->uiFont = fonts.body;
    p->uiFontBold = fonts.semibold;
    fonts.body = fonts.semibold = nullptr;

    // FontPickerSearchProc paints the outer frame. A native WS_BORDER also
    // outlines the centered text band on focus changes, creating a second box.
    p->hwndSearch = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                    hwnd, (HMENU)100, inst, nullptr);
    SendMessageW(p->hwndSearch, WM_SETFONT, (WPARAM)p->uiFont, TRUE);
    p->prevSearchProc = (WNDPROC)SetWindowLongPtrW(p->hwndSearch, GWLP_WNDPROC, (LONG_PTR)FontPickerSearchProc);
    SendMessageW(p->hwndSearch, EM_SETCUEBANNER, TRUE, (LPARAM)ToWStrTemp(_TRA("Search fonts")));

    p->hwndList =
        CreateWindowExW(0, kFontPickerListClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN,
                        0, 0, 0, 0, hwnd, nullptr, inst, nullptr);

    p->hwndOk = CreateWindowExW(0, L"BUTTON", _TRW("OK"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0,
                                0, 0, hwnd, (HMENU)(INT_PTR)IDOK, inst, nullptr);
    SendMessageW(p->hwndOk, WM_SETFONT, (WPARAM)p->uiFont, TRUE);
    p->hwndCancel = CreateWindowExW(0, L"BUTTON", _TRW("Cancel"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                    0, 0, 0, hwnd, (HMENU)(INT_PTR)IDCANCEL, inst, nullptr);
    SendMessageW(p->hwndCancel, WM_SETFONT, (WPARAM)p->uiFont, TRUE);

    FontPickerRebuildVisible(p);
    const char* current = both ? pickedCurrent : (cjk ? GetEbookCjkFontFamily() : GetEbookLatinFontFamily());
    p->sel = 0;
    for (int i = 0; i < p->visible.Size(); i++) {
        int nameIdx = p->visible[i];
        if (nameIdx < 0) {
            continue;
        }
        bool same = false;
        if (both) {
            same = current[0] && (str::EqI(p->names[nameIdx], current) ||
                                  EbookLatinFontFamiliesEquivalent(p->names[nameIdx], current) ||
                                  EbookCjkFontFamiliesEquivalent(p->names[nameIdx], current));
        } else {
            same = cjk ? EbookCjkFontFamiliesEquivalent(p->names[nameIdx], current)
                       : EbookLatinFontFamiliesEquivalent(p->names[nameIdx], current);
        }
        if (same) {
            p->sel = i;
            break;
        }
    }
    Size outer = FontPickerLayout(p, dpi);
    if (!outer.IsEmpty()) {
        SetWindowPos(hwnd, nullptr, 0, 0, outer.dx, outer.dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    gFontPicker = p;
    CenterDialog(hwnd, owner);
    AppDialogApplyChrome(hwnd);
    FontPickerApplyControlTheme(p);
    RegisterAppDialogForTheme(hwnd, FontPickerThemeRefreshCb, p);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(p->hwndSearch);
}
