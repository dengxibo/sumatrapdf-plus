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

    // Guild photo book: same CTM on both pages; keeper clip hides ~23pt of a
    // 640.8pt-tall image. Unclipped dest is the full photo.
    RectF vis(77.19f, 101.67f, 462.81f, 617.82f);
    RectF unclipped(77.19f, 101.67f, 462.81f, 640.81f);
    RectF strip(77.19f, 72.f, 462.81f, 22.99f);
    RectF dest = PdfJoinSplitKeeperDest(vis, unclipped, strip, true);
    utassert(dest.dx > 462.f && dest.dy > 640.f && dest.dy < 641.f);

    // No unclipped dest: stack the strip height onto the keeper.
    RectF stacked = PdfJoinSplitKeeperDest(vis, {}, strip, true);
    utassert(stacked.dy > 640.f && stacked.dy < 641.f);
}
