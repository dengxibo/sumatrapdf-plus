/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"

#include "Theme.h"
#include "PdfDarkModeInternal.h"

static bool dm_soft_cream_is_faded_office_scan(const DarkImageFeatures& f, float pageCoverage) {
    return pageCoverage >= kMaxPreserveImagePageCoverage && PdfDarkModeFeaturesLookLikeFadedOfficeScanNotCream(f);
}

static bool dm_should_use_government_paper_pixmap(fz_context* ctx, fz_image* srcImage,
                                                  const DarkImageAnalysis* imgAnalysis, float pageCoverage) {
    if (!imgAnalysis || pageCoverage < kMaxPreserveImagePageCoverage) {
        return false;
    }
    const DarkImageFeatures& f = imgAnalysis->features;
    // Ultra-flat office scans (人社 PaperStream): red header can trip photo-rect probe — still binarize.
    if (PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f) && f.luminanceVariance < 0.006f) {
        return true;
    }
    // Thumbnail already says office/text scan: do not decode a 1400px pixmap just
    // to hunt photo rects (that path dominates dark-mode scan-page render time).
    if (PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(f) && f.saturatedPixelRatio < 0.06f) {
        return true;
    }
    if (dm_soft_cream_is_faded_office_scan(f, pageCoverage)) {
        return true;
    }
    // Picture-book photos and B&W portraits: preserve tonal art, not office binarize.
    if (imgAnalysis->kind == DarkImageKind::Photo) {
        return false;
    }
    if (PdfDarkModeFeaturesLookLikeGrayscalePhoto(f)) {
        return false;
    }
    if (ctx && srcImage && PdfDarkModeImageHasPreservablePhotoRects(ctx, srcImage)) {
        return false;
    }
    return PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(f);
}

// Low-chroma full-page text scans (DuXiu, medical/office scans): steep ink/paper binarize,
// not per-pixel AdaptiveDocument (JPEG grain becomes white speckles on dark theme).
static bool dm_fullpage_low_chroma_text_scan(const DarkImageFeatures& f) {
    return PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(f);
}

struct DarkModeShadeCacheEntry {
    fz_shade* shade = nullptr;
    fz_matrix ctm{};
    float alpha = 0.f;
    fz_irect bounds{};
    fz_image* processedImage = nullptr;
};

struct DarkModeProcessCache {
    Vec<fz_image*> processedImages;
    Vec<DarkModeShadeCacheEntry> shadeCache;
};

static const int kMaxShadeCacheEntries = 32;

static DarkModeProcessCache* PdfDarkModeEnsureProcessCache(DarkModePageAnalysis* analysis) {
    if (!analysis) {
        return nullptr;
    }
    auto* cache = (DarkModeProcessCache*)analysis->processCache;
    if (!cache) {
        cache = new DarkModeProcessCache();
        analysis->processCache = cache;
    }
    int n = analysis->images.Size();
    if (cache->processedImages.Size() != n) {
        cache->processedImages.SetSize(n);
        for (int i = 0; i < n; i++) {
            cache->processedImages[i] = nullptr;
        }
    }
    return cache;
}

void PdfDarkModeFreeProcessCache(fz_context* ctx, DarkModePageAnalysis* analysis) {
    if (!analysis || !analysis->processCache) {
        return;
    }
    auto* cache = (DarkModeProcessCache*)analysis->processCache;
    if (ctx) {
        for (fz_image* img : cache->processedImages) {
            if (img) {
                fz_drop_image(ctx, img);
            }
        }
        for (DarkModeShadeCacheEntry& entry : cache->shadeCache) {
            if (entry.processedImage) {
                fz_drop_image(ctx, entry.processedImage);
            }
        }
    }
    delete cache;
    analysis->processCache = nullptr;
}

typedef void (*dm_rgb_map_fn)(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                              float* outB);

