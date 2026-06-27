/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

enum class CadEnhanceReason {
    None,
    Pdfe,
    Metadata,
    Heuristic,
    Manual,
};

enum class CadEnhanceOverride {
    Unset = 0,
    ForceOff = 1,
    ForceOn = 2,
};

enum class EngineeringDrawingEnhanceMode {
    Off,
    Auto,
    On,
};

struct CadDetectResult {
    bool enable = false;
    CadEnhanceReason reason = CadEnhanceReason::None;
    int score = 0;
};

struct fz_context;
struct pdf_document;

EngineeringDrawingEnhanceMode GetEngineeringDrawingEnhanceMode();
CadDetectResult DetectCadPdf(fz_context* ctx, pdf_document* doc);
bool CadEnhanceEnabledForEngine(const CadDetectResult& detect, CadEnhanceOverride overrideState);
const char* CadEnhanceReasonName(CadEnhanceReason reason);
