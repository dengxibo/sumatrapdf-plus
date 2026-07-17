/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Selection.h"
#include "Commands.h"
#include "Translations.h"
#include "Theme.h"
#include "FloatingPopupStyle.h"
#include "WordLookup.h"
#include "SumatraPDF.h"
#include "SelectionToolbar.h"
#include "EbookAnnotations.h"

#define kSelectionToolbarClassName L"SumatraSelectionToolbar"

struct SelectionToolbarButton {
    int cmdId;
    const char* label; // English literal, translated via _TRA at paint time
    bool enabled = true;
    Rect rc; // position within the toolbar client area
};

struct SelectionToolbar {
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr; // tab the current selection belongs to
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    bool fontOwned = false;
    int hotIndex = -1;
    int pressedIndex = -1;
    bool trackingMouse = false;
    Size size;
    Rect lastPlaced;    // last screen rect we moved the window to (avoids redundant SetWindowPos)
    Rect lastSelBounds; // last canvas-space selection bounds used for placement
    SelectionToolbarButton buttons[9];
    int nButtons = 0;
};

static void InitButtons(SelectionToolbar* tb, MainWindow* win) {
    int i = 0;
    if (gGlobalPrefs->enableAskAI) {
        tb->buttons[i++] = {CmdAnalyzeSelectionWithDoubao, "Ask AI", true, {}};
    }
    bool lookupEnabled = CanLookupSelectionInTab(win->CurrentTab());
    tb->buttons[i++] = {CmdLookupSelection, "Look Up", lookupEnabled, {}};
    tb->buttons[i++] = {CmdReadAloudSelection, "Read Aloud", HasPermission(Perm::CopySelection), {}};
    tb->buttons[i++] = {CmdCopySelection, "Copy", true, {}};

    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (engine && EngineSupportsAnnotations(engine)) {
        tb->buttons[i++] = {CmdCreateAnnotHighlight, "Highlight", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotUnderline, "Underline", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotSquiggly, "Squiggly", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotStrikeOut, "Strike Out", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotText, "&Text", true, {}};
    } else if (EbookAnnotationsSupported(win->CurrentTab())) {
        tb->buttons[i++] = {CmdCreateAnnotHighlight, "Highlight", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotUnderline, "Underline", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotSquiggly, "Squiggly", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotStrikeOut, "Strike Out", true, {}};
        tb->buttons[i++] = {CmdCreateAnnotText, "&Text", true, {}};
    }
    tb->nButtons = i;
}

constexpr int kBtnPadX = 8; // horizontal padding inside a button
constexpr int kBtnPadY = 4; // vertical padding inside a button
constexpr int kMargin = 5;  // margin around the row of buttons
constexpr int kBtnGap = 2;  // gap between buttons
// compact variant of the dictionary popup chrome (same palette, smaller scale)
constexpr int kToolbarCornerRadius = 10;
constexpr int kToolbarButtonRadius = 6;
constexpr int kToolbarFontPct = 108;

static HFONT CreateScaledFontFrom(HFONT base, int pct) {
    if (!base) {
        return nullptr;
    }
    LOGFONTW lf{};
    GetObjectW(base, sizeof(lf), &lf);
    lf.lfHeight = MulDiv(lf.lfHeight, pct, 100);
    return CreateFontIndirectW(&lf);
}

static TempStr ToolbarButtonLabelTemp(const SelectionToolbarButton& button) {
    TempStr label = str::ReplaceTemp(_TRA(button.label), "(&T)", "");
    return str::ReplaceTemp(label, "&", "");
}

static void LayoutToolbar(SelectionToolbar* tb) {
    HWND hwnd = tb->hwnd;
    int padX = DpiScale(hwnd, kBtnPadX);
    int padY = DpiScale(hwnd, kBtnPadY);
    int margin = DpiScale(hwnd, kMargin);
    int gap = DpiScale(hwnd, kBtnGap);

    int x = margin;
    int maxDy = 0;
    int n = tb->nButtons;
    for (int i = 0; i < n; i++) {
        SelectionToolbarButton& b = tb->buttons[i];
        TempStr txt = ToolbarButtonLabelTemp(b);
        Size s = HwndMeasureText(hwnd, txt, tb->font);
        int dx = s.dx + 2 * padX;
        int dy = s.dy + 2 * padY;
        b.rc = Rect(x, margin, dx, dy);
        x += dx + gap;
        if (dy > maxDy) {
            maxDy = dy;
        }
    }
    if (n > 0) {
        x -= gap;
    }
    for (int i = 0; i < n; i++) {
        tb->buttons[i].rc.dy = maxDy;
    }
    tb->size = Size(x + margin, maxDy + 2 * margin);
    UpdateFloatingPopupWindowRgn(hwnd, kToolbarCornerRadius);
}

static int ButtonFromPoint(SelectionToolbar* tb, int x, int y) {
    Point pt(x, y);
    for (int i = 0; i < tb->nButtons; i++) {
        if (tb->buttons[i].rc.Contains(pt)) {
            return i;
        }
    }
    return -1;
}

