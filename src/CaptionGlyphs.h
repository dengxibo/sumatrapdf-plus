/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

struct Rect;

enum class CaptionSysButtonKind {
    Minimize,
    Maximize,
    Restore,
    Close,
};

void DrawCaptionSysButtonGlyph(HDC hdc, CaptionSysButtonKind kind, Rect rc, COLORREF iconCol, int iconPx);
