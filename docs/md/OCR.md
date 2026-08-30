# OCR · 扫描页识别

Sumatra PDF Plus can recognize **scanned pages** (little or no text layer) so you can select, search, look up words, read aloud, and extract bookmarks. Engine: **RapidOCR / PP-OCRv6** Tiny/Small ONNX, fully offline.  
可识别**几乎没有文字层的扫描页**，便于选择、搜索、查词、朗读和提取书签。引擎为 **RapidOCR / PP-OCRv6** Tiny/Small ONNX，**完全离线**。

## Setup · 准备

Keep the `ocr` folder next to `SumatraPDF-Plus.exe` (it is included in the release zip). Typical files:  
请保留 exe 同目录的 `ocr` 文件夹（发行包已带）。通常包括：

- `onnxruntime.dll` (ONNX Runtime **1.20.x** CPU, plus `onnxruntime_providers_shared.dll`)
- **Fast** (full-document OCR): `PP-OCRv6_det_tiny.onnx` + `PP-OCRv6_rec_tiny.onnx` + `ppocrv6_tiny_dict.txt`
- **Balanced** (current-page OCR): `PP-OCRv6_det_small.onnx` + `PP-OCRv6_rec_small.onnx` + `ppocrv6_dict.txt`
- Optional Hybrid: small det + tiny rec + `ppocrv6_tiny_dict.txt`

The recognition model and its dictionary must match. Generic `rec.onnx` requires `keys.txt` (the engine will not guess a v6 dict).  
识别模型和字典必须成对；不要混用 tiny / small 字典。通用 `rec.onnx` 必须配 `keys.txt`。

If this folder is missing or incomplete, OCR commands show a hint and do nothing. Do not copy only the exe.  
缺少模型时命令会提示且不会识别。不要只复制 exe。

Models are RapidOCR builds of Baidu Paddle PP-OCR. See `ocr/README.txt` in the zip if you need to replace them.  
模型来自 RapidOCR 的 PP-OCR ONNX。自行更换时见发行包内 `ocr/README.txt`。

## How to use · 使用方法

Toolbar **Auto OCR** (before Read Aloud). Click the icon to toggle; the arrow opens the rest.  
工具栏 **自动 OCR**（在朗读按钮前）。单击图标开关；箭头打开其余命令。

| Action | 中文 | What it does |
| --- | --- | --- |
| Auto OCR | 自动 OCR | Click the OCR icon to toggle. On: recognize **visible** scanned pages as you browse (high accuracy). Does **not** OCR the whole file. Default **off**. Setting: `AutoOcrScanPages`. |
| OCR region | 框选识别 | Drag a rectangle; recognized text is copied. High accuracy. |
| Recognize All Scanned Pages (Fast) | 识别所有扫描页（快速） | Clear this session's OCR results and re-recognize **every** page with the fast profile. Then extract bookmarks (in memory). Progress and cancel. |
| Recognize All Scanned Pages (Accurate) | 识别所有扫描页（精确） | Same as Fast, using high accuracy. Then extract bookmarks (in memory). |
| Auto-save | 自动保存 | Toggle. On: after Recognize All Scanned Pages, overwrite the current PDF (and extract bookmarks to disk). Extracting bookmarks also saves. Setting: `OcrAutoSave`, default **off**. |
| Cancel OCR | 取消 OCR | Stop queued page jobs; pages already done are kept. |

Auto OCR / region also work on DjVu, images, and comic books. Vertical books stay upright (columns are recognized in place). Landscape official forms may still get a per-page `/Rotate`.  
自动识别 / 框选也适用于 DjVu、图片和漫画。竖版书保持正立（按栏识别，不转页面）。公文横表仍可能按页写入 `/Rotate`。

## After OCR · 识别之后

You can search (`Ctrl+F`), select text, use the offline dictionary, Read Aloud (TTS), and extract bookmarks from a printed table of contents.  
之后可搜索（`Ctrl+F`）、选中文字、离线查词、朗读，以及从印刷目录提取书签。

Session recognition (Auto OCR / recognize all pages) is **in memory** until you save. Close the tab without saving and the hidden text layer is not written to disk.  
自动 OCR / 识别所有扫描页的结果先放在**本次打开的文档内存**里；不保存就关标签，不会写回文件。

## Settings · 设置

```
AutoOcrScanPages = false
OcrFullDocumentMode = fast
```

`OcrFullDocumentMode`: `fast` (default) or `accurate`. Interactive OCR (auto, region) always uses high accuracy. Recognize All Scanned Pages (Fast / Accurate) sets this and uses it. `SUMATRA_OCR_PROFILE=fast|balanced|hybrid` still overrides for debugging.

See [Advanced options](Advanced-options-settings.md) and commands in [Commands.md](Commands.md) (`CmdToggleAutoOcr`, `CmdOcrDocument`, `CmdOcrReRecognizeAllPages`, `CmdSaveSearchablePdf`, `CmdOcrRegion`, `CmdOcrCancel`, `CmdOcrFullDocumentModeFast`, `CmdOcrFullDocumentModeAccurate`).
