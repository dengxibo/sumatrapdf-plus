/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once
#include <math.h>

// Shared by saved PDF geometry and display-only preview. Keep the established
// angle thresholds and inscribed-rectangle algorithm unchanged.
inline bool PdfAdjustmentInscribedSize(float w, float h, float deg, float* wr, float* hr) {
    if (!wr || !hr || w < 1.f || h < 1.f) {
        return false;
    }
    float a = fabsf(fmodf(deg, 180.f));
    if (a > 90.f) {
        a = 180.f - a;
    }
    if (a < 0.05f || a > 89.95f) {
        return false;
    }
    float rad = a * 0.0174532925f;
    float sinA = fabsf(sinf(rad));
    float cosA = fabsf(cosf(rad));
    if (sinA < 1e-4f || cosA < 1e-4f) {
        return false;
    }
    bool widthIsLonger = w >= h;
    float sideLong = widthIsLonger ? w : h;
    float sideShort = widthIsLonger ? h : w;
    float outW = 0;
    float outH = 0;
    if (sideShort <= 2.f * sinA * cosA * sideLong || fabsf(sinA - cosA) < 1e-4f) {
        float x = 0.5f * sideShort;
        if (widthIsLonger) {
            outW = x / sinA;
            outH = x / cosA;
        } else {
            outW = x / cosA;
            outH = x / sinA;
        }
    } else {
        float cos2 = cosA * cosA - sinA * sinA;
        if (fabsf(cos2) < 1e-4f) {
            return false;
        }
        outW = (w * cosA - h * sinA) / cos2;
        outH = (h * cosA - w * sinA) / cos2;
    }
    if (outW < 1.f || outH < 1.f) {
        return false;
    }
    *wr = outW;
    *hr = outH;
    return true;
}

// Crop coordinates are fractions of the final axis-aligned page, after rotation
// and flips. Use the same mapping for preview pixels and saved page points.
struct PdfAdjustmentCropBounds {
    float x, y, w, h;
};
inline bool PdfAdjustmentManualCrop(float x, float y, float w, float h, float left, float top, float width,
                                    float height, PdfAdjustmentCropBounds* result) {
    if (!result || !isfinite(left) || !isfinite(top) || !isfinite(width) || !isfinite(height) || left < 0 || top < 0 ||
        width < .01f || height < .01f || left + width > 1.00001f || top + height > 1.00001f || w <= 0 || h <= 0)
        return false;
    *result = {x + left * w, y + top * h, width * w, height * h};
    return true;
}

// Final page dimensions shared by the preview, crop preflight and PDF writer.
inline void PdfAdjustmentOutputSize(float w, float h, float angle, float deskew, bool flipH, bool flipV, bool autoCrop,
                                    float* ow, float* oh) {
    float a = (angle + ((flipH != flipV) ? -deskew : deskew)) * 0.0174532925f;
    *ow = fabsf(w * cosf(a)) + fabsf(h * sinf(a));
    *oh = fabsf(w * sinf(a)) + fabsf(h * cosf(a));
    if (autoCrop) PdfAdjustmentInscribedSize(w, h, angle + deskew, ow, oh);
}

inline bool PdfAdjustmentCropFits(const PdfAdjustmentCropBounds& c, float w, float h) {
    return isfinite(c.x) && isfinite(c.y) && isfinite(c.w) && isfinite(c.h) && c.x >= 0 && c.y >= 0 && c.w >= 1 &&
           c.h >= 1 && c.x + c.w <= w + .001f && c.y + c.h <= h + .001f;
}

