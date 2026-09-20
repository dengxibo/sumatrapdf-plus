/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/WinUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"

#include "DocumentEnhancer.h"

#include <math.h>

// DocumentEnhancer runs on paint-path BGRA after Engine::RenderPage has already
// baked theme / PdfDocumentColorMode recolor into the RenderedBitmap. Dark /
// Dracula pages therefore look dark here. We invert luminance, run the normal
// light-paper Scanned/Reading pipeline, then invert back so gray ink is
// brightened (not crushed) on dark paper.

namespace {

constexpr float kEps = 1e-3f;

static float Clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static int Clampi(int v, int lo, int hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

static BYTE* PixelAt(uint8_t* bgra, int stride, int x, int y) {
    return bgra + y * stride + x * 4;
}

static float PixelY(const BYTE* px) {
    // BGRA
    float b = (float)px[0];
    float g = (float)px[1];
    float r = (float)px[2];
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static float PixelSat(const BYTE* px) {
    float b = (float)px[0];
    float g = (float)px[1];
    float r = (float)px[2];
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx < 1.f) {
        return 0.f;
    }
    return (mx - mn) / mx;
}

struct HistogramStats {
    int hist[256]{};
    int total = 0;
    float mean = 0.f;
    float variance = 0.f;
    float meanSat = 0.f;
    float p1 = 0.f;
    float p5 = 0.f;
    float p50 = 0.f;
    float p90 = 0.f;
    float p95 = 0.f;
    float p99 = 0.f;
    float bright220 = 0.f;
    float bright235 = 0.f;
    float bright245 = 0.f;
};

static float PercentileFromHist(const int* hist, int total, float pct) {
    if (total <= 0) {
        return 0.f;
    }
    int target = (int)((pct / 100.f) * (float)total);
    if (target < 1) {
        target = 1;
    }
    if (target > total) {
        target = total;
    }
    int acc = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc >= target) {
            return (float)i;
        }
    }
    return 255.f;
}

static void BuildHistogram(const float* yBuf, const float* satBuf, int n, HistogramStats* out) {
    ZeroMemory(out, sizeof(*out));
    if (n <= 0) {
        return;
    }
    double sum = 0.0;
    double sumSq = 0.0;
    double sumSat = 0.0;
    int c220 = 0, c235 = 0, c245 = 0;
    for (int i = 0; i < n; i++) {
        int yi = Clampi((int)(yBuf[i] + 0.5f), 0, 255);
        out->hist[yi]++;
        sum += yBuf[i];
        sumSq += (double)yBuf[i] * (double)yBuf[i];
        if (satBuf) {
            sumSat += satBuf[i];
        }
        if (yBuf[i] > 220.f) {
            c220++;
        }
        if (yBuf[i] > 235.f) {
            c235++;
        }
        if (yBuf[i] > 245.f) {
            c245++;
        }
    }
    out->total = n;
    out->mean = (float)(sum / (double)n);
    out->variance = (float)(sumSq / (double)n - (double)out->mean * (double)out->mean);
    if (out->variance < 0.f) {
        out->variance = 0.f;
    }
    out->meanSat = satBuf ? (float)(sumSat / (double)n) : 0.f;
    out->p1 = PercentileFromHist(out->hist, n, 1.f);
    out->p5 = PercentileFromHist(out->hist, n, 5.f);
    out->p50 = PercentileFromHist(out->hist, n, 50.f);
    out->p90 = PercentileFromHist(out->hist, n, 90.f);
    out->p95 = PercentileFromHist(out->hist, n, 95.f);
    out->p99 = PercentileFromHist(out->hist, n, 99.f);
    out->bright220 = (float)c220 / (float)n;
    out->bright235 = (float)c235 / (float)n;
    out->bright245 = (float)c245 / (float)n;
}

// High when the page looks like paper + dark ink (including gray/yellow scans).
static float EstimateDocumentConfidence(const HistogramStats& hs) {
    // Gray/yellow scans rarely exceed Y=220; score paper-like mass near the
    // upper half of the histogram instead of requiring near-white pixels.
    float paperFloor = Clampf(hs.p50 - 25.f, 140.f, 210.f);
    float paperMass = 0.f;
    if (hs.total > 0) {
        int acc = 0;
        int lo = Clampi((int)(paperFloor + 0.5f), 0, 255);
        for (int i = lo; i < 256; i++) {
            acc += hs.hist[i];
        }
        paperMass = (float)acc / (float)hs.total;
    }
    float bright = Clampf(paperMass * 1.15f + hs.bright220 * 0.5f, 0.f, 1.f);
    float medianScore = Clampf((hs.p50 - 100.f) / 110.f, 0.f, 1.f);
    float highlightPeak = Clampf((hs.p95 - 150.f) / 80.f, 0.f, 1.f);
    float lowSat = Clampf(1.f - hs.meanSat * 3.5f, 0.f, 1.f);
    // Photos: high variance + high sat → confidence down. B&W line art alone is OK.
    float photoPenalty =
        Clampf(hs.meanSat * 2.8f + Clampf((hs.variance - 3500.f) / 7000.f, 0.f, 1.f) * hs.meanSat, 0.f, 1.f);
    // Dark / black-bg pages: low median.
    float darkPenalty = Clampf((120.f - hs.p50) / 120.f, 0.f, 1.f);

    float conf = 0.40f * bright + 0.25f * medianScore + 0.15f * highlightPeak + 0.20f * lowSat;
    conf *= (1.f - 0.80f * photoPenalty);
    conf *= (1.f - 0.90f * darkPenalty);
    return Clampf(conf, 0.f, 1.f);
}

// Percentile among histogram bins with Y <= yMax (ink / midtones only).
static float PercentileBelow(const int* hist, float yMax, float pct) {
    int hi = Clampi((int)(yMax + 0.5f), 0, 255);
    int darkTotal = 0;
    for (int i = 0; i <= hi; i++) {
        darkTotal += hist[i];
    }
    if (darkTotal < 64) {
        return PercentileFromHist(hist, darkTotal > 0 ? darkTotal : 1, pct);
    }
    return PercentileFromHist(hist, darkTotal, pct);
}

// Washed photocopy / phone-scan of a book: paper already near white, but ink is
// medium gray (p1 often 140–180). Classic black-point caps (~80) leave text gray.
static bool LooksLikeWashedGrayInk(const HistogramStats& hs) {
    return hs.p50 >= 228.f && hs.bright245 >= 0.45f && hs.p1 >= 70.f && hs.p1 <= 210.f && hs.meanSat < 0.18f;
}

