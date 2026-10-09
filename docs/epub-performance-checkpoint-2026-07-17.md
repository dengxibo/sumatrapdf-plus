# EPUB 大合集性能与稳定性检查点（2026-07-17）

## 字号调整后的阅读位置恢复（2026-10-07）

### 保留适应模式及连续字号调整的内容锚点（2026-10-08）

字体重载不应使用历史文件设置覆盖当前视图。保存并在新文档首次布局前恢复虚拟缩放模式、显示模式及旋转；保存 Fit Width / Fit Page 的模式值，而非 DPI 换算后的百分比。后台标签页的延迟重载也保留这些数据与原文字锚点。

Fit Single Page 重排后，原文字可能落在新页面中部或底部。下一次字号调整不能重新抓取该页首行，否则反复增减会向前漂移。保存已恢复的文字锚点及视图位置；位置、缩放和显示模式不变时沿用该文字，导航或视图改变后重新捕获。单页模式显示包含原文字的新整页，不能同时强制原行永远处于视口顶部。

独立 Release 窗口界面回归：合成长章节 14 pt 第 20 页、第 189 段，Fit Single Page 放大后第 22 页仍含第 189 段，缩回后准确返回第 20 页；随后翻至第 21 页、第 199 段，字号往返仍返回第 21 页，验证旧锚点不会阻止导航。Fit Width 字号往返保持模式，并将第 199 段恢复至视口顶部。构建 0 警告、0 错误。未验证后台标签页与旋转的界面组合；这些路径已按相同保存/恢复逻辑处理。

字号调整使用异步重新打开文档，并保留原画面直到首行定位完成。原定位从当前章节第一页开始逐页 `GetTextForPage`，加载进度通知又会从头重试；长章节会在重新分页之外再付出大量文字提取成本。

- 新路径在 MuPDF 的章节 HTML flow 中按 Unicode 文本查找，忽略与原首行锚点相同的空白字符，返回目标页与文字位置。保留原有 structured-text 查找作为不支持排版的回退。
- 查找结果在标签页内缓存；新一轮字体重载必须清除结果，保留首行文字，避免快速连续调整字号时使用上一轮分页坐标。
- 找到目标页后优先完成该页所需的 DisplayModel 布局，不等待进度通知逐批推进。视口高度有效后才能执行布局。
- 文档锁暂时不可用时主动安排进度重试，避免设置 UI 等锁标志后后台让出、但 UI 没有新通知而停止恢复。
- 命令行 EPUB 基准要将引擎设为 foreground，否则渐进计数会一直等待标签页激活。

回归入口：`powershell -File cmd/test-epub-reflow-anchor.ps1`。生成含中英文长章节、行内样式及不换行空格的 EPUB，在 14/22 pt 下比较三个位置的直接查找与原逐页查找结果。另用真实大合集运行 `-bench-epub`；性能日志中的 `reflow_anchor` 给出两条路径耗时。这些数字只测定位阶段，不代表文件打开、章节排版及最终渲染的总耗时。

本机 Debug 对照：真实 206 MB / 3432 页合集的第 1716 页定位由 424 ms 降至 1.2 ms。合成长章节在 14 pt 的中部／后部由 3.19／3.28 秒降至 9.05／7.28 ms；22 pt 时由 4.52／6.53 秒降至 7.38／4.77 ms。六个合成样本的定位页码均与原 structured-text 搜索一致。

### 137 册书虫合订本实测

原书约 148 MB、3110 个 spine 章节；14 pt 为 10891 页，22 pt 为 19986 页。14/22 pt 的直接定位与逐页提取结果一致。完整字号切换基准通过 `SUMATRA_EPUB_FONT_BENCH=1` 启用，测量重新打开、找到原首行、核对文字以及渲染完成，避免只测定位算法。

Debug 下，从 14 pt 起依次切换 22、14、20 pt，书籍中部恢复并渲染需 18.36、19.93、17.98 秒；后部需 32.98、34.44、47.24 秒，六次文字核对及页面渲染均成功。主要等待来自到目标章节之前的重新分页，当前定位优化不能消除该成本。

原地复用及固定 CSS 字号参数的试验，后部首次切换仍需 22.36 秒，随后切换需 32.83、36.91 秒。尚未达到交互目标，已撤回试验实现，未接入界面。后续应研究目标章节优先排版及全书页码异步协调，不应通过同步重排冻结界面。上述数字为本机 Debug 诊断，不能作为 Release 用户体验的速度承诺。

