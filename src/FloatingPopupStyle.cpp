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
    return ThemeWindowTextColor();
}

COLORREF FloatingPopupMutedTextColor() {
    if (ThemeUsesDarkChrome()) {
        return ThemeReadingTextDisabledColor();
    }
    return AccentColor(ThemeWindowTextColor(), -35);
}

COLORREF FloatingPopupAccentColor() {
    return ThemeWindowLinkColor();
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

// --- tool-panel tokens (Warm / White / Dracula / Black) ---
// AccentColor(base, n): on light colors, positive n darkens; on dark colors, positive n lightens.

COLORREF FloatingToolPanelBg() {
    if (ThemeUsesBlackChrome()) {
        // Slight elevation off pure black document chrome (#050505 → ~#121212).
        return AccentColor(ThemeWindowControlBackgroundColor(), 18);
    }
    if (ThemeUsesDarkChrome()) {
        // Dracula: control bg (#21222C), not the larger purple window (#282A36).
        return ThemeWindowControlBackgroundColor();
    }
    if (ThemeUsesOriginalPageColors()) {
        // White: soft gray panel (#f5f5f5), not pure white so tracks/thumbs can separate.
        return ThemeWindowBackgroundColor();
    }
    // Warm: keep cream control chrome.
    return ThemeWindowControlBackgroundColor();
}

COLORREF FloatingToolPanelBorder() {
    COLORREF bg = FloatingToolPanelBg();
    if (ThemeUsesBlackChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 72);
    }
    if (ThemeUsesDarkChrome()) {
        // Neutralize Dracula purple for chrome edges.
        COLORREF n = BlendFloatingPopupColors(bg, RGB(GetRValue(bg), GetRValue(bg), GetRValue(bg)), 0.45f);
        return AccentColor(n, 55);
    }
    if (ThemeUsesOriginalPageColors()) {
        return AccentColor(bg, 42);
    }
    return AccentColor(bg, 32);
}

COLORREF FloatingToolPanelSeparator() {
    COLORREF bg = FloatingToolPanelBg();
    if (ThemeUsesBlackChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 52);
    }
    if (ThemeUsesDarkChrome()) {
        COLORREF n = BlendFloatingPopupColors(bg, RGB(GetRValue(bg), GetRValue(bg), GetRValue(bg)), 0.45f);
        return AccentColor(n, 42);
    }
    if (ThemeUsesOriginalPageColors()) {
        return AccentColor(bg, 32);
    }
    return AccentColor(bg, 22);
}

COLORREF FloatingToolSliderTrack() {
    COLORREF bg = FloatingToolPanelBg();
    if (ThemeUsesBlackChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 68);
    }
    if (ThemeUsesDarkChrome()) {
        COLORREF n = BlendFloatingPopupColors(bg, RGB(GetRValue(bg), GetRValue(bg), GetRValue(bg)), 0.50f);
        return AccentColor(n, 52);
    }
    if (ThemeUsesOriginalPageColors()) {
        // ~#C9CDD2: clearly darker than #f5f5f5 panel.
        return AccentColor(bg, 58);
    }
    return AccentColor(bg, 36);
}

COLORREF FloatingToolSliderTrackActive() {
    COLORREF idle = FloatingToolSliderTrack();
    if (ThemeUsesBlackChrome()) {
        return AccentColor(idle, 48);
    }
    if (ThemeUsesDarkChrome()) {
        return AccentColor(idle, 36);
    }
    if (ThemeUsesOriginalPageColors()) {
        return AccentColor(idle, 30);
    }
    return AccentColor(idle, 24);
}

COLORREF FloatingToolSliderThumbFill(bool dragging, bool hovered) {
    COLORREF bg = FloatingToolPanelBg();
    if (ThemeUsesBlackChrome()) {
        if (dragging) {
            return AccentColor(ThemeWindowControlBackgroundColor(), 155);
        }
        if (hovered) {
            return AccentColor(ThemeWindowControlBackgroundColor(), 135);
        }
        return AccentColor(ThemeWindowControlBackgroundColor(), 120);
    }
    if (ThemeUsesDarkChrome()) {
        COLORREF n = BlendFloatingPopupColors(bg, RGB(GetRValue(bg), GetRValue(bg), GetRValue(bg)), 0.55f);
        if (dragging) {
            return AccentColor(n, 125);
        }
        if (hovered) {
            return AccentColor(n, 110);
        }
        return AccentColor(n, 98);
    }
    if (ThemeUsesOriginalPageColors()) {
        if (dragging) {
            return AccentColor(ThemeWindowControlBackgroundColor(), 4);
        }
        return ThemeWindowControlBackgroundColor();
    }
    if (dragging) {
        return BlendFloatingPopupColors(bg, RGB(0xFF, 0xFF, 0xFF), 0.70f);
    }
    return BlendFloatingPopupColors(bg, RGB(0xFF, 0xFF, 0xFF), 0.55f);
}

COLORREF FloatingToolSliderThumbBorder(bool dragging, bool hovered) {
    if (ThemeUsesBlackChrome()) {
        if (dragging || hovered) {
            return AccentColor(ThemeWindowControlBackgroundColor(), 110);
        }
        return AccentColor(ThemeWindowControlBackgroundColor(), 92);
    }
    if (ThemeUsesDarkChrome()) {
        COLORREF n = BlendFloatingPopupColors(
            FloatingToolPanelBg(),
            RGB(GetRValue(FloatingToolPanelBg()), GetRValue(FloatingToolPanelBg()), GetRValue(FloatingToolPanelBg())),
            0.55f);
        if (dragging || hovered) {
            return AccentColor(n, 85);
        }
        return AccentColor(n, 70);
    }
    if (ThemeUsesOriginalPageColors()) {
        if (dragging || hovered) {
            return AccentColor(FloatingToolPanelBg(), 82);
        }
        return AccentColor(FloatingToolPanelBg(), 72);
    }
    if (dragging || hovered) {
        return AccentColor(FloatingToolPanelBg(), 58);
    }
    return AccentColor(FloatingToolPanelBg(), 48);
}

COLORREF FloatingToolButtonBorder() {
    return FloatingToolPanelBorder();
}

COLORREF FloatingToolButtonHoverBg() {
    return FloatingPopupHoverBg(FloatingToolPanelBg());
}

COLORREF FloatingToolButtonPressedBg() {
    COLORREF bg = FloatingToolPanelBg();
    if (ThemeUsesBlackChrome()) {
        return AccentColor(ThemeWindowControlBackgroundColor(), 28);
    }
    if (ThemeUsesDarkChrome()) {
        return AccentColor(bg, 26);
    }
    if (ThemeUsesOriginalPageColors()) {
        return AccentColor(bg, 12);
    }
    return AccentColor(bg, 10);
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
