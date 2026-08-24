# OCR · 扫描页识别

Sumatra PDF Plus can recognize **scanned pages** (little or no text layer) so you can select, search, look up words, read aloud, and extract bookmarks. Engine: **RapidOCR / PP-OCR** Chinese mobile ONNX, fully offline.  
可识别**几乎没有文字层的扫描页**，便于选择、搜索、查词、朗读和提取书签。引擎为 **RapidOCR / PP-OCR** 中文 mobile ONNX，**完全离线**。

## Setup · 准备

Keep the `ocr` folder next to `SumatraPDF-Plus.exe` (it is included in the release zip). Typical files:  
请保留 exe 同目录的 `ocr` 文件夹（发行包已带）。通常包括：

- `onnxruntime.dll`
- `det.onnx` / `rec.onnx` (and optional `cls.onnx`)
- `keys.txt` or `ppocr_keys_v1.txt`

If this folder is missing or incomplete, OCR commands show a hint and do nothing. Do not copy only the exe.  
缺少模型时命令会提示且不会识别。不要只复制 exe。

Models are RapidOCR builds of Baidu Paddle PP-OCR. See `ocr/README.txt` in the zip if you need to replace them.  
模型来自 RapidOCR 的 PP-OCR ONNX。自行更换时见发行包内 `ocr/README.txt`。

## How to use · 使用方法

Toolbar **Auto OCR** (before Read Aloud). Click to toggle; use the dropdown for the rest.  
工具栏 **自动 OCR**（在朗读按钮前）。单击开关；下拉菜单为其余命令。

| Action | 中文 | What it does |
| --- | --- | --- |
| Auto OCR | 自动 OCR | Recognize scanned pages as you view or search them. Default **off**. Setting: `AutoOcrScanPages`. |
| Recognize all pages | 全文识别 | Re-OCR every page even if a text layer already exists (useful for garbled dual-layer PDFs). Stays in memory for this session unless you save. If there is no outline, bookmarks are extracted after OCR. |
| Recognize all pages and save | 全文识别并保存 | Same scan with progress, then **overwrite the current PDF** (no Save As). Prompts before replacing an existing text layer or outline. |
| OCR region | 框选识别 | Drag a rectangle; recognized text is copied. |
| Cancel OCR | 取消 OCR | Stop queued page jobs; pages already done are kept. |

File menu also has OCR current page / OCR all pages.  
文件菜单里也有「识别当前页 / 全文识别」。

**Recognize all pages and save** is PDF-only. Auto OCR / recognize / region also work on DjVu, images, and comic books.  
「全文识别并保存」仅 PDF。自动识别 / 全文识别 / 框选也适用于 DjVu、图片和漫画。

## After OCR · 识别之后

You can search (`Ctrl+F`), select text, use the offline dictionary, Read Aloud (TTS), and extract bookmarks from a printed table of contents.  
之后可搜索（`Ctrl+F`）、选中文字、离线查词、朗读，以及从印刷目录提取书签。

Session recognition (Auto OCR / 全文识别) is **in memory** until you save. Close the tab without saving and the hidden text layer is not written to disk.  
自动 OCR / 全文识别的结果先放在**本次打开的文档内存**里；不保存就关标签，不会写回文件。

## Settings · 设置

```
AutoOcrScanPages = false
```

See [Advanced options](Advanced-options-settings.md) and commands in [Commands.md](Commands.md) (`CmdToggleAutoOcr`, `CmdOcrDocument`, `CmdSaveSearchablePdf`, `CmdOcrRegion`, `CmdOcrCancel`, `CmdOcrCurrentPage`).
