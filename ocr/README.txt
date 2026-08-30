RapidOCR / PP-OCR sidecar for SumatraPDF Plus
=============================================

Scan-only pages (little or no text layer) can be recognized so you can select
and search. Results stay in memory for this open document unless you use
"Recognize all pages and save" (toolbar OCR menu), which writes a searchable
PDF over the current file.

User guide: docs/md/OCR.md

Put these files in this folder (next to SumatraPDF-Plus.exe after a build they are
also copied to out/dbg64/ocr and out/rel64/ocr):

  onnxruntime.dll                      ONNX Runtime CPU, Windows x64 (1.20.x)
  onnxruntime_providers_shared.dll     required sibling of onnxruntime.dll 1.20+

  RapidOCR's PP-OCRv6 ONNX files are ONNX IR 10. Ship ORT 1.18+ so they load
  without rewriting the model header.

Recommended default models
--------------------------

Fast / full-document OCR (OCR all pages, Recognize all pages and save):

  PP-OCRv6_det_tiny.onnx
  PP-OCRv6_rec_tiny.onnx
  ppocrv6_tiny_dict.txt

Balanced / current-page OCR:

  PP-OCRv6_det_small.onnx
  PP-OCRv6_rec_small.onnx
  ppocrv6_dict.txt

Experimental Hybrid (small detection + tiny recognition; not a default):

  PP-OCRv6_det_small.onnx
  PP-OCRv6_rec_tiny.onnx
  ppocrv6_tiny_dict.txt

Recognition model and dictionary MUST match. Do not mix:

  v6 tiny rec  + ppocrv6_tiny_dict.txt
  v6 small rec + ppocrv6_dict.txt

The engine pairs them; a missing requested profile falls back
(Fast → Balanced) and logs the files it actually loaded.

Generic filenames
-----------------

  rec.onnx still works, but then keys.txt is required. The engine will not
  guess a v6 tiny / v6 small dictionary for an unknown rec.onnx.

  det.onnx is accepted as a generic detector with rec.onnx + keys.txt.

Optional:

  cls.onnx                 0/180 angle classifier

Where to get them
-----------------
Models are RapidOCR ONNX builds of Baidu Paddle PP-OCR (Chinese).

  https://github.com/RapidAI/RapidOCR
  python/rapidocr/default_models.yaml  (PP-OCRv6 tiny/small ONNX + dicts)

  https://github.com/microsoft/onnxruntime/releases  (1.20.x CPU x64)

Do not ship PP-OCRv6 medium, Python, or PaddlePaddle.

Advanced setting: AutoOcrScanPages (default false). Set true or use the toolbar
switch to turn on automatic recognition; File menu still has "OCR current page"
/ "OCR all pages".
