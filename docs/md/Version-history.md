# Version history

## next

## 3.7.26 (2026-08-06)

- PDF Match-theme dark mode: never show white manuscript scans in Match theme; route misclassified colorful scans and Acrobat Elements/PScript stacked-strip pages through PureScan bitmap recolor; fix dual/triple-layer image scans (e.g. telecom contracts); skip rects no longer skip whole-tile bitmap recolor pages
  PDF「匹配主题」暗色：扫描稿不再显示白底原稿；彩色扫描误判与 Acrobat Elements/PScript 叠条扫描走 PureScan 位图重着色；修复双层/三层图像扫描（如电信合同）；整页位图重着色页不再被 skip rects 跳过
- MOBI/AZW Chinese first-line indent: two fullwidth characters for Chinese MOBI/AZW/AZW3 in reader style
  MOBI/AZW 中文首行缩进：阅读样式下中文 MOBI/AZW/AZW3 首行缩进两个全角字符
- unify Match-theme RAZ / picture-book pages on sharp dark paper + light text (soft mid-grey paper only for true soft-cream notebook pages); route photo-ish FullPageScan misclass through picture-book protect
  「匹配主题」RAZ/绘本页统一为深色纸+浅色字（仅真软奶油手帐页保留浅灰纸面柔化）；带照片特征的 FullPageScan 误判改走绘本保护路径

## 3.7.25 (2026-08-05)

- faster Match-theme for DuXiu/Pdg2Pic scan books: metadata → whole-tile bitmap recolor; do not treat paper+ink variance as grayscale photos (avoids picture-book multi-rect)
  加快读秀/Pdg2Pic 扫描书「匹配主题」：元数据走整页位图重着色；纸面+墨迹方差不再误判为灰度照片（避开绘本多矩形）
- restore Match-theme open speed for Acrobat textbooks (e.g. Exploring Our World): do not content-probe LayoutPhoto docs with heavy vector pages
  恢复 Acrobat 教材「匹配主题」打开速度：不再对矢量极重的 LayoutPhoto 文档做内容探测
- faster Easy RL / figure-heavy LaTeX flips: Mixed pages use wrap; non-full-bleed Preserve figures draw original (no multi-MP soft remap); dense-text pages stay BitmapRecolor
  加快 Easy RL 等插图 LaTeX 翻页：Mixed 走 wrap；非全幅插图原样绘制；密文本页仍 BitmapRecolor
- soften Match-theme on soft cream illustrated pages (e.g. notebook design books): Preserve with gentle paper softening instead of steep ink remap (avoids dirty grid noise)
  「匹配主题」柔和奶油色插图页（如手帐/设计书）：轻柔纸面柔化，避免陡峭墨水映射造成的网格脏噪
- sharper Match-theme text on scanned/picture-book pages; multi photo-rect protect for RAZ B&W portraits; RGB/Gray fast remap paths
  扫描/绘本页文字更清晰；RAZ 多照片区保护；RGB/Gray 快速重映射
- show home-page vertical scrollbar when recent files overflow; compact Home tab; hide native tab UpDown arrows
  主页最近文件溢出时显示纵向滚动条；主页标签紧凑；隐藏系统标签左右箭头
- stop embedded PDF audio when closing a tab or window
  关闭标签或窗口时停止 PDF 内嵌音频
- fix 32-bit DIB `GetPixel` reading alpha as red
  修复 32 位 DIB `GetPixel` 把 alpha 当成红色

## 3.7.24 (2026-08-04)

- dictionary popup: show a speaker button for Chinese lookups and speak via TTS; polyphone tabs use the selected pinyin (SSML); hide the duplicate phonetic line when tabs are shown
  词典弹窗：中文查词支持喇叭朗读；多音字按当前拼音标签发声（SSML）；有拼音标签时不再另显一行拼音
- add optional TTS pronunciation dictionary (`tts-pronunciation.json` next to the exe or in AppData): longest-match text rewrites before WinRT/SAPI speak; see `tts-pronunciation.sample.json`
  可选 TTS 发音词典（程序目录或 AppData 下的 `tts-pronunciation.json`）：送入 WinRT/SAPI 前按最长匹配改写朗读文本；示例见 `tts-pronunciation.sample.json`
- improve PDF Match-theme dark mode routing: keep colorful picture-book art (e.g. RAZ) with grey paper softening; only AdaptiveDocument-recolor true paper scans; text-heavy literature stays on OKLab remapping
  改进 PDF「匹配主题」暗色分流：绘本彩色插图保留原色（纸面柔化为灰），仅对真扫描页做 Adaptive 重着色；文字类文学书继续 OKLab 重映射
- fix Screen annotation audio playback when the media rendition lives on an `/A` Rendition action (Media Clip `/D` filespec), e.g. RAZ-AA *The City*
  修复 Screen 注释音频：支持 `/A` Rendition 动作中的 Media Clip `/D` 内嵌 MP3（如 RAZ-AA《The City》）

## 3.7.23 (2026-08-04)

- fix PDF smart dark mode inverting colors on RAZ picture-book pages: classify large images as photo/illustration vs scanned page background so illustrations keep natural colors
  修复 PDF 智能暗色模式下 RAZ 绘本页面颜色反转：区分大图是插图/照片还是扫描页背景，插图保持正常色彩
- play embedded PDF audio from Sound, RichMedia, and Screen annotations (e.g. RAZ speaker icons); audio is read from the PDF Assets stream, no network required
  支持播放 PDF 内嵌音频（Sound、RichMedia、Screen 注释，如 RAZ 扬声器图标）；从 PDF Assets 读取 MP3，无需联网
- faster tab close and switch: detach the document and delete the engine on a background thread instead of blocking on slow synchronous teardown
  加快关闭与切换标签：异步释放文档引擎，关闭标签不再等待耗时的同步析构
- faster application exit: abort background reflow/loading and skip synchronous per-page engine teardown on quit (especially while large ebooks are still loading)
  加快退出程序：退出时中止后台重排/加载，跳过逐页同步释放（大电子书仍在加载时尤其明显）

## 3.7.22 (2026-08-03)

- add PDF table-of-contents editing: create, rename, retarget, delete, reorder and change hierarchy; preserve outline styling and save together with other PDF changes
  新增 PDF 目录编辑：可创建、重命名、修改目标、删除、排序和调整层级；保留目录样式，并与其他 PDF 修改一并保存

- fix image context menu: restore **Copy To Clipboard** on PDF images; port full **Image** submenu (copy, save, crop, resize, convert to PDF) to EPUB and other formats
  修复图片右键菜单：PDF 图片恢复「复制到剪贴板」；将完整「图像」子菜单（复制、保存、裁切、调整尺寸、转换为 PDF）移植到 EPUB 等格式
- show a progress notification while ebook font or font-size changes relayout or reload the document (wait cursor + progress bar)
  调整电子书字体或字号时显示进度通知（等待光标与进度条），避免长时间重排时误以为程序无响应
- add toolbar **Decrease Font Size** / **Increase Font Size** buttons for reflowable ebooks (EPUB/MOBI); adjusts `EBookUI.FontSize` in 2 pt steps (6–26) with scroll position preserved
  为可重排电子书（EPUB/MOBI）增加工具栏缩小/放大字号按钮；每次 2 磅（6–26），并尽量保持阅读位置
- fix cumulative search highlight drift on reflow EPUB when UTF-8 and WCHAR page text diverge; align text extraction and map search hits to selection glyphs
  修复可重排 EPUB 搜索高亮逐段错位（累积借位）：对齐 UTF-8/WCHAR 文本流并正确映射命中到字形索引
- fix vertical reflow EPUB text selection, word lookup, and search highlight offset
  修复竖排可重排 EPUB 划词、查词与搜索高亮偏移的问题
- rescan document search after progressive EPUB reflow completes; fix duplicate search results and failed jump-to-match navigation
  渐进加载完成后重新搜索全书；修复重复结果与无法跳转到匹配项

## 3.7.21 (2026-08-01)

