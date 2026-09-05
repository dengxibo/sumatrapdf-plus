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
