/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include <math.h>

#include "WhiteMatteSuppression.h"

const char* WhiteMatteDecisionName(WhiteMatteDecision decision) {
    switch (decision) {
        case WhiteMatteDecision::Accept:
            return "ACCEPT";
        case WhiteMatteDecision::TooSmall:
            return "SKIP_TooSmall";
        case WhiteMatteDecision::HasAlpha:
            return "SKIP_HasAlpha";
        case WhiteMatteDecision::FullPageRaster:
            return "SKIP_FullPageRaster";
        case WhiteMatteDecision::CornersNotWhite:
            return "SKIP_CornersNotWhite";
        case WhiteMatteDecision::PerimeterInsufficient:
            return "SKIP_PerimeterInsufficient";
        case WhiteMatteDecision::BorderTooTextured:
            return "SKIP_BorderTooTextured";
        case WhiteMatteDecision::BackgroundNotNeutral:
            return "SKIP_BackgroundNotNeutral";
    }
    return "SKIP_Unknown";
}

struct Lab {
    float L = 0;
    float a = 0;
    float b = 0;
};

static float SrgbToLinear(int c) {
    float s = (float)c / 255.f;
    if (s <= 0.04045f) {
        return s / 12.92f;
    }
    return powf((s + 0.055f) / 1.055f, 2.4f);
}

static float LabF(float t) {
    const float d = 6.f / 29.f;
    const float d3 = d * d * d;
    if (t > d3) {
        return cbrtf(t);
    }
    return t / (3.f * d * d) + 4.f / 29.f;
}

static Lab RgbToLab(int r, int g, int b) {
    float R = SrgbToLinear(r);
    float G = SrgbToLinear(g);
    float B = SrgbToLinear(b);
    float X = 0.4124564f * R + 0.3575761f * G + 0.1804375f * B;
    float Y = 0.2126729f * R + 0.7151522f * G + 0.0721750f * B;
    float Z = 0.0193339f * R + 0.1191920f * G + 0.9503041f * B;
    // D65
    float fx = LabF(X / 0.95047f);
    float fy = LabF(Y / 1.f);
    float fz = LabF(Z / 1.08883f);
    Lab lab;
    lab.L = 116.f * fy - 16.f;
    lab.a = 500.f * (fx - fy);
    lab.b = 200.f * (fy - fz);
    return lab;
}

static float LabC(Lab lab) {
    return sqrtf(lab.a * lab.a + lab.b * lab.b);
}

static bool LabIsMatteWhite(Lab lab) {
    return lab.L >= 93.f && LabC(lab) <= 8.f;
}

static void SortFloat(float* a, int n) {
    for (int gap = n / 2; gap > 0; gap /= 2) {
        for (int i = gap; i < n; i++) {
            float v = a[i];
            int j = i;
            while (j >= gap && a[j - gap] > v) {
                a[j] = a[j - gap];
                j -= gap;
            }
            a[j] = v;
        }
    }
}

static float MedianFloat(float* a, int n) {
    if (n <= 0) {
        return 0;
    }
    SortFloat(a, n);
    return a[n / 2];
}

static float MedianAbsDev(float* a, int n, float med) {
    if (n <= 0) {
        return 0;
    }
    float* dev = AllocArray<float>((size_t)n);
    if (!dev) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        float d = a[i] - med;
        dev[i] = d < 0 ? -d : d;
    }
    float mad = MedianFloat(dev, n);
    free(dev);
    return mad;
}

static bool MeaningfulAlpha(const u8* samples, int w, int h, int comps, int stride) {
    if (comps != 4) {
        return false;
    }
    i64 soft = 0;
    i64 n = 0;
    int step = 1;
    int m = w > h ? w : h;
    if (m > 256) {
        step = (m + 255) / 256;
    }
    for (int y = 0; y < h; y += step) {
        const u8* row = samples + (i64)y * stride;
        for (int x = 0; x < w; x += step) {
            n++;
            if (row[x * comps + 3] < 250) {
                soft++;
            }
        }
    }
    return n > 0 && soft * 100 >= n * 8;
}

static bool CoversFullPage(float imageW, float imageH, float pageW, float pageH) {
    if (pageW <= 1.f || pageH <= 1.f || imageW <= 0.f || imageH <= 0.f) {
        return false;
    }
    if (imageW / pageW > 0.90f && imageH / pageH > 0.90f) {
        return true;
    }
    return imageW * imageH > pageW * pageH * 0.82f;
}

