# SumatraPDF Plus 3.7.41

## 中文

本版可以在页面上直接编辑 PDF 自由文本，显示增强可用于 EPUB，并修正连续阅读滚到顶部时的跳动。全文识别不再自行旋转或倾斜校正页面。

- PDF 自由文本可以在页面上直接输入和修改：新建后输入，双击编辑，Ctrl+Enter 提交，Esc 取消。编辑框与页面使用同一背景、边框颜色和粗细；边框颜色可单独设置。字体选择记住最近三种。浮动工具栏与划词工具栏外观一致。
- 显示增强也可用于 EPUB 等原生文字图书。图片和漫画仍然关闭。
- 修复连续阅读 PDF 时，高缩放下向上滚到文档顶部会跳到当前页底部并反复抖动的问题（#101）。
- 电子书调整字号时，进度条一直保持到当前页面按新字号画完。
- 「识别所有扫描页（快速／精确）」不再自动旋转页面，也不再对页面做倾斜校正。识别仍可在内部使用方向和倾斜来提高准确度，文字写回原来的页面。手动「倾斜校正」和「倾斜校正全部扫描页」保留。识别菜单去掉「识别时自动校正倾斜」。
- 显示或隐藏侧栏时，工具栏搜索框不再一次次向左移动。
- 再点工具栏搜索框上的「…」会关闭详细查找，查找文字留在工具栏里。
- 手写签名窗口的底色与标题栏一致，签名纸保留原来的颜色。
- 标注「恢复默认」会把所选标注恢复为出厂外观，并把该外观记成这一类型下次新建时的样式。

### 下载

[SumatraPDF-Plus.exe（Windows x64 Release）](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.41/SumatraPDF-Plus.exe)

已有完整程序目录的用户可退出程序后替换 EXE。字典、OCR 及其他配套数据继续使用原有文件。

基于 SumatraPDF 的非官方 Plus 版本，GPLv3。

---

## English

This release adds on-page PDF free-text editing, extends Enhance Display to EPUB and other native-text books, and fixes a jump when scrolling to the top of a continuous PDF. Recognize All no longer rotates or deskews the page.

- Edit PDF free text directly on the page: type after creating it, double-click to edit, Ctrl+Enter to apply, and Esc to cancel. The editor matches the page background, border color, and border width. Border color is separate from the text color. The font picker keeps the three most recent fonts. The floating toolbar matches the selection toolbar.
- Enhance Display also works on EPUB and other native-text books. Pictures and comics stay off.
- Fix upward scrolling at the document top jumping to the current page bottom and jittering at high zoom in continuous PDF view (#101).
- The ebook font-size progress bar stays up until the current page has been painted at the new size.
- Recognize All Scanned Pages (Fast / Accurate) no longer rotates or deskews the PDF page. Orientation and deskew may still be used internally, and the text is mapped back onto the original page. Deskew Page and Deskew All Scanned Pages remain. The Deskew during OCR menu item is removed.
- Showing or hiding the sidebar no longer shifts the toolbar search field to the left.
- Clicking the toolbar search “…” again closes Detailed Search and leaves the query in the toolbar.
- The handwritten-signature window matches the title bar; the signature paper keeps its own color.
- Restore Defaults resets the selected annotation to the factory appearance and remembers that appearance for the next annotation of the same type.

### Download

[SumatraPDF-Plus.exe (Windows x64 Release)](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.41/SumatraPDF-Plus.exe)

Users with an existing complete program folder can close the application and replace its EXE. Keep the existing dictionary, OCR, and other supporting data files.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
