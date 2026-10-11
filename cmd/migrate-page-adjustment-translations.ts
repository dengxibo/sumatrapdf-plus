import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const root = join(import.meta.dir, "..");
// Add only missing Chinese translations; preserve all existing language entries.
const labels: Record<string, [string, string]> = {
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