struct LowRes {
    u8* rgb = nullptr;
    int w = 0;
    int h = 0;
};

static LowRes Downsample(const u8* samples, int w, int h, int comps, int stride) {
    LowRes out{};
    int srcMax = w > h ? w : h;
    int scale = (srcMax + 255) / 256;
    if (scale < 1) {
        scale = 1;
    }
    int dw = (w + scale - 1) / scale;
    int dh = (h + scale - 1) / scale;
    if (dw < 2 || dh < 2) {
        return out;
    }
    u8* rgb = AllocArray<u8>((size_t)dw * (size_t)dh * 3);
    if (!rgb) {
        return out;
    }
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int x0 = x * scale;
            int y0 = y * scale;
            int x1 = x0 + scale;
            int y1 = y0 + scale;
            if (x1 > w) {
                x1 = w;
            }
            if (y1 > h) {
                y1 = h;
            }
            i64 sr = 0;
            i64 sg = 0;
            i64 sb = 0;
            int n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const u8* row = samples + (i64)yy * stride;
                for (int xx = x0; xx < x1; xx++) {
                    const u8* p = row + xx * comps;
                    sr += p[0];
                    sg += p[1];
                    sb += p[2];
                    n++;
                }
            }
            u8* d = rgb + ((size_t)y * (size_t)dw + (size_t)x) * 3;
            if (n < 1) {
                n = 1;
            }
            d[0] = (u8)(sr / n);
            d[1] = (u8)(sg / n);
            d[2] = (u8)(sb / n);
        }
    }
    out.rgb = rgb;
    out.w = dw;
    out.h = dh;
    return out;
}

static Lab At(const LowRes& im, int x, int y) {
    const u8* p = im.rgb + ((size_t)y * (size_t)im.w + (size_t)x) * 3;
    return RgbToLab(p[0], p[1], p[2]);
}

static bool CornerIsWhite(const LowRes& im, int x0, int y0, int patch) {
    int n = patch * patch;
    float* Ls = AllocArray<float>((size_t)n);
    float* Cs = AllocArray<float>((size_t)n);
    if (!Ls || !Cs) {
        free(Ls);
        free(Cs);
        return false;
    }
    int k = 0;
    for (int y = y0; y < y0 + patch; y++) {
        for (int x = x0; x < x0 + patch; x++) {
            Lab lab = At(im, x, y);
            Ls[k] = lab.L;
            Cs[k] = LabC(lab);
            k++;
        }
    }
    Lab med;
    med.L = MedianFloat(Ls, n);
    med.a = 0;
    med.b = MedianFloat(Cs, n);
    bool ok = med.L >= 93.f && med.b <= 8.f;
    free(Ls);
    free(Cs);
    return ok;
}

