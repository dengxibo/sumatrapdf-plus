/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"
#include "utils/Log.h"
#include "utils/Timer.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "Theme.h"
#include "Translations.h"

#include "PdfDarkModeInternal.h"
#include "PdfDarkModeV2.h"

// Hardcoded PDF dark mode defaults (not persisted in settings file).
static constexpr int kPreservePdfImagesMinSize = 72;
// Object-level Smart Dark for dark + follow-theme PDF (experiment; was LegacyBitmapPostProcess).
static constexpr PdfDarkModeRenderer kPdfDarkModeRenderer = PdfDarkModeRenderer::ObjectLevelDevice;

static bool gPreservePdfImagesInDarkMode = true;

bool PdfDarkModePagePerfOn() {
    static int on = -1;
    if (on < 0) {
        on = GetEnvironmentVariableA("SUMATRA_PAGE_PERF", nullptr, 0) > 0 ? 1 : 0;
    }
    return on == 1;
}

static PdfDocumentColorMode PdfDocumentColorModeFromString(const char* v) {
    if (!v || !*v || str::EqI(v, "auto") || str::EqI(v, "smart")) {
        return PdfDocumentColorMode::Auto;
    }
    // Legacy "theme"/"black" (old Match Theme) now uses smart follow-theme behavior.
    if (str::EqI(v, "black") || str::EqI(v, "theme")) {
        return PdfDocumentColorMode::Auto;
    }
    if (str::EqI(v, "light") || str::EqI(v, "original")) {
        return PdfDocumentColorMode::Light;
    }
    return PdfDocumentColorMode::Auto;
}

static const char* PdfDocumentColorModeToString(PdfDocumentColorMode mode) {
    switch (mode) {
        case PdfDocumentColorMode::Black:
        case PdfDocumentColorMode::Auto:
        default:
            return "theme";
        case PdfDocumentColorMode::Light:
            return "original";
    }
}

static int gShadeForwardCount = 0;

void PdfDarkModeRecordShadeForward() {
    gShadeForwardCount++;
}

int PdfDarkModeTakeShadeForwardCount() {
    int n = gShadeForwardCount;
    gShadeForwardCount = 0;
    return n;
}

bool GetPreservePdfImagesInDarkMode() {
    return gPreservePdfImagesInDarkMode;
}

bool PdfSmartModePreservesEmbeddedImages() {
    if (GetPdfDocumentColorMode() != PdfDocumentColorMode::Auto) {
        return false;
    }
    if (ThemeUsesEyeCareChrome()) {
        return true;
    }
    if (PdfFollowThemePreservesEmbeddedImageColors()) {
        return true;
    }
    return GetPreservePdfImagesInDarkMode() && ThemeUsesDarkChrome();
}

bool PdfFollowThemePreservesEmbeddedImageColors() {
    return GetPdfDocumentColorMode() == PdfDocumentColorMode::Auto && ThemeUsesDarkChrome();
}

void SetPreservePdfImagesInDarkMode(bool preserve) {
    gPreservePdfImagesInDarkMode = preserve;
}

int GetPreservePdfImagesMinSize() {
    return kPreservePdfImagesMinSize;
}

PdfDarkModeRenderer GetPdfDarkModeRenderer() {
    return kPdfDarkModeRenderer;
}

PdfDocumentColorMode GetPdfDocumentColorMode() {
    if (!gGlobalPrefs || !gGlobalPrefs->documentColorMode) {
        return PdfDocumentColorMode::Auto;
    }
    return PdfDocumentColorModeFromString(gGlobalPrefs->documentColorMode);
}

static const char* PdfImageDarkStrategyToString(PdfImageDarkStrategy strategy) {
    switch (strategy) {
        case PdfImageDarkStrategy::Original:
            return "original";
        case PdfImageDarkStrategy::Tone:
            return "tone";
        case PdfImageDarkStrategy::Auto:
        default:
            return "auto";
    }
}

PdfImageDarkStrategy GetPdfImageDarkStrategy() {
    if (!ThemeUsesDarkChrome() || GetPdfDocumentColorMode() != PdfDocumentColorMode::Auto) {
        return PdfImageDarkStrategy::Auto;
    }
    if (!gGlobalPrefs || !gGlobalPrefs->documentImageDarkStrategy) {
        return PdfImageDarkStrategy::Auto;
    }
    const char* v = gGlobalPrefs->documentImageDarkStrategy;
    if (str::EqI(v, "original")) {
        return PdfImageDarkStrategy::Original;
    }
    // Retired per-channel invert. Tone covers the same job and keeps hue.
    if (str::EqI(v, "simple")) {
        str::ReplaceWithCopy(&gGlobalPrefs->documentImageDarkStrategy, "tone");
        return PdfImageDarkStrategy::Tone;
    }
    if (str::EqI(v, "tone")) {
        return PdfImageDarkStrategy::Tone;
    }
    return PdfImageDarkStrategy::Auto;
}

void SetPdfImageDarkStrategy(PdfImageDarkStrategy strategy) {
    if (!gGlobalPrefs) {
        return;
    }
    const char* name = PdfImageDarkStrategyToString(strategy);
    if (!str::EqI(gGlobalPrefs->documentImageDarkStrategy, name)) {
        str::ReplaceWithCopy(&gGlobalPrefs->documentImageDarkStrategy, name);
    }
}

void SetPdfDocumentColorMode(PdfDocumentColorMode mode) {
    if (mode < PdfDocumentColorMode::Auto || mode > PdfDocumentColorMode::Light) {
        mode = PdfDocumentColorMode::Auto;
    }
    if (mode == PdfDocumentColorMode::Black) {
        mode = PdfDocumentColorMode::Auto;
    }
    if (!gGlobalPrefs) {
        return;
    }
    const char* name = PdfDocumentColorModeToString(mode);
    if (!str::EqI(gGlobalPrefs->documentColorMode, name)) {
        str::ReplaceWithCopy(&gGlobalPrefs->documentColorMode, name);
    }
}

const char* PdfDocumentColorModeDescription(PdfDocumentColorMode mode) {
    switch (mode) {
        case PdfDocumentColorMode::Light:
            return _TRN("Document Color Mode: Original (document colors unchanged)");
        case PdfDocumentColorMode::Black:
        case PdfDocumentColorMode::Auto:
        default:
            return _TRN("Document Color Mode: Match theme (use current theme colors)");
    }
}

bool PdfDarkModeUsesObjectLevel() {
    if (!ThemeUsesDarkChrome()) {
        return false;
    }
    if (GetPdfDocumentColorMode() != PdfDocumentColorMode::Auto) {
        return false;
    }
    return GetPdfDarkModeRenderer() == PdfDarkModeRenderer::ObjectLevelDevice;
}

void PdfDarkModeClearPixmapToThemeBackground(fz_context* ctx, fz_pixmap* pix, const DarkModePalette& palette) {
    if (!pix || !pix->samples) {
        return;
    }
    byte rb, gb, bb;
    if (ThemeUsesDarkChrome() && !ThemeUsesBlackChrome()) {
        COLORREF bgCol;
        ThemePageRenderColors(bgCol, true);
        UnpackColor(bgCol, rb, gb, bb);
    } else {
        rb = (byte)(palette.bgR * 255.f + 0.5f);
        gb = (byte)(palette.bgG * 255.f + 0.5f);
        bb = (byte)(palette.bgB * 255.f + 0.5f);
    }
    int w = pix->w;
    int h = pix->h;
    int n = pix->n;
    for (int y = 0; y < h; y++) {
        unsigned char* row = pix->samples + (size_t)y * pix->stride;
        for (int x = 0; x < w; x++) {
            unsigned char* p = row + x * n;
            p[0] = rb;
            p[1] = gb;
            p[2] = bb;
            if (pix->alpha && n >= 4) {
                p[3] = 255;
            }
        }
    }
}

DarkModeOptions PdfDarkModeCurrentOptions() {
    DarkModeOptions opts;
    if (PdfDarkModeUsesObjectLevel()) {
        // Mild soft for small embedded figures; full-bleed uses picture-book paper+ink remap.
        opts.preserveImagePaperSoftening = 0.45f;
    }
    return opts;
}

u32 PdfDarkModeComputeOptionsHash() {
    DarkModeProfile profile;
    BuildViewDarkModeProfile(nullptr, &profile);
    return profile.hash;
}

DarkModePalette PdfDarkModeBuildPalette() {
    DarkModeProfile profile;
    BuildViewDarkModeProfile(nullptr, &profile);
    return profile.palette;
}

static bool IsLikelyLinkRgb(float r, float g, float b) {
    int ri = (int)(r * 255.f + 0.5f);
    int gi = (int)(g * 255.f + 0.5f);
    int bi = (int)(b * 255.f + 0.5f);
    int maxRG = ri > gi ? ri : gi;
    if (bi < maxRG + 25) {
        return false;
    }
    if (bi < 72) {
        return false;
    }
    int lum = (ri + gi + bi) / 3;
    if (lum > 230) {
        return false;
    }
    return true;
}

// 公文 红头 / 红章. Linear invert maps red → cyan/green; keep hue red on dark paper.
static bool IsOfficialRedInkRgb(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (maxC - minC < 0.11f) {
        return false;
    }
    if (r <= g + 0.05f || r <= b + 0.05f) {
        return false;
    }
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum > 0.90f) {
        return false;
    }
    if (g > 0.62f && b > 0.55f) {
        return false;
    }
    return true;
}

static void MapOfficialRedInkToDark(float r, float g, float b, float* outRgb) {
    float nr = r * 1.15f;
    float ng = g * 0.45f;
    float nb = b * 0.45f;
    if (nr > 1.f) {
        nr = 1.f;
    }
    if (ng > nr * 0.45f) {
        ng = nr * 0.45f;
    }
    if (nb > nr * 0.45f) {
        nb = nr * 0.45f;
    }
    outRgb[0] = nr;
    outRgb[1] = ng;
    outRgb[2] = nb;
}

static float SmoothStep(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x >= edge1 ? 1.f : 0.f;
    }
    float t = (x - edge0) / (edge1 - edge0);
    if (t <= 0.f) {
        return 0.f;
    }
    if (t >= 1.f) {
        return 1.f;
    }
    return t * t * (3.f - 2.f * t);
}

// Steep ink↔paper map for near-gray document pixels.
// Soft SmoothStep across a wide band leaves anti-aliased text fringes as mid-gray
// "halos" on dark backgrounds; a narrower + squared curve snaps them to theme text/bg.
// Photos must not go through this — callers gate by chroma and/or photo rects.
static bool ApplySharpDocumentInkPaper(float r, float g, float b, const DarkModePalette& palette, float* outR,
                                       float* outG, float* outB) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    // Pastel cream/mint callouts are high-lum with a little chroma; mid-tone photo
    // pixels stay under the tighter 0.11 gate.
    float lowChroma = 0.11f;
    if (lum > 0.88f) {
        lowChroma = 0.20f;
    } else if (lum > 0.78f) {
        lowChroma = 0.15f;
    }
    if (chroma >= lowChroma) {
        return false;
    }
    float t = SmoothStep(0.34f, 0.58f, lum);
    t = t * t * (3.f - 2.f * t);
    float inkW = 1.f - t;
    float paperW = t;
    float nr = palette.textR * inkW + palette.bgR * paperW;
    float ng = palette.textG * inkW + palette.bgG * paperW;
    float nb = palette.textB * inkW + palette.bgB * paperW;
    float grayW = 1.f - chroma / lowChroma;
    *outR = nr * grayW + r * (1.f - grayW);
    *outG = ng * grayW + g * (1.f - grayW);
    *outB = nb * grayW + b * (1.f - grayW);
    return true;
}

// Near-binarize gray scan pixels for 公文 / office paper (JPEG noise, glare strips).
// Colored stamps and red headers use threshold-based snap; no original-color blend.
static float dm_gov_paper_luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static void dm_gov_sample_rgb(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb, int components,
                              int x, int y, float* outR, float* outG, float* outB) {
    int n = pix->n;
    int stride = pix->stride;
    unsigned char* px = pix->samples + y * stride + x * n;
    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);
    if (fastRgb) {
        *outR = px[0] / 255.f;
        *outG = px[1] / 255.f;
        *outB = px[2] / 255.f;
    } else if (fastGray) {
        *outR = *outG = *outB = px[0] / 255.f;
    } else {
        float conv[FZ_MAX_COLORS] = {};
        float srcRgb[FZ_MAX_COLORS] = {};
        for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
            conv[c] = px[c] / 255.f;
        }
        fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
        *outR = srcRgb[0];
        *outG = srcRgb[1];
        *outB = srcRgb[2];
    }
}

static float dm_gov_estimate_binary_threshold(fz_context* ctx, fz_pixmap* src, fz_colorspace* cs, fz_colorspace* rgb,
                                              int components, int w, int h) {
    int paperN = 0;
    int inkN = 0;
    float paperSum = 0.f;
    float inkSum = 0.f;
    int stepX = w > 96 ? w / 96 : 1;
    int stepY = h > 96 ? h / 96 : 1;
    for (int y = 0; y < h; y += stepY) {
        for (int x = 0; x < w; x += stepX) {
            float r, g, b;
            dm_gov_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            float lum = dm_gov_paper_luminance(r, g, b);
            if (lum >= 0.78f) {
                paperSum += lum;
                paperN++;
            } else if (lum <= 0.62f) {
                inkSum += lum;
                inkN++;
            }
        }
    }
    float threshold = 0.50f;
    if (paperN > 8 && inkN > 8) {
        threshold = (paperSum / (float)paperN + inkSum / (float)inkN) * 0.5f;
        if (threshold > 0.75f) {
            threshold = 0.75f;
        }
        if (threshold < 0.42f) {
            threshold = 0.42f;
        }
    } else if (paperN > 8) {
        threshold = (paperSum / (float)paperN) * 0.55f;
        if (threshold > 0.50f) {
            threshold = 0.50f;
        }
    }
    if (threshold < 0.40f) {
        threshold = 0.40f;
    }
    return threshold;
}

struct GovPaperThemeBytes {
    byte paperR = 0;
    byte paperG = 0;
    byte paperB = 0;
    byte inkR = 0;
    byte inkG = 0;
    byte inkB = 0;
};

static DarkModePalette PdfDarkModeGovernmentPaperPalette(const DarkModePalette& palette) {
    COLORREF bgCol;
    COLORREF textCol = ThemePageRenderColors(bgCol, true);
    byte tr, tg, tb, br, bg, bb;
    UnpackColor(textCol, tr, tg, tb);
    UnpackColor(bgCol, br, bg, bb);

    DarkModePalette p = palette;
    p.textR = tr / 255.f;
    p.textG = tg / 255.f;
    p.textB = tb / 255.f;
    p.bgR = br / 255.f;
    p.bgG = bg / 255.f;
    p.bgB = bb / 255.f;
    p.diffR = p.bgR - p.textR;
    p.diffG = p.bgG - p.textG;
    p.diffB = p.bgB - p.textB;
    return p;
}

static GovPaperThemeBytes PdfDarkModeGovernmentPaperThemeBytes(const DarkModePalette& palette) {
    GovPaperThemeBytes t;
    COLORREF bgCol;
    COLORREF textCol = ThemePageRenderColors(bgCol, true);
    UnpackColor(textCol, t.inkR, t.inkG, t.inkB);
    UnpackColor(bgCol, t.paperR, t.paperG, t.paperB);
    // Black chrome keeps ThemePageRenderColors pure black; non-dark falls back to palette floats.
    if (!ThemeUsesDarkChrome()) {
        t.paperR = (byte)(palette.bgR * 255.f + 0.5f);
        t.paperG = (byte)(palette.bgG * 255.f + 0.5f);
        t.paperB = (byte)(palette.bgB * 255.f + 0.5f);
        t.inkR = (byte)(palette.textR * 255.f + 0.5f);
        t.inkG = (byte)(palette.textG * 255.f + 0.5f);
        t.inkB = (byte)(palette.textB * 255.f + 0.5f);
    }
    return t;
}

static bool ApplyGovernmentPaperInkPaper(float r, float g, float b, float threshold, const DarkModePalette& palette,
                                         float* outR, float* outG, float* outB) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    if (chroma >= 0.14f) {
        return false;
    }
    float lum = dm_gov_paper_luminance(r, g, b);
    float paperW = lum >= threshold ? 1.f : 0.f;
    float inkW = 1.f - paperW;
    *outR = palette.textR * inkW + palette.bgR * paperW;
    *outG = palette.textG * inkW + palette.bgG * paperW;
    *outB = palette.textB * inkW + palette.bgB * paperW;
    return true;
}

// Red 红头 / stamp pixels: snap paper bleed to bg; keep saturated red ink readable on dark theme.
static bool ApplyGovernmentPaperColoredPixel(float r, float g, float b, float threshold, const DarkModePalette& palette,
                                             float* outR, float* outG, float* outB) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    if (chroma < 0.11f) {
        return false;
    }
    float lum = dm_gov_paper_luminance(r, g, b);
    if (r <= g + 0.03f || r <= b + 0.03f) {
        return false;
    }
    if (lum >= threshold + 0.03f) {
        *outR = palette.bgR;
        *outG = palette.bgG;
        *outB = palette.bgB;
        return true;
    }
    float nr = palette.textR * 0.12f + r * 1.12f;
    float ng = palette.textG * 0.12f + g * 0.40f;
    float nb = palette.textB * 0.12f + b * 0.40f;
    if (nr > 1.f) {
        nr = 1.f;
    }
    if (ng > 1.f) {
        ng = 1.f;
    }
    if (nb > 1.f) {
        nb = 1.f;
    }
    if (nr < 0.f) {
        nr = 0.f;
    }
    if (ng < 0.f) {
        ng = 0.f;
    }
    if (nb < 0.f) {
        nb = 0.f;
    }
    *outR = nr;
    *outG = ng;
    *outB = nb;
    return true;
}

static void ApplyGovernmentPaperFallbackPixel(float r, float g, float b, float threshold,
                                              const DarkModePalette& palette, float* outR, float* outG, float* outB) {
    float lum = dm_gov_paper_luminance(r, g, b);
    if (lum >= threshold) {
        *outR = palette.bgR;
        *outG = palette.bgG;
        *outB = palette.bgB;
    } else {
        *outR = palette.textR;
        *outG = palette.textG;
        *outB = palette.textB;
    }
}

void ApplyAdaptiveDocumentDarkMode(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                                   float* outB) {
    if (ApplySharpDocumentInkPaper(r, g, b, palette, outR, outG, outB)) {
        return;
    }

    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float delta = maxC - minC;
    float h = 0.f;
    if (delta > 0.0001f) {
        if (maxC == r) {
            h = fmodf((g - b) / delta, 6.f);
        } else if (maxC == g) {
            h = (b - r) / delta + 2.f;
        } else {
            h = (r - g) / delta + 4.f;
        }
        h /= 6.f;
        if (h < 0.f) {
            h += 1.f;
        }
    }

    float cappedV = lum;
    const float maxBright = 0.82f;
    if (cappedV > maxBright) {
        cappedV = maxBright;
    }
    const float minBright = 0.12f;
    if (cappedV < minBright) {
        cappedV = minBright;
    }

    float s = maxC > 0.f ? delta / maxC : 0.f;
    float c = cappedV * s;
    float x = c * (1.f - fabsf(fmodf(h * 6.f, 2.f) - 1.f));
    float m = cappedV - c;
    float rr = 0.f, gg = 0.f, bb = 0.f;
    int hi = (int)(h * 6.f);
    switch (hi % 6) {
        case 0:
            rr = c;
            gg = x;
            break;
        case 1:
            rr = x;
            gg = c;
            break;
        case 2:
            gg = c;
            bb = x;
            break;
        case 3:
            gg = x;
            bb = c;
            break;
        case 4:
            rr = x;
            bb = c;
            break;
        default:
            rr = c;
            bb = x;
            break;
    }
    *outR = rr + m;
    *outG = gg + m;
    *outB = bb + m;
}

void MapRgbToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    if (IsOfficialRedInkRgb(r, g, b)) {
        MapOfficialRedInkToDark(r, g, b, outRgb);
        return;
    }
    if (PdfDarkModeUsesObjectLevel()) {
        MapRgbToDarkThemeOklab(r, g, b, palette, outRgb);
        return;
    }
    outRgb[0] = palette.textR + r * palette.diffR;
    outRgb[1] = palette.textG + g * palette.diffG;
    outRgb[2] = palette.textB + b * palette.diffB;
}

void MapColorToDarkTheme(fz_context* ctx, fz_colorspace* cs, const float* color, fz_color_params colorParams,
                         const DarkModePalette& palette, float* outRgb) {
    float rgb[FZ_MAX_COLORS] = {};
    fz_colorspace* ds = fz_device_rgb(ctx);
    fz_convert_color(ctx, cs, color, ds, rgb, cs, colorParams);
    if (ThemeUsesDarkChrome() && IsLikelyLinkRgb(rgb[0], rgb[1], rgb[2])) {
        outRgb[0] = palette.linkR;
        outRgb[1] = palette.linkG;
        outRgb[2] = palette.linkB;
        return;
    }
    MapRgbToDarkTheme(rgb[0], rgb[1], rgb[2], palette, outRgb);
}

void MapFillColorToDarkTheme(fz_context* ctx, fz_colorspace* cs, const float* color, fz_color_params colorParams,
                             const DarkModePalette& palette, float* outRgb) {
    float rgb[FZ_MAX_COLORS] = {};
    fz_colorspace* ds = fz_device_rgb(ctx);
    fz_convert_color(ctx, cs, color, ds, rgb, cs, colorParams);
    MapRgbFillToDarkTheme(rgb[0], rgb[1], rgb[2], palette, outRgb);
}

void MapRgbFillToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float chroma = maxC - minC;
    DarkModeOptions opts = PdfDarkModeCurrentOptions();
    // Tinted page paper / pastel callout fills: linear invert for textbook readability.
    // Cream/mint panels often have chroma a bit above plain gray — allow more when bright.
    float paperChroma = lum >= 0.82f ? 0.22f : 0.14f;
    if (PdfFollowThemePreservesEmbeddedImageColors() && chroma < paperChroma) {
        outRgb[0] = palette.textR + r * palette.diffR;
        outRgb[1] = palette.textG + g * palette.diffG;
        outRgb[2] = palette.textB + b * palette.diffB;
        return;
    }
    if (GetPdfDocumentColorMode() != PdfDocumentColorMode::Auto && lum >= opts.lightFillLuminanceThreshold &&
        chroma >= opts.lightFillChromaThreshold) {
        ApplyAdaptiveDocumentDarkMode(r, g, b, palette, &outRgb[0], &outRgb[1], &outRgb[2]);
        return;
    }
    MapRgbToDarkTheme(r, g, b, palette, outRgb);
}

