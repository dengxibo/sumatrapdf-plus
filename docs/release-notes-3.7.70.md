# SumatraPDF Plus 3.7.70

## 中文

- 改善 SAPI 分段朗读衔接，提前准备下一段。安装兼容的 HD SAPI 适配器后，可使用其公开预合成服务；未安装、准备失败或未及时完成时自动回退普通 SAPI。
- SAPI 播放、取消和事件处理移到串行工作线程，隔离停止、跳转、变速、换声后的旧事件，避免旧词边界影响新段高亮。
- 同名 HD 与普通音色分别保留、独立选择；变速和换声时先保存朗读位置。
- 浅色及护眼主题明确使用浅色原生菜单，避免继承 Windows 深色菜单配色。针对偶发明暗混杂做了修正，尚未稳定复现该现象，需继续观察。

智能双语模式暂不提前排队。跨页文字提取或 OCR 尚未完成、首段冷启动及合成耗时超过缓冲预算时，仍可能等待。本程序不包含 HD 引擎或语音模型。

[下载 Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.70/SumatraPDF-Plus.exe)。退出程序后替换完整安装目录中的 EXE，保留配置及配套文件；重启后生效。

## English

- Reduce gaps between SAPI reading chunks by preparing the next chunk ahead of playback. Use the optional public HD preparation service when a compatible adapter is installed; fall back to ordinary SAPI when unavailable, failed or not ready in time.
- Serialize playback, cancellation and event handling on a worker thread. Reject obsolete events after stopping, seeking, changing rate or switching voices, and preserve the reading position before restarting.
- Keep HD and regular variants of the same speaker independently selectable.
- Explicitly select light native menu colors for light and eye-care themes under dark Windows settings. This addresses a possible cause of intermittent mixed menu colors; the reported flicker has not been consistently reproduced.

Smart bilingual mode does not queue ahead yet. Text extraction/OCR, first-chunk startup and synthesis exceeding the buffer budget can still cause delays. HD engines and voice models are not bundled.

[Download Windows x64 EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.70/SumatraPDF-Plus.exe). Close the application before replacing the EXE; retain settings and supporting files, then restart.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.

[Mermaid dependency notices](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.70/THIRD-PARTY-NOTICES.txt).