// Aged textbook scans: paper is yellow/sepia (not near-white), ink is gray
// rather than black. Mild mode leaves these looking unchanged.
static bool LooksLikeAgedPaperScan(const HistogramStats& hs) {
    if (hs.p50 < 155.f || hs.p50 > 236.f) {
        return false;
    }
    if (hs.p95 < 180.f || hs.p95 > 250.f) {
        return false;
    }
    if (hs.p5 < 40.f || hs.p5 > 234.f) {
        return false;
    }
    if (hs.meanSat > 0.45f) {
        return false;
    }
    // Born-digital black text is not a faded scan.
    if (hs.p1 < 28.f && hs.p5 < 48.f) {
        return false;
    }
    // Flat digital fills have no scan grain.
    if (hs.variance < 25.f) {
        return false;
    }
    // Color photos: high chroma and high variance. Line art stays.
    if (hs.variance > 5500.f && hs.meanSat > 0.22f) {
        return false;
    }
    return true;
}

static bool PreferStrongEnhancement(const HistogramStats& hs, float conf) {
    // Washed photocopy / phone-scan of a book: white paper, gray ink.
    if (LooksLikeWashedGrayInk(hs)) {
        return true;
    }
    if (LooksLikeAgedPaperScan(hs)) {
        return true;
    }
    // Uneven gray paper that still looks like a document.
    if (conf >= 0.40f && hs.p50 < 225.f && hs.p95 < 252.f && hs.meanSat < 0.20f) {
        return true;
    }
    // Soft gray ink even when paper is not fully white.
    if (conf >= 0.35f && hs.p5 > 100.f && hs.p5 < 210.f && hs.bright245 > 0.25f) {
        return true;
    }
    return false;
}

static void EstimateBlackWhitePoints(const HistogramStats& hs, float conf, float blackStrength, float whiteStrength,
                                     bool scannedMode, float* outBlack, float* outWhite) {
    bool washed = scannedMode && LooksLikeWashedGrayInk(hs);

    float blackCand;
    float whiteCand = Lerp(hs.p95, hs.p99, 0.35f);
    if (whiteStrength > 0.6f) {
        whiteCand = Lerp(hs.p90, hs.p95, 0.55f);
    }

    float bMax;
    if (washed) {
        // ScanTailor / k2pdfopt-style: anchor black near the ink mass, not the
        // global floor. p10 of pixels below paper pulls gray strokes down.
        float inkP10 = PercentileBelow(hs.hist, 238.f, 10.f);
        float inkP25 = PercentileBelow(hs.hist, 238.f, 25.f);
        blackCand = Lerp(inkP10, inkP25, 0.35f) * 0.94f;
        bMax = Lerp(110.f, 185.f, blackStrength * Clampf(0.55f + 0.45f * conf, 0.f, 1.f));
        whiteCand = Clampf(Lerp(hs.p95, 252.f, 0.65f), 245.f, 255.f);
    } else {
        blackCand = Lerp(hs.p1, hs.p5, 0.45f);
        // After background norm, paper sits near ~200; use a lower white anchor so
        // tone mapping can finish the push to screen-white.
        bMax = Lerp(35.f, 100.f, blackStrength * Clampf(0.45f + 0.55f * conf, 0.f, 1.f));
        if (scannedMode) {
            bMax = Lerp(bMax, 130.f, Clampf(blackStrength, 0.f, 1.f) * 0.55f);
        }
    }

    float wMin = Lerp(230.f, 165.f, whiteStrength * Clampf(0.45f + 0.55f * conf, 0.f, 1.f));
    if (washed) {
        wMin = 242.f;
    }
    blackCand = Clampf(blackCand, 0.f, bMax);
    whiteCand = Clampf(whiteCand, wMin, 255.f);

    if (whiteCand - blackCand < 40.f) {
        float mid = 0.5f * (whiteCand + blackCand);
        blackCand = mid - 20.f;
        whiteCand = mid + 20.f;
        blackCand = Clampf(blackCand, 0.f, washed ? 160.f : 80.f);
        whiteCand = Clampf(whiteCand, washed ? 230.f : 180.f, 255.f);
    }

    // Blend toward identity when confidence is low — but keep a usable floor
    // so gray scans still get a visible paper push.
    float idBlack = 0.f;
    float idWhite = 255.f;
    float strength = Clampf(0.40f + 0.60f * conf, 0.f, 1.f);
    strength *= Clampf(0.45f + 0.55f * whiteStrength, 0.f, 1.f);
    if (washed) {
        // Washed pages are already high-confidence paper; do not dilute the stretch.
        strength = Clampf(0.88f + 0.12f * conf, 0.f, 1.f);
    }
    *outBlack = Lerp(idBlack, blackCand, strength * Clampf(0.5f + blackStrength, 0.f, 1.2f));
    *outWhite = Lerp(idWhite, whiteCand, strength);
}

static float SoftSCurve(float t, float strength) {
    // Gentle contrast around midtones. strength 0 = identity.
    if (strength <= 0.001f) {
        return t;
    }
    float x = Clampf(t, 0.f, 1.f);
    float centered = (x - 0.5f) * (1.f + strength * 0.85f) + 0.5f;
    centered = Clampf(centered, 0.f, 1.f);
    float shaped = centered * centered * (3.f - 2.f * centered);
    return Lerp(x, shaped, Clampf(strength, 0.f, 1.f) * 0.65f);
}

static void BuildToneLut(float blackPoint, float whitePoint, float gamma, float sCurve, uint8_t lut[256]) {
    float denom = whitePoint - blackPoint;
    if (denom < 1.f) {
        denom = 1.f;
    }
    float invGamma = 1.f / Clampf(gamma, 0.35f, 2.5f);
    for (int i = 0; i < 256; i++) {
        float t = ((float)i - blackPoint) / denom;
        t = Clampf(t, 0.f, 1.f);
        t = SoftSCurve(t, sCurve);
        // gamma < 1 darkens midtones (ink looks heavier).
        t = powf(t, invGamma);
        int v = (int)(t * 255.f + 0.5f);
        lut[i] = (BYTE)Clampi(v, 0, 255);
    }
}

static int ChooseTileSize(int width, int height) {
    int m = width < height ? width : height;
    // Finer tiles so book-spine illumination can vary across the page.
    int t = m / 72;
    if (t < 6) {
        t = 6;
    }
    if (t > 16) {
        t = 16;
    }
    return t;
}

struct BackgroundMap {
    int mapW = 0;
    int mapH = 0;
    int tile = 0;
    float* values = nullptr;  // mapW * mapH, paper luminance estimate
    float* protect = nullptr; // 0..1 image protection per tile
};

