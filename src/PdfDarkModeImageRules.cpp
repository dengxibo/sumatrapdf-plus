/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "PdfDarkMode.h"

static int PdfDarkModeFeatureColorBuckets(const DarkImageFeatures& f) {
    return (int)(f.colorBucketRatio * 4096.f + 0.5f);
}

// Soft picture-book art can fail LookLikePhoto's bright-paper vetoes while still
// being colorful enough that AdaptiveDocument / FullPageScan would ruin it.
static bool PdfDarkModeFeaturesLookLikeColorfulIllustration(const DarkImageFeatures& f) {
    int buckets = PdfDarkModeFeatureColorBuckets(f);
    if (f.saturatedPixelRatio >= 0.12f && (buckets >= 14 || f.luminanceVariance >= 0.010f)) {
        return true;
    }
    if (f.saturatedPixelRatio >= 0.15f && buckets >= 10) {
        return true;
    }
    if (f.chromaticPixelRatio >= 0.20f && f.luminanceVariance >= 0.008f) {
        return true;
    }
    return false;
}

// Historical / documentary grayscale photos: low saturation but real tonal range.
// Must not fall through to LayoutBackground / FullPageScan (those AdaptiveDocument-invert).
bool PdfDarkModeFeaturesLookLikeGrayscalePhoto(const DarkImageFeatures& f) {
    if (f.saturatedPixelRatio > 0.08f || f.chromaticPixelRatio > 0.12f) {
        return false;
    }
    // Need photographic tonal variation — flat cream panels / icons fail this.
    if (f.luminanceVariance < 0.014f) {
        return false;
    }
    if (f.flatAreaRatio > 0.58f && f.luminanceVariance < 0.022f) {
        return false;
    }
    // Paper-dominated text scans (DuXiu): very flat paper panels.
    if (f.flatAreaRatio > 0.48f && f.highLuminanceRatio > 0.50f) {
        return false;
    }
    // High white area + low tonal variance — office / textbook text scans, not portraits.
    if (f.highLuminanceRatio > 0.62f && f.luminanceVariance < 0.035f) {
        return false;
    }
    return true;
}

// Soft cream / pastel design pages (e.g. notebook interiors): near-all light paper,
// low chroma, mild contrast. Steep AdaptiveDocument ink/paper remap turns faint grids
// into dirty horizontal noise — these must Preserve with gentle paper softening.
// Keep lumVar tight: RAZ glossary/back-matter (white + black text) has higher variance
// (~0.024–0.030) and must not match SoftCream (would stay mid-grey while photo pages go black).
bool PdfDarkModeFeaturesLookLikeSoftCreamIllustration(const DarkImageFeatures& f) {
    if (f.highLuminanceRatio <= 0.90f || f.luminanceVariance >= 0.022f || f.saturatedPixelRatio >= 0.10f) {
        return false;
    }
    // Neutral soft cream / mint notebooks (low paper chroma).
    if (f.chromaticPixelRatio < 0.15f) {
        return true;
    }
    // Warm cream / peach paper (RAZ Telescopes "Galileo's Dilemma"): chroma is the paper tint,
    // not saturated illustration ink. Steep remap turns that cream muddy grey-brown.
    return f.chromaticPixelRatio < 0.42f && f.saturatedPixelRatio < 0.04f;
}

bool PdfDarkModeFeaturesLookLikeLightDocumentPanel(const DarkImageFeatures& f) {
    // Cream/mint callout boxes (RAZ "Do You Know?"): light-dominated, little photo chroma.
    // Looser than SoftCream so bordered panels still match. Exclude photo-like tonal range.
    if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
        return true;
    }
    if (f.luminanceVariance >= 0.028f && f.highLuminanceRatio < 0.85f) {
        return false;
    }
    return f.highLuminanceRatio > 0.72f && f.luminanceVariance < 0.040f && f.saturatedPixelRatio < 0.14f &&
           f.chromaticPixelRatio < 0.22f;
}

