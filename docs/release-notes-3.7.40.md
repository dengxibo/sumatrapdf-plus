# SumatraPDF Plus 3.7.40

## 中文

本版修复 EPUB 调整字号后的阅读位置跳动，优化四主题工具栏、全屏退出体验和暗色阅读，并补充界面翻译。

- EPUB 调整字号时保留适合宽度／适合单页等视图模式及原阅读文字，修复反复增减字号时内容向前漂移。
- 优化大型 EPUB 调整字号后的阅读位置恢复：直接查找章节文本，缓存定位结果并优先布局目标页，减少逐页提取文字及等待造成的延迟。
- 工具栏放大／缩小、字体＋／字体－改为大在前、小在后。
- 工具栏选中按钮采用圆角底色与柔和阴影，图标和下拉箭头间距更协调；更新朗读及 OCR 图标。
- 工具栏使用缓冲绘制，减少打开文档、更新按钮状态时的闪烁；初始化完成后再显示工具栏。
- 全屏／演示模式隐藏工具栏时，鼠标移到右上角即可显示退出按钮，单击退出。按钮适配四种主题，修正浅暖及暗色主题中的图标背景。
- 修复夜间模式下部分带软蒙版的彩色封面变灰的问题（#98）。
- 改善暗色菜单裁切和标注底部按钮布局，统一更多对话框与朗读、标注界面的主题样式。
- PDF 未保存提示出现时仍可使用工具栏主题按钮；补充主要语言的界面翻译。

### 下载

[SumatraPDF-Plus.exe（Windows x64 Release）](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.40/SumatraPDF-Plus.exe)

已有完整程序目录的用户可退出程序后替换 EXE。字典、OCR 及其他配套数据继续使用原有文件。

基于 SumatraPDF 的非官方 Plus 版本，GPLv3。

---

## English

This release fixes reading-position jumps after EPUB font-size changes and refines the toolbar, fullscreen exit controls, dark reading, and UI translations.

- EPUB font-size changes preserve Fit Width / Fit Single Page and the original reading text, fixing backward drift when repeatedly increasing and decreasing the size.
- Faster reading-position restoration after font-size changes in large EPUBs: search chapter text directly, cache the result, and prioritize the target page, reducing delays from extracting every preceding page and waiting for UI layout batches.
- Zoom In / Zoom Out and Font + / Font − now put the larger-size action first.
- Selected toolbar buttons use rounded backgrounds and soft shadows, with improved spacing between icons and dropdown arrows. Read Aloud and OCR icons have been updated.
- Buffered toolbar painting reduces flicker when opening documents and updating button states. The toolbar is shown after initialization finishes.
- With the toolbar hidden in fullscreen or presentation mode, move the mouse to the top-right corner and click the exit button. Its colors follow all four themes, with corrected icon backgrounds in warm and dark themes.
- Fixed colorful covers with soft masks turning gray in night mode (#98).
- Improved dark-menu clipping and annotation footer button layout, and aligned more dialogs, Read Aloud controls, and annotation controls with the active theme.
- The toolbar theme button remains usable while the unsaved-PDF prompt is open. Added missing UI translations for major languages.

### Download

[SumatraPDF-Plus.exe (Windows x64 Release)](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.40/SumatraPDF-Plus.exe)

Users with an existing complete program folder can close the application and replace its EXE. Keep the existing dictionary, OCR, and other supporting data files.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
