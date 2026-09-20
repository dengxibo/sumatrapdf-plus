import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

// Add placeholder translations for Read Aloud dialog strings.
// For cn/tw we provide real Chinese translations; for all other languages we
// fall back to the English source string so that every supported language stays
// in the "good" subset and the UI at least shows English instead of nothing.

const rootDir = import.meta.dir + "/..";
const goodPath = join(rootDir, "translations", "translations-good.txt");

// Source string -> { cn, tw }
const stringsToAdd: Record<string, { cn: string; tw: string }> = {
  General: { cn: "常规", tw: "一般" },
  Interface: { cn: "界面", tw: "介面" },
  Reading: { cn: "阅读", tw: "閱讀" },
  Advanced: { cn: "高级", tw: "進階" },
  "SumatraPDF Options": { cn: "SumatraPDF 选项", tw: "SumatraPDF 選項" },
  "Default &Layout:": { cn: "默认布局(&L)：", tw: "預設版面配置(&L)：" },
  "Default &Zoom:": { cn: "默认缩放(&Z)：", tw: "預設縮放(&Z)：" },
  "Show the &bookmarks sidebar when available": {
    cn: "可用时显示书签侧栏(&B)",
    tw: "可用時顯示書籤側欄(&B)",
  },
  "&Remember settings for each document": { cn: "记住每个文档的设置(&R)", tw: "記住每份文件的設定(&R)" },
  "Use &tabs (requires restart)": { cn: "使用标签页（需要重启）(&T)", tw: "使用分頁（需要重新啟動）(&T)" },
  "Remember &opened files": { cn: "记录打开过的文件(&O)", tw: "記錄開啟過的文件(&O)" },
  "Restore the last &session at startup": { cn: "启动时恢复上次会话(&S)", tw: "啟動時還原上次工作階段(&S)" },
  "Open new files in the existing &instance": { cn: "在现有进程中打开新文件(&I)", tw: "在現有程序中開啟新文件(&I)" },
  "Keep a &Home tab (requires restart)": { cn: "保留主页标签（需要重启）(&H)", tw: "保留首頁分頁（需要重新啟動）(&H)" },
  "Show the &menu bar with tabs": { cn: "标签模式下显示菜单栏(&M)", tw: "分頁模式下顯示選單列(&M)" },
  "Show the tool&bar": { cn: "显示工具栏(&B)", tw: "顯示工具列(&B)" },
  "Show quick &annotation buttons": { cn: "显示标注快捷按钮(&A)", tw: "顯示註解快捷按鈕(&A)" },
  "Ctrl+Tab uses most recently used &order": { cn: "Ctrl+Tab 按最近使用顺序切换(&O)", tw: "Ctrl+Tab 依最近使用順序切換(&O)" },
  "Use the &floating search window": { cn: "使用浮动搜索窗口(&F)", tw: "使用浮動搜尋視窗(&F)" },
  "&Scrollbars:": { cn: "滚动条(&S)：", tw: "捲動軸(&S)：" },
  "Use s&mooth scrolling": { cn: "使用平滑滚动(&M)", tw: "使用平滑捲動(&M)" },
  "Show a scrollbar in single-&page mode": { cn: "单页模式显示滚动条(&P)", tw: "單頁模式顯示捲動軸(&P)" },
  "Automatically &reload changed documents": { cn: "文件变化后自动重新加载(&R)", tw: "文件變更後自動重新載入(&R)" },
  "Prevent sleep in &fullscreen or presentation mode": { cn: "全屏或演示模式下防止休眠(&F)", tw: "全螢幕或簡報模式下防止休眠(&F)" },
  "Some changes require restarting the application.": { cn: "某些更改需要重新启动应用程序。", tw: "某些變更需要重新啟動應用程式。" },
  "Open &Advanced Options File...": { cn: "打开高级选项文件(&A)...", tw: "開啟進階選項檔案(&A)..." },
  "Windows scrollbars": { cn: "Windows 滚动条", tw: "Windows 捲動軸" },
  "Smart auto-hide scrollbars": { cn: "智能自动隐藏滚动条", tw: "智慧型自動隱藏捲動軸" },
  "Always-visible overlay scrollbars": { cn: "始终显示的叠加滚动条", tw: "永遠顯示的浮動捲動軸" },
  "No scrollbars": { cn: "不显示滚动条", tw: "不顯示捲動軸" },
  "OCR and AI": { cn: "OCR 与 AI", tw: "OCR 與 AI" },
  Updates: { cn: "更新", tw: "更新" },
  "Startup and session": { cn: "启动和会话", tw: "啟動和工作階段" },
  "File changes": { cn: "文件变化", tw: "文件變更" },
  "&Lazy-load inactive tabs": { cn: "延迟加载未激活的标签页(&L)", tw: "延遲載入未啟用的分頁(&L)" },
  Appearance: { cn: "外观", tw: "外觀" },
  "&Theme:": { cn: "主题(&T)：", tw: "主題(&T)：" },
  "Default document &colors:": { cn: "默认文档颜色(&C)：", tw: "預設文件色彩(&C)：" },
  "Keep original document colors": { cn: "保留原稿颜色", tw: "保留原稿色彩" },
  "Match the current theme": { cn: "跟随当前主题", tw: "跟隨目前主題" },
  "Display &Filter...": { cn: "显示滤镜(&F)...", tw: "顯示濾鏡(&F)..." },
  "Display filter": { cn: "显示滤镜", tw: "顯示濾鏡" },
  "Enhance Display": { cn: "增强显示", tw: "增強顯示" },
  "Enable Enhance Display": { cn: "开启增强显示", tw: "開啟增強顯示" },
  "Enhance Display is enabled": { cn: "增强显示已开启", tw: "增強顯示已開啟" },
  "Deskew Page": { cn: "倾斜校正当前页", tw: "傾斜校正目前頁" },
  "Deskew &Page": { cn: "倾斜校正当前页(&P)", tw: "傾斜校正目前頁(&P)" },
  "Deskew All Scanned Pages": { cn: "倾斜校正全部扫描页", tw: "傾斜校正全部掃描頁" },
  "Deskew &All Scanned Pages": { cn: "倾斜校正全部扫描页(&A)", tw: "傾斜校正全部掃描頁(&A)" },
  "Deskew during OCR": { cn: "识别时自动校正倾斜", tw: "識別時自動校正傾斜" },
  "Toggle Deskew During OCR": { cn: "开关识别时自动校正倾斜", tw: "開關識別時自動校正傾斜" },
  "No scanned pages needed deskewing.": { cn: "没有需要倾斜校正的扫描页。", tw: "沒有需要傾斜校正的掃描頁。" },
  "Deskewed %d page(s).": { cn: "已倾斜校正 %d 页。", tw: "已傾斜校正 %d 頁。" },
  "Deskewing…": { cn: "正在倾斜校正…", tw: "正在傾斜校正…" },
  "Deskewing… %d / %d": { cn: "正在倾斜校正… %d / %d", tw: "正在傾斜校正… %d / %d" },
  "Deskew is already running.": { cn: "倾斜校正已在进行。", tw: "傾斜校正已在進行。" },
  "This page is not skewed.": { cn: "这一页没有需要校正的倾斜。", tw: "這一頁沒有需要校正的傾斜。" },
  "Deskewed by %.1f degrees.": { cn: "已倾斜校正 %.1f 度。", tw: "已傾斜校正 %.1f 度。" },
  Sharpness: { cn: "锐度", tw: "銳度" },
  Sharpness: { cn: "锐度", tw: "銳度" },
  Brightness: { cn: "亮度", tw: "亮度" },
  Contrast: { cn: "对比度", tw: "對比度" },
  Reading: { cn: "阅读优化", tw: "閱讀優化" },
  Scan: { cn: "扫描件", tw: "掃描件" },
  Reset: { cn: "重置", tw: "重設" },
  Scrollbars: { cn: "滚动条", tw: "捲動軸" },
  "Contents / favorites sidebar": { cn: "目录 / 收藏侧边栏", tw: "目錄 / 我的最愛側欄" },
  "Tree &font:": { cn: "目录字体(&F)：", tw: "目錄字型(&F)：" },
  "&Size:": { cn: "字号(&S)：", tw: "字型大小(&S)：" },
  "Tab fo&nt size:": { cn: "标签字号(&N)：", tw: "分頁字型大小(&N)：" },
  "Tab bar &height:": { cn: "标签栏高度(&H)：", tw: "分頁列高度(&H)：" },
  "Tab font size must be 0 (automatic) or between 6 and 72.": {
    cn: "标签字号必须为 0（自动）或 6 到 72。",
    tw: "分頁字型大小必須為 0（自動）或 6 到 72。",
  },
  "Tab bar height must be 0 (automatic) or between 16 and 128.": {
    cn: "标签栏高度必须为 0（自动）或 16 到 128。",
    tw: "分頁列高度必須為 0（自動）或 16 到 128。",
  },
  "&Wrap long titles": { cn: "长标题自动换行(&W)", tw: "長標題自動換行(&W)" },
  "Default view": { cn: "默认打开方式", tw: "預設開啟方式" },
  Scrolling: { cn: "滚动", tw: "捲動" },
  "Fast page scrolling over the scroll&bar": { cn: "鼠标位于滚动条区域时快速翻页(&B)", tw: "滑鼠位於捲動軸區域時快速翻頁(&B)" },
  "Display quality": { cn: "显示质量", tw: "顯示品質" },
  "Engineering drawing &enhancement:": { cn: "工程图纸增强(&E)：", tw: "工程圖紙增強(&E)：" },
  Off: { cn: "关闭", tw: "關閉" },
  On: { cn: "开启", tw: "開啟" },
  "Enable PDF &anti-aliasing": { cn: "启用 PDF 抗锯齿(&A)", tw: "啟用 PDF 反鋸齒(&A)" },
  Dictionary: { cn: "词典", tw: "詞典" },
  "Look up words on &double-click": { cn: "双击单词时查词(&D)", tw: "按兩下單字時查詞(&D)" },
  "Offline dictionary:": { cn: "离线词典：", tw: "離線詞典：" },
  "&Browse...": { cn: "浏览(&B)...", tw: "瀏覽(&B)..." },
  "Choose the offline dictionary folder": { cn: "选择离线词典文件夹", tw: "選擇離線詞典資料夾" },
  Fullscreen: { cn: "全屏", tw: "全螢幕" },
  OCR: { cn: "OCR", tw: "OCR" },
  "Automatically OCR scanned pages": { cn: "自动识别没有文字层的扫描页面", tw: "自動辨識沒有文字層的掃描頁面" },
  "Recognized text can be selected, copied, searched, and read aloud.": { cn: "识别后可直接选择、复制、搜索和朗读扫描页文字。", tw: "辨識後可直接選取、複製、搜尋和朗讀掃描頁文字。" },
  "Full-document OCR &mode:": { cn: "全文 OCR 模式(&M)：", tw: "全文 OCR 模式(&M)：" },
  Fast: { cn: "极速", tw: "極速" },
  "High accuracy": { cn: "高精度", tw: "高精確度" },
  "Automatically &save PDF after OCR or TOC processing": { cn: "OCR 或目录处理完成后自动保存 PDF(&S)", tw: "OCR 或目錄處理完成後自動儲存 PDF(&S)" },
  "Processing results may overwrite the current PDF file.": { cn: "开启后处理结果可能覆盖当前 PDF 文件。", tw: "開啟後處理結果可能覆寫目前 PDF 文件。" },
  "Smart contents": { cn: "智能目录", tw: "智慧目錄" },
  "Extraction &detail:": { cn: "目录提取详细程度(&D)：", tw: "目錄擷取詳細程度(&D)：" },
  Conservative: { cn: "保守", tw: "保守" },
  "Standard (recommended)": { cn: "标准（推荐）", tw: "標準（建議）" },
  Detailed: { cn: "详细", tw: "詳細" },
  "Balanced accuracy and completeness is recommended.": { cn: "标准模式可平衡准确率与完整度，推荐使用。", tw: "標準模式可平衡準確率與完整度，建議使用。" },
  "Ask AI": { cn: "Ask AI", tw: "Ask AI" },
  "&Enable Ask AI": { cn: "启用 Ask AI(&E)", tw: "啟用 Ask AI(&E)" },
  "AI &service:": { cn: "AI 服务(&S)：", tw: "AI 服務(&S)：" },
  Window: { cn: "窗口", tw: "視窗" },
  "E&xit the application with Esc": { cn: "按 Esc 退出程序(&X)", tw: "按 Esc 結束程式(&X)" },
  "Show the full file &path in the title bar": { cn: "标题栏显示完整文件路径(&P)", tw: "標題列顯示完整檔案路徑(&P)" },
  Display: { cn: "显示", tw: "顯示" },
  "Custom &DPI (0 = automatic):": { cn: "自定义 DPI（0 = 自动）(&D)：", tw: "自訂 DPI（0 = 自動）(&D)：" },
  PDF: { cn: "PDF", tw: "PDF" },
  "Show &link borders": { cn: "显示链接边框(&L)", tw: "顯示連結邊框(&L)" },
  "More expert settings": { cn: "更多专家级设置", tw: "更多專家級設定" },
  "Tree font size must be 0 (automatic) or between 6 and 72.": { cn: "目录字号必须为 0（自动）或 6 到 72。", tw: "目錄字型大小必須為 0（自動）或 6 到 72。" },
  "Custom DPI must be 0 (automatic) or between 72 and 600.": { cn: "自定义 DPI 必须为 0（自动）或 72 到 600。", tw: "自訂 DPI 必須為 0（自動）或 72 到 600。" },
  "Invalid value": { cn: "无效值", tw: "無效值" },
  "These changes require restarting the application. Restart now?": { cn: "这些更改需要重新启动应用程序。现在重启吗？", tw: "這些變更需要重新啟動應用程式。現在重新啟動嗎？" },
  "Restart required": { cn: "需要重新启动", tw: "需要重新啟動" },
  Doubao: { cn: "豆包", tw: "豆包" },
  DeepSeek: { cn: "DeepSeek", tw: "DeepSeek" },
  ChatGPT: { cn: "ChatGPT", tw: "ChatGPT" },
  "Delete %d TOC items?": { cn: "删除这 %d 条目录项？", tw: "刪除這 %d 條目錄項？" },
  "Delete %d TOC items and their child items?": {
    cn: "删除这 %d 条目录项及其所有子项？",
    tw: "刪除這 %d 條目錄項及其所有子項？",
  },
  "Match Case": { cn: "匹配大小写", tw: "符合大小寫" },
  "Match Whole Word": { cn: "全词匹配", tw: "全字匹配" },
  "Read aloud speed": { cn: "朗读速度", tw: "朗讀速度" },
  "English voice:": { cn: "英文语音：", tw: "英文語音：" },
  "Chinese voice:": { cn: "中文语音：", tw: "中文語音：" },
  "Reset to 1.00x": { cn: "重置为 1.00x", tw: "重設為 1.00x" },
  "0.25x - 2.00x; buttons change by 0.05x": {
    cn: "0.25x - 2.00x；按钮每次调整 0.05x",
    tw: "0.25x - 2.00x；按鈕每次調整 0.05x",
  },
  "Enter a speed from 0.25x to 2.00x.": {
    cn: "请输入 0.25x 到 2.00x 之间的速度。",
    tw: "請輸入 0.25x 到 2.00x 之間的速度。",
  },
  "Local smart bilingual settings": {
    cn: "本地智能双语设置",
    tw: "本地智慧雙語設定",
  },
  "Local smart bilingual settings...": {
    cn: "本地智能双语设置...",
    tw: "本地智慧雙語設定...",
  },
  "Online smart bilingual settings": {
    cn: "在线智能双语设置",
    tw: "線上智慧雙語設定",
  },
  "Online smart bilingual settings...": {
    cn: "在线智能双语设置...",
    tw: "線上智慧雙語設定...",
  },
  "No text available to read aloud": {
    cn: "没有可朗读的文本",
    tw: "沒有可朗讀的文字",
  },
  "Custom...": {
    cn: "自定义...",
    tw: "自訂...",
  },
  "Applying theme or document colors, re-pagination in progress…": {
    cn: "正在应用主题或文档颜色，全书重新分页…",
    tw: "正在套用主題或文件顏色，全書重新分頁…",
  },
  "Applying theme or document colors… chapter %d / %d": {
    cn: "正在应用主题或文档颜色…第 %d / %d 章",
    tw: "正在套用主題或文件顏色…第 %d / %d 章",
  },
  "Applying document colors, re-rendering pages…": {
    cn: "正在应用文档颜色，重新渲染页面…",
    tw: "正在套用文件顏色，重新渲染頁面…",
  },
  "Adjusting font size, reformatting pages…": {
    cn: "正在调整字号，重新排版…",
    tw: "正在調整字級，重新排版…",
  },
  "Applying ebook font, reloading…": {
    cn: "正在应用字体，重新加载…",
    tw: "正在套用字體，重新載入…",
  },
  "Applying ebook font, reformatting pages…": {
    cn: "正在应用字体，重新排版…",
    tw: "正在套用字體，重新排版…",
  },
  "Adjusting ebook layout… chapter %d / %d": {
    cn: "正在调整电子书版式…第 %d / %d 章",
    tw: "正在調整電子書版式…第 %d / %d 章",
  },
  "No bookmarks": { cn: "暂无书签", tw: "暫無書籤" },
  "Convert to &PDF": { cn: "转换为 PDF(&P)", tw: "轉換為 PDF(&P)" },
  "Convert Image to PDF": { cn: "将图像转换为 PDF", tw: "將圖像轉換為 PDF" },

  "Auto OCR": { cn: "自动 OCR", tw: "自動 OCR" },
  "Enable Auto OCR": { cn: "开启自动 OCR", tw: "開啟自動 OCR" },
  "Auto OCR is enabled": { cn: "自动 OCR 已开启", tw: "自動 OCR 已開啟" },
  "OCR region": { cn: "框选识别", tw: "框選識別" },
  "OCR Region": { cn: "框选识别", tw: "框選識別" },
  "OCR Current Page": { cn: "识别当前页", tw: "識別目前頁" },
  "Recognize Current Page": { cn: "识别当前页", tw: "識別目前頁" },
  "OCR All Pages": { cn: "识别所有扫描页", tw: "識別所有掃描頁" },
  "Recognize All Scanned Pages": { cn: "识别所有扫描页", tw: "識別所有掃描頁" },
  "Recognize All Scanned Pages (Fast)": { cn: "识别所有扫描页（快速）", tw: "識別所有掃描頁（快速）" },
  "Recognize All Scanned Pages (Accurate)": { cn: "识别所有扫描页（精确）", tw: "識別所有掃描頁（精確）" },
  "Auto-save": { cn: "自动保存", tw: "自動儲存" },
  "Toggle Auto-save After OCR": { cn: "开关识别后自动保存", tw: "開關識別後自動儲存" },
  "Toggle Auto OCR": { cn: "开关自动 OCR", tw: "開關自動 OCR" },
  "Cancel OCR": { cn: "取消 OCR", tw: "取消 OCR" },
  "Recognize all pages and save": { cn: "另存为可搜索 PDF...", tw: "另存為可搜尋 PDF..." },
  "Save as Searchable PDF...": { cn: "另存为可搜索 PDF...", tw: "另存為可搜尋 PDF..." },
  "Full-document OCR Mode": { cn: "全文识别模式", tw: "全文識別模式" },
  "Full-document OCR Mode: Fast": { cn: "全文识别模式：极速", tw: "全文識別模式：極速" },
  "Full-document OCR Mode: High Accuracy": { cn: "全文识别模式：高精度", tw: "全文識別模式：高精度" },
  "Fast (Recommended)": { cn: "极速（推荐）", tw: "極速（推薦）" },
  "High Accuracy": { cn: "高精度", tw: "高精度" },
  "No scanned pages to recognize.": { cn: "没有需要识别的扫描页", tw: "沒有需要識別的掃描頁" },
  "Re-recognize All Pages": { cn: "重新识别所有页", tw: "重新識別所有頁" },
  "This PDF already has searchable text. Replace it with newly recognized text?": {
    cn: "这个 PDF 已有可搜索文字。要用新识别的文字覆盖吗？",
    tw: "這個 PDF 已有可搜尋文字。要用新識別的文字覆蓋嗎？",
  },
  "Saving this PDF will change it after it was digitally signed. The existing signature will remain, but viewers will report that the document was modified. Continue?":
    {
      cn: "保存会修改这份已数字签名的 PDF。原签名仍在，但阅读器会提示文件已被更改。要继续吗？",
      tw: "儲存會修改這份已數位簽署的 PDF。原簽章仍在，但閱讀器會提示檔案已被更改。要繼續嗎？",
    },
  "Could not replace the original PDF. The recognized file was kept as a temporary copy.": {
    cn: "无法覆盖原 PDF。已识别的文件保留为临时副本。",
    tw: "無法覆蓋原 PDF。已識別的檔案保留為暫存副本。",
  },
  "Save as searchable PDF...": { cn: "保存为可搜索 PDF...", tw: "儲存為可搜尋 PDF..." },
  "Save as searchable PDF": { cn: "保存为可搜索 PDF", tw: "儲存為可搜尋 PDF" },
  "Save as Searchable PDF": { cn: "保存为可搜索 PDF", tw: "儲存為可搜尋 PDF" },
  "Save as searchable PDF is only available for PDF files.": {
    cn: "仅 PDF 文件可保存为可搜索 PDF。",
    tw: "僅 PDF 檔可儲存為可搜尋 PDF。",
  },
  "Text Annotation (Ctrl+click to lock)": {
    cn: "文本批注（Ctrl+点击锁定）",
    tw: "文字註解（Ctrl+點選鎖定）",
  },
  "Rectangle Annotation (Ctrl+click to lock)": {
    cn: "矩形批注（Ctrl+点击锁定）",
    tw: "矩形註解（Ctrl+點選鎖定）",
  },
  "Circle Annotation (Ctrl+click to lock)": {
    cn: "圆形批注（Ctrl+点击锁定）",
    tw: "圓形註解（Ctrl+點選鎖定）",
  },
  "Line Annotation (Ctrl+click to lock)": {
    cn: "直线批注（Ctrl+点击锁定）",
    tw: "直線註解（Ctrl+點選鎖定）",
  },
  "Ink Annotation (Ctrl+click to lock)": {
    cn: "墨迹批注（Ctrl+点击锁定）",
    tw: "墨跡註解（Ctrl+點選鎖定）",
  },
  "OCR models not found.\\nPut onnxruntime.dll, det.onnx, rec.onnx and keys.txt in:\\n%s": {
    cn: "未找到 OCR 模型。\\n请将 onnxruntime.dll、det.onnx、rec.onnx 和 keys.txt 放到：\\n%s",
    tw: "找不到 OCR 模型。\\n請將 onnxruntime.dll、det.onnx、rec.onnx 和 keys.txt 放到：\\n%s",
  },
  "Scanning…": { cn: "正在识别…", tw: "正在識別…" },
  "Scanning… %d / %d": { cn: "正在识别… %d / %d", tw: "正在識別… %d / %d" },
  "Saved.": { cn: "已保存。", tw: "已儲存。" },
  "Copied.": { cn: "已复制。", tw: "已複製。" },
  "Cancelled.": { cn: "已取消。", tw: "已取消。" },
  "OCR is already running. Please wait until it finishes.": {
    cn: "已有识别任务正在进行，请等待完成后再试。",
    tw: "已有識別任務正在進行，請等待完成後再試。",
  },
  "Ready to search": { cn: "可以搜索了", tw: "可以搜尋了" },
  "Could not save searchable PDF.": { cn: "无法保存可搜索 PDF。", tw: "無法儲存可搜尋 PDF。" },
  "Could not recognize text on this page.": { cn: "无法识别此页文字。", tw: "無法識別此頁文字。" },
  "OCR is only available for PDF and similar documents.": {
    cn: "OCR 仅适用于 PDF 及同类文档。",
    tw: "OCR 僅適用於 PDF 及同類文件。",
  },
  "OCR is not available for this document type.": {
    cn: "此文档类型不支持 OCR。",
    tw: "此文件類型不支援 OCR。",
  },
  "No page to recognize.": { cn: "没有可识别的页面。", tw: "沒有可識別的頁面。" },
  "Selection too small.": { cn: "选区太小。", tw: "選取範圍太小。" },
  "No OCR text to save. Recognize pages first.": {
    cn: "没有可保存的 OCR 文字。请先识别页面。",
    tw: "沒有可儲存的 OCR 文字。請先識別頁面。",
  },
  "Extract Table of Contents": { cn: "提取目录书签", tw: "提取目錄書籤" },
  "Extract Table of Contents Locally": { cn: "提取目录书签", tw: "擷取目錄書籤" },
  "AI Recognize Table of Contents": { cn: "AI提取目录书签", tw: "AI擷取目錄書籤" },
  "Extracting bookmarks… %d / %d": { cn: "正在提取书签… %d / %d", tw: "正在提取書籤… %d / %d" },
  "Replace the existing PDF bookmarks with extracted headings?": {
    cn: "用提取的标题替换现有 PDF 书签？",
    tw: "用提取的標題取代現有 PDF 書籤？",
  },
  "This file is read-only. Bookmark extraction was not started.": {
    cn: "此文件为只读，未开始提取书签。",
    tw: "此檔案為唯讀，未開始提取書籤。",
  },
  "Extract for review": { cn: "提取后预览", tw: "擷取後預覽" },
  "Extract and overwrite file": { cn: "提取并覆盖文件", tw: "擷取並覆寫檔案" },
  "Extracted bookmarks will replace the current bookmarks for review. Choose 'Extract and overwrite file' only if you want to save those changes to the original file. You can cancel review to restore the original bookmarks.": {
    cn: "提取的书签将先替换当前书签以供预览。只有选择“提取并覆盖文件”才会将更改保存到原文件。取消预览可恢复原书签。",
    tw: "擷取的書籤會先取代目前書籤供預覽。只有選擇「擷取並覆寫檔案」才會將變更儲存到原始檔案。取消預覽可還原原書籤。",
  },
  "Could not prepare the extracted table of contents.": {
    cn: "无法准备提取的目录书签。",
    tw: "無法準備擷取的目錄書籤。",
  },
  "Keep for this session": { cn: "保留到本次会话", tw: "保留至本次工作階段" },
  "Discard": { cn: "丢弃", tw: "捨棄" },
  "Bookmark extraction cancelled.": { cn: "已取消提取书签。", tw: "已取消提取書籤。" },
  "This document has too little text to extract bookmarks. OCR scanned pages first.": {
    cn: "文档文字太少，无法提取书签。请先对扫描页做 OCR。",
    tw: "文件文字太少，無法提取書籤。請先對掃描頁做 OCR。",
  },
  "No headings found. OCR scanned pages first, then try again.": {
    cn: "没有找到标题。请先识别扫描页，然后再试。",
    tw: "找不到標題。請先識別掃描頁，然後再試。",
  },
  "Could not write the PDF table of contents.": {
    cn: "无法写入 PDF 目录。",
    tw: "無法寫入 PDF 目錄。",
  },
  "Extracted %d bookmarks.": { cn: "已提取 %d 条书签。", tw: "已提取 %d 條書籤。" },
  "No printed table of contents found in the first pages.": {
    cn: "前几页没有找到印刷目录。",
    tw: "前幾頁沒有找到印刷目錄。",
  },
  "Text was found, but this file has no table of contents or chapter headings to extract.": {
    cn: "文字已经识别。没有找到印刷目录或章节标题，无法提取书签。",
    tw: "文字已經識別。沒有找到印刷目錄或章節標題，無法提取書籤。",
  },
  "This document looks like a scan.": {
    cn: "这份文档看起来是扫描件。",
    tw: "這份文件看起來是掃描件。",
  },
  "There is too little text to extract bookmarks. OCR all pages and save a searchable PDF?": {
    cn: "文字太少，无法提取书签。是否对全部页面做 OCR，并保存为可全文搜索的 PDF？",
    tw: "文字太少，無法提取書籤。是否對全部頁面做 OCR，並儲存為可全文搜尋的 PDF？",
  },
  "Not now": { cn: "暂不", tw: "暫不" },
  "Don't save": { cn: "不保存", tw: "不儲存" },
  "This file has no text layer. Recognize all pages to extract bookmarks. Save a searchable PDF, or recognize without saving.": {
    cn: "当前文件没有文字层。识别全部页面后即可提取目录。可另存为可搜索 PDF，或不保存、仅识别后提取。",
    tw: "目前檔案沒有文字層。識別全部頁面後即可提取目錄。可另存為可搜尋 PDF，或不儲存、僅識別後提取。",
  },
  "Saved PDF changes to '%s'": {
    cn: "已将 PDF 更改保存到 '%s'",
    tw: "已將 PDF 變更儲存到 '%s'",
  },
  "Unsaved PDF changes in '%s'": {
    cn: "'%s' 中有未保存的 PDF 更改",
    tw: "'%s' 中有未儲存的 PDF 變更",
  },
  "Save PDF changes?": {
    cn: "是否保存 PDF 更改？",
    tw: "是否儲存 PDF 變更？",
  },
  "Unsaved PDF changes": {
    cn: "未保存的 PDF 更改",
    tw: "未儲存的 PDF 變更",
  },
  "Remove missing files from home": {
    cn: "从首页移除丢失的文件",
    tw: "從首頁移除遺失的檔案",
  },
  "Rotate PDF Pages...": { cn: "旋转 PDF 页面...", tw: "旋轉 PDF 頁面..." },
  "Rotate PDF Pages": { cn: "旋转 PDF 页面", tw: "旋轉 PDF 頁面" },
  "Pages To Rotate:": { cn: "要旋转的页面:", tw: "要旋轉的頁面:" },
  "Rotation Angle:": { cn: "旋转角度:", tw: "旋轉角度:" },
  "90° Clockwise": { cn: "顺时针 90°", tw: "順時針 90°" },
  "270° Clockwise": { cn: "顺时针 270°", tw: "順時針 270°" },
  "Rotate Pages": { cn: "旋转页面", tw: "旋轉頁面" },
  "Selected pages are already at that rotation.": {
    cn: "所选页面已经是该旋转角度。",
    tw: "所選頁面已經是該旋轉角度。",
  },
  "Failed to save rotated PDF pages.": {
    cn: "保存旋转后的页面失败。",
    tw: "儲存旋轉後的頁面失敗。",
  },
  "Rotated %d page(s) and saved to '%s'": {
    cn: "已旋转 %d 页并保存到 '%s'",
    tw: "已旋轉 %d 頁並儲存到 '%s'",
  },
  "Apply To": { cn: "应用到", tw: "套用至" },
  "Current Page (Page %d)": {
    cn: "当前页（第 %d 页）",
    tw: "目前頁（第 %d 頁）",
  },
  "All Pages (%d)": { cn: "全部页面（共 %d 页）", tw: "所有頁面（共 %d 頁）" },
  "Specified Pages": { cn: "指定页面", tw: "指定頁面" },
  "e.g. 2, 5-7, 13-": { cn: "例如：2, 5-7, 13-", tw: "例如：2, 5-7, 13-" },
  "Rotation": { cn: "旋转方向", tw: "旋轉方向" },
  "Left 90°": { cn: "向左 90°", tw: "向左 90°" },
  "Right 90°": { cn: "向右 90°", tw: "向右 90°" },
  "Apply Rotation": { cn: "应用旋转", tw: "套用旋轉" },
  "Invalid page range.": { cn: "页面范围无效。", tw: "頁面範圍無效。" },
};