// Notebook with a colored figure block (小家越住越大): paper-dominated grid, center illustration
// adds saturation — must not route to picture-book partial protect (dirty JPEG blocks).
bool PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(const DarkImageFeatures& f) {
    // RAZ map + TOC (The Apaches p.3): very white paper + colorful art — picture-book invert.
    if (f.highLuminanceRatio > 0.94f) {
        return false;
    }
    if (f.highLuminanceRatio < 0.88f || f.luminanceVariance >= 0.018f) {
        return false;
    }
    return f.saturatedPixelRatio >= 0.08f && f.saturatedPixelRatio < 0.24f;
}

// 红头文件 / office paper scans: white paper, black text, small red header — steep ink/paper
// remap for readable text; red header handled separately in ProcessGovernmentPaperPixmap.
bool PdfDarkModeFeaturesLookLikeGovernmentPaperScan(const DarkImageFeatures& f) {
    if (f.highLuminanceRatio < 0.82f) {
        return false;
    }
    // Red-header page 1: saturated area is small — allow higher sat/chroma when paper-dominated.
    const float maxSat = f.highLuminanceRatio > 0.88f ? 0.22f : 0.12f;
    const float maxChroma = f.highLuminanceRatio > 0.88f ? 0.25f : 0.18f;
    if (f.saturatedPixelRatio >= maxSat || f.chromaticPixelRatio >= maxChroma) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) &&
        (f.highLuminanceRatio < 0.85f || f.luminanceVariance >= 0.055f)) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeColorfulIllustration(f) || PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f)) {
        return false;
    }
    // Near-white office scans collapse to almost no variance in the thumbnail.
    // Decide these before SoftCream: neutral paper with either a broad flat field
    // or an effectively all-white sample is a scan, not a pastel illustration.
    bool neutralPaper = f.saturatedPixelRatio < 0.01f && f.chromaticPixelRatio < 0.01f;
    if (neutralPaper && f.luminanceVariance < 0.004f && f.highLuminanceRatio > 0.98f) {
        return true;
    }
    if (f.flatAreaRatio >= 0.48f && f.luminanceVariance < 0.020f && f.saturatedPixelRatio < 0.08f &&
        f.chromaticPixelRatio < 0.15f) {
        return true;
    }
    if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
        return false;
    }
    if (f.flatAreaRatio < 0.44f) {
        // 128px thumbnails under-estimate flat paper; text scans keep ink variance (人社 PaperStream).
        if (f.luminanceVariance < 0.022f) {
            // Ultra-white office scans: low flatArea on downscaled thumb is normal.
            if (f.highLuminanceRatio > 0.92f && f.saturatedPixelRatio < 0.08f && f.chromaticPixelRatio < 0.15f) {
                return true;
            }
            return false;
        }
        return f.luminanceVariance < 0.055f && f.saturatedPixelRatio < 0.04f && f.chromaticPixelRatio < 0.02f &&
               f.highLuminanceRatio < 0.92f;
    }
    if (f.luminanceVariance < 0.022f) {
        return true;
    }
    return f.luminanceVariance < 0.055f && f.saturatedPixelRatio < 0.06f && f.chromaticPixelRatio < 0.14f &&
           f.highLuminanceRatio < 0.92f;
}

// Office / government full-page scans that should use steep ink/paper binarize (not SoftCream gray).
bool PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(const DarkImageFeatures& f) {
    // Government-paper detection already excludes portraits and illustrations.
    // Check it before the broad grayscale-photo heuristic: text ink in a paper
    // scan can have the same thumbnail variance as a continuous-tone photo.
    if (PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f)) {
        return true;
    }
    if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikePhoto(f) && f.luminanceVariance >= 0.018f) {
        return false;
    }
    // Portrait / photo band on paper (RAZ): tonal range, not flat text scan.
    if (f.flatAreaRatio < 0.38f && f.luminanceVariance >= 0.022f) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f)) {
        return false;
    }
    return PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f) && f.highLuminanceRatio > 0.92f &&
           f.luminanceVariance < 0.025f && f.saturatedPixelRatio < 0.08f && f.flatAreaRatio >= 0.48f;
}

bool PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(const DarkImageFeatures& f) {
    if (PdfDarkModeFeaturesLookLikeBwLineArtScan(f)) {
        return true;
    }
    if (PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f) || PdfDarkModeFeaturesLookLikeColorfulIllustration(f) ||
        PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(f)) {
        return true;
    }
    // Cream/yellow household text scans (faint gray ink on tinted paper) — not RAZ photo pages.
    if (f.saturatedPixelRatio >= 0.10f || f.chromaticPixelRatio >= 0.25f) {
        return false;
    }
    if (f.luminanceVariance >= 0.10f) {
        return false;
    }
    return f.highLuminanceRatio >= 0.45f && f.highLuminanceRatio <= 0.80f && f.chromaticPixelRatio >= 0.04f &&
           f.saturatedPixelRatio < 0.08f;
}