void ApplyPreserveImagePaperSoftening(float r, float g, float b, const DarkModePalette& palette, float strength,
                                      float* outR, float* outG, float* outB) {
    if (strength <= 0.f) {
        *outR = r;
        *outG = g;
        *outB = b;
        return;
    }

    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;

    // Only near-white paper — mid-tone photo shadows must not be pulled to black.
    const float lowChroma = 0.085f;
    float paperW = 0.f;
    if (chroma < lowChroma) {
        paperW = SmoothStep(0.78f, 0.95f, lum);
    } else {
        float chromaFactor = 1.f - chroma / 0.25f;
        if (chromaFactor < 0.f) {
            chromaFactor = 0.f;
        }
        paperW = SmoothStep(0.88f, 0.98f, lum) * chromaFactor;
    }
    paperW *= strength;

    *outR = r + (palette.bgR - r) * paperW;
    *outG = g + (palette.bgG - g) * paperW;
    *outB = b + (palette.bgB - b) * paperW;
}

// Full-bleed RAZ pages bake photo + black body text + white paper into one image.
// Per-pixel ink/paper heuristics destroy photo interiors (hair→white, walls→black).
// Instead: find dense photo rectangle(s) and leave them untouched; only remap
// surrounding paper/text margins (never linear-invert — that turns B&W portraits negative).
void ApplyPreservePictureBookPaperAndInk(float r, float g, float b, const DarkModePalette& palette, float* outR,
                                         float* outG, float* outB) {
    // Margin-only fallback when no photo rect is available: linear paper↔ink remap.
    *outR = palette.textR + r * palette.diffR;
    *outG = palette.textG + g * palette.diffG;
    *outB = palette.textB + b * palette.diffB;
}

static bool dm_pb_is_paper_rgb(float r, float g, float b) {
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return (maxC - minC) < 0.08f && lum > 0.88f;
}

struct DmPbPhotoRect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    // Side-harvested sparse drawings (line art). Skip photo-mat flood / callout drop.
    bool sparse = false;
    // Closed black frame around a color drawing. Interior stays original; no flood.
    bool framed = false;
};

static constexpr int kDmPbMaxPhotoRects = 8;

static int dm_pb_find_photo_rects(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* outRects, int maxRects,
                                  const float* lumPlane);
static bool dm_pb_sample_rgb(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb, int components,
                             int x, int y, float* outR, float* outG, float* outB);

// One RGB→luminance pass; local variance then reads this plane (not 9× RGB samples / pixel).
static float* dm_pb_build_lum_plane(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb,
                                    int components) {
    if (!ctx || !pix || !pix->samples || pix->w <= 0 || pix->h <= 0) {
        return nullptr;
    }
    int w = pix->w;
    int h = pix->h;
    float* lum = AllocArray<float>(w * h);
    if (!lum) {
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            lum[y * w + x] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }
    }
    return lum;
}

static float dm_pb_lum_var_3x3(const float* lum, int w, int h, int x, int y) {
    if (!lum) {
        return 0.f;
    }
    float sum = 0.f;
    float sumSq = 0.f;
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        int yy = y + dy;
        if (yy < 0 || yy >= h) {
            continue;
        }
        const float* row = lum + yy * w;
        for (int dx = -1; dx <= 1; dx++) {
            int xx = x + dx;
            if (xx < 0 || xx >= w) {
                continue;
            }
            float v = row[xx];
            sum += v;
            sumSq += v * v;
            n++;
        }
    }
    if (n < 4) {
        return 0.f;
    }
    float mean = sum / (float)n;
    float var = sumSq / (float)n - mean * mean;
    return var > 0.f ? var : 0.f;
}

// Precompute 3×3 lum variance once for the remap / edge-mat loops.
static float* dm_pb_build_local_var_plane(const float* lum, int w, int h) {
    if (!lum || w <= 0 || h <= 0) {
        return nullptr;
    }
    float* var = AllocArray<float>(w * h);
    if (!var) {
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            var[y * w + x] = dm_pb_lum_var_3x3(lum, w, h, x, y);
        }
    }
    return var;
}

// Narrow photo-texture gate: moderate local variance + low chroma.
// Dark hair/fur must qualify — lum < 0.36 forced crown hair through SharpDocument
// (ink→theme text = white skullcap on Match-theme). Flat paper / glyph AA stay out.
static bool dm_pb_rgb_is_photo_texture(float r, float g, float b, float localVar) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (chroma >= 0.18f || lum < 0.08f || lum > 0.96f) {
        return false;
    }
    // Fur/fabric/tile grain sits in a moderate band; flat JPEG paper is lower; AA text higher.
    return localVar >= 0.00055f && localVar <= 0.014f;
}

// Dense-band content: non-paper, or paper-colored pixels that still look like photo grain
// (light fur / white clothes). Flat cream/white margins stay paper so text pages do not
// collapse into one giant photo rect.
static bool dm_pb_is_dense_content_rgb(float r, float g, float b, const float* lum, int w, int h, int x, int y) {
    if (!dm_pb_is_paper_rgb(r, g, b)) {
        return true;
    }
    return dm_pb_lum_var_3x3(lum, w, h, x, y) >= 0.00055f;
}

struct DmPbPageStats {
    float paperRatio = 0.f;
    float borderPaperRatio = 0.f;
    float satRatio = 0.f;
    float chromaRatio = 0.f;
    float lumVar = 0.f;
    float redInkRatio = 0.f;
};
static DmPbPageStats dm_pb_estimate_page_stats(fz_context* ctx, fz_pixmap* src, fz_colorspace* cs, fz_colorspace* rgb,
                                               int components);
static bool dm_pb_should_seek_photo_rects(float satRatio, float chromaRatio, float paperRatio, float lumVar,
                                          float borderPaperRatio, const DarkImageAnalysis* imgAnalysis);
static bool dm_pb_pixmap_has_picture_island(fz_context* ctx, fz_pixmap* pix);

// Flat near-white used for oval-portrait rectangular mats (not specular highlights).
// JPEG mats often sit at 0.88–0.93; 0.94 left a light AA ring around the oval.
// Highlights usually have texture / are not edge-connected from the rect border.
static bool dm_pb_is_photo_rect_margin_paper_rgb(float r, float g, float b) {
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return (maxC - minC) < 0.05f && lum > 0.88f;
}

// Mark flat white that is 4-connected to a photo-rect border. Oval portrait mats are
// edge-connected; forehead/soap highlights sit inside tonal skin and are not.
static u8* dm_pb_build_edge_connected_margin_mask(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs,
                                                  fz_colorspace* rgb, int components, int w, int h,
                                                  const DmPbPhotoRect* rects, int nRects, const float* localVar) {
    if (!pix || !rects || nRects <= 0 || w <= 0 || h <= 0) {
        return nullptr;
    }
    u8* mask = AllocArray<u8>(w * h);
    if (!mask) {
        return nullptr;
    }
    u8* paper = AllocArray<u8>(w * h);
    if (!paper) {
        free(mask);
        return nullptr;
    }
    for (int i = 0; i < nRects; i++) {
        const DmPbPhotoRect& r = rects[i];
        int x0 = r.x0 < 0 ? 0 : r.x0;
        int y0 = r.y0 < 0 ? 0 : r.y0;
        int x1 = r.x1 > w ? w : r.x1;
        int y1 = r.y1 > h ? h : r.y1;
        if (x1 - x0 < 4 || y1 - y0 < 4 || r.sparse || r.framed) {
            continue;
        }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                // Flat mat only — textured near-white (fur / fabric) must stay photo-protected.
                float lv = localVar ? localVar[y * w + x] : 0.f;
                if (dm_pb_is_photo_rect_margin_paper_rgb(rv, gv, bv) && !dm_pb_rgb_is_photo_texture(rv, gv, bv, lv)) {
                    paper[y * w + x] = 1;
                }
            }
        }
        int* q = AllocArray<int>((x1 - x0) * (y1 - y0));
        if (!q) {
            continue;
        }
        int qn = 0;
        // No depth cap: cut-out photos (studio shots on flat white) need the flood to
        // reach the rect center. Sparse line art is protected by the r.sparse skip above;
        // textured near-white (fur / fabric) is blocked by the photo-texture gate.
        //
        // Seed only from *long* paper runs on the rect border. Cut-out mats occupy a
        // whole side; prison bars that open at the frame are short stripes and must
        // not become flood gates into the photo.
        auto trySeed = [&](int x, int y) {
            if (x < x0 || x >= x1 || y < y0 || y >= y1) {
                return;
            }
            int idx = y * w + x;
            if (!paper[idx] || mask[idx]) {
                return;
            }
            mask[idx] = 1;
            q[qn++] = idx;
        };
        int shortSide = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
        int minRun = shortSide / 10;
        if (minRun < 28) {
            minRun = 28;
        }
        if (minRun > 80) {
            minRun = 80;
        }
        auto flushHoriz = [&](int y, int runEnd, int runLen) {
            if (runLen < minRun) {
                return;
            }
            for (int t = 0; t < runLen; t++) {
                trySeed(runEnd - runLen + t, y);
            }
        };
        auto flushVert = [&](int x, int runEnd, int runLen) {
            if (runLen < minRun) {
                return;
            }
            for (int t = 0; t < runLen; t++) {
                trySeed(x, runEnd - runLen + t);
            }
        };
        for (int edge = 0; edge < 2; edge++) {
            int y = edge == 0 ? y0 : y1 - 1;
            int run = 0;
            for (int x = x0; x <= x1; x++) {
                bool on = x < x1 && paper[y * w + x];
                if (on) {
                    run++;
                } else {
                    flushHoriz(y, x, run);
                    run = 0;
                }
            }
        }
        for (int edge = 0; edge < 2; edge++) {
            int x = edge == 0 ? x0 : x1 - 1;
            int run = 0;
            for (int y = y0; y <= y1; y++) {
                bool on = y < y1 && paper[y * w + x];
                if (on) {
                    run++;
                } else {
                    flushVert(x, y, run);
                    run = 0;
                }
            }
        }
        for (int qi = 0; qi < qn; qi++) {
            int idx = q[qi];
            int x = idx % w;
            int y = idx / w;
            const int nx[4] = {x - 1, x + 1, x, x};
            const int ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; k++) {
                int xx = nx[k];
                int yy = ny[k];
                if (xx < x0 || xx >= x1 || yy < y0 || yy >= y1) {
                    continue;
                }
                int nidx = yy * w + xx;
                if (!paper[nidx] || mask[nidx]) {
                    continue;
                }
                mask[nidx] = 1;
                q[qn++] = nidx;
            }
        }
        // Enclosed text panels ("Do You Know?" boxes, caption gutters) are flat paper
        // sealed off from the rect border by a ruled line or the photo itself, so the
        // border flood never reaches them. Fold a large flat-paper component into the
        // mat mask only when its boundary touches mostly ink (glyphs / ruled lines).
        // White objects inside the photo itself (Mandela's prison bars) border photo
        // pixels instead and must stay protected.
        int rectArea = (x1 - x0) * (y1 - y0);
        int minPanel = rectArea / 64;
        if (minPanel < 400) {
            minPanel = 400;
        }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                int sidx = y * w + x;
                if (!paper[sidx] || mask[sidx]) {
                    continue;
                }
                int compStart = 0;
                qn = 0;
                int inkAdj = 0;
                int photoAdj = 0;
                mask[sidx] = 1;
                q[qn++] = sidx;
                while (compStart < qn) {
                    int idx = q[compStart++];
                    int cx = idx % w;
                    int cy = idx / w;
                    const int nx[4] = {cx - 1, cx + 1, cx, cx};
                    const int ny[4] = {cy, cy, cy - 1, cy + 1};
                    for (int k = 0; k < 4; k++) {
                        int xx = nx[k];
                        int yy = ny[k];
                        if (xx < x0 || xx >= x1 || yy < y0 || yy >= y1) {
                            continue;
                        }
                        int nidx = yy * w + xx;
                        if (mask[nidx]) {
                            continue;
                        }
                        if (!paper[nidx]) {
                            float rv, gv, bv;
                            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, xx, yy, &rv, &gv, &bv);
                            float maxC = rv > gv ? (rv > bv ? rv : bv) : (gv > bv ? gv : bv);
                            float minC = rv < gv ? (rv < bv ? rv : bv) : (gv < bv ? gv : bv);
                            float lumN = 0.2126f * rv + 0.7152f * gv + 0.0722f * bv;
                            float lvN = localVar ? localVar[nidx] : 0.f;
                            if (lumN < 0.62f && (maxC - minC) < 0.16f && !dm_pb_rgb_is_photo_texture(rv, gv, bv, lvN)) {
                                inkAdj++;
                            } else {
                                photoAdj++;
                            }
                            continue;
                        }
                        mask[nidx] = 1;
                        q[qn++] = nidx;
                    }
                }
                bool keepPanel = qn >= minPanel && inkAdj * 10 >= (inkAdj + photoAdj) * 4;
                if (!keepPanel) {
                    for (int k = 0; k < qn; k++) {
                        mask[q[k]] = 0;
                    }
                    // clear paper so we don't re-visit this component
                    for (int k = 0; k < qn; k++) {
                        paper[q[k]] = 0;
                    }
                }
            }
        }
        free(q);
        // clear paper marks for this rect so the next rect starts clean
        for (int y = y0; y < y1; y++) {
            memset(paper + y * w + x0, 0, (size_t)(x1 - x0));
        }
    }
    free(paper);
    return mask;
}

static bool dm_pb_sample_rgb(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb, int components,
                             int x, int y, float* outR, float* outG, float* outB) {
    int n = pix->n;
    unsigned char* px = pix->samples + y * pix->stride + x * n;
    // Fast path: avoid fz_convert_color on every sample (dominant cost on large pages).
    if (cs == rgb || fz_colorspace_is_rgb(ctx, cs)) {
        *outR = px[0] / 255.f;
        *outG = px[1] / 255.f;
        *outB = px[2] / 255.f;
        return true;
    }
    if (components == 1 || fz_colorspace_is_gray(ctx, cs)) {
        float g = px[0] / 255.f;
        *outR = *outG = *outB = g;
        return true;
    }
    float conv[FZ_MAX_COLORS] = {};
    float srcRgb[FZ_MAX_COLORS] = {};
    for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
        conv[c] = px[c] / 255.f;
    }
    fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
    *outR = srcRgb[0];
    *outG = srcRgb[1];
    *outB = srcRgb[2];
    return true;
}

static fz_pixmap* dm_pb_decode_image(fz_context* ctx, fz_image* image) {
    if (!ctx || !image) {
        return nullptr;
    }
    int w = image->w;
    int h = image->h;
    const int maxDim = 1400;
    if (w <= 0 || h <= 0) {
        return fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
    }
    if (w <= maxDim && h <= maxDim) {
        return fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
    }
    float s = (float)maxDim / (float)(w > h ? w : h);
    fz_matrix ctm = fz_scale(s, s);
    return fz_get_pixmap_from_image(ctx, image, nullptr, &ctm, nullptr, nullptr);
}

bool PdfDarkModeImageDecodeLooksLikeGrayscalePortrait(fz_context* ctx, fz_image* image) {
    if (!ctx || !image) {
        return false;
    }
    fz_pixmap* src = nullptr;
    bool portrait = false;
    fz_var(src);
    fz_try(ctx) {
        src = dm_pb_decode_image(ctx, image);
        if (!src || !src->samples || src->w < 8 || src->h < 8) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "empty image for portrait probe");
        }
        fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
        fz_colorspace* rgb = fz_device_rgb(ctx);
        int components = fz_colorspace_n(ctx, cs);
        int w = src->w;
        int h = src->h;
        int paperSamples = 0;
        int satHits = 0;
        int chromaHits = 0;
        float lumSum = 0.f;
        float lumSqSum = 0.f;
        int estStepX = w > 64 ? w / 64 : 1;
        int estStepY = h > 64 ? h / 64 : 1;
        for (int y = 0; y < h; y += estStepY) {
            for (int x = 0; x < w; x += estStepX) {
                float r, g, b;
                dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
                paperSamples++;
                float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                lumSum += lum;
                lumSqSum += lum * lum;
                float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
                float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
                float chroma = maxC - minC;
                if (chroma >= 0.12f) {
                    satHits++;
                }
                if (chroma >= 0.06f) {
                    chromaHits++;
                }
            }
        }
        if (paperSamples <= 0) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "no portrait samples");
        }
        float satRatio = (float)satHits / (float)paperSamples;
        float chromaRatio = (float)chromaHits / (float)paperSamples;
        float lumMean = lumSum / (float)paperSamples;
        float lumVar = lumSqSum / (float)paperSamples - lumMean * lumMean;
        if (satRatio < 0.08f && chromaRatio < 0.10f && lumVar >= 0.035f) {
            portrait = true;
        } else {
            DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, 0.97f, true);
            portrait =
                PdfDarkModeFeaturesLookLikeGrayscalePhoto(analysis.features) && lumVar >= 0.035f && satRatio < 0.12f;
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
    }
    fz_catch(ctx) {
        portrait = false;
    }
    return portrait;
}

bool PdfDarkModeImageShouldStayOriginal(fz_context* ctx, fz_image* image) {
    if (!ctx || !image) {
        return false;
    }
    fz_pixmap* src = nullptr;
    bool stay = false;
    fz_var(src);
    fz_try(ctx) {
        src = dm_pb_decode_image(ctx, image);
        if (!src || !src->samples || src->w < 8 || src->h < 8) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "empty image for original-photo probe");
        }
        fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
        fz_colorspace* rgb = fz_device_rgb(ctx);
        int components = fz_colorspace_n(ctx, cs);
        DmPbPageStats st = dm_pb_estimate_page_stats(ctx, src, cs, rgb, components);
        // Flat paper with little color is a text/office scan, not a photo.
        bool textPage = st.paperRatio >= 0.70f && st.satRatio < 0.08f && st.lumVar < 0.040f;
        // 连环画 / line art: modest color, paper still dominates.
        bool lineArt = st.satRatio < 0.06f && st.chromaRatio < 0.12f && st.paperRatio >= 0.45f && st.lumVar < 0.050f;
        if (!textPage && !lineArt && st.lumVar >= 0.018f) {
            stay = true;
        } else if (dm_pb_pixmap_has_picture_island(ctx, src)) {
            // Mostly white page with a localized color drawing (RAZ "In"). The
            // thumbnail looks like a text scan; binarize would invert the picture.
            stay = true;
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
    }
    fz_catch(ctx) {
        stay = false;
    }
    return stay;
}

bool PdfDarkModeImageHasPreservablePhotoRects(fz_context* ctx, fz_image* image) {
    if (!ctx || !image) {
        return false;
    }
    fz_pixmap* src = nullptr;
    bool ok = false;
    fz_var(src);
    fz_try(ctx) {
        src = dm_pb_decode_image(ctx, image);
        if (!src || !src->samples || src->w < 8 || src->h < 8) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "empty image for photo-rect probe");
        }
        fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
        fz_colorspace* rgb = fz_device_rgb(ctx);
        int components = fz_colorspace_n(ctx, cs);
        DmPbPageStats st = dm_pb_estimate_page_stats(ctx, src, cs, rgb, components);
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, 0.92f, true);
        if (dm_pb_should_seek_photo_rects(st.satRatio, st.chromaRatio, st.paperRatio, st.lumVar, st.borderPaperRatio,
                                          &analysis)) {
            DmPbPhotoRect rects[kDmPbMaxPhotoRects] = {};
            int nRects = dm_pb_find_photo_rects(ctx, src, rects, kDmPbMaxPhotoRects, nullptr);
            i64 imgArea = (i64)src->w * src->h;
            for (int i = 0; i < nRects; i++) {
                i64 a = (i64)(rects[i].x1 - rects[i].x0) * (rects[i].y1 - rects[i].y0);
                if (imgArea > 0 && a * 20 >= imgArea) {
                    ok = true;
                    break;
                }
            }
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
    }
    fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

void PdfDarkModeAppendImagePhotoSkipDevRects(fz_context* ctx, fz_image* image, const RectF& imgOnPage,
                                             const fz_matrix& ctm, Vec<Rect>& outSkip) {
    if (!ctx || !image || imgOnPage.IsEmpty()) {
        return;
    }
    fz_pixmap* src = nullptr;
    fz_var(src);
    fz_try(ctx) {
        src = dm_pb_decode_image(ctx, image);
        if (!src || !src->samples || src->w < 8 || src->h < 8) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "empty image for photo skip rects");
        }
        fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
        fz_colorspace* rgb = fz_device_rgb(ctx);
        int components = fz_colorspace_n(ctx, cs);
        DmPbPageStats st = dm_pb_estimate_page_stats(ctx, src, cs, rgb, components);
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, 0.92f, true);
        if (dm_pb_should_seek_photo_rects(st.satRatio, st.chromaRatio, st.paperRatio, st.lumVar, st.borderPaperRatio,
                                          &analysis)) {
            DmPbPhotoRect rects[kDmPbMaxPhotoRects] = {};
            int nRects = dm_pb_find_photo_rects(ctx, src, rects, kDmPbMaxPhotoRects, nullptr);
            int pw = src->w;
            int ph = src->h;
            for (int i = 0; i < nRects; i++) {
                float fx0 = (float)rects[i].x0 / (float)pw;
                float fy0 = (float)rects[i].y0 / (float)ph;
                float fx1 = (float)rects[i].x1 / (float)pw;
                float fy1 = (float)rects[i].y1 / (float)ph;
                fz_rect pageR = fz_make_rect(imgOnPage.x + fx0 * imgOnPage.dx, imgOnPage.y + fy0 * imgOnPage.dy,
                                             imgOnPage.x + fx1 * imgOnPage.dx, imgOnPage.y + fy1 * imgOnPage.dy);
                fz_irect dev = fz_round_rect(fz_transform_rect(pageR, ctm));
                int dx = dev.x1 - dev.x0;
                int dy = dev.y1 - dev.y0;
                if (dx >= 8 && dy >= 8) {
                    outSkip.Append(Rect(dev.x0, dev.y0, dx, dy));
                }
            }
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
    }
    fz_catch(ctx) {
        fz_rethrow_if(ctx, FZ_ERROR_TRYLATER);
        fz_rethrow_if(ctx, FZ_ERROR_ABORT);
        fz_warn(ctx, "photo skip rects failed: %s", fz_caught_message(ctx));
    }
}

