# Version history

## next

- home: restore the two icon toggles (list/thumbnails, recent/frequent); drop the “Recently Opened” dropdown and header “Open a document…” link
  首页：恢复两个图标开关（列表/缩略图、最近/最热），不再用「最近打开」下拉和顶栏「打开文档」

- extract bookmarks: `2、设区市负担…万元` stays in the outline even when the same page lists `附件2` and a later form is `附件2`; only 函末 `2.申请表` / `2.一览表` is the same item as that attachment
  提取目录：正文「2、设区市负担…万元」不要因为同页函末清单和后面的附件2就删掉；只有「2.申请表」「2.一览表」这种才和附件2是同一条

- extract bookmarks: `附件 2` (space before the digit) is attachment 2, not a second `附件1`; glue a following large `…填写规范` title; do not remap `附件1 年度考核登记表` onto that spec
  提取目录：「附件 2」中间有空格仍是附件2，不要并进附件1；下面大号「…填写规范」收成附件2的标题，不要把附件1的跳转改到这份规范上

- OCR toolbar: Auto-save (`CmdToggleOcrAutoSave`, setting `OcrAutoSave`, default off). When checked, Recognize All Scanned Pages overwrites the current PDF after the scan, and bookmark extraction writes the outline to disk
  OCR 下拉增加「自动保存」（`OcrAutoSave`，默认关）。勾选后，识别完全文会覆盖保存当前 PDF；提取目录也会写入文件

- extract bookmarks: drop a leftover `征求〈…要点` of the 函 title; cover-list `附件1 某某` and a later large `某某` become one `附件1` at the heading
  提取目录：函标题残片「征求〈…要点」不要；函末「附件1 某某」和后面大号「某某」合成一条「附件1」，跳到大标题

- extract bookmarks: 红头 `…领导小组办公室` is not part of the title — keep `关于印发《…》的通知` so the sidebar shows the notice, not the letterhead
  提取目录：红头「…领导小组办公室」不拼进标题，书签只留「关于印发《…》的通知」，侧栏先看见通知本身

- extract bookmarks: a 通知 may mix `一、` then `1.` with `三、` then `（一）`/`（二）` then `1.`; `1.12333电话` stays a list item under `（二）`, not a `1.1` sibling
  提取目录：同一份通知里，一、下面可以直接跟 1.，三、也可以先（一）（二）再跟 1.；「1.12333电话」仍是（二）下的条目，不当成 1.1 跟（二）平级

- read aloud: vertical books turn to the next page when TTS leaves the current one (column highlights stay mid-screen, so the old 78% Y threshold never fired)
  朗读：竖版读完当前页会翻到下一页（竖栏高亮一直在画面中部，原先按纵向 78% 跟读不会触发翻页）

- home: pinned list/thumbnail pushpins go near-black (near-white in dark chrome); idle pins stay mid-gray — not the theme link color
  首页：钉住的图钉变深（深色主题变亮），未钉住仍是中灰，不再用链接色

- extract bookmarks: official TOC titles drop the issuing-agency prefix (中共…人民政府 / …厅) so the sidebar shows 关于… / 印发… / 转发… first
  提取目录：公文标题去掉发文单位（中共…人民政府 / …厅），书签栏先显示关于… / 印发… / 转发…

- home: opening the tab only loads on-screen thumbnails and does not restat every history file (missing/network paths no longer freeze the UI)
  首页：点开只加载屏幕上的封面，不再对历史里每一项做磁盘探测（缺失/网络路径不再把界面卡死）

- OCR: vertical books stay upright — do not bake a 90° page rotate. Recognize tall columns (crop 90° CCW, right-to-left order) without transposing the page. Official landscape forms still stand up.
  OCR：竖版书不再被转 90°。按竖栏识别（裁块逆时针转、从右往左排），页面保持正立。公文横表仍会立起来。

- extract bookmarks: `1.…@qq.com` is a task line, not a `600mm` spec row — keep it under 四、 when 2. 3. are already there
  提取目录：1.…@qq.com 是事项，不是 600mm 规格行；四、下已有 2. 3. 时把这条 1. 补上

- extract bookmarks: 函末 `附件：报名表` is a cite — keep one later real 附件 heading, do not keep three copies or invent 附件2
  提取目录：函末「附件：报名表」是正文引用，只留后面真正的附件标题，不再留三份或编出附件2

- extract bookmarks: CID / missing-ToUnicode titles like 人汴 / 通亦 / 部11 are not 繁体 — OCR those pages and rewrite to 人社 / 通办 / 部门
  提取目录：人汴、通亦、部11 不是繁体，是字库 ToUnicode 对错了；这类页走 OCR，并改回人社、通办、部门

- extract bookmarks: 印发《方案》的通知 stays the first TOC item; do not rewrite it to the inner 方案 name
  提取目录：印发《方案》的通知仍作第一条，不再收成书名号里的方案名

- dark mode: newly drawn shapes, lines, and stamps appear immediately (drop the match-theme page bitmap when annotations change)
  暗黑模式：画完图形、线条、印章后马上显示（改标注时丢掉跟随主题的整页缓存）