// Full-res pixmap stats must confirm flat paper text — thumbnail lumVar is often too low on
// RAZ text+illustration pages (downscaled inset art looks like office paper).
bool PdfDarkModeFullResStatsAllowGovernmentPaperBinarize(const DarkImageAnalysis* imgAnalysis, float paperRatio,
                                                         float satRatio, float chromaRatio, float lumVar) {
    if (!imgAnalysis) {
        return lumVar < 0.035f && paperRatio >= 0.85f;
    }
    const DarkImageFeatures& f = imgAnalysis->features;
    if (PdfDarkModeFeaturesLookLikeBwLineArtScan(f)) {
        if (lumVar >= 0.045f && paperRatio < 0.88f) {
            return false;
        }
        return lumVar <= 0.050f;
    }
    if (PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f)) {
        return lumVar < 0.010f || (lumVar < 0.035f && paperRatio >= 0.88f && satRatio < 0.04f);
    }
    if (PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(f)) {
        return lumVar < 0.032f && paperRatio >= 0.88f;
    }
    // Cream household thumb fallback (LookLikeFullPageTextScanForBinarize tail).
    return lumVar < 0.038f && paperRatio >= 0.86f && satRatio < 0.055f && chromaRatio >= 0.035f && chromaRatio < 0.22f;
}

// Full-res pixmap stats: picture-book / photo pages that thumbnail analysis mislabels as text scans.
bool PdfDarkModeVetoGovernmentPaperBinarize(const DarkImageAnalysis* imgAnalysis, float paperRatio, float satRatio,
                                            float chromaRatio, float lumVar) {
    if (imgAnalysis) {
        // Uniform sepia paper makes old line drawings look highly chromatic even
        // though there is no color artwork. The line-art classifier has already
        // checked the tight paper/variance signature, so do not veto it as Photo.
        if (PdfDarkModeFeaturesLookLikeBwLineArtScan(imgAnalysis->features)) {
            return false;
        }
        if (imgAnalysis->kind == DarkImageKind::Photo ||
            PdfDarkModeFeaturesLookLikeColorfulIllustration(imgAnalysis->features) ||
            PdfDarkModeFeaturesLookLikeGrayscalePhoto(imgAnalysis->features) ||
            PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(imgAnalysis->features)) {
            return true;
        }
    }
    if (lumVar >= 0.040f) {
        return true;
    }
    if (lumVar >= 0.032f && paperRatio < 0.88f) {
        return true;
    }
    if (paperRatio < 0.82f && lumVar >= 0.035f) {
        return true;
    }
    if (satRatio >= 0.055f) {
        return true;
    }
    if (satRatio >= 0.10f) {
        return true;
    }
    if (chromaRatio >= 0.09f && lumVar >= 0.025f) {
        return true;
    }
    if (chromaRatio >= 0.22f && lumVar >= 0.008f) {
        return true;
    }
    if (chromaRatio >= 0.13f && lumVar >= 0.048f) {
        return true;
    }
    return false;
}

bool PdfDarkModeFullResStatsLookLikeInsetPhotoOnPaper(float paperRatio, float satRatio, float chromaRatio,
                                                      float lumVar) {
    // Wildlife Rescue p.8 (runtime): paper=0.839 sat=0.053 chroma=0.090 lumVar=0.072.
    // Majority paper + photographic variance + a little localized color.
    // Private Spaceships p.5 (runtime): paper=0.734 sat=0.149 chroma=0.248 lumVar=0.045.
    // White launch smoke keeps lumVar just under the old 0.050 floor; sat/chroma are
    // still a real photo (连环画 yellow paper has chroma >> sat, not this pairing).
    if (lumVar < 0.040f) {
        return false;
    }
    if (lumVar < 0.050f) {
        if (satRatio < 0.08f || chromaRatio < 0.12f || chromaRatio >= satRatio * 2.0f) {
            return false;
        }
    }
    if (paperRatio < 0.55f || paperRatio >= 0.94f) {
        return false;
    }
    if (satRatio < 0.035f && chromaRatio < 0.07f) {
        return false;
    }
    // Whole-page aged sepia 连环画: high paper-wide chroma, not a photo island.
    if (satRatio >= 0.15f && chromaRatio >= 0.40f && chromaRatio >= satRatio * 1.5f) {
        return false;
    }
    return true;
}