// RAZ / Adobe Image Conversion pages often place several photos (color + B&W portrait)
// side-by-side — or overlapping — in one full-bleed pixmap. Keeping only the largest
// dense block left secondary portraits in the "margin" path (ink/paper → look negative).
// Scan each dense Y-band in horizontal slices so a portrait hanging below a color photo
// still gets its own rect (axis-aligned union would swallow caption text into "preserve").
static int dm_pb_find_photo_rects(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* outRects, int maxRects,
                                  const float* lumPlane) {
    if (!ctx || !pix || !pix->samples || !outRects || maxRects <= 0 || pix->w < 8 || pix->h < 8) {
        return 0;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int w = pix->w;
    int h = pix->h;
    int step = w > 800 ? 4 : (w > 400 ? 2 : 1);

    float* ownedLum = nullptr;
    const float* lum = lumPlane;
    if (!lum) {
        ownedLum = dm_pb_build_lum_plane(ctx, pix, cs, rgb, components);
        lum = ownedLum;
    }

    float* rowDense = AllocArray<float>(h);
    if (!rowDense) {
        free(ownedLum);
        return 0;
    }
    for (int y = 0; y < h; y++) {
        int dense = 0;
        int samples = 0;
        for (int x = 0; x < w; x += step) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            samples++;
            if (dm_pb_is_dense_content_rgb(r, g, b, lum, w, h, x, y)) {
                dense++;
            }
        }
        rowDense[y] = samples > 0 ? (float)dense / (float)samples : 0.f;
    }

    const float kDense = 0.22f;
    const int minBandH = h / 10;
    const int minSliceH = h / 16;
    const int minRunW = w / 12;
    int pad = (w < 200 || h < 200) ? 1 : 2;
    int nRects = 0;

    auto appendRect = [&](int x0, int y0, int x1, int y1, bool sparse = false) {
        int rx0 = x0 + pad;
        int ry0 = y0 + pad;
        int rx1 = x1 - pad;
        int ry1 = y1 - pad;
        if (rx1 <= rx0 + 4 || ry1 <= ry0 + 4 || nRects >= maxRects) {
            return;
        }
        // Merge into an existing rect when heavily overlapping (same photo, adjacent slices).
        for (int i = 0; i < nRects; i++) {
            DmPbPhotoRect& r = outRects[i];
            int ox0 = rx0 > r.x0 ? rx0 : r.x0;
            int oy0 = ry0 > r.y0 ? ry0 : r.y0;
            int ox1 = rx1 < r.x1 ? rx1 : r.x1;
            int oy1 = ry1 < r.y1 ? ry1 : r.y1;
            if (ox1 > ox0 && oy1 > oy0) {
                i64 ov = (i64)(ox1 - ox0) * (oy1 - oy0);
                i64 aNew = (i64)(rx1 - rx0) * (ry1 - ry0);
                i64 aOld = (i64)(r.x1 - r.x0) * (r.y1 - r.y0);
                if (ov * 5 >= aNew * 2 || ov * 5 >= aOld * 2) {
                    if (rx0 < r.x0) {
                        r.x0 = rx0;
                    }
                    if (ry0 < r.y0) {
                        r.y0 = ry0;
                    }
                    if (rx1 > r.x1) {
                        r.x1 = rx1;
                    }
                    if (ry1 > r.y1) {
                        r.y1 = ry1;
                    }
                    return;
                }
            }
        }
        outRects[nRects].x0 = rx0;
        outRects[nRects].y0 = ry0;
        outRects[nRects].x1 = rx1;
        outRects[nRects].y1 = ry1;
        outRects[nRects].sparse = sparse;
        nRects++;
    };

    int runY0 = -1;
    for (int y = 0; y <= h; y++) {
        bool on = y < h && rowDense[y] >= kDense;
        if (on && runY0 < 0) {
            runY0 = y;
        } else if (!on && runY0 >= 0) {
            int bandY0 = runY0;
            int bandY1 = y;
            runY0 = -1;
            if (bandY1 - bandY0 < minBandH) {
                continue;
            }

            int bandH = bandY1 - bandY0;
            int nSlices = bandH >= minSliceH * 3 ? 3 : (bandH >= minSliceH * 2 ? 2 : 1);
            int bandRectStart = nRects;
            for (int s = 0; s < nSlices && nRects < maxRects; s++) {
                int sliceY0 = bandY0 + (bandH * s) / nSlices;
                int sliceY1 = bandY0 + (bandH * (s + 1)) / nSlices;
                if (sliceY1 - sliceY0 < 4) {
                    continue;
                }

                float* colDense = AllocArray<float>(w);
                if (!colDense) {
                    continue;
                }
                for (int x = 0; x < w; x++) {
                    int dense = 0;
                    int samples = 0;
                    for (int yy = sliceY0; yy < sliceY1; yy += step) {
                        float r, g, b;
                        dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, yy, &r, &g, &b);
                        samples++;
                        if (dm_pb_is_dense_content_rgb(r, g, b, lum, w, h, x, yy)) {
                            dense++;
                        }
                    }
                    colDense[x] = samples > 0 ? (float)dense / (float)samples : 0.f;
                }

                int runX0 = -1;
                for (int x = 0; x <= w; x++) {
                    bool xOn = x < w && colDense[x] >= kDense;
                    if (xOn && runX0 < 0) {
                        runX0 = x;
                    } else if (!xOn && runX0 >= 0) {
                        int x0 = runX0;
                        int x1 = x;
                        runX0 = -1;
                        if (x1 - x0 < minRunW) {
                            continue;
                        }
                        appendRect(x0, sliceY0, x1, sliceY1);
                        if (nRects >= maxRects) {
                            break;
                        }
                    }
                }
                free(colDense);
            }
            // Same dense band, several horizontal slices: keep one bbox. Stacked wide
            // slices are otherwise refused by the caption-merge skip, so a light sky in
            // the top-left of the photo (Prehistoric Trade p.3) stays outside every rect
            // and remaps to theme background.
            if (nRects > bandRectStart + 1) {
                int ux0 = outRects[bandRectStart].x0;
                int uy0 = outRects[bandRectStart].y0;
                int ux1 = outRects[bandRectStart].x1;
                int uy1 = outRects[bandRectStart].y1;
                for (int i = bandRectStart + 1; i < nRects; i++) {
                    if (outRects[i].x0 < ux0) {
                        ux0 = outRects[i].x0;
                    }
                    if (outRects[i].y0 < uy0) {
                        uy0 = outRects[i].y0;
                    }
                    if (outRects[i].x1 > ux1) {
                        ux1 = outRects[i].x1;
                    }
                    if (outRects[i].y1 > uy1) {
                        uy1 = outRects[i].y1;
                    }
                }
                outRects[bandRectStart].x0 = ux0;
                outRects[bandRectStart].y0 = uy0;
                outRects[bandRectStart].x1 = ux1;
                outRects[bandRectStart].y1 = uy1;
                nRects = bandRectStart + 1;
            }
        }
    }

    // Sparse inset drawings (Giant Insects Meganeura): full-page row density stays
    // below kDense because most of the row is paper/text, so the art never becomes a
    // band. Search each half-page with a lower threshold; text lines still fail minBandH.
    const float kDenseSide = 0.10f;
    const int maxSideH = h * 55 / 100;
    auto harvestSide = [&](int xA, int xB) {
        if (xB - xA < minRunW || nRects >= maxRects) {
            return;
        }
        int sideY0 = -1;
        for (int y = 0; y <= h; y++) {
            bool on = false;
            if (y < h) {
                int dense = 0;
                int samples = 0;
                for (int x = xA; x < xB; x += step) {
                    float r, g, b;
                    dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
                    samples++;
                    if (dm_pb_is_dense_content_rgb(r, g, b, lum, w, h, x, y)) {
                        dense++;
                    }
                }
                on = samples > 0 && (float)dense / (float)samples >= kDenseSide;
            }
            if (on && sideY0 < 0) {
                sideY0 = y;
            } else if (!on && sideY0 >= 0) {
                int y0 = sideY0;
                int y1 = y;
                sideY0 = -1;
                int bandH = y1 - y0;
                if (bandH < minBandH || bandH > maxSideH || nRects >= maxRects) {
                    continue;
                }
                float* colDense = AllocArray<float>(w);
                if (!colDense) {
                    continue;
                }
                for (int x = xA; x < xB; x++) {
                    int dense = 0;
                    int samples = 0;
                    for (int yy = y0; yy < y1; yy += step) {
                        float r, g, b;
                        dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, yy, &r, &g, &b);
                        samples++;
                        if (dm_pb_is_dense_content_rgb(r, g, b, lum, w, h, x, yy)) {
                            dense++;
                        }
                    }
                    colDense[x] = samples > 0 ? (float)dense / (float)samples : 0.f;
                }
                int runX0 = -1;
                for (int x = xA; x <= xB; x++) {
                    bool xOn = x < xB && colDense[x] >= kDenseSide;
                    if (xOn && runX0 < 0) {
                        runX0 = x;
                    } else if (!xOn && runX0 >= 0) {
                        int x0 = runX0;
                        int x1 = x;
                        runX0 = -1;
                        if (x1 - x0 < minRunW) {
                            continue;
                        }
                        appendRect(x0, y0, x1, y1, true);
                        if (nRects >= maxRects) {
                            break;
                        }
                    }
                }
                free(colDense);
            }
        }
    };
    harvestSide(0, w / 2);
    harvestSide(w / 2, w);

    free(rowDense);

    // Grow each rect vertically using only its own columns so a portrait that
    // continues below the page-wide dense band stays fully protected.
    for (int i = 0; i < nRects; i++) {
        DmPbPhotoRect& r = outRects[i];
        if (r.sparse) {
            continue;
        }
        float* colRowDense = AllocArray<float>(h);
        if (!colRowDense) {
            break;
        }
        for (int yy = 0; yy < h; yy++) {
            int dense = 0;
            int samples = 0;
            for (int xx = r.x0; xx < r.x1; xx += step) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, xx, yy, &rv, &gv, &bv);
                samples++;
                if (dm_pb_is_dense_content_rgb(rv, gv, bv, lum, w, h, xx, yy)) {
                    dense++;
                }
            }
            colRowDense[yy] = samples > 0 ? (float)dense / (float)samples : 0.f;
        }
        int bestOv = 0;
        int pick0 = r.y0;
        int pick1 = r.y1;
        int ry = -1;
        for (int yy = 0; yy <= h; yy++) {
            bool yOn = yy < h && colRowDense[yy] >= kDense;
            if (yOn && ry < 0) {
                ry = yy;
            } else if (!yOn && ry >= 0) {
                int yA = ry;
                int yB = yy;
                ry = -1;
                if (yB - yA < minBandH / 2) {
                    continue;
                }
                int ov0 = yA > r.y0 ? yA : r.y0;
                int ov1 = yB < r.y1 ? yB : r.y1;
                int ov = ov1 > ov0 ? ov1 - ov0 : 0;
                if (ov > bestOv || (ov == bestOv && (yB - yA) > (pick1 - pick0))) {
                    bestOv = ov;
                    pick0 = yA;
                    pick1 = yB;
                }
            }
        }
        free(colRowDense);
        if (bestOv > 0) {
            // Cap grow: RAZ pages with a top photo + body text have dense ink columns that
            // otherwise stretch the photo rect over the whole article (Ella Fitzgerald p.10).
            // Preserved black glyphs on remapped paper look like hollow white outlines.
            const int maxGrowY = h / 18;
            int origY0 = r.y0;
            int origY1 = r.y1;
            if (pick0 < origY0 - maxGrowY) {
                pick0 = origY0 - maxGrowY;
            }
            if (pick1 > origY1 + maxGrowY) {
                pick1 = origY1 + maxGrowY;
            }
            if (pick0 < 0) {
                pick0 = 0;
            }
            if (pick1 > h) {
                pick1 = h;
            }
            r.y0 = pick0 + pad;
            r.y1 = pick1 - pad;
            if (r.y1 <= r.y0 + 4) {
                r.y1 = r.y0 + 5;
            }
        }
    }

    // White prison bars / frames / gutters register as "paper" and split one photo into
    // several rects (Historic Peacemakers Mandela). Mid-face then falls in the gap and
    // gets ink/paper remapped (blue-gray skin). Bridge thin gaps between neighbors.
    {
        const int maxGapX = w / 10;
        const int maxGapY = h / 18;
        bool merged = true;
        while (merged && nRects > 1) {
            merged = false;
            for (int i = 0; i < nRects && !merged; i++) {
                for (int j = i + 1; j < nRects; j++) {
                    DmPbPhotoRect& a = outRects[i];
                    DmPbPhotoRect& b = outRects[j];
                    int ovY0 = a.y0 > b.y0 ? a.y0 : b.y0;
                    int ovY1 = a.y1 < b.y1 ? a.y1 : b.y1;
                    int ovX0 = a.x0 > b.x0 ? a.x0 : b.x0;
                    int ovX1 = a.x1 < b.x1 ? a.x1 : b.x1;
                    int ovY = ovY1 > ovY0 ? ovY1 - ovY0 : 0;
                    int ovX = ovX1 > ovX0 ? ovX1 - ovX0 : 0;
                    int hA = a.y1 - a.y0;
                    int hB = b.y1 - b.y0;
                    int wA = a.x1 - a.x0;
                    int wB = b.x1 - b.x0;
                    int shortH = hA < hB ? hA : hB;
                    int shortW = wA < wB ? wA : wB;
                    int gapX = 0;
                    if (a.x1 < b.x0) {
                        gapX = b.x0 - a.x1;
                    } else if (b.x1 < a.x0) {
                        gapX = a.x0 - b.x1;
                    }
                    int gapY = 0;
                    if (a.y1 < b.y0) {
                        gapY = b.y0 - a.y1;
                    } else if (b.y1 < a.y0) {
                        gapY = a.y0 - b.y1;
                    }
                    bool closeX = gapX <= maxGapX && shortH > 0 && ovY * 5 >= shortH * 2;
                    bool closeY = gapY <= maxGapY && shortW > 0 && ovX * 5 >= shortW * 2;
                    // Stacked full-width bands are photo+caption slices, not Mandela gutters —
                    // merging them swallows body text into the preserve path.
                    bool eitherWide = wA * 5 >= w * 3 || wB * 5 >= w * 3;
                    if (closeY && eitherWide) {
                        continue;
                    }
                    if (!closeX && !closeY) {
                        continue;
                    }
                    int uniX0 = a.x0 < b.x0 ? a.x0 : b.x0;
                    int uniY0 = a.y0 < b.y0 ? a.y0 : b.y0;
                    int uniX1 = a.x1 > b.x1 ? a.x1 : b.x1;
                    int uniY1 = a.y1 > b.y1 ? a.y1 : b.y1;
                    // Refuse page-sized unions (text columns caught between photos).
                    if ((i64)(uniX1 - uniX0) * (uniY1 - uniY0) * 5 > (i64)w * h * 2) {
                        continue;
                    }
                    a.x0 = uniX0;
                    a.y0 = uniY0;
                    a.x1 = uniX1;
                    a.y1 = uniY1;
                    a.sparse = a.sparse && b.sparse;
                    for (int k = j; k < nRects - 1; k++) {
                        outRects[k] = outRects[k + 1];
                    }
                    nRects--;
                    merged = true;
                    break;
                }
            }
        }
    }

    // Drop speech-bubble / callout clusters: lots of white paper in the *interior*
    // of the bbox. Real photos (and portraits with a white mat) have paper on the rim.
    int kept = 0;
    for (int i = 0; i < nRects; i++) {
        DmPbPhotoRect r = outRects[i];
        int rw = r.x1 - r.x0;
        int rh = r.y1 - r.y0;
        int ix0 = r.x0 + rw * 15 / 100;
        int iy0 = r.y0 + rh * 15 / 100;
        int ix1 = r.x1 - rw * 15 / 100;
        int iy1 = r.y1 - rh * 15 / 100;
        if (ix1 - ix0 >= 4 && iy1 - iy0 >= 4) {
            int samples = 0;
            int paperHits = 0;
            int textureHits = 0;
            int chromaHits = 0;
            u32 tonalMask = 0;
            float lumSum = 0.f;
            int insetStep = (ix1 - ix0) > 40 ? 2 : 1;
            for (int y = iy0; y < iy1; y += insetStep) {
                for (int x = ix0; x < ix1; x += insetStep) {
                    float rv, gv, bv;
                    dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                    samples++;
                    if (dm_pb_is_paper_rgb(rv, gv, bv)) {
                        paperHits++;
                    }
                    float maxC = rv > gv ? (rv > bv ? rv : bv) : (gv > bv ? gv : bv);
                    float minC = rv < gv ? (rv < bv ? rv : bv) : (gv < bv ? gv : bv);
                    if (maxC - minC >= 0.06f) {
                        chromaHits++;
                    }
                    float pixelLum = 0.2126f * rv + 0.7152f * gv + 0.0722f * bv;
                    lumSum += pixelLum;
                    int tonalBin = (int)(pixelLum * 31.f + 0.5f);
                    tonalBin = tonalBin < 0 ? 0 : (tonalBin > 31 ? 31 : tonalBin);
                    tonalMask |= 1u << tonalBin;
                    float lv = dm_pb_lum_var_3x3(lumPlane, w, h, x, y);
                    if (dm_pb_rgb_is_photo_texture(rv, gv, bv, lv)) {
                        textureHits++;
                    }
                }
            }
            float insetPaper = samples > 0 ? (float)paperHits / (float)samples : 0.f;
            float textureRatio = samples > 0 ? (float)textureHits / (float)samples : 0.f;
            float chromaRatio = samples > 0 ? (float)chromaHits / (float)samples : 0.f;
            float meanLum = samples > 0 ? lumSum / (float)samples : 0.f;
            int tonalBins = 0;
            for (u32 bits = tonalMask; bits; bits >>= 1) {
                tonalBins += (int)(bits & 1u);
            }
            // Sparse line art (Meganeura: tex 0.25, chroma 0.33) never trips the callout
            // gate; only mis-harvested text columns do (insP 0.85, tex 0.01) — drop them.
            if (PdfDarkModeV2PhotoRectIsCalloutCluster(insetPaper, textureRatio, tonalBins, chromaRatio)) {
                continue;
            }
            if (!r.sparse &&
                PdfDarkModeV2PhotoRectIsLightIllustrationWash(insetPaper, textureRatio, meanLum, chromaRatio)) {
                continue;
            }
        }
        outRects[kept++] = r;
    }
    nRects = kept;

    // Sparse line art is harvested by its dense trunk only; faint strokes (Meganeura's
    // pale wings) fall outside and would get remapped into invisibility. Grow sparse
    // rects over any adjacent rows/columns that still contain faint non-paper content.
    for (int i = 0; i < nRects; i++) {
        DmPbPhotoRect& r = outRects[i];
        if (!r.sparse) {
            continue;
        }
        auto colHasInk = [&](int x, int yA, int yB) -> bool {
            int ink = 0;
            for (int y = yA; y < yB; y += 2) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                if (!dm_pb_is_paper_rgb(rv, gv, bv)) {
                    ink++;
                }
            }
            return ink * 2 >= (yB - yA) / 100 + 2;
        };
        auto rowHasInk = [&](int y, int xA, int xB) -> bool {
            int ink = 0;
            for (int x = xA; x < xB; x += 2) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                if (!dm_pb_is_paper_rgb(rv, gv, bv)) {
                    ink++;
                }
            }
            return ink * 2 >= (xB - xA) / 100 + 2;
        };
        const int capX = w / 4;
        const int capY = h / 4;
        int miss = 0;
        for (int x = r.x0 - 1; x >= 0 && x >= r.x0 - capX && miss < 6; x--) {
            if (colHasInk(x, r.y0, r.y1)) {
                r.x0 = x;
                miss = 0;
            } else {
                miss++;
            }
        }
        miss = 0;
        for (int x = r.x1; x < w && x <= r.x1 + capX && miss < 6; x++) {
            if (colHasInk(x, r.y0, r.y1)) {
                r.x1 = x + 1;
                miss = 0;
            } else {
                miss++;
            }
        }
        miss = 0;
        for (int y = r.y0 - 1; y >= 0 && y >= r.y0 - capY && miss < 6; y--) {
            if (rowHasInk(y, r.x0, r.x1)) {
                r.y0 = y;
                miss = 0;
            } else {
                miss++;
            }
        }
        miss = 0;
        for (int y = r.y1; y < h && y <= r.y1 + capY && miss < 6; y++) {
            if (rowHasInk(y, r.x0, r.x1)) {
                r.y1 = y + 1;
                miss = 0;
            } else {
                miss++;
            }
        }
    }

    // Color titles on paper (RAZ SPRAK p.2) share a dense Y-band with the illustration.
    // Protecting that union keeps the black type as photo (black fill + white mat halo).
    int trimmedN = 0;
    for (int i = 0; i < nRects; i++) {
        DmPbPhotoRect r = outRects[i];
        if (r.sparse) {
            outRects[trimmedN++] = r;
            continue;
        }
        int x0 = r.x0 < 0 ? 0 : r.x0;
        int y0 = r.y0 < 0 ? 0 : r.y0;
        int x1 = r.x1 > w ? w : r.x1;
        int y1 = r.y1 > h ? h : r.y1;
        auto inkPaperRow = [&](int y) -> bool {
            if (y < 0 || y >= h) {
                return false;
            }
            int n = 0, chromaN = 0, midN = 0, paperN = 0;
            int stepX = (x1 - x0) > 80 ? 4 : 2;
            for (int x = x0; x < x1; x += stepX) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                n++;
                float maxC = rv > gv ? (rv > bv ? rv : bv) : (gv > bv ? gv : bv);
                float minC = rv < gv ? (rv < bv ? rv : bv) : (gv < bv ? gv : bv);
                float lum = 0.2126f * rv + 0.7152f * gv + 0.0722f * bv;
                if (maxC - minC >= 0.12f) {
                    chromaN++;
                } else if (lum > 0.88f) {
                    paperN++;
                } else if (lum > 0.38f) {
                    midN++;
                }
            }
            if (n <= 0) {
                return false;
            }
            return PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper((float)chromaN / (float)n, (float)midN / (float)n,
                                                                (float)paperN / (float)n);
        };
        while (y0 < y1 && inkPaperRow(y0)) {
            y0++;
        }
        while (y1 > y0 && inkPaperRow(y1 - 1)) {
            y1--;
        }
        auto inkPaperCol = [&](int x) -> bool {
            if (x < 0 || x >= w) {
                return false;
            }
            int n = 0, chromaN = 0, midN = 0, paperN = 0;
            int stepY = (y1 - y0) > 80 ? 4 : 2;
            for (int y = y0; y < y1; y += stepY) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                n++;
                float maxC = rv > gv ? (rv > bv ? rv : bv) : (gv > bv ? gv : bv);
                float minC = rv < gv ? (rv < bv ? rv : bv) : (gv < bv ? gv : bv);
                float lum = 0.2126f * rv + 0.7152f * gv + 0.0722f * bv;
                if (maxC - minC >= 0.12f) {
                    chromaN++;
                } else if (lum > 0.88f) {
                    paperN++;
                } else if (lum > 0.38f) {
                    midN++;
                }
            }
            if (n <= 0) {
                return false;
            }
            return PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper((float)chromaN / (float)n, (float)midN / (float)n,
                                                                (float)paperN / (float)n);
        };
        // Genetics at Work p.18: dense Y-band merged the inset photo with the text
        // column. The photo's JPEG-white right edge stayed protected → bright hairline.
        while (x0 < x1 && inkPaperCol(x0)) {
            x0++;
        }
        while (x1 > x0 && inkPaperCol(x1 - 1)) {
            x1--;
        }
        if (y1 - y0 < 24 || x1 - x0 < 24) {
            continue;
        }
        r.x0 = x0;
        r.x1 = x1;
        r.y0 = y0;
        r.y1 = y1;
        outRects[trimmedN++] = r;
    }
    nRects = trimmedN;

    free(ownedLum);
    return nRects;
}

