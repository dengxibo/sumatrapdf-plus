Official ONNX Runtime C API header (Apache-2.0), used only for LoadLibrary of
ocr/onnxruntime.dll. Do not link onnxruntime into SumatraPDF.exe.

Source: https://github.com/microsoft/onnxruntime
Header version: 1.20.1 (ORT_API_VERSION in onnxruntime_c_api.h)
Ship the matching CPU x64 DLL (and onnxruntime_providers_shared.dll) in ocr/.
PP-OCRv6 RapidOCR ONNX uses IR 10; ORT 1.18+ loads that natively.
