// One-off migration: add i18n for update download progress notification.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const transPath = join(import.meta.dir, "..", "translations", "translations.txt");

type BlockMap = Record<string, string>;

const downloadingByLang: BlockMap = {
  am: "Արդիացումների ներբեռնում...",
  ar: "جاري تنزيل التحديث...",
  az: "Yeniləmə endirilir...",
  bg: "Изтегляне на обновление...",
  br: "Baixando atualização...",
  by: "Спампоўка абнаўлення…",
  ca: "S'està baixant l'actualització...",
  "ca-xv": "S'està baixant l'actualització...",
  cn: "正在下载更新...",
  co: "Scaricamentu di a messa à livellu…",
  cy: "Lawrlwytho diweddariad...",
  cz: "Stahuji aktualizaci...",
  de: "Update wird heruntergeladen…",
  dk: "Downloader opdatering...",
  el: "Λήψη ενημέρωσης...",
  es: "Descargando actualización...",
  et: "Uuenduse allalaadimine...",
  eu: "Eguneraketa deskargatzen...",
  fa: "در حال دانلود بروزرسانی...",
  fi: "Ladataan päivitystä...",
  fo: "Hevur dagføring...",
  fr: "Téléchargement de la mise à jour...",
  ga: "Nuashonrú á íoslódáil...",
  gl: "Descargando actualización...",
  he: "מוריד עדכון...",
  hi: "अपडेट डाउनलोड हो रहा है...",
  hr: "Preuzimanje ažuriranja...",
  hu: "Frissítés letöltése...",
  id: "Mengunduh pembaruan...",
  it: "Download dell'aggiornamento...",
  ja: "更新をダウンロード中...",
  kr: "업데이트를 다운로드하는 중...",
  kw: "Ow iskraga nowedhans...",
  lv: "Lejupielādē atjauninājumu...",
  mk: "Преземање на надградба...",
  ml: "അപ്‌ഡേറ്റ് ഡൗൺലോഡ് ചെയ്യുന്നു...",
  my: "Memuat turun kemas kini...",
  nl: "Update downloaden...",
  nn: "Lastar ned oppdatering...",
  no: "Laster ned oppdatering...",
  pl: "Pobieranie aktualizacji...",
  pt: "Baixando atualização...",
  ro: "Se descarcă actualizarea...",
  ru: "Загрузка обновления...",
  sat: "ᱦᱟᱹᱞᱤᱭᱟᱹᱠ ᱰᱟᱩᱱᱞᱚᱰ ᱦᱩᱭᱩᱜ ᱠᱟᱱᱟ...",
  sk: "Sťahuje sa aktualizácia…",
  sl: "Prenos posodobitve …",
  "sr-rs": "Преузимање ажурирања...",
  sv: "Laddar ner uppdatering...",
  ta: "புதுப்பிப்பு பதிவிறக்கப்படுகிறது...",
  th: "กำลังดาวน์โหลดการอัปเดต...",
  tl: "Nagda-download ng update...",
  tr: "Güncelleme indiriliyor...",
  tw: "正在下載更新...",
  uk: "Завантаження оновлення...",
  uz: "Yangilanish yuklab olinmoqda...",
  vn: "Đang tải bản cập nhật...",
};

const downloadUpdateByLang: BlockMap = {
  am: "Ներբեռնել արդիացումը",
  ar: "تنزيل التحديث",
  az: "Yeniləməni endir",
  bg: "Изтегляне на обновление",
  br: "Baixar atualização",
  by: "Спампаваць абнаўленне",
  ca: "Baixa l'actualització",
  "ca-xv": "Baixa l'actualització",
  cn: "下载更新",
  co: "Scaricà a messa à livellu",
  cy: "Lawrlwytho diweddariad",
  cz: "Stáhnout aktualizaci",
  de: "Update herunterladen",
  dk: "Download opdatering",
  el: "Λήψη ενημέρωσης",
  es: "Descargar actualización",
  et: "Laadi uuendus alla",
  eu: "Deskargatu eguneraketa",
  fa: "دانلود بروزرسانی",
  fi: "Lataa päivitys",
  fo: "Hevur dagføring",
  fr: "Télécharger la mise à jour",
  ga: "Íoslódáil nuashonrú",
  gl: "Descargar actualización",
  he: "הורד עדכון",
  hi: "अपडेट डाउनलोड करें",
  hr: "Preuzmi ažuriranje",
  hu: "Frissítés letöltése",
  id: "Unduh pembaruan",
  it: "Scarica aggiornamento",
  ja: "更新をダウンロード",
  kr: "업데이트 다운로드",
  kw: "Iskraga nowedhans",
  lv: "Lejupielādēt atjauninājumu",
  mk: "Преземи надградба",
  ml: "അപ്‌ഡേറ്റ് ഡൗൺലോഡ് ചെയ്യുക",
  my: "Muat turun kemas kini",
  nl: "Update downloaden",
  nn: "Last ned oppdatering",
  no: "Last ned oppdatering",
  pl: "Pobierz aktualizację",
  pt: "Baixar atualização",
  ro: "Descarcă actualizarea",
  ru: "Загрузить обновление",
  sat: "ᱦᱟᱹᱞᱤᱭᱟᱹᱠ ᱰᱟᱩᱱᱞᱚᱰ",
  sk: "Stiahnuť aktualizáciu",
  sl: "Prenesi posodobitev",
  "sr-rs": "Преузми ажурирање",
  sv: "Ladda ner uppdatering",
  ta: "புதுப்பிப்பைப் பதிவிறக்கு",
  th: "ดาวน์โหลดการอัปเดต",
  tl: "I-download ang update",
  tr: "Güncellemeyi indir",
  tw: "下載更新",
  uk: "Завантажити оновлення",
  uz: "Yangilanishni yuklab olish",
  vn: "Tải bản cập nhật",
};

