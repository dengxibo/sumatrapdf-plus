/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Timer.h"
#include "utils/Log.h"

#include "PdfDarkMode.h"
#include "FaceLandmarkEngine.h"

#include <math.h>
#include <stdlib.h>

struct OklabColor {
    float L = 0.f;
    float a = 0.f;
    float b = 0.f;
};

static float SrgbToLinear(float c) {
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static float LinearToSrgb(float c) {
    if (c <= 0.0031308f) {
        return 12.92f * c;
    }
    return 1.055f * powf(c, 1.f / 2.4f) - 0.055f;
}

static float Clamp01(float v) {
    if (v < 0.f) {
        return 0.f;
    }
    if (v > 1.f) {
        return 1.f;
    }
    return v;
}

static OklabColor SrgbToOklab(float r, float g, float b) {
    float lr = SrgbToLinear(r);
    float lg = SrgbToLinear(g);
    float lb = SrgbToLinear(b);

    float l = 0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb;
    float m = 0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb;
    float s = 0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb;

    l = cbrtf(l);
    m = cbrtf(m);
    s = cbrtf(s);

    OklabColor lab;
    lab.L = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab.a = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab.b = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
    return lab;
}

static void OklabToLinear(const OklabColor& lab, float* lr, float* lg, float* lb) {
    float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
    float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
    float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

    float l = l_ * l_ * l_;
    float m = m_ * m_ * m_;
    float s = s_ * s_ * s_;

    *lr = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    *lg = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    *lb = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

static void OklabToSrgb(const OklabColor& lab, float* outR, float* outG, float* outB) {
    float lr, lg, lb;
    OklabToLinear(lab, &lr, &lg, &lb);
    *outR = Clamp01(LinearToSrgb(lr));
    *outG = Clamp01(LinearToSrgb(lg));
    *outB = Clamp01(LinearToSrgb(lb));
}

static float OklabChroma(const OklabColor& lab) {
    return sqrtf(lab.a * lab.a + lab.b * lab.b);
}

void MapRgbToDarkThemeOklab(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    OklabColor src = SrgbToOklab(r, g, b);
    OklabColor text = SrgbToOklab(palette.textR, palette.textG, palette.textB);
    OklabColor bg = SrgbToOklab(palette.bgR, palette.bgG, palette.bgB);

    // Monotone lightness remap in OKLab; preserve hue via a/b direction.
    float outL = text.L + src.L * (bg.L - text.L);

    const float minL = 0.08f;
    const float maxL = 0.92f;
    if (outL < minL) {
        outL = minL;
    }
    if (outL > maxL) {
        outL = maxL;
    }

    float chroma = OklabChroma(src);
    const float maxChroma = 0.38f;
    if (chroma > maxChroma) {
        chroma = maxChroma;
    }

    float outA = 0.f;
    float outB = 0.f;
    float srcChroma = OklabChroma(src);
    if (srcChroma > 1e-5f) {
        float scale = chroma / srcChroma;
        outA = src.a * scale;
        outB = src.b * scale;
    }

    // Pull extreme highlights onto the theme page color (Dracula #282A36), not a
    // neutral gray at bg.L — that reads as pure black next to a chromatic canvas.
    // JPEG-softened figure paper (~170–245) must snap too, not only pure white.
    if (src.L > 0.70f && chroma < 0.08f) {
        outRgb[0] = palette.bgR;
        outRgb[1] = palette.bgG;
        outRgb[2] = palette.bgB;
        return;
    }

    OklabColor out{outL, outA, outB};
    OklabToSrgb(out, &outRgb[0], &outRgb[1], &outRgb[2]);
}

void MapRgbLightMarkerToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    OklabColor src = SrgbToOklab(r, g, b);
    OklabColor text = SrgbToOklab(palette.textR, palette.textG, palette.textB);
    OklabColor bg = SrgbToOklab(palette.bgR, palette.bgG, palette.bgB);
    // Far enough below the theme text that light glyphs stay readable, and far
    // enough above the background that the marker is still a colored band.
    float outL = bg.L + 0.42f * (text.L - bg.L);
    if (outL < 0.16f) {
        outL = 0.16f;
    }
    if (outL > 0.55f) {
        outL = 0.55f;
    }
    float chroma = OklabChroma(src);
    if (chroma > 0.16f) {
        chroma = 0.16f;
    }
    float srcChroma = OklabChroma(src);
    float outA = 0.f;
    float outB = 0.f;
    if (srcChroma > 1e-5f) {
        float scale = chroma / srcChroma;
        outA = src.a * scale;
        outB = src.b * scale;
    }
    OklabColor out{outL, outA, outB};
    OklabToSrgb(out, &outRgb[0], &outRgb[1], &outRgb[2]);
}

// Only the bright end moves. Below the knee, lightness is unchanged, so shadows are not lifted or flipped.
// White (L = 1) lands at 0.58. Slope on the shoulder stays positive.
static float PhotoDarkToneCurve(float L) {
    if (L < 0.f) {
        L = 0.f;
    }
    if (L > 1.f) {
        L = 1.f;
    }
    const float knee = 0.45f;
    const float whiteOut = 0.58f;
    if (L <= knee) {
        return L;
    }
    float u = (L - knee) / (1.f - knee);
    return knee + (whiteOut - knee) * u;
}

static float SmoothStep01(float t) {
    if (t < 0.f) {
        t = 0.f;
    }
    if (t > 1.f) {
        t = 1.f;
    }
    return t * t * (3.f - 2.f * t);
}

static bool PhotoOklchInGamut(float L, float chroma, float hue) {
    OklabColor lab;
    lab.L = L;
    lab.a = chroma * cosf(hue);
    lab.b = chroma * sinf(hue);
    float lr, lg, lb;
    OklabToLinear(lab, &lr, &lg, &lb);
    const float eps = 1e-3f;
    return lr >= -eps && lg >= -eps && lb >= -eps && lr <= 1.f + eps && lg <= 1.f + eps && lb <= 1.f + eps;
}

// Largest chroma at this lightness and hue that still fits in sRGB. Not a channel clamp.
static float PhotoMaxChroma(float L, float hue) {
    const float hiCap = 0.37f;
    if (!PhotoOklchInGamut(L, 0.f, hue)) {
        return 0.f;
    }
    if (PhotoOklchInGamut(L, hiCap, hue)) {
        return hiCap;
    }
    float lo = 0.f;
    float hi = hiCap;
    for (int i = 0; i < 10; i++) {
        float mid = 0.5f * (lo + hi);
        if (PhotoOklchInGamut(L, mid, hue)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void PhotoWriteAdapted(const OklabColor& src, float outL, float* outRgb) {
    float chroma = OklabChroma(src);
    float hue = atan2f(src.b, src.a);
    if (outL < 0.f) {
        outL = 0.f;
    }
    if (outL > 1.f) {
        outL = 1.f;
    }

    // Near-gray shadows keep their cast from being read as a real color. Never raise chroma.
    float neutral = SmoothStep01((chroma - 0.015f) / 0.045f);
    float outC = chroma * (0.20f + 0.80f * neutral) * 0.90f;
    // The binary search is the slow part. One in-gamut test covers almost every pixel.
    if (outC > 0.07f && !PhotoOklchInGamut(outL, outC, hue)) {
        float limit = PhotoMaxChroma(outL, hue) * 0.95f;
        if (outC > limit) {
            outC = limit;
        }
    }
    if (outC > chroma) {
        outC = chroma;
    }

    OklabColor out;
    out.L = outL;
    out.a = outC * cosf(hue);
    out.b = outC * sinf(hue);
    OklabToSrgb(out, &outRgb[0], &outRgb[1], &outRgb[2]);
}

void MapRgbPhotoDarkAdapt(float r, float g, float b, const DarkModePalette& palette, float* outRgb) {
    (void)palette;
    OklabColor src = SrgbToOklab(Clamp01(r), Clamp01(g), Clamp01(b));
    PhotoWriteAdapted(src, PhotoDarkToneCurve(src.L), outRgb);
}

static bool PhotoBoxMean(const float* src, int w, int h, int radius, float* dst) {
    size_t iw = (size_t)w + 1;
    size_t ih = (size_t)h + 1;
    float* integral = (float*)calloc(iw * ih, sizeof(float));
    if (!integral) {
        return false;
    }
    for (int y = 0; y < h; y++) {
        float rowSum = 0.f;
        const float* row = src + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            rowSum += row[x];
            integral[(size_t)(y + 1) * iw + (x + 1)] = integral[(size_t)y * iw + (x + 1)] + rowSum;
        }
    }
    for (int y = 0; y < h; y++) {
        int y0 = y - radius;
        int y1 = y + radius;
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 >= h) {
            y1 = h - 1;
        }
        for (int x = 0; x < w; x++) {
            int x0 = x - radius;
            int x1 = x + radius;
            if (x0 < 0) {
                x0 = 0;
            }
            if (x1 >= w) {
                x1 = w - 1;
            }
            float sum = integral[(size_t)(y1 + 1) * iw + (x1 + 1)] - integral[(size_t)y0 * iw + (x1 + 1)] -
                        integral[(size_t)(y1 + 1) * iw + x0] + integral[(size_t)y0 * iw + x0];
            int count = (x1 - x0 + 1) * (y1 - y0 + 1);
            dst[(size_t)y * w + x] = sum / (float)count;
        }
    }
    free(integral);
    return true;
}

// Self-guided smooth of OKLab L. Edges stay in the base; fine grain becomes L - base.
static bool PhotoGuidedBase(const float* L, int w, int h, int radius, float* base) {
    size_t n = (size_t)w * (size_t)h;
    float* mean = (float*)malloc(n * sizeof(float));
    float* meanSq = (float*)malloc(n * sizeof(float));
    float* sq = (float*)malloc(n * sizeof(float));
    float* a = (float*)malloc(n * sizeof(float));
    float* b = (float*)malloc(n * sizeof(float));
    float* meanA = (float*)malloc(n * sizeof(float));
    float* meanB = (float*)malloc(n * sizeof(float));
    if (!mean || !meanSq || !sq || !a || !b || !meanA || !meanB) {
        free(mean);
        free(meanSq);
        free(sq);
        free(a);
        free(b);
        free(meanA);
        free(meanB);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        sq[i] = L[i] * L[i];
    }
    const float eps = 0.00016f;
    bool ok = PhotoBoxMean(L, w, h, radius, mean) && PhotoBoxMean(sq, w, h, radius, meanSq);
    free(sq);
    sq = nullptr;
    if (ok) {
        for (size_t i = 0; i < n; i++) {
            float var = meanSq[i] - mean[i] * mean[i];
            if (var < 0.f) {
                var = 0.f;
            }
            float ai = var / (var + eps);
            a[i] = ai;
            b[i] = mean[i] * (1.f - ai);
        }
        ok = PhotoBoxMean(a, w, h, radius, meanA) && PhotoBoxMean(b, w, h, radius, meanB);
    }
    if (ok) {
        for (size_t i = 0; i < n; i++) {
            base[i] = meanA[i] * L[i] + meanB[i];
        }
    }
    free(mean);
    free(meanSq);
    free(a);
    free(b);
    free(meanA);
    free(meanB);
    return ok;
}

bool PdfDarkModePhotoAdaptRgbSamples(unsigned char* samples, int w, int h, int n, int stride) {
    if (!samples || w < 2 || h < 2 || n < 3 || stride < w * n) {
        return false;
    }
    size_t count = (size_t)w * (size_t)h;
    if (count > 6000000) {
        return false;
    }
    float* lightness = (float*)malloc(count * sizeof(float));
    float* base = (float*)malloc(count * sizeof(float));
    float* axisA = (float*)malloc(count * sizeof(float));
    float* axisB = (float*)malloc(count * sizeof(float));
    if (!lightness || !base || !axisA || !axisB) {
        free(lightness);
        free(base);
        free(axisA);
        free(axisB);
        return false;
    }
    for (int y = 0; y < h; y++) {
        unsigned char* row = samples + (size_t)y * stride;
        size_t rowBase = (size_t)y * w;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + (size_t)x * n;
            OklabColor lab = SrgbToOklab(px[0] / 255.f, px[1] / 255.f, px[2] / 255.f);
            lightness[rowBase + x] = lab.L;
            axisA[rowBase + x] = lab.a;
            axisB[rowBase + x] = lab.b;
        }
    }
    int radius = w < h ? w : h;
    radius = radius / 80;
    if (radius < 2) {
        radius = 2;
    }
    if (radius > 6) {
        radius = 6;
    }
    // Page figures do not need a guided filter. The per-pixel curve is enough and much faster.
    bool ok = count < 700000 || PhotoGuidedBase(lightness, w, h, radius, base);
    if (ok && count < 700000) {
        memcpy(base, lightness, count * sizeof(float));
    }
    if (!ok) {
        free(lightness);
        free(base);
        free(axisA);
        free(axisB);
        return false;
    }
    for (int y = 0; y < h; y++) {
        unsigned char* row = samples + (size_t)y * stride;
        size_t rowBase = (size_t)y * w;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + (size_t)x * n;
            float L = lightness[rowBase + x];
            float structure = base[rowBase + x];
            if (structure < 0.f) {
                structure = 0.f;
            }
            if (structure > 1.f) {
                structure = 1.f;
            }
            float detail = L - structure;
            float outL = PhotoDarkToneCurve(structure) + detail;
            OklabColor src;
            src.L = L;
            src.a = axisA[rowBase + x];
            src.b = axisB[rowBase + x];
            float rgb[3];
            PhotoWriteAdapted(src, outL, rgb);
            for (int c = 0; c < 3; c++) {
                int v = (int)(rgb[c] * 255.f + 0.5f);
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
    free(lightness);
    free(base);
    free(axisA);
    free(axisB);
    return true;
}

static float ToneThemeLightness(float srcL, float textL, float bgL) {
    float outL = textL + srcL * (bgL - textL);
    if (outL < 0.08f) {
        outL = 0.08f;
    }
    if (outL > 0.92f) {
        outL = 0.92f;
    }
    return outL;
}

// Soft-mask diagrams (Easy RL): mid-gray fills must become light ink on dark paper,
// not muddy midtones that later snap to paper and vanish (ghost robot icons).
static thread_local int gToneSoftMaskDiagram = 0;

static float ToneDiagramLightness(float srcL, float textL, float bgL) {
    // Only near-white → paper. Dark and mid-gray fills (robot bodies, strokes)
    // become bright text so they don't disappear into Dracula paper.
    float t = (srcL - 0.32f) / 0.50f;
    if (t < 0.f) {
        t = 0.f;
    }
    if (t > 1.f) {
        t = 1.f;
    }
    t = t * t * (3.f - 2.f * t);
    return textL + t * (bgL - textL);
}

// Same grade as MapRgbToDarkThemeOklab, including the near-white paper pull.
static float ToneInvertLightness(float srcL, float chroma, float textL, float bgL) {
    if (gToneSoftMaskDiagram) {
        return ToneDiagramLightness(srcL, textL, bgL);
    }
    float outL = ToneThemeLightness(srcL, textL, bgL);
    if (srcL > 0.82f && chroma < 0.06f) {
        float paperMix = (srcL - 0.82f) / 0.18f;
        if (paperMix > 1.f) {
            paperMix = 1.f;
        }
        outL = outL * (1.f - 0.35f * paperMix) + bgL * (0.35f * paperMix);
    }
    return outL;
}

static bool PointInLandmarkLoop(float x, float y, const DetectedFace& face, const int* idx, int nIdx, float scale) {
    float cx = 0.f;
    float cy = 0.f;
    for (int i = 0; i < nIdx; i++) {
        cx += face.landmarks[idx[i]].x;
        cy += face.landmarks[idx[i]].y;
    }
    cx /= (float)nIdx;
    cy /= (float)nIdx;
    bool inside = false;
    for (int i = 0, j = nIdx - 1; i < nIdx; j = i++) {
        float xi = cx + (face.landmarks[idx[i]].x - cx) * scale;
        float yi = cy + (face.landmarks[idx[i]].y - cy) * scale;
        float xj = cx + (face.landmarks[idx[j]].x - cx) * scale;
        float yj = cy + (face.landmarks[idx[j]].y - cy) * scale;
        if ((yi > y) != (yj > y)) {
            float denom = yj - yi;
            if (denom > -1e-6f && denom < 1e-6f) {
                continue;
            }
            float xint = (xj - xi) * (y - yi) / denom + xi;
            if (x < xint) {
                inside = !inside;
            }
        }
    }
    return inside;
}

struct HullPt {
    float x;
    float y;
};

static float HullCross(HullPt o, HullPt a, HullPt b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// A profile face folds the oval into a self-crossing loop. Even-odd fill then
// leaves holes, and those pixels stay in the dark grade as black patches.
static int BuildConvexHull(HullPt* pts, int n, HullPt* hull) {
    for (int i = 1; i < n; i++) {
        HullPt key = pts[i];
        int j = i - 1;
        while (j >= 0 && (pts[j].x > key.x || (pts[j].x == key.x && pts[j].y > key.y))) {
            pts[j + 1] = pts[j];
            j--;
        }
        pts[j + 1] = key;
    }
    if (n < 3) {
        for (int i = 0; i < n; i++) {
            hull[i] = pts[i];
        }
        return n;
    }
    int h = 0;
    for (int i = 0; i < n; i++) {
        while (h >= 2 && HullCross(hull[h - 2], hull[h - 1], pts[i]) <= 0.f) {
            h--;
        }
        hull[h++] = pts[i];
    }
    int lower = h + 1;
    for (int i = n - 2; i >= 0; i--) {
        while (h >= lower && HullCross(hull[h - 2], hull[h - 1], pts[i]) <= 0.f) {
            h--;
        }
        hull[h++] = pts[i];
    }
    if (h > 0) {
        h--;
    }
    return h;
}

static bool PointInConvex(float x, float y, const HullPt* hull, int n) {
    if (n < 3) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        HullPt a = hull[i];
        HullPt b = hull[(i + 1) % n];
        float c = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (c < -0.01f) {
            return false;
        }
    }
    return true;
}

static void FillConvexLandmarkLoop(float* mask, int w, int h, const DetectedFace& face, const int* idx, int nIdx,
                                   float scale) {
    if (nIdx < 3 || nIdx > 80) {
        return;
    }
    HullPt pts[80];
    float cx = 0.f;
    float cy = 0.f;
    for (int i = 0; i < nIdx; i++) {
        cx += face.landmarks[idx[i]].x;
        cy += face.landmarks[idx[i]].y;
    }
    cx /= (float)nIdx;
    cy /= (float)nIdx;
    for (int i = 0; i < nIdx; i++) {
        pts[i].x = cx + (face.landmarks[idx[i]].x - cx) * scale;
        pts[i].y = cy + (face.landmarks[idx[i]].y - cy) * scale;
    }
    HullPt hull[80];
    int hn = BuildConvexHull(pts, nIdx, hull);
    if (hn < 3) {
        return;
    }
    float minX = hull[0].x;
    float minY = hull[0].y;
    float maxX = hull[0].x;
    float maxY = hull[0].y;
    for (int i = 1; i < hn; i++) {
        if (hull[i].x < minX) {
            minX = hull[i].x;
        }
        if (hull[i].y < minY) {
            minY = hull[i].y;
        }
        if (hull[i].x > maxX) {
            maxX = hull[i].x;
        }
        if (hull[i].y > maxY) {
            maxY = hull[i].y;
        }
    }
    int x0 = (int)floorf(minX);
    int y0 = (int)floorf(minY);
    int x1 = (int)ceilf(maxX);
    int y1 = (int)ceilf(maxY);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (PointInConvex((float)x + 0.5f, (float)y + 0.5f, hull, hn)) {
                mask[(size_t)y * (size_t)w + (size_t)x] = 1.f;
            }
        }
    }
}

static void FillLandmarkLoop(float* mask, int w, int h, const DetectedFace& face, const int* idx, int nIdx,
                             float scale) {
    float minX = (float)w;
    float minY = (float)h;
    float maxX = 0.f;
    float maxY = 0.f;
    float cx = 0.f;
    float cy = 0.f;
    for (int i = 0; i < nIdx; i++) {
        cx += face.landmarks[idx[i]].x;
        cy += face.landmarks[idx[i]].y;
    }
    cx /= (float)nIdx;
    cy /= (float)nIdx;
    for (int i = 0; i < nIdx; i++) {
        float x = cx + (face.landmarks[idx[i]].x - cx) * scale;
        float y = cy + (face.landmarks[idx[i]].y - cy) * scale;
        if (x < minX) {
            minX = x;
        }
        if (y < minY) {
            minY = y;
        }
        if (x > maxX) {
            maxX = x;
        }
        if (y > maxY) {
            maxY = y;
        }
    }
    int x0 = (int)floorf(minX);
    int y0 = (int)floorf(minY);
    int x1 = (int)ceilf(maxX);
    int y1 = (int)ceilf(maxY);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (PointInLandmarkLoop((float)x + 0.5f, (float)y + 0.5f, face, idx, nIdx, scale)) {
                mask[(size_t)y * (size_t)w + (size_t)x] = 1.f;
            }
        }
    }
}

