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
            } else if (lum <= 0.35f) {
                inkSum += lum;
                inkN++;
            }
        }
    }
    float threshold = 0.50f;
    if (paperN > 8 && inkN > 8) {
        threshold = (paperSum / (float)paperN + inkSum / (float)inkN) * 0.5f;
    } else if (paperN > 8) {
        threshold = (paperSum / (float)paperN) * 0.72f;
    }
    if (threshold < 0.44f) {
        threshold = 0.44f;
    }
    if (threshold > 0.54f) {
        threshold = 0.54f;
    }
    return threshold;
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
};

static constexpr int kDmPbMaxPhotoRects = 8;

static int dm_pb_find_photo_rects(fz_context* ctx, fz_pixmap* pix, DmPbPhotoRect* outRects, int maxRects);
static bool dm_pb_sample_rgb(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs, fz_colorspace* rgb, int components,
                             int x, int y, float* outR, float* outG, float* outB);

// Flat near-white used for oval-portrait rectangular mats (not specular highlights).
// Stricter than generic paper: highlights usually have texture / slightly lower lum.
static bool dm_pb_is_photo_rect_margin_paper_rgb(float r, float g, float b) {
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return (maxC - minC) < 0.05f && lum > 0.94f;
}

// Mark flat white that is 4-connected to a photo-rect border. Oval portrait mats are
// edge-connected; forehead/soap highlights sit inside tonal skin and are not.
static u8* dm_pb_build_edge_connected_margin_mask(fz_context* ctx, fz_pixmap* pix, fz_colorspace* cs,
                                                  fz_colorspace* rgb, int components, int w, int h,
                                                  const DmPbPhotoRect* rects, int nRects) {
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
        if (x1 - x0 < 4 || y1 - y0 < 4) {
            continue;
        }
        for (int y = y0; y < y1; y++) {
            for (int x = x0; x < x1; x++) {
                float rv, gv, bv;
                dm_pb_sample_rgb(ctx, pix, cs, rgb, components, x, y, &rv, &gv, &bv);
                if (dm_pb_is_photo_rect_margin_paper_rgb(rv, gv, bv)) {
                    paper[y * w + x] = 1;
                }
            }
        }
        int* q = AllocArray<int>((x1 - x0) * (y1 - y0));
        if (!q) {
            continue;
        }
        int qn = 0;
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
        for (int x = x0; x < x1; x++) {
            trySeed(x, y0);
            trySeed(x, y1 - 1);
        }
        for (int y = y0; y < y1; y++) {
            trySeed(x0, y);
            trySeed(x1 - 1, y);
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
        DmPbPhotoRect rects[kDmPbMaxPhotoRects] = {};
        int nRects = dm_pb_find_photo_rects(ctx, src, rects, kDmPbMaxPhotoRects);
        i64 imgArea = (i64)src->w * src->h;
        for (int i = 0; i < nRects; i++) {
            i64 a = (i64)(rects[i].x1 - rects[i].x0) * (rects[i].y1 - rects[i].y0);
            if (imgArea > 0 && a * 20 >= imgArea) {
                ok = true;
                break;
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
        DmPbPhotoRect rects[kDmPbMaxPhotoRects] = {};
        int nRects = dm_pb_find_photo_rects(ctx, src, rects, kDmPbMaxPhotoRects);
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

// Color pages use chroma to find photo blocks. B&W portraits (RAZ Abraham Lincoln etc.) have
// satRatio < 0.04 so they skipped rect detection and the whole tile was ink/paper inverted.
static bool dm_pb_should_seek_photo_rects(float satRatio, float chromaRatio, float paperRatio, float lumVar,
                                          const DarkImageAnalysis* imgAnalysis) {
    // Analysis thumbnails can look line-art (high paper) while full decode is photographic.
    if (imgAnalysis && PdfDarkModeFeaturesLookLikeBwLineArtScan(imgAnalysis->features) && lumVar < 0.038f) {
        return false;
    }
    if (satRatio >= 0.04f) {
        return true;
    }
    if (imgAnalysis) {
        const DarkImageFeatures& f = imgAnalysis->features;
        if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
            return true;
        }
        if (PdfDarkModeFeaturesLookLikePhoto(f)) {
            return true;
        }
    }
    // B&W portrait embedded in text page (RAZ Image Conversion): high lumVar, not line-art scan.
    if (satRatio < 0.04f && lumVar >= 0.035f) {
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
    int paperSamples = 0;
    int paperHits = 0;
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
            if (chroma < 0.10f && lum > 0.72f) {
                paperHits++;
            }
            if (chroma >= 0.12f) {
                satHits++;
            }
            if (chroma >= 0.06f) {
                chromaHits++;
            }
        }
    }
    float paperRatio = paperSamples > 0 ? (float)paperHits / (float)paperSamples : 0.f;
    float satRatio = paperSamples > 0 ? (float)satHits / (float)paperSamples : 0.f;
    float chromaRatio = paperSamples > 0 ? (float)chromaHits / (float)paperSamples : 0.f;
    float lumMean = paperSamples > 0 ? lumSum / (float)paperSamples : 0.f;
    float lumVar = paperSamples > 0 ? lumSqSum / (float)paperSamples - lumMean * lumMean : 0.f;

    DmPbPhotoRect photoRects[kDmPbMaxPhotoRects] = {};
    int nPhotoRects = 0;
    // Colorful pages + B&W documentary portraits: protect photo rects. B&W ink lines (连环画)
    // register as dense via luminance contrast but have no color — skip rects (full-page remap).
    if (dm_pb_should_seek_photo_rects(satRatio, chromaRatio, paperRatio, lumVar, imgAnalysis)) {
        nPhotoRects = dm_pb_find_photo_rects(ctx, src, photoRects, kDmPbMaxPhotoRects);
    }

    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);

    // True photo page with little flat white mat and no photo rects: keep original
    // (avoids crushing specular highlights). Pages with white portrait frames have
    // higher paperRatio and still need ink/paper remapping below.
    if (nPhotoRects == 0 && paperRatio < 0.18f && imgAnalysis &&
        (imgAnalysis->kind == DarkImageKind::Photo || PdfDarkModeFeaturesLookLikePhoto(imgAnalysis->features) ||
         PdfDarkModeFeaturesLookLikeGrayscalePhoto(imgAnalysis->features)) &&
        !PdfDarkModeFeaturesLookLikeBwLineArtScan(imgAnalysis->features)) {
        return dst;
    }

    u8* edgeWhiteMask = nullptr;
    if (nPhotoRects > 0) {
        edgeWhiteMask =
            dm_pb_build_edge_connected_margin_mask(ctx, src, cs, rgb, components, w, h, photoRects, nPhotoRects);
    }

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
            if (nPhotoRects > 0 && dm_pb_point_in_photo_rects(x, y, photoRects, nPhotoRects)) {
                // Preserve photo interiors (including specular highlights). Remap only
                // flat white that is edge-connected — the rectangular mat around ovals.
                bool edgeMat = edgeWhiteMask && edgeWhiteMask[y * w + x];
                if (!edgeMat) {
                    continue;
                }
            }
            float nr = r, ng = g, nb = b;
            if (!ApplySharpDocumentInkPaper(r, g, b, palette, &nr, &ng, &nb)) {
                // Steep ink/paper remap (dark paper + light text). Colorful pixels left alone.
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
    free(edgeWhiteMask);
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

    float threshold = dm_gov_estimate_binary_threshold(ctx, src, cs, rgb, components, w, h);

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
            float nr = r, ng = g, nb = b;
            if (!ApplyGovernmentPaperInkPaper(r, g, b, threshold, palette, &nr, &ng, &nb)) {
                if (!ApplyGovernmentPaperColoredPixel(r, g, b, threshold, palette, &nr, &ng, &nb)) {
                    ApplyGovernmentPaperFallbackPixel(r, g, b, threshold, palette, &nr, &ng, &nb);
                }
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
        // Large light panels (government scans) may fall slightly below 75% due to PDF placement.
        if (ctx && image && coverage >= 0.50f) {
            bool scannedHint = true;
            DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, scannedHint);
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
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, scannedHint);
        DarkImagePolicy policy = PdfDarkModePolicyForImageKind(analysis.kind, false);
        DarkImagePolicy finalPolicy = PdfDarkModeClampFollowThemePolicy(policy, coverage, analysis);
        return finalPolicy;
    }
    if (coverage >= kMaxPreserveImagePageCoverage) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    return DarkImagePolicy::AdaptiveDocument;
}