2026-10-08 使用当前代码重新编译 Release，按相同入口、独立 14 pt 设置及相同三个字号测试：中部耗时 15.33、12.31、11.78 秒，后部耗时 22.82、24.12、24.16 秒。六次定位文字核对和渲染均通过，目标页码与 Debug 一致。比此前 Debug 样本约缩短 17%–49%，但后部仍等待约 23–24 秒，优化编译不能消除前置章节重新分页的瓶颈。原始指标保存于 `out/bookworm-font-test/font-change-release.jsonl`，该测试测量引擎重新打开、定位及渲染总耗时，不包含工具栏防抖、界面通知及绘制提交时间。

记录时间：2026-07-17 01:17:23 +08:00（北京时间）

这是一次重要的可回退检查点，集中记录大型 EPUB／合集书在渐进加载、目录、文本交互和主题切换方面的优化。后续升级 MuPDF、合并上游 SumatraPDF 或调整渲染缓存时，应优先参考本提交；若出现明显性能或稳定性回退，可回到本提交进行对比。

## 解决的问题

- 大型 EPUB 首次打开时，采用渐进式章节计数和页面增长，让正文与 TOC 尽早可用。
- TOC 尚未完全加载时，未到达的标签显示为灰色；页面可达后主动刷新为正常颜色，不再依赖鼠标悬停或拖动滚动条刷新。
- 移除 EPUB 加速数据和元数据的硬盘旁路缓存依赖，保留运行期内存元数据，避免缓存文件带来的版本一致性问题。
- 避免鼠标移动为了命中文字而同步提取整章结构化文本；真正点击选择时仍允许按需加载，修复部分页面无法划词的问题。
- 解除渐进加载期间对主题和文档色彩模式命令的禁用。
- 主题和色彩模式切换不再按书籍页数或加载耗时选择“重载整本书”的特殊路径；可重排 MuPDF 文档统一使用原地 CSS 更新和可见区渲染。
- 切换时保留旧图块作为过渡，新图块完成后立即替换，减少黑屏和“请稍等，正在渲染”。
- 修复切换后必须滚动鼠标或点击 TOC 才刷新的问题：取消旧任务后主动重新提交可见页，并保证完成回调刷新当前文档。
- 修复连续快速切换时的渲染请求竞态：已取消但尚未退出的同页任务不再错误阻止新主题请求。
- 修复主题重排与 `fz_run_page_contents()` 并发导致的页面对象释放后使用（访问冲突）：可重排页面指针从获取到渲染完成始终受文档锁保护。
- 将主题后的 EPUB 缓存失效从“每个可见页重复清理整章”改为“每章只清理一次并同步该章页面 epoch”，重点改善单章包含数千页的合集书。
- 保持 TOC、链接锚点、文本选择、电子书批注和朗读高亮在重排后的同步与定位。

## 性能诊断支持

- 增加 EPUB 性能事件日志和 `--bench-epub` 基准入口，用于测量打开、滚动、片段链接解析和文本提取。
- 保留渐进加载与性能日志高级设置，便于后续回归对比。
- 仓库中的 EPUB 基准脚本用于与上游行为及本版本结果做对照。

## 后续维护注意事项

- 不要恢复按“大书页数／加载时间”直接重载整本文档的主题切换分支，除非有新的可复现证据。
- 不要仅依赖异步 `CancelRendering()` 就修改或释放 MuPDF 可重排页面；取消标志不代表工作线程已经停止使用页面对象。
- 修改渲染去重时，必须区分正在正常执行的请求与已标记 `abort`、但仍留在当前槽位的请求。
- MuPDF 的 EPUB 布局缓存以章节为单位，失效策略也应以章节为单位，避免合集书重复整章布局。
- 回归测试至少覆盖：超大多章节 EPUB、单一超大章节 EPUB、快速连续切换主题／文档色彩模式、切回文档原生、TOC 渐进加载期间操作、划词和朗读。

## 可重排 EPUB：主题／文档颜色切换后出现大段空白（2026-07-26）

记录时间：2026-07-26（修复萤火虫等大合集 EPUB 在「特洛伊战争背后的真相」与「艺术与文化」等目录段之间切换文档颜色模式后出现空白页，且往往要再切一次才恢复。）

### 现象

- 第一次切换**文档颜色模式**（工具栏：智能 / 原稿 / 匹配主题）或应用主题 CSS 后，连续阅读模式下中间出现大段空白；再切一次颜色或主题后往往正常。
- 调试日志中可见：`DisplayModel` 在引擎重排后 `pos.dy == 0`、`canvasDy` 仅等于视口高度（例如 ~976），而引擎 `PageMediabox` 仍正常。

