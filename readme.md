# Sumatra PDF Plus

**Unofficial fork** of [SumatraPDF](https://github.com/sumatrapdfreader/sumatrapdf) · **非官方**社区增强版  
Not affiliated with [sumatrapdfreader.org](https://www.sumatrapdfreader.org/) · 与官方站点无关

A Windows document reader (PDF, EPUB, MOBI, and more) with improvements for Chinese ebooks, offline lookup, themes, and smart PDF dark mode.  
Windows 下的 PDF / 电子书阅读器，针对中文 EPUB/MOBI、离线查词、主题与 PDF 智能暗黑模式等做了增强。

- **Repository / 本仓库：** source code for GPLv3 compliance · 满足 GPLv3 源码随同分发要求  
- **User guide (Chinese) / 中文详细说明：** [readme.txt](readme.txt)  
- **Upstream / 上游项目：** [https://github.com/sumatrapdfreader/sumatrapdf](https://github.com/sumatrapdfreader/sumatrapdf)  
- **License / 许可：** [COPYING](COPYING) (GPLv3) + [AUTHORS](AUTHORS)

---

## Features · 主要特性


|                                                                                                                     |                                               |
| ------------------------------------------------------------------------------------------------------------------- | --------------------------------------------- |
| **Multi-monitor DPI** — per-monitor UI fonts; correct sidebar, toolbar, menu, tabs, and TOC search when dragging between 100%/125%/150% displays; fixes tab-detach toolbar glitch | **双屏 DPI** — 按显示器 DPI 刷新 UI 字号；跨屏拖动时书签、工具栏、菜单、标签、目录搜索字号正确；修复拖标签分窗后工具栏字体异常 |
| **Chinese EPUB/MOBI** — improved mixed text/image layout; fixes for broken table-of-contents navigation             | **中文 EPUB/MOBI** — 优化图文混排；修复目录跳转错误            |
| **Faster large ebooks** — quicker loading for big EPUB/MOBI/AZW files                                                   | **大文件加速** — 大型 EPUB/MOBI/AZW 打开更快                 |
| **Session-restore fix** — double-clicking a file no longer crashes when “reopen last session” is enabled            | **会话恢复修复** — 开启“恢复上次会话”后，双击打开文件不再崩溃           |
| **Offline dictionary** — double-click a word; place `.idx`/`.dat` files in `{exe}\dict\`                            | **离线查词** — 双击词语查词；词典放在 `{exe}\dict\`          |
| **Light / dark themes** — toolbar toggle; Light-Warm (eye-care) and Light-White (neutral) UI themes                 | **亮/暗主题** — 工具栏切换；暖色护眼与中性浅色 UI                |
| **Document color mode** — Smart / Original / Match Theme for PDF, EPUB, MOBI, CHM, XPS, DjVu, Markdown, etc.; toolbar buttons when a document is open | **文档颜色模式** — 自动 / 原稿 / 强制主题；适用于 PDF、EPUB、MOBI、CHM、XPS、DjVu、Markdown 等 |
| **Read Aloud (TTS)** — Windows text-to-speech with word-by-word highlight; start from top, cursor, or selection; pause/continue; voice and speed presets (0.25×–2.0×) | **朗读 (TTS)** — Windows 语音朗读，逐词高亮；从页首/光标/选中开始；暂停/继续；可选语音与语速（0.25×–2.0×） |
| **Selection toolbar** — highlight, underline, strike out, Ask AI on PDF text selection                              | **划词工具栏** — PDF 选中文字后可高亮、下划线、删除线、Ask AI       |
| **UI polish** — Windows 11–style caption, refined toolbar spacing, theme-aware chrome                               | **界面优化** — Win11 风格标题栏、工具栏间距与主题配色             |


Product name: **Sumatra PDF Plus** (`kAppName` in `src/Version.h`). Executable is still `SumatraPDF.exe`.  
显示名称：**Sumatra PDF Plus**，可执行文件名仍为 `SumatraPDF.exe`。

**Supported formats / 常见格式：** PDF, EPUB, MOBI, AZW/AZW3, FB2, CHM, CBZ/CBR, DjVu, XPS, and more.

---

## Quick start · 快速开始

1. Extract the full folder to any path (avoid special characters in the path).
  解压整个文件夹到任意目录（路径尽量不要含特殊符号）。
2. Run `SumatraPDF.exe`. Keep the `fonts` folder beside the exe.
  双击 `SumatraPDF.exe` 运行；不要只复制 exe，请保留同目录的 `fonts` 文件夹。
   For Traditional Chinese PDF lookup, also keep the `opencc` folder.  
   繁体 PDF 查词还需保留 `opencc` 文件夹。
3. Use the toolbar to toggle light/dark theme and word lookup.
  可通过工具栏切换亮/暗主题和查词开关。
4. Click the **speaker** icon (after word lookup) or open **Read Aloud (TTS)** in the menu bar to read aloud.
  点击工具栏**喇叭**图标（在查词按钮后），或菜单栏 **Read Aloud (TTS)** 开始朗读。

---

## Read Aloud (TTS) · 朗读

Read documents aloud with **word-by-word highlighting** synced to speech. Works on PDF, EPUB, MOBI, and other formats with extractable text.  
**逐词高亮**跟随朗读进度，支持 PDF、EPUB、MOBI 等可提取文本的格式。

**How to use / 使用方法**

- **Toolbar / 工具栏** — speaker icon after word lookup; click to start/pause/continue; dropdown for voice, speed, and start options  
  查词按钮后的**喇叭**；单击开始/暂停/继续；下拉菜单可选语音、语速与起始方式
- **Menu bar / 菜单栏** — **Read Aloud (TTS)** → start from top, cursor, or selection; **Voice** and **Speed** submenus  
  **Read Aloud (TTS)** → 从页首/光标/选中开始；**Voice** 选语音、**Speed** 选调速
- **Right-click / 右键** — **Start Reading From Cursor Position**; **Pause Reading** / **Continue Reading** while active  
  **从光标处开始朗读**；朗读中可**暂停朗读** / **继续朗读**
- **Speed presets / 语速** — 0.25×, 0.5×, 0.75×, 1.0×, 1.25×, 1.5×, 2.0×

**Natural voices / 自然语音**

Default Windows SAPI voices (e.g. Huihui, Kangkang, Yaoyao) sound robotic. For natural speech, install **[NaturalVoiceSAPIAdapter](https://github.com/gexgd0419/NaturalVoiceSAPIAdapter)** and configure local Narrator voices or Edge online voices — see [docs/md/Read-Aloud-TTS.md](docs/md/Read-Aloud-TTS.md).  
系统默认 SAPI 语音（如慧慧、康康、瑶瑶）较机械。自然语音需安装 **[NaturalVoiceSAPIAdapter](https://github.com/gexgd0419/NaturalVoiceSAPIAdapter)**，详见 [docs/md/Read-Aloud-TTS.md](docs/md/Read-Aloud-TTS.md)。

Advanced settings: `ReadAloudVoiceId`, `ReadAloudSpeakingRate`.  
高级设置：`ReadAloudVoiceId`（语音）、`ReadAloudSpeakingRate`（语速）。

---

## Offline dictionary · 离线查词

Dictionary **code** is in this repo;   
查词**功能代码**在本仓库；

1. Create `{exe}\dict\` next to the executable.
  在 exe 同目录建立 `dict` 文件夹。
2. Add dictionary files, for example:
  放入词典文件，例如：
  - `SumatraDict.idx` / `SumatraDict.dat /` `SumatraDictAudio.dat`— English–Chinese · 英汉
  - `SumatraDictZh.idx` / `SumatraDictZh.dat` — Chinese dictionary · 汉语
3. Enable the dictionary button on the toolbar, then double-click a word in the document.
  打开工具栏「查词」按钮，在正文中双击词语即可。

Override the dictionary folder with the `OfflineDictionaryPath` advanced setting.  
可用高级设置 `OfflineDictionaryPath` 指定其他词典目录。

**Traditional Chinese PDF lookup / 繁体 PDF 查词** — keep the `opencc` folder next to the exe (OpenCC t2s data). Upgrading only the exe without `opencc` disables Traditional lookup.  
繁体 PDF 查词需保留 exe 同目录的 `opencc` 文件夹；只升级 exe 将无法繁体查词。

---

## Settings · 配置文件

Settings are stored in `SumatraPDF-settings.txt`:

- **Portable / 便携版:** next to `SumatraPDF.exe`
- **Installed / 安装版:** `%LOCALAPPDATA%\SumatraPDF\SumatraPDF-settings.txt`

**Annotated reference / 带中文注释的参考模板:** [SumatraPDF-settings-annotated.txt](SumatraPDF-settings-annotated.txt)

Copy only the lines you need into your live settings file. Comments must be on their own `#` lines (inline comments after values are not supported).  
只复制需要的配置行到真实配置文件；注释必须单独成行，不能写在值后面。

User guide (Chinese) / 中文说明: [readme.txt](readme.txt)

---

## Build · 编译 (Windows)

Requirements: Visual Studio 2022 build tools, [Bun](https://bun.sh).

```powershell
bun ./cmd/build.ts          # debug → out/dbg64/SumatraPDF.exe
bun ./cmd/build-all.ts      # release → out/rel64/
```

See [AGENTS.md](AGENTS.md) for contributor notes.  
贡献者说明见 [AGENTS.md](AGENTS.md)。

---

## Feedback · 反馈

Bug reports and pull requests are welcome **on this repository only** — not on the official SumatraPDF issue tracker.  
欢迎在本仓库提交 Issue 和 PR，请勿发到官方 SumatraPDF 项目。

When reporting problems, please include the file type, book title (if ebook), and steps to reproduce.  
反馈问题时请说明文件类型、书名（电子书）和复现步骤。

---

## Disclaimer · 免责声明

Sumatra PDF Plus is a community modification for testing and personal use.  
Sumatra PDF Plus 为基于 GPLv3 的社区修改版，供测试与个人使用。

---

## Upstream SumatraPDF

[Build](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml)

SumatraPDF is a multi-format reader for Windows under (A)GPLv3, with some code under BSD license (see [AUTHORS](AUTHORS)).

- [Website](https://www.sumatrapdfreader.org/free-pdf-reader)
- [Manual](https://www.sumatrapdfreader.org/manual)
- [Developer Information](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)