static void FreeBackgroundMap(BackgroundMap* m) {
    free(m->values);
    free(m->protect);
    m->values = nullptr;
    m->protect = nullptr;
    m->mapW = m->mapH = m->tile = 0;
}

static void FillMapHoles(float* map, uint8_t* valid, int mw, int mh, int rounds) {
    float* tmp = (float*)malloc((size_t)mw * (size_t)mh * sizeof(float));
    uint8_t* validTmp = (uint8_t*)malloc((size_t)mw * (size_t)mh);
    if (!tmp || !validTmp) {
        free(tmp);
        free(validTmp);
        return;
    }
    for (int r = 0; r < rounds; r++) {
        memcpy(tmp, map, (size_t)mw * (size_t)mh * sizeof(float));
        memcpy(validTmp, valid, (size_t)mw * (size_t)mh);
        bool any = false;
        for (int y = 0; y < mh; y++) {
            for (int x = 0; x < mw; x++) {
                int idx = y * mw + x;
                if (valid[idx]) {
                    continue;
                }
                float sum = 0.f;
                int cnt = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        int nx = x + dx;
                        int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) {
                            continue;
                        }
                        int nidx = ny * mw + nx;
                        if (valid[nidx]) {
                            sum += map[nidx];
                            cnt++;
                        }
                    }
                }
                if (cnt > 0) {
                    tmp[idx] = sum / (float)cnt;
                    validTmp[idx] = 1;
                    any = true;
                }
            }
        }
        memcpy(map, tmp, (size_t)mw * (size_t)mh * sizeof(float));
        memcpy(valid, validTmp, (size_t)mw * (size_t)mh);
        if (!any) {
            break;
        }
    }
    // Any remaining holes: fill with global median of valid cells.
    float fallback = 200.f;
    double sum = 0.0;
    int cnt = 0;
    for (int i = 0; i < mw * mh; i++) {
        if (valid[i]) {
            sum += map[i];
            cnt++;
        }
    }
    if (cnt > 0) {
        fallback = (float)(sum / (double)cnt);
    }
    for (int i = 0; i < mw * mh; i++) {
        if (!valid[i]) {
            map[i] = fallback;
            valid[i] = 1;
        }
    }
    free(tmp);
    free(validTmp);
}

static void SmoothMap(float* map, int mw, int mh) {
    float* tmp = (float*)malloc((size_t)mw * (size_t)mh * sizeof(float));
    if (!tmp) {
        return;
    }
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < mw; x++) {
            float sum = 0.f;
            int cnt = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) {
                        continue;
                    }
                    sum += map[ny * mw + nx];
                    cnt++;
                }
            }
            tmp[y * mw + x] = sum / (float)cnt;
        }
    }
    memcpy(map, tmp, (size_t)mw * (size_t)mh * sizeof(float));
    free(tmp);
}

static bool EstimateBackgroundMap(const float* yBuf, const float* satBuf, int width, int height, float paperHint,
                                  float imageProtection, bool highPercentile, BackgroundMap* out) {
    FreeBackgroundMap(out);
    int tile = ChooseTileSize(width, height);
    int mapW = (width + tile - 1) / tile;
    int mapH = (height + tile - 1) / tile;
    float* values = (float*)malloc((size_t)mapW * (size_t)mapH * sizeof(float));
    float* protect = (float*)malloc((size_t)mapW * (size_t)mapH * sizeof(float));
    uint8_t* valid = (uint8_t*)malloc((size_t)mapW * (size_t)mapH);
    if (!values || !protect || !valid) {
        free(values);
        free(protect);
        free(valid);
        return false;
    }

    // Ink-only exclusion. Book-spine / page-edge shadows are illumination and
    // MUST stay in the background map — if they are treated as foreground,
    // holes fill with bright-paper neighbors and multiplicative correction
    // cannot lift the shadow (Y * target/B_bright ≈ Y).
    float fgThresh = paperHint - 100.f;
    if (fgThresh < 45.f) {
        fgThresh = 45.f;
    }
    if (fgThresh > 95.f) {
        fgThresh = 95.f;
    }

    const float minBgRatio = 0.25f;
    // Per-tile sample buffer for percentile (tile ≤ 16×16).
    float samples[16 * 16];

    for (int ty = 0; ty < mapH; ty++) {
        for (int tx = 0; tx < mapW; tx++) {
            int x0 = tx * tile;
            int y0 = ty * tile;
            int x1 = x0 + tile;
            int y1 = y0 + tile;
            if (x1 > width) {
                x1 = width;
            }
            if (y1 > height) {
                y1 = height;
            }

            double sumBg = 0.0;
            double sumAll = 0.0;
            double sumAll2 = 0.0;
            double sumSat = 0.0;
            int bgCount = 0;
            int total = 0;
            int nSamples = 0;

            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    int pidx = y * width + x;
                    float yv = yBuf[pidx];
                    float sat = satBuf ? satBuf[pidx] : 0.f;
                    total++;
                    sumAll += yv;
                    sumAll2 += (double)yv * (double)yv;
                    sumSat += sat;
                    // Foreground = dark ink only. Keep shaded paper in the map.
                    bool isFg = (yv < fgThresh);
                    if (!isFg) {
                        sumBg += yv;
                        bgCount++;
                        if (nSamples < (int)dimof(samples)) {
                            samples[nSamples++] = yv;
                        }
                    }
                }
            }

            int idx = ty * mapW + tx;
            double meanAll = total > 0 ? sumAll / (double)total : paperHint;
            double var = total > 0 ? (sumAll2 / (double)total - meanAll * meanAll) : 0.0;
            if (var < 0.0) {
                var = 0.0;
            }
            float meanSat = total > 0 ? (float)(sumSat / (double)total) : 0.f;
            float prot = 0.f;
            if (imageProtection > 0.01f) {
                // Require color; B&W line drawings must not disable whitening.
                float satPart = Clampf((meanSat - 0.18f) / 0.30f, 0.f, 1.f);
                float varPart = Clampf(((float)var - 1200.f) / 4500.f, 0.f, 1.f);
                prot = Clampf(satPart * (0.55f + 0.45f * varPart), 0.f, 1.f) * imageProtection;
            }
            protect[idx] = prot;

            float ratio = total > 0 ? (float)bgCount / (float)total : 0.f;
            if (ratio >= minBgRatio && bgCount >= 6) {
                float est = (float)(sumBg / (double)bgCount);
                // High percentile ≈ local paper (ScanTailor morph-open idea on a
                // budget). Text-dense tiles no longer pull the estimate down.
                if (highPercentile && nSamples >= 6) {
                    int hist64[64]{};
                    for (int i = 0; i < nSamples; i++) {
                        int b = Clampi((int)(samples[i] * (63.f / 255.f) + 0.5f), 0, 63);
                        hist64[b]++;
                    }
                    int target = (int)(0.88f * (float)nSamples);
                    if (target < 1) {
                        target = 1;
                    }
                    int acc = 0;
                    est = samples[0];
                    for (int b = 0; b < 64; b++) {
                        acc += hist64[b];
                        if (acc >= target) {
                            est = ((float)b + 0.5f) * (255.f / 63.f);
                            break;
                        }
                    }
                }
                values[idx] = est;
                valid[idx] = 1;
            } else {
                values[idx] = paperHint;
                valid[idx] = 0;
            }
        }
    }

    // Only strongly colorful tiles become holes (photos).
    for (int y = 0; y < mapH; y++) {
        for (int x = 0; x < mapW; x++) {
            int idx = y * mapW + x;
            if (protect[idx] > 0.65f) {
                valid[idx] = 0;
            }
        }
    }

    FillMapHoles(values, valid, mapW, mapH, 16);
    SmoothMap(values, mapW, mapH);
    SmoothMap(values, mapW, mapH);
    SmoothMap(values, mapW, mapH);

    free(valid);
    out->mapW = mapW;
    out->mapH = mapH;
    out->tile = tile;
    out->values = values;
    out->protect = protect;
    return true;
}