### 根因（勿再犯）

1. **排版顺序错误（主因）**  
   `EngineMupdfRelayoutForThemeChange` 之后会调用 `RefreshDisplayModelAfterThemeChange` → `Relayout` / `CalcZoomReal`。若此时尚未执行 `MainWindow::UpdateCanvasSize()`（`DisplayModel::SetViewPortSize`），视口宽高为 0，`ZoomRealFromVirtualForPage` 返回 0，连续模式下每页 `pos.dy` 为 0，画布高度不随全书页数增长，滚动位置与内容错位 → 表现为空白带。  
   **文档颜色**走 `UpdateDocumentColors` → `ApplyDocumentColorModeChangeToAllTabs`，**不会**经过 `UpdateAfterThemeChange`；若只在一处补了「视口就绪后再排版」，另一路径仍会复发。

2. **章节页码表重算竞态**  
   主题重算会清空 `reflowChapterStartPage` 并重新 `CountReflowChaptersUpTo`。后台渐进加载线程若在 `reflowCountLock` 释放间隙继续往映射里 append，会导致第一次重算分页与 `LoadReflowPageMediabox` 章节索引不一致。重算期间应持有 `reflowCountLock`，并设 `reflowThemeRecountInProgress` 让其它计数方等待或退让。

3. **重算后分页仍用旧布局计数**  
   仅 `fz_purge_stored_html` 不足以保证 `fz_count_chapter_pages` 立即反映新 CSS。主题重算按章计数前应对该章 `fz_purge_stored_html_chapter` 并 `fz_load_chapter_page(..., 0)` 再计数，否则第一次切换仍可能保留旧 `chapterStarts`（第二次切换时因阅读过程已暖章而「碰巧」正确）。

### 正确做法（实现约定）

| 步骤 | 要求 |
|------|------|
| 引擎 CSS / 分页 | `EngineMupdfRelayoutForThemeChange` → `RecountReflowPageMapForThemeChange`：独占 `reflowCountLock`；按章 purge + warm 后计数；再同步 mediabox。 |
| UI 首次重排 | `RefreshDisplayModelAfterThemeChange`：若 `totalViewPortSize.dy <= 0`，只做 `InvalidateReflowLayoutAfterEngineReparse` + `SyncPageCountWithEngine`，**不要**在此调用完整 `Relayout`。 |
| UI 最终重排 | 在 `RelayoutFrame` + `UpdateCanvasSize` 之后，对当前窗口 reflow MuPDF EPUB 调用 `DisplayModel::RelayoutPreservingAnchorPageAfterViewPortUpdate()`（封装在 `ReflowMupdfRelayoutUiAfterCanvasResize`）。 |
| 两条入口都必须调用 | **`UpdateAfterThemeChange`**（应用主题）与 **`UpdateDocumentColors`**（文档颜色模式，`updateReflowDocuments == true`）在 EPUB 重排后都要走上述「画布尺寸已知后再排版」。 |
| 防御 | `DisplayModel::Relayout` 中若 `GetZoomReal` ≈ 0，可用 `getZoomSafe` 避免页高为 0；不能替代「视口就绪后再排版」。 |

### 相关代码（便于检索）

- `SumatraPDF.cpp`：`ReflowMupdfRelayoutUiAfterCanvasResize`、`RefreshDisplayModelAfterThemeChange`、`UpdateDocumentColors`、`UpdateAfterThemeChange`
- `DisplayModel.cpp`：`RelayoutAfterReflowEngineReparsePreservingScroll`、`RelayoutPreservingAnchorPageAfterViewPortUpdate`
- `EngineMupdf.cpp`：`RecountReflowPageMapForThemeChange`、`CountReflowChaptersUpTo`（`forThemeRecount`）

### 回归检查

- 大合集 EPUB，连续模式，滚到目录段交界（多章分页变化大的区域）。
- **只切换一次**文档颜色模式（智能 ↔ 匹配主题 ↔ 原稿），确认无大段空白、无需第二次切换。
- 切换应用浅色/深色主题（若会触发 EPUB CSS 重排）同样测一次。
- 渐进加载未完成时切换颜色/主题（若仍允许）不应崩溃或 TOC 错乱。

## 调整字号后的阅读位置跳动（2026-10-08）

测试书籍：书虫·牛津英汉双语读物，第 3 级全，19 册合订本。