static DmPbPageStats dm_pb_estimate_page_stats(fz_context* ctx, fz_pixmap* src, fz_colorspace* cs, fz_colorspace* rgb,
                                               int components) {
    DmPbPageStats st;
    if (!ctx || !src || !src->samples) {
        return st;
    }
    int w = src->w;
    int h = src->h;
    int paperSamples = 0;
    int paperHits = 0;
    int borderSamples = 0;
    int borderPaperHits = 0;
    int satHits = 0;
    int chromaHits = 0;
    int redInkHits = 0;
    float lumSum = 0.f;
    float lumSqSum = 0.f;
    int estStepX = w > 64 ? w / 64 : 1;
    int estStepY = h > 64 ? h / 64 : 1;
    for (int y = 0; y < h; y += estStepY) {
        for (int x = 0; x < w; x += estStepX) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            paperSamples++;
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            lumSum += lum;
            lumSqSum += lum * lum;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            if (chroma < 0.10f && lum > 0.72f) {
                paperHits++;
            }
            int borderDx = w / 16;
            int borderDy = h / 16;
            if (x < borderDx || x >= w - borderDx || y < borderDy || y >= h - borderDy) {
                borderSamples++;
                if (dm_pb_is_paper_rgb(r, g, b)) {
                    borderPaperHits++;
                }
            }
            if (chroma >= 0.12f) {
                satHits++;
            }
            if (chroma >= 0.06f) {
                chromaHits++;
            }
            // 红头 / 印章: red well above green/blue. Peach cream paper (r≈g) stays out.
            if (chroma >= 0.11f && r >= g + 0.15f && r >= b + 0.15f) {
                redInkHits++;
            }
        }
    }
    if (paperSamples <= 0) {
        return st;
    }
    st.paperRatio = (float)paperHits / (float)paperSamples;
    st.borderPaperRatio = borderSamples > 0 ? (float)borderPaperHits / (float)borderSamples : 0.f;
    st.satRatio = (float)satHits / (float)paperSamples;
    st.chromaRatio = (float)chromaHits / (float)paperSamples;
    st.redInkRatio = (float)redInkHits / (float)paperSamples;
    float lumMean = lumSum / (float)paperSamples;
    st.lumVar = lumSqSum / (float)paperSamples - lumMean * lumMean;
    return st;
}

// Color pages use chroma to find photo blocks. B&W portraits (RAZ Abraham Lincoln etc.) have
// satRatio < 0.04 so they need an extra path — but paper-heavy 连环画 line art must never get
// partial photo-rect protect (leaves white rectangular patches on an otherwise inverted page).
static bool dm_pb_should_seek_photo_rects(float satRatio, float chromaRatio, float paperRatio, float lumVar,
                                          float borderPaperRatio, const DarkImageAnalysis* imgAnalysis) {
    // Inset B&W portrait on a paper-heavy RAZ page (Historic Peacemakers Betty Williams):
    // the photo is small so full-page lumVar stays modest (~0.04). 128px thumbs often look
    // like 连环画 / text scans and would veto seeking below — ApplySharp then remaps the
    // portrait into a photographic negative. Detect the paper+island pattern first.
    if (satRatio < 0.05f && chromaRatio < 0.08f && paperRatio >= 0.75f && paperRatio < 0.96f && lumVar >= 0.032f &&
        lumVar <= 0.080f) {
        return true;
    }

    // 连环画 / woodblock B&W: uniform ink+paper remap only — never partial rects.
    // Exception: full-pixmap stats can show photographic variance while the 128px analysis
    // thumb collapses (RAZ text+photo pages). Trust high full-page lumVar instead.
    if (imgAnalysis && PdfDarkModeFeaturesLookLikeBwLineArtScan(imgAnalysis->features)) {
        // Also allow modest variance on very paper-heavy pages (small inset portraits).
        bool allow =
            PdfDarkModeFullResStatsLookLikeInsetPhotoOnPaper(paperRatio, satRatio, chromaRatio, lumVar) ||
            (satRatio < 0.08f && paperRatio < 0.92f && (lumVar >= 0.055f || (paperRatio >= 0.75f && lumVar >= 0.035f)));
        if (!allow) {
            return false;
        }
    }
    // Aged / sepia scan paper (FreePic2Pdf 连环画): many pixels pass chroma>=0.06 from the
    // yellow cast while few reach sat>=0.12. Color picture books keep sat closer to chroma.
    // Require real chroma — pure B&W (chroma==sat==0) must not hit this gate, or RAZ
    // portraits with white caption margins never seek photo rects when analysis is absent.
    // Strong photographic variance (historical sepia prints) is not aged line-art paper.
    if (paperRatio >= 0.55f && satRatio < 0.22f && chromaRatio >= 0.06f && chromaRatio >= satRatio * 2.0f &&
        lumVar >= 0.018f && lumVar <= 0.070f) {
        // Keep seeking when analysis says real photo; otherwise treat as aged line-art paper.
        // Exception: RAZ text+inset photo pages (Ella Fitzgerald p.5 Benny Carter) often have
        // mild paper chroma and a crushed 128px thumb that fails LookLikePhoto — full-page
        // lumVar still shows the portrait. Blocking seek here Okular-inverts the photo.
        bool photographicVariance = satRatio < 0.08f && lumVar >= 0.055f && paperRatio < 0.92f;
        // Pastel RAZ art (Lunch at School: sat 0.11, chroma 0.23) matches the cream
        // chroma/sat ratio, but the border is blank white paper. 红楼梦 keeps a
        // printed border (p.7 borderPaper 0.02, p.56 0.31) and must still skip rects.
        bool whiteMarginColorPage = borderPaperRatio >= 0.70f && satRatio >= 0.08f;
        if (!photographicVariance && !whiteMarginColorPage &&
            (!imgAnalysis || (!PdfDarkModeFeaturesLookLikeGrayscalePhoto(imgAnalysis->features) &&
                              !PdfDarkModeFeaturesLookLikePhoto(imgAnalysis->features)))) {
            return false;
        }
    }
    if (satRatio >= 0.04f) {
        return true;
    }
    if (imgAnalysis) {
        const DarkImageFeatures& f = imgAnalysis->features;
        if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) || PdfDarkModeFeaturesLookLikePhoto(f)) {
            return true;
        }
    }
    // Sepia / warm historical photos (RAZ Lincoln p.14 Tad): chroma without much sat.
    // Analysis thumbs often crush lumVar and fail LookLikeGrayscalePhoto / LookLikePhoto.
    if (satRatio < 0.08f && chromaRatio >= 0.10f && lumVar >= 0.040f && paperRatio < 0.92f) {
        return true;
    }
    // B&W portrait / text+photo pages: use full-pixmap lumVar (not crushed 128px analysis).
    // Paper can exceed 0.80 on oval-portrait pages (Mary Todd) with large white margins.
    if (satRatio < 0.04f && chromaRatio < 0.10f && lumVar >= 0.035f && paperRatio < 0.92f) {
        return true;
    }
    // Fallback: midtone-heavy B&W photo vs paper-heavy line-art scan (连环画).
    if (satRatio < 0.08f && chromaRatio < 0.10f && lumVar >= 0.014f && paperRatio < 0.52f) {
        return true;
    }
    return false;
}

static bool dm_pb_point_in_photo_rects(int x, int y, const DmPbPhotoRect* rects, int nRects) {
    for (int i = 0; i < nRects; i++) {
        if (x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 && y < rects[i].y1) {
            return true;
        }
    }
    return false;
}

static bool dm_pb_point_in_framed_rect(int x, int y, const DmPbPhotoRect* rects, int nRects) {
    for (int i = 0; i < nRects; i++) {
        if (rects[i].framed && x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 && y < rects[i].y1) {
            return true;
        }
    }
    return false;
}

static bool dm_pb_point_in_sparse_rect(int x, int y, const DmPbPhotoRect* rects, int nRects) {
    for (int i = 0; i < nRects; i++) {
        if (rects[i].sparse && x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 && y < rects[i].y1) {
            return true;
        }
    }
    return false;
}

static bool dm_pb_frame_ink(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return lum < 0.62f && (maxC - minC) < 0.22f;
}

static bool dm_pb_frame_margin(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return lum > 0.85f && (maxC - minC) < 0.12f;
}

// First-to-last dark pixels on a row. Small gaps where the drawing touches the
// stroke still count, as long as most of that span is ink.
static bool dm_pb_frame_row_span(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb, int components,
                                 int y, int* outA, int* outB) {
    int w = pix->w;
    int a = -1;
    int b = -1;
    int darkN = 0;
    for (int x = 0; x < w; x++) {
        float r, g, bch;
        dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &bch);
        if (!dm_pb_frame_ink(r, g, bch)) {
            continue;
        }
        if (a < 0) {
            a = x;
        }
        b = x + 1;
        darkN++;
    }
    if (a < 0 || b - a < w * 45 / 100) {
        return false;
    }
    if (darkN * 100 < (b - a) * 75) {
        return false;
    }
    *outA = a;
    *outB = b;
    return true;
}

static float dm_pb_frame_paper_frac(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb,
                                    int components, int x0, int x1, int y0, int y1, int vertical) {
    int n = 0;
    int hit = 0;
    int w = pix->w;
    int h = pix->h;
    if (vertical) {
        int x = x0;
        if (x < 0 || x >= w) {
            return 0.f;
        }
        for (int y = y0; y < y1; y += 2) {
            if (y < 0 || y >= h) {
                continue;
            }
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            n++;
            if (dm_pb_frame_margin(r, g, b)) {
                hit++;
            }
        }
    } else {
        int y = y0;
        if (y < 0 || y >= h) {
            return 0.f;
        }
        for (int x = x0; x < x1; x += 2) {
            if (x < 0 || x >= w) {
                continue;
            }
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            n++;
            if (dm_pb_frame_margin(r, g, b)) {
                hit++;
            }
        }
    }
    return n > 0 ? (float)hit / (float)n : 0.f;
}

static float dm_pb_frame_ink_frac_col(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb,
                                      int components, int x, int y0, int y1) {
    int n = 0;
    int hit = 0;
    for (int y = y0; y < y1; y += 2) {
        float r, g, b;
        dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
        n++;
        if (dm_pb_frame_ink(r, g, b)) {
            hit++;
        }
    }
    return n > 0 ? (float)hit / (float)n : 0.f;
}

// A thin black rectangle with white paper outside and a color drawing inside.
// Text boxes and cream line-art panels fail the interior color check.
static bool dm_pb_find_ink_frame(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* out) {
    if (!ctx || !pix || !pix->samples || !out || pix->w < 40 || pix->h < 40) {
        return false;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int w = pix->w;
    int h = pix->h;

    // Outermost stroke. A broken run still counts when most of the span is ink
    // and the paper outside the line is white (cookie page: the drawing cuts the line).
    int topY = -1;
    int topA = 0;
    int topB = 0;
    for (int y = 6; y < h * 55 / 100; y++) {
        int a, b;
        if (!dm_pb_frame_row_span(ctx, pix, cs, rgb, components, y, &a, &b)) {
            continue;
        }
        if (dm_pb_frame_paper_frac(ctx, pix, cs, rgb, components, a, b, y - 3, y - 3, 0) < 0.80f) {
            continue;
        }
        topY = y;
        topA = a;
        topB = b;
        break;
    }
    int botY = -1;
    int botA = 0;
    int botB = 0;
    for (int y = h - 7; y > h * 45 / 100; y--) {
        int a, b;
        if (!dm_pb_frame_row_span(ctx, pix, cs, rgb, components, y, &a, &b)) {
            continue;
        }
        if (dm_pb_frame_paper_frac(ctx, pix, cs, rgb, components, a, b, y + 3, y + 3, 0) < 0.80f) {
            continue;
        }
        botY = y;
        botA = a;
        botB = b;
        break;
    }
    if (topY < 0 || botY < 0 || botY - topY < h / 6) {
        return false;
    }

    int leftX = -1;
    for (int x = 4; x < w * 40 / 100; x++) {
        if (dm_pb_frame_ink_frac_col(ctx, pix, cs, rgb, components, x, topY, botY) < 0.62f) {
            continue;
        }
        if (dm_pb_frame_paper_frac(ctx, pix, cs, rgb, components, x - 3, x - 3, topY, botY, 1) < 0.75f) {
            continue;
        }
        leftX = x;
        break;
    }
    int rightX = -1;
    for (int x = w - 5; x > w * 60 / 100; x--) {
        if (dm_pb_frame_ink_frac_col(ctx, pix, cs, rgb, components, x, topY, botY) < 0.62f) {
            continue;
        }
        if (dm_pb_frame_paper_frac(ctx, pix, cs, rgb, components, x + 3, x + 3, topY, botY, 1) < 0.75f) {
            continue;
        }
        rightX = x;
        break;
    }
    if (leftX < 0 || rightX < 0 || rightX - leftX < w / 4) {
        return false;
    }
    if (topA > leftX + 12 || topB < rightX - 12 || botA > leftX + 12 || botB < rightX - 12) {
        return false;
    }

    int x0 = leftX;
    int y0 = topY;
    int x1 = rightX + 1;
    int y1 = botY + 1;
    int samples = 0;
    int colorN = 0;
    int paperN = 0;
    for (int y = y0 + 2; y < y1 - 2; y += 4) {
        for (int x = x0 + 2; x < x1 - 2; x += 4) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            samples++;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (chroma >= 0.15f) {
                colorN++;
            }
            if (chroma < 0.08f && lum > 0.88f) {
                paperN++;
            }
        }
    }
    if (samples < 16) {
        return false;
    }
    float colorFrac = (float)colorN / (float)samples;
    float paperFrac = (float)paperN / (float)samples;
    // Color drawing, not a ruled text box and not cream line art.
    // Pastel pages in this book sit near 0.16; text boxes are mostly paper.
    if (colorFrac < 0.14f || paperFrac >= 0.50f) {
        return false;
    }
    out->x0 = x0;
    out->y0 = y0;
    out->x1 = x1;
    out->y1 = y1;
    out->sparse = false;
    out->framed = true;
    return true;
}

static int dm_pb_append_ink_frame(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* rects, int nRects, int maxRects) {
    if (!rects || maxRects <= 0) {
        return nRects;
    }
    DmPbPhotoRect frame;
    if (!dm_pb_find_ink_frame(ctx, pix, &frame)) {
        return nRects;
    }
    if (nRects >= maxRects) {
        nRects = maxRects - 1;
    }
    rects[nRects] = frame;
    return nRects + 1;
}

// Text that overlaps a photo rect sits on the flooded white mat, not on the photo.
// Dilate the mat mask so glyphs (and their antialiasing) inside the halo get the
// normal ink/paper treatment instead of being "protected" as photo pixels.
// Keep this small: a 12px box dilation cut rectangular notches into oval portraits.
static u8* dm_pb_build_mat_halo(const u8* edgeWhiteMask, int w, int h) {
    if (!edgeWhiteMask) {
        return nullptr;
    }
    const int kHaloR = 3;
    u8* tmpH = AllocArray<u8>(w * h);
    u8* matHalo = AllocArray<u8>(w * h);
    if (!tmpH || !matHalo) {
        free(tmpH);
        free(matHalo);
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        const u8* srcRow = edgeWhiteMask + y * w;
        u8* dstRow = tmpH + y * w;
        int cnt = 0;
        for (int x = 0; x < w + kHaloR; x++) {
            if (x < w && srcRow[x]) {
                cnt++;
            }
            int xo = x - 2 * kHaloR - 1;
            if (xo >= 0 && srcRow[xo]) {
                cnt--;
            }
            int xc = x - kHaloR;
            if (xc >= 0 && xc < w) {
                dstRow[xc] = cnt > 0 ? 1 : 0;
            }
        }
    }
    for (int x = 0; x < w; x++) {
        int cnt = 0;
        for (int y = 0; y < h + kHaloR; y++) {
            if (y < h && tmpH[y * w + x]) {
                cnt++;
            }
            int yo = y - 2 * kHaloR - 1;
            if (yo >= 0 && tmpH[yo * w + x]) {
                cnt--;
            }
            int yc = y - kHaloR;
            if (yc >= 0 && yc < h) {
                matHalo[yc * w + x] = cnt > 0 ? 1 : 0;
            }
        }
    }
    free(tmpH);
    return matHalo;
}

// Per-tile ratio of strict flat paper. Text overlapping a sparse line-art rect sits in
// tiles that are mostly paper; drawing strokes (pale wings) sit in tiles that are not.
static float* dm_pb_build_tile_paper_ratio(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb,
                                           int components, int w, int h, int tile) {
    int tw = (w + tile - 1) / tile;
    int th = (h + tile - 1) / tile;
    float* out = AllocArray<float>(tw * th);
    if (!out) {
        return nullptr;
    }
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            int x0 = tx * tile;
            int y0 = ty * tile;
            int x1 = x0 + tile > w ? w : x0 + tile;
            int y1 = y0 + tile > h ? h : y0 + tile;
            int paper = 0;
            int samples = 0;
            for (int y = y0; y < y1; y += 2) {
                for (int x = x0; x < x1; x += 2) {
                    float rv, gv, bv;
                    dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                    samples++;
                    if (dm_pb_is_photo_rect_margin_paper_rgb(rv, gv, bv)) {
                        paper++;
                    }
                }
            }
            out[ty * tw + tx] = samples > 0 ? (float)paper / (float)samples : 0.f;
        }
    }
    return out;
}

// Bilinear sample of a per-tile map at pixel resolution. Hard per-tile lookups leave
// visible 24px square steps where a decision flips at a tile border.
static float dm_pb_sample_tile_map(const float* map, int tilesW, int tilesH, int tile, int x, int y) {
    float fx = ((float)x + 0.5f) / (float)tile - 0.5f;
    float fy = ((float)y + 0.5f) / (float)tile - 0.5f;
    int tx0 = (int)fx;
    int ty0 = (int)fy;
    if (fx < 0) {
        tx0 = 0;
        fx = 0;
    }
    if (fy < 0) {
        ty0 = 0;
        fy = 0;
    }
    int tx1 = tx0 + 1 < tilesW ? tx0 + 1 : tilesW - 1;
    int ty1 = ty0 + 1 < tilesH ? ty0 + 1 : tilesH - 1;
    if (tx0 > tilesW - 1) {
        tx0 = tilesW - 1;
    }
    if (ty0 > tilesH - 1) {
        ty0 = tilesH - 1;
    }
    float ax = fx - (float)tx0;
    float ay = fy - (float)ty0;
    float v00 = map[ty0 * tilesW + tx0];
    float v10 = map[ty0 * tilesW + tx1];
    float v01 = map[ty1 * tilesW + tx0];
    float v11 = map[ty1 * tilesW + tx1];
    float top = v00 + (v10 - v00) * ax;
    float bot = v01 + (v11 - v01) * ax;
    return top + (bot - top) * ay;
}

// Dark 24px tiles inside thick black title strokes look like inverted panels, so a
// mean-lum skip leaves white outlines around black interiors. A tile that sees paper
// on opposite sides within a few steps is a glyph stroke, not a dark callout fill.
static bool dm_pb_tile_is_glyph_stroke(const float* tileLum, int tilesW, int tilesH, int tx, int ty) {
    if (!tileLum || tilesW <= 0 || tilesH <= 0) {
        return false;
    }
    auto lumAt = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= tilesW || y >= tilesH) {
            return 1.f;
        }
        return tileLum[y * tilesW + x];
    };
    auto distPaper = [&](int dx, int dy) -> int {
        for (int s = 1; s <= 3; s++) {
            if (lumAt(tx + s * dx, ty + s * dy) >= 0.70f) {
                return s;
            }
        }
        return 4;
    };
    int wH = distPaper(-1, 0) + distPaper(1, 0);
    int wV = distPaper(0, -1) + distPaper(0, 1);
    int minW = wH < wV ? wH : wV;
    return minW <= 3;
}

