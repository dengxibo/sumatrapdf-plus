/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/GdiPlusUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "AppTools.h"

#include "Theme.h"

#include "wingui/LabelWithCloseWnd.h"

#include "utils/Log.h"

#define kCloseBtnDx 16
#define kCloseBtnDy 16
#define kButtonSpaceDx 8
#define kHeaderActionDx 24
#define kHeaderActionDy 24
#define kHeaderActionGapDx 3

static void DrawHeaderAction(HDC hdc, const Rect& r, bool isExpand, bool isHover, bool isPressed, COLORREF bgCol,
                             COLORREF iconCol) {
    if (r.dx <= 0 || r.dy <= 0) {
        return;
    }
    HWND hwnd = WindowFromDC(hdc);
    if (isHover || isPressed) {
        COLORREF fillCol = isPressed ? (ThemeUsesDarkChrome() ? AccentColor(bgCol, 0, 14) : AccentColor(bgCol, 10))
                                     : (ThemeUsesDarkChrome() ? AccentColor(bgCol, 0, 8) : AccentColor(bgCol, 6));
        int radius = std::max(2, DpiScale(hwnd, 3));
        AutoDeleteBrush brush(CreateSolidBrush(fillCol));
        HRGN rgn = CreateRoundRectRgn(r.x, r.y, r.x + r.dx, r.y + r.dy, radius * 2, radius * 2);
        FillRgn(hdc, rgn, brush);
        DeleteObject(rgn);
    }

    int inset = DpiScale(hwnd, 6);
    // A 12x5 logical-pixel chevron is a softer midpoint between the original
    // narrow glyph and a 90-degree toolbar-style chevron.
    int step = DpiScale(hwnd, 5);
    int centerY = r.y + r.dy / 2;
    AutoDeletePen pen(CreatePen(PS_SOLID, std::max(1, DpiScale(hwnd, 1)), iconCol));
    ScopedSelectPen selectPen(hdc, pen);
    int left = r.x + inset;
    int right = r.x + r.dx - inset;
    int middle = (left + right) / 2;
    int direction = isExpand ? 1 : -1;
    MoveToEx(hdc, left, centerY - direction * step / 2, nullptr);
    LineTo(hdc, middle, centerY + direction * step / 2);
    LineTo(hdc, right, centerY - direction * step / 2);
}

static void PaintHDC(LabelWithCloseWnd* w, HDC hdc, const PAINTSTRUCT& ps) {
    HBRUSH br = w->BackgroundBrush();
    FillRect(hdc, &ps.rcPaint, br);

    Rect cr = ClientRect(w->hwnd);

    int x = DpiScale(w->hwnd, w->padX);
    int y = DpiScale(w->hwnd, w->padY);

    HGDIOBJ prevFont = nullptr;
    if (w->font) {
        prevFont = SelectObject(hdc, w->font);
    }
    if (!IsSpecialColor(w->textColor)) {
        SetTextColor(hdc, w->textColor);
    }
    if (!IsSpecialColor(w->bgColor)) {
        SetBkColor(hdc, w->bgColor);
    }

    uint fmt = DT_SINGLELINE | DT_TOP | DT_LEFT;
    if (HwndIsRtl(w->hwnd)) {
        fmt |= DT_RTLREADING;
    }
    char* s = HwndGetTextTemp(w->hwnd);
    RECT rs{x, y, x + cr.dx, y + cr.dy};
    HdcDrawText(hdc, s, &rs, fmt);

    // Text might be too long and invade header action area. We just re-paint
    // the background, which is not the pretties but works.
    // A better way would be to intelligently truncate text or shrink the font
    // size (within reason)
    bool isRtl = HwndIsRtl(w->hwnd);
    // TODO: make this work in rtl
    if (!isRtl) {
        x = w->firstActionPos.x;
        if (x == 0) {
            x = w->closeBtnPos.x - DpiScale(w->hwnd, kButtonSpaceDx);
        }
        Rect ri(x, 0, cr.dx - x, cr.dy);
        RECT r = ToRECT(ri);
        FillRect(hdc, &r, br);
    }
    Point curPos = HwndGetCursorPos(w->hwnd);
    // TODO: hack
    UnmirrorRtl(w->hwnd, curPos);
    DrawCloseButtonArgs args;
    args.hdc = hdc;
    args.r = w->closeBtnPos;
    args.isHover = w->closeBtnPos.Contains(curPos);
    // args.noMirror = true;
    DrawCloseButton(args);

    COLORREF iconCol = AccentColor(w->textColor, 70);
    DrawHeaderAction(hdc, w->firstActionPos, true, w->firstActionPos.Contains(curPos), w->pressedAction == 1,
                     w->bgColor, iconCol);
    DrawHeaderAction(hdc, w->secondActionPos, false, w->secondActionPos.Contains(curPos), w->pressedAction == 2,
                     w->bgColor, iconCol);

    if (w->font) {
        SelectObject(hdc, prevFont);
    }
}

