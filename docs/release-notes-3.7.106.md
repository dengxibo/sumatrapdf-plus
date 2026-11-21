# SumatraPDF Plus 3.7.106

- 修复 Light-Warm 主题下，配置重载后旧窗口菜单仍使用失效字体句柄的问题，避免菜单字体异常、文字被截断（#129）。
- 菜单绘制前检查字体缓存是否更新，并在必要时重新绑定字体、重新计算按钮尺寸。
- 已通过三个菜单控件连续三轮字体缓存失效和不同 DPI 字体复用的回归测试。报告者的 Windows 11、4K、125% 缩放环境仍待复测。

## English

- Fix stale menu font handles after settings reloads in Light-Warm, which could distort or truncate menu text in older windows (#129).
- Refresh the menu font and button sizes before painting when the shared font cache changes.
- Pass regression tests covering three menu controls, repeated font cache invalidation, and font handle reuse across DPI values. Retesting on the reporter's Windows 11 system at 4K and 125% scaling is still needed.
