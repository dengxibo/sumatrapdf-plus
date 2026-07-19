/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinDynCalls.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

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

#include "utils/Log.h"

constexpr UINT_PTR kFindEditSubclassId = 9101;

static LRESULT CALLBACK FindEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId,
                                             DWORD_PTR refData) {
    MainWindow* win = (MainWindow*)refData;
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
    int statusDx = scale(88);
    // Keep enough room for the loading message (and a typical match counter).
    // The compact bar is an owned popup, so its former fixed widths could push
    // the status text beyond a narrow frame. Prefer shrinking the edit instead.
    const char* loadingMsg = _TRA("Please wait - loading...");
    const char* countSample = "99999 / 99999";
    Size loadingSz = HwndMeasureText(status->hwnd, loadingMsg, status->font);
    Size countSz = HwndMeasureText(status->hwnd, countSample, status->font);
    statusDx = std::max(statusDx, std::max(loadingSz.dx, countSz.dx) + scale(4));

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
                if (!IsFindUIVisible(win)) {
                    FindFirst(win);
                } else {
                    FocusFindEditSelectAll(win);
                }
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

// "ShowFindBar" is the entry point used by FindFirst/Ctrl+F; it shows whichever
// find UI the user has chosen (compact overlay or floating window)
void ShowFindBar(MainWindow* win) {
    // Both appearances are one FindWindowWnd. The compact appearance only
    // changes its frame/layout and hides the result list.
    ShowFindWindow(win);
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
    win->hwndFindEdit = nullptr;
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
    return IsFindWindowVisible(win);
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
}

TempStr FindUIGetStatusText(MainWindow* win) {
    TempStr s = FindWindowGetStatusText(win);
    if (str::IsEmpty(s) && win->findBar && win->findBar->status) {
        s = HwndGetTextTemp(win->findBar->status->hwnd);
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
    if (s && str::Eq(s, _TRA("Please wait - loading..."))) {
        FindBarSetStatus(win, "");
        return;
    }
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
