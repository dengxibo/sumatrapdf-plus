# SumatraPDF Plus 3.7.114

- 修复字典中文发音及无录音英语词条未正确使用当前在线／离线朗读音色的问题，继续跟随朗读设置。
- 字典 JoyInside 发音完整接收后再播放，避免网络分片造成短词中途停顿；正文朗读保留流式播放。
- 字典 JoyInside 女声裁剪最多 80 毫秒尾音（不超过整段约 8%），加上 12 毫秒淡出，减轻词尾拖音。

**新用户请先到 [3.7.107 发布页](https://github.com/dengxibo/sumatrapdf-plus/releases/tag/3.7.107)下载完整包，解压后用本版 EXE 替换其中的 SumatraPDF-Plus.exe。已有完整目录的用户只需替换 EXE，保留配置及配套资源。**

Fixes dictionary pronunciation fallback to the selected online or offline read-aloud voice. JoyInside dictionary clips are fully buffered before playback to prevent network gaps; document reading remains streamed. Female JoyInside dictionary clips receive a bounded tail trim and a short fade-out. New users can download a complete package from [3.7.107](https://github.com/dengxibo/sumatrapdf-plus/releases/tag/3.7.107), then replace its executable with this release's EXE.
