/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3

   Postl / Leptonica-style skew finder (differential square-sum of row sums
   after vertical shear). Cherry-picked algorithm; no libleptonica link. */

#include "utils/BaseUtil.h"
#include "DeskewPostl.h"

#include <math.h>

namespace DeskewPostl {

static int Popcount32(u32 v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (int)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}

static int OtsuThreshold(const u8* gray, int n) {
    int hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++) {
        hist[gray[i]]++;
    }
    double sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += (double)i * hist[i];
    }
    double sumB = 0;
    int wB = 0;
    double maxv = 0;
    int best = 128;
    for (int t = 0; t < 256; t++) {
        wB += hist[t];
        if (!wB) {
            continue;
        }
        int wF = n - wB;
        if (!wF) {
            break;
        }
        sumB += (double)t * hist[t];
        double d = sumB / wB - (sum - sumB) / wF;
        double v = (double)wB * wF * d * d;
        if (v > maxv) {
            maxv = v;
            best = t;
        }
    }
    return best;
}

// Pack ink (dark) pixels as 1 bits. wpl = words per line.
static u32* GrayTo1BitInk(const u8* gray, int w, int h, int thresh, int* outWpl) {
    int wpl = (w + 31) / 32;
    *outWpl = wpl;
    u32* bits = (u32*)calloc((size_t)wpl * h, sizeof(u32));
    if (!bits) {
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        const u8* row = gray + y * w;
        u32* line = bits + y * wpl;
        for (int x = 0; x < w; x++) {
            if (row[x] < thresh) {
                line[x >> 5] |= (1u << (31 - (x & 31)));
            }
        }
    }
    return bits;
}

static u32* Reduce2x(const u32* src, int w, int h, int wpl, int* outW, int* outH, int* outWpl) {
    int rw = w / 2;
    int rh = h / 2;
    if (rw < 8 || rh < 8) {
        return nullptr;
    }
    int rwpl = (rw + 31) / 32;
    *outW = rw;
    *outH = rh;
    *outWpl = rwpl;
    u32* dst = (u32*)calloc((size_t)rwpl * rh, sizeof(u32));
    if (!dst) {
        return nullptr;
    }
    auto getBit = [](const u32* line, int x) -> int { return (line[x >> 5] >> (31 - (x & 31))) & 1; };
    for (int y = 0; y < rh; y++) {
        const u32* r0 = src + (y * 2) * wpl;
        const u32* r1 = src + (y * 2 + 1) * wpl;
        u32* dline = dst + y * rwpl;
        for (int x = 0; x < rw; x++) {
            int sx = x * 2;
            if (getBit(r0, sx) || getBit(r0, sx + 1) || getBit(r1, sx) || getBit(r1, sx + 1)) {
                dline[x >> 5] |= (1u << (31 - (x & 31)));
            }
        }
    }
    return dst;
}

// Vertical shear about image center. Positive angle_rad shears the same way
// Leptonica pixVShearCorner does for skew search.
static void VShear1Bit(const u32* src, u32* dst, int w, int h, int wpl, float angleRad) {
    memset(dst, 0, (size_t)wpl * h * sizeof(u32));
    float tanA = tanf(angleRad);
    int cx = w / 2;
    for (int x = 0; x < w; x++) {
        int shift = (int)(tanA * (float)(x - cx) + (tanA >= 0 ? 0.5f : -0.5f));
        for (int y = 0; y < h; y++) {
            int sy = y - shift;
            if (sy < 0 || sy >= h) {
                continue;
            }
            if ((src[sy * wpl + (x >> 5)] >> (31 - (x & 31))) & 1) {
                dst[y * wpl + (x >> 5)] |= (1u << (31 - (x & 31)));
            }
        }
    }
}

static void RowSums(const u32* bits, int h, int wpl, int* sums) {
    for (int y = 0; y < h; y++) {
        const u32* line = bits + y * wpl;
        int count = 0;
        for (int i = 0; i < wpl; i++) {
            count += Popcount32(line[i]);
        }
        sums[y] = count;
    }
}

// Leptonica pixFindDifferentialSquareSum: skip margins, sum (row[i]-row[i-1])^2.
static double DiffSquareSum(const int* sums, int h, int w) {
    int skiph = (int)(0.05 * w);
    int skip = skiph;
    if (h / 10 < skip) {
        skip = h / 10;
    }
    int nskip = skip / 2;
    if (nskip < 1) {
        nskip = 1;
    }
    double score = 0;
    for (int i = nskip; i < h - nskip; i++) {
        double d = (double)(sums[i] - sums[i - 1]);
        score += d * d;
    }
    return score;
}

