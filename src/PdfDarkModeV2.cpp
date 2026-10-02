/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"
#include "utils/Log.h"
#include "utils/Timer.h"

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
    fz_rect clipStack[24];
    int clipTop;
    int clipExtra;
} pdf_dark_mode_v2_device;

static void v2_push_clip(pdf_dark_mode_v2_device* d, fz_rect scissor) {
    fz_rect next = fz_intersect_rect(d->clipStack[d->clipTop], scissor);
    if (d->clipTop + 1 < 24) {
        d->clipTop++;
        d->clipStack[d->clipTop] = next;
    } else {
        d->clipExtra++;
    }
}

static void v2_pop_clip_rect(pdf_dark_mode_v2_device* d) {
    if (d->clipExtra > 0) {
        d->clipExtra--;
    } else if (d->clipTop > 0) {
        d->clipTop--;
    }
}

struct V2PerfCounters {
    int mapCalls = 0;
    int textCalls = 0;
    int pathCalls = 0;
    int groupCalls = 0;
    double mapMs = 0;
    double textMs = 0;
    double pathMs = 0;
};

static V2PerfCounters gV2Perf;

void PdfDarkModeV2FlushPerfLog() {
    if (!PdfDarkModePagePerfOn()) {
        return;
    }
    logf("page-perf v2-ops text=%d path=%d map=%d mapMs=%.1f textMs=%.1f pathMs=%.1f groups=%d\n", gV2Perf.textCalls,
         gV2Perf.pathCalls, gV2Perf.mapCalls, gV2Perf.mapMs, gV2Perf.textMs, gV2Perf.pathMs, gV2Perf.groupCalls);
    gV2Perf = {};
}

// Yellow highlight rules are light and saturated. Okular invert leaves that yellow
// in place, then black text becomes light and disappears on it.
static bool v2_is_light_marker(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return lum > 0.62f && (maxC - minC) > 0.18f;
}

// Light cyan/cream plates under atlas map silhouettes (Exploring Our World table).
// Okular invert leaves a pale rectangle on dark table cells — skip the fill instead.
static bool v2_is_atlas_pastel_plate(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if (lum <= 0.78f || chroma >= 0.36f) {
        return false;
    }
    // Warm highlighter yellows/oranges stay on the marker path — not ocean plates.
    if (r > b + 0.08f && g > b + 0.05f && chroma > 0.20f) {
        return false;
    }
    return true;
}

