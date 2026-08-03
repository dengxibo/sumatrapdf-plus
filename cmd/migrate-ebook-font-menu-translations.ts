import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const rootDir = join(import.meta.dir, "..");
const goodPath = join(rootDir, "translations", "translations-good.txt");
const transPath = join(rootDir, "translations", "translations.txt");

const stringsToAdd: Record<string, { cn: string; tw: string }> = {
  "&Reading Font": { cn: "字体(&R)", tw: "字體(&R)" },
  "&Western Body Font": { cn: "西文字体(&W)", tw: "西文字體(&W)" },
  "&CJK Body Font": { cn: "中文字体(&C)", tw: "中文字體(&C)" },
  "&System Font": { cn: "系统字体(&S)", tw: "系統字體(&S)" },
  "Reset Font Si&ze to Default": { cn: "恢复默认字号(&Z)", tw: "恢復預設字號(&Z)" },
  "Ebook Font Size: Reset to Default": { cn: "电子书字号：恢复默认", tw: "電子書字號：恢復預設" },
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

function parseBlocks(text: string): Map<string, Map<string, string>> {
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

function serializeBlocks(data: Map<string, Map<string, string>>, header: string[]): string {
  const out = [...header];
  for (const s of [...data.keys()].sort()) {
    out.push(":" + s);
    for (const lang of [...data.get(s)!.keys()].sort()) {
      out.push(`${lang}:${data.get(s)!.get(lang)}`);
    }
  }
  return out.join("\n");
}

function addToFile(path: string, isGood: boolean) {
  const text = readFileSync(path, "utf-8");
  const header = isGood ? ["AppTranslator: SumatraPDF", "AppTranslator: SumatraPDF"] : text.split("\n").slice(0, 2);
  const data = parseBlocks(text);
  const allLangCodes = readLangCodes();

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

  const out = isGood ? serializeBlocks(data, header) : text + "\n\n" + Object.keys(stringsToAdd).map((s) => {
    const t = stringsToAdd[s];
    return `:${s}\ncn:${t.cn}\ntw:${t.tw}\n`;
  }).join("\n");
  writeFileSync(path, out, "utf-8");
  console.log(`Wrote ${path}`);
}

addToFile(goodPath, true);
addToFile(transPath, false);

const makeLzsa = join(rootDir, "bin", "MakeLZSA.exe");
const lzsaPath = join(rootDir, "translations", "translations.txt.lzsa");
const res = spawnSync(makeLzsa, [lzsaPath, `${goodPath}:translations-good.txt`], { stdio: "inherit" });
if (res.status !== 0) {
  process.exit(res.status ?? 1);
}
console.log(`Wrote ${lzsaPath}`);