bool PdfDarkModeFullResStatsLookLikeColorIllustrationNotLineArt(float satRatio, float chromaRatio) {
    // Vincent's Bedroom p.8 (runtime): paper=0.548 sat=0.399 chroma=0.426 lumVar=0.095
    // and paper=0.523 sat=0.428 chroma=0.444 lumVar=0.044. Line-art thumbnail + paper
    // just under the 0.55 inset-photo floor sent the red portrait to 公文 stencil.
    // 红楼梦 cream pages stay sat~0.16 and must remain line-art / gov-paper invert.
    return satRatio >= 0.28f && chromaRatio >= 0.30f;
}

bool PdfDarkModeFullResStatsLookLikeAgedYellowLineArtPage(float paperRatio, float satRatio, float chromaRatio,
                                                          float lumVar, float borderPaperRatio, float redInkRatio) {
    // Runtime 红楼梦《拷打宝玉》:
    // p.7 (thumb lineArt=0): paper=0.560 sat=0.163 chroma=0.697 lumVar=0.053 borderPaper=0.016
    // p.56 (insetPhoto): paper=0.688 sat=0.161 chroma=0.339 lumVar=0.050 borderPaper=0.310
    // Cover (keep original): sat=0.977
    // Private Spaceships p.5 (keep photo): chroma/sat ≈ 1.66, white borderPaper ~1
    // RAZ red callout pages (Vincent's Bedroom): white margins and/or redInk — not cream 连环画
    if (redInkRatio >= 0.004f) {
        return false;
    }
    if (borderPaperRatio >= 0.45f) {
        return false;
    }
    if (satRatio < 0.08f || satRatio >= 0.28f) {
        return false;
    }
    if (chromaRatio < satRatio * 2.0f) {
        return false;
    }
    if (paperRatio < 0.45f || paperRatio >= 0.88f) {
        return false;
    }
    if (lumVar > 0.070f) {
        return false;
    }
    return true;
}

bool PdfDarkModeFullResStatsLookLikeInsetGrayPhotoIslands(float paperRatio, float satRatio, float chromaRatio,
                                                          float lumVar, int nPhotoRects, float largestRectCoverage) {
    // Dust Bowl Disauster reader p.16 (runtime): paper=0.782 sat=0 chroma=0 lumVar=0.069
    // nPeek=3, first island ~16% of the page. Color gate rejects sat=chroma=0.
    if (nPhotoRects < 1) {
        return false;
    }
    // Dust Bowl p.18 (runtime): paper=0.909 lumVar=0.045 — two smaller photos on a
    // text-heavy page. %.3f logged 0.045 but the old 0.045 exclusive floor missed it.
    if (lumVar < 0.040f) {
        return false;
    }
    if (paperRatio < 0.55f || paperRatio >= 0.94f) {
        return false;
    }
    if (satRatio >= 0.035f || chromaRatio >= 0.07f) {
        return false;
    }
    // Islands, not a full-page ink field (连环画) or a caption sliver.
    if (largestRectCoverage < 0.04f || largestRectCoverage > 0.50f) {
        return false;
    }
    return true;
}

bool PdfDarkModeFullResStatsLookLikeOfficeScanForGovPaper(float paperRatio, float satRatio, float chromaRatio,
                                                          float lumVar, float redInkRatio) {
    // Hangzhou 质疑函 p.1: paper=0.937 sat=0.035 chroma=0.230 lumVar=0.021 redInk=0.019.
    // p.2–4: same white scans but redInk=0; JPEG chroma 0.16–0.17 hit SoftCream instead.
    // Dust Bowl p.18 is lumVar=0.045. Cream RAZ paper is not 90%+ chroma<0.10 samples.
    if (satRatio >= 0.12f) {
        return false;
    }
    if (paperRatio >= 0.88f && lumVar < 0.040f) {
        if (chromaRatio < 0.12f) {
            return true;
        }
        if (redInkRatio >= 0.004f && chromaRatio < 0.35f) {
            return true;
        }
        return satRatio < 0.04f && chromaRatio < 0.28f;
    }
    // Faded gray photocopies: paper lum is often 0.62–0.72, so the lum>0.72 paper
    // counter under-counts and PictureBook Sharp leaves mid-gray glyphs on dark paper.
    if (paperRatio >= 0.58f && paperRatio < 0.88f && lumVar < 0.038f && satRatio < 0.045f && chromaRatio < 0.10f) {
        return true;
    }
    return false;
}

