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

// Soft cream notebook / design-book page: near-all light paper, mild contrast, little
// chroma — must Preserve gently, not AdaptiveDocument sharp remap (dirty grid noise).
static DarkImageFeatures SoftCreamNotebookFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 14.f / 4096.f;
    f.highLuminanceRatio = 0.93f;
    f.saturatedPixelRatio = 0.01f;
    f.chromaticPixelRatio = 0.02f;
    f.luminanceVariance = 0.016f;
    f.borderLightRatio = 0.88f;
    f.borderUniformity = 0.70f;
    f.flatAreaRatio = 0.40f;
    return f;
}

// RAZ Telescopes "Galileo's Dilemma": warm cream panel + historical diagram — paper chroma
// without saturated ink; must SoftCream, not muddy picture-book steep remap.
static DarkImageFeatures WarmCreamGalileoPageFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 18.f / 4096.f;
    f.highLuminanceRatio = 0.92f;
    f.saturatedPixelRatio = 0.019f;
    f.chromaticPixelRatio = 0.31f;
    f.luminanceVariance = 0.010f;
    f.borderLightRatio = 0.90f;
    f.borderUniformity = 0.72f;
    f.flatAreaRatio = 0.42f;
    return f;
}

// 小家越住越大 page 62: paper grid + colored figure block — whole-tile recolor, not picture-book rects.
static DarkImageFeatures XiaojiaNotebookIllustrationPage62Features() {
    DarkImageFeatures f;
    f.isColorful = true;
    f.colorBucketRatio = 48.f / 4096.f;
    f.highLuminanceRatio = 0.90f;
    f.saturatedPixelRatio = 0.19f;
    f.chromaticPixelRatio = 0.20f;
    f.luminanceVariance = 0.015f;
    f.borderLightRatio = 0.85f;
    f.borderUniformity = 0.65f;
    f.flatAreaRatio = 0.38f;
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

// Contract scan (flat paper, low ink variance): mimics SoftCream stats but must remap.
static DarkImageFeatures ContractScanFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 8.f / 4096.f;
    f.highLuminanceRatio = 0.97f;
    f.saturatedPixelRatio = 0.002f;
    f.chromaticPixelRatio = 0.003f;
    f.luminanceVariance = 0.008f;
    f.borderLightRatio = 0.90f;
    f.borderUniformity = 0.82f;
    f.flatAreaRatio = 0.55f;
    return f;
}

// Government / office notice scan: bright paper, mild ink variance — was ambiguous Photo.
static DarkImageFeatures GovernmentOfficeScanFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 14.f / 4096.f;
    f.highLuminanceRatio = 0.92f;
    f.saturatedPixelRatio = 0.04f;
    f.chromaticPixelRatio = 0.05f;
    f.luminanceVariance = 0.018f;
    f.borderLightRatio = 0.88f;
    f.borderUniformity = 0.78f;
    f.flatAreaRatio = 0.50f;
    return f;
}

// 人社 PaperStream scan (825通知): text ink variance, tiny red header — soft cream, not picture-book.
static DarkImageFeatures RenshePaperStreamScanFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 12.f / 4096.f;
    f.highLuminanceRatio = 0.905f;
    f.saturatedPixelRatio = 0.0f;
    f.chromaticPixelRatio = 0.007f;
    f.luminanceVariance = 0.0464f;
    f.borderLightRatio = 0.88f;
    f.borderUniformity = 0.75f;
    f.flatAreaRatio = 0.48f;
    return f;
}

// 人社 Image Conversion scan (2020通知): near-white paper thumbnail stats.
static DarkImageFeatures RensheImageConversionScanFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 8.f / 4096.f;
    f.highLuminanceRatio = 0.999f;
    f.saturatedPixelRatio = 0.0f;
    f.chromaticPixelRatio = 0.002f;
    f.luminanceVariance = 0.0016f;
    f.borderLightRatio = 0.90f;
    f.borderUniformity = 0.82f;
    f.flatAreaRatio = 0.55f;
    return f;
}

// 人社红头首页: large red header raises thumbnail sat — must still route to government binarize.
static DarkImageFeatures RensheRedHeaderPage1Features() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 12.f / 4096.f;
    f.highLuminanceRatio = 0.96f;
    f.saturatedPixelRatio = 0.14f;
    f.chromaticPixelRatio = 0.16f;
    f.luminanceVariance = 0.008f;
    f.borderLightRatio = 0.88f;
    f.borderUniformity = 0.75f;
    f.flatAreaRatio = 0.52f;
    return f;
}