struct FaceBlendSpec {
    float faceBlend = 0.f;
    float eyeBlend = -1.f;
    float mouthBlend = -1.f;
    bool mild = false;
};

static float LoopWidth(const DetectedFace& face, const int* idx, int nIdx) {
    float minX = 1e9f;
    float maxX = -1e9f;
    for (int i = 0; i < nIdx; i++) {
        float x = face.landmarks[idx[i]].x;
        if (x < minX) {
            minX = x;
        }
        if (x > maxX) {
            maxX = x;
        }
    }
    float w = maxX - minX;
    if (w < 8.f) {
        w = 8.f;
    }
    return w;
}

static bool FaceOvalUsable(const DetectedFace& face, int w, int h) {
    if (face.presenceScore < 0.5f) {
        return false;
    }
    int outside = 0;
    float minX = 1e9f;
    float maxX = -1e9f;
    for (int i = 0; i < kFaceOvalCount; i++) {
        float x = face.landmarks[kFaceOvalContour[i]].x;
        float y = face.landmarks[kFaceOvalContour[i]].y;
        if (x < minX) {
            minX = x;
        }
        if (x > maxX) {
            maxX = x;
        }
        if (x < -8.f || y < -8.f || x > (float)w + 8.f || y > (float)h + 8.f) {
            outside++;
        }
    }
    float fw = maxX - minX;
    // A face in a group photo can be a small slice of the frame. Skip only
    // boxes too small to cover eyes and a mouth.
    if (fw < 22.f) {
        return false;
    }
    if (outside > kFaceOvalCount / 3) {
        return false;
    }
    return true;
}

