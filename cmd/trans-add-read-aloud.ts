import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

// New Read Aloud settings / bar / menu strings.
// cn/tw are authored; other languages fall back to English so the UI never
// shows a blank string. Run cmd/trans-with-ai.ts afterwards if an API key is
// available, then re-run this script only for keys that are still missing.

const rootDir = import.meta.dir + "/..";
const goodPath = join(rootDir, "translations", "translations-good.txt");

const stringsToAdd: Record<string, { cn: string; tw: string }> = {
  "&Read Aloud": { cn: "朗读(&R)", tw: "朗讀(&R)" },
  "Read Aloud Settings...": { cn: "朗读设置...", tw: "朗讀設定..." },
  "Read Aloud Settings": { cn: "朗读设置", tw: "朗讀設定" },
  "&Voice:": { cn: "语音(&V)：", tw: "語音(&V)：" },
  "&Chinese voice:": { cn: "中文语音(&C)：", tw: "中文語音(&C)：" },
  "&English voice:": { cn: "英文语音(&E)：", tw: "英文語音(&E)：" },
  "&Multilingual voice:": { cn: "多语言语音(&M)：", tw: "多語言語音(&M)：" },
  "&Preview": { cn: "试听(&P)", tw: "試聽(&P)" },
  "C&hinese:": { cn: "中文(&H)：", tw: "中文(&H)：" },
  "E&nglish:": { cn: "英文(&N)：", tw: "英文(&N)：" },
  "0.25x to 2.00x": { cn: "0.25x 到 2.00x", tw: "0.25x 到 2.00x" },
  "Highlight and follow": { cn: "高亮与跟随", tw: "醒目顯示與跟隨" },
  "Highlight co&lor:": { cn: "高亮颜色(&L)：", tw: "醒目顯示顏色(&L)：" },
  "&Follow the text being read": { cn: "朗读时自动跟随(&F)", tw: "朗讀時自動跟隨(&F)" },
  "Narrated books (EPUB 3)": { cn: "有声书（EPUB 3）", tw: "有聲書（EPUB 3）" },
  "Play the book's recorded narration when &available": {
    cn: "有书内录音时播放录音(&A)",
    tw: "有書內錄音時播放錄音(&A)",
  },
  "When off, narrated books are read with the voice above.": {
    cn: "关闭后，有声书改用上面的系统语音朗读。",
    tw: "關閉後，有聲書改用上面的系統語音朗讀。",
  },
  "Use the book's highlight c&olor": { cn: "使用书内高亮色(&O)", tw: "使用書內醒目顯示色(&O)" },
  "Narration spee&d:": { cn: "录音倍速(&D)：", tw: "錄音倍速(&D)：" },
  "Previous sentence": { cn: "上一句", tw: "上一句" },
  "Next sentence": { cn: "下一句", tw: "下一句" },
  "Play / Pause reading": { cn: "播放 / 暂停朗读", tw: "播放 / 暫停朗讀" },
  "Follow": { cn: "跟随", tw: "跟隨" },
  "Scroll to the text being read": { cn: "滚动到正在朗读的文字", tw: "捲動到正在朗讀的文字" },
  "Previous phrase": { cn: "上一句", tw: "上一句" },
  "Next phrase": { cn: "下一句", tw: "下一句" },
  "Play / Pause narration": { cn: "播放 / 暂停朗读", tw: "播放 / 暫停朗讀" },
  "Hide narration controls": { cn: "隐藏朗读条", tw: "隱藏朗讀列" },
  "Back 10 seconds": { cn: "后退 10 秒", tw: "後退 10 秒" },
  "Forward 10 seconds": { cn: "前进 10 秒", tw: "前進 10 秒" },
  "Playback speed": { cn: "播放速度", tw: "播放速度" },
  "End of narration": { cn: "朗读结束", tw: "朗讀結束" },
  "Narration could not be loaded": { cn: "无法加载录音", tw: "無法載入錄音" },
};

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
    if (!current || line.length === 0) {
      continue;
    }
    const idx = line.indexOf(":");
    if (idx <= 0) {
      continue;
    }
    result.get(current)!.set(line.substring(0, idx), line.substring(idx + 1));
  }
  return result;
}

function serializeGoodTranslations(data: Map<string, Map<string, string>>): string {
  const out: string[] = ["AppTranslator: SumatraPDF", "AppTranslator: SumatraPDF"];
  const sortedStrings = [...data.keys()].sort();
  for (const s of sortedStrings) {
    out.push(":" + s);
    const perLang = data.get(s)!;
    for (const lang of [...perLang.keys()].sort()) {
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
      if (lang === "en") {
        continue;
      }
      if (perLang.has(lang)) {
        continue;
      }
      if (lang === "cn") {
        perLang.set(lang, trans.cn);
      } else if (lang === "tw") {
        perLang.set(lang, trans.tw);
      } else {
        perLang.set(lang, english);
      }
    }
  }

  writeFileSync(goodPath, serializeGoodTranslations(data), "utf-8");
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