// Image Conversion thumb: ultra-white paper, low flatArea on 128px — must still binarize.
static DarkImageFeatures RensheImageConversionLowFlatFeatures() {
    DarkImageFeatures f = RensheImageConversionScanFeatures();
    f.flatAreaRatio = 0.35f;
    return f;
}

// RAZ Lincoln page 17 full-bleed portrait: must not match government paper scan.
static DarkImageFeatures LincolnPage17FullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 10.f / 4096.f;
    f.highLuminanceRatio = 0.940f;
    f.saturatedPixelRatio = 0.0f;
    f.chromaticPixelRatio = 0.0f;
    f.luminanceVariance = 0.0361f;
    f.borderLightRatio = 0.45f;
    f.borderUniformity = 0.38f;
    f.flatAreaRatio = 0.22f;
    return f;
}

// Near-grayscale contract scan: very low sat, soft-cream-like lumVar.
static DarkImageFeatures PaleContractScanFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 8.f / 4096.f;
    f.highLuminanceRatio = 0.97f;
    f.saturatedPixelRatio = 0.001f;
    f.chromaticPixelRatio = 0.002f;
    f.luminanceVariance = 0.016f;
    f.borderLightRatio = 0.90f;
    f.borderUniformity = 0.82f;
    f.flatAreaRatio = 0.40f;
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

// 1954 连环画-style B&W line art on aged paper: ink variance, no color — FullPageScan, not Photo rects.
static DarkImageFeatures LianhuanhuaBwLineArtScanFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 18.f / 4096.f;
    f.highLuminanceRatio = 0.72f;
    f.saturatedPixelRatio = 0.02f;
    f.chromaticPixelRatio = 0.035f;
    f.luminanceVariance = 0.032f;
    f.borderLightRatio = 0.82f;
    f.borderUniformity = 0.70f;
    f.flatAreaRatio = 0.42f;
    return f;
}

// Dense hatching (红楼梦·尤二姐 style panels): higher lumVar but still paper-dominated line art.
static DarkImageFeatures LianhuanhuaDenseHatchingLineArtFeatures() {
    DarkImageFeatures f = LianhuanhuaBwLineArtScanFeatures();
    f.luminanceVariance = 0.040f;
    f.highLuminanceRatio = 0.68f;
    f.flatAreaRatio = 0.40f;
    f.borderLightRatio = 0.78f;
    return f;
}

// RAZ-Z Abraham Lincoln: full-bleed B&W portrait + caption — must Preserve, not invert interior.
static DarkImageFeatures RazBwPortraitFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 10.f / 4096.f;
    f.highLuminanceRatio = 0.40f;
    f.saturatedPixelRatio = 0.015f;
    f.chromaticPixelRatio = 0.025f;
    f.luminanceVariance = 0.026f;
    f.borderLightRatio = 0.45f;
    f.borderUniformity = 0.38f;
    f.flatAreaRatio = 0.22f;
    return f;
}

// Same book at measured stats (white paper + portrait band): highLum ~0.84, lumVar ~0.044.
static DarkImageFeatures RazBwPortraitHighPaperFullBleedFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 12.f / 4096.f;
    f.highLuminanceRatio = 0.84f;
    f.saturatedPixelRatio = 0.001f;
    f.chromaticPixelRatio = 0.008f;
    f.luminanceVariance = 0.044f;
    f.borderLightRatio = 0.50f;
    f.borderUniformity = 0.40f;
    f.flatAreaRatio = 0.35f;
    return f;
}

// RAZ Lincoln page 15 map: thumb loses map color (sat=0) — Photo + soft-cream lumVar, not office binarize.
static DarkImageFeatures LincolnPage15MapFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 10.f / 4096.f;
    f.highLuminanceRatio = 0.970f;
    f.saturatedPixelRatio = 0.0f;
    f.chromaticPixelRatio = 0.0f;
    f.luminanceVariance = 0.0208f;
    f.borderLightRatio = 0.85f;
    f.borderUniformity = 0.75f;
    f.flatAreaRatio = 0.45f;
    return f;
}

// RAZ Lincoln page 21 battlefield: ultra-flat thumb — government stats, picture-book photo protect wins.
static DarkImageFeatures LincolnPage21BattlefieldFeatures() {
    DarkImageFeatures f;
    f.isColorful = false;
    f.colorBucketRatio = 8.f / 4096.f;
    f.highLuminanceRatio = 1.0f;
    f.saturatedPixelRatio = 0.0f;
    f.chromaticPixelRatio = 0.0f;
    f.luminanceVariance = 0.0011f;
    f.borderLightRatio = 0.90f;
    f.borderUniformity = 0.82f;
    f.flatAreaRatio = 0.55f;
    return f;
}

