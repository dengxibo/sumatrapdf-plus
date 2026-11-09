# SumatraPDF Plus 3.7.89

## 中文

- OCR 文字与图片链接重叠时优先选字，避免单击、拖动选字误触整图跳转；图片空白处及普通文字链接保留跳转。
- 新增“定位当前章节”，可从目录顶部、查看菜单或命令面板清除筛选、展开并选中当前目录项，同时保持正文阅读位置。
- 缩小侧栏分隔条命中容差，避免抢占目录关闭按钮、搜索框与滚动条。
- 修复 PDF 中 Ctrl+触控板、远程桌面捏合或高精度滚轮缩放过快的问题（#123）。默认分级缩放现在累计滚轮 delta，每满 120 才跳一级，保留不足一格的余量。
- 保持普通滚轮每格一级的操作方式；正百分比缩放模式继续按 delta 大小计算。

下载 Windows x64 EXE。退出程序后替换安装目录中的 EXE，保留配置及配套文件。

## English

- Prefer OCR text selection over overlapping full-image links; preserve navigation from blank image areas and ordinary text links.
- Add Locate Current Chapter to the TOC header, View menu, and command palette. Clear filters, reveal and select the current entry while preserving the reading position.
- Narrow the sidebar splitter hit area so it no longer intercepts nearby buttons, search fields, and scrollbars.
- Fix excessively fast PDF zoom with Ctrl+touchpad scrolling, remote desktop pinch gestures, and high-resolution wheels (#123). Default stepped zoom now accumulates wheel delta and advances one level per 120 units, preserving partial-notch input.
- Retain one zoom level per normal wheel notch and delta-proportional behavior for positive ZoomIncrement settings.

Download the Windows x64 EXE. Close the application before replacing the EXE; retain settings and supporting files.
