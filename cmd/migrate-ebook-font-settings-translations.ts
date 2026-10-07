import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const root = join(import.meta.dir, "..");
// Title, Western body font, CJK body font, size hint. Font family names stay unchanged.
const labels: Record<string, string[]> = {
  cn: ["电子书字体", "西文字体(&W)：", "中日韩字体(&C)：", "字号（0 = 自动，6–26）(&S)："],
  tw: ["電子書字體", "西文字體(&W)：", "中日韓字體(&C)：", "字號（0 = 自動，6–26）(&S)："],
  ja: ["電子書籍のフォント", "欧文本文フォント(&W):", "日中韓本文フォント(&C):", "サイズ（0 = 自動、6–26）(&S):"],
  kr: ["전자책 글꼴", "서양 문자 본문 글꼴(&W):", "한중일 본문 글꼴(&C):", "크기 (0 = 자동, 6–26)(&S):"],
  de: ["E-Book-Schriften", "&Westliche Textschrift:", "&CJK-Textschrift:", "Größe (0 = automatisch, 6–26)(&S):"],
  fr: ["Polices des livres numériques", "Police du texte &occidental :", "Police du texte &CJK :", "Taille (0 = auto, 6–26)(&S) :"],
  es: ["Fuentes de libros electrónicos", "Fuente del texto &occidental:", "Fuente del texto &CJK:", "Tamaño (0 = automático, 6–26)(&S):"],
  it: ["Caratteri degli ebook", "Carattere del testo &occidentale:", "Carattere del testo &CJK:", "Dimensione (0 = automatica, 6–26)(&S):"],
  pt: ["Tipos de letra dos livros digitais", "Tipo de letra do texto &ocidental:", "Tipo de letra do texto &CJK:", "Tamanho (0 = automático, 6–26)(&S):"],
  br: ["Fontes de livros digitais", "Fonte do texto &ocidental:", "Fonte do texto &CJK:", "Tamanho (0 = automático, 6–26)(&S):"],
  ru: ["Шрифты электронных книг", "Шрифт западного текста(&W):", "Шрифт текста CJK(&C):", "Размер (0 = авто, 6–26)(&S):"],
  uk: ["Шрифти електронних книг", "Шрифт західного тексту(&W):", "Шрифт тексту CJK(&C):", "Розмір (0 = авто, 6–26)(&S):"],
  pl: ["Czcionki e-booków", "Czcionka tekstu &zachodniego:", "Czcionka tekstu &CJK:", "Rozmiar (0 = auto, 6–26)(&S):"],
  nl: ["E-booklettertypen", "&Westers tekstlettertype:", "&CJK-tekstlettertype:", "Grootte (0 = automatisch, 6–26)(&S):"],
  tr: ["E-kitap yazı tipleri", "Batı metni yazı tipi(&W):", "CJK metni yazı tipi(&C):", "Boyut (0 = otomatik, 6–26)(&S):"],
  vn: ["Phông chữ sách điện tử", "Phông chữ văn bản phương Tây(&W):", "Phông chữ văn bản CJK(&C):", "Cỡ chữ (0 = tự động, 6–26)(&S):"],
};
const keys = ["Ebook fonts", "&Western body font:", "&CJK body font:", "Si&ze (0 = auto, 6-26):"];
function blocks(text: string) {
  const result = new Map<string, Map<string, string>>();
  let current: Map<string, string> | undefined;
  for (const line of text.split(/\r?\n/)) {
    if (line.startsWith(":")) {
      current = new Map();
      result.set(line.slice(1), current);
    } else if (current) {
      const sep = line.indexOf(":");
      if (sep > 0) current.set(line.slice(0, sep), line.slice(sep + 1));
    }
  }
  return result;
}
for (const name of ["translations-good.txt", "translations.txt"]) {
  const path = join(root, "translations", name);
  let text = readFileSync(path, "utf8");
  const existing = blocks(text);
  keys.forEach((key, index) => {
    const values = new Map(existing.get(key));
    if (index === 3) {
      for (const [lang, value] of existing.get("Si&ze (0 = auto, 6-72):") ?? []) {
        if (!values.has(lang)) values.set(lang, value.replace(/6([-–])72/g, "6$126"));
      }
    }
    for (const [lang, translated] of Object.entries(labels)) values.set(lang, translated[index]);
    const block = ":" + key + "\n" + [...values].sort(([a], [b]) => a.localeCompare(b)).map(([lang, value]) => `${lang}:${value}`).join("\n") + "\n";
    const start = text.indexOf(":" + key + "\n");
    if (start >= 0) {
      const end = text.indexOf("\n:", start + 1);
      text = text.slice(0, start) + block + (end < 0 ? "" : text.slice(end + 1));
    } else {
      text = text.trimEnd() + "\n\n" + block;
    }
  });
  writeFileSync(path, text);
}
const archive = join(root, "translations", "translations.txt.lzsa");
const good = join(root, "translations", "translations-good.txt");
const result = spawnSync(join(root, "bin", "MakeLZSA.exe"), [archive, `${good}:translations-good.txt`], { stdio: "inherit" });
if (result.status !== 0) process.exit(result.status ?? 1);
