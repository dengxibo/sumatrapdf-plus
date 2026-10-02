import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const rootDir = join(import.meta.dir, "..");
const goodPath = join(rootDir, "translations", "translations-good.txt");

const stringsToAdd: Record<string, { cn: string; tw: string }> = {
  AI: { cn: "AI", tw: "AI" },
  Translate: { cn: "翻译", tw: "翻譯" },
  "Enable &inline translate": { cn: "启用划词翻译(&I)", tw: "啟用劃詞翻譯(&I)" },
  "Target &language:": { cn: "目标语言(&L)：", tw: "目標語言(&L)：" },
  "Volc &Access Key:": { cn: "火山 Access Key(&A)：", tw: "火山 Access Key(&A)：" },
  "Volc &Secret Key:": { cn: "火山 Secret Key(&S)：", tw: "火山 Secret Key(&S)：" },
  "Without Volc keys, translation uses the AI API below when configured.": {
    cn: "未配置火山密钥时，若已配置下方 AI API，则用大模型翻译。",
    tw: "未設定火山金鑰時，若已設定下方 AI API，則用大模型翻譯。",
  },
  "AI API (Q&A, translate fallback, TOC)": {
    cn: "AI API（问答、翻译托底、目录）",
    tw: "AI API（問答、翻譯備援、目錄）",
  },
  "Web AI": { cn: "网页 AI", tw: "網頁 AI" },
  "&Enable Web AI": { cn: "启用网页 AI(&E)", tw: "啟用網頁 AI(&E)" },
  "Auto (follow UI, flip when source matches)": {
    cn: "自动（跟随界面语言，源语相同时对调）",
    tw: "自動（跟隨介面語言，來源語相同時對調）",
  },
  "UI language": { cn: "界面语言", tw: "介面語言" },
  "Chinese (Simplified)": { cn: "简体中文", tw: "簡體中文" },
  "Chinese (Traditional)": { cn: "繁体中文", tw: "繁體中文" },
  Japanese: { cn: "日语", tw: "日語" },
  Korean: { cn: "韩语", tw: "韓語" },
  French: { cn: "法语", tw: "法語" },
  German: { cn: "德语", tw: "德語" },
  Spanish: { cn: "西班牙语", tw: "西班牙語" },
  Russian: { cn: "俄语", tw: "俄語" },
  Portuguese: { cn: "葡萄牙语", tw: "葡萄牙語" },
  Italian: { cn: "意大利语", tw: "義大利語" },
  Vietnamese: { cn: "越南语", tw: "越南語" },
  Thai: { cn: "泰语", tw: "泰語" },
  Arabic: { cn: "阿拉伯语", tw: "阿拉伯語" },
  Indonesian: { cn: "印尼语", tw: "印尼語" },
  Hindi: { cn: "印地语", tw: "印地語" },
  Turkish: { cn: "土耳其语", tw: "土耳其語" },
  Dutch: { cn: "荷兰语", tw: "荷蘭語" },
  Polish: { cn: "波兰语", tw: "波蘭語" },
  Swedish: { cn: "瑞典语", tw: "瑞典語" },
  Ukrainian: { cn: "乌克兰语", tw: "烏克蘭語" },
  Malay: { cn: "马来语", tw: "馬來語" },
  English: { cn: "英语", tw: "英語" },
  Chinese: { cn: "中文", tw: "中文" },
  "Translating…": { cn: "正在翻译…", tw: "正在翻譯…" },
  "Thinking…": { cn: "正在思考…", tw: "正在思考…" },
  "You:": { cn: "你：", tw: "你：" },
  "AI:": { cn: "AI：", tw: "AI：" },
  Copied: { cn: "已复制", tw: "已複製" },
  "Set up Volcengine Translate or an AI API in Settings → AI": {
    cn: "请在 选项 → AI 中配置火山翻译或 AI API",
    tw: "請在 選項 → AI 中設定火山翻譯或 AI API",
  },
  "Set up an AI API in Settings → AI to ask follow-up questions": {
    cn: "在 选项 → AI 中配置 AI API 后即可追问",
    tw: "在 選項 → AI 中設定 AI API 後即可追問",
  },
  "Ask about the selected text…": { cn: "就选中的文字提问…", tw: "就選取的文字提問…" },
  "Explain the selected text": { cn: "解释选中的文字", tw: "解釋選取的文字" },
  "Translation failed.": { cn: "翻译失败。", tw: "翻譯失敗。" },
  "The AI request failed.": { cn: "AI 请求失败。", tw: "AI 請求失敗。" },
  "Volcengine Translate": { cn: "火山翻译", tw: "火山翻譯" },
  "Volcengine Translate works.": { cn: "火山翻译连接正常。", tw: "火山翻譯連線正常。" },
  "Volcengine Translate failed:": { cn: "火山翻译测试失败：", tw: "火山翻譯測試失敗：" },
  "Enter both the Access Key and Secret Key first.": {
    cn: "请先填写 Access Key 和 Secret Key。",
    tw: "請先填寫 Access Key 和 Secret Key。",
  },
};

