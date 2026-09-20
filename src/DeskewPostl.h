/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3

   Document skew estimator: Postl differential line-sum variance
   (W. Postl, U.S. Patent 4,723,297), as used by Leptonica pixFindSkew.
   Self-contained — does not link libleptonica. */

struct DeskewPostlResult {
    float angleDeg = 0.f;    // fz_rotate correction (CCW positive)
    float confidence = 0.f;  // maxScore / minScore over sweep
    float improvement = 0.f; // (scoreBest - score0) / score0
    bool ok = false;
};

namespace DeskewPostl {

// gray: row-major luminance. Returns ok=false when the page is too small / empty.
DeskewPostlResult FindSkew(const unsigned char* gray, int w, int h);

} // namespace DeskewPostl