LabelWithCloseWnd::~LabelWithCloseWnd() {
    delete actionsTooltip;
}

void LabelWithCloseWnd::OnPaint(HDC hdc, PAINTSTRUCT* ps) {
    DoubleBuffer buffer(hwnd, ToRect(ps->rcPaint));
    PaintHDC(this, buffer.GetDC(), *ps);
    buffer.Flush(hdc);
}

LRESULT LabelWithCloseWnd::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_ERASEBKGND == msg) {
        return TRUE; // tells Windows we handle background erasing so it doesn't do it
    }

#if 0
    // to match other controls, preferred way is explict SetFont() call
    if (WM_SETFONT == msg) {
        SetFont((HFONT)wp);
        return 0;
    }

    if (WM_GETFONT == msg) {
        return (LRESULT)font;
    }
#endif

    if (WM_SIZE == msg) {
        int dx = LOWORD(lp);
        int dy = HIWORD(lp);
        Layout();
        return 0;
    }

    Point cursorPos = HwndGetCursorPos(hwnd);
    // TODO: this is a hack
    // HwhndGetCursorPos() does rtl mirroring but we calculate position
    // in absolute coords. Need to be more principled here
    UnmirrorRtl(hwnd, cursorPos);
    Rect br = closeBtnPos;
    if (WM_MOUSEMOVE == msg) {
        // logf("WM_MOUSEMOVE\n");
        // logf("closeBtnPos: (%d,%d) size: (%d, %d)\n", br.x, br.y, br.dx, br.dy);
        // logf("cursorPos: (%d, %d)\n", cursorPos.x, cursorPos.y);
        HwndScheduleRepaint(hwnd);

        if (closeBtnPos.Contains(cursorPos)) {
            TrackMouseLeave(hwnd);
        }
        goto DoDefault;
    }

    if (WM_MOUSELEAVE == msg) {
        // logf("WM_MOUSELEAVE\n");
        // logf("closeBtnPos: (%d,%d) size: (%d, %d)\n", br.x, br.y, br.dx, br.dy);
        // logf("cursorPos: (%d, %d)\n", cursorPos.x, cursorPos.y);
        HwndScheduleRepaint(hwnd);
        return 0;
    }

    if (WM_LBUTTONUP == msg) {
        // logf("WM_LBUTTONUP\n");
        // logf("closeBtnPos: (%d,%d) size: (%d, %d)\n", br.x, br.y, br.dx, br.dy);
        // logf("cursorPos: (%d, %d)\n", cursorPos.x, cursorPos.y);
        if (closeBtnPos.Contains(cursorPos)) {
            HWND parent = GetParent(hwnd);
            HwndSendCommand(parent, cmdId);
        }
        int action = pressedAction;
        pressedAction = 0;
        if (action != 0 && GetCapture() == hwnd) {
            ReleaseCapture();
        }
        HwndScheduleRepaint(hwnd);
        if (action == 1 && firstActionPos.Contains(cursorPos)) {
            firstAction.Call();
        } else if (action == 2 && secondActionPos.Contains(cursorPos)) {
            secondAction.Call();
        }
        return 0;
    }

    if (WM_LBUTTONDOWN == msg) {
        if (firstActionPos.Contains(cursorPos)) {
            pressedAction = 1;
        } else if (secondActionPos.Contains(cursorPos)) {
            pressedAction = 2;
        }
        if (pressedAction != 0) {
            SetCapture(hwnd);
            HwndScheduleRepaint(hwnd);
            return 0;
        }
    }

    if (WM_CAPTURECHANGED == msg && pressedAction != 0) {
        pressedAction = 0;
        HwndScheduleRepaint(hwnd);
    }

DoDefault:
    return WndProcDefault(hwnd, msg, wp, lp);
}

void LabelWithCloseWnd::SetHeaderActions(const Func0& first, const char* firstTooltip, const Func0& second,
                                         const char* secondTooltip) {
    firstAction = first;
    secondAction = second;
    firstActionTooltip = firstTooltip;
    secondActionTooltip = secondTooltip;
    if (!actionsTooltip) {
        Tooltip::CreateArgs args;
        args.parent = hwnd;
        args.font = font;
        args.isRtl = HwndIsRtl(hwnd);
        actionsTooltip = new Tooltip();
        actionsTooltip->Create(args);
        firstActionTooltipId = actionsTooltip->Add(firstActionTooltip, firstActionPos, false);
        secondActionTooltipId = actionsTooltip->Add(secondActionTooltip, secondActionPos, false);
    }
    Layout();
}

