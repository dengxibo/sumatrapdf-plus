/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "DocController.h"
#include "EngineBase.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "Commands.h"
#include "Accelerators.h"
#include "SvgIcons.h"
#include "Toolbar.h"
#include "SearchAndDDE.h"
#include "FindBar.h"
#include "AppSettings.h"
#include "FindWindow.h"
#include "CommandPalette.h" // DrawMaybeHighlightedText
#include "Translations.h"
#include "Theme.h"
#include "FloatingPopupStyle.h"
#include "DarkModeSubclass.h"

#include "utils/Log.h"

// Match the frame's caption to the current theme, including Light-Warm.
static void ApplyTitleBarTheme(HWND hwnd) {
    UpdateWindowCaptionTheme(hwnd);
}

static COLORREF BlendColor(COLORREF background, COLORREF foreground, int foregroundPercent) {
    int backgroundPercent = 100 - foregroundPercent;
    u8 br, bg, bb, fr, fg, fb;
    UnpackColor(background, br, bg, bb);
    UnpackColor(foreground, fr, fg, fb);
    return MkColor((u8)((br * backgroundPercent + fr * foregroundPercent) / 100),
                   (u8)((bg * backgroundPercent + fg * foregroundPercent) / 100),
                   (u8)((bb * backgroundPercent + fb * foregroundPercent) / 100));
}

// default floating find window size at 96 dpi (saved pos overrides on later opens)
constexpr int kFindWindowDefaultDx = 560;
constexpr int kFindWindowDefaultDy = 400;

// command ids for the window's toolbar buttons (handled in OnCommand)
constexpr int kFindWinPinCmdId = (int)CmdLast + 51;
constexpr int kFindWinCloseCmdId = (int)CmdLast + 53;

struct FindWindowWnd;

struct DeferredGoToFindMatchData {
    MainWindow* win = nullptr;
    FindWindowWnd* findWindow = nullptr;
    int startPage = 0;
    int startGlyph = 0;
    int endPage = 0;
    int endGlyph = 0;
    LONG epoch = 0;
};

// list model backed live by win->findMatches (the snippet for each match)
struct FindResultsModel : ListBoxModel {
    MainWindow* win = nullptr;
    explicit FindResultsModel(MainWindow* win) { this->win = win; }
    int ItemsCount() override { return (int)win->findMatches.size(); }
    const char* Item(int i) override {
        const char* s = win->findMatches[i].snippet;
        return s ? s : "";
    }
};

struct FindWindowWnd : Wnd {
    MainWindow* win = nullptr;
    Edit* edit = nullptr;
    Static* status = nullptr;
    COLORREF statusTxtCol = 0;
    COLORREF statusBgCol = 0;
    HWND hwndBtns = nullptr; // prev / next / match-case / unpin(dock)
    HIMAGELIST himl = nullptr;
    ListBox* results = nullptr;
    StrVec filterWords; // search term(s) to highlight in snippets
    Vec<u8> hlScratch;  // reused highlight mask for DrawMaybeHighlightedText
    // coalesce rapid list selections: only the latest deferred navigation runs
    LONG pendingNavEpoch = 0;
    // in an interactive size/move loop (between WM_ENTERSIZEMOVE/EXITSIZEMOVE)
    bool inSizeMove = false;
    // list redraw is paused only while interactively *resizing* (a WM_SIZE
    // arrived during the size/move loop), not while merely moving the window
    bool listRedrawPaused = false;
    bool resizedDuringMove = false;
    bool editHasFocus = false;
    bool suppressTextChanged = false;
    bool docked = false;
    int lastDpi = 0;
    int lastSnippetGlyphBudget = 0;
    int lastClientCx = 0;
    int lastClientCy = 0;
    LONG displayedResultsEpoch = 0;
    bool displayedResultsInitialized = false;
    // true when the on-screen list rows are already in the sorted (document
    // order) layout of displayedResultsEpoch, so a same-count refresh can
    // skip the full LB_RESETCONTENT rebuild (which makes the list flash)
    bool displayedResultsSorted = false;
    int displayedSnippetBudget = -1;
    u32 displayedTextCacheGen = 0;
    bool hasPendingNavigation = false;
    Rect closeRc;
    bool closeHover = false;
    bool closePressed = false;
    int hoverResult = -1;
    // Same overlay bar as the document canvas. The list's own bar is hidden
    // while this is active, so the two styles cannot flash over each other.
    OverlayScrollbar* resultsScroll = nullptr;
    int chromeSeparatorY = -1;
    int dragBottom = 0;
    int pendingStartPage = 0;
    int pendingStartGlyph = 0;
    int pendingEndPage = 0;
    int pendingEndGlyph = 0;
    LONG pendingNavigationCountEpoch = 0;

    FindWindowWnd() = default;
    ~FindWindowWnd() override;

    bool Create(MainWindow* win);
    void Layout();
    void SavePos();
    void RefreshResults(bool allowNavigation = true);
    void EnsureResultsListRedraw();
    void UpdateTheme();
    void RefreshToolbarDpi();
    void RefreshNonClientChrome();
    void SyncDpi(bool force = false, int explicitDpi = 0);
    void FlashStatusText(bool flash);
    void DrawEditUnderline(HDC hdc);
    void DrawSearchChrome(HDC hdc);
    void ApplySurfaceColors();
    void InvalidateCloseButton();
    void SetDocked(bool dock);
    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;

    void OnTextChanged();
    void DrawResultItem(ListBox::DrawItemEvent* ev);
    void OnResultSelected();
    bool MoveResultSelection(WPARAM vkey);
    int CurrentMatchIndex();         // list index of the document's current match, or -1
    int FirstMatchFromCurrentPage(); // list index of the first match at/after the current page

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;
    LRESULT OnNotify(int controlId, NMHDR* nmh) override;
    bool PreTranslateMessage(MSG& msg) override;
    bool OnCommand(WPARAM wparam, LPARAM lparam) override;
};

static void DeferredGoToFindMatch(DeferredGoToFindMatchData* d) {
    AutoDelete del(d);
    MainWindow* win = d->win;
    if (!IsMainWindowValid(win) || !d->findWindow) {
        return;
    }
    if (win->findWindow != d->findWindow) {
        return;
    }
    if (d->epoch != d->findWindow->pendingNavEpoch) {
        return;
    }
    if (d->findWindow->pendingNavigationCountEpoch != win->findCountEpoch) {
        d->findWindow->hasPendingNavigation = false;
        return;
    }
    if (!win->IsDocLoaded()) {
        d->findWindow->hasPendingNavigation = false;
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSearch || !dm->GetEngine()) {
        d->findWindow->hasPendingNavigation = false;
        return;
    }
    if (win->findCountEngine && (void*)dm->GetEngine() != win->findCountEngine) {
        d->findWindow->hasPendingNavigation = false;
        return;
    }
    if (win->findCountThread) {
        EngineBase* engine = dm->GetEngine();
        logf("search: navigate during count epoch=%ld match=%d:%d-%d:%d pageCount=%d\n", (long)d->epoch, d->startPage,
             d->startGlyph, d->endPage, d->endGlyph, engine->PageCount());
        // The count worker has already extracted and cached every page touched
        // by this match. Cached text access is protected by EngineBase's text
        // cache lock, so exact selection can proceed without stopping or
        // restarting the full-document scan.
        if (engine->PromoteCachedTextUtf8ForSelection(d->startPage) &&
            engine->PromoteCachedTextUtf8ForSelection(d->endPage)) {
            d->findWindow->hasPendingNavigation = false;
            GoToFindMatch(win, d->startPage, d->startGlyph, d->endPage, d->endGlyph);
            return;
        }
        if (win->ctrl) {
            win->ctrl->GoToPage(d->startPage, true);
        }
        d->findWindow->hasPendingNavigation = false;
        return;
    }
    d->findWindow->hasPendingNavigation = false;
    GoToFindMatch(win, d->startPage, d->startGlyph, d->endPage, d->endGlyph);
}

// append a command's keyboard shortcut to its tooltip, e.g. "Find Next (F3)"
static const char* AppendCmdAccel(const char* base, int cmd) {
    const char* accel = AppendAccelKeyToMenuStringTemp(nullptr, cmd);
    if (!accel) {
        return base;
    }
    return str::JoinTemp(base, str::FormatTemp(" (%s)", accel + 1)); // +1 skips the leading \t
}

static const char* FindWindowButtonTooltip(int cmd) {
    switch (cmd) {
        case CmdFindPrev:
            return AppendCmdAccel(_TRA("Find Previous"), cmd);
        case CmdFindNext:
            return AppendCmdAccel(_TRA("Find Next"), cmd);
        case CmdFindToggleMatchCase:
            return AppendCmdAccel(_TRA("Match Case"), cmd);
        case CmdFindToggleMatchWholeWord:
            return AppendCmdAccel(_TRA("Match Whole Word"), cmd);
        case kFindWinPinCmdId:
            return nullptr;
        case kFindWinCloseCmdId:
            return _TRA("Close");
    }
    return nullptr;
}

constexpr UINT_PTR kFindResultsSubclassId = 0x46696E64;

static COLORREF FindSurfaceBg(bool docked) {
    if (docked) {
        return ThemeWindowControlBackgroundColor();
    }
    // Warm / White / Darcula / Black tool-panel surface, not a fixed gray.
    return FloatingToolPanelBg();
}

static COLORREF FindSurfaceText(bool docked) {
    return docked ? ThemeWindowTextColor() : FloatingPopupTextColor();
}

static COLORREF FindSurfaceMuted(bool docked) {
    return docked ? ThemeWindowTextDisabledColor() : FloatingPopupMutedTextColor();
}

static void InvalidateFindResultRow(HWND hwnd, int idx) {
    if (!hwnd || idx < 0) {
        return;
    }
    RECT ir{};
    if (ListBox_GetItemRect(hwnd, idx, &ir) != LB_ERR) {
        InvalidateRect(hwnd, &ir, FALSE);
    }
}

static bool FindResultsHideNativeScroll() {
    return ScrollbarsAreHidden() || ScrollbarsUseOverlay();
}

// List-box scroll info is in items. When the native bar is hidden, Windows
// sometimes reports an empty range, so fall back to the top index.
static void FindListScrollInfo(HWND hwnd, SCROLLINFO* si) {
    si->cbSize = sizeof(*si);
    si->fMask = SIF_POS | SIF_RANGE | SIF_PAGE;
    si->nMin = 0;
    si->nMax = 0;
    si->nPage = 1;
    si->nPos = 0;
    int count = (int)SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
    if (count < 0) {
        count = 0;
    }
    int itemH = (int)SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int page = 1;
    if (itemH > 0) {
        page = std::max(1, (int)(rc.bottom - rc.top) / itemH);
    }
    int top = (int)SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0);
    if (top < 0) {
        top = 0;
    }
    if (count > 0 && top > count - 1) {
        top = count - 1;
    }
    si->nMin = 0;
    si->nMax = std::max(0, count - 1);
    si->nPage = (UINT)page;
    si->nPos = top;
}

