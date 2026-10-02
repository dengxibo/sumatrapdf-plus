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

// Layout textbooks skip white-mat on medium/large photo tiles. Small badges/icons
// and thin shadow strips stay eligible for knockout (Exploring Our World atlas).
inline bool PdfDarkModeV2LayoutTextbookSkipFigureRemap(int w, int h, float coverage) {
    if (w <= 0 || h <= 0) {
        return true;
    }
    const int minDim = w < h ? w : h;
    const int maxDim = w > h ? w : h;
    const long long area = (long long)w * (long long)h;
    if (minDim > 0 && minDim < 16 && maxDim >= 24 && area <= (long long)64 * 1024) {
        return false;
    }
    // Tall/narrow Guide-to-Reading wave dividers (e.g. 34×697): white only on the
    // open side — must not take the photo-tile fast path.
    if (minDim >= 6 && minDim <= 96 && maxDim >= minDim * 5 && area <= (long long)128 * 1024) {
        return false;
    }
    // Include ~390-wide title pills / UNIT mats; skip only larger photo tiles.
    if (maxDim <= 512 && area <= (long long)220 * 1024 && coverage < 0.22f) {
        return false;
    }
    return true;
}

// Sidebar / display-type plates (Guide to Reading, section-badge slices) are warm
// parchment, chroma about 0.25–0.40. That is the artwork. Knocking it out leaves a
// black rectangle where no matching plate sits behind the tile. Low-chroma cream
// mats (Visual Summary pills, chroma under 0.22) and cool cyan ocean fills stay mats.
inline bool PdfDarkModeV2IsWarmDecorativeParchment(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum < 0.72f || chroma < 0.22f || chroma >= 0.48f) {
        return false;
    }
    // Cool cyan is an atlas mat, not parchment.
    if (b + 0.02f >= r || b + 0.02f >= g) {
        return false;
    }
    return true;
}

// Parchment and its JPEG fringe on the artwork side of a colored divider.
// Neutral white and cool cyan (the open page side of the wave) are not this.
inline bool PdfDarkModeV2IsWarmStripArtwork(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum < 0.62f || chroma < 0.06f || chroma >= 0.50f) {
        return false;
    }
    if (b + 0.02f >= r || b + 0.02f >= g) {
        return false;
    }
    return true;
}

// JPEG drop shadow under display type: warm gray, not green/blue/red ink.
// Clearing it keeps the colored glyphs; leaving it paints a black letter fill.
inline bool PdfDarkModeV2IsWarmNeutralShadow(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum < 0.40f || lum > 0.92f || chroma >= 0.24f) {
        return false;
    }
    if (b > r + 0.02f || b > g + 0.02f) {
        return false;
    }
    if (g > r + 0.15f && g > b + 0.15f) {
        return false;
    }
    if (r > g + 0.15f && r > b + 0.15f) {
        return false;
    }
    return true;
}

// Tall colorful divider strips: paper usually on only 1–2 open sides (right of wave).
inline bool PdfDarkModeV2ShouldKnockOutDecorativeStripMat(int w, int h, int paperSides, float edgeWhiteRatio,
                                                          float satRatio, float chromaRatio) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    const int minDim = w < h ? w : h;
    const int maxDim = w > h ? w : h;
    if (minDim < 6 || minDim > 96) {
        return false;
    }
    if (maxDim < minDim * 5) {
        return false;
    }
    if (edgeWhiteRatio < 0.04f || edgeWhiteRatio > 0.90f) {
        return false;
    }
    if (satRatio < 0.04f && chromaRatio < 0.06f) {
        return false;
    }
    return paperSides >= 1 && paperSides <= 2;
}