void LabelWithCloseWnd::SetLabel(const char* label) {
    HwndSetText(this->hwnd, label);
    this->Layout();
    HwndScheduleRepaint(this->hwnd);
}

void LabelWithCloseWnd::Layout() {
    Rect r = ClientRect(hwnd);
    int dx = r.dx;
    int dy = r.dy;

    int btnDx = DpiScale(hwnd, kCloseBtnDx);
    int btnDy = DpiScale(hwnd, kCloseBtnDy);
    int padXScaled = DpiScale(hwnd, padX);
    auto isRtl = HwndIsRtl(hwnd);
    int x = isRtl ? padX : dx - btnDx - padXScaled;
    int y = 0;
    if (dy > btnDy) {
        y = (dy - btnDy) / 2;
    }
    closeBtnPos = Rect(x, y, btnDx, btnDy);
    firstActionPos = {};
    secondActionPos = {};
    if (firstAction.IsValid() && secondAction.IsValid()) {
        int actionDx = DpiScale(hwnd, kHeaderActionDx);
        int actionDy = DpiScale(hwnd, kHeaderActionDy);
        int gapDx = DpiScale(hwnd, kHeaderActionGapDx);
        int actionY = (dy - actionDy) / 2;
        if (isRtl) {
            firstActionPos = Rect(closeBtnPos.x + closeBtnPos.dx + gapDx, actionY, actionDx, actionDy);
            secondActionPos = Rect(firstActionPos.x + actionDx + gapDx, actionY, actionDx, actionDy);
        } else {
            secondActionPos = Rect(closeBtnPos.x - gapDx - actionDx, actionY, actionDx, actionDy);
            firstActionPos = Rect(secondActionPos.x - gapDx - actionDx, actionY, actionDx, actionDy);
        }
        if (actionsTooltip) {
            actionsTooltip->Update(firstActionTooltipId, firstActionTooltip, firstActionPos, false);
            actionsTooltip->Update(secondActionTooltipId, secondActionTooltip, secondActionPos, false);
        }
    }
    // logf("closeBtnPos: (%d,%d) size: (%d, %d)\n", x, y, btnDx, btnDy);
    HwndScheduleRepaint(hwnd);
}

// cmd is both the id of the window as well as id of WM_COMMAND sent
// when close button is clicked
// caller needs to free() the result
HWND LabelWithCloseWnd::Create(const LabelWithCloseWnd::CreateArgs& args) {
    CreateCustomArgs cargs;
    cargs.parent = args.parent;
    cargs.font = args.font;
    cargs.pos = Rect(0, 0, 0, 0);
    cargs.style = WS_VISIBLE;
    cargs.cmdId = cmdId; // TODO: not sure if needed
    cargs.isRtl = args.isRtl;
    cmdId = args.cmdId;

    CreateCustom(cargs);

#if 0
    auto bgCol = GetSysColor(COLOR_BTNFACE);
    auto txtCol = GetSysColor(COLOR_BTNTEXT);
    SetColors(txtCol, bgCol);
#endif
    return hwnd;
}

Size LabelWithCloseWnd::GetIdealSize() {
    char* s = HwndGetTextTemp(this->hwnd);
    Size size = HwndMeasureText(this->hwnd, s);
    int btnDx = DpiScale(this->hwnd, kCloseBtnDx);
    int btnDy = DpiScale(this->hwnd, kCloseBtnDy);
    size.dx += btnDx;
    if (firstAction.IsValid() && secondAction.IsValid()) {
        size.dx += 2 * DpiScale(this->hwnd, kHeaderActionDx) + 2 * DpiScale(this->hwnd, kHeaderActionGapDx);
    }
    size.dx += DpiScale(this->hwnd, kButtonSpaceDx);
    size.dx += 2 * DpiScale(this->hwnd, this->padX);
    if (firstAction.IsValid() && secondAction.IsValid()) {
        size.dy = std::max(size.dy, DpiScale(this->hwnd, kHeaderActionDy));
    }
    if (size.dy < btnDy) {
        size.dy = btnDy;
    }
    size.dy += 2 * DpiScale(this->hwnd, this->padY);
    return size;
}

void LabelWithCloseWnd::SetFont(HFONT f) {
    this->font = f;
    // TODO: if created, set on the label?
}

void LabelWithCloseWnd::SetPaddingXY(int x, int y) {
    this->padX = x;
    this->padY = y;
    HwndScheduleRepaint(this->hwnd);
}
