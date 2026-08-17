/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"

#include "PdfDarkModeInternal.h"
#include "PdfDarkModeV2.h"

static constexpr float kV2FullPageCoverage = 0.75f;
// Keep native resolution for page images — downsampling caused scanline/posterize artifacts.
static constexpr int kV2MaxDecodeDim = 4096;

typedef struct {
    fz_device super;
    fz_device* inner;
    const DarkModePalette* palette;
    RectF pageBounds;
    DarkModeEngineCache* engineCache;
    u32 profileHash;
} pdf_dark_mode_v2_device;

// Map paint for paths/text; neutralize soft drop shadows that would become white fringes.
static void v2_map_paint(fz_context* ctx, pdf_dark_mode_v2_device* d, fz_colorspace* cs, const float* color,
                         float alpha, fz_color_params colorParams, float* mapped) {
    float rgb[FZ_MAX_COLORS] = {};
    fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, cs, colorParams);
    if (PdfDarkModeV2IsSoftShadowPaint(rgb[0], rgb[1], rgb[2], alpha)) {
        mapped[0] = d->palette->bgR;
        mapped[1] = d->palette->bgG;
        mapped[2] = d->palette->bgB;
        return;
    }
    MapRgbDarkModeV2(rgb[0], rgb[1], rgb[2], *d->palette, mapped);
}

static float v2_image_coverage(fz_matrix ctm, const RectF& pageBounds) {
    if (pageBounds.IsEmpty() || pageBounds.dx <= 0.f || pageBounds.dy <= 0.f) {
        return 0.f;
    }
    fz_rect bbox = fz_transform_rect(fz_unit_rect, ctm);
    RectF img(bbox.x0, bbox.y0, bbox.x1 - bbox.x0, bbox.y1 - bbox.y0);
    img = img.Intersect(pageBounds);
    if (img.IsEmpty()) {
        return 0.f;
    }
    return (img.dx * img.dy) / (pageBounds.dx * pageBounds.dy);
}

// 公文 scans often sit inside the media box (coverage 0.4–0.75) and were treated as
// small badge images: white-mat knockout leaves light paper + neon red headers.
static bool v2_large_office_scan_image(fz_context* ctx, fz_image* image, float coverage) {
    if (!ctx || !image) {
        return false;
    }
    if (coverage < 0.35f || coverage >= kV2FullPageCoverage) {
        return false;
    }
    if (image->w < 1000 || image->h < 1200) {
        return false;
    }
    DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, true);
    const DarkImageFeatures& f = analysis.features;
    if (PdfDarkModeFeaturesLookLikePhoto(f) || PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) ||
        PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f)) {
        return false;
    }
    return PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f) ||
           PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(f) ||
           PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(f) || PdfDarkModeFeaturesLookLikeBwLineArtScan(f) ||
           (f.highLuminanceRatio > 0.85f && f.saturatedPixelRatio < 0.15f && f.luminanceVariance < 0.040f);
}

