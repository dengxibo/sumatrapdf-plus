/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Low-resolution gate for an artificial white canvas around an ebook figure.
// This does not build a mask and does not change pixels. Uncertain images skip.

enum class WhiteMatteDecision : int {
    Accept = 0,
    TooSmall,
    HasAlpha,
    FullPageRaster,
    CornersNotWhite,
    PerimeterInsufficient,
    BorderTooTextured,
    BackgroundNotNeutral,
};

struct WhiteMatteGateReport {
    int srcW = 0;
    int srcH = 0;
    int cornerCount = 0;
    float perimeter = 0;
    float top = 0;
    float bottom = 0;
    float left = 0;
    float right = 0;
    float bgL = 0;
    float bgA = 0;
    float bgB = 0;
    float bgC = 0;
    float borderMad = 0;
    float texture = 0;
};

const char* WhiteMatteDecisionName(WhiteMatteDecision decision);

// samples are RGB or RGBA (alpha last). imageOnPage / page sizes are in the
// same space (page points). Pass pageW <= 0 to skip the full-page check.
WhiteMatteDecision ClassifyWhiteMatte(const u8* samples, int w, int h, int comps, int stride, float imageOnPageW,
                                      float imageOnPageH, float pageW, float pageH, WhiteMatteGateReport* report);
