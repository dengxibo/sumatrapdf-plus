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

void FillFloatingPopupRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col);
void StrokeFloatingPopupRoundedRect(HDC hdc, const Rect& rc, int radius, COLORREF col);
void UpdateFloatingPopupWindowRgn(HWND hwnd, int cornerRadius);
