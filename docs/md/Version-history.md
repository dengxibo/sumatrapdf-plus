# Version history

## next

## 3.7.37 (2026-10-04)

- 暗色匹配主题下，半透明渐变和带蒙版的图在变色之后仍按原来的透明度合成。
  In dark Match-theme, translucent gradients and masked images keep their transparency after recoloring.
- 从光标处朗读时，点在英文单词的任意字母上，都从该单词的开头读起。汉字仍从点中的那个字开始。
  Read aloud from the cursor starts at the beginning of an English word, whichever letter was clicked. A Chinese character still starts at the character that was clicked.
- 查找界面打开时，滚动条左侧留一条位置刻度。蓝色是全部命中，橙色是当前命中。关掉查找后，页面宽度恢复。
  While the find UI is open, a position strip sits beside the scrollbar. Blue ticks are every hit, and the orange tick is the current hit. Closing find gives that width back to the page.
- 连续视图里，触控板不足一格的滚动按像素累计。鼠标滚轮一格仍按行滚动，滚到头会翻页。
  In continuous view, a precision-touchpad delta smaller than one notch accumulates into pixels. A full mouse-wheel notch still scrolls by lines, and turns the page at the edge.
- 暗色匹配主题下，叠在整页照片上的切片、文字和色块保持原色。章节首页的云朵和标题阴影不再被抠成黑块，白色标题不再反成深色；地图上的地名和图例也能看清。
  In dark Match-theme, slices, text, and shapes laid on a full-page photo keep their original colors. Chapter-opener clouds and title shadows no longer turn into black blots, and a white title stays white. Place names and legends on maps stay readable.
- 暗色匹配主题下，放在白色方块上的纯色圆章、渐变按钮等平面图形，即使被切成几块，周围的白底也会抠掉，不再留下白色矩形。
  In dark Match-theme, the white square around flat color art (a badge disc, a gradient button) is knocked out even when the art is sliced into pieces, so no white rectangle is left.
- 暗色匹配主题下，奶油色整页底变暗之后，叠在上面的徽章、标题板和按钮条会带上同一块奶油色。这些切片里的纸色会一起抠掉，不再留下白色圆角卡片或色条。
  In dark Match-theme, after a cream full-page background turns dark, flattened badges, title plates, and button strips that still carry that cream have the paper knocked out, so no white rounded card or bar is left.
- 暗色匹配主题下，浅黄、高亮一类彩色底如果保持原色，落在上面的字和线也不反色。黄底黑字的导读栏不再变成黄底白字。
  In dark Match-theme, when a light chromatic plate (a yellow sidebar, a highlight) keeps its original color, text and strokes on it stay original too. Black type on a yellow Guide-to-Reading bar is no longer inverted to white.
- 暗色匹配主题下，课文里整页奶油色若是矢量铺底，叠在上面的标题板、卡片框也会把纸色换掉。Visual Summary 后面不再留一块浅色圆角底。
  In dark Match-theme, when a chapter wash is a cream vector fill, title plates and card frames on it also remap that paper. The light rounded block behind Visual Summary is no longer left as-is.
- 暗色匹配主题下，彩色边框里的课文白纸会换成主题底色，字改成浅色。不再整页保持原样。
  In dark Match-theme, the white paper inside a colored textbook frame becomes the theme background and the type goes light, instead of leaving the whole page in its original colors.
- 收藏夹标题超出宽度时，行尾显示省略号，和书签栏一样。
  A favorite title that does not fit ends with an ellipsis, the same as a bookmark.
- 切换标签时，PDF 内嵌音频会暂停，和朗读、EPUB3 内嵌音频一样。回到该标签后可以接着播。
  Switching tabs pauses PDF embedded audio, the same as read-aloud and EPUB 3 embedded audio. Coming back to that tab can resume it.
- 没配火山翻译或 AI 接口时，划词翻译发给网页 AI。密钥被拒绝时也一样。
  When Volcengine Translate and the AI API are not set up, translating a selection sends it to the web AI. An authorization failure does the same.
- 书虫这类整页漫画，每一幅图单独成页，图和下一幅之间不再夹一页空白。
  Full-page comic panels, such as in the Bookworms omnibus, each stay on their own page, with no blank page between them.
- 朗读时点其他目录条目会留在那一页。要回到朗读位置，用朗读条上的跟随按钮。
  During read-aloud, choosing another bookmark stays on that page. The Follow button on the read-aloud bar returns to the spoken place.
- 侧栏标题上书签、缩略图、收藏三个图标的间距是 4 像素。
  The Bookmarks, Thumbnails, and Favorites icons in the sidebar title are 4 pixels apart.
- 主页封面的选中框再向外移开一点，不再贴着封面的灰影。
  The home-page cover selection box sits a little farther out, clear of the cover's gray shadow.
- 明亮主题下，主页和侧栏缩略图的选中框改成低饱和的灰蓝。
  In light themes, selection boxes on the home page and sidebar thumbnails use a muted slate blue.
- 主页第一行缩略图的选中框上边也会画出来。
  The selection box on the first row of home-page thumbnails includes its top edge.
- 主页缩略图的选中框画在封面外面的底色上，封面是蓝色时也能看出已选中。
  A selected home-page thumbnail draws its box on the page background outside the cover, so a blue cover still shows as selected.
- 主页列表的选中框左边也会封上，缩略图不再盖住这一条边。
  The home-page list selection box includes its left edge. The thumbnail no longer covers that side.
- 删除文件的确认框跟界面语言走，标题、说明和「是」「否」都使用当前语言。
  The delete-file confirmation follows the interface language, including the title, the message, and Yes and No.
- 主页右键会弹出菜单。列表方式下，暗色选中框留在本行内，与相邻条目分开。
  A right-click on the home page opens its menu. In list view, the dark-theme selection box stays inside its row and clear of the next row.
- 单页显示时，在页面两侧空白处右键，菜单与点在纸面上相同，包括当前页的收藏和在光标位置创建批注。
  In single-page view, a right-click in the blank area beside the page shows the same menu as a click on the page, including that page's favorite and creating an annotation at the cursor.
- 折叠或展开书签后，暗色选中框仍框住整行，不再只在左边留一条。
  Collapsing or expanding a bookmark keeps the dark-theme selection box around the whole row.
- 选项里「延迟加载未激活的标签页」默认关闭。
  Options defaults Lazy-load inactive tabs off.
- 目录栏标题左侧是书签、缩略图、收藏三个图标，点一个就在同一栏里切换。当前看的是哪一种，记在这份文档上，换标签会恢复。没有目录时书签图标变灰；没有页面时缩略图图标变灰。右侧关闭叉和窗口、标签页是同一套细叉。
  The sidebar title has Bookmarks, Thumbnails, and Favorites icons. Clicking one switches that column. The view is remembered per document and restored when switching tabs. Bookmarks is dimmed when there is no table of contents. Thumbnails is dimmed when there are no pages. The close mark on the right is the same thin glyph as the window and tab close buttons.
- 缩略图点一下跳到那一页。上色和印刷目录确认页的缩略图相同，跟当前正文的主题颜色走，底部只标数字页码。印刷目录确认页的缩略图同样只标数字。流式 EPUB 用已经排好的页，不为缩略图重算页数。加载时在后台出图，格子不跟着晃，当前页不变时也不把列表拽回去。
  A thumbnail click jumps to that page. Colors match the printed-TOC page picker, following the open document's theme colors, and the tag is the page number alone. The printed-TOC picker uses the same number. Reflowable EPUBs use the pages already laid out. Thumbnails render in the background while loading, so the grid does not shake, and the list is not pulled back while the current page stays the same.
- 暗色主题下，工具栏按钮按下或保持选中时的底和边，跟侧栏收藏星标选中时一样，比原来更清楚。
  In dark themes, a pressed or checked toolbar button uses the same well and edge as the selected Favorites star in the sidebar.
- 暗色主题下，书签和收藏的悬停会像亮色那样把整行变色。书签提示只在标题被栏截断时出现。
  In dark themes, bookmark and favorites rows change color on hover the way they do in light themes. A bookmark tip appears only when the title is cut off by the column.
- 收藏栏标题右侧是加号和减号，位置、大小、笔画和颜色与书签栏的展开、收起箭头相同，只有提示，没有悬停底色。加号把当前页加入收藏，减号去掉当前页的收藏；选中了某一条时减号删这一条。书签和缩略图不显示这两个按钮。侧栏的关闭叉在书签、缩略图、收藏下都关掉这一栏，悬停不再画红圈。
  The Favorites title has a plus and a minus in the same place, size, stroke, and color as the bookmark expand and collapse arrows. They show a tip only, with no hover well. Plus adds the current page. Minus removes that page's favorite, or the highlighted row when one is selected. Bookmarks and thumbnails do not show those buttons. The sidebar X closes the column from bookmarks, thumbnails, and favorites, and no longer draws a red circle on hover.
- 向右拖收藏夹分隔条时，新露出的一竖条会马上用侧栏底色补上，页面跟着走，不再闪一条黑带。
  Dragging the Favorites splitter to the right fills the newly uncovered strip with the sidebar color immediately, and the page moves with it, so a black band no longer flashes there.
