/**
 * Runtime loads translations-good.txt (via lzsa), not translations.txt.
 * generateGoodSubset() skips any language with >180 missing keys — so Japanese
 * (and most majors) lose classic menu strings like "&File" even when full file
 * has them. Always merge the 17 major languages from translations.txt into good.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const root = join(import.meta.dir, "..");
const majors = [
  "cn",
  "tw",
  "ja",
  "kr",
  "de",
  "fr",
  "es",
  "it",
  "pt",
  "br",
  "ru",
  "uk",
  "pl",
  "nl",
  "tr",
  "vn",
] as const;

function parseBlocks(text: string): Map<string, Map<string, string>> {
  const blocks = new Map<string, Map<string, string>>();
  let key: string | null = null;
  let map: Map<string, string> | null = null;
  for (const line of text.split(/\r?\n/)) {
    if (line.startsWith(":")) {
      if (key && map) {
        blocks.set(key, map);
      }
      key = line.slice(1);
      map = new Map();
    } else if (map) {
      const i = line.indexOf(":");
      if (i > 0) {
        map.set(line.slice(0, i), line.slice(i + 1));
      }
    }
  }
  if (key && map) {
    blocks.set(key, map);
  }
  return blocks;
}

function serialize(blocks: Map<string, Map<string, string>>, headerLines: string[]): string {
  const keys = [...blocks.keys()].sort((a, b) => a.localeCompare(b));
  const out = [...headerLines];
  for (const key of keys) {
    out.push(":" + key);
    const values = blocks.get(key)!;
    for (const lang of [...values.keys()].sort((a, b) => a.localeCompare(b))) {
      out.push(`${lang}:${values.get(lang)}`);
    }
  }
  return out.join("\n") + "\n";
}

const fullPath = join(root, "translations", "translations.txt");
const goodPath = join(root, "translations", "translations-good.txt");
const fullText = readFileSync(fullPath, "utf8");
const goodText = readFileSync(goodPath, "utf8");
const full = parseBlocks(fullText);
const good = parseBlocks(goodText);

let filled = 0;
let touchedKeys = 0;
for (const [key, fullMap] of full) {
  let map = good.get(key);
  if (!map) {
    map = new Map();
    good.set(key, map);
  }
  let keyTouched = false;
  for (const lang of majors) {
    const v = fullMap.get(lang);
    if (!v) {
      continue;
    }
    if (map.get(lang) !== v) {
      map.set(lang, v);
      filled++;
      keyTouched = true;
    }
  }
  if (keyTouched) {
    touchedKeys++;
  }
}

const header = goodText.split(/\r?\n/).slice(0, 2);
while (header.length < 2) {
  header.push("AppTranslator: SumatraPDF");
}
writeFileSync(goodPath, serialize(good, header));
console.log(`Synced majors into translations-good.txt: ${filled} cells, ${touchedKeys} keys`);

const archive = join(root, "translations", "translations.txt.lzsa");
const result = spawnSync(join(root, "bin", "MakeLZSA.exe"), [archive, `${goodPath}:translations-good.txt`], {
  stdio: "inherit",
});
if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
console.log("Rebuilt translations.txt.lzsa, size", readFileSync(archive).byteLength);
