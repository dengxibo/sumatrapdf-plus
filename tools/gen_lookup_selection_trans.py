#!/usr/bin/env python3
"""Generate multilingual blocks for selection lookup toolbar and menu strings."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOOD = ROOT / "translations" / "translations-good.txt"
TXT = ROOT / "translations" / "translations.txt"

LANGS = """
af am ar az bg bn br bs by ca ca-xv cn co cy cz de dk el es et eu fa fi fo fr fy-nl ga gl he hi hr hu id it ja jv ka kr ku kw lt lv mk ml mm my ne nl nn no pa pl pt ro ru sat si sk sl sn sp-rs sq sr-rs sv ta th tl tr tw uk uz vn
""".split()

KEYS = ["Look Up", "Look Up &Selection"]
# Legacy key removed when regenerating translations.
KEYS_TO_REMOVE = ["Look Up Selection", *KEYS]

# fmt: off
LOOK_UP = {
    "af": "Soek op", "am": "ፍለጋ", "ar": "بحث", "az": "Axtar", "bg": "Търсене", "bn": "খুঁজুন",
    "br": "Consultar", "bs": "Traži", "by": "Пошук", "ca": "Cerca", "ca-xv": "Cerca", "cn": "查词",
    "co": "Circà", "cy": "Chwilio", "cz": "Vyhledat", "de": "Nachschlagen", "dk": "Slå op",
    "el": "Αναζήτηση", "es": "Buscar", "et": "Otsi", "eu": "Bilatu", "fa": "جستجو", "fi": "Hae",
    "fo": "Leita", "fr": "Rechercher", "fy-nl": "Sykje", "ga": "Cuardaigh", "gl": "Buscar",
    "he": "חיפוש", "hi": "खोजें", "hr": "Traži", "hu": "Keresés", "id": "Cari", "it": "Cerca",
    "ja": "辞書", "jv": "Golek", "ka": "ძებნა", "kr": "찾기", "ku": "Lêgerîn", "kw": "Hwilas",
    "lt": "Ieškoti", "lv": "Meklēt", "mk": "Пребарај", "ml": "തിരയുക", "mm": "ရှာပါ", "my": "ရှာပါ",
    "ne": "खोज", "nl": "Opzoeken", "nn": "Slå opp", "no": "Slå opp", "pa": "ਖੋਜੋ", "pl": "Szukaj",
    "pt": "Consultar", "ro": "Caută", "ru": "Искать", "sat": "ᱥᱮᱸᱫᱽ", "si": "සොයන්න",
    "sk": "Hľadať", "sl": "Išči", "sn": "Tsvaga", "sp-rs": "Traži", "sq": "Kërko", "sr-rs": "Тражи",
    "sv": "Slå upp", "ta": "தேடு", "th": "ค้นหา", "tl": "Hanapin", "tr": "Ara", "tw": "查詞",
    "uk": "Шукати", "uz": "Qidirish", "vn": "Tra từ",
}

LOOK_UP_SELECTION = {
    "af": "Soek seleksie op", "am": "የተመረጠውን ፍለጋ", "ar": "بحث عن التحديد", "az": "Seçilmiş axtar",
    "bg": "Търсене на избраното", "bn": "নির্বাচিত অংশ খুঁজুন", "br": "Consultar seleção",
    "bs": "Traži izbor", "by": "Пошук вылучанага", "ca": "Cerca la selecció", "ca-xv": "Cerca selecció",
    "cn": "查词选中内容(&S)", "co": "Circà a selezzione", "cy": "Chwilio'r dewis", "cz": "Vyhledat výběr",
    "de": "Auswahl nachschlagen", "dk": "Slå markering op", "el": "Αναζήτηση επιλογής",
    "es": "Buscar selección", "et": "Otsi valikut", "eu": "Bilatu hautapena", "fa": "جستجوی انتخاب",
    "fi": "Hae valinta", "fo": "Leita val", "fr": "Rechercher la sélection", "fy-nl": "Seleksje sykje",
    "ga": "Cuardaigh an roghnú", "gl": "Buscar selección", "he": "חפש בחירה", "hi": "चयन खोजें",
    "hr": "Traži odabrano", "hu": "Kijelölés keresése", "id": "Cari pilihan", "it": "Cerca selezione",
    "ja": "選択語句を辞書検索", "jv": "Golek pilihan", "ka": "არჩეულის ძებნა", "kr": "선택어 찾기",
    "ku": "Hilbijartinê bigere", "kw": "Hwilas dewisyans", "lt": "Ieškoti pažymėto", "lv": "Meklēt atlasi",
    "mk": "Пребарај избор", "ml": "തിരഞ്ഞെടുത്തത് തിരയുക", "mm": "ရွေးချယ်ထားသည်ကို ရှာပါ",
    "my": "ရွေးချယ်ထားသည်ကို ရှာပါ", "ne": "चयन खोज", "nl": "Selectie opzoeken", "nn": "Slå opp val",
    "no": "Slå opp valg", "pa": "ਚੋਣ ਖੋਜੋ", "pl": "Wyszukaj zaznaczenie", "pt": "Consultar seleção",
    "ro": "Caută selecția", "ru": "Искать выделение", "sat": "ᱵᱟᱛᱟᱣ ᱥᱮᱸᱫᱽ", "si": "තේරීම සොයන්න",
    "sk": "Vyhľadať výber", "sl": "Išči izbor", "sn": "Tsvaga sarudzwa", "sp-rs": "Traži izbor",
    "sq": "Kërko përzgjedhjen", "sr-rs": "Тражи избор", "sv": "Slå upp markering", "ta": "தேர்வைத் தேடு",
    "th": "ค้นหาที่เลือก", "tl": "Hanapin ang napili", "tr": "Seçimi ara", "tw": "查詞選取內容(&S)",
    "uk": "Шукати виділене", "uz": "Tanlovni qidirish", "vn": "Tra nội dung đã chọn",
}
# fmt: on


def format_block(key: str, trans: dict[str, str]) -> str:
    lines = [f":{key}"]
    for lang in LANGS:
        val = trans.get(lang)
        if not val:
            raise KeyError(f"missing {lang} for {key}")
        lines.append(f"{lang}:{val}")
    return "\n".join(lines)


def append_blocks(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    for key in KEYS_TO_REMOVE:
        marker = f":{key}\n"
        if marker in text:
            start = text.index(marker)
            end = start + 1
            while end < len(text) and not (text[end] == ":" and (end == 0 or text[end - 1] == "\n")):
                end = text.index("\n", end) + 1
            text = text[:start] + text[end:]
    blocks = {
        KEYS[0]: LOOK_UP,
        KEYS[1]: LOOK_UP_SELECTION,
    }
    addition = "\n".join(format_block(k, blocks[k]) for k in KEYS) + "\n"
    if not text.endswith("\n"):
        text += "\n"
    path.write_text(text + addition, encoding="utf-8")


def main() -> None:
    append_blocks(GOOD)
    append_blocks(TXT)
    print(f"Appended {len(KEYS)} translation blocks x {len(LANGS)} languages")


if __name__ == "__main__":
    main()