// Map paint for paths/text; neutralize soft drop shadows that would become white fringes.
// marker: fills and strokes. Light highlight colors are parked darker. Text stays
// on the ordinary invert so black glyphs become light.
static void v2_map_paint(fz_context* ctx, pdf_dark_mode_v2_device* d, fz_colorspace* cs, const float* color,
                         float alpha, fz_color_params colorParams, float* mapped, bool marker) {
    bool perf = PdfDarkModePagePerfOn();
    LARGE_INTEGER mapStart = {};
    if (perf) {
        gV2Perf.mapCalls++;
        mapStart = TimeGet();
    }
    float rgb[FZ_MAX_COLORS] = {};
    fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, cs, colorParams);
    if (perf) {
        gV2Perf.mapMs += TimeSinceInMs(mapStart);
    }
    if (PdfDarkModeV2IsSoftShadowPaint(rgb[0], rgb[1], rgb[2], alpha)) {
        mapped[0] = d->palette->bgR;
        mapped[1] = d->palette->bgG;
        mapped[2] = d->palette->bgB;
        return;
    }
    if (marker && v2_is_light_marker(rgb[0], rgb[1], rgb[2])) {
        MapRgbLightMarkerToDarkTheme(rgb[0], rgb[1], rgb[2], *d->palette, mapped);
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

static float v2_path_coverage(fz_context* ctx, const fz_path* path, fz_matrix ctm, const RectF& pageBounds) {
    if (!path || pageBounds.IsEmpty()) {
        return 1.f;
    }
    fz_rect bounds = fz_bound_path(ctx, path, nullptr, ctm);
    RectF box(bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
    box = box.Intersect(pageBounds);
    if (box.IsEmpty() || pageBounds.dx <= 0.f || pageBounds.dy <= 0.f) {
        return 0.f;
    }
    return (box.dx * box.dy) / (pageBounds.dx * pageBounds.dy);
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

// Photographs that are not inverted still cannot keep a studio-white card.
// Luminance above 0.55 is pulled toward 0.62. Darker pixels are copied through.
static void v2_press_highlights_pixmap(fz_pixmap* pix) {
    if (!pix || !pix->samples || pix->w <= 0 || pix->h <= 0) {
        return;
    }
    int n = pix->n;
    int comps = n - pix->alpha;
    if (comps < 1 || n < 1) {
        return;
    }
    for (int y = 0; y < pix->h; y++) {
        unsigned char* row = pix->samples + (size_t)y * (size_t)pix->stride;
        for (int x = 0; x < pix->w; x++) {
            unsigned char* px = row + (size_t)x * (size_t)n;
            float r, g, b;
            if (comps >= 3) {
                r = px[0] / 255.f;
                g = px[1] / 255.f;
                b = px[2] / 255.f;
            } else {
                r = g = b = px[0] / 255.f;
            }
            float or_, og, ob;
            PdfDarkModeCompressPhotoHighlights(r, g, b, &or_, &og, &ob);
            auto put = [](float v) -> unsigned char {
                int i = (int)(v * 255.f + 0.5f);
                if (i < 0) {
                    i = 0;
                }
                if (i > 255) {
                    i = 255;
                }
                return (unsigned char)i;
            };
            if (comps >= 3) {
                px[0] = put(or_);
                px[1] = put(og);
                px[2] = put(ob);
            } else {
                px[0] = put(or_);
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
        if (srcImage->mask) {
            // Rebuilding from the color pixmap drops the SMask and paints a solid
            // rectangle (Word 红头 2×2 red stretched across the header).
            fz_throw(ctx, FZ_ERROR_GENERIC, "skip white-mat on masked image");
        }
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
            if (!dst && !PdfDarkModeImageShouldStayOriginal(ctx, srcImage)) {
                // Not a photograph: press studio-white cards. Photos stay original
                // when there is no white margin to flood.
                fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
                dst = fz_new_pixmap(ctx, cs, src->w, src->h, src->seps, src->alpha);
                fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, src->w, src->h), nullptr);
                v2_press_highlights_pixmap(dst);
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

static bool v2_image_is_paint_chip(fz_image* image) {
    if (!image || !image->mask) {
        return false;
    }
    return PdfDarkModeV2LooksLikeSoftMaskPaintChip(image->w, image->h, image->mask->w, image->mask->h);
}

// Remap the tiny color plate (black → theme text, red 红头 keeps hue) and keep the SMask.
static fz_image* v2_build_paint_chip_image(fz_context* ctx, fz_image* srcImage, const DarkModePalette& palette) {
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
            v2_transform_pixmap(ctx, dst, palette);
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

static bool v2_office_scan_may_decode_at_view(const DarkImageAnalysis& analysis) {
    if (analysis.kind == DarkImageKind::Photo) {
        return false;
    }
    const DarkImageFeatures& f = analysis.features;
    if (PdfDarkModeFeaturesLookLikePhoto(f) || PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) ||
        PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f)) {
        return false;
    }
    return analysis.kind == DarkImageKind::FullPageScan || PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(f) ||
           PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(f) ||
           PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f);
}

static fz_image* v2_build_page_image(fz_context* ctx, fz_image* srcImage, const DarkModePalette& palette, int destW,
                                     int destH, bool* outDownsampled) {
    fz_pixmap* src = nullptr;
    fz_pixmap* dst = nullptr;
    fz_image* result = nullptr;
    fz_var(src);
    fz_var(dst);
    fz_var(result);
    if (outDownsampled) {
        *outDownsampled = false;
    }
    LARGE_INTEGER pageImageStart = TimeGet();
    double analyzeMs = 0;
    double decodeMs = 0;
    fz_try(ctx) {
        // Thumbnail classify first. Office/text scans can remap at view size;
        // native 1–6MP decode+float remap is why dark-mode 公文 shows
        // "Please wait - rendering..." while light theme stays snappy.
        LARGE_INTEGER a0 = TimeGet();
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, srcImage, 0.97f, true);
        analyzeMs = TimeSinceInMs(a0);
        int w = srcImage->w;
        int h = srcImage->h;
        int maxDim = kV2MaxDecodeDim;
        if (v2_office_scan_may_decode_at_view(analysis)) {
            int need = destW > destH ? destW : destH;
            if (need < 8) {
                need = 1600;
            }
            need = (need * 5) / 4;
            if (need > 2048) {
                need = 2048;
            }
            if (need < maxDim) {
                maxDim = need;
                if (outDownsampled) {
                    *outDownsampled = true;
                }
            }
        }
        LARGE_INTEGER d0 = TimeGet();
        if (w > 0 && h > 0 && (w > maxDim || h > maxDim)) {
            float s = (float)maxDim / (float)(w > h ? w : h);
            fz_matrix scale = fz_scale(s, s);
            src = fz_get_pixmap_from_image(ctx, srcImage, nullptr, &scale, nullptr, nullptr);
        } else {
            src = fz_get_pixmap_from_image(ctx, srcImage, nullptr, nullptr, nullptr, nullptr);
        }
        decodeMs = TimeSinceInMs(d0);
        // Gray and CMYK cannot store a chromatic theme background. Indexed textbook
        // plates decode to CMYK; writing the theme color back into that space leaves
        // the original cream. Promote before remapping.
        fz_colorspace* deviceRgb = fz_device_rgb(ctx);
        if (src && src->colorspace && src->colorspace != deviceRgb && !fz_colorspace_is_rgb(ctx, src->colorspace)) {
            fz_pixmap* rgbSrc = fz_convert_pixmap(ctx, src, deviceRgb, nullptr, nullptr, fz_default_color_params, 1);
            fz_drop_pixmap(ctx, src);
            src = rgbSrc;
        }
        if (src && src->samples) {
            // Photo rects preserved; margins / paper / baked text → Okular→theme.
            dst = PdfDarkModeProcessV2FullPagePixmap(ctx, src, palette, &analysis);
            if (!dst) {
                fz_colorspace* cs = src->colorspace ? src->colorspace : fz_device_rgb(ctx);
                dst = fz_new_pixmap(ctx, cs, src->w, src->h, src->seps, src->alpha);
                fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, src->w, src->h), nullptr);
                v2_transform_pixmap(ctx, dst, palette);
            }
            result = fz_new_image_from_pixmap(ctx, dst, nullptr);
        }
        if (PdfDarkModePagePerfOn()) {
            logf("page-perf page-image src=%dx%d out=%dx%d maxDim=%d analyze=%.1f decode=%.1f total=%.1f ms\n",
                 srcImage->w, srcImage->h, src ? src->w : 0, src ? src->h : 0, maxDim, analyzeMs, decodeMs,
                 TimeSinceInMs(pageImageStart));
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
            fz_colorspace* deviceRgb = fz_device_rgb(ctx);
            // CalRGB (Easy RL) must become DeviceRGB before writing theme paper bytes;
            // otherwise #282A36 is re-decoded to a darker charcoal and no longer matches.
            if (src->colorspace && src->colorspace != deviceRgb) {
                fz_pixmap* rgb = fz_convert_pixmap(ctx, src, deviceRgb, nullptr, nullptr, fz_default_color_params, 1);
                fz_drop_pixmap(ctx, src);
                src = rgb;
            }
            dst = fz_new_pixmap(ctx, deviceRgb, src->w, src->h, src->seps, src->alpha);
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

static bool v2_image_wants_full_page_process(const DarkImageAnalysis& analysis) {
    if (analysis.kind == DarkImageKind::Photo) {
        return false;
    }
    const DarkImageFeatures& f = analysis.features;
    if (PdfDarkModeFeaturesLookLikePhoto(f) || PdfDarkModeFeaturesLookLikeGrayscalePhoto(f) ||
        PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(f)) {
        return false;
    }
    if (analysis.kind == DarkImageKind::FullPageScan) {
        return true;
    }
    return PdfDarkModeFeaturesLookLikeGovernmentPaperScan(f) ||
           PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(f) ||
           PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(f) || PdfDarkModeFeaturesLookLikeBwLineArtScan(f);
}

fz_image* PdfDarkModeAutoProcessImage(fz_context* ctx, fz_image* image, const DarkModePalette& palette) {
    if (!ctx || !image || image->imagemask) {
        return nullptr;
    }
    if (v2_image_is_paint_chip(image)) {
        return v2_build_paint_chip_image(ctx, image, palette);
    }
    // No page box here. A large scan is treated like a PDF image that covers the page.
    // A figure skips that analysis: white mat, soft shadow, or a cheap highlight press.
    bool big = image->w >= 1000 && image->h >= 800;
    if (!image->mask && !big) {
        return v2_build_white_mat_image(ctx, image, palette);
    }
    DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, big ? 0.90f : 0.40f, big);
    bool fullPage = v2_image_wants_full_page_process(analysis);
    if (image->mask) {
        if (!fullPage && !big) {
            return nullptr;
        }
        return v2_build_masked_ink_image(ctx, image, palette);
    }
    if (fullPage) {
        return v2_build_page_image(ctx, image, palette, image->w, image->h, nullptr);
    }
    return v2_build_white_mat_image(ctx, image, palette);
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
    bool perf = PdfDarkModePagePerfOn();
    LARGE_INTEGER opStart = {};
    if (perf) {
        gV2Perf.pathCalls++;
        opStart = TimeGet();
    }
    float rgb[FZ_MAX_COLORS] = {};
    fz_convert_color(ctx, colorspace, color, fz_device_rgb(ctx), rgb, colorspace, color_params);
    // Tiny atlas map ocean/cream plates: skip so the dark table cell shows through.
    if (v2_is_atlas_pastel_plate(rgb[0], rgb[1], rgb[2]) && v2_path_coverage(ctx, path, ctm, d->pageBounds) < 0.04f) {
        if (perf) {
            gV2Perf.pathMs += TimeSinceInMs(opStart);
        }
        return;
    }
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped, true);
    fz_fill_path(ctx, d->inner, path, even_odd, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
    if (perf) {
        gV2Perf.pathMs += TimeSinceInMs(opStart);
    }
}

static void v2_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                           fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                           fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped, true);
    fz_stroke_path(ctx, d->inner, path, stroke, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_colorspace* colorspace,
                         const float* color, float alpha, fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    bool perf = PdfDarkModePagePerfOn();
    LARGE_INTEGER opStart = {};
    if (perf) {
        gV2Perf.textCalls++;
        opStart = TimeGet();
    }
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped, false);
    fz_fill_text(ctx, d->inner, text, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
    if (perf) {
        gV2Perf.textMs += TimeSinceInMs(opStart);
    }
}

static void v2_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                           fz_matrix ctm, fz_colorspace* colorspace, const float* color, float alpha,
                           fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    float mapped[FZ_MAX_COLORS] = {};
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped, false);
    fz_stroke_text(ctx, d->inner, text, stroke, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_recolor_shade_pixmap(fz_pixmap* pix, const DarkModePalette& palette) {
    if (!pix || !pix->samples || pix->n < 3) {
        return;
    }
    int n = pix->n;
    int samples = 0;
    int shadowish = 0;
    int stepX = pix->w / 8;
    int stepY = pix->h / 8;
    if (stepX < 1) {
        stepX = 1;
    }
    if (stepY < 1) {
        stepY = 1;
    }
    for (int y = 0; y < pix->h; y += stepY) {
        unsigned char* row = pix->samples + (size_t)y * pix->stride;
        for (int x = 0; x < pix->w; x += stepX) {
            unsigned char* px = row + (size_t)x * n;
            float r = px[0] / 255.f;
            float g = px[1] / 255.f;
            float b = px[2] / 255.f;
            float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (maxC - minC < 0.08f && lum < 0.45f) {
                shadowish++;
            }
            samples++;
        }
    }
    // A dark neutral shade is a drop shadow. Inverting it paints a light fringe.
    bool shadow = samples > 0 && shadowish * 10 >= samples * 7;
    for (int y = 0; y < pix->h; y++) {
        unsigned char* row = pix->samples + (size_t)y * pix->stride;
        for (int x = 0; x < pix->w; x++) {
            unsigned char* px = row + (size_t)x * n;
            float mapped[3];
            if (shadow) {
                mapped[0] = palette.bgR;
                mapped[1] = palette.bgG;
                mapped[2] = palette.bgB;
            } else {
                // Okular invert leaves pure yellow / cyan / magenta where they are
                // (lightness stays ~0.5). A callout gradient is a background: reseat
                // lightness the same way as smart-invert pictures.
                MapRgbToDarkThemeOklab(px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, palette, mapped);
            }
            int vr = (int)(mapped[0] * 255.f + 0.5f);
            int vg = (int)(mapped[1] * 255.f + 0.5f);
            int vb = (int)(mapped[2] * 255.f + 0.5f);
            px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
            px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
            px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
        }
    }
}

// Gradients are not path fills. Leaving them alone keeps a bright swoosh on a
// dark page (the yellow reading-guide oval) while the book image beside it inverts.
static void v2_fill_shade(fz_context* ctx, fz_device* dev, fz_shade* shd, fz_matrix ctm, float alpha,
                          fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    if (!shd || alpha == 0.f || !d->palette) {
        fz_fill_shade(ctx, d->inner, shd, ctm, alpha, color_params);
        return;
    }
    fz_rect bounds = fz_bound_shade(ctx, shd, ctm);
    fz_rect page = fz_make_rect(d->pageBounds.x, d->pageBounds.y, d->pageBounds.x + d->pageBounds.dx,
                                d->pageBounds.y + d->pageBounds.dy);
    bounds = fz_intersect_rect(bounds, page);
    if (d->clipTop >= 0) {
        bounds = fz_intersect_rect(bounds, d->clipStack[d->clipTop]);
    }
    if (fz_is_empty_rect(bounds)) {
        return;
    }
    float bw = bounds.x1 - bounds.x0;
    float bh = bounds.y1 - bounds.y0;
    if (bw < 0.5f || bh < 0.5f) {
        return;
    }
    float scale = 2.f;
    if (bw * scale > 2048.f) {
        scale = 2048.f / bw;
    }
    if (bh * scale > 2048.f) {
        scale = 2048.f / bh;
    }
    if (scale < 1.f) {
        scale = 1.f;
    }
    int w = (int)(bw * scale + 0.5f);
    int h = (int)(bh * scale + 0.5f);
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    if (w > 2048 || h > 2048) {
        fz_fill_shade(ctx, d->inner, shd, ctm, alpha, color_params);
        return;
    }

    fz_pixmap* pix = nullptr;
    fz_device* shadeDev = nullptr;
    fz_image* image = nullptr;
    fz_var(pix);
    fz_var(shadeDev);
    fz_var(image);
    fz_try(ctx) {
        fz_irect ib = fz_make_irect(0, 0, w, h);
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), ib, nullptr, 1);
        fz_clear_pixmap_with_value(ctx, pix, 0xff);
        fz_matrix local = fz_concat(fz_translate(-bounds.x0, -bounds.y0), ctm);
        local = fz_concat(fz_scale(scale, scale), local);
        shadeDev = fz_new_draw_device(ctx, local, pix);
        // Page overprint / DeviceN spots are not available on this temporary
        // device. Paint the alternate color, then reseat lightness.
        fz_fill_shade(ctx, shadeDev, shd, fz_identity, alpha, fz_default_color_params);
        fz_close_device(ctx, shadeDev);
        fz_drop_device(ctx, shadeDev);
        shadeDev = nullptr;
        v2_recolor_shade_pixmap(pix, *d->palette);
        image = fz_new_image_from_pixmap(ctx, pix, nullptr);
        fz_drop_pixmap(ctx, pix);
        pix = nullptr;
        fz_matrix imageCtm = fz_make_matrix(bw, 0.f, 0.f, bh, bounds.x0, bounds.y0);
        fz_fill_image(ctx, d->inner, image, imageCtm, 1.f, color_params);
    }
    fz_always(ctx) {
        if (shadeDev) {
            fz_drop_device(ctx, shadeDev);
        }
        if (pix) {
            fz_drop_pixmap(ctx, pix);
        }
        if (image) {
            fz_drop_image(ctx, image);
        }
    }
    fz_catch(ctx) {
        logf("shade-recolor failed: %s\n", fz_caught_message(ctx));
        fz_fill_shade(ctx, d->inner, shd, ctm, alpha, color_params);
    }
}

