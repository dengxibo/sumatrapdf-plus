/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"

#include "PdfDarkModeInternal.h"

#include <math.h>

static constexpr int kMaxScanPixels = 4096 * 4096;

static float Clamp01(float v) {
    if (v < 0.f) {
        return 0.f;
    }
    if (v > 1.f) {
        return 1.f;
    }
    return v;
}

static float SmoothStep(float e0, float e1, float x) {
    if (e0 == e1) {
        return x >= e1 ? 1.f : 0.f;
    }
    float t = (x - e0) / (e1 - e0);
    if (t <= 0.f) {
        return 0.f;
    }
    if (t >= 1.f) {
        return 1.f;
    }
    return t * t * (3.f - 2.f * t);
}

static void ReadPixmapRgb(fz_context* ctx, fz_pixmap* pix, int x, int y, float* outR, float* outG, float* outB) {
    *outR = *outG = *outB = 0.f;
    if (!pix || !pix->samples || x < 0 || y < 0 || x >= pix->w || y >= pix->h) {
        return;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int n = pix->n;
    int components = fz_colorspace_n(ctx, cs);
    unsigned char* px = pix->samples + y * pix->stride + x * n;
    if (cs == rgb || fz_colorspace_is_rgb(ctx, cs)) {
        *outR = px[0] / 255.f;
        *outG = px[1] / 255.f;
        *outB = px[2] / 255.f;
        return;
    }
    if (components == 1 || fz_colorspace_is_gray(ctx, cs)) {
        *outR = *outG = *outB = px[0] / 255.f;
        return;
    }
    float conv[FZ_MAX_COLORS] = {};
    float srcRgb[FZ_MAX_COLORS] = {};
    for (int c = 0; c < components && c < FZ_MAX_COLORS; c++) {
        conv[c] = px[c] / 255.f;
    }
    fz_convert_color(ctx, cs, conv, rgb, srcRgb, cs, fz_default_color_params);
    *outR = srcRgb[0];
    *outG = srcRgb[1];
    *outB = srcRgb[2];
}

static PixelColor EstimatePaperFromPixmap(fz_context* ctx, fz_pixmap* pix, const DarkImageAnalysis& analysis) {
    PixelColor paper = analysis.estimatedBackground;
    if (!pix || pix->w <= 0 || pix->h <= 0) {
        return paper;
    }

    float rs[512] = {};
    float gs[512] = {};
    float bs[512] = {};
    int n = 0;
    int w = pix->w;
    int h = pix->h;
    int stepX = w >= 64 ? w / 64 : 1;
    int stepY = h >= 64 ? h / 64 : 1;

    auto sample_edge = [&](int x, int y) {
        if (n >= 512) {
            return;
        }
        float r, g, b;
        ReadPixmapRgb(ctx, pix, x, y, &r, &g, &b);
        rs[n] = r;
        gs[n] = g;
        bs[n] = b;
        n++;
    };

    for (int x = 0; x < w; x += stepX) {
        sample_edge(x, 0);
        sample_edge(x, h - 1);
    }
    for (int y = stepY; y < h - 1; y += stepY) {
        sample_edge(0, y);
        sample_edge(w - 1, y);
    }
    if (n <= 0) {
        return paper;
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n - 1; i++) {
            for (int j = i + 1; j < n; j++) {
                float li = rs[i] + gs[i] + bs[i];
                float lj = rs[j] + gs[j] + bs[j];
                if (li > lj) {
                    float tr = rs[i];
                    rs[i] = rs[j];
                    rs[j] = tr;
                    tr = gs[i];
                    gs[i] = gs[j];
                    gs[j] = tr;
                    tr = bs[i];
                    bs[i] = bs[j];
                    bs[j] = tr;
                }
            }
        }
    }
    int mid = n / 2;
    paper.r = rs[mid];
    paper.g = gs[mid];
    paper.b = bs[mid];
    return paper;
}

void PdfDarkModeRemapScanPixel(float r, float g, float b, const DarkImageAnalysis& analysis,
                               const DarkModePalette& palette, float* outR, float* outG, float* outB) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float chroma = maxC - minC;

    const float lowChroma = 0.10f;
    if (chroma < lowChroma) {
        ApplyAdaptiveDocumentDarkMode(r, g, b, palette, outR, outG, outB);
        return;
    }

    float docR = palette.bgR;
    float docG = palette.bgG;
    float docB = palette.bgB;
    ApplyAdaptiveDocumentDarkMode(r, g, b, palette, &docR, &docG, &docB);

    float photoR = r;
    float photoG = g;
    float photoB = b;
    PdfDarkModeCompressPhotoHighlights(r, g, b, &photoR, &photoG, &photoB);
    float photoLum = 0.2126f * photoR + 0.7152f * photoG + 0.0722f * photoB;
    float darken = SmoothStep(0.55f, 0.88f, photoLum) * 0.12f;
    photoR = photoR * (1.f - darken) + palette.bgR * darken;
    photoG = photoG * (1.f - darken) + palette.bgG * darken;
    photoB = photoB * (1.f - darken) + palette.bgB * darken;

    float photoW = SmoothStep(0.09f, 0.24f, chroma);
    if (analysis.features.saturatedPixelRatio >= 0.14f) {
        photoW = Clamp01(photoW + 0.18f);
    }
    if (analysis.features.textureScore >= 0.20f) {
        photoW = Clamp01(photoW + 0.08f);
    }

    *outR = docR * (1.f - photoW) + photoR * photoW;
    *outG = docG * (1.f - photoW) + photoG * photoW;
    *outB = docB * (1.f - photoW) + photoB * photoW;
}

