# SumatraPDF Plus 3.7.89

## 中文

- 修复 PDF 中 Ctrl+触控板、远程桌面捏合或高精度滚轮缩放过快的问题（#123）。默认分级缩放现在累计滚轮 delta，每满 120 才跳一级，保留不足一格的余量。
- 保持普通滚轮每格一级的操作方式；正百分比缩放模式继续按 delta 大小计算。

下载 Windows x64 EXE。退出程序后替换安装目录中的 EXE，保留配置及配套文件。

## English

- Fix excessively fast PDF zoom with Ctrl+touchpad scrolling, remote desktop pinch gestures, and high-resolution wheels (#123). Default stepped zoom now accumulates wheel delta and advances one level per 120 units, preserving partial-notch input.
- Retain one zoom level per normal wheel notch and delta-proportional behavior for positive ZoomIncrement settings.

Download the Windows x64 EXE. Close the application before replacing the EXE; retain settings and supporting files.