static void FillFaceEllipse(float* mask, int w, int h, float cx, float cy, float rx, float ry) {
    if (rx < 2.f || ry < 2.f) {
        return;
    }
    int x0 = (int)floorf(cx - rx);
    int y0 = (int)floorf(cy - ry);
    int x1 = (int)ceilf(cx + rx);
    int y1 = (int)ceilf(cy + ry);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float nx = ((float)x + 0.5f - cx) / rx;
            float ny = ((float)y + 0.5f - cy) / ry;
            if (nx * nx + ny * ny <= 1.f) {
                mask[(size_t)y * (size_t)w + (size_t)x] = 1.f;
            }
        }
    }
}

static void ZeroMask(float* mask, size_t n);
static void MaxBlurred(float* layer, int w, int h, int radius, float scale, float* weight, bool keepCore);

// Short dark hair sits above the face oval, inside a head ellipse. A rectangle
// there reads as a square pasted on the head. A light cap stays in the dark
// grade: it is blue, while hair is not.
static void AddShortHairAboveOval(const u8* rgb, int stride, float* layer, int w, int h, const DetectedFace& face,
                                  float scale, float* weight) {
    float minX = 1e9f;
    float maxX = -1e9f;
    float minY = 1e9f;
    float maxY = -1e9f;
    for (int i = 0; i < kFaceOvalCount; i++) {
        float x = face.landmarks[kFaceOvalContour[i]].x;
        float y = face.landmarks[kFaceOvalContour[i]].y;
        if (x < minX) {
            minX = x;
        }
        if (x > maxX) {
            maxX = x;
        }
        if (y < minY) {
            minY = y;
        }
        if (y > maxY) {
            maxY = y;
        }
    }
    float faceW = maxX - minX;
    float faceH = maxY - minY;
    if (faceW < 18.f || faceH < 18.f) {
        return;
    }
    float cx = (minX + maxX) * 0.5f;
    float top = minY - faceH * 0.72f;
    float bot = minY + faceH * 0.22f;
    float cy = (top + bot) * 0.5f;
    float rx = faceW * 0.62f;
    float ry = (bot - top) * 0.5f;
    if (rx < 8.f || ry < 8.f) {
        return;
    }
    int x0 = (int)floorf(cx - rx);
    int y0 = (int)floorf(cy - ry);
    int x1 = (int)ceilf(cx + rx);
    int y1 = (int)ceilf(cy + ry);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    size_t n = (size_t)w * (size_t)h;
    ZeroMask(layer, n);
    float invRx = 1.f / rx;
    float invRy = 1.f / ry;
    for (int y = y0; y <= y1; y++) {
        const u8* row = rgb + (size_t)y * (size_t)stride;
        for (int x = x0; x <= x1; x++) {
            float nx = ((float)x + 0.5f - cx) * invRx;
            float ny = ((float)y + 0.5f - cy) * invRy;
            if (nx * nx + ny * ny > 1.f) {
                continue;
            }
            const u8* px = row + (size_t)x * 3;
            int r = px[0];
            int g = px[1];
            int b = px[2];
            int sum = r + g + b;
            if (sum > 500) {
                continue;
            }
            if (b > r + 35 && b > g + 18 && b > 70) {
                continue;
            }
            layer[(size_t)y * (size_t)w + (size_t)x] = 1.f;
        }
    }
    int feather = (int)(faceW * 0.05f);
    MaxBlurred(layer, w, h, feather, scale, weight, true);
    int pad = feather + 2;
    int cx0 = x0 - pad;
    int cy0 = y0 - pad;
    int cx1 = x1 + pad;
    int cy1 = y1 + pad;
    if (cx0 < 0) {
        cx0 = 0;
    }
    if (cy0 < 0) {
        cy0 = 0;
    }
    if (cx1 >= w) {
        cx1 = w - 1;
    }
    if (cy1 >= h) {
        cy1 = h - 1;
    }
    // Drop pale paper the hair feather picked up. Stay above the forehead so
    // this does not cut a flat line through the face.
    float faceTop = minY + faceH * 0.05f;
    for (int y = cy0; y <= cy1; y++) {
        if ((float)y >= faceTop) {
            continue;
        }
        const u8* row = rgb + (size_t)y * (size_t)stride;
        for (int x = cx0; x <= cx1; x++) {
            float nx = ((float)x + 0.5f - cx) * invRx;
            float ny = ((float)y + 0.5f - cy) * invRy;
            if (nx * nx + ny * ny > 1.35f) {
                continue;
            }
            const u8* px = row + (size_t)x * 3;
            int sum = (int)px[0] + (int)px[1] + (int)px[2];
            if (sum > 700) {
                weight[(size_t)y * (size_t)w + (size_t)x] = 0.f;
            }
        }
    }
}