// Near-solid cream/white spacer chips (Visual Summary 6×63 gutter between CHAPTER and
// title). The usual white-mat gates reject edgeWhiteRatio > 0.92 as "pure white fragment".
inline bool PdfDarkModeV2ShouldKnockOutAlmostPaperChip(int w, int h, int paperSides, float edgeWhiteRatio,
                                                       float satRatio, float chromaRatio) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (edgeWhiteRatio < 0.90f) {
        return false;
    }
    if (paperSides < 2) {
        return false;
    }
    const int minDim = w < h ? w : h;
    const int maxDim = w > h ? w : h;
    if (minDim > 24 || maxDim < 16 || maxDim > 220) {
        return false;
    }
    const long long area = (long long)w * (long long)h;
    if (area > (long long)24 * 220) {
        return false;
    }
    // Cream textbook paper carries mild chroma; keep pure white photo chips out unless
    // the chip is a hairline spacer or a tall pure-white column (11×129 gutter beside
    // a divider). Square chips stay.
    if (chromaRatio >= 0.06f || satRatio >= 0.05f) {
        return true;
    }
    if (minDim <= 10) {
        return true;
    }
    return minDim <= 16 && maxDim >= minDim * 6;
}

// Blank white gutter beside a divider (30×349 column, 44×70 cap). No hue, paper
// on every side, so it is not a photo. A colored slice (globe edge) stays out.
inline bool PdfDarkModeV2ShouldKnockOutBlankWhiteGutter(int w, int h, int paperSides, float edgeWhiteRatio,
                                                        float satRatio, float chromaRatio) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (edgeWhiteRatio < 0.96f || paperSides < 3) {
        return false;
    }
    if (satRatio >= 0.04f || chromaRatio >= 0.05f) {
        return false;
    }
    const int minDim = w < h ? w : h;
    const int maxDim = w > h ? w : h;
    if (minDim > 48 || maxDim > 720) {
        return false;
    }
    return (long long)w * (long long)h <= (long long)48 * 720;
}

// After cream mat flood, remaining soft-gray drop shadows (no ink / no hue) read as
// narrow bright bands on a dark page. Photos keep chroma or luminance spread.
inline bool PdfDarkModeV2RemainLooksLikeSoftShadowOnly(float remainChromaRatio, float remainInkRatio,
                                                       float remainLumVar) {
    if (remainChromaRatio >= 0.08f) {
        return false;
    }
    if (remainInkRatio >= 0.05f) {
        return false;
    }
    if (remainLumVar >= 0.012f) {
        return false;
    }
    return true;
}

// Soft studio photos (RAZ "My bear"): edge flood only nibbles the rim, so the
// opaque remainder is still a paper-heavy near-rectangle with jagged bites.
// Abort knockout and keep the original card — cheaper than a second flood, and
// one linear mask pass (~1 byte/px) after the flood already paid for itself.
// remainSolidity = remainCount / remainBboxArea; remainBboxCoverage = bbox / image;
// remainPaperRatio = near-white fraction among remain samples.
// edgeWhiteRatio: when flood already cleared most of the mat, keep the cutout
// even if the subject bbox is fairly rectangular (sitting animal / soft AA).
inline bool PdfDarkModeV2RemainLooksLikeFailedRectFlood(float remainSolidity, float remainBboxCoverage,
                                                        float remainPaperRatio, float edgeWhiteRatio = 0.f) {
    if (remainSolidity < 0.88f) {
        return false;
    }
    if (remainBboxCoverage < 0.72f) {
        return false; // real silhouette cutouts shrink the remain bbox
    }
    if (remainPaperRatio < 0.22f) {
        return false; // mat already gone; leftover is subject tone
    }
    // Substantial flood progress: keep alpha even if remainder is still blocky.
    if (edgeWhiteRatio >= 0.45f) {
        return false;
    }
    return true;
}

// Soft color studio (RAZ Getting Dressed): flood cleared a lot of the card but left
// bright paper blobs (hair specular / white tee). Prefer the clean opaque card —
// a chewed cutout reads as a white "skullcap". edgeWhite can be 0.93–0.96 on large
// mats; do not exclude those.
// Do NOT abort on gray AA fringe alone: Zoo fur / scale edges always fringe that way
// after a good flood, and aborting leaves the original white rectangle.
inline bool PdfDarkModeV2RemainLooksLikeChewedSoftStudio(float edgeWhiteRatio, float remainSoftWhiteRatio,
                                                         float remainChromaRatio, float fringeGrayRatio) {
    if (edgeWhiteRatio < 0.35f || edgeWhiteRatio > 0.97f) {
        return false;
    }
    if (remainChromaRatio < 0.08f) {
        return false; // not a colorful photo cutout
    }
    // Soft-white holes are required (hair/tee). Fringe without them is a clean Zoo cutout.
    if (remainSoftWhiteRatio < 0.012f) {
        return false;
    }
    // Soft-white remnant after a real flood → highlight / clothing holes.
    if (edgeWhiteRatio >= 0.45f) {
        return true;
    }
    if (remainSoftWhiteRatio >= 0.025f && fringeGrayRatio >= 0.05f) {
        return true;
    }
    return false;
}

