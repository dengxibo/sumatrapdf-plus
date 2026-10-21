# SumatraPDF Plus 3.7.63

## 中文

- 修复扫描 PDF 的文字选区与新建高亮范围不一致，以及页边栏、正文和旋转页面的行列误合并。保留已有批注坐标，长选区不再受旧 512 个矩形缓冲区限制。
- PDF 矩形、圆形的标注属性支持 0–100%「不透明度」，同时影响边框与填充；更换边框颜色保留不透明度。
- PDF 矩形、圆形、直线和墨迹支持 0.25 pt 步长的小数线宽，并按标注类型记住样式。
- 在图片裁剪选框内按住鼠标中键即可移动；原有左键重新框选和空格加拖动操作保持可用。
- 补充四主题、多语言、模拟 DPI 及原生标注属性回归。

## English

- Align new PDF highlights with the visible text selection. Avoid merging scan margins, body text and neighboring columns on rotated pages; retain existing annotation coordinates and support selections exceeding 512 quads.
- Add 0–100% opacity for PDF rectangles and circles, affecting both stroke and fill. Preserve opacity when changing the border color.
- Support fractional stroke widths in 0.25 pt steps for PDF rectangles, circles, lines and ink, with remembered styles per annotation type.
- Move image crop selections with the middle mouse button while retaining left-drag replacement and Space-drag movement.
- Add native annotation-property, four-theme, multilingual and simulated DPI regression coverage.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.63/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

Validation: six independent scanned documents, digital controls and a cropped/rotated derivative; 432 render combinations; independent PDFium opacity checks; 40 native inspector theme/language/simulated DPI cases. Physical mixed-DPI monitor dragging remains a manual check. Incorrect positions in a damaged source OCR layer may still require recognizing the page again.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