static void WritePixmapRgb(fz_context* ctx, fz_pixmap* pix, int x, int y, float r, float g, float b) {
    if (!pix || !pix->samples || x < 0 || y < 0 || x >= pix->w || y >= pix->h) {
        return;
    }
    fz_colorspace* cs = pix->colorspace ? pix->colorspace : fz_device_rgb(ctx);
    fz_colorspace* rgb = fz_device_rgb(ctx);
    int n = pix->n;
    int components = fz_colorspace_n(ctx, cs);
    unsigned char* px = pix->samples + y * pix->stride + x * n;
    if (cs == rgb || fz_colorspace_is_rgb(ctx, cs)) {
        int vr = (int)(r * 255.f + 0.5f);
        int vg = (int)(g * 255.f + 0.5f);
        int vb = (int)(b * 255.f + 0.5f);
        px[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
        px[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
        px[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
        return;
    }
    if (components == 1 || fz_colorspace_is_gray(ctx, cs)) {
        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        int v = (int)(lum * 255.f + 0.5f);
        px[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
        return;
    }
    float out[FZ_MAX_COLORS] = {r, g, b};
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

fz_pixmap* PdfDarkModeProcessScanPixmap(fz_context* ctx, fz_pixmap* src, const DarkImageAnalysis& analysis,
                                        const DarkModePalette& palette) {
    if (!ctx || !src || !src->samples || src->w <= 0 || src->h <= 0) {
        return nullptr;
    }
    if (analysis.kind != DarkImageKind::FullPageScan || analysis.confidence < 0.65f) {
        return nullptr;
    }
    if ((i64)src->w * src->h > kMaxScanPixels) {
        return nullptr;
    }

    DarkImageAnalysis work = analysis;
    work.estimatedBackground = EstimatePaperFromPixmap(ctx, src, analysis);

    fz_colorspace* rgb = fz_device_rgb(ctx);
    fz_colorspace* srcCs = src->colorspace ? src->colorspace : rgb;
    int srcComponents = fz_colorspace_n(ctx, srcCs);
    bool srcFastRgb = srcCs == rgb || fz_colorspace_is_rgb(ctx, srcCs);
    bool srcFastGray = srcComponents == 1 || fz_colorspace_is_gray(ctx, srcCs);

    // A Gray pixmap cannot represent tinted dark-theme colors. Writing Dracula
    // #282A36 back into Gray produces its luminance #2A2A2A, leaving a visible
    // rectangle against the reading area. Promote grayscale scans to RGB, just
    // like the government-paper processor does.
    bool promoteThemeRgb = srcFastGray && !srcFastRgb;
    fz_colorspace* dstCs = promoteThemeRgb ? rgb : srcCs;
    fz_pixmap* dst = fz_new_pixmap(ctx, dstCs, src->w, src->h, src->seps, src->alpha);
    if (!promoteThemeRgb) {
        fz_copy_pixmap_rect(ctx, dst, src, fz_make_irect(0, 0, src->w, src->h), nullptr);
    }

    int srcN = src->n;
    int srcStride = src->stride;
    int dstN = dst->n;
    int dstStride = dst->stride;
    bool dstFastRgb = dstCs == rgb || fz_colorspace_is_rgb(ctx, dstCs);
    bool dstFastGray = fz_colorspace_n(ctx, dstCs) == 1 || fz_colorspace_is_gray(ctx, dstCs);

    for (int y = 0; y < dst->h; y++) {
        unsigned char* srcRow = src->samples + y * srcStride;
        unsigned char* dstRow = dst->samples + y * dstStride;
        for (int x = 0; x < dst->w; x++) {
            unsigned char* srcPx = srcRow + x * srcN;
            unsigned char* dstPx = dstRow + x * dstN;
            float r, g, b;
            if (srcFastRgb) {
                r = srcPx[0] / 255.f;
                g = srcPx[1] / 255.f;
                b = srcPx[2] / 255.f;
            } else if (srcFastGray) {
                r = g = b = srcPx[0] / 255.f;
            } else {
                ReadPixmapRgb(ctx, src, x, y, &r, &g, &b);
            }
            float nr, ng, nb;
            PdfDarkModeRemapScanPixel(r, g, b, work, palette, &nr, &ng, &nb);
            if (dstFastRgb) {
                int vr = (int)(nr * 255.f + 0.5f);
                int vg = (int)(ng * 255.f + 0.5f);
                int vb = (int)(nb * 255.f + 0.5f);
                dstPx[0] = (unsigned char)(vr < 0 ? 0 : (vr > 255 ? 255 : vr));
                dstPx[1] = (unsigned char)(vg < 0 ? 0 : (vg > 255 ? 255 : vg));
                dstPx[2] = (unsigned char)(vb < 0 ? 0 : (vb > 255 ? 255 : vb));
            } else if (dstFastGray) {
                float lum = 0.2126f * nr + 0.7152f * ng + 0.0722f * nb;
                int v = (int)(lum * 255.f + 0.5f);
                dstPx[0] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            } else {
                WritePixmapRgb(ctx, dst, x, y, nr, ng, nb);
            }
            if (promoteThemeRgb && src->alpha && dst->alpha) {
                dstPx[dstN - 1] = srcPx[srcN - 1];
            }
        }
    }
    return dst;
}