static float SampleMapBilinear(const BackgroundMap& m, float px, float py) {
    // px,py in pixel coordinates of full image.
    float gx = (px + 0.5f) / (float)m.tile - 0.5f;
    float gy = (py + 0.5f) / (float)m.tile - 0.5f;
    int x0 = (int)floorf(gx);
    int y0 = (int)floorf(gy);
    float tx = gx - (float)x0;
    float ty = gy - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = Clampi(x0, 0, m.mapW - 1);
    x1 = Clampi(x1, 0, m.mapW - 1);
    y0 = Clampi(y0, 0, m.mapH - 1);
    y1 = Clampi(y1, 0, m.mapH - 1);
    float v00 = m.values[y0 * m.mapW + x0];
    float v10 = m.values[y0 * m.mapW + x1];
    float v01 = m.values[y1 * m.mapW + x0];
    float v11 = m.values[y1 * m.mapW + x1];
    float v0 = Lerp(v00, v10, tx);
    float v1 = Lerp(v01, v11, tx);
    return Lerp(v0, v1, ty);
}

static float SampleProtectNearest(const BackgroundMap& m, int x, int y) {
    int tx = Clampi(x / m.tile, 0, m.mapW - 1);
    int ty = Clampi(y / m.tile, 0, m.mapH - 1);
    return m.protect[ty * m.mapW + tx];
}

static void ApplyBackgroundNormalization(float* yBuf, int width, int height, const BackgroundMap& map, float strength,
                                         float target, float conf, bool scannedMode) {
    if (strength <= 0.001f || !map.values) {
        return;
    }
    // Floor conf so gray scans are not nearly skipped.
    float s = strength * Clampf(0.55f + 0.45f * conf, 0.f, 1.f);
    float maxFactor = Lerp(1.35f, scannedMode ? 3.10f : 2.60f, s);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            float B = SampleMapBilinear(map, (float)x, (float)y);
            if (B < 12.f) {
                B = 12.f;
            }
            // Never darken paper already at/above the internal target.
            // target/B < 1 on clean white PDFs was turning pages gray under Reading.
            float factor = target / B;
            if (factor < 1.f) {
                factor = 1.f;
            }
            factor = Clampf(factor, 1.f, maxFactor);
            float prot = SampleProtectNearest(map, x, y);
            float localS = s * (1.f - prot);
            float Y = yBuf[idx];
            float Yn = Y * factor;
            // Keep true ink from being pulled up with shaded paper.
            if (Y < 55.f) {
                localS *= Clampf(Y / 55.f, 0.15f, 1.f);
            }
            yBuf[idx] = Lerp(Y, Yn, localS);
        }
    }
}

// Push near-paper pixels the rest of the way to screen white (k2pdfopt-style).
static void SoftPushPaperWhite(float* yBuf, int n, float thresh, float amount) {
    if (amount <= 0.001f || thresh >= 254.f) {
        return;
    }
    float span = 255.f - thresh;
    if (span < 1.f) {
        span = 1.f;
    }
    for (int i = 0; i < n; i++) {
        float Y = yBuf[i];
        if (Y <= thresh) {
            continue;
        }
        float t = (Y - thresh) / span;
        yBuf[i] = Lerp(Y, 255.f, amount * t);
    }
}

// Soft ink punch after tone (ScanTailor RaiseAboveBackground idea, without hard
// binarization). Darkens residual gray strokes on already-white paper while
// leaving true paper alone. Protects colorful tiles when a map is available.
static void ApplyGrayInkPunch(float* yBuf, int width, int height, float amount, float paperRef,
                              const BackgroundMap* protectMap) {
    if (amount <= 0.01f || paperRef < 180.f) {
        return;
    }
    float paper = Clampf(paperRef, 220.f, 252.f);
    float power = 1.f + amount * 1.15f;
    float mix = Clampf(amount * 0.92f, 0.f, 0.95f);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            float Y = yBuf[idx];
            if (Y >= paper - 0.5f) {
                continue;
            }
            float localMix = mix;
            if (protectMap && protectMap->protect) {
                float prot = SampleProtectNearest(*protectMap, x, y);
                localMix *= (1.f - 0.90f * prot);
            }
            if (localMix <= 0.01f) {
                continue;
            }
            float t = Clampf(Y / paper, 0.f, 1.f);
            float tp = powf(t, power);
            // Keep deepest ink from crushing to pure 0 (preserves thin formula strokes).
            float Yn = tp * paper;
            if (Yn < 8.f) {
                Yn = 8.f + Yn * 0.35f;
            }
            yBuf[idx] = Lerp(Y, Yn, localMix);
        }
    }
}

static void ApplyToneToY(float* yBuf, int n, const uint8_t lut[256]) {
    for (int i = 0; i < n; i++) {
        int yi = Clampi((int)(yBuf[i] + 0.5f), 0, 255);
        yBuf[i] = (float)lut[yi];
    }
}