- extract bookmarks: 3-line 意见 titles (`中共…人民政府` / `关于…` / `“一号…”的意见`) become the TOC root; do not treat the issuer line as 红头 or glue the date. CID `关千` / `［程` and a basename ending `（OA）` still count as that title; do not salvage `和自我革新相结合、` as `1.`
  提取目录：三行意见标题（中共…人民政府 / 关于… / “一号…”的意见）收成目录根；签发机关行不当红头，也不要把日期拼进标题。CID「关千／［程」和文件名末「（OA）」仍算这份标题；不要把「和自我革新相结合、」补成 1.

- OCR toolbar dropdown: OCR region, Recognize All Scanned Pages (Fast) / (Accurate). Fast / Accurate clear this session's OCR and re-recognize every page.
  OCR 下拉：框选识别、识别所有扫描页（快速）/（精确）。快速 / 精确会清除本次识别结果并重新识别全部页面。

- OCR: PP-OCRv6 Tiny/Small (Fast / Balanced / experimental Hybrid) with paired dictionaries; current page uses Small, full-document OCR uses Tiny. ONNX Runtime 1.20.x so RapidOCR v6 IR 10 models load without a header patch
  OCR：支持 PP-OCRv6 Tiny/Small（Fast / Balanced / 实验 Hybrid），识别模型与字典成对；当前页用 Small，全文用 Tiny。ONNX Runtime 升到 1.20.x，直接加载 RapidOCR v6 的 IR 10 模型，不再改模型头

- extract bookmarks: two 办法 in one PDF keep their own 附件1/2/3; landscape scanned 公文 body stands up with /Rotate 270
  提取目录：同一 PDF 里两份办法各自保留附件 1/2/3；横向扫描的办法正文用 /Rotate 270 立起来，不再侧着

- toolbar: stamp after the line tools — click to place a built-in PDF stamp (Draft by default; last type chosen in the editor is reused). Drag to size. Ctrl+click to keep stamping
  工具栏：画线工具后增加图章。单击盖系统内置图章（默认 Draft，编辑器里选过的类型会记住）。拖动改大小，Ctrl+点击可连续盖章

- extract bookmarks: restore missing English word spaces (ACupofCider → A Cup of Cider)
  提取目录：补回英文词间被 OCR 吃掉的窄空格，不再粘成 ACupofCider

- home: list-view filenames use the theme text color instead of washed-out gray
  首页：列表模式的书名用主题正文色，不再发灰发淡

- extract bookmarks: still find a printed Contents page when OCR glues "Contents" to the next title (ContentsA / TableofContentsACupofCider)
  提取目录：OCR 把 Contents 和下一条标题粘在一起时仍能认出印刷目录，不再整页漏掉

- extract bookmarks: if OCR/text exists but there is no printed TOC or chapter list, say so — do not tell the user to recognize text again
  提取目录：已经有 OCR/文字、只是没有印刷目录或章节标题时，直说无法提取，不再误报「请先识别文字」

- sidebar: dragging the TOC splitter keeps the last page on screen without flashing; full relayout runs on mouse-up
  拖目录分隔条时正文保留上一帧且不再闪抖，松手后再重排

