# SumatraPDF Plus 3.7.104

- 修复扫描 PDF 的 OCR 文字层保存后重新打开，文字选区高度变大、偏离扫描文字的问题。已经保存的 OCR 文字层可直接受益，无需重新识别。
- 优化「识别所有扫描页（快速）」和「识别所有扫描页（精确）」共用的处理流程：复用图像缩放采样坐标，使用像素归一化查表和 SIMD 解码，避免复制整块识别输出，并减少线程池空转竞争。
- 保留原有识别模型、分辨率和批次设置。9 页并发开发版样本中，两种模式的识别文本均与优化前逐字一致；实际耗时取决于硬件和文档。

## English

- Fix oversized or shifted text selection after saving and reopening scanned PDFs with OCR text. Existing saved OCR layers benefit without re-recognition.
- Optimize preprocessing, CTC decoding, output memory use, and thread-pool scheduling shared by fast and accurate document OCR.
- Keep the existing models, resolution, and batch settings. Both modes produced identical text in the nine-page concurrent development-build comparison; performance varies by hardware and document.