static bool GetSelectionEndPoint(MainWindow* win, Point& out) {
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSelection || dm->textSelection->result.len <= 0) {
        return false;
    }
    TextSel& result = dm->textSelection->result;
    int i = result.len - 1;
    Rect r = dm->CvtToScreen(result.pages[i], ToRectF(result.rects[i]));
    if (r.IsEmpty()) {
        return false;
    }
    out = Point(r.x + r.dx, r.y + r.dy / 2);
    return true;
}

static void PaintToolbar(SelectionToolbar* tb, HDC hdc) {
    HWND hwnd = tb->hwnd;
    Rect rc = ClientRect(hwnd);
    COLORREF bgCol = FloatingPopupBg();
    COLORREF borderCol = FloatingPopupBorderColor();
    COLORREF textCol = FloatingPopupTextColor();
    COLORREF mutedCol = FloatingPopupMutedTextColor();
    COLORREF hoverBg = FloatingPopupHoverBg(bgCol);
    int cornerRadius = DpiScale(hwnd, kToolbarCornerRadius);
    int btnRadius = DpiScale(hwnd, kToolbarButtonRadius);

    FillFloatingPopupRoundedRect(hdc, rc, cornerRadius, bgCol);
    StrokeFloatingPopupRoundedRect(hdc, rc, cornerRadius, borderCol);

    ScopedSelectObject selFont(hdc, tb->font);
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < tb->nButtons; i++) {
        SelectionToolbarButton& b = tb->buttons[i];
        bool isHot = b.enabled && (i == tb->hotIndex);
        if (isHot) {
            FillFloatingPopupRoundedRect(hdc, b.rc, btnRadius, hoverBg);
        }
        SetTextColor(hdc, b.enabled ? textCol : mutedCol);
        TempStr txt = ToolbarButtonLabelTemp(b);
        DrawCenteredText(hdc, b.rc, txt);
    }
}

static void TrackMouseLeave(SelectionToolbar* tb) {
    if (tb->trackingMouse) {
        return;
    }
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = tb->hwnd;
    TrackMouseEvent(&tme);
    tb->trackingMouse = true;
}