// Read the full list of supported language codes from TranslationLangs.cpp.
function readLangCodes(): string[] {
  const cppPath = join(rootDir, "src", "TranslationLangs.cpp");
  const text = readFileSync(cppPath, "utf-8");
  const codes: string[] = [];
  const re = /"([a-z]{2,7}(?:-[a-z]+)?)\\0"\s*\\?/g;
  let m: RegExpExecArray | null;
  while ((m = re.exec(text)) !== null) {
    codes.push(m[1]);
  }
  return codes;
}

function parseGoodTranslations(text: string): Map<string, Map<string, string>> {
  const lines = text.split("\n");
  const result = new Map<string, Map<string, string>>();
  let current = "";
  for (const line of lines) {
    if (line.startsWith(":")) {
      current = line.substring(1);
      result.set(current, new Map());
      continue;
    }
    if (!current || line.length === 0) continue;
    const idx = line.indexOf(":");
    if (idx <= 0) continue;
    const lang = line.substring(0, idx);
    const trans = line.substring(idx + 1);
    result.get(current)!.set(lang, trans);
  }
  return result;
}

function serializeGoodTranslations(data: Map<string, Map<string, string>>): string {
  // Keep header lines.
  const out: string[] = ["AppTranslator: SumatraPDF", "AppTranslator: SumatraPDF"];
  const sortedStrings = [...data.keys()].sort();
  for (const s of sortedStrings) {
    out.push(":" + s);
    const perLang = data.get(s)!;
    const sortedLangs = [...perLang.keys()].sort();
    for (const lang of sortedLangs) {
      const trans = perLang.get(lang)!;
      if (trans.includes("\n")) {
        throw new Error(`translation contains newline: ${s} ${lang}`);
      }
      out.push(`${lang}:${trans}`);
    }
  }
  return out.join("\n");
}

