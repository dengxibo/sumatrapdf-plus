/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "PdfCadDetect.h"
#include "PdfCadEnhanceDevice.h"

// Stub implementations for binaries that compile EngineMupdf.cpp but not
// PdfCadDetect.cpp / PdfCadEnhanceDevice.cpp (PdfFilter, PdfPreview, etc.).

EngineeringDrawingEnhanceMode GetEngineeringDrawingEnhanceMode() {
    return EngineeringDrawingEnhanceMode::Off;
}

const char* CadEnhanceReasonName(CadEnhanceReason reason) {
    (void)reason;
    return "none";
}

bool CadEnhanceEnabledForEngine(const CadDetectResult& detect, CadEnhanceOverride overrideState) {
    (void)detect;
    (void)overrideState;
    return false;
}

CadDetectResult DetectCadPdf(fz_context* ctx, pdf_document* doc) {
    (void)ctx;
    (void)doc;
    return CadDetectResult{};
}

fz_device* PdfCadEnhanceWrapDevice(fz_context* ctx, fz_device* inner, const CadEnhanceRenderOpts& opts) {
    (void)ctx;
    (void)inner;
    (void)opts;
    return inner;
}

CadMinLineWidthScope::CadMinLineWidthScope(fz_context* ctx, float zoom, bool active, bool hairlineDoc) {
    (void)ctx;
    (void)zoom;
    (void)active;
    (void)hairlineDoc;
}

CadMinLineWidthScope::~CadMinLineWidthScope() {}

void PdfCadEnhancePixmap(fz_context* ctx, fz_pixmap* pix, float zoom, bool rasterDominant) {
    (void)ctx;
    (void)pix;
    (void)zoom;
    (void)rasterDominant;
}