// Separable box blur via prefix sums (O(width*height), not O(r)).
static void BoxBlurSeparable(const float* src, float* dst, float* tmp, int width, int height, int radius) {
    if (radius < 1) {
        memcpy(dst, src, (size_t)width * (size_t)height * sizeof(float));
        return;
    }
    int prefN = width > height ? width : height;
    float* prefix = (float*)malloc(((size_t)prefN + 1) * sizeof(float));
    if (!prefix) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                float sum = 0.f;
                int x0 = x - radius;
                int x1 = x + radius;
                if (x0 < 0) {
                    x0 = 0;
                }
                if (x1 >= width) {
                    x1 = width - 1;
                }
                for (int xi = x0; xi <= x1; xi++) {
                    sum += src[y * width + xi];
                }
                tmp[y * width + x] = sum / (float)(x1 - x0 + 1);
            }
        }
        for (int y = 0; y < height; y++) {
            int y0 = y - radius;
            int y1 = y + radius;
            if (y0 < 0) {
                y0 = 0;
            }
            if (y1 >= height) {
                y1 = height - 1;
            }
            for (int x = 0; x < width; x++) {
                float sum = 0.f;
                for (int yi = y0; yi <= y1; yi++) {
                    sum += tmp[yi * width + x];
                }
                dst[y * width + x] = sum / (float)(y1 - y0 + 1);
            }
        }
        return;
    }
    for (int y = 0; y < height; y++) {
        const float* row = src + y * width;
        float* out = tmp + y * width;
        prefix[0] = 0.f;
        for (int x = 0; x < width; x++) {
            prefix[x + 1] = prefix[x] + row[x];
        }
        for (int x = 0; x < width; x++) {
            int x0 = x - radius;
            int x1 = x + radius;
            if (x0 < 0) {
                x0 = 0;
            }
            if (x1 >= width) {
                x1 = width - 1;
            }
            out[x] = (prefix[x1 + 1] - prefix[x0]) / (float)(x1 - x0 + 1);
        }
    }
    for (int x = 0; x < width; x++) {
        prefix[0] = 0.f;
        for (int y = 0; y < height; y++) {
            prefix[y + 1] = prefix[y] + tmp[y * width + x];
        }
        for (int y = 0; y < height; y++) {
            int y0 = y - radius;
            int y1 = y + radius;
            if (y0 < 0) {
                y0 = 0;
            }
            if (y1 >= height) {
                y1 = height - 1;
            }
            dst[y * width + x] = (prefix[y1 + 1] - prefix[y0]) / (float)(y1 - y0 + 1);
        }
    }
    free(prefix);
}

// Mild paper-grain denoise (unpaper grayfilter-inspired): only blend near-paper
// pixels toward a tiny blur when the residual looks like texture, not ink edge.
static void MildPaperDenoise(float* yBuf, int width, int height, float paperFloor, float amount) {
    if (amount <= 0.01f || width < 3 || height < 3) {
        return;
    }
    int n = width * height;
    float* blur = (float*)malloc((size_t)n * sizeof(float));
    float* tmp = (float*)malloc((size_t)n * sizeof(float));
    if (!blur || !tmp) {
        free(blur);
        free(tmp);
        return;
    }
    BoxBlurSeparable(yBuf, blur, tmp, width, height, 1);
    float thr = 6.5f;
    for (int i = 0; i < n; i++) {
        float Y = yBuf[i];
        if (Y < paperFloor) {
            continue;
        }
        float d = fabsf(Y - blur[i]);
        if (d >= thr) {
            continue;
        }
        float w = (1.f - d / thr) * amount;
        yBuf[i] = Lerp(Y, blur[i], w);
    }
    free(blur);
    free(tmp);
}

// Luminance-only thresholded unsharp. Soft gate + edge fade to limit black/white halo.
static void ApplyLuminanceSharpen(float* yBuf, int width, int height, float amount, float radiusPx, float threshold,
                                  const BackgroundMap* protectMap) {
    if (amount <= 0.001f || width < 3 || height < 3) {
        return;
    }
    int n = width * height;
    float* blur = (float*)malloc((size_t)n * sizeof(float));
    float* tmp = (float*)malloc((size_t)n * sizeof(float));
    if (!blur || !tmp) {
        free(blur);
        free(tmp);
        return;
    }

    int r = (int)(radiusPx + 0.35f);
    if (r < 1) {
        r = 1;
    }
    if (r > 2) {
        r = 2; // keep screen-pixel scale; avoid large halo
    }
    BoxBlurSeparable(yBuf, blur, tmp, width, height, r);

    float thr = Clampf(threshold, 1.5f, 24.f);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = y * width + x;
            float Y = yBuf[idx];
            float detail = Y - blur[idx];
            float ad = fabsf(detail);
            // Soft threshold: ignore scan noise / paper grain.
            float w = 0.f;
            if (ad > thr) {
                w = Clampf((ad - thr) / (thr * 1.25f + 1.f), 0.f, 1.f);
                w = w * w * (3.f - 2.f * w);
            }
            if (w <= 0.001f) {
                continue;
            }
            float localAmt = amount;
            if (protectMap && protectMap->protect) {
                float prot = SampleProtectNearest(*protectMap, x, y);
                localAmt *= (1.f - 0.85f * prot);
            }
            // Fade near black/white to reduce ring halo on strokes and table lines.
            float headroom = Y < 255.f - Y ? Y : 255.f - Y;
            float edgeFade = Clampf(headroom / 16.f, 0.f, 1.f);
            float delta = localAmt * w * edgeFade * detail;
            // Cap overshoot so a 1px line cannot grow a bright halo.
            float maxDelta = 12.f + localAmt * 10.f;
            delta = Clampf(delta, -maxDelta, maxDelta);
            yBuf[idx] = Clampf(Y + delta, 0.f, 255.f);
        }
    }
    free(blur);
    free(tmp);
}

static void RecombineColor(uint8_t* bgra, int width, int height, int stride, const float* yOld, const float* yNew,
                           bool preserveColor, bool whitenPaper) {
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            BYTE* px = PixelAt(bgra, stride, x, y);
            int idx = y * width + x;
            float yo = yOld[idx];
            float yn = yNew[idx];
            if (!preserveColor) {
                BYTE v = (BYTE)Clampi((int)(yn + 0.5f), 0, 255);
                px[0] = v;
                px[1] = v;
                px[2] = v;
                continue;
            }
            float scale = yn / (yo > kEps ? yo : kEps);
            // Limit chroma explosion on dark / colorful pixels.
            scale = Clampf(scale, 0.20f, 3.5f);
            float b = (float)px[0] * scale;
            float g = (float)px[1] * scale;
            float r = (float)px[2] * scale;
            // Soft shoulder near 255 to reduce clipping hue shifts.
            auto softClip = [](float v) -> float {
                if (v <= 255.f) {
                    return v < 0.f ? 0.f : v;
                }
                return 255.f;
            };
            // Yellowed scan paper stays beige if we only scale luminance.
            // Pull near-white, low-chroma pixels toward neutral. Saturated marks stay.
            if (whitenPaper && yn >= 205.f && yo >= 155.f) {
                float maxc = b > g ? b : g;
                if (r > maxc) {
                    maxc = r;
                }
                float minc = b < g ? b : g;
                if (r < minc) {
                    minc = r;
                }
                if (maxc - minc < 96.f) {
                    float t = Clampf((yn - 205.f) / 40.f, 0.f, 1.f);
                    b = Lerp(b, yn, t);
                    g = Lerp(g, yn, t);
                    r = Lerp(r, yn, t);
                }
            }
            px[0] = (BYTE)Clampi((int)(softClip(b) + 0.5f), 0, 255);
            px[1] = (BYTE)Clampi((int)(softClip(g) + 0.5f), 0, 255);
            px[2] = (BYTE)Clampi((int)(softClip(r) + 0.5f), 0, 255);
        }
    }
}