- 实际复现：适合宽度模式下，正文中途放大字号，会退回前面几个段落。只验证显示模式不变或文字锚点命中，不足以证明阅读位置保持正确。
- 渐进分页刷新改为保留视口滚动位置；仍有待执行的正常打开／跳页请求时，继续走原有恢复流程。
- 文字锚点放置前清除旧的待恢复滚动请求，并排好目标页后面的页面，避免被临时画布末尾截短页内滚动量。
- HTML flow 的位置在中文换行／分页时可能早于实际字形；使用目标页及相邻两页文字提取结果校准坐标，跨新分页边界核对完整锚点，不扫描前面的整本书。
- 捕获起点改为实际可见的第一个字形，并与页面媒体框及视口相交，排除空白间隙中已经被裁掉的文字；锚点后续字符也过滤页面外的字形，通常在本页内截取，章节编号取实际捕获字形所在页。

验证状态：

- 中间修订版在英文第一章反复放大、缩小保持同一句；中文及页末复测暴露字形坐标、裁切和跨页锚点问题，随后修正。
- 最终 x64 Release 构建成功，零警告、零错误：`out/bookworm19-final-release-build-retry.log`。默认高并发构建曾遇到头文件读取错误，降低至 `/m:2`、`CL_MPCount=4` 后完成。
- 发布 EXE：`out/rel64/SumatraPDF-Plus.exe`，版本 3.7.40，SHA-256 `92BA96F3908F839A474029130FB723E41550908E007118B433ABE34310873A53`。
- 使用最终发布 EXE 执行 `cmd/test-epub-font-position.ps1 -Book <19册书虫路径> -Exe out/rel64/SumatraPDF-Plus.exe`：六个阅读位置，36 次字号变化，72 项检查全部通过。覆盖英文、中文、书籍中部／后部、Fit Width、Fit Single Page，以及主动翻页后的新锚点。
- 每次分别在文字恢复后及后台分页完全结束后核对锚点字形的实际视口坐标、文字缓存、缩放和显示模式。连续模式检查原文字位于顶部；单页适应检查完整页面中仍能看到原文字。原始指标：`out/perf/epub-30231500.jsonl`；表格：`out/epub-font-position-test/positions.csv`。
- 现有 Release 单元测试全部通过（102694 项）：`out/bookworm19-final-unit-tests.log`。
- 最终回归直接运行真实引擎、DisplayModel 和生产文字恢复函数，不创建阅读窗口。测试中 Windows 自动锁屏，因此最终版本没有进行解锁后的鼠标／键盘界面复测；不能把这些模型检查描述为最终 GUI 自动化通过。后台标签页及旋转组合仍未做最终界面回归。

## 字号调整进度条与视口绘制同步（2026-10-08）

原通知只覆盖文件解压／创建引擎，在 `EarlyEngineDisplayUI` 或 `LoadDocumentAsyncFinish` 中立即到 100% 并移除；对于大合集，之后仍要等待目标章节分页、恢复文字锚点及渲染，因而用户看到进度条已经结束但字号尚未改变。

- 字体异步重载通知的生命周期移交给目标标签页；引擎交付和加载线程退出不再提前移除它。
- 解压映射到 10%–15%；按已完成章节与原阅读章节的比例推进至 85%，使用原子章节计数，不锁住引擎获取进度。
- 文字锚点恢复至 90%；等待渲染时为 95%；进度只增不退，同一数值不重复重绘通知。
- Canvas 只有在可见页面的缓存绘制全部完成、无旧图替代及渲染错误，且完整视口已刷新后，才把该次字体调整推进至 100% 并移除通知。局部绘制会补一次完整重绘。
- 加载失败、渲染失败、换书及关闭标签页均清理相应通知；新一轮字体调整替换通知时不会误删其它标签页的进度。

验证：最终独立 x64 Release 构建零警告、零错误（`out/epub-font-progress-release-build-final.log`）。使用 Windows computer-use 在独立设置目录中打开 137 册书虫合集，定位 14 pt 第 9000 页（Lucy Steele's secret），执行字号 14→16→14 两次真实界面操作。第一次等待约 13／27 秒的截图仍显示原画面和推进中的通知；之后新字号与同一章节正文显示，通知消失。第二次等待约 21 秒仍显示进度，结束后返回原第 9000 页及相同文字。