static void dm_transform_pixmap_rgb(fz_context* ctx, fz_pixmap* pix, const DarkModePalette& palette, dm_rgb_map_fn fn) {
    if (!pix || !pix->samples) {
        return;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = pix->n;
    int stride = pix->stride;
    int w = pix->w;
    int h = pix->h;
    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);

    for (int y = 0; y < h; y++) {
        unsigned char* row = pix->samples + y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + x * n;
            float r, g, b;
            if (fastRgb) {
                r = px[0] / 255.f;
                g = px[1] / 255.f;
                b = px[2] / 255.f;
            } else if (fastGray) {
                r = g = b = px[0] / 255.f;
            } else {
                float conv[FZ_MAX_COLORS] = {};
                float srcRgb[FZ_MAX_COLORS] = {};
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    conv[c] = px[c] / 255.f;
                }
                fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
                r = srcRgb[0];
                g = srcRgb[1];
                b = srcRgb[2];
            }
            float nr = 0.f, ng = 0.f, nb = 0.f;
            fn(r, g, b, palette, &nr, &ng, &nb);
            if (fastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (fastGray) {
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(lum * 255.f + 0.5f);
                px[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                float out[FZ_MAX_COLORS] = {nr, ng, nb};
                float back[FZ_MAX_COLORS] = {};
                fz_convert_color(ctx, rgb, out, cs, back, cs, fz_default_color_params);
                for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
                    int v = (int)(back[c] * 255.f + 0.5f);
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > 255) {
                        v = 255;
                    }
                    px[c] = (unsigned char)v;
                }
            }
        }
    }
}

static void dm_preserve_pixel(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                              float* outB) {
    float cr = r, cg = g, cb = b;
    if (ThemeUsesDarkChrome()) {
        PdfDarkModeCompressPhotoHighlights(r, g, b, &cr, &cg, &cb);
    }
    float softening = PdfDarkModeCurrentOptions().preserveImagePaperSoftening;
    if (softening > 0.f) {
        ApplyPreserveImagePaperSoftening(cr, cg, cb, palette, softening, outR, outG, outB);
    } else {
        *outR = cr;
        *outG = cg;
        *outB = cb;
    }
}

static void dm_theme_recolor_pixel(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                                   float* outB) {
    float mapped[3] = {};
    MapRgbToDarkTheme(r, g, b, palette, mapped);
    *outR = mapped[0];
    *outG = mapped[1];
    *outB = mapped[2];
}

static void dm_adaptive_pixel(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                              float* outB) {
    ApplyAdaptiveDocumentDarkMode(r, g, b, palette, outR, outG, outB);
}

// Same remap as legacy UpdateBitmapColors — keeps ink readable on layout rasters.
static void dm_legacy_linear_pixel(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                                   float* outB) {
    *outR = palette.textR + r * palette.diffR;
    *outG = palette.textG + g * palette.diffG;
    *outB = palette.textB + b * palette.diffB;
}

static void dm_shade_pixel(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                           float* outB) {
    float mapped[FZ_MAX_COLORS] = {};
    MapRgbFillToDarkTheme(r, g, b, palette, mapped);
    *outR = mapped[0];
    *outG = mapped[1];
    *outB = mapped[2];
}

static bool dm_picture_book_embedded_photo_page(fz_context* ctx, fz_image* srcImage, float pageCoverage) {
    if (!PdfFollowThemePreservesEmbeddedImageColors() || pageCoverage < kMaxPreserveImagePageCoverage) {
        return false;
    }
    if (!ctx || !srcImage) {
        return false;
    }
    if (!PdfDarkModeImageHasPreservablePhotoRects(ctx, srcImage)) {
        return false;
    }
    // B&W portrait / battlefield photo on a text page — not a red-header band on office paper.
    return PdfDarkModeImageDecodeLooksLikeGrayscalePortrait(ctx, srcImage);
}

static fz_pixmap* dm_copy_and_transform_pixmap(fz_context* ctx, fz_pixmap* src, const DarkModePalette& palette,
                                               dm_rgb_map_fn fn) {
    if (!src || !src->samples) {
        return src;
    }
    fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
    int w = src->w;
    int h = src->h;
    fz_pixmap* dst = fz_new_pixmap(ctx, cs, w, h, src->seps, src->alpha);
    fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, w, h), nullptr);
    dm_transform_pixmap_rgb(ctx, dst, palette, fn);
    return dst;
}

static bool dm_matrix_near_equal(fz_matrix a, fz_matrix b) {
    const float eps = 1e-4f;
    return fabsf(a.a - b.a) < eps && fabsf(a.b - b.b) < eps && fabsf(a.c - b.c) < eps && fabsf(a.d - b.d) < eps &&
           fabsf(a.e - b.e) < eps && fabsf(a.f - b.f) < eps;
}

