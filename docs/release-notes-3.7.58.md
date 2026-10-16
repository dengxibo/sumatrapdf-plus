# SumatraPDF Plus 3.7.58

## 中文

- 启动还原会话时跳过不存在的文件，避免生成载入失败的标签页。
- 修复 AI 流式回答时对话区与滚动条闪动；更新文本时保留阅读位置和选区，避免反复重排整个侧栏。
- 优化 AI 对话排版：等待提示改用小号正体，去掉问题的逐行灰底，调整正文字号、消息间距和输入框留白。
- 目录编辑恢复统一的展开/折叠箭头，简化页码框，并恢复较宽的保存和取消按钮。
- PDF 和 EPUB 的高亮显示统一为小圆角矩形，替代胶囊形两端。
- EPUB 暗黑模式高亮改为彩色背景、深色文字，与 PDF 保持一致。
- 修复暗黑模式下 PDF 高亮文字锯齿、图片原色模式下高亮变黑，以及标注被反色或丢失的问题。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.58/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型及配套文件。

语音适配器附件沿用上一版：[替换说明、许可证及对应源码](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md)。这些附件为可选组件，不要放入 SumatraPDF 安装目录。

## English

- Skip missing files when restoring a session instead of opening failed tabs.
- Stabilize streamed AI replies and scrollbar geometry while preserving the reading position and selection.
- Refine AI chat typography, use a compact upright waiting status, remove striped question backgrounds, and improve message and composer spacing.
- Restore consistent TOC editing chevrons, quieter page fields, and wider Save/Cancel buttons.
- Display PDF and EPUB highlights as subtle rounded rectangles instead of capsule-shaped bands.
- Match PDF highlights in dark EPUBs with colored backgrounds and dark text.
- Preserve PDF annotation colors and native highlight text antialiasing across dark image modes, including scanned pages.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.58/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

Optional speech adapter assets are unchanged; see [instructions, licenses and source links](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md).

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
