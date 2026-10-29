# SumatraPDF Plus 3.7.82

## 中文

- 修复 InDesign 等原生竖排 PDF 双击取词错位到上一个字的问题：精确字框使用真实字形轮廓，并避免旧重排文字补偿再次移动坐标。
- 修复竖排 PDF 新建下划线、删除线、波浪线方向错误及波浪异常放大的问题，按选中文字的实际书写方向绘制。
- 横排扫描 PDF 的新建下划线、波浪线根据实际字迹下沿和相邻行间距定位，覆盖程序 OCR 和文件自带的隐藏 OCR 文字层；行距紧密时缩小波浪幅度，避免压字或跑到下一行。
- 修复 EPUB 下划线偏高、压住字底的问题，并同步调整线条的点击范围。
- 上述 PDF 位置调整针对新建标注，高亮和已有 PDF 标注保持原样。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.82/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置及配套文件，重启后生效。

## English

- Fix double-click lookup selecting the preceding character in native vertical PDFs, including InDesign exports. Use actual glyph outlines for accurate bounds and avoid applying the legacy reflow adjustment again.
- Orient new underlines, strikeouts and squiggles along vertical PDF text, avoiding short horizontal strokes and oversized waves.
- Position new OCR underlines and squiggles using visible ink and neighboring line spacing, including existing invisible OCR layers. Reduce wave amplitude in tight line spacing to avoid crossing into the next line.
- Place EPUB underlines below the text and extend their clickable area accordingly.
- PDF placement changes apply to new annotations; highlights and existing PDF annotations are preserved.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.82/SumatraPDF-Plus.exe). Close the application before replacing the EXE; retain settings and supporting files, then restart.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.

[Third-party dependency notices](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.82/THIRD-PARTY-NOTICES.txt).