static void ZeroMask(float* mask, size_t n) {
    for (size_t i = 0; i < n; i++) {
        mask[i] = 0.f;
    }
}

// Blur a binary mask and keep the larger weight. Center stays near `scale`, edge falls to 0.
// keepCore holds the filled interior at full scale. Eyes and brows are smaller than their
// feather, so a plain blur would wash the pupil and the brow out.
static void MaxBlurred(float* layer, int w, int h, int radius, float scale, float* weight, bool keepCore) {
    if (scale <= 0.f) {
        return;
    }
    if (radius < 2) {
        radius = 2;
    }
    if (radius > 48) {
        radius = 48;
    }
    size_t n = (size_t)w * (size_t)h;
    float* soft = (float*)malloc(n * sizeof(float));
    if (!soft || !PhotoBoxMean(layer, w, h, radius, soft)) {
        free(soft);
        for (size_t i = 0; i < n; i++) {
            float v = layer[i] * scale;
            if (v > weight[i]) {
                weight[i] = v;
            }
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float v = soft[i];
        if (v < 0.f) {
            v = 0.f;
        }
        if (v > 1.f) {
            v = 1.f;
        }
        if (keepCore && layer[i] > 0.5f) {
            v = 1.f;
        }
        v *= scale;
        if (v > weight[i]) {
            weight[i] = v;
        }
    }
    free(soft);
}

// Left and right of the face, not the shoulders. A flat studio backdrop is what
// turns a wide feather into a pale ring after the page goes dark.
static bool SideBackdropIsFlat(const u8* rgb, int stride, int w, int h, float minX, float minY, float maxX, float maxY,
                               int* outR, int* outG, int* outB) {
    float fw = maxX - minX;
    float fh = maxY - minY;
    if (fw < 18.f || fh < 18.f) {
        return false;
    }
    double sumR = 0, sumG = 0, sumB = 0, sumL = 0, sumL2 = 0;
    int count = 0;
    auto acc = [&](int x, int y) {
        if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) {
            return;
        }
        const u8* px = rgb + (size_t)y * (size_t)stride + (size_t)x * 3;
        int r = px[0];
        int g = px[1];
        int b = px[2];
        int hi = r > g ? r : g;
        if (b > hi) {
            hi = b;
        }
        int lo = r < g ? r : g;
        if (b < lo) {
            lo = b;
        }
        // A red shirt or a hand in the strip is not the backdrop.
        if (hi - lo > 28) {
            return;
        }
        int L = (r + g + b) / 3;
        sumR += r;
        sumG += g;
        sumB += b;
        sumL += L;
        sumL2 += (double)L * (double)L;
        count++;
    };
    int y0 = (int)(minY + fh * 0.05f);
    int y1 = (int)(minY + fh * 0.62f);
    int lx0 = (int)(minX - fw * 0.40f);
    int lx1 = (int)(minX - fw * 0.14f);
    int rx0 = (int)(maxX + fw * 0.14f);
    int rx1 = (int)(maxX + fw * 0.40f);
    for (int y = y0; y <= y1; y += 2) {
        for (int x = lx0; x <= lx1; x += 2) {
            acc(x, y);
        }
        for (int x = rx0; x <= rx1; x += 2) {
            acc(x, y);
        }
    }
    if (count < 24) {
        return false;
    }
    double meanL = sumL / (double)count;
    double var = sumL2 / (double)count - meanL * meanL;
    if (var < 0) {
        var = 0;
    }
    if (var > 280.0) {
        return false;
    }
    *outR = (int)(sumR / (double)count + 0.5);
    *outG = (int)(sumG / (double)count + 0.5);
    *outB = (int)(sumB / (double)count + 0.5);
    return true;
}

static void ClearBackdropFringe(const u8* rgb, int stride, const u8* core, int w, int h, int x0, int y0, int x1, int y1,
                                int br, int bgc, int bb, float* weight) {
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    const int limit = 55 * 55;
    for (int y = y0; y <= y1; y++) {
        const u8* row = rgb + (size_t)y * (size_t)stride;
        for (int x = x0; x <= x1; x++) {
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            const u8* px = row + (size_t)x * 3;
            int dr = (int)px[0] - br;
            int dg = (int)px[1] - bgc;
            int db = (int)px[2] - bb;
            if (dr * dr + dg * dg + db * db > limit) {
                continue;
            }
            if (core && core[i]) {
                // Only the light backdrop the oval swallowed along its edge.
                // Teeth sit deeper in the face and stay.
                bool onEdge = false;
                for (int dy = -28; dy <= 28 && !onEdge; dy += 7) {
                    int yy = y + dy;
                    if ((unsigned)yy >= (unsigned)h) {
                        continue;
                    }
                    for (int dx = -28; dx <= 28; dx += 7) {
                        int xx = x + dx;
                        if ((unsigned)xx >= (unsigned)w) {
                            continue;
                        }
                        if (!core[(size_t)yy * (size_t)w + (size_t)xx]) {
                            onEdge = true;
                            break;
                        }
                    }
                }
                if (!onEdge) {
                    continue;
                }
                int hi = px[0] > px[1] ? px[0] : px[1];
                if (px[2] > hi) {
                    hi = px[2];
                }
                int lo = px[0] < px[1] ? px[0] : px[1];
                if (px[2] < lo) {
                    lo = px[2];
                }
                if (hi - lo > 36) {
                    continue;
                }
            }
            weight[i] = 0.f;
        }
    }
}