function main() {
  const allLangCodes = readLangCodes();
  const goodText = readFileSync(goodPath, "utf-8");
  const data = parseGoodTranslations(goodText);

  for (const [english, trans] of Object.entries(stringsToAdd)) {
    if (!data.has(english)) {
      data.set(english, new Map());
    }
    const perLang = data.get(english)!;
    for (const lang of allLangCodes) {
      if (lang === "en") continue; // English is the source, no entry needed.
      if (perLang.has(lang)) continue; // keep existing community translation.
      if (lang === "cn") {
        perLang.set(lang, trans.cn);
      } else if (lang === "tw") {
        perLang.set(lang, trans.tw);
      } else {
        perLang.set(lang, english); // placeholder: show English.
      }
    }
  }

  const out = serializeGoodTranslations(data);
  writeFileSync(goodPath, out, "utf-8");
  console.log(`Wrote ${goodPath}`);
  console.log(`Added/updated ${Object.keys(stringsToAdd).length} strings for ${allLangCodes.length - 1} languages.`);

  const transPath = join(rootDir, "translations", "translations.txt");
  let transText = readFileSync(transPath, "utf-8");
  for (const [english, trans] of Object.entries(stringsToAdd)) {
    if (transText.includes(":" + english + "\n") || transText.includes(":" + english + "\r\n")) {
      continue;
    }
    transText += `\n:${english}\ncn:${trans.cn}\ntw:${trans.tw}\n`;
  }
  writeFileSync(transPath, transText, "utf-8");
  console.log(`Updated ${transPath}`);

  const makeLzsa = join(rootDir, "bin", "MakeLZSA.exe");
  const lzsaPath = join(rootDir, "translations", "translations.txt.lzsa");
  const res = spawnSync(makeLzsa, [lzsaPath, `${goodPath}:translations-good.txt`], { stdio: "inherit" });
  if (res.status !== 0) {
    throw new Error(`MakeLZSA failed with exit code ${res.status}`);
  }
  console.log(`Wrote ${lzsaPath}`);
}

main();
