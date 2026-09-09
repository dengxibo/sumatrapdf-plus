/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/BitManip.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"
#include "utils/WinDynCalls.h"

#include "wingui/UIModels.h"

#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Theme.h"

#include "utils/Log.h"

//- Splitter

// the technique for drawing the splitter for non-live resize is described
// at http://www.catch22.net/tuts/splitter-windows

Kind kindSplitter = "splitter";

static void DrawXorBar(HDC hdc, HBRUSH br, int x1, int y1, int width, int height) {
    SetBrushOrgEx(hdc, x1, y1, nullptr);
    HBRUSH hbrushOld = (HBRUSH)SelectObject(hdc, br);
    PatBlt(hdc, x1, y1, width, height, PATINVERT);
    SelectObject(hdc, hbrushOld);
}

static HDC InitDraw(HWND hwnd, Rect& rc) {
    rc = ChildPosWithinParent(hwnd);
    HDC hdc = GetDC(GetParent(hwnd));
    SetROP2(hdc, R2_NOTXORPEN);
    return hdc;
}

static void DrawResizeLineV(HWND hwnd, HBRUSH br, int x) {
    Rect rc;
    HDC hdc = InitDraw(hwnd, rc);
    DrawXorBar(hdc, br, x, rc.y, 4, rc.dy);
    ReleaseDC(GetParent(hwnd), hdc);
}

static void DrawResizeLineH(HWND hwnd, HBRUSH br, int y) {
    Rect rc;
    HDC hdc = InitDraw(hwnd, rc);
    DrawXorBar(hdc, br, rc.x, y, rc.dx, 4);
    ReleaseDC(GetParent(hwnd), hdc);
}

static void DrawResizeLineVH(HWND hwnd, HBRUSH br, bool isVert, Point pos) {
    if (isVert) {
        DrawResizeLineV(hwnd, br, pos.x);
    } else {
        DrawResizeLineH(hwnd, br, pos.y);
    }
}

static void DrawResizeLine(HWND hwnd, HBRUSH br, SplitterType stype, bool erasePrev, bool drawCurr,
                           Point& prevResizeLinePos) {
    Point pos = HwndGetCursorPos(GetParent(hwnd));
    bool isVert = stype != SplitterType::Horiz;

    if (erasePrev) {
        DrawResizeLineVH(hwnd, br, isVert, prevResizeLinePos);
    }
    if (drawCurr) {
        DrawResizeLineVH(hwnd, br, isVert, pos);
    }
    prevResizeLinePos = pos;
}

static WORD dotPatternBmp[8] = {0x00aa, 0x0055, 0x00aa, 0x0055, 0x00aa, 0x0055, 0x00aa, 0x0055};

Splitter::Splitter() {
    kind = kindSplitter;
}

Splitter::~Splitter() {
    DeleteObject(brush);
    DeleteObject(bmp);
}

HWND Splitter::Create(const CreateArgs& args) {
    ReportIf(!args.parent);

    isLive = args.isLive;
    type = args.type;
    auto bgCol = args.backgroundColor;
    if (bgCol == kColorUnset) {
        bgCol = GetSysColor(COLOR_BTNFACE);
    }
    SetColors(kColorUnset, bgCol);

    bmp = CreateBitmap(8, 8, 1, 1, dotPatternBmp);
    ReportIf(!bmp);
    brush = CreatePatternBrush(bmp);
    ReportIf(!brush);

    DWORD style = GetWindowLong(args.parent, GWL_STYLE);
    parentClipsChildren = bit::IsMaskSet<DWORD>(style, WS_CLIPCHILDREN);

    CreateCustomArgs cargs;
    // cargs.className = L"SplitterWndClass";
    cargs.parent = args.parent;
    cargs.style = WS_CHILDWINDOW | WS_CLIPSIBLINGS;
    cargs.exStyle = 0;
    CreateCustom(cargs);

    return hwnd;
}

