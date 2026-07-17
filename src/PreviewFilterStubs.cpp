/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

#include "AppTools.h"

struct GlobalPrefs;

// Minimal stubs for PdfFilter/PdfPreview builds that compile EngineMupdf.cpp and
// EpubMeta.cpp without the full SumatraPDF settings / app-data stack.

GlobalPrefs* gGlobalPrefs = nullptr;

TempStr GetPathInAppDataDirTemp(const char* fileName) {
    (void)fileName;
    return nullptr;
}
