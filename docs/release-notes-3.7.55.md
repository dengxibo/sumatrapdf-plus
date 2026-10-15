# SumatraPDF Plus 3.7.55

## 中文

- 目录校准中印刷页与 PDF 页无偏移时只显示箭头，不再显示“+0”。
- 修复同一窗口打开或切换多个文档时，缩略图沿用上一文档相同页码图片的问题（#110）。
- 补上缩略图异步渲染完成时的空标签检查。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.55/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型及配套文件。

语音适配器附件沿用上一版：[替换说明、许可证及对应源码](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md)。这些附件为可选组件，不要放入 SumatraPDF 安装目录。

构建及已有单元测试通过；尚未完成多文档切换的完整界面回归验证。

## English

- Show only the TOC calibration arrow for zero page offsets, without the "+0" label.
- Clear sidebar thumbnail caches when opening or switching documents in the same window, preventing images from the previous document from appearing at matching page numbers (#110).
- Check for a current tab when delivering an asynchronous thumbnail render.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.55/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

Optional speech adapter assets are unchanged; see [instructions, licenses and source links](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md).

Build and existing unit tests passed; full interactive document-switching regression verification remains pending.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