static void v2_transform_pixmap(fz_context* ctx, fz_pixmap* pix, const DarkModePalette& palette) {
    if (!pix || !pix->samples) {
        return;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = pix->n;
    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);

    for (int y = 0; y < pix->h; y++) {
        unsigned char* row = pix->samples + (size_t)y * pix->stride;
        for (int x = 0; x < pix->w; x++) {
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
            float mapped[3] = {};
            MapRgbDarkModeV2PageImage(r, g, b, palette, mapped);
            float nr = mapped[0];
            float ng = mapped[1];
            float nb = mapped[2];
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

// MRC / OCR text plates are dark ink (mean lum ~0.15–0.25) drawn through a 1-bit ImageMask.
// Okular chroma invert turns JPEG noise into colored fringes; map luminance only:
// original black ink → theme text, residual paper → theme bg.
static void v2_remap_ink_plate(fz_context* ctx, fz_pixmap* pix, const DarkModePalette& palette) {
    if (!pix || !pix->samples) {
        return;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int components = fz_colorspace_n(ctx, cs);
    int n = pix->n;
    bool fastRgb = cs == rgb || fz_colorspace_is_rgb(ctx, cs);
    bool fastGray = components == 1 || fz_colorspace_is_gray(ctx, cs);

    for (int y = 0; y < pix->h; y++) {
        unsigned char* row = pix->samples + (size_t)y * pix->stride;
        for (int x = 0; x < pix->w; x++) {
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
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float inkW = 1.f - lum;
            float nr = palette.textR * inkW + palette.bgR * lum;
            float ng = palette.textG * inkW + palette.bgG * lum;
            float nb = palette.textB * inkW + palette.bgB * lum;
            if (fastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (fastGray) {
                float outLum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(outLum * 255.f + 0.5f);
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

static fz_image* v2_build_white_mat_image(fz_context* ctx, fz_image* srcImage, const DarkModePalette& palette) {
    fz_pixmap* src = nullptr;
    fz_pixmap* dst = nullptr;
    fz_image* result = nullptr;
    fz_var(src);
    fz_var(dst);
    fz_var(result);
    fz_try(ctx) {
        int w = srcImage->w;
        int h = srcImage->h;
        if (w > 0 && h > 0 && (w > kV2MaxDecodeDim || h > kV2MaxDecodeDim)) {
            // Huge images are never textbook badge mats.
            fz_throw(ctx, FZ_ERROR_GENERIC, "skip white-mat on huge image");
        }
        src = fz_get_pixmap_from_image(ctx, srcImage, nullptr, nullptr, nullptr, nullptr);
        if (src && src->samples) {
            dst = PdfDarkModeProcessV2WhiteMatPixmap(ctx, src, palette);
            if (!dst) {
                // Glencoe-style callout drop shadows: soft gray rasters, not white mats.
                dst = PdfDarkModeProcessV2SoftShadowPlatePixmap(ctx, src, palette);
            }
            if (dst) {
                result = fz_new_image_from_pixmap(ctx, dst, nullptr);
            }
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
        if (dst) {
            fz_drop_pixmap(ctx, dst);
        }
    }
    fz_catch(ctx) {
        if (result) {
            fz_drop_image(ctx, result);
        }
        return nullptr;
    }
    return result;
}

static fz_image* v2_build_page_image(fz_context* ctx, fz_image* srcImage, const DarkModePalette& palette) {
    fz_pixmap* src = nullptr;
    fz_pixmap* dst = nullptr;
    fz_image* result = nullptr;
    fz_var(src);
    fz_var(dst);
    fz_var(result);
    fz_try(ctx) {
        int w = srcImage->w;
        int h = srcImage->h;
        if (w > 0 && h > 0 && (w > kV2MaxDecodeDim || h > kV2MaxDecodeDim)) {
            float s = (float)kV2MaxDecodeDim / (float)(w > h ? w : h);
            fz_matrix scale = fz_scale(s, s);
            src = fz_get_pixmap_from_image(ctx, srcImage, nullptr, &scale, nullptr, nullptr);
        } else {
            src = fz_get_pixmap_from_image(ctx, srcImage, nullptr, nullptr, nullptr, nullptr);
        }
        // A gray destination cannot represent a chromatic theme background. Promote
        // before remapping so Dracula and similar palettes retain their exact tint.
        if (src && src->colorspace && fz_colorspace_is_gray(ctx, src->colorspace)) {
            fz_pixmap* rgbSrc =
                fz_convert_pixmap(ctx, src, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 1);
            fz_drop_pixmap(ctx, src);
            src = rgbSrc;
        }
        if (src && src->samples) {
            // Photo rects preserved; margins / paper / baked text → Okular→theme.
            // Analyze so B&W documentary portraits (RAZ Abraham Lincoln) seek photo rects.
            DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, srcImage, 0.97f, true);
            dst = PdfDarkModeProcessV2FullPagePixmap(ctx, src, palette, &analysis);
            if (!dst) {
                fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
                dst = fz_new_pixmap(ctx, cs, src->w, src->h, src->seps, src->alpha);
                fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, src->w, src->h), nullptr);
                v2_transform_pixmap(ctx, dst, palette);
            }
            result = fz_new_image_from_pixmap(ctx, dst, nullptr);
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
        if (dst) {
            fz_drop_pixmap(ctx, dst);
        }
    }
    fz_catch(ctx) {
        if (result) {
            fz_drop_image(ctx, result);
        }
        return nullptr;
    }
    return result;
}

static fz_image* v2_build_masked_ink_image(fz_context* ctx, fz_image* srcImage, const DarkModePalette& palette) {
    fz_pixmap* src = nullptr;
    fz_pixmap* dst = nullptr;
    fz_image* result = nullptr;
    fz_var(src);
    fz_var(dst);
    fz_var(result);
    fz_try(ctx) {
        src = fz_get_pixmap_from_image(ctx, srcImage, nullptr, nullptr, nullptr, nullptr);
        if (src && src->samples) {
            fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
            dst = fz_new_pixmap(ctx, cs, src->w, src->h, src->seps, src->alpha);
            fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, src->w, src->h), nullptr);
            v2_remap_ink_plate(ctx, dst, palette);
            // Keep the JBIG2 / ImageMask stencil so only ink paints over the background plate.
            result = fz_new_image_from_pixmap(ctx, dst, srcImage->mask);
        }
    }
    fz_always(ctx) {
        if (src) {
            fz_drop_pixmap(ctx, src);
        }
        if (dst) {
            fz_drop_pixmap(ctx, dst);
        }
    }
    fz_catch(ctx) {
        if (result) {
            fz_drop_image(ctx, result);
        }
        return nullptr;
    }
    return result;
}

static void v2_close(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    if (d->inner && d->inner->close_device) {
        d->inner->close_device(ctx, d->inner);
    }
}

static void v2_drop(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    if (d->inner) {
        fz_drop_device(ctx, d->inner);
        d->inner = nullptr;
    }
}

static void v2_fill_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                         fz_colorspace* colorspace, const float* color, float alpha, fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped);
    fz_fill_path(ctx, d->inner, path, even_odd, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                           fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                           fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped);
    fz_stroke_path(ctx, d->inner, path, stroke, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_colorspace* colorspace,
                         const float* color, float alpha, fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped);
    fz_fill_text(ctx, d->inner, text, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                           fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                           fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped);
    fz_stroke_text(ctx, d->inner, text, stroke, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_fill_shade(fz_context* ctx, fz_device* dev, fz_shade* shd, fz_matrix ctm, float alpha,
                          fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_fill_shade(ctx, d->inner, shd, ctm, alpha, color_params);
}

static void v2_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                          fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    if (!image) {
        fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
        return;
    }
    float coverage = v2_image_coverage(ctm, d->pageBounds);
    bool largeOffice = v2_large_office_scan_image(ctx, image, coverage);

    // Small/medium images: knock out JPEG white mats around colorful badges (UNIT / Atlas).
    // Full-page path below handles scans; do not Okular-wash ordinary photos here.
    if (coverage < kV2FullPageCoverage && !largeOffice) {
        fz_image* cached = nullptr;
        if (d->engineCache) {
            cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                           DarkImagePolicy::Preserve, DarkImageKind::IconOrLineArt);
        }
        fz_image* built = nullptr;
        fz_image* draw = cached;
        if (!draw) {
            built = v2_build_white_mat_image(ctx, image, *d->palette);
            draw = built ? built : image;
            if (built && d->engineCache) {
                PdfDarkModeEngineCacheStoreProcessed(ctx, d->engineCache, image, d->profileHash,
                                                     DarkImagePolicy::Preserve, DarkImageKind::IconOrLineArt, built);
            }
        }
        fz_try(ctx) {
            fz_fill_image(ctx, d->inner, draw, ctm, alpha, color_params);
        }
        fz_always(ctx) {
            if (cached) {
                fz_drop_image(ctx, cached);
            }
            if (built) {
                fz_drop_image(ctx, built);
            }
        }
        fz_catch(ctx) {
            fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
        }
        return;
    }

    // MRC text plate: color JPEG + 1-bit ImageMask. Must keep the mask (rebuilding from a
    // pixmap without it paints an opaque dark plate over the page and hides all text).
    if (image->mask) {
        fz_image* cached = nullptr;
        if (d->engineCache) {
            cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                           DarkImagePolicy::ThemeRecolor, DarkImageKind::IconOrLineArt);
        }
        fz_image* built = nullptr;
        fz_image* draw = cached;
        if (!draw) {
            built = v2_build_masked_ink_image(ctx, image, *d->palette);
            draw = built ? built : image;
            if (built && d->engineCache) {
                PdfDarkModeEngineCacheStoreProcessed(ctx, d->engineCache, image, d->profileHash,
                                                     DarkImagePolicy::ThemeRecolor, DarkImageKind::IconOrLineArt,
                                                     built);
            }
        }
        fz_try(ctx) {
            fz_fill_image(ctx, d->inner, draw, ctm, alpha, color_params);
        }
        fz_always(ctx) {
            if (cached) {
                fz_drop_image(ctx, cached);
            }
            if (built) {
                fz_drop_image(ctx, built);
            }
        }
        fz_catch(ctx) {
            fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
        }
        return;
    }

    // Full-page raster: remap once and cache (RAZ / scans).
    fz_image* cached = nullptr;
    if (d->engineCache) {
        cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                       DarkImagePolicy::ThemeRecolor, DarkImageKind::FullPageScan);
    }
    fz_image* built = nullptr;
    fz_image* draw = cached;
    if (!draw) {
        built = v2_build_page_image(ctx, image, *d->palette);
        draw = built ? built : image;
        if (built && d->engineCache) {
            PdfDarkModeEngineCacheStoreProcessed(ctx, d->engineCache, image, d->profileHash,
                                                 DarkImagePolicy::ThemeRecolor, DarkImageKind::FullPageScan, built);
        }
    }
    fz_try(ctx) {
        fz_fill_image(ctx, d->inner, draw, ctm, alpha, color_params);
    }
    fz_always(ctx) {
        if (cached) {
            fz_drop_image(ctx, cached);
        }
        if (built) {
            fz_drop_image(ctx, built);
        }
    }
    fz_catch(ctx) {
        fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
    }
}

static void v2_fill_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                               fz_colorspace* colorspace, const float* color, float alpha,
                               fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped);
    fz_fill_image_mask(ctx, d->inner, image, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_clip_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                         fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_clip_path(ctx, d->inner, path, even_odd, ctm, scissor);
}

static void v2_clip_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                                fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_clip_stroke_path(ctx, d->inner, path, stroke, ctm, scissor);
}