function hasBlock(lines: string[], key: string): boolean {
  return lines.some((l) => l === ":" + key);
}

function appendBlock(lines: string[], key: string, byLang: BlockMap): void {
  if (hasBlock(lines, key)) {
    console.log(`skip existing: ${key}`);
    return;
  }
  lines.push(":" + key);
  for (const lang of Object.keys(byLang).sort()) {
    lines.push(`${lang}:${byLang[lang]}`);
  }
  console.log(`added: ${key}`);
}

function replaceBlock(lines: string[], key: string, byLang: BlockMap): string[] {
  const marker = ":" + key;
  const out: string[] = [];
  let i = 0;
  while (i < lines.length) {
    if (lines[i] !== marker) {
      out.push(lines[i]);
      i++;
      continue;
    }
    out.push(marker);
    i++;
    while (i < lines.length && !lines[i].startsWith(":")) {
      i++;
    }
    for (const lang of Object.keys(byLang).sort()) {
      out.push(`${lang}:${byLang[lang]}`);
    }
  }
  return out;
}

let lines = readFileSync(transPath, "utf8").split("\n");
appendBlock(lines, "Downloading update...", downloadingByLang);
lines = replaceBlock(lines, "Download update", downloadUpdateByLang);
writeFileSync(transPath, lines.join("\n"), "utf8");
console.log("Updated translations/translations.txt");

function parseTranslations(d: string) {
  const lines = d.split("\n");
  const perLang = new Map<string, Map<string, string>>();
  const allStrings: string[] = [];
  let currString = "";
  for (const s of lines.slice(2)) {
    if (s.length === 0) continue;
    if (s.startsWith(":")) {
      currString = s.substring(1);
      allStrings.push(currString);
      continue;
    }
    const colonIdx = s.indexOf(":");
    if (colonIdx === -1) continue;
    const lang = s.substring(0, colonIdx);
    const trans = s.substring(colonIdx + 1);
    let m = perLang.get(lang);
    if (!m) {
      m = new Map();
      perLang.set(lang, m);
    }
    m.set(currString, trans);
  }
  return { perLang, allStrings };
}

function packTranslationsGood() {
  const content = readFileSync(transPath, "utf8");
  const { perLang, allStrings } = parseTranslations(content);
  const langsToSkip = new Set<string>();
  for (const [lang, m] of perLang) {
    if (allStrings.length - m.size > 180) {
      langsToSkip.add(lang);
    }
  }
  const out = ["AppTranslator: SumatraPDF", "AppTranslator: SumatraPDF"];
  const sortedLangs = [...perLang.keys()].filter((lang) => !langsToSkip.has(lang)).sort();
  const sortedStrings = [...allStrings].sort();
  for (const s of sortedStrings) {
    out.push(":" + s);
    for (const lang of sortedLangs) {
      const trans = perLang.get(lang)!.get(s);
      if (trans) out.push(`${lang}:${trans}`);
    }
  }
  const goodPath = join(import.meta.dir, "..", "translations", "translations-good.txt");
  writeFileSync(goodPath, out.join("\n"), "utf8");
  console.log(`Wrote ${goodPath}`);

  const lzsaPath = join(import.meta.dir, "..", "translations", "translations.txt.lzsa");
  const makeLzsa = join(import.meta.dir, "..", "bin", "MakeLZSA.exe");
  const res = spawnSync(makeLzsa, [lzsaPath, `${goodPath}:translations-good.txt`], { stdio: "inherit" });
  if (res.status !== 0) {
    throw new Error(`MakeLZSA failed with exit code ${res.status}`);
  }
  console.log(`Wrote ${lzsaPath}`);
}

packTranslationsGood();