fz_pixmap* PdfDarkModeProcessPictureBookPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette,
                                               const DarkImageAnalysis* imgAnalysis) {
    if (!ctx || !src || !src->samples) {
        return src;
    }
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = src->n;
    int w = src->w;
    int h = src->h;
    int stride = src->stride;

    // Always sharp dark paper + light text for picture books / RAZ. Soft-cream notebooks
    // are routed separately via PdfDarkModeProcessSoftCreamPixmap (classifier SoftCream).
    DmPbPageStats st = dm_pb_estimate_page_stats(ctx, src, cs, rgb, components);
    float paperRatio = st.paperRatio;
    float satRatio = st.satRatio;
    float chromaRatio = st.chromaRatio;
    float lumVar = st.lumVar;
    float redInkRatio = st.redInkRatio;
    bool officeDivert =
        PdfDarkModeFullResStatsLookLikeOfficeScanForGovPaper(paperRatio, satRatio, chromaRatio, lumVar, redInkRatio) &&
        !PdfDarkModeFullResStatsLookLikeInsetPhotoOnPaper(paperRatio, satRatio, chromaRatio, lumVar) &&
        !PdfDarkModeFullResStatsLookLikeColorIllustrationNotLineArt(satRatio, chromaRatio);
    if (officeDivert) {
        DarkModePalette govPalette = PdfDarkModeGovernmentPaperPalette(palette);
        return PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, govPalette);
    }

    if (PdfDarkModeV2ShouldPreserveFullBleedPhoto(st.borderPaperRatio, satRatio, chromaRatio, lumVar)) {
        fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
        fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);
        return dst;
    }

    float* lumPlane = dm_pb_build_lum_plane(ctx, src, cs, rgb, components);
    float* localVar = dm_pb_build_local_var_plane(lumPlane, w, h);

    DmPbPhotoRect photoRects[kDmPbMaxPhotoRects] = {};
    int nPhotoRects = 0;
    // Colorful pages + B&W documentary portraits: protect photo rects. B&W ink lines (连环画)
    // register as dense via luminance contrast but have no color — full-page remap only.
    if (dm_pb_should_seek_photo_rects(satRatio, chromaRatio, paperRatio, lumVar, st.borderPaperRatio, imgAnalysis)) {
        nPhotoRects = dm_pb_find_photo_rects(ctx, src, photoRects, kDmPbMaxPhotoRects, lumPlane);
    }
    // Black frame: the drawing inside stays original. Flood stays outside the stroke.
    nPhotoRects = dm_pb_append_ink_frame(ctx, src, photoRects, nPhotoRects, kDmPbMaxPhotoRects);
    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);

    // True photo page with little flat white mat and no photo rects: keep original
    // (avoids crushing specular highlights). Pages with white portrait frames have
    // higher paperRatio and still need ink/paper remapping below.
    bool keepOriginal =
        nPhotoRects == 0 && paperRatio < 0.18f && imgAnalysis &&
        (imgAnalysis->kind == DarkImageKind::Photo || PdfDarkModeFeaturesLookLikePhoto(imgAnalysis->features) ||
         PdfDarkModeFeaturesLookLikeGrayscalePhoto(imgAnalysis->features)) &&
        !PdfDarkModeFeaturesLookLikeBwLineArtScan(imgAnalysis->features);
    bool crushGhosts = PdfDarkModeV2ShouldCrushMrcBackgroundGhosts(paperRatio);
    if (keepOriginal) {
        free(localVar);
        free(lumPlane);
        return dst;
    }

    u8* edgeWhiteMask = nullptr;
    if (nPhotoRects > 0) {
        edgeWhiteMask = dm_pb_build_edge_connected_margin_mask(ctx, src, cs, rgb, components, w, h, photoRects,
                                                               nPhotoRects, localVar);
    }
    u8* matHalo = dm_pb_build_mat_halo(edgeWhiteMask, w, h);
    const int kPaperTile = 24;
    int paperTilesW = (w + kPaperTile - 1) / kPaperTile;
    int paperTilesH = (h + kPaperTile - 1) / kPaperTile;
    // Always build: color-studio pages need paper-tile gates so dark hair is not
    // SharpDocument-whitened (Getting Dressed). Sparse glyph detection uses it too.
    float* tilePaper = dm_pb_build_tile_paper_ratio(ctx, src, cs, rgb, components, w, h, kPaperTile);

    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);

    // Inverted panels (dark box, light text) are already dark-theme friendly.
    // Remapping them turns the light text into theme paper (dark-on-dark mush),
    // so skip pixels whose coarse neighborhood is predominantly dark.
    const int kDarkTile = 24;
    int tilesW = (w + kDarkTile - 1) / kDarkTile;
    int tilesH = (h + kDarkTile - 1) / kDarkTile;
    float* tileLum = lumPlane ? AllocArray<float>(tilesW * tilesH) : nullptr;
    if (tileLum) {
        for (int ty = 0; ty < tilesH; ty++) {
            for (int tx = 0; tx < tilesW; tx++) {
                int px0 = tx * kDarkTile;
                int py0 = ty * kDarkTile;
                int px1 = px0 + kDarkTile > w ? w : px0 + kDarkTile;
                int py1 = py0 + kDarkTile > h ? h : py0 + kDarkTile;
                float sum = 0.f;
                int cnt = 0;
                for (int yy = py0; yy < py1; yy++) {
                    for (int xx = px0; xx < px1; xx++) {
                        sum += lumPlane[yy * w + xx];
                        cnt++;
                    }
                }
                tileLum[ty * tilesW + tx] = cnt > 0 ? sum / (float)cnt : 1.f;
            }
        }
    }
    int nGlyphFill = 0;
    u8* tilePanel = nullptr;
    if (tileLum) {
        tilePanel = AllocArray<u8>(tilesW * tilesH);
        if (tilePanel) {
            for (int ty = 0; ty < tilesH; ty++) {
                for (int tx = 0; tx < tilesW; tx++) {
                    if (tileLum[ty * tilesW + tx] >= 0.35f) {
                        continue;
                    }
                    if (dm_pb_tile_is_glyph_stroke(tileLum, tilesW, tilesH, tx, ty)) {
                        nGlyphFill++;
                    } else {
                        tilePanel[ty * tilesW + tx] = 1;
                    }
                }
            }
            // Fill glyph interiors: dark tiles 8-connected to a stroke tile, then any
            // leftover dark island of a few tiles (JPEG holes inside letters).
            int* q = AllocArray<int>(tilesW * tilesH);
            int qn = 0;
            if (q) {
                if (nGlyphFill > 0) {
                    for (int i = 0; i < tilesW * tilesH; i++) {
                        if (tileLum[i] < 0.35f && !tilePanel[i]) {
                            q[qn++] = i;
                        }
                    }
                    for (int qi = 0; qi < qn; qi++) {
                        int i = q[qi];
                        int tx = i % tilesW;
                        int ty = i / tilesW;
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                if (dx == 0 && dy == 0) {
                                    continue;
                                }
                                int xx = tx + dx;
                                int yy = ty + dy;
                                if (xx < 0 || yy < 0 || xx >= tilesW || yy >= tilesH) {
                                    continue;
                                }
                                int ni = yy * tilesW + xx;
                                if (tileLum[ni] >= 0.35f || !tilePanel[ni]) {
                                    continue;
                                }
                                tilePanel[ni] = 0;
                                nGlyphFill++;
                                q[qn++] = ni;
                            }
                        }
                    }
                }
                u8* seen = AllocArray<u8>(tilesW * tilesH);
                if (seen) {
                    for (int i = 0; i < tilesW * tilesH; i++) {
                        if (!tilePanel[i] || seen[i]) {
                            continue;
                        }
                        qn = 0;
                        seen[i] = 1;
                        q[qn++] = i;
                        for (int qi = 0; qi < qn; qi++) {
                            int cur = q[qi];
                            int tx = cur % tilesW;
                            int ty = cur / tilesW;
                            for (int dy = -1; dy <= 1; dy++) {
                                for (int dx = -1; dx <= 1; dx++) {
                                    if (dx == 0 && dy == 0) {
                                        continue;
                                    }
                                    int xx = tx + dx;
                                    int yy = ty + dy;
                                    if (xx < 0 || yy < 0 || xx >= tilesW || yy >= tilesH) {
                                        continue;
                                    }
                                    int ni = yy * tilesW + xx;
                                    if (!tilePanel[ni] || seen[ni]) {
                                        continue;
                                    }
                                    seen[ni] = 1;
                                    q[qn++] = ni;
                                }
                            }
                        }
                        int minTy = tilesH;
                        int maxTy = 0;
                        for (int k = 0; k < qn; k++) {
                            int ty = q[k] / tilesW;
                            if (ty < minTy) {
                                minTy = ty;
                            }
                            if (ty > maxTy) {
                                maxTy = ty;
                            }
                        }
                        int compH = maxTy - minTy + 1;
                        // Solid display type (SPRAK slab title): one short ink island, not a
                        // dark callout with light text. Tiny JPEG holes stay on the qn<=6 path.
                        // Expanded fill must not swallow oval-portrait poles (those tiles sit
                        // in a photo rect and are mostly not flooded mat).
                        if (qn <= 6 || (qn <= 96 && compH <= 12)) {
                            bool fillIsland = qn <= 6;
                            if (qn > 6) {
                                int photoHits = 0;
                                int matHits = 0;
                                for (int k = 0; k < qn; k++) {
                                    int ptx = q[k] % tilesW;
                                    int pty = q[k] / tilesW;
                                    int px = ptx * kDarkTile + 8;
                                    int py = pty * kDarkTile + 8;
                                    if (px >= w) {
                                        px = w - 1;
                                    }
                                    if (py >= h) {
                                        py = h - 1;
                                    }
                                    if (!dm_pb_point_in_photo_rects(px, py, photoRects, nPhotoRects)) {
                                        continue;
                                    }
                                    photoHits++;
                                    if (edgeWhiteMask && edgeWhiteMask[py * w + px]) {
                                        matHits++;
                                    }
                                }
                                // SPRAK title is trimmed out of the photo rect. Oval hair is
                                // inside it with little mat at the tile center.
                                fillIsland = photoHits == 0 || matHits * 2 >= photoHits;
                            }
                            if (fillIsland) {
                                for (int k = 0; k < qn; k++) {
                                    tilePanel[q[k]] = 0;
                                    nGlyphFill++;
                                }
                            }
                        }
                    }
                    free(seen);
                }
                free(q);
            }
        }
    }

    for (int y = 0; y < h; y++) {
        unsigned char* row = dst->samples + y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + x * n;
            float r, g, b;
            if (fastRgb) {
                r = px[0] / 255.f;
                g = px[1] / 255.f;
                b = px[2] / 255.f;
            } else if (fastGray) {
                r = g = b = px[0] / 255.f;
            } else {
                float conv[FZ_MAX_COLORS] = {};
                float srcRgb[FZ_MAX_COLORS] = {};
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    conv[c] = px[c] / 255.f;
                }
                fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
                r = srcRgb[0];
                g = srcRgb[1];
                b = srcRgb[2];
            }
            bool sparseBgOnly = false;
            if (nPhotoRects > 0 && dm_pb_point_in_photo_rects(x, y, photoRects, nPhotoRects)) {
                // Closed black frame: original pixels, including light walls and white
                // objects. Do not flood or ink-remap anything inside the stroke.
                if (dm_pb_point_in_framed_rect(x, y, photoRects, nPhotoRects)) {
                    continue;
                }
                if (dm_pb_point_in_sparse_rect(x, y, photoRects, nPhotoRects)) {
                    // Sparse line art: swap flat paper for theme background, keep every
                    // stroke (pale wings, colored body) untouched — no white slab, and
                    // faint strokes stay visible on the dark page.
                    float lvS = localVar ? localVar[y * w + x] : 0.f;
                    if (!dm_pb_is_photo_rect_margin_paper_rgb(r, g, b) || dm_pb_rgb_is_photo_texture(r, g, b, lvS)) {
                        // Overlapping body text: dark low-chroma glyphs in tiles that are
                        // mostly flat paper — give them the normal ink treatment.
                        float maxS = r > g ? (r > b ? r : b) : (g > b ? g : b);
                        float minS = r < g ? (r < b ? r : b) : (g < b ? g : b);
                        float lumS = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        bool glyphOnPaper =
                            tilePaper && lumS < 0.60f && (maxS - minS) < 0.15f &&
                            dm_pb_sample_tile_map(tilePaper, paperTilesW, paperTilesH, kPaperTile, x, y) >= 0.50f;
                        if (!glyphOnPaper) {
                            continue;
                        }
                    } else {
                        sparseBgOnly = true;
                    }
                } else {
                    // Preserve photo interiors (including dark hair). Remap flat white mat
                    // — including flood pockets that read as a white "skullcap" on dark pages.
                    // White clothing on the subject sits on low-paper tiles and stays.
                    bool edgeMat = edgeWhiteMask && edgeWhiteMask[y * w + x];
                    float lvP = localVar ? localVar[y * w + x] : 0.f;
                    bool flatMat =
                        dm_pb_is_photo_rect_margin_paper_rgb(r, g, b) && !dm_pb_rgb_is_photo_texture(r, g, b, lvP);
                    float tilePr =
                        tilePaper ? dm_pb_sample_tile_map(tilePaper, paperTilesW, paperTilesH, kPaperTile, x, y) : 0.f;
                    bool matPocket = flatMat && matHalo && matHalo[y * w + x];
                    if (!edgeMat && !matPocket) {
                        bool haloTxt = matHalo && matHalo[y * w + x];
                        if (!haloTxt || !tilePaper) {
                            continue;
                        }
                        float lumH = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        float maxS = r > g ? (r > b ? r : b) : (g > b ? g : b);
                        float minS = r < g ? (r < b ? r : b) : (g < b ? g : b);
                        bool glyphOnPaper = lumH < 0.55f && (maxS - minS) < 0.14f && tilePr >= 0.55f;
                        if (!glyphOnPaper) {
                            continue;
                        }
                    }
                }
            }
            int tx = x / kDarkTile;
            int ty = y / kDarkTile;
            if (tx >= tilesW) {
                tx = tilesW - 1;
            }
            if (ty >= tilesH) {
                ty = tilesH - 1;
            }
            if (tilePanel && tilePanel[ty * tilesW + tx]) {
                continue;
            }
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            float srcLum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float nr = r, ng = g, nb = b;
            bool remap = false;
            if (sparseBgOnly) {
                nr = palette.bgR;
                ng = palette.bgG;
                nb = palette.bgB;
                remap = true;
            } else
                // Leftover MRC JPEG text under the JBIG2 mask: force theme paper before
                // photo-texture / SharpDocument can leave light-brown ghosts.
                if (crushGhosts && PdfDarkModeV2IsMrcBackgroundGhostPixel(srcLum, chroma)) {
                    nr = palette.bgR;
                    ng = palette.bgG;
                    nb = palette.bgB;
                    remap = true;
                } else {
                    float lv = localVar ? localVar[y * w + x] : 0.f;
                    if (dm_pb_rgb_is_photo_texture(r, g, b, lv)) {
                        continue;
                    }
                    // Dark low-chroma on a non-paper tile is photo (hair/eyes/shadows).
                    // SharpDocument would paint it theme-white. Body text sits on paper
                    // tiles (high tilePaper) and still remaps.
                    if (srcLum < 0.48f && chroma < 0.15f) {
                        float pr = tilePaper
                                       ? dm_pb_sample_tile_map(tilePaper, paperTilesW, paperTilesH, kPaperTile, x, y)
                                       : 1.f;
                        if (pr < 0.42f) {
                            continue;
                        }
                    }
                    if (ApplySharpDocumentInkPaper(r, g, b, palette, &nr, &ng, &nb)) {
                        remap = true;
                    }
                }
            if (!remap) {
                continue;
            }
            if (fastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (fastGray) {
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(lum * 255.f + 0.5f);
                px[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                float out[FZ_MAX_COLORS] = {nr, ng, nb};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, out, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[c] = (unsigned char)v;
                }
            }
        }
    }
    free(tilePanel);
    free(tilePaper);
    free(tileLum);
    free(matHalo);
    free(edgeWhiteMask);
    free(localVar);
    free(lumPlane);
    return dst;
}

// A color drawing or photo sitting on white paper, even when the page is mostly
// blank. The picture is the chromatic island; the white around it and the glyphs
// on that white are the margin. One rule for framed cartoons, inset photos, and
// captioned figures — no separate frame or "is it a photo" gate.
static int dm_pb_collect_picture_islands(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* out, int maxOut) {
    if (!ctx || !pix || !pix->samples || !out || maxOut <= 0 || pix->w < 32 || pix->h < 32) {
        return 0;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int w = pix->w;
    int h = pix->h;
    int step = (w > 500 || h > 500) ? 4 : 2;
    int cw = (w + step - 1) / step;
    int ch = (h + step - 1) / step;
    u8* mark = AllocArray<u8>((size_t)cw * ch);
    u8* seen = AllocArray<u8>((size_t)cw * ch);
    int* q = AllocArray<int>((size_t)cw * ch);
    if (!mark || !seen || !q) {
        free(mark);
        free(seen);
        free(q);
        return 0;
    }
    for (int cy = 0; cy < ch; cy++) {
        int y = cy * step;
        if (y >= h) {
            y = h - 1;
        }
        for (int cx = 0; cx < cw; cx++) {
            int x = cx * step;
            if (x >= w) {
                x = w - 1;
            }
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            if (maxC - minC >= 0.14f) {
                mark[cy * cw + cx] = 1;
            }
        }
    }

    struct IslandBox {
        int x0, y0, x1, y1, n;
    };
    IslandBox boxes[16];
    int nBoxes = 0;
    int minCount = (cw * ch) / 500;
    if (minCount < 6) {
        minCount = 6;
    }
    for (int i = 0; i < cw * ch; i++) {
        if (!mark[i] || seen[i]) {
            continue;
        }
        int qn = 0;
        int count = 0;
        int x0 = cw, y0 = ch, x1 = 0, y1 = 0;
        seen[i] = 1;
        q[qn++] = i;
        while (count < qn) {
            int cur = q[count++];
            int cx = cur % cw;
            int cy = cur / cw;
            if (cx < x0) {
                x0 = cx;
            }
            if (cy < y0) {
                y0 = cy;
            }
            if (cx + 1 > x1) {
                x1 = cx + 1;
            }
            if (cy + 1 > y1) {
                y1 = cy + 1;
            }
            const int dxs[4] = {-1, 1, 0, 0};
            const int dys[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; k++) {
                int nx = cx + dxs[k];
                int ny = cy + dys[k];
                if (nx < 0 || ny < 0 || nx >= cw || ny >= ch) {
                    continue;
                }
                int ni = ny * cw + nx;
                if (!mark[ni] || seen[ni]) {
                    continue;
                }
                seen[ni] = 1;
                q[qn++] = ni;
            }
        }
        if (qn < minCount || nBoxes >= 16) {
            continue;
        }
        boxes[nBoxes].x0 = x0 * step;
        boxes[nBoxes].y0 = y0 * step;
        boxes[nBoxes].x1 = x1 * step;
        boxes[nBoxes].y1 = y1 * step;
        if (boxes[nBoxes].x1 > w) {
            boxes[nBoxes].x1 = w;
        }
        if (boxes[nBoxes].y1 > h) {
            boxes[nBoxes].y1 = h;
        }
        boxes[nBoxes].n = qn;
        nBoxes++;
    }
    free(mark);
    free(seen);
    free(q);
    if (nBoxes <= 0) {
        return 0;
    }

    int parent[16];
    for (int i = 0; i < nBoxes; i++) {
        parent[i] = i;
    }
    auto findP = [&](int i) -> int {
        while (parent[i] != i) {
            i = parent[i];
        }
        return i;
    };
    for (int i = 0; i < nBoxes; i++) {
        for (int j = i + 1; j < nBoxes; j++) {
            int yGap = 0;
            if (boxes[i].y1 < boxes[j].y0) {
                yGap = boxes[j].y0 - boxes[i].y1;
            } else if (boxes[j].y1 < boxes[i].y0) {
                yGap = boxes[i].y0 - boxes[j].y1;
            }
            int xGap = 0;
            if (boxes[i].x1 < boxes[j].x0) {
                xGap = boxes[j].x0 - boxes[i].x1;
            } else if (boxes[j].x1 < boxes[i].x0) {
                xGap = boxes[i].x0 - boxes[j].x1;
            }
            bool sameBand = yGap <= h / 10 && xGap <= w / 3;
            bool sameStack = xGap <= w / 10 && yGap <= h / 5;
            if (!sameBand && !sameStack) {
                continue;
            }
            int a = findP(i);
            int b = findP(j);
            if (a != b) {
                parent[b] = a;
            }
        }
    }

    int nOut = 0;
    int pad = (w < h ? w : h) / 48;
    if (pad < 16) {
        pad = 16;
    }
    i64 pageArea = (i64)w * h;
    for (int i = 0; i < nBoxes && nOut < maxOut; i++) {
        if (findP(i) != i) {
            continue;
        }
        int x0 = boxes[i].x0;
        int y0 = boxes[i].y0;
        int x1 = boxes[i].x1;
        int y1 = boxes[i].y1;
        for (int j = 0; j < nBoxes; j++) {
            if (findP(j) != i) {
                continue;
            }
            if (boxes[j].x0 < x0) {
                x0 = boxes[j].x0;
            }
            if (boxes[j].y0 < y0) {
                y0 = boxes[j].y0;
            }
            if (boxes[j].x1 > x1) {
                x1 = boxes[j].x1;
            }
            if (boxes[j].y1 > y1) {
                y1 = boxes[j].y1;
            }
        }
        x0 -= pad;
        y0 -= pad;
        x1 += pad;
        y1 += pad;
        if (x0 < 0) {
            x0 = 0;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (x1 > w) {
            x1 = w;
        }
        if (y1 > h) {
            y1 = h;
        }
        i64 area = (i64)(x1 - x0) * (y1 - y0);
        if (area * 100 < pageArea * 2 || area * 100 > pageArea * 88) {
            continue;
        }
        out[nOut].x0 = x0;
        out[nOut].y0 = y0;
        out[nOut].x1 = x1;
        out[nOut].y1 = y1;
        out[nOut].sparse = false;
        // Open color region, not a sealed frame. Studio white inside still floods.
        out[nOut].framed = false;
        nOut++;
    }
    return nOut;
}

// Flat white that touches an open picture's border is the page or studio mat.
// Flood it. Do not cross a sealed ink frame, and do not swallow white that the
// frame (or the subject) has already cut off from that border.
static u8* dm_pb_build_island_mat_mask(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb,
                                       int components, const DmPbPhotoRect* rects, int nRects,
                                       const DmPbPhotoRect* frame) {
    if (!pix || !rects || nRects <= 0) {
        return nullptr;
    }
    int w = pix->w;
    int h = pix->h;
    u8* mask = AllocArray<u8>((size_t)w * h);
    u8* paper = AllocArray<u8>((size_t)w * h);
    if (!mask || !paper) {
        free(mask);
        free(paper);
        return nullptr;
    }
    auto inFrame = [&](int x, int y) -> bool {
        return frame && x >= frame->x0 && x < frame->x1 && y >= frame->y0 && y < frame->y1;
    };
    for (int i = 0; i < nRects; i++) {
        const DmPbPhotoRect& r = rects[i];
        if (r.framed) {
            continue;
        }
        int x0 = r.x0 < 0 ? 0 : r.x0;
        int y0 = r.y0 < 0 ? 0 : r.y0;
        int x1 = r.x1 > w ? w : r.x1;
        int y1 = r.y1 > h ? h : r.y1;
        if (x1 - x0 < 4 || y1 - y0 < 4) {
            continue;
        }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                if (inFrame(x, y)) {
                    continue;
                }
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                if (dm_pb_is_photo_rect_margin_paper_rgb(rv, gv, bv)) {
                    paper[y * w + x] = 1;
                }
            }
        }
        int* q = AllocArray<int>((size_t)(x1 - x0) * (y1 - y0));
        if (!q) {
            continue;
        }
        int qn = 0;
        auto trySeed = [&](int x, int y) {
            if (x < x0 || x >= x1 || y < y0 || y >= y1 || inFrame(x, y)) {
                return;
            }
            int idx = y * w + x;
            if (!paper[idx] || mask[idx]) {
                return;
            }
            mask[idx] = 1;
            q[qn++] = idx;
        };
        int shortSide = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
        int minRun = shortSide / 10;
        if (minRun < 28) {
            minRun = 28;
        }
        if (minRun > 80) {
            minRun = 80;
        }
        auto flushHoriz = [&](int y, int runEnd, int runLen) {
            if (runLen < minRun) {
                return;
            }
            for (int t = 0; t < runLen; t++) {
                trySeed(runEnd - runLen + t, y);
            }
        };
        auto flushVert = [&](int x, int runEnd, int runLen) {
            if (runLen < minRun) {
                return;
            }
            for (int t = 0; t < runLen; t++) {
                trySeed(x, runEnd - runLen + t);
            }
        };
        for (int edge = 0; edge < 2; edge++) {
            int y = edge == 0 ? y0 : y1 - 1;
            int run = 0;
            for (int x = x0; x <= x1; x++) {
                bool on = x < x1 && paper[y * w + x];
                if (on) {
                    run++;
                } else {
                    flushHoriz(y, x, run);
                    run = 0;
                }
            }
        }
        for (int edge = 0; edge < 2; edge++) {
            int x = edge == 0 ? x0 : x1 - 1;
            int run = 0;
            for (int y = y0; y <= y1; y++) {
                bool on = y < y1 && paper[y * w + x];
                if (on) {
                    run++;
                } else {
                    flushVert(x, y, run);
                    run = 0;
                }
            }
        }
        for (int qi = 0; qi < qn; qi++) {
            int idx = q[qi];
            int x = idx % w;
            int y = idx / w;
            const int nx[4] = {x - 1, x + 1, x, x};
            const int ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; k++) {
                int xx = nx[k];
                int yy = ny[k];
                if (xx < x0 || xx >= x1 || yy < y0 || yy >= y1 || inFrame(xx, yy)) {
                    continue;
                }
                int nidx = yy * w + xx;
                if (!paper[nidx] || mask[nidx]) {
                    continue;
                }
                mask[nidx] = 1;
                q[qn++] = nidx;
            }
        }
        free(q);
        for (int y = y0; y < y1; y++) {
            memset(paper + y * w + x0, 0, (size_t)(x1 - x0));
        }
    }
    free(paper);
    return mask;
}