static void v2_clip_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_clip_text(ctx, d->inner, text, ctm, scissor);
}

static void v2_clip_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                                fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_clip_stroke_text(ctx, d->inner, text, stroke, ctm, scissor);
}

static void v2_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_clip_image_mask(ctx, d->inner, image, ctm, scissor);
}

static void v2_pop_clip(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_pop_clip(ctx, d->inner);
}

static void v2_begin_mask(fz_context* ctx, fz_device* dev, fz_rect area, int luminosity, fz_colorspace* colorspace,
                          const float* bc, fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_begin_mask(ctx, d->inner, area, luminosity, colorspace, bc, color_params);
}

static void v2_end_mask(fz_context* ctx, fz_device* dev, fz_function* fn) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_end_mask_tr(ctx, d->inner, fn);
}

static void v2_begin_group(fz_context* ctx, fz_device* dev, fz_rect area, fz_colorspace* cs, int isolated, int knockout,
                           int blendmode, float alpha) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_begin_group(ctx, d->inner, area, cs, isolated, knockout, blendmode, alpha);
}

static void v2_end_group(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_end_group(ctx, d->inner);
}

static int v2_begin_tile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep, float ystep,
                         fz_matrix ctm, int id, int doc_id) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    return fz_begin_tile_tid(ctx, d->inner, area, view, xstep, ystep, ctm, id, doc_id);
}

