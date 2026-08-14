/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "PdfDarkMode.h"

struct fz_context;
struct fz_device;
struct DarkModeEngineCache;

// Minimal dark Match path (V2):
// - Okular-style lightness invert mapped to theme text/bg for text/vector
// - small images unchanged
// - full-page images: cheap chroma-aware remap (paper/ink dark; photo midtones kept)

inline void MapRgbDarkModeV2(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    auto clamp01 = [](float v) -> float {
        if (v < 0.f) {
            return 0.f;
        }
        if (v > 1.f) {
            return 1.f;
        }
        return v;
    };
    // Okular PagePainter::invertLightness — keep hue/chroma, invert lightness.
    float R = clamp01(r);
    float G = clamp01(g);
    float B = clamp01(b);
    float m = R < G ? (R < B ? R : B) : (G < B ? G : B);
    R -= m;
    G -= m;
    B -= m;
    float C = R > G ? (R > B ? R : B) : (G > B ? G : B);
    float mPrime = 1.f - C - m;
    R = clamp01(R + mPrime);
    G = clamp01(G + mPrime);
    B = clamp01(B + mPrime);
    // Original white → theme bg; original black → theme text.
    outRgb[0] = palette.bgR + R * (palette.textR - palette.bgR);
    outRgb[1] = palette.bgG + G * (palette.textG - palette.bgG);
    outRgb[2] = palette.bgB + B * (palette.textB - palette.bgB);
}

// Fallback when photo-rect processing is unavailable: whole-image Okular→theme.
inline void MapRgbDarkModeV2PageImage(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    MapRgbDarkModeV2(r, g, b, palette, outRgb);
}

// White JPEG mat / soft drop-shadow plates. Unit-tested; used by ProcessV2WhiteMat.
// inkRatio = fraction of near-black pixels (icons have ink; pure shadow plates do not).
inline bool PdfDarkModeV2ShouldKnockOutWhiteMat(int paperSides, float edgeWhiteRatio, float satRatio, float chromaRatio,
                                                float inkRatio = 0.f) {
    if (edgeWhiteRatio < 0.03f) {
        return false;
    }
    // Colorful badge/icon in a square: need a multi-side mat frame (not open sky).
    if (satRatio >= 0.07f || chromaRatio >= 0.10f) {
        return paperSides >= 3 && edgeWhiteRatio <= 0.92f;
    }
    // Soft drop-shadow plates are often L-shaped (only 1–2 sides of the image bbox).
    if (inkRatio < 0.08f && paperSides >= 1 && edgeWhiteRatio >= 0.15f && edgeWhiteRatio <= 0.98f) {
        return true;
    }
    return false;
}

// Soft PDF drop shadows: dark color + partial alpha. Okular would turn them into light
// fringes on a dark page — keep them dark (theme bg) instead.
inline bool PdfDarkModeV2IsSoftShadowPaint(float r, float g, float b, float alpha) {
    if (alpha >= 0.92f) {
        return false;
    }
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    // Classic shadow: dark/neutral + translucency. Also light gray soft plates.
    if (chroma > 0.18f) {
        return false;
    }
    return lum < 0.55f || (lum > 0.70f && alpha < 0.85f);
}

// Raster drop-shadow / soft-edge plates (Glencoe callout cards): light mid-gray, no ink,
// no chroma. V2 preserves small images, so these stay bright on a dark page unless filled
// with theme bg. meanLum is average luminance of opaque samples.
inline bool PdfDarkModeV2LooksLikeSoftShadowPlate(float satRatio, float chromaRatio, float inkRatio, float meanLum) {
    if (satRatio >= 0.06f || chromaRatio >= 0.10f) {
        return false;
    }
    if (inkRatio >= 0.08f) {
        return false;
    }
    return meanLum >= 0.50f && meanLum <= 0.98f;
}

// Axis-aligned "photo" rects that are mostly white in the interior are speech-bubble /
// callout clusters (textbook cartoons), not photographs. Protecting them leaves white
// patches on a dark page. Portraits with a white mat have paper on the rim, not inside.
inline bool PdfDarkModeV2PhotoRectIsCalloutCluster(float insetPaperRatio) {
    return insetPaperRatio >= 0.35f;
}

// MRC background plates (hi-res JPEG under a JBIG2 text mask) keep faint leftover
// glyphs. PictureBook's SharpDocument chroma gate (~0.11–0.20) leaves light-brown
// JPEG ghosts on the dark page under the sharp mask → muddy double text.
// Paper-heavy pages crush those ghosts to theme bg; cover/photo pages do not.
inline bool PdfDarkModeV2ShouldCrushMrcBackgroundGhosts(float paperRatio) {
    return paperRatio >= 0.65f;
}

// Light leftover JPEG text: high lum, low-to-mid chroma (brown ghosts, cream paper).
// Dark ink and high-chroma art (orange sidebar, cartoons) stay out.
inline bool PdfDarkModeV2IsMrcBackgroundGhostPixel(float lum, float chroma) {
    return lum > 0.55f && chroma < 0.32f;
}

fz_device* PdfDarkModeWrapV2Device(fz_context* ctx, fz_device* inner, const DarkModePalette* palette,
                                   const RectF& pageBounds, DarkModeEngineCache* engineCache = nullptr,
                                   u32 profileHash = 0);