static bool dm_irect_equal(fz_irect a, fz_irect b) {
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

static fz_pixmap* dm_load_src_pixmap(fz_context* ctx, fz_image* srcImage, int maxDim) {
    int w = srcImage->w;
    int h = srcImage->h;
    if (w <= 0 || h <= 0) {
        return fz_get_pixmap_from_image(ctx, srcImage, nullptr, nullptr, nullptr, nullptr);
    }
    if (maxDim <= 0 || (w <= maxDim && h <= maxDim)) {
        return fz_get_pixmap_from_image(ctx, srcImage, nullptr, nullptr, nullptr, nullptr);
    }
    float s = (float)maxDim / (float)(w > h ? w : h);
    fz_matrix ctm = fz_scale(s, s);
    return fz_get_pixmap_from_image(ctx, srcImage, nullptr, &ctm, nullptr, nullptr);
}

static fz_image* dm_build_processed_image(fz_context* ctx, fz_image* srcImage, DarkImagePolicy policy,
                                          const DarkImageAnalysis* imgAnalysis, float pageCoverage,
                                          const DarkModePalette& palette, DarkModeEngineCache* engineCache) {
    fz_pixmap* src = nullptr;
    fz_pixmap* processed = nullptr;
    fz_image* result = nullptr;
    fz_var(src);
    fz_var(processed);
    fz_var(result);
    fz_try(ctx) {
        const bool layoutTextbookFast = PdfDarkModeEngineCacheLayoutTextbookFastRemap(engineCache);
        bool layoutRaster = policy == DarkImagePolicy::AdaptiveDocument && imgAnalysis &&
                            PdfFollowThemePreservesEmbeddedImageColors() && pageCoverage >= 0.10f &&
                            imgAnalysis->kind != DarkImageKind::Photo &&
                            imgAnalysis->kind != DarkImageKind::FullPageScan;
        // Soft-cap decode for Match-theme image processing. Easy RL figures can be 20–29MP;
        // full-page DuXiu scans are ~6MP — remapping at native size dominates render time.
        int decodeMaxDim = 1600;
        if (layoutTextbookFast) {
            // Acrobat/PageMaker textbooks: cheaper decode; no PictureBook lum/var planes.
            decodeMaxDim = 1200;
        } else if (layoutRaster) {
            decodeMaxDim = 1200;
        } else if (policy == DarkImagePolicy::AdaptiveDocument && imgAnalysis &&
                   imgAnalysis->kind == DarkImageKind::FullPageScan) {
            decodeMaxDim = 1400;
        } else if (policy == DarkImagePolicy::Preserve && pageCoverage >= kMaxPreserveImagePageCoverage) {
            decodeMaxDim = 2000;
        } else if (policy == DarkImagePolicy::Preserve) {
            // Embedded figures (Easy RL CalRGB): keep quality for screen, avoid native 7k decode.
            decodeMaxDim = 1200;
        }
        src = dm_load_src_pixmap(ctx, srcImage, decodeMaxDim);
        // Theme paper and ink colors can be chromatic (for example Dracula #282A36).
        // Keeping a grayscale source pixmap would collapse those colors to luminance,
        // producing #2A2A2A and visibly separating scanned pages from the canvas.
        if (src && src->colorspace && fz_colorspace_is_gray(ctx, src->colorspace)) {
            fz_pixmap* rgbSrc =
                fz_convert_pixmap(ctx, src, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 1);
            fz_drop_pixmap(ctx, src);
            src = rgbSrc;
        }
        if (policy == DarkImagePolicy::Preserve) {
            if (pageCoverage >= kMaxPreserveImagePageCoverage) {
                // Soft-cream notebooks: classifier SoftCream → gentle soften only.
                // RAZ / picture books: sharp dark paper + light text with photo-rect protect.
                if (dm_should_use_government_paper_pixmap(ctx, srcImage, imgAnalysis, pageCoverage)) {
                    processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                } else if (imgAnalysis && PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(imgAnalysis->features)) {
                    processed = PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
                } else if (imgAnalysis && PdfDarkModeFeaturesLookLikeSoftCreamIllustration(imgAnalysis->features)) {
                    if (dm_soft_cream_is_faded_office_scan(imgAnalysis->features, pageCoverage)) {
                        processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                    } else {
                        // Soft cream under FollowTheme too — steep picture-book remap muddies cream.
                        processed = PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
                    }
                } else if (layoutTextbookFast) {
                    // Journey Across Time / Exploring Our World: vector text + full-bleed art.
                    // PictureBook photo-rect + lum/var planes dominate render; cheap preserve remap.
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_preserve_pixel);
                } else {
                    processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                }
            } else if (imgAnalysis && PdfDarkModeFeaturesLookLikeLightDocumentPanel(imgAnalysis->features)) {
                // Cream callout panels ("Do You Know?"): steep paper/ink remap, not Preserve.
                if (layoutTextbookFast) {
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_preserve_pixel);
                } else {
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_legacy_linear_pixel);
                }
            } else {
                processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_preserve_pixel);
            }
        } else if (policy == DarkImagePolicy::AdaptiveDocument) {
            if (imgAnalysis && pageCoverage >= kMaxPreserveImagePageCoverage &&
                imgAnalysis->kind == DarkImageKind::Photo) {
                if (dm_should_use_government_paper_pixmap(ctx, srcImage, imgAnalysis, pageCoverage)) {
                    processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                } else if (PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(imgAnalysis->features)) {
                    processed = PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
                } else if (layoutTextbookFast) {
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_preserve_pixel);
                } else if (PdfFollowThemePreservesEmbeddedImageColors() &&
                           pageCoverage >= kMaxPreserveImagePageCoverage) {
                    processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                } else if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(imgAnalysis->features)) {
                    if (dm_soft_cream_is_faded_office_scan(imgAnalysis->features, pageCoverage)) {
                        processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                    } else {
                        processed = PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
                    }
                } else {
                    processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                }
            } else if (layoutRaster) {
                if (layoutTextbookFast) {
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_preserve_pixel);
                } else {
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_legacy_linear_pixel);
                }
            } else if (imgAnalysis && imgAnalysis->kind == DarkImageKind::FullPageScan) {
                // RAZ / picture-book full-bleed misclassified as FullPageScan: use picture-book
                // sharp paper/ink + photo-rect protect (same black look as Preserve path) instead
                // of whole-tile AdaptiveDocument (photo edge halos). Plain DuXiu text scans stay
                // on AdaptiveDocument (low sat + low chroma).
                const DarkImageFeatures& f = imgAnalysis->features;
                if (layoutTextbookFast) {
                    processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_adaptive_pixel);
                } else {
                    bool grayscalePhotoScan =
                        PdfFollowThemePreservesEmbeddedImageColors() && pageCoverage >= kMaxPreserveImagePageCoverage &&
                        PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) && !PdfDarkModeFeaturesLookLikeBwLineArtScan(f);
                    if (grayscalePhotoScan) {
                        processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                    } else if (dm_picture_book_embedded_photo_page(ctx, srcImage, pageCoverage)) {
                        processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                    } else if (dm_should_use_government_paper_pixmap(ctx, srcImage, imgAnalysis, pageCoverage)) {
                        processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                    } else if (PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f)) {
                        processed = PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
                    } else if (PdfDarkModeFeaturesLookLikeSoftCreamIllustration(f)) {
                        if (dm_soft_cream_is_faded_office_scan(f, pageCoverage)) {
                            processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                        } else {
                            processed = PdfDarkModeProcessSoftCreamPixmap(ctx, src, palette);
                        }
                    } else if (PdfFollowThemePreservesEmbeddedImageColors() &&
                               pageCoverage >= kMaxPreserveImagePageCoverage) {
                        processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                    } else {
                        bool pictureBookishScan = PdfFollowThemePreservesEmbeddedImageColors() &&
                                                  pageCoverage >= kMaxPreserveImagePageCoverage &&
                                                  (f.saturatedPixelRatio >= 0.08f || f.chromaticPixelRatio >= 0.10f);
                        if (pictureBookishScan) {
                            processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                        } else if (f.saturatedPixelRatio < 0.10f) {
                            if (PdfDarkModeImageHasPreservablePhotoRects(ctx, srcImage)) {
                                processed = PdfDarkModeProcessPictureBookPixmap(ctx, src, palette, imgAnalysis);
                            } else if (dm_fullpage_low_chroma_text_scan(f)) {
                                processed = PdfDarkModeProcessGovernmentPaperPixmap(ctx, src, palette);
                            } else {
                                processed = PdfDarkModeProcessScanPixmap(ctx, src, *imgAnalysis, palette);
                            }
                        } else {
                            // Keep ProcessScan when the page has meaningful color (photos on a scan).
                            processed = PdfDarkModeProcessScanPixmap(ctx, src, *imgAnalysis, palette);
                        }
                    }
                } // !layoutTextbookFast
            }
            if (!processed && imgAnalysis && PdfDarkModeShouldBlendLightBackground(*imgAnalysis)) {
                processed = PdfDarkModeProcessLightBackgroundPixmap(ctx, src, *imgAnalysis, palette);
            }
            if (!processed) {
                processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_adaptive_pixel);
            }
        } else {
            processed = dm_copy_and_transform_pixmap(ctx, src, palette, dm_theme_recolor_pixel);
        }
        if (processed == src) {
            fz_drop_pixmap(ctx, src);
            src = nullptr;
            return nullptr;
        }
        fz_drop_pixmap(ctx, src);
        src = nullptr;
        result = fz_new_image_from_pixmap(ctx, processed, nullptr);
        fz_drop_pixmap(ctx, processed);
        processed = nullptr;
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
        if (processed) {
            fz_drop_pixmap(ctx, processed);
        }
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return result;
}