- 收藏列表的悬停和选中，在四种主题下都跟书签条目同一套：整行底色、暗色选中框，文字不改成系统蓝底白字。打开收藏不会自动圈住第一行，选中框左边是连上的。点一行就跳到那一页，侧栏仍停在收藏；点展开按钮只展开。截断标题的提示在条目右下一点，样式和配色与书签提示相同。右键是「从收藏中删除」。
  Favorites rows use the same hover and selection as bookmark entries in all four themes: full-row fill, a dark-theme selection box, and text that stays off the system blue highlight. Opening Favorites does not select the first row, and the selection box includes its left edge. A click on a row jumps to that page and leaves the sidebar on Favorites. The expand button only expands. A truncated title's tip sits just to the lower right of the row, with the same look as a bookmark tip. Right-click offers Remove from favorites.
- 书签、缩略图、收藏都有搜索框。书签和缩略图共用一次按标题的搜索，切换时文字和筛出来的页都留着。缩略图只显示标题对得上的那些页。收藏按名称另筛。
  Bookmarks, thumbnails, and favorites each keep a search box. Bookmarks and thumbnails share one title search; switching keeps the text and the matching pages. Thumbnails show the pages of the matching headings. Favorites filter by name on their own.
- 主页支持 Ctrl+单击加减、Shift+单击连选、Ctrl+A 全选。搜索框有焦点时 Ctrl+A 仍只选中搜索文字。右键在多选上可以全部打开、全部移出历史，删除文件只确认一次。
  The home page supports Ctrl+click to toggle, Shift+click to select a range, and Ctrl+A to select every file. With the search box focused, Ctrl+A still selects only the search text. A right-click on a multi-selection can open all, remove all from history, and delete files with one confirmation.
- 主页和文件菜单的「删除文件」会先确认，文件进入回收站。取消则不动。「从历史记录删除」和 Del 不确认。
  Delete File on the home page and in the File menu asks first, then moves the file to the Recycle Bin. Cancel does nothing. Remove From History and Del do not ask.
- 设置 → 阅读字体里的西文、中文正文字体，改成和其他对话框一样的小窗口：标题栏可以关闭，搜索框下面是字体名单，滚动条只在名单上，底部是确定和取消。单击只选中，确定或双击才换字体。每一行用该字体画自己的名字，颜色跟着当前主题。
  Settings → Reading Font opens Western and CJK body fonts in a dialog with a title bar, a search box, a font list whose scrollbar stays on the list, and OK and Cancel. A click selects a row; OK or a double-click applies it. Each row is drawn in that font, and the window follows the current theme.

## 3.7.36 (2026-10-02)

- 查词、划词工具栏、朗读条、命令面板、文档属性、标签组、截图、图片裁剪，以及烘焙、提取、压缩、加密这些 PDF 窗口，都按所在屏幕的缩放显示。拖到另一块屏幕时会跟着变。
  Lookup, the selection toolbar, the read-aloud bar, the command palette, document properties, tab groups, screenshot, image crop, and the PDF bake, extract, compress, and encrypt windows follow the screen they are on, and update when dragged to another screen.
- 翻译和问问 AI 浮层按所在屏幕的缩放显示文字和窗口大小。拖到另一块屏幕时会跟着变。
  The translation and Ask AI popup sizes its text and window for the screen it is on, and updates when dragged to another screen.

- 选区工具栏点「复制」后，选区高亮会消失。查词、翻译、问问 AI 的窗口关掉后，打开它们时的那一段选区也会消失。
  Copy on the selection toolbar clears the highlight. Closing Lookup, Translate, or Ask AI clears the selection those windows were opened with.
- 切换明亮/暗黑主题时，翻译和问问 AI 浮层先把窗口和里面的文字框准备好，再和主窗口一起换上新颜色。
  Switching light and dark prepares the translation and Ask AI popup first, then paints it together with the main window.
- 第一次打开问问 AI 后再切换明亮/暗黑主题，界面不再卡住十几秒。回答框只改滚动条颜色，不再套用整套 Explorer 主题。
  Switching light and dark after opening Ask AI for the first time no longer stalls. The answer box only restyles its scrollbar, instead of loading the full Explorer text-services theme.
- 暗黑主题下，翻译和问问 AI 的正文区、输入框更贴近浮层底色。「发送」平时与浮层同色，只留淡边，悬停时再铺浅底。
  In dark themes, the translation and Ask AI text areas and the input sit closer to the popup background. Send matches that background with a light border, and fills only while hovered.
- 划词翻译的译文和问问 AI 的回答比界面字体大 2 磅。标题、按钮、原文摘录和输入框仍用界面字体。
  The translation and the Ask AI answer are two points larger than the UI font. The title, buttons, source excerpt, and input stay at the UI size.
- 选项 → AI 的模型是一个普通输入框，旁边的「选择」向 API 查询名单，在可滚动的列表里点一个填回去。名单里没有的模型仍可直接输入。列表上方可以按名称片段搜索。语音合成、语音识别、实时语音、图片生成和向量模型不列入名单。
  Options → AI keeps the model as a plain text field. Choose asks the API for names and fills the field from a scrolling list. A name that is not in the list can still be typed. A search box above the list filters by part of a name. Speech synthesis, speech recognition, realtime audio, image generation, and embedding models are left off the list.
- 划词翻译目标语支持多国语言（简繁中文、英、日、韩、法、德、西、俄等）。自动模式跟随界面语言，源语与目标相近时对调（中英日韩）。
  Selection translate supports many target languages (Simplified/Traditional Chinese, English, Japanese, Korean, French, German, Spanish, Russian, and more). Auto follows the UI language and flips when the source matches (Chinese, English, Japanese, Korean).
- 选项对话框把「OCR 和 AI」拆成「OCR」和「AI」两个分类。OCR 管扫描识别和智能目录详细程度；AI 管 Ask AI、划词翻译和 API。
  Options splits OCR and AI into two categories. OCR covers scan recognition and smart-contents detail; AI covers Ask AI, selection translate, and the API.
- 划词翻译：选区工具栏和菜单增加「翻译」。优先火山翻译；未配置或失败时，若已配置 AI API 则用大模型只出译文；都没有则提示去配置，菜单里的 Google / DeepL 网页翻译仍可用。目标语默认跟界面语言，源语接近目标时在中英之间对调（`TranslateTargetMode`：`auto` / `ui` / `zh` / `en`）。
  Selection translate: the selection toolbar and menu have Translate. Volcengine is preferred; if it is missing or fails and an AI API is configured, the model returns a translation only. With neither, the panel asks you to configure settings; Google / DeepL web translate remain in the menu. The target language follows the UI by default and flips between Chinese and English when the source matches (`TranslateTargetMode`: `auto` / `ui` / `zh` / `en`).
- Ask AI：已配置 AI API 时在应用内浮层对选区问答，不再默认打开浏览器；未配置 API 时仍打开豆包 / DeepSeek / ChatGPT 网页。浮层可继续追问当前选区。
  Ask AI: with an AI API configured, questions about the selection open an in-app panel instead of the browser. Without an API, Doubao / DeepSeek / ChatGPT web chat is unchanged. The panel can continue the conversation for the current selection.
- 连续模式下改缩放（如工具栏「页宽连续」、适合宽度 / 页面、开关侧栏或改窗口大小导致的重新适配）不再跳到页面中下部或别的页：以页面为锚点保持位置，从单页切换过来时停在本页顶端。PDF、EPUB 等固定版式文档都受益。
  Changing zoom in continuous mode (Fit Width and Continuous toolbar button, Fit Width / Fit Page, or a refit after toggling the sidebar or resizing the window) no longer jumps to the middle of the page or to another page. The position is kept relative to the page; switching from single page keeps the page top in view. Applies to PDF, EPUB and other fixed-layout documents.
- 选项 → AI 的火山 Secret Key 旁增加「测试」按钮，直接显示火山返回的错误码和信息。
  Options → AI has a Test button next to the Volc Secret Key; failures show Volcengine's error code and message.
- 翻译 / 问答浮层：修复点关闭会退出程序；标题栏可拖动，右下角可调大小，按内容自动长高；显示语向和引擎（火山 / AI）；一键复制译文；回车发送、Esc 关闭，输入时不再触发阅读器快捷键；未配置 AI API 时给出可点击的去设置提示；四个主题配色统一；AI 回答的 Markdown（标题、粗体、列表、引用、代码、表格）按格式显示。
  Translate / Ask AI panel: closing it no longer exits the app. Drag by the header, resize from the bottom-right, and it grows with its content. It shows the language direction and engine (Volcengine / AI) and has Copy. Enter sends and Esc closes; typing no longer triggers reader shortcuts. Without an AI API, a clickable hint opens settings. Colors match all four themes. AI answers render Markdown (headings, bold, lists, quotes, code, tables).
- 页内喇叭只负责播放。正在播放时再点同一只喇叭会从头再放，不再停止。停止仍用朗读条或工具栏。
  An in-page speaker only starts playback. Clicking the same speaker while it is playing starts the clip again instead of stopping. Stop with the read-aloud bar or the toolbar.
- 暗色模式下，教材整页的奶油色底图会换成主题背景，正文仍是矢量浅字，彩色标题和地图保持原色。取样不再只看图像顶部的灰条，所以这类底图不会整页留在浅色上。
  In dark mode, a textbook's full-page cream plate becomes the theme background. Body text stays vector and light, and colored headers and maps keep their colors. Sampling covers the whole image, so a gray band at the top no longer leaves the page in its original colors.
- 暗色模式下，雾、雪、白毛这类高调照片保持连续色调，不再被当成扫描件做成只有黑白的剪影。页边和正文仍是深底浅字，空白竖条和正文扫描不受影响。
  In dark mode, high-key photographs such as fog, snow, and white fur keep their tones instead of being flattened into a black-and-white silhouette. Page margins and body text stay light on the dark page. Blank gutters and text scans are unchanged.
