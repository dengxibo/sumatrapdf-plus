# EPUB 大合集性能与稳定性检查点（2026-07-17）

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

- 第一次切换**文档颜色模式**（工具栏：自动 / 原稿 / 跟随主题）或应用主题 CSS 后，连续阅读模式下中间出现大段空白；再切一次颜色或主题后往往正常。
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
- **只切换一次**文档颜色模式（自动 ↔ 跟随主题 ↔ 原稿），确认无大段空白、无需第二次切换。
- 切换应用浅色/深色主题（若会触发 EPUB CSS 重排）同样测一次。
- 渐进加载未完成时切换颜色/主题（若仍允许）不应崩溃或 TOC 错乱。

