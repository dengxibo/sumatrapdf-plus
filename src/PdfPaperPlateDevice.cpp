/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Near-white vector fills that sit on a previously painted image hide the
// raster (Excel cell backgrounds over a pasted screenshot, whiteout boxes).
// The fill is paper, not ink: skipping it reveals the image. Black text drawn
// afterwards still paints. A plate that covers most of the image is kept, so
// an intentional caption card is not punched out.

#include "utils/BaseUtil.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "PdfPaperPlateDevice.h"

constexpr int kMaxPaperPlateImages = 64;

struct PdfPaperPlateDevice {
    fz_device super;
    fz_rect images[kMaxPaperPlateImages];
    int nImages;
};

static bool IsNearWhitePaper(float r, float g, float b) {
    float maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    // Pure white and off-white only. Pale yellow highlights and tinted cards stay.
    return lum >= 0.92f && (maxC - minC) < 0.08f;
}

static float RectArea(fz_rect r) {
    float w = r.x1 - r.x0;
    float h = r.y1 - r.y0;
    if (w <= 0.f || h <= 0.f) {
        return 0.f;
    }
    return w * h;
}

// True when the plate is a small patch of one image, not a card over the whole picture.
static bool PlateCoversPartOfImage(fz_rect plate, fz_rect image) {
    float plateArea = RectArea(plate);
    float imageArea = RectArea(image);
    if (plateArea <= 0.f || imageArea <= 0.f) {
        return false;
    }
    fz_rect in = fz_intersect_rect(plate, image);
    float inArea = RectArea(in);
    if (inArea < plateArea * 0.90f) {
        return false;
    }
    float pw = plate.x1 - plate.x0;
    float ph = plate.y1 - plate.y0;
    float iw = image.x1 - image.x0;
    float ih = image.y1 - image.y0;
    if (pw >= iw * 0.85f || ph >= ih * 0.85f) {
        return false;
    }
    if (plateArea >= imageArea * 0.25f) {
        return false;
    }
    return true;
}

static void RememberImage(PdfPaperPlateDevice* d, fz_rect r) {
    if (fz_is_empty_rect(r) || fz_is_infinite_rect(r)) {
        return;
    }
    if (d->nImages < kMaxPaperPlateImages) {
        d->images[d->nImages++] = r;
        return;
    }
    d->images[d->nImages % kMaxPaperPlateImages] = r;
    d->nImages++;
}

static bool PlateSitsOnImage(const PdfPaperPlateDevice* d, fz_rect plate) {
    int n = d->nImages;
    if (n > kMaxPaperPlateImages) {
        n = kMaxPaperPlateImages;
    }
    for (int i = 0; i < n; i++) {
        if (PlateCoversPartOfImage(plate, d->images[i])) {
            return true;
        }
    }
    return false;
}

static void PaperPlateFillImage(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                                fz_color_params colorParams) {
    PdfPaperPlateDevice* d = (PdfPaperPlateDevice*)dev;
    RememberImage(d, fz_transform_rect(fz_unit_rect, ctm));
    fz_fill_image(ctx, dev->passthrough, image, ctm, alpha, colorParams);
}

static void PaperPlateFillPath(fz_context* ctx, fz_device* dev, const fz_path* path, int evenOdd, fz_matrix ctm,
                               fz_colorspace* colorspace, const float* color, float alpha,
                               fz_color_params colorParams) {
    PdfPaperPlateDevice* d = (PdfPaperPlateDevice*)dev;
    bool skip = false;
    if (d->nImages > 0 && alpha >= 0.90f && colorspace && color) {
        float rgb[FZ_MAX_COLORS] = {};
        fz_try(ctx) {
            fz_convert_color(ctx, colorspace, color, fz_device_rgb(ctx), rgb, colorspace, colorParams);
            if (IsNearWhitePaper(rgb[0], rgb[1], rgb[2])) {
                fz_rect plate = fz_bound_path(ctx, path, nullptr, ctm);
                skip = PlateSitsOnImage(d, plate);
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            skip = false;
        }
    }
    if (skip) {
        return;
    }
    fz_fill_path(ctx, dev->passthrough, path, evenOdd, ctm, colorspace, color, alpha, colorParams);
}

fz_device* PdfPaperPlateWrapDevice(fz_context* ctx, fz_device* inner) {
    if (!inner) {
        return inner;
    }
    PdfPaperPlateDevice* d =
        (PdfPaperPlateDevice*)fz_new_passthrough_device_of_size(ctx, inner, (int)sizeof(PdfPaperPlateDevice));
    d->nImages = 0;
    d->super.fill_image = PaperPlateFillImage;
    d->super.fill_path = PaperPlateFillPath;
    return &d->super;
}