static void MaybeDumpGray(const char* dir, const char* name, const float* yBuf, int width, int height) {
    if (!dir || !dir[0] || !yBuf) {
        return;
    }
    TempStr path = str::FormatTemp("%s\\%s.pgm", dir, name);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    char header[64];
    int hdrLen = snprintf(header, sizeof(header), "P5\n%d %d\n255\n", width, height);
    DWORD written = 0;
    WriteFile(h, header, hdrLen, &written, nullptr);
    uint8_t* row = (uint8_t*)malloc((size_t)width);
    if (row) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                row[x] = (uint8_t)Clampi((int)(yBuf[y * width + x] + 0.5f), 0, 255);
            }
            WriteFile(h, row, width, &written, nullptr);
        }
        free(row);
    }
    CloseHandle(h);
}

static void MaybeDumpStats(const char* dir, const DocumentEnhancementStats& st, const DocumentEnhancementParams& p) {
    if (!dir || !dir[0]) {
        return;
    }
    TempStr path = str::FormatTemp("%s\\stats.txt", dir);
    FILE* f = fopen(path, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "mode=%d brightness=%.1f contrast=%.1f sharpness=%.1f\n", (int)p.mode, p.brightness, p.contrast,
            p.sharpness);
    fprintf(f, "mean=%.2f median=%.2f p1=%.1f p5=%.1f p95=%.1f p99=%.1f\n", st.mean, st.median, st.p1, st.p5, st.p95,
            st.p99);
    fprintf(f, "bright220=%.3f bright235=%.3f bright245=%.3f var=%.1f sat=%.3f\n", st.bright220, st.bright235,
            st.bright245, st.variance, st.meanSaturation);
    fprintf(f, "documentConfidence=%.3f black=%.1f white=%.1f gamma=%.3f bgStrength=%.3f\n", st.documentConfidence,
            st.blackPoint, st.whitePoint, st.gamma, st.backgroundStrengthUsed);
    fprintf(f, "msTotal=%.2f msHist=%.2f msBg=%.2f msTone=%.2f msSharpen=%.2f\n", st.msTotal, st.msHistogram,
            st.msBackground, st.msTone, st.msSharpen);
    fclose(f);
}

} // namespace

static bool gDocumentEnhancerPreviewLite = false;

void SetDocumentEnhancerPreviewLite(bool lite) {
    gDocumentEnhancerPreviewLite = lite;
}

bool DocumentEnhancerPreviewLite() {
    return gDocumentEnhancerPreviewLite;
}

DocumentEnhancementParams GetReadingPreset() {
    DocumentEnhancementParams p;
    p.mode = DocumentEnhancementMode::Reading;
    p.backgroundStrength = 0.55f;
    p.backgroundTarget = 200.f;
    p.blackPointStrength = 0.40f;
    p.whitePointStrength = 0.70f;
    p.gamma = 0.88f;
    p.sCurveStrength = 0.22f;
    p.sharpenRadius = 0.8f;
    p.sharpenAmount = 0.22f;
    p.sharpenThreshold = 5.f;
    p.imageProtection = 0.85f;
    p.preserveColor = true;
    return p;
}

DocumentEnhancementParams GetScannedPreset() {
    DocumentEnhancementParams p;
    p.mode = DocumentEnhancementMode::Scanned;
    p.backgroundStrength = 1.05f;
    p.backgroundTarget = 248.f;
    p.blackPointStrength = 0.95f;
    p.whitePointStrength = 1.15f;
    p.gamma = 0.70f;
    p.sCurveStrength = 0.52f;
    p.sharpenRadius = 1.0f;
    p.sharpenAmount = 0.52f;
    p.sharpenThreshold = 3.5f;
    p.imageProtection = 0.55f;
    p.preserveColor = true;
    return p;
}

DocumentEnhancementParams BuildEnhancementParams(DocumentEnhancementMode mode, int brightness, int contrast,
                                                 int sharpness) {
    DocumentEnhancementParams p;
    if (mode == DocumentEnhancementMode::Reading) {
        p = GetReadingPreset();
    } else if (mode == DocumentEnhancementMode::Scanned) {
        p = GetScannedPreset();
    } else if (mode == DocumentEnhancementMode::Auto) {
        // Strength is chosen inside ApplyDocumentEnhancement after histogram.
        p.mode = DocumentEnhancementMode::Auto;
        p.brightness = (float)brightness;
        p.contrast = (float)contrast;
        p.sharpness = (float)sharpness;
        return p;
    } else {
        p.mode = mode;
        return p;
    }

    float b = Clampf((float)brightness, -100.f, 100.f) / 100.f;
    float c = Clampf((float)contrast, -100.f, 100.f) / 100.f;
    float s = Clampf((float)sharpness, 0.f, 100.f) / 100.f;
    p.brightness = (float)brightness;
    p.contrast = (float)contrast;
    p.sharpness = (float)sharpness;

    // Brightness: mainly gamma + white point. Positive lifts paper/midtones.
    p.gamma = Clampf(p.gamma - b * 0.22f, 0.50f, 1.35f);
    p.whitePointStrength = Clampf(p.whitePointStrength + b * 0.30f, 0.f, 1.35f);
    p.backgroundStrength = Clampf(p.backgroundStrength + b * 0.15f, 0.f, 1.25f);

    // Contrast: black/white expansion + S curve.
    p.blackPointStrength = Clampf(p.blackPointStrength + c * 0.40f, 0.f, 1.35f);
    p.whitePointStrength = Clampf(p.whitePointStrength + c * 0.25f, 0.f, 1.35f);
    p.sCurveStrength = Clampf(p.sCurveStrength + c * 0.45f, 0.f, 1.35f);
    p.gamma = Clampf(p.gamma - c * 0.08f, 0.50f, 1.35f);
    p.backgroundStrength = Clampf(p.backgroundStrength + c * 0.10f, 0.f, 1.25f);

    // Sharpness UI: 0 = preset baseline amount; positive raises amount and
    // slightly lowers the noise gate / widens radius.
    p.sharpenAmount = Clampf(p.sharpenAmount + s * 0.65f, 0.f, 1.6f);
    p.sharpenRadius = Clampf(p.sharpenRadius + s * 0.35f, 0.5f, 1.4f);
    p.sharpenThreshold = Clampf(p.sharpenThreshold - s * 1.5f, 2.5f, 12.f);
    return p;
}

