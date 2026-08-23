/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// RapidOCR / PP-OCR ONNX sidecar. Models and onnxruntime.dll live in {exedir}/ocr/.

struct OcrBox {
    Rect rect{};
    char* text = nullptr;
    int* charX = nullptr; // 2*nChar crop-x pairs [x0,x1) from CTC
    int nChar = 0;
};

void FreeOcrBoxes(Vec<OcrBox>& boxes);

// Directory we look in for onnxruntime.dll + det/rec/cls + keys.
char* OcrSidecarDirTemp();
bool OcrSidecarLooksPresent();
bool OcrModelsAvailable();
const char* OcrModelsMissingHint();
const char* OcrLastError();

// RGB top-down 24-bit (3 bytes/pixel, packed or strided). boxesOut owns text strings.
bool OcrRecognizeRgb(const u8* rgb, int w, int h, int stride, Vec<OcrBox>& boxesOut);

// How many pages we OCR at once (1–4). Same count as ONNX session slots.
int OcrInferenceSlotCount();