static fz_image* dm_build_processed_shade(fz_context* ctx, fz_shade* shade, fz_matrix ctm, float alpha, fz_irect bounds,
                                          const DarkModePalette& palette) {
    int w = bounds.x1 - bounds.x0;
    int h = bounds.y1 - bounds.y0;
    if (w <= 0 || h <= 0) {
        return nullptr;
    }

    fz_pixmap* pix = nullptr;
    fz_device* shadeDev = nullptr;
    fz_image* result = nullptr;
    fz_var(pix);
    fz_var(shadeDev);
    fz_var(result);
    fz_try(ctx) {
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bounds, nullptr, 1);
        fz_clear_pixmap_with_value(ctx, pix, 0xff);
        fz_matrix local_ctm = fz_concat(fz_translate(-bounds.x0, -bounds.y0), ctm);
        shadeDev = fz_new_draw_device(ctx, local_ctm, pix);
        fz_fill_shade(ctx, shadeDev, shade, fz_identity, alpha, fz_default_color_params);
        fz_close_device(ctx, shadeDev);
        fz_drop_device(ctx, shadeDev);
        shadeDev = nullptr;
        dm_transform_pixmap_rgb(ctx, pix, palette, dm_shade_pixel);
        result = fz_new_image_from_pixmap(ctx, pix, nullptr);
        fz_drop_pixmap(ctx, pix);
        pix = nullptr;
    }
    fz_always(ctx) {
        if (shadeDev) {
            fz_drop_device(ctx, shadeDev);
        }
        if (pix) {
            fz_drop_pixmap(ctx, pix);
        }
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
    return result;
}

