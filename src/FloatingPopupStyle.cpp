/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "Theme.h"
#include "FloatingPopupStyle.h"

static Gdiplus::Color GdipColor(COLORREF col, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(col), GetGValue(col), GetBValue(col));
}

COLORREF FloatingPopupBg() {
    if (ThemeUsesDarkChrome()) {
        return ThemeWindowBackgroundColor();
    }
    COLORREF contentBg;
    ThemePageRenderColors(contentBg, true);
    return AccentColor(contentBg, 12);
}

COLORREF FloatingPopupBorderColor() {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 35);
    }
    return AccentColor(FloatingPopupBg(), 8);
}

COLORREF FloatingPopupSeparatorColor() {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 20);
    }
    return AccentColor(FloatingPopupBg(), 5);
}

COLORREF FloatingPopupTextColor() {
    if (ThemeUsesDarkChrome()) {
        COLORREF col = ThemeReadingTextColor();
        int dim = ThemeUsesBlackChrome() ? -22 : -14;
        return AdjustLightness2(col, dim);
    }
    return RGB(27, 29, 33);
}

COLORREF FloatingPopupMutedTextColor() {
    if (ThemeUsesDarkChrome()) {
        return ThemeReadingTextDisabledColor();
    }
    return RGB(92, 96, 104);
}

COLORREF FloatingPopupAccentColor() {
    return ThemeUsesDarkChrome() ? ThemeWindowLinkColor() : RGB(38, 108, 184);
}

COLORREF FloatingPopupHoverBg(COLORREF bg) {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 15);
    }
    return AccentColor(bg, 10);
}

COLORREF FloatingPopupCloseHoverBg(COLORREF bg) {
    if (ThemeUsesDarkChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 18);
    }
    return AccentColor(bg, 15);
}

COLORREF BlendFloatingPopupColors(COLORREF from, COLORREF to, float t) {
    auto blend = [&](int a, int b) { return (int)(a * (1.f - t) + b * t); };
    return RGB(blend(GetRValue(from), GetRValue(to)), blend(GetGValue(from), GetGValue(to)),
               blend(GetBValue(from), GetBValue(to)));
}

void FillFloatingPopupRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(GdipColor(col));
    Gdiplus::GraphicsPath path;
    int d = radius;
    path.AddArc(rc.x, rc.y, d, d, 180, 90);
    path.AddArc(rc.x + rc.dx - d - 1, rc.y, d, d, 270, 90);
    path.AddArc(rc.x + rc.dx - d - 1, rc.y + rc.dy - d - 1, d, d, 0, 90);
    path.AddArc(rc.x, rc.y + rc.dy - d - 1, d, d, 90, 90);
    path.CloseFigure();
    g.FillPath(&br, &path);
}

void StrokeFloatingPopupRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(GdipColor(col), 1);
    Gdiplus::GraphicsPath path;
    int d = radius;
    path.AddArc(rc.x, rc.y, d, d, 180, 90);
    path.AddArc(rc.x + rc.dx - d - 1, rc.y, d, d, 270, 90);
    path.AddArc(rc.x + rc.dx - d - 1, rc.y + rc.dy - d - 1, d, d, 0, 90);
    path.AddArc(rc.x, rc.y + rc.dy - d - 1, d, d, 90, 90);
    path.CloseFigure();
    g.DrawPath(&pen, &path);
}

void UpdateFloatingPopupWindowRgn(HWND hwnd, int cornerRadius) {
    if (!hwnd) {
        return;
    }
    Rect card = ClientRect(hwnd);
    if (card.dx < 1) {
        card.dx = 1;
    }
    if (card.dy < 1) {
        card.dy = 1;
    }
    int radius = DpiScale(hwnd, cornerRadius);
    HRGN rgn = CreateRoundRectRgn(card.x, card.y, card.x + card.dx + 1, card.y + card.dy + 1, radius, radius);
    if (!SetWindowRgn(hwnd, rgn, TRUE)) {
        DeleteObject(rgn);
    }
}
