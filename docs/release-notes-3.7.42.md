# SumatraPDF Plus 3.7.42

## 中文

本版改善跨页渲染调度和滚轮导航，整理选项窗口，并统一常用提示窗口的外观。

- 跨页渲染调度保留可见页面任务，已有缓存的页面不会反复取消相邻页预渲染，减少快速滚动时页面等待渲染的情况。
- 选项窗口支持调整大小，左侧分类和底部按钮固定，右侧内容独立滚动；收紧分组空白，移除重复分类标题与外层边框。“详细设置”改为“高级”。键盘导航会自动显示获得焦点的设置项。
- 新增垂直滚轮行数设置：0 跟随 Windows，1–100 自定义。放大单页与连续阅读使用一致的行高计算；保留整页翻页及横向滚动规则。修正连续阅读中明确请求上一页页尾时的导航。
- PDF 与电子书的未保存更改提示采用更简洁的布局；标题直接询问是否保存，移除多余的默认按钮内框。
- 自由文本标注在列表中直接显示文本摘要。
- AI 识别目录的处理提示改用普通句点动画。
- 语言筛选框采用普通边框，取消底部蓝色强调线；修复暗黑菜单子菜单箭头上下不对称。

### 下载

[SumatraPDF-Plus.exe（Windows x64 Release）](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.42/SumatraPDF-Plus.exe)

已有完整程序目录的用户可退出程序后替换 EXE，保留原有字典、OCR 及其他配套数据。本次仅上传 EXE。

基于 SumatraPDF 的非官方 Plus 版本，GPLv3。

---

## English

This release improves page rendering scheduling and wheel navigation, reorganizes Options, and simplifies common dialogs.

- Preserve visible-page render requests and keep neighbor prefetch running for cached pages, reducing rendering waits during fast scrolling.
- Resizable Options with fixed navigation and footer, independently scrolling content, tighter section spacing, and no redundant outer category frames. Keyboard focus reveals offscreen controls.
- Add vertical wheel lines: 0 follows Windows; 1–100 overrides it. Enlarged single pages and continuous reading use consistent line heights, preserving whole-page navigation and horizontal scrolling. Honor explicit previous-page-bottom requests in continuous view.
- Simplify unsaved PDF and ebook annotation dialogs, use save questions as captions, and remove the extra default-button inner frame.
- Show free-text annotation content summaries in the annotation list.
- Use ordinary animated periods for AI table-of-contents processing status.
- Replace the language filter focus underline with an ordinary border and fix asymmetric dark-menu submenu chevrons.

### Download

[SumatraPDF-Plus.exe (Windows x64 Release)](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.42/SumatraPDF-Plus.exe)

Close the application and replace the EXE in your existing complete program folder. Keep existing dictionary, OCR, and other supporting files. Only the EXE is uploaded for this release.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