- 暗色模式下，抠图的淡色边缘会沿物体收完，杯口这种弯过去的一圈不再因为路程稍长被切掉一块。紧贴文字的灰色描边不会被收进去，标题和正文仍是深底浅字。
  In dark mode, a pale cut-out edge is followed for the whole connected object, so a curved rim such as the lip of a glass is not clipped where the path runs long. Gray pixels touching body text are left alone, and headings stay light on the dark page.
- 暗色模式下，抠图伸出照片密实区域的淡色边缘（玻璃杯沿、蛋壳、玻璃盘角）仍保持原色，不再被切掉后反成黑块。页面空白和正文文字不受影响。
  In dark mode, pale edges that stick out of a photo, such as a glass rim, an eggshell, or a glass-dish corner, stay in their original color instead of being clipped and inverted to black. Page margins and body text are unchanged.
- 暗色模式下，同一行并排的抠图不再合成一个大框。较矮的那一张不再把另一张多出来的一截（如毛衣袖口）切出去反色，高光也不会被反成黑块。上下叠在一起的同一张照片仍合成一个框。
  In dark mode, side-by-side cut-outs on the same row stay separate rectangles. The taller one is no longer clipped where the shorter one ends, so a strip such as a sweater cuff is not inverted and its highlights do not turn black. Stacked slices of the same photo still merge into one rectangle.
- 选中的英文带上句号、逗号这类标点时，查词不再变灰，标点会自动去掉。查词窗口外观不变；双击释义或例句里的英文单词会改查这个词，没有悬停提示。
  Look Up stays available when a selected English word includes a period or comma; that punctuation is removed. The lookup window looks the same. Double-clicking an English word in the definition or example looks that word up, with no hover cue.
- 内嵌录音的喇叭在句末、后面没有正文时，高亮绑到喇叭前面的文字，不再从喇叭往下找而把前面的句子丢掉。喇叭后面还有正文时，仍从喇叭所在位置开始。
  When an embedded-audio speaker sits at the end of the text and nothing follows it, the highlight follows the words in front of the icon. A speaker that still has text after it starts the highlight there.
- EPUB 改变字号后直接停在当前阅读窗口第一行的文字上，重排过程中不再先闪出封面。
  Changing the EPUB font size stays on the first line of the current reading window. The cover is not shown while the book reflows.
- 选项里的朗读页不再有中文速度、英语速度和内嵌速度。语速只在朗读条上调整。
  The Read Aloud settings page no longer has Chinese speed, English speed, or embedded narration speed. Speed is changed on the read-aloud bar.
- 朗读条可以拖到页面上的其他位置。这次显示期间停在拖到的地方；关掉后再打开，回到底部居中。位置不写入设置。
  The Read Aloud bar can be dragged elsewhere on the page. It stays there while it remains open. The next time it opens, it sits at the bottom center again. The position is not saved.
- 「自动识别没有文字层的扫描页面」（`AutoOcrScanPages`）系统默认改为开启。设置里没有这一项时生效；已经保存为关闭的保持关闭。
  Automatically OCR scanned pages (`AutoOcrScanPages`) now defaults to on when the setting is missing. A saved off value stays off.
- 有声书倍速与朗读语速对齐，最慢一档为 0.25x（朗读条）。
  Narrated-book speed matches the Read Aloud range. The slowest preset is 0.25x, on the bar.
- 有内嵌录音的 PDF（喇叭、Sound / RichMedia / Screen）用朗读条做整页播放：播放、暂停、倍速、上下一页录音。工具栏上的喇叭在这一页有录音时也播放这段录音。喇叭后面还有正文时，高亮从喇叭图标所在的位置开始；句末喇叭绑到前面的文字。跳过图下较小的说明和页边页码；录音按语音能量对齐剩下的正文，句间停顿不会把高亮提前推走。没有文字层时先识别这一页。
  PDFs with embedded audio use the Read Aloud bar for page playback: play, pause, speed, and previous or next page recording. The toolbar speaker plays that recording when the page has one. When text follows the speaker, highlight starts there; a speaker at the end of the text follows the words in front. It skips smaller captions under pictures and page numbers in the margin. The clip is aligned to the remaining text by speech energy, and pauses do not push the highlight ahead. A page with no text layer is recognized first.
- 朗读条倍速改为点开菜单直接选择，系统语音和有声书都从 0.25x 到 2x，不再逐档循环。菜单底色跟浮层一样，随暖色、白色、Dracula、纯黑四种主题变化。
  The Read Aloud bar speed opens a menu. Text-to-speech and narrated books both offer 0.25x through 2x. The menu background follows the floating panels in Warm, White, Dracula, and Black.
- 朗读条中文「跟随」与同一行的图标、倍速垂直对齐。汉字字面偏高，绘制时下移约一字高的十分之一。关闭按钮改用与播放图标同尺寸的叉，不再用标题栏那个更大的叉。
  The Read Aloud bar Chinese Follow label lines up with the icons and speed. Han glyphs sit high in the em box, so the label is shifted down by about a tenth of the font height. The close button uses the same-size X as the playback icons, not the larger title-bar X.
- 选项对话框左侧分类：行距略加大；选中框上下留白一致，字不再贴着下沿。
  Options categories on the left have a little more space between rows. The selection box pads the label equally above and below.
- 查词喇叭按当前朗读模式发音，不再改掉后面的朗读音色。在线多语言（如 Brian）查没有音标的词时，不再先切到已保存的本地英文女声；系统默认、本地中英、在线中英也只使用该模式自己的音色。下一次朗读会把合成器设回当前模式。
  The dictionary speaker uses the active Read Aloud voice and no longer leaves a different voice selected. With Online multilingual (for example Brian), a word with no phonetic no longer switches to a saved local English voice. System default, local bilingual, and online bilingual each use only that mode’s voice. The next Read Aloud chunk restores the configured voice.
