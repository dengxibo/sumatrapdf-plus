/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

// RapidOCR / PP-OCR ONNX sidecar. Models and onnxruntime.dll live in {exedir}/ocr/.

struct OcrBox {
    Rect rect{};
    char* text = nullptr;
    int* charX = nullptr; // 2*nChar crop-x pairs [x0,x1) from CTC
    int nChar = 0;
    // Tall CJK column: crop was rotated 90° CCW for rec; charX is along original Y.
    bool vertical = false;
};

void FreeOcrBoxes(Vec<OcrBox>& boxes);

enum class OcrProfile {
    Fast = 0, // PP-OCRv6 tiny det + tiny rec
    Balanced, // PP-OCRv6 small det + small rec
    Hybrid,   // PP-OCRv6 small det + tiny rec (experimental)
};

enum class OcrOperation {
    CurrentPage = 0,
    AllPages,
    SaveSearchable,
};

struct OcrPageTiming {
    double rasterizeMs = 0;
    double detPreprocessMs = 0;
    double detInferenceMs = 0;
    double detPostprocessMs = 0;
    double cropMs = 0;
    double recPreprocessMs = 0;
    double recInferenceMs = 0;
    double recPostprocessMs = 0;
    double textLayerMs = 0;
    double pageTotalMs = 0;
    int nDetBoxes = 0;
    int nRecBoxes = 0;
};

// Directory we look in for onnxruntime.dll + det/rec/cls + keys.
char* OcrSidecarDirTemp();
bool OcrSidecarLooksPresent();
bool OcrModelsAvailable();
const char* OcrModelsMissingHint();
const char* OcrLastError();
const char* OcrProfileName(OcrProfile profile);
OcrProfile GetOcrProfileForOperation(OcrOperation op);
void OcrSetForcedProfile(OcrProfile profile, bool enable);

// RGB top-down 24-bit (3 bytes/pixel, packed or strided). boxesOut owns text strings.
bool OcrRecognizeRgb(const u8* rgb, int w, int h, int stride, Vec<OcrBox>& boxesOut, OcrProfile profile,
                     OcrPageTiming* timing = nullptr);

// How many pages we OCR at once (1–4). Same count as ONNX session slots.
int OcrInferenceSlotCount();
