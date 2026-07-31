/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"

#include "Theme.h"

class EngineBase;

// Stub implementations for binaries that compile EngineMupdf.cpp / EngineEbook.cpp
// but not SumatraPDF.cpp (PdfFilter, PdfPreview, etc.).

void NotifyEbookPagesLoadingProgress(const char* filePath, bool reloadToc) {
    (void)filePath;
    (void)reloadToc;
}

void NotifyPdfFollowThemeProbeComplete(const char* filePath) {
    (void)filePath;
}

void NotifyEngineDisplayReady(EngineBase* engine) {
    (void)engine;
}

COLORREF ThemePageRenderColors(COLORREF& bg, bool respectPdfDocColorMode) {
    (void)respectPdfDocColorMode;
    bg = WIN_COL_WHITE;
    return WIN_COL_BLACK;
}

COLORREF ThemeWindowLinkColor() {
    return RgbToCOLORREF(0x0020a0);
}

bool ThemeUsesDarkChrome() {
    return false;
}

bool ThemeUsesOriginalPageColors() {
    return false;
}
