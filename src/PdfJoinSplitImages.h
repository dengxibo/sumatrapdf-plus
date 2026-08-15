/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Heuristics / helpers for JoinSplitPdfImages (Calibre-style photo PDFs).

#pragma once

class EngineBase;

// True when the lower-coverage page of a same-image pair is a thin clipped
// strip and the higher-coverage page shows a substantial photo.
inline bool PdfJoinSplitShouldHideLowerCoverage(float covA, float covB) {
    float lo = covA < covB ? covA : covB;
    float hi = covA < covB ? covB : covA;
    return lo > 0.f && lo < 0.20f && hi >= 0.35f && hi >= lo * 2.5f;
}

// Page-space dest for the full photo on the keeper page: prefer the unclipped
// image placement when the CTM already puts the whole photo on that page;
// otherwise stack the strip's visible height onto the keeper.
inline RectF PdfJoinSplitKeeperDest(const RectF& vis, const RectF& unclipped, const RectF& stripVis, bool stripAfter) {
    if (!unclipped.IsEmpty() && unclipped.dy > vis.dy * 1.01f) {
        return unclipped;
    }
    RectF dest = vis;
    if (dest.IsEmpty() || stripVis.IsEmpty()) {
        return dest;
    }
    if (stripAfter) {
        dest.dy += stripVis.dy;
    } else {
        dest.y -= stripVis.dy;
        dest.dy += stripVis.dy;
    }
    return dest;
}

int EngineMupdfMapDisplayPageToEngine(EngineBase* engine, int displayPageNo);
int EngineMupdfMapEnginePageToDisplay(EngineBase* engine, int enginePageNo);
bool EngineMupdfHasJoinSplitPdfImages(EngineBase* engine);
