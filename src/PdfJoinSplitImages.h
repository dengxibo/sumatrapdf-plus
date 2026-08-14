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

int EngineMupdfMapDisplayPageToEngine(EngineBase* engine, int displayPageNo);
int EngineMupdfMapEnginePageToDisplay(EngineBase* engine, int enginePageNo);
bool EngineMupdfHasJoinSplitPdfImages(EngineBase* engine);