static bool dm_pb_pixmap_has_picture_island(fz_context* ctx, fz_pixmap* pix) {
    DmPbPhotoRect rect;
    return dm_pb_collect_picture_islands(ctx, pix, &rect, 1) > 0;
}

// Keep chromatic picture islands in original colors. White that touches the
// island border is the studio mat and is knocked out. A sealed ink frame blocks
// that flood, so a drawing's interior stays. Text outside the island is restyled.
static fz_pixmap* dm_pb_process_picture_islands(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
    DmPbPhotoRect rects[kDmPbMaxPhotoRects];
    int nRects = dm_pb_collect_picture_islands(ctx, src, rects, kDmPbMaxPhotoRects);
    if (nRects <= 0) {
        return nullptr;
    }
    DmPbPhotoRect frame;
    bool hasFrame = dm_pb_find_ink_frame(ctx, src, &frame);
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = src->n;
    int w = src->w;
    int h = src->h;
    int stride = src->stride;
    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);
    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);
    u8* mat = dm_pb_build_island_mat_mask(ctx, src, cs, rgb, components, rects, nRects, hasFrame ? &frame : nullptr);
    int br = (int)(palette.bgR * 255.f + 0.5f);
    int bg = (int)(palette.bgG * 255.f + 0.5f);
    int bb = (int)(palette.bgB * 255.f + 0.5f);
    if (br < 0) {
        br = 0;
    }
    if (br > 255) {
        br = 255;
    }
    if (bg < 0) {
        bg = 0;
    }
    if (bg > 255) {
        bg = 255;
    }
    if (bb < 0) {
        bb = 0;
    }
    if (bb > 255) {
        bb = 255;
    }
    for (int y = 0; y < h; y++) {
        unsigned char* row = dst->samples + y * stride;
        for (int x = 0; x < w; x++) {
            bool inIsland = dm_pb_point_in_photo_rects(x, y, rects, nRects);
            bool sealed = hasFrame && x >= frame.x0 && x < frame.x1 && y >= frame.y0 && y < frame.y1;
            if (mat && mat[y * w + x] && !sealed) {
                unsigned char* px = row + x * n;
                if (fastRgb) {
                    px[0] = (unsigned char)br;
                    px[1] = (unsigned char)bg;
                    px[2] = (unsigned char)bb;
                } else if (fastGray) {
                    int v =
                        (int)((0.2126f * palette.bgR + 0.7152f * palette.bgG + 0.0722f * palette.bgB) * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[0] = (unsigned char)v;
                } else {
                    float outc[FZ_MAX_COLORS] = {palette.bgR, palette.bgG, palette.bgB};
                    float back[FZ_MAX_COLORS] = {};
                    fz_convert_color(ctx, rgb, outc, cs, back, cs, fz_default_color_params);
                    for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                        int v = (int)(back[c] * 255.f + 0.5f);
                        if (v < 0) {
                            v = 0;
                        }
                        if (v > 255) {
                            v = 255;
                        }
                        px[c] = (unsigned char)v;
                    }
                }
                continue;
            }
            if (inIsland || sealed) {
                continue;
            }
            unsigned char* px = row + x * n;
            float r, g, b;
            if (fastRgb) {
                r = px[0] / 255.f;
                g = px[1] / 255.f;
                b = px[2] / 255.f;
            } else if (fastGray) {
                r = g = b = px[0] / 255.f;
            } else {
                float conv[FZ_MAX_COLORS] = {};
                float srcRgb[FZ_MAX_COLORS] = {};
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    conv[c] = px[c] / 255.f;
                }
                fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
                r = srcRgb[0];
                g = srcRgb[1];
                b = srcRgb[2];
            }
            float nr, ng, nb;
            if (!ApplySharpDocumentInkPaper(r, g, b, palette, &nr, &ng, &nb)) {
                continue;
            }
            if (fastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (fastGray) {
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(lum * 255.f + 0.5f);
                px[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                float outc[FZ_MAX_COLORS] = {nr, ng, nb};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, outc, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[c] = (unsigned char)v;
                }
            }
        }
    }
    free(mat);
    return dst;
}

fz_pixmap* PdfDarkModeProcessV2FullPagePixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette,
                                              const DarkImageAnalysis* imgAnalysis) {
    if (!ctx || !src || !src->samples) {
        return nullptr;
    }
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = src->n;
    int w = src->w;
    int h = src->h;
    int stride = src->stride;
    struct V2FullPerf {
        LARGE_INTEGER t;
        int w;
        int h;
        const char* branch;
        V2FullPerf(int ww, int hh) : t(TimeGet()), w(ww), h(hh), branch("pixel") {}
        ~V2FullPerf() {
            if (PdfDarkModePagePerfOn()) {
                logf("page-perf v2-full %dx%d branch=%s %.1f ms\n", w, h, branch, TimeSinceInMs(t));
            }
        }
    } v2Perf(w, h);

    DmPbPageStats st = dm_pb_estimate_page_stats(ctx, src, cs, rgb, components);
    float paperRatio = st.paperRatio;
    float satRatio = st.satRatio;
    float chromaRatio = st.chromaRatio;
    float lumVar = st.lumVar;
    float redInkRatio = st.redInkRatio;
    // Photo on a page: flood the white margin and keep the picture. Line-art and
    // text-scan binarize below would flatten the photograph.
    bool textPage = paperRatio >= 0.70f && satRatio < 0.08f && lumVar < 0.040f;
    bool lineArtPage = satRatio < 0.06f && chromaRatio < 0.12f && paperRatio >= 0.45f && lumVar < 0.050f;
    // RAZ Getting Dressed pp.5–8: white studio mats crush page sat (~0.03) so the
    // line-art gate fired and later paths whitened dark hair. Real 连环画 rarely has
    // a solid paper border plus photographic midtone spread.
    if (PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(lineArtPage, paperRatio, lumVar, st.borderPaperRatio)) {
        lineArtPage = false;
    }
    if (!textPage && !lineArtPage && lumVar >= 0.018f && paperRatio >= 0.08f) {
        v2Perf.branch = "picturebook-early";
        return PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
    }
    // Mostly-white text scans with a localized color drawing never reach the photo
    // path above. Eyes or a small patch of color on a real photograph must not take
    // this branch: only those boxes would stay original and the rest of the picture
    // would be ink-remapped (white fox, snow).
    fz_pixmap* island = dm_pb_process_picture_islands(ctx, src, palette);
    if (island) {
        v2Perf.branch = "islands";
        return island;
    }

    bool yellow = PdfDarkModeFullResStatsLookLikeAgedYellowLineArtPage(paperRatio, satRatio, chromaRatio, lumVar,
                                                                       st.borderPaperRatio, redInkRatio);
    bool officeScan =
        PdfDarkModeFullResStatsLookLikeOfficeScanForGovPaper(paperRatio, satRatio, chromaRatio, lumVar, redInkRatio);
    if (yellow) {
        v2Perf.branch = "gov-yellow";
        DarkModePalette lineArtPalette = PdfDarkModeGovernmentPaperPalette(palette);
        return PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, lineArtPalette);
    }

    // Thumbnail line-art is for 连环画 / woodblock: one uniform paper/ink map.
    // RAZ text+inset photos crush to the same thumbnail. Color pages use full-res
    // sat/chroma; grayscale historical photos (Dust Bowl) use found photo islands.
    if (imgAnalysis && PdfDarkModeFeaturesLookLikeBwLineArtScan(imgAnalysis->features)) {
        bool insetPhoto = PdfDarkModeFullResStatsLookLikeInsetPhotoOnPaper(paperRatio, satRatio, chromaRatio, lumVar);
        bool seekGray =
            dm_pb_should_seek_photo_rects(satRatio, chromaRatio, paperRatio, lumVar, st.borderPaperRatio, imgAnalysis);
        // Small colorful photo on a very white page (Jazz Greats TOC gold trumpet):
        // full-page sat/lumVar are crushed by the paper so every gate above fails and the
        // page fell to gov-paper binarize (red/ink stencil). Real 连环画 / text scans have
        // sat == chroma == 0, so a little of both means a color object is present.
        bool seekColorInset = !seekGray && paperRatio >= 0.88f && satRatio >= 0.015f && chromaRatio >= 0.02f;
        int nPeek = 0;
        float largestCov = 0.f;
        if (seekGray || seekColorInset) {
            float* lumPeek = dm_pb_build_lum_plane(ctx, src, cs, rgb, components);
            DmPbPhotoRect peekRects[kDmPbMaxPhotoRects] = {};
            nPeek = dm_pb_find_photo_rects(ctx, src, peekRects, kDmPbMaxPhotoRects, lumPeek);
            float pageArea = (float)w * (float)h;
            for (int i = 0; i < nPeek; i++) {
                float a = (float)(peekRects[i].x1 - peekRects[i].x0) * (float)(peekRects[i].y1 - peekRects[i].y0);
                float cov = pageArea > 0.f ? a / pageArea : 0.f;
                if (cov > largestCov) {
                    largestCov = cov;
                }
            }
            free(lumPeek);
        }
        bool insetGray = PdfDarkModeFullResStatsLookLikeInsetGrayPhotoIslands(paperRatio, satRatio, chromaRatio, lumVar,
                                                                              nPeek, largestCov);
        // A found island of sane size on the white page is the color object itself.
        bool insetColor = seekColorInset && nPeek > 0 && largestCov >= 0.04f && largestCov <= 0.55f;
        bool colorArt = PdfDarkModeFullResStatsLookLikeColorIllustrationNotLineArt(satRatio, chromaRatio);
        if (!insetPhoto && !insetGray && !insetColor && !colorArt) {
            v2Perf.branch = "gov-lineart";
            DarkModePalette lineArtPalette = PdfDarkModeGovernmentPaperPalette(palette);
            return PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, lineArtPalette);
        }
        v2Perf.branch = "picturebook-lineart";
        return PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
    }

    bool thumbLooksTextScan =
        imgAnalysis && PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(imgAnalysis->features);
    bool textScanBinarize = false;
    if (thumbLooksTextScan &&
        PdfDarkModeFullResStatsAllowGovernmentPaperBinarize(imgAnalysis, paperRatio, satRatio, chromaRatio, lumVar)) {
        textScanBinarize = true;
    }
    if (textScanBinarize &&
        PdfDarkModeVetoGovernmentPaperBinarize(imgAnalysis, paperRatio, satRatio, chromaRatio, lumVar)) {
        textScanBinarize = false;
    }
    if (textScanBinarize) {
        v2Perf.branch = "gov-text";
        DarkModePalette govPalette = PdfDarkModeGovernmentPaperPalette(palette);
        return PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, govPalette);
    }

    if (officeScan) {
        v2Perf.branch = "gov-office";
        DarkModePalette govPalette = PdfDarkModeGovernmentPaperPalette(palette);
        return PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, govPalette);
    }

    // Full-bleed photos: show the original. A gray scene with a few colored
    // subjects fails the high-sat full-bleed gate and was Okular-inverted.
    if (PdfDarkModeV2ShouldKeepOriginalPhotograph(paperRatio, lumVar)) {
        v2Perf.branch = "keep-original";
        fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
        fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);
        return dst;
    }

    // Warm cream / peach paper pages (RAZ Telescopes Galileo callouts, etc.): paper chroma
    // without saturated ink. Picture-book steep remap turns cream into muddy grey-brown;
    // SoftCream keeps the wash (gentle paper softening) instead of ink/paper invert.
    // Cap paper below 公文 scans (Hangzhou p.2–4: paper≥0.93, JPEG chroma, not cream).
    if (paperRatio >= 0.55f && paperRatio < 0.90f && satRatio < 0.04f && chromaRatio >= 0.12f && chromaRatio < 0.42f &&
        lumVar < 0.055f) {
        v2Perf.branch = "softcream";
        return PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
    }

    // Color RAZ / comic pages (Word Smith, Adaptive Athletes, …): Okular invert on
    // low-chroma ink/skin inside illustrations looks like a negative even when some
    // saturated pixels are kept. Picture-book path protects whole photo/art rects and
    // only steep-remaps true paper/ink outside them.
    // Do not use chroma alone: warm cream paper is high-chroma / low-sat and must stay SoftCream.
    // Paper-heavy RAZ text pages (Ella Fitzgerald): also PictureBook — Okular on JPEG
    // anti-aliased glyphs leaves hollow speckled outlines.
    if (satRatio >= 0.08f || (paperRatio >= 0.50f && satRatio < 0.15f && lumVar < 0.052f)) {
        v2Perf.branch = "picturebook";
        return PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
    }

    float* lumPlane = dm_pb_build_lum_plane(ctx, src, cs, rgb, components);
    float* localVar = dm_pb_build_local_var_plane(lumPlane, w, h);

    DmPbPhotoRect photoRects[kDmPbMaxPhotoRects] = {};
    int nPhotoRects = 0;
    // Must pass analysis: B&W RAZ portraits with large white margins fail the null-analysis
    // paperRatio gates and otherwise get whole-page Okular invert (negative portraits).
    if (dm_pb_should_seek_photo_rects(satRatio, chromaRatio, paperRatio, lumVar, st.borderPaperRatio, imgAnalysis)) {
        nPhotoRects = dm_pb_find_photo_rects(ctx, src, photoRects, kDmPbMaxPhotoRects, lumPlane);
    }

    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);

    u8* edgeWhiteMask = nullptr;
    if (nPhotoRects > 0) {
        edgeWhiteMask = dm_pb_build_edge_connected_margin_mask(ctx, src, cs, rgb, components, w, h, photoRects,
                                                               nPhotoRects, localVar);
    }
    u8* matHalo = dm_pb_build_mat_halo(edgeWhiteMask, w, h);
    const int kPaperTile = 24;
    int paperTilesW = (w + kPaperTile - 1) / kPaperTile;
    int paperTilesH = (h + kPaperTile - 1) / kPaperTile;
    // Always build: color-studio pages need paper-tile gates so dark hair is not
    // SharpDocument-whitened (Getting Dressed). Sparse glyph detection uses it too.
    float* tilePaper = dm_pb_build_tile_paper_ratio(ctx, src, cs, rgb, components, w, h, kPaperTile);

    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);

    for (int y = 0; y < h; y++) {
        unsigned char* row = dst->samples + y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + x * n;
            float r, g, b;
            if (fastRgb) {
                r = px[0] / 255.f;
                g = px[1] / 255.f;
                b = px[2] / 255.f;
            } else if (fastGray) {
                r = g = b = px[0] / 255.f;
            } else {
                float conv[FZ_MAX_COLORS] = {};
                float srcRgb[FZ_MAX_COLORS] = {};
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    conv[c] = px[c] / 255.f;
                }
                fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
                r = srcRgb[0];
                g = srcRgb[1];
                b = srcRgb[2];
            }
            bool sparseBgOnly = false;
            if (nPhotoRects > 0 && dm_pb_point_in_photo_rects(x, y, photoRects, nPhotoRects)) {
                if (dm_pb_point_in_sparse_rect(x, y, photoRects, nPhotoRects)) {
                    // Sparse line art: flat paper -> theme background, strokes untouched.
                    float lvS = localVar ? localVar[y * w + x] : 0.f;
                    if (!dm_pb_is_photo_rect_margin_paper_rgb(r, g, b) || dm_pb_rgb_is_photo_texture(r, g, b, lvS)) {
                        // Overlapping body text: dark low-chroma glyphs in mostly-paper tiles.
                        float maxS = r > g ? (r > b ? r : b) : (g > b ? g : b);
                        float minS = r < g ? (r < b ? r : b) : (g < b ? g : b);
                        float lumS = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        bool glyphOnPaper =
                            tilePaper && lumS < 0.60f && (maxS - minS) < 0.15f &&
                            dm_pb_sample_tile_map(tilePaper, paperTilesW, paperTilesH, kPaperTile, x, y) >= 0.50f;
                        if (!glyphOnPaper) {
                            continue;
                        }
                    } else {
                        sparseBgOnly = true;
                    }
                } else {
                    // Same as PictureBook: keep photo interiors; remap mat pockets / edge mat.
                    bool edgeMat = edgeWhiteMask && edgeWhiteMask[y * w + x];
                    float lvP = localVar ? localVar[y * w + x] : 0.f;
                    bool flatMat =
                        dm_pb_is_photo_rect_margin_paper_rgb(r, g, b) && !dm_pb_rgb_is_photo_texture(r, g, b, lvP);
                    float tilePr =
                        tilePaper ? dm_pb_sample_tile_map(tilePaper, paperTilesW, paperTilesH, kPaperTile, x, y) : 0.f;
                    bool matPocket = flatMat && matHalo && matHalo[y * w + x];
                    if (!edgeMat && !matPocket) {
                        bool haloTxt = matHalo && matHalo[y * w + x];
                        if (!haloTxt || !tilePaper) {
                            continue;
                        }
                        float lumH = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                        float maxS = r > g ? (r > b ? r : b) : (g > b ? g : b);
                        float minS = r < g ? (r < b ? r : b) : (g < b ? g : b);
                        bool glyphOnPaper = lumH < 0.55f && (maxS - minS) < 0.14f && tilePr >= 0.55f;
                        if (!glyphOnPaper) {
                            continue;
                        }
                    }
                }
            }
            // Same narrow photo-texture veto as PictureBook (light fur / fabric JPEG grain).
            float lv = localVar ? localVar[y * w + x] : 0.f;
            if (!sparseBgOnly && dm_pb_rgb_is_photo_texture(r, g, b, lv)) {
                continue;
            }
            float srcLum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            if (!sparseBgOnly && srcLum < 0.48f && chroma < 0.15f) {
                float pr =
                    tilePaper ? dm_pb_sample_tile_map(tilePaper, paperTilesW, paperTilesH, kPaperTile, x, y) : 1.f;
                if (pr < 0.42f) {
                    continue;
                }
            }
            // Steep ink/paper for near-gray document pixels. Saturated pixels are
            // photograph color — leave them. Okular here turned gray photos into
            // red/blue false color.
            float nr = r, ng = g, nb = b;
            if (sparseBgOnly) {
                nr = palette.bgR;
                ng = palette.bgG;
                nb = palette.bgB;
            } else if (!ApplySharpDocumentInkPaper(r, g, b, palette, &nr, &ng, &nb)) {
                continue;
            }
            if (fastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (fastGray) {
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(lum * 255.f + 0.5f);
                px[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                float out[FZ_MAX_COLORS] = {nr, ng, nb};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, out, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[c] = (unsigned char)v;
                }
            }
        }
    }
    free(tilePaper);
    free(matHalo);
    free(edgeWhiteMask);
    free(localVar);
    free(lumPlane);
    return dst;
}

static bool dm_v2_is_white_mat_paper_rgb(float r, float g, float b, bool iconPastelMat, bool stripMat = false) {
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    // Include soft textbook drop-shadow grays (often ~0.82–0.95), not only pure white.
    if (chroma < 0.10f && lum > 0.82f) {
        return true;
    }
    // Warm cream JPEG/CMYK mats around Glencoe badges (R,G ahead of B).
    if (lum > 0.88f && chroma < 0.22f && r + 0.02f >= b && g + 0.02f >= b) {
        return true;
    }
    // Tiny atlas map chips: light cyan ocean / pale fills that frame the landmass.
    // Only for small icons — larger photos keep sky. Cream sidebar mats (SECTION tile)
    // need a slightly higher chroma cap than cool ocean chips.
    if (iconPastelMat && lum > 0.72f && chroma < 0.40f) {
        return true;
    }
    // Tall Guide-to-Reading wave: JPEG AA on the open (white) side is light cyan
    // against the blue stroke — not warm cream, so the checks above miss it and a
    // straight white/cyan column survives at the image’s right edge.
    if (stripMat && lum > 0.78f && chroma < 0.28f && b + 0.02f >= r) {
        return true;
    }
    return false;
}

// 0 = solid subject, 1 = pure paper. Used for soft (anti-aliased) alpha cutout.
static float dm_v2_paper_amount_rgb(float r, float g, float b, bool iconPastelMat, bool stripMat = false) {
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    auto clamp01 = [](float v) -> float {
        if (v < 0.f) {
            return 0.f;
        }
        if (v > 1.f) {
            return 1.f;
        }
        return v;
    };
    if (chroma < 0.12f && lum > 0.78f) {
        return clamp01((lum - 0.72f) / 0.26f);
    }
    if (lum > 0.86f && chroma < 0.24f && r + 0.02f >= b && g + 0.02f >= b) {
        return clamp01((lum - 0.80f) / 0.18f) * clamp01(1.f - (chroma - 0.08f) / 0.20f);
    }
    if (iconPastelMat && lum > 0.70f && chroma < 0.42f) {
        return clamp01((lum - 0.68f) / 0.28f) * clamp01(1.f - (chroma - 0.08f) / 0.38f);
    }
    if (stripMat && lum > 0.74f && chroma < 0.32f && b + 0.02f >= r) {
        return clamp01((lum - 0.70f) / 0.26f);
    }
    // JPEG AA fringe: light but not quite paper.
    if (lum > 0.80f && chroma < 0.22f) {
        return clamp01((lum - 0.80f) / 0.18f) * 0.65f;
    }
    return 0.f;
}