- 修复教材 PDF 矩阵外括号再次显示为普通方括号（[#35](https://github.com/dengxibo/sumatrapdf-plus/issues/35) 回归）：`ABCDEF+Symbol` 子集名仍走内置 Base14 Symbol，不用 Windows Symbol.ttf。
  Fix matrix outer brackets again rendering as plain square brackets ([#35](https://github.com/dengxibo/sumatrapdf-plus/issues/35) regression): subset names like `ABCDEF+Symbol` use built-in Base14 Symbol, not Windows Symbol.ttf.

- 选项对话框「OCR 和 AI」页恢复「自动识别没有文字层的扫描页面」。打开后，遇到没有文字层的扫描版 PDF 会自动打开自动 OCR。
  Options → OCR and AI again has Automatically OCR scanned pages. When on, opening a PDF with no text layer turns Auto OCR on.
- 书签栏较窄时，放不下的标题都在末尾加省略号，不只选中的那一行。能完整显示的标题不加。
  When the bookmark column is narrow, every title that does not fit ends with an ellipsis, not only the selected row. Titles that fit are unchanged.
- 暗色主题下拖书签分隔条时，选中条目的框右边会被藏滚动条的遮罩盖住一竖。这条竖线改画在遮罩左缘上，框保持闭合。
  In dark theme, dragging the bookmark splitter hides the scrollbar with a mask that used to cover the selected row's right stroke. That stroke is now drawn on the mask's left edge, so the box stays closed.
- 取词：英式和美式音标相同时只显示一次，不再出现 `/kɔːz/ /kɔːz/`。两边不同时仍都保留。
  Word lookup shows one phonetic when the British and American readings match, instead of `/kɔːz/ /kɔːz/`. Different readings are still both shown.
- 取词：一个词有多个词性时仍显示音标。多音字标签本身就是读音，不再重复一行。
  Word lookup shows the phonetic line when a word has several parts of speech. Polyphone tabs already show the reading, so that line stays hidden there.
- OCR 英文扫描页折行拼接时插入空格，双击取词和复制不再把 from/California 粘成一词。
  English OCR soft wraps insert a space, so double-click lookup and copy no longer glue fromCalifornia into one word.
- 选项对话框去掉「全文 OCR 模式」（与工具栏快速/精确重复）；OCR 页其余分区上移重排。
  Options no longer has Full-document OCR mode (toolbar Fast/Accurate already choose it). The OCR page is reflowed so nothing leaves a blank gap.
- 书签默认不换行；选项默认开启延迟加载未激活标签页。
  Bookmarks default to single-line titles. Options defaults Lazy-load inactive tabs on.
- Light-Warm 下选项对话框输入框改用主题米色，不再刺眼的纯白。
  On Light-Warm, Options dialog edit fields use the theme cream instead of pure white.
- 书签行距只收紧上下留白，字号与字体不变（仍比菜单略大 2 像素）。
  Bookmark row spacing is tighter via padding only; font face and size stay the same (still 2px above the menu size).
- 选项对话框「界面」页里，Theme 和 Default document colors 的下拉框同宽、左右对齐。
  On Options → Interface, the Theme and Default document colors dropdowns are the same width and line up.
- 书签分隔条：出现 ↔ 光标时按下就拖侧栏宽度。悬停范围与滚动条那条相同，不盖住滚动条滑块。
  Bookmark splitter: a press while the ↔ cursor is showing drags the sidebar width. The hover strip matches the scrollbar edge and does not cover the thumb.
- 选项对话框「OCR 和 AI」页里，AI 服务下拉框与分区底边留出空隙。
  On Options → OCR and AI, the AI service dropdown sits clear of the section border.
- 取词卡换词性时，上沿固定，只伸缩下沿。
  Switching part of speech on a word card keeps the top edge fixed and resizes the bottom.
- 朗读条「跟随」支持翻译（中文显示「跟随」）。朗读时页面仍会跟着读；你自己翻页或把正在读的文字滚出安全区后，跟随停下并出现「跟随」，系统语音和有声书一样。
  The Read Aloud bar Follow button is translated (Chinese: 跟随). Playback still turns pages to follow the text. Turning the page yourself, or scrolling the spoken text out of view, stops following and shows Follow, for both text-to-speech and narrated books.

## 3.7.35 (2026-09-30)

- 有声书跟读：SMIL 一段里按词高亮（按字母/汉字分时间），不再整段黄底。词级 SMIL 不变。
  Narration follow: inside a SMIL phrase the current word is highlighted by letter/character time, not the whole paragraph. Word-level SMIL is unchanged.
- 朗读条：播放中显示暂停图标；点倍速立刻作用于当前录音或系统语音。系统语音朗读（含暂停）不显示 ±10 秒，避免播放/暂停时控制条跳动。
  Read Aloud bar: Play becomes Pause while speaking; a speed tap applies to the current recording or text-to-speech right away. Text-to-speech (including while paused) does not show ±10 s, so the bar does not jump between play and pause.
- EPUB 3 有声书：从当前页或光标处开始，只在这一处有录音时放录音；没有录音的章节用系统语音从这里读，不再跳回第一章。
  EPUB 3 narration: start from the current page or cursor plays the recording only if that location has audio; later chapters without overlays use text-to-speech from here, and no longer jump to chapter 1.
- EPUB 3 有声书倍速增加 0.5x（朗读条和设置页），方便听慢一点。
  EPUB 3 narration speed presets include 0.5x on the bar and in Settings.
- 朗读统一：工具栏喇叭、菜单栏「朗读」和底部朗读条是同一套控制。书里有录音就放录音，没有就用系统语音。设置里新增「朗读」分类页（语音、语速、高亮色、自动跟随、是否使用书内录音和书内高亮色）。从当前页或光标处开始，在有声书里不再误走系统语音。
  Read Aloud is one feature: the speaker button, the Read Aloud menu and the bottom bar share the same controls. A book with recorded narration plays that audio; otherwise text-to-speech is used. Settings has a Read Aloud page (voice, speed, highlight colour, auto-follow, and whether to use the book's audio and highlight colour). Start from top or from the cursor no longer falls back to TTS on a narrated book.
- EPUB 3 有声书（Media Overlays）：书里自带真人朗读时，底部出现朗读条。点「朗读」或朗读条上的播放键，就放书里的录音，正在读的句子会高亮，页面自动跟着翻，一章读完接着读下一章。高亮颜色用书里定义的 `media:active-class`，没有就用朗读的黄色。朗读条上有上一句/下一句、后退/前进 10 秒、0.5–2 倍速和时间。手动滚走后停止跟随，点「跟随」或滚回来就接着跟。`linear="no"` 的章节不会被连播进去；没有录音的句子用系统语音读。录音只从书内读取，远程地址和指向书外的路径一律不放。普通 EPUB 不受影响。
  EPUB 3 narrated books (Media Overlays): when a book ships its own narration, a narration bar appears at the bottom. Read Aloud, or Play on the bar, plays the book's audio, highlights the sentence being read, turns pages to follow it and continues into the next chapter. The highlight uses the book's `media:active-class` colour, or the Read Aloud yellow. The bar has previous/next phrase, back/forward 10 s, 0.5–2x speed and elapsed time. Scrolling away stops following; Follow, or scrolling back, resumes it. Spine items with `linear="no"` are not played through; phrases without audio are spoken with text-to-speech. Audio is only read from inside the book: remote URLs and paths leaving the book are not played. EPUBs without narration are unchanged.
  播放中调字号会保留读到的位置；录音文件缺失的句子改用系统语音；读完后高亮消失，再按播放从当前页开始；窗口很窄时朗读条自动收起时间、倍速和 ±10 秒按钮；深色主题和近白色的出版方高亮色会自动调整，保证看得清。
  Changing the font size during playback keeps the position; phrases whose audio file is missing are spoken with text-to-speech; at the end the highlight is cleared and Play starts from the current page; in a narrow window the bar hides time, speed and ±10 s; dark themes and near-white publisher colours are adjusted so the highlight stays readable.
- 修复固定版式 EPUB 在渐进加载时追加的页面被排成 0×0、无法翻到（如 SVG 页）；平滑滚动遇到无法到达的目标位置不再一直空转。
  Fixed-layout EPUB: a page appended during progressive loading was laid out as 0x0 and could not be reached (e.g. an SVG page). Smooth scrolling no longer spins forever on an unreachable target.
- Ctrl+滚轮再次尊重高级设置：`ZoomIncrement = 0`（默认）按 `ZoomLevels` 档位步进；大于 0 时按该百分比相对缩放（并仍限制连滚过冲）。此前滚轮被写死为约 10% 相对缩放，改 `ZoomLevels` 无效。
  Ctrl+wheel again honors Advanced Options: `ZoomIncrement = 0` (default) steps through `ZoomLevels`; a positive value uses that percent relative zoom (still rate-limited). The wheel had been hard-coded to ~10% relative, so changing `ZoomLevels` did nothing.
- 提取目录：没找到印刷目录、自己输入页码后，网页 AI API 按钮也可点（以前只有网页 AI 那个键能用）。
  Extract TOC: after typing page numbers when no printed TOC was found, the web AI API button is enabled (previously only the web-AI button worked).
- 跟随主题：扫描标题上的色块不再被当成照片头发保护而留下；照片里的黑发仍然不涂成主题白。
  Match theme: fat scanned titles SharpDocument to theme white again, without undoing photo hair protection.

## 3.7.34 (2026-09-29)

- 跟随主题：RAZ 影棚绘本（如 Getting Dressed）不再因整页白底被当成连环画，黑发也不再被涂成主题白；动物园这类白卡动物图继续抠掉白底，浅色鳞片和毛边不会把整张卡退回白矩形。
  Match theme: soft studio photo books (such as Getting Dressed) are no longer treated as line art because of a full-page white mat, and dark hair is no longer painted theme-white. Zoo-style animal cards still knock out the white mat; light scales and fur AA no longer abort the cutout back to a white rectangle.
- Explorer 里 PDF 文件关联图标改用与 epub/chm 同风格的 `pdf-32bit` 色带图标，小尺寸不再糊成一团（issue #83）。
  The PDF file-association icon in Explorer uses the same sash-style `pdf-32bit` artwork as epub/chm, so it stays readable at 16–32px (issue #83).
- AI 提取目录：可配置网页 AI API（密钥用 Windows DPAPI 加密存放）；新增 AutoOcrOn 等设置；对话框底栏按钮按翻译文案自动加宽，并显示应用图标。
  AI TOC: optional web AI API profiles with keys stored via Windows DPAPI; AutoOcrOn and related settings; footer buttons size to their translated labels, and the dialog shows the app icon.
- 连续适合页面时，向上滚轮翻到上一页底部。以前 `GoToPrevPage(bottom)` 把整页高度加在上一页顶上，视口仍停在当前页。
  In continuous fit-page view, wheel-up lands on the bottom of the previous page. `GoToPrevPage(bottom)` used to add a full page height to the previous page top and leave the view on the current page.
- 跟随主题白底 flood：泡沫高光、填色矩形与椭圆照片不再被内部 flood 啃掉；啃边的户外场景会 abort，影棚白卡仍可抠底。
  Match-theme white-mat flood: foam highlights, filled rectangles, and ellipse/circle photos are not chewed by an interior flood; nibbled outdoor scenes abort while studio cards still knock out.
- 智能反色：人脸粘贴用带边距的 landmark 框，侧脸鼻子和低头的眼睛不再留下反色缺口；教材布局大图跳过重 remap，远距离目录跳转优先渲染当前页。
  Smart invert: face paste uses padded landmark bounds so profile noses and looking-down eyes are not left inverted; layout-textbook photos skip heavy remaps, and far TOC jumps prioritize the visible page.
- 发布构建复制 OCR 侧车时一并带上人脸模型文件。
  Release builds that copy the OCR sidecar also include the face-model files.

## 3.7.33 (2026-09-28)

- 魔法棒（增强显示）按下去只记在当前这本书上。关掉再打开，淡色扫描书还是按下的，别的书还是关的。换一本书，按钮跟着那本书变。
  The Enhance Display wand stays with the book it was pressed on. After a restart, a pale scan is still on and other books stay off. Switching books follows that book's own state.

- 暗黑主题的智能反色翻到新图片更快。以前每张图都要对每个像素做一整套颜色换算，照片还要逐块找人脸。现在同一套主题色只换算一次，后面的图查表；找人脸的几块同时进行。人脸仍按原来的方式保留原色。翻回已经看过的页还是用缓存。
  Smart invert in a dark theme turns onto a new picture faster. Each picture used to convert every pixel, and a photo also scanned for faces one block at a time. One theme palette is converted once and later pictures look it up; the face scan runs several blocks at once. Faces still keep their original color the same way. A page already seen still comes from the cache.

- 套装电子书里，点目录会跳到另一本书。目录页码是按大纲顺序存的，建侧边栏时每个条目把序号加了两次，越往后越对错位。维京传奇的书名页因此打开了失落的古城。同一套书里重复的「书名页」「版权页」也会串到后面的书。
  In a boxed-set ebook, a contents entry could open a different book. Page numbers are stored in outline order, but building the sidebar advanced the index twice per entry, so later entries read the wrong page. The Viking Sagas title page therefore opened Lost Cities. Repeated entries such as title page and copyright page in the same set jumped ahead the same way.
- 页面已经完整显示在窗口里时，滚轮一格翻一页。以前只有「适应宽度并连续」或「适合单页」按下时滚轮才翻页；两个都没按下（常见的是单页但不是适合页面，或连续但不是适应宽度）时，滚轮不动，只能拖右侧滚动条。页面比窗口高时，滚轮仍在页内滚动，滚到底再进入下一页。
  When the whole page already fits in the window, one wheel notch turns a page. Previously the wheel turned pages only while Fit Width and Continuous or Fit a Single Page was pressed. With neither pressed (single page at another zoom, or continuous but not fit width), the wheel did nothing and only the right scrollbar moved. A page taller than the window still scrolls inside the page, then continues to the next page.
- 自动 OCR 不再因为文件是扫描件就自己打开。工具栏上的开关完全手动，开或关记在这份文档上，下次打开还是这个状态。别的文档不受影响。
  Automatic OCR no longer turns itself on for a scanned PDF. The toolbar switch is manual, and its on or off state is stored with that document, so reopening it keeps the same choice. Other documents stay unchanged.
- 暗黑主题里，文档颜色模式按钮右边多了一个三角菜单，用来选图片怎么处理。自动仍是现在的按图判断。另外两项是：图片保持原色，以及智能反色（色相不动，明暗改落到主题背景和主题文字上）。简单反色已去掉，以前选过它的会改用智能反色。浅色主题和「原版」不受影响。
  In a dark theme, the document color button has a menu for how pictures are treated. Automatic is still the current per-picture choice. The other two leave picture pixels unchanged, or smart-invert: keep hue and reseat lightness onto the theme background and theme text. Simple invert is gone; a saved choice of it becomes smart invert. Light themes and Original document colors are unchanged.
- 自动 OCR 只补文字，不再转动页面，也不再做倾斜校正。自己点的识别仍会转正。文件菜单里的「手动调整页面」可以按任意角度转动、左右或上下翻转，并裁掉转出来的空白三角。正好 90° 时只改页面旋转标记，批注跟着转；其他角度写进页面内容，批注留在原来的位置。
  Automatic OCR only adds text. It no longer turns a page or straightens a tilt. OCR you start yourself still does. Manually Adjust Pages can turn by any angle, flip horizontally or vertically, and crop the empty corners. A multiple of 90° only changes the page rotation flag, so annotations turn with the page. Any other angle is written into the page content, and annotations stay where they are.
- 电子书暗黑模式下，白纸上已经抠好的插图会去掉这层白底，底下露出页面背景。只有四边都是白纸、主体不贴边、轮廓清楚的图才会这样；版画、整幅照片和拿不准的图保持原样。
  In a dark ebook theme, a picture that is clearly a cutout on white paper loses that paper and shows the page background. Only a picture with white on all four sides, a subject that does not touch the edge, and a clear outline is changed. Engravings, full photos, and uncertain pictures stay as they are.
- 朗读菜单里的「在线多语言语音」分成选定和设置两项，和本地、在线中英双语一样。选定用当前多语言语音，设置里再换具体的声音。
  Read Aloud’s online multilingual entry is a choice plus a settings item, like the local and online English–Chinese modes. Choosing it uses the saved multilingual voice; settings picks which voice that is.
- 文档里表格右边的线和其余边一样，是完整的黑线，角上接齐。这条竖线画在格子外侧，裁在页边里时只剩半道灰线，横线还从它右边冒出去。
  A document table's right edge is a full black line and meets the corners, like the other sides. That stroke sits just outside the cell; clipping it left a gray half-line, with the horizontal rules sticking out past it.
- 电子书放大字体后，行末的字不再被切掉。页边裁剪用的矩形少算了左边距，每一行最右边大约一个字被裁没；字号保存着，所以重启也还是缺字。
  Enlarging the ebook font no longer cuts off the last character of each line. The page clip rectangle ignored the left margin, so about one glyph at the right edge was discarded. The font size is saved, so restarting did not bring that character back.
- 电子书里写在居中段落中的定宽插图会回到段落中间。改成块级显示之后，段落的居中只作用在说明文字上，图本身贴在左边。
  A fixed-width picture inside a centered ebook paragraph sits in the middle of that paragraph again. Making pictures block-level left the centering on the caption only, so the picture stayed on the left.
- 电子书插图不再被页边切开。图排在下一页开头时，上一页的下边距里以前会露出一条，翻过去又是同一张图的开头。
  An ebook picture is no longer sliced by the page edge. A figure that starts at the top of the next page used to leave a strip in the previous page's bottom margin, then appear again in full.
- 电子书里写死宽度的插图（`width="900"` 这类）会缩进版心，整张图都看得到。以前比页面宽的部分被裁掉，表格和示意图的右边显示不全。
  Ebook figures with a fixed pixel width (`width="900"`) shrink to the page, so the whole picture stays visible. The part wider than the page used to be clipped, which cut off the right side of tables and diagrams.
- 电子书的下划线、删除线和波浪线按每一行来画。换行的段落以前合成一个大框，线按整段高度来算，所以又粗，又只出现在最后一行。
  Ebook underline, strikeout, and squiggly marks are drawn per line. A wrapped paragraph used to become one box, so the stroke was as tall as the whole paragraph and showed up only on the last line.
- 电子书暗黑模式不再把插图涂成主题色。照片、线条图、表格图各不一样，整张换色会误伤。
  Dark theme for ebooks no longer recolors pictures into the theme colors. Photos, line art, and diagram images differ too much to recolor as a group.
- 扫描页 OCR 之后复制选中文字会进剪贴板。段落合并复制以前只在遇到换行标记时才写出文字，选区末尾那一段（常常是整段）被丢掉，剪贴板因此是空的。
  Copying a text selection on an OCR'd scan puts the text on the clipboard. Paragraph-merged copy used to emit text only at a line-break marker, so the tail of the selection (often the whole selection) was dropped and the clipboard stayed empty.
- 提取目录、校准书签时，书签栏底部不再露出一块白底。校准条还没出现时不再给它留空。
  While bookmarks are being calibrated, the sidebar no longer shows a white strip at the bottom. That space is reserved only once the calibration bar is visible.
- 对准印刷目录：页差显示完整数字（`+199` 不再被截成 `+1...`）。页脚页码和当前页相同、或大于 400 时仍保留。隔得很远的页面上重复出现的同一个数字（页眉）不再当成印刷页。
  Align printed TOC: the offset shows the full number (`+199` is no longer cut down to `+1...`). A footer folio is kept when it matches the viewer page or is past 400. A digit repeated on distant pages (a running header) is not used as the printed page.
- Word 正文标题会带上样式里的自动编号（`第1章`、`1.1`、`1.1.1`）。目录页上的条目不再混进侧边栏；同一标题上后加的书签也能跳到这一节。
  Word headings show the numbering stored on the style (`第1章`, `1.1`, `1.1.1`). Printed contents lines stay out of the sidebar, and a later bookmark on the same heading still jumps to that section.
- Word 正文里的目录页会画出标题和页码之间的点线（`........`），点击一条会跳到对应标题。
  A Word document's printed contents page draws the dotted leaders between each title and its page number, and clicking an entry jumps to that heading.
- 切换标签时不再崩溃：延迟加载和异步打开各持有一份阅读进度，完成加载时只应用一次。先前两处共用同一份状态，后一次在 `displayMode` 已被释放后仍去比较，触发访问冲突。
  Switching tabs no longer crashes. A lazy restore and the async open each keep their own copy of the reading state, and the load is finished only once. They used to share one `TabState`, so the second finish compared `displayMode` after it had been freed.
- 本地提取目录：`1.1` / `1.1.1` 挂在当前 `第N节` 下，没有节时挂在 `第N章` 下，再没有章时挂在 `附件N` 下。报告标题把章压到第 2 层时，`1.1` 不再和 `第1章` 并列。Word 与公文 PDF 同一套层级。
  Local TOC extract: `1.1` / `1.1.1` nest under the open `第N节`, otherwise under `第N章`, otherwise under `附件N`. When a report title already places the chapter at level 2, `1.1` is no longer a sibling of `第1章`. Word and official PDFs share this layout.
- 对准印刷目录：印刷页和 PDF 页中间显示页差（如 `+5`），箭头在数字下面，字号与页码相同。和上一行不同的页差用正文色，其余用灰色。没有印刷页时不显示页差。
  Align printed TOC: the gap between the printed folio and the PDF page shows the offset (for example `+5`), with the arrow under that number, at the same size as the page digits. An offset that differs from the previous row uses normal text; the others stay gray. Rows without a folio show no offset.
- 对准印刷目录：双击一条目录，在已经核对并发给 AI 的那几页目录里找这条标题，落到它所在的那一页，不再总是打开目录首页。底栏「目录页」和顶上的「目录」打开这份名单的第一页。扫描书只识别这几页，不重扫全书。
  Align printed TOC: double-click a row searches only the TOC pages already checked and sent to the AI, and opens the sheet that row is printed on. The bottom TOC-page button and the top Contents entry open the first of those pages. A scan OCRs just those sheets.
- 对准印刷目录：在一条目录上右键「后面都用这个页差」，只改这一条往下的 PDF 页，印刷页保持原样。每缺一页就多一段页差，不再收成全书一个页差，因此左边的印刷页仍与正文上印的页码一致。再缺一页时，在下一条不准的地方改一次 PDF 页，再点一次。改 PDF 页时如果读不到页脚，保留原来的印刷页。
  Align printed TOC: “Apply This Page Offset Below” changes only the PDF dests from that row downward and leaves printed folios unchanged. Each missing sheet adds its own offset instead of collapsing the book to one, so the left-hand folio still matches the number on the page. After another missing sheet, correct the next bad row and run the command again. Editing a PDF page keeps the existing folio when that page has no footer text.
- 对准印刷目录：导入时若后台正在识别同一页，保留该页，不再把正在渲染的页面拆掉（否则会在 `pdf_document_output_intent` 崩溃）。
  Align printed TOC: import keeps a page that background OCR is still rendering instead of dropping it, which crashed in `pdf_document_output_intent`.
- AI 目录：超过 10 张目录页时，每批只提取本批 JSON。复制回这一批后才发下一批，程序按顺序把各批条目接上，不再让 AI 合并（合并时会编造条目）。重新发送在等待下一批时不再使用已被关掉的对话框里的文件列表。
  AI TOC: when printed TOC pages exceed 10, each batch extracts only its own JSON. The next batch is sent after that JSON is copied back, and the app joins the items in order. The AI is not asked to merge them, because that step invented entries. Resend keeps its own copy of the page images, so closing the dialog during the wait between batches no longer crashes.
- 暗黑模式：黑白照片不再被当成教材投影底板整块涂成背景色。
  Dark mode: grayscale photos are no longer filled with the page background as if they were textbook drop-shadow plates.
- 对准印刷目录：校准栏并排两个页码框，中间用箭头连成「印刷页 → PDF 页」，去掉加减和栏头。空框里是浅色提示，悬停显示「印刷页码 / PDF 页码」。没有印刷页的行左框留空，点进去不会把 PDF 序号当成印刷页。改 PDF 页只改跳转目标，再按该页页脚回填或清空印刷页。
  Align printed TOC: each row is two page boxes joined by an arrow (printed folio → PDF page), with no plus/minus and no column header. An empty box shows a faint cue; hover names the field. A row with no folio stays blank on the left, so a click cannot treat the PDF index as a printed page. Editing the PDF box sets the dest, then fills or clears the folio from that page's footer.
- 对准印刷目录：定位把标题填进查询窗后立即开始搜索，并在当前页及之后的第一个命中出现时自动翻到该页（不必再按回车或点结果行）。
  Align printed TOC: Locate starts the search as soon as the title is in the find box, and jumps to the first hit at or after the current page as soon as that hit is found.
- 对准印刷目录：正文页脚被拆碎时，不再按「最长阿拉伯段」选主正文（书后技能/附录重印常更长），改为 TOC 之后最早命中；R1/R20 等handbook页脚忽略印刷目录点线假命中，取书后真实 folio；同名 R1 可共享目标；页码栏只显示印刷页/标签。
  Align printed TOC: when body footers fragment, use the earliest post-TOC hit (not the longest run — late skills reprints often win). R1/R20 handbook folios ignore printed-TOC leader-dot false hits and prefer the real end-of-book page; duplicate R1 rows may share one PDF dest. Page field shows printed folios/labels only.
- UI: page display filter (brightness / contrast / sharpness) is per-document and opened from the toolbar; paint-time only so sliders do not re-render. Presets: Reading / Scan / Reset. Saved on the file's `FileState` (`DisplayFilterBrightness` / `Contrast` / `Sharpness`). Does not affect print or copy-page-as-image.
  界面：页面显示滤镜（亮度/对比度/锐度）按文档保存，从工具栏打开；贴屏时套用，拖滑块不重渲染。预设：阅读优化/扫描件/重置。写入该文件的 FileState。不影响打印与复制页面图。
- UI: add `TabFontSize` (0 = follow UI font) and `TabBarHeight` (0 = automatic ~24 DIP) so the tab bar can be sized independently of global `UIFontSize`; exposed in Options → Interface → Tabs and toolbar.
  界面：新增 `TabFontSize`（0=自动）与 `TabBarHeight`（0=自动约 24 DIP），标签栏可单独调字号/高度；选项对话框「界面 → 标签页和工具栏」可改。
- office: honor Word table `trHeight` (atLeast/exact) as cell height floor so blank form rows are not hairline-thin; emit vertical `textDirection` cells one glyph per line; do not apply auto line-height &lt; 1 (crushed overlapping glyphs in form labels).
  Office：表格行高 `trHeight` 生效（空白表行不再挤成一条线）；竖排单元格逐字换行；自动行距小于 1 时不再压叠字形。
- office: 公文红头 that Word packs with `w:jc=right` + character scale (`w:w`) no longer wraps into a right-aligned mess; treat as a centered banner and fit one line. Large `w:ind left` fake-centering (文号 / 副标题) maps to true `text-align:center`.
  Office：红头用右对齐+字符缩放压成一行时，不再折成右对齐多行；按居中条幅拟合。文号等大 left 假居中改为真正居中。
- OCR: region select is a temporary tool — hold `Alt` and drag with the left mouse button (modifier only needed at mouse-down). Keyboard backup `Ctrl+Shift+X` (`CmdOcrRegion` / 框选识别); `Ctrl+Shift+O` kept for compatibility. Override via Advanced Settings → Shortcuts if needed. (`Shift`+drag remains document pan.)
  OCR：框选识别改为按住 `Alt` 拖框的临时工具（仅需在按下时按住修饰键）；键盘备用 `Ctrl+Shift+X`；保留 `Ctrl+Shift+O` 兼容。`Shift`+拖仍为文档平移。
- plugin: when launched with `-plugin` (Total Commander sLister / similar), do not flash a top-level window or steal focus before embedding; force off session restore / remembered files / update checks in plugin mode (fixes dengxibo/sumatrapdf-plus#32). Prefer [TCSumatraPDF](https://totalcmd.net/plugring/wlx_TCSumatraPDF.html) over the unmaintained sLister for F3 / Quick View.
  插件：`-plugin` 嵌入（TC sLister 等）时不再先闪一个独立窗口再嵌进去，也不抢 TC 焦点；插件模式关闭会话恢复/历史文件/自动更新（#32）。F3 / 快速查看更推荐用 TCSumatraPDF。
- automation: document DDE `Open` 5th arg `inCurrentTab=1` (replace file in the current tab); add `WM_COPYDATA` magic `'Opn2'` (`SumatraOpenCopyDataEx`) for Excel VBA / SendMessage to do the same without DDE.
  自动化：文档化 DDE `Open` 第 5 参数 `inCurrentTab=1`（当前标签页替换打开）；新增 `WM_COPYDATA` 魔数 `'Opn2'`，便于 Excel VBA 用 SendMessage 在当前标签页快速预览 PDF。
- office: when a run omits w:sz, resolve size from pPr rPr / paragraph style (basedOn) / Normal; set body CSS font-size from Normal (fixes 公文 recipient lines like 省政务服务办公室： rendering at 12pt while body runs are 16pt).
  Office：run 未写字号时从段落样式链解析；body 默认字号取自 Normal（修复主送机关等行偏小）。
- office: load `word/numbering.xml` and emit real list markers (`1.` / `（1）` / `一` / bullets) with left/hanging/firstLine indent instead of blind `<ul>` bullets; typed 一、/（一）/1. outline headings use `hN.Outline` so TOC keeps structure without enlarging/bolding body text; inherit \irstLine\ from paragraph styles (e.g. Normal) when the paragraph omits \w:ind\; map Word spacing/line to CSS margin and line-height.
  Office：读取 numbering.xml，按大纲生成真实编号与悬挂缩进（不再把 numPr 一律变成无心圆点列表）；正文里手打的一、/（一）/1. 仍进书签，但不改成放大加粗标题字号；段落未写缩进时继承正文样式的首行缩进；映射 Word 段前/段后/行距到 CSS。
- office: resolve Word table borders from `tblStyle` / `tblBorders` / `tcBorders` (`insideH`/`insideV`, `w:sz` → fractional pt) instead of forcing every cell to `1px` black; seal collapsed-border paint so shared edges do not leave 1px AA gaps.
- office: 落款 (agency + date): keep the block right-aligned, but center the shorter date line under the agency name (Word pads with spaces; we use `padding-right` after stripping).
  Office：落款靠右不变，日期相对单位名称居中（不再与单位右缘齐平）。
- office: do not inject Word `lastRenderedPageBreak` page `<div>`s mid-paragraph (orphaned font-size spans made continuation text shrink / change face); same rule as mid-table — MuPDF reflow paginates.
  Office：段落中间的 Word 分页标记不再插入 page `<div>`（会拆断字号/字体 span）；与表格内一样交给排版分页。
- office: skip empty Word spacer paragraphs (they painted a blank line); a paragraph that only holds `sectPr` is a section break — omitted `w:type` means next page, so the following block gets `page-break-before:always` (附件 starts its own page). Do not wrap the body in `lastRenderedPageBreak` page `<div>`s.
  Office：跳过空白占位段落（不再画出句中空行）；只含 `sectPr` 的段落是分节符，省略类型按下一页处理，下一块 `page-break-before:always`（附件另起一页）。正文不再包 `lastRenderedPageBreak` 的 page `<div>`。
- office: a short `2.` / `4.` line with a large `w:ind left` is a real list indent, not a fake-centered title. A second `3.` run packed into the same paragraph breaks onto its own line so 附件 items share one left edge.
  Office：短的 `2.`/`4.` 大左缩进是附件条目，不再当成假居中标题。同一段里后一个 `3.` 另起一行，附件条目左缘对齐。
- office: keep table-cell `w:ind left` (form header labels are centered that way) and drop the default cell padding that wrapped them; a fake-centered title that only slightly overflows the measure stays one line.
  Office：单元格保留 `w:ind left`（表头靠它居中），并去掉默认内边距以免表头换行；略超版心的假居中标题收成一行。
- office: honor `w:ind right` so a narrow line of `1. 2. 3. 4.` wraps one marker per line; drop tracking-shim spaces (negative `w:spacing`); do not center a `2025年 月 日` line that is positioned by left indent.
  Office：右缩进生效，`1. 2. 3. 4.` 各占一行；负字距的空格不再留洞；`2025年 月 日` 按左缩进靠右，不再居中。
- office: center text in the first two table rows (表头). Word fakes that with `w:ind left`, which stays stuck to the left once the cell width is a percentage; also vertically center those header cells.
  Office：表头两行文字水平、垂直居中。Word 用左缩进假居中，格子按比例变宽后字会贴在左边。
- office: leading spaces on a heading are the indent when `w:ind` is missing (`一、` / `二、` use four spaces; `三、` uses firstLine). Keep that width as `text-indent` after the spaces are stripped for the TOC.
  Office：一级标题有的用行首空格缩进、有的用首行缩进。空格被标题去掉后，`一、` `二、` 会比 `三、` 靠左。现在按空格宽度补回缩进。
- office: keep a blank paragraph's line spacing (落款后的空行) so 附件1 starts on the next page. Outline headings do not add extra margin, so the signature and date stay on the previous page. A sectPr paragraph is still a page break, not a blank line.
  Office：落款后的空行留出行距，附件1才换页。一级标题不再额外加段前段后，落款和日期留在上一页。分节空段仍是换页。
- office: do not paint blank paragraphs that sit in front of a 落款日期. Those gaps were pushing the date onto the next page, away from the unit name. Blanks after the date (before 附件) are unchanged.
  Office：单位落款和日期之间的空行不再撑开，日期不会被顶到下一页。日期后面、附件前面的空行仍保留。
- office: size a Word drawing from its `wp:extent` in CSS pt, not the bitmap pixels and not HTML width/height (those are read as pt and come out a third too tall). A scaled screenshot was filling most of a page and leaving 落款 alone on the next page.
- display: Deskew lives with OCR — toolbar OCR dropdown and the page context menu offer Deskew Page / Deskew All Scanned Pages; Recognize All can deskew before OCR (`OcrDeskew`, default on). Manual deskew marks the tab dirty (red dot) and bakes into the PDF on Save; it does not auto-save.
  显示：倾斜校正挂在 OCR 上——工具栏 OCR 下拉和页面右键提供「倾斜校正当前页 / 全部扫描页」；全文识别可先校正再认字（`OcrDeskew`，默认开）。手动校正会标脏（小红点），保存时写入 PDF，不会自动保存。
- display: View → Deskew Page straightens a slightly tilted scan (about 0.4° to 12°). Separate from 90° rotate and PDF `/Rotate`.
  显示：视图菜单「倾斜校正」把略微歪斜的扫描页转正（大约 0.4° 到 12°）。与 90° 旋转和 PDF `/Rotate` 分开。
- display: Ctrl+Plus and Ctrl+Minus zoom by 5% of the current zoom. The mouse wheel still uses ZoomIncrement, or the preset ladder when that is 0.
  显示：Ctrl+加号和 Ctrl+减号按当前缩放的 5% 步进。滚轮仍走缩放步长；步长为 0 时用原来的固定档位。
- office: keep Word heading and title font sizes. `h1 { font-size: 1.7em }` was wrapping form titles such as `党支部委员会…整改清单` onto a second line. Bold centered titles that are only a little wider than the page are fitted onto one line.
  Office：Word 标题不再被 HTML 的 h1 放大。加粗居中、略超页宽的表题（如整改清单）缩到一行，和 Word 一样。
- office: Word `.doc` / `.docx` can extract a sidebar table of contents the same way as PDF (menu and the empty-bookmark link). The result stays in the sidebar for this session; it is not written back into the Word file.
  Office：Word 的 `.doc` / `.docx` 也可以提取目录（菜单和「暂无书签」下的链接）。结果只留在本次侧栏，不写回 Word 文件。
- office: a later section with a different page size (函 portrait, 附件横表 landscape) keeps page 1 and lays the table on its own paper, so the grid is not squeezed until the cell borders disappear. The section after that横表 uses its own paper again, so a portrait 附件 title and a large image (QR code) stay on one page.
  Office：后面的节如果纸张不同（函是竖版、附件是横表），第 1 页保持竖版，表格按横表排，格子线不会被挤没。横表之后的节再换回自己的纸张，竖版附件的标题和大图（二维码）留在同一页。
  Office：图片按 Word 里的显示尺寸排。截图被放大后，落款和日期会单独掉到下一页。
- office: an attachment line like `2.` uses `w:ind` left+hanging plus spaces in that gap so the number lines up under `1.`. Stripping the spaces for the TOC must leave the gap.
  Office：函末「2.」用左缩进加悬挂，空格填在悬挂空隙里，这样才能和「1.」对齐。标题去掉空格后要把这段空隙留住。
- office: sidebar TOC from HTML headings (not Word's TOC field); promote centered 公文文头 (`关于…的函/通知/请示`) and Word Title/标题 to `h1` so the document title appears in 书签.
  Office：书签来自 HTML 标题层级（不是 Word 目录域）；居中公文文头与 Title/标题样式升为 h1，文头进入书签。
- display: Fit Content on fixed-page reflow (docx/EPUB) no longer zooms past Fit Page on sparse pages (e.g. a short table on page 2 looking suddenly huge).
  显示：docx/EPUB 等固定页高文档在「适合内容」下，稀疏页（如第 2 页短表）不再放大超过「适合页面」。
- office/TOC: drop blank Heading paragraphs from the sidebar (empty `<hN>` / whitespace-only outline titles); strip leading Word space-padding on real headings.
  Office/书签：空的 Heading 段落不再进侧栏；标题去掉 Word 前导空格填充。
- office: do not inject `lastRenderedPageBreak` page `<div>`s inside tables (was orphaning rowspan borders and breaking HTML); use MuPDF `overflow-wrap:break-word` (not unsupported `word-wrap`); stop rowspan row-height floors from shoving later rows; do not paint cell backgrounds over suppressed shared borders.
  Office：按 OOXML 解析表格边框（表样式 / tblBorders / tcBorders，含 insideH/V 与 w:sz→pt），不再给每个单元格强加 1px 黑边；折叠边框绘制时密封接缝，避免亚像素缺口。
  Office：表格内不再插入 Word 分页 `<div>`（会拆坏 rowspan、产生孤立竖线）；单元格改用 MuPDF 支持的 overflow-wrap；修正 rowspan 行高地板；背景不再盖住共享边。
- EPUB: fix HTML box generation depth accounting so deeply nested or very large single-spine chapters no longer leak parse depth (could truncate later content or fail hard during open). Soft-cap nest depth at 100 with a balanced hard limit.
- UI: fix Warm/Light theme white gap or stray frame line between the menu bar and toolbar (rebar always paints chrome background; frame erase no longer leaves unpainted strips).
- annotations: show a small note badge on highlights/underlines that have written contents, and mark them with ✎ in the annotations list (PDF and EPUB).
- options: expand the Options dialog into General, Interface, Reading, and Advanced categories, exposing commonly used settings without editing the advanced settings file.
- options: add OCR and AI settings, theme/document colors, dictionary, sidebar font, display quality, and expert window settings to the categorized Options dialog.
- settings: remove the redundant `Settings` / `Advanced Options...` menu; open the settings file from Options → Advanced instead.
- options: rename "Document colors" to "Default document colors" (toolbar still switches the current document).
- extract TOC: keep `关于印发…的通知` as the first bookmark and nest the attached 实施方案 under it (do not rewrite the notice into a duplicate plan title).
- extract TOC: fold a bare sheet title that repeats under a named `附件N-M` (e.g. `…报告（参考格式）` next to `附件2-1 …摘要`) into that attachment and drop trailing glued `摘要`.
- extract TOC: short complete outline titles under narrative 附件 (`二、评估方式和方法` / `四、评估结论` / `一、基本情况`) are not table-column cells; truncated first-column wraps (`一、实现人社数`) still are, and salvage must not revive them.
- extract TOC: after `（一）…4.` restart `1.` under the next `（二）` even when DP has not kept `（二）` yet; Mixed 附件 short `一、窗口设置` / `一、工作流程` stay as outline, not table cells.
- extract TOC: body `1.规范经办流程` is not a 函末 attachment index just because it ends with 流程 — only strong sheet names (`申请表` / `一览表`) or a matching 附件 title drop ArabicDot as 附件N.
- extract TOC: body cites like `按照…关于印发《他文》的通知（文号）要求` under a named 附件 must not become TOC parents of `一、`; do not strip the `按照…厅` lead-in into a fake notice title.
- extract TOC: bare `附表` after numbered `附件N` stays one unnumbered bookmark — do not invent `附表4` or glue table-header cells (`财政非税`) onto the sheet name.
- extract TOC: narrative 附件 member-unit spines (`三、省财政厅` … `二十五、商业保险公司`) are not table-column cells; salvage holes from `二、` to `二十、` and trailing `二十三`…`二十五`.
- extract TOC: OCR-glued 红头+文号 (`…厅文件` / `吉人社发〔2020〕25号`) before `关于印发…的通知` is stripped — 文号 is not a bookmark, and the mash must not parent `一、`.
- office: read-only viewing of classic `.doc` (via Microsoft Word COM → temporary `.docx` when Word is installed) and improved `.docx` HTML fidelity (images, bold/italic/underline, headings, lists, tables, font size/color). `.xls` / `.ppt` still unsupported.
- office: Word sidebar TOC from heading styles (including numeric styleIds via `styles.xml`) and named bookmarks; `_Toc*` anchors preserved for navigation.
  选项：将“选项”窗口扩充为“常规、界面、阅读、高级”四个分类，常用设置无需再手工编辑高级设置文件。
  EPUB：修复超深嵌套／超大单章 HTML 解析深度计数错误（此前会泄漏 depth，打开时可能截断或崩溃）。
  界面：修复 Warm/浅色主题下菜单栏与工具栏之间白缝或细框线（缩放时忽隐忽现）。
  批注：高亮/下划线若写了备注内容，页面上显示小便签角标，标注列表里带 ✎，便于区分摘抄与批注。
  设置：去掉菜单里重复的「高级选项…」，改从「选项 → 高级」打开设置文件。
  选项：将「文档颜色」改为「默认文档颜色」（工具栏仍负责切换当前文档）。
  提取目录：保留「关于印发…的通知」为第一条，实施方案挂在其下，不再把通知改写成重复的方案标题。
  提取目录：附件N-M 已带表名时，旁边重复的裸表名（如「…报告（参考格式）」）并入该附件，并去掉粘上的「摘要」。
  提取目录：附件正文里完整的短大纲（「二、评估方式和方法」「四、评估结论」）不当成表格竖列；截断的「一、实现人社数」仍丢掉，且补洞不再救回。
  提取目录：（一）…4. 后，下一节（二）下的 1. 要重启；Mixed 附件里「一、窗口设置」「一、工作流程」等短大纲保留。
  提取目录：正文「1.规范经办流程」不以「流程」结尾就当成函末附件索引；只有申请表/一览表等强表名或与附件同名才并进附件N。
  提取目录：附件正文里「按照…关于印发《他文》的通知（文号）要求」是引用依据，不得当成一、之上的大标题。
  提取目录：正文「附表」在附件N之后仍是一条无编号书签，不编成「附表4」，也不把表头「财政非税」粘进标题。
  提取目录：附件里成员单位职责「三、省财政厅」…「二十五、商业保险公司」不当成表格竖列；二→二十的空洞和二十三…二十五也要补全。
  提取目录：OCR 把红头「…厅文件」和文号粘到「关于印发…的通知」前时剥掉红头文号，文号不做书签，也不再当一、的父节点。
  Office：只读打开老版 `.doc`（本机装有 Word 时经 COM 转为临时 `.docx`）；`.docx` 显示更接近 Word（图片、加粗斜体下划线、标题、列表、表格、字号颜色）。`.xls` / `.ppt` 仍不支持。
  Office：侧栏目录支持 Word 标题样式（含 styles.xml 里的数字 styleId）和命名书签；保留 `_Toc*` 锚点便于跳转。

## 3.7.31 (2026-09-15)

- PDF repair: documents that require lenient page-tree recovery are now marked as repaired, forcing a full rewrite when AI TOC or other PDF edits are saved instead of producing an invalid incremental update.
  PDF 修复：需要宽容页面树恢复的损坏文档现在会标记为已修复；保存 AI 目录或其他 PDF 编辑时将强制完整重写，避免生成无效的增量更新。

- PDF tools: add a command for rotating a selected range of PDF pages.
  PDF 工具：新增旋转指定 PDF 页面范围的命令。

- Text copy: paragraph-merged copy is on by default (`CmdToggleOcrCopyMerged` in the OCR menu, setting `OcrCopyMerged`). Copying from OCR results or native-text PDF pages now merges soft-wrapped lines into coherent paragraphs while keeping true paragraph breaks, and a long wrapped title copies as a single line so it no longer truncates when pasted as a file name in Windows Explorer. Uncheck the menu item to restore the old line-per-layout behavior
  文本复制默认按段落合并（OCR 菜单「段落合并复制」，`OcrCopyMerged`）：OCR 结果及原生文字 PDF 复制出来的都是连贯段落，不再按版面逐行断开；折行长标题复制为单行，另存文件名时不会再被回车截断。取消勾选可恢复旧的逐行行为

- toolbar: fullscreen button moves to the far right (after Read Aloud)
  工具栏：全屏按钮移到最右侧（朗读后面）

- bookmarks sidebar: empty “No bookmarks / Extract Table of Contents” text no longer flickers when dragging the splitter
  书签栏为空时拖分隔条，「无书签 / 提取目录」不再闪抖

- extract bookmarks: unnumbered `附件` plus `浙江省…“一本账S1”` is the real attachment (including landscape vertical titles); if the form page has no title, reuse the 函末 `附件：…` name; nest `附件` beside `（一）`–`（四）`, not under the last point; 函末 `附件：…` stays a cite
  提取目录：单独一行「附件」和「浙江省…一本账」（横页竖排也能对上）收成附件标题；表页没有标题时用函末「附件：…」的名字；附件和（一）到（四）同级，不挂在最后一点下面；函末「附件：…」仍是引用

- extract bookmarks: page-top wrap of a 公文 paragraph (`财政局、…还应报送`, leftover `请各`) is not a heading; `（一）…年报，请各` keeps only up to 年报
  提取目录：公文换页正文（「财政局、…还应报送」、标题末「请各」）不当标题；（一）只收到「年报」

- extract bookmarks: `附件4` plus the large `…任务分工表` is the attachment title; table row `1 对照全国数据共享清单` and first-column `一、实现人社数` are not (including when sequence-gap salvage tries to revive them)
  提取目录：「附件4」跟大标题「…任务分工表」收成附件名；表里「1 对照全国数据共享清单」、竖列「一、实现人社数」不当标题（补洞 salvage 也不再救回）

- extract bookmarks: split 规章 title (`江西省…国有资产` + `配置管理暂行办法`) is joined; 附件一 with 第N章 nests under it and is not merged into a later form 附件1 (`年月日` / 审批表)
  提取目录：拆开的规章标题（「江西省…国有资产」+「配置管理暂行办法」）会拼回；附件一挂上第N章，不跟后面的表单附件1（年月日/审批表）合并

- extract bookmarks: 函/附件 multi-line titles (`关于征求《…` wrap, `附件1` + `…工作要点` + `（征求意见稿）`) stay one bookmark; lone `（征求意见稿）` is not a child; title years like `2026年工作要点` are not trimmed as 落款 dates
  提取目录：函和附件多行标题（「关于征求《…」换行、「附件1」+「…工作要点」+「（征求意见稿）」）收成一条；单独的「（征求意见稿）」不当子标题；标题里的「2026年工作要点」不再被当成落款日期裁掉

- fix crash: after reload, FreeNotVisible no longer calls PageVisibleNearby on a freed DisplayModel (invisible cache tiles cleared or dm nulled)
  修崩溃：重载后 FreeNotVisible 不再对已释放的 DisplayModel 调 PageVisibleNearby（不可见图块释放或清空 dm）

- extract bookmarks: pages that already have a file text layer, or finished OCR, are not OCR'd again during extract; if accurate stext drops CJK because Song is missing, fall back to plain stext; the printed-TOC unit suite runs only with `-extract-toc-debug` / `-extract-toc-bench`
  提取目录：文件已有文字层、或已经识别过的页，提取时不再重新 OCR；缺宋体时改用不依赖字框的文字层；日常提取不再跑目录单测

- home: header view/sort icons sit closer together and use the same 1px stroke / text color as the toolbar
  首页：列表/宫格和最近/最热两个图标收紧，线宽和颜色跟工具栏一致

- home: list search no longer flashes black when deleting the query (do not resize the canvas scrollbar during paint)
  首页：列表里删搜索字不再整屏闪黑（滚动条改在画完之后再调）

- home: file names use Medium (same band as thumbnail labels), not Semibold
  首页：列表文件名用 Medium，和缩略图标签同一档，不再用 Semibold

- home: pin icon is only slightly larger than the original 1024-viewBox size
  首页：图钉只比原来大一点点

- reopen: last file paints on the first frame after incremental relayout (recalc visible pages before the first paint)
  重开上次文件：增量重排后立刻算出可见页，不再先白屏、滚一下才出内容

- extract bookmarks: RAZ printed TOC keeps each row when the font bbox is 2–3× too tall; printed-page offset 0 is a real calibration
  提取目录：RAZ 小书印刷目录在虚高 bbox 时不再把相邻行粘在一起；页码偏移 0 算有效校准，不再当成没校准丢掉

- OCR: Auto OCR turns on by default only for image-only scanned PDFs; other PDFs and EPUB/MOBI/AZW stay off until you click the toolbar
  OCR：只有几乎没有文字层的扫描 PDF 才默认开自动 OCR；其他 PDF 以及 EPUB/MOBI/AZW 默认关，需手动点工具栏

- bookmarks: next/prev/calibrate/close header icons share one darker gray (not the faint close-X gray)
  书签栏：上一项/下一项/校准/关闭四个图标用同一套更深的灰

- bookmarks: tighter gaps between the next/prev/calibrate header icons; keep a wider gap before the close button
  书签栏：上一项/下一项/校准三个图标收紧，关闭按钮前仍留一点空隙

- home: search field uses the home UI font (slightly larger than menu type) and sits on the font line height so the query is vertically centered in the chrome
  首页：搜索框用主页字号（比菜单字略大），输入高度跟行高走，文字在圆角框里垂直居中

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
