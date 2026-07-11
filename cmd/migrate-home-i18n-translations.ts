// One-off migration: add Home tab / Home page i18n strings.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const transPath = join(import.meta.dir, "..", "translations", "translations.txt");

type LangMap = Record<string, string>;

const strings: { key: string; byLang: LangMap }[] = [
  {
    key: "search files (Ctrl + F)",
    byLang: {
      cn: "搜索文件 (Ctrl + F)",
      tw: "搜尋檔案 (Ctrl + F)",
      ja: "ファイルを検索 (Ctrl + F)",
      kr: "파일 검색 (Ctrl + F)",
      de: "Dateien suchen (Strg + F)",
      fr: "Rechercher des fichiers (Ctrl + F)",
      ru: "Поиск файлов (Ctrl + F)",
    },
  },
  {
    key: "Tip: ",
    byLang: {
      cn: "提示：",
      tw: "提示：",
      ja: "ヒント：",
      kr: "팁: ",
      de: "Tipp: ",
      fr: "Astuce : ",
      ru: "Подсказка: ",
    },
  },
  {
    key: "You can [customize scrollbar](CmdChangeScrollbar).",
    byLang: {
      cn: "你可以[自定义滚动条](CmdChangeScrollbar)。",
      tw: "你可以[自訂捲軸](CmdChangeScrollbar)。",
      ja: "[スクロールバーをカスタマイズ](CmdChangeScrollbar)できます。",
      kr: "[스크롤바 사용자 지정](CmdChangeScrollbar)이 가능합니다.",
      de: "Sie können die [Scrollleiste anpassen](CmdChangeScrollbar).",
      fr: "Vous pouvez [personnaliser la barre de défilement](CmdChangeScrollbar).",
      ru: "Можно [настроить полосу прокрутки](CmdChangeScrollbar).",
    },
  },
  {
    key: "You can [customize keyboard shortcuts](Help/Customizing-keyboard-shortcuts).",
    byLang: {
      cn: "你可以[自定义键盘快捷键](Help/Customizing-keyboard-shortcuts)。",
      tw: "你可以[自訂鍵盤快速鍵](Help/Customizing-keyboard-shortcuts)。",
      ja: "[キーボードショートカットをカスタマイズ](Help/Customizing-keyboard-shortcuts)できます。",
      kr: "[키보드 단축키 사용자 지정](Help/Customizing-keyboard-shortcuts)이 가능합니다.",
      de: "Sie können [Tastenkürzel anpassen](Help/Customizing-keyboard-shortcuts).",
      fr: "Vous pouvez [personnaliser les raccourcis clavier](Help/Customizing-keyboard-shortcuts).",
      ru: "Можно [настроить сочетания клавиш](Help/Customizing-keyboard-shortcuts).",
    },
  },
  {
    key: "You can [customize toolbar](Help/Customize-toolbar).",
    byLang: {
      cn: "你可以[自定义工具栏](Help/Customize-toolbar)。",
      tw: "你可以[自訂工具列](Help/Customize-toolbar)。",
      ja: "[ツールバーをカスタマイズ](Help/Customize-toolbar)できます。",
      kr: "[도구 모음 사용자 지정](Help/Customize-toolbar)이 가능합니다.",
      de: "Sie können die [Symbolleiste anpassen](Help/Customize-toolbar).",
      fr: "Vous pouvez [personnaliser la barre d'outils](Help/Customize-toolbar).",
      ru: "Можно [настроить панель инструментов](Help/Customize-toolbar).",
    },
  },
  {
    key: "Press (Key/CmdCommandPalette) to open [command palette](CmdCommandPalette).",
    byLang: {
      cn: "按 (Key/CmdCommandPalette) 打开[命令面板](CmdCommandPalette)。",
      tw: "按 (Key/CmdCommandPalette) 開啟[命令面板](CmdCommandPalette)。",
      ja: "(Key/CmdCommandPalette) で[コマンドパレット](CmdCommandPalette)を開けます。",
      kr: "(Key/CmdCommandPalette)로 [명령 팔레트](CmdCommandPalette)를 엽니다.",
      de: "Drücken Sie (Key/CmdCommandPalette), um die [Befehlsliste](CmdCommandPalette) zu öffnen.",
      fr: "Appuyez sur (Key/CmdCommandPalette) pour ouvrir la [palette de commandes](CmdCommandPalette).",
      ru: "Нажмите (Key/CmdCommandPalette), чтобы открыть [палитру команд](CmdCommandPalette).",
    },
  },
  {
    key: "To open file from history open [command palette](CmdCommandPalette) with (Key/CmdCommandPalette) and type `#`.",
    byLang: {
      cn: "要从历史记录打开文件，请用 (Key/CmdCommandPalette) 打开[命令面板](CmdCommandPalette)并输入 `#`。",
      tw: "要從歷史記錄開啟檔案，請用 (Key/CmdCommandPalette) 開啟[命令面板](CmdCommandPalette)並輸入 `#`。",
      ja: "履歴からファイルを開くには (Key/CmdCommandPalette) で[コマンドパレット](CmdCommandPalette)を開き、`#` と入力します。",
      kr: "기록에서 파일을 열려면 (Key/CmdCommandPalette)로 [명령 팔레트](CmdCommandPalette)를 연 뒤 `#`를 입력하세요.",
      de: "Um eine Datei aus dem Verlauf zu öffnen, öffnen Sie die [Befehlsliste](CmdCommandPalette) mit (Key/CmdCommandPalette) und geben `#` ein.",
      fr: "Pour ouvrir un fichier depuis l'historique, ouvrez la [palette de commandes](CmdCommandPalette) avec (Key/CmdCommandPalette) et tapez `#`.",
      ru: "Чтобы открыть файл из истории, откройте [палитру команд](CmdCommandPalette) через (Key/CmdCommandPalette) и введите `#`.",
    },
  },
  {
    key: "You can [extract text from PDF file](Help/Tool-x-extract-text-from-pdf).",
    byLang: {
      cn: "你可以[从 PDF 文件提取文本](Help/Tool-x-extract-text-from-pdf)。",
      tw: "你可以[從 PDF 檔案擷取文字](Help/Tool-x-extract-text-from-pdf)。",
      ja: "[PDF からテキストを抽出](Help/Tool-x-extract-text-from-pdf)できます。",
      kr: "[PDF에서 텍스트 추출](Help/Tool-x-extract-text-from-pdf)이 가능합니다.",
      de: "Sie können [Text aus PDF-Dateien extrahieren](Help/Tool-x-extract-text-from-pdf).",
      fr: "Vous pouvez [extraire le texte d'un PDF](Help/Tool-x-extract-text-from-pdf).",
      ru: "Можно [извлечь текст из PDF](Help/Tool-x-extract-text-from-pdf).",
    },
  },
  {
    key: "You can [toggle menu bar](CmdToggleMenuBar) with (Key/CmdToggleMenuBar).",
    byLang: {
      cn: "你可以用 (Key/CmdToggleMenuBar) [切换菜单栏](CmdToggleMenuBar)。",
      tw: "你可以用 (Key/CmdToggleMenuBar) [切換功能表列](CmdToggleMenuBar)。",
      ja: "(Key/CmdToggleMenuBar) で[メニューバーの表示を切り替え](CmdToggleMenuBar)できます。",
      kr: "(Key/CmdToggleMenuBar)로 [메뉴 모음 표시 전환](CmdToggleMenuBar)이 가능합니다.",
      de: "Mit (Key/CmdToggleMenuBar) können Sie die [Menüleiste ein-/ausblenden](CmdToggleMenuBar).",
      fr: "Vous pouvez [afficher/masquer la barre de menus](CmdToggleMenuBar) avec (Key/CmdToggleMenuBar).",
      ru: "Можно [переключить строку меню](CmdToggleMenuBar) с помощью (Key/CmdToggleMenuBar).",
    },
  },
  {
    key: "You can [toggle toolbar](CmdToggleToolbar) with (Key/CmdToggleToolbar).",
    byLang: {
      cn: "你可以用 (Key/CmdToggleToolbar) [切换工具栏](CmdToggleToolbar)。",
      tw: "你可以用 (Key/CmdToggleToolbar) [切換工具列](CmdToggleToolbar)。",
      ja: "(Key/CmdToggleToolbar) で[ツールバーの表示を切り替え](CmdToggleToolbar)できます。",
      kr: "(Key/CmdToggleToolbar)로 [도구 모음 표시 전환](CmdToggleToolbar)이 가능합니다.",
      de: "Mit (Key/CmdToggleToolbar) können Sie die [Symbolleiste ein-/ausblenden](CmdToggleToolbar).",
      fr: "Vous pouvez [afficher/masquer la barre d'outils](CmdToggleToolbar) avec (Key/CmdToggleToolbar).",
      ru: "Можно [переключить панель инструментов](CmdToggleToolbar) с помощью (Key/CmdToggleToolbar).",
    },
  },
  {
    key: "You can [edit PDF annotations](Help/Editing-annotations).",
    byLang: {
      cn: "你可以[编辑 PDF 标注](Help/Editing-annotations)。",
      tw: "你可以[編輯 PDF 註解](Help/Editing-annotations)。",
      ja: "[PDF 注釈を編集](Help/Editing-annotations)できます。",
      kr: "[PDF 주석 편집](Help/Editing-annotations)이 가능합니다.",
      de: "Sie können [PDF-Anmerkungen bearbeiten](Help/Editing-annotations).",
      fr: "Vous pouvez [modifier les annotations PDF](Help/Editing-annotations).",
      ru: "Можно [редактировать аннотации PDF](Help/Editing-annotations).",
    },
  },
  {
    key: "note",
    byLang: { cn: "说明", tw: "說明", ja: "注記", kr: "참고", de: "Hinweis", fr: "Note", ru: "Примечание" },
  },
  {
    key: "Community fork; not affiliated with sumatrapdfreader.org",
    byLang: {
      cn: "社区分支；与 sumatrapdfreader.org 无关",
      tw: "社群分支；與 sumatrapdfreader.org 無關",
      ja: "コミュニティ版；sumatrapdfreader.org とは無関係",
      kr: "커뮤니티 포크; sumatrapdfreader.org와 무관",
      de: "Community-Fork; nicht mit sumatrapdfreader.org verbunden",
      fr: "Fork communautaire ; non affilié à sumatrapdfreader.org",
      ru: "Сообщественный форк; не связан с sumatrapdfreader.org",
    },
  },
  {
    key: "Plus source",
    byLang: { cn: "Plus 源码", tw: "Plus 原始碼", ja: "Plus ソース", kr: "Plus 소스", de: "Plus-Quellcode", fr: "Source Plus", ru: "Исходники Plus" },
  },
  {
    key: "Sumatra PDF Plus on GitHub",
    byLang: {
      cn: "GitHub 上的 Sumatra PDF Plus",
      tw: "GitHub 上的 Sumatra PDF Plus",
      ja: "GitHub の Sumatra PDF Plus",
      kr: "GitHub의 Sumatra PDF Plus",
      de: "Sumatra PDF Plus auf GitHub",
      fr: "Sumatra PDF Plus sur GitHub",
      ru: "Sumatra PDF Plus на GitHub",
    },
  },
  {
    key: "Plus issues",
    byLang: { cn: "Plus 问题反馈", tw: "Plus 問題回報", ja: "Plus 問題報告", kr: "Plus 이슈", de: "Plus-Probleme", fr: "Problèmes Plus", ru: "Проблемы Plus" },
  },
  {
    key: "Report bugs (this fork only)",
    byLang: {
      cn: "报告错误（仅限本分支）",
      tw: "回報錯誤（僅限本分支）",
      ja: "バグ報告（このフォークのみ）",
      kr: "버그 보고(이 포크만)",
      de: "Fehler melden (nur dieser Fork)",
      fr: "Signaler des bugs (ce fork uniquement)",
      ru: "Сообщить об ошибках (только этот форк)",
    },
  },
  {
    key: "Plus guide",
    byLang: { cn: "Plus 指南", tw: "Plus 指南", ja: "Plus ガイド", kr: "Plus 가이드", de: "Plus-Anleitung", fr: "Guide Plus", ru: "Руководство Plus" },
  },
  {
    key: "User guide (readme.txt)",
    byLang: {
      cn: "用户指南 (readme.txt)",
      tw: "使用者指南 (readme.txt)",
      ja: "ユーザーガイド (readme.txt)",
      kr: "사용자 가이드 (readme.txt)",
      de: "Benutzerhandbuch (readme.txt)",
      fr: "Guide utilisateur (readme.txt)",
      ru: "Руководство пользователя (readme.txt)",
    },
  },
  {
    key: "official site",
    byLang: { cn: "官方网站", tw: "官方網站", ja: "公式サイト", kr: "공식 사이트", de: "Offizielle Website", fr: "Site officiel", ru: "Официальный сайт" },
  },
  {
    key: "SumatraPDF website (upstream)",
    byLang: {
      cn: "SumatraPDF 网站（上游）",
      tw: "SumatraPDF 網站（上游）",
      ja: "SumatraPDF サイト（上流）",
      kr: "SumatraPDF 웹사이트(업스트림)",
      de: "SumatraPDF-Website (Upstream)",
      fr: "Site SumatraPDF (amont)",
      ru: "Сайт SumatraPDF (upstream)",
    },
  },
  {
    key: "official manual",
    byLang: { cn: "官方手册", tw: "官方手冊", ja: "公式マニュアル", kr: "공식 매뉴얼", de: "Offizielles Handbuch", fr: "Manuel officiel", ru: "Официальное руководство" },
  },
  {
    key: "SumatraPDF manual (upstream)",
    byLang: {
      cn: "SumatraPDF 手册（上游）",
      tw: "SumatraPDF 手冊（上游）",
      ja: "SumatraPDF マニュアル（上流）",
      kr: "SumatraPDF 매뉴얼(업스트림)",
      de: "SumatraPDF-Handbuch (Upstream)",
      fr: "Manuel SumatraPDF (amont)",
      ru: "Руководство SumatraPDF (upstream)",
    },
  },
  {
    key: "official forums",
    byLang: { cn: "官方论坛", tw: "官方論壇", ja: "公式フォーラム", kr: "공식 포럼", de: "Offizielle Foren", fr: "Forums officiels", ru: "Официальные форумы" },
  },
  {
    key: "SumatraPDF forums (upstream)",
    byLang: {
      cn: "SumatraPDF 论坛（上游）",
      tw: "SumatraPDF 論壇（上游）",
      ja: "SumatraPDF フォーラム（上流）",
      kr: "SumatraPDF 포럼(업스트림)",
      de: "SumatraPDF-Foren (Upstream)",
      fr: "Forums SumatraPDF (amont)",
      ru: "Форумы SumatraPDF (upstream)",
    },
  },
  {
    key: "programming",
    byLang: { cn: "编程", tw: "程式設計", ja: "プログラミング", kr: "프로그래밍", de: "Programmierung", fr: "Programmation", ru: "Программирование" },
  },
  {
    key: "The Programmers",
    byLang: { cn: "程序员", tw: "程式設計師", ja: "プログラマー", kr: "프로그래머", de: "Die Programmierer", fr: "Les programmeurs", ru: "Программисты" },
  },
  {
    key: "licenses",
    byLang: { cn: "许可证", tw: "授權", ja: "ライセンス", kr: "라이선스", de: "Lizenzen", fr: "Licences", ru: "Лицензии" },
  },
  {
    key: "Various Open Source",
    byLang: {
      cn: "多种开源许可",
      tw: "多種開源授權",
      ja: "各種オープンソース",
      kr: "다양한 오픈 소스",
      de: "Verschiedene Open Source",
      fr: "Divers open source",
      ru: "Различные лицензии open source",
    },
  },
  {
    key: "last change",
    byLang: { cn: "最近更改", tw: "最近變更", ja: "最新の変更", kr: "최근 변경", de: "Letzte Änderung", fr: "Dernière modification", ru: "Последнее изменение" },
  },
  {
    key: "git commit",
    byLang: { cn: "git 提交", tw: "git 提交", ja: "git コミット", kr: "git 커밋", de: "Git-Commit", fr: "commit git", ru: "git-коммит" },
  },
  {
    key: "a note",
    byLang: { cn: "说明", tw: "說明", ja: "注記", kr: "참고", de: "Hinweis", fr: "Note", ru: "Примечание" },
  },
  {
    key: "Pre-release version, for testing only!",
    byLang: {
      cn: "预发布版本，仅供测试！",
      tw: "預發行版本，僅供測試！",
      ja: "プレリリース版、テスト専用！",
      kr: "프리릴리스 버전, 테스트 전용!",
      de: "Vorabversion, nur zum Testen!",
      fr: "Version préliminaire, pour tests uniquement !",
      ru: "Предварительная версия, только для тестирования!",
    },
  },
  {
    key: "Debug version, for testing only!",
    byLang: {
      cn: "调试版本，仅供测试！",
      tw: "偵錯版本，僅供測試！",
      ja: "デバッグ版、テスト専用！",
      kr: "디버그 버전, 테스트 전용!",
      de: "Debug-Version, nur zum Testen!",
      fr: "Version de débogage, pour tests uniquement !",
      ru: "Отладочная версия, только для тестирования!",
    },
  },
  {
    key: "Pre-release",
    byLang: { cn: "预发布", tw: "預發行", ja: "プレリリース", kr: "프리릴리스", de: "Vorabversion", fr: "Préversion", ru: "Предрелиз" },
  },
  {
    key: "64-bit",
    byLang: { cn: "64 位", tw: "64 位元", ja: "64 ビット", kr: "64비트", de: "64-Bit", fr: "64 bits", ru: "64-bit" },
  },
  {
    key: "32-bit",
    byLang: { cn: "32 位", tw: "32 位元", ja: "32 ビット", kr: "32비트", de: "32-Bit", fr: "32 bits", ru: "32-bit" },
  },
  {
    key: "(dbg)",
    byLang: { cn: "(调试)", tw: "(偵錯)", ja: "(dbg)", kr: "(dbg)", de: "(dbg)", fr: "(dbg)", ru: "(dbg)" },
  },
];

function hasBlock(lines: string[], key: string): boolean {
  const marker = ":" + key;
  return lines.some((l) => l === marker);
}

function appendBlock(lines: string[], key: string, byLang: LangMap): void {
  if (hasBlock(lines, key)) {
    console.log(`skip existing: ${key}`);
    return;
  }
  lines.push(":" + key);
  const langs = Object.keys(byLang).sort();
  for (const lang of langs) {
    lines.push(`${lang}:${byLang[lang]}`);
  }
  console.log(`added: ${key}`);
}

let lines = readFileSync(transPath, "utf8").split("\n");
for (const { key, byLang } of strings) {
  appendBlock(lines, key, byLang);
}
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