// Soft studio photo books (RAZ Getting Dressed): white mats crush page sat so the
// full-page "line art" gate misfires. Solid paper border + photo midtone spread
// means PictureBook, not 连环画 binarize.
inline bool PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(bool lineArtGate, float paperRatio, float lumVar,
                                                          float borderPaperRatio) {
    if (!lineArtGate) {
        return false;
    }
    if (paperRatio < 0.70f || lumVar < 0.040f || borderPaperRatio < 0.90f) {
        return false;
    }
    return true;
}

// Center of image is soft-white (tee / foam / mat showing through). Color studio
// cutout will chew it — keep the opaque card. Zoo fur is chromatic in the center.
inline bool PdfDarkModeV2ColorStudioHasSoftWhiteCenter(float centerSoftWhiteRatio) {
    return centerSoftWhiteRatio >= 0.06f;
}

// Small colorful atlas icons (flags/maps/UNIT) often have white only in the corners,
// so side-band paper ratios stay below the usual 3-side badge gate.
inline bool PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(int w, int h, int paperSides, float edgeWhiteRatio,
                                                          float satRatio, float chromaRatio) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (satRatio < 0.07f && chromaRatio < 0.10f) {
        return false;
    }
    if (edgeWhiteRatio < 0.015f || edgeWhiteRatio > 0.55f) {
        return false;
    }
    const int maxDim = w > h ? w : h;
    const long long area = (long long)w * (long long)h;
    if (maxDim > 220 || area > (long long)220 * 220) {
        return false;
    }
    // Corner-only mats can report paperSides == 0; thin rims often report 1–2.
    return paperSides <= 2;
}

// RAZ studio cutouts (ape / chimp / snake on white) are often 1500–2500 px on a side.
// The old 1200² cap skipped them → opaque white cards on Match-theme dark pages.
inline constexpr i64 kPdfDarkModeV2WhiteMatMaxArea = (i64)2800 * 2800;

inline bool PdfDarkModeV2WhiteMatDimsAllowed(int w, int h, bool thinWide) {
    if ((!thinWide && (w < 8 || h < 8)) || w <= 0 || h <= 0) {
        return false;
    }
    return (i64)w * (i64)h <= kPdfDarkModeV2WhiteMatMaxArea;
}

// Layout-textbook fast path skips full-page photo tiles. Paper-heavy picture-book
// pages (white margins + color art / text) still need PictureBook remapping.
inline bool PdfDarkModeV2LayoutFullPageNeedsPictureBookRemap(const DarkImageFeatures& f) {
    // High paper alone is enough: RAZ-A PDFdo pages are mostly white with a small
    // illustration; sat/chroma on a 128px thumb can dip under older dual gates.
    return f.highLuminanceRatio >= 0.35f;
}

