/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "PdfDarkMode.h"
#include "PdfDarkModeInternal.h"
#include "PdfDarkModeV2.h"

// Stub implementations for binaries that compile EngineMupdf.cpp but not
// PdfDarkMode*.cpp / Theme.cpp (PdfFilter, PdfPreview, etc.).

bool PdfDarkModeUsesObjectLevel() {
    return false;
}

bool DarkModeProfileUsesObjectLevel(const DarkModeProfile* profile) {
    (void)profile;
    return false;
}

bool DarkModeProfileUsesFollowThemeDirect(const DarkModeProfile* profile) {
    (void)profile;
    return false;
}

bool DarkModeProfileUsesFollowThemeV2(const DarkModeProfile* profile) {
    (void)profile;
    return false;
}

bool DarkModeProfileUsesLegacyPostProcess(const DarkModeProfile* profile) {
    (void)profile;
    return false;
}

void BuildViewDarkModeProfile(EngineBase* engine, DarkModeProfile* profile) {
    (void)engine;
    if (profile) {
        *profile = DarkModeProfile{};
    }
}

u32 PdfDarkModeComputeProfileHash(const DarkModeProfile* profile) {
    (void)profile;
    return 0;
}

u32 PdfDarkModeComputeOptionsHash() {
    return 0;
}

DarkModePalette PdfDarkModeBuildPalette() {
    return DarkModePalette{};
}

DarkModeEngineCache* PdfDarkModeEngineCacheCreate() {
    return nullptr;
}

void PdfDarkModeEngineCacheFree(fz_context* ctx, DarkModeEngineCache* cache) {
    (void)ctx;
    (void)cache;
}

void PdfDarkModeEngineCacheClear(fz_context* ctx, DarkModeEngineCache* cache) {
    (void)ctx;
    (void)cache;
}

void PdfDarkModeEngineCacheSetLayoutTextbookFastRemap(DarkModeEngineCache* cache, bool enabled) {
    (void)cache;
    (void)enabled;
}

bool PdfDarkModeEngineCacheLayoutTextbookFastRemap(const DarkModeEngineCache* cache) {
    (void)cache;
    return false;
}

DarkModePageAnalysis* PdfDarkModeGetOrBuildAnalysis(fz_context* ctx, FzPageInfo* pageInfo, fz_display_list* list,
                                                    u32 optionsHash, DarkModeEngineCache* engineCache) {
    (void)ctx;
    (void)pageInfo;
    (void)list;
    (void)optionsHash;
    (void)engineCache;
    return nullptr;
}

fz_device* PdfDarkModeWrapDevice(fz_context* ctx, fz_device* inner, DarkModePageAnalysis* analysis,
                                 const DarkModePalette* palette, DarkModeReplayState* replayState,
                                 DarkModeEngineCache* engineCache, u32 profileHash, bool debugOverlay) {
    (void)ctx;
    (void)analysis;
    (void)palette;
    (void)replayState;
    (void)engineCache;
    (void)profileHash;
    (void)debugOverlay;
    return inner;
}

fz_device* PdfDarkModeWrapFollowThemeDevice(fz_context* ctx, fz_device* inner, const DarkModePalette* palette,
                                            const RectF& pageBounds, DarkModeEngineCache* engineCache, u32 profileHash,
                                            const Vec<RectF>* artworkBounds) {
    (void)ctx;
    (void)palette;
    (void)pageBounds;
    (void)engineCache;
    (void)profileHash;
    (void)artworkBounds;
    return inner;
}

fz_device* PdfDarkModeWrapV2Device(fz_context* ctx, fz_device* inner, const DarkModePalette* palette,
                                   const RectF& pageBounds, DarkModeEngineCache* engineCache, u32 profileHash) {
    (void)ctx;
    (void)palette;
    (void)pageBounds;
    (void)engineCache;
    (void)profileHash;
    return inner;
}

fz_image* PdfDarkModeGetCachedFollowThemeImage(fz_context* ctx, DarkModeEngineCache* engineCache, fz_image* srcImage,
                                               DarkImagePolicy policy, float pageCoverage,
                                               const DarkModePalette& palette, u32 profileHash) {
    (void)ctx;
    (void)engineCache;
    (void)srcImage;
    (void)policy;
    (void)pageCoverage;
    (void)palette;
    (void)profileHash;
    return nullptr;
}

void PdfDarkModeInvalidatePage(fz_context* ctx, FzPageInfo* pageInfo) {
    (void)ctx;
    (void)pageInfo;
}

bool PdfDarkModePdfMetadataSuggestsBitmapRecolorDoc(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return false;
}

bool PdfDarkModePdfMetadataSuggestsFullPageScanDoc(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return false;
}

bool PdfDarkModePdfMetadataSuggestsPrintToPdfScanDoc(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return false;
}

bool PdfDarkModePdfMetadataSuggestsLayoutPhotoDoc(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return false;
}

bool PdfDarkModePdfMetadataSuggestsPaperCaptureDoc(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return false;
}

bool PdfDarkModePdfMetadataSuggestsImageConversionPictureBook(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return false;
}

bool PdfDarkModeImageHasPreservablePhotoRects(fz_context* ctx, fz_image* image) {
    (void)ctx;
    (void)image;
    return false;
}