// Thumbnail SoftCream also matches sparse gray office scans (crushed lumVar). Cream
// notebooks keep a little saturation; ultra-white RAZ thumbs (Lincoln map) stay cream.
bool PdfDarkModeFeaturesLookLikeFadedOfficeScanNotCream(const DarkImageFeatures& f) {
    if (f.highLuminanceRatio <= 0.82f || f.highLuminanceRatio > 0.96f) {
        return false;
    }
    if (f.saturatedPixelRatio >= 0.008f || f.chromaticPixelRatio >= 0.08f) {
        return false;
    }
    if (f.luminanceVariance < 0.006f || f.luminanceVariance >= 0.022f) {
        return false;
    }
    return true;
}

// Classic B&W line-art scans (连环画 / woodblock reprints): paper + ink lines, no color.
// Must be FullPageScan / uniform remap — not Photo → partial photo-rect protect (gray noise).
bool PdfDarkModeFeaturesLookLikeBwLineArtScan(const DarkImageFeatures& f) {
    // Faded 1950s line-art scans can be almost entirely light sepia paper. That
    // uniform tint raises saturation/chroma enough to resemble color artwork in
    // a thumbnail, while the very low luminance variance and light border still
    // distinguish it from a real illustration or photograph.
    bool agedSepiaLineArt =
        f.highLuminanceRatio > 0.86f && f.highLuminanceRatio < 0.985f && f.luminanceVariance >= 0.004f &&
        f.luminanceVariance < 0.030f && f.saturatedPixelRatio >= 0.20f && f.saturatedPixelRatio < 0.34f &&
        f.chromaticPixelRatio >= 0.45f && f.chromaticPixelRatio < 0.98f && f.flatAreaRatio >= 0.035f &&
        f.flatAreaRatio < 0.13f && f.borderLightRatio >= 0.62f && f.borderLightRatio < 0.80f;
    if (agedSepiaLineArt) {
        return true;
    }
    if (f.saturatedPixelRatio >= 0.05f) {
        return false;
    }
    if (f.chromaticPixelRatio >= 0.06f) {
        return false;
    }
    if (f.highLuminanceRatio < 0.55f) {
        return false;
    }
    if (f.luminanceVariance < 0.018f) {
        return false;
    }
    // An almost-white full-page map can lose all color in its thumbnail and
    // resemble sparse black line art. Keep that ambiguous case on the photo /
    // soft-cream path instead of applying the harsh line-art remap.
    if (f.highLuminanceRatio > 0.92f && f.luminanceVariance < 0.022f) {
        return false;
    }
    // Dense hatching can push variance above sparse ink; still line-art when the field is
    // paper-dominated. Continuous tonal portraits (RAZ) stay out via low flat/border paper.
    if (f.luminanceVariance > 0.034f) {
        if (f.luminanceVariance > 0.048f) {
            return false;
        }
        if (f.flatAreaRatio < 0.34f || f.borderLightRatio < 0.62f) {
            return false;
        }
    }
    return true;
}

// True scanned page / flat paper UI — not a colorful illustration or grayscale photo.
static bool PdfDarkModeFeaturesLookLikeTrueFullPageScan(const DarkImageFeatures& f) {
    int buckets = PdfDarkModeFeatureColorBuckets(f);
    if (PdfDarkModeFeaturesLookLikeColorfulIllustration(f) || PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) ||
        PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
        return false;
    }
    // Paper-heavy grayscale/near-gray page (text scan): high white area, little chroma.
    // Ink variance can be high — do not require low lumVar (that missed DuXiu books).
    if (f.highLuminanceRatio > 0.58f && f.saturatedPixelRatio < 0.10f && f.chromaticPixelRatio < 0.12f) {
        return true;
    }
    if (f.highLuminanceRatio > 0.45f && f.saturatedPixelRatio < 0.12f &&
        (f.luminanceVariance < 0.020f || buckets <= 18)) {
        return true;
    }
    return false;
}

