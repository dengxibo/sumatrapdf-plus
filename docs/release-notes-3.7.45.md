# SumatraPDF Plus 3.7.45

## 中文

本版改善 Word 文档的目录、图片和大文件打开体验。

- **印刷目录：** 带可跳转印刷目录的 DOC/DOCX，侧栏目录优先按目录里的稳定锚点提取，不再依赖推断标题或印刷页码。
- **自动书签：** 隐藏 OLE_LINK、Excel 风格的 `RANGE!…` 等 Word 自动书签，正文与目录都不再显示这些标签。
- **编号兼容：** 兼容 `1.2.`、`1.2.1.` 这类带尾点的章节编号，避免印刷目录被提错。
- **WMF/EMF 图片：** 正确显示文档中的 WMF/EMF 图（含实际为 Aldus WMF 却命名为 `.emf` 的文件），不再只显示 `[image]` 占位符。
- **大文档打开更快：** 减少多余字体/颜色/字号标记，并对热路径做编译优化；例如大型验收报告从约两分钟降到大约四十秒量级。
- **滚动条：** 修复个别大型 DOCX 首次打开右侧没有滚动条、切换标签后才出现的问题。

### 下载

[SumatraPDF-Plus.exe（Windows x64 Release）](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.45/SumatraPDF-Plus.exe)

退出程序后替换完整程序目录内的 EXE，保留配置、字典、OCR 和其他配套数据。本次先提供 EXE，ZIP 由维护者另行上传。

## English

This release improves Word document tables of contents, images, and large-file open times.

- **Printed TOC:** for DOC/DOCX with a hyperlinked printed table of contents, the sidebar prefers those stable anchors instead of inferred headings or printed page numbers.
- **Auto bookmarks:** hide Word auto bookmarks such as OLE_LINK and Excel-style `RANGE!…` from both the body and the sidebar TOC.
- **Numbering:** accept trailing-dot section numbers such as `1.2.` and `1.2.1.` so printed TOCs are not misread.
- **WMF/EMF images:** show WMF/EMF pictures in Word documents, including Aldus placeable WMF files named `.emf`, instead of an `[image]` placeholder.
- **Faster large DOCX:** skip redundant font/color/size spans and optimize hot layout paths; a large acceptance report that took about two minutes now opens in roughly forty seconds.
- **Scrollbar:** fix some large DOCX files that had no vertical scrollbar on first open until another tab was selected.

[Download Windows x64 Release EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.45/SumatraPDF-Plus.exe)

Close the app before replacing the EXE in an existing complete installation. Keep settings, dictionary, OCR and supporting files. The EXE is provided first; the maintainer will upload a ZIP separately.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