- add **Settings** menu → **Reading Font** for EPUB/MOBI: **Western Body Font** and **CJK Body Font** separately; fonts above the separator line are from the `fonts\` folder next to the executable, fonts below are installed on the system
  设置菜单新增「字体」（EPUB/MOBI）：可分别选择西文字体与中文字体；横线上方为 `fonts\` 文件夹字体，横线下方为系统已安装字体
- upgrade from earlier builds: delete `SumatraPDF-settings.txt` before first run (portable: next to the exe; installed: `%LOCALAPPDATA%\SumatraPDF\SumatraPDF-settings.txt`)
  从旧版升级：首次运行前删除配置文件（便携版在 exe 同目录；安装版见 `%LOCALAPPDATA%\SumatraPDF\`）

## 3.7.20 (2026-07-31)

- restore synchronous PDF page layout at open (revert progressive PDF loading that slowed large textbooks and caused repeated relayout)
  恢复 PDF 打开时同步加载全部页信息，撤销误用 EPUB 渐进式加载导致的慢打开与多次重布局
- defer PDF match-theme content probe until after first paint; skip heavy probe when metadata already classifies the document; avoid loading imageless pages during probe
  暗黑+跟随下延后 PDF 主题探测至首帧之后；元数据已分类时跳过重探测；无图页用轻量检查避免整页加载
- fix crash when theme probe completed on a background thread while switching tabs (marshal UI refresh to main thread)
  修复主题探测在后台线程触发 UI 重绘、切换标签时崩溃的问题
- fix tab bar font scaling on high-DPI ultrawide monitors
  修复超宽高分屏下标签栏字体过小的问题
- fix document color mode toggle, read-aloud double-click, and text selection UX
  修复文档颜色模式切换、朗读双击与划词体验

## 3.7.19 (2026-07-28)

- rename advanced setting `PdfDocumentColorMode` to `DocumentColorMode` with clearer values `smart`, `original`, and `theme`; migrate legacy names on load
  将高级设置 `PdfDocumentColorMode` 重命名为 `DocumentColorMode`，取值改为 smart / original / theme，加载时自动迁移旧配置
- fix reflow EPUB blank gaps after the first document color mode or theme switch: relayout only after `UpdateCanvasSize`, serialize theme page-map recount, warm chapters before recount; see `docs/epub-performance-checkpoint-2026-07-17.md` (2026-07-26)
  修复可重排 EPUB 首次切换文档颜色/主题后出现大段空白：须在画布视口就绪后再排版，主题重算串行化并按章暖布局后重数页码
- reflow EPUB/MOBI in dark UI with document color mode **Match theme**: recolor the rendered page bitmap so illustrations follow theme colors (not only CSS text/background)
  暗黑界面下可重排电子书选择「匹配主题」文档颜色时，对整页位图（含插图）做主题反色，与正文一致
- fix matrix outer brackets in some textbook PDFs: use built-in Symbol for subset names like `YFLGZT+Symbol` (extensible bracket glyphs), not Windows Symbol.ttf
  修复部分教材 PDF 矩阵外括号显示为方括号：对子集 Symbol 字体使用内置 Base14 Symbol（含可伸缩矩阵括号字形）
- fix PDF URI links (e.g. DOI on references pages) not showing a hand cursor until the page was fully parsed: load link annotations on hover for PDF as for EPUB
  修复部分 PDF 外链（如参考文献页 DOI）在仅快速渲染时无法手型点击：悬停时加载链接注释
- align text selection drag with upstream SumatraPDF: same FindClosestGlyph range logic, IsDragDistance before updating selection, semi-transparent highlight instead of per-frame multiply blend
  划词行为对齐上游：恢复原版字形区间算法，拖动超过系统阈值再更新选区，半透明高亮替代整帧正片叠底
- fix right-to-left text drag excluding the anchor glyph (range now ends at startGlyph+1; drag end uses glyph under cursor)
  修复从右向左划词时选区不含按下字、左边界偏到左侧邻字的问题
- fix right-to-left drag selecting two characters when only the anchor glyph is intended (immediate left neighbor)
  修复从右向左只拖一格时误选两字、无法只选按下字的问题
- improve CJK/EPUB text selection and math formula highlights (drag row, accent marks, merged highlight bands)
  改进 CJK/EPUB 划词与公式高亮（拖动行、变音符号、合并高亮带）
- show wait cursor and a progress notification when reflow EPUB/MOBI re-pagination runs for a theme or document color change (chapter x / n); large PDFs show a similar notice while pages re-render; ignore overlapping relayout on the same document
  切换主题或文档颜色导致可重排电子书全书重算分页时显示等待光标与章进度通知；大 PDF 重着色时显示提示；同一文档重算进行中时忽略重复触发
- CrashHandler code analysis cleanups (constexpr URLs, static helpers, uninitialized struct init)
  CrashHandler 静态分析清理

## 3.7.18 (2026-07-26)

- fix EPUB reflow layout for cnepub comparison tables (table cell widths, portrait alignment) and MuPDF HTML table percent widths;
  修复萤火虫等 cnepub 对比表排版（单元格宽度、肖像居中）及 MuPDF 表格百分比宽度
- preserve continuous-view scroll position when zooming with Ctrl+wheel or resizing the canvas (e.g. closing the TOC sidebar);
  修复连续阅读模式下 Ctrl+滚轮缩放或关闭目录侧栏后页码乱跳
- improve find-match highlight alignment after zoom and layout changes; drop stale per-chapter page caches when progressive EPUB loading recounts chapters;
  修复搜索高亮错位，并在渐进加载重新统计章节后刷新该章页面缓存，避免同一页排版逐渐漂移

## 3.7.17 (2026-07-23)

- fix selection toolbar jitter and flicker while dragging text selection and during edge autoscroll;
  修复拖选文本及边缘自动滚动时划词工具栏高频抖动、闪烁的问题

## 3.7.16 (2026-07-22)

- fix illustrated PDF rendering where some books (e.g. scanned art catalogues) showed dark blotches or posterized graphics in both light and dark themes;
  修复部分插图类 PDF（如扫描版艺术画册）在浅色和深色主题下出现暗斑、画面失真的问题
- support Chinese word lookup in Traditional Chinese PDFs by converting text to Simplified before dictionary lookup;
  支持繁体中文 PDF 查词：查词典前自动繁转简，可查询《现代汉语词典》释义

## 3.7.15 (2026-07-20)

- comprehensively upgrade document search with incremental results synchronized to progressive EPUB/MOBI/AZW loading;
  全面升级文档搜索，实现搜索结果与 EPUB、MOBI、AZW 渐进式加载同步，书籍可以边解析、边分页、边检索
- unify and refine the compact and floating search interfaces for smoother result browsing, match navigation and mode switching;
  优化并统一搜索栏和悬浮搜索窗口，使结果浏览、命中跳转和模式切换更加自然流畅
- optimize search caching, page-text reuse and background task scheduling for substantially faster and more stable large-document search;
  深度优化搜索缓存、页面文本复用及后台任务调度，显著提升大文档搜索速度、响应能力和整体稳定性
- improve ebook stamp annotations and color handling;
  改进电子书印章标注及颜色处理

## 3.7.14 (2026-07-17)

- extend EPUB/MOBI/AZW annotations with PDF-style text notes, free text, stamps, carets, lines, squares and circles;
  use matching appearance controls and annotation editor options
- add PDF-style Highlight, Underline, Squiggly and Strike Out controls to ebook text-selection actions, plus a Text
  annotation action anchored at the end of the selected text
- improve annotation editing layout and interaction consistency, including point-annotation dragging and text-note icons
- set the default text selection color to Acrobat-style blue while retaining yellow as the default highlight annotation color
- refine themed menu drawing for dynamically created Read Aloud submenus and remove the unwanted native submenu gutter
- add Expand All and Collapse All controls to the Bookmarks sidebar for quickly expanding or collapsing the table of contents;
  书签侧栏新增「全部展开」和「全部折叠」按钮，可快速展开或折叠目录

## 3.7.13 (2026-07-17)

- dramatically improve large EPUB anthology performance: progressive pagination and TOC loading, responsive navigation,
  faster theme and document-color switching, stable redraws, and safer background rendering
- fix EPUB TOC entries becoming enabled only after mouse movement, selection failures on some pages, stale theme redraws,
  and crashes caused by concurrent rendering or stale link/selection data
- add a Settings shortcut to register common document and ebook formats (including PDF, EPUB, MOBI, AZW and Markdown)
  and open the Windows Default Apps page
- improve the Home page with a visible Recently Opened / Frequently Read sort menu
- make document links show the hand cursor immediately, stabilize theme-menu selection marks, and clean up read-aloud menus
- translate Back and Forward toolbar tooltips, enlarge the read-aloud icon slightly, and refine Settings menu ordering
- refine mouse-wheel behavior for zoomed single-page and horizontal scrolling modes
- fix annotation note saving and several MuPDF static-analysis warnings
- improve release auto-update reliability for first launch and long-running sessions: check daily while the app stays open,
  record only successful checks, and retry transient failures with backoff
- ask before downloading an update, then show progress and replace/relaunch the portable executable after one confirmation
- defer a declined version for seven days while allowing a newer release to prompt immediately
- users still on Plus 3.7.3, or on 3.7.4 without ever using **Check for Updates**, must upgrade manually once;
  those releases cannot discover current Plus updates automatically

## 3.7.12 (2026-07-15)

- fix EPUB progressive loading for large anthologies: cooperative background chapter counting so TOC navigation works before pagination finishes; incremental page layout during loading instead of relayouting every page on each batch
- pause background EPUB pagination while switching themes or editing annotations so the UI stays responsive (no long spinner until load completes)
- pre-build the TOC tree model on the background loader thread at end of load so the completion UI freeze is roughly halved on huge multi-book EPUBs
- fix crash when toggling light/dark theme during EPUB load (stale text-selection indices after relayout)

## 3.7.11 (2026-07-14)

- replace the toolbar find UI with a Chrome-style floating find bar: compact bar docked near the toolbar, optional pinned floating results window with snippet list, match counter, **Match whole word** toggle, debounced find-as-you-type (Enter forces search), and independent find-match vs selection highlights
- unify command visibility for toolbar and menu through `CommandAvailability`; fix hamburger menu on the Home tab (empty menu or dead clicks when no document is open)
- add a pinned **Home** tab that stays when all documents are closed; **Ctrl+W** on Home-only closes the window
- fix find bar and floating find window positioning on multi-monitor / mixed-DPI setups
- improve EPUB progressive loading: incremental page layout, throttled toolbar updates during background pagination, faster theme/document-color-mode switches while loading
- read-aloud: resync highlight after layout changes, dedicated highlight color, remove verbose diagnostic logging; speed/bilingual voice dialogs support all UI languages
- fix text selection misalignment when switching themes in continuous view

## 3.7.10 (2026-07-12)

- unify document color mode (Smart / Original / Match theme) across all themes and readable formats (PDF, EPUB, MOBI, CHM, XPS, DjVu): toolbar buttons are always available when a document is open
- Smart mode in **dark themes** preserves embedded images where object-level rendering is enabled; in **Light-Warm**, Smart applies uniform eye-care recolor to the whole page (choose Original for publisher colors)
- EPUB/MOBI/CHM respect document color mode for theme CSS injection; Original shows publisher colors
- rename document color mode command and tooltip strings from "PDF Document Color Mode" to "Document Color Mode" (all languages)
- add **Export Notes** in the PDF annotation editor to save all annotations as Markdown (page, excerpt, note, author, date), same format as EPUB/MOBI export
- Home tab and Home page strings are translatable (search cue, tips, About dialog, version labels)
- translate in-app update download progress ("Downloading update...") for all languages; show download notification during automatic update checks
- add Markdown (.md, .markdown) reading via md4c (GitHub Flavored Markdown: tables, task lists, strikethrough, footnotes, admonitions) rendered through MuPDF reflow with readable typography; supports theme switching like other reflowable documents
- fix Markdown theme switching reloading the whole document (slow flicker); update user CSS in place like EPUB
- fix Markdown theme / document color mode not updating after the fast path (reparse cached HTML with new CSS)
- fix crash when switching Markdown theme/color mode with TOC visible (stale bookmark destinations)

## 3.7.9 (2026-07-12)

- add persistent annotations for EPUB and MOBI/AZW ebooks (highlight, underline, squiggly, strike-out, and text notes). Annotations are stored as per-book JSON sidecars in the SumatraPDF data directory and survive repagination; colors and markup style match PDF annotations (`Annotations.HighlightColor` etc.)
- add **Export Notes** in the ebook annotation editor to save all annotations as Markdown (page, excerpt, note, author, date)
- ebook annotation author is the reader (Windows user name, or `Annotations.DefaultAuthor` if set), not the book author
- fix PDF text markup (highlight/underline/etc.) sometimes appearing only after a long delay: draw an immediate overlay while tiles refresh
- fix annotation editor docking and multi-monitor DPI (close/reopen on drag, width ratio vs main window, correct initial position for EPUB panel)
- fix EPUB annotation window growing onto a second monitor or shrinking when switching list items

## 3.7.8 (2026-07-10)

- fix dark-mode TOC sidebar scrollbar staying white until theme is toggled
- fix TOC scrollbar mouse capture getting stuck when the tree reloads during progressive ebook loading

## 3.7.7 (2026-07-10)

- show download speed and downloaded/total size during in-app update download
- fix Ask AI with ChatGPT: continue in the same conversation instead of starting a new chat each time
- fix Ask AI with ChatGPT: paste into an empty new-chat page when reusing an already-open ChatGPT tab
- fix Ask AI with ChatGPT: do not treat MSN, news, and other non-ChatGPT tabs as ChatGPT when reusing the browser window
- improve Ask AI with ChatGPT: faster paste when continuing an ongoing conversation

## 3.7.6 (2026-07-09)

- fix multi-monitor DPI scaling for bookmark sidebar, toolbar, menu bar, tabs, and TOC search when moving the window between displays with different scale factors
- fix bookmark sidebar tree font on cross-monitor moves: recreate tree views and apply per-monitor menu fonts via SystemParametersInfoForDpi
- fix original window toolbar fonts becoming oversized after dragging a tab out to a new window
- apply UI DPI refresh immediately during cross-monitor drag (trust WM_DPICHANGED wParam) instead of waiting until mouse release
- improve progressive ebook loading performance for large documents (throttle relayout and sidebar invalidation during page counting)

## 3.7.5 (2026-07-08)

- fix Ask AI paste missing the input box for ChatGPT, DeepSeek, and Doubao on Edge and other layouts: click in the main content pane (sidebar-aware) instead of the full window center

## 3.7.4 (2026-07-07)

- add `AiChatProvider` advanced setting (`doubao`, `deepseek`, or `chatgpt`) to choose Ask AI backend; default is `doubao`; `AiChatUseDeepSeekInsteadOfDoubao` is deprecated
- add home page list view (`HomePageViewMode = list`) showing full filename and directory path; toggle via link next to the header
- persist `PdfDocumentColorMode` advanced setting (auto, black, light) across sessions
- in PDF auto (smart dark mode), remove special-case that kept page 1 full-bleed covers at original brightness; first page now follows the same dark-mode rules as other pages
- refine read-aloud bilingual voice switching for short embedded English sentences ending in punctuation
- automatic update check runs on startup when enabled, including the first launch (no longer requires a prior manual check)
- fix portable self-update failing to replace the running exe when installing from the update dialog (especially on first check after download)

**next: 3.7**

Available in [pre-release](https://www.sumatrapdfreader.org/prerelease) builds.

- fix update check reporting "already latest" when a newer version is on GitHub: fetch update-check.txt from both GitHub raw and jsDelivr and use the highest Latest version (jsDelivr @main can lag behind)
- fix multi-monitor DPI: recreate UI fonts per monitor DPI (via SystemParametersInfoForDpi), refresh on first show and when moving between displays with different scale (issue #8)

- add `AiChatProvider` advanced setting (`doubao`, `deepseek`, or `chatgpt`) to choose Ask AI backend; `AiChatUseDeepSeekInsteadOfDoubao` is deprecated (migrates to `deepseek` when `AiChatProvider` is absent from settings)
- add `EnableAskAI` advanced setting (default true); set to false to hide Ask AI in the selection toolbar, context menu, and command palette
- add a floating selection toolbar: after selecting text in a PDF that supports annotations, a small toolbar appears near the selection with one-click `Highlight`, `Underline`, `Squiggly`, `Strike Out` and `Ask AI` actions. Disable it with `Annotations.SelectionToolbar = false` in advanced settings
- add `CmdLookupSelection` (`Look Up Selection` in selection context menu and `Ctrl + k` [command palette](Command-Palette.md)) to look up the selected text with offline dictionaries; the selection toolbar shows `Ask AI`, `Look Up`, `Copy`, then annotation actions when supported
- add **Light-White** theme (renamed from Original): neutral light UI with PDF and fixed-layout documents shown in their original colors (no eye-care yellow tint); **Light-Warm** theme (renamed from Light) still uses eye-care colors for document content
- PDF smart dark mode options (`PreservePdfImagesInDarkMode`, `PreservePdfImagesMinSize`, `PdfDarkModeRenderer`, etc.) are built into the program and are not written to the settings file; toolbar and command-palette toggles apply for the current session only
- persist `PdfDocumentColorMode` advanced setting (auto, black, light) across sessions; toolbar and command-palette choices are remembered on restart
- add home page list view (`HomePageViewMode = list`) showing full filename and directory path; toggle via link next to the header
- in PDF auto (smart dark mode), remove special-case that kept page 1 full-bleed covers at original brightness; first page now follows the same dark-mode rules as other pages
- add `CmdTogglePreservePdfImages` (`Toggle Preserve PDF Image Colors in Dark Mode` in `Ctrl + k` [command palette](Command-Palette.md))
- add `CmdToggleLightDarkTheme` (`Toggle Light/Dark Theme` in `Ctrl + k` [command palette](Command-Palette.md)) and toolbar button to switch between light and dark theme
- add `CmdSetPdfDocumentColorModeAuto`, `CmdSetPdfDocumentColorModeBlack`, and `CmdSetPdfDocumentColorModeLight` toolbar buttons (visible for PDF in dark theme) to set PDF document rendering: auto (smart dark mode), black (full dark), light (original colors)
- add `CmdToggleDoubleClickWordLookup` (`Toggle Double-Click Word Lookup` in `Ctrl + k` [command palette](Command-Palette.md)) and toolbar button to enable or disable offline dictionary lookup on double-click; default is enabled (`EnableDoubleClickWordLookup` advanced setting)
- add `CmdEbookFontSizeDecrease` and `CmdEbookFontSizeIncrease` toolbar buttons to change reflowable ebook font size and reload the document
- add `CmdEbookFontSizeReset` to restore the built-in reflowable ebook font size from the Reading Font menu or command palette
- add cmd-line tools `SumatraPDF <tool> <args>`. Tools: draw, convert, audit, bake, clean, create, extract, info, merge, pages, poster, recolor, show, trim, grep, trace
- add `CmdPdShowInfo` (`Show PDF Info` in `Ctrl + k` [command palette](Command-Palette.md))
- add `CmdDocumentShowOutline` (`Show Document Outline` in `Ctrl + k` [command palette](Command-Palette.md))
- improved overlay scrollbar
- make thumbnails on home page scrollable
- add ability to register / unregister Windows preview handler and search filter from `Ctrl + k` command palette. Use "Register Windows Preview", Un-register Windows Preview", "Register Windows Search Filter", "Un-register Windows Search Filter".
- add `CmdToggleEscToExit` (`Toggle Esc to Exit` in `Ctrl + k` [command palette](Command-Palette.md)) to toggle `EscToExit` advanced setting
- add `CmdToggleTips` (`Toggle Tips` in `Ctrl + k` [command palette](Command-Palette.md)) to toggle `ShowTips` advanced setting
- add `CmdToggleReuseInstance` (`Toggle Reuse Instance` in `Ctrl + k` [command palette](Command-Palette.md)) to toggle `ReuseInstance` advanced setting
- add `CmdToggleChmUI` (`Toggle CHM UI` in `Ctrl + k` [command palette](Command-Palette.md)) to toggle dedicated CHM UI for CHM documents
- add `CmdAnalyzeSelectionWithDoubao` (`Ask Doubao` in selection context menu and `Ctrl + k` [command palette](Command-Palette.md)) to copy selected text to the clipboard and open [Doubao](https://www.doubao.com/chat/)
- remove hover dictionary lookup; double-click an English word to look it up offline with local `SumatraDict.*` files; dictionary files default to `{exedir}/dict` (override with `OfflineDictionaryPath`)
- add `CmdSetTabColor` (`Set Tab Color`) to set a custom color for a document's tab, available from tab right-click context menu
- add `CmdPdfCompress` (`Compress PDF` in `Ctrl + k` [command palette](Command-Palette.md)) to compress a PDF file
- add `CmdPdfDecompress` (`Decompress PDF` in `Ctrl + k` [command palette](Command-Palette.md)) to decompress a PDF file
- add `CmdPdfDeletePages` (`Delete Pages From PDF` in `Ctrl + k` [command palette](Command-Palette.md)) to delete pages from a PDF file
- add `CmdPdfExtractPages` (`Extract Pages From PDF` in `Ctrl + k` [command palette](Command-Palette.md)) to extract pages from a PDF file
- add `CmdPdfEncrypt` (`Encrypt PDF` in `Ctrl + k` [command palette](Command-Palette.md)) to encrypt a PDF file with a password using AES-256
- add `CmdPdfDecrypt` (`Decrypt PDF` in `Ctrl + k` [command palette](Command-Palette.md)) to decrypt an encrypted PDF file, removing password protection
- add `CmdDocumentExtractText` (`Extract Text From Document` in `Ctrl + k` [command palette](Command-Palette.md)) to extract text from document pages to a .txt file
- add `ToolbarText` parameter for `ExternalViewers` advanced setting to show external viewer as a toolbar button
- move `Scrollbars` advanced setting from `FixedPageUI` to top-level
- add `EBookUI.BackgroundColor` advanced setting to override background color for ebook documents (epub, mobi etc.)
- add `ComicBookUI.BackgroundColor` advanced setting to override the default black background for comic book files
- add `ImageUI.BackgroundColor` advanced setting to override the default black background for image files
- background color settings (`FixedPageUI.BackgroundColor`, `EBookUI.BackgroundColor`, `ComicBookUI.BackgroundColor`, `ImageUI.BackgroundColor`) accept `checkered` value to show a checkerboard transparency pattern
- add `CmdChangeBackgroundColor` (`Change Background Color` in `Ctrl + k` [command palette](Command-Palette.md)) to change document background color per-file or for all files of the same type
- `Ctrl + click` on a PDF link opens it in a new tab (instead of navigating in the current tab)
- you can now drag&drop selected text to another application, like a text editor
- added `List Printers` (`CmdListPrinters`) command to `Ctrl + k` Command Palette to list available printers
- add `-log-to-file <file>` cmd-line flag to log to a specific file (like `-log` but with custom log file path)
- move `DefaultImageZoom` advanced setting to `ImageUI.DefaultZoom`, default to `shrink to fit`
- improve `Toggle Use Tabs` (`CmdToggleUseTabs`). You can now transition between using tabs / not using tabs witout restarting the app
- allow showing menu bar when using tabs (previously menu bar was only shown when not using tabs)
- add `CmdScreenshot` (`Take Screenshot` in `Ctrl + k` [command palette](Command-Palette.md)) to capture screenshots of the desktop and all visible windows, saved as PNG files in `Screenshots` sub-directory of SumatraPDF data directory. Global hotkey (e.g. PrtSc) requires a Shortcuts entry.
- you can drag&drop images from a browser onto SumatraPDF window. We'll download it to Downloads folder and open
- add `CmdCropImage` (`Crop Image`) command for cropping images when viewing image files
- add `CmdResizeImage` (`Resize Image`) command for resizing images when viewing image files
- `Ctrl + V` pastes image from clipboard, saves as PNG in Downloads folder and opens it
- Can save images in different formats: PNG, JPEG, BMP, GIF, TIFF.
- add `CmdPdfBake` (`Bake PDF File` in `Ctrl + k` [command palette](Command-Palette.md)) to bake interactive form and annotation content into static graphics in a new PDF file
- add `Fullscreen` advanced setting with `ShowToolbar` and `ShowMenubar` options to show toolbar and menu bar in fullscreen mode. Use `F9` / `F8` to toggle them while in fullscreen
- add `CmdSetScreenshotHotkey` (`Set Screenshot Hotkey` in `Ctrl + k` [command palette](Command-Palette.md)) to set or remove a global hotkey for taking screenshots
- add `Show Errors` in right-click context menu for PDF documents that have mupdf warnings/errors
- add `CmdToggleSmoothScroll` (`Toggle Smooth Scroll`) command to toggle `SmoothScroll` advanced setting
- replace `HideScrollbars` and `UseOverlayScrollbar` settings with `Scrollbars` setting (values: `windows`, `smart`, `overlay`, `hidden`)
- add `CmdTabGroupSave` (`Save Tab Group`) and `CmdTabGroupRestore` (`Restore Tab Group`) commands to save and restore groups of tabs. Saved groups are persisted in `TabGroups` advanced setting
- add `CmdChangeScrollbar` (`Change Scrollbar`) command to open scrollbar mode dialog
- add `CmdZoomShrinkToFit` (`Shrink To Fit`) zoom mode: shows at 100% if page is smaller than view area, otherwise fits page
- add `CmdToggleScrollbarInSinglePage` (`Toggle Scrollbar In Single Page`) command to toggle `ScrollbarInSinglePage` advanced setting
- add `TabsMru` advanced setting and `CmdToggleTabsMru` (`Toggle Tabs MRU`) command to toggle it. It changes order of navigating tabs when usint `Ctrl + Tab` (`CmdNextTabSmart`)
- improve document properties for comic book files (CBZ, CBR, CB7, CBT). We now show list of image files and per-image EXIF metadata
- improve document properties for image files: size, dimensions, DPI, exif metadata
- support encrypted .cbz, .cbr files
- you can drag&drop images from PDF documents to other applications (web apps, image editors, file explorer etc.)
- pen/stylus input now works for text selection on Windows tablets
- add **Read Aloud (TTS)** with word-by-word highlight synced to speech; start from top of page, cursor, or selection; pause, continue, and stop from toolbar and **Read Aloud (TTS)** menu. **Configure natural voices via [NaturalVoiceSAPIAdapter](Read-Aloud-TTS.md#voice-setup)** — default Windows SAPI voices sound robotic without it
- add `CmdReadAloud`, `CmdPauseReadAloud`, `CmdContinueReadAloud`, `CmdStopReadAloud`, `CmdReadAloudFromTopPage`, and `CmdReadAloudSelection` commands; voice picker under **Read Aloud (TTS) → Voice**; advanced setting `ReadAloudVoiceId`
- add **Read Aloud (TTS) → Speed** submenu with preset speaking rates (0.5×, 0.75×, 1.0×, 1.25×, 1.5×, 2.0×); advanced setting `ReadAloudSpeakingRate`
- fix Edit Annotations window not restoring to the correct monitor in multi-monitor setups
- use `GetFileAttributesEx` instead of opening files for change detection on network drives, avoiding Windows Defender re-scans
- fix toolbar page number misalignment when `PrinterAccess` is revoked in `sumatrapdfrestrict.ini`
- add a **Match whole word** toggle to the Find bar (next to **Match Case**) so a search only matches complete words (fixes #4295)
- find-as-you-type now waits briefly after typing stops; pressing Enter searches immediately (fixes #4626)
- highlight the current search match with `FixedPageUI.SelectionColor` and other matches with a secondary orange color (fixes #5740)

## 3.6.1 (2026-04-06)

- bugfixes

## 3.6 (2026-03-17)

- add `DisableAntiAlias` advanced setting and `CmdToggleAntiAlias` command
- add `CmdShowAnnotations`, `CmdHideAnnotations`, `CmdToggleShowAnnotations` commands for temporarily hiding / showing annotations
- add `CmdToggleInverseSearch` to temporarily disable mouse click invoking tex inverse search
- add `bgcolor`, `opacity`, `textsize`, `borderWidth` arguments to `CmdCreateAnnot*` commands
- add `Annotations.FreeTextBackgroundColor` and `Annotations.FreeTextOpacity` advanced settings
- sort thumbnails on home page by most recently used date. Set advanced setting `HomePageSortByFrequentlyRead = true` to revert to pre-3.6 behavior of sorting by frequency of use.
- support brotli compression in PDF files
- in Command Palette, if you start search with `:` we show everything (like in 3.5)
- in Command Palette, when viewing opened files history (`#`), you can press Delete to remove the entry from history
- improved zooming:
  - zooming with pinch touch screen gesture or with ctrl + scroll wheel now zooms around the mouse position and does continuous zoom levels. Used to zoom around top-left corner and progress fixed zoom levels shown in menu
