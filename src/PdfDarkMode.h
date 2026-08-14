/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

class EngineBase;
struct FzPageInfo;
struct DarkModeEngineCache;
struct fz_context;
struct fz_display_list;
struct fz_image;
struct fz_page;

enum class DarkImagePolicy {
    Preserve,
    AdaptiveDocument,
    ThemeRecolor,
};

// Image role for Match-theme (FollowTheme) routing:
// - Photo / Unknown → Preserve (+ optional paper softening): grey paper, keep art colors
//   (picture books / large colorful figures). Prefer this when ambiguous.
// - FullPageScan / LightBackgroundArtwork / IconOrLineArt → AdaptiveDocument:
//   recolor true scans and flat UI panels so text-heavy pages stay readable.
enum class DarkImageKind {
    Photo,
    LightBackgroundArtwork,
    IconOrLineArt,
    FullPageScan,
    Unknown,
};

struct PixelColor {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

struct DarkImageFeatures {
    bool isColorful = false;
    float colorBucketRatio = 0.f;
    float transparentRatio = 0.f;
    float highLuminanceRatio = 0.f;
    float saturatedPixelRatio = 0.f;
    float chromaticPixelRatio = 0.f;
    float borderUniformity = 0.f;
    float borderLightRatio = 0.f;
    float flatAreaRatio = 0.f;
    float textureScore = 0.f;
    float luminanceVariance = 0.f;
    float pageCoverage = 0.f;
};

struct DarkImageAnalysis {
    DarkImageKind kind = DarkImageKind::Unknown;
    float confidence = 0.f;
    PixelColor estimatedBackground{};
    DarkImageFeatures features{};
};

enum class PdfDarkModeRenderer {
    LegacyBitmapPostProcess = 0,
    ObjectLevelDevice = 1,
};

enum class PdfDocumentColorMode {
    Auto = 0,
    Black = 1,
    Light = 2,
};

// Per-render dark mode path (View target only for Smart/Legacy PDF paths).
enum class PageColorMode {
    Normal,
    LegacyInvert,
    SmartDark,
    FollowThemeDirect,
    FollowThemeV2, // Okular text/vector; full-page: photo-rect protect + Okular margins
    PreserveImages,
    ScanDark,
};

struct DarkModeOptions {
    float scanImageCoverageThreshold = 0.75f;
    float minScanDominantCoverage = 0.85f;
    float maxScanAspectSkew = 1.15f;
    int maxTextOpsForScanPage = 10;
    int maxVectorOpsForScanPage = 20;
    // Follow-theme fast path: LaTeX-like pages (many tiny text ops, almost no images).
    int followThemeBitmapRecolorMinTextOps = 60;
    int followThemeBitmapRecolorMaxImageOps = 4;
    float followThemeBitmapRecolorMaxImageCoverage = 0.12f;
    // 0=off, 1=blend near-white Preserve-image pixels toward page background
    float preserveImagePaperSoftening = 0.f;
    float lightFillChromaThreshold = 0.05f;
    float lightFillLuminanceThreshold = 0.45f;
};

// Full-bleed backgrounds / scans at or above this threshold are classified;
// colorful illustrations stay Preserve, true paper scans use AdaptiveDocument.
static constexpr float kMaxPreserveImagePageCoverage = 0.75f;

struct DarkModePalette {
    float textR = 0.f, textG = 0.f, textB = 0.f;
    float bgR = 1.f, bgG = 1.f, bgB = 1.f;
    float linkR = 0.f, linkG = 0.f, linkB = 0.f;
    float diffR = 1.f, diffG = 1.f, diffB = 1.f;
};

struct DarkModeProfile {
    PageColorMode mode = PageColorMode::Normal;
    COLORREF foreground = 0;
    COLORREF pageBackground = 0;
    COLORREF linkColor = 0;
    float strength = 1.f;
    bool debugOverlay = false;
    bool preservePdfImages = false;
    int preservePdfImagesMinSize = 72;
    DarkModePalette palette{};
    DarkModeOptions options{};
    u32 hash = 0;
};

struct ImageOccurrenceInfo {
    int occurrenceIndex = 0;
    RectF pageBounds{};
    bool isImageMask = false;
    bool hasAlpha = false;
    float pageCoverage = 0.f;
    bool looksLikePhoto = true;
    DarkImagePolicy policy = DarkImagePolicy::Preserve;
    DarkImageAnalysis analysis{};
};

struct DarkModePageAnalysis {
    int pageNumber = 0;
    RectF pageBounds{};
    bool isScannedPage = false;
    Vec<ImageOccurrenceInfo> images;
    u32 optionsHash = 0;
    void* processCache = nullptr;
};

struct DarkModeReplayState {
    int nextImageOccurrence = 0;
};

// PDF dark mode runtime options (not stored in settings file)
bool GetPreservePdfImagesInDarkMode();
void SetPreservePdfImagesInDarkMode(bool preserve);
// Smart/Auto and Light-Warm Smart: legacy post-process may skip embedded image rects on fixed PDFs.
bool PdfSmartModePreservesEmbeddedImages();
// Follow theme on dark themes: keep embedded/page images at original colors.
bool PdfFollowThemePreservesEmbeddedImageColors();
int GetPreservePdfImagesMinSize();
PdfDarkModeRenderer GetPdfDarkModeRenderer();

bool PdfDarkModeUsesObjectLevel();
bool DarkModeProfileUsesObjectLevel(const DarkModeProfile* profile);
bool DarkModeProfileUsesFollowThemeDirect(const DarkModeProfile* profile);
bool DarkModeProfileUsesFollowThemeV2(const DarkModeProfile* profile);
bool DarkModeProfileUsesLegacyPostProcess(const DarkModeProfile* profile);
void BuildViewDarkModeProfile(EngineBase* engine, DarkModeProfile* profile);
u32 PdfDarkModeComputeProfileHash(const DarkModeProfile* profile);
PdfDocumentColorMode GetPdfDocumentColorMode();
void SetPdfDocumentColorMode(PdfDocumentColorMode mode);
const char* PdfDocumentColorModeDescription(PdfDocumentColorMode mode);
// Reflow EPUB/MOBI (MuPDF): Match-theme mode recolors the rendered page bitmap (including images).
bool ReflowEbookUsesThemeBitmapRecolor();
DarkModeOptions PdfDarkModeCurrentOptions();
u32 PdfDarkModeComputeOptionsHash();
DarkModePalette PdfDarkModeBuildPalette();

void PdfDarkModeFreeAnalysis(fz_context* ctx, DarkModePageAnalysis* analysis);

enum class FollowThemeScanProbe : u8 {
    Unknown = 0,
    Mixed = 1,
    PureScan = 2,
    // Few images; micro-span text (typical LaTeX). Whole-tile legacy recolor, not per-op wrap.
    BitmapRecolor = 3,
};

struct FollowThemePageProbeStats {
    int textOps = 0;
    int imageOps = 0;
    int vectorOps = 0;
    float maxImageCoverage = 0.f;
    float stackedImageCoverage = 0.f;
};

inline bool FollowThemePageStatsAllowBitmapRecolor(const FollowThemePageProbeStats& st, const DarkModeOptions& opts) {
    return st.imageOps <= opts.followThemeBitmapRecolorMaxImageOps &&
           st.maxImageCoverage <= opts.followThemeBitmapRecolorMaxImageCoverage;
}

inline bool FollowThemePageStatsMatchBitmapRecolor(const FollowThemePageProbeStats& st, const DarkModeOptions& opts) {
    return st.textOps >= opts.followThemeBitmapRecolorMinTextOps && FollowThemePageStatsAllowBitmapRecolor(st, opts);
}

// LaTeX books: only keep Mixed (per-image preserve) when a page has a large figure; dense text pages
// with a few small XObjects should still use whole-tile recolor for contrast and speed.
inline FollowThemeScanProbe PdfDarkModeLaTeXRefineFollowThemeProbe(FollowThemeScanProbe probe,
                                                                   const FollowThemePageProbeStats& st,
                                                                   const DarkModeOptions& opts) {
    if (probe != FollowThemeScanProbe::Mixed) {
        return probe;
    }
    if (st.textOps >= opts.followThemeBitmapRecolorMinTextOps && st.maxImageCoverage < 0.18f) {
        return FollowThemeScanProbe::BitmapRecolor;
    }
    return probe;
}

FollowThemeScanProbe PdfDarkModeProbeFollowThemeScanPage(fz_context* ctx, fz_page* page, const RectF& pageBounds,
                                                         FollowThemePageProbeStats* stats = nullptr);
FollowThemeScanProbe PdfDarkModeProbeFollowThemeScanList(fz_context* ctx, fz_display_list* list,
                                                         const RectF& pageBounds,
                                                         FollowThemePageProbeStats* stats = nullptr);

struct pdf_document;
bool PdfDarkModePdfMetadataSuggestsBitmapRecolorDoc(fz_context* ctx, pdf_document* doc);
bool PdfDarkModePdfMetadataSuggestsLayoutPhotoDoc(fz_context* ctx, pdf_document* doc);
bool PdfDarkModePdfMetadataSuggestsPaperCaptureDoc(fz_context* ctx, pdf_document* doc);
// DuXiu / Pdg2Pic / SuperStar etc.: full-page image scans → whole-tile bitmap recolor.
bool PdfDarkModePdfMetadataSuggestsFullPageScanDoc(fz_context* ctx, pdf_document* doc);
// Print-to-PDF / Acrobat Elements / PScript scans (multi-layer image pages, no text layer).
bool PdfDarkModePdfMetadataSuggestsPrintToPdfScanDoc(fz_context* ctx, pdf_document* doc);
// RAZ / Adobe Image Conversion: one full-bleed raster per page with embedded photos.
bool PdfDarkModePdfMetadataSuggestsImageConversionPictureBook(fz_context* ctx, pdf_document* doc);

void PdfDarkModeInvalidatePage(fz_context* ctx, FzPageInfo* pageInfo);

bool PdfDarkModeImageHasPreservablePhotoRects(fz_context* ctx, fz_image* image);
// Full-decode portrait probe: B&W face on paper (RAZ) vs colored illustration on notebook scan.
bool PdfDarkModeImageDecodeLooksLikeGrayscalePortrait(fz_context* ctx, fz_image* image);

void ApplyAdaptiveDocumentDarkMode(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                                   float* outB);

bool PdfDarkModeIsDecorativeStripImage(const RectF& imgRect, const RectF& pageBounds);
bool PdfDarkModeIsSubstantialFollowThemeArtwork(const RectF& imgRect, float pageArea);
bool PdfDarkModeIsPhotoFrameStripImage(const RectF& imgRect, const RectF& pageBounds, const Vec<RectF>* artworkBounds);
DarkImagePolicy PdfDarkModePolicyForFollowThemeImage(const RectF& imgBounds, bool isImageMask, const RectF& pageBounds,
                                                     const Vec<RectF>* artworkBounds = nullptr,
                                                     fz_context* ctx = nullptr, fz_image* image = nullptr,
                                                     DarkModeEngineCache* engineCache = nullptr);

// OKLab perceptual remap for SmartDark text/vector colors (Phase 2).
void MapRgbToDarkThemeOklab(float r, float g, float b, const DarkModePalette& palette, float* outRgb);

// Perceptual distance in OKLab (Phase 4 background matching).
float PdfDarkModeOklabDistance(float r1, float g1, float b1, float r2, float g2, float b2);

// Phase 4: edge-connected light background removal for LightBackgroundArtwork.
bool PdfDarkModeShouldBlendLightBackground(const DarkImageAnalysis& analysis);

// Phase 5: full-page scan remapping (Smart path only).
void PdfDarkModeRemapScanPixel(float r, float g, float b, const DarkImageAnalysis& analysis,
                               const DarkModePalette& palette, float* outR, float* outG, float* outB);

bool PdfDarkModeImageLooksLikePhoto(fz_context* ctx, fz_image* image);
bool PdfDarkModeImageLooksLikeDarkArtwork(fz_context* ctx, fz_image* image, float pageCoverage);

// Page-sized images normally recolor with the page (scans / full-bleed backgrounds).
// Cover illustrations that fill the page should still be preserved.
bool PdfDarkModePageDominantImageRecolors(fz_context* ctx, fz_image* image, float pageCoverage);

RectF PdfDarkModeClampImagePageRect(const RectF& imgPage, int imageW, int imageH);

// Cap bbox when embedded image dimensions are unknown (common with content-stream tiles).
RectF PdfDarkModeCapUnknownImagePageRect(const RectF& imgPage, float pageHeight);

// Gate for Legacy skip-rect preserve: combines bbox size, pixel stats, and artwork heuristics.
bool PdfDarkModeShouldPreserveEmbeddedImageRect(fz_context* ctx, fz_image* image, float pageCoverage, int devW,
                                                int devH);

// Stricter pixel gate used by PdfDarkModeShouldPreserveEmbeddedImageRect.
bool PdfDarkModeImageShouldPreserveInLegacy(fz_context* ctx, fz_image* image, float pageCoverage = 0.f, int devW = 0,
                                            int devH = 0);

bool PdfDarkModeImageIsConfirmedArtwork(fz_context* ctx, fz_image* image, float pageCoverage, int devW, int devH);

// Phase 3: fz_image pixel analysis (page-independent; safe for tile-free classification).
DarkImageAnalysis PdfDarkModeAnalyzeImage(fz_context* ctx, fz_image* image, float pageCoverage,
                                          bool pageIsScannedHint = false);

DarkImageKind PdfDarkModeClassifyImageFeatures(const DarkImageFeatures& features, float pageCoverage,
                                               bool pageIsScannedHint, float* outConfidence);

bool PdfDarkModeFeaturesLookLikePhoto(const DarkImageFeatures& f);
bool PdfDarkModeFeaturesLookLikeGrayscalePhoto(const DarkImageFeatures& f);
bool PdfDarkModeFeaturesLookLikeBwLineArtScan(const DarkImageFeatures& f);
bool PdfDarkModeFeaturesLookLikeSoftCreamIllustration(const DarkImageFeatures& f);
// Pastel callout / "Do You Know?" panels: mostly light paper, not a photo.
bool PdfDarkModeFeaturesLookLikeLightDocumentPanel(const DarkImageFeatures& f);
bool PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(const DarkImageFeatures& f);
bool PdfDarkModeFeaturesLookLikeGovernmentPaperScan(const DarkImageFeatures& f);
bool PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(const DarkImageFeatures& f);
// Noisy JPEG text scans (medical/office): steep ink/paper binarize, not linear invert.
bool PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(const DarkImageFeatures& f);
bool PdfDarkModeFullResStatsAllowGovernmentPaperBinarize(const DarkImageAnalysis* imgAnalysis, float paperRatio,
                                                         float satRatio, float chromaRatio, float lumVar);
bool PdfDarkModeVetoGovernmentPaperBinarize(const DarkImageAnalysis* imgAnalysis, float paperRatio, float satRatio,
                                            float chromaRatio, float lumVar);
bool PdfDarkModeShouldPreserveImageFeatures(const DarkImageFeatures& f, float pageCoverage);

DarkImagePolicy PdfDarkModePolicyForImageKind(DarkImageKind kind, bool isImageMask);
// Match-theme guard: map Preserve → AdaptiveDocument for white scan pages (not designed art).
DarkImagePolicy PdfDarkModeClampFollowThemePolicy(DarkImagePolicy policy, float pageCoverage,
                                                  const DarkImageAnalysis& analysis);

void PdfDarkModeCompressPhotoHighlights(float r, float g, float b, float* outR, float* outG, float* outB);

const char* PdfDarkModeKindDebugLabel(DarkImageKind kind);
