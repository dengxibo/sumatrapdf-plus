# SumatraPDF Plus 3.7.39

本版集中改进标注编辑、问问 AI、目录识别以及四主题界面的一致性。

## 标注与侧栏

- PDF 与 EPUB 标注编辑栏共用操作布局。删除按钮放在标注行右侧，悬停或选中时显示；导出笔记靠左，保存与另存为靠右，中间空间随栏宽伸缩。
- EPUB 标注改为手动保存：新增、修改、删除不会立即写入标注存储；关闭有未保存修改的文档时提示保存。标注仍存于原有外部存储，不改 EPUB 正文。
- 矩形使用圆角，拖动绘制时也显示圆角；PDF 与 EPUB 的形状预览及线宽更一致。
- 自由文本新建默认字号为 21、边框为 1；颜色选择增加色样，线段起终点增加形状预览。
- 工具栏加入图章入口；不支持的功能保留显示并禁用，而不是隐藏。
- 目录编辑期间切换其他侧栏后，可正常缩窄，不再继承目录编辑的最小宽度限制。缩略图栏不再挂目录编辑右键菜单。

## 问问 AI 与目录识别

- 问问 AI 位于侧栏，支持文档内继续提问；输入框支持 Enter 发送、Shift+Enter 换行和最多五行自动伸展。
- 标题右侧可临时选择当前 API 服务的模型。选择不写入选项配置，也不影响 API 目录识别所使用的模型。
- 去掉“当前文档”固定标签；完整提问及所选文本直接显示在会话中。AI 入口换为简洁机器人图标。
- 网页 AI 与 API 印刷目录识别共用核心规则，包括无页码标题、层级、同行条目拆分、公式和主目录筛选。网页保留 JSON，API 保留分阶段 CSV。
- API 不再重复发送整段提取提示词，也不在提取后立即猜填缺失页码；保留 null 和目录阅读顺序，交由后续校准处理。
- 调整 API 识别进度文字与计数的间距。

## 界面与阅读

- 多个原生／自制对话框统一应用字体、字号、按钮尺寸规则和四种主题面板配色，包括字体、设置、目录提取、收藏夹、页面调整和 PDF 保存提示。
- 自制 PDF 保存提示增加透明底主题信息图标；收藏夹提示行高与输入框间距修正。
- Dracula 与 Dark 菜单采用各自主题颜色，并统一暗色菜单绘制，减少打开闪烁及悬停时的文字跳动；模型菜单使用同一处理。
- 工具栏固定快速查找框，详细查找使用浮动面板；优化字体搜索框布局及侧栏图标选中底色。
- 优化暗色 PDF 标注颜色、绘制后显示、拖动与缩放，以及 EPUB 手写签名支持。

## 验证与说明

- 提示词共用及格式包装的离线回归测试已通过；实际识别结果仍受模型、图片质量和批次上下文影响，并不保证 Web/API 输出逐条相同。
- 已有自定义字号等高级设置保持有效，新默认值不覆盖用户配置。
- Windows x64 Release EXE。基于 SumatraPDF 的非官方 Plus 版本，GPLv3；未签名的 EXE 可能触发 Windows SmartScreen 提示。

---

## English release notes

This release focuses on annotation editing, Ask AI, TOC recognition, and consistent UI styling across the four application themes.

### Annotations and sidebar

- PDF and EPUB annotation editors share the same layout. Delete appears at the right edge of a hovered or selected annotation row. Export Notes stays on the left; Save and Save as stay on the right, with flexible space between the groups.
- EPUB annotations now use manual saving. Creating, editing, or deleting an annotation no longer immediately writes the annotation store. Closing a document with unsaved annotation changes prompts you to save. Annotations remain in the existing external store; the EPUB document itself is not rewritten.
- Rectangles use rounded corners, including while drawing. Shape previews and stroke widths are more consistent between PDF and EPUB.
- New free-text annotations default to font size 21 and border width 1. Color selectors show swatches, and line-ending selectors show shape previews.
- The toolbar includes a stamp tool. Unsupported toolbar actions remain visible but disabled instead of being hidden.
- Switching away from TOC editing restores normal sidebar resizing: the TOC editor's minimum width no longer constrains other sidebar pages. Thumbnail panes no longer display TOC-edit context menus.

### Ask AI and TOC recognition

- Ask AI lives in the sidebar and supports follow-up questions within the current document. Enter sends, Shift+Enter inserts a line break, and the composer grows to a maximum of five visible lines.
- The model picker beside the heading lists models from the current API provider. Your selection is session-only: it does not change the model saved in Options or the model used for API-based TOC recognition.
- The redundant “Current document” label has been removed. The conversation shows the complete question and selected passage. The AI entry uses a minimal robot icon.
- Web AI and API printed-TOC recognition now share core rules for headings without page numbers, hierarchy, multiple entries on one line, mathematical expressions, and filtering the main TOC. Web AI retains JSON output; the API retains its staged CSV workflow.
- The API no longer sends the full extraction prompt twice or immediately guesses missing page numbers after extraction. Missing numbers remain null, and entries retain TOC reading order for subsequent calibration.
- Adjusted spacing between API recognition progress text and its counter.

### Interface and reading

- Multiple native and custom dialogs share application fonts, font sizes, button-sizing rules, and panel colors across all four themes. These include font selection, settings, TOC extraction, favorites, page adjustment, and the PDF save prompt.
- The custom PDF save prompt includes a transparent-background, theme-aware information icon. The favorite dialog's label height and spacing above the input field have been corrected.
- Dracula and Dark menus use distinct theme colors. Consistent dark-menu rendering reduces opening flicker and hover-related text shifts; the model menu uses the same handling.
- Quick Find stays at the right side of the toolbar, with Detailed Search in a floating panel. Font-search layout and sidebar selection backgrounds have also been refined.
- Improved dark-theme PDF annotation colors, drawing feedback, dragging and resizing, and handwritten-signature support for EPUB.

### Validation and compatibility

- Offline regression tests for shared prompt rules and output-format wrappers passed. Actual recognition still depends on the model, image quality, and batch context; Web and API results are not guaranteed to match item for item.
- Existing custom settings, including font sizes, remain effective. New defaults do not overwrite user configuration.
- Windows x64 Release EXE. This is an unofficial Plus version based on SumatraPDF, licensed under GPLv3. The unsigned executable may trigger a Windows SmartScreen warning.
