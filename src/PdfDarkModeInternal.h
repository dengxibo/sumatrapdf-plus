/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

extern "C" {
#include <mupdf/fitz.h>
}

#include "PdfDarkMode.h"

DarkModePageAnalysis* PdfDarkModeGetOrBuildAnalysis(fz_context* ctx, FzPageInfo* pageInfo, fz_display_list* list,
                                                    u32 optionsHash);

fz_device* PdfDarkModeWrapDevice(fz_context* ctx, fz_device* inner, DarkModePageAnalysis* analysis,
                                 const DarkModePalette* palette, DarkModeReplayState* replayState);

void MapColorToDarkTheme(fz_context* ctx, fz_colorspace* cs, const float* color, fz_color_params colorParams,
                         const DarkModePalette& palette, float* outRgb);

void MapFillColorToDarkTheme(fz_context* ctx, fz_colorspace* cs, const float* color, fz_color_params colorParams,
                             const DarkModePalette& palette, float* outRgb);

void MapRgbFillToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb);

void MapRgbToDarkTheme(float r, float g, float b, const DarkModePalette& palette, float* outRgb);

void ApplyPreserveImagePaperSoftening(float r, float g, float b, const DarkModePalette& palette, float strength,
                                      float* outR, float* outG, float* outB);

void PdfDarkModeFreeProcessCache(fz_context* ctx, DarkModePageAnalysis* analysis);

// Returns a kept fz_image for fill_image, or nullptr to use the source image.
fz_image* PdfDarkModeGetCachedImage(fz_context* ctx, DarkModePageAnalysis* analysis, int occurrenceIndex,
                                    fz_image* srcImage, DarkImagePolicy policy, const DarkModePalette& palette);

// Returns a kept fz_image covering bounds, or nullptr to fall back to direct shade fill.
fz_image* PdfDarkModeGetCachedShade(fz_context* ctx, DarkModePageAnalysis* analysis, fz_shade* shade, fz_matrix ctm,
                                    float alpha, fz_irect bounds, const DarkModePalette& palette);