bool TryParseDocumentEnhancerEnvOverride(DocumentEnhancementMode* outMode) {
    if (!outMode) {
        return false;
    }
    TempStr v = GetEnvVariableTemp("SUMATRA_DOC_ENHANCER");
    if (!v || !v[0]) {
        return false;
    }
    if (str::EqI(v, "off") || str::EqI(v, "0") || str::EqI(v, "none")) {
        *outMode = DocumentEnhancementMode::Off;
        return true;
    }
    if (str::EqI(v, "legacy") || str::EqI(v, "1")) {
        *outMode = DocumentEnhancementMode::Legacy;
        return true;
    }
    if (str::EqI(v, "reading") || str::EqI(v, "2") || str::EqI(v, "read")) {
        *outMode = DocumentEnhancementMode::Reading;
        return true;
    }
    if (str::EqI(v, "scanned") || str::EqI(v, "scan") || str::EqI(v, "3")) {
        *outMode = DocumentEnhancementMode::Scanned;
        return true;
    }
    if (str::EqI(v, "auto") || str::EqI(v, "4")) {
        *outMode = DocumentEnhancementMode::Auto;
        return true;
    }
    return false;
}

bool DocumentEnhancerEnvForceDisablePaperNormalize() {
    TempStr v = GetEnvVariableTemp("SUMATRA_DOC_ENHANCER_NO_BG");
    return v && v[0] && !str::EqI(v, "0") && !str::EqI(v, "false");
}

