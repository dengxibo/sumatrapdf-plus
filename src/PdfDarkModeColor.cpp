/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "Theme.h"
#include "Translations.h"

#include "PdfDarkModeInternal.h"

// Hardcoded PDF dark mode defaults (not persisted in settings file).
static constexpr int kPreservePdfImagesMinSize = 72;
// Object-level Smart Dark for dark + follow-theme PDF (experiment; was LegacyBitmapPostProcess).
static constexpr PdfDarkModeRenderer kPdfDarkModeRenderer = PdfDarkModeRenderer::ObjectLevelDevice;

static bool gPreservePdfImagesInDarkMode = true;

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
    byte rb = (byte)(palette.bgR * 255.f + 0.5f);
    byte gb = (byte)(palette.bgG * 255.f + 0.5f);
    byte bb = (byte)(palette.bgB * 255.f + 0.5f);
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
    const float lowChroma = 0.11f;
    if (chroma >= lowChroma) {
        return false;
    }
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
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
    // Tinted page paper: linear invert matches legacy readability on textbook pages.
    if (PdfFollowThemePreservesEmbeddedImageColors() && chroma < 0.14f) {
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
};

static constexpr int kDmPbMaxPhotoRects = 8;

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

// RAZ / Adobe Image Conversion pages often place several photos (color + B&W portrait)
// side-by-side — or overlapping — in one full-bleed pixmap. Keeping only the largest
// dense block left secondary portraits in the "margin" path (ink/paper → look negative).
// Scan each dense Y-band in horizontal slices so a portrait hanging below a color photo
// still gets its own rect (axis-aligned union would swallow caption text into "preserve").
static int dm_pb_find_photo_rects(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* outRects, int maxRects) {
    if (!ctx || !pix || !pix->samples || !outRects || maxRects <= 0 || pix->w < 8 || pix->h < 8) {
        return 0;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int w = pix->w;
    int h = pix->h;
    int step = w > 800 ? 4 : (w > 400 ? 2 : 1);

    float* rowDense = AllocArray<float>(h);
    if (!rowDense) {
        return 0;
    }
    for (int y = 0; y < h; y++) {
        int dense = 0;
        int samples = 0;
        for (int x = 0; x < w; x += step) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &r, &g, &b);
            samples++;
            if (!dm_pb_is_paper_rgb(r, g, b)) {
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

    auto appendRect = [&](int x0, int y0, int x1, int y1) {
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
                        if (!dm_pb_is_paper_rgb(r, g, b)) {
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
        }
    }
    free(rowDense);

    // Grow each rect vertically using only its own columns so a portrait that
    // continues below the page-wide dense band stays fully protected.
    for (int i = 0; i < nRects; i++) {
        DmPbPhotoRect& r = outRects[i];
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
                if (!dm_pb_is_paper_rgb(rv, gv, bv)) {
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
            r.y0 = pick0 + pad;
            r.y1 = pick1 - pad;
            if (r.y1 <= r.y0 + 4) {
                r.y1 = r.y0 + 5;
            }
        }
    }

    return nRects;
}

static bool dm_pb_point_in_photo_rects(int x, int y, const DmPbPhotoRect* rects, int nRects) {
    for (int i = 0; i < nRects; i++) {
        if (x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 && y < rects[i].y1) {
            return true;
        }
    }
    return false;
}

fz_pixmap* PdfDarkModeProcessPictureBookPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
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

    // Coarse bright-paper estimate: text-scan pages misrouted here must not run
    // multi-rect photo search (several full-image passes on 2k–4k rasters).
    int paperSamples = 0;
    int paperHits = 0;
    int estStepX = w > 64 ? w / 64 : 1;
    int estStepY = h > 64 ? h / 64 : 1;
    for (int y = 0; y < h; y += estStepY) {
        for (int x = 0; x < w; x += estStepX) {
            float r, g, b;
            dm_pb_sample_rgb(ctx, src, cs, rgb, components, x, y, &r, &g, &b);
            paperSamples++;
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            // Broader than dm_pb_is_paper_rgb: cream/gray textbook paper also counts.
            if ((maxC - minC) < 0.10f && lum > 0.72f) {
                paperHits++;
            }
        }
    }
    float paperRatio = paperSamples > 0 ? (float)paperHits / (float)paperSamples : 0.f;

    DmPbPhotoRect photoRects[kDmPbMaxPhotoRects] = {};
    int nPhotoRects = 0;
    if (paperRatio < 0.75f) {
        nPhotoRects = dm_pb_find_photo_rects(ctx, src, photoRects, kDmPbMaxPhotoRects);
    }

    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);

    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);

    for (int y = 0; y < h; y++) {
        unsigned char* row = dst->samples + y * stride;
        for (int x = 0; x < w; x++) {
            if (nPhotoRects > 0 && dm_pb_point_in_photo_rects(x, y, photoRects, nPhotoRects)) {
                continue; // photo interior (color or B&W): untouched
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
            float nr = r, ng = g, nb = b;
            // Outside photo rects: steep ink/paper remap so AA text fringes don't
            // stay mid-gray (halo). Colorful pixels and protected photos are left alone.
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
                                                     const Vec<RectF>* artworkBounds, fz_context* ctx,
                                                     fz_image* image) {
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
        return DarkImagePolicy::Preserve;
    }
    if (ctx && image) {
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, false);
        return PdfDarkModePolicyForImageKind(analysis.kind, false);
    }
    return DarkImagePolicy::AdaptiveDocument;
}