// Mirrors PdfDarkModeStatsLookLikePhoto in PdfDarkModeImageStats.cpp.
bool PdfDarkModeFeaturesLookLikePhoto(const DarkImageFeatures& f) {
    if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
        return true;
    }
    int buckets = PdfDarkModeFeatureColorBuckets(f);
    bool isPhoto = buckets >= 16 || f.saturatedPixelRatio >= 0.18f || f.luminanceVariance >= 0.014f;
    if (f.highLuminanceRatio > 0.58f && f.saturatedPixelRatio < 0.18f) {
        isPhoto = false;
    }
    if (buckets <= 12 && f.luminanceVariance < 0.012f && f.highLuminanceRatio > 0.45f) {
        isPhoto = false;
    }
    if (f.highLuminanceRatio > 0.72f && f.saturatedPixelRatio < 0.18f) {
        isPhoto = false;
    }
    return isPhoto;
}

static bool PdfDarkModeFeaturesLookLikeFlatLayoutPanel(const DarkImageFeatures& f) {
    int buckets = PdfDarkModeFeatureColorBuckets(f);
    return f.highLuminanceRatio > 0.76f && f.luminanceVariance < 0.011f && buckets <= 11 &&
           f.saturatedPixelRatio < 0.17f;
}

// Mirrors PdfDarkModeStatsLookLikeLayoutBackground in PdfDarkModeImageStats.cpp.
static bool PdfDarkModeFeaturesLookLikeLayoutBackground(const DarkImageFeatures& f, float pageCoverage) {
    // Colorful illustrations and grayscale photos must not be treated as UI panels.
    if (PdfDarkModeFeaturesLookLikeColorfulIllustration(f) || PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
        return false;
    }
    int buckets = PdfDarkModeFeatureColorBuckets(f);
    if (PdfDarkModeFeaturesLookLikeFlatLayoutPanel(f)) {
        return pageCoverage >= 0.04f;
    }
    if (f.highLuminanceRatio > 0.58f && f.luminanceVariance < 0.018f && f.saturatedPixelRatio < 0.12f) {
        return pageCoverage >= 0.04f;
    }
    if (f.highLuminanceRatio > 0.44f && f.luminanceVariance < 0.022f && f.saturatedPixelRatio < 0.12f) {
        return pageCoverage >= 0.06f;
    }
    if (f.highLuminanceRatio > 0.50f && f.luminanceVariance < 0.038f && f.saturatedPixelRatio < 0.22f &&
        buckets <= 14) {
        return pageCoverage >= 0.04f;
    }
    return false;
}

static bool PdfDarkModeFeaturesLookLikePaperTextBox(const DarkImageFeatures& f) {
    int buckets = PdfDarkModeFeatureColorBuckets(f);
    return f.highLuminanceRatio > 0.64f && f.luminanceVariance < 0.014f && buckets <= 12 &&
           f.saturatedPixelRatio < 0.20f;
}

