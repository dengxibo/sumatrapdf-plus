import { readFileSync } from "node:fs";
const source = readFileSync("src/PdfTools.cpp", "utf8");
const section = source.slice(source.indexOf("idRotateAny ="), source.indexOf("// --- Encrypt PDF dialog ---"));
const translations = readFileSync("translations/translations-good.txt", "utf8");
const blocks = new Map<string, string>();
for (const block of translations.split(/\r?\n(?=:)/)) {
  const lines = block.split(/\r?\n/);
  blocks.set(lines[0].slice(1), block);
}
const keys = new Set([...section.matchAll(/_TR[AW]?\("([^"\n]+)"\)/g)].map(match => match[1]));
const missing = [...keys].filter(key => !/(?:^|\n)cn:[^\r\n]+/.test(blocks.get(key) ?? ""));
if (missing.length) throw new Error("Missing Chinese translations: " + missing.join(", "));
console.log(`Chinese localization coverage passed for ${keys.size} manual-page-adjustment labels/messages`);