static void SyncFindResultsScrollbar(FindWindowWnd* w) {
    if (!w || !w->results || !w->results->hwnd) {
        return;
    }
    // ShowScrollBar posts a size back into this list. Don't re-enter.
    static bool syncing = false;
    if (syncing) {
        return;
    }
    syncing = true;
    HWND hwnd = w->results->hwnd;
    bool hide = ScrollbarsAreHidden();
    bool overlay = ScrollbarsUseOverlay() && !hide;
    if (overlay || hide) {
        ShowScrollBar(hwnd, SB_VERT, FALSE);
    }

    SCROLLINFO si{};
    FindListScrollInfo(hwnd, &si);
    int range = si.nMax - si.nMin + 1;
    bool canScroll = range > 1 && (int)si.nPage < range;

    if (overlay) {
        if (!w->resultsScroll) {
            w->resultsScroll = OverlayScrollbarCreate(hwnd, OverlayScrollbar::Type::Vert, ScrollbarsOverlayMode());
        } else {
            OverlayScrollbarSetMode(w->resultsScroll, ScrollbarsOverlayMode());
        }
        if (canScroll) {
            OverlayScrollbarShow(w->resultsScroll, true);
            OverlayScrollbarSetInfo(w->resultsScroll, &si, TRUE);
            OverlayScrollbarUpdatePos(w->resultsScroll);
        } else {
            OverlayScrollbarShow(w->resultsScroll, false);
        }
    } else {
        OverlayScrollbarShow(w->resultsScroll, false);
        if (!hide && canScroll) {
            if (UseDarkModeLib()) {
                if (ThemeUsesDarkChrome()) {
                    DarkMode::setDarkScrollBar(hwnd);
                } else {
                    SetWindowTheme(hwnd, nullptr, nullptr);
                    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
                }
            }
            ShowScrollBar(hwnd, SB_VERT, TRUE);
        } else {
            ShowScrollBar(hwnd, SB_VERT, FALSE);
        }
    }
    syncing = false;
}