bool PdfDarkModeImageDecodeLooksLikeGrayscalePortrait(fz_context* ctx, fz_image* image) {
    (void)ctx;
    (void)image;
    return false;
}

void PdfDarkModeAppendImagePhotoSkipDevRects(fz_context* ctx, fz_image* image, const RectF& imgOnPage,
                                             const fz_matrix& ctm, Vec<Rect>& outSkip) {
    (void)ctx;
    (void)image;
    (void)imgOnPage;
    (void)ctm;
    (void)outSkip;
}

FollowThemeScanProbe PdfDarkModeProbeFollowThemeScanPage(fz_context* ctx, fz_page* page, const RectF& pageBounds,
                                                         FollowThemePageProbeStats* stats) {
    (void)stats;
    (void)ctx;
    (void)page;
    (void)pageBounds;
    return FollowThemeScanProbe::Mixed;
}

int GetPreservePdfImagesMinSize() {
    return 72;
}

bool GetPreservePdfImagesInDarkMode() {
    return true;
}

bool PdfSmartModePreservesEmbeddedImages() {
    return false;
}

bool PdfFollowThemePreservesEmbeddedImageColors() {
    return false;
}

void SetPreservePdfImagesInDarkMode(bool preserve) {
    (void)preserve;
}

PdfDarkModeRenderer GetPdfDarkModeRenderer() {
    return PdfDarkModeRenderer::LegacyBitmapPostProcess;
}

PdfDocumentColorMode GetPdfDocumentColorMode() {
    return PdfDocumentColorMode::Auto;
}

void SetPdfDocumentColorMode(PdfDocumentColorMode mode) {
    (void)mode;
}

bool ReflowEbookUsesThemeBitmapRecolor() {
    return false;
}

bool PdfDarkModeIsDecorativeStripImage(const RectF& imgRect, const RectF& pageBounds) {
    (void)imgRect;
    (void)pageBounds;
    return false;
}

bool PdfDarkModeIsPhotoFrameStripImage(const RectF& imgRect, const RectF& pageBounds, const Vec<RectF>* artworkBounds) {
    (void)imgRect;
    (void)pageBounds;
    (void)artworkBounds;
    return false;
}

bool PdfDarkModeIsSubstantialFollowThemeArtwork(const RectF& imgRect, float pageArea) {
    (void)imgRect;
    (void)pageArea;
    return false;
}

bool PdfDarkModeImageLooksLikeDarkArtwork(fz_context* ctx, fz_image* image, float pageCoverage) {
    (void)ctx;
    (void)image;
    (void)pageCoverage;
    return false;
}

bool PdfDarkModePageDominantImageRecolors(fz_context* ctx, fz_image* image, float pageCoverage) {
    (void)ctx;
    (void)image;
    (void)pageCoverage;
    return false;
}

bool PdfDarkModeImageIsConfirmedArtwork(fz_context* ctx, fz_image* image, float pageCoverage, int devW, int devH) {
    (void)ctx;
    (void)image;
    (void)pageCoverage;
    (void)devW;
    (void)devH;
    return false;
}

DarkImageAnalysis PdfDarkModeAnalyzeImage(fz_context* ctx, fz_image* image, float pageCoverage,
                                          bool pageIsScannedHint) {
    (void)ctx;
    (void)image;
    (void)pageCoverage;
    (void)pageIsScannedHint;
    return DarkImageAnalysis{};
}

bool PdfDarkModeFeaturesLookLikePhoto(const DarkImageFeatures& f) {
    (void)f;
    return false;
}

bool PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(const DarkImageFeatures& f) {
    (void)f;
    return false;
}

bool PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(const DarkImageFeatures& f) {
    (void)f;
    return false;
}

bool PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(const DarkImageFeatures& f) {
    (void)f;
    return false;
}

fz_pixmap* PdfDarkModeProcessGovernmentPaperPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette) {
    (void)ctx;
    (void)palette;
    return src;
}

bool PdfDarkModeShouldPreserveImageFeatures(const DarkImageFeatures& f, float pageCoverage) {
    (void)f;
    (void)pageCoverage;
    return false;
}

RectF PdfDarkModeClampImagePageRect(const RectF& imgPage, int imageW, int imageH) {
    (void)imageW;
    (void)imageH;
    return imgPage;
}

RectF PdfDarkModeCapUnknownImagePageRect(const RectF& imgPage, float pageHeight) {
    (void)pageHeight;
    return imgPage;
}

bool PdfDarkModeShouldPreserveEmbeddedImageRect(fz_context* ctx, fz_image* image, float pageCoverage, int devW,
                                                int devH) {
    (void)ctx;
    (void)image;
    (void)pageCoverage;
    (void)devW;
    (void)devH;
    return false;
}

void PdfDarkModeClearPixmapToThemeBackground(fz_context* ctx, fz_pixmap* pix, const DarkModePalette& palette) {
    (void)ctx;
    (void)pix;
    (void)palette;
}

DarkModeOptions PdfDarkModeCurrentOptions() {
    return DarkModeOptions{};
}

FollowThemeScanProbe PdfDarkModeProbeFollowThemeScanList(fz_context* ctx, fz_display_list* list,
                                                         const RectF& pageBounds, FollowThemePageProbeStats* stats) {
    (void)ctx;
    (void)list;
    (void)pageBounds;
    (void)stats;
    return FollowThemeScanProbe::Unknown;
}
