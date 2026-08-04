/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "PdfDarkMode.h"

#include "utils/UtAssert.h"

static DarkImageFeatures PhotoLikeFeatures() {
    DarkImageFeatures f;
    f.isColorful = true;
    f.colorBucketRatio = 0.04f;
    f.highLuminanceRatio = 0.28f;
    f.saturatedPixelRatio = 0.18f;
    f.luminanceVariance = 0.022f;
    f.borderLightRatio = 0.35f;
    f.borderUniformity = 0.40f;
    f.flatAreaRatio = 0.12f;
    return f;
}

static DarkImageFeatures LightBackgroundArtFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 0.015f;
    f.highLuminanceRatio = 0.58f;
    f.saturatedPixelRatio = 0.04f;
    f.luminanceVariance = 0.008f;
    f.borderLightRatio = 0.78f;
    f.borderUniformity = 0.82f;
    f.flatAreaRatio = 0.62f;
    return f;
}

static DarkImageFeatures IconFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 0.008f;
    f.highLuminanceRatio = 0.68f;
    f.saturatedPixelRatio = 0.02f;
    f.luminanceVariance = 0.006f;
    f.borderLightRatio = 0.55f;
    f.borderUniformity = 0.50f;
    f.flatAreaRatio = 0.70f;
    return f;
}

static DarkImageFeatures BrightFilmStillFeatures() {
    DarkImageFeatures f;
    f.isColorful = true;
    f.colorBucketRatio = 22.f / 4096.f;
    f.highLuminanceRatio = 0.62f;
    f.saturatedPixelRatio = 0.22f;
    f.chromaticPixelRatio = 0.28f;
    f.luminanceVariance = 0.021f;
    f.borderLightRatio = 0.72f;
    f.borderUniformity = 0.75f;
    f.flatAreaRatio = 0.40f;
    return f;
}

// RAZ-style soft picture-book page: bright margins + colorful art below LookLikePhoto's
// sat>=0.18 bar, but still clearly illustration (must Preserve, not FullPageScan).
static DarkImageFeatures SoftPictureBookFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = true;
    f.colorBucketRatio = 28.f / 4096.f;
    f.highLuminanceRatio = 0.64f;
    f.saturatedPixelRatio = 0.14f;
    f.chromaticPixelRatio = 0.22f;
    f.luminanceVariance = 0.016f;
    f.borderLightRatio = 0.70f;
    f.borderUniformity = 0.55f;
    f.flatAreaRatio = 0.28f;
    return f;
}

static DarkImageFeatures TruePaperScanFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 10.f / 4096.f;
    f.highLuminanceRatio = 0.70f;
    f.saturatedPixelRatio = 0.05f;
    f.chromaticPixelRatio = 0.04f;
    f.luminanceVariance = 0.009f;
    f.borderLightRatio = 0.85f;
    f.borderUniformity = 0.80f;
    f.flatAreaRatio = 0.55f;
    return f;
}

// DuXiu / SuperStar textbook scan: white paper + text ink variance (high lumVar).
// Must be FullPageScan / AdaptiveDocument — never Photo → picture-book path.
static DarkImageFeatures DuXiuTextScanFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 22.f / 4096.f;
    f.highLuminanceRatio = 0.86f;
    f.saturatedPixelRatio = 0.03f;
    f.chromaticPixelRatio = 0.04f;
    f.luminanceVariance = 0.045f;
    f.borderLightRatio = 0.90f;
    f.borderUniformity = 0.75f;
    f.flatAreaRatio = 0.52f;
    return f;
}

// Historical B&W portrait (e.g. White House Pets): low sat, real tonal range — must Preserve.
static DarkImageFeatures GrayscalePortraitFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 8.f / 4096.f;
    f.highLuminanceRatio = 0.35f;
    f.saturatedPixelRatio = 0.02f;
    f.chromaticPixelRatio = 0.03f;
    f.luminanceVariance = 0.028f;
    f.borderLightRatio = 0.40f;
    f.borderUniformity = 0.35f;
    f.flatAreaRatio = 0.18f;
    return f;
}

