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

    // White-mat knockout gate: UNIT/Atlas badges + circle icons vs open-sky / B&W photos.
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.22f, 0.18f, 0.25f));
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(3, 0.15f, 0.10f, 0.08f));
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.70f, 0.20f, 0.25f));         // circle "2" in square
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.75f, 0.02f, 0.03f, 0.02f));  // soft drop-shadow plate
    utassert(PdfDarkModeV2ShouldKnockOutWhiteMat(2, 0.40f, 0.02f, 0.03f, 0.02f));  // L-shaped shadow
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(1, 0.30f, 0.20f, 0.25f));        // colorful + one side = sky
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.01f, 0.20f, 0.25f));        // no mat
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.95f, 0.20f, 0.25f));        // almost pure white fragment
    utassert(!PdfDarkModeV2ShouldKnockOutWhiteMat(4, 0.20f, 0.02f, 0.04f, 0.20f)); // B&W photo/ink

    // Soft shadow paints must not Okular-invert into light fringes.
    utassert(PdfDarkModeV2IsSoftShadowPaint(0.f, 0.f, 0.f, 0.35f));
    utassert(PdfDarkModeV2IsSoftShadowPaint(0.2f, 0.2f, 0.2f, 0.5f));
    utassert(PdfDarkModeV2IsSoftShadowPaint(0.85f, 0.85f, 0.85f, 0.4f));
    utassert(!PdfDarkModeV2IsSoftShadowPaint(0.f, 0.f, 0.f, 1.f));     // solid black fill
    utassert(!PdfDarkModeV2IsSoftShadowPaint(0.9f, 0.1f, 0.1f, 0.5f)); // colored translucent

    // Raster soft-shadow plates (Glencoe Social Studies ONLINE callout).
    utassert(PdfDarkModeV2LooksLikeSoftShadowPlate(0.0f, 0.0f, 0.0f, 0.60f));
    utassert(PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.01f, 0.80f));
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.10f, 0.12f, 0.01f, 0.70f)); // colorful
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.20f, 0.70f)); // ink/icon
    utassert(!PdfDarkModeV2LooksLikeSoftShadowPlate(0.02f, 0.03f, 0.01f, 0.30f)); // dark plate

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
}
