# SumatraPDF Plus 3.7.51

## 中文

本版改进 PDF 和 EPUB 批注的绘制与拖动体验。

- **自由文本字体：** 修复所选 Windows 字体无法嵌入 PDF 外观的问题，避免编辑时与退出编辑后的字体不一致或文字不显示。
- **拖动缩放：** PDF 图形、直线、印章和手写批注复用缓存预览，松开后才更新正式外观；自由文本使用实时换行预览。EPUB 缩放移除不必要的整页渲染。
- **Shift 约束：** 绘制直线时保持水平或竖直；矩形、椭圆保持正方形、正圆。调整矩形、椭圆尺寸时也支持等宽高约束。
- **图章即时显示：** 落下后显示缓存图章，避免只看到空选框等待整页渲染。
- **线宽一致：** PDF 和 EPUB 的直线、任意线、圆形、方框预览读取最终使用的线宽，按缩放或 DPI 换算，减少松开鼠标后的粗细跳变。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.51/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型和配套文件。

语音适配器附件沿用 3.7.50 的非官方修复版本：[替换方法、许可证、对应源码和验证记录](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md)。ZIP 和单独的 x64/x86 DLL 均为可选附件，不要放入 SumatraPDF 安装目录。

本轮字体生成回归、Shift 几何检查和已有单元测试通过；鼠标手感与界面效果尚未完成完整人工验收。拖动图形缩放期间，缓存预览的线宽可能暂时随尺寸变化，松开后恢复正式线宽；旋转页面的自由文本使用缓存图像预览。

## English

- Allow selected Windows fonts to be embedded in PDF free-text appearances.
- Reuse cached PDF shape, line, stamp and ink previews during resize and commit on release. Use live wrapping for unrotated free text; avoid unnecessary EPUB page renders during resize.
- Hold Shift to draw horizontal/vertical lines, squares and circles. Shift also constrains rectangle/ellipse resizing to equal width and height.
- Display stamps immediately using a cached appearance while page rendering completes.
- Match PDF and EPUB drawing preview widths to the final annotation widths, including remembered styles and zoom/DPI conversion.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.51/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

Optional speech adapter assets are unchanged from 3.7.50; see its [instructions, licenses and source links](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md).

Font appearance generation, Shift geometry checks and existing unit tests passed; full interactive verification remains pending. Cached resize previews may temporarily scale stroke thickness; final stroke widths are restored on release.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
