# SumatraPDF Plus 3.7.95

## 中文

- 主页现在可以在横版缩略图、竖版封面和列表之间切换。点击搜索框右侧的视图按钮即可切换。横版和竖版都支持 Ctrl + 滚轮调整大小（#125）。
- EPUB 预览优先使用书内封面。用首页生成预览时，会检测并裁去图案周围的大块白边。横版缩略图按宽度缩放，页首留白过多时会把内容往上提。
- 修复 EPUB 章首图压住正文的问题，避免多行段落的第一行单独留在页底，并改善中英混排两端对齐时过宽的英文词间距。
- 按作者声明的顺序加载本机和嵌入字体，修复常规体误选粗体。EPUB 的中文黑体在有对应常规体时优先使用常规字重。

下载 Windows x64 EXE。退出程序后替换安装目录中的 EXE，保留配置及配套文件。旧的 EPUB 封面预览可在重新打开对应图书后更新。

### 本地 HD 语音无法朗读

如果更新 Windows 语音扩展后，本地 HD 音色仍出现在列表里却无法发声，请另外下载 [NaturalVoiceSAPIAdapter v0.2.9-secv2-hd.3](https://github.com/dengxibo/NaturalVoiceSAPIAdapter/releases/tag/v0.2.9-secv2-hd.3)。解压 x86/x64 安装包，运行其中的 `Installer.exe` 覆盖升级，然后完全退出并重新打开 SumatraPDF Plus。本地 HD 语音仍需要匹配的 Windows 语音扩展和模型。

## English

- Switch between landscape thumbnails, portrait covers, and the list using the view button beside Home search. Use Ctrl + mouse wheel to resize thumbnails in either grid (#125).
- EPUB previews prefer the book's cover image and trim broad white margins around artwork on fallback cover pages. Landscape previews fit the card width and shift past excessive space at the top of a page.
- Fix EPUB chapter images overlapping the accompanying text, keep the first line of a multi-line paragraph from being stranded at the bottom of a page, and improve English word spacing in justified mixed-script text.
- Resolve local and embedded fonts in the order declared by the book and fix regular faces being mistaken for bold. EPUB rendering prefers regular weight for Chinese sans-serif families when a regular face is available.

Download the Windows x64 EXE. Close the application before replacing the EXE and retain settings and supporting files. Reopen a book to refresh an existing EPUB cover preview.

### Local HD voices stop speaking

If local HD voices are listed but no longer speak after a Windows speech extension update, download [NaturalVoiceSAPIAdapter v0.2.9-secv2-hd.3](https://github.com/dengxibo/NaturalVoiceSAPIAdapter/releases/tag/v0.2.9-secv2-hd.3) separately. Extract the x86/x64 installer package, run `Installer.exe` to upgrade the adapter, then fully close and reopen SumatraPDF Plus. Local HD voices still require compatible Windows speech extensions and models.
