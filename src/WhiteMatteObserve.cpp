/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Debug-only observer. It classifies ebook images at fill_image and logs
// ACCEPT / SKIP. The original image is always forwarded unchanged.

#include "utils/BaseUtil.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/Log.h"
#include "WhiteMatteSuppression.h"

struct WhiteMatteObserveDevice {
    fz_device super;
    fz_rect pageBounds;
};

static fz_image* gWhiteMatteLogged[64];
static int gWhiteMatteLoggedN = 0;

static bool AlreadyLogged(fz_image* image) {
    for (int i = 0; i < gWhiteMatteLoggedN && i < dimof(gWhiteMatteLogged); i++) {
        if (gWhiteMatteLogged[i] == image) {
            return true;
        }
    }
    return false;
}

static void RememberLogged(fz_image* image) {
    if (gWhiteMatteLoggedN < dimof(gWhiteMatteLogged)) {
        gWhiteMatteLogged[gWhiteMatteLoggedN++] = image;
        return;
    }
    gWhiteMatteLogged[gWhiteMatteLoggedN % dimof(gWhiteMatteLogged)] = image;
    gWhiteMatteLoggedN++;
}

static void ObserveFillImage(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                             fz_color_params color_params) {
    WhiteMatteObserveDevice* d = (WhiteMatteObserveDevice*)dev;
    if (image && !AlreadyLogged(image)) {
        RememberLogged(image);
        fz_pixmap* pix = nullptr;
        fz_pixmap* rgb = nullptr;
        fz_var(pix);
        fz_var(rgb);
        fz_try(ctx) {
            pix = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
            if (pix) {
                rgb = fz_convert_pixmap(ctx, pix, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 1);
            }
            fz_rect dest = fz_transform_rect(fz_unit_rect, ctm);
            float pageW = d->pageBounds.x1 - d->pageBounds.x0;
            float pageH = d->pageBounds.y1 - d->pageBounds.y0;
            WhiteMatteGateReport report{};
            WhiteMatteDecision decision = WhiteMatteDecision::TooSmall;
            if (rgb && (rgb->n == 3 || rgb->n == 4)) {
                decision = ClassifyWhiteMatte(rgb->samples, rgb->w, rgb->h, rgb->n, (int)rgb->stride, dest.x1 - dest.x0,
                                              dest.y1 - dest.y0, pageW, pageH, &report);
            }
            logf(
                "WhiteMatte src=%dx%d bbox=%.0fx%.0f page=%.0fx%.0f corners=%d perim=%.2f top=%.2f bot=%.2f "
                "left=%.2f right=%.2f bgL=%.1f a=%.1f b=%.1f C=%.1f mad=%.2f tex=%.2f result=%s\n",
                report.srcW, report.srcH, dest.x1 - dest.x0, dest.y1 - dest.y0, pageW, pageH, report.cornerCount,
                report.perimeter, report.top, report.bottom, report.left, report.right, report.bgL, report.bgA,
                report.bgB, report.bgC, report.borderMad, report.texture, WhiteMatteDecisionName(decision));
        }
        fz_always(ctx) {
            fz_drop_pixmap(ctx, rgb);
            fz_drop_pixmap(ctx, pix);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            logf("WhiteMatte result=SKIP_DecodeFailed\n");
        }
    }
    fz_fill_image(ctx, dev->passthrough, image, ctm, alpha, color_params);
}

fz_device* WhiteMatteObserveWrap(fz_context* ctx, fz_device* inner, fz_rect pageBounds) {
    WhiteMatteObserveDevice* d =
        (WhiteMatteObserveDevice*)fz_new_passthrough_device_of_size(ctx, inner, (int)sizeof(WhiteMatteObserveDevice));
    d->pageBounds = pageBounds;
    d->super.fill_image = ObserveFillImage;
    return &d->super;
}
