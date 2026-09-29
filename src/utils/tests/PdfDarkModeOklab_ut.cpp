/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"

#include "PdfDarkMode.h"

#include <math.h>

#include "utils/UtAssert.h"

static float SrgbToLinear(float c) {
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static float RelLuminance(float r, float g, float b) {
    float lr = SrgbToLinear(r);
    float lg = SrgbToLinear(g);
    float lb = SrgbToLinear(b);
    return 0.2126f * lr + 0.7152f * lg + 0.0722f * lb;
}

static float ContrastRatio(float r1, float g1, float b1, float r2, float g2, float b2) {
    float l1 = RelLuminance(r1, g1, b1);
    float l2 = RelLuminance(r2, g2, b2);
    if (l1 < l2) {
        float t = l1;
        l1 = l2;
        l2 = t;
    }
    return (l1 + 0.05f) / (l2 + 0.05f);
}

static DarkModePalette TestPalette() {
    DarkModePalette p;
    p.textR = 0.90f;
    p.textG = 0.90f;
    p.textB = 0.88f;
    p.bgR = 0.07f;
    p.bgG = 0.09f;
    p.bgB = 0.14f;
    p.diffR = p.bgR - p.textR;
    p.diffG = p.bgG - p.textG;
    p.diffB = p.bgB - p.textB;
    return p;
}

void PdfDarkModeOklab_UnitTests() {
    DarkModePalette palette = TestPalette();
    float out[3] = {};

    MapRgbToDarkThemeOklab(0.f, 0.f, 0.f, palette, out);
    utassert(out[0] > 0.5f && out[1] > 0.5f && out[2] > 0.5f);

    MapRgbToDarkThemeOklab(1.f, 1.f, 1.f, palette, out);
    utassert(fabsf(out[0] - palette.bgR) < 1e-5f && fabsf(out[1] - palette.bgG) < 1e-5f &&
             fabsf(out[2] - palette.bgB) < 1e-5f);

    // Dracula paper must stay chromatic gray-black, not a neutral #2A2A2A.
    DarkModePalette dracula = palette;
    dracula.bgR = 40.f / 255.f;
    dracula.bgG = 42.f / 255.f;
    dracula.bgB = 54.f / 255.f;
    unsigned char plate[8 * 8 * 3];
    for (int i = 0; i < 8 * 8; i++) {
        plate[i * 3] = 255;
        plate[i * 3 + 1] = 255;
        plate[i * 3 + 2] = 255;
    }
    utassert(PdfDarkModeToneThemeVariant(plate, 8, 8, 3, 8 * 3, dracula, 0));
    utassert(plate[0] == 40 && plate[1] == 42 && plate[2] == 54);
    // JPEG-softened figure paper must snap too (was a visible charcoal rectangle).
    for (int i = 0; i < 8 * 8; i++) {
        plate[i * 3] = 180;
        plate[i * 3 + 1] = 180;
        plate[i * 3 + 2] = 180;
    }
    utassert(PdfDarkModeToneThemeVariant(plate, 8, 8, 3, 8 * 3, dracula, 0));
    utassert(plate[0] == 40 && plate[1] == 42 && plate[2] == 54);

    float midOut[3] = {};
    MapRgbToDarkThemeOklab(0.5f, 0.5f, 0.5f, palette, midOut);
    utassert(midOut[0] > out[0] && midOut[0] < out[0] + 0.6f);

    // Monotone gray ramp: lighter inputs map to darker outputs.
    float prevOutL = 2.f;
    for (int i = 10; i <= 90; i += 20) {
        float g = i / 100.f;
        MapRgbToDarkThemeOklab(g, g, g, palette, out);
        float outL = RelLuminance(out[0], out[1], out[2]);
        utassert(outL <= prevOutL + 0.001f);
        prevOutL = outL;
    }

    MapRgbToDarkThemeOklab(0.85f, 0.15f, 0.12f, palette, out);
    utassert(out[0] > out[1] && out[0] > out[2]);

    MapRgbToDarkThemeOklab(0.12f, 0.55f, 0.48f, palette, out);
    utassert(out[1] > out[0] && out[1] > out[2]);

    MapRgbToDarkThemeOklab(0.15f, 0.20f, 0.85f, palette, out);
    utassert(out[2] > out[0] && out[2] > out[1]);

    MapRgbToDarkThemeOklab(0.72f, 0.74f, 0.78f, palette, out);
    utassert(fabsf(out[0] - out[1]) < 0.15f && fabsf(out[1] - out[2]) < 0.15f);

    MapRgbToDarkThemeOklab(0.f, 0.f, 0.f, palette, out);
    float cr = ContrastRatio(out[0], out[1], out[2], palette.bgR, palette.bgG, palette.bgB);
    utassert(cr >= 4.0f);

    // Photos darken without flipping light and shadow, and a weak warm shadow stays dull.
    float prevPhoto = -1.f;
    for (int i = 0; i <= 100; i += 10) {
        float g = i / 100.f;
        MapRgbPhotoDarkAdapt(g, g, g, palette, out);
        float outL = RelLuminance(out[0], out[1], out[2]);
        utassert(outL + 0.002f >= prevPhoto);
        float inL = RelLuminance(g, g, g);
        if (i >= 50) {
            utassert(outL + 0.02f < inL);
        } else {
            utassert(outL <= inL + 0.005f);
        }
        prevPhoto = outL;
    }
    MapRgbPhotoDarkAdapt(0.18f, 0.10f, 0.08f, palette, out);
    float shadowL = RelLuminance(out[0], out[1], out[2]);
    utassert(shadowL < 0.08f);
    utassert(out[0] < out[1] + 0.12f);
    MapRgbPhotoDarkAdapt(0.92f, 0.90f, 0.86f, palette, out);
    utassert(RelLuminance(out[0], out[1], out[2]) > shadowL);
    utassert(fabsf(out[0] - out[1]) < 0.08f && fabsf(out[1] - out[2]) < 0.08f);

    unsigned char flat[8 * 8 * 3];
    memset(flat, 180, sizeof(flat));
    utassert(PdfDarkModePhotoAdaptRgbSamples(flat, 8, 8, 3, 8 * 3));
    MapRgbPhotoDarkAdapt(180.f / 255.f, 180.f / 255.f, 180.f / 255.f, palette, out);
    utassert(abs(flat[0] - (int)(out[0] * 255.f + 0.5f)) <= 1);
    utassert(abs(flat[1] - (int)(out[1] * 255.f + 0.5f)) <= 1);
    utassert(abs(flat[2] - (int)(out[2] * 255.f + 0.5f)) <= 1);

    unsigned char grain[16 * 16 * 3];
    memset(grain, 80, sizeof(grain));
    grain[(8 * 16 + 8) * 3 + 0] = 220;
    grain[(8 * 16 + 8) * 3 + 1] = 220;
    grain[(8 * 16 + 8) * 3 + 2] = 220;
    utassert(PdfDarkModePhotoAdaptRgbSamples(grain, 16, 16, 3, 16 * 3));
    int spike = grain[(8 * 16 + 8) * 3];
    int field = grain[0];
    utassert(spike > field + 20);

    utassert(PdfDarkModeOklabDistance(1.f, 1.f, 1.f, 1.f, 1.f, 1.f) < 0.001f);
    utassert(PdfDarkModeOklabDistance(1.f, 1.f, 1.f, 0.f, 0.f, 0.f) > 0.15f);
    utassert(PdfDarkModeOklabDistance(0.95f, 0.93f, 0.88f, 0.97f, 0.95f, 0.90f) < 0.06f);
}
