/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "PdfDarkModeV2.h"

#include <math.h>

#include "utils/UtAssert.h"

static DarkModePalette TestPalette() {
    DarkModePalette p;
    p.textR = 0.90f;
    p.textG = 0.90f;
    p.textB = 0.88f;
    p.bgR = 0.12f;
    p.bgG = 0.12f;
    p.bgB = 0.12f;
    p.diffR = p.bgR - p.textR;
    p.diffG = p.bgG - p.textG;
    p.diffB = p.bgB - p.textB;
    return p;
}

static float Luma(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static void ExpectNear(float a, float b, float eps = 0.02f) {
    utassert(fabsf(a - b) < eps);
}

void PdfDarkModeV2_UnitTests() {
    DarkModePalette palette = TestPalette();
    float out[3] = {};

    // Paper ↔ ink endpoints map to theme bg / text.
    MapRgbDarkModeV2(0.f, 0.f, 0.f, palette, out);
    ExpectNear(out[0], palette.textR);
    ExpectNear(out[1], palette.textG);
    ExpectNear(out[2], palette.textB);

    MapRgbDarkModeV2(1.f, 1.f, 1.f, palette, out);
    ExpectNear(out[0], palette.bgR);
    ExpectNear(out[1], palette.bgG);
    ExpectNear(out[2], palette.bgB);

    // Mid gray lands between theme text and bg (inverted lightness).
    MapRgbDarkModeV2(0.5f, 0.5f, 0.5f, palette, out);
    float midL = Luma(out[0], out[1], out[2]);
    float textL = Luma(palette.textR, palette.textG, palette.textB);
    float bgL = Luma(palette.bgR, palette.bgG, palette.bgB);
    utassert(midL > bgL + 0.05f);
    utassert(midL < textL - 0.05f);

    // Gray ramp stays ordered after remap (darker input → lighter output in dark theme).
    float prev = 2.f;
    for (int i = 0; i <= 8; i++) {
        float g = (float)i / 8.f;
        MapRgbDarkModeV2(g, g, g, palette, out);
        float l = Luma(out[0], out[1], out[2]);
        utassert(l <= prev + 0.001f);
        prev = l;
    }

    // Hue preserved: red stays red-dominant, cyan stays green/blue-dominant.
    MapRgbDarkModeV2(0.85f, 0.15f, 0.12f, palette, out);
    utassert(out[0] > out[1] && out[0] > out[2]);

    MapRgbDarkModeV2(0.12f, 0.55f, 0.48f, palette, out);
    utassert(out[1] > out[0] && out[1] > out[2]);

    // Out-of-range inputs clamp; still finite theme colors.
    MapRgbDarkModeV2(-0.25f, 1.25f, 0.5f, palette, out);
    utassert(out[0] >= 0.f && out[0] <= 1.f);
    utassert(out[1] >= 0.f && out[1] <= 1.f);
    utassert(out[2] >= 0.f && out[2] <= 1.f);

    // Near-paper / near-ink stay close to theme endpoints (连环画 ink+paper).
    MapRgbDarkModeV2(0.96f, 0.95f, 0.93f, palette, out);
    utassert(Luma(out[0], out[1], out[2]) < bgL + 0.12f);

    MapRgbDarkModeV2(0.05f, 0.05f, 0.05f, palette, out);
    utassert(Luma(out[0], out[1], out[2]) > textL - 0.12f);

    // Full-page fallback matches vector remap (whole-image Okular→theme).
    MapRgbDarkModeV2PageImage(1.f, 1.f, 1.f, palette, out);
    ExpectNear(out[0], palette.bgR);

    MapRgbDarkModeV2PageImage(0.f, 0.f, 0.f, palette, out);
    ExpectNear(out[0], palette.textR);

    MapRgbDarkModeV2PageImage(0.85f, 0.15f, 0.12f, palette, out);
    utassert(out[0] > out[1] && out[0] > out[2]);

    // Layout textbook fast path: skip photo tiles, keep badge/flag cutout.
    utassert(!PdfDarkModeV2LayoutTextbookSkipFigureRemap(80, 64, 0.01f));   // flag chip
    utassert(!PdfDarkModeV2LayoutTextbookSkipFigureRemap(142, 142, 0.03f)); // UNIT badge
    utassert(!PdfDarkModeV2LayoutTextbookSkipFigureRemap(390, 68, 0.04f));  // title pill
    utassert(!PdfDarkModeV2LayoutTextbookSkipFigureRemap(5, 578, 0.02f));   // thin shadow
    utassert(!PdfDarkModeV2LayoutTextbookSkipFigureRemap(34, 697, 0.01f));  // Guide wave
    utassert(PdfDarkModeV2LayoutTextbookSkipFigureRemap(640, 480, 0.08f));  // photo tile
    utassert(PdfDarkModeV2LayoutTextbookSkipFigureRemap(600, 800, 0.55f));

    // Corner-only mats on small atlas icons (flag/map/UNIT) — sides stay low.
    utassert(PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(94, 41, 1, 0.026f, 0.97f, 0.98f));
    utassert(PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(34, 53, 1, 0.024f, 0.93f, 0.94f));
    utassert(PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(129, 76, 0, 0.050f, 0.88f, 0.90f));
    utassert(!PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(640, 480, 1, 0.05f, 0.40f, 0.50f)); // photo
    utassert(!PdfDarkModeV2ShouldKnockOutSmallIconCornerMat(80, 64, 1, 0.005f, 0.90f, 0.90f));  // no mat

    // Warm sidebar parchment is artwork. Low-chroma cream mats and cyan ocean still knock out.
    utassert(PdfDarkModeV2IsWarmDecorativeParchment(0.94f, 0.88f, 0.63f));  // Guide plate
    utassert(!PdfDarkModeV2IsWarmDecorativeParchment(0.94f, 0.94f, 0.82f)); // Visual Summary cream
    utassert(!PdfDarkModeV2IsWarmDecorativeParchment(0.70f, 0.85f, 0.95f)); // cyan ocean
    utassert(!PdfDarkModeV2IsWarmDecorativeParchment(0.0f, 0.63f, 0.31f));  // green display type
    utassert(PdfDarkModeV2IsWarmStripArtwork(247.f / 255.f, 246.f / 255.f, 204.f / 255.f));
    utassert(!PdfDarkModeV2IsWarmStripArtwork(1.f, 1.f, 1.f));
    utassert(!PdfDarkModeV2IsWarmStripArtwork(170.f / 255.f, 198.f / 255.f, 215.f / 255.f));
    utassert(PdfDarkModeV2IsWarmNeutralShadow(0.69f, 0.63f, 0.50f));  // badge JPEG shadow
    utassert(PdfDarkModeV2IsWarmNeutralShadow(0.56f, 0.56f, 0.44f));  // glyph drop shadow
    utassert(!PdfDarkModeV2IsWarmNeutralShadow(0.0f, 0.63f, 0.31f));  // green ink
    utassert(!PdfDarkModeV2IsWarmNeutralShadow(0.05f, 0.31f, 0.50f)); // blue stroke

    // Tall Guide-to-Reading wave: white only on the open side (aspect >= 5).
    utassert(PdfDarkModeV2ShouldKnockOutDecorativeStripMat(34, 697, 1, 0.19f, 0.81f, 0.83f));
    utassert(PdfDarkModeV2ShouldKnockOutDecorativeStripMat(33, 253, 1, 0.12f, 0.70f, 0.75f));
    utassert(!PdfDarkModeV2ShouldKnockOutDecorativeStripMat(38, 101, 2, 0.27f, 0.73f, 0.76f)); // aspect < 5
    utassert(!PdfDarkModeV2ShouldKnockOutDecorativeStripMat(68, 87, 3, 0.27f, 0.64f, 0.68f));  // not a strip
    utassert(!PdfDarkModeV2ShouldKnockOutDecorativeStripMat(34, 697, 1, 0.19f, 0.02f, 0.03f)); // gray photo

    // Visual Summary cream gutter (6×63) is almost all paper — usual 0.92 cap rejects it.
    utassert(PdfDarkModeV2ShouldKnockOutAlmostPaperChip(3, 89, 4, 1.f, 0.f, 0.f));
    utassert(PdfDarkModeV2ShouldKnockOutAlmostPaperChip(11, 129, 4, 1.f, 0.f, 0.f));  // white column
    utassert(!PdfDarkModeV2ShouldKnockOutAlmostPaperChip(16, 80, 4, 1.f, 0.f, 0.f));  // not tall enough
    utassert(!PdfDarkModeV2ShouldKnockOutAlmostPaperChip(20, 140, 4, 1.f, 0.f, 0.f)); // wider than a gutter
    utassert(PdfDarkModeV2ShouldKnockOutBlankWhiteGutter(30, 349, 4, 1.f, 0.f, 0.f));
    utassert(PdfDarkModeV2ShouldKnockOutBlankWhiteGutter(44, 70, 4, 1.f, 0.f, 0.f));
    utassert(!PdfDarkModeV2ShouldKnockOutBlankWhiteGutter(35, 102, 2, 0.70f, 0.20f, 0.30f));
    utassert(!PdfDarkModeV2ShouldKnockOutBlankWhiteGutter(200, 200, 4, 1.f, 0.f, 0.f));
    utassert(!PdfDarkModeV2ShouldKnockOutBlankWhiteGutter(30, 349, 4, 1.f, 0.10f, 0.10f));
    utassert(PdfDarkModeV2ShouldKnockOutAlmostPaperChip(6, 63, 4, 1.00f, 1.00f, 1.00f));
    utassert(PdfDarkModeV2ShouldKnockOutAlmostPaperChip(8, 80, 3, 0.96f, 0.80f, 0.90f));
    utassert(!PdfDarkModeV2ShouldKnockOutAlmostPaperChip(68, 87, 4, 0.95f, 0.20f, 0.25f)); // badge, not a chip
    utassert(!PdfDarkModeV2ShouldKnockOutAlmostPaperChip(6, 63, 4, 0.50f, 1.00f, 1.00f));  // not almost-paper

    // Cream flood leftovers that are only soft gray shadow (not photo cutouts).
    utassert(PdfDarkModeV2RemainLooksLikeSoftShadowOnly(0.02f, 0.01f, 0.002f));
    utassert(!PdfDarkModeV2RemainLooksLikeSoftShadowOnly(0.20f, 0.01f, 0.002f)); // teal jacket edge
    utassert(!PdfDarkModeV2RemainLooksLikeSoftShadowOnly(0.02f, 0.10f, 0.002f)); // ink
    utassert(!PdfDarkModeV2RemainLooksLikeSoftShadowOnly(0.02f, 0.01f, 0.040f)); // photo tone spread

    // Soft studio photo: edge flood left a paper-heavy near-rectangle → abort.
    utassert(PdfDarkModeV2RemainLooksLikeFailedRectFlood(0.94f, 0.88f, 0.40f));
    utassert(PdfDarkModeV2RemainLooksLikeFailedRectFlood(0.90f, 0.75f, 0.25f));
    utassert(!PdfDarkModeV2RemainLooksLikeFailedRectFlood(0.70f, 0.90f, 0.40f)); // silhouette holey
    utassert(!PdfDarkModeV2RemainLooksLikeFailedRectFlood(0.95f, 0.50f, 0.40f)); // mat mostly gone
    utassert(!PdfDarkModeV2RemainLooksLikeFailedRectFlood(0.95f, 0.90f, 0.05f)); // subject only
    // RAZ The Zoo: flood cleared most of the card — keep cutout even if blocky.
    utassert(!PdfDarkModeV2RemainLooksLikeFailedRectFlood(0.92f, 0.80f, 0.30f, 0.55f));

    // Soft color studio (Getting Dressed): soft-white remnant → abort.
    utassert(PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.60f, 0.02f, 0.20f, 0.06f));
    utassert(PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.40f, 0.03f, 0.25f, 0.12f));  // soft-white + fringe
    utassert(PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.94f, 0.015f, 0.20f, 0.02f)); // large mat + specular
    // Zoo fur AA fringe without soft-white holes → keep the cutout.
    utassert(!PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.55f, 0.005f, 0.20f, 0.15f));
    utassert(!PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.80f, 0.008f, 0.25f, 0.20f));
    utassert(!PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.55f, 0.005f, 0.20f, 0.03f)); // clean animal cutout
    utassert(!PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.30f, 0.06f, 0.20f, 0.20f));  // mat barely flooded
    utassert(!PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.60f, 0.06f, 0.02f, 0.20f));  // gray cutout
    utassert(!PdfDarkModeV2RemainLooksLikeChewedSoftStudio(0.98f, 0.06f, 0.20f, 0.20f));  // empty chip

    // Soft studio RAZ pages must not take the line-art full-page gate.
    utassert(PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(true, 0.82f, 0.048f, 1.00f));
    utassert(PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(true, 0.84f, 0.047f, 0.95f));
    utassert(!PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(false, 0.82f, 0.048f, 1.00f));
    utassert(!PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(true, 0.60f, 0.048f, 1.00f)); // not mat-heavy
    utassert(!PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(true, 0.82f, 0.030f, 1.00f)); // flat line-art
    utassert(!PdfDarkModeV2LineArtGateIsSoftStudioPhotoBook(true, 0.82f, 0.048f, 0.50f)); // no paper border

    // White-mat knockout gate: UNIT/Atlas badges + circle icons vs open-sky / B&W photos.
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.22f, 0.18f, 0.25f));
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(3, 0.15f, 0.10f, 0.08f));
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.70f, 0.20f, 0.25f));        // circle "2" in square
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.75f, 0.02f, 0.03f, 0.02f)); // soft drop-shadow plate
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(2, 0.40f, 0.02f, 0.03f, 0.02f)); // L-shaped shadow
    // RAZ The Zoo: colorful animal on a white card, feet may touch the bottom edge.
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(2, 0.55f, 0.25f, 0.30f, 0.08f));
    // Small subject on a large white JPEG: flooded mat >0.92 but ink proves a cutout.
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(3, 0.94f, 0.25f, 0.30f, 0.05f));
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(1, 0.50f, 0.25f, 0.30f, 0.06f));
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(1, 0.30f, 0.20f, 0.25f));        // colorful + one side = sky
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(2, 0.20f, 0.20f, 0.25f));        // colorful, mat too thin
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.01f, 0.20f, 0.25f));        // no mat
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.95f, 0.20f, 0.25f));        // almost pure white, no ink
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.20f, 0.02f, 0.04f, 0.20f)); // B&W photo/ink

    // Dims: RAZ studio cutouts (~1600×1200) must be eligible; giant scans still skip.
    utassert(PdfDarkModeV2WhiteMatDimsAllowed(1600, 1200, false));
    utassert(PdfDarkModeV2WhiteMatDimsAllowed(2400, 2400, false));
    utassert(!PdfDarkModeV2WhiteMatDimsAllowed(3000, 3000, false));
    utassert(!PdfDarkModeV2WhiteMatDimsAllowed(4, 4, false));

    // Layout fast-path: full-bleed photo stays original; paper-heavy pages remapped.
    DarkImageFeatures photoBleed{};
    photoBleed.highLuminanceRatio = 0.20f;
    photoBleed.saturatedPixelRatio = 0.40f;
    photoBleed.chromaticPixelRatio = 0.50f;
    utassert(!PdfDarkModeV2LayoutFullPageNeedsPictureBookRemap(photoBleed));
    DarkImageFeatures razCover{};
    razCover.highLuminanceRatio = 0.72f;
    razCover.saturatedPixelRatio = 0.12f;
    razCover.chromaticPixelRatio = 0.18f;
    utassert(PdfDarkModeV2LayoutFullPageNeedsPictureBookRemap(razCover));
    DarkImageFeatures grayScan{};
    grayScan.highLuminanceRatio = 0.90f;
    grayScan.saturatedPixelRatio = 0.01f;
    grayScan.chromaticPixelRatio = 0.02f;
    utassert(PdfDarkModeV2LayoutFullPageNeedsPictureBookRemap(grayScan));
    DarkImageFeatures pondPage{};
    pondPage.highLuminanceRatio = 0.53f;
    pondPage.saturatedPixelRatio = 0.02f;
    pondPage.chromaticPixelRatio = 0.03f;
    utassert(PdfDarkModeV2LayoutFullPageNeedsPictureBookRemap(pondPage));

    // Soft shadow paints must not Okular-invert into light fringes.
    utassert(PdfDarkModeV2IsSoftShadowPaint(0.f, 0.f, 0.f, 0.35f));
    utassert(PdfDarkModeV2IsSoftShadowPaint(0.2f, 0.2f, 0.2f, 0.5f));
    utassert(PdfDarkModeV2IsSoftShadowPaint(0.85f, 0.85f, 0.85f, 0.4f));
    utassert(!PdfDarkModeV2IsSoftShadowPaint(0.f, 0.f, 0.f, 1.f));     // solid black fill
    utassert(!PdfDarkModeV2IsSoftShadowPaint(0.9f, 0.1f, 0.1f, 0.5f)); // colored translucent

    // Raster soft-shadow plates (Glencoe Social Studies ONLINE callout).
    utassert(PdfDarkModeV2LooksLikeSoftShadowPlate(0.0f, 0.0f, 0.0f, 0.60f, 0.001f));
    utassert(PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.01f, 0.80f, 0.002f));
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.10f, 0.12f, 0.01f, 0.70f, 0.001f)); // colorful
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.20f, 0.70f, 0.001f)); // ink/icon
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.01f, 0.30f, 0.001f)); // dark plate
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.01f, 0.62f, 0.040f)); // grayscale portrait

    // Speech-bubble clusters vs portrait mats: interior paper, not the rim.
    utassert(PdfDarkModeV2PhotoRectIsCalloutCluster(0.50f, 0.01f, 5, 0.02f));
    utassert(PdfDarkModeV2PhotoRectIsCalloutCluster(0.35f, 0.02f, 8, 0.04f));
    utassert(!PdfDarkModeV2PhotoRectIsCalloutCluster(0.20f, 0.01f, 5, 0.02f));
    utassert(!PdfDarkModeV2PhotoRectIsCalloutCluster(0.10f, 0.01f, 5, 0.02f));
    // White-heavy photographs retain continuous tone / local texture and must not
    // be discarded as callout clusters before edge-connected mat removal.
    utassert(!PdfDarkModeV2PhotoRectIsCalloutCluster(0.72f, 0.08f, 24, 0.18f));
    utassert(!PdfDarkModeV2PhotoRectIsCalloutCluster(0.80f, 0.11f, 18, 0.03f));
    // Jazz Greats trumpet: cream fill, no grain. Meganeura: high inset paper.
    utassert(PdfDarkModeV2PhotoRectIsLightIllustrationWash(0.20f, 0.00f, 0.835f, 0.04f));
    utassert(!PdfDarkModeV2PhotoRectIsLightIllustrationWash(0.70f, 0.00f, 0.90f, 0.02f));
    utassert(!PdfDarkModeV2PhotoRectIsLightIllustrationWash(0.20f, 0.10f, 0.835f, 0.04f));
    utassert(!PdfDarkModeV2PhotoRectIsLightIllustrationWash(0.20f, 0.00f, 0.40f, 0.04f));
    utassert(!PdfDarkModeV2PhotoRectIsLightIllustrationWash(0.20f, 0.00f, 0.835f, 0.22f));

    // Fog / snow / white fur: light-tone ramp, both mid bands, almost no ink.
    utassert(PdfDarkModeV2LooksLikeHighKeyPhotograph(0.60f, 0.010f, 0.008f, 0.13f, 0.48f));
    utassert(PdfDarkModeV2LooksLikeHighKeyPhotograph(0.56f, 0.016f, 0.009f, 0.16f, 0.42f));
    // Mostly-white text page: light mass is the paper spike, not a ramp.
    utassert(!PdfDarkModeV2LooksLikeHighKeyPhotograph(0.06f, 0.037f, 0.016f, 0.04f, 0.03f));
    // One-level gray photocopy, and a text page with a real ink spike.
    utassert(!PdfDarkModeV2LooksLikeHighKeyPhotograph(0.70f, 0.002f, 0.020f, 0.02f, 0.68f));
    utassert(!PdfDarkModeV2LooksLikeHighKeyPhotograph(0.40f, 0.020f, 0.12f, 0.15f, 0.25f));
    // Blank gutter stats are not a photograph (no midtone mass).
    utassert(!PdfDarkModeV2LooksLikeHighKeyPhotograph(0.02f, 0.001f, 0.00f, 0.00f, 0.01f));
    // Cream textbook plate: flat, no ink. Fog (sat 0.04) and a text scan (ink) stay out.
    utassert(PdfDarkModeV2IsBlankPaperPlate(0.93f, 0.00f, 0.00f, 0.011f));
    utassert(!PdfDarkModeV2IsBlankPaperPlate(0.84f, 0.043f, 0.007f, 0.024f));
    utassert(!PdfDarkModeV2IsBlankPaperPlate(0.90f, 0.00f, 0.06f, 0.010f));
    utassert(!PdfDarkModeV2IsBlankPaperPlate(0.90f, 0.00f, 0.00f, 0.040f));

    // Mostly gray full-bleed photo (RAZ geese): keep original, do not Okular it.
    utassert(PdfDarkModeV2ShouldKeepOriginalPhotograph(0.12f, 0.045f));
    utassert(!PdfDarkModeV2ShouldKeepOriginalPhotograph(0.80f, 0.045f));
    utassert(!PdfDarkModeV2ShouldKeepOriginalPhotograph(0.10f, 0.008f));

    // Full-bleed photo: colorful continuous tone with no white-paper frame.
    utassert(PdfDarkModeV2ShouldPreserveFullBleedPhoto(0.15f, 0.55f, 0.64f, 0.080f));
    // Photo inset on white paper (photo book / every tested RAZ page): keep rect protection.
    utassert(!PdfDarkModeV2ShouldPreserveFullBleedPhoto(1.00f, 0.55f, 0.64f, 0.080f));
    // Borderless flat artwork and low-color scans are not automatically photos.
    utassert(!PdfDarkModeV2ShouldPreserveFullBleedPhoto(0.10f, 0.05f, 0.08f, 0.010f));
    // 红楼梦连环画 cream paper (runtime p.7 / p.56): not a full-bleed photograph.
    utassert(!PdfDarkModeV2ShouldPreserveFullBleedPhoto(0.016f, 0.163f, 0.697f, 0.053f));
    utassert(!PdfDarkModeV2ShouldPreserveFullBleedPhoto(0.310f, 0.161f, 0.339f, 0.050f));
    // Color cover of the same PDF must still preserve original pixels.
    utassert(PdfDarkModeV2ShouldPreserveFullBleedPhoto(0.00f, 0.977f, 0.993f, 0.044f));

    // MRC leftover-text crush: paper-heavy backgrounds only; keep cover photos.
    utassert(PdfDarkModeV2ShouldCrushMrcBackgroundGhosts(0.723f));
    utassert(PdfDarkModeV2ShouldCrushMrcBackgroundGhosts(0.922f));
    utassert(!PdfDarkModeV2ShouldCrushMrcBackgroundGhosts(0.100f));
    utassert(PdfDarkModeV2IsMrcBackgroundGhostPixel(0.70f, 0.20f));  // light-brown leftover
    utassert(PdfDarkModeV2IsMrcBackgroundGhostPixel(0.95f, 0.00f));  // paper
    utassert(!PdfDarkModeV2IsMrcBackgroundGhostPixel(0.35f, 0.75f)); // orange sidebar
    utassert(!PdfDarkModeV2IsMrcBackgroundGhostPixel(0.20f, 0.05f)); // dark ink
    utassert(!PdfDarkModeV2IsMrcBackgroundGhostPixel(0.70f, 0.50f)); // colorful midtone

    // Word 赣税函: 2×2 Indexed chip + SMask of 方正小标宋 / title glyphs.
    utassert(PdfDarkModeV2LooksLikeSoftMaskPaintChip(2, 2, 3809, 497));
    utassert(PdfDarkModeV2LooksLikeSoftMaskPaintChip(2, 2, 3118, 218));
    utassert(PdfDarkModeV2LooksLikeSoftMaskPaintChip(2, 2, 368, 218));
    utassert(!PdfDarkModeV2LooksLikeSoftMaskPaintChip(2, 2, 2, 2));
    utassert(!PdfDarkModeV2LooksLikeSoftMaskPaintChip(64, 64, 64, 64));
    utassert(!PdfDarkModeV2LooksLikeSoftMaskPaintChip(2, 2, 0, 218));

    // Same photo, stacked slices: overlap in x. Side-by-side cut-outs do not.
    // White page: glass rim and egg shell are not paper; the margin and body ink are not edges.
    utassert(PdfDarkModeV2IsPaleCutoutEdgePixel(0.90f, 0.06f, 1.00f, 0.00f));
    utassert(PdfDarkModeV2IsPaleCutoutEdgePixel(0.965f, 0.02f, 1.00f, 0.00f));
    utassert(!PdfDarkModeV2IsPaleCutoutEdgePixel(0.995f, 0.01f, 1.00f, 0.00f));
    utassert(!PdfDarkModeV2IsPaleCutoutEdgePixel(0.20f, 0.02f, 1.00f, 0.00f));
    // Cream paper matches itself, so the margin does not become a preserved halo.
    utassert(!PdfDarkModeV2IsPaleCutoutEdgePixel(0.93f, 0.04f, 0.93f, 0.04f));
    utassert(PdfDarkModeV2IsPaleCutoutEdgePixel(0.84f, 0.08f, 0.93f, 0.04f));
    utassert(PdfDarkModeV2IsGlyphFringePixel(0.60f, 0.01f, true));
    utassert(!PdfDarkModeV2IsGlyphFringePixel(0.90f, 0.08f, true)); // glass rim beside an arrow
    utassert(!PdfDarkModeV2IsGlyphFringePixel(0.60f, 0.01f, false));
    utassert(PdfDarkModeV2IsCutoutHighlightFringe(23, 49));
    utassert(!PdfDarkModeV2IsCutoutHighlightFringe(22, 49));
    utassert(!PdfDarkModeV2IsCutoutHighlightFringe(0, 49));

    utassert(PdfDarkModeV2PhotoRectsShareObjectX(100, 400, 200, 500));
    utassert(PdfDarkModeV2PhotoRectsShareObjectX(20, 800, 200, 800));
    utassert(!PdfDarkModeV2PhotoRectsShareObjectX(165, 413, 748, 1115));
    utassert(!PdfDarkModeV2PhotoRectsShareObjectX(10, 10, 0, 100));

    // RAZ SPRAK p.2 title row vs illustration / B&W portrait (Lincoln suit has little paper).
    utassert(PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper(0.02f, 0.04f, 0.70f));
    utassert(PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper(0.00f, 0.00f, 1.00f)); // white gap under the title
    utassert(!PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper(0.25f, 0.40f, 0.15f));
    utassert(!PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper(0.04f, 0.45f, 0.20f));
    utassert(!PdfDarkModeV2PhotoRectRowLooksLikeInkOnPaper(0.02f, 0.05f, 0.08f)); // dark suit / silhouette

    // Oval portrait poles: keep dark hair in the mat halo; invert ink that sits on paper.
    utassert(PdfDarkModeV2PhotoHaloKeepDarkPixel(0.20f, false));  // hair away from mat
    utassert(!PdfDarkModeV2PhotoHaloKeepDarkPixel(0.18f, true));  // wrapped text on mat
    utassert(!PdfDarkModeV2PhotoHaloKeepDarkPixel(0.40f, true));  // near-mat gray uses ink/paper map
    utassert(!PdfDarkModeV2PhotoHaloKeepDarkPixel(0.90f, true));  // white mat
    utassert(!PdfDarkModeV2PhotoHaloKeepDarkPixel(0.80f, false)); // light AA still remaps

    // Dark non-paper tiles: hair inside a photo; fat title fills outside must invert.
    utassert(PdfDarkModeV2ShouldKeepDarkNonPaperTile(0.20f, 0.05f, 0.20f, true));
    utassert(!PdfDarkModeV2ShouldKeepDarkNonPaperTile(0.20f, 0.05f, 0.20f, false)); // title outside photo
    utassert(!PdfDarkModeV2ShouldKeepDarkNonPaperTile(0.20f, 0.05f, 0.55f, true));  // body text on paper
    utassert(!PdfDarkModeV2ShouldKeepDarkNonPaperTile(0.60f, 0.05f, 0.20f, true));  // not dark
}