// Center of a painted oval (the yellow reading-guide blob) matches almost
// every sample. A head the mesh refused still has eyes, hair, and shade.
static bool EllipseInteriorIsFlatPaint(const u8* rgb, int stride, int w, int h, float cx, float cy, float rx,
                                       float ry) {
    if (!rgb || rx < 2.f || ry < 2.f) {
        return false;
    }
    int ccx = (int)(cx + 0.5f);
    int ccy = (int)(cy + 0.5f);
    if (ccx < 0 || ccy < 0 || ccx >= w || ccy >= h) {
        return false;
    }
    const u8* center = rgb + (size_t)ccy * (size_t)stride + (size_t)ccx * 3;
    int cr = center[0];
    int cg = center[1];
    int cb = center[2];
    int stepX = (int)(rx / 6.f);
    int stepY = (int)(ry / 6.f);
    if (stepX < 1) {
        stepX = 1;
    }
    if (stepY < 1) {
        stepY = 1;
    }
    int n = 0;
    int same = 0;
    int x0 = (int)floorf(cx - rx);
    int y0 = (int)floorf(cy - ry);
    int x1 = (int)ceilf(cx + rx);
    int y1 = (int)ceilf(cy + ry);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= w) {
        x1 = w - 1;
    }
    if (y1 >= h) {
        y1 = h - 1;
    }
    for (int y = y0; y <= y1; y += stepY) {
        for (int x = x0; x <= x1; x += stepX) {
            float nx = ((float)x + 0.5f - cx) / rx;
            float ny = ((float)y + 0.5f - cy) / ry;
            if (nx * nx + ny * ny > 1.f) {
                continue;
            }
            const u8* px = rgb + (size_t)y * (size_t)stride + (size_t)x * 3;
            int dr = px[0] - cr;
            int dg = px[1] - cg;
            int db = px[2] - cb;
            if (dr < 0) {
                dr = -dr;
            }
            if (dg < 0) {
                dg = -dg;
            }
            if (db < 0) {
                db = -db;
            }
            int d = dr > dg ? dr : dg;
            if (db > d) {
                d = db;
            }
            if (d <= 30) {
                same++;
            }
            n++;
        }
    }
    return n >= 16 && same * 5 >= n * 4;
}

static void BuildFaceBlendWeight(const u8* rgb, int w, int h, int stride, const FaceBlendSpec& spec, float* weight) {
    size_t n = (size_t)w * (size_t)h;
    ZeroMask(weight, n);
    if (spec.faceBlend <= 0.f && spec.eyeBlend <= 0.f && spec.mouthBlend <= 0.f) {
        return;
    }
    FaceLandmarkEngine engine;
    Vec<DetectedFace> faces;
    if (!engine.Detect(rgb, w, h, stride, faces, nullptr)) {
        return;
    }
    float* layer = (float*)malloc(n * sizeof(float));
    if (!layer) {
        return;
    }
    for (const DetectedFace& face : faces) {
        if (!FaceOvalUsable(face, w, h)) {
            if (spec.faceBlend <= 0.f || face.bbox.dx < 28.f || face.bbox.dy < 28.f) {
                continue;
            }
            float cx = face.bbox.x + face.bbox.dx * 0.5f;
            float cy = face.bbox.y + face.bbox.dy * 0.46f;
            float rx = face.bbox.dx * 0.62f;
            float ry = face.bbox.dy * 0.70f;
            if (EllipseInteriorIsFlatPaint(rgb, stride, w, h, cx, cy, rx, ry)) {
                continue;
            }
            ZeroMask(layer, n);
            FillFaceEllipse(layer, w, h, cx, cy, rx, ry);
            int feather = (int)(face.bbox.dx * 0.12f);
            MaxBlurred(layer, w, h, feather, spec.faceBlend, weight, true);
            continue;
        }
        float faceW = LoopWidth(face, kFaceOvalContour, kFaceOvalCount);
        if (spec.faceBlend > 0.f) {
            ZeroMask(layer, n);
            float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (int i = 0; i < kFaceOvalCount; i++) {
                float x = face.landmarks[kFaceOvalContour[i]].x;
                float y = face.landmarks[kFaceOvalContour[i]].y;
                if (x < minX) {
                    minX = x;
                }
                if (x > maxX) {
                    maxX = x;
                }
                if (y < minY) {
                    minY = y;
                }
                if (y > maxY) {
                    maxY = y;
                }
            }
            // A yellow reading-guide oval can land a full mesh. Its interior is one
            // paint color, so do not paste it back as a face.
            if (EllipseInteriorIsFlatPaint(rgb, stride, w, h, (minX + maxX) * 0.5f, (minY + maxY) * 0.5f,
                                           (maxX - minX) * 0.42f, (maxY - minY) * 0.42f)) {
                continue;
            }
            int bgR = 0, bgG = 0, bgB = 0;
            bool flatBg = SideBackdropIsFlat(rgb, stride, w, h, minX, minY, maxX, maxY, &bgR, &bgG, &bgB);
            // Convex hull of face oval + nose silhouette (bridge / tip / alae).
            // A pure oval leaves the nose tip and outer alae short of paste coverage.
            int silhouette[kFaceOvalCount + kNoseSilhouetteCount];
            int nSil = 0;
            for (int i = 0; i < kFaceOvalCount; i++) {
                silhouette[nSil++] = kFaceOvalContour[i];
            }
            for (int i = 0; i < kNoseSilhouetteCount; i++) {
                silhouette[nSil++] = kNoseSilhouette[i];
            }
            FillConvexLandmarkLoop(layer, w, h, face, silhouette, nSil, flatBg ? 1.0f : 1.08f);
            int feather = (int)(faceW * 0.10f);
            if (flatBg) {
                // A wide feather on a flat backdrop paints a pale ring once that backdrop goes dark.
                feather = (int)(faceW * 0.018f);
                if (feather < 2) {
                    feather = 2;
                }
                if (feather > 5) {
                    feather = 5;
                }
            }
            MaxBlurred(layer, w, h, feather, spec.faceBlend, weight, true);
            u8* core = nullptr;
            if (flatBg) {
                core = (u8*)malloc(n);
                if (core) {
                    for (size_t i = 0; i < n; i++) {
                        core[i] = layer[i] > 0.5f ? 1 : 0;
                    }
                    ClearBackdropFringe(rgb, stride, core, w, h, (int)(minX - faceW), (int)(minY - faceW),
                                        (int)(maxX + faceW), (int)(maxY + faceW), bgR, bgG, bgB, weight);
                }
            }
            AddShortHairAboveOval(rgb, stride, layer, w, h, face, spec.faceBlend, weight);
            if (flatBg && core) {
                ClearBackdropFringe(rgb, stride, core, w, h, (int)(minX - faceW), (int)(minY - faceW * 1.2f),
                                    (int)(maxX + faceW), (int)(maxY + faceW), bgR, bgG, bgB, weight);
                free(core);
            }
        }
        if (spec.eyeBlend > 0.f) {
            ZeroMask(layer, n);
            FillLandmarkLoop(layer, w, h, face, kLeftEyeContour, kEyeContourCount, 1.20f);
            FillLandmarkLoop(layer, w, h, face, kRightEyeContour, kEyeContourCount, 1.20f);
            float eyeW = LoopWidth(face, kLeftEyeContour, kEyeContourCount);
            int feather = (int)(eyeW * 0.35f);
            MaxBlurred(layer, w, h, feather, spec.eyeBlend, weight, true);
        }
        if (spec.mouthBlend > 0.f) {
            ZeroMask(layer, n);
            FillLandmarkLoop(layer, w, h, face, kOuterLipContour, kOuterLipCount, 1.18f);
            float mouthW = LoopWidth(face, kOuterLipContour, kOuterLipCount);
            int feather = (int)(mouthW * 0.12f);
            MaxBlurred(layer, w, h, feather, spec.mouthBlend, weight, true);
        }
    }
    free(layer);
}

