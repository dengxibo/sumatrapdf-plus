/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

struct FzPageInfo;
struct fz_context;

enum class DarkImagePolicy {
    Preserve,
    AdaptiveDocument,
    ThemeRecolor,
};

enum class PdfDarkModeRenderer {
    LegacyBitmapPostProcess = 0,
    ObjectLevelDevice = 1,
};

enum class PdfDocumentColorMode {
    Auto = 0,
    Black = 1,
    Light = 2,
};

struct DarkModeOptions {
    float scanImageCoverageThreshold = 0.75f;
    int maxTextOpsForScanPage = 10;
    int maxVectorOpsForScanPage = 20;
    // 0=off, 1=blend near-white Preserve-image pixels toward page background
    float preserveImagePaperSoftening = 0.f;
    float lightFillChromaThreshold = 0.05f;
    float lightFillLuminanceThreshold = 0.45f;
};

struct ImageOccurrenceInfo {
    int occurrenceIndex = 0;
    RectF pageBounds{};
    bool isImageMask = false;
    bool hasAlpha = false;
    float pageCoverage = 0.f;
    DarkImagePolicy policy = DarkImagePolicy::Preserve;
};

struct DarkModePageAnalysis {
    int pageNumber = 0;
    RectF pageBounds{};
    bool isScannedPage = false;
    Vec<ImageOccurrenceInfo> images;
    u32 optionsHash = 0;
    void* processCache = nullptr;
};

struct DarkModePalette {
    float textR = 0.f, textG = 0.f, textB = 0.f;
    float bgR = 1.f, bgG = 1.f, bgB = 1.f;
    float linkR = 0.f, linkG = 0.f, linkB = 0.f;
    float diffR = 1.f, diffG = 1.f, diffB = 1.f;
};

struct DarkModeReplayState {
    int nextImageOccurrence = 0;
};

// PDF dark mode runtime options (not stored in settings file)
bool GetPreservePdfImagesInDarkMode();
void SetPreservePdfImagesInDarkMode(bool preserve);
int GetPreservePdfImagesMinSize();
PdfDarkModeRenderer GetPdfDarkModeRenderer();

bool PdfDarkModeUsesObjectLevel();
PdfDocumentColorMode GetPdfDocumentColorMode();
void SetPdfDocumentColorMode(PdfDocumentColorMode mode);
const char* PdfDocumentColorModeDescription(PdfDocumentColorMode mode);
DarkModeOptions PdfDarkModeCurrentOptions();
u32 PdfDarkModeComputeOptionsHash();
DarkModePalette PdfDarkModeBuildPalette();

void PdfDarkModeFreeAnalysis(fz_context* ctx, DarkModePageAnalysis* analysis);
void PdfDarkModeInvalidatePage(fz_context* ctx, FzPageInfo* pageInfo);

void ApplyAdaptiveDocumentDarkMode(float r, float g, float b, const DarkModePalette& palette, float* outR, float* outG,
                                   float* outB);