// RAZ-Z The Apaches page 3 (map + TOC): was Preserve SoftCream gray — picture-book invert.
static DarkImageFeatures ApachesPage3TocMapFeatures() {
    DarkImageFeatures f;
    f.isColorful = true;
    f.colorBucketRatio = 24.f / 4096.f;
    f.highLuminanceRatio = 0.975f;
    f.saturatedPixelRatio = 0.163f;
    f.chromaticPixelRatio = 0.318f;
    f.luminanceVariance = 0.0148f;
    f.borderLightRatio = 0.85f;
    f.borderUniformity = 0.75f;
    f.flatAreaRatio = 0.42f;
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

    // Soft cream notebook (小家越住越大): Photo / Preserve — not FullPageScan sharp remap.
    utassert(PdfDarkModeFeaturesLookLikeSoftCreamIllustration(WarmCreamGalileoPageFeatures()));
    kind = PdfDarkModeClassifyImageFeatures(WarmCreamGalileoPageFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    kind = PdfDarkModeClassifyImageFeatures(SoftCreamNotebookFullBleedFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    utassert(PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(XiaojiaNotebookIllustrationPage62Features()));
    utassert(!PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(ApachesPage3TocMapFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeGovernmentPaperScan(XiaojiaNotebookIllustrationPage62Features()));
    utassert(!PdfDarkModeFeaturesLookLikeGovernmentPaperScan(SoftCreamNotebookFullBleedFeatures()));

    // High-variance text scan (Merck / DuXiu): FullPageScan, not grayscale Photo.
    kind = PdfDarkModeClassifyImageFeatures(DuXiuTextScanFullBleedFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);
    utassert(!PdfDarkModeFeaturesLookLikePhoto(DuXiuTextScanFullBleedFeatures()));
    utassert(!PdfDarkModeShouldPreserveImageFeatures(DuXiuTextScanFullBleedFeatures(), 0.95f));

    // Government office notice scan: paper-heavy full bleed — AdaptiveDocument, not Preserve.
    kind = PdfDarkModeClassifyImageFeatures(GovernmentOfficeScanFullBleedFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);
    utassert(PdfDarkModeFeaturesLookLikeGovernmentPaperScan(GovernmentOfficeScanFullBleedFeatures()));

    kind = PdfDarkModeClassifyImageFeatures(RenshePaperStreamScanFeatures(), 1.0f, true, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModeFeaturesLookLikeGovernmentPaperScan(RenshePaperStreamScanFeatures()));

    kind = PdfDarkModeClassifyImageFeatures(RensheImageConversionScanFeatures(), 1.0f, true, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModeFeaturesLookLikeGovernmentPaperScan(RensheImageConversionScanFeatures()));
    utassert(PdfDarkModeFeaturesLookLikeGovernmentPaperScan(RensheImageConversionLowFlatFeatures()));
    utassert(PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(RensheImageConversionLowFlatFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(SoftCreamNotebookFullBleedFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(LincolnPage17FullBleedFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(RazBwPortraitFullBleedFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(RazBwPortraitHighPaperFullBleedFeatures()));
    utassert(PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(RenshePaperStreamScanFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeGovernmentPaperScan(LincolnPage17FullBleedFeatures()));
    utassert(PdfDarkModeFeaturesLookLikeGovernmentPaperScan(RensheRedHeaderPage1Features()));
    kind = PdfDarkModeClassifyImageFeatures(LincolnPage15MapFeatures(), 1.0f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModeFeaturesLookLikeSoftCreamIllustration(LincolnPage15MapFeatures()));
    utassert(PdfDarkModeFeaturesLookLikeGovernmentPaperScan(LincolnPage21BattlefieldFeatures()));
    kind = PdfDarkModeClassifyImageFeatures(RensheRedHeaderPage1Features(), 1.0f, true, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);

    // Flat contract scan: soft-cream-like lumVar but high flatArea — FullPageScan, not Preserve.
    kind = PdfDarkModeClassifyImageFeatures(ContractScanFullBleedFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);
    utassert(PdfDarkModeFeaturesLookLikeSoftCreamIllustration(ContractScanFullBleedFeatures()));

    // Pale contract scan: soft-cream lumVar but near-zero saturation — FullPageScan.
    kind = PdfDarkModeClassifyImageFeatures(PaleContractScanFeatures(), 0.95f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);

    // Grayscale portrait: Photo / Preserve — never AdaptiveDocument invert.
    kind = PdfDarkModeClassifyImageFeatures(GrayscalePortraitFeatures(), 0.28f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);
    utassert(PdfDarkModeFeaturesLookLikePhoto(GrayscalePortraitFeatures()));
    utassert(PdfDarkModeShouldPreserveImageFeatures(GrayscalePortraitFeatures(), 0.28f));

    kind = PdfDarkModeClassifyImageFeatures(GrayscalePortraitFeatures(), 0.80f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);

    // B&W line-art scan (连环画): FullPageScan — never partial photo-rect picture-book path.
    kind = PdfDarkModeClassifyImageFeatures(LianhuanhuaBwLineArtScanFeatures(), 0.92f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::AdaptiveDocument);
    utassert(!PdfDarkModeShouldPreserveImageFeatures(LianhuanhuaBwLineArtScanFeatures(), 0.92f));
    utassert(!PdfDarkModeFeaturesLookLikeGrayscalePhoto(LianhuanhuaBwLineArtScanFeatures()));
    utassert(PdfDarkModeFeaturesLookLikeBwLineArtScan(LianhuanhuaBwLineArtScanFeatures()));

    // Dense 连环画 hatching: still line-art / FullPageScan (not RAZ portrait photo-rect path).
    kind = PdfDarkModeClassifyImageFeatures(LianhuanhuaDenseHatchingLineArtFeatures(), 0.92f, false, &confidence);
    utassert(kind == DarkImageKind::FullPageScan);
    utassert(PdfDarkModeFeaturesLookLikeBwLineArtScan(LianhuanhuaDenseHatchingLineArtFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeBwLineArtScan(RazBwPortraitHighPaperFullBleedFeatures()));

    // RAZ B&W portrait full bleed: Photo / Preserve — photo rect protect, not negative invert.
    kind = PdfDarkModeClassifyImageFeatures(RazBwPortraitFullBleedFeatures(), 0.92f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);
    utassert(PdfDarkModeFeaturesLookLikeGrayscalePhoto(RazBwPortraitFullBleedFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeBwLineArtScan(RazBwPortraitFullBleedFeatures()));
    utassert(!PdfDarkModeFeaturesLookLikeBwLineArtScan(GrayscalePortraitFeatures()));

    kind = PdfDarkModeClassifyImageFeatures(RazBwPortraitHighPaperFullBleedFeatures(), 0.92f, false, &confidence);
    utassert(kind == DarkImageKind::Photo);
    utassert(PdfDarkModePolicyForImageKind(kind, false) == DarkImagePolicy::Preserve);
    utassert(PdfDarkModeFeaturesLookLikeGrayscalePhoto(RazBwPortraitHighPaperFullBleedFeatures()));

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

    // Match-theme clamp: misclassified white contract scan must not stay Preserve.
    DarkImageAnalysis contractClamp;
    contractClamp.kind = DarkImageKind::Photo;
    contractClamp.features = ContractScanFullBleedFeatures();
    utassert(PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, 0.95f, contractClamp) ==
             DarkImagePolicy::AdaptiveDocument);

    // Red-header government page 1 misclassified as Photo must still binarize.
    DarkImageAnalysis redHeaderClamp;
    redHeaderClamp.kind = DarkImageKind::Photo;
    redHeaderClamp.features = RensheRedHeaderPage1Features();
    utassert(PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, 1.0f, redHeaderClamp) ==
             DarkImagePolicy::AdaptiveDocument);

    DarkImageAnalysis apachesClamp;
    apachesClamp.kind = DarkImageKind::Photo;
    apachesClamp.features = ApachesPage3TocMapFeatures();
    utassert(PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, 1.0f, apachesClamp) ==
             DarkImagePolicy::AdaptiveDocument);

    // Soft-cream notebook design pages may still Preserve (designed art, not white scan).
    DarkImageAnalysis notebookClamp;
    notebookClamp.kind = DarkImageKind::Photo;
    notebookClamp.features = SoftCreamNotebookFullBleedFeatures();
    utassert(PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, 0.95f, notebookClamp) ==
             DarkImagePolicy::Preserve);

    DarkImageAnalysis razPortraitClamp;
    razPortraitClamp.kind = DarkImageKind::Photo;
    razPortraitClamp.features = RazBwPortraitHighPaperFullBleedFeatures();
    utassert(PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, 0.95f, razPortraitClamp) ==
             DarkImagePolicy::Preserve);

    // Small inline figure stays Preserve.
    DarkImageAnalysis smallFig;
    smallFig.kind = DarkImageKind::Photo;
    smallFig.features = PhotoLikeFeatures();
    utassert(PdfDarkModeClampFollowThemePolicy(DarkImagePolicy::Preserve, 0.10f, smallFig) ==
             DarkImagePolicy::Preserve);

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
