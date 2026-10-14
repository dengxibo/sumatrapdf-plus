# SumatraPDF Plus 3.7.49

## 中文

本版完善正文目录生成方式，统一目录页码输入框，并提供 Edge 联网自然语音适配器的非官方修复 DLL。

- **从正文生成目录：** 首次选择 API 或网页版 AI；已配置 API 时默认选 API。记住上次选择，入口旁可随时切换，也可进入 API 配置。API 回复自动导入；失败时由用户选择重试或切换网页版，不自动切换。重试和切换复用已扫描的标题候选。
- **目录页码输入框：** 四个主题下的印刷页码、PDF 页码统一使用对话框圆角样式，编辑框先完成样式初始化再显示，避免原生黑色直角边框残留。
- **倾斜校正：** 角度输入框和上下箭头的垂直中心对齐标尺横线，并按实际控件位置适配 DPI 缩放。

### 下载与更新

- [SumatraPDF-Plus.exe（Windows x64 Release）](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.49/SumatraPDF-Plus.exe)
- [适配器修复包：x64、x86 DLL、替换说明、校验值及许可证](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.49/NaturalVoiceSAPIAdapter-v0.2.9-gec-v2-x86-x64.zip)
- [单独下载 x64 DLL](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.49/NaturalVoiceSAPIAdapter-x64.dll)
- [单独下载 x86 DLL](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.49/NaturalVoiceSAPIAdapter-x86.dll)

退出 SumatraPDF 后替换完整安装目录中的 EXE，保留配置、字典、OCR 模型及配套文件。语音 DLL 是可选附件，不是 SumatraPDF 的必需依赖。

### 为什么旧适配器的联网声音可能不能使用？

NaturalVoiceSAPIAdapter v0.2.9 使用旧的 Sec-MS-GEC v1 校验。edge-tts 的修复提交报告微软联网语音服务改为要求 v2，旧请求可能在 WebSocket 握手时被拒绝并返回 HTTP 403。这与“Edge 可以朗读，而适配器联网声音不能朗读、本地声音正常”的现象相符，不能据此断言 Windows 11 更新导致故障或中国区 IP 被全面封禁。

本次附带的两个 DLL 基于 v0.2.9，移植 edge-tts 7.3.0 的 v2 校验；它们是本项目作者提供的非官方修复测试版，并非上游官方新发行版。原固定客户端令牌没有更换，更新的是动态校验算法和版本声明。

### 如何替换 DLL

1. 已安装 NaturalVoiceSAPIAdapter 的用户，先关闭 SumatraPDF、TtsApplication 及其他使用语音的程序。
2. 找到适配器原来的安装目录，备份其中 `x64/NaturalVoiceSAPIAdapter.dll` 和 `x86/NaturalVoiceSAPIAdapter.dll`。
3. 推荐解压修复 ZIP，将对应架构的 DLL 替换到上述原目录，其他依赖 DLL、语音包和配置保持不变。不要将它们直接放进 SumatraPDF 目录。
4. 若下载单独 DLL，先将 `NaturalVoiceSAPIAdapter-x64.dll` 或 `NaturalVoiceSAPIAdapter-x86.dll` 重命名为 `NaturalVoiceSAPIAdapter.dll`，再替换到对应目录。
5. x64 程序使用 x64 DLL；32 位程序使用 x86 DLL，即使系统是 64 位。架构不匹配的 DLL 无法加载。本附件没有提供 ARM64 原生 DLL。
6. 原目录已注册的适配器通常无需重新安装或注册。重启应用后先用 TtsApplication 测试联网声音及本地声音；文件被占用时关闭剩余语音程序或重启 Windows。需要回退时恢复备份。

未安装适配器的用户不能只靠两个 DLL 完成安装，应先准备官方完整 v0.2.9 安装包及依赖。修复不保证所有网络、声音或未来服务变更都可用。

### 源码与验证

