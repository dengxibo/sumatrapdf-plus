/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "PdfDarkMode.h"
#include "PdfDarkModeInternal.h"

// Stub implementations for binaries that compile EngineMupdf.cpp but not
// PdfDarkMode*.cpp / Theme.cpp (PdfFilter, PdfPreview, etc.).

bool ThemeUsesDarkChrome() {
    return false;
}

bool PdfDarkModeUsesObjectLevel() {
    return false;
}

u32 PdfDarkModeComputeOptionsHash() {
    return 0;
}

DarkModePalette PdfDarkModeBuildPalette() {
    return DarkModePalette{};
}

DarkModePageAnalysis* PdfDarkModeGetOrBuildAnalysis(fz_context* ctx, FzPageInfo* pageInfo, fz_display_list* list,
                                                    u32 optionsHash) {
    (void)ctx;
    (void)pageInfo;
    (void)list;
    (void)optionsHash;
    return nullptr;
}

fz_device* PdfDarkModeWrapDevice(fz_context* ctx, fz_device* inner, DarkModePageAnalysis* analysis,
                                 const DarkModePalette* palette, DarkModeReplayState* replayState) {
    (void)ctx;
    (void)analysis;
    (void)palette;
    (void)replayState;
    return inner;
}

void PdfDarkModeInvalidatePage(fz_context* ctx, FzPageInfo* pageInfo) {
    (void)ctx;
    (void)pageInfo;
}

int GetPreservePdfImagesMinSize() {
    return 72;
}