DarkImageKind PdfDarkModeClassifyImageFeatures(const DarkImageFeatures& f, float pageCoverage, bool pageIsScannedHint,
                                               float* outConfidence) {
    float confidence = 0.4f;
    DarkImageKind kind = DarkImageKind::Unknown;

    // Full-bleed: preserve colorful picture-book art; only AdaptiveDocument for true scans.
    // When ambiguous, prefer Photo (Preserve) over FullPageScan.
    if (pageCoverage >= kMaxPreserveImagePageCoverage && f.highLuminanceRatio > 0.45f) {
        if (PdfDarkModeFeaturesLookLikeBwLineArtScan(f)) {
            kind = DarkImageKind::FullPageScan;
            confidence = 0.76f;
        } else if (PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f)) {
            kind = DarkImageKind::FullPageScan;
            confidence = 0.78f;
        } else if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
            // Scanned contracts mimic soft-cream stats; notebook art has lower flatAreaRatio.
            // Sparse gray photocopies also look like cream in a 128px thumb (low lumVar) and
            // must FullPageScan — Preserve+SoftCream keeps original dark ink on dark paper.
            if (f.flatAreaRatio > 0.48f ||
                (f.highLuminanceRatio > 0.92f && f.saturatedPixelRatio < 0.008f && f.luminanceVariance < 0.018f) ||
                PdfDarkModeFeaturesLookLikeFadedOfficeScanNotCream(f)) {
                kind = DarkImageKind::FullPageScan;
                confidence = 0.80f;
            } else {
                kind = DarkImageKind::Photo;
                confidence = 0.74f;
            }
        } else if (PdfDarkModeFeaturesLookLikePhoto(f) || PdfDarkModeFeaturesLookLikeColorfulIllustration(f)) {
            kind = DarkImageKind::Photo;
            confidence = 0.76f;
        } else if (PdfDarkModeFeaturesLookLikeTrueFullPageScan(f)) {
            kind = DarkImageKind::FullPageScan;
            confidence = 0.82f;
        } else if (f.highLuminanceRatio > 0.78f && f.saturatedPixelRatio < 0.12f && f.chromaticPixelRatio < 0.12f &&
                   !PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
            // Office / government text scans: paper-heavy full bleed, not illustration.
            kind = DarkImageKind::FullPageScan;
            confidence = 0.68f;
        } else {
            kind = DarkImageKind::Photo;
            confidence = 0.55f;
        }
    } else if (pageIsScannedHint && pageCoverage >= 0.55f && !PdfDarkModeFeaturesLookLikeColorfulIllustration(f) &&
               !PdfDarkModeFeaturesLookLikePhoto(f) && !PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
        kind = DarkImageKind::FullPageScan;
        confidence = 0.72f;
    } else if (PdfDarkModeFeaturesLookLikeLayoutBackground(f, pageCoverage)) {
        kind = DarkImageKind::LightBackgroundArtwork;
        confidence = 0.80f;
    } else if (PdfDarkModeFeaturesLookLikePhoto(f) || PdfDarkModeFeaturesLookLikeColorfulIllustration(f)) {
        if (pageCoverage < 0.14f && PdfDarkModeFeaturesLookLikePaperTextBox(f) &&
            !PdfDarkModeFeaturesLookLikeColorfulIllustration(f)) {
            kind = DarkImageKind::IconOrLineArt;
            confidence = 0.66f;
        } else {
            kind = DarkImageKind::Photo;
            confidence = 0.78f;
        }
    } else if (f.borderLightRatio > 0.62f && f.borderUniformity > 0.62f && f.highLuminanceRatio > 0.44f &&
               f.flatAreaRatio > 0.35f && f.saturatedPixelRatio < 0.12f) {
        kind = DarkImageKind::LightBackgroundArtwork;
        confidence = 0.76f;
    } else if (f.highLuminanceRatio > 0.52f && f.luminanceVariance < 0.014f && f.colorBucketRatio < 0.025f &&
               f.saturatedPixelRatio < 0.10f) {
        kind = DarkImageKind::IconOrLineArt;
        confidence = 0.68f;
    } else if (pageCoverage < 0.05f && f.colorBucketRatio < 0.018f) {
        kind = DarkImageKind::IconOrLineArt;
        confidence = 0.60f;
    } else {
        kind = DarkImageKind::Unknown;
        confidence = 0.42f;
    }

    if (outConfidence) {
        *outConfidence = confidence;
    }
    return kind;
}

DarkImagePolicy PdfDarkModePolicyForImageKind(DarkImageKind kind, bool isImageMask) {
    if (isImageMask) {
        return DarkImagePolicy::ThemeRecolor;
    }
    switch (kind) {
        case DarkImageKind::Photo:
        case DarkImageKind::Unknown:
            return DarkImagePolicy::Preserve;
        case DarkImageKind::FullPageScan:
        case DarkImageKind::LightBackgroundArtwork:
        case DarkImageKind::IconOrLineArt:
            return DarkImagePolicy::AdaptiveDocument;
    }
    return DarkImagePolicy::AdaptiveDocument;
}

