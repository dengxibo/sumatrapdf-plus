# SumatraPDF Plus 3.7.52

## 中文

本版改进 PDF 批注松开鼠标结束绘制时的稳定性。

- PDF 直线、任意线、方框和圆形完成后不再自动选中或显示编辑外框；点击批注仍可选中编辑。
- 先准备正式批注外观缓存，再更新侧栏，避免选中新批注时提前刷新整页，造成短暂消失和重新出现。
- 页面渲染完成前复用正式外观缓存，避免整窗擦除重绘。
- 新建圆形和方框按最终线宽重新计算边缘内缩，减少预览与落下后尺寸的差异。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.52/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型和配套文件。

增强模式的颜色处理保持原有行为。语音适配器附件与上一版相同：[替换说明、许可证和对应源码](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md)。语音 ZIP 和 x64/x86 DLL 为可选附件，不要放入 SumatraPDF 安装目录。

已完成构建、圆形/方框边缘内缩回归及已有单元测试。根据录屏修正刷新顺序；最新界面行为尚未完成完整人工验收。

## English

- Leave completed PDF lines, ink, rectangles and ellipses unselected without edit frames. Click an annotation to select and edit it.
- Prepare cached final appearances before updating the sidebar, avoiding an early page refresh and transient disappearance on selection.
- Keep final appearance previews visible during background rendering without erasing the entire window.
- Recompute new ellipse and rectangle insets from their final stroke widths to reduce geometry differences on release.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.52/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

Enhanced-mode color handling is unchanged. Optional speech adapter assets are unchanged; see [instructions, licenses and source links](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md).

Builds, shape-inset regression checks and existing unit tests passed. Refresh ordering was corrected using the recorded behavior; full interactive verification of the latest changes remains pending.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