- include manual (`F1` to launch browser with documentation)
- add `LazyLoading` advanced setting, defaults to true. When restoring a session lazy loading delays loading a file until its tab is selected. Makes SumatraPDF startup faster.
- new commands in command palette (`Ctrl + K`):
  - `CmdCloseAllTabs` : "Close All Tabs"
  - `CmdCloseTabsToTheLeft` : "Close Tabs To The Left"
  - `CmdDeleteFile`: "Delete File"
  - `CmdToggleFrequentlyRead` : "Toggle Frequently Read"
  - `CmdToggleLinks` : "Toggle Show Links"
  - `CmdInvokeInverseSearch`
  - `CmdMoveTabRight` (`Ctrl + Shift + PageUp`), `CmdMoveTabLeft` (`Ctrl + Shift + PageDown`) to move tabs left / right, like in Chrome
- add ability to provide arguments to some commands when creating bindings in `Shortcuts`:
  - `CmdCreateAnnot*` commands take a color argument, `openedit` to automatically open edit annotations window when creating an annotation, `copytoclipboard` to copy selection to clipboard and `setcontent` to set contents of annotation to selection
  - `CmdScrollDown`, `CmdScrollUp` : integer argument, how many lines to scroll
  - `CmdGoToNextPage`, `CmdGoToPrevPage` : integer argument, how many pages to advance
  - `CmdNextTabSmart`, `CmdPrevTabSmart` (`Smart Tab Switch`), shortcut: `Ctrl + Tab`, `Ctrl + Shift + Tab`
