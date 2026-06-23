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
#include "SelectionToolbar.h"

#define kSelectionToolbarClassName L"SumatraSelectionToolbar"

struct SelectionToolbarButton {
    int cmdId;
    const char* label; // English literal, translated via _TRA at paint time
    Rect rc;           // position within the toolbar client area
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
    Rect lastPlaced; // last screen rect we moved the window to (avoids redundant SetWindowPos)
    SelectionToolbarButton buttons[6];
};

static void InitButtons(SelectionToolbar* tb) {
    int i = 0;
    tb->buttons[i++] = {CmdCreateAnnotHighlight, "Highlight", {}};
    tb->buttons[i++] = {CmdCreateAnnotUnderline, "Underline", {}};
    tb->buttons[i++] = {CmdCreateAnnotSquiggly, "Squiggly", {}};
    tb->buttons[i++] = {CmdCreateAnnotStrikeOut, "Strike Out", {}};
    tb->buttons[i++] = {CmdCopySelection, "Copy", {}};
    tb->buttons[i++] = {CmdAnalyzeSelectionWithDoubao, "Ask AI", {}};
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

static void LayoutToolbar(SelectionToolbar* tb) {
    HWND hwnd = tb->hwnd;
    int padX = DpiScale(hwnd, kBtnPadX);
    int padY = DpiScale(hwnd, kBtnPadY);
    int margin = DpiScale(hwnd, kMargin);
    int gap = DpiScale(hwnd, kBtnGap);

    int x = margin;
    int maxDy = 0;
    int n = (int)dimof(tb->buttons);
    for (int i = 0; i < n; i++) {
        SelectionToolbarButton& b = tb->buttons[i];
        const char* txt = _TRA(b.label);
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
    // normalize button heights to the tallest one
    for (int i = 0; i < n; i++) {
        tb->buttons[i].rc.dy = maxDy;
    }
    tb->size = Size(x + margin, maxDy + 2 * margin);
    UpdateFloatingPopupWindowRgn(hwnd, kToolbarCornerRadius);
}

static int ButtonFromPoint(SelectionToolbar* tb, int x, int y) {
    Point pt(x, y);
    int n = (int)dimof(tb->buttons);
    for (int i = 0; i < n; i++) {
        if (tb->buttons[i].rc.Contains(pt)) {
            return i;
        }
    }
    return -1;
}

static void PaintToolbar(SelectionToolbar* tb, HDC hdc) {
    HWND hwnd = tb->hwnd;
    Rect rc = ClientRect(hwnd);
    COLORREF bgCol = FloatingPopupBg();
    COLORREF borderCol = FloatingPopupBorderColor();
    COLORREF textCol = FloatingPopupTextColor();
    COLORREF hoverBg = FloatingPopupHoverBg(bgCol);
    int cornerRadius = DpiScale(hwnd, kToolbarCornerRadius);
    int btnRadius = DpiScale(hwnd, kToolbarButtonRadius);

    FillFloatingPopupRoundedRect(hdc, rc, cornerRadius, bgCol);
    StrokeFloatingPopupRoundedRect(hdc, rc, cornerRadius, borderCol);

    ScopedSelectObject selFont(hdc, tb->font);
    SetBkMode(hdc, TRANSPARENT);
    int n = (int)dimof(tb->buttons);
    for (int i = 0; i < n; i++) {
        SelectionToolbarButton& b = tb->buttons[i];
        bool isHot = (i == tb->hotIndex);
        if (isHot) {
            FillFloatingPopupRoundedRect(hdc, b.rc, btnRadius, hoverBg);
        }
        SetTextColor(hdc, textCol);
        const char* txt = _TRA(b.label);
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
            return TRUE; // we paint the whole client area in WM_PAINT

        case WM_MOUSEACTIVATE:
            // don't steal focus/activation from the main window
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = ButtonFromPoint(tb, x, y);
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
            tb->pressedIndex = ButtonFromPoint(tb, x, y);
            return 0;
        }

        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int idx = ButtonFromPoint(tb, x, y);
            int pressed = tb->pressedIndex;
            tb->pressedIndex = -1;
            if (idx >= 0 && idx == pressed) {
                int cmdId = tb->buttons[idx].cmdId;
                MainWindow* win = tb->win;
                HideSelectionToolbar(win);
                // post so we finish handling this message before the command
                // (which may open dialogs) runs
                HwndPostCommand(win->hwndFrame, cmdId);
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

// computes the bounding box of the current text selection in canvas-client
// coordinates, clipped to the visible canvas. Returns false if there is no
// usable selection.
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
    // prefer above the selection, fall back to below it
    int y = sel.y - gap - h;
    if (y < canvas.y) {
        y = sel.y + sel.dy + gap;
    }

    // clamp inside the canvas
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
    // skip redundant moves: repositioning happens on every document paint, so
    // calling SetWindowPos unconditionally would add overhead and can trigger
    // extra invalidation of the canvas underneath the toolbar
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
    InitButtons(tb);
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
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    if (!EngineSupportsAnnotations(dm->GetEngine())) {
        return;
    }
    // only for real text selections (not image/rectangle selections)
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
    // OnSelectionStop runs before the async repaint that finalizes page layout,
    // so the first selection after cold start often can't compute bounds yet.
    // Retry here after PaintSelection, when selection rects are usable.
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
    PositionToolbar(tb, sel);
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