- [适配器对应源码与许可证](https://github.com/dengxibo/NaturalVoiceSAPIAdapter/tree/v0.2.9-gec-v2-test.1)，源码提交 `37a62a3`。
- [已提交的上游 PR #121](https://github.com/gexgd0419/NaturalVoiceSAPIAdapter/pull/121)。
- [edge-tts v2 校验修复说明](https://github.com/rany2/edge-tts/pull/492)。

适配器已记录验证：x64/x86 Release 构建、318 组校验值对照、x64/x86 英文联网声音、x64 中文联网声音、本地 Aria、词语/句子边界（按引擎支持）、书签、口型和取消。ARM64、Windows XP 运行行为及 Azure 订阅服务未验证。移植部分遵循 LGPLv3，原项目代码保留 MIT 许可；ZIP 包含许可证和对应源码链接。

SumatraPDF x64 Release 构建通过，0 警告、0 错误；已有 102801 项单元测试通过。正文 AI 的真实 API/网页交互尚未端到端实测。

## English

- Body TOC generation now offers API or Web AI, defaults to API when configured, remembers the selected method, and provides a method/configuration button. API replies are imported automatically. Failures offer an explicit retry or switch to Web AI, reusing the cached heading candidates.
- TOC calibration fields share rounded dialog styling in all four themes. Inline edits are initialized before being shown to prevent a residual native square border.
- Align the deskew angle field and spinner to the actual slider track at different DPI settings.

[Download the Windows x64 Release EXE](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.49/SumatraPDF-Plus.exe). Close SumatraPDF before replacing its EXE in an existing complete installation; retain settings, dictionaries, OCR models and supporting files.

### Optional Edge online voice fix

The attached ZIP and standalone x64/x86 DLLs are unofficial replacement builds of NaturalVoiceSAPIAdapter v0.2.9, implementing Sec-MS-GEC v2. The old adapter uses v1; upstream edge-tts testing reports that the service now rejects v1 handshakes with HTTP 403. This does not establish a Windows update defect or a blanket regional IP ban. Local voices use a separate path.

Close all speech applications, back up the original adapter DLLs, and replace `x64/NaturalVoiceSAPIAdapter.dll` and `x86/NaturalVoiceSAPIAdapter.dll` in the existing adapter installation with the matching ZIP files. Keep other dependencies, voice packs and settings. Do not place these DLLs in the SumatraPDF directory. Standalone downloads have architecture suffixes and must be renamed to `NaturalVoiceSAPIAdapter.dll` before replacement. A 32-bit application requires x86 even on 64-bit Windows; a mismatched DLL cannot load. No native ARM64 DLL is provided. An adapter registered from the same folders normally does not need re-registration. Restore the backups to roll back. New users need the complete official installation and dependencies first.

[Replacement ZIP](https://github.com/dengxibo/sumatrapdf-plus/releases/download/3.7.49/NaturalVoiceSAPIAdapter-v0.2.9-gec-v2-x86-x64.zip) includes licenses, SHA-256 checksums and instructions. [Corresponding source](https://github.com/dengxibo/NaturalVoiceSAPIAdapter/tree/v0.2.9-gec-v2-test.1) is available under the original MIT license and LGPLv3 for the ported portions. [Upstream PR #121](https://github.com/gexgd0419/NaturalVoiceSAPIAdapter/pull/121) describes the fix and recorded tests. These are not official upstream release DLLs and do not guarantee availability for every voice/network or future service changes.

Recorded adapter checks cover x64/x86 builds, 318 token vectors, English online speech on both architectures, Chinese online speech on x64, local Aria, supported boundary events, bookmarks, visemes and cancellation. ARM64, Windows XP runtime behavior and Azure subscription endpoints were not tested. SumatraPDF x64 Release built with zero warnings/errors and passed all 102801 existing unit tests. Live body-TOC API/browser interaction has not been tested end to end.

An unofficial Plus version based on SumatraPDF, licensed under GPLv3.