WhiteMatteDecision ClassifyWhiteMatte(const u8* samples, int w, int h, int comps, int stride, float imageOnPageW,
                                      float imageOnPageH, float pageW, float pageH, WhiteMatteGateReport* report) {
    WhiteMatteGateReport local{};
    WhiteMatteGateReport* r = report ? report : &local;
    *r = WhiteMatteGateReport{};
    r->srcW = w;
    r->srcH = h;
    if (!samples || w < 2 || h < 2 || (comps != 3 && comps != 4) || stride < w * comps) {
        return WhiteMatteDecision::TooSmall;
    }
    int minorSide = w < h ? w : h;
    if (minorSide < 32) {
        return WhiteMatteDecision::TooSmall;
    }
    if (MeaningfulAlpha(samples, w, h, comps, stride)) {
        return WhiteMatteDecision::HasAlpha;
    }
    if (CoversFullPage(imageOnPageW, imageOnPageH, pageW, pageH)) {
        return WhiteMatteDecision::FullPageRaster;
    }

    LowRes im = Downsample(samples, w, h, comps, stride);
    if (!im.rgb) {
        return WhiteMatteDecision::TooSmall;
    }
    int patch = (int)((float)(im.w < im.h ? im.w : im.h) * 0.015f);
    if (patch < 3) {
        patch = 3;
    }
    if (patch * 2 >= im.w || patch * 2 >= im.h) {
        free(im.rgb);
        return WhiteMatteDecision::TooSmall;
    }
    int corners = 0;
    if (CornerIsWhite(im, 0, 0, patch)) {
        corners++;
    }
    if (CornerIsWhite(im, im.w - patch, 0, patch)) {
        corners++;
    }
    if (CornerIsWhite(im, 0, im.h - patch, patch)) {
        corners++;
    }
    if (CornerIsWhite(im, im.w - patch, im.h - patch, patch)) {
        corners++;
    }
    r->cornerCount = corners;

    auto sideFrac = [&](bool horizontal, int fixed, int length) {
        int hit = 0;
        for (int i = 0; i < length; i++) {
            Lab lab = horizontal ? At(im, i, fixed) : At(im, fixed, i);
            if (LabIsMatteWhite(lab)) {
                hit++;
            }
        }
        return length > 0 ? (float)hit / (float)length : 0.f;
    };
    r->top = sideFrac(true, 0, im.w);
    r->bottom = sideFrac(true, im.h - 1, im.w);
    r->left = sideFrac(false, 0, im.h);
    r->right = sideFrac(false, im.w - 1, im.h);
    int perimN = im.w * 2 + im.h * 2;
    int perimHit = 0;
    // Recount without double-counting corners by using the four fractions.
    perimHit = (int)(r->top * im.w + 0.5f) + (int)(r->bottom * im.w + 0.5f) + (int)(r->left * im.h + 0.5f) +
               (int)(r->right * im.h + 0.5f);
    r->perimeter = perimN > 0 ? (float)perimHit / (float)perimN : 0.f;

    int band = im.w < im.h ? im.w : im.h;
    band = band / 40;
    if (band < 2) {
        band = 2;
    }
    if (band > 6) {
        band = 6;
    }
    int cap = (im.w + im.h) * 2 * band;
    if (cap < 16) {
        cap = 16;
    }
    float* borderL = AllocArray<float>((size_t)cap);
    float* borderA = AllocArray<float>((size_t)cap);
    float* borderB = AllocArray<float>((size_t)cap);
    int bn = 0;
    float gradSum = 0;
    int gradN = 0;
    if (borderL && borderA && borderB) {
        for (int y = 0; y < im.h; y++) {
            bool yBand = y < band || y >= im.h - band;
            for (int x = 0; x < im.w; x++) {
                bool xBand = x < band || x >= im.w - band;
                if (!yBand && !xBand) {
                    continue;
                }
                Lab lab = At(im, x, y);
                if (bn < cap) {
                    borderL[bn] = lab.L;
                    borderA[bn] = lab.a;
                    borderB[bn] = lab.b;
                    bn++;
                }
                if (x + 1 < im.w && (yBand || x + 1 >= im.w - band || x < band)) {
                    Lab nxt = At(im, x + 1, y);
                    float d = lab.L - nxt.L;
                    gradSum += d < 0 ? -d : d;
                    gradN++;
                }
            }
        }
    }
    float mad = 0;
    if (bn > 0) {
        float* lcopy = AllocArray<float>((size_t)bn);
        float* acopy = AllocArray<float>((size_t)bn);
        float* bcopy = AllocArray<float>((size_t)bn);
        if (lcopy && acopy && bcopy) {
            memcpy(lcopy, borderL, sizeof(float) * (size_t)bn);
            memcpy(acopy, borderA, sizeof(float) * (size_t)bn);
            memcpy(bcopy, borderB, sizeof(float) * (size_t)bn);
            r->bgL = MedianFloat(lcopy, bn);
            r->bgA = MedianFloat(acopy, bn);
            r->bgB = MedianFloat(bcopy, bn);
            r->bgC = sqrtf(r->bgA * r->bgA + r->bgB * r->bgB);
            mad = MedianAbsDev(borderL, bn, r->bgL);
        }
        free(lcopy);
        free(acopy);
        free(bcopy);
    }
    r->borderMad = mad;
    r->texture = gradN > 0 ? gradSum / (float)gradN : 0;
    free(borderL);
    free(borderA);
    free(borderB);
    free(im.rgb);

    if (bn < 8 || r->bgL < 93.f || r->bgC > 8.f) {
        return WhiteMatteDecision::BackgroundNotNeutral;
    }
    if (corners < 4) {
        return WhiteMatteDecision::CornersNotWhite;
    }
    int sides = 0;
    if (r->top >= 0.50f) {
        sides++;
    }
    if (r->bottom >= 0.50f) {
        sides++;
    }
    if (r->left >= 0.50f) {
        sides++;
    }
    if (r->right >= 0.50f) {
        sides++;
    }
    if (r->perimeter < 0.55f || sides < 3) {
        return WhiteMatteDecision::PerimeterInsufficient;
    }
    if (mad > 3.5f || r->texture > 4.5f) {
        return WhiteMatteDecision::BorderTooTextured;
    }
    return WhiteMatteDecision::Accept;
}
