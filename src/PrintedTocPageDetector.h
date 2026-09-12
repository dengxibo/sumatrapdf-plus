/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */
#pragma once
#include "PrintedTocModel.h"

// Page detection deliberately does not use the TOC entry parser.
// Geometry and histograms use page width/height fractions. Counts express
// repeated evidence, not pixel distances. Scores are evidence, not probabilities.
struct TocPageFeatures {
    int page = 0;
    int lines = 0;
    int entries = 0;
    int pageNumbers = 0;
    int columns = 0;
    bool title = false;
    bool excludedHeading = false;
    float pageColumn = 0;
    float inlineLabels = 0;
    float leaders = 0;
    float indentation = 0;
    float spacing = 0;
    float paragraph = 0;
    float tableImage = 0; // OCR-layout proxy, not an image classifier
    float titlePage = 0;
    float density = 0;
    float meanHeight = 0;
    float anchorX[2]{};
    float leftHistogram[10]{};
    float yHistogram[8]{};
    float raw = 0;
    float start = 0;
    float smoothed = 0;
    float neighbor = 0;
    float similarity = 0;
    bool selected = false;
    const char* state = "NON_TOC";
    const char* reason = "insufficient entry structure";
};
struct TocPageInterval {
    int startPage = 0;
    int endPage = 0;
    float confidence = 0;
};
TocPageFeatures MeasureTocPage(const PtPageData& page, int documentPages);
TocPageInterval DetectTocPageInterval(Vec<TocPageFeatures>& pages);
void WriteTocPageDiagnostics(const char* output, const Vec<TocPageFeatures>& pages, const TocPageInterval& interval);