// Resize about an anchor, preserving ratio and clipping the size (not the ratio).
inline PdfAdjustmentCropBounds PdfAdjustmentRatioRect(float ax, float ay, float bx, float by, float ratio, float pageW,
                                                      float pageH) {
    float sx = bx < ax ? -1.f : 1.f, sy = by < ay ? -1.f : 1.f;
    float w = fabsf(bx - ax), h = fabsf(by - ay);
    if (ratio > 0) {
        if (w > h * ratio)
            h = w / ratio;
        else
            w = h * ratio;
    }
    float maxW = sx > 0 ? pageW - ax : ax, maxH = sy > 0 ? pageH - ay : ay;
    float scale = 1.f;
    if (w > maxW && w > 0) scale = maxW / w;
    if (h * scale > maxH && h > 0) scale = maxH / h;
    w *= scale;
    h *= scale;
    return {sx > 0 ? ax : ax - w, sy > 0 ? ay : ay - h, w, h};
}

inline PdfAdjustmentCropBounds PdfAdjustmentDragCrop(PdfAdjustmentCropBounds c, int hit, float dx, float dy, float ax,
                                                     float ay, float bx, float by, float ratio, float pw, float ph) {
    if (hit == 32) return PdfAdjustmentRatioRect(ax, ay, bx, by, ratio, pw, ph);
    if (hit == 16) {
        c.x = fmaxf(0, fminf(c.x + dx, fmaxf(0, pw - c.w)));
        c.y = fmaxf(0, fminf(c.y + dy, fmaxf(0, ph - c.h)));
        return c;
    }
    float l = c.x, t = c.y, r = c.x + c.w, b = c.y + c.h;
    if (hit & 1) l = fmaxf(0, fminf(l + dx, r - 1));
    if (hit & 2) r = fminf(pw, fmaxf(r + dx, l + 1));
    if (hit & 4) t = fmaxf(0, fminf(t + dy, b - 1));
    if (hit & 8) b = fminf(ph, fmaxf(b + dy, t + 1));
    if (ratio <= 0) return {l, t, r - l, b - t};
    if ((hit & 3) && (hit & 12)) {
        float anchorX = hit & 1 ? c.x + c.w : c.x;
        float anchorY = hit & 4 ? c.y + c.h : c.y;
        return PdfAdjustmentRatioRect(anchorX, anchorY, hit & 1 ? l : r, hit & 4 ? t : b, ratio, pw, ph);
    }
    float w, h, cx = c.x + c.w / 2, cy = c.y + c.h / 2;
    if (hit & 3) {
        w = r - l;
        h = w / ratio;
        float maxH = 2 * fminf(cy, ph - cy);
        if (h > maxH) {
            h = maxH;
            w = h * ratio;
        }
        l = hit & 1 ? c.x + c.w - w : c.x;
        t = cy - h / 2;
    } else {
        h = b - t;
        w = h * ratio;
        float maxW = 2 * fminf(cx, pw - cx);
        if (w > maxW) {
            w = maxW;
            h = w / ratio;
        }
        t = hit & 4 ? c.y + c.h - h : c.y;
        l = cx - w / 2;
    }
    return {l, t, w, h};
}

// Per-page copies are independent from future edits to the shared template.
struct PdfRotateCropState {
    int pageNo = 0;
    PdfAdjustmentCropBounds box{};
    int ratioMode = 0;
    float ratio = 0;
    bool initialized = false;
};
inline PdfRotateCropState* PdfAdjustmentCropForPage(PdfRotateCropState* shared, PdfRotateCropState* overrides,
                                                    int count, int pageNo) {
    for (int i = 0; i < count; i++)
        if (overrides[i].pageNo == pageNo) return &overrides[i];
    return shared;
}

inline PdfAdjustmentCropBounds PdfAdjustmentFitRatio(PdfAdjustmentCropBounds c, float ratio, float pw, float ph) {
    if (ratio <= 0 || pw < 1 || ph < 1) return c;
    float cx = c.x + c.w * .5f, cy = c.y + c.h * .5f;
    float w = fmaxf(c.w, c.h * ratio), h = w / ratio;
    float scale = fminf(1.f, fminf(pw / w, ph / h));
    w *= scale;
    h *= scale;
    cx = fmaxf(w * .5f, fminf(cx, pw - w * .5f));
    cy = fmaxf(h * .5f, fminf(cy, ph - h * .5f));
    return {cx - w * .5f, cy - h * .5f, w, h};
}
