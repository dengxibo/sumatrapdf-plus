# SumatraPDF Plus 3.7.108

## 下载与更新

**新用户请先到 [3.7.107 发布页](https://github.com/dengxibo/sumatrapdf-plus/releases/tag/3.7.107)下载完整包，解压后，再用本版 `SumatraPDF-Plus.exe` 替换其中的同名文件。**

- 完整程序包：`Sumatra.PDF.Plus.MainProgram.zip`。
- 需要词典的用户可选择：`Sumatra.PDF.Plus.MainProgram+dictionary.zip`。
- 已有完整程序目录的用户：退出程序后替换 EXE，保留原有设置、OCR 模型、词典和其他配套资源。本版仅提供 EXE 更新文件。

## 本版改进

- 优化选项左侧分类导航：移除列表外框，增加留白，以柔和底色和强调线标记当前页，适配四种主题，保留键盘导航。
- 对话框单行输入框的文字、选区和插入点统一垂直居中；语言选择和在线语音设置中的输入框略微收紧。
- 搜索输入框的插入点光标按字号取高度、垂直居中，保留上下留白。
- EPUB 等重排电子书首次打开默认连续双页显示；已有阅读记录继续恢复上次的显示模式。
- 单页默认字号 11 pt，双页默认字号 17 pt；每本书分别保存两种字号，切换模式时恢复对应值，重置也只影响当前模式。高级设置 `EBookUI.SinglePageFontSize` / `EBookUI.FacingFontSize` 可调整两个默认值。
- 字号范围调整为 8–32 pt，兼顾阅读舒适度与调整空间。
- 普通文字的选区反显、查找、朗读和新建高亮，默认高度比例从 1.35 调整为 1.0，减少上下留白。
- OCR 使用独立的 `FixedPageUI.OcrSelectionHeightRatio`，默认保留 1.35，避免扫描文字的高亮过窄；支持 OCR 缓存和扫描 PDF 的隐藏文字层。
- 已有 `FixedPageUI.SelectionHeightRatio` 设置继续保留；希望使用较薄选区的用户可将其设为 1.0。
- 已保存的高亮和下划线、删除线、波浪线的定位不变。

## English

New users: download a complete package from [3.7.107](https://github.com/dengxibo/sumatrapdf-plus/releases/tag/3.7.107), extract it, then replace its executable with this release's `SumatraPDF-Plus.exe`. Existing users can replace only the EXE after closing the app, keeping settings and supporting resources.

- Reduce the default native text selection, find, read-aloud and new highlight height ratio from 1.35 to 1.0.
- Open new reflowable ebooks in continuous facing view. Use independent 11 pt single-page and 17 pt facing defaults, remember both sizes per book, and allow adjustments from 8 to 32 pt.
- Keep OCR compensation at 1.35 through the independent `FixedPageUI.OcrSelectionHeightRatio` setting, including cached OCR and hidden text layers in scanned PDFs.
- Existing `FixedPageUI.SelectionHeightRatio` values are preserved; set it to 1.0 to use thinner bands.
- Saved highlights and underline, strikeout and squiggly positioning are unchanged.