// White JPEG mat / soft drop-shadow plates. Unit-tested; used by ProcessV2WhiteMat.
// inkRatio = fraction of near-black pixels (icons have ink; pure shadow plates do not).
inline bool PdfDarkModeV2ShouldKnockOutWhiteMat(int paperSides, float edgeWhiteRatio, float satRatio, float chromaRatio,
                                                float inkRatio = 0.f) {
    if (edgeWhiteRatio < 0.03f) {
        return false;
    }
    // Colorful badge/icon / studio animal on a white card.
    if (satRatio >= 0.07f || chromaRatio >= 0.10f) {
        // Empty white chips / almost-pure fragments: no real subject ink.
        const float matCap = (inkRatio >= 0.04f) ? 0.97f : 0.92f;
        if (edgeWhiteRatio > matCap) {
            return false;
        }
        if (paperSides >= 3) {
            return true;
        }
        // Sitting studio subjects often touch the bottom edge (feet) → 2 paper sides.
        // Tiny subject on a large card can also report only 1 long paper side while
        // inkRatio proves a real animal (RAZ The Zoo); open sky stays out via
        // edgeWhite≈0.30 + paperSides==1 (unit-tested).
        if (paperSides >= 2 && edgeWhiteRatio >= 0.40f) {
            return true;
        }
        if (paperSides >= 1 && inkRatio >= 0.04f && edgeWhiteRatio >= 0.45f) {
            return true;
        }
        return false;
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
// no chroma, almost one gray. V2 preserves small images, so these stay bright on a dark
// page unless filled with theme bg. meanLum is average luminance of opaque samples.
// lumVar is the luminance variance of those samples: a grayscale portrait shares the
// low chroma / low ink of a shadow plate, but its tonal spread is photographic.
inline bool PdfDarkModeV2LooksLikeSoftShadowPlate(float satRatio, float chromaRatio, float inkRatio, float meanLum,
                                                  float lumVar) {
    if (satRatio >= 0.06f || chromaRatio >= 0.10f) {
        return false;
    }
    if (inkRatio >= 0.08f) {
        return false;
    }
    if (lumVar >= 0.010f) {
        return false;
    }
    return meanLum >= 0.50f && meanLum <= 0.98f;
}

// Axis-aligned "photo" rects that are mostly white in the interior are speech-bubble /
// callout clusters (textbook cartoons), not photographs. Protecting them leaves white
// patches on a dark page. Portraits with a white mat have paper on the rim, not inside.
inline bool PdfDarkModeV2PhotoRectIsCalloutCluster(float insetPaperRatio, float photoTextureRatio, int tonalBins,
                                                   float chromaRatio) {
    if (insetPaperRatio < 0.35f) {
        return false;
    }
    // White clothes, fur, walls and oval portrait mats can make a real photo mostly
    // paper-colored. Continuous tone / local photographic grain is stronger evidence
    // than the amount of white. Flat speech bubbles and callout panels lack both.
    bool continuousTone = tonalBins >= 12 && photoTextureRatio >= 0.045f;
    bool strongTexture = photoTextureRatio >= 0.10f;
    bool colorfulTone = tonalBins >= 16 && chromaRatio >= 0.08f;
    return !(continuousTone || strongTexture || colorfulTone);
}

// Light-filled drawings (Jazz Greats TOC trumpet): interior is cream/white wash,
// not photographic grain and not mostly "paper" by the 0.88 lum gate. Protecting
// the bbox keeps that wash on the dark page. Sparse ink drawings (Meganeura) have
// high inset paper and stay protected.
inline bool PdfDarkModeV2PhotoRectIsLightIllustrationWash(float insetPaperRatio, float photoTextureRatio, float meanLum,
                                                          float chromaRatio) {
    if (insetPaperRatio >= 0.35f) {
        return false;
    }
    if (photoTextureRatio >= 0.045f) {
        return false;
    }
    if (meanLum < 0.70f) {
        return false;
    }
    if (chromaRatio >= 0.18f) {
        return false;
    }
    return true;
}

// Horizontal slices of one photo overlap in x, so their boxes should become one rect
// (a light corner such as sky is then inside the union). Side-by-side cut-outs on the
// same row do not overlap; unioning them clips the taller object where the shorter
// one ends, and that strip is ink-inverted (RAZ sweater cuff).
// Page margin vs a pale cut-out edge (glass rim, egg shell, glass-dish corner).
// The edge is outside the dense photo box, so the ink map turns it black. It matches
// the page only when both luminance and chroma sit on the page's own paper; cream
// paper therefore does not grow a halo, and a whiter glass lip still qualifies.
inline bool PdfDarkModeV2PixelMatchesPagePaper(float lum, float chroma, float pageLum, float pageChroma) {
    float dLum = lum > pageLum ? lum - pageLum : pageLum - lum;
    if (dLum >= 0.030f) {
        return false;
    }
    if (chroma >= pageChroma + 0.050f) {
        return false;
    }
    return lum > 0.80f;
}

// Body ink stays on the ink path. Everything else that is not the page paper can
// belong to a cut-out sticking out of the photo.
inline bool PdfDarkModeV2IsPaleCutoutEdgePixel(float lum, float chroma, float pageLum, float pageChroma) {
    if (PdfDarkModeV2PixelMatchesPagePaper(lum, chroma, pageLum, pageChroma)) {
        return false;
    }
    if (lum < 0.42f && chroma < 0.14f) {
        return false;
    }
    return true;
}

// Gray fringe of a glyph. The cut-out flood must not climb it into a caption.
// A glass rim next to an arrow still qualifies as the object: it has chroma.
inline bool PdfDarkModeV2IsGlyphFringePixel(float lum, float chroma, bool touchesInk) {
    if (!touchesInk) {
        return false;
    }
    if (chroma >= 0.04f || lum >= 0.92f || lum < 0.42f) {
        return false;
    }
    return true;
}

// Specular lip: same color as the page, but the neighborhood is the cut-out rather
// than the open margin. 45% of the window already kept as edge.
inline bool PdfDarkModeV2IsCutoutHighlightFringe(int maskCount, int windowCount) {
    if (windowCount <= 0 || maskCount <= 0) {
        return false;
    }
    return maskCount * 20 >= windowCount * 9;
}

inline bool PdfDarkModeV2PhotoRectsShareObjectX(int ax0, int ax1, int bx0, int bx1) {
    int ov0 = ax0 > bx0 ? ax0 : bx0;
    int ov1 = ax1 < bx1 ? ax1 : bx1;
    int ov = ov1 > ov0 ? ov1 - ov0 : 0;
    int wa = ax1 - ax0;
    int wb = bx1 - bx0;
    if (wa <= 0 || wb <= 0) {
        return false;
    }
    int shortW = wa < wb ? wa : wb;
    return ov * 5 >= shortW * 2;
}

// Color-page photo rects often swallow a display-type title on paper above the art
// (RAZ SPRAK p.2). Those rows are paper + black ink, not continuous-tone photo.
inline bool PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper(float chromaRatio, float midtoneRatio, float paperRatio) {
    if (chromaRatio >= 0.10f) {
        return false;
    }
    if (midtoneRatio >= 0.28f) {
        return false;
    }
    // Display type sits on page paper (SPRAK). A dark suit / silhouette is mostly ink
    // with little paper — Abraham Lincoln p.2 was sliced in half without this gate.
    if (paperRatio < 0.50f) {
        return false;
    }
    return true;
}

// Dark low-chroma on a low-paper tile: hair/eyes inside a photo rect must not be
// SharpDocument-whitened. Large title glyph fills (Guess That President) also make
// tilePaper low — those sit *outside* photo rects and must still invert to theme text.
inline bool PdfDarkModeV2ShouldKeepDarkNonPaperTile(float srcLum, float chroma, float tilePaperRatio,
                                                    bool inPhotoRect) {
    if (!inPhotoRect) {
        return false;
    }
    if (srcLum >= 0.48f || chroma >= 0.15f) {
        return false;
    }
    if (tilePaperRatio >= 0.42f) {
        return false;
    }
    return true;
}

// 12px box halo around oval mats painted rectangular notches into the photo.
// 3px is enough for wrapped-text AA on the mat; photo interiors stay protected.
inline bool PdfDarkModeV2PhotoHaloKeepDarkPixel(float lum, bool nearMat) {
    if (lum >= 0.58f) {
        return false;
    }
    if (!nearMat) {
        return true;
    }
    return false;
}

// High-key photograph (fog, snow, white fur). Light pixels span a ramp, so page
// lumVar stays small and the text / aged-paper gates treat the picture as a scan.
// Flat office paper sits in one band; its light-tone variance is tiny. Real text
// pages keep an ink spike. Blank gutters are not this: they have no midtone mass.
inline bool PdfDarkModeV2LooksLikeHighKeyPhotograph(float lightToneRatio, float lightToneVar, float inkRatio,
                                                    float lowMidRatio, float highMidRatio) {
    if (lightToneRatio < 0.22f || lightToneVar < 0.008f) {
        return false;
    }
    if (inkRatio >= 0.05f) {
        return false;
    }
    // One gray level (a photocopy) fills only one band. Fog, fur, and snow span both.
    if (lowMidRatio < 0.04f || highMidRatio < 0.15f) {
        return false;
    }
    return true;
}

// Full-page textbook plate that is only tinted paper (a gray rule at the top, no
// photo, no baked ink). SharpDocument blends that tint back in and the page stays
// cream; flatten it to the theme background instead. Fog and white fur fail this
// (they have saturation or a real luminance spread).
inline bool PdfDarkModeV2IsBlankPaperPlate(float paperRatio, float satRatio, float inkRatio, float lumVar) {
    if (paperRatio < 0.85f || satRatio >= 0.02f || inkRatio >= 0.01f || lumVar >= 0.025f) {
        return false;
    }
    return true;
}

// Edge-to-edge photograph, including a mostly gray one with a few saturated
// subjects (RAZ "These geese"). Okular on those saturated pixels paints false
// color. White-margin pages stay on the paper path so the margin can go dark.
inline bool PdfDarkModeV2ShouldKeepOriginalPhotograph(float paperRatio, float lumVar) {
    if (lumVar < 0.018f || paperRatio >= 0.35f) {
        return false;
    }
    return true;
}

// A colorful, continuous-tone image whose outer band is not paper is already a
// full-bleed photograph. Running photo-rect extraction on it mistakes bright sky
// or walls for page paper and produces cut-out halos. White-margin RAZ/photo-book
// pages stay on the existing paper + protected-photo path.
inline bool PdfDarkModeV2ShouldPreserveFullBleedPhoto(float borderPaperRatio, float satRatio, float chromaRatio,
                                                      float lumVar) {
    if (borderPaperRatio >= 0.50f || satRatio < 0.12f || chromaRatio < 0.20f || lumVar < 0.025f) {
        return false;
    }
    // Cream 连环画 paper (runtime 红楼梦 p.7 sat=0.163 chroma=0.697): yellow-paper
    // chroma, not a photograph. Covers keep sat≈chroma (~0.98).
    if (satRatio < 0.28f && chromaRatio >= satRatio * 2.0f) {
        return false;
    }
    return true;
}

// MRC background plates (hi-res JPEG under a JBIG2 text mask) keep faint leftover
// glyphs. PictureBook's SharpDocument chroma gate (~0.11–0.20) leaves light-brown
// JPEG ghosts on the dark page under the sharp mask → muddy double text.
// Paper-heavy pages crush those ghosts to theme bg; cover/photo pages do not.
inline bool PdfDarkModeV2ShouldCrushMrcBackgroundGhosts(float paperRatio) {
    return paperRatio >= 0.65f;
}

// Word/WPS 红头与标题: a 2×2 Indexed color chip plus a high-res SMask of the glyphs.
// Treating that chip as a "small photo" keeps original black ink on a dark page.
inline bool PdfDarkModeV2LooksLikeSoftMaskPaintChip(int colorW, int colorH, int maskW, int maskH) {
    if (colorW < 1 || colorH < 1 || maskW < 1 || maskH < 1) {
        return false;
    }
    if (colorW <= 8 && colorH <= 8 && maskW >= 32 && maskH >= 32) {
        return true;
    }
    i64 colorN = (i64)colorW * (i64)colorH;
    i64 maskN = (i64)maskW * (i64)maskH;
    return maskN >= colorN * 64 && maskW >= 32 && maskH >= 32;
}

// Light leftover JPEG text: high lum, low-to-mid chroma (brown ghosts, cream paper).
// Dark ink and high-chroma art (orange sidebar, cartoons) stay out.
inline bool PdfDarkModeV2IsMrcBackgroundGhostPixel(float lum, float chroma) {
    return lum > 0.55f && chroma < 0.32f;
}

void PdfDarkModeV2FlushPerfLog();

fz_device* PdfDarkModeWrapV2Device(fz_context* ctx, fz_device* inner, const DarkModePalette* palette,
                                   const RectF& pageBounds, DarkModeEngineCache* engineCache = nullptr,
                                   u32 profileHash = 0);