fz_image* PdfDarkModeGetCachedImage(fz_context* ctx, DarkModeEngineCache* engineCache, DarkModePageAnalysis* analysis,
                                    int occurrenceIndex, fz_image* srcImage, DarkImagePolicy policy,
                                    const DarkModePalette& palette, u32 profileHash) {
    if (!analysis || !srcImage) {
        return nullptr;
    }
    if (policy != DarkImagePolicy::Preserve && policy != DarkImagePolicy::AdaptiveDocument) {
        return nullptr;
    }
    if (occurrenceIndex < 0 || occurrenceIndex >= analysis->images.Size()) {
        return nullptr;
    }

    const DarkImageAnalysis* imgAnalysis = &analysis->images[occurrenceIndex].analysis;
    float pageCoverage = analysis->images[occurrenceIndex].pageCoverage;
    DarkImageKind kind = imgAnalysis->kind;

    if (engineCache) {
        fz_image* engineHit =
            PdfDarkModeEngineCacheLookupProcessed(ctx, engineCache, srcImage, profileHash, policy, kind);
        if (engineHit) {
            return engineHit;
        }
    }

    DarkModeProcessCache* cache = PdfDarkModeEnsureProcessCache(analysis);
    if (!cache) {
        return nullptr;
    }

    fz_image* cached = cache->processedImages[occurrenceIndex];
    if (cached) {
        return fz_keep_image(ctx, cached);
    }

    fz_image* built = dm_build_processed_image(ctx, srcImage, policy, imgAnalysis, pageCoverage, palette, engineCache);
    if (!built) {
        return nullptr;
    }
    cache->processedImages[occurrenceIndex] = built;
    if (engineCache) {
        PdfDarkModeEngineCacheStoreProcessed(ctx, engineCache, srcImage, profileHash, policy, kind, built);
    }
    return fz_keep_image(ctx, built);
}