static void MildFaceRgb(float r, float g, float b, float* out) {
    // Keep hue. Drop lightness a little so the restored face sits with the dark grade.
    OklabColor lab = SrgbToOklab(Clamp01(r), Clamp01(g), Clamp01(b));
    lab.L *= 0.86f;
    if (lab.L < 0.02f) {
        lab.L = 0.02f;
    }
    if (lab.L > 0.96f) {
        lab.L = 0.96f;
    }
    OklabToSrgb(lab, &out[0], &out[1], &out[2]);
}

static void WriteDarkPixel(unsigned char* px, const OklabColor& text, const OklabColor& bg, float paperR, float paperG,
                           float paperB) {
    OklabColor lab = SrgbToOklab(px[0] / 255.f, px[1] / 255.f, px[2] / 255.f);
    float chroma = OklabChroma(lab);
    // Near-white / light-gray paper (JPEG often ~170–230) must become the theme
    // page color (Dracula #282A36). Mapping only pure white leaves a darker
    // charcoal rectangle that looks black next to the canvas.
    float paperL = gToneSoftMaskDiagram ? 0.88f : 0.70f;
    if (lab.L > paperL && chroma < 0.08f) {
        px[0] = (unsigned char)(paperR * 255.f + 0.5f);
        px[1] = (unsigned char)(paperG * 255.f + 0.5f);
        px[2] = (unsigned char)(paperB * 255.f + 0.5f);
        return;
    }
    float outL = ToneInvertLightness(lab.L, chroma, text.L, bg.L);
    float outC = chroma;
    if (outC > 0.38f) {
        outC = 0.38f;
    }
    // Soft-mask diagrams: keep colored arrows/cells vivid on dark paper.
    if (gToneSoftMaskDiagram && chroma > 0.04f && outL < (text.L * 0.55f + bg.L * 0.45f)) {
        outL = text.L * 0.55f + bg.L * 0.45f;
    }
    float outA = 0.f;
    float outB = 0.f;
    if (chroma > 1e-5f) {
        float scale = outC / chroma;
        outA = lab.a * scale;
        outB = lab.b * scale;
    }
    OklabColor out{outL, outA, outB};
    float r, g, b;
    OklabToSrgb(out, &r, &g, &b);
    px[0] = (unsigned char)(r * 255.f + 0.5f);
    px[1] = (unsigned char)(g * 255.f + 0.5f);
    px[2] = (unsigned char)(b * 255.f + 0.5f);
}

// Pull remapped pixels that landed next to the theme paper onto the exact paper
// RGB. Trilinear LUT and mid-gray paper both leave a few levels of charcoal that
// read as a black box under Dracula.
static void SnapNearPaperPixels(unsigned char* samples, int w, int h, int n, int stride, float paperR, float paperG,
                                float paperB) {
    int pr = (int)(paperR * 255.f + 0.5f);
    int pg = (int)(paperG * 255.f + 0.5f);
    int pb = (int)(paperB * 255.f + 0.5f);
    for (int y = 0; y < h; y++) {
        unsigned char* row = samples + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + (size_t)x * n;
            int dr = px[0] - pr;
            int dg = px[1] - pg;
            int db = px[2] - pb;
            if (dr < 0) {
                dr = -dr;
            }
            if (dg < 0) {
                dg = -dg;
            }
            if (db < 0) {
                db = -db;
            }
            int manhattan = dr + dg + db;
            if (manhattan == 0 || manhattan > 28) {
                continue;
            }
            int hi = px[0];
            int lo = px[0];
            if (px[1] > hi) {
                hi = px[1];
            }
            if (px[1] < lo) {
                lo = px[1];
            }
            if (px[2] > hi) {
                hi = px[2];
            }
            if (px[2] < lo) {
                lo = px[2];
            }
            if (hi - lo > 22) {
                continue;
            }
            px[0] = (unsigned char)pr;
            px[1] = (unsigned char)pg;
            px[2] = (unsigned char)pb;
        }
    }
}