static LRESULT CALLBACK WndProcSelectionToolbar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SelectionToolbar* tb;
    if (msg == WM_NCCREATE) {
        LPCREATESTRUCT cs = reinterpret_cast<LPCREATESTRUCT>(lp);
        tb = reinterpret_cast<SelectionToolbar*>(cs->lpCreateParams);
        tb->hwnd = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tb));
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    tb = reinterpret_cast<SelectionToolbar*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!tb) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return TRUE;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = ButtonFromPoint(tb, x, y);
            if (idx >= 0 && !tb->buttons[idx].enabled) {
                idx = -1;
            }
            if (idx != tb->hotIndex) {
                tb->hotIndex = idx;
                HwndScheduleRepaint(hwnd);
            }
            TrackMouseLeave(tb);
            return 0;
        }

        case WM_MOUSELEAVE:
            tb->trackingMouse = false;
            if (tb->hotIndex != -1) {
                tb->hotIndex = -1;
                HwndScheduleRepaint(hwnd);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = ButtonFromPoint(tb, x, y);
            if (idx >= 0 && tb->buttons[idx].enabled) {
                tb->pressedIndex = idx;
            } else {
                tb->pressedIndex = -1;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = ButtonFromPoint(tb, x, y);
            int pressed = tb->pressedIndex;
            tb->pressedIndex = -1;
            if (idx >= 0 && idx == pressed && tb->buttons[idx].enabled) {
                int cmdId = tb->buttons[idx].cmdId;
                MainWindow* win = tb->win;
                LPARAM commandPoint = 0;
                if (cmdId == CmdCreateAnnotText) {
                    Point selectionEnd;
                    if (GetSelectionEndPoint(win, selectionEnd)) {
                        commandPoint = MAKELPARAM(selectionEnd.x, selectionEnd.y);
                    }
                    DeleteOldSelectionInfo(win, true);
                }
                HideSelectionToolbar(win);
                HwndPostCommand(win->hwndFrame, cmdId, commandPoint);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintToolbar(tb, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void RegisterSelectionToolbarClass() {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEX wcex{};
    FillWndClassEx(wcex, kSelectionToolbarClassName, WndProcSelectionToolbar);
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&wcex);
    registered = true;
}

static bool GetSelectionBounds(MainWindow* win, Rect& out) {
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->selectionOnPage) {
        return false;
    }
    Rect canvas = win->canvasRc;
    Rect bounds;
    bool first = true;
    for (SelectionOnPage& sel : *tab->selectionOnPage) {
        Rect r = sel.GetRect(dm).Intersect(canvas);
        if (r.IsEmpty()) {
            continue;
        }
        if (first) {
            bounds = r;
            first = false;
        } else {
            bounds = bounds.Union(r);
        }
    }
    if (first) {
        return false;
    }
    out = bounds;
    return true;
}

static void PositionToolbar(SelectionToolbar* tb, const Rect& sel) {
    MainWindow* win = tb->win;
    Rect canvas = win->canvasRc;
    int gap = DpiScale(tb->hwnd, 6);
    int w = tb->size.dx;
    int h = tb->size.dy;

    int x = sel.x + sel.dx / 2 - w / 2;
    int y = sel.y - gap - h;
    if (y < canvas.y) {
        y = sel.y + sel.dy + gap;
    }

    int maxX = canvas.x + canvas.dx - w;
    if (x > maxX) {
        x = maxX;
    }
    if (x < canvas.x) {
        x = canvas.x;
    }
    int maxY = canvas.y + canvas.dy - h;
    if (y > maxY) {
        y = maxY;
    }
    if (y < canvas.y) {
        y = canvas.y;
    }

    POINT p{x, y};
    ClientToScreen(win->hwndCanvas, &p);
    Rect placed(p.x, p.y, w, h);
    if (placed == tb->lastPlaced) {
        return;
    }
    tb->lastPlaced = placed;
    SetWindowPos(tb->hwnd, nullptr, p.x, p.y, w, h, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
}

static SelectionToolbar* GetOrCreateToolbar(MainWindow* win) {
    if (win->selectionToolbar) {
        return win->selectionToolbar;
    }
    RegisterSelectionToolbarClass();
    auto tb = new SelectionToolbar();
    tb->win = win;
    tb->font = CreateScaledFontFrom(GetAppFont(), kToolbarFontPct);
    tb->fontOwned = tb->font && tb->font != GetAppFont();
    DWORD style = WS_POPUP;
    DWORD styleEx = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    HWND hwnd = CreateWindowEx(styleEx, kSelectionToolbarClassName, nullptr, style, 0, 0, 0, 0, win->hwndFrame, nullptr,
                               GetModuleHandle(nullptr), tb);
    if (!hwnd) {
        delete tb;
        return nullptr;
    }
    win->selectionToolbar = tb;
    return tb;
}

void ShowSelectionToolbar(MainWindow* win) {
    if (!win || !gGlobalPrefs->annotations.selectionToolbar) {
        return;
    }
    if (IsWordLookupVisible()) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    if (dm->textSelection->result.len <= 0) {
        return;
    }
    Rect sel;
    if (!GetSelectionBounds(win, sel)) {
        return;
    }
    SelectionToolbar* tb = GetOrCreateToolbar(win);
    if (!tb) {
        return;
    }
    tb->tab = win->CurrentTab();
    tb->hotIndex = -1;
    tb->pressedIndex = -1;
    tb->lastSelBounds = sel;
    InitButtons(tb, win);
    LayoutToolbar(tb);
    PositionToolbar(tb, sel);
    ShowWindow(tb->hwnd, SW_SHOWNOACTIVATE);
    HwndScheduleRepaint(tb->hwnd);
}

void UpdateSelectionToolbarPosition(MainWindow* win) {
    if (!win) {
        return;
    }
    SelectionToolbar* tb = win->selectionToolbar;
    if (!tb || !tb->hwnd || !IsWindowVisible(tb->hwnd)) {
        if (win->showSelection) {
            ShowSelectionToolbar(win);
        }
        return;
    }
    if (win->CurrentTab() != tb->tab) {
        HideSelectionToolbar(win);
        return;
    }
    Rect sel;
    if (!GetSelectionBounds(win, sel)) {
        HideSelectionToolbar(win);
        return;
    }
    // Canvas is repainted often during read-aloud highlight updates. Skip all
    // toolbar work when the selection has not moved, otherwise SetWindowRgn /
    // ScheduleRepaint makes the floating bar jitter.
    if (sel == tb->lastSelBounds) {
        return;
    }
    tb->lastSelBounds = sel;

    Rect prevPlaced = tb->lastPlaced;
    InitButtons(tb, win);
    LayoutToolbar(tb);
    PositionToolbar(tb, sel);
    if (tb->lastPlaced != prevPlaced) {
        HwndScheduleRepaint(tb->hwnd);
    }
}

void HideSelectionToolbar(MainWindow* win) {
    SelectionToolbar* tb = win ? win->selectionToolbar : nullptr;
    if (!tb || !tb->hwnd) {
        return;
    }
    if (IsWindowVisible(tb->hwnd)) {
        ShowWindow(tb->hwnd, SW_HIDE);
    }
    tb->hotIndex = -1;
    tb->pressedIndex = -1;
    tb->tab = nullptr;
    tb->lastPlaced = Rect();
    tb->lastSelBounds = Rect();
}

void DeleteSelectionToolbar(MainWindow* win) {
    SelectionToolbar* tb = win ? win->selectionToolbar : nullptr;
    if (!tb) {
        return;
    }
    if (tb->hwnd) {
        DestroyWindow(tb->hwnd);
    }
    if (tb->fontOwned && tb->font) {
        DeleteObject(tb->font);
    }
    delete tb;
    win->selectionToolbar = nullptr;
}