fz_image* PdfDarkModeGetCachedFollowThemeImage(fz_context* ctx, DarkModeEngineCache* engineCache, fz_image* srcImage,
                                               DarkImagePolicy policy, float pageCoverage,
                                               const DarkModePalette& palette, u32 profileHash) {
    if (!srcImage) {
        return nullptr;
    }
    DarkImageAnalysis analysis{};
    // Non-full-bleed Preserve (textbook photos): usually draw the original image.
    // Exception: cream/mint callout panels must still remap or they stay bright.
    if (policy == DarkImagePolicy::Preserve && pageCoverage < kMaxPreserveImagePageCoverage) {
        analysis = PdfDarkModeAnalyzeImageCached(ctx, srcImage, pageCoverage, false, engineCache);
        if (!PdfDarkModeFeaturesLookLikeLightDocumentPanel(analysis.features)) {
            return nullptr;
        }
    } else if (policy == DarkImagePolicy::Preserve) {
        if (pageCoverage >= kMaxPreserveImagePageCoverage) {
            analysis = PdfDarkModeAnalyzeImageCached(ctx, srcImage, pageCoverage, false, engineCache);
        }
        if (analysis.kind == DarkImageKind::Unknown) {
            analysis.kind = DarkImageKind::Photo;
        }
    } else if (pageCoverage >= kMaxPreserveImagePageCoverage) {
        analysis = PdfDarkModeAnalyzeImageCached(ctx, srcImage, pageCoverage, false, engineCache);
    } else {
        analysis.kind = DarkImageKind::Unknown;
    }
    DarkImageKind kind = analysis.kind;
    if (engineCache) {
        fz_image* engineHit =
            PdfDarkModeEngineCacheLookupProcessed(ctx, engineCache, srcImage, profileHash, policy, kind);
        if (engineHit) {
            return fz_keep_image(ctx, engineHit);
        }
    }
    fz_image* built = dm_build_processed_image(ctx, srcImage, policy, &analysis, pageCoverage, palette, engineCache);
    if (!built) {
        return nullptr;
    }
    if (engineCache) {
        PdfDarkModeEngineCacheStoreProcessed(ctx, engineCache, srcImage, profileHash, policy, kind, built);
    }
    return fz_keep_image(ctx, built);
}

fz_image* PdfDarkModeGetCachedShade(fz_context* ctx, DarkModePageAnalysis* analysis, fz_shade* shade, fz_matrix ctm,
                                    float alpha, fz_irect bounds, const DarkModePalette& palette) {
    if (!analysis || !shade) {
        return nullptr;
    }

    DarkModeProcessCache* cache = PdfDarkModeEnsureProcessCache(analysis);
    if (!cache) {
        return nullptr;
    }

    for (DarkModeShadeCacheEntry& entry : cache->shadeCache) {
        if (entry.shade == shade && entry.alpha == alpha && dm_irect_equal(entry.bounds, bounds) &&
            dm_matrix_near_equal(entry.ctm, ctm) && entry.processedImage) {
            return fz_keep_image(ctx, entry.processedImage);
        }
    }

    if (cache->shadeCache.Size() >= kMaxShadeCacheEntries) {
        return nullptr;
    }

    fz_image* built = dm_build_processed_shade(ctx, shade, ctm, alpha, bounds, palette);
    if (!built) {
        return nullptr;
    }

    DarkModeShadeCacheEntry entry;
    entry.shade = shade;
    entry.ctm = ctm;
    entry.alpha = alpha;
    entry.bounds = bounds;
    entry.processedImage = built;
    cache->shadeCache.Append(entry);
    return fz_keep_image(ctx, built);
}
