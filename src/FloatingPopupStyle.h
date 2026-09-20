/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct Rect;

constexpr int kFloatingPopupCornerRadius = 16;
constexpr int kFloatingPopupButtonRadius = 8;

COLORREF FloatingPopupBg();
COLORREF FloatingPopupBorderColor();
COLORREF FloatingPopupSeparatorColor();
COLORREF FloatingPopupTextColor();
COLORREF FloatingPopupMutedTextColor();
COLORREF FloatingPopupAccentColor();
COLORREF FloatingPopupHoverBg(COLORREF bg);
COLORREF FloatingPopupCloseHoverBg(COLORREF bg);
COLORREF BlendFloatingPopupColors(COLORREF from, COLORREF to, float t);

// Compact tool panels (display filter, etc.): four-theme hierarchy tokens.
// Prefer these over ad-hoc AccentColor deltas / hardcoded RGB in paint code.
COLORREF FloatingToolPanelBg();
COLORREF FloatingToolPanelBorder();
COLORREF FloatingToolPanelSeparator();
COLORREF FloatingToolSliderTrack();
COLORREF FloatingToolSliderTrackActive();
COLORREF FloatingToolSliderThumbFill(bool dragging, bool hovered = false);
COLORREF FloatingToolSliderThumbBorder(bool dragging, bool hovered = false);
COLORREF FloatingToolButtonBorder();
COLORREF FloatingToolButtonHoverBg();
COLORREF FloatingToolButtonPressedBg();

void FillFloatingPopupRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col);
void StrokeFloatingPopupRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col);
void UpdateFloatingPopupWindowRgn(HWND hwnd, int cornerRadius);