static LRESULT CALLBACK FindResultsSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* w = (FindWindowWnd*)ref;
    if (!w) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    // The document canvas hides its window scrollbar and draws OverlayScrollbar.
    // Swallow the list's non-client bar so it cannot flash a second style.
    if (FindResultsHideNativeScroll()) {
        if (msg == WM_NCCALCSIZE && wp) {
            return 0;
        }
        if (msg == WM_NCPAINT) {
            return 0;
        }
    }
    if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL || msg == WM_KEYDOWN) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        SyncFindResultsScrollbar(w);
        return r;
    }
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
        int idx = -1;
        if (msg == WM_MOUSEMOVE) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
            if (HIWORD(hit) == 0) {
                idx = (int)LOWORD(hit);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        if (idx != w->hoverResult) {
            int old = w->hoverResult;
            w->hoverResult = idx;
            InvalidateFindResultRow(hwnd, old);
            InvalidateFindResultRow(hwnd, idx);
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void FindSnippetHighlightColors(COLORREF listBg, COLORREF* primary, COLORREF* secondary) {
    ParsedColor* parsed = GetPrefsColor(gGlobalPrefs->fixedPageUI.findMatchColor);
    COLORREF src = (parsed && parsed->parsedOk) ? parsed->col : ThemeWindowLinkColor();
    if (ThemeUsesDarkChrome()) {
        // Keep the match hue, but pull it toward the palette so Darcula / Black
        // do not reuse the light-theme neon yellow.
        int mix = ThemeUsesBlackChrome() ? 68 : 74;
        *primary = BlendColor(listBg, src, mix);
        *secondary = BlendColor(listBg, src, 40);
    } else {
        *primary = src;
        *secondary = BlendColor(listBg, src, 48);
    }
}

static LRESULT PaintFindPaletteToolbarItem(NMTBCUSTOMDRAW* cd, COLORREF bgCol) {
    UINT itemState = cd->nmcd.uItemState;
    COLORREF txtCol = FloatingPopupTextColor();
    if (itemState & CDIS_DISABLED) {
        txtCol = FloatingPopupMutedTextColor();
    }
    cd->clrText = txtCol;
    cd->clrTextHighlight = txtCol;
    cd->clrBtnFace = bgCol;
    cd->clrBtnHighlight = bgCol;
    cd->clrHighlightHotTrack = bgCol;
    cd->nStringBkMode = TRANSPARENT;
    cd->nHLStringBkMode = TRANSPARENT;

    bool isSelected = (itemState & CDIS_SELECTED) != 0;
    bool isHot = (itemState & CDIS_HOT) != 0;
    cd->nmcd.uItemState &= ~(CDIS_CHECKED | CDIS_SELECTED);

    HWND hwndToolbar = cd->nmcd.hdr.hwndFrom;
    int cmd = (int)cd->nmcd.dwItemSpec;
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_STATE;
    SendMessageW(hwndToolbar, TB_GETBUTTONINFOW, cmd, (LPARAM)&tbi);
    bool isChecked = (tbi.fsState & TBSTATE_CHECKED) != 0;

    COLORREF fill = bgCol;
    if (isChecked || isSelected) {
        fill = FloatingPopupCloseHoverBg(bgCol);
    } else if (isHot) {
        fill = FloatingPopupHoverBg(bgCol);
    }
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(cd->nmcd.hdc, &cd->nmcd.rc, br);
    DeleteObject(br);
    return TBCDRF_USECDCOLORS | TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES;
}

FindWindowWnd::~FindWindowWnd() {
    EnsureResultsListRedraw();
    if (results && results->hwnd) {
        RemoveWindowSubclass(results->hwnd, FindResultsSubclassProc, kFindResultsSubclassId);
    }
    OverlayScrollbarDestroy(resultsScroll);
    resultsScroll = nullptr;
    delete edit;
    delete status;
    delete results; // also deletes its FindResultsModel
    HwndDestroyWindowSafe(&hwndBtns);
    if (himl) {
        ImageList_Destroy(himl);
    }
}

bool FindWindowWnd::Create(MainWindow* mainWin) {
    win = mainWin;

    // Floating Detailed Search is a palette, not a captioned dialog. The red
    // close was the system caption button (gray when the window was inactive).
    auto colBg = FindSurfaceBg(false);
    auto colTxt = FindSurfaceText(false);

    {
        CreateCustomArgs args;
        args.visible = false;
        args.title = _TRA("Find");
        // WS_CLIPCHILDREN neutralizes CS_PARENTDC of the standard controls
        // (their DCs get clipped to the control, not to this window), so e.g.
        // the results listbox can't paint its partially visible bottom row
        // below itself onto this window. WS_THICKFRAME keeps resize; the
        // non-client band is removed in WM_NCCALCSIZE.
        args.style = WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN;
        args.exStyle = WS_EX_TOOLWINDOW; // off the taskbar, owned by the frame
        args.isRtl = IsUIRtl();
        CreateCustom(args);
    }
    if (!hwnd) {
        return false;
    }
    // owned by the frame so it groups/minimizes with it but isn't a child
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)win->hwndFrame);
    SetColors(colTxt, colBg);

    {
        Edit::CreateArgs args;
        args.parent = hwnd;
        args.isMultiLine = false;
        // A native client edge turns into a bright rectangular outline in dark
        // themes. Keep the edit surface borderless and draw a restrained
        // focus underline in the parent instead.
        args.withBorder = false;
        args.cueText = _TRA("Find");
        args.isRtl = IsUIRtl();
        edit = new Edit();
        edit->maxDx = DpiScale(hwnd, 1000);
        edit->SetColors(colTxt, colBg);
        edit->Create(args);
        edit->onTextChanged = MkMethod0<FindWindowWnd, &FindWindowWnd::OnTextChanged>(this);
        InstallFindEditKeyboardHandler(win, edit->hwnd);
    }

    {
        Static::CreateArgs args;
        args.parent = hwnd;
        args.text = "";
        args.isRtl = IsUIRtl();
        status = new Static();
        statusTxtCol = FindSurfaceMuted(false);
        statusBgCol = colBg;
        status->SetColors(statusTxtCol, colBg);
        status->Create(args);
        SetWindowStyle(status->hwnd, SS_CENTERIMAGE, true);
    }

    {
        DWORD style = WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TOOLTIPS | CCS_NODIVIDER |
                      CCS_NORESIZE | CCS_NOPARENTALIGN;
        DWORD exStyle = IsUIRtl() ? WS_EX_LAYOUTRTL : 0;
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        hwndBtns = CreateWindowExW(exStyle, TOOLBARCLASSNAMEW, nullptr, style, 0, 0, 0, 0, hwnd, (HMENU) nullptr, hinst,
                                   nullptr);
        // drop the visual-style button background so the flat toolbar shows the
        // window's themed background instead of a light box in dark themes
        SetWindowTheme(hwndBtns, L"", L"");
        // Custom draw is only called once painting starts. Set the native
        // toolbar background too, so its initial surface is never system white.
        SendMessageW(hwndBtns, CCM_SETBKCOLOR, 0, (LPARAM)colBg);
        SendMessageW(hwndBtns, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);

        int icon = RoundUp(DpiScale(hwnd, 16), 4);
        int slot = std::max(icon, DpiScale(hwnd, 26));
        himl = BuildStdToolbarImageList(icon);
        SendMessageW(hwndBtns, TB_SETIMAGELIST, 0, (LPARAM)himl);
        SendMessageW(hwndBtns, TB_SETBUTTONSIZE, 0, MAKELONG(slot, slot));

        TBBUTTON b[6]{};
        b[0].iBitmap = (int)TbIcon::ChevronUp;
        b[0].idCommand = CmdFindPrev;
        b[0].fsState = TBSTATE_ENABLED;
        b[0].fsStyle = BTNS_BUTTON;
        b[1].iBitmap = (int)TbIcon::ChevronDown;
        b[1].idCommand = CmdFindNext;
        b[1].fsState = TBSTATE_ENABLED;
        b[1].fsStyle = BTNS_BUTTON;
        b[2].iBitmap = (int)TbIcon::MatchCase;
        b[2].idCommand = CmdFindToggleMatchCase;
        b[2].fsState = TBSTATE_ENABLED;
        b[2].fsStyle = BTNS_CHECK;
        b[3].iBitmap = (int)TbIcon::MatchWholeWord;
        b[3].idCommand = CmdFindToggleMatchWholeWord;
        b[3].fsState = TBSTATE_ENABLED;
        b[3].fsStyle = BTNS_CHECK;
        b[4].iBitmap = (int)TbIcon::ArrowsDiagonalMinimize;
        b[4].idCommand = kFindWinPinCmdId;
        b[4].fsState = TBSTATE_ENABLED;
        b[4].fsStyle = BTNS_BUTTON;
        b[5].iBitmap = (int)TbIcon::Close;
        b[5].idCommand = kFindWinCloseCmdId;
        b[5].fsState = TBSTATE_ENABLED;
        b[5].fsStyle = BTNS_BUTTON;
        SendMessageW(hwndBtns, TB_ADDBUTTONS, 6, (LPARAM)&b);
        // Find stays in the toolbar, so the floating window has no "back to toolbar" button.
        SendMessageW(hwndBtns, TB_HIDEBUTTON, kFindWinPinCmdId, MAKELONG(TRUE, 0));
        SendMessageW(hwndBtns, TB_HIDEBUTTON, kFindWinCloseCmdId, MAKELONG(TRUE, 0));
        SendMessageW(hwndBtns, TB_AUTOSIZE, 0, 0);
    }

    {
        ListBox::CreateArgs args;
        args.parent = hwnd;
        args.font = GetDefaultGuiFont();
        results = new ListBox();
        results->onDrawItem = MkMethod1<FindWindowWnd, ListBox::DrawItemEvent*, &FindWindowWnd::DrawResultItem>(this);
        results->onSelectionChanged = MkMethod0<FindWindowWnd, &FindWindowWnd::OnResultSelected>(this);
        results->onDoubleClick = MkMethod0<FindWindowWnd, &FindWindowWnd::OnResultSelected>(this);
        args.itemHeightExtra = 6;
        results->SetColors(colTxt, colBg);
        results->Create(args);
        results->SetModel(new FindResultsModel(win));
        SetWindowSubclass(results->hwnd, FindResultsSubclassProc, kFindResultsSubclassId, (DWORD_PTR)this);
        SetWindowPos(results->hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SyncFindResultsScrollbar(this);
    }

    lastDpi = DpiGetForMonitorOfHwnd(hwnd);
    if (lastDpi <= 0) {
        lastDpi = DpiGet(hwnd);
    }
    return true;
}

static int EffectiveDpiForFindWindow(HWND hwnd, int explicitDpi) {
    if (explicitDpi > 0) {
        return RoundUp(explicitDpi, 4);
    }
    int monDpi = DpiGetForMonitorOfHwnd(hwnd);
    if (monDpi > 0) {
        return RoundUp(monDpi, 4);
    }
    return DpiGet(hwnd);
}

static int FindWindowDpiScale(FindWindowWnd* w, int x) {
    int dpi = w->lastDpi > 0 ? w->lastDpi : DpiGet(w->hwnd);
    return MulDiv(x, dpi, 96);
}

static COLORREF FlashFindStatusTextColor(COLORREF normal) {
    return BlendColor(normal, MkColor(255, 255, 255), 40);
}

void FindWindowWnd::FlashStatusText(bool flash) {
    if (!status) {
        return;
    }
    if (flash) {
        status->SetColors(FlashFindStatusTextColor(statusTxtCol), statusBgCol);
    } else {
        status->SetColors(statusTxtCol, statusBgCol);
    }
}

struct RebuildSnippetsTaskData {
    MainWindow* win = nullptr;
};

static void RebuildSnippetsTask(RebuildSnippetsTaskData* d) {
    AutoDelete delData(d);
    if (!IsMainWindowValid(d->win) || !IsFindWindowVisible(d->win)) {
        return;
    }
    RebuildFindMatchSnippets(d->win);
}

void FindWindowWnd::Layout() {
    // a WS_CAPTION/WS_THICKFRAME window gets WM_SIZE during CreateCustom, before
    // the child controls exist; ignore layout until they're created
    if (!edit || !status || !hwndBtns || !results) {
        return;
    }
    Rect rc = ClientRect(hwnd);
    int pad = FindWindowDpiScale(this, docked ? 8 : 18);
    int gap = FindWindowDpiScale(this, 6);
    closeRc = Rect();
    chromeSeparatorY = -1;
    dragBottom = 0;
    int statusDx = FindWindowDpiScale(this, 64);
    if (status && status->hwnd) {
        const char* countSample = "99999 / 99999";
        Size countSz = HwndMeasureText(status->hwnd, countSample, status->font);
        int measured = countSz.dx + FindWindowDpiScale(this, 4);
        statusDx = std::max(statusDx, measured);
    }
    int minEditDx = FindWindowDpiScale(this, 48);

    int editDy = edit->GetIdealSize().dy;
    SIZE tbSz{};
    SendMessageW(hwndBtns, TB_GETMAXSIZE, 0, (LPARAM)&tbSz);
    int tbW = (int)tbSz.cx;
    int tbH = (int)tbSz.cy;

    // A non-client style change can leave native child windows hidden until a
    // later redraw. Both forms use these exact same controls, so make their
    // visibility explicit on every layout pass.
    ShowWindow(edit->hwnd, SW_SHOWNA);
    ShowWindow(status->hwnd, SW_SHOWNA);
    ShowWindow(hwndBtns, SW_SHOWNA);

    if (docked) {
        int editDx = FindWindowDpiScale(this, 220);
        int desiredDx = 2 * pad + editDx + gap + statusDx + gap + tbW;
        Rect frameRect = WindowVisibleRect(win->hwndFrame);
        int maxDx = frameRect.dx - FindWindowDpiScale(this, 8);
        if (maxDx > 0 && desiredDx > maxDx) {
            editDx = std::max(minEditDx, editDx - (desiredDx - maxDx));
        }
        int headerDy = std::max(editDy, tbH);
        int outerDx = 2 * pad + editDx + gap + statusDx + gap + tbW;
        int outerDy = 2 * pad + headerDy;
        MoveWindow(edit->hwnd, pad, pad + (headerDy - editDy) / 2, editDx, editDy, TRUE);
        int x = pad + editDx + gap;
        MoveWindow(status->hwnd, x, pad + (headerDy - editDy) / 2, statusDx, editDy, TRUE);
        x += statusDx + gap;
        MoveWindow(hwndBtns, x, pad + (headerDy - tbH) / 2, tbW, tbH, TRUE);
        ShowWindow(results->hwnd, SW_HIDE);
        SetWindowPos(hwnd, nullptr, 0, 0, outerDx, outerDy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    ShowWindow(results->hwnd, SW_SHOWNA);

    int closeSz = FindWindowDpiScale(this, 16);
    int closeGap = FindWindowDpiScale(this, 8);
    int trail = closeSz + closeGap;
    int contentDx = std::max(0, rc.dx - 2 * pad);
    // minimum width for [edit][status][toolbar][close] on one row without overlap
    int singleRowDx = minEditDx + gap + statusDx + gap + tbW + trail;

    int y = pad;
    int headerDy;
    if (contentDx >= singleRowDx) {
        // wide: query, n/m, previous/next/options, close
        headerDy = std::max(editDy, std::max(tbH, closeSz));
        int tbX = pad + contentDx - trail - tbW;
        int statusX = tbX - gap - statusDx;
        int editDx = std::max(minEditDx, statusX - gap - pad);
        MoveWindow(hwndBtns, tbX, y + (headerDy - tbH) / 2, tbW, tbH, TRUE);
        MoveWindow(status->hwnd, statusX, y + (headerDy - editDy) / 2, statusDx, editDy, TRUE);
        MoveWindow(edit->hwnd, pad, y + (headerDy - editDy) / 2, editDx, editDy, TRUE);
        closeRc = Rect(pad + contentDx - closeSz, y + (headerDy - closeSz) / 2, closeSz, closeSz);
    } else {
        // narrow: full-width edit, then [n/m][toolbar][close] (issue #5692)
        MoveWindow(edit->hwnd, pad, y, contentDx, editDy, TRUE);
        y += editDy + gap;
        int row2Dy = std::max(editDy, std::max(tbH, closeSz));
        headerDy = editDy + gap + row2Dy;
        int tbX = pad + contentDx - trail - tbW;
        int statusW = std::max(0, tbX - gap - pad);
        MoveWindow(status->hwnd, pad, y + (row2Dy - editDy) / 2, statusW, editDy, TRUE);
        MoveWindow(hwndBtns, tbX, y + (row2Dy - tbH) / 2, tbW, tbH, TRUE);
        closeRc = Rect(pad + contentDx - closeSz, y + (row2Dy - closeSz) / 2, closeSz, closeSz);
    }

    // the results list fills the rest of the window below the header
    int listTop = pad + headerDy + pad;
    chromeSeparatorY = pad + headerDy + FindWindowDpiScale(this, 4);
    dragBottom = listTop;
    int listDy = std::max(0, rc.dy - listTop - pad);
    RECT listNow{};
    GetWindowRect(results->hwnd, &listNow);
    MapWindowPoints(nullptr, hwnd, (LPPOINT)&listNow, 2);
    bool listMoved = listNow.left != pad || listNow.top != listTop || (listNow.right - listNow.left) != contentDx ||
                     (listNow.bottom - listNow.top) != listDy;
    if (listMoved) {
        MoveWindow(results->hwnd, pad, listTop, contentDx, listDy, TRUE);
    }

    int newBudget = FindWindowSnippetGlyphBudget(win);
    if (lastSnippetGlyphBudget > 0 && newBudget > lastSnippetGlyphBudget + 10 && win->findCountHasSnippets &&
        win->findMatches.size() > 0) {
        auto d = new RebuildSnippetsTaskData;
        d->win = win;
        uitask::Post(MkFunc0<RebuildSnippetsTaskData>(RebuildSnippetsTask, d), "RebuildFindSnippets");
    }
    lastSnippetGlyphBudget = newBudget;
    SyncFindResultsScrollbar(this);
}

void FindWindowWnd::DrawEditUnderline(HDC hdc) {
    if (!edit || !edit->hwnd || !hdc) {
        return;
    }
    RECT r{};
    GetWindowRect(edit->hwnd, &r);
    MapWindowPoints(nullptr, hwnd, (LPPOINT)&r, 2);

    COLORREF bg = FindSurfaceBg(docked);
    COLORREF col =
        editHasFocus ? BlendColor(bg, ThemeWindowLinkColor(), 28) : AccentColor(bg, ThemeUsesDarkChrome() ? 30 : 22);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ old = SelectObject(hdc, pen);
    int y = r.bottom;
    MoveToEx(hdc, r.left, y, nullptr);
    LineTo(hdc, r.right, y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

void FindWindowWnd::InvalidateCloseButton() {
    if (closeRc.IsEmpty()) {
        return;
    }
    RECT r = ToRECT(closeRc);
    InflateRect(&r, 2, 2);
    InvalidateRect(hwnd, &r, FALSE);
}

void FindWindowWnd::DrawSearchChrome(HDC hdc) {
    if (!hdc) {
        return;
    }
    Rect rc = ClientRect(hwnd);
    int radius = DpiScale(hwnd, kFloatingPopupCornerRadius);
    StrokeFloatingPopupRoundedRect(hdc, rc, radius, FloatingToolPanelBorder());

    // Focus lives on the query only. Unfocused, the header stays quiet.
    if (edit && edit->hwnd && editHasFocus) {
        RECT r{};
        GetWindowRect(edit->hwnd, &r);
        MapWindowPoints(nullptr, hwnd, (LPPOINT)&r, 2);
        COLORREF col = BlendColor(FindSurfaceBg(false), ThemeWindowLinkColor(), ThemeUsesDarkChrome() ? 55 : 42);
        HPEN pen = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ old = SelectObject(hdc, pen);
        MoveToEx(hdc, r.left, r.bottom, nullptr);
        LineTo(hdc, r.right, r.bottom);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }

    if (chromeSeparatorY > 0) {
        int inset = FindWindowDpiScale(this, 12);
        HPEN pen = CreatePen(PS_SOLID, 1, FloatingToolPanelSeparator());
        HGDIOBJ old = SelectObject(hdc, pen);
        MoveToEx(hdc, inset, chromeSeparatorY, nullptr);
        LineTo(hdc, std::max(inset, rc.dx - inset), chromeSeparatorY);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }

    if (!closeRc.IsEmpty()) {
        COLORREF bg = FindSurfaceBg(false);
        DrawCloseButtonArgs cb;
        cb.hdc = hdc;
        cb.r = closeRc;
        cb.isHover = closeHover || closePressed;
        cb.colX = FloatingPopupMutedTextColor();
        cb.colXHover = FloatingPopupTextColor();
        COLORREF hoverBg = FloatingPopupCloseHoverBg(bg);
        cb.colHoverBg = closePressed ? AccentColor(hoverBg, ThemeUsesDarkChrome() ? 8 : 6) : hoverBg;
        DrawCloseButton(cb);
    }
}

void FindWindowWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    HBRUSH br = BackgroundBrush();
    if (br && ps) {
        FillRect(hdc, &ps->rcPaint, br);
    }
    if (docked) {
        DrawEditUnderline(hdc);
    } else {
        DrawSearchChrome(hdc);
    }
}

void FindWindowWnd::EnsureResultsListRedraw() {
    if (results && listRedrawPaused) {
        SendMessageW(results->hwnd, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(results->hwnd, nullptr, TRUE);
        listRedrawPaused = false;
    }
}

void FindWindowWnd::RefreshResults(bool allowNavigation) {
    if (!results) {
        return;
    }
    // rebuild the highlight terms from the current search text
    filterWords.Reset();
    // During a streamed scan findCountText still describes the previous
    // completed cache; highlight using the live edit text until this scan lands.
    TempStr term = win->findCountValid && win->findCountText ? ToUtf8Temp(win->findCountText) : nullptr;
    if (str::IsEmpty(term)) {
        term = win->hwndFindEdit ? HwndGetTextTemp(win->hwndFindEdit) : nullptr;
    }
    if (!str::IsEmpty(term)) {
        filterWords.Append(term);
    }
    int oldSel = results->GetCurrentSelection();
    int oldTop = (int)SendMessageW(results->hwnd, LB_GETTOPINDEX, 0, 0);
    int oldCount = results->GetCount();
    int newCount = results->model->ItemsCount();
    bool sameScan = displayedResultsInitialized && displayedResultsEpoch == win->findCountEpoch;
    // After the count finishes the list is reordered into document order; a
    // same-count append would leave wrap-order rows in place.
    bool sortedComplete = win->findCountValid && !win->findCountPartial;
    bool canAppend = sameScan && !sortedComplete && oldCount >= 0 && oldCount < newCount;
    // A same-count refresh of an already-sorted list must not run FillWithItems:
    // its LB_RESETCONTENT wipes and refills every row, and the trailing
    // erase invalidate makes the visible list flash (find-result click flicker).
    // The rows only need rebuilding when the count, scan epoch, sort state or
    // snippet width actually changed.
    int snippetBudget = FindWindowSnippetGlyphBudget(win);
    bool alreadySortedDisplayed = sameScan && sortedComplete && displayedResultsSorted && oldCount == newCount &&
                                  displayedSnippetBudget == snippetBudget &&
                                  displayedTextCacheGen == win->findCountTextCacheGeneration;
    if (!canAppend && !alreadySortedDisplayed) {
        FillWithItems(results->hwnd, results->model);
        oldSel = -1;
        oldTop = 0;
        displayedResultsSorted = sortedComplete;
        displayedSnippetBudget = snippetBudget;
        displayedTextCacheGen = win->findCountTextCacheGeneration;
    } else if (oldCount < newCount) {
        // Streamed results are append-only within one count epoch. Do not
        // WM_SETREDRAW + full InvalidateRect: that repaints every visible row
        // (~75 ms) and the yellow highlights flicker even when the top rows
        // have not changed.
        for (int i = oldCount; i < newCount; i++) {
            TempWStr ws = ToWStrTemp(results->model->Item(i));
            ListBox_AddString(results->hwnd, ws);
        }
        int top = (int)SendMessageW(results->hwnd, LB_GETTOPINDEX, 0, 0);
        if (oldTop >= 0 && oldTop < newCount && top != oldTop) {
            SendMessageW(results->hwnd, LB_SETTOPINDEX, (WPARAM)oldTop, 0);
        }
        for (int i = oldCount; i < newCount; i++) {
            RECT ir{};
            if (ListBox_GetItemRect(results->hwnd, i, &ir) != LB_ERR) {
                InvalidateRect(results->hwnd, &ir, FALSE);
            }
        }
    }
    displayedResultsEpoch = win->findCountEpoch;
    displayedResultsInitialized = true;
    // keep a result selected so it's visible as you type and Next/Prev have a
    // sensible starting point.
    int sel = oldSel >= 0 && oldSel < newCount ? oldSel : -1;
    if (hasPendingNavigation) {
        for (int i = 0; i < (int)win->findMatches.size(); i++) {
            const FindMatch& fm = win->findMatches[i];
            if (fm.startPage == pendingStartPage && fm.startGlyph == pendingStartGlyph) {
                sel = i;
                break;
            }
        }
    } else {
        // Document navigation (Enter/F3/toolbar buttons) can happen without a
        // list selection notification. Prefer the match now selected in the
        // document over the previous list row, so the list follows it.
        int current = CurrentMatchIndex();
        if (current >= 0) {
            sel = current;
        }
    }
    if (sel < 0) {
        sel = CurrentMatchIndex();
    }
    if (sel >= 0) {
        // the document already sits on a match: mirror it in the list
        if (sel != oldSel) {
            results->SetCurrentSelection(sel);
        }
    } else if (win->findMatches.size() > 0) {
        // the document isn't on a match yet: go to the first match at/after the
        // current page so Next/Prev have a sensible starting point
        sel = FirstMatchFromCurrentPage();
        results->SetCurrentSelection(sel);
        // streamed partial updates must not navigate: OnResultSelected joins
        // the in-flight count worker (GoToFindMatch), which would cancel the
        // very scan that's producing these results
        if (allowNavigation) {
            OnResultSelected();
        }
    }
    SyncFindResultsScrollbar(this);
}

void FindWindowWnd::DrawResultItem(ListBox::DrawItemEvent* ev) {
    ListBox* lb = ev->listBox;
    if (ev->itemIndex < 0 || ev->itemIndex >= (int)win->findMatches.size()) {
        return;
    }
    HDC hdc = ev->hdc;
    RECT rc = ev->itemRect;

    COLORREF colBg = IsSpecialColor(lb->bgColor) ? GetSysColor(COLOR_WINDOW) : lb->bgColor;
    COLORREF colText = IsSpecialColor(lb->textColor) ? GetSysColor(COLOR_WINDOWTEXT) : lb->textColor;
    int dpi = lastDpi > 0 ? lastDpi : EffectiveDpiForFindWindow(hwnd, 0);
    // Keyword highlight is the strong mark. The current row is only a light tint.
    if (ev->selected) {
        if (ThemeUsesDarkChrome()) {
            colBg = BlendColor(colBg, ThemeWindowLinkColor(), ThemeUsesBlackChrome() ? 16 : 14);
        } else {
            colBg = AccentColor(colBg, 16);
        }
    } else if (ev->itemIndex == hoverResult) {
        colBg = FloatingPopupHoverBg(colBg);
    }
    SetBkColor(hdc, colBg);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
    SetBkMode(hdc, TRANSPARENT);

    HFONT oldFont = lb->font ? SelectFont(hdc, lb->font) : nullptr;
    int pad = MulDiv(6, dpi, 96);
    RECT rcText = rc;
    rcText.left += pad;
    rcText.right -= pad;

    // page number in a fixed right column so it can't overlap the snippet while
    // the window is being resized (issue #5692)
    const FindMatch& fm = win->findMatches[ev->itemIndex];
    TempStr pageStr = str::FormatTemp("%s", win->ctrl->GetPageLabeTemp(fm.startPage));
    WCHAR* pageW = ToWStrTemp(pageStr);
    SIZE pSz{};
    GetTextExtentPoint32W(hdc, L"99999", 5, &pSz);
    int pageGap = MulDiv(10, dpi, 96);
    int pageColDx = std::max((int)pSz.cx, MulDiv(40, dpi, 96));
    RECT rcPage = rcText;
    rcPage.left = std::max(rcText.left, (LONG)(rcText.right - pageColDx));

    // snippet on the left, with the matched term highlighted
    RECT rcSnippet = rcText;
    rcSnippet.right = std::max(rcSnippet.left, rcPage.left - pageGap);
    if (rcSnippet.right > rcSnippet.left) {
        SetTextColor(hdc, colText);
        DrawMaybeHighlightedTextArgs args(filterWords, hlScratch);
        args.hdc = hdc;
        args.rc = rcSnippet;
        args.text = fm.snippet ? fm.snippet : "";
        args.colBg = colBg;
        args.isRtl = false;
        // Matches are already filtered by TextSearch; don't re-apply whole-word
        // boundaries on snippets (CJK context bytes fail the byte-level check).
        args.matchWholeWord = false;
        args.primaryHighlightStart = fm.snippetMatchStart;
        args.primaryHighlightEnd = fm.snippetMatchEnd;
        COLORREF primaryHl = 0;
        COLORREF secondaryHl = 0;
        FindSnippetHighlightColors(colBg, &primaryHl, &secondaryHl);
        args.highlightColor = primaryHl;
        args.secondaryHighlightColor = secondaryHl;
        args.drawFmt = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_LEFT | DT_END_ELLIPSIS;
        // clip snippet drawing so match highlights cannot bleed into the page
        // number column when the floating window is narrow (issue #5736);
        // SaveDC/RestoreDC (rather than SelectClipRgn(nullptr)) so the outer
        // listbox-client clip stays in effect afterwards
        int snippetDC = SaveDC(hdc);
        IntersectClipRect(hdc, rcSnippet.left, rcSnippet.top, rcSnippet.right, rcSnippet.bottom);
        DrawMaybeHighlightedText(args);
        RestoreDC(hdc, snippetDC);
    }

    // repaint the page column on top in case a prior draw left stray pixels
    SetBkColor(hdc, colBg);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rcPage, nullptr, 0, nullptr);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, FindSurfaceMuted(docked));
    DrawTextW(hdc, pageW, -1, &rcPage, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT | DT_END_ELLIPSIS);

    if (oldFont) {
        SelectFont(hdc, oldFont);
    }
}

void FindWindowWnd::OnResultSelected() {
    int idx = results ? results->GetCurrentSelection() : -1;
    if (idx < 0 || idx >= (int)win->findMatches.size()) {
        return;
    }
    const FindMatch& fm = win->findMatches[idx];
    DisplayModel* dm = win->AsFixed();
    if (dm && dm->textSearch && win->ctrl && win->ctrl->CurrentPageNo() == fm.startPage &&
        dm->textSearch->startPage == fm.startPage && dm->textSearch->startGlyph == fm.startGlyph) {
        return; // already on this match
    }
    // Keep the row chosen by Enter/arrows selected until its deferred document
    // navigation runs. Streamed result refreshes must not snap it back to the
    // document's previous match in the meantime.
    hasPendingNavigation = true;
    pendingStartPage = fm.startPage;
    pendingStartGlyph = fm.startGlyph;
    pendingEndPage = fm.endPage;
    pendingEndGlyph = fm.endGlyph;
    pendingNavigationCountEpoch = win->findCountEpoch;
    // defer document navigation so the results list can scroll/repaint first
    // (issue #5692). Coalesce rapid F3 / arrow presses to the latest selection.
    auto data = new DeferredGoToFindMatchData;
    data->win = win;
    data->findWindow = this;
    data->startPage = fm.startPage;
    data->startGlyph = fm.startGlyph;
    data->endPage = fm.endPage;
    data->endGlyph = fm.endGlyph;
    data->epoch = InterlockedIncrement(&pendingNavEpoch);
    uitask::Post(MkFunc0<DeferredGoToFindMatchData>(DeferredGoToFindMatch, data), "GoToFindMatch");
}

// list index of the match the document is currently on (so the selection can
// track the current match), or -1 if it isn't in the list
int FindWindowWnd::CurrentMatchIndex() {
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSearch) {
        return -1;
    }
    int page = dm->textSearch->startPage;
    int glyph = dm->textSearch->startGlyph;
    int n = (int)win->findMatches.size();
    for (int i = 0; i < n; i++) {
        const FindMatch& fm = win->findMatches[i];
        if (fm.startPage == page && fm.startGlyph == glyph) {
            return i;
        }
    }
    return -1;
}

// first match at/after the current page. After the count finishes, matches are
// in document order; while streaming they are still wrap-scan order.
int FindWindowWnd::FirstMatchFromCurrentPage() {
    int n = (int)win->findMatches.size();
    if (n == 0) {
        return -1;
    }
    int curPage = win->ctrl ? win->ctrl->CurrentPageNo() : 1;
    if (win->findCountValid && !win->findCountPartial) {
        for (int i = 0; i < n; i++) {
            if (win->findMatches[i].startPage >= curPage) {
                return i;
            }
        }
        return 0;
    }
    int nPages = win->ctrl ? win->ctrl->PageCount() : 1;
    int best = 0;
    int bestDist = INT_MAX;
    for (int i = 0; i < n; i++) {
        int dist = win->findMatches[i].startPage - curPage;
        if (dist < 0) {
            dist += nPages;
        }
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
            if (dist == 0) {
                break; // first match on the current page
            }
        }
    }
    return best;
}

// move the results-list selection (keyboard arrows or the Next/Prev buttons)
// while focus stays in the search edit, navigating to the newly selected match.
// Returns false (not handled) when there are no results, so the caller can fall
// back to a normal document search.
bool FindWindowWnd::MoveResultSelection(WPARAM vkey) {
    if (!results) {
        return false;
    }
    int n = (int)win->findMatches.size();
    if (n == 0) {
        return false;
    }
    constexpr int kPage = 10;
    int cur = results->GetCurrentSelection();
    if (cur < 0) {
        cur = CurrentMatchIndex(); // start from where the document already is
    }
    int idx;
    switch (vkey) {
        case VK_DOWN:
            // wrap like the compact bar's Find Next (issue #5692)
            idx = (cur < 0) ? 0 : (cur + 1) % n;
            break;
        case VK_UP:
            idx = (cur < 0) ? n - 1 : (cur - 1 + n) % n;
            break;
        case VK_NEXT: // Page Down
            // unlike the arrow keys, paging doesn't wrap around; it clamps to the
            // last match (issue #5742)
            if (cur < 0) {
                idx = 0;
            } else {
                idx = cur + kPage;
                if (idx >= n) {
                    idx = n - 1;
                }
            }
            break;
        case VK_PRIOR: // Page Up
            // clamp to the first match instead of wrapping (issue #5742)
            if (cur < 0) {
                idx = n - 1;
            } else {
                idx = cur - kPage;
                if (idx < 0) {
                    idx = 0;
                }
            }
            break;
        default:
            return false;
    }
    if (idx == cur) {
        return true; // e.g. a single match wrapping onto itself
    }
    results->SetCurrentSelection(idx);
    // ListBox_SetCurSel does not send LBN_SELCHANGE; navigate explicitly
    OnResultSelected();
    return true;
}

void FindWindowWnd::SavePos() {
    if (docked || !IsWindowVisible(hwnd)) {
        return;
    }
    Rect r = WindowRect(hwnd);
    gGlobalPrefs->searchUIWindowPos = r;
}

void FindWindowWnd::SetDocked(bool dock) {
    if (docked == dock) {
        Layout();
        return;
    }
    if (dock) {
        SavePos();
    }
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    docked = dock;
    DWORD style = WS_POPUP | WS_CLIPCHILDREN;
    if (docked) {
        style |= WS_BORDER;
    } else {
        // Resize stays. The caption and its red close do not.
        style |= WS_THICKFRAME;
    }
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    SendMessageW(hwndBtns, TB_CHANGEBITMAP, kFindWinPinCmdId,
                 docked ? (LPARAM)TbIcon::ArrowsDiagonal : (LPARAM)TbIcon::ArrowsDiagonalMinimize);
    // The pin only expands the docked overlay. Floating close is painted in the header.
    SendMessageW(hwndBtns, TB_HIDEBUTTON, kFindWinPinCmdId, MAKELONG(docked ? FALSE : TRUE, 0));
    SendMessageW(hwndBtns, TB_HIDEBUTTON, kFindWinCloseCmdId, MAKELONG(docked ? FALSE : TRUE, 0));
    RefreshToolbarDpi();
    SendMessageW(hwndBtns, TB_AUTOSIZE, 0, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (docked) {
        SetWindowRgn(hwnd, nullptr, FALSE);
    } else {
        UpdateFloatingPopupWindowRgn(hwnd, kFloatingPopupCornerRadius, false);
    }
    ApplySurfaceColors();
    Layout();
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

// re-apply theme colors after the user switches themes. The toolbar icons are
// baked into an image list at the current text color, so rebuild it; the
// controls and caption also need recoloring.
void FindWindowWnd::RefreshToolbarDpi() {
    if (!hwndBtns) {
        return;
    }
    int dpi = lastDpi > 0 ? lastDpi : DpiGet(hwnd);
    int icon = RoundUp(MulDiv(16, dpi, 96), 4);
    int slot = docked ? icon : std::max(icon, MulDiv(26, dpi, 96));
    HIMAGELIST oldHiml = himl;
    himl = BuildStdToolbarImageList(icon);
    SendMessageW(hwndBtns, TB_SETIMAGELIST, 0, (LPARAM)himl);
    SendMessageW(hwndBtns, TB_SETBUTTONSIZE, 0, MAKELONG(slot, slot));
    SendMessageW(hwndBtns, TB_AUTOSIZE, 0, 0);
    if (oldHiml) {
        ImageList_Destroy(oldHiml);
    }
}

void FindWindowWnd::RefreshNonClientChrome() {
    if (!docked) {
        UpdateFloatingPopupWindowRgn(hwnd, kFloatingPopupCornerRadius, true);
        return;
    }
    int dpi = lastDpi > 0 ? lastDpi : DpiGet(hwnd);
    RECT rcClient{};
    GetClientRect(hwnd, &rcClient);
    DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    RECT rcWindow = rcClient;
    using SigAdjustWindowRectExForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static SigAdjustWindowRectExForDpi pfnAdjustWindowRectExForDpi = nullptr;
    static bool triedAdjustWindowRectExForDpi = false;
    if (!triedAdjustWindowRectExForDpi) {
        triedAdjustWindowRectExForDpi = true;
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            pfnAdjustWindowRectExForDpi =
                (SigAdjustWindowRectExForDpi)GetProcAddress(hUser32, "AdjustWindowRectExForDpi");
        }
    }
    if (pfnAdjustWindowRectExForDpi) {
        pfnAdjustWindowRectExForDpi(&rcWindow, style, FALSE, exStyle, (UINT)dpi);
    } else {
        AdjustWindowRectEx(&rcWindow, style, FALSE, exStyle);
    }

    int dx = rcWindow.right - rcWindow.left;
    int dy = rcWindow.bottom - rcWindow.top;
    SetWindowPos(hwnd, nullptr, 0, 0, dx, dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ApplyTitleBarTheme(hwnd);
}

void FindWindowWnd::SyncDpi(bool force, int explicitDpi) {
    int dpi = EffectiveDpiForFindWindow(hwnd, explicitDpi);
    if (!force && dpi == lastDpi) {
        return;
    }
    lastDpi = dpi;

    HFONT font = GetAppFontForDpi(dpi);
    if (edit) {
        edit->SetFont(font);
        edit->maxDx = MulDiv(1000, dpi, 96);
    }
    if (status) {
        status->SetFont(font);
    }
    if (results) {
        results->SetFont(font);
        results->UpdateItemHeightForDpi();
    }
    RefreshToolbarDpi();
    Layout();
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void FindWindowWnd::ApplySurfaceColors() {
    COLORREF colBg = FindSurfaceBg(docked);
    COLORREF colTxt = FindSurfaceText(docked);
    statusTxtCol = docked ? colTxt : FindSurfaceMuted(false);
    statusBgCol = colBg;
    SetColors(colTxt, colBg);
    if (edit) {
        COLORREF editBg = docked ? ThemeFindEditBackgroundColor() : FloatingPopupFieldBg();
        edit->SetColors(colTxt, editBg);
    }
    if (status) {
        status->SetColors(statusTxtCol, colBg);
    }
    if (results) {
        results->SetColors(colTxt, colBg);
    }
    if (hwndBtns) {
        SendMessageW(hwndBtns, CCM_SETBKCOLOR, 0, (LPARAM)colBg);
    }
}

void FindWindowWnd::UpdateTheme() {
    ApplySurfaceColors();
    RefreshToolbarDpi();
    if (!docked) {
        UpdateFloatingPopupWindowRgn(hwnd, kFloatingPopupCornerRadius, false);
    }
    SyncFindResultsScrollbar(this);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
}

void FindWindowWnd::OnTextChanged() {
    if (suppressTextChanged) {
        return;
    }
    if (edit && edit->hwnd) {
        win->hwndFindEdit = edit->hwnd;
    }
    OnFindBarTextChanged(win);
    if (!docked && edit) {
        ToolbarFindSetText(win, HwndGetTextTemp(edit->hwnd), true);
    }
}

LRESULT FindWindowWnd::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (!docked) {
                // Client fills the window. WS_THICKFRAME stays for resizing,
                // but it must not bring back a caption or a system border.
                return 0;
            }
            break;
        case WM_NCPAINT:
            if (!docked) {
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            if (!docked) {
                // -1 keeps DefWindowProc from painting inactive caption chrome.
                return DefWindowProc(h, msg, wp, -1);
            }
            break;
        case WM_NCHITTEST: {
            if (docked) {
                break;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr{};
            GetWindowRect(h, &wr);
            int x = pt.x - wr.left;
            int y = pt.y - wr.top;
            int w = wr.right - wr.left;
            int ht = wr.bottom - wr.top;
            int b = FindWindowDpiScale(this, 6);
            bool onLeft = x < b;
            bool onRight = x >= w - b;
            bool onTop = y < b;
            bool onBottom = y >= ht - b;
            if (onTop && onLeft) {
                return HTTOPLEFT;
            }
            if (onTop && onRight) {
                return HTTOPRIGHT;
            }
            if (onBottom && onLeft) {
                return HTBOTTOMLEFT;
            }
            if (onBottom && onRight) {
                return HTBOTTOMRIGHT;
            }
            if (onLeft) {
                return HTLEFT;
            }
            if (onRight) {
                return HTRIGHT;
            }
            if (onTop) {
                return HTTOP;
            }
            if (onBottom) {
                return HTBOTTOM;
            }
            POINT client = pt;
            ScreenToClient(h, &client);
            if (closeRc.Contains(client.x, client.y)) {
                return HTCLIENT;
            }
            HWND child = ChildWindowFromPointEx(h, client, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (child && child != h) {
                return HTCLIENT;
            }
            if (dragBottom > 0 && client.y >= 0 && client.y < dragBottom) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            if (!docked && !closeRc.IsEmpty()) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                bool hot = closeRc.Contains(pt.x, pt.y);
                if (hot != closeHover) {
                    closeHover = hot;
                    InvalidateCloseButton();
                }
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (closeHover && !closePressed) {
                closeHover = false;
                InvalidateCloseButton();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (!docked && closeRc.Contains(pt.x, pt.y)) {
                closePressed = true;
                closeHover = true;
                SetCapture(h);
                InvalidateCloseButton();
                return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            if (closePressed) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                bool inside = closeRc.Contains(pt.x, pt.y);
                closePressed = false;
                closeHover = inside;
                if (GetCapture() == h) {
                    ReleaseCapture();
                }
                InvalidateCloseButton();
                if (inside && win) {
                    ToolbarFindCloseDetailed(win, false);
                }
                return 0;
            }
            break;
        }
        case WM_ERASEBKGND: {
            // The floating find window is shown while the docked bar is hidden.
            // Paint the first exposed frame with the theme color, rather than
            // letting DefWindowProc briefly use the system (white) background.
            HBRUSH br = BackgroundBrush();
            if (br) {
                HDC hdc = (HDC)wp;
                RECT rc;
                GetClientRect(h, &rc);
                FillRect(hdc, &rc, br);
                return 1;
            }
            break;
        }
        case WM_ENTERSIZEMOVE:
            inSizeMove = true;
            resizedDuringMove = false;
            break;
        case WM_CAPTURECHANGED:
            if (closePressed && hwnd != (HWND)lp) {
                closePressed = false;
                InvalidateCloseButton();
            }
            if (hwnd != (HWND)lp) {
                EnsureResultsListRedraw();
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                EnsureResultsListRedraw();
            }
            break;
        case WM_DPICHANGED: {
            auto prc = (RECT*)lp;
            int dpi = LOWORD(wp);
            SetWindowPos(h, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            SyncDpi(true, dpi);
            return 0;
        }
        case WM_WINDOWPOSCHANGED:
            if (resultsScroll) {
                OverlayScrollbarUpdatePos(resultsScroll);
            }
            break;
        case WM_MOVE: {
            int monDpi = DpiGetForMonitorOfHwnd(hwnd);
            if (monDpi > 0) {
                monDpi = RoundUp(monDpi, 4);
                if (monDpi != lastDpi) {
                    SyncDpi(true, monDpi);
                }
            }
            break;
        }
        case WM_SIZE: {
            int cx = LOWORD(lp);
            int cy = HIWORD(lp);
            bool clientSizeChanged = (cx != lastClientCx || cy != lastClientCy);
            lastClientCx = cx;
            lastClientCy = cy;
            Layout();
            if (!docked) {
                UpdateFloatingPopupWindowRgn(h, kFloatingPopupCornerRadius, !inSizeMove);
            }
            if (inSizeMove && clientSizeChanged) {
                resizedDuringMove = true;
            }
            // Pause list redraws only when the client area actually changes size
            // during an interactive resize. A caption drag can still deliver WM_SIZE
            // even though the user is only moving the window (#5737).
            if (inSizeMove && results && !listRedrawPaused && wp != SIZE_MINIMIZED && clientSizeChanged) {
                SendMessageW(results->hwnd, WM_SETREDRAW, FALSE, 0);
                listRedrawPaused = true;
            }
            break;
        }
        case WM_EXITSIZEMOVE: {
            inSizeMove = false;
            bool didResize = resizedDuringMove || listRedrawPaused;
            EnsureResultsListRedraw();
            if (didResize && results) {
                InvalidateRect(results->hwnd, nullptr, FALSE);
            }
            int monDpi = DpiGetForMonitorOfHwnd(hwnd);
            if (monDpi <= 0) {
                monDpi = lastDpi > 0 ? lastDpi : DpiGet(hwnd);
            } else {
                monDpi = RoundUp(monDpi, 4);
            }
            if (monDpi != lastDpi) {
                SyncDpi(true, monDpi);
                RefreshNonClientChrome();
            }
            SavePos();
            if (didResize && win->findMatches.size() > 0) {
                auto d = new RebuildSnippetsTaskData;
                d->win = win;
                uitask::Post(MkFunc0<RebuildSnippetsTaskData>(RebuildSnippetsTask, d), "RebuildFindSnippets");
            }
            resizedDuringMove = false;
            break;
        }
        case WM_GETMINMAXINFO: {
            auto mmi = (MINMAXINFO*)lp;
            int pad = FindWindowDpiScale(this, docked ? 8 : 18);
            int gap = FindWindowDpiScale(this, 6);
            int closeExtra = docked ? 0 : FindWindowDpiScale(this, 24);
            int editDy = edit ? edit->GetIdealSize().dy : FindWindowDpiScale(this, 22);
            int tbH = FindWindowDpiScale(this, 24);
            int tbW = DpiScale(h, 120);
            if (hwndBtns) {
                SIZE tbSz{};
                SendMessageW(hwndBtns, TB_GETMAXSIZE, 0, (LPARAM)&tbSz);
                tbW = (int)tbSz.cx;
                tbH = (int)tbSz.cy;
            }
            int row2Dy = std::max(editDy, tbH);
            // narrow two-row header: edit, then status+toolbar
            mmi->ptMinTrackSize.x = 2 * pad + std::max(tbW + closeExtra, FindWindowDpiScale(this, 160));
            mmi->ptMinTrackSize.y = 2 * pad + editDy + gap + row2Dy + pad + FindWindowDpiScale(this, 48);
            return 0;
        }
        case WM_CLOSE:
            // Detailed Search hides and keeps the query. The docked overlay still
            // clears, because that form is the whole find UI.
            if (!docked) {
                ToolbarFindCloseDetailed(win, false);
            } else {
                HideFindBar(win);
            }
            return 0;
        case WM_NOTIFY: {
            // the embedded toolbar paints a light button background in dark
            // themes; repaint it with the window's theme background so the icons
            // sit on the same color as the rest of the window
            auto nmh = (NMHDR*)lp;
            if (nmh->hwndFrom == hwndBtns && nmh->code == NM_CUSTOMDRAW) {
                auto cd = (NMTBCUSTOMDRAW*)nmh;
                switch (cd->nmcd.dwDrawStage) {
                    case CDDS_PREPAINT:
                        FillRect(cd->nmcd.hdc, &cd->nmcd.rc, BackgroundBrush());
                        return CDRF_NOTIFYITEMDRAW;
                    case CDDS_ITEMPREPAINT:
                        if (!docked) {
                            return PaintFindPaletteToolbarItem(cd, FindSurfaceBg(false));
                        }
                        return PrepaintFlatToolbarItem(cd, ThemeWindowControlBackgroundColor());
                }
            }
            break;
        }
    }
    return WndProcDefault(h, msg, wp, lp);
}

LRESULT FindWindowWnd::OnNotify(int, NMHDR* nmh) {
    if (nmh->code == TTN_GETDISPINFOW) {
        auto di = (NMTTDISPINFOW*)nmh;
        int cmd = (int)nmh->idFrom;
        const char* s = cmd == kFindWinPinCmdId ? (docked ? _TRA("Detailed Search") : _TRA("Back to toolbar"))
                                                : FindWindowButtonTooltip(cmd);
        if (s) {
            lstrcpynW(di->szText, ToWStrTemp(s), dimof(di->szText));
            di->lpszText = di->szText;
        }
    }
    return 0;
}

bool FindWindowWnd::PreTranslateMessage(MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_CHAR) {
        return false;
    }
    if (msg.message == WM_CHAR && (msg.wParam == '\r' || msg.wParam == '\n')) {
        FindBarResyncActiveEdit(win);
        if (FindFlushPendingSearch(win)) {
            return true;
        }
        if (!IsShiftPressed() && FindEnterFromCurrentPageIfNeeded(win)) {
            return true;
        }
        WPARAM dir = IsShiftPressed() ? VK_UP : VK_DOWN;
        if (!MoveResultSelection(dir)) {
            IsShiftPressed() ? FindPrev(win) : FindNext(win);
        }
        return true;
    }
    if (msg.message != WM_KEYDOWN) {
        return false;
    }
    switch (msg.wParam) {
        case 'F':
            if (IsCtrlPressed() && !IsAltPressed()) {
                FindWindowActivateForShortcut(win);
                return true;
            }
            break;
        case VK_ESCAPE:
            if (!docked) {
                ToolbarFindCloseDetailed(win, false);
            } else {
                HideFindBar(win);
            }
            return true;
        case VK_RETURN:
        case VK_F3: {
            // Enter starts a search only after the query changes; otherwise it
            // steps through the results list (#4626).
            if (msg.wParam == VK_RETURN && FindFlushPendingSearch(win)) {
                return true;
            }
            if (msg.wParam == VK_RETURN && !IsShiftPressed() && FindEnterFromCurrentPageIfNeeded(win)) {
                return true;
            }
            // step through the results list; fall back to a document search when
            // there's no list (e.g. count not ready)
            WPARAM dir = IsShiftPressed() ? VK_UP : VK_DOWN;
            if (!MoveResultSelection(dir)) {
                IsShiftPressed() ? FindPrev(win) : FindNext(win);
            }
            return true;
        }
        case VK_DOWN:
        case VK_UP:
        case VK_NEXT:
        case VK_PRIOR:
            // walk the results list from the search edit
            return MoveResultSelection(msg.wParam);
    }
    return false;
}

bool FindWindowWnd::OnCommand(WPARAM wparam, LPARAM) {
    int notification = HIWORD(wparam);
    if (notification == EN_SETFOCUS || notification == EN_KILLFOCUS) {
        editHasFocus = notification == EN_SETFOCUS;
        if (!docked && edit && edit->hwnd) {
            RECT r{};
            GetWindowRect(edit->hwnd, &r);
            MapWindowPoints(nullptr, hwnd, (LPPOINT)&r, 2);
            r.top = r.bottom - 2;
            r.bottom += 3;
            InvalidateRect(hwnd, &r, FALSE);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    int cmd = LOWORD(wparam);
    switch (cmd) {
        case CmdFindPrev:
            if (!MoveResultSelection(VK_UP)) {
                FindPrev(win);
            }
            return true;
        case CmdFindNext:
            if (!MoveResultSelection(VK_DOWN)) {
                FindNext(win);
            }
            return true;
        case CmdFindToggleMatchCase:
            FindToggleMatchCase(win);
            return true;
        case CmdFindToggleMatchWholeWord:
            FindToggleMatchWholeWord(win);
            return true;
        case kFindWinPinCmdId:
            if (!docked) {
                ToolbarFindCloseDetailed(win, true);
                return true;
            }
            ToggleFloatingFindUI(win);
            return true;
        case kFindWinCloseCmdId:
            if (!docked) {
                ToolbarFindCloseDetailed(win, false);
            } else {
                HideFindBar(win);
            }
            return true;
    }
    return false;
}

//--- public API

FindWindowWnd* CreateFindWindow(MainWindow* win) {
    auto w = new FindWindowWnd();
    if (!w->Create(win)) {
        delete w;
        return nullptr;
    }
    return w;
}

void DeleteFindWindow(MainWindow* win) {
    if (!win->findWindow) {
        return;
    }
    win->findWindow->SavePos();
    delete win->findWindow;
    win->findWindow = nullptr;
}

static int FindWindowDpiForLayout(MainWindow* win, HWND hwnd) {
    int dpi = win->frameDpi > 0 ? win->frameDpi : DpiGetForMonitorOfHwnd(win->hwndFrame);
    if (dpi <= 0) {
        dpi = DpiGet(hwnd ? hwnd : win->hwndFrame);
    }
    return dpi;
}

static void FindWindowDefaultSizeForDpi(int dpi, int* dxOut, int* dyOut) {
    *dxOut = MulDiv(kFindWindowDefaultDx, dpi, 96);
    *dyOut = MulDiv(kFindWindowDefaultDy, dpi, 96);
}

static int FindWindowDpiForSavedRect(MainWindow* win, const Rect& r) {
    RECT rc = ToRECT(r);
    HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    int dpi = DpiGetForMonitor(monitor);
    return dpi > 0 ? RoundUp(dpi, 4) : FindWindowDpiForLayout(win, nullptr);
}

static void PositionFindWindow(FindWindowWnd* w) {
    MainWindow* win = w->win;
    Rect r = gGlobalPrefs->searchUIWindowPos;
    if (r.IsEmpty()) {
        // default: a reasonable size near the top-right of the frame
        int dpi = FindWindowDpiForLayout(win, w->hwnd);
        int defaultDx = 0;
        int defaultDy = 0;
        FindWindowDefaultSizeForDpi(dpi, &defaultDx, &defaultDy);
        w->SyncDpi(false, dpi);
        SetWindowPos(w->hwnd, nullptr, 0, 0, defaultDx, defaultDy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        w->Layout();
        Rect mainVis = WindowVisibleRect(win->hwndFrame);
        int y = mainVis.y + MulDiv(80, dpi, 96);
        PositionOwnedPopupAtFrameRight(w->hwnd, win->hwndFrame, y);
        return;
    }
    int targetDpi = FindWindowDpiForSavedRect(win, r);
    int defaultDx = 0;
    int defaultDy = 0;
    FindWindowDefaultSizeForDpi(targetDpi, &defaultDx, &defaultDy);
    w->SyncDpi(false, targetDpi);
    // Stale saved sizes (e.g. from an older default or a hidden window that was
    // never positioned) must not shrink the dialog when expanding from the bar.
    if (r.dx < defaultDx) {
        r.dx = defaultDx;
    }
    if (r.dy < defaultDy) {
        r.dy = defaultDy;
    }
    r = ShiftRectToWorkArea(r, win->hwndFrame, true);
    SetWindowPos(w->hwnd, HWND_TOP, r.x, r.y, r.dx, r.dy, SWP_NOACTIVATE);
    w->Layout();
}

static void PositionDockedFindWindow(FindWindowWnd* w) {
    w->Layout();
    Rect btn = GetToolbarButtonScreenRect(w->win, CmdFindFirst);
    Rect r = WindowRect(w->hwnd);
    int dpi = FindWindowDpiForLayout(w->win, w->hwnd);
    int y = btn.IsEmpty() ? WindowVisibleRect(w->win->hwndFrame).y + MulDiv(4, dpi, 96) : btn.y + btn.dy / 2 - r.dy / 2;
    PositionOwnedPopupAtFrameRight(w->hwnd, w->win->hwndFrame, y);
}

void FindWindowReposition(MainWindow* win) {
    if (!IsFindWindowVisible(win)) {
        return;
    }
    if (!NeedsFindUI(win)) {
        HideFindWindow(win);
        return;
    }
    FindWindowWnd* w = win->findWindow;
    // Don't re-apply SearchUIFloating here. ↗ can show Detailed Search while
    // the setting stays false; a move must not turn that window into the overlay.
    // Only reposition; don't push the frame DPI onto a window the user may have
    // moved to another monitor (GetDpiForWindow can lag during cross-monitor drags).
    w->Layout();
    if (w->docked) {
        PositionDockedFindWindow(w);
    } else {
        PositionFindWindow(w);
    }
}

void FindWindowActivateForShortcut(MainWindow* win) {
    if (!win) {
        return;
    }
    // SearchUIFloating false: Ctrl+F always lands in the toolbar box.
    if (!gGlobalPrefs || !gGlobalPrefs->searchUIFloating) {
        HwndSendCommand(win->hwndFrame, CmdFindFirst);
        return;
    }
    HWND hwndFind = FindWindowHwnd(win);
    bool visible = hwndFind && IsWindowVisible(hwndFind) && !IsFindWindowDocked(win);
    if (!visible) {
        HwndSendCommand(win->hwndFrame, CmdFindFirst);
        return;
    }
    FindBarResyncActiveEdit(win);
    HWND fg = GetForegroundWindow();
    if (fg != hwndFind && fg != win->hwndFindEdit && (!fg || !IsChild(hwndFind, fg))) {
        SetForegroundWindow(hwndFind);
    }
    FindWindowReposition(win);
    ShowWindow(hwndFind, SW_SHOW);
    if (win->hwndFindEdit && IsWindow(win->hwndFindEdit)) {
        HwndSetFocus(win->hwndFindEdit);
        Edit_SetSel(win->hwndFindEdit, 0, -1);
        return;
    }
    win->hwndFindEdit = nullptr;
    HwndSendCommand(win->hwndFrame, CmdFindFirst);
}

static void ShowFindWindowImpl(MainWindow* win, bool forceDetailed) {
    if (!win->findWindow) {
        win->findWindow = CreateFindWindow(win);
    }
    if (!win->findWindow) {
        return;
    }
    FindWindowWnd* w = win->findWindow;
    bool docked = !forceDetailed && gGlobalPrefs && !gGlobalPrefs->searchUIFloating;
    if (forceDetailed && w->edit) {
        HWND tb = ToolbarFindEdit(win);
        if (tb) {
            AutoFreeStr owned;
            owned.SetCopy(HwndGetTextTemp(tb));
            w->suppressTextChanged = true;
            HwndSetText(w->edit->hwnd, owned.Get() ? owned.Get() : "");
            w->suppressTextChanged = false;
        }
    }
    w->SetDocked(docked);
    win->hwndFindEdit = w->edit->hwnd; // make this the active find edit
    FindWindowSetMatchCaseChecked(win, win->findMatchCase);
    FindWindowSetMatchWholeWordChecked(win, win->findMatchWholeWord);
    if (w->docked) {
        PositionDockedFindWindow(w);
    } else {
        PositionFindWindow(w);
    }
    w->SyncDpi(true);
    w->Layout();
    Rect rc = ClientRect(w->hwnd);
    w->lastClientCx = rc.dx;
    w->lastClientCy = rc.dy;
    bool opening = !IsWindowVisible(w->hwnd);
    ShowWindow(w->hwnd, SW_SHOW);
    // build/refresh snippet text for the current results-list width
    if (win->findMatches.size() > 0) {
        RebuildFindMatchSnippets(win);
    }
    HwndSetFocus(win->hwndFindEdit);
    Edit_SetSel(win->hwndFindEdit, 0, -1);
    // populate the results list: show what's cached, and (re)run the search for
    // the current term so snippets get built now that the window is visible
    w->RefreshResults(false);
    if (win->hwndFindEdit && HwndGetTextLen(win->hwndFindEdit) > 0 && IsDocumentSearchReady(win)) {
        SyncFindResultsList(win);
        RequestFindCount(win);
    }
    RefreshFindUIStatus(win);
    if (opening) {
        win->UpdateCanvasSize();
    }
}

void ShowFindWindow(MainWindow* win) {
    ShowFindWindowImpl(win, false);
}

void ShowDetailedSearchWindow(MainWindow* win) {
    ShowFindWindowImpl(win, true);
}

void HideFindWindow(MainWindow* win, bool keepSearchState) {
    if (!keepSearchState) {
        HideFindBar(win, false);
        return;
    }
    if (!win->findWindow) {
        return;
    }
    bool wasVisible = IsWindowVisible(win->findWindow->hwnd);
    win->findWindow->SavePos();
    ShowWindow(win->findWindow->hwnd, SW_HIDE);
    OverlayScrollbarHide(win->findWindow->resultsScroll);
    FindBarResyncActiveEdit(win);
    if (wasVisible) {
        win->UpdateCanvasSize();
    }
}

bool IsFindWindowVisible(MainWindow* win) {
    return win->findWindow && IsWindowVisible(win->findWindow->hwnd);
}

bool IsFindWindowDocked(MainWindow* win) {
    return win && win->findWindow && win->findWindow->docked;
}

void FindWindowSetDocked(MainWindow* win, bool docked) {
    if (!win || !win->findWindow) {
        return;
    }
    win->findWindow->SetDocked(docked);
    if (docked) {
        PositionDockedFindWindow(win->findWindow);
    } else {
        PositionFindWindow(win->findWindow);
        win->findWindow->RefreshResults(false);
    }
    RedrawWindow(win->findWindow->hwnd, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

HWND FindWindowHwnd(MainWindow* win) {
    return win && win->findWindow ? win->findWindow->hwnd : nullptr;
}

void FindWindowSetStatusText(MainWindow* win, const char* s) {
    if (win->findWindow && win->findWindow->status) {
        HwndSetText(win->findWindow->status->hwnd, s ? s : "");
        // Status width is reserved from a fixed "99999 / 99999" sample.
        // Relayout here MoveWindow'd the results list on every n/m / "..."
        // tick and made the list jitter during a live search.
    }
}

void FindWindowFlashStatusText(MainWindow* win, bool flash) {
    if (win->findWindow) {
        win->findWindow->FlashStatusText(flash);
    }
}

TempStr FindWindowGetStatusText(MainWindow* win) {
    if (!win->findWindow || !win->findWindow->status) {
        return nullptr;
    }
    return HwndGetTextTemp(win->findWindow->status->hwnd);
}

void FindWindowSetSuppressTextChanged(MainWindow* win, bool suppress) {
    if (win->findWindow) {
        win->findWindow->suppressTextChanged = suppress;
    }
}

void FindWindowClearEditText(MainWindow* win) {
    if (!win || !win->findWindow || !win->findWindow->edit) {
        return;
    }
    win->findWindow->suppressTextChanged = true;
    HwndSetText(win->findWindow->edit->hwnd, "");
    Edit_SetModify(win->findWindow->edit->hwnd, FALSE);
    win->findWindow->suppressTextChanged = false;
}

void FindWindowResyncActiveEdit(MainWindow* win) {
    if (!win || !IsFindWindowVisible(win) || !win->findWindow || !win->findWindow->edit) {
        return;
    }
    win->hwndFindEdit = win->findWindow->edit->hwnd;
}

void FindWindowSetStatus(MainWindow* win, const char* s) {
    FindBarSetStatus(win, s);
}

void FindWindowSetMatchCaseChecked(MainWindow* win, bool checked) {
    if (win->findWindow && win->findWindow->hwndBtns) {
        SendMessageW(win->findWindow->hwndBtns, TB_CHECKBUTTON, CmdFindToggleMatchCase, MAKELONG(checked ? 1 : 0, 0));
    }
}

void FindWindowSetMatchWholeWordChecked(MainWindow* win, bool checked) {
    if (win->findWindow && win->findWindow->hwndBtns) {
        SendMessageW(win->findWindow->hwndBtns, TB_CHECKBUTTON, CmdFindToggleMatchWholeWord,
                     MAKELONG(checked ? 1 : 0, 0));
    }
}

void FindWindowRefreshResults(MainWindow* win, bool allowNavigation) {
    if (IsFindWindowVisible(win)) {
        win->findWindow->RefreshResults(allowNavigation);
    }
}

void FindWindowApplyPendingNavigation(MainWindow* win) {
    if (!win || !win->findWindow) {
        return;
    }
    FindWindowWnd* w = win->findWindow;
    if (!w->hasPendingNavigation) {
        return;
    }
    if (w->pendingNavigationCountEpoch != win->findCountEpoch) {
        w->hasPendingNavigation = false;
        return;
    }
    if (!win->IsDocLoaded()) {
        w->hasPendingNavigation = false;
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSearch || !dm->GetEngine()) {
        w->hasPendingNavigation = false;
        return;
    }
    if (win->findCountEngine && (void*)dm->GetEngine() != win->findCountEngine) {
        w->hasPendingNavigation = false;
        return;
    }
    int startPage = w->pendingStartPage;
    int startGlyph = w->pendingStartGlyph;
    int endPage = w->pendingEndPage;
    int endGlyph = w->pendingEndGlyph;
    w->hasPendingNavigation = false;
    GoToFindMatch(win, startPage, startGlyph, endPage, endGlyph);
}

int FindWindowPendingNavigationIndex(MainWindow* win) {
    if (!win || !win->findWindow || !win->findWindow->hasPendingNavigation) {
        return 0;
    }
    FindWindowWnd* w = win->findWindow;
    for (int i = 0; i < (int)win->findMatches.size(); i++) {
        const FindMatch& fm = win->findMatches[i];
        if (fm.startPage == w->pendingStartPage && fm.startGlyph == w->pendingStartGlyph) {
            return i + 1;
        }
    }
    return 0;
}

int FindWindowCurrentSelectionIndex(MainWindow* win) {
    if (!win || !win->findWindow || !win->findWindow->results) {
        return 0;
    }
    int idx = win->findWindow->results->GetCurrentSelection();
    return idx >= 0 && idx < (int)win->findMatches.size() ? idx + 1 : 0;
}

int FindWindowSnippetGlyphBudget(MainWindow* win) {
    constexpr int kFallbackGlyphs = 72;
    if (!IsFindWindowVisible(win) || !win->findWindow || !win->findWindow->results) {
        return kFallbackGlyphs;
    }
    FindWindowWnd* fw = win->findWindow;
    Rect rc = ClientRect(fw->results->hwnd);
    if (rc.dx <= 0) {
        return kFallbackGlyphs;
    }
    int dpi = fw->lastDpi > 0 ? fw->lastDpi : DpiGet(fw->hwnd);
    int pad = MulDiv(12, dpi, 96);
    int pageCol = MulDiv(42, dpi, 96);
    int snippetPx = std::max(0, rc.dx - pad - pageCol);
    if (snippetPx <= 0) {
        return kFallbackGlyphs;
    }

    HDC hdc = GetDC(fw->results->hwnd);
    HFONT font = fw->results->font ? fw->results->font : GetAppFontForDpi(dpi);
    HFONT prev = (HFONT)SelectObject(hdc, font);
    SIZE sz{};
    // mix Latin and CJK so the budget tracks one visible line in either script
    const WCHAR* sample = L"中国China sample text";
    int sampleLen = 19;
    GetTextExtentPoint32W(hdc, sample, sampleLen, &sz);
    SelectObject(hdc, prev);
    ReleaseDC(fw->results->hwnd, hdc);

    int avgCharPx = std::max(1, (int)((sz.cx + sampleLen / 2) / sampleLen));
    int budget = snippetPx / avgCharPx;
    return limitValue(budget, 48, 96);
}

void UpdateFindWindowTheme(MainWindow* win) {
    if (win->findWindow) {
        win->findWindow->UpdateTheme();
    }
}

void ResyncFloatingFindEdit(MainWindow* win) {
    if (!IsFindWindowVisible(win) || !win->findWindow || !win->findWindow->edit) {
        return;
    }
    win->hwndFindEdit = win->findWindow->edit->hwnd;
    FindWindowRefreshResults(win, false);
    ScheduleRepaint(win, 0);
}

char* TestFindResultPageColumnClipResult(int* exitCodeOut) {
    StrBuilder out;
    auto fail = [&](const char* msg) -> char* {
        out.Append(msg);
        out.AppendChar('\n');
        if (exitCodeOut) {
            *exitCodeOut = 1;
        }
        return out.StealData();
    };

    if (gWindows.IsEmpty()) {
        return fail("NOTREADY no-window");
    }
    MainWindow* win = gWindows.at(0);
    if (!win || !win->ctrl) {
        return fail("NOTREADY no-doc");
    }
    if (!win->findWindow) {
        win->findWindow = CreateFindWindow(win);
    }
    FindWindowWnd* fw = win->findWindow;
    if (!fw || !fw->results) {
        return fail("ERROR no-find-window");
    }

    ClearFindMatches(win);
    FindMatch fm;
    fm.startPage = 1;
    fm.snippet = str::Dup("longprefix testword suffix");
    win->findMatches.Append(fm);
    fw->filterWords.Reset();
    fw->filterWords.Append("testword");

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) {
        return fail("ERROR no-screen-dc");
    }
    const int w = 110;
    const int h = DpiScale(fw->hwnd, 20);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmp = CreateCompatibleBitmap(hdcScreen, w, h);
    if (!hdcMem || !hbmp) {
        ReleaseDC(nullptr, hdcScreen);
        DeleteDC(hdcMem);
        DeleteObject(hbmp);
        return fail("ERROR no-mem-dc");
    }
    HGDIOBJ oldBmp = SelectObject(hdcMem, hbmp);

    ListBox::DrawItemEvent ev;
    ev.listBox = fw->results;
    ev.hdc = hdcMem;
    ev.itemRect = {0, 0, w, h};
    ev.itemIndex = 0;
    ev.selected = false;
    fw->DrawResultItem(&ev);

    COLORREF px = GetPixel(hdcMem, w - 3, h / 2);
    SelectObject(hdcMem, oldBmp);
    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    ClearFindMatches(win);

    bool isYellow = GetRValue(px) > 200 && GetGValue(px) > 200 && GetBValue(px) < 100;
    if (isYellow) {
        out.AppendFmt("FAIL pixel=0x%06x\n", (unsigned)px);
        if (exitCodeOut) {
            *exitCodeOut = 1;
        }
        return out.StealData();
    }
    out.AppendFmt("OK pixel=0x%06x\n", (unsigned)px);
    if (exitCodeOut) {
        *exitCodeOut = 0;
    }
    return out.StealData();
}
