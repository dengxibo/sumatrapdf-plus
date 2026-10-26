# Issue 117：移动窗口时页码偶发闪烁

## 问题与修改

用户最后反馈滚动条已恢复正常，但页码和总页数仍偶尔闪烁。录屏第 5–6 秒也出现页码文字变化。

- 当前页输入框虽然已有缓冲 WM_PAINT，WM_ERASEBKGND 仍可能提前擦除数字。非编辑状态下跳过可见背景擦除；编辑时保留原生选区和光标。
- “Page:”和总页数静态控件原先直接在窗口 DC 绘制。现在两者均先在内存 DC 完成背景、文字绘制，再提交完整画面。共享子类位于 `src/ToolbarPageText.h`，供应用与回归测试共同使用。
- 坐标、尺寸、文本、可见状态和页码槽宽度没有变化时，不再重复设置。
- `ControllerCallbackHandler::PageNoChanged` 和选择结束路径此前会直接重复写入页码框，绕过工具栏已有的相同文本判断。统一调用 `UpdateToolbarCurrentPageText`，避免相同页码重写，且不覆盖编辑中的页码。
- DPI 预览、拖动结束后的完整刷新、共享字体失效后的其他窗口刷新，均改用 `RefreshToolbarForDpi`。原位更新字体、图标、工具栏间距及 rebar 高度，保留页码和搜索控件 HWND。
- 工具栏间距以创建时的基准 DPI 缩放，避免跨屏往返时累积舍入误差。正在编辑的页码不被布局同步覆盖，退出输入时同步为实际页码。

## 验证

`tools/test_toolbar_page_erase.cpp` 是独立 Win32 回归程序，不使用 STL。

在 Visual Studio 开发环境中编译：

```powershell
cl /nologo /W4 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE tools/test_toolbar_page_erase.cpp /Fe:out/test_toolbar_page_erase.exe /Fo:out/test_toolbar_page_erase.obj /link user32.lib gdi32.lib comctl32.lib
```

不带参数运行时，检查四种配色：原生静态控件直接绘制到窗口 DC，修复子类使用内存 DC，擦除不清空可见内容，离屏文字和背景仍正确。

带三个参数 `exe appdata pdf` 时，仅创建和移动它自己的隔离应用进程，在实际显示器间往返 24 次。断言：发生真实 DPI 变化，工具栏与四个页码控件 HWND 始终存活，页码控件没有超出工具栏边界，最终 DPI 刷新后仍保留原控件。需要两块不同 DPI 的显示器。

第四个参数任意值启用页码编辑回归：设置输入“7”并选中，验证跨屏和拖动结束后输入与选区保持。测试仅为自身创建的进程设焦点，不操作已有用户窗口。

四种实际应用主题：Light-White、Dark-Dracula、Dark-Black、Light-Warm。

2026-10-26 本机验证结果：

- 完整调试构建：0 警告、0 错误。
- 四配色真实控件绘制回归：全部通过，包含原生直接绘制的对照。
- 四个应用主题分别完成 24 次双屏往返；实际 DPI 变化被检测到，控件身份及边界断言全部通过。
- 编辑态回归：24 次真实 DPI 变化，输入“7”和选区在移动期间及最终刷新后保持，全部通过。此测试在页码通知尚未收口时曾失败（输入被重写成实际页码“1”），修复后通过。
- 最终构建的现有单元测试：102424 项通过。

测试结果说明源代码中相关绘制、控件生命周期和布局路径已受到验证；不能据此声称覆盖所有 Windows、显卡与刷新率组合。
