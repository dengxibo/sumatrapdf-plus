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