void ApplyDocumentEnhancement(uint8_t* bgra, int width, int height, int stride, const DocumentEnhancementParams& params,
                              DocumentEnhancementStats* outStats) {
    DocumentEnhancementStats local{};
    DocumentEnhancementStats* st = outStats ? outStats : &local;
    *st = {};

    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) {
        return;
    }
    if (params.mode != DocumentEnhancementMode::Reading && params.mode != DocumentEnhancementMode::Scanned &&
        params.mode != DocumentEnhancementMode::Auto) {
        return;
    }

    LARGE_INTEGER freq, t0, t1, tHist0, tHist1, tBg0, tBg1, tTone0, tTone1, tSh0, tSh1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    const int n = width * height;
    float* yBuf = (float*)malloc((size_t)n * sizeof(float));
    float* ySrc = (float*)malloc((size_t)n * sizeof(float));
    float* satBuf = (float*)malloc((size_t)n * sizeof(float));
    if (!yBuf || !ySrc || !satBuf) {
        free(yBuf);
        free(ySrc);
        free(satBuf);
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            BYTE* px = PixelAt(bgra, stride, x, y);
            int idx = y * width + x;
            yBuf[idx] = PixelY(px);
            ySrc[idx] = yBuf[idx];
            satBuf[idx] = PixelSat(px);
        }
    }

    // Dark-theme pages arrive already recolored (dark paper, light ink). The
    // Scanned/Reading pipeline is built for light paper + dark ink — running it
    // as-is darkens the (already light-gray) text into mud. Invert luminance,
    // enhance as a normal scan, then invert back before recombine.
    const bool darkPage = params.disablePaperNormalize || DocumentEnhancerEnvForceDisablePaperNormalize();
    if (darkPage) {
        for (int i = 0; i < n; i++) {
            yBuf[i] = 255.f - yBuf[i];
        }
    }

    TempStr dumpDir = GetEnvVariableTemp("SUMATRA_DUMP_DOCUMENT_ENHANCER");
    MaybeDumpGray(dumpDir, "02_luminance", yBuf, width, height);

    QueryPerformanceCounter(&tHist0);
    HistogramStats hs{};
    BuildHistogram(yBuf, satBuf, n, &hs);
    float conf = EstimateDocumentConfidence(hs);
    QueryPerformanceCounter(&tHist1);

    st->mean = hs.mean;
    st->median = hs.p50;
    st->p1 = hs.p1;
    st->p5 = hs.p5;
    st->p95 = hs.p95;
    st->p99 = hs.p99;
    st->bright220 = hs.bright220;
    st->bright235 = hs.bright235;
    st->bright245 = hs.bright245;
    st->variance = hs.variance;
    st->meanSaturation = hs.meanSat;
    st->documentConfidence = conf;

    // Auto: pick Mild (Reading) or Strong (Scanned) from tile stats, then run
    // that preset's knobs (UI no longer exposes the two modes separately).
    DocumentEnhancementParams work = params;
    if (params.mode == DocumentEnhancementMode::Auto) {
        bool strong = PreferStrongEnhancement(hs, conf);
        int b = (int)params.brightness;
        int c = (int)params.contrast;
        int s = (int)params.sharpness;
        work = BuildEnhancementParams(strong ? DocumentEnhancementMode::Scanned : DocumentEnhancementMode::Reading, b,
                                      c, s);
        work.disablePaperNormalize = params.disablePaperNormalize;
    }

    // Clean vector / already-white pages: skip heavy work so Mild does not
    // gray the paper or burn CPU on every paint. Strong washed photocopies are
    // also "white paper" but still need gray-ink punch — do not early-out.
    bool alreadyClean = (hs.p50 >= 232.f && hs.bright220 >= 0.50f && hs.meanSat < 0.10f);
    bool washedInk = LooksLikeWashedGrayInk(hs);
    bool agedPaper = LooksLikeAgedPaperScan(hs);
    bool baselineOnly = (fabsf(work.brightness) < 0.5f && fabsf(work.contrast) < 0.5f);
    bool scanned = work.mode == DocumentEnhancementMode::Scanned;
    bool lite = DocumentEnhancerPreviewLite();
    // Yellow scans fail the low-sat confidence score; do not let that dilute Strong.
    if (scanned && agedPaper && conf < 0.85f) {
        conf = 0.85f;
    }

    float bgStrength = work.backgroundStrength;
    if (alreadyClean && !scanned && baselineOnly) {
        // Keep pure white paper; still allow light baseline sharpen for crisp text.
        QueryPerformanceCounter(&tSh0);
        if (work.sharpenAmount > 0.01f) {
            ApplyLuminanceSharpen(yBuf, width, height, work.sharpenAmount * 0.85f, work.sharpenRadius,
                                  work.sharpenThreshold, nullptr);
            if (darkPage) {
                for (int i = 0; i < n; i++) {
                    yBuf[i] = 255.f - yBuf[i];
                }
            }
            RecombineColor(bgra, width, height, stride, ySrc, yBuf, work.preserveColor, false);
        }
        QueryPerformanceCounter(&tSh1);
        free(yBuf);
        free(ySrc);
        free(satBuf);
        QueryPerformanceCounter(&t1);
        st->msSharpen = 1000.0 * (double)(tSh1.QuadPart - tSh0.QuadPart) / (double)freq.QuadPart;
        st->msTotal = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        st->backgroundStrengthUsed = 0.f;
        return;
    } else if (washedInk || lite) {
        // White paper (or live slider preview): skip illumination map entirely.
        bgStrength = 0.f;
    } else if (alreadyClean) {
        bgStrength *= 0.15f;
    } else if (scanned) {
        float floor = agedPaper ? 0.92f : 0.70f;
        bgStrength *= Clampf(floor + (1.f - floor) * conf, 0.f, 1.f);
    } else {
        bgStrength *= Clampf(0.45f + 0.55f * conf, 0.f, 1.f);
    }
    st->backgroundStrengthUsed = bgStrength;

    QueryPerformanceCounter(&tBg0);
    BackgroundMap map{};
    bool didBg = false;
    if (bgStrength > 0.02f) {
        float paperHint = Lerp(hs.p90, hs.p95, 0.6f);
        if (paperHint < 140.f) {
            paperHint = hs.p95 > 130.f ? hs.p95 : 190.f;
        }
        if (EstimateBackgroundMap(yBuf, satBuf, width, height, paperHint, work.imageProtection, scanned, &map)) {
            ApplyBackgroundNormalization(yBuf, width, height, map, bgStrength, work.backgroundTarget, conf, scanned);
            MaybeDumpGray(dumpDir, "04_background_map", map.values, map.mapW, map.mapH);
            MaybeDumpGray(dumpDir, "05_normalized", yBuf, width, height);
            didBg = true;
        }
    }
    QueryPerformanceCounter(&tBg1);

    // Rebuild histogram only when background normalization changed luminances.
    HistogramStats hs2 = hs;
    if (didBg) {
        BuildHistogram(yBuf, nullptr, n, &hs2);
    }

    float blackPoint = 0.f;
    float whitePoint = 255.f;
    EstimateBlackWhitePoints(hs2, conf, work.blackPointStrength, work.whitePointStrength, scanned, &blackPoint,
                             &whitePoint);
    st->blackPoint = blackPoint;
    st->whitePoint = whitePoint;
    st->gamma = work.gamma;

    uint8_t lut[256];
    BuildToneLut(blackPoint, whitePoint, work.gamma, work.sCurveStrength, lut);

    QueryPerformanceCounter(&tTone0);
    ApplyToneToY(yBuf, n, lut);

    const BackgroundMap* prot = (map.protect && map.values) ? &map : nullptr;
    if (scanned) {
        float punch = washedInk ? 0.82f : (agedPaper ? 0.75f : 0.55f);
        punch *= Clampf(0.55f + 0.45f * work.blackPointStrength, 0.f, 1.2f);
        if (lite) {
            punch *= 0.85f;
        }
        float paperRef = Clampf(hs2.p95 > 200.f ? hs2.p95 : work.backgroundTarget, 230.f, 252.f);
        ApplyGrayInkPunch(yBuf, width, height, punch, paperRef, prot);
        // Denoise is a full extra blur pass — skip on washed white paper (SoftPush
        // cleans grain) and during live slider preview.
        if (!washedInk && !lite) {
            MildPaperDenoise(yBuf, width, height, 236.f, 0.22f);
        }
    }

    // Finish paper → screen white (Strong stronger). Mild keeps a milder push.
    {
        float pushAmt = scanned ? 0.95f : 0.35f;
        pushAmt *= Clampf(0.5f + 0.5f * work.whitePointStrength, 0.f, 1.2f);
        float pushThresh = Lerp(240.f, 215.f, Clampf(work.whitePointStrength, 0.f, 1.f));
        if (scanned) {
            pushThresh = washedInk ? 218.f : (agedPaper ? 198.f : 225.f);
            pushAmt = Clampf(pushAmt, 0.f, 1.0f);
        } else if (work.mode == DocumentEnhancementMode::Reading) {
            pushThresh = 245.f;
            pushAmt = Clampf(pushAmt, 0.f, 0.40f);
        }
        SoftPushPaperWhite(yBuf, n, pushThresh, pushAmt);
    }
    QueryPerformanceCounter(&tTone1);

    QueryPerformanceCounter(&tSh0);
    {
        float sharpAmt = work.sharpenAmount;
        float sharpRad = work.sharpenRadius;
        float sharpThr = work.sharpenThreshold;
        if (scanned && washedInk) {
            sharpAmt *= 1.15f;
        }
        if (lite) {
            // Cheaper preview: one lighter unsharp pass.
            sharpAmt *= 0.70f;
            sharpRad = 0.8f;
            sharpThr = sharpThr + 1.f;
        }
        ApplyLuminanceSharpen(yBuf, width, height, sharpAmt, sharpRad, sharpThr, prot);
    }
    QueryPerformanceCounter(&tSh1);

    MaybeDumpGray(dumpDir, "06_tone", yBuf, width, height);
    MaybeDumpGray(dumpDir, "07_final", yBuf, width, height);
    if (darkPage) {
        for (int i = 0; i < n; i++) {
            yBuf[i] = 255.f - yBuf[i];
        }
    }
    bool whitenPaper = scanned && (washedInk || agedPaper);
    RecombineColor(bgra, width, height, stride, ySrc, yBuf, work.preserveColor, whitenPaper);

    FreeBackgroundMap(&map);
    free(yBuf);
    free(ySrc);
    free(satBuf);

    QueryPerformanceCounter(&t1);
    auto ms = [&](LARGE_INTEGER a, LARGE_INTEGER b) -> double {
        return 1000.0 * (double)(b.QuadPart - a.QuadPart) / (double)freq.QuadPart;
    };
    st->msHistogram = ms(tHist0, tHist1);
    st->msBackground = ms(tBg0, tBg1);
    st->msTone = ms(tTone0, tTone1);
    st->msSharpen = ms(tSh0, tSh1);
    st->msTotal = ms(t0, t1);
    MaybeDumpStats(dumpDir, *st, params);
}