void PdfDarkModeImageClassifier_UnitTests() {
    float confidence = 0.f;

    DarkImageKind kind = PdfDarkModeClassifyImageFeatures(PhotoLikeFeatures(), 0.22f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(confidence >= 0.6f);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    kind = PdfDarkModeClassifyImageFeatures(LightBackgroundArtFeatures(), 0.35f, false, &confidence);
    utassert(kind == DarkImageKind::LightBackgroundArtwork);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);

    kind = PdfDarkModeClassifyImageFeatures(IconFeatures(), 0.02f, false, &confidence);
    utassert(kind == DarkImageKind::IconOrLineArt);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);

    kind = PdfDarkModeClassifyImageFeatures(BrightFilmStillFeatures(), 0.08f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    DarkImageFeatures scan = PhotoLikeFeatures();
    scan.highLuminanceRatio = 0.62f;
    kind = PdfDarkModeClassifyImageFeatures(scan, 0.88f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);
    utassert(PdfDarkModeShouldPreserveImageFeatures(scan, 0.88f));

    DarkImageFeatures flatScan = LightBackgroundArtFeatures();
    flatScan.highLuminanceRatio = 0.62f;
    kind = PdfDarkModeClassifyImageFeatures(flatScan, 0.88f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);

    // Soft picture-book full-bleed: Preserve (grey paper + keep art), not AdaptiveDocument.
    kind = PdfDarkModeClassifyImageFeatures(SoftPictureBookFullBleedFeatures(), 0.90f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);
    utassert(PdfDarkModeShouldPreserveImageFeatures(SoftPictureBookFullBleedFeatures(), 0.90f));

    // Soft art must not be classified as layout background / LightBg.
    kind = PdfDarkModeClassifyImageFeatures(SoftPictureBookFullBleedFeatures(), 0.40f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    kind = PdfDarkModeClassifyImageFeatures(TruePaperScanFullBleedFeatures(), 0.90f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);

    // High-variance text scan (Merck / DuXiu): FullPageScan, not grayscale Photo.
    kind = PdfDarkModeClassifyImageFeatures(DuXiuTextScanFullBleedFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);
    utassert(!PdfDarkModeFeaturesLookLikePhoto(DuXiuTextScanFullBleedFeatures()));
    utassert(!PdfDarkModeShouldPreserveImageFeatures(DuXiuTextScanFullBleedFeatures(), 0.95f));

    // Grayscale portrait: Photo / Preserve — never AdaptiveDocument invert.
    kind = PdfDarkModeClassifyImageFeatures(GrayscalePortraitFeatures(), 0.28f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);
    utassert(PdfDarkModeFeaturesLookLikePhoto(GrayscalePortraitFeatures()));
    utassert(PdfDarkModeShouldPreserveImageFeatures(GrayscalePortraitFeatures(), 0.28f));

    kind = PdfDarkModeClassifyImageFeatures(GrayscalePortraitFeatures(), 0.80f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    // Scanned-page hint: colorful art stays Preserve; flat paper becomes FullPageScan.
    kind = PdfDarkModeClassifyImageFeatures(scan, 0.58f, true, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    kind = PdfDarkModeClassifyImageFeatures(TruePaperScanFullBleedFeatures(), 0.58f, true, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(confidence >= 0.7f);

    // Literature-style small figure: low coverage → Photo / Preserve.
    kind = PdfDarkModeClassifyImageFeatures(PhotoLikeFeatures(), 0.10f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    utassert(PdfDarkModePolicyForImageKind(DarkImageKind::Photo, true) == DarkImagePolicy::ThemeRecolor);

    float outR = 0.f, outG = 0.f, outB = 0.f;
    PdfDarkModeCompressPhotoHighlights(0.5f, 0.5f, 0.5f, &outR, &outG, &outB);
    utassert(outR == 0.5f && outG == 0.5f && outB == 0.5f);

    PdfDarkModeCompressPhotoHighlights(1.f, 1.f, 1.f, &outR, &outG, &outB);
    utassert(outR <= 0.91f && outG <= 0.91f && outB <= 0.91f);
    utassert(outR > 0.82f);

    utassert(str::Eq(PdfDarkModeKindDebugLabel(DarkImageKind::Photo), "Photo"));

    DarkImageAnalysis blendArt;
    blendArt.kind = DarkImageKind::LightBackgroundArtwork;
    blendArt.confidence = 0.78f;
    blendArt.features.borderUniformity = 0.72f;
    blendArt.features.borderLightRatio = 0.68f;
    blendArt.features.saturatedPixelRatio = 0.06f;
    blendArt.features.luminanceVariance = 0.009f;
    utassert(PdfDarkModeShouldBlendLightBackground(blendArt));

    DarkImageAnalysis photoArt = blendArt;
    photoArt.kind = DarkImageKind::Photo;
    utassert(!PdfDarkModeShouldBlendLightBackground(photoArt));

    DarkImageAnalysis lowConf = blendArt;
    lowConf.confidence = 0.40f;
    utassert(!PdfDarkModeShouldBlendLightBackground(lowConf));
}