static void v2_end_tile(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_end_tile(ctx, d->inner);
}

static void v2_render_flags(fz_context* ctx, fz_device* dev, int set, int clear) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_render_flags(ctx, d->inner, set, clear);
}

static void v2_set_default_colorspaces(fz_context* ctx, fz_device* dev, fz_default_colorspaces* cs) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_set_default_colorspaces(ctx, d->inner, cs);
}

static void v2_begin_layer(fz_context* ctx, fz_device* dev, const char* layer_name) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_begin_layer(ctx, d->inner, layer_name);
}

static void v2_end_layer(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_end_layer(ctx, d->inner);
}

static void v2_begin_structure(fz_context* ctx, fz_device* dev, fz_structure standard, const char* raw, int uid) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_begin_structure(ctx, d->inner, standard, raw, uid);
}

static void v2_end_structure(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_end_structure(ctx, d->inner);
}

static void v2_begin_metatext(fz_context* ctx, fz_device* dev, fz_metatext meta, const char* text) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_begin_metatext(ctx, d->inner, meta, text);
}

static void v2_end_metatext(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_end_metatext(ctx, d->inner);
}

static void v2_ignore_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    fz_ignore_text(ctx, d->inner, text, ctm);
}

fz_device* PdfDarkModeWrapV2Device(fz_context* ctx, fz_device* inner, const DarkModePalette* palette,
                                   const RectF& pageBounds, DarkModeEngineCache* engineCache, u32 profileHash) {
    pdf_dark_mode_v2_device* d = fz_new_derived_device(ctx, pdf_dark_mode_v2_device);
    d->inner = inner;
    d->palette = palette;
    d->pageBounds = pageBounds;
    d->engineCache = engineCache;
    d->profileHash = profileHash;

    d->super.close_device = v2_close;
    d->super.drop_device = v2_drop;
    d->super.fill_path = v2_fill_path;
    d->super.stroke_path = v2_stroke_path;
    d->super.fill_text = v2_fill_text;
    d->super.stroke_text = v2_stroke_text;
    d->super.fill_shade = v2_fill_shade;
    d->super.fill_image = v2_fill_image;
    d->super.fill_image_mask = v2_fill_image_mask;
    d->super.clip_path = v2_clip_path;
    d->super.clip_stroke_path = v2_clip_stroke_path;
    d->super.clip_text = v2_clip_text;
    d->super.clip_stroke_text = v2_clip_stroke_text;
    d->super.clip_image_mask = v2_clip_image_mask;
    d->super.pop_clip = v2_pop_clip;
    d->super.begin_mask = v2_begin_mask;
    d->super.end_mask = v2_end_mask;
    d->super.begin_group = v2_begin_group;
    d->super.end_group = v2_end_group;
    d->super.begin_tile = v2_begin_tile;
    d->super.end_tile = v2_end_tile;
    d->super.render_flags = v2_render_flags;
    d->super.set_default_colorspaces = v2_set_default_colorspaces;
    d->super.begin_layer = v2_begin_layer;
    d->super.end_layer = v2_end_layer;
    d->super.begin_structure = v2_begin_structure;
    d->super.end_structure = v2_end_structure;
    d->super.begin_metatext = v2_begin_metatext;
    d->super.end_metatext = v2_end_metatext;
    d->super.ignore_text = v2_ignore_text;

    return &d->super;
}
