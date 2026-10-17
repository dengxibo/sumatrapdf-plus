# SumatraPDF Plus 3.7.59

## 中文

- 同步上游中文注释修复：按文字内容选择字符集合，并为日语等语言设置下缺失的汉字切换可用映射，避免自由文本部分汉字不显示。

- 修复启用 LaTeX 增强功能时，双击 EPUB 文字弹出“无法打开同步文件”而无法查词的问题。反向搜索仅处理 PDF。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.59/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型及配套文件。

## English

- Port upstream CJK annotation fixes: infer the text language and fall back to a character collection that covers each glyph, including PDFs marked as Japanese.

- Restrict inverse search to PDF so double-click word lookup in EPUB works with TeX enhancements enabled, without an unrelated synchronization error.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.59/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

CJK annotation fix ported from upstream commit `e4b14348b` (#6082), preserving the local Chinese wrapping and CP-1250 fixes.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
