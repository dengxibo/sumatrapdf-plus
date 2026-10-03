/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
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
#include "EbookInstalledFonts.h"

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
    Vec<char*> names;
    Vec<int> visible; // index into names, or -1 for the separator
    int sel = 0;
    int rowDy = 22;
    HFONT uiFont = nullptr;
    HFONT uiFontBold = nullptr;
    HBRUSH bgBrush = nullptr;
    HBRUSH ctrlBrush = nullptr;
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

static bool FontFaceCanDraw(HDC hdc, HFONT font, const WCHAR* text) {
    if (!font || !text || !text[0]) {
        return false;
    }
    HFONT old = (HFONT)SelectObject(hdc, font);
    WORD glyphs[8];
    int n = (int)wcsnlen(text, 8);
    DWORD got = GetGlyphIndicesW(hdc, text, n, glyphs, GGI_MARK_NONEXISTING_GLYPHS);
    bool ok = got != GDI_ERROR;
    if (ok) {
        for (int i = 0; i < n; i++) {
            if (glyphs[i] == 0xFFFF) {
                ok = false;
                break;
            }
        }
    }
    SelectObject(hdc, old);
    return ok;
}

static HFONT CreateFamilyFont(const char* family, int dpi) {
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(11, dpi, 96);
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    TempWStr face = ToWStrTemp(family);
    wcsncpy_s(lf.lfFaceName, face ? face : L"", _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

static void FontPickerRebuildVisible(FontPickerWnd* p) {
    p->visible.Reset();
    char* query = HwndGetTextTemp(p->hwndSearch);
    int shownBundled = 0;
    for (int i = 0; i < p->bundledCount && i < p->names.Size(); i++) {
        if (str::IsEmpty(query) || str::ContainsI(p->names[i], query)) {
            p->visible.Append(i);
            shownBundled++;
        }
    }
    bool anyInstalled = false;
    for (int i = p->bundledCount; i < p->names.Size(); i++) {
        if (str::IsEmpty(query) || str::ContainsI(p->names[i], query)) {
            anyInstalled = true;
            break;
        }
    }
    if (shownBundled > 0 && anyInstalled) {
        p->visible.Append(-1);
    }
    for (int i = p->bundledCount; i < p->names.Size(); i++) {
        if (str::IsEmpty(query) || str::ContainsI(p->names[i], query)) {
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
    int orig = p->cjk ? CmdSetEbookCjkFont : CmdSetEbookLatinFont;
    int cmdId = FindFontMenuCmdId(orig, p->names[nameIdx]);
    HWND owner = GetWindow(p->hwnd, GW_OWNER);
    DestroyWindow(p->hwnd);
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
    COLORREF bg = ThemeWindowBackgroundColor();
    if (ThemeUsesDarkChrome()) {
        return AccentColor(bg, 14);
    }
    return AccentColor(bg, 8);
}

static COLORREF FontPickerSelColor() {
    COLORREF bg = ThemeWindowBackgroundColor();
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
    p->bgBrush = CreateSolidBrush(ThemeWindowBackgroundColor());
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
    int btnH = MulDiv(23, dpi, 96);
    int btnW = MulDiv(75, dpi, 96);
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
            HFONT face = CreateFamilyFont(family, p->dpi);
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
    int orig = cjk ? CmdSetEbookCjkFont : CmdSetEbookLatinFont;
    Vec<CustomCommand*> cmds;
    GetCommandsWithOrigId(cmds, orig);
    for (CustomCommand* cmd : cmds) {
        const char* family = GetCommandStringArg(cmd, kCmdArgFontFamily, nullptr);
        if (family && family[0]) {
            p->names.Append(str::Dup(family));
        }
    }

    const WCHAR* title = cjk ? _TRW("CJK Body Font") : _TRW("Western Body Font");
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

    p->hwndSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, hwnd, (HMENU)100, inst, nullptr);
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
    const char* current = cjk ? GetEbookCjkFontFamily() : GetEbookLatinFontFamily();
    p->sel = 0;
    for (int i = 0; i < p->visible.Size(); i++) {
        int nameIdx = p->visible[i];
        if (nameIdx < 0) {
            continue;
        }
        bool same = cjk ? EbookCjkFontFamiliesEquivalent(p->names[nameIdx], current)
                        : EbookLatinFontFamiliesEquivalent(p->names[nameIdx], current);
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