LRESULT Splitter::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (WM_ERASEBKGND == msg) {
        // TODO: should this be FALSE?
        return TRUE;
    }

    if (WM_LBUTTONDOWN == msg) {
        SetCapture(hwnd);
        // reflect the active (dragging) separator state immediately
        HwndScheduleRepaint(hwnd);
        if (!isLive) {
            if (parentClipsChildren) {
                SetWindowStyle(GetParent(hwnd), WS_CLIPCHILDREN, false);
            }
            DrawResizeLine(hwnd, brush, type, false, true, prevResizeLinePos);
        }
        return 1;
    }

    if (WM_LBUTTONUP == msg) {
        if (!isLive) {
            DrawResizeLine(hwnd, brush, type, true, false, prevResizeLinePos);
            if (parentClipsChildren) {
                SetWindowStyle(GetParent(hwnd), WS_CLIPCHILDREN, true);
            }
        }
        ReleaseCapture();
        // onMove can perform a costly final EPUB pagination. Do not leave the
        // resize cursor stuck on screen throughout that synchronous work.
        SetCursorCached(IDC_ARROW);
        Splitter::MoveEvent arg;
        arg.w = this;
        arg.finishedDragging = true;
        onMove.Call(&arg);
        HwndScheduleRepaint(hwnd);
        return 0;
    }

    if (WM_MOUSEMOVE == msg) {
        LPWSTR curId = IDC_SIZENS;
        if (SplitterType::Vert == type) {
            curId = IDC_SIZEWE;
        }
        if (!mouseTracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            mouseTracking = true;
        }
        if (!isMouseOver) {
            isMouseOver = true;
            HwndScheduleRepaint(hwnd);
        }
        if (hwnd == GetCapture()) {
            Splitter::MoveEvent arg;
            arg.w = this;
            arg.finishedDragging = false;
            onMove.Call(&arg);
            if (arg.resizeAllowed && !isLive) {
                DrawResizeLine(hwnd, brush, type, true, true, prevResizeLinePos);
            }
        }
        SetCursorCached(curId);
        return 0;
    }

    if (WM_MOUSELEAVE == msg) {
        mouseTracking = false;
        if (isMouseOver) {
            isMouseOver = false;
            HwndScheduleRepaint(hwnd);
        }
        return 0;
    }

    if (WM_PAINT == msg) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // Keep the wider mouse hit band visually merged into the sidebar.
        // bgColor is captured when the control is created and can be a stale
        // system color after a theme switch (most visible in Dracula).
        AutoDeleteBrush brBg = CreateSolidBrush(ThemeSidebarBackgroundColor());
        FillRect(hdc, &ps.rcPaint, brBg);

        // explicit, theme-aware separator line centered in the (wider) hit
        // area. it must stay visible even when the sidebar and canvas
        // backgrounds are identical (e.g. full-black theme), so we can't rely
        // on background color differences. hover/drag brighten the line.
        SidebarSeparatorState state = SidebarSeparatorState::Normal;
        if (GetCapture() == hwnd) {
            state = SidebarSeparatorState::Active;
        } else if (isMouseOver) {
            state = SidebarSeparatorState::Hover;
        }
        int lineSize = SplitterType::Vert == type ? 1 : DpiScale(hwnd, 1);
        // Geometry is independent of the dirty rectangle. BeginPaint already
        // clips drawing; using rcPaint here moves the line on partial paints.
        RECT rl{};
        GetClientRect(hwnd, &rl);
        if (SplitterType::Vert == type) {
            // The hit target stays wide, but its only visible boundary is one
            // physical pixel directly beside the TreeView/scrollbar. Centering
            // it creates a shadow-like gutter in themes whose colors differ.
            rl.right = rl.left + lineSize;
        } else {
            int y = rl.top + (((rl.bottom - rl.top) - lineSize) / 2);
            rl.top = y;
            rl.bottom = y + lineSize;
        }
        AutoDeleteBrush brLine = CreateSolidBrush(ThemeSidebarSeparatorColor(state));
        FillRect(hdc, &rl, brLine);
        EndPaint(hwnd, &ps);
        return 0;
    }

    return WndProcDefault(hwnd, msg, wparam, lparam);
}