// Binary box dilate. tmp is w*h scratch.
static void dm_v2_box_dilate(const u8* src, u8* dst, int w, int h, int rad, u8* tmp) {
    int* pref = AllocArray<int>((size_t)w + 1);
    if (!pref) {
        memset(dst, 0, (size_t)w * h);
        return;
    }
    for (int y = 0; y < h; y++) {
        pref[0] = 0;
        const u8* row = src + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            pref[x + 1] = pref[x] + (row[x] ? 1 : 0);
        }
        u8* out = tmp + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int a = x - rad;
            if (a < 0) {
                a = 0;
            }
            int b = x + rad + 1;
            if (b > w) {
                b = w;
            }
            out[x] = (pref[b] - pref[a]) > 0 ? 1 : 0;
        }
    }
    free(pref);
    int* prefY = AllocArray<int>((size_t)h + 1);
    if (!prefY) {
        memset(dst, 0, (size_t)w * h);
        return;
    }
    for (int x = 0; x < w; x++) {
        prefY[0] = 0;
        for (int y = 0; y < h; y++) {
            prefY[y + 1] = prefY[y] + (tmp[(size_t)y * w + x] ? 1 : 0);
        }
        for (int y = 0; y < h; y++) {
            int a = y - rad;
            if (a < 0) {
                a = 0;
            }
            int b = y + rad + 1;
            if (b > h) {
                b = h;
            }
            dst[(size_t)y * w + x] = (prefY[b] - prefY[a]) > 0 ? 1 : 0;
        }
    }
    free(prefY);
}

static void dm_v2_box_erode(const u8* src, u8* dst, int w, int h, int rad, u8* tmp) {
    int* pref = AllocArray<int>((size_t)w + 1);
    if (!pref) {
        memset(dst, 0, (size_t)w * h);
        return;
    }
    for (int y = 0; y < h; y++) {
        pref[0] = 0;
        const u8* row = src + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            pref[x + 1] = pref[x] + (row[x] ? 1 : 0);
        }
        u8* out = tmp + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int a = x - rad;
            if (a < 0) {
                a = 0;
            }
            int b = x + rad + 1;
            if (b > w) {
                b = w;
            }
            out[x] = (pref[b] - pref[a]) == (b - a) ? 1 : 0;
        }
    }
    free(pref);
    int* prefY = AllocArray<int>((size_t)h + 1);
    if (!prefY) {
        memset(dst, 0, (size_t)w * h);
        return;
    }
    for (int x = 0; x < w; x++) {
        prefY[0] = 0;
        for (int y = 0; y < h; y++) {
            prefY[y + 1] = prefY[y] + (tmp[(size_t)y * w + x] ? 1 : 0);
        }
        for (int y = 0; y < h; y++) {
            int a = y - rad;
            if (a < 0) {
                a = 0;
            }
            int b = y + rad + 1;
            if (b > h) {
                b = h;
            }
            dst[(size_t)y * w + x] = (prefY[b] - prefY[a]) == (b - a) ? 1 : 0;
        }
    }
    free(prefY);
}

// Close the non-paper subject so light fur / white hats / highlights that sit
// inside the silhouette are not treated as mat. Grayscale zebras use a tighter
// radius; soft color studio cards (RAZ Getting Dressed) use a wider close so a
// white skullcap touching the mat is sealed by the face/hair before flood.
static void dm_v2_seal_subject(u8* paper, int w, int h, bool wideSeal) {
    int rad = (w < h ? w : h) / (wideSeal ? 40 : 60);
    if (wideSeal) {
        if (rad < 8) {
            rad = 8;
        }
        if (rad > 18) {
            rad = 18;
        }
    } else {
        if (rad < 6) {
            rad = 6;
        }
        if (rad > 10) {
            rad = 10;
        }
    }
    if (w < rad * 4 || h < rad * 4) {
        return;
    }
    size_t n = (size_t)w * h;
    u8* ink = AllocArray<u8>(n);
    u8* tmp = AllocArray<u8>(n);
    u8* dilated = AllocArray<u8>(n);
    u8* closed = AllocArray<u8>(n);
    if (!ink || !tmp || !dilated || !closed) {
        free(ink);
        free(tmp);
        free(dilated);
        free(closed);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        ink[i] = paper[i] ? 0 : 1;
    }
    dm_v2_box_dilate(ink, dilated, w, h, rad, tmp);
    dm_v2_box_erode(dilated, closed, w, h, rad, tmp);
    for (size_t i = 0; i < n; i++) {
        if (closed[i]) {
            paper[i] = 0;
        }
    }
    free(ink);
    free(tmp);
    free(dilated);
    free(closed);
}

// strictPaper: grayscale cutouts only mark near-pure white as mat (fur stays subject).
// sealSubject: morphological close of non-paper so interior whites are not floodable.
// wideSeal: larger close radius for soft color studio photos (white hat on rim).
static u8* dm_v2_build_border_edge_white_mask(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb,
                                              int components, int w, int h, int* outMasked, int* outPaperSides,
                                              bool strictPaper, bool sealSubject, bool wideSeal, bool iconPastelMat,
                                              bool stripMat = false) {
    if (outMasked) {
        *outMasked = 0;
    }
    if (outPaperSides) {
        *outPaperSides = 0;
    }
    if (!pix || !pix->samples || w < 4 || h < 4) {
        return nullptr;
    }
    u8* paper = AllocArray<u8>(w * h);
    u8* mask = AllocArray<u8>(w * h);
    if (!paper || !mask) {
        free(paper);
        free(mask);
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            bool isPaper = strictPaper ? ((maxC - minC) < 0.08f && lum > 0.94f)
                                       : dm_v2_is_white_mat_paper_rgb(r, g, b, iconPastelMat, stripMat);
            if (isPaper) {
                paper[y * w + x] = 1;
            }
        }
    }
    if (sealSubject) {
        dm_v2_seal_subject(paper, w, h, wideSeal);
    }

    auto sidePaperRatio = [&](int x0, int y0, int x1, int y1) -> float {
        if (x0 < 0) {
            x0 = 0;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (x1 > w) {
            x1 = w;
        }
        if (y1 > h) {
            y1 = h;
        }
        if (x1 <= x0 || y1 <= y0) {
            return 0.f;
        }
        int samples = 0, hits = 0;
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                samples++;
                if (paper[y * w + x]) {
                    hits++;
                }
            }
        }
        return samples > 0 ? (float)hits / (float)samples : 0.f;
    };
    const float kSide = 0.30f;
    // Band must be per-axis and clamped — wide+short images made h-band negative (AV).
    int bandX = w > 64 ? w / 32 : 2;
    int bandY = h > 64 ? h / 32 : 2;
    if (bandX < 1) {
        bandX = 1;
    }
    if (bandY < 1) {
        bandY = 1;
    }
    if (bandX > w) {
        bandX = w;
    }
    if (bandY > h) {
        bandY = h;
    }
    int paperSides = 0;
    if (sidePaperRatio(0, 0, w, bandY) >= kSide) {
        paperSides++; // top
    }
    if (sidePaperRatio(0, h - bandY, w, h) >= kSide) {
        paperSides++; // bottom
    }
    if (sidePaperRatio(0, 0, bandX, h) >= kSide) {
        paperSides++; // left
    }
    if (sidePaperRatio(w - bandX, 0, w, h) >= kSide) {
        paperSides++; // right
    }
    if (outPaperSides) {
        *outPaperSides = paperSides;
    }
    // No light border at all — skip flood. (L-shaped shadows may have only 1–2 sides.)
    if (paperSides < 1) {
        free(paper);
        free(mask);
        return nullptr;
    }

    int* q = AllocArray<int>(w * h);
    if (!q) {
        free(paper);
        free(mask);
        return nullptr;
    }
    int qn = 0;
    auto trySeed = [&](int x, int y) {
        if (x < 0 || x >= w || y < 0 || y >= h) {
            return;
        }
        int idx = y * w + x;
        if (!paper[idx] || mask[idx]) {
            return;
        }
        mask[idx] = 1;
        q[qn++] = idx;
    };
    for (int x = 0; x < w; x++) {
        trySeed(x, 0);
        trySeed(x, h - 1);
    }
    for (int y = 0; y < h; y++) {
        trySeed(0, y);
        trySeed(w - 1, y);
    }
    for (int qi = 0; qi < qn; qi++) {
        int idx = q[qi];
        int x = idx % w;
        int y = idx / w;
        const int nx[4] = {x - 1, x + 1, x, x};
        const int ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; k++) {
            int xx = nx[k];
            int yy = ny[k];
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) {
                continue;
            }
            int nidx = yy * w + xx;
            if (!paper[nidx] || mask[nidx]) {
                continue;
            }
            mask[nidx] = 1;
            q[qn++] = nidx;
        }
    }
    free(q);
    free(paper);
    // Tall wave strips: dilate flood 1px into remaining light cyan AA so a straight
    // column at the open edge does not survive beside the blue stroke.
    if (stripMat && qn > 0) {
        u8* dil = AllocArray<u8>(w * h);
        if (dil) {
            memcpy(dil, mask, (size_t)w * (size_t)h);
            int added = 0;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    if (mask[y * w + x]) {
                        continue;
                    }
                    bool adjFlood = false;
                    for (int dy = -1; dy <= 1 && !adjFlood; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx;
                            int yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
                                continue;
                            }
                            if (mask[yy * w + xx]) {
                                adjFlood = true;
                                break;
                            }
                        }
                    }
                    if (!adjFlood) {
                        continue;
                    }
                    float r, g, b;
                    dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
                    if (dm_v2_paper_amount_rgb(r, g, b, false, true) >= 0.35f) {
                        dil[y * w + x] = 1;
                        added++;
                    }
                }
            }
            memcpy(mask, dil, (size_t)w * (size_t)h);
            free(dil);
            if (outMasked) {
                *outMasked = qn + added;
            }
            return mask;
        }
    }
    if (outMasked) {
        *outMasked = qn;
    }
    return mask;
}

fz_pixmap* PdfDarkModeProcessV2SoftShadowPlatePixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
    if (!ctx || !src || !src->samples) {
        return nullptr;
    }
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = src->n;
    int w = src->w;
    int h = src->h;
    int stride = src->stride;
    // Thin wide shadow strips are common (e.g. 180×7 under a callout card).
    bool thinWide = (h >= 2 && h < 8 && w >= 24 && w >= h * 6) || (w >= 2 && w < 8 && h >= 24 && h >= w * 6);
    if ((!thinWide && (w < 8 || h < 8)) || (i64)w * (i64)h > (i64)1200 * 1200) {
        return nullptr;
    }

    int paperSamples = 0, satHits = 0, chromaHits = 0, inkHits = 0;
    float lumSum = 0.f;
    float lumSq = 0.f;
    int estStepX = w > 48 ? w / 48 : 1;
    int estStepY = h > 48 ? h / 48 : 1;
    for (int y = 0; y < h; y += estStepY) {
        for (int x = 0; x < w; x += estStepX) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            paperSamples++;
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            lumSum += lum;
            lumSq += lum * lum;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            if (chroma >= 0.12f) {
                satHits++;
            }
            if (chroma >= 0.06f) {
                chromaHits++;
            }
            if (lum < 0.22f) {
                inkHits++;
            }
        }
    }
    if (paperSamples <= 0) {
        return nullptr;
    }
    float satRatio = (float)satHits / (float)paperSamples;
    float chromaRatio = (float)chromaHits / (float)paperSamples;
    float inkRatio = (float)inkHits / (float)paperSamples;
    float meanLum = lumSum / (float)paperSamples;
    float lumVar = lumSq / (float)paperSamples - meanLum * meanLum;
    if (lumVar < 0.f) {
        lumVar = 0.f;
    }
    if (!PdfDarkModeV2LooksLikeSoftShadowPlate(satRatio, chromaRatio, inkRatio, meanLum, lumVar)) {
        return nullptr;
    }

    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_clear_pixmap_with_value(ctx, dst, 0);
    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);
    int br = (int)(palette.bgR * 255.f + 0.5f);
    int bg = (int)(palette.bgG * 255.f + 0.5f);
    int bb = (int)(palette.bgB * 255.f + 0.5f);
    if (br < 0) {
        br = 0;
    }
    if (br > 255) {
        br = 255;
    }
    if (bg < 0) {
        bg = 0;
    }
    if (bg > 255) {
        bg = 255;
    }
    if (bb < 0) {
        bb = 0;
    }
    if (bb > 255) {
        bb = 255;
    }
    for (int y = 0; y < h; y++) {
        unsigned char* row = dst->samples + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + x * n;
            if (fastRgb) {
                px[0] = (unsigned char)br;
                px[1] = (unsigned char)bg;
                px[2] = (unsigned char)bb;
            } else if (fastGray) {
                int v = (int)((0.2126f * palette.bgR + 0.7152f * palette.bgG + 0.0722f * palette.bgB) * 255.f + 0.5f);
                if (v < 0) {
                    v = 0;
                }
                if (v > 255) {
                    v = 255;
                }
                px[0] = (unsigned char)v;
            } else {
                float out[FZ_MAX_COLORS] = {palette.bgR, palette.bgG, palette.bgB};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, out, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[c] = (unsigned char)v;
                }
            }
            if (src->alpha && n > components) {
                px[n - 1] = 255;
            }
        }
    }
    return dst;
}

bool PdfDarkModeV2QuickStudioWhiteMatCandidate(fz_context* ctx, fz_image* image) {
    if (!ctx || !image || image->mask) {
        return false;
    }
    int iw = image->w;
    int ih = image->h;
    if (iw < 120 || ih < 120) {
        return false;
    }
    // Studio cards are mid-size; giant scans / tiny chips stay on other paths.
    if ((i64)iw * (i64)ih > kPdfDarkModeV2WhiteMatMaxArea) {
        return false;
    }
    fz_pixmap* src = nullptr;
    bool ok = false;
    fz_var(src);
    fz_try(ctx) {
        // Border-only decision: decode tiny so layout textbooks do not pay full
        // white-mat cost on every photo tile (Exploring Our World atlas).
        const int maxSide = 96;
        int maxDim = iw > ih ? iw : ih;
        float s = maxDim > maxSide ? (float)maxSide / (float)maxDim : 1.f;
        fz_matrix ctm = fz_scale(s, s);
        src = fz_get_pixmap_from_image(ctx, image, nullptr, &ctm, nullptr, nullptr);
        if (!src || !src->samples || src->w < 16 || src->h < 16) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "studio-mat probe decode failed");
        }
        fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
        fz_colorspace* rgb = fz_device_rgb(ctx);
        int components = fz_colorspace_n(ctx, cs);
        int w = src->w;
        int h = src->h;
        auto isPaper = [&](int x, int y) -> bool {
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            return (maxC - minC) < 0.10f && lum > 0.90f;
        };
        auto sideRatio = [&](int x0, int y0, int x1, int y1) -> float {
            int n = 0, hit = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    n++;
                    if (isPaper(x, y)) {
                        hit++;
                    }
                }
            }
            return n > 0 ? (float)hit / (float)n : 0.f;
        };
        int bandX = w > 32 ? w / 16 : 2;
        int bandY = h > 32 ? h / 16 : 2;
        if (bandX < 1) {
            bandX = 1;
        }
        if (bandY < 1) {
            bandY = 1;
        }
        int paperSides = 0;
        if (sideRatio(0, 0, w, bandY) >= 0.55f) {
            paperSides++;
        }
        if (sideRatio(0, h - bandY, w, h) >= 0.55f) {
            paperSides++;
        }
        if (sideRatio(0, 0, bandX, h) >= 0.55f) {
            paperSides++;
        }
        if (sideRatio(w - bandX, 0, w, h) >= 0.55f) {
            paperSides++;
        }
        if (paperSides < 3) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "studio-mat probe: rim not paper");
        }
        // Interior must have subject (not a blank white chip).
        int ink = 0, samples = 0;
        int x0 = w / 4, x1 = w - w / 4;
        int y0 = h / 4, y1 = h - h / 4;
        for (int y = y0; y < y1; y += 2) {
            for (int x = x0; x < x1; x += 2) {
                samples++;
                if (!isPaper(x, y)) {
                    ink++;
                }
            }
        }
        float inkRatio = samples > 0 ? (float)ink / (float)samples : 0.f;
        // Upper bound omitted: a tight subject can fill the probe window (ink≈1).
        // Blank white chips fail via inkRatio≈0; real photos fail via paperSides.
        ok = inkRatio >= 0.08f;
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
    }
    fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

