# 消息框统一主题检查（2026-10-11）

## 统计口径

统计 `src/**/*.cpp` 中的消息框调用位置，排除注释和函数定义；不是不同文案或运行时弹窗次数。

- 应用层：85 个调用位置（包含应用层公共警告封装的转发）。
- 工具库内部转发与测试程序：6 处。
- 合计：91 处，分布在 21 个文件。

| 应用层入口 | 调用位置数 |
|---|---:|
| `MessageBoxNYI` | 1 |
| `MessageBoxW` | 12 |
| `MessageBoxWarning` | 50 |
| `MsgBox` | 11 |
| `TaskDialogIndirect` | 11 |

## 实现

- `AppMessageBoxW` 保留 Windows 原生 MessageBox 的类型、默认按钮、返回值、模态及 RTL 标志。只在当前线程本次模态调用期间安装 CBT hook，在消息框第一次显示前应用统一标题栏、字体、背景、文字和按钮样式。
- 工具库的 `MsgBox` / `MessageBoxWarningSimple` 通过可注册回调进入同一主题入口，工具库无需依赖应用主题代码。未初始化主题的独立工具及启动早期错误保留原生回退。
- 全部应用层直接 `MessageBoxW` 和 11 个 `TaskDialogIndirect` 调用改走公共入口。TaskDialog 复制配置并转发原回调及原 `lpCallbackData`，保留自定义按钮、链接、验证框与单选结果。
- 原生模态框使用随窗口释放的主题资源，不进入主线程长期窗口注册表，避免后台线程弹窗修改主线程注册表。
- 四种主题复用现有选项窗口配色和按钮逻辑；TaskDialog 还接入 DarkModeLib 专用子控件处理。

## 验证

- 运行 `bun cmd/audit-message-boxes.ts` 检查业务代码是否还有绕过公共入口的直接调用。
- 构建检查全部调用点及工具库链接。
- 实际窗口仍需在四种主题下确认，尤其是系统 TaskDialog 的 DirectUI 主面板/底部区域；仅编译不能证明系统绘制的最终颜色。
- 验证删除 PDF 目录项（否为默认项）、错误与警告提示、重启确认、删除文件、更新提示的按钮、Esc/Enter、链接及长文本换行。
- 验证 100%/150%/200% DPI，以及后台线程错误提示关闭后 owner 恢复。

## 调用位置清单（改动前）

- `src/AiToc.cpp`：939 (MessageBoxWarning), 1392 (MessageBoxWarning), 1810 (MessageBoxWarning), 1910 (MessageBoxWarning), 1926 (MessageBoxWarning), 1951 (MessageBoxWarning), 1959 (MessageBoxWarning), 1971 (MessageBoxWarning), 1984 (MessageBoxWarning), 2330 (MessageBoxWarning), 2343 (MessageBoxWarning)
- `src/EditAnnotations.cpp`：2733 (MessageBoxNYI)
- `src/ExternalViewers.cpp`：444 (MsgBox)
- `src/ExtractPdfToc.cpp`：31157 (MessageBoxW)
- `src/HandwrittenSignature.cpp`：938 (MessageBoxW)
- `src/HomePage.cpp`：3017 (TaskDialogIndirect)
- `src/ImageSaveCropResize.cpp`：1295 (MessageBoxWarning), 1303 (MessageBoxWarning), 1310 (MessageBoxWarning), 1328 (MessageBoxWarning)
- `src/InlineTranslate.cpp`：3568 (MessageBoxWarning)
- `src/Installer.cpp`：1074 (MsgBox), 1180 (TaskDialogIndirect)
- `src/PdfTools.cpp`：206 (MessageBoxWarning), 464 (MessageBoxWarning), 694 (MessageBoxWarning), 905 (MessageBoxWarning), 1300 (MessageBoxWarning), 2079 (MessageBoxWarning), 2085 (MessageBoxWarning), 2093 (MessageBoxWarning), 2104 (MessageBoxWarning), 2115 (MessageBoxWarning), 2137 (MessageBoxWarning), 2148 (MessageBoxWarning), 3877 (MessageBoxWarning), 4121 (MessageBoxWarning)
- `src/Print.cpp`：568 (MessageBoxWarning), 1101 (MsgBox), 1154 (MessageBoxWarning), 1177 (MessageBoxWarning), 1194 (MessageBoxWarning)
- `src/SumatraDialogs.cpp`：1881 (MessageBoxW), 1891 (MessageBoxW), 1935 (MessageBoxW), 1974 (MessageBoxW), 2897 (MessageBoxWarning), 2915 (MessageBoxWarning), 2921 (MessageBoxWarning), 2927 (MessageBoxWarning), 2934 (MessageBoxWarning), 2940 (MessageBoxWarning), 2948 (MessageBoxWarning), 3171 (MessageBoxW)
- `src/SumatraPDF.cpp`：1034 (MsgBox), 7185 (MsgBox), 7608 (MessageBoxWarning), 7645 (TaskDialogIndirect), 7654 (MessageBoxW), 7809 (TaskDialogIndirect), 7952 (MessageBoxWarning), 9308 (MessageBoxW), 11405 (MessageBoxWarning), 17879 (MsgBox), 17889 (MsgBox)
- `src/SumatraStartup.cpp`：943 (TaskDialogIndirect), 1022 (TaskDialogIndirect), 1074 (TaskDialogIndirect), 1103 (MsgBox), 1123 (TaskDialogIndirect), 1830 (MsgBox)
- `src/TableOfContents.cpp`：1346 (MessageBoxW), 1569 (MessageBoxW), 1606 (TaskDialogIndirect)
- `src/TestPlugin.cpp`：125 (MsgBox), 141 (MsgBox)
- `src/TocCalib.cpp`：6463 (MessageBoxW)
- `src/Uninstaller.cpp`：271 (MsgBox), 539 (MsgBox)
- `src/UpdateCheck.cpp`：330 (TaskDialogIndirect), 453 (MessageBoxWarning), 464 (MessageBoxWarning), 576 (TaskDialogIndirect), 656 (MessageBoxWarning), 782 (MessageBoxWarning), 802 (MessageBoxWarning), 856 (MessageBoxWarning)
- `src/tools/plugin-test.cpp`：96 (MsgBox)
- `src/utils/WinUtil.cpp`：4297 (MessageBox), 4301 (MessageBoxWarningSimple), 5158 (MessageBoxW)