// Manual menu choice. 1-bit ImageMask ink plates (MRC) stay on the automatic
// path: rebuilding them without the stencil paints an opaque plate over text.
// Soft masks (Easy RL textbook figures) remapped under Tone keep their SMask.
static bool v2_mask_is_mrc_stencil(fz_image* image) {
    fz_image* mask = image ? image->mask : nullptr;
    if (!mask) {
        return false;
    }
    return mask->imagemask || mask->bpc <= 1;
}

static bool v2_fill_image_with_strategy(fz_context* ctx, pdf_dark_mode_v2_device* d, fz_image* image, fz_matrix ctm,
                                        float alpha, fz_color_params color_params) {
    PdfImageDarkStrategy strategy = GetPdfImageDarkStrategy();
    if (strategy == PdfImageDarkStrategy::Auto || v2_image_is_paint_chip(image) || v2_mask_is_mrc_stencil(image)) {
        return false;
    }
    if (strategy == PdfImageDarkStrategy::Original) {
        fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
        return true;
    }
    if (strategy != PdfImageDarkStrategy::Tone) {
        return false;
    }
    fz_image* cached = nullptr;
    if (d->engineCache) {
        cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                       DarkImagePolicy::ThemeRecolor, DarkImageKind::Photo);
    }
    fz_image* built = nullptr;
    fz_image* draw = cached;
    if (!draw) {
        // Remap at least as sharp as the on-screen tile (plus headroom for modest zoom).
        int aw = (int)(sqrtf(ctm.a * ctm.a + ctm.b * ctm.b) + 0.5f);
        int ah = (int)(sqrtf(ctm.c * ctm.c + ctm.d * ctm.d) + 0.5f);
        int prefer = aw > ah ? aw : ah;
        prefer = (prefer * 5 + 3) / 4;
        built = PdfDarkModeRecolorImage(ctx, image, *d->palette, prefer);
        draw = built ? built : image;
        if (built && d->engineCache) {
            PdfDarkModeEngineCacheStoreProcessed(ctx, d->engineCache, image, d->profileHash,
                                                 DarkImagePolicy::ThemeRecolor, DarkImageKind::Photo, built);
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
    return true;
}

static void v2_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                          fz_color_params color_params) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    if (!image) {
        fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
        return;
    }
    struct FillPerf {
        LARGE_INTEGER t;
        fz_image* image;
        float coverage;
        const char* branch;
        int cache;
        FillPerf(fz_image* img) : t(TimeGet()), image(img), coverage(0), branch("?"), cache(0) {}
        ~FillPerf() {
            if (!PdfDarkModePagePerfOn() || !image) {
                return;
            }
            logf("page-perf fill %dx%d cov=%.2f branch=%s cache=%d %.1f ms\n", image->w, image->h, coverage, branch,
                 cache, TimeSinceInMs(t));
        }
    } fillPerf(image);
    fillPerf.coverage = v2_image_coverage(ctm, d->pageBounds);
    if (v2_fill_image_with_strategy(ctx, d, image, ctm, alpha, color_params)) {
        fillPerf.branch = "strategy";
        return;
    }
    float coverage = v2_image_coverage(ctm, d->pageBounds);
    bool largeOffice = v2_large_office_scan_image(ctx, image, coverage);

    // Word 红头/标题: 2×2 color + glyph SMask. Must not take the "small photo" path.
    if (v2_image_is_paint_chip(image)) {
        fillPerf.branch = "chip";
        fz_image* cached = nullptr;
        if (d->engineCache) {
            cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                           DarkImagePolicy::ThemeRecolor, DarkImageKind::IconOrLineArt);
        }
        fz_image* built = nullptr;
        fz_image* draw = cached;
        fillPerf.cache = cached ? 1 : 0;
        if (!draw) {
            built = v2_build_paint_chip_image(ctx, image, *d->palette);
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

    // Small/medium images: knock out JPEG white mats around colorful badges (UNIT / Atlas).
    // Full-page path below handles scans; do not Okular-wash ordinary photos here.
    if (coverage < kV2FullPageCoverage && !largeOffice) {
        // InDesign/iText textbooks slice one photo into dozens of JPEGs. White-mat
        // walks every pixel of each slice (and often decodes it twice). That is the
        // multi-second "Please wait - rendering..." on a TOC jump. Keep picture
        // colors for those tiles, but still knock out small badge/icon mats —
        // and InDesign RAZ studio cards (The Zoo ape/chimp) that have a white rim.
        if (PdfDarkModeEngineCacheLayoutTextbookFastRemap(d->engineCache) &&
            PdfDarkModeV2LayoutTextbookSkipFigureRemap(image->w, image->h, coverage) &&
            !PdfDarkModeV2QuickStudioWhiteMatCandidate(ctx, image)) {
            fillPerf.branch = "layout";
            fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
            return;
        }
        fillPerf.branch = "mat";
        fz_image* cached = nullptr;
        if (d->engineCache) {
            cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                           DarkImagePolicy::Preserve, DarkImageKind::IconOrLineArt);
        }
        fz_image* built = nullptr;
        fz_image* draw = cached;
        fillPerf.cache = cached ? 1 : 0;
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
        fillPerf.branch = "mask";
        fz_image* cached = nullptr;
        if (d->engineCache) {
            cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                           DarkImagePolicy::ThemeRecolor, DarkImageKind::IconOrLineArt);
        }
        fz_image* built = nullptr;
        fz_image* draw = cached;
        fillPerf.cache = cached ? 1 : 0;
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

    // A layout textbook's nearly full-page photo is the same picture as the
    // slices above. Running it through the full-page picture-book pass walks
    // every pixel (several seconds in a debug build) even though automatic
    // mode keeps the photo. Text and office scans still take that pass.
    //
    // Exception: paper-heavy picture-book pages (RAZ "In and Out" / PDFdo.com
    // packs: white margins + color art). Those must not stay original or the
    // whole page remains a white card on Match-theme.
    if (PdfDarkModeEngineCacheLayoutTextbookFastRemap(d->engineCache)) {
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, true);
        if (!v2_office_scan_may_decode_at_view(analysis) &&
            !PdfDarkModeV2LayoutFullPageNeedsPictureBookRemap(analysis.features)) {
            fillPerf.branch = "layout";
            fz_fill_image(ctx, d->inner, image, ctm, alpha, color_params);
            return;
        }
    }

    // Full-page raster: remap once and cache (RAZ / scans).
    fillPerf.branch = "page";
    fz_image* cached = nullptr;
    if (d->engineCache) {
        cached = PdfDarkModeEngineCacheLookupProcessed(ctx, d->engineCache, image, d->profileHash,
                                                       DarkImagePolicy::ThemeRecolor, DarkImageKind::FullPageScan);
    }
    fz_image* built = nullptr;
    fz_image* draw = cached;
    fillPerf.cache = cached ? 1 : 0;
    if (!draw) {
        fz_rect dest = fz_transform_rect(fz_unit_rect, ctm);
        int destW = (int)(fz_abs(dest.x1 - dest.x0) + 0.5f);
        int destH = (int)(fz_abs(dest.y1 - dest.y0) + 0.5f);
        bool downsampled = false;
        built = v2_build_page_image(ctx, image, *d->palette, destW, destH, &downsampled);
        draw = built ? built : image;
        // View-sized office-scan remaps must not be reused at a higher zoom.
        if (built && d->engineCache && !downsampled) {
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
    v2_map_paint(ctx, d, colorspace, color, alpha, color_params, mapped, false);
    fz_fill_image_mask(ctx, d->inner, image, ctm, fz_device_rgb(ctx), mapped, alpha, color_params);
}

static void v2_clip_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                         fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    v2_push_clip(d, scissor);
    fz_clip_path(ctx, d->inner, path, even_odd, ctm, scissor);
}

