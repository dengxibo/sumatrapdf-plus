# SumatraPDF Plus 3.7.83

## 中文

- 优化原生文字 PDF 的选区和标注：按实际字形行框定位，避免高亮连块；新建下划线、波浪线与文字保持间隙，删除线居中。
- 修复 EPUB 英文空格把标注边界拉低的问题，统一行内文字边界；优化删除线和波浪线位置。
- 修复 OCR PDF 中个别行波浪线异常变细的问题，波幅、线宽和波长不再因局部行距检测而缩小。
- 修复竖排 PDF 创建下划线、波浪线、删除线时短暂闪现横排线条的问题。
- 修复自由文本边框改色后不立即生效，以及退出编辑后显示旧颜色的问题；新外观保存重开后保持一致。
- 修复自由文本即时工具栏上出现下层 PDF 插入符或其他错误鼠标光标的问题。

PDF 位置修正适用于新建标注；已有 PDF 标注保留原坐标。已有 EPUB 标注会按新规则重新绘制。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.83/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置及配套文件，重启后生效。

## English

- Use actual glyph line bounds for native PDF selection and markup, avoiding merged highlight bands. Position new underlines and squiggles below the text and center strikeouts.
- Fix EPUB spaces pulling markup below its intended line. Use consistent line bounds and refine strikeout and squiggle placement.
- Keep OCR PDF squiggle amplitude, stroke width and wavelength consistent with text size instead of shrinking individual rows after local spacing probes.
- Remove the brief horizontal stand-in when creating line markup on vertical PDF text; preview the actual annotation appearance from the first frame.
- Refresh free-text borders immediately and regenerate the PDF appearance after changing their color, including after leaving edit mode and saving/reopening.
- Keep the free-text property toolbar's arrow cursor independent of the underlying PDF canvas.

PDF placement changes apply to new annotations. Existing EPUB annotations are redrawn using the updated geometry.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.83/SumatraPDF-Plus.exe). Close the application before replacing the EXE; retain settings and supporting files, then restart.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.

[Third-party dependency notices](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.83/THIRD-PARTY-NOTICES.txt).
