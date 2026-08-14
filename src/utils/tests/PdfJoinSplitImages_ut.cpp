/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "PdfJoinSplitImages.h"

#include "utils/UtAssert.h"

void PdfJoinSplitImages_UnitTests() {
    // Calibre photo pair: keeper ~60% coverage, strip ~9%.
    utassert(PdfJoinSplitShouldHideLowerCoverage(0.61f, 0.09f));
    utassert(PdfJoinSplitShouldHideLowerCoverage(0.09f, 0.61f));

    // True 50/50 tall-image split: keep both pages.
    utassert(!PdfJoinSplitShouldHideLowerCoverage(0.48f, 0.47f));

    // Tiny noise / empty: do not hide.
    utassert(!PdfJoinSplitShouldHideLowerCoverage(0.f, 0.80f));
    utassert(!PdfJoinSplitShouldHideLowerCoverage(0.05f, 0.10f));

    // Marginal but not strip-like enough.
    utassert(!PdfJoinSplitShouldHideLowerCoverage(0.25f, 0.55f));
}