// Match-theme: never show an unprocessed white manuscript page. Users who want the
// original look switch document color mode to Original explicitly.
DarkImagePolicy PdfDarkModeClampFollowThemePolicy(DarkImagePolicy policy, float pageCoverage,
                                                  const DarkImageAnalysis& analysis) {
    if (PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(analysis.features) &&
        analysis.kind != DarkImageKind::Photo && !PdfDarkModeFeaturesLookLikeGrayscalePhoto(analysis.features)) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    if (policy != DarkImagePolicy::Preserve || pageCoverage < 0.50f) {
        return policy;
    }
    // Full-bleed photos (RAZ portraits): keep Preserve → picture-book + photo-rect protect.
    if (analysis.kind == DarkImageKind::Photo) {
        if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(analysis.features) ||
            (PdfDarkModeFeaturesLookLikePhoto(analysis.features) && analysis.features.luminanceVariance >= 0.020f)) {
            return policy;
        }
    }
    if (analysis.kind == DarkImageKind::FullPageScan) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    // RAZ map + TOC (The Apaches p.3): colorful art on white paper — picture-book invert,
    // not Preserve SoftCream gray compromise.
    if (pageCoverage >= kMaxPreserveImagePageCoverage && analysis.features.highLuminanceRatio > 0.82f &&
        !PdfDarkModeFeaturesLookLikeGrayscalePhoto(analysis.features) &&
        PdfDarkModeFeaturesLookLikeColorfulIllustration(analysis.features)) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(analysis.features) ||
        PdfDarkModeFeaturesLookLikeColorfulIllustration(analysis.features)) {
        float conf = 0.f;
        DarkImageKind kind = PdfDarkModeClassifyImageFeatures(analysis.features, pageCoverage, true, &conf);
        if (kind != DarkImageKind::FullPageScan) {
            return policy;
        }
    }
    if (analysis.features.highLuminanceRatio > 0.82f) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    if (pageCoverage >= kMaxPreserveImagePageCoverage) {
        return DarkImagePolicy::AdaptiveDocument;
    }
    return policy;
}

bool PdfDarkModeShouldPreserveImageFeatures(const DarkImageFeatures& f, float pageCoverage) {
    if (PdfDarkModeFeaturesLookLikeFlatLayoutPanel(f)) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeLayoutBackground(f, pageCoverage)) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
        return true;
    }
    if (PdfDarkModeFeaturesLookLikePhoto(f) || PdfDarkModeFeaturesLookLikeColorfulIllustration(f)) {
        if (pageCoverage < 0.14f && PdfDarkModeFeaturesLookLikePaperTextBox(f) &&
            !PdfDarkModeFeaturesLookLikeColorfulIllustration(f)) {
            return false;
        }
        return true;
    }
    return false;
}

void PdfDarkModeCompressPhotoHighlights(float r, float g, float b, float* outR, float* outG, float* outB) {
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    const float knee = 0.82f;
    const float cap = 0.90f;
    if (lum <= knee) {
        *outR = r;
        *outG = g;
        *outB = b;
        return;
    }
    float t = (lum - knee) / (1.f - knee);
    if (t > 1.f) {
        t = 1.f;
    }
    float targetLum = knee + (cap - knee) * t;
    float scale = lum > 0.0001f ? targetLum / lum : 1.f;
    *outR = r * scale;
    *outG = g * scale;
    *outB = b * scale;
}

const char* PdfDarkModeKindDebugLabel(DarkImageKind kind) {
    switch (kind) {
        case DarkImageKind::Photo:
            return "Photo";
        case DarkImageKind::LightBackgroundArtwork:
            return "LightBg";
        case DarkImageKind::IconOrLineArt:
            return "Icon";
        case DarkImageKind::FullPageScan:
            return "Scan";
        case DarkImageKind::Unknown:
        default:
            return "Unknown";
    }
}

bool PdfDarkModeShouldBlendLightBackground(const DarkImageAnalysis& analysis) {
    if (analysis.kind != DarkImageKind::LightBackgroundArtwork) {
        return false;
    }
    if (analysis.confidence < 0.65f) {
        return false;
    }
    const DarkImageFeatures& f = analysis.features;
    if (f.borderUniformity < 0.55f || f.borderLightRatio < 0.50f) {
        return false;
    }
    if (f.saturatedPixelRatio >= 0.22f && f.luminanceVariance >= 0.020f) {
        return false;
    }
    return true;
}
