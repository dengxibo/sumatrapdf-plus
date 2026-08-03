/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

class EngineBase;

// Stubs linked only by PdfFilter.dll (PdfPreview links EngineEbook.cpp instead).

int EngineEbookGetFormattedPageCount(EngineBase* engine) {
    (void)engine;
    return 0;
}

bool EngineIsProgressiveEbookLoading(EngineBase* engine) {
    (void)engine;
    return false;
}