两次进度日志各有 48 个递增更新，均通过单调性、分页 85%、文字定位 90%、视口绘制 100% 的检查，无持有旧画面时提前达到 100%。原始日志：`out/font-progress-ui-test/run.log`；进度表：`out/font-progress-ui-test/progress.csv`。测试窗口已经关闭，用户原有窗口未改动。

测试文件已复制至正常输出 `out/rel64/SumatraPDF-Plus.exe`；本地文件版本仍为 3.7.40，SHA-256 `2B732177F35FE144FD95C182B0DB683625F138AA792E5B3E2F077D15209C48D2`。按用户要求，此进度条修订已于 2026-10-08 覆盖 GitHub 3.7.40 的 SumatraPDF-Plus.exe；线上 SHA-256 与本地一致，公开下载返回 HTTP 200。

## 大合集首次主题切换停顿（2026-10-08）

纯颜色切换保留 DisplayModel 布局、引擎页码映射及 MuPDF accelerator 的章节页数。否则初次换色后的链接解析可能同步重算前面所有章节。UpdateCanvasSize 比较完整视口尺寸，避免把扣除滚动条后的内部尺寸误判为窗口变化。字体、尺寸和其它影响分页的变更继续走完整布局流程。

CSS 更新使用章节样式世代：首次访问时替换旧 HTML，避免 UI 线程销毁整本文档缓存；单章缓存删除使用已有 hash key，避免扫描共享 store。增加 SUMATRA_EPUB_THEME_BENCH 引擎回归入口和 ThemeChange 整体 UI 耗时日志。

验证：最终构建零警告、零错误（out/toc-regression/epub-theme-final-build.log）。独立设置打开 137 册书虫，等待 19,355 页全部加载，跳到第 1564 页，首次点击月亮后正文及界面均正常显示暗色，页码、侧栏保持；首次 ThemeChange 152.21 ms，切回浅色 132.03 ms。渲染日志中相邻页最高 147.47 ms，没有出现中间版本曾记录的 27 秒渲染等待。原始 GUI 日志：out/theme-bench/gui-final.log。该计时是同步 UI 处理时间，不包含截图工具耗时，也不等同于完整渲染耗时；此次没有复现用户原版本精确的十余秒基线。

测试程序：out/toc-fix/SumatraPDF-Plus-epub-theme-final.exe。此前测试工具 102,578 项通过，git diff --check 通过。

## 保留章节排版的颜色更新试验（2026-10-08）

在上一轮基础上，EPUB 主题切换保留已有 HTML 排版树。临时解析新样式树，先校验盒树结构和全部非颜色样式一致，再只复制文字、背景、文字填充/描边和四边框颜色；原来的字形坐标、页运行区间、布局尺寸和页边距保持。每个盒的可写样式只分配一次，避免反复切换累积分配。非颜色样式不同或发生字体/尺寸变更时退回旧的完整排版路径。当前仍有临时 HTML/CSS 解析开销，不能称为完全在绘制层换色。

进一步确认 fz_style_document 仅标记待更新，不立即执行 epub_style。palette 保留 API 现在先执行待应用的样式回调，再同步 accelerator checksum 和布局状态，避免下一次访问页面才使分页缓存失效。移除了 EPUB 暗色 CSS 残留的 text-decoration:none，主题严格不改变链接下划线。

真正的匹配主题配置回归：out/theme-bench/palette-retain.log。19,355 页第 1564 页，四次 Dark-Black/Light-Warm 切换全部 same_layout=true、passed=true；逐字文字内容和字形矩形完全一致，日志为 retained chapter 347 layout（同一警告重复四次），无 rebuilt chapter。颜色更新 0.42–0.63 ms，单页引擎渲染 11.10–16.31 ms；这些是引擎测试数据，不是完整窗口计时。早期 baseline-settings 被发现使用 original 颜色模式，因此该设置目录的测试不能作为 CSS 换色性能证据。

使用 computer-use 在真实窗口复测：等待完整 19,355 页后定位第 1564 页，暗→亮→暗，正文、链接、背景实际变色，下划线保留，页码及侧栏不变；UI ThemeChange 分别 142.52 / 141.66 ms。日志 out/theme-bench/gui-palette-retain.log。测试窗口已关闭。

程序 out/toc-fix/SumatraPDF-Plus-palette-retain.exe，C++ 编译和链接零警告零错误（out/toc-regression/epub-palette-deferred-build.log）；build.ts 最后的 OCR DLL 复制被正在使用该输出目录的程序锁住，未替换其 DLL，已生成的 EXE 可正常运行并完成上述验证。未提交。
