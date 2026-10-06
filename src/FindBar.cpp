/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include <uxtheme.h>
#include <commctrl.h>

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
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
#include "FindWindow.h"
#include "Translations.h"
#include "Theme.h"
#include "FloatingPopupStyle.h"

#include "utils/Log.h"

constexpr UINT_PTR kFindEditSubclassId = 9101;

static LRESULT CALLBACK FindEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId,
                                             DWORD_PTR refData) {
    MainWindow* win = (MainWindow*)refData;
    if (win && msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        // Esc clears search and returns focus to the document.
        if (GetWindowTextLengthW(hwnd) > 0) {
            SetWindowTextW(hwnd, L"");
        }
        HWND dest = win->hwndCanvas ? win->hwndCanvas : win->hwndFrame;
        if (dest) {
            HwndSetFocus(dest);
        }
        return 0;
    }
    if (win && msg == WM_CHAR) {
        if (wp == '\r' || wp == '\n') {
            win->hwndFindEdit = hwnd;
            FindBarResyncActiveEdit(win);
            if (FindFlushPendingSearch(win)) {
                return 0;
            }
            IsShiftPressed() ? FindPrev(win) : FindNext(win);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void InstallFindEditKeyboardHandler(MainWindow* win, HWND hwndEdit) {
    if (!win || !hwndEdit) {
        return;
    }
    RemoveWindowSubclass(hwndEdit, FindEditSubclassProc, kFindEditSubclassId);
    SetWindowSubclass(hwndEdit, FindEditSubclassProc, kFindEditSubclassId, (DWORD_PTR)win);
}

// command ids for the bar's toolbar buttons; must not collide with real commands
constexpr int kFindBarCloseCmdId = (int)CmdLast + 50;
constexpr int kFindBarPinCmdId = (int)CmdLast + 52;

static COLORREF BlendColor(COLORREF background, COLORREF foreground, int foregroundPercent) {
    int backgroundPercent = 100 - foregroundPercent;
    u8 br, bg, bb, fr, fg, fb;
    UnpackColor(background, br, bg, bb);
    UnpackColor(foreground, fr, fg, fb);
    return MkColor((u8)((br * backgroundPercent + fr * foregroundPercent) / 100),
                   (u8)((bg * backgroundPercent + fg * foregroundPercent) / 100),
                   (u8)((bb * backgroundPercent + fb * foregroundPercent) / 100));
}

struct FindBarWnd : Wnd {
    MainWindow* win = nullptr;
    Edit* edit = nullptr;
    Static* status = nullptr;
    COLORREF statusTxtCol = 0;
    COLORREF statusBgCol = 0;
    HWND hwndBtns = nullptr; // small toolbar: prev / next / match-case / close
    HIMAGELIST himl = nullptr;

    int barDx = 0;
    int barDy = 0;
    // when set, programmatic edits to the text don't kick off a search
    // (used while restoring text during a theme-change recreate)
    bool suppressTextChanged = false;
    bool editHasFocus = false;
    int lastDpi = 0;

    FindBarWnd() = default;
    ~FindBarWnd() override;

    bool Create(MainWindow* win);
    void Layout();
    void RefreshToolbarDpi();
    void SyncDpi(bool force = false, int explicitDpi = 0);
    void FlashStatusText(bool flash);

    void OnTextChanged();
    void DrawEditUnderline();

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;
    LRESULT OnNotify(int controlId, NMHDR* nmh) override;
    bool PreTranslateMessage(MSG& msg) override;
    bool OnCommand(WPARAM wparam, LPARAM lparam) override;
};

// tooltip text for the bar's toolbar buttons
// append a command's keyboard shortcut to its tooltip, e.g. "Find Next (F3)"
static const char* AppendCmdAccel(const char* base, int cmd) {
    const char* accel = AppendAccelKeyToMenuStringTemp(nullptr, cmd);
    if (!accel) {
        return base;
    }
    return str::JoinTemp(base, str::FormatTemp(" (%s)", accel + 1)); // +1 skips the leading \t
}

static const char* FindBarButtonTooltip(int cmd) {
    switch (cmd) {
        case CmdFindPrev:
            return AppendCmdAccel(_TRA("Find Previous"), cmd);
        case CmdFindNext:
            return AppendCmdAccel(_TRA("Find Next"), cmd);
        case CmdFindToggleMatchCase:
            return AppendCmdAccel(_TRA("Match Case"), cmd);
        case CmdFindToggleMatchWholeWord:
            return AppendCmdAccel(_TRA("Match Whole Word"), cmd);
        case kFindBarPinCmdId:
            return _TRA("Open in a window");
        case kFindBarCloseCmdId:
            return _TRA("Close");
    }
    return nullptr;
}

FindBarWnd::~FindBarWnd() {
    delete edit;
    delete status;
    HwndDestroyWindowSafe(&hwndBtns);
    if (himl) {
        ImageList_Destroy(himl);
    }
}

bool FindBarWnd::Create(MainWindow* mainWin) {
    win = mainWin;

    auto colBg = ThemeWindowControlBackgroundColor();
    auto colTxt = ThemeWindowTextColor();

    {
        CreateCustomArgs args;
        args.visible = false;
        args.style = WS_POPUP | WS_BORDER;
        // WS_EX_TOOLWINDOW keeps it off the taskbar. Not topmost: we make the
        // frame our owner instead (below) so the bar floats above the frame but
        // not above other apps.
        args.exStyle = WS_EX_TOOLWINDOW;
        args.isRtl = IsUIRtl();
        CreateCustom(args);
    }
    if (!hwnd) {
        return false;
    }
    // make the frame our owner: an owned window always renders above its owner
    // (so it stays visible when the user clicks into the document) yet drops
    // behind when another application is activated.
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
        edit->maxDx = DpiScale(hwnd, 240);
        edit->SetColors(colTxt, ThemeFindEditBackgroundColor());
        edit->Create(args);
        edit->onTextChanged = MkMethod0<FindBarWnd, &FindBarWnd::OnTextChanged>(this);
        win->hwndFindEdit = edit->hwnd;
        InstallFindEditKeyboardHandler(win, edit->hwnd);
    }

    {
        Static::CreateArgs args;
        args.parent = hwnd;
        args.text = "";
        args.isRtl = IsUIRtl();
        status = new Static();
        statusTxtCol = colTxt;
        statusBgCol = colBg;
        status->SetColors(colTxt, colBg);
        status->Create(args);
        // vertically center the single line of text so it lines up with the
        // (taller, bordered) edit box's text instead of sitting at the top
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
        // bar's themed background instead of a light box in dark themes (the
        // background is painted from NM_CUSTOMDRAW in WndProc)
        SetWindowTheme(hwndBtns, L"", L"");
        // NM_CUSTOMDRAW starts too late to cover the toolbar's initial native
        // surface. Seed it with the theme color to avoid a white transition frame.
        SendMessageW(hwndBtns, CCM_SETBKCOLOR, 0, (LPARAM)colBg);
        SendMessageW(hwndBtns, TB_BUTTONSTRUCTSIZE, (WPARAM)sizeof(TBBUTTON), 0);

        int isz = RoundUp(DpiScale(hwnd, 16), 4);
        himl = BuildStdToolbarImageList(isz);
        SendMessageW(hwndBtns, TB_SETIMAGELIST, 0, (LPARAM)himl);
        SendMessageW(hwndBtns, TB_SETBUTTONSIZE, 0, MAKELONG(isz, isz));

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
        b[4].iBitmap = (int)TbIcon::ArrowsDiagonal;
        b[4].idCommand = kFindBarPinCmdId;
        b[4].fsState = TBSTATE_ENABLED;
        b[4].fsStyle = BTNS_BUTTON;
        b[5].iBitmap = (int)TbIcon::Close;
        b[5].idCommand = kFindBarCloseCmdId;
        b[5].fsState = TBSTATE_ENABLED;
        b[5].fsStyle = BTNS_BUTTON;
        SendMessageW(hwndBtns, TB_ADDBUTTONS, 6, (LPARAM)&b);
        SendMessageW(hwndBtns, TB_AUTOSIZE, 0, 0);
    }

    lastDpi = DpiGet(hwnd);
    Layout();
    return true;
}

static int ToolbarDpiForFindBar(FindBarWnd* bar, int explicitDpi = 0) {
    if (explicitDpi > 0) {
        return RoundUp(explicitDpi, 4);
    }
    MainWindow* win = bar->win;
    if (IsWindowVisible(bar->hwnd)) {
        int monDpi = DpiGetForMonitorOfHwnd(bar->hwnd);
        if (monDpi > 0) {
            return RoundUp(monDpi, 4);
        }
        return DpiGet(bar->hwnd);
    }
    int dpi = win->frameDpi > 0 ? win->frameDpi : DpiGetForMonitorOfHwnd(win->hwndFrame);
    if (dpi <= 0) {
        dpi = DpiGet(win->hwndFrame);
    }
    return RoundUp(dpi, 4);
}

void FindBarWnd::RefreshToolbarDpi() {
    if (!hwndBtns) {
        return;
    }
    int dpi = ToolbarDpiForFindBar(this);
    int isz = RoundUp(MulDiv(16, dpi, 96), 4);
    HIMAGELIST oldHiml = himl;
    himl = BuildStdToolbarImageList(isz);
    SendMessageW(hwndBtns, TB_SETIMAGELIST, 0, (LPARAM)himl);
    SendMessageW(hwndBtns, TB_SETBUTTONSIZE, 0, MAKELONG(isz, isz));
    SendMessageW(hwndBtns, TB_AUTOSIZE, 0, 0);
    if (oldHiml) {
        ImageList_Destroy(oldHiml);
    }
}

void FindBarWnd::SyncDpi(bool force, int explicitDpi) {
    int dpi = ToolbarDpiForFindBar(this, explicitDpi);
    if (!force && dpi == lastDpi) {
        return;
    }
    lastDpi = dpi;

    HFONT font = GetAppFontForDpi(dpi);
    if (edit) {
        edit->SetFont(font);
        edit->maxDx = MulDiv(240, dpi, 96);
    }
    if (status) {
        status->SetFont(font);
    }
    RefreshToolbarDpi();
    Layout();
    RedrawWindow(hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static COLORREF FlashFindStatusTextColor(COLORREF normal) {
    return BlendColor(normal, MkColor(255, 255, 255), 40);
}

void FindBarWnd::FlashStatusText(bool flash) {
    if (!status) {
        return;
    }
    if (flash) {
        status->SetColors(FlashFindStatusTextColor(statusTxtCol), statusBgCol);
    } else {
        status->SetColors(statusTxtCol, statusBgCol);
    }
}

void FindBarWnd::Layout() {
    // CreateCustom can synchronously dispatch WM_DPICHANGED before child controls exist.
    if (!edit || !status || !hwndBtns) {
        return;
    }
    int dpi = lastDpi > 0 ? lastDpi : ToolbarDpiForFindBar(this);
    auto scale = [dpi](int x) { return MulDiv(x, dpi, 96); };
    int p = scale(6);
    int gap = scale(4);
    int editDx = scale(220);
    // The status control contains only n/m now. Measure that directly instead
    // of preserving space for the removed loading message.
    const char* countSample = "99999 / 99999";
    Size countSz = HwndMeasureText(status->hwnd, countSample, status->font);
    int statusDx = std::max(scale(64), countSz.dx + scale(4));

    int editDy = edit->GetIdealSize().dy;

    SIZE tbSz{};
    SendMessageW(hwndBtns, TB_GETMAXSIZE, 0, (LPARAM)&tbSz);

    Rect frameRect = WindowVisibleRect(win->hwndFrame);
    int maxBarDx = frameRect.dx - scale(8);
    int minEditDx = scale(48);
    int desiredBarDx = 2 * p + editDx + statusDx + 2 * gap + (int)tbSz.cx;
    if (maxBarDx > 0 && desiredBarDx > maxBarDx) {
        editDx = std::max(minEditDx, editDx - (desiredBarDx - maxBarDx));
    }

    int innerDy = std::max(editDy, (int)tbSz.cy);
    barDy = innerDy + 2 * p;
    barDx = p + editDx + gap + statusDx + gap + (int)tbSz.cx + p;

    int x = p;
    MoveWindow(edit->hwnd, x, (barDy - editDy) / 2, editDx, editDy, TRUE);
    x += editDx + gap;
    MoveWindow(status->hwnd, x, (barDy - editDy) / 2, statusDx, editDy, TRUE);
    x += statusDx + gap;
    MoveWindow(hwndBtns, x, (barDy - (int)tbSz.cy) / 2, (int)tbSz.cx, (int)tbSz.cy, TRUE);

    SetWindowPos(hwnd, nullptr, 0, 0, barDx, barDy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void FindBarWnd::OnTextChanged() {
    if (suppressTextChanged) {
        return;
    }
    if (edit && edit->hwnd) {
        win->hwndFindEdit = edit->hwnd;
    }
    OnFindBarTextChanged(win);
}

void FindBarWnd::DrawEditUnderline() {
    if (!edit || !edit->hwnd) {
        return;
    }
    RECT r{};
    GetWindowRect(edit->hwnd, &r);
    MapWindowPoints(nullptr, hwnd, (LPPOINT)&r, 2);

    COLORREF bg = ThemeWindowControlBackgroundColor();
    COLORREF col =
        editHasFocus ? BlendColor(bg, ThemeWindowLinkColor(), 28) : AccentColor(bg, ThemeUsesDarkChrome() ? 30 : 22);
    HDC hdc = GetDC(hwnd);
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ old = SelectObject(hdc, pen);
    int y = r.bottom;
    MoveToEx(hdc, r.left, y, nullptr);
    LineTo(hdc, r.right, y);
    SelectObject(hdc, old);
    DeleteObject(pen);
    ReleaseDC(hwnd, hdc);
}

LRESULT FindBarWnd::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        LRESULT res = WndProcDefault(h, msg, wp, lp);
        DrawEditUnderline();
        return res;
    }
    if (msg == WM_ERASEBKGND) {
        HBRUSH br = BackgroundBrush();
        if (br) {
            HDC hdc = (HDC)wp;
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(hdc, &rc, br);
            return 1;
        }
    }
    if (msg == WM_DPICHANGED) {
        auto prc = (RECT*)lp;
        int dpi = LOWORD(wp);
        SetWindowPos(h, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SyncDpi(true, dpi);
        return 0;
    }
    if (msg == WM_NOTIFY) {
        // the embedded toolbar paints a light button background in dark themes;
        // repaint it with the bar's theme background so the icons sit on the
        // same color as the rest of the bar
        auto nmh = (NMHDR*)lp;
        if (nmh->hwndFrom == hwndBtns && nmh->code == NM_CUSTOMDRAW) {
            auto cd = (NMTBCUSTOMDRAW*)nmh;
            switch (cd->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    FillRect(cd->nmcd.hdc, &cd->nmcd.rc, BackgroundBrush());
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    return PrepaintFlatToolbarItem(cd, ThemeWindowControlBackgroundColor());
            }
        }
    }
    return WndProcDefault(h, msg, wp, lp);
}

LRESULT FindBarWnd::OnNotify(int, NMHDR* nmh) {
    if (nmh->code == TTN_GETDISPINFOW) {
        auto di = (NMTTDISPINFOW*)nmh;
        const char* s = FindBarButtonTooltip((int)nmh->idFrom);
        if (s) {
            lstrcpynW(di->szText, ToWStrTemp(s), dimof(di->szText));
            di->lpszText = di->szText;
        }
    }
    return 0;
}

bool FindBarWnd::PreTranslateMessage(MSG& msg) {
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
        IsShiftPressed() ? FindPrev(win) : FindNext(win);
        return true;
    }
    if (msg.message != WM_KEYDOWN) {
        return false;
    }
    // the find edit lives in this owned popup, not as a child of the frame, so
    // the frame's edit accelerator table doesn't reach it; handle the find keys
    // here (Esc, Enter/Shift+Enter, F3/Shift+F3)
    switch (msg.wParam) {
        case 'F':
            if (IsCtrlPressed() && !IsAltPressed()) {
                FindWindowActivateForShortcut(win);
                return true;
            }
            break;
        case VK_ESCAPE:
            HideFindBar(win);
            return true;
        case VK_RETURN:
        case VK_F3:
            // Enter starts a search only after the query changes; otherwise it
            // steps to the next match (issue #4626).
            if (msg.wParam == VK_RETURN && FindFlushPendingSearch(win)) {
                return true;
            }
            if (msg.wParam == VK_RETURN && !IsShiftPressed() && FindEnterFromCurrentPageIfNeeded(win)) {
                return true;
            }
            if (IsShiftPressed()) {
                FindPrev(win);
            } else {
                FindNext(win);
            }
            return true;
    }
    return false;
}

bool FindBarWnd::OnCommand(WPARAM wparam, LPARAM) {
    int notification = HIWORD(wparam);
    if (notification == EN_SETFOCUS || notification == EN_KILLFOCUS) {
        editHasFocus = notification == EN_SETFOCUS;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    int cmd = LOWORD(wparam);
    switch (cmd) {
        case CmdFindPrev:
            FindPrev(win);
            return true;
        case CmdFindNext:
            FindNext(win);
            return true;
        case CmdFindToggleMatchCase:
            FindToggleMatchCase(win);
            return true;
        case CmdFindToggleMatchWholeWord:
            FindToggleMatchWholeWord(win);
            return true;
        case kFindBarPinCmdId:
            ToggleFloatingFindUI(win); // pop out into the floating window
            return true;
        case kFindBarCloseCmdId:
            HideFindBar(win);
            return true;
    }
    return false;
}

//--- public API

FindBarWnd* CreateFindBar(MainWindow* win) {
    auto bar = new FindBarWnd();
    if (!bar->Create(win)) {
        delete bar;
        return nullptr;
    }
    return bar;
}

void DeleteFindBar(MainWindow* win) {
    if (!win->findBar) {
        return;
    }
    delete win->findBar;
    win->findBar = nullptr;
    win->hwndFindEdit = nullptr;
}

// rebuild the bar so it picks up new theme colors / icons (called on theme change)
void RecreateFindBar(MainWindow* win) {
    if (win->findWindow) {
        UpdateFindWindowTheme(win);
        FindWindowReposition(win);
        return;
    }
    if (!win->findBar) {
        return;
    }
    bool floatingVisible = IsFindWindowVisible(win);
    HWND floatingEdit = floatingVisible ? win->hwndFindEdit : nullptr;
    TempStr floatingText = floatingEdit ? str::DupTemp(HwndGetTextTemp(floatingEdit)) : nullptr;
    // stop any in-flight find/count that captured the old bar's state
    AbortFinding(win, true);
    bool wasVisible = IsWindowVisible(win->findBar->hwnd);
    TempStr text = wasVisible ? str::DupTemp(HwndGetTextTemp(win->hwndFindEdit)) : nullptr;
    DeleteFindBar(win);
    win->findBar = CreateFindBar(win);
    if (win->findBar && wasVisible) {
        ShowFindBar(win);
        if (!str::IsEmpty(text)) {
            // restore the text without re-running the search (the existing
            // document highlight is preserved across the recreate)
            win->findBar->suppressTextChanged = true;
            HwndSetText(win->hwndFindEdit, text);
            win->findBar->suppressTextChanged = false;
        }
    } else if (floatingVisible) {
        ResyncFloatingFindEdit(win);
        if (!str::IsEmpty(floatingText) && win->hwndFindEdit) {
            HwndSetText(win->hwndFindEdit, floatingText);
        }
    }
}

// --- permanent toolbar search box -------------------------------------------
// One painted frame. The edit is the only child HWND; the count and icons are
// drawn here and hit-tested separately. The old docked overlay is left in place.

static const WCHAR* kToolbarFindClass = L"SumatraToolbarFind";

enum class ToolbarFindPart {
    None,
    Prev,
    Next,
    Detail,
    Clear,
    Search
};

struct ToolbarFindState {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    HWND tip = nullptr;
    HBRUSH fillBrush = nullptr;
    AutoFreeStr status;
    COLORREF fill = 0;
    COLORREF textCol = 0;
    COLORREF mutedCol = 0;
    // Toolbar button size. Icon slots use this so the field matches the row.
    int iconSlot = 0;
    bool suppress = false;
    bool focused = false;
    bool flash = false;
    bool hasQuery = false;
    bool showNav = false;
    bool showDetail = false;
    bool showStatus = false;
    bool tracking = false;
    // Set while the edit is repainted from the parent's WM_PAINT, so that
    // repaint cannot re-enter and schedule another one.
    bool repairingEdit = false;
    ToolbarFindPart hot = ToolbarFindPart::None;
    ToolbarFindPart pressed = ToolbarFindPart::None;
    Rect rcPrev;
    Rect rcNext;
    Rect rcDetail;
    Rect rcClear;
    Rect rcSearch;
    Rect rcStatus;
};

static ToolbarFindState* ToolbarFindGet(HWND hwnd) {
    return (ToolbarFindState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static ToolbarFindState* ToolbarFindGet(MainWindow* win) {
    if (!win || !win->hwndToolbarFind) {
        return nullptr;
    }
    return ToolbarFindGet(win->hwndToolbarFind);
}

static int TfS(HWND hwnd, int dip) {
    return DpiScale(hwnd, dip);
}

static void ToolbarFindApplyColors(ToolbarFindState* st) {
    st->fill = ThemeChromeBackgroundColor();
    st->textCol = ThemeWindowTextColor();
    st->mutedCol = ThemeWindowTextDisabledColor();
    if (st->fillBrush) {
        DeleteObject(st->fillBrush);
    }
    st->fillBrush = CreateSolidBrush(st->fill);
}

static void ToolbarFindSyncQueryFlag(ToolbarFindState* st) {
    st->hasQuery = st->edit && GetWindowTextLengthW(st->edit) > 0;
}

static ToolbarFindPart ToolbarFindHit(ToolbarFindState* st, int x, int y) {
    if (st->showNav && st->rcPrev.Contains(x, y)) {
        return ToolbarFindPart::Prev;
    }
    if (st->showNav && st->rcNext.Contains(x, y)) {
        return ToolbarFindPart::Next;
    }
    if (st->showDetail && st->rcDetail.Contains(x, y)) {
        return ToolbarFindPart::Detail;
    }
    if (st->hasQuery && st->rcClear.Contains(x, y)) {
        return ToolbarFindPart::Clear;
    }
    if (st->rcSearch.Contains(x, y)) {
        return ToolbarFindPart::Search;
    }
    return ToolbarFindPart::None;
}

static const char* ToolbarFindTipText(ToolbarFindState* st, ToolbarFindPart part) {
    switch (part) {
        case ToolbarFindPart::Prev:
            return _TRA("Find Previous");
        case ToolbarFindPart::Next:
            return _TRA("Find Next");
        case ToolbarFindPart::Detail:
            return _TRA("Detailed Search");
        case ToolbarFindPart::Clear:
            return _TRA("Clear search text");
        case ToolbarFindPart::Search:
            return st->hasQuery ? _TRA("Find Next") : _TRA("Find");
        case ToolbarFindPart::None:
            break;
    }
    return nullptr;
}

static void ToolbarFindHideTip(ToolbarFindState* st) {
    if (!st->tip) {
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = st->hwnd;
    ti.uId = (UINT_PTR)st->hwnd;
    SendMessageW(st->tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

static void ToolbarFindShowTip(ToolbarFindState* st, ToolbarFindPart part) {
    const char* s = ToolbarFindTipText(st, part);
    if (!st->tip || !s) {
        ToolbarFindHideTip(st);
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = st->hwnd;
    ti.uId = (UINT_PTR)st->hwnd;
    ti.lpszText = (WCHAR*)ToWStrTemp(s);
    SendMessageW(st->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    POINT pt{};
    GetCursorPos(&pt);
    pt.y += TfS(st->hwnd, 18);
    SendMessageW(st->tip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y));
    SendMessageW(st->tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

static void ToolbarFindEnsureTip(ToolbarFindState* st) {
    if (st->tip) {
        return;
    }
    st->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, st->hwnd, nullptr,
                              GetModuleHandle(nullptr), nullptr);
    if (!st->tip) {
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_IDISHWND | TTF_TRANSPARENT;
    ti.hwnd = st->hwnd;
    ti.uId = (UINT_PTR)st->hwnd;
    ti.lpszText = (WCHAR*)L"";
    SendMessageW(st->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

// Right-hand accessories. Previous/Next sit together. The ellipsis is a
// lighter secondary hit. Clear and the magnifier stay the right-end pair.
struct ToolbarFindCluster {
    int search = 0;
    int clear = 0;
    int detail = 0;
    int nav = 0;
    int gapNavToDetail = 0;
    int gapDetailToClear = 0;
};

static void ToolbarFindClusterMetrics(HWND hwnd, int slot, ToolbarFindCluster* m) {
    m->search = slot;
    m->clear = slot;
    int nav = slot - DpiScale(hwnd, 6);
    int minNav = DpiScale(hwnd, 22);
    if (nav < minNav) {
        nav = minNav;
    }
    if (nav > slot) {
        nav = slot;
    }
    m->nav = nav;
    int detail = DpiScale(hwnd, 26);
    if (detail < DpiScale(hwnd, 24)) {
        detail = DpiScale(hwnd, 24);
    }
    if (detail > slot) {
        detail = slot;
    }
    m->detail = detail;
    m->gapNavToDetail = DpiScale(hwnd, 6);
    m->gapDetailToClear = DpiScale(hwnd, 4);
}

static int ToolbarFindIconSlot(ToolbarFindState* st, int fieldH) {
    if (st->iconSlot > 0) {
        return st->iconSlot;
    }
    return fieldH > 0 ? fieldH : 1;
}

static void ToolbarFindLayoutInner(ToolbarFindState* st) {
    if (!st || !st->hwnd) {
        return;
    }
    ToolbarFindSyncQueryFlag(st);
    RECT crc{};
    GetClientRect(st->hwnd, &crc);
    int w = crc.right;
    int h = crc.bottom;
    if (w <= 0 || h <= 0) {
        return;
    }
    // Same inset the page box uses, so this field sits in the toolbar row.
    int edge = GetSystemMetrics(SM_CXEDGE);
    if (edge < 1) {
        edge = 1;
    }
    int slot = ToolbarFindIconSlot(st, h);
    if (slot > h) {
        slot = h;
    }
    if (slot < 1) {
        slot = 1;
    }

    st->showNav = st->hasQuery;
    st->showDetail = st->hasQuery;
    st->showStatus = st->hasQuery;

    HFONT font = st->edit ? GetWindowFont(st->edit) : nullptr;
    int statusW = 0;
    if (st->showStatus) {
        // Wide enough for "103 / 1250". Never ellipsize the count.
        statusW = HwndMeasureText(st->hwnd, "0000 / 0000", font).dx + edge * 2;
        if (st->status.Get() && st->status.Get()[0]) {
            int actual = HwndMeasureText(st->hwnd, st->status.Get(), font).dx + edge * 2;
            if (actual > statusW) {
                statusW = actual;
            }
        }
    }

    ToolbarFindCluster cluster;
    ToolbarFindClusterMetrics(st->hwnd, slot, &cluster);
    auto clusterW = [&](bool detail) -> int {
        int dx = cluster.search; // magnifier, always
        if (st->hasQuery) {
            dx += cluster.clear;
        }
        if (detail) {
            dx += cluster.gapDetailToClear + cluster.detail;
        }
        if (st->showNav) {
            dx += cluster.gapNavToDetail + cluster.nav * 2;
        }
        return dx + statusW;
    };

    int minEdit = HwndMeasureText(st->hwnd, "0000", font).dx;
    int right = clusterW(st->showDetail);
    int editW = w - edge - right - edge;
    if (st->hasQuery && editW < minEdit && st->showDetail) {
        st->showDetail = false;
        right = clusterW(false);
        editW = w - edge - right - edge;
    }
    if (editW < 1) {
        editW = 1;
    }

    // A single-line edit pins its text to the top. Size the control to the
    // line and center that strip, the same way the page box does.
    int textH = font ? HwndMeasureText(st->hwnd, "Page:", font).dy : 0;
    int editH = textH > 0 ? textH : h - edge * 2;
    if (editH > h - 2) {
        editH = h - 2;
    }
    if (editH < 1) {
        editH = 1;
    }
    int editY = (h - editH + 1) / 2;
    if (st->edit) {
        RECT cur{};
        GetWindowRect(st->edit, &cur);
        MapWindowPoints(nullptr, st->hwnd, (LPPOINT)&cur, 2);
        int curW = cur.right - cur.left;
        int curH = cur.bottom - cur.top;
        if (cur.left != edge || cur.top != editY || curW != editW || curH != editH) {
            MoveWindow(st->edit, edge, editY, editW, editH, TRUE);
        }
        // The cue sits on the edit's left edge. Give it a little air inside the field.
        int textLeft = DpiScale(st->hwnd, 8);
        SendMessageW(st->edit, EM_SETMARGINS, EC_LEFTMARGIN, MAKELONG(textLeft, 0));
        // Keep the edit's square fill inside the field's round corners.
        RECT fieldRc{};
        GetClientRect(st->hwnd, &fieldRc);
        int ellipse = DpiScale(st->hwnd, 8);
        int fieldH = fieldRc.bottom - fieldRc.top;
        if (fieldH > 4 && ellipse > fieldH - 2) {
            ellipse = fieldH - 2;
        }
        HRGN rgn =
            CreateRoundRectRgn(-edge, -editY, fieldRc.right - edge + 1, fieldRc.bottom - editY + 1, ellipse, ellipse);
        if (rgn && !SetWindowRgn(st->edit, rgn, FALSE)) {
            DeleteObject(rgn);
        }
    }

    st->rcStatus = Rect();
    st->rcPrev = Rect();
    st->rcNext = Rect();
    st->rcDetail = Rect();
    st->rcClear = Rect();
    st->rcSearch = Rect();
    // Magnifier is the right anchor. Previous/Next are one tight pair.
    // The ellipsis sits a little apart from them, then clear and the magnifier.
    int x = w - edge - cluster.search;
    st->rcSearch = Rect(x, 0, cluster.search, h);
    if (st->hasQuery) {
        x -= cluster.clear;
        st->rcClear = Rect(x, 0, cluster.clear, h);
    }
    if (st->showDetail) {
        x -= cluster.gapDetailToClear + cluster.detail;
        st->rcDetail = Rect(x, 0, cluster.detail, h);
    }
    if (st->showNav) {
        x -= cluster.gapNavToDetail + cluster.nav;
        st->rcNext = Rect(x, 0, cluster.nav, h);
        x -= cluster.nav;
        st->rcPrev = Rect(x, 0, cluster.nav, h);
    }
    if (st->showStatus) {
        x -= statusW;
        st->rcStatus = Rect(x, 0, statusW, h);
    }
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static void ToolbarFindOnQueryChanged(ToolbarFindState* st) {
    MainWindow* win = st->win;
    if (!win || st->suppress) {
        ToolbarFindLayoutInner(st);
        return;
    }
    bool detailed = IsFindWindowVisible(win) && !IsFindWindowDocked(win);
    if (detailed && win->hwndFindEdit && win->hwndFindEdit != st->edit) {
        AutoFreeStr owned;
        owned.SetCopy(HwndGetTextTemp(st->edit));
        FindWindowSetSuppressTextChanged(win, true);
        HwndSetText(win->hwndFindEdit, owned.Get());
        FindWindowSetSuppressTextChanged(win, false);
    } else {
        win->hwndFindEdit = st->edit;
    }
    bool wasSearching = st->showNav;
    OnFindBarTextChanged(win);
    ToolbarFindLayoutInner(st);
    // The match gutter appears only while a query is active. Resize the page
    // once, when search starts or clears, so the marks do not cover the scrollbar.
    if (wasSearching != st->showNav) {
        win->UpdateCanvasSize();
    }
}

// Three dots inside the field. Larger than a hairline so they read as a
// button next to the clear mark, still lighter than a toolbar icon.
static void ToolbarFindDrawEllipsis(ToolbarFindState* st, HDC hdc, const Rect& hit) {
    if (hit.IsEmpty()) {
        return;
    }
    int iconPx = 0;
    if (st->win && st->win->hwndToolbar) {
        HIMAGELIST himl = (HIMAGELIST)SendMessageW(st->win->hwndToolbar, TB_GETIMAGELIST, 0, 0);
        int ih = 0;
        if (himl) {
            ImageList_GetIconSize(himl, &iconPx, &ih);
        }
    }
    if (iconPx < 8) {
        iconPx = DpiScale(st->hwnd, 20);
    }
    // Three grid units. Two was easy to miss beside the 14px clear and search marks.
    int dot = (iconPx * 3 + 12) / 24;
    if (dot < 3) {
        dot = 3;
    }
    int gap = dot;
    int total = dot * 3 + gap * 2;
    int x = hit.x + (hit.dx - total) / 2;
    int y = hit.y + (hit.dy - dot) / 2;
    // The field's rounded fill uses GDI+ and leaves the DC in advanced mode.
    // A GDI Ellipse then draws nothing, so the dots disappeared.
    COLORREF col = st->textCol;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(col), GetGValue(col), GetBValue(col)));
    for (int i = 0; i < 3; i++) {
        g.FillEllipse(&br, x, y, dot, dot);
        x += dot + gap;
    }
}

static void ToolbarFindDrawTbIcon(ToolbarFindState* st, HDC hdc, const Rect& hit, TbIcon icon, int drawPx = 0) {
    if (hit.IsEmpty() || !st->win || !st->win->hwndToolbar) {
        return;
    }
    // Same image list Detailed Find and the main toolbar use.
    HIMAGELIST himl = (HIMAGELIST)SendMessageW(st->win->hwndToolbar, TB_GETIMAGELIST, 0, 0);
    if (!himl) {
        return;
    }
    int iw = 0;
    int ih = 0;
    ImageList_GetIconSize(himl, &iw, &ih);
    int dw = iw;
    int dh = ih;
    if (drawPx > 0 && drawPx < iw) {
        dw = drawPx;
        dh = ih > 0 ? MulDiv(ih, drawPx, iw) : drawPx;
    }
    int x = hit.x + (hit.dx - dw) / 2;
    int y = hit.y + (hit.dy - dh) / 2;
    if (dw == iw && dh == ih) {
        ImageList_Draw(himl, (int)icon, hdc, x, y, ILD_TRANSPARENT);
        return;
    }
    HICON hicon = ImageList_GetIcon(himl, (int)icon, ILD_TRANSPARENT);
    if (!hicon) {
        return;
    }
    DrawIconEx(hdc, x, y, hicon, dw, dh, 0, nullptr, DI_NORMAL);
    DestroyIcon(hicon);
}

// The system Edit class is CS_PARENTDC, so WS_CLIPCHILDREN does not keep this
// window's fill off the child. A finished search (status flash) or Prev/Next
// paints the field color over the keyword until the edit is painted again.
static bool ToolbarFindEditClientRect(ToolbarFindState* st, RECT* rc) {
    if (!st || !st->edit || !rc || !GetWindowRect(st->edit, rc)) {
        return false;
    }
    MapWindowPoints(nullptr, st->hwnd, (LPPOINT)rc, 2);
    return rc->right > rc->left && rc->bottom > rc->top;
}

static void ToolbarFindPaint(ToolbarFindState* st, HDC hdc) {
    RECT rc{};
    GetClientRect(st->hwnd, &rc);
    RECT editRc{};
    bool clipEdit = ToolbarFindEditClientRect(st, &editRc);
    int savedDc = clipEdit ? SaveDC(hdc) : 0;
    if (clipEdit) {
        ExcludeClipRect(hdc, editRc.left, editRc.top, editRc.right, editRc.bottom);
    }
    // Windows text-box corners. The square corners stay toolbar-colored.
    COLORREF chrome = ThemeChromeBackgroundColor();
    HBRUSH chromeBr = CreateSolidBrush(chrome);
    FillRect(hdc, &rc, chromeBr);
    DeleteObject(chromeBr);
    COLORREF border = AccentColor(chrome, st->focused ? 72 : 36);
    int arc = DpiScale(st->hwnd, 8);
    int fieldH = rc.bottom - rc.top;
    if (fieldH > 4 && arc > fieldH - 2) {
        arc = fieldH - 2;
    }
    Rect box(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    FillFloatingPopupRoundedRect(hdc, box, arc, st->fill);
    StrokeFloatingPopupRoundedRect(hdc, box, arc, border);

    auto hover = [&](const Rect& hit, ToolbarFindPart part) {
        if (hit.IsEmpty() || (st->hot != part && st->pressed != part)) {
            return;
        }
        RECT hr{hit.x, hit.y, hit.Right(), hit.Bottom()};
        COLORREF bg = AccentColor(st->fill, 18);
        HBRUSH br = CreateSolidBrush(bg);
        FillRect(hdc, &hr, br);
        DeleteObject(br);
    };
    hover(st->rcPrev, ToolbarFindPart::Prev);
    hover(st->rcNext, ToolbarFindPart::Next);
    hover(st->rcDetail, ToolbarFindPart::Detail);
    hover(st->rcClear, ToolbarFindPart::Clear);
    hover(st->rcSearch, ToolbarFindPart::Search);

    if (st->showStatus && !st->rcStatus.IsEmpty() && st->status.Get()) {
        COLORREF col = st->flash ? st->textCol : st->mutedCol;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, col);
        HFONT font = st->edit ? GetWindowFont(st->edit) : nullptr;
        HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
        RECT tr{st->rcStatus.x, st->rcStatus.y, st->rcStatus.Right(), st->rcStatus.Bottom()};
        DrawTextW(hdc, ToWStrTemp(st->status.Get()), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (oldFont) {
            SelectObject(hdc, oldFont);
        }
    }

    ToolbarFindDrawTbIcon(st, hdc, st->rcPrev, TbIcon::ChevronUp);
    ToolbarFindDrawTbIcon(st, hdc, st->rcNext, TbIcon::ChevronDown);
    ToolbarFindDrawEllipsis(st, hdc, st->rcDetail);
    // Clear and the magnifier sit inside the field, next to the match count,
    // so they are smaller than the toolbar icons. The hit rect stays full size.
    int innerIcon = DpiScale(st->hwnd, 14);
    ToolbarFindDrawTbIcon(st, hdc, st->rcClear, TbIcon::Close, innerIcon);
    ToolbarFindDrawTbIcon(st, hdc, st->rcSearch, TbIcon::Search, innerIcon);
    if (savedDc) {
        RestoreDC(hdc, savedDc);
    }
}

static void ToolbarFindActivate(ToolbarFindState* st, ToolbarFindPart part) {
    MainWindow* win = st->win;
    if (!win) {
        return;
    }
    if (!(IsFindWindowVisible(win) && !IsFindWindowDocked(win))) {
        win->hwndFindEdit = st->edit;
    }
    auto keepEditFocus = [&]() {
        if (st->edit) {
            HwndSetFocus(st->edit);
        }
    };
    switch (part) {
        case ToolbarFindPart::Prev:
            FindPrev(win);
            keepEditFocus();
            break;
        case ToolbarFindPart::Next:
        case ToolbarFindPart::Search:
            if (st->hasQuery) {
                FindNext(win);
            }
            keepEditFocus();
            break;
        case ToolbarFindPart::Detail:
            ShowDetailedSearchWindow(win);
            break;
        case ToolbarFindPart::Clear:
            if (st->edit) {
                HwndSetText(st->edit, "");
            }
            keepEditFocus();
            break;
        case ToolbarFindPart::None:
            break;
    }
}

static void ToolbarFindSetHot(ToolbarFindState* st, ToolbarFindPart part) {
    if (st->hot == part) {
        return;
    }
    st->hot = part;
    if (part == ToolbarFindPart::None) {
        ToolbarFindHideTip(st);
    } else {
        ToolbarFindEnsureTip(st);
        ToolbarFindShowTip(st, part);
    }
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK ToolbarFindWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ToolbarFindState* st = ToolbarFindGet(hwnd);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        st = (ToolbarFindState*)cs->lpCreateParams;
        st->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
    }
    if (!st) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            ToolbarFindLayoutInner(st);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT editRc{};
            RECT overlap{};
            bool repaintEdit = ToolbarFindEditClientRect(st, &editRc) && IntersectRect(&overlap, &editRc, &ps.rcPaint);
            ToolbarFindPaint(st, hdc);
            EndPaint(hwnd, &ps);
            // Paint the query after the parent fill. Otherwise the keyword stays
            // blank until a later edit paint, such as moving the pointer onto the page.
            if (repaintEdit && st->edit && !st->repairingEdit) {
                st->repairingEdit = true;
                RedrawWindow(st->edit, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
                st->repairingEdit = false;
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, st->textCol);
            SetBkColor(hdc, st->fill);
            return (LRESULT)st->fillBrush;
        }
        case WM_COMMAND:
            if ((HWND)lp == st->edit) {
                int code = HIWORD(wp);
                if (code == EN_CHANGE) {
                    ToolbarFindOnQueryChanged(st);
                } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
                    st->focused = code == EN_SETFOCUS;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_SETFOCUS:
            if (st->edit) {
                HwndSetFocus(st->edit);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
            ToolbarFindSetHot(st, ToolbarFindHit(st, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            return 0;
        }
        case WM_MOUSELEAVE:
            st->tracking = false;
            st->pressed = ToolbarFindPart::None;
            ToolbarFindSetHot(st, ToolbarFindPart::None);
            return 0;
        case WM_LBUTTONDOWN:
            st->pressed = ToolbarFindHit(st, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (st->pressed != ToolbarFindPart::None) {
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (st->edit) {
                HwndSetFocus(st->edit);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            ToolbarFindPart was = st->pressed;
            st->pressed = ToolbarFindPart::None;
            ToolbarFindPart now = ToolbarFindHit(st, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            if (was != ToolbarFindPart::None && was == now) {
                ToolbarFindActivate(st, was);
            }
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (ToolbarFindHit(st, pt.x, pt.y) != ToolbarFindPart::None) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_DESTROY:
            if (st->win) {
                if (st->win->hwndFindEdit == st->edit) {
                    st->win->hwndFindEdit = nullptr;
                }
                if (st->win->hwndToolbarFind == hwnd) {
                    st->win->hwndToolbarFind = nullptr;
                }
            }
            ToolbarFindHideTip(st);
            if (st->tip) {
                DestroyWindow(st->tip);
                st->tip = nullptr;
            }
            return 0;
        case WM_NCDESTROY:
            if (st->fillBrush) {
                DeleteObject(st->fillBrush);
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete st;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ToolbarFindRegister() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToolbarFindWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kToolbarFindClass;
    registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND ToolbarFindEdit(MainWindow* win) {
    ToolbarFindState* st = ToolbarFindGet(win);
    return st ? st->edit : nullptr;
}

void CreateToolbarFind(MainWindow* win) {
    if (!win || !win->hwndToolbar || win->hwndToolbarFind) {
        return;
    }
    ToolbarFindRegister();
    auto* st = new ToolbarFindState();
    st->win = win;
    ToolbarFindApplyColors(st);
    DWORD ex = WS_EX_CONTROLPARENT;
    if (IsUIRtl()) {
        ex |= WS_EX_LAYOUTRTL;
    }
    HWND hwnd = CreateWindowExW(ex, kToolbarFindClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, win->hwndToolbar, nullptr, GetModuleHandle(nullptr), st);
    if (!hwnd) {
        // WM_NCDESTROY already deletes st once creation reaches it.
        return;
    }
    win->hwndToolbarFind = hwnd;
    DWORD editEx = 0;
    HWND edit = CreateWindowExW(editEx, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10,
                                hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    st->edit = edit;
    if (edit) {
        SetWindowTheme(edit, L"", L"");
        HFONT font = GetWindowFont(win->hwndToolbar);
        if (font) {
            SetWindowFont(edit, font, FALSE);
        }
        InstallFindEditKeyboardHandler(win, edit);
    }
    if (IsFindWindowVisible(win) && !IsFindWindowDocked(win) && win->hwndFindEdit) {
        st->suppress = true;
        HwndSetText(edit, HwndGetTextTemp(win->hwndFindEdit));
        st->suppress = false;
    } else if (edit) {
        win->hwndFindEdit = edit;
    }
    ToolbarFindLayout(win);
}

// Right edge of the search field, in toolbar client pixels. When a vertical
// scrollbar takes a column, that edge is the bar's inner side. Otherwise keep
// a small gap off the window edge.
static int ToolbarFindRightLimit(MainWindow* win, HWND toolbar, int tbW, int margin) {
    int fallback = tbW - margin - DpiScale(toolbar, 12);
    if (fallback < 1) {
        fallback = tbW;
    }
    if (!win || !toolbar || tbW <= 0 || !win->hwndCanvas || !IsWindowVisible(win->hwndCanvas)) {
        return fallback;
    }
    int limit = -1;
    if (!ScrollbarsAreHidden() && !ScrollbarsUseOverlay()) {
        SCROLLBARINFO sbi{};
        sbi.cbSize = sizeof(sbi);
        if (GetScrollBarInfo(win->hwndCanvas, OBJID_VSCROLL, &sbi) && (sbi.rgstate[0] & STATE_SYSTEM_INVISIBLE) == 0 &&
            sbi.rcScrollBar.right > sbi.rcScrollBar.left) {
            RECT canvasWnd{};
            GetWindowRect(win->hwndCanvas, &canvasWnd);
            int mid = (canvasWnd.left + canvasWnd.right) / 2;
            // The bar sits on the right. Its inner edge is the left side.
            if (sbi.rcScrollBar.left >= mid) {
                POINT pt{sbi.rcScrollBar.left, sbi.rcScrollBar.top};
                ScreenToClient(toolbar, &pt);
                limit = pt.x;
            }
        }
    } else if (ScrollbarsUseOverlay() && win->overlayScrollV && win->overlayScrollV->hwnd &&
               IsWindowVisible(win->overlayScrollV->hwnd)) {
        RECT sbRc{};
        if (GetWindowRect(win->overlayScrollV->hwnd, &sbRc) && sbRc.right > sbRc.left) {
            RECT canvasWnd{};
            GetWindowRect(win->hwndCanvas, &canvasWnd);
            int mid = (canvasWnd.left + canvasWnd.right) / 2;
            if (sbRc.left >= mid) {
                POINT pt{sbRc.left, sbRc.top};
                ScreenToClient(toolbar, &pt);
                limit = pt.x;
            }
        }
    }
    if (limit > 0 && limit < tbW) {
        // Four device pixels short of the scrollbar's inner edge.
        limit -= 4;
        return limit > 0 ? limit : 1;
    }
    // Native bar not reported yet, but the canvas client already stops at it.
    if (!ScrollbarsUseOverlay() && !ScrollbarsAreHidden()) {
        RECT page{};
        if (GetClientRect(win->hwndCanvas, &page) && page.right > page.left) {
            MapWindowPoints(win->hwndCanvas, toolbar, (LPPOINT)&page, 2);
            if (page.right > 0 && page.right < tbW) {
                int right = page.right - 4;
                return right > 0 ? right : 1;
            }
        }
    }
    return fallback;
}

void ToolbarFindLayout(MainWindow* win) {
    if (!win || !win->hwndToolbar || !win->hwndToolbarFind) {
        return;
    }
    RECT trc{};
    GetClientRect(win->hwndToolbar, &trc);
    int tbW = trc.right;
    int tbH = trc.bottom;
    HWND box = win->hwndToolbarFind;
    ToolbarFindState* st = ToolbarFindGet(box);
    int buttonsRight = 0;
    RECT sample{};
    bool gotButton = false;
    int n = (int)SendMessageW(win->hwndToolbar, TB_BUTTONCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        TBBUTTON btn{};
        if (SendMessageW(win->hwndToolbar, TB_GETBUTTON, i, (LPARAM)&btn)) {
            if (btn.fsState & TBSTATE_HIDDEN) {
                continue;
            }
        }
        RECT br{};
        if (!SendMessageW(win->hwndToolbar, TB_GETITEMRECT, i, (LPARAM)&br)) {
            continue;
        }
        if (br.right <= br.left) {
            continue;
        }
        if (!gotButton) {
            sample = br;
            gotButton = true;
        }
        if (br.right > buttonsRight) {
            buttonsRight = br.right;
        }
    }
    int edge = GetSystemMetrics(SM_CXEDGE);
    if (edge < 1) {
        edge = 1;
    }
    int fieldH = gotButton ? (sample.bottom - sample.top) : tbH;
    int y = gotButton ? sample.top : 0;
    // Match the page box, whose rounded field is two pixels taller than the
    // toolbar icon row and centered around it.
    Rect pageField = WindowRect(win->hwndPageBg);
    bool matchesPageBox = !pageField.IsEmpty();
    if (matchesPageBox) {
        fieldH = pageField.dy;
        y = gotButton ? (sample.bottom - fieldH) / 2 : 0;
    }
    if (!matchesPageBox) {
        if (y < 0) {
            y = 0;
        }
        if (fieldH > tbH - y) {
            fieldH = tbH - y;
        }
    }
    int slot = gotButton ? (sample.right - sample.left) : fieldH;
    if (st) {
        st->iconSlot = slot;
    }
    int margin = sample.left > 0 ? sample.left : edge;
    int rightLimit = ToolbarFindRightLimit(win, win->hwndToolbar, tbW, margin) - 6;
    HFONT font = GetWindowFont(win->hwndToolbar);
    int cue = HwndMeasureText(box, "Find in document...", font).dx;
    int status = HwndMeasureText(box, "0000 / 0000", font).dx + edge * 2;
    int queryMin = HwndMeasureText(box, "0000", font).dx;
    ToolbarFindCluster cluster;
    ToolbarFindClusterMetrics(box, slot, &cluster);
    int fullRight = cluster.search + cluster.clear + cluster.gapDetailToClear + cluster.detail +
                    cluster.gapNavToDetail + cluster.nav * 2 + status;
    int minRight = cluster.search + cluster.clear + cluster.gapNavToDetail + cluster.nav * 2 + status;
    // Cue and the full hit cluster share one width, so typing does not resize the field.
    int preferred = edge * 2 + cue + edge + fullRight;
    // Below this the count and prev/next/clear/magnifier would overlap the query.
    int minShow = edge * 2 + queryMin + minRight;
    int avail = rightLimit - buttonsRight - margin;
    if (tbW <= 0 || tbH <= 0 || fieldH < 1 || avail < minShow) {
        ShowWindow(box, SW_HIDE);
        return;
    }
    int width = preferred;
    if (width > avail) {
        width = avail;
    }
    int x = rightLimit - width;
    if (x < buttonsRight + margin) {
        x = buttonsRight + margin;
        width = rightLimit - x;
    }
    if (width < minShow) {
        ShowWindow(box, SW_HIDE);
        return;
    }
    ShowWindow(box, SW_SHOW);
    SetWindowPos(box, HWND_TOP, x, y, width, fieldH, SWP_NOACTIVATE);
}

void ToolbarFindUpdateTheme(MainWindow* win) {
    ToolbarFindState* st = ToolbarFindGet(win);
    if (!st) {
        return;
    }
    ToolbarFindApplyColors(st);
    if (st->edit) {
        SetWindowTheme(st->edit, L"", L"");
        HFONT font = win->hwndToolbar ? GetWindowFont(win->hwndToolbar) : nullptr;
        if (font) {
            SetWindowFont(st->edit, font, FALSE);
        }
    }
    ToolbarFindLayout(win);
    InvalidateRect(st->hwnd, nullptr, TRUE);
}

void ToolbarFindSetStatus(MainWindow* win, const char* s) {
    ToolbarFindState* st = ToolbarFindGet(win);
    if (!st) {
        return;
    }
    const char* next = s ? s : "";
    const char* prev = st->status.Get();
    if (str::Eq(prev, next)) {
        return;
    }
    if (!st->hwnd) {
        st->status.SetCopy(next);
        return;
    }
    HFONT font = st->edit ? GetWindowFont(st->edit) : nullptr;
    int edge = GetSystemMetrics(SM_CXEDGE);
    if (edge < 1) {
        edge = 1;
    }
    int slot = HwndMeasureText(st->hwnd, "0000 / 0000", font).dx + edge * 2;
    auto textW = [&](const char* t) -> int {
        if (!t || !t[0]) {
            return 0;
        }
        return HwndMeasureText(st->hwnd, t, font).dx + edge * 2;
    };
    int oldW = textW(prev);
    st->status.SetCopy(next);
    int newW = textW(next);
    // "... / 647" -> "198 / 647" stays inside the reserved count slot.
    // Repaint that slot only, so the magnifier, arrows, and edit do not move.
    if (st->showStatus && st->hasQuery && !st->rcStatus.IsEmpty() && oldW <= slot && newW <= slot) {
        RECT rc{st->rcStatus.x, st->rcStatus.y, st->rcStatus.x + st->rcStatus.dx, st->rcStatus.y + st->rcStatus.dy};
        InvalidateRect(st->hwnd, &rc, FALSE);
        return;
    }
    ToolbarFindLayoutInner(st);
}

void ToolbarFindFlashStatus(MainWindow* win, bool flash) {
    ToolbarFindState* st = ToolbarFindGet(win);
    if (!st || st->flash == flash) {
        return;
    }
    st->flash = flash;
    InvalidateRect(st->hwnd, nullptr, FALSE);
}

TempStr ToolbarFindStatusText(MainWindow* win) {
    ToolbarFindState* st = ToolbarFindGet(win);
    if (!st || st->status.empty()) {
        return nullptr;
    }
    return str::DupTemp(st->status.Get());
}

void ToolbarFindSetText(MainWindow* win, const char* s, bool suppress) {
    ToolbarFindState* st = ToolbarFindGet(win);
    if (!st || !st->edit) {
        return;
    }
    if (suppress) {
        st->suppress = true;
    }
    HwndSetText(st->edit, s ? s : "");
    if (suppress) {
        st->suppress = false;
        ToolbarFindLayoutInner(st);
    }
}

void ToolbarFindFocus(MainWindow* win) {
    HWND ed = ToolbarFindEdit(win);
    if (!ed) {
        return;
    }
    if (win->hwndToolbarFind && !IsWindowVisible(win->hwndToolbarFind)) {
        ShowWindow(win->hwndToolbarFind, SW_SHOW);
    }
    win->hwndFindEdit = ed;
    HwndSetFocus(ed);
    Edit_SetSel(ed, 0, -1);
}

void ToolbarFindCloseDetailed(MainWindow* win, bool focusToolbarEdit) {
    if (!win) {
        return;
    }
    AutoFreeStr text;
    if (IsFindWindowVisible(win) && win->hwndFindEdit) {
        text.SetCopy(HwndGetTextTemp(win->hwndFindEdit));
    }
    if (IsFindWindowVisible(win)) {
        HideFindWindow(win, true);
    }
    if (text.Get()) {
        ToolbarFindSetText(win, text.Get(), true);
    }
    HWND ed = ToolbarFindEdit(win);
    if (ed) {
        win->hwndFindEdit = ed;
    }
    if (focusToolbarEdit) {
        ToolbarFindFocus(win);
        return;
    }
    HWND dest = win->hwndCanvas ? win->hwndCanvas : win->hwndFrame;
    if (dest) {
        HwndSetFocus(dest);
    }
}

// "ShowFindBar" is the entry point used by FindFirst/Ctrl+F.
// SearchUIFloating false: focus the toolbar box.
// SearchUIFloating true: open the existing floating window (Detailed Search).
// The docked overlay is not shown from here.
void ShowFindBar(MainWindow* win) {
    if (!win) {
        return;
    }
    // Ctrl+F stays in the toolbar field. Detailed Search is the list button.
    if (IsFindWindowVisible(win) && !IsFindWindowDocked(win)) {
        if (win->hwndFindEdit) {
            FocusFindEditSelectAll(win);
        }
        RefreshFindSearchBlockedStatus(win);
        return;
    }
    HWND ed = ToolbarFindEdit(win);
    if (ed) {
        win->hwndFindEdit = ed;
    }
    ToolbarFindFocus(win);
    RefreshFindSearchBlockedStatus(win);
}

void StealFocusFromFindUI(MainWindow* win) {
    if (!win) {
        return;
    }
    HWND focus = GetFocus();
    if (focus && IsFindUIHwnd(win, focus)) {
        HwndSetFocus(win->hwndFrame);
    }
}

void DestroyFindUI(MainWindow* win) {
    if (!win) {
        return;
    }
    DeleteFindWindow(win);
    DeleteFindBar(win);
    // Tab switch clears the search. The toolbar box stays; drop its query
    // without starting another search.
    HWND ed = ToolbarFindEdit(win);
    win->hwndFindEdit = ed;
    ToolbarFindSetText(win, "", true);
    ToolbarFindSetStatus(win, "");
}

void HideFindBar(MainWindow* win, bool keepSearchState) {
    if (!win) {
        return;
    }
    if (!keepSearchState) {
        CloseFindUI(win);
        return;
    }
    // temporarily hide both UIs (e.g. compact <-> floating toggle) without
    // destroying HWNDs or dropping search state
    if (IsFindWindowVisible(win)) {
        HideFindWindow(win, true);
    }
    if (win->findBar && IsFindBarVisible(win)) {
        ShowWindow(win->findBar->hwnd, SW_HIDE);
    }
    StealFocusFromFindUI(win);
    FindBarResyncActiveEdit(win);
}

// note: the floating window is not anchored to the search icon, so "visible"
// here means specifically the compact bar (used to reposition it on move)
bool IsFindBarVisible(MainWindow* win) {
    return IsFindWindowVisible(win) && IsFindWindowDocked(win);
}

bool IsFindUIVisible(MainWindow* win) {
    if (!win) {
        return false;
    }
    if (IsFindWindowVisible(win)) {
        return true;
    }
    // The toolbar field is the find UI while it holds a query. Match highlights
    // and the position gutter follow that, the same as the detailed window.
    HWND ed = ToolbarFindEdit(win);
    return ed && GetWindowTextLengthW(ed) > 0;
}

bool IsFindUIHwnd(MainWindow* win, HWND hwnd) {
    if (!win || !hwnd) {
        return false;
    }
    HWND barHwnd = win->findBar ? win->findBar->hwnd : nullptr;
    HWND winHwnd = FindWindowHwnd(win);
    for (HWND h = hwnd; h; h = GetParent(h)) {
        if (barHwnd && h == barHwnd) {
            return true;
        }
        if (winHwnd && h == winHwnd) {
            return true;
        }
        if (win->hwndToolbarFind && h == win->hwndToolbarFind) {
            return true;
        }
    }
    return false;
}

void FocusFindEditSelectAll(MainWindow* win) {
    if (!win->hwndFindEdit) {
        return;
    }
    HwndSetFocus(win->hwndFindEdit);
    Edit_SetSel(win->hwndFindEdit, 0, -1);
}

void FindBarClearEditText(MainWindow* win) {
    FindWindowClearEditText(win);
}

void FindBarResyncActiveEdit(MainWindow* win) {
    if (!win) {
        return;
    }
    if (IsFindWindowVisible(win)) {
        FindWindowResyncActiveEdit(win);
        return;
    }
    HWND ed = ToolbarFindEdit(win);
    if (ed) {
        win->hwndFindEdit = ed;
        return;
    }
    if (win->findBar && win->findBar->edit) {
        win->hwndFindEdit = win->findBar->edit->hwnd;
    } else {
        win->hwndFindEdit = nullptr;
    }
}

void ToggleFloatingFindUI(MainWindow* win) {
    bool wasShowing = IsFindWindowVisible(win);
    gGlobalPrefs->searchUIFloating = !gGlobalPrefs->searchUIFloating;
    SaveSettings();
    if (!wasShowing) {
        return;
    }
    // Same HWND, edit control, result model, and event handlers. Only its
    // presentation changes, so no text copy and no search restart are needed.
    FindWindowSetDocked(win, !gGlobalPrefs->searchUIFloating);
    RefreshFindUIStatus(win);
    HwndSetFocus(win->hwndFindEdit);
}

void FindBarReposition(MainWindow* win) {
    if (!IsFindBarVisible(win)) {
        return;
    }
    // the current document may not support find (e.g. switched to an
    // image-only doc / CHM); don't leave an orphaned, inert bar floating
    if (!NeedsFindUI(win)) {
        HideFindBar(win);
        return;
    }
    FindWindowReposition(win);
}

void FindBarSetStatus(MainWindow* win, const char* s) {
    if (win->findBar && win->findBar->status) {
        HwndSetText(win->findBar->status->hwnd, s ? s : "");
    }
    if (win->findWindow) {
        FindWindowSetStatusText(win, s);
    }
    ToolbarFindSetStatus(win, s);
}

TempStr FindUIGetStatusText(MainWindow* win) {
    TempStr s = FindWindowGetStatusText(win);
    if (str::IsEmpty(s) && win->findBar && win->findBar->status) {
        s = HwndGetTextTemp(win->findBar->status->hwnd);
    }
    if (str::IsEmpty(s)) {
        s = ToolbarFindStatusText(win);
    }
    return s;
}

void FindBarBeginStatusCompleteFlash(MainWindow* win) {
    if (!win || !win->hwndFrame) {
        return;
    }
    if (win->findBar) {
        win->findBar->FlashStatusText(true);
    }
    FindWindowFlashStatusText(win, true);
    ToolbarFindFlashStatus(win, true);
    SetTimer(win->hwndFrame, kFindStatusCompleteFlashTimerId, kFindStatusCompleteFlashMs, nullptr);
}

void FindStatusCompleteFlashTimerFired(MainWindow* win) {
    if (!win || !win->hwndFrame) {
        return;
    }
    KillTimer(win->hwndFrame, kFindStatusCompleteFlashTimerId);
    if (win->findBar) {
        win->findBar->FlashStatusText(false);
    }
    FindWindowFlashStatusText(win, false);
    ToolbarFindFlashStatus(win, false);
}

void RefreshFindUIStatus(MainWindow* win) {
    if (!win) {
        return;
    }
    if (!IsDocumentSearchReady(win) && IsFindUIVisible(win)) {
        RefreshFindSearchBlockedStatus(win);
        return;
    }
    if (win->findCountValid) {
        UpdateFindMatchCountDisplay(win);
        return;
    }
    TempStr s = FindUIGetStatusText(win);
    if (!str::IsEmpty(s)) {
        FindBarSetStatus(win, s);
    }
}

void FindBarSetMatchCaseChecked(MainWindow* win, bool checked) {
    FindWindowSetMatchCaseChecked(win, checked);
}

void FindBarSetMatchWholeWordChecked(MainWindow* win, bool checked) {
    FindWindowSetMatchWholeWordChecked(win, checked);
}
