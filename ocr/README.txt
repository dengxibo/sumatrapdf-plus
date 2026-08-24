RapidOCR / PP-OCR sidecar for SumatraPDF Plus
=============================================

Scan-only pages (little or no text layer) can be recognized so you can select
and search. Results stay in memory for this open document unless you use
"Recognize all pages and save" (toolbar OCR menu), which writes a searchable
PDF over the current file.

User guide: docs/md/OCR.md

Put these files in this folder (next to SumatraPDF-Plus.exe after a build they are
also copied to out/dbg64/ocr and out/rel64/ocr):

  onnxruntime.dll          ONNX Runtime CPU, Windows x64
                           (same major version as ext/onnxruntime/onnxruntime_c_api.h,
                            currently 1.17.x)

  det.onnx                 PP-OCR detection (mobile)
  rec.onnx                 PP-OCR recognition (mobile)
  keys.txt                 character table, one token per line
                           (CTC blank is implied as index 0)

Optional:

  cls.onnx                 0/180 angle classifier

Accepted filenames (first match wins):

  det:  det.onnx
        ch_PP-OCRv4_det_mobile.onnx
        ch_PP-OCRv4_det_infer.onnx
        ch_PP-OCRv5_det_mobile.onnx
        ch_PP-OCRv5_det_infer.onnx

  rec:  rec.onnx
        ch_PP-OCRv4_rec_mobile.onnx
        ch_PP-OCRv4_rec_infer.onnx
        ch_PP-OCRv5_rec_mobile.onnx
        ch_PP-OCRv5_rec_infer.onnx

  cls:  cls.onnx
        ch_ppocr_mobile_v2.0_cls_infer.onnx
        ch_ppocr_mobile_v2.0_cls_mobile.onnx

  keys: keys.txt
        ppocr_keys_v1.txt
        ppocr_keys.txt

Where to get them
-----------------
Models are RapidOCR builds of Baidu Paddle PP-OCR (ONNX). Download the Chinese
mobile set, not the large server models (those are optional later).

  https://github.com/RapidAI/RapidOCR
  https://github.com/microsoft/onnxruntime/releases

Default zip ships mobile only (about 30-50 MB with the runtime).

Advanced setting: AutoOcrScanPages (default false). Set true or use the toolbar
switch to turn on automatic recognition; File menu still has "OCR current page"
/ "OCR all pages".