- added `UIFontSize` advanced setting
- removed `TreeFontWeightOffset` advanced setting
- increase number of thumbnails on home page from 10 => 30
- add `ShowLinks` advanced setting and "Toggle Show Links" (`CmdToggleLinks`) for command palette
- default `ReuseInstance` setting to true
- added `Key` arg to `ExternalViewers` advanced setting (keyboard shortcut)
- added `Key` arg to `SelectionHandlers` advanced setting (keyboard shortcut)
- improved scrolling with mouse wheel and touch gestures
- theming improvements
- go back to opening settings file with default .txt editor (notepad most likely)
- don't exit fullscreen on double-click. must double-click in upper-right corner
- when opening via double-click, if `Ctrl` is pressed will always open in new tab (vs. activating existing tab)
- register for handling `.webp` files
- bug fix: Del should not delete an annotation if editing content
- bug fix: re-enable tree view full row select
- change: `CmdCreateAnnotHighlight` etc. no longer copies selection to clipboard by default. To get that behavior back, you can use `copytoclipboard` argument [instead](Commands.md#cmdcreateannothighlight-and-other-cmdcreateannot).
- change: `Ctrl + Tab` is now `CmdNextTabSmart`, was `CmdNextTab`. `Ctrl + Shift + Tab` is now `CmdPrevTabSmart`, was `CmdPrevTab`. You can [re-bind it](Customizing-keyboard-shortcuts.md) if you prefer old behavior
- `CmdCommandPalette` takes optional `mode` argument: `@` for tab selection, `#` for selecting from file history and `>` for commands.
- command palette no longer shows combined tabs/file history/commands. `CmdCommandPalette` only shows commands. Because of that removed `CmdCommandPaletteNoFiles` because now `CmdCommandPalette` behaves like it
- removed `CmdCommandPaletteOnlyTabs`, replaced by `CmdCommandPaletteNoFiles @`
- `Ctrl + Shift + K` no longer active, use `Ctrl + K`. You can restore this shortcut by binding it to `CmdCommandPalette >` command.
- add `Name` field for shortcuts. If given, the command will show up in Command Palette (`Ctrl + K`)
- closing a current tab now behaves like in Chrome: selects next tab (to the right). We used to select previously active tab, but that's unpredictable and we prefer to align SumatraPDF behavior with other popular apps.
- swapped key bindings: `i` is now CmdTogglePageInfo, `I` is CmdInvertColors. Several people were confused by accidentally typing `i` to invert colors, is less likely to type it accidentally
- allow creating custom themes in advanced settings in `Themes` section. [See docs](https://www.sumatrapdfreader.org/docs/Customize-theme-colors).
- improve scrolling with middle click drag [#4529](https://github.com/sumatrapdfreader/sumatrapdf/issues/4529)
- make built-in keyboard shortcuts work on non-us keyboards (cyrillic , hebrew etc.)
- add `CmdDuplicateInNewTab` (`Open Current Document In New Tab`) command

## 3.5.2 (2023-10-25)

- fix not showing tab text
- make menus in dark themes look more like standard menus (bigger padding)
- fix Bookmarks for folder showing bad file names
- update translations

## 3.5.1 (2023-10-24)

- fix uninstaller crash
- disable lazy loading of files when restoring a session

## 3.5 (2023-10-23)

- Arm 64-bit builds
- dark mode (menu `Settings / Theme` or `Ctrl + K` command `Select next theme`)
  you can use `i` (invert colors) to match the background / text color of rendered
  PDF document. Due to technical limitations, it doesn't work well with images
- `i` (invert colors) is remembered in settings
- `CmdEditAnnotations` select annotation under cursor and open annotation edit window
- rename `CmdShowCursorPosition` => `CmdToggleCursorPosition`
- add `Annotations [ FreeTextColor, FreeTextSize, FreeTextBorderWidth ]` settings
- ability to move annotations. `Ctrl + click` to select annotation and then move via drag & drop
- add `CmdCommandPaletteOnlyTabs` command with `Alt + K` shortcut
- exit full screen / presentation modes via double click with left mouse button
- ability to drag out a tab to open it in new window
- support opening `.avif` images (including inside .cbz/,cbr files)
- respect image orientation `exif` metadata in .jpeg and .png images
- support Adobe Reader syntax for opening files `/A "page=<pageno>#nameddest=<dest>search=<string>`
- add `Next Tab` / `Prev Tab` commands with `Ctrl + PageUp` / `Ctrl + PageDown` shortcuts
- keep Home tab open; add `NoHomeTab` advanced option to disable that
- add context menu to tabs
- bugfix: handle files we can't open in `next file in folder` / `prev file in folder` commands
- command palette: when search starts with `>`, only show commands, not files (like in Visual Studio Code)
- add `reopen last closed` command (`Ctrl + Shift + T`, like in web browsers)
- add `clear history` command
- can send commands via [DDE](https://www.sumatrapdfreader.org/docs/DDE-Commands)
- added `CmdOpenWithExplorer`, `CmdOpenWithDirectoryOpus`, `CmdOpenWithTotalCommander`, `CmdOpenWithDoubleCommander` commands
- enable `CmdCloseOtherTabs`, `CmdCloseTabsToTheRight` commands from command palette
- recognize `PgUp` / `PgDown` and a few more in keyboard shortcuts
- add `-disable-auto-rotation` cmd-line print option
- add `-dde` cmd-line option

## 3.4.6 (2022-06-08)

- fix crashes
- fix hang in Fit Content mode and Bookmark links

## 3.4.5 (2022-06-05)

- fix crashes

## 3.4.4 (2022-06-02)

- restore `HOME` and `END` in find edit field
- fix crashes

## 3.4.3 (2022-05-29)

- re-enable `Backspace` in edit field
- fix installation for all users when using custom installation directory
- re-enable `Copy Image` context menu for comic book files
- fix display of some PDF images
- fix slow loading of some ePub files

## 3.4.2 (2022-05-27)

- make keyboard accelerators work when tree view has focus
- fix `-set-color-range` and `-bg-color` replacing `MainWindowBackground`
- fix crash with incorrectly defined selection handlers

## 3.4.1 (2022-05-25)

- fix downloading of symbols for better crash reports

## 3.4 (2022-05-24)

- [Command Palette](Command-Palette.md)
- [customizable keyboard shortcuts](Customizing-keyboard-shortcuts.md)
- better support for epub files using mupdf's epub engine. Adds text selection and search in ebook files. Better rendering fidelity. On the downside, might be slower.
- [search / translate selected text](Customize-search-translation-services.md) with web services
  - we have few built-in and you can [add your own](https://www.sumatrapdfreader.org/settings/settings3-4#SelectionHandlers)
- installer: `-all-users` cmd-line arg for system-wide install
- added `Annotations.TextIconColor` and `TextIconType` advanced settings
- added `Annotations.UnderlineColor` advanced setting
- added `Annotations.DefaultAuthor` advanced setting
- `i` keyboard shortcuts inverts document colors `Shift + i` does what `i` used to do i.e. show page number
- `u` and `Shift + u` keyboard shortcuts adds underline annotation for currently selected text
- `Delete` / `Backspace` keyboard shortcuts delete an annotation under mouse cursor
- support `.svg` files
- faster scrolling with mouse wheel when cursor over scrollbar
- add `-search` cmd-line option and `[Search("<file>", "<search-term>")]` DDE command
- a way to get list of used fonts in properties window
- support opening `.heic` image files (if Windows heic codec is installed)
- add experimental smooth scrolling (enabled with `SmoothScroll` advanced setting)

## 3.3.3 (2021-07-20)

- fix a crash in PdfFilter.dll

## 3.3.2 (2021-07-19)

- restore showing Table Of Contents for `.chm` files
- fix crashes

## 3.3.1 (2021-07-14)

- fix rotation in DjVu documents

## 3.3 (2021-07-06)

- added support for adding / removing / editing annotations in PDF files. Read [the tutorial](Editing-annotations.md)
- new toolbar
  - changed toolbar to scale with DPI by using new, vector icons
  - added rotate left / right to the toolbar
  - new toolbar:

  ![Toolbar](img/toolbar.png)

- added ability to hide scrollbar (more screen space for the document). Use right-click context menu.
- add `-paperkind=${num}` printing option ([checkin](https://github.com/sumatrapdfreader/sumatrapdf/pull/1815/commits/2104e6104ea759dc4f839c7e8be5973f5a4f0488))

Minor improvements and bug-fixes:

- advanced setting to change font size in bookmarks / favorites tree view e.g. `TreeFontSize = 12`
- support newer versions of ghostscript (≥ 9.54) for opening `.ps` files
- support jpeg-xr images in `.xps` files
- restore tooltips (regression in 3.2)
- update mupdf to latest version
- make silent installation always silent
- don't crash when attempting to zoom in on home page
- don't show "manga" view menu item for documents that are not comic books
- allow opening `fb2.zip` files ([#1657](https://github.com/sumatrapdfreader/sumatrapdf/issues/1657))
- restore ability to save embedded files (fixes [#1557](https://github.com/sumatrapdfreader/sumatrapdf/issues/1557))
- `Alt + Space` opens a sys menu

## 3.2 (2020-03-15)

This release upgrades the core PDF parsing and rendering library mupdf to the latest version. This fixes PDF rendering bugs and improves performance.

Added support for multiple windows with tabs:

- added `File / New Window` (`Ctrl-n`) which opens a new window
- to compare the same file side-by-side, `Ctrl-Shift-n` shortcut opens current file in a new window. The same file is now opened in 2 windows that you can re-arrange as needed
- `-new-window` cmd-line option will open the document in new window
- if you hold `SHIFT` when drag&dropping files from Explorer (and other apps), the file will be opened in a new window

Improved management of favorites:

- context menu (right mouse click) on the document area adds menu items for:
  - showing / hiding favorites view
  - adding current page to favorites (or removing if already is in favorites)
- context menu in bookmarks view adds menu item for adding selected page to favorites

This release no longer supports Windows XP. Latest version that support XP is 3.1.2 that you can download from

[https://www.sumatrapdfreader.org/download-prev.html](https://www.sumatrapdfreader.org/download-prev.html)

## 3.1.2 (2016-08-14)

- fixed issue with icons being purple in latest Windows 10 update
- tell Windows 10 that SumatraPDF can open supported file types

## 3.1.1 (2015-11-02)

- (re)add support for old processors that don’t have SSE2
- support newer versions of unrar.dll
- allow keeping the browser plugin if it’s already installed
- crash fixes

## 3.1 (2015-10-24)

- 64bit builds
- all documents are restored at startup if a window with multiple tabs is closed (or if closing happened through File -> Exit); this can be disabled through the `RestoreSession` advanced setting
- printing happens (again) always as image which leads to more reliable results at the cost of requiring more printer memory; the "Print as Image" advanced printing option has been removed
- scrolling with touchpad (e.g. on Surface Pro) now works
- many crash and other bug fixes

## 3.0 (2014-10-18)

- Tabs! Enabled by default. Use Settings/Options... menu to go back to the old UI
- support table of contents and links in ebook UI
- add support for PalmDoc ebooks
- add support for displaying CB7 and CBT comic books (in addition to CBZ and CBR)
- add support for LZMA and PPMd compression in CBZ comic books
- allow saving Comic Book files as PDF
- swapped keybindings:
  - `F11` : Fullscreen mode (still also `Ctrl + Shift + L`)
  - `F5` : Presentation mode (also `Shift + F11`, still also `Ctrl + L`)
- added a document measurement UI. Press `m` to start. Keep pressing `m` to change measurement units
- new advanced settings: `FullPathInTitle`, `UseSysColors` (no longer exposed through the Options dialog), `UseTabs`
- replaced non-free UnRAR with a free RAR extraction library. If some CBR files fail to open for you, download unrar.dll from https://www.rarlab.com/rar_add.htm and place it alongside SumatraPDF.exe
- deprecated browser plugin. We keep it if it was installed in earlier version

## 2.5.2 (2014-05-13)

- use less memory for comic book files
- PDF rendering fixes

## 2.5.1 (2014-05-07)

- hopefully fix frequent ebook crashes

## 2.5 (2014-05-05)

- 2 page view for ebooks
- new keybindings:
  - `Ctrl + PgDn`, `Ctrl + Right` : go to next page
  - `Ctrl + PgUp`, `Ctrl + Left` : go to previous page
- 10x faster ebook layout
- support JP2 images
- new **[advanced settings](https://www.sumatrapdfreader.org/settings.html)**: `ShowMenuBar`, `ReloadModifiedDocuments`, `CustomScreenDPI`
- left/right clicking no longer changes pages in fullscreen mode (use Presentation mode if you rely on this feature)
- fixed multiple crashes and made multiple minor improvements

## 2.4 (2013-10-01)

- full-screen mode for ebooks (`Ctrl-L`)
- new key bindings:
  - `F9` - show/hide menu (not remembered after quitting)
  - `F8` - show/hide toolbar
- support WebP images (standalone and in comic books)
- support for RAR5 compressed comic books
- fixed multiple crashes

## 2.3.2 (2013-05-25)

- fix changing a language via Settings/Change Language

## 2.3.1 (2013-05-23)

- don't require SSE2 (to support old computers without SSE2 support)

## 2.3 (2013-05-22)

- greater configurability via **[advanced settings](https://www.sumatrapdfreader.org/settings.html)**
- "Go To Page" in ebook ui
- add View/Manga Mode menu item for Comic Book (CBZ/CBR) files
- new key bindings:
  - `Ctrl-Up` : page up
  - `Ctrl-Down` : page down
- add support for OpenXPS documents
- support Deflate64 in Comic Book (CBZ/CBR) files
- fixed missing paragraph indentation in EPUB documents
- printing with "Use original page sizes" no longer centers pages on paper
- reduced size. Installer is ~1MB smaller
- downside: this release no longer supports very old processors without **[SSE2 instructions](https://en.wikipedia.org/wiki/SSE2)**. Using SSE2 makes Sumatra faster. If you have an old computer without SSE2, you need to use 2.2.1.

## 2.2.1 (2013-01-12)

- fixed ebooks sometimes not remembering the viewing position
- fixed Sumatra not exiting when opening files from a network drive
- fixes for most frequent crashes and PDF parsing robustness fixes

## 2.2 (2012-12-24)

- add support for FictionBook ebook format
- add support for PDF documents encrypted with Acrobat X
- “Print as image” compatibility option in print dialog for documents that fail to print properly
- new command-line option: `-manga-mode [1|true|0|false]` for proper display of manga comic books
- many robustness fixes and small improvements

## 2.1.1 (2012-05-07)

- fixes for a few crashes

## 2.1 (2012-05-03)

- support for EPUB ebook format
- added File/Rename menu item to rename currently viewed file (contributed by Vasily Fomin)
- support multi-page TIFF files
- support TGA images
- support for some comic book (CBZ) metadata
- support JPEG XR images (available on Windows Vista or later, for Windows XP the **[Windows Imaging Component](https://www.microsoft.com/en-us/download/details.aspx?id=32)** has to be installed)
- the installer is now signed

## 2.0.1 (2012-04-08)

- fix loading `.mobi` files from command line
- fix a crash loading multiple `.mobi` files at once
- fix a crash showing tooltips for table of contents tree entries

## 2.0 (2012-04-02)

- support for **[MOBI](https://blog.kowalczyk.info/articles/mobi-ebook-reader-viewer-for-windows.html)** eBook format
- support opening CHM documents from network drives
- a selection can be copied to a clipboard as an image by using right-click context menu
- using ucrt to reduce program size

## 1.9 (2011-11-23)

- support for **[CHM](https://blog.kowalczyk.info/articles/chm-reader-viewer-for-windows.html)** documents
- support touch gestures, available on Windows 7 or later. Contributed by Robert Prouse
- open linked audio and video files in an external media player
- improved support for PDF transparency groups

## 1.8 (2011-09-18)

- improved support for PDF form text fields
- various minor improvements and bug fixes
- speedup handling some types of djvu files

## 1.7 (2011-07-18)

- favorites
- improved support for right-to-left languages e.g. Arabic
- logical page numbers are displayed and used, if a document provides them (such as i, ii, iii, etc.)
- allow to restrict SumatraPDF's features with more granularity; see **[sumatrapdfrestrict.ini](https://github.com/sumatrapdfreader/sumatrapdf/blob/master/docs/sumatrapdfrestrict.ini)** for documentation
- `-named-dest` also matches strings in table of contents
- improved support for EPS files (requires Ghostscript)
- more robust installer
- many minor improvements and bugfixes

## 1.6 (2011-05-30)

- add support for displaying DjVu documents
- display Frequently Read list when no document is open
- add support for displaying Postscript documents (requires recent Ghostscript version to be already installed)
- add support for displaying a folder containing images: drag the folder to SumatraPDF window
- support clickable links and a Table of Content for XPS documents
- display printing progress and allow to cancel it
- add Print toolbar button
- experimental: previewing of PDF documents in Windows Vista and 7. Creates thumbnails and displays documents in Explorer's Preview pane. Needs to be explicitly selected during install process. We've had reports that it doesn't work on Windows 7 x64.

## 1.5.1 (2011-04-26)

- fixes for rare crashes

## 1.5 (2011-04-23)

- add support for viewing XPS documents
- add support for viewing CBZ and CBR comic books
- add File/Save Shortcut menu item to create shortcuts to a specific place in a document
- add context menu for copying text, link addresses and comments. In browser plugin it also adds saving and printing commands
- add folder browsing (`Ctrl + Shift + Right` opens next PDF document in the current folder, `Ctrl + Shift + Left` opens previous document)

## 1.4 (2011-03-12)

- browser plugin for Firefox/Chrome/Opera (Internet Explorer is not supported). It's not installed by default so you have to check the appropriate checkbox in the installer
- IFilter that enables full-text search of PDF files in Windows Desktop Search (i.e. search from Windows Vista/7's Start Menu). Also not installed by default
- scrolling with right mouse button
- you can choose a custom installation directory in the installer
- menu items for re-opening current document in Foxit and PDF-XChange (if they're installed)
- we no longer compress the installer executable with mpress. It caused some anti-virus programs to falsely report Sumatra as a virus. The downside is that the binaries on disk are now bigger. Note: we still compress the portable .zip version
- `-title` cmd-line option was removed
- support for AES-256 encrypted PDF documents
- fixed an integer overflow reported by Jeroen van der Gun and other small fixes and improvements to PDF handling

## 1.3 (2011-02-04)

- improved text selection and copying. We now mimic the way a browser or Adobe Reader works: just select text with mouse and use `Ctrl-C` to copy it to a clipboard
- `Shift + Left Mouse` now scrolls the document, `Ctrl + Left mouse` still creates a rectangular selection (for copying images)
- `c` shortcut toggles continuous mode
- `+` / `*` on the numeric keyboard now do zoom and rotation
- added toolbar icons for Fit Page and Fit Width and updated the look of toolbar icons
- add support for back/forward mouse buttons for back/forward navigation
- 1.2 introduces a new full screen mode and made it the default full screen mode. Old mode was still available but not easily discoverable. We've added View/Presentation menu item for new full screen mode and View/Fullscreen menu item for the old full screen mode, to make it more discoverable
- new, improved installer
- improved zoom performance (zooming to 6400% no longer crashes)
- text find uses less memory
- further printing improvements
- translation updates
- updated to latest mupdf for misc bugfixes and improvements
- use libjpeg-turbo library instead of libjpeg, for faster decoding of some PDFs
- updated openjpeg library to version 1.4 and freetype to version 2.4.4
- fixed 2 integer overflows reported by Stefan Cornelius from Secunia Research

## 1.2 (2010-11-26)

- improved printing: faster and uses less resources
- add `Ctrl-Y` as a shortcut for Custom Zoom
- add `Ctrl-A` as a shortcut for Select All Text
- improved full screen mode
- open embedded PDF documents
- allow saving PDF document attachments to disk
- latest fixes and improvements to PDF rendering from mupdf project

## 1.1 (2010-05-20)

- added book view (“View/Book View” menu item) option. It’s known as “Show Cover Page During Two-Up” in Adobe Reader
- added “File/Properties” menu item, showing basic information about PDF file
- added “File/Send by email” menu
- added export as text. When doing “File/Save As”, change “Save As types” from “ PDF documents” to “Text documents”. Don’t expect miracles, though. Conversion to text is not very good in most cases.
- auto-detect commonly used TeX editors for inverse-search command
- bug fixes to PDF handling (more PDFs are shown correctly)
- misc bug fixes and small improvements in UI
- add `Ctrl +` and `Ctrl –` as shortcuts for zooming (matches Adobe Reader)

## 1.0.1 (2009-11-27)

- many memory leaks fixed (Simon Bünzli)
- potential crash due to stack corruption (pointed out by Christophe Devine)
- making Sumatra default PDF reader no longer asks for admin privileges on Vista/Windows 7
- translation updates

## 1.0 (2009-11-17)

- lots of small bug fixes and improvements

## 0.9.4 (2009-07-19)

- improved PDF compatibility (more types of documents can be rendered)
- added settings dialog (contributed by Simon Bünzli)
- improvements in handling unicode
- changed default view from single page to continuous
- SyncTex improvements (contributed by William Blum)
- add option to not remember opened files
- a new icon for documents association (contributed by George Georgiou)
- lots of bugfixes and UI polish

## 0.9.3 (2008-10-07)

- fix an issue with opening non-ascii files
- updated Japanese and Brazilian translation

## 0.9.2 (2008-10-06)

- ability to disable auto-update check
- improved text rendering - should fix problems with overlapping text
- improved font substitution for fonts not present in PDF file
- can now open PDF files with non-ascii names
- improvements to DDE (contributed by Danilo Roascio)
- SyncTex improvements
- improve persistence of state (contributed by Robert Liu)
- fix crash when pressing `Cancel` when entering a password
- updated translations

## 0.9.1 (2008-08-22)

- improved rendering of some PDFs
- support for links inside PDF file
- added `-restrict` and `-title` cmd-line options (contributed by Matthew Wilcoxson)
- enabled SyncTex support which mistakenly disabled in 0.9
- misc fixes and translation updates

## 0.9 (2008-08-10)

- add `Ctrl-P` as print shortcut
- add `F11` as full-screen shortcut
- password dialog no longer shows the password
- support for AES-encrypted PDF files
- updates to SyncTeX/PdfSync integration (contributed by William Blum)
- add `-nameddest` command-line option and DDE commands for jumping to named destination (contributed by Alexander Klenin)
- add `-reuse-instance` command-line option (contributed by William Blum)
- add DDE command to open PDF file (contributed by William Blum)
- removed poppler rendering engine resulting in smaller program and updated to latest mupdf sources
- misc bugfixes and translation updates

## 0.8.1 (2008-05-27)

- automatic reloading of changed PDFs (contributed by William Blum)
- tex integration (contributed by William Blum)
- updated icon for case-sensitivity selection in find (contributed by Sonke Tesch)
- language change is now a separate dialog instead of a menu
- remember more settings (like default view)
- automatic checks for new versions
- add command-line option `-lang $lang`
- add command-line option `-print-dialog` (contributed by Peter Astrand)
- ESC or single mouse click hides selection
- fix showing boxes in table of contents tree
- translation updates

## 0.8 (2008-01-01)

- added search (contributed by MrChuoi)
- added table of contents (contributed by MrChuoi)
- added many translations
- new program icon
- fixed printing
- fixed some crashes
- rendering speedups
- fixed loading of some PDFs
- add command-line option `-esc-to-exit`
- add command-line option `-bgcolor $color`

## 0.7 (2007-07-28)

- added ability to select the text and copy to clipboard - contributed by Tomek Weksej
- made it multi-lingual (13 translations)
- added Save As option
- list of recently opened files is updated immediately
- fixed `.pdf` extension registration on Vista
- added ability to compile as DLL and C# sample application - contributed by Valery Possoz
- mingw compilation fixes and project files for CodeBlocks - contributed by MrChuoi
- fixed a few crashes
- moved the sources to Google Code project hosting

## 0.6 (2007-04-29)

- enable opening password-protected PDFs
- don't allow printing in PDFs that have printing forbidden
- don't automatically reopen files at startup
- fix opening PDFs from network shares
- new, better icon
- reload the document when changing rendering engine
- improve cursor shown when dragging
- fix toolbar appearance on XP and Vista with classic theme
- when MuPDF engine cannot load a file or render a page, we fallback to poppler engine to make rendering more robust
- fixed a few crashes

## 0.5 (2007-03-04)

- fixed rendering problems with some PDF files
- speedups - the application should feel snappy and there should be less waiting for rendering
- added `r` keybinding for reloading currently open PDF file
- added `Ctrl-Shift-+` and `Ctrl-Shift--` keybindings to rotate clockwise and counter-clockwise (just like Acrobat Reader)
- fixed a crash or two

## 0.4 (2007-02-18)

- printing
- ask before registering as a default handler for PDF files
- faster rendering thanks to alternative PDF rendering engine. Previous engine is available as well.
- scrolling with mouse wheel
- fix toolbar issues on win2k
- improve the way fonts directory is found
- improvements to portable mode
- uninstaller completely removes the program
- changed name of preferences files from `prefs.txt` to `sumatrapdfprefs.txt`

## 0.3 (2006-11-25)

- added toolbar for most frequently used operations
- should be more snappy because rendering is done in background and it caches one page ahead
- some things are faster

## 0.2 (2006-08-06)

- added facing, continuous and continuous facing viewing modes
- remember history of opened files
- session saving i.e. on exit remember which files are opened and restore the session when the program is started without any command-line parameters
- ability to open encrypted files
- "Go to page dialog"
- less invasive (less yellow) icon that doesn't jump at you on desktop
- fixed problem where sometimes text wouldn't show (better mapping for fonts; use a default font if can't find the font specified in PDF file)
- handle URI links inside PDF documents
- show "About" screen
- provide a download in a .zip file for those who can't run installation program
- switched to poppler code instead of xpdf

## 0.1 (2006-06-01)

- first version released
