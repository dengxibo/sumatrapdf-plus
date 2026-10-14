# SumatraPDF Plus 3.7.48

## 中文

本版修正对话框控件边框和 OCR 内存生命周期，并完善按页识别。

- **输入框和下拉框：** 调整页面、选项及字体选择中的控件统一边框绘制，修正方框与圆角叠加、输入框获得焦点后上方圆角缺失，以及字体搜索框的重复边框。
- **选项滚动：** 修复滚动后的文字残影和控件重叠，调整可输入下拉框的圆角间距与闭合区域裁剪。
- **OCR 稳定性：** 模型路径及运行库版本字符串使用独立内存，修复临时字符串池回收后会话重载可能访问失效内存的问题；各识别槽位独立保存输入输出名称，加载失败时清理已创建的会话。
- **OCR 内存：** 连续识别保留内存池加速，空闲 30 秒后在后台释放模型会话，下次使用时自动加载。
- **按页识别：** 文档右键菜单增加「识别当前页」；缩略图右键菜单支持识别选中的页面，并共用批量识别进度条。
- **自动识别：** 仅预取下一页，关闭时取消待执行的自动任务；根据当前扫描页判断是否自动启用。

### 下载

[SumatraPDF-Plus.exe（Windows x64 Release）](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.48/SumatraPDF-Plus.exe)

退出程序后替换完整安装目录内的 EXE，保留配置、字典、OCR 模型及其他配套文件。本次提供 EXE。

## English

This release corrects dialog control borders and OCR memory ownership, and improves page-level recognition.

- Use one border painter for fields in page adjustment, Options and font selection; correct overlapping square and rounded frames, damaged focused corners and the font search field's duplicate frame.
- Fix stale text and overlapping controls after scrolling Options, and adjust editable dropdown corner spacing and closed-face clipping.
- Store OCR model paths, runtime version strings and per-slot IO names in independently owned memory. Correct invalid path access after temporary-pool reclamation and clean up partially loaded sessions.
- Keep inference arenas for consecutive pages; release model sessions after 30 seconds idle and reload them on demand.
- Add current-page OCR to the document context menu and selected-page OCR to the thumbnail menu, using batch recognition progress.
- Limit automatic prefetch to the next page, cancel queued automatic work when disabled, and decide automatic activation from the current scanned page.

[Download Windows x64 Release EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.48/SumatraPDF-Plus.exe)

Close the app before replacing the EXE in an existing complete installation. Keep settings, dictionaries, OCR models and supporting files.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