function readLangCodes(): string[] {
  const path = join(rootDir, "src", "Translations.cpp");
  // fall back: parse from translations-good first keys pattern lang:
  const good = readFileSync(goodPath, "utf-8");
  const langs = new Set<string>();
  for (const line of good.split(/\r?\n/)) {
    const m = /^([a-z]{2}(?:-[a-z]{2})?):/.exec(line);
    if (m && m[1] !== "en") {
      langs.add(m[1]);
    }
  }
  // Always include cn/tw
  langs.add("cn");
  langs.add("tw");
  return ["en", ...[...langs].sort()];
}

function parseGoodTranslations(text: string): Map<string, Map<string, string>> {
  const data = new Map<string, Map<string, string>>();
  let current: string | null = null;
  for (const raw of text.split(/\r?\n/)) {
    if (raw.startsWith("AppTranslator:")) {
      continue;
    }
    if (raw.startsWith(":")) {
      current = raw.slice(1);
      if (!data.has(current)) {
        data.set(current, new Map());
      }
      continue;
    }
    if (!current) {
      continue;
    }
    const colon = raw.indexOf(":");
    if (colon <= 0) {
      continue;
    }
    const lang = raw.slice(0, colon);
    const trans = raw.slice(colon + 1);
    data.get(current)!.set(lang, trans);
  }
  return data;
}

function serializeGoodTranslations(data: Map<string, Map<string, string>>): string {
  const out: string[] = ["AppTranslator: SumatraPDF", "AppTranslator: SumatraPDF"];
  const sortedStrings = [...data.keys()].sort();
  for (const s of sortedStrings) {
    out.push(":" + s);
    const perLang = data.get(s)!;
    const sortedLangs = [...perLang.keys()].sort();
    for (const lang of sortedLangs) {
      out.push(`${lang}:${perLang.get(lang)!}`);
    }
  }
  return out.join("\n");
}

const allLangCodes = readLangCodes();
const data = parseGoodTranslations(readFileSync(goodPath, "utf-8"));

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
  // Force-update cn/tw for our strings so UI is correct even if English placeholder existed
  perLang.set("cn", trans.cn);
  perLang.set("tw", trans.tw);
}

writeFileSync(goodPath, serializeGoodTranslations(data), "utf-8");
console.log(`Wrote ${goodPath}`);

const transPath = join(rootDir, "translations", "translations.txt");
let transText = readFileSync(transPath, "utf-8");
for (const [english, trans] of Object.entries(stringsToAdd)) {
  const marker = ":" + english + "\n";
  if (transText.includes(marker) || transText.includes(":" + english + "\r\n")) {
    continue;
  }
  transText += `\n:${english}\ncn:${trans.cn}\ntw:${trans.tw}\n`;
}
writeFileSync(transPath, transText, "utf-8");
console.log(`Updated ${transPath}`);

// MakeLZSA can't parse absolute "C:\..." in name:path, so run from rootDir with relative paths.
const makeLzsa = join(rootDir, "bin", "MakeLZSA.exe");
const lzsaPath = join(rootDir, "translations", "translations.txt.lzsa");
const res = spawnSync(
  makeLzsa,
  ["translations/translations.txt.lzsa", "translations/translations-good.txt:translations-good.txt"],
  { stdio: "inherit", cwd: rootDir },
);
if (res.status !== 0) {
  throw new Error(`MakeLZSA failed with exit code ${res.status}`);
}
console.log(`Wrote ${lzsaPath}`);
