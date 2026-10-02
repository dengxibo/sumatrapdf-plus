/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "WhiteMatteSuppression.h"
#include "utils/UtAssert.h"

static void PutPx(u8* samples, int comps, int stride, int x, int y, int r, int g, int b, int a = 255) {
    u8* p = samples + (i64)y * stride + x * comps;
    p[0] = (u8)r;
    p[1] = (u8)g;
    p[2] = (u8)b;
    if (comps == 4) {
        p[3] = (u8)a;
    }
}

static void Fill(u8* samples, int w, int h, int comps, int r, int g, int b) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            PutPx(samples, comps, w * comps, x, y, r, g, b);
        }
    }
}

static WhiteMatteDecision Gate(u8* samples, int w, int h, int comps, float imageW, float imageH, float pageW,
                               float pageH, WhiteMatteGateReport* report = nullptr) {
    return ClassifyWhiteMatte(samples, w, h, comps, w * comps, imageW, imageH, pageW, pageH, report);
}

static void WhiteCanvasAroundSubjectIsAccepted() {
    constexpr int w = 64;
    constexpr int h = 64;
    constexpr int comps = 3;
    u8 samples[w * h * comps];
    Fill(samples, w, h, comps, 255, 255, 255);
    for (int y = 12; y < 52; y++) {
        for (int x = 12; x < 52; x++) {
            int dx = x - 32;
            int dy = y - 32;
            if (dx * dx + dy * dy < 16 * 16) {
                PutPx(samples, comps, w * comps, x, y, 170, 80, 40);
            }
        }
    }
    WhiteMatteGateReport report{};
    WhiteMatteDecision d = Gate(samples, w, h, comps, 200, 200, 600, 800, &report);
    utassert(d == WhiteMatteDecision::Accept);
    utassert(report.cornerCount == 4);
    utassert(report.perimeter >= 0.55f);
}

static void LightGrayCanvasIsAccepted() {
    constexpr int w = 48;
    constexpr int h = 48;
    constexpr int comps = 3;
    u8 samples[w * h * comps];
    Fill(samples, w, h, comps, 245, 245, 245);
    for (int y = 10; y < 38; y++) {
        for (int x = 10; x < 38; x++) {
            PutPx(samples, comps, w * comps, x, y, 40, 40, 50);
        }
    }
    utassert(Gate(samples, w, h, comps, 180, 180, 600, 800) == WhiteMatteDecision::Accept);
}

static void SkyOnlyOnTopIsSkipped() {
    constexpr int w = 48;
    constexpr int h = 48;
    constexpr int comps = 3;
    u8 samples[w * h * comps];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (y < 16) {
                PutPx(samples, comps, w * comps, x, y, 250, 250, 252);
            } else {
                PutPx(samples, comps, w * comps, x, y, 70, 90, 40);
            }
        }
    }
    WhiteMatteDecision d = Gate(samples, w, h, comps, 200, 200, 600, 800);
    utassert(d == WhiteMatteDecision::CornersNotWhite || d == WhiteMatteDecision::PerimeterInsufficient ||
             d == WhiteMatteDecision::BackgroundNotNeutral);
    utassert(d != WhiteMatteDecision::Accept);
}

static void TexturedBorderIsSkipped() {
    constexpr int w = 48;
    constexpr int h = 48;
    constexpr int comps = 3;
    u8 samples[w * h * comps];
    Fill(samples, w, h, comps, 30, 30, 30);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool edge = x < 6 || y < 6 || x >= w - 6 || y >= h - 6;
            if (!edge) {
                continue;
            }
            if (((x + y) & 1) == 0) {
                PutPx(samples, comps, w * comps, x, y, 255, 255, 255);
            } else {
                PutPx(samples, comps, w * comps, x, y, 236, 236, 236);
            }
        }
    }
    utassert(Gate(samples, w, h, comps, 200, 200, 600, 800) == WhiteMatteDecision::BorderTooTextured);
}

static void FullPageRasterIsSkipped() {
    constexpr int w = 40;
    constexpr int h = 40;
    constexpr int comps = 3;
    u8 samples[w * h * comps];
    Fill(samples, w, h, comps, 255, 255, 255);
    utassert(Gate(samples, w, h, comps, 560, 760, 600, 800) == WhiteMatteDecision::FullPageRaster);
}

static void SoftAlphaIsSkipped() {
    constexpr int w = 40;
    constexpr int h = 40;
    constexpr int comps = 4;
    u8 samples[w * h * comps];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            PutPx(samples, comps, w * comps, x, y, 255, 255, 255, 10);
        }
    }
    utassert(Gate(samples, w, h, comps, 100, 100, 600, 800) == WhiteMatteDecision::HasAlpha);
}

void WhiteMatteSuppression_UnitTests() {
    WhiteCanvasAroundSubjectIsAccepted();
    LightGrayCanvasIsAccepted();
    SkyOnlyOnTopIsSkipped();
    TexturedBorderIsSkipped();
    FullPageRasterIsSkipped();
    SoftAlphaIsSkipped();
}
