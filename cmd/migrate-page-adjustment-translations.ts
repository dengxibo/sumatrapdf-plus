import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const root = join(import.meta.dir, "..");
// Add only missing Chinese translations; preserve all existing language entries.
const labels: Record<string, [string, string]> = {
  "Free": ["自由", "自由"],
  "Lock current ratio": ["锁定当前框比例", "鎖定目前框比例"],
  "Original page ratio": ["原页比例", "原頁比例"],
  "A4 Portrait": ["A4 竖向", "A4 直向"],
  "A4 Landscape": ["A4 横向", "A4 橫向"],
  "Adjust this page separately": ["本页单独调整", "本頁單獨調整"],
  "This page only": ["本页单独调整", "本頁單獨調整"],
  "Shared crop": ["统一框", "統一框"],
  "Pending %d/%d - %s%s": ["待处理 %d/%d · %s%s", "待處理 %d/%d · %s%s"],
  " - Crop outside page": [" · 裁切框越界", " · 裁切框越界"],
  "Crop area outside page: %s": ["以下页面裁切框越界，请逐页修正：%s", "以下頁面裁切框越界，請逐頁修正：%s"],
  "Unable to load page.": ["无法加载页面。", "無法載入頁面。"],
  "Crop aspect ratio": ["裁切宽高比", "裁切寬高比"],
  "Adjust this page separately; uncheck to restore the shared crop.": ["仅调整当前预览页；取消勾选则恢复统一裁切框。", "僅調整目前預覽頁；取消勾選則恢復統一裁切框。"],
  "Page number within the selected range; press Enter to preview.": ["输入待处理范围内的页码，按回车预览。", "輸入待處理範圍內的頁碼，按 Enter 預覽。"],
  "Pending  %d / %d   -   %s%s": ["待处理  %d / %d   ·   %s%s", "待處理  %d / %d   ·   %s%s"],
  "Preview  %d / %d   -   %s%s": ["预览  %d / %d   ·   %s%s", "預覽  %d / %d   ·   %s%s"],
  "Applying changes…": ["正在应用…", "正在套用…"],
  "Single page": ["单页", "單頁"],
  "Keep adjusting pages separately while browsing; uncheck to restore this page to the shared crop.": ["翻页后继续单独调整各页；取消勾选则将本页恢复为统一框。", "翻頁後繼續單獨調整各頁；取消勾選則將本頁恢復為統一框。"],
  "Manual Crop": ["手动裁切", "手動裁切"],
  "Drag to select a crop area; drag its edges to resize or its inside to move.": ["拖动框选保留区域；拖动边框调整大小，拖动框内移动选区。", "拖動框選保留區域；拖動邊框調整大小，拖動框內移動選區。"],
  "Deskew": ["倾斜校正", "傾斜校正"],
  "Reference grid mode: horizontal lines or cross grid": ["参考网格模式：水平线或十字网格", "參考網格模式：水平線或十字網格"],
  "Reference grid spacing: sparse, normal or dense": ["参考网格间距：疏、标准或密", "參考網格間距：疏、標準或密"],
  "Minimize": ["最小化", "最小化"],
  "Maximize": ["最大化", "最大化"],
  "Check the angle and reopen this dialog if the document has changed.": ["请检查角度；文档若已变化，请重新打开此窗口。", "請檢查角度；文件若已變更，請重新開啟此視窗。"],
  "Reference Grid": ["参考网格", "參考網格"],
  "Show Grid": ["显示网格", "顯示網格"],
  "Sparse": ["疏", "疏"],
  "Normal": ["标准", "標準"],
  "Dense": ["密", "密"],
  "Horizontal Lines": ["水平线", "水平線"],
  "Cross Grid": ["十字网格", "十字網格"],
  "Page Rotation": ["页面旋转", "頁面旋轉"],
  "Flip & Crop": ["翻转与裁剪", "翻轉與裁剪"],
  "Page %d  |  %.0f%%": ["第 %d 页  |  %.0f%%", "第 %d 頁  |  %.0f%%"],
  "Enter an angle from -180° to +180°.": ["请输入 −180° 到 +180° 的角度。", "請輸入 −180° 到 +180° 的角度。"],
  "Delete pages": ["删除页面", "刪除頁面"],
  "Abort": ["中止", "中止"],
  "Ignore": ["忽略", "忽略"],
  "Try again": ["重试", "重試"],
  "Continue": ["继续", "繼續"],
  "Pages": ["页面", "頁面"],
  "Copy pages\tCtrl+C": ["复制页面\tCtrl+C", "複製頁面\tCtrl+C"],
  "Cut pages\tCtrl+X": ["剪切页面\tCtrl+X", "剪下頁面\tCtrl+X"],
  "Paste pages before": ["在此页前粘贴页面", "在此頁前貼上頁面"],
  "Paste pages after\tCtrl+V": ["在此页后粘贴页面\tCtrl+V", "在此頁後貼上頁面\tCtrl+V"],
  "Delete pages\tDelete": ["删除页面\tDelete", "刪除頁面\tDelete"],
  "Undo page edit\tCtrl+Z": ["撤销页面编辑\tCtrl+Z", "復原頁面編輯\tCtrl+Z"],
  "Select all\tCtrl+A": ["全选\tCtrl+A", "全選\tCtrl+A"],
  "Insert PDF or image before…": ["在此页前插入 PDF 或图片…", "在此頁前插入 PDF 或圖片…"],
  "Insert PDF or image after…": ["在此页后插入 PDF 或图片…", "在此頁後插入 PDF 或圖片…"],
  "Insert clipboard image after": ["在此页后插入剪贴板图片", "在此頁後插入剪貼簿圖片"],
  "Extract selected pages…": ["提取选中页面…", "擷取所選頁面…"],
  "Delete %d selected page(s)?": ["删除选中的 %d 页？", "刪除所選的 %d 頁？"],
  "Keep at least one page in the document.": ["文档至少需要保留一页。", "文件至少需要保留一頁。"],
  "Save the recognized text before changing the page order.": ["请先保存识别的文字，再编辑、插入或提取页面。", "請先儲存辨識的文字，再編輯、插入或擷取頁面。"],
  "Wait for text recognition to finish before editing pages.": ["文字识别正在进行，请完成后再编辑页面。", "文字辨識正在進行，請完成後再編輯頁面。"],
  "Choose a different file for extracted pages.": ["请将提取页面保存到其他文件。", "請將擷取頁面儲存到其他檔案。"],
  "Unable to edit or copy the selected pages.": ["无法处理所选页面，请检查文件是否有效、未加密，或是否可写入。", "無法處理所選頁面，請檢查檔案是否有效、未加密，或是否可寫入。"],

};

for (const name of ["translations-good.txt", "translations.txt"]) {
  const path = join(root, "translations", name);
  const original = readFileSync(path, "utf8");
  const newline = original.includes("\r\n") ? "\r\n" : "\n";
  const lines = original.split(/\r?\n/);
  for (const [key, values] of Object.entries(labels)) {
    let start = lines.indexOf(":" + key);
    if (start < 0) {
      while (lines.length && lines.at(-1) === "") lines.pop();
      lines.push(":" + key);
      start = lines.length - 1;
    }
    let end = start + 1;
    while (end < lines.length && !lines[end].startsWith(":")) end++;
    for (const [index, code] of ["cn", "tw"].entries()) {
      if (!lines.slice(start + 1, end).some(line => line.startsWith(code + ":"))) {
        lines.splice(end++, 0, code + ":" + values[index]);
      }
    }
  }
  writeFileSync(path, lines.join(newline).replace(/(?:\r?\n)*$/, newline));
}

const result = spawnSync(join(root, "bin", "MakeLZSA.exe"), [
  join(root, "translations", "translations.txt.lzsa"),
  join(root, "translations", "translations-good.txt") + ":translations-good.txt",
], { stdio: "inherit" });
if (result.status !== 0) process.exit(result.status ?? 1);