// Soft-mask LaTeX / Easy RL diagrams: OKLab tone leaves milky anti-aliased fringes
// and muddy mid-gray fills that look soft/ghostly on dark paper. Snap near-neutral
// pixels hard to paper or text; lift dim chromatic ink so arrows/cells stay vivid.
static void CrispSoftMaskDiagramAfterTone(unsigned char* samples, int w, int h, int n, int stride, float paperR,
                                          float paperG, float paperB, float textR, float textG, float textB) {
    int pr = (int)(paperR * 255.f + 0.5f);
    int pg = (int)(paperG * 255.f + 0.5f);
    int pb = (int)(paperB * 255.f + 0.5f);
    int tr = (int)(textR * 255.f + 0.5f);
    int tg = (int)(textG * 255.f + 0.5f);
    int tb = (int)(textB * 255.f + 0.5f);
    int paperLuma = pr + pg + pb;
    int textLuma = tr + tg + tb;
    for (int y = 0; y < h; y++) {
        unsigned char* row = samples + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + (size_t)x * n;
            int hi = px[0];
            int lo = px[0];
            if (px[1] > hi) {
                hi = px[1];
            }
            if (px[1] < lo) {
                lo = px[1];
            }
            if (px[2] > hi) {
                hi = px[2];
            }
            if (px[2] < lo) {
                lo = px[2];
            }
            int span = hi - lo;
            int dr = px[0] - pr;
            int dg = px[1] - pg;
            int db = px[2] - pb;
            if (dr < 0) {
                dr = -dr;
            }
            if (dg < 0) {
                dg = -dg;
            }
            if (db < 0) {
                db = -db;
            }
            int dPaper = dr + dg + db;
            dr = px[0] - tr;
            dg = px[1] - tg;
            db = px[2] - tb;
            if (dr < 0) {
                dr = -dr;
            }
            if (dg < 0) {
                dg = -dg;
            }
            if (db < 0) {
                db = -db;
            }
            int dText = dr + dg + db;
            if (span <= 30) {
                // Neutral: hard paper/ink decision restores AA stroke edges.
                if (dPaper == 0 || dText == 0) {
                    continue;
                }
                int luma = px[0] + px[1] + px[2];
                float t = 0.5f;
                if (textLuma != paperLuma) {
                    t = (float)(luma - paperLuma) / (float)(textLuma - paperLuma);
                }
                // Bias toward text — mid-gray fills were ink, not paper.
                if (dPaper <= 36 || t < 0.28f) {
                    px[0] = (unsigned char)pr;
                    px[1] = (unsigned char)pg;
                    px[2] = (unsigned char)pb;
                } else {
                    px[0] = (unsigned char)tr;
                    px[1] = (unsigned char)tg;
                    px[2] = (unsigned char)tb;
                }
                continue;
            }
            // Chromatic ink sunk toward paper: lift lightness toward text, keep hue.
            if (dPaper < 90) {
                float lift = (90.f - (float)dPaper) / 90.f;
                if (lift > 0.55f) {
                    lift = 0.55f;
                }
                for (int c = 0; c < 3; c++) {
                    int src = px[c];
                    int dst = (c == 0) ? tr : (c == 1) ? tg : tb;
                    int v = (int)(src + (dst - src) * lift * 0.65f + 0.5f);
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

// One OKLab conversion per pixel is most of smart-invert time (about 85 ms for a
// 533x824 picture, several times that at the 1600px decode cap). The grade depends
// only on the source RGB and the theme text/background lightness, so a 64^3 cube
// with trilinear interpolation replaces the pow/cbrt loop. The cube is built once
// per palette.
static const int kToneLutN = 64;

struct ToneLut {
    float textL = -1.f;
    float bgL = -1.f;
    float paperR = -1.f;
    float paperG = -1.f;
    float paperB = -1.f;
    int version = 0;
    int diagram = 0;
    u8* data = nullptr;
};

static ToneLut gToneLut;
static ToneLut gToneDiagramLut;
static const int kToneLutAlgoVersion = 4; // soft-mask diagram steep mid→text curve
static INIT_ONCE gToneLutOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION gToneLutCs;
static thread_local const u8* gToneLutActiveData = nullptr;

static BOOL CALLBACK ToneLutInitCs(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&gToneLutCs);
    return TRUE;
}

static int ToneLutOffset(int r, int g, int b) {
    return ((r * kToneLutN + g) * kToneLutN + b) * 3;
}

static bool EnsureToneLut(const OklabColor& text, const OklabColor& bg, float paperR, float paperG, float paperB,
                          bool diagram, double* buildMs) {
    if (buildMs) {
        *buildMs = 0;
    }
    InitOnceExecuteOnce(&gToneLutOnce, ToneLutInitCs, nullptr, nullptr);
    EnterCriticalSection(&gToneLutCs);
    ToneLut* lut = diagram ? &gToneDiagramLut : &gToneLut;
    bool same = lut->data && lut->version == kToneLutAlgoVersion && lut->diagram == (diagram ? 1 : 0) &&
                fabsf(lut->textL - text.L) < 1e-5f && fabsf(lut->bgL - bg.L) < 1e-5f &&
                fabsf(lut->paperR - paperR) < 1e-5f && fabsf(lut->paperG - paperG) < 1e-5f &&
                fabsf(lut->paperB - paperB) < 1e-5f;
    if (same) {
        gToneLutActiveData = lut->data;
        LeaveCriticalSection(&gToneLutCs);
        return true;
    }
    u8* data = lut->data;
    if (!data) {
        data = (u8*)malloc((size_t)kToneLutN * kToneLutN * kToneLutN * 3);
        if (!data) {
            LeaveCriticalSection(&gToneLutCs);
            return false;
        }
        lut->data = data;
    }
    LARGE_INTEGER buildStart = TimeGet();
    int prevDiagram = gToneSoftMaskDiagram;
    gToneSoftMaskDiagram = diagram ? 1 : 0;
    // WriteDarkPixel overwrites its input, so each lattice color needs its own buffer.
    // Reusing one buffer fed the previous output back in and the cube was garbage.
    for (int r = 0; r < kToneLutN; r++) {
        unsigned char r8 = (unsigned char)((r * 255) / (kToneLutN - 1));
        for (int g = 0; g < kToneLutN; g++) {
            unsigned char g8 = (unsigned char)((g * 255) / (kToneLutN - 1));
            for (int b = 0; b < kToneLutN; b++) {
                unsigned char px[3];
                px[0] = r8;
                px[1] = g8;
                px[2] = (unsigned char)((b * 255) / (kToneLutN - 1));
                WriteDarkPixel(px, text, bg, paperR, paperG, paperB);
                u8* slot = data + ToneLutOffset(r, g, b);
                slot[0] = px[0];
                slot[1] = px[1];
                slot[2] = px[2];
            }
        }
    }
    gToneSoftMaskDiagram = prevDiagram;
    lut->textL = text.L;
    lut->bgL = bg.L;
    lut->paperR = paperR;
    lut->paperG = paperG;
    lut->paperB = paperB;
    lut->version = kToneLutAlgoVersion;
    lut->diagram = diagram ? 1 : 0;
    gToneLutActiveData = lut->data;
    if (buildMs) {
        *buildMs = TimeSinceInMs(buildStart);
    }
    LeaveCriticalSection(&gToneLutCs);
    return true;
}

struct ToneApplyStats {
    int maxDelta = 0;
    int samples = 0;
};

static void ApplyTonePixels(unsigned char* samples, int w, int h, int n, int stride, const OklabColor& text,
                            const OklabColor& bg, float paperR, float paperG, float paperB, bool useLut, bool nearest,
                            bool measure, ToneApplyStats* stats) {
    for (int y = 0; y < h; y++) {
        unsigned char* row = samples + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char* px = row + (size_t)x * n;
            bool sample = measure && stats && (((x + y) & 63) == 0);
            unsigned char exact[3];
            if (sample) {
                exact[0] = px[0];
                exact[1] = px[1];
                exact[2] = px[2];
                WriteDarkPixel(exact, text, bg, paperR, paperG, paperB);
            }
            if (useLut) {
                const u8* data = gToneLutActiveData ? gToneLutActiveData : gToneLut.data;
                const float scale = (kToneLutN - 1) / 255.f;
                if (nearest) {
                    // Soft-mask / no-face diagrams: nearest is several times faster
                    // than trilinear and paper already snaps to exact theme RGB.
                    int r0 = (int)(px[0] * scale + 0.5f);
                    int g0 = (int)(px[1] * scale + 0.5f);
                    int b0 = (int)(px[2] * scale + 0.5f);
                    if (r0 >= kToneLutN) {
                        r0 = kToneLutN - 1;
                    }
                    if (g0 >= kToneLutN) {
                        g0 = kToneLutN - 1;
                    }
                    if (b0 >= kToneLutN) {
                        b0 = kToneLutN - 1;
                    }
                    const u8* slot = data + ToneLutOffset(r0, g0, b0);
                    px[0] = slot[0];
                    px[1] = slot[1];
                    px[2] = slot[2];
                } else {
                    // Trilinear for photo grades where face restore needs smooth tone.
                    float rf = px[0] * scale;
                    float gf = px[1] * scale;
                    float bf = px[2] * scale;
                    int r0 = (int)rf;
                    int g0 = (int)gf;
                    int b0 = (int)bf;
                    if (r0 >= kToneLutN - 1) {
                        r0 = kToneLutN - 1;
                    }
                    if (g0 >= kToneLutN - 1) {
                        g0 = kToneLutN - 1;
                    }
                    if (b0 >= kToneLutN - 1) {
                        b0 = kToneLutN - 1;
                    }
                    float fr = rf - (float)r0;
                    float fg = gf - (float)g0;
                    float fb = bf - (float)b0;
                    if (fr < 0.f) {
                        fr = 0.f;
                    }
                    if (fg < 0.f) {
                        fg = 0.f;
                    }
                    if (fb < 0.f) {
                        fb = 0.f;
                    }
                    int r1 = r0 < kToneLutN - 1 ? r0 + 1 : r0;
                    int g1 = g0 < kToneLutN - 1 ? g0 + 1 : g0;
                    int b1 = b0 < kToneLutN - 1 ? b0 + 1 : b0;
                    const u8* c000 = data + ToneLutOffset(r0, g0, b0);
                    const u8* c100 = data + ToneLutOffset(r1, g0, b0);
                    const u8* c010 = data + ToneLutOffset(r0, g1, b0);
                    const u8* c110 = data + ToneLutOffset(r1, g1, b0);
                    const u8* c001 = data + ToneLutOffset(r0, g0, b1);
                    const u8* c101 = data + ToneLutOffset(r1, g0, b1);
                    const u8* c011 = data + ToneLutOffset(r0, g1, b1);
                    const u8* c111 = data + ToneLutOffset(r1, g1, b1);
                    float ir = 1.f - fr;
                    float ig = 1.f - fg;
                    float ib = 1.f - fb;
                    for (int c = 0; c < 3; c++) {
                        float v00 = c000[c] * ir + c100[c] * fr;
                        float v10 = c010[c] * ir + c110[c] * fr;
                        float v01 = c001[c] * ir + c101[c] * fr;
                        float v11 = c011[c] * ir + c111[c] * fr;
                        float v = (v00 * ig + v10 * fg) * ib + (v01 * ig + v11 * fg) * fb;
                        int iv = (int)(v + 0.5f);
                        if (iv < 0) {
                            iv = 0;
                        }
                        if (iv > 255) {
                            iv = 255;
                        }
                        px[c] = (unsigned char)iv;
                    }
                }
            } else {
                WriteDarkPixel(px, text, bg, paperR, paperG, paperB);
            }
            if (sample) {
                for (int c = 0; c < 3; c++) {
                    int d = px[c] - exact[c];
                    if (d < 0) {
                        d = -d;
                    }
                    if (d > stats->maxDelta) {
                        stats->maxDelta = d;
                    }
                }
                stats->samples++;
            }
        }
    }
}

static FaceBlendSpec FaceBlendSpecForVariant(int variant) {
    FaceBlendSpec spec;
    switch (variant) {
        case 1:
            spec.faceBlend = 0.50f;
            break;
        case 2:
            spec.faceBlend = 0.65f;
            break;
        case 3:
            spec.faceBlend = 0.80f;
            break;
        case 4:
            spec.faceBlend = 1.00f;
            spec.mild = true;
            break;
        case 5:
            spec.faceBlend = 0.60f;
            spec.eyeBlend = 1.00f;
            spec.mouthBlend = 1.00f;
            break;
        case 6:
            spec.faceBlend = 1.00f;
            spec.mild = true;
            break;
        default:
            break;
    }
    return spec;
}

// Face restore is for photographs. Text, stamps, line art, maps, and charts are
// not: they are flat color or hard edges, and running the face model there is
// most of the cost of smart invert.
static bool ImageWantsFaceRestore(const unsigned char* samples, int w, int h, int n, int stride) {
    if (w < 48 || h < 48) {
        return false;
    }
    int stepX = w / 32;
    int stepY = h / 32;
    if (stepX < 2) {
        stepX = 2;
    }
    if (stepY < 2) {
        stepY = 2;
    }
    int count = 0;
    int color = 0;
    int soft = 0;
    for (int y = 0; y + stepY < h; y += stepY) {
        const unsigned char* row = samples + (size_t)y * (size_t)stride;
        const unsigned char* row2 = samples + (size_t)(y + stepY / 2) * (size_t)stride;
        for (int x = 0; x + stepX < w; x += stepX) {
            const unsigned char* a = row + (size_t)x * (size_t)n;
            const unsigned char* b = row2 + (size_t)(x + stepX / 2) * (size_t)n;
            int hi = a[0];
            int lo = a[0];
            if ((int)a[1] > hi) {
                hi = a[1];
            }
            if ((int)a[1] < lo) {
                lo = a[1];
            }
            if ((int)a[2] > hi) {
                hi = a[2];
            }
            if ((int)a[2] < lo) {
                lo = a[2];
            }
            int d = abs((int)a[0] - (int)b[0]) + abs((int)a[1] - (int)b[1]) + abs((int)a[2] - (int)b[2]);
            count++;
            if (hi - lo > 28 && hi > 48) {
                color++;
            }
            if (d > 8 && d < 96) {
                soft++;
            }
        }
    }
    return count > 0 && color * 10 >= count && soft * 5 >= count;
}

static bool TonePerfOn() {
    static int on = -1;
    if (on < 0) {
        on = GetEnvironmentVariableA("SUMATRA_TONE_PERF", nullptr, 0) > 0 ? 1 : 0;
    }
    return on == 1;
}

static bool TonePerfDeltaOn() {
    static int on = -1;
    if (on < 0) {
        on = GetEnvironmentVariableA("SUMATRA_TONE_PERF_DELTA", nullptr, 0) > 0 ? 1 : 0;
    }
    return on == 1;
}

bool PdfDarkModeToneThemeVariant(unsigned char* samples, int w, int h, int n, int stride,
                                 const DarkModePalette& palette, int variant) {
    if (!samples || w < 8 || h < 8 || n < 3 || stride < w * n) {
        return false;
    }
    LARGE_INTEGER toneStart = TimeGet();
    bool perf = TonePerfOn();
    bool measureDelta = perf && TonePerfDeltaOn();
    size_t count = (size_t)w * (size_t)h;
    if (count > 6000000) {
        return false;
    }
    FaceBlendSpec spec = FaceBlendSpecForVariant(variant);
    OklabColor text = SrgbToOklab(palette.textR, palette.textG, palette.textB);
    OklabColor bg = SrgbToOklab(palette.bgR, palette.bgG, palette.bgB);
    float paperR = palette.bgR;
    float paperG = palette.bgG;
    float paperB = palette.bgB;
    double lutBuildMs = 0;
    bool useLut = EnsureToneLut(text, bg, paperR, paperG, paperB, /*diagram=*/variant == 0, &lutBuildMs);
    bool faces = (spec.faceBlend > 0.f || spec.eyeBlend > 0.f || spec.mouthBlend > 0.f) &&
                 ImageWantsFaceRestore(samples, w, h, n, stride);
    ToneApplyStats stats;
    if (!faces) {
        // Soft-mask diagrams use the steep mid→text LUT (built above when variant==0).
        bool diagram = variant == 0;
        if (diagram) {
            gToneSoftMaskDiagram = 1;
        }
        ApplyTonePixels(samples, w, h, n, stride, text, bg, paperR, paperG, paperB, useLut, /*nearest=*/true,
                        measureDelta, &stats);
        if (diagram) {
            gToneSoftMaskDiagram = 0;
        }
        SnapNearPaperPixels(samples, w, h, n, stride, paperR, paperG, paperB);
        if (diagram) {
            CrispSoftMaskDiagramAfterTone(samples, w, h, n, stride, paperR, paperG, paperB, palette.textR,
                                          palette.textG, palette.textB);
        }
        if (perf) {
            logf("tone-grade %dx%d faces=0 lut=%.2f grade=%.2f maxDelta=%d samples=%d ms\n", w, h, lutBuildMs,
                 TimeSinceInMs(toneStart), stats.maxDelta, stats.samples);
        }
        return true;
    }
    u8* orig = (u8*)malloc(count * 3);
    float* weight = (float*)calloc(count, sizeof(float));
    if (!orig || !weight) {
        free(orig);
        free(weight);
        return false;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char* row = samples + (size_t)y * stride;
        u8* dst = orig + (size_t)y * (size_t)w * 3;
        for (int x = 0; x < w; x++) {
            const unsigned char* px = row + (size_t)x * n;
            dst[x * 3] = px[0];
            dst[x * 3 + 1] = px[1];
            dst[x * 3 + 2] = px[2];
        }
    }
    ApplyTonePixels(samples, w, h, n, stride, text, bg, paperR, paperG, paperB, useLut, /*nearest=*/false, measureDelta,
                    &stats);
    SnapNearPaperPixels(samples, w, h, n, stride, paperR, paperG, paperB);
    LARGE_INTEGER faceStart = TimeGet();
    BuildFaceBlendWeight(orig, w, h, w * 3, spec, weight);
    double faceMs = TimeSinceInMs(faceStart);
    for (int y = 0; y < h; y++) {
        unsigned char* row = samples + (size_t)y * stride;
        const u8* oRow = orig + (size_t)y * (size_t)w * 3;
        size_t rowBase = (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            float a = weight[rowBase + x];
            if (a < 0.004f) {
                continue;
            }
            if (a > 1.f) {
                a = 1.f;
            }
            unsigned char* px = row + (size_t)x * n;
            float sr = oRow[x * 3] / 255.f;
            float sg = oRow[x * 3 + 1] / 255.f;
            float sb = oRow[x * 3 + 2] / 255.f;
            if (spec.mild) {
                float mild[3];
                MildFaceRgb(sr, sg, sb, mild);
                sr = mild[0];
                sg = mild[1];
                sb = mild[2];
            }
            float dr = px[0] / 255.f;
            float dg = px[1] / 255.f;
            float db = px[2] / 255.f;
            float r = LinearToSrgb(SrgbToLinear(dr) * (1.f - a) + SrgbToLinear(sr) * a);
            float g = LinearToSrgb(SrgbToLinear(dg) * (1.f - a) + SrgbToLinear(sg) * a);
            float b = LinearToSrgb(SrgbToLinear(db) * (1.f - a) + SrgbToLinear(sb) * a);
            px[0] = (unsigned char)(r * 255.f + 0.5f);
            px[1] = (unsigned char)(g * 255.f + 0.5f);
            px[2] = (unsigned char)(b * 255.f + 0.5f);
        }
    }
    free(orig);
    free(weight);
    if (perf) {
        logf("tone-grade %dx%d faces=1 lut=%.2f face=%.2f total=%.2f maxDelta=%d samples=%d ms\n", w, h, lutBuildMs,
             faceMs, TimeSinceInMs(toneStart), stats.maxDelta, stats.samples);
    }
    return true;
}

bool PdfDarkModeToneThemeRgbSamples(unsigned char* samples, int w, int h, int n, int stride,
                                    const DarkModePalette& palette) {
    // Face stays the photo, a little darker, so it matches the tone grade around it.
    return PdfDarkModeToneThemeVariant(samples, w, h, n, stride, palette, 4);
}

float PdfDarkModeOklabDistance(float r1, float g1, float b1, float r2, float g2, float b2) {
    OklabColor a = SrgbToOklab(r1, g1, b1);
    OklabColor c = SrgbToOklab(r2, g2, b2);
    float dL = a.L - c.L;
    float da = a.a - c.a;
    float db = a.b - c.b;
    return sqrtf(dL * dL + da * da + db * db);
}
