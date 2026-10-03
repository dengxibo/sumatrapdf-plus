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
#include "CaptionGlyphs.h"
#include "SvgIcons.h"
#include "Theme.h"
#include "Toolbar.h"
#include "Translations.h"

#include "wingui/LabelWithCloseWnd.h"

#include "utils/Log.h"

#define kCloseBtnDx 16
#define kCloseBtnDy 16
#define kButtonSpaceDx 8
#define kHeaderActionDx 20
#define kHeaderActionDy 20
#define kHeaderActionGapDx 1
#define kHeaderCloseGapDx 8
#define kViewBtnDx 16
#define kViewBtnDy 16
#define kViewBtnIcon 16
#define kViewBtnGapDx 4

static void DrawHeaderAction(HDC hdc, const Rect& r, int kind, bool isHover, bool isPressed, COLORREF bgCol,
                             COLORREF iconCol) {
    if (r.dx <= 0 || r.dy <= 0) {
        return;
    }
    HWND hwnd = WindowFromDC(hdc);
    COLORREF iconBgCol = bgCol;
    if (isHover || isPressed) {
        iconBgCol = isPressed ? (ThemeUsesDarkChrome() ? AccentColor(bgCol, 0, 14) : AccentColor(bgCol, 10))
                              : (ThemeUsesDarkChrome() ? AccentColor(bgCol, 0, 8) : AccentColor(bgCol, 6));
        int radius = std::max(2, DpiScale(hwnd, 3));
        AutoDeleteBrush brush(CreateSolidBrush(iconBgCol));
        HRGN rgn = CreateRoundRectRgn(r.x, r.y, r.x + r.dx, r.y + r.dy, radius * 2, radius * 2);
        FillRgn(hdc, rgn, brush);
        DeleteObject(rgn);
    }

    int iconSz = std::min(DpiScale(hwnd, 16), std::min(r.dx, r.dy));
    Rect iconRc(r.x + (r.dx - iconSz) / 2, r.y + (r.dy - iconSz) / 2, iconSz, iconSz);
    TbIcon icon = TbIcon::ChevronDown;
    if (kind == 2) {
        icon = TbIcon::ChevronUp;
    } else if (kind == 3) {
        icon = TbIcon::CalibrateToc;
    }
    DrawSvgIcon(hdc, iconRc, icon, iconCol, iconBgCol);
}