DeskewPostlResult FindSkew(const unsigned char* gray, int w, int h) {
    DeskewPostlResult r{};
    if (!gray || w < 80 || h < 80) {
        return r;
    }

    int thresh = OtsuThreshold(gray, w * h);
    if (thresh < 255) {
        thresh++;
    }
    int wpl = 0;
    u32* bits = GrayTo1BitInk(gray, w, h, thresh, &wpl);
    if (!bits) {
        return r;
    }

    // 4x reduction (two 2x passes) — Leptonica default sweep reduction.
    int rw = 0, rh = 0, rwpl = 0;
    u32* r1 = Reduce2x(bits, w, h, wpl, &rw, &rh, &rwpl);
    free(bits);
    if (!r1) {
        return r;
    }
    int rw2 = 0, rh2 = 0, rwpl2 = 0;
    u32* reduced = Reduce2x(r1, rw, rh, rwpl, &rw2, &rh2, &rwpl2);
    free(r1);
    if (!reduced) {
        return r;
    }

    constexpr float kSweepRange = 7.f;
    constexpr float kSweepDelta = 1.f;
    constexpr float kDeg2Rad = 3.14159265f / 180.f;
    int nAngles = (int)((2.f * kSweepRange) / kSweepDelta) + 1;
    if (nAngles < 3 || nAngles > 64) {
        free(reduced);
        return r;
    }

    float angles[64];
    double scores[64];
    int* sums = new int[rh2];
    u32* sheared = (u32*)calloc((size_t)rwpl2 * rh2, sizeof(u32));
    if (!sheared) {
        delete[] sums;
        free(reduced);
        return r;
    }

    double maxScore = 0;
    double minScore = 1e300;
    int maxIdx = 0;
    double score0 = 0;
    for (int i = 0; i < nAngles; i++) {
        angles[i] = -kSweepRange + i * kSweepDelta;
        VShear1Bit(reduced, sheared, rw2, rh2, rwpl2, angles[i] * kDeg2Rad);
        RowSums(sheared, rh2, rwpl2, sums);
        scores[i] = DiffSquareSum(sums, rh2, rw2);
        if (scores[i] > maxScore) {
            maxScore = scores[i];
            maxIdx = i;
        }
        if (scores[i] < minScore) {
            minScore = scores[i];
        }
        if (angles[i] > -0.01f && angles[i] < 0.01f) {
            score0 = scores[i];
        }
    }

    // Binary-search refinement around the coarse peak (Leptonica search stage).
    float lo = maxIdx > 0 ? angles[maxIdx - 1] : angles[0];
    float hi = maxIdx < nAngles - 1 ? angles[maxIdx + 1] : angles[nAngles - 1];
    float bestAngle = angles[maxIdx];
    double bestScore = maxScore;
    for (int iter = 0; iter < 12; iter++) {
        float midLo = (lo + bestAngle) * 0.5f;
        float midHi = (bestAngle + hi) * 0.5f;
        VShear1Bit(reduced, sheared, rw2, rh2, rwpl2, midLo * kDeg2Rad);
        RowSums(sheared, rh2, rwpl2, sums);
        double scoreLo = DiffSquareSum(sums, rh2, rw2);
        VShear1Bit(reduced, sheared, rw2, rh2, rwpl2, midHi * kDeg2Rad);
        RowSums(sheared, rh2, rwpl2, sums);
        double scoreHi = DiffSquareSum(sums, rh2, rw2);
        if (scoreLo > bestScore) {
            hi = bestAngle;
            bestAngle = midLo;
            bestScore = scoreLo;
        } else if (scoreHi > bestScore) {
            lo = bestAngle;
            bestAngle = midHi;
            bestScore = scoreHi;
        } else {
            lo = midLo;
            hi = midHi;
        }
    }

    free(sheared);
    delete[] sums;
    free(reduced);

    float conf = (minScore > 1.0) ? (float)(bestScore / minScore) : 0.f;
    float improvement = (score0 > 1.0) ? (float)((bestScore - score0) / score0) : 0.f;
    if (improvement < 0.05f && fabsf(bestAngle) < 0.6f) {
        conf *= 0.5f;
    }

    // Shear angle that maximizes Postl score is the page skew in Leptonica's
    // clockwise-positive sense. Fitz page space is y-down; fz_rotate(deg) with
    // that same signed angle straightens the page (negating it rotated the
    // wrong way).
    r.ok = true;
    r.angleDeg = bestAngle;
    r.confidence = conf;
    r.improvement = improvement;
    return r;
}

} // namespace DeskewPostl
