/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include <math.h>
#include <stdlib.h>

// UI state remains independent of controls and bitmap resolution. Rotation
// actions accumulate quarter turns without discarding pending deskew.
struct PdfPageAdjustment {
    double deskew = 0;
    int direction = 0;

    double TotalAngle() const {
        double angle = fmod(deskew + direction, 360.0);
        if (angle > 180.0) angle -= 360.0;
        if (angle < -180.0) angle += 360.0;
        return angle;
    }

    void RotateBy(int degrees) {
        direction = (direction + degrees) % 360;
        if (direction > 180) direction -= 360;
        if (direction < -180) direction += 360;
    }

    void SetDeskew(double value) { deskew = value < -180.0 ? -180.0 : value > 180.0 ? 180.0 : value; }
};

inline bool PdfParseAdjustmentAngle(const char* text, double* angle) {
    if (!text || !angle) return false;
    const char* p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+' || *p == '-') p++;
    bool digits = false;
    while (*p >= '0' && *p <= '9') {
        digits = true;
        p++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            digits = true;
            p++;
        }
    }
    while (*p == ' ' || *p == '\t') p++;
    if (!digits || *p) return false;
    double value = strtod(text, nullptr);
    if (!(value >= -180.0 && value <= 180.0)) return false;
    *angle = value;
    return true;
}

// Analytic integral of a smoothstep speed ramp. It depends only on elapsed
// time, so a delayed/dropped UI timer cannot change the resulting angle.
inline double PdfAdjustmentHoldTravel(double seconds) {
    double t = seconds - 0.25;
    if (t <= 0) return 0;
    constexpr double ramp = 1.5;
    constexpr double initial = 0.3;
    constexpr double maximum = 1.8;
    if (t >= ramp) return ramp * (initial + maximum) * 0.5 + (t - ramp) * maximum;
    double u = t / ramp;
    return initial * t + (maximum - initial) * ramp * (u * u * u - u * u * u * u * 0.5);
}
