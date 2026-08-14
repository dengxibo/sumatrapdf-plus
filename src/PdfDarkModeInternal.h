/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

extern "C" {
#include <mupdf/fitz.h>
}

#include "PdfDarkMode.h"

struct DarkModeEngineCache;

DarkModeEngineCache* PdfDarkModeEngineCacheCreate();
void PdfDarkModeEngineCacheFree(fz_context* ctx, DarkModeEngineCache* cache);
void PdfDarkModeEngineCacheClear(fz_context* ctx, DarkModeEngineCache* cache);
// LayoutPhoto textbooks (Acrobat/PageMaker): cheap remap, not RAZ PictureBook.
void PdfDarkModeEngineCacheSetLayoutTextbookFastRemap(DarkModeEngineCache* cache, bool enabled);
bool PdfDarkModeEngineCacheLayoutTextbookFastRemap(const DarkModeEngineCache* cache);

bool PdfDarkModeEngineCacheLookupFeatures(DarkModeEngineCache* cache, fz_image* image, DarkImageFeatures* outFeatures,
                                          PixelColor* outBackground);
void PdfDarkModeEngineCacheStoreFeatures(fz_context* ctx, DarkModeEngineCache* cache, fz_image* image,
                                         const DarkImageFeatures& features, const PixelColor& background);

fz_image* PdfDarkModeEngineCacheLookupProcessed(fz_context* ctx, DarkModeEngineCache* cache, fz_image* src,
                                                u32 profileHash, DarkImagePolicy policy, DarkImageKind kind);
void PdfDarkModeEngineCacheStoreProcessed(fz_context* ctx, DarkModeEngineCache* cache, fz_image* src, u32 profileHash,
                                          DarkImagePolicy policy, DarkImageKind kind, fz_image* processed);

DarkModePageAnalysis* PdfDarkModeGetOrBuildAnalysis(fz_context* ctx, FzPageInfo* pageInfo, fz_display_list* list,
                                                    u32 optionsHash, DarkModeEngineCache* engineCache = nullptr);

DarkImageAnalysis PdfDarkModeAnalyzeImageCached(fz_context* ctx, fz_image* image, float pageCoverage,
                                                bool pageIsScannedHint, DarkModeEngineCache* engineCache);

fz_device* PdfDarkModeWrapDevice(fz_context* ctx, fz_device* inner, DarkModePageAnalysis* analysis,
                                 const DarkModePalette* palette, DarkModeReplayState* replayState,
                                 DarkModeEngineCache* engineCache, u32 profileHash, bool debugOverlay = false);

fz_device* PdfDarkModeWrapFollowThemeDevice(fz_context* ctx, fz_device* inner, const DarkModePalette* palette,
                                            const RectF& pageBounds, DarkModeEngineCache* engineCache, u32 profileHash,
                                            const Vec<RectF>* artworkBounds = nullptr);

fz_image* PdfDarkModeGetCachedFollowThemeImage(fz_context* ctx, DarkModeEngineCache* engineCache, fz_image* srcImage,
                                               DarkImagePolicy policy, float pageCoverage,
                                               const DarkModePalette& palette, u32 profileHash);

void PdfDarkModeAppendImagePhotoSkipDevRects(fz_context* ctx, fz_image* image, const RectF& imgOnPage,
                                             const fz_matrix& ctm, Vec<Rect>& outSkip);

void MapColorToDarkTheme(fz_context* ctx, fz_colorspace* cs, const float* color, fz_color_params colorParams,
                         const DarkModePalette& palette, float* outRgb);

void MapFillColorToDarkTheme(fz_context* ctx, fz_colorspace* cs, const float* color, fz_color_params colorParams,
                             const DarkModePalette& palette, float* outRgb);

void MapRgbFillToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb);

void MapRgbToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb);

void PdfDarkModeRecordShadeForward();
int PdfDarkModeTakeShadeForwardCount();

void ApplyPreserveImagePaperSoftening(float r, float g, float b, const DarkModePalette& palette, float strength,
                                      float* outR, float* outG, float* outB);
// Full-bleed picture books: linear remap margins only; photo rect left untouched.
void ApplyPreservePictureBookPaperAndInk(float r, float g, float b, const DarkModePalette& palette, float* outR,
                                         float* outG, float* outB);
fz_pixmap* PdfDarkModeProcessPictureBookPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette,
                                               const DarkImageAnalysis* imgAnalysis = nullptr);
// V2 full-page: photo-rect interiors preserved; everything else Okular→theme (cached by caller).
// Pass imgAnalysis so B&W documentary portraits (RAZ) seek photo rects even with large white margins.
fz_pixmap* PdfDarkModeProcessV2FullPagePixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette,
                                              const DarkImageAnalysis* imgAnalysis = nullptr);
// V2 small images: knock out JPEG white mats around colorful badges (UNIT / Atlas headers).
// Returns nullptr when knockout does not apply (caller keeps the original image).
fz_pixmap* PdfDarkModeProcessV2WhiteMatPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette);
// Soft gray raster drop-shadow plates (Glencoe callouts) → solid theme bg.
fz_pixmap* PdfDarkModeProcessV2SoftShadowPlatePixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette);
// Soft-cream notebook pages: gentle paper softening only (no steep ink remap).
fz_pixmap* PdfDarkModeProcessSoftCreamPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette);
// Government / office paper scans: steep ink↔paper remap for readable text on dark theme.
fz_pixmap* PdfDarkModeProcessGovernmentPaperPixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette);

// Margin strips, drop shadows, and similar layout art — not photos to preserve.
bool PdfDarkModeIsDecorativeStripImage(const RectF& imgRect, const RectF& pageBounds);
bool PdfDarkModeIsSubstantialFollowThemeArtwork(const RectF& imgRect, float pageArea);
bool PdfDarkModeIsPhotoFrameStripImage(const RectF& imgRect, const RectF& pageBounds, const Vec<RectF>* artworkBounds);

void PdfDarkModeFreeProcessCache(fz_context* ctx, DarkModePageAnalysis* analysis);

// Returns a kept fz_image for fill_image, or nullptr to use the source image.
fz_image* PdfDarkModeGetCachedImage(fz_context* ctx, DarkModeEngineCache* engineCache, DarkModePageAnalysis* analysis,
                                    int occurrenceIndex, fz_image* srcImage, DarkImagePolicy policy,
                                    const DarkModePalette& palette, u32 profileHash);

// Phase 4: returns kept fz_image with alpha, or nullptr to fall back to per-pixel adaptive recolor.
fz_pixmap* PdfDarkModeProcessLightBackgroundPixmap(fz_context* ctx, fz_pixmap* src, const DarkImageAnalysis& analysis,
                                                   const DarkModePalette& palette);

// Phase 5: returns processed pixmap for FullPageScan, or nullptr to fall back.
fz_pixmap* PdfDarkModeProcessScanPixmap(fz_context* ctx, fz_pixmap* src, const DarkImageAnalysis& analysis,
                                        const DarkModePalette& palette);

// Returns a kept fz_image covering bounds, or nullptr to fall back to direct shade fill.
fz_image* PdfDarkModeGetCachedShade(fz_context* ctx, DarkModePageAnalysis* analysis, fz_shade* shade, fz_matrix ctm,
                                    float alpha, fz_irect bounds, const DarkModePalette& palette);

void PdfDarkModeClearPixmapToThemeBackground(fz_context* ctx, fz_pixmap* pix, const DarkModePalette& palette);
