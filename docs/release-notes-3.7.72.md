# SumatraPDF Plus 3.7.72

## 中文

- 修复移动窗口时页码区域偶发闪烁（#117）：减少重复页码写入和控件布局，页码标签与总页数采用缓冲绘制。
- 跨屏 DPI 切换时原位更新工具栏，保留页码、搜索框及正在编辑的输入和选区；改善暗色界面的残留色块。
- 改善标签分离时的闪动和抓取位置，持续拖动时保持鼠标在原标签位置，并减少移动过程中的同步工作。
- 修复无障碍客户端读取文字时与标签切换、文档关闭并发访问失效文档导致的崩溃。
- 新增 PDF 页面导出为 PNG / JPEG：支持全部页面或当前页、96 / 150 / 200 / 300 / 600 DPI（默认 300）、进度和取消。每次导出创建独立子文件夹，适配四个主题及多屏 DPI。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.72/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置及配套文件，重启后生效。

## English

- Fix intermittent flicker in the page-number area while moving a window (#117), avoiding redundant text/layout changes and buffering page-label and total-count painting.
- Refresh the toolbar in place across monitor DPI changes, preserving page/search controls, pending page input and selection, and improving dark-theme repainting.
- Improve detached-tab positioning and initial presentation, retain the original tab grab point throughout the move, and reduce synchronous work during dragging.
- Fix an accessibility crash when text-range queries race with tab switching or document closure.
- Export PDF pages as PNG or JPEG: all pages or the current page, selectable 96 / 150 / 200 / 300 / 600 DPI (300 by default), progress and cancellation. Each export creates a separate subfolder; the dialog supports all four themes and multiple monitor DPIs.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.72/SumatraPDF-Plus.exe). Close the application before replacing the EXE; retain settings and supporting files, then restart.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.

[Third-party dependency notices](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.72/THIRD-PARTY-NOTICES.txt).
