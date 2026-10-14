# SumatraPDF Plus 3.7.53

## 中文

- **印刷页码保留：** 提取目录确认后，将印刷页码及罗马数字等特殊标签保存到 PDF 书签，重新编辑或重开文件时恢复；修改、移动和重排书签时保留这些信息。旧版本已经丢失的值需要重新导入或手动补填。
- **输入框定位：** 正在编辑的印刷页和 PDF 页输入框随右侧列移动，避免拖动侧栏宽度后落到标题区域。
- **提示文字：** 未编辑且无内容时显示“印刷页”；进入编辑后只显示内容，无内容则为空白。
- **箭头样式：** 使用上半边右箭头，有无偏移值都靠下对齐；有偏移值时按实际文字宽度绘制箭头。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.53/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型及配套文件。

语音适配器附件沿用上一版：[替换说明、许可证及对应源码](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md)。这些附件为可选组件，不要放入 SumatraPDF 安装目录。

已通过构建、数字页码/罗马标签/子目录元数据保存重开回归及已有单元测试；最新界面布局尚未完成完整人工验收。

## English

- Persist extracted printed page numbers and special labels in PDF outlines, restore them when editing or reopening, and preserve them across bookmark edits and reordering. Previously lost values require reimport or manual entry.
- Keep active printed/PDF page editors aligned with the right-side columns as the sidebar width changes.
- Show the printed-page placeholder only outside editing; show existing content or an empty field during editing.
- Use an upper half-arrow on a consistent lower baseline, matching the displayed offset text width.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.53/SumatraPDF-Plus.exe). Close the application before replacing the EXE in an existing complete installation; retain settings and supporting files.

Optional speech adapter assets are unchanged; see [instructions, licenses and source links](https://github.com/dengxibo/sumatrapdf-plus/blob/3.7.50/docs/release-notes-3.7.50.md).

Builds, printed-page metadata save/reopen regression checks and existing unit tests passed. Full interactive verification of the latest layout remains pending.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