static void DrawChromeIconButton(HDC hdc, HWND hwnd, const Rect& vr, TbIcon icon, bool enabled, bool hot, bool pressed,
                                 COLORREF bgCol, COLORREF iconCol) {
    if (vr.IsEmpty()) {
        return;
    }
    COLORREF iconBg = bgCol;
    if (pressed || hot) {
        // Same step as a checked toolbar button.
        if (pressed) {
            if (ThemeUsesBlackChrome()) {
                iconBg = AccentColor(bgCol, 20, 42);
            } else if (ThemeUsesDarkChrome()) {
                iconBg = AccentColor(bgCol, 8, 32);
            } else {
                iconBg = AccentColor(bgCol, 24);
            }
        } else if (ThemeUsesBlackChrome()) {
            iconBg = AccentColor(bgCol, 16, 34);
        } else if (ThemeUsesDarkChrome()) {
            iconBg = AccentColor(bgCol, 12, 28);
        } else {
            iconBg = AccentColor(bgCol, 10);
        }
        int pad = std::max(1, DpiScale(hwnd, 2));
        int radius = std::max(2, DpiScale(hwnd, 3) + pad);
        AutoDeleteBrush brush(CreateSolidBrush(iconBg));
        HRGN rgn =
            CreateRoundRectRgn(vr.x - pad, vr.y - pad, vr.x + vr.dx + pad, vr.y + vr.dy + pad, radius * 2, radius * 2);
        FillRgn(hdc, rgn, brush);
        if (pressed) {
            COLORREF edge = ThemeUsesBlackChrome()  ? AccentColor(bgCol, 20, 58)
                            : ThemeUsesDarkChrome() ? AccentColor(bgCol, 16, 48)
                                                    : AccentColor(bgCol, 42);
            AutoDeleteBrush edgeBr(CreateSolidBrush(edge));
            int edgePx = std::max(1, DpiScale(hwnd, 1));
            FrameRgn(hdc, rgn, edgeBr, edgePx, edgePx);
        }
        DeleteObject(rgn);
    }
    int iconSz = std::min(DpiScale(hwnd, kViewBtnIcon), std::min(vr.dx, vr.dy));
    Rect iconRc(vr.x + (vr.dx - iconSz) / 2, vr.y + (vr.dy - iconSz) / 2, iconSz, iconSz);
    COLORREF col = enabled ? iconCol : ThemeWindowTextDisabledColor();
    DrawSvgIcon(hdc, iconRc, icon, col, iconBg);
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
    if (w->nViewButtons == 0) {
        char* s = HwndGetTextTemp(w->hwnd);
        RECT rs{x, y, x + cr.dx, y + cr.dy};
        HdcDrawText(hdc, s, &rs, fmt);
    }

    // Text might be too long and invade header action area. We just re-paint
    // the background, which is not the pretties but works.
    // A better way would be to intelligently truncate text or shrink the font
    // size (within reason)
    bool isRtl = HwndIsRtl(w->hwnd);
    // TODO: make this work in rtl
    if (!isRtl) {
        x = w->firstActionPos.x;
        if (x == 0 && w->nRightButtons > 0) {
            x = w->rightBtnPos[0].x;
        }
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
    // Chevrons stay on the header ink. The close mark matches the window and
    // tab X: a 10px caption glyph in the same gray, not a 2px stroke.
    COLORREF iconCol = AccentColor(ThemeWindowTextColor(), ThemeUsesDarkChrome() ? 22 : 36);
    {
        HWND hwnd = w->hwnd;
        int iconPx = DpiScale(hwnd, 10);
        const Rect& r = w->closeBtnPos;
        Rect glyph(r.x + (r.dx - iconPx) / 2, r.y + (r.dy - iconPx) / 2, iconPx, iconPx);
        DrawCaptionSysButtonGlyph(hdc, CaptionSysButtonKind::Close, glyph, kColCloseX, iconPx);
    }

    DrawHeaderAction(hdc, w->firstActionPos, 1, w->firstActionPos.Contains(curPos), w->pressedAction == 1, w->bgColor,
                     iconCol);
    DrawHeaderAction(hdc, w->secondActionPos, 2, w->secondActionPos.Contains(curPos), w->pressedAction == 2, w->bgColor,
                     iconCol);
    DrawHeaderAction(hdc, w->thirdActionPos, 3, w->thirdActionPos.Contains(curPos), w->pressedAction == 3, w->bgColor,
                     iconCol);

    HWND hwnd = w->hwnd;
    for (int i = 0; i < w->nViewButtons; i++) {
        const Rect& vr = w->viewBtnPos[i];
        bool hot = vr.Contains(curPos) && w->viewBtns[i].enabled;
        bool pressed = w->viewBtns[i].selected || w->pressedAction == 11 + i;
        DrawChromeIconButton(hdc, hwnd, vr, w->viewBtns[i].icon, w->viewBtns[i].enabled, hot, pressed, w->bgColor,
                             iconCol);
    }
    for (int i = 0; i < w->nRightButtons; i++) {
        const Rect& vr = w->rightBtnPos[i];
        if (!w->rightBtns[i].chromeWell) {
            if (vr.dx <= 0 || vr.dy <= 0) {
                continue;
            }
            int iconSz = std::min(DpiScale(hwnd, 16), std::min(vr.dx, vr.dy));
            Rect iconRc(vr.x + (vr.dx - iconSz) / 2, vr.y + (vr.dy - iconSz) / 2, iconSz, iconSz);
            DrawSvgIcon(hdc, iconRc, w->rightBtns[i].icon, iconCol, w->bgColor);
            continue;
        }
        bool hot = vr.Contains(curPos) && w->rightBtns[i].enabled;
        bool pressed = w->rightBtns[i].selected || w->pressedAction == 21 + i;
        DrawChromeIconButton(hdc, hwnd, vr, w->rightBtns[i].icon, w->rightBtns[i].enabled, hot, pressed, w->bgColor,
                             iconCol);
    }

    if (w->font) {
        SelectObject(hdc, prevFont);
    }
}

LabelWithCloseWnd::~LabelWithCloseWnd() {
    delete actionsTooltip;
    DeleteObject(actionsTooltipFont);
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
        } else if (action == 3 && thirdActionPos.Contains(cursorPos)) {
            thirdAction.Call();
        } else if (action >= 11 && action <= 13) {
            int idx = action - 11;
            if (idx < nViewButtons && viewBtns[idx].enabled && viewBtnPos[idx].Contains(cursorPos)) {
                viewBtns[idx].onClick.Call();
            }
        } else if (action >= 21 && action <= 23) {
            int idx = action - 21;
            if (idx < nRightButtons && rightBtns[idx].enabled && rightBtnPos[idx].Contains(cursorPos)) {
                rightBtns[idx].onClick.Call();
            }
        }
        return 0;
    }

    if (WM_LBUTTONDOWN == msg) {
        for (int i = 0; i < nViewButtons; i++) {
            if (viewBtns[i].enabled && viewBtnPos[i].Contains(cursorPos)) {
                pressedAction = 11 + i;
                break;
            }
        }
        if (pressedAction == 0) {
            for (int i = 0; i < nRightButtons; i++) {
                if (rightBtns[i].enabled && rightBtnPos[i].Contains(cursorPos)) {
                    pressedAction = 21 + i;
                    break;
                }
            }
        }
        if (pressedAction == 0) {
            if (firstActionPos.Contains(cursorPos)) {
                pressedAction = 1;
            } else if (secondActionPos.Contains(cursorPos)) {
                pressedAction = 2;
            } else if (thirdActionPos.Contains(cursorPos)) {
                pressedAction = 3;
            }
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

static const char* HeaderActionTooltipTemp(const char* key) {
    return trans::GetTranslation(key);
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
        // Make an explicit non-underlined copy of the system tooltip font.
        // The sidebar title font may carry underline styles into this control.
        LOGFONTW lf{};
        GetObjectW(GetDefaultGuiFont(), sizeof(lf), &lf);
        lf.lfUnderline = FALSE;
        actionsTooltipFont = CreateFontIndirectW(&lf);
        args.font = actionsTooltipFont;
        args.isRtl = HwndIsRtl(hwnd);
        actionsTooltip = new Tooltip();
        actionsTooltip->Create(args);
        firstActionTooltipId = actionsTooltip->Add(HeaderActionTooltipTemp(firstActionTooltip), firstActionPos, false);
        secondActionTooltipId =
            actionsTooltip->Add(HeaderActionTooltipTemp(secondActionTooltip), secondActionPos, false);
    }
    Layout();
}

void LabelWithCloseWnd::SetThirdHeaderAction(const Func0& action, const char* tooltip) {
    thirdAction = action;
    thirdActionTooltip = tooltip;
    if (actionsTooltip && thirdActionTooltipId < 0 && tooltip) {
        thirdActionTooltipId = actionsTooltip->Add(HeaderActionTooltipTemp(tooltip), thirdActionPos, false);
    }
    Layout();
}

void LabelWithCloseWnd::ClearThirdHeaderAction() {
    thirdAction = {};
    thirdActionTooltip = nullptr;
    thirdActionPos = {};
    if (actionsTooltip && thirdActionTooltipId >= 0) {
        actionsTooltip->Update(thirdActionTooltipId, "", Rect(), false);
    }
    Layout();
}

void LabelWithCloseWnd::SetViewButtons(const LabelViewButton* buttons, int count) {
    if (count < 0) {
        count = 0;
    }
    if (count > 3) {
        count = 3;
    }
    nViewButtons = count;
    for (int i = 0; i < count; i++) {
        viewBtns[i] = buttons[i];
    }
    if (!actionsTooltip && count > 0) {
        Tooltip::CreateArgs args;
        args.parent = hwnd;
        LOGFONTW lf{};
        GetObjectW(GetDefaultGuiFont(), sizeof(lf), &lf);
        lf.lfUnderline = FALSE;
        actionsTooltipFont = CreateFontIndirectW(&lf);
        args.font = actionsTooltipFont;
        args.isRtl = HwndIsRtl(hwnd);
        actionsTooltip = new Tooltip();
        actionsTooltip->Create(args);
    }
    if (actionsTooltip) {
        for (int i = 0; i < count; i++) {
            const char* tip = viewBtns[i].tooltip ? trans::GetTranslation(viewBtns[i].tooltip) : "";
            if (viewBtnTooltipId[i] < 0) {
                viewBtnTooltipId[i] = actionsTooltip->Add(tip, viewBtnPos[i], false);
            } else {
                actionsTooltip->Update(viewBtnTooltipId[i], tip, viewBtnPos[i], false);
            }
        }
    }
    Layout();
}

void LabelWithCloseWnd::SetRightButtons(const LabelViewButton* buttons, int count) {
    if (count < 0) {
        count = 0;
    }
    if (count > 3) {
        count = 3;
    }
    nRightButtons = count;
    for (int i = 0; i < count; i++) {
        rightBtns[i] = buttons[i];
    }
    if (!actionsTooltip && count > 0) {
        Tooltip::CreateArgs args;
        args.parent = hwnd;
        LOGFONTW lf{};
        GetObjectW(GetDefaultGuiFont(), sizeof(lf), &lf);
        lf.lfUnderline = FALSE;
        actionsTooltipFont = CreateFontIndirectW(&lf);
        args.font = actionsTooltipFont;
        args.isRtl = HwndIsRtl(hwnd);
        actionsTooltip = new Tooltip();
        actionsTooltip->Create(args);
    }
    if (actionsTooltip) {
        for (int i = 0; i < 3; i++) {
            const char* tip = "";
            if (i < count && rightBtns[i].tooltip) {
                tip = trans::GetTranslation(rightBtns[i].tooltip);
            }
            if (rightBtnTooltipId[i] < 0) {
                if (i >= count) {
                    continue;
                }
                rightBtnTooltipId[i] = actionsTooltip->Add(tip, rightBtnPos[i], false);
            } else {
                actionsTooltip->Update(rightBtnTooltipId[i], tip, i < count ? rightBtnPos[i] : Rect(), false);
            }
        }
    }
    Layout();
}

void LabelWithCloseWnd::SetHeaderActionsVisible(bool visible) {
    if (headerActionsVisible == visible) {
        return;
    }
    headerActionsVisible = visible;
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
    thirdActionPos = {};
    int viewDx = DpiScale(hwnd, kViewBtnDx);
    int viewDy = DpiScale(hwnd, kViewBtnDy);
    int viewGap = DpiScale(hwnd, kViewBtnGapDx);
    int closeGapDx = DpiScale(hwnd, kHeaderCloseGapDx);
    for (int i = 0; i < 3; i++) {
        rightBtnPos[i] = {};
    }
    // Plain favorites buttons sit in the chevron slots: same size, gap, and
    // distance from the close mark as the expand and collapse arrows.
    bool plainRight = nRightButtons > 0 && !rightBtns[0].chromeWell;
    int rbDx = plainRight ? DpiScale(hwnd, kHeaderActionDx) : viewDx;
    int rbDy = plainRight ? DpiScale(hwnd, kHeaderActionDy) : viewDy;
    int rbGap = plainRight ? DpiScale(hwnd, kHeaderActionGapDx) : viewGap;
    int rightY = 0;
    if (dy > rbDy) {
        rightY = (dy - rbDy) / 2;
    }
    // Left edge (LTR) or right edge (RTL) that the chevrons sit against.
    int anchorX = closeBtnPos.x;
    int anchorRight = closeBtnPos.x + closeBtnPos.dx;
    if (nRightButtons > 0) {
        if (isRtl) {
            int bx = closeBtnPos.x + closeBtnPos.dx + closeGapDx;
            for (int i = 0; i < nRightButtons; i++) {
                rightBtnPos[i] = Rect(bx, rightY, rbDx, rbDy);
                bx += rbDx + rbGap;
            }
            anchorRight = rightBtnPos[nRightButtons - 1].x + rbDx;
        } else {
            int bx = closeBtnPos.x - closeGapDx - rbDx;
            for (int i = nRightButtons - 1; i >= 0; i--) {
                rightBtnPos[i] = Rect(bx, rightY, rbDx, rbDy);
                bx -= rbDx + rbGap;
            }
            anchorX = rightBtnPos[0].x;
        }
    }
    if (headerActionsVisible && firstAction.IsValid() && secondAction.IsValid()) {
        int actionDx = DpiScale(hwnd, kHeaderActionDx);
        int actionDy = DpiScale(hwnd, kHeaderActionDy);
        int gapDx = DpiScale(hwnd, kHeaderActionGapDx);
        int actionY = (dy - actionDy) / 2;
        if (isRtl) {
            if (thirdAction.IsValid()) {
                thirdActionPos = Rect(anchorRight + closeGapDx, actionY, actionDx, actionDy);
                secondActionPos = Rect(thirdActionPos.x + actionDx + gapDx, actionY, actionDx, actionDy);
            } else {
                secondActionPos = Rect(anchorRight + closeGapDx, actionY, actionDx, actionDy);
            }
            firstActionPos = Rect(secondActionPos.x + actionDx + gapDx, actionY, actionDx, actionDy);
        } else {
            if (thirdAction.IsValid()) {
                thirdActionPos = Rect(anchorX - closeGapDx - actionDx, actionY, actionDx, actionDy);
                secondActionPos = Rect(thirdActionPos.x - gapDx - actionDx, actionY, actionDx, actionDy);
            } else {
                secondActionPos = Rect(anchorX - closeGapDx - actionDx, actionY, actionDx, actionDy);
            }
            firstActionPos = Rect(secondActionPos.x - gapDx - actionDx, actionY, actionDx, actionDy);
        }
        if (actionsTooltip) {
            actionsTooltip->Update(firstActionTooltipId, HeaderActionTooltipTemp(firstActionTooltip), firstActionPos,
                                   false);
            actionsTooltip->Update(secondActionTooltipId, HeaderActionTooltipTemp(secondActionTooltip), secondActionPos,
                                   false);
            if (thirdActionTooltipId >= 0) {
                actionsTooltip->Update(thirdActionTooltipId,
                                       thirdAction.IsValid() ? HeaderActionTooltipTemp(thirdActionTooltip) : "",
                                       thirdActionPos, false);
            }
        }
    } else if (actionsTooltip) {
        if (firstActionTooltipId >= 0) {
            actionsTooltip->Update(firstActionTooltipId, "", Rect(), false);
        }
        if (secondActionTooltipId >= 0) {
            actionsTooltip->Update(secondActionTooltipId, "", Rect(), false);
        }
        if (thirdActionTooltipId >= 0) {
            actionsTooltip->Update(thirdActionTooltipId, "", Rect(), false);
        }
    }
    int left = isRtl ? closeBtnPos.x + closeBtnPos.dx + DpiScale(hwnd, kHeaderActionGapDx) : padXScaled;
    int viewY = (dy - viewDy) / 2;
    for (int i = 0; i < nViewButtons; i++) {
        viewBtnPos[i] = Rect(left, viewY, viewDx, viewDy);
        left += viewDx + viewGap;
    }
    for (int i = nViewButtons; i < 3; i++) {
        viewBtnPos[i] = {};
    }
    if (actionsTooltip) {
        for (int i = 0; i < nViewButtons; i++) {
            if (viewBtnTooltipId[i] >= 0) {
                actionsTooltip->Update(viewBtnTooltipId[i],
                                       viewBtns[i].tooltip ? trans::GetTranslation(viewBtns[i].tooltip) : "",
                                       viewBtnPos[i], false);
            }
        }
        for (int i = 0; i < 3; i++) {
            if (rightBtnTooltipId[i] < 0) {
                continue;
            }
            const char* tip = "";
            Rect rc;
            if (i < nRightButtons && rightBtns[i].tooltip) {
                tip = trans::GetTranslation(rightBtns[i].tooltip);
                rc = rightBtnPos[i];
            }
            actionsTooltip->Update(rightBtnTooltipId[i], tip, rc, false);
        }
    }
    // logf("closeBtnPos: (%d,%d) size: (%d, %d)\n", x, y, btnDx, btnDy);
    HwndScheduleRepaint(hwnd);
}

void LabelWithCloseWnd::UpdateActionsTooltipTheme() {
    if (actionsTooltip) {
        actionsTooltip->UpdateTheme();
    }
}

void LabelWithCloseWnd::UpdateHeaderActionTooltips() {
    if (!actionsTooltip) {
        return;
    }
    if (firstActionTooltipId >= 0) {
        actionsTooltip->Update(firstActionTooltipId, HeaderActionTooltipTemp(firstActionTooltip), firstActionPos,
                               false);
    }
    if (secondActionTooltipId >= 0) {
        actionsTooltip->Update(secondActionTooltipId, HeaderActionTooltipTemp(secondActionTooltip), secondActionPos,
                               false);
    }
    if (thirdActionTooltipId >= 0 && thirdAction.IsValid()) {
        actionsTooltip->Update(thirdActionTooltipId, HeaderActionTooltipTemp(thirdActionTooltip), thirdActionPos,
                               false);
    }
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
        int n = thirdAction.IsValid() ? 3 : 2;
        size.dx += n * DpiScale(this->hwnd, kHeaderActionDx);
        size.dx += (n - 1) * DpiScale(this->hwnd, kHeaderActionGapDx);
        size.dx += DpiScale(this->hwnd, kHeaderCloseGapDx);
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