static void v2_clip_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path, const fz_stroke_state* stroke,
                                fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    v2_push_clip(d, scissor);
    fz_clip_stroke_path(ctx, d->inner, path, stroke, ctm, scissor);
}

static void v2_clip_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    v2_push_clip(d, scissor);
    fz_clip_text(ctx, d->inner, text, ctm, scissor);
}

static void v2_clip_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text, const fz_stroke_state* stroke,
                                fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    v2_push_clip(d, scissor);
    fz_clip_stroke_text(ctx, d->inner, text, stroke, ctm, scissor);
}

static void v2_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, fz_rect scissor) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    v2_push_clip(d, scissor);
    fz_clip_image_mask(ctx, d->inner, image, ctm, scissor);
}

static void v2_pop_clip(fz_context* ctx, fz_device* dev) {
    pdf_dark_mode_v2_device* d = (pdf_dark_mode_v2_device*)dev;
    v2_pop_clip_rect(d);
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
    if (PdfDarkModePagePerfOn()) {
        gV2Perf.groupCalls++;
    }
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
    d->clipTop = 0;
    d->clipExtra = 0;
    d->clipStack[0] =
        fz_make_rect(pageBounds.x, pageBounds.y, pageBounds.x + pageBounds.dx, pageBounds.y + pageBounds.dy);

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
