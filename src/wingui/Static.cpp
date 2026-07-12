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

//- Static

// https://docs.microsoft.com/en-us/windows/win32/controls/static-controls

Kind kindStatic = "static";

Static::Static() {
    kind = kindStatic;
}

HWND Static::Create(const CreateArgs& args) {
    wordWrap = args.wordWrap;
    CreateControlArgs cargs;
    cargs.className = WC_STATICW;
    cargs.parent = args.parent;
    cargs.font = args.font;
    cargs.style = WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_LEFT;
    if (wordWrap) {
        // Match DrawText word-wrap metrics used in HwndMeasureTextWrapped().
        cargs.style |= SS_EDITCONTROL;
    }
    cargs.text = args.text;
    cargs.isRtl = args.isRtl;

    Wnd::CreateControl(cargs);
    SizeToIdealSize(this);

    return hwnd;
}

static int StaticMeasureWidth(HWND hwnd, const Constraints& bc, int hinset, bool wordWrap, int layoutWidthHint,
                              int lastBoundsDx) {
    int width = bc.max.dx;
    if (width == Inf || width <= 0) {
        width = bc.min.dx > 0 ? bc.min.dx : 0;
    }
    if (width <= 0 && layoutWidthHint > 0) {
        width = layoutWidthHint - hinset;
    }
    if (width <= 0 && lastBoundsDx > hinset) {
        width = lastBoundsDx - hinset;
    }
    if (width > 0 && wordWrap) {
        // Static controls wrap slightly inside the client rect; measure conservatively.
        width = std::max(1, width - DpiScale(hwnd, 4));
    }
    return width;
}

static Size StaticMeasureText(HWND hwnd, int maxDx, bool wordWrap) {
    char* txt = HwndGetTextTemp(hwnd);
    HFONT hfont = HwndGetFont(hwnd);
    if (wordWrap && maxDx > 0) {
        return HwndMeasureTextWrapped(hwnd, txt, hfont, maxDx);
    }
    return HwndMeasureText(hwnd, txt, hfont);
}

Size Static::GetIdealSize() {
    ReportIf(!hwnd);
    return StaticMeasureText(hwnd, 0, wordWrap);
}

int Static::MinIntrinsicHeight(int width) {
    if (wordWrap && width > 0) {
        width = std::max(1, width - DpiScale(hwnd, 4));
    }
    return StaticMeasureText(hwnd, width, wordWrap).dy;
}

int Static::MinIntrinsicWidth(int) {
    return StaticMeasureText(hwnd, 0, wordWrap).dx;
}

Size Static::Layout(const Constraints bc) {
    auto hinset = insets.left + insets.right;
    auto vinset = insets.top + insets.bottom;
    auto innerConstraints = bc.Inset(hinset, vinset);

    int width = StaticMeasureWidth(hwnd, innerConstraints, hinset, wordWrap, layoutWidthHint, lastBounds.dx);
    Size s = StaticMeasureText(hwnd, width, wordWrap);
    childSize = innerConstraints.Constrain(s);
    return Size{
        childSize.dx + hinset,
        childSize.dy + vinset,
    };
}

bool Static::OnCommand(WPARAM wparam, LPARAM lparam) {
    auto code = HIWORD(wparam);
    if (code == STN_CLICKED && onClick.IsValid()) {
        onClick.Call();
        return true;
    }
    return false;
}

LRESULT Static::OnMessageReflect(UINT msg, WPARAM wp, LPARAM lparam) {
    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)wp;
        if (!IsSpecialColor(textColor)) {
            SetTextColor(hdc, textColor);
        }
        if (!IsSpecialColor(bgColor)) {
            SetBkColor(hdc, bgColor);
        }
        auto br = BackgroundBrush();
        return (LRESULT)br;
    }
    return 0;
}