- home: recently-opened toggle uses a stroke history icon (gap 9–11 o'clock, filled arrowhead, L-shaped hands, no white tile)
  首页：「最近打开」为描边历史图标：开口 9 点到 11 点、实心箭头、中间 L 形时针分针，无白底

- home: search matches the file name only, not folders in the path
  首页搜索只匹配文件名，目录名命中不算

- home: thumbnail search highlights stay inside the filename box (no stray yellow squares in an empty slot)
  首页：缩略图搜索高亮不出文件名框，不再在空栏里画出黄块

- home: list/grid and recent/frequent are icon toggles with tooltips
  首页：列表/缩略图、最近/经常打开都是图标开关，悬停有提示

## 3.7.30 (2026-08-25)

- home: thumbnail cards have pin/remove; right-click a card for Open / Show in folder / Pin / Remove; empty canvas offers Remove missing files from home (fixed-drive only). Thumbnails grow/shrink slightly to fill the window width (`HomePageThumbnailDx`)
  首页：缩略图可钉住/删除；右键打开、在文件夹中显示、钉住、从历史删除；空白处可清掉丢失的本地文件（U 盘/网络盘不算）。缩略图略微伸缩铺满窗口宽度（`HomePageThumbnailDx`）

- home: list rows are two-line (name / path + size); list covers fill the thumb box; header view toggle and title sit on one baseline; list pin/remove capsules match the page instead of flashing white
  首页：列表改成两行（文件名 / 路径+大小）；列表封面铺满缩略图格；顶栏视图开关和标题对齐；列表钉住/删除胶囊跟页面底色，不再刺眼白块

- fullscreen: Ctrl+Tab document switcher and Alt+Tab work again. Cover leftover Win11 tray widgets with a one-shot topmost bump instead of staying always-on-top
  全屏：Ctrl+Tab 文档切换浮层和 Alt+Tab 恢复可用。进全屏时顶一下托盘残留控件，不再始终置顶

- open: landscape 图解/PPT PDFs with `/PageLayout /TwoPageRight` no longer start in book view (cover in the right slot, looking like half a page). Automatic uses single-column until you pick facing/book yourself.
  打开：横版图解/PPT 若带了双页目录标记，不再一进来就是书籍模式（封面挤在右栏、只露半张）。自动布局改用单栏，需要双页再手动选。

- open: fixed-layout EPUB covers (`<meta viewport>` larger than the reflow page) no longer layout at the placeholder 750×1025 size while rendering at 1398×2000. Opening such a book uses fit-width so the cover fills the pane; fit-page still centers a cover that fits.
  打开：固定版式 EPUB 封面不再按占位页尺寸排、按真实 viewport 画。打开时用适合宽度铺满阅读区；适合页面时能放下的封面仍居中。

- dark match-theme: Word 红头/标题 that are a 2×2 color chip plus a glyph SMask remap like ink, instead of staying black (or stretching into a red smear)
  深色「匹配主题」：Word 红头/标题若是 2×2 色块加字形软遮罩，按墨色重映射，不再黑字叠黑底，也不再拉成一条红带

- extract bookmarks: born-digital PDFs parse the printed Contents in the front and stop; scans still OCR / search the rest of the book and may refine dests. Show progress immediately.
  提取目录：电子书只从前部印刷目录取书签，不再整本扫、也不做扫描书那套页码校准；扫描书仍可 OCR、往后找、再校准。一点就显示进度。

- save PDF changes: when the open file cannot be overwritten, write a sidecar next to it and replace after close — do not reopen `%TEMP%\smpXXXX.tmp` as the document
  保存 PDF：原文件被占用写不进去时，先写到旁边再替换回去，不再把 `%TEMP%\smpXXXX.tmp` 当成当前文档打开

- save searchable PDF: do not strip a page's visible text unless it is a scanned image; a text-only page kept its hidden OCR layer and looked blank
  保存可搜索 PDF：只有扫描图页才剥掉旧文字层。纯文字页以前会留下隐形 OCR，打开就是空白

## 3.7.29 (2026-08-24)

- sidebar splitter: live drag repaints the TOC tree, footer, and canvas immediately (no leftover bits / overlay scrollbar)
  拖侧栏分隔条时立刻重绘目录树、底部栏和画布，不再留下残影或旧滚动条

- dark theme: inverted scan paper keeps the theme background (Dracula `#282A36`) instead of sharpening to near-black
  深色主题：反色扫描页的纸色保持主题背景（Dracula 为 `#282A36`），不再被锐化成近纯黑

- enhance-mode: changing the printed page box or ± updates the dest immediately, same as Link
  对准印刷目录：改页码框或加减后立刻刷新 dest，再点标题跳到新页

- enhance-mode: Save shows a short notification after writing the PDF
  对准印刷目录：点「保存」写入后弹出提示

- enhance-mode: Link updates the bookmark dest immediately; clicking the title jumps to the new page, not the old outline URI
  对准印刷目录：点「关联」后立刻改 dest，再点标题跳到新页，不再走旧书签 URI

- enhance-mode: front matter (序, i, ii, A) shows the real page label, not a negative offset such as -3
  对准印刷目录：目录前的序等用原本的页码（i / ii / A），不再用 offset 算出的负数

- PDF TOC: hovering or clicking an item after extract/edit no longer crashes when the outline dest URI is stale (`strchr` on `(char*)-1`)
  PDF 目录：提取或改书签后悬停/单击不再因过期的 outline URI 崩溃

- enhance-mode: click a TOC title to jump to its dest page; double-click jumps to that entry on the printed contents pages. Page box, ±, locate, link, merge, and delete do not change the view
  对准印刷目录：单击标题跳到关联页；双击跳到它在印刷目录里的位置。页码框、加减、定位、关联、合并、删除不跳转

- extract bookmarks (books): `(144)` / `（155）` on a printed-TOC line is that entry's page, not a heading number, and is stripped from the title (right-column page still wins when both exist)
  提取书目录：`(144)` / `（155）` 是这条的印刷页，不当编号、不留在标题里（同一行右边还有页码时用右边的）

- extract bookmarks (books, phase 4): keep OCR x-gaps so a right-side page number stays its own span; printed 目录 can start later and continue across a couple of weak pages, and titles may wrap onto the next contents page; books with no printed TOC cluster large/bold body lines as headings (still ignore 公文 `一、`)
  提取书目录（第4阶段）：OCR 大间距把右侧页码拆成独立 span；印刷目录可出现在更后面、中间隔一两页插图也接着认，标题可折到下一页目录；没有印刷目录时按正文大字/加粗聚标题（仍不把公文的 `一、` 当书的骨架）

- extract bookmarks (books): `(144)` / `（147）` at the start of a printed-TOC line is a page number, not a heading number, and does not deepen the outline (公文 `(1)(2)` still does)
  提取书目录：行首 `（144）` 这类是页码，不当目录编号、不往下缩进（公文的 `（1）（2）` 仍是条款层级）

- PDF TOC: `Ctrl+B` with text selected inserts the new item as the first child if the current bookmark has children; otherwise as the next sibling (same level, immediately below)
  PDF 目录：选定正文后 `Ctrl+B`，当前条目有子项则作为第一条子项；没有则与当前平级、插在正下方

- PDF bookmark sidebar: drag drop line is indented to the target level. Dropping after an expanded item draws the line after its last child (sibling, before the next chapter), not under the title (which looked like first child)
  书签栏：拖放线按目标层级缩进。拖到已展开条目的「下面」时，线画在它整棵子树后面（并列、插到下一章前），不再画在标题底下（看起来像第一条子项）

- PDF bookmark sidebar / enhance-mode: merge or add a TOC item keeps the sidebar scroll and expand state (no jump back to the first row)
  书签栏 / 对准印刷目录：合并或添加一条后保留侧栏滚动和展开，不再跳回第一条

- PDF bookmark sidebar: clicking a selected title no longer starts rename (too easy to trigger). Use F2; double-click still jumps
  PDF 书签栏：再点已选中标题不再改名（容易误触）。用 F2；双击仍跳转

- enhance-mode: bottom **Save** writes bookmarks and stays in calibration; **Exit** leaves. Former labels were Write bookmarks / Cancel
  对准印刷目录：底栏「保存」写入书签后不退出；「退出」离开。原先是「写入书签」/「取消」

- enhance-mode: Link / Set to current page always updates the printed-page box, even when the row already had a number (a stale 56 no longer hides the pin)
  对准印刷目录：关联 / 关联到当前页会改印刷页框，已有数字（比如错的 56）也会被当前页覆盖

- enhance-mode: a TOC row with no printed page between neighbors (e.g. 160 / empty / 162) is filled as 161; Link writes that printed page even when PDF offset is not solved yet
  对准印刷目录：夹在 160 和 162 之间的空页码会补成 161；关联时即使还没算出 PDF 偏移也会写下印刷页

- bookmark sidebar: committing an in-place rename no longer reloads the tree inside TreeView_EndEditLabelNow (crash). Enhance-mode rename stays in the session until Save
  书签栏：就地改名不再在 TreeView_EndEditLabelNow 里重载目录（会崩溃）。对准印刷目录里改名只改会话，保存才落盘

- extract bookmarks: English `Table of Contents` / `Title .... 4` printed TOCs are handled as books, not skipped as “no headings”
  提取书签：英文 `Table of Contents` / `Title .... 4` 印刷目录按书处理，不再报「没有找到标题」

- enhance-mode / extract: dests that land on printed contents pages are cleared; locate skips the whole contents range, not just the first 目录 page
  对准印刷目录 / 提取：目的页落在印刷目录上的会清掉；定位会跳过整段目录页，不只是「目录」书签那一页

- book printed TOC: keep numbered rows that are full sentences ending with `。` (e.g. `4．电视中的“知心老师”…。`)
  书的印刷目录：带 `4．` 的完整陈述句不再因句号被当成正文漏句丢掉

- extract bookmarks / enhance-mode auto-verify: if nearby match and BM25 both miss, fall back to Find (same queries as the locate button)
  提取目录和自动核对：近页和 BM25 都找不到时，改用查找（查询和定位按钮一样）

- enhance-mode: click a TOC title to jump to its dest; double-click goes to that entry on the printed contents pages (not always the first contents page)
  对准印刷目录：单击标题去关联页；双击去它在印刷目录里的那一页（不再总是目录第一页）

- TOC numbering `1.` / `1.2.3` uses halfwidth dots (`1．2．3` → `1.2.3`)
  目录编号 `1.` / `1.2.3` 统一成半角点（`1．2．3` → `1.2.3`）

- enhance-mode: if BM25 cannot locate a heading, the locate button falls back to Find (same as Ctrl+F), trying the cleaned title then 12/8/6-glyph prefixes
  对准印刷目录：BM25 找不到标题时，定位按钮改用查找（和 Ctrl+F 一样），先搜去掉页码的标题，再试 12/8/6 字前缀

- enhance-mode: Ctrl+Z undoes the last TOC edit; Ctrl+Shift+Z / Ctrl+Y redo. Text fields keep their own undo
  对准印刷目录：Ctrl+Z 撤销上一步；Ctrl+Shift+Z / Ctrl+Y 重做。输入框里的 Ctrl+Z 仍只改文字

- enhance-mode: merge appends the next title and removes that bookmark (live apply no longer restores it from the old tree)
  对准印刷目录：合并把下一条标题接到本条后面，并删掉下面那一条

- enhance-mode: merge-row icon is a rounded square with up/down chevrons, same stroke weight as locate/link/delete
  对准印刷目录：合并按钮改为方框里上下箭头，粗细与定位/关联/删除一致

- enhance-mode: do not crash TOC custom-draw after write/reload when the calib session still pointed at a freed engine
  对准印刷目录：写入书签或文件重载后不再在绘制印刷页时访问已释放的引擎

- enhance-mode: Write Bookmarks keeps promote/demote/move nesting, not just page numbers
  对准印刷目录：写入书签会保存升降级、拖动后的层级，不只是页码

- enhance-mode: changing a printed page no longer blanks and rebuilds the whole TOC tree
  对准印刷目录：改印刷页不再整树清空重建，侧栏不再猛闪

- PDF bookmark sidebar: F2 in-place rename no longer jumps or flashes a system-white edit box
  PDF 书签栏：F2 原地改名不再错位抖动，也不再闪系统白底输入框

- enhance-mode: row buttons (locate / link / merge / delete / page ±) do not jump; only the title click does
  对准印刷目录：行内按钮（定位 / 关联 / 合并 / 删除 / 页码加减）不跳转，只有点标题才跳

- enhance-mode: locate (map-pin) only jumps; a separate link button pins the current view. Long titles ellipsize so they do not run under the row controls
  对准印刷目录：水滴只定位不改页，链条按钮把当前页关联到这条；标题单行截断，不叠在右侧按钮上

- enhance-mode: remove the Offset field; printed-to-PDF offset stays majority-voted from pinned rows
  对准印刷目录：去掉底部「偏移」；印刷页到 PDF 页仍按多数条目自动算

- enhance-mode: each TOC row has a locate button that finds the heading in the body (BM25) and pins that dest; the bookmark menu **Find TOC Item in Body** still jumps without changing the page
  对准印刷目录：每条目录右侧有「定位并关联」按钮，在正文里找到标题后钉上目的页；右键「在正文中定位」仍只跳转、不改页

- extract bookmarks writes the outline and stays in the normal TOC; 对准印刷目录 is opened only from the bookmark header or `CmdPdfTocCalibrate`
  提取目录书签后直接写入，不自动进入对准印刷目录；需要时再点书签栏按钮或命令手动进入

- bookmark header tooltip and Calibrate TOC Pages menu: 对准印刷目录; bookmark context menu puts Find TOC Item in Body above linking the current page
  书签栏图标提示和「校准页码」菜单改为「对准印刷目录」；书签右键「在正文中定位」排到「将第 N 页关联到选中的目录」上面

- PDF TOC: with text selected, `Ctrl+B` adds an item under the current bookmark (first child if it has children, otherwise the next sibling); `Ctrl+Shift+B` (`CmdPdfTocReplaceFromSelection`) replaces that bookmark's title and dest. No selection: `Ctrl+B` still adds a favorite
  PDF 目录：选定正文后 `Ctrl+B` 钉在当前条目正下方（有子项则第一条子项，否则平级下一条），`Ctrl+Shift+B` 用选中文字替换当前条目（含目的页）。没选文字时 `Ctrl+B` 仍是加收藏

- book printed TOC: merge a chapter title with the next-line em-dash subtitle, and a wrapped question with its answer/page line, into one bookmark
  书的印刷目录：章节正题与下一行「——」副题、问句折行后的「不是！+(页码)」合成一条书签

- enhance-mode: if a TOC row has no printed page (or nearby verify misses), locate the heading in the body with CJK-bigram BM25; skip printed contents pages; require a clear winner. Bookmark menu **Find TOC Item in Body** jumps to that hit without changing the printed page
  校准页码：没有印刷页或附近对不上时，用中文二元组 BM25 在正文里定位标题（跳过印刷目录页，分不够高或和第二名太近则不钉）。书签菜单「在正文中定位」只跳到命中页，不改印刷页

- enhance-mode: click a TOC title to jump to its dest; editing printed page / ± stays on the current view; double-click jumps to that entry on the printed contents pages
  校准页码：单击标题去关联页；改印刷页、± 不跳页；双击去印刷目录上的那一条

- page right-click menu no longer includes Extract Table of Contents or Calibrate TOC Pages (still on View, bookmark sidebar, and command palette)
  正文右键菜单不再显示「提取目录书签」和「校准页码」（查看菜单、书签栏、命令面板仍保留）

- PDF bookmark sidebar and page right-click: **Set TOC Item to Current Page** names the page like favorites (e.g. 将第 12 页关联到选中的目录)
  PDF 书签栏和正文右键：「将第 N 页关联到选中的目录」，说法与收藏夹一致（校准模式只改会话映射；普通模式改 outline 目标页）

- bookmark header: the list/checkbox icon tooltip is Calibrate TOC (目录校准)
  书签栏标题旁清单图标的提示改为「目录校准」

- enhance-mode page calibration: every TOC row shows only the printed page (no PDF page); numbers are visible without selecting a row
  校准页码：每条目录后面只显示印刷页，不再显示 PDF 页；不用点选也会出现页码

- PDF bookmark sidebar: releasing a dragged TOC item now keeps the new order or nests it as a child of the drop target
  PDF 书签栏：拖动目录后松手会改顺序；拖到另一条中间则收成它的子项

- enhance-mode page calibration: deleting a TOC item promotes its children one level instead of removing the whole subtree
  校准页码：删除一条目录时把下一层上提一级，不再连子项一起删掉

- official-document bookmark extract: drop bare 第N节/第N章 numbering with no title (those used to write bookmarks with no dest and show as gray); treat trailing OCR `o`/`○`/`c` after CJK as a misread `。` or speckle
  公文提取：丢掉只有「第N节」没有标题的条目（写入后没有目的页，书签栏显示灰色）；标题末尾的 OCR `o`/`○`/`c` 按误识别的 `。` 或噪点去掉

- experimental: open OOXML Office files (`.docx`, `.xlsx`, `.pptx`, `.hwpx`) via MuPDF's HTML conversion. Layout is often poor; classic `.doc` / `.xls` / `.ppt` remain unsupported
  试验：用 MuPDF 的 HTML 转换打开 OOXML（`.docx` / `.xlsx` / `.pptx` / `.hwpx`）。版式往往较差；老的 `.doc` / `.xls` / `.ppt` 仍不支持

- PDF bookmark sidebar: F2 on a selected bookmark edits that item in place; it no longer opens Rename File
  PDF 书签栏：选中条目时按 F2 原地改书签名，不再弹出「重命名文件」

- Match-theme: faded gray office photocopies invert like high-contrast scans (bright text), instead of staying dim SoftCream / PictureBook gray
  「匹配主题」：发灰的办公扫描件也按公文纸反色（浅色文字），不再停在 SoftCream / 绘本路径里发暗

- PDF bookmark sidebar: Acrobat-style editing — Ctrl multi-select, Shift range-select, Delete, F2 rename, drag to reorder or nest; Ctrl+Up/Down move, Ctrl+Left/Right promote/demote, Ctrl+A select all, Insert add after
  PDF 书签栏：接近 Acrobat 的编辑 — Ctrl 多选、Shift 连选、Delete 删除、F2 重命名、拖动改变顺序和层级；Ctrl+↑/↓ 上移下移，Ctrl+←/→ 升级降级，Ctrl+A 全选，Insert 在后方添加

- official-document bookmark extraction: document heading schema, sequence scoring, and a keep/skip DP pass; advanced setting `ExtractPdfTocMode` (`conservative` / `standard` / `detailed`); `-extract-toc-debug` writes a sidecar explanation next to the PDF
  公文智能提取目录：按本文标题格式压缩层级、编号连续性打分、全局 keep/skip 校正；高级设置 `ExtractPdfTocMode`（保守/标准/详细）；`-extract-toc-debug` 在 PDF 旁写出逐条说明

- portable/debug builds output `SumatraPDF-Plus.exe` (was `SumatraPDF.exe`) so antivirus is less likely to treat the binary as a cracked official build
  便携/调试编译产物改为 `SumatraPDF-Plus.exe`，降低被免费杀毒当成官方破解版的概率

- extract PDF bookmarks from a printed table of contents or heading styles (`CmdExtractPdfToc`); View menu, bookmark sidebar, and command palette. Writes the outline so it can be edited and saved. An empty bookmark sidebar shows a short hint and a clickable extract action. If the file has no text layer, all pages are recognized automatically and bookmarks are extracted (no dialog). Scanned books can then be calibrated in enhance mode (`CmdPdfTocCalibrate`).
  从印刷目录页或标题样式提取 PDF 书签（`CmdExtractPdfToc`）：查看菜单、书签栏和命令面板。写入 outline 后可编辑并保存。空书签栏显示简短提示，可点击提取。没有文字层时自动全文识别再提取目录，不弹对话框。扫描书可再进增强模式校准页码（`CmdPdfTocCalibrate`）。

- scanned books with a printed contents page: extract writes bookmarks like official documents. Open enhance mode from the bookmark header button or `CmdPdfTocCalibrate` to set printed page numbers (PDF page is shown, not edited). Offset is taken from the majority of entries and can be overridden; front matter keeps its own page label (i, ii, A) instead of a negative offset. Save writes the file and stays in enhance mode; Exit leaves. Click a bookmark title to jump to its dest; double-click jumps to that entry on the printed contents pages.
  扫描书若有印刷目录：提取后与公文一样直接写入书签。需要校准页码时，点书签栏标题旁的小按钮或用 `CmdPdfTocCalibrate` 进入增强模式，只改印刷页（PDF 页只读）。偏移按多数条目自动测算，也可手动锁定；译者序等用原本的页码（i / ii / A），不用负数。「保存」写入后不退出；「退出」离开。单击标题去关联页；双击去印刷目录上的那一条。

- OCR scanned pages (RapidOCR / PP-OCR Chinese mobile ONNX): toolbar Auto OCR (`CmdToggleAutoOcr`, setting `AutoOcrScanPages`, default off) recognizes visible/scanned pages when enabled. The OCR dropdown lists Recognize all pages (`CmdOcrDocument`), Recognize all pages and save (`CmdSaveSearchablePdf`), and OCR region (`CmdOcrRegion`). Recognize all pages re-OCRs even when a text layer already exists. Recognize all pages and save shows scan progress, prompts before replacing an existing text layer or outline, extracts bookmarks if none exist (or if you confirm overwrite), then saves over the current PDF with no Save As dialog. `CmdOcrCancel` stops queued page OCR. Models live in `{exedir}/ocr/`.
  扫描页 OCR：工具栏「自动 OCR」（`CmdToggleAutoOcr` / `AutoOcrScanPages`，默认关闭）打开后会识别当前扫描页。下拉菜单为「全文识别」「全文识别并保存」「框选识别」。全文识别在已有文字层时仍会再扫一遍。全文识别并保存会显示扫描进度；已有文字层或目录时先询问是否覆盖；没有目录则识别后提取；然后直接覆盖保存当前 PDF，不弹另存对话框。`CmdOcrCancel` 可取消排队识别。模型在 `{exedir}/ocr/`。

## 3.7.28 (2026-08-17)

- Match-theme: oval portraits (e.g. Abraham Lincoln) use a small mat halo so poles are not eaten into rectangular bars; wrapped text on the mat still inverts
  「匹配主题」：椭圆肖像（如 Abraham Lincoln）衬纸光晕缩小，上下两极不再被啃成方块；绕排文字仍正常反色
- Match-theme: inset RAZ photos (Genetics at Work p.18) do not keep a white hairline where the photo meets body text
  「匹配主题」：嵌入照片与正文交界不再留白竖线（如 Genetics at Work 第 18 页）
- Match-theme: RAZ display-type titles on paper above a photo (SPRAK p.2) invert to theme text instead of staying black with a white halo
  「匹配主题」：照片上方纸面上的粗黑标题（如 SPRAK 第 2 页）反成主题文字色，不再黑心白边
- Match-theme: RAZ color pages whose thumbnail looks like line-art (Vincent's Bedroom p.8 red portrait) stay on the picture-book path; cream 连环画 (sat~0.16) still inverts as line art
  「匹配主题」：缩略图像线稿的 RAZ 彩色页（如 Vincent's Bedroom 第 8 页红框肖像）走绘本路径，不再公文二值化；泛黄连环画（sat≈0.16）仍按线稿反色
- Ctrl+wheel zoom: same ~10% step per notch for slow and fast wheels; cap zoom speed so a flick no longer jumps to 6400% or 20%
  Ctrl+滚轮缩放：慢滚和快滚都按每格约 10%；限制缩放速度，避免轻轻一甩就到 6400% 或 20%
- fix TOC sidebar splitter: live drag only moves windows (no per-move page Relayout / SETREDRAW); full layout on mouse-up
  修复 TOC 分隔条拖动：拖动中只挪窗口，不每次重排页面；松开后再完整布局
- fix TOC search vs. first bookmark jumping while dragging the sidebar splitter: layout the filter from WS_VISIBLE, not IsWindowVisible (false during WM_SETREDRAW)
  修复拖 TOC 分隔条时搜索框与第一条书签抢位置：按 WS_VISIBLE 留出搜索行，不再用 IsWindowVisible（整窗暂停重绘时会误判为隐藏）
- fix EPUB ink in single-page view: keep the stroke on its page instead of drawing it on every flipped page
  修复 EPUB 单页模式下自由曲线翻页后仍浮在画面上：墨迹只画在所属页
- fix Windows 11 fullscreen leftover of the taskbar volume icon: make the frame topmost and mark it fullscreen so the tray (and other topmost notify widgets) stay behind the window
  修复 Windows 11 全屏后右下角喇叭图标残影：全屏窗口置顶并通知资源管理器，任务栏和其它托盘置顶控件不再盖在页面上
- toolbar fullscreen button (`CmdToggleFullscreen`): enter or leave fullscreen reading; tooltip includes F11 (exit still works with F11 / `f` when the toolbar is hidden)
  工具栏全屏按钮：切换全屏阅读；提示含 F11（工具栏隐藏时仍可用 F11 / `f` 退出）

## 3.7.27 (2026-08-14)

- close the find window when closing the current tab (same as switching tabs)
  关闭当前标签时一并关闭查找窗口（与切换标签一致）
- find results list: stop jittering while the live match count updates
  搜索结果列表：全文计数刷新 `n/m` 时不再跟着抖动
- find results list: selected row is easier to see in dark themes (theme accent wash + left bar)
  搜索结果列表：暗色主题下当前选中行更明显（主题强调色底 + 左侧色条）
- search `n/m` and the results list use document order (1 = first hit in the book); first jump still starts from the current page; Enter from a different page starts at that page, F3 / Next / Prev keep stepping from the current hit
  搜索 `n/m` 与结果列表按全书出现顺序编号（1 = 书中第一条）；首次跳转仍从当前页起；在另一页按 Enter 从该页起跳，F3 / 下一个 / 上一个仍从当前命中继续
- toolbar quick annotation buttons (rectangle, circle, line, ink): one click to enter drag-to-draw mode, click again to exit; hide with `ShowAnnotToolbarButtons = false`
  工具栏快捷标注（矩形/椭圆/直线/画笔）：单击进入拖拽绘制，再单击退出；`ShowAnnotToolbarButtons = false` 可隐藏
- faster Match-theme render for Acrobat/PageMaker textbooks (e.g. Journey Across Time): cache per-image policy analysis; skip RAZ PictureBook lum/var planes — use cheap preserve/linear remap (Image-Conversion picture books unchanged)
  加快 Acrobat/PageMaker 教材（如 Journey Across Time）「匹配主题」渲染：缓存每图策略分析；跳过绘本 PictureBook 全图亮度/方差平面，改廉价 preserve/线性重映射（Image Conversion 绘本路径不变）
- Match-theme dark: stop “worm”/posterization on light photo textures (fur, white clothes, tile) — treat textured near-white as photo content for photo-rect seek; skip ApplySharp on moderate local luminance variance (paper/line-art paths unchanged)
  「匹配主题」暗色：减轻浅色照片纹理上的蚯蚓/色阶伪影（毛发、白衣、瓷砖）——稠密检测把带纹理近白当照片区；对中等局部亮度方差跳过 ApplySharp（纸面/线稿路径不变）
- fix freeze/beeps when opening dialogs from the hamburger/popup menus (e.g. smart bilingual settings, custom TTS speed): `CenterDialog` places on the cursor’s monitor and forces ShowWindow so DialogBox is not left invisible while the owner is disabled
  修复汉堡/弹出菜单打开对话框时卡死并系统蜂鸣（如智能双语设置、自定义语速）：`CenterDialog` 按鼠标所在显示器居中并强制 ShowWindow，避免 DialogBox 不可见而主窗已被禁用
- fix missing tooltips / dead ebook font-size buttons / native scrollbar hold-to-page: TreeWrapLabels custom-draw was calling `TreeView_SetItem` during paint (~500–850 WM_PAINT/s), starving low-priority `WM_TIMER` (tooltip, font-size debounce, and scrollbar auto-repeat); recalc item heights outside paint; restore native tab/toolbar tips and DefWindowProc scrollbar repeat (remove TimerQueue workaround)
  修复汽泡提示偶发不显示、电子书字号加减失灵、滚动条槽区按住不连续翻页：TreeWrapLabels 绘制中反复 `TreeView_SetItem` 形成每秒数百次 WM_PAINT，饿死低优先级 `WM_TIMER`（tooltip、字号防抖、滚动条自动重复）；高度改在绘制外重算；恢复原生 tip 与 DefWindowProc 滚动条按住重复（去掉 TimerQueue 绕道）
- fix sticky TOC/favorites sidebar resize with TreeWrapLabels: suspend wrap-height updates while dragging the splitter (single-line clipped paint); recalc + flush only on mouse-up
  修复开启 TreeWrapLabels 时拖 TOC/收藏侧栏宽度不跟手/残留竖线：拖动中暂停换行高度更新（单行裁剪绘制）；仅在松开鼠标时重算并 flush
- match upstream: disable custom owner-draw menus; let darkmodelib/OS theme popups (including submenu chevrons)
  与上游一致：关闭自绘菜单，弹出菜单交由 darkmodelib/系统主题绘制（含子菜单箭头）
- PDF Sound/RichMedia/Screen (e.g. RAZ speakers): click the playing icon again to stop; stop audio when switching tabs
  PDF 内嵌音频（如 RAZ 喇叭）：再点正在播放的图标即停；切换标签页时停止播放
- fix dark-theme hamburger menu: command visibility uses the current tab’s loaded state so Go/Zoom/Selection are not built empty; independent popup tree; avoid Settings freeze from live owner-draw recurse
  修复暗色汉堡菜单：命令可见性按当前标签是否已加载文档判断，避免「前往/缩放/选择」建成空菜单；独立弹出菜单树；避免设置菜单因 owner-draw 递归卡死
- fix oversized “Loading …” banner text: paint with a freshly resolved UI HFONT (cached fonts are deleted on DPI/chrome refresh while the last multi-file banner is still up); fill with GDI not Gdiplus; no full-canvas centered loading line
  修复「载入…」横幅字号过大：绘制时重新取 UI 字体（多文件打开时 DPI/界面刷新会删掉缓存 HFONT，最后一本横幅仍显示就会落到系统大字体）；背景用 GDI 填充；不再画布居中第二行
- fix duplicate “Loading …” banners when opening a file: tab selection during prepare no longer starts a second async load
  修复打开文件时出现两条「载入…」提示：准备标签时不再重复启动异步加载
- faster PDF open: skip `JoinSplitPdfImages` whole-document page parse for Acrobat/layout textbooks (e.g. Exploring Our World); abort early when the first pages show no strip-image pairs; do not re-probe Match-theme when metadata already classified the doc
  加快 PDF 打开：Acrobat/排版教材（如 Exploring Our World）跳过 `JoinSplitPdfImages` 全文档解析；前几页无细条拼图信号则提前结束；元数据已分类时不再重复 Match-theme 探测
- Match-theme dark: protect small inset B&W portraits on paper-heavy RAZ pages (e.g. Historic Peacemakers Betty Williams) so they are not ApplySharp-inverted; stop photo-rect grow from swallowing body text; warm cream SoftCream; colorful RAZ/comic picture-book protect
  「匹配主题」暗色：纸面为主的 RAZ 页上小幅黑白肖像（如 Historic Peacemakers Betty Williams）纳入照片保护，避免被陡峭重映射成负片；照片保护区不再吞正文；暖奶油 SoftCream；彩色 RAZ/漫画 picture-book 保护
- add `JoinSplitPdfImages` (default true): hide redundant thin-strip pages in Calibre-style photo PDFs so continuous view joins split images; toggle with `CmdToggleJoinSplitPdfImages`
  新增 `JoinSplitPdfImages`（默认 true）：隐藏 Calibre 写真集一类「同图细条重复页」，连续滚动时把断开的图片接起来；可用 `CmdToggleJoinSplitPdfImages` 开关
- add `TreeWrapLabels` setting (default true): bookmarks and favorites wrap long titles to multiple lines; set false for single-line labels with ellipsis and full text in tooltip
  新增 `TreeWrapLabels`（默认 true）：书签与收藏长标题多行换行；设为 false 时为单行省略 + 悬停气泡显示全文
- light match theme: stop routing colorful PDF covers through SmartDark image remap; preserve full-bleed photos and skip highlight compression on light themes
  浅色「匹配主题」：彩色封面不再走 SmartDark 图像重映射；全幅照片保留原色，浅色主题跳过高光压缩

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