fz_pixmap* PdfDarkModeProcessV2WhiteMatPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
    if (!ctx || !src || !src->samples) {
        return nullptr;
    }
    (void)palette; // cutout is alpha; surrounding paper shows through
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int w = src->w;
    int h = src->h;
    // Use 64-bit area — int w*h overflows on large pixmaps and bypassed the cap (AV).
    // Allow thin wide strips (callout shadow plates under Glencoe banners).
    bool thinWide = (h >= 2 && h < 8 && w >= 24 && w >= h * 6) || (w >= 2 && w < 8 && h >= 24 && h >= w * 6);
    if (!PdfDarkModeV2WhiteMatDimsAllowed(w, h, thinWide)) {
        // Tiny noise or huge full-bleed photos: skip. Studio cutouts up to ~2800² OK.
        return nullptr;
    }

    int paperSamples = 0, satHits = 0, chromaHits = 0, inkHits = 0;
    float lumSum = 0.f;
    float lumSq = 0.f;
    int estStepX = w > 48 ? w / 48 : 1;
    int estStepY = h > 48 ? h / 48 : 1;
    for (int y = 0; y < h; y += estStepY) {
        for (int x = 0; x < w; x += estStepX) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            paperSamples++;
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            lumSum += lum;
            lumSq += lum * lum;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            if (chroma >= 0.12f) {
                satHits++;
            }
            if (chroma >= 0.06f) {
                chromaHits++;
            }
            if (lum < 0.22f) {
                inkHits++;
            }
        }
    }
    float satRatio = paperSamples > 0 ? (float)satHits / (float)paperSamples : 0.f;
    float chromaRatio = paperSamples > 0 ? (float)chromaHits / (float)paperSamples : 0.f;
    float inkRatio = paperSamples > 0 ? (float)inkHits / (float)paperSamples : 0.f;
    float meanLum = paperSamples > 0 ? lumSum / (float)paperSamples : 0.f;
    float lumVar = paperSamples > 0 ? lumSq / (float)paperSamples - meanLum * meanLum : 0.f;
    if (lumVar < 0.f) {
        lumVar = 0.f;
    }
    // Grayscale animal on a white card (zebra). Flat text stays out: its luminance
    // variance is much lower, and knocking out its paper would leave black glyphs.
    bool grayCutout = satRatio < 0.08f && chromaRatio < 0.10f && inkRatio >= 0.04f && lumVar >= 0.05f;
    // Early out only when neither colorful badge nor light shadow-plate is plausible.
    if (!grayCutout && satRatio < 0.05f && chromaRatio < 0.08f && inkRatio >= 0.08f) {
        return nullptr;
    }

    int masked = 0;
    int paperSides = 0;
    const int maxDimProbe = w > h ? w : h;
    const int minDimProbe = w < h ? w : h;
    // Wide short title pills ("Visual Summary") exceed the 180px icon cap but still sit
    // on cream + soft gray drop shadow that must knock out on dark pages.
    const bool titlePillMat = h >= 24 && h <= 96 && w >= 140 && w <= 480 && (i64)w * (i64)h <= (i64)480 * 96;
    const bool iconPastelMat = (maxDimProbe <= 180 && (i64)w * (i64)h <= (i64)180 * 180) || titlePillMat;
    // Geometry-only strip candidate: cyan-as-paper must run during flood, before the
    // DecorativeStripMat gate (which needs the flooded edge ratio).
    const bool stripMatGeom =
        minDimProbe >= 6 && minDimProbe <= 96 && maxDimProbe >= minDimProbe * 5 && (i64)w * (i64)h <= (i64)128 * 1024;
    // Seal only grayscale cutouts (zebra stripes). Soft color studio portraits
    // (Getting Dressed) are full-page → PictureBook; do not wide-seal Zoo cards —
    // that left vertical white mat islands beside the chimp and aborted the snake
    // (light scales read as soft-white clothing).
    u8* edgeMask = dm_v2_build_border_edge_white_mask(ctx, src, cs, rgb, components, w, h, &masked, &paperSides,
                                                      /*strictPaper=*/grayCutout, /*sealSubject=*/grayCutout,
                                                      /*wideSeal=*/false, iconPastelMat, stripMatGeom);
    if (!edgeMask) {
        return nullptr;
    }
    float edgeWhiteRatio = (w > 0 && h > 0) ? (float)masked / (float)(w * h) : 0.f;
    bool knock = PdfDarkModeV2ShouldKnockOutWhiteMat(paperSides, edgeWhiteRatio, satRatio, chromaRatio, inkRatio);
    // Wide mat around a grayscale subject. A thin rim (edgeWhite ~0.20) is a photo border, not a card.
    if (!knock && grayCutout && paperSides >= 3 && edgeWhiteRatio >= 0.35f && edgeWhiteRatio <= 0.92f) {
        knock = true;
    }
    // Color studio cutout (RAZ animals): large white card, small subject → edgeWhite
    // often 0.93–0.96 (old 0.92 cap treated it as an empty white chip).
    if (!knock && !grayCutout && (satRatio >= 0.07f || chromaRatio >= 0.10f) && inkRatio >= 0.03f && lumVar >= 0.04f &&
        paperSides >= 1 && edgeWhiteRatio >= 0.35f && edgeWhiteRatio <= 0.97f) {
        knock = true;
    }
    if (!knock &&
        PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(w, h, paperSides, edgeWhiteRatio, satRatio, chromaRatio)) {
        knock = true;
    }
    bool stripKnock =
        PdfDarkModeV2ShouldKnockOutDecorativeStripMat(w, h, paperSides, edgeWhiteRatio, satRatio, chromaRatio);
    if (!knock && stripKnock) {
        knock = true;
    }
    // Pastel ocean fills on atlas map chips: edge ratio is high once cyan counts as paper.
    if (!knock && iconPastelMat && (satRatio >= 0.07f || chromaRatio >= 0.10f) && edgeWhiteRatio >= 0.08f &&
        edgeWhiteRatio <= 0.92f && paperSides >= 1) {
        knock = true;
    }
    if (!knock && PdfDarkModeV2ShouldKnockOutAlmostPaperChip(w, h, paperSides, edgeWhiteRatio, satRatio, chromaRatio)) {
        knock = true;
    }
    if (!knock) {
        free(edgeMask);
        return nullptr;
    }
    const bool stripMat = stripKnock || stripMatGeom;

    // Cream + soft drop-shadow plates (Visual Summary card frames): after the cream
    // floods away, mid-gray shadow blobs stay opaque and read as narrow bright bands.
    // If the non-flood remainder has no real ink/hue (unlike a photo cutout), clear it.
    int remainN = 0, remainChroma = 0, remainInk = 0, remainPaper = 0;
    float remainLumSum = 0.f, remainLumSq = 0.f;
    int remStepX = w > 64 ? w / 64 : 1;
    int remStepY = h > 64 ? h / 64 : 1;
    for (int y = 0; y < h; y += remStepY) {
        for (int x = 0; x < w; x += remStepX) {
            if (edgeMask[y * w + x]) {
                continue;
            }
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            remainN++;
            remainLumSum += lum;
            remainLumSq += lum * lum;
            if (chroma >= 0.16f) {
                remainChroma++;
            }
            if (lum < 0.22f) {
                remainInk++;
            }
            // Near-pure mat white still inside the opaque remainder (not light fur).
            if (chroma < 0.08f && lum > 0.92f) {
                remainPaper++;
            }
        }
    }
    float remainChromaRatio = remainN > 0 ? (float)remainChroma / (float)remainN : 0.f;
    float remainInkRatio = remainN > 0 ? (float)remainInk / (float)remainN : 0.f;
    float remainPaperRatio = remainN > 0 ? (float)remainPaper / (float)remainN : 0.f;
    float remainMean = remainN > 0 ? remainLumSum / (float)remainN : 0.f;
    float remainLumVar = remainN > 0 ? remainLumSq / (float)remainN - remainMean * remainMean : 0.f;
    if (remainLumVar < 0.f) {
        remainLumVar = 0.f;
    }
    const bool clearShadowRemain =
        remainN > 0 && edgeWhiteRatio >= 0.35f && maxDimProbe <= 220 &&
        PdfDarkModeV2RemainLooksLikeSoftShadowOnly(remainChromaRatio, remainInkRatio, remainLumVar);

    // Soft-edge studio cards: flood only chewed the rim → jagged white rectangle.
    // Abort before writing alpha; keep the original opaque image (clean card).
    // Icons / strips / chips skip — their remain is meant to be small and rectangular.
    if (!clearShadowRemain && !iconPastelMat && !stripMat && maxDimProbe > 220 && remainN > 0) {
        int remCount = 0;
        int minX = w, minY = h, maxX = -1, maxY = -1;
        for (int y = 0; y < h; y++) {
            const u8* row = edgeMask + (size_t)y * (size_t)w;
            for (int x = 0; x < w; x++) {
                if (row[x]) {
                    continue;
                }
                remCount++;
                if (x < minX) {
                    minX = x;
                }
                if (y < minY) {
                    minY = y;
                }
                if (x > maxX) {
                    maxX = x;
                }
                if (y > maxY) {
                    maxY = y;
                }
            }
        }
        if (remCount > 0 && maxX >= minX && maxY >= minY) {
            const float bboxArea = (float)(maxX - minX + 1) * (float)(maxY - minY + 1);
            const float imgArea = (float)w * (float)h;
            const float solidity = bboxArea > 0.f ? (float)remCount / bboxArea : 0.f;
            const float coverage = imgArea > 0.f ? bboxArea / imgArea : 0.f;
            if (PdfDarkModeV2RemainLooksLikeFailedRectFlood(solidity, coverage, remainPaperRatio, edgeWhiteRatio)) {
                free(edgeMask);
                return nullptr;
            }
        }
    }

    // Always cut to alpha — never paint theme bg. Theme-colored mats look black on
    // yellow sidebars / olive Visual Summary paper (SECTION 2 / CHAPTER badges).
    // Soft alpha only on the AA fringe; flooded paper is fully clear so the
    // surrounding page color shows through.
    fz_pixmap* dst = fz_new_pixmap(ctx, rgb, w, h, nullptr, 1);
    fz_clear_pixmap(ctx, dst);
    if (clearShadowRemain || edgeWhiteRatio >= 0.98f) {
        // Fully paper / paper+shadow chip: nothing to keep.
        free(edgeMask);
        return dst;
    }
    int dstN = dst->n;
    int dstStride = dst->stride;
    auto nearFlood = [&](int x, int y) -> bool {
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                int xx = x + dx;
                int yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
                    continue;
                }
                if (edgeMask[yy * w + xx]) {
                    return true;
                }
            }
        }
        return false;
    };
    for (int y = 0; y < h; y++) {
        unsigned char* drow = dst->samples + (size_t)y * (size_t)dstStride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = drow + (size_t)x * (size_t)dstN;
            if (edgeMask[y * w + x]) {
                px[0] = px[1] = px[2] = 0;
                px[3] = 0;
                continue;
            }
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            float paperAmt = dm_v2_paper_amount_rgb(r, g, b, iconPastelMat, stripMat);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float chroma = maxC - minC;
            // Title/badge cream plates: neutral mid-gray drop shadow under glyphs is not
            // border-connected once letters seal pockets — clear by color, keep hue.
            if (iconPastelMat && edgeWhiteRatio >= 0.35f && chroma < 0.14f && lum > 0.42f && lum < 0.92f) {
                px[0] = px[1] = px[2] = 0;
                px[3] = 0;
                continue;
            }
            // Mid-gray JPEG fringe next to the mat (oval photo cutouts).
            if (paperAmt < 0.18f && nearFlood(x, y) && lum > 0.62f && chroma < 0.28f) {
                paperAmt = (lum - 0.55f) / 0.40f;
                if (paperAmt > 1.f) {
                    paperAmt = 1.f;
                }
                if (paperAmt < 0.f) {
                    paperAmt = 0.f;
                }
            }
            // Soft drop-shadow gray glued to cream mats.
            if (paperAmt < 0.20f && nearFlood(x, y) && chroma < 0.16f && lum > 0.40f && lum < 0.90f) {
                float shadowClear = (lum - 0.35f) / 0.45f;
                if (shadowClear > 1.f) {
                    shadowClear = 1.f;
                }
                if (shadowClear > paperAmt) {
                    paperAmt = shadowClear;
                }
            }
            float a = 1.f;
            if (nearFlood(x, y)) {
                float clear = paperAmt;
                // Circle/oval AA against former white: light mix pixels that still
                // look like subject chroma but read as a pale halo on dark paper.
                if (lum > 0.68f) {
                    float lightClear = (lum - 0.68f) / 0.30f;
                    if (lightClear > 1.f) {
                        lightClear = 1.f;
                    }
                    if (lightClear > clear) {
                        clear = lightClear;
                    }
                }
                // Wave open-side cyan AA: steeper clear so a 1px white column dies.
                if (stripMat && lum > 0.70f && (chroma < 0.40f || b >= r)) {
                    float stripClear = (lum - 0.65f) / 0.28f;
                    if (stripClear > 1.f) {
                        stripClear = 1.f;
                    }
                    if (stripClear > clear) {
                        clear = stripClear;
                    }
                }
                if (clear > 0.08f) {
                    a = 1.f - clear;
                    if (a < 0.f) {
                        a = 0.f;
                    }
                }
            }
            if (a < 0.02f) {
                px[0] = px[1] = px[2] = 0;
                px[3] = 0;
                continue;
            }
            if (a > 0.98f) {
                a = 1.f;
            }
            px[0] = (unsigned char)(r * a * 255.f + 0.5f);
            px[1] = (unsigned char)(g * a * 255.f + 0.5f);
            px[2] = (unsigned char)(b * a * 255.f + 0.5f);
            px[3] = (unsigned char)(a * 255.f + 0.5f);
        }
    }
    free(edgeMask);
    return dst;
}

fz_pixmap* PdfDarkModeProcessSoftCreamPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
    if (!ctx || !src || !src->samples) {
        return src;
    }
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = src->n;
    int w = src->w;
    int h = src->h;
    int stride = src->stride;

    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);

    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);
    float softening = PdfDarkModeCurrentOptions().preserveImagePaperSoftening;
    if (softening <= 0.f) {
        softening = 0.45f;
    }

    for (int y = 0; y < h; y++) {
        unsigned char* row = dst->samples + y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + x * n;
            float r, g, b;
            if (fastRgb) {
                r = px[0] / 255.f;
                g = px[1] / 255.f;
                b = px[2] / 255.f;
            } else if (fastGray) {
                r = g = b = px[0] / 255.f;
            } else {
                float conv[FZ_MAX_COLORS] = {};
                float srcRgb[FZ_MAX_COLORS] = {};
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    conv[c] = px[c] / 255.f;
                }
                fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
                r = srcRgb[0];
                g = srcRgb[1];
                b = srcRgb[2];
            }
            float cr = r, cg = g, cb = b;
            PdfDarkModeCompressPhotoHighlights(r, g, b, &cr, &cg, &cb);
            float nr = r, ng = g, nb = b;
            ApplyPreserveImagePaperSoftening(cr, cg, cb, palette, softening, &nr, &ng, &nb);
            if (fastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (fastGray) {
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(lum * 255.f + 0.5f);
                px[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                float out[FZ_MAX_COLORS] = {nr, ng, nb};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, out, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[c] = (unsigned char)v;
                }
            }
        }
    }
    return dst;
}

fz_pixmap* PdfDarkModeProcessGovernmentPaperPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
    if (!ctx || !src || !src->samples) {
        return src;
    }
    DarkModePalette govPalette = PdfDarkModeGovernmentPaperPalette(palette);
    GovPaperThemeBytes themeBytes = PdfDarkModeGovernmentPaperThemeBytes(palette);
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int srcN = src->n;
    int w = src->w;
    int h = src->h;
    int srcStride = src->stride;

    bool srcGray = components == 1 || fz_colorspace_is_gray(ctx, cs);
    bool srcRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    // Gray JPEG scans: promote to RGB so theme paper/ink match UpdateBitmapColors (Darcula tint, not flat gray).
    bool promoteThemeRgb = srcGray && !srcRgb;

    fz_pixmap* dst = nullptr;
    int dstN = 0;
    int dstStride = 0;
    if (promoteThemeRgb) {
        dst = fz_new_pixmap(ctx, rgb, w, h, src->seps, src->alpha);
        dstN = dst->n;
        dstStride = dst->stride;
    } else {
        dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
        fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);
        dstN = srcN;
        dstStride = dst->stride;
    }

    bool writeThemeRgb = promoteThemeRgb || srcRgb;
    bool writeGray = srcGray && !promoteThemeRgb;

    float threshold = dm_gov_estimate_binary_threshold(ctx, src, cs, rgb, components, w, h);
    int thresh8 = (int)(threshold * 255.f + 0.5f);
    if (thresh8 < 1) {
        thresh8 = 1;
    }
    if (thresh8 > 254) {
        thresh8 = 254;
    }

    // Office scans are almost all gray ink/paper. Integer binarize avoids a
    // float remap of every pixel (native 公文 pages are often 1–6 million px).
    if ((srcRgb || srcGray) && writeThemeRgb) {
        for (int y = 0; y < h; y++) {
            unsigned char* srcRow = src->samples + y * srcStride;
            unsigned char* dstRow = dst->samples + y * dstStride;
            for (int x = 0; x < w; x++) {
                unsigned char* spx = srcRow + x * srcN;
                unsigned char* dpx = dstRow + x * dstN;
                int r8, g8, b8;
                if (srcGray) {
                    r8 = g8 = b8 = spx[0];
                } else {
                    r8 = spx[0];
                    g8 = spx[1];
                    b8 = spx[2];
                }
                int maxC = r8 > g8 ? (r8 > b8 ? r8 : b8) : (g8 > b8 ? g8 : b8);
                int minC = r8 < g8 ? (r8 < b8 ? r8 : b8) : (g8 < b8 ? g8 : b8);
                if (maxC - minC < 36) {
                    int lum8 = (r8 * 54 + g8 * 183 + b8 * 19) >> 8;
                    if (lum8 >= thresh8) {
                        dpx[0] = themeBytes.paperR;
                        dpx[1] = themeBytes.paperG;
                        dpx[2] = themeBytes.paperB;
                    } else {
                        dpx[0] = themeBytes.inkR;
                        dpx[1] = themeBytes.inkG;
                        dpx[2] = themeBytes.inkB;
                    }
                } else {
                    float r = r8 / 255.f, g = g8 / 255.f, b = b8 / 255.f;
                    float nr = r, ng = g, nb = b;
                    if (!ApplyGovernmentPaperColoredPixel(r, g, b, threshold, govPalette, &nr, &ng, &nb)) {
                        ApplyGovernmentPaperFallbackPixel(r, g, b, threshold, govPalette, &nr, &ng, &nb);
                    }
                    int vr = (int)(nr * 255.f + 0.5f);
                    int vg = (int)(ng * 255.f + 0.5f);
                    int vb = (int)(nb * 255.f + 0.5f);
                    dpx[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                    dpx[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                    dpx[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
                }
                if (dst->alpha && dstN >= 4) {
                    dpx[3] = 255;
                }
            }
        }
        return dst;
    }

    for (int y = 0; y < h; y++) {
        unsigned char* srcRow = src->samples + y * srcStride;
        unsigned char* dstRow = dst->samples + y * dstStride;
        for (int x = 0; x < w; x++) {
            unsigned char* spx = srcRow + x * srcN;
            unsigned char* dpx = dstRow + x * dstN;
            float r, g, b;
            if (srcRgb) {
                r = spx[0] / 255.f;
                g = spx[1] / 255.f;
                b = spx[2] / 255.f;
            } else if (srcGray) {
                r = g = b = spx[0] / 255.f;
            } else {
                float conv[FZ_MAX_COLORS] = {};
                float srcRgbPx[FZ_MAX_COLORS] = {};
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    conv[c] = spx[c] / 255.f;
                }
                fz_convert_color(ctx, cs, conv, rgb, srcRgbPx, cs, fz_default_color_params);
                r = srcRgbPx[0];
                g = srcRgbPx[1];
                b = srcRgbPx[2];
            }
            float nr = r, ng = g, nb = b;
            float srcLum = dm_gov_paper_luminance(r, g, b);
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float srcChroma = maxC - minC;
            bool plainInkPaper = false;
            if (!ApplyGovernmentPaperInkPaper(r, g, b, threshold, govPalette, &nr, &ng, &nb)) {
                if (!ApplyGovernmentPaperColoredPixel(r, g, b, threshold, govPalette, &nr, &ng, &nb)) {
                    ApplyGovernmentPaperFallbackPixel(r, g, b, threshold, govPalette, &nr, &ng, &nb);
                }
            } else {
                plainInkPaper = srcChroma < 0.14f;
            }
            if (writeThemeRgb) {
                if (plainInkPaper) {
                    // Binary mask → theme paper/ink (same end colors as UpdateBitmapColors white/black).
                    if (srcLum >= threshold) {
                        dpx[0] = themeBytes.paperR;
                        dpx[1] = themeBytes.paperG;
                        dpx[2] = themeBytes.paperB;
                    } else {
                        dpx[0] = themeBytes.inkR;
                        dpx[1] = themeBytes.inkG;
                        dpx[2] = themeBytes.inkB;
                    }
                } else {
                    int vr = (int)(nr * 255.f + 0.5f);
                    int vg = (int)(ng * 255.f + 0.5f);
                    int vb = (int)(nb * 255.f + 0.5f);
                    dpx[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                    dpx[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                    dpx[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
                }
                if (dst->alpha && dstN >= 4) {
                    dpx[3] = 255;
                }
            } else if (writeGray) {
                if (plainInkPaper) {
                    int lum =
                        srcLum >= threshold
                            ? (int)(0.2126f * themeBytes.paperR + 0.7152f * themeBytes.paperG +
                                    0.0722f * themeBytes.paperB)
                            : (int)(0.2126f * themeBytes.inkR + 0.7152f * themeBytes.inkG + 0.0722f * themeBytes.inkB);
                    dpx[0] = (unsigned char)(lum < 0 ? 0 : (lum > 255 ? 255 : lum));
                } else {
                    float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                    int v = (int)(lum * 255.f + 0.5f);
                    dpx[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
            } else {
                float out[FZ_MAX_COLORS] = {nr, ng, nb};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, out, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    dpx[c] = (unsigned char)v;
                }
            }
        }
    }
    return dst;
}

bool PdfDarkModeIsDecorativeStripImage(const RectF& imgRect, const RectF& pageBounds) {
    if (imgRect.IsEmpty() || pageBounds.IsEmpty()) {
        return false;
    }
    float w = imgRect.dx;
    float h = imgRect.dy;
    if (w <= 0.f || h <= 0.f) {
        return false;
    }
    float pageW = pageBounds.dx;
    float pageH = pageBounds.dy;
    if (pageW <= 0.f || pageH <= 0.f) {
        return false;
    }

    float wFrac = w / pageW;
    float hFrac = h / pageH;
    float minDim = w < h ? w : h;
    float maxDim = w > h ? w : h;
    float aspect = minDim / maxDim;

    // Tall narrow or wide shallow strips (spiral margins, side shadows).
    if (aspect < 0.22f) {
        return true;
    }
    // Edge-aligned column/row spanning a substantial part of the page.
    if (wFrac < 0.20f && hFrac > 0.30f) {
        return true;
    }
    if (hFrac < 0.20f && wFrac > 0.30f) {
        return true;
    }
    return false;
}

static bool PdfDarkModeStripIsExternallyAdjacentToArt(const RectF& strip, const RectF& art, float tol = 4.f) {
    constexpr float kMaxGap = 20.f;
    float sw = strip.dx;
    float sh = strip.dy;
    if (sh > sw) {
        bool yOverlap = strip.y < art.y + art.dy + tol && strip.y + sh > art.y - tol;
        if (!yOverlap) {
            return false;
        }
        if (strip.x + sw <= art.x + tol) {
            return art.x - (strip.x + sw) <= kMaxGap;
        }
        if (strip.x >= art.x + art.dx - tol) {
            return strip.x - (art.x + art.dx) <= kMaxGap;
        }
        return false;
    }
    if (sw > sh) {
        bool xOverlap = strip.x < art.x + art.dx + tol && strip.x + sw > art.x - tol;
        if (!xOverlap) {
            return false;
        }
        if (strip.y + sh <= art.y + tol) {
            return art.y - (strip.y + sh) <= kMaxGap;
        }
        if (strip.y >= art.y + art.dy - tol) {
            return strip.y - (art.y + art.dy) <= kMaxGap;
        }
        return false;
    }
    return false;
}

bool PdfDarkModeIsSubstantialFollowThemeArtwork(const RectF& imgRect, float pageArea) {
    if (imgRect.IsEmpty() || pageArea <= 0.f) {
        return false;
    }
    float w = imgRect.dx;
    float h = imgRect.dy;
    float minDim = w < h ? w : h;
    float maxDim = w > h ? w : h;
    if (minDim <= 0.f || maxDim <= 0.f) {
        return false;
    }
    float coverage = (w * h) / pageArea;
    float aspect = minDim / maxDim;
    return coverage >= 0.04f && minDim >= 50.f && aspect >= 0.25f;
}

bool PdfDarkModeIsPhotoFrameStripImage(const RectF& imgRect, const RectF& pageBounds, const Vec<RectF>* artworkBounds) {
    if (!PdfDarkModeIsDecorativeStripImage(imgRect, pageBounds)) {
        return false;
    }
    float w = imgRect.dx;
    float h = imgRect.dy;
    if (w <= 0.f || h <= 0.f) {
        return false;
    }
    float minDim = w < h ? w : h;
    float maxDim = w > h ? w : h;
    // Photo vignette/frame strips are thin on one axis and span a long edge beside artwork.
    if (minDim >= 40.f || maxDim <= 80.f) {
        return false;
    }
    if (!artworkBounds) {
        return false;
    }
    for (const RectF& art : *artworkBounds) {
        if (PdfDarkModeStripIsExternallyAdjacentToArt(imgRect, art)) {
            return true;
        }
    }
    return false;
}

// Follow-theme image policy:
// - Text-heavy / small figures (< 75% coverage): Preserve (literature sweet spot).
// - Full-bleed: classify pixels — Photo/colorful art Preserve; true scans AdaptiveDocument.
DarkImagePolicy PdfDarkModePolicyForFollowThemeImage(const RectF& imgBounds, bool isImageMask, const RectF& pageBounds,
                                                     const Vec<RectF>* artworkBounds, fz_context* ctx, fz_image* image,
                                                     DarkModeEngineCache* engineCache) {
    if (isImageMask) {
        return PdfDarkModePolicyForImageKind(DarkImageKind::Unknown, true);
    }
    if (imgBounds.IsEmpty() || pageBounds.IsEmpty()) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    bool photoFrameStrip = PdfDarkModeIsPhotoFrameStripImage(imgBounds, pageBounds, artworkBounds);
    if (!photoFrameStrip) {
        int minPx = GetPreservePdfImagesMinSize();
        if (minPx > 0 && (imgBounds.dx < (float)minPx || imgBounds.dy < (float)minPx)) {
            return DarkImagePolicy::AdaptiveDocument;
        }
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    float coverage = pageArea > 0.f ? (imgBounds.dx * imgBounds.dy) / pageArea : 0.f;
    if (coverage < kMaxPreserveImagePageCoverage) {
        // Large light panels (government scans) may fall slightly below 75% due to PDF placement.
        if (ctx && image && coverage >= 0.50f) {
            bool scannedHint = true;
            DarkImageAnalysis analysis = PdfDarkModeAnalyzeImageCached(ctx, image, coverage, scannedHint, engineCache);
            DarkImagePolicy policy = PdfDarkModePolicyForImageKind(analysis.kind, false);
            if (policy != DarkImagePolicy::Preserve) {
                return PdfDarkModeClampFollowThemePolicy(policy, coverage, analysis);
            }
            return PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, coverage, analysis);
        }
        return DarkImagePolicy::Preserve;
    }
    if (ctx && image) {
        bool scannedHint = coverage >= 0.55f;
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImageCached(ctx, image, coverage, scannedHint, engineCache);
        DarkImagePolicy policy = PdfDarkModePolicyForImageKind(analysis.kind, false);
        DarkImagePolicy finalPolicy = PdfDarkModeClampFollowThemePolicy(policy, coverage, analysis);
        return finalPolicy;
    }
    if (coverage >= kMaxPreserveImagePageCoverage) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    return DarkImagePolicy::AdaptiveDocument;
}
