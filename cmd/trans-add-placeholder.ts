import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

// Add placeholder translations for Read Aloud dialog strings.
// For cn/tw we provide real Chinese translations; for all other languages we
// fall back to the English source string so that every supported language stays
// in the "good" subset and the UI at least shows English instead of nothing.

const rootDir = import.meta.dir + "/..";
const goodPath = join(rootDir, "translations", "translations-good.txt");

// Source string -> { cn, tw }
const stringsToAdd: Record<string, { cn: string; tw: string }> = {
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
}

main();