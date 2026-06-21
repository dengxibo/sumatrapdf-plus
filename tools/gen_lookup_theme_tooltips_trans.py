#!/usr/bin/env python3
"""Generate full multilingual blocks for lookup/theme toolbar tooltips."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOOD = ROOT / "translations" / "translations-good.txt"
TXT = ROOT / "translations" / "translations.txt"

LANGS = """
af am ar az bg bn br bs by ca ca-xv cn co cy cz de dk el es et eu fa fi fo fr fy-nl ga gl he hi hr hu id it ja jv ka kr ku kw lt lv mk ml mm my ne nl nn no pa pl pt ro ru sat si sk sl sn sp-rs sq sr-rs sv ta th tl tr tw uk uz vn
""".split()

KEYS = [
    "Toggle Double-Click Word Lookup",
    "Toggle &Light/Dark Theme",
    "&Dark Theme",
    "&Light Theme",
    "Double-Click Word Lookup (enabled)",
    "Double-Click Word Lookup (disabled)",
    "Toggle light/dark theme",
]

# fmt: off
WORD_TOGGLE = {
    "af": "Wissel dubbelklik-woordopsoek", "am": "Միացնել/անջատել կրկնակի սեղմումով բառային որոնում",
    "ar": "تبديل البحث عن الكلمات بالنقر المزدوج", "az": "Cüt kliklə söz axtarışını aç/bağla",
    "bg": "Превключване на търсене на думи с двойно щракване", "bn": "ডাবল-ক্লিক শব্দ অনুসন্ধান টগল করুন",
    "br": "Ativar/Desativar pesquisa por duplo clique", "bs": "Uključi/isključi pretragu riječi dvostrukim klikom",
    "by": "Пераключыць пошук слова двойчым пстрыканнем", "ca": "Commuta la cerca de paraules amb doble clic",
    "ca-xv": "Commuta cerca de paraules amb doble clic", "cn": "开启/关闭双击查词", "co": "Attivà o disattivà a ricerca di parolle cun doppiu cliccu",
    "cy": "Toglo chwilio geiriau trwy glicio dwbl", "cz": "Přepnout vyhledávání slov dvojitým kliknutím",
    "de": "Doppelklick-Wortsuche ein/aus", "dk": "Slå opslag ved dobbeltklik til/fra",
    "el": "Εναλλαγή αναζήτησης λέξης με διπλό κλικ", "es": "Alternar búsqueda de palabras con doble clic",
    "et": "Lülita topeltklõpsuga sõnaotsing", "eu": "Txandakatu hitz bilaketa klik bikoitzarekin",
    "fa": "تغییر وضعیت جستجوی واژه با دوبار کلیک", "fi": "Vaihda sanan haku kaksoisnapsautuksella",
    "fo": "Skifta orðaleiting við tveyfalt klikk", "fr": "Activer/Désactiver la recherche par double-clic",
    "fy-nl": "Dûbelklik-wurdsyk wikselje", "ga": "Scoránaigh cuardach focal le déchliceáil",
    "gl": "Alternar busca de palabras con doble clic", "he": "החלף חיפוש מילים בלחיצה כפולה",
    "hi": "डबल-क्लिक शब्द खोज टॉगल करें", "hr": "Uključi/isključi pretraživanje riječi dvostrukim klikom",
    "hu": "Dupla kattintásos szókeresés kapcsolása", "id": "Alihkan pencarian kata klik ganda",
    "it": "Attiva/Disattiva ricerca parole con doppio clic", "ja": "ダブルクリック辞書検索の切り替え",
    "jv": "Ubah panelusuran tembung klik ganda", "ka": "ორმაგი დაწკაპუნებით სიტყვის ძებნის ჩართვა/გამორთვა",
    "kr": "더블클릭 단어 찾기 켜기/끄기", "ku": "Lêgerîna peyvê bi du car klîk bike/vemirîne",
    "kw": "Kestolya hwilas gerow gans klik dewblek", "lt": "Perjungti žodžio paiešką dvigubu spustelėjimu",
    "lv": "Pārslēgt vārdu meklēšanu ar dubultklikšķi", "mk": "Вклучи/исклучи пребарување на збор со двоен клик",
    "ml": "ഇരട്ടക്ലിക്ക് വാക്ക് തിരയൽ മാറ്റുക", "mm": "စကားလုံး နှစ်ချက်နှိပ်ရှာဖွေမှု ပြောင်းလဲပါ",
    "my": "စကားလုံး နှစ်ချက်နှိပ်ရှာဖွေမှု ဖွင့်/ပိ", "ne": "डबल-क्लिक शब्द खोज टगल गर्नुहोस्",
    "nl": "Dubbelklik woordopzoeking in/uit", "nn": "Slå av/på oppslag ved dobbeltklikk",
    "no": "Slå av/på oppslag ved dobbeltklikk", "pa": "ਡਬਲ-ਕਲਿਕ ਸ਼ਬਦ ਖੋਜ ਟੌਗਲ ਕਰੋ",
    "pl": "Przełącz wyszukiwanie słów dwuklikiem", "pt": "Alternar pesquisa de palavras com duplo clique",
    "ro": "Comută căutarea cuvintelor prin dublu clic", "ru": "Вкл./выкл. поиск слова двойным щелчком",
    "sat": "ᱵᱟᱲᱟᱭ ᱠᱞᱤᱠ ᱟᱹᱨᱩ ᱥᱮᱸᱫᱽᱟ ᱟᱹᱨᱩ", "si": "දෙවරක් ක්ලික් කර වචන සෙවීම සක්‍රීය/අක්‍රීය කරන්න",
    "sk": "Prepnúť vyhľadávanie slov dvojitým kliknutím", "sl": "Preklopi iskanje besed z dvojnim klikom",
    "sn": "Shandura kutsvaga mazwi nekukanda kaviri", "sp-rs": "Uključi/isključi pretragu reči dvostrukim klikom",
    "sq": "Aktivizo/Çaktivizo kërkimin e fjalëve me klikim të dyfishtë", "sr-rs": "Укључи/искључи претрагу речи двоструким кликом",
    "sv": "Växla orduppslag med dubbelklick", "ta": "இரட்டை கிளிக் சொல் தேடலை மாற்று",
    "th": "สลับการค้นหาคำด้วยดับเบิลคลิก", "tl": "I-toggle ang paghahanap ng salita sa double-click",
    "tr": "Çift tıklama sözcük aramayı aç/kapat", "tw": "開啟/關閉雙擊查詞",
    "uk": "Увімк./вимк. пошук слова подвійним клацанням", "uz": "Ikki marta bosib so‘z qidirishni yoqish/o‘chirish",
    "vn": "Bật/tắt tra từ bằng nhấp đúp",
}

THEME_TOGGLE = {
    "af": "Wissel lig/donker tema", "am": "Միացնել/անջատել բաց/մուգ թեմա",
    "ar": "تبديل السمة الفاتحة/الداكنة", "az": "Açıq/tünd mövzunu dəyiş",
    "bg": "Превключване на светла/тъмна тема", "bn": "হালকা/গাঢ় থিম টগল করুন",
    "br": "Ativar/Desativar tema claro/escuro", "bs": "Uključi/isključi svijetlu/tamnu temu",
    "by": "Пераключыць светлую/цёмную тэму", "ca": "Commuta el tema clar/fosc",
    "ca-xv": "Commuta tema clar/fosc", "cn": "切换亮/暗主题", "co": "Attivà o disattivà u tema chjaru/scuru",
    "cy": "Toglo thema golau/tywyll", "cz": "Přepnout světlý/tmavý motiv",
    "de": "Hell/Dunkel umschalten", "dk": "Skift lyst/mørkt tema",
    "el": "Εναλλαγή φωτεινού/σκοτεινού θέματος", "es": "Alternar tema claro/oscuro",
    "et": "Lülita hele/tume teema", "eu": "Txandakatu gaia argia/iluna",
    "fa": "تغییر پوسته روشن/تیره", "fi": "Vaihda vaalea/tumma teema",
    "fo": "Skifta ljóst/myrt tema", "fr": "Activer/Désactiver thème clair/sombre",
    "fy-nl": "Ljocht/donker tema wikselje", "ga": "Scoránaigh téama geal/dorcha",
    "gl": "Alternar tema claro/escuro", "he": "החלף ערכת נושא בהירה/כהה",
    "hi": "हल्का/गहरा थीम टॉगल करें", "hr": "Uključi/isključi svijetlu/tamnu temu",
    "hu": "Világos/sötét téma kapcsolása", "id": "Alihkan tema terang/gelap",
    "it": "Commuta tema chiaro/scuro", "ja": "ライト/ダークテーマの切り替え",
    "jv": "Ganti tema terang/peteng", "ka": "ღია/მუქი თემის ჩართვა/გამორთვა",
    "kr": "밝/어두 테마 전환", "ku": "Rûkara ron/a tarî biguherîne",
    "kw": "Kestolya tewlen golow/tewl", "lt": "Perjungti šviesią/tamsią temą",
    "lv": "Pārslēgt gaišu/tumšu tēmu", "mk": "Вклучи/исклучи светла/темна тема",
    "ml": "തിളക്കമുള്ള/ഇരുണ്ട തീം മാറ്റുക", "mm": "အလင်း/အမှောင် အပြင်အဆင် ပြောင်းလဲပါ",
    "my": "အလင်း/အမှောင် အပြင်အဆင် ဖွင့်/ပိ", "ne": "उज्यालो/गाढा थिम टगल गर्नुहोस्",
    "nl": "Licht/donker thema wisselen", "nn": "Slå av/på lyst/mørkt tema",
    "no": "Slå av/på lyst/mørkt tema", "pa": "ਹਲਕਾ/ਗੂੜ੍ਹਾ ਥੀਮ ਟੌਗਲ ਕਰੋ",
    "pl": "Przełącz jasny/ciemny motyw", "pt": "Alternar tema claro/escuro",
    "ro": "Comută tema deschisă/întunecată", "ru": "Переключить светлую/тёмную тему",
    "sat": "ᱞᱟᱹᱜᱤᱡ/ᱧᱩᱛ ᱛᱷᱤᱢ ᱴᱚᱜᱚᱞ ᱢᱮ", "si": "දිවා/අඳුරු තේමාව සක්‍රීය/අක්‍රීය කරන්න",
    "sk": "Prepnúť svetlý/tmavý motív", "sl": "Preklopi svetlo/temno temo",
    "sn": "Shandura dingindira rakanaka/rima", "sp-rs": "Uključi/isključi svetlu/tamnu temu",
    "sq": "Aktivizo/Çaktivizo temën e çelët/të errët", "sr-rs": "Укључи/искључи светлу/тамну тему",
    "sv": "Växla ljust/mörkt tema", "ta": "வெளிர/இருள் தீமை மாற்று",
    "th": "สลับธีมสว่าง/มืด", "tl": "I-toggle ang light/dark theme",
    "tr": "Açık/koyu temayı aç/kapat", "tw": "切換亮/暗主題",
    "uk": "Перемкнути світлу/темну тему", "uz": "Yorug‘/qorong‘i mavzuni almashtirish",
    "vn": "Bật/tắt giao diện sáng/tối",
}

DARK_THEME = {
    "af": "Donker tema", "am": "Մուգ թեմա", "ar": "السمة الداكنة", "az": "Tünd mövzu",
    "bg": "Тъмна тема", "bn": "গাঢ় থিম", "br": "Tema escuro", "bs": "Tamna tema",
    "by": "Цёмная тэма", "ca": "Tema fosc", "ca-xv": "Tema fosc", "cn": "暗色主题",
    "co": "Tema scuru", "cy": "Thema tywyll", "cz": "Tmavý motiv", "de": "Dunkles Design",
    "dk": "Mørkt tema", "el": "Σκοτεινό θέμα", "es": "Tema oscuro", "et": "Tume teema",
    "eu": "Gai iluna", "fa": "پوسته تیره", "fi": "Tumma teema", "fo": "Myrkt tema",
    "fr": "Thème sombre", "fy-nl": "Donker tema", "ga": "Téama dorcha", "gl": "Tema escuro",
    "he": "ערכת נושא כהה", "hi": "गहरा थीम", "hr": "Tamna tema", "hu": "Sötét téma",
    "id": "Tema gelap", "it": "Tema scuro", "ja": "ダークテーマ", "jv": "Tema peteng",
    "ka": "მუქი თემა", "kr": "어두운 테마", "ku": "Rûkara tarî", "kw": "Tewlen tewl",
    "lt": "Tamsi tema", "lv": "Tumša tēma", "mk": "Темна тема", "ml": "ഇരുണ്ട തീം",
    "mm": "အမှောင် အပြင်အဆင်", "my": "အမှောင် အပြင်အဆင်", "ne": "गाढा थिम",
    "nl": "Donker thema", "nn": "Mørkt tema", "no": "Mørkt tema", "pa": "ਗੂੜ੍ਹਾ ਥੀਮ",
    "pl": "Ciemny motyw", "pt": "Tema escuro", "ro": "Temă întunecată", "ru": "Тёмная тема",
    "sat": "ᱧᱩᱛ ᱛᱷᱤᱢ", "si": "අඳුරු තේමාව", "sk": "Tmavý motív", "sl": "Temna tema",
    "sn": "Dingindira rima", "sp-rs": "Tamna tema", "sq": "Temë e errët", "sr-rs": "Тамна тема",
    "sv": "Mörkt tema", "ta": "இருள் தீம்", "th": "ธีมมืด", "tl": "Madilim na tema",
    "tr": "Koyu tema", "tw": "暗色主題", "uk": "Темна тема", "uz": "Qorong‘i mavzu",
    "vn": "Giao diện tối",
}

LIGHT_THEME = {
    "af": "Lig tema", "am": "Բաց թեմա", "ar": "السمة الفاتحة", "az": "Açıq mövzu",
    "bg": "Светла тема", "bn": "হালকা থিম", "br": "Tema claro", "bs": "Svijetla tema",
    "by": "Светлая тэма", "ca": "Tema clar", "ca-xv": "Tema clar", "cn": "亮色主题",
    "co": "Tema chjaru", "cy": "Thema golau", "cz": "Světlý motiv", "de": "Helles Design",
    "dk": "Lyst tema", "el": "Φωτεινό θέμα", "es": "Tema claro", "et": "Hele teema",
    "eu": "Gai argia", "fa": "پوسته روشن", "fi": "Vaalea teema", "fo": "Ljóst tema",
    "fr": "Thème clair", "fy-nl": "Ljocht tema", "ga": "Téama geal", "gl": "Tema claro",
    "he": "ערכת נושא בהירה", "hi": "हल्का थीम", "hr": "Svijetla tema", "hu": "Világos téma",
    "id": "Tema terang", "it": "Tema chiaro", "ja": "ライトテーマ", "jv": "Tema terang",
    "ka": "ღია თემა", "kr": "밝은 테마", "ku": "Rûkara ron", "kw": "Tewlen golow",
    "lt": "Šviesi tema", "lv": "Gaiša tēma", "mk": "Светла тема", "ml": "തിളക്കമുള്ള തീം",
    "mm": "အလင်း အပြင်အဆင်", "my": "အလင်း အပြင်အဆင်", "ne": "उज्यालो थिम",
    "nl": "Licht thema", "nn": "Lyst tema", "no": "Lyst tema", "pa": "ਹਲਕਾ ਥੀਮ",
    "pl": "Jasny motyw", "pt": "Tema claro", "ro": "Temă deschisă", "ru": "Светлая тема",
    "sat": "ᱞᱟᱹᱜᱤᱡ ᱛᱷᱤᱢ", "si": "දිවා තේමාව", "sk": "Svetlý motív", "sl": "Svetla tema",
    "sn": "Dingindira rakanaka", "sp-rs": "Svetla tema", "sq": "Temë e çelët", "sr-rs": "Светла тема",
    "sv": "Ljust tema", "ta": "வெளிர் தீம்", "th": "ธีมสว่าง", "tl": "Maliwanag na tema",
    "tr": "Açık tema", "tw": "亮色主題", "uk": "Світла тема", "uz": "Yorug‘ mavzu",
    "vn": "Giao diện sáng",
}

ENABLED = {
    "af": "Dubbelklik-woordopsoek (aan)", "am": "Կրկնակի սեղմումով բառային որոնում (միաց.)",
    "ar": "البحث بالنقر المزدوج (مفعّل)", "az": "Cüt kliklə söz axtarışı (aktiv)",
    "bg": "Търсене с двойно щракване (вкл.)", "bn": "ডাবল-ক্লিক শব্দ অনুসন্ধান (চালু)",
    "br": "Pesquisa por duplo clique (ativada)", "bs": "Pretraga dvostrukim klikom (uključeno)",
    "by": "Пошук двойчым пстрыканнем (укл.)", "ca": "Cerca per doble clic (activada)",
    "ca-xv": "Cerca doble clic (activada)", "cn": "双击查词（已开启）", "co": "Ricerca doppiu cliccu (attivata)",
    "cy": "Chwilio geiriau dwbl (wedi'i droi ymlaen)", "cz": "Vyhledávání dvojitým kliknutím (zap.)",
    "de": "Doppelklick-Wortsuche (ein)", "dk": "Opslag ved dobbeltklik (til)",
    "el": "Αναζήτηση διπλού κλικ (ενεργή)", "es": "Búsqueda con doble clic (activada)",
    "et": "Topeltklõpsuga otsing (sees)", "eu": "Klik bikoitzeko bilaketa (aktibatuta)",
    "fa": "جستجوی دوبار کلیک (فعال)",     "fi": "Kaksoisnapsautuksen haku (päällä)",
    "fo": "Orðaleiting við tveyfalt klikk (kveikt)", "fr": "Recherche par double-clic (activée)",
    "fy-nl": "Dûbelklik-wurdsyk (oan)", "ga": "Cuardach déchliceála (cumasaithe)",
    "gl": "Busca con doble clic (activada)", "he": "חיפוש בלחיצה כפולה (פעיל)",
    "hi": "डबल-क्लिक शब्द खोज (चालू)", "hr": "Pretraživanje dvostrukim klikom (uključeno)",
    "hu": "Dupla kattintásos keresés (be)", "id": "Pencarian klik ganda (aktif)",
    "it": "Ricerca con doppio clic (attiva)", "ja": "ダブルクリック辞書検索（オン）",
    "jv": "Panelusuran klik ganda (urip)", "ka": "ორმაგი დაწკაპუნების ძებნა (ჩართ.)",
    "kr": "더블클릭 단어 찾기 (켜짐)", "ku": "Lêgerîna du car klîk (vekirî)",
    "kw": "Hwilas gerow klik dewblek (byw)", "lt": "Paieška dvigubu spustelėjimu (įj.)",
    "lv": "Meklēšana ar dubultklikšķi (iesl.)", "mk": "Пребарување со двоен клик (вкл.)",
    "ml": "ഇരട്ടക്ലിക്ക് തിരയൽ (ഓൺ)", "mm": "နှစ်ချက်နှိပ်ရှာဖွေမှု (ဖွင့်)",
    "my": "နှစ်ချက်နှိပ်ရှာဖွေမှု (ဖွင့်)", "ne": "डबल-क्लिक शब्द खोज (सक्रिय)",
    "nl": "Dubbelklik woordopzoeking (aan)", "nn": "Oppslag ved dobbeltklikk (på)",
    "no": "Oppslag ved dobbeltklikk (på)", "pa": "ਡਬਲ-ਕਲਿਕ ਸ਼ਬਦ ਖੋਜ (ਚਾਲੂ)",
    "pl": "Wyszukiwanie dwuklikiem (włączone)", "pt": "Pesquisa com duplo clique (ativada)",
    "ro": "Căutare prin dublu clic (activată)", "ru": "Поиск двойным щелчком (вкл.)",
    "sat": "ᱵᱟᱲᱟᱭ ᱠᱞᱤᱠ ᱟᱹᱨᱩ (ᱪalu)", "si": "දෙවරක් ක්ලික් සෙවීම (සක්‍රීය)",
    "sk": "Vyhľadávanie dvojitým kliknutím (zap.)", "sl": "Iskanje z dvojnim klikom (vklop.)",
    "sn": "Kutsvaga nekukanda kaviri (zvakavhura)", "sp-rs": "Pretraga dvostrukim klikom (uključeno)",
    "sq": "Kërkim me klikim të dyfishtë (aktiv)", "sr-rs": "Претрага двоструким кликом (укл.)",
    "sv": "Orduppslag med dubbelklick (på)", "ta": "இரட்டை கிளிக் தேடல் (இயக்க.)",
    "th": "ค้นหาคำด้วยดับเบิลคลิก (เปิด)", "tl": "Paghahanap sa double-click (naka-on)",
    "tr": "Çift tıklama arama (açık)", "tw": "雙擊查詞（已開啟）",
    "uk": "Пошук подвійним клацанням (увімк.)", "uz": "Ikki marta bosib qidirish (yoq.)",
    "vn": "Tra từ nhấp đúp (bật)",
}

DISABLED = {
    "af": "Dubbelklik-woordopsoek (af)", "am": "Կրկնակի սեղմումով բառային որոնում (անջ.)",
    "ar": "البحث بالنقر المزدوج (معطّل)", "az": "Cüt kliklə söz axtarışı (deaktiv)",
    "bg": "Търсене с двойно щракване (изкл.)", "bn": "ডাবল-ক্লিক শব্দ অনুসন্ধান (বন্ধ)",
    "br": "Pesquisa por duplo clique (desativada)", "bs": "Pretraga dvostrukim klikom (isključeno)",
    "by": "Пошук двойчым пстрыканнем (выкл.)", "ca": "Cerca per doble clic (desactivada)",
    "ca-xv": "Cerca doble clic (desactivada)", "cn": "双击查词（已关闭）", "co": "Ricerca doppiu cliccu (disattivata)",
    "cy": "Chwilio geiriau dwbl (wedi'i droi i ffwrdd)", "cz": "Vyhledávání dvojitým kliknutím (vyp.)",
    "de": "Doppelklick-Wortsuche (aus)", "dk": "Opslag ved dobbeltklik (fra)",
    "el": "Αναζήτηση διπλού κλικ (ανενεργή)", "es": "Búsqueda con doble clic (desactivada)",
    "et": "Topeltklõpsuga otsing (väljas)", "eu": "Klik bikoitzeko bilaketa (desaktibatuta)",
    "fa": "جستجوی دوبار کلیک (غیرفعال)", "fi": "Kaksoisnapsautuksen haku (pois)",
    "fo": "Orðaleiting við tveyfalt klikk (sløkt)", "fr": "Recherche par double-clic (désactivée)",
    "fy-nl": "Dûbelklik-wurdsyk (út)", "ga": "Cuardach déchliceála (díchumasaithe)",
    "gl": "Busca con doble clic (desactivada)", "he": "חיפוש בלחיצה כפולה (כבוי)",
    "hi": "डबल-क्लिक शब्द खोज (बंद)", "hr": "Pretraživanje dvostrukim klikom (isključeno)",
    "hu": "Dupla kattintásos keresés (ki)", "id": "Pencarian klik ganda (nonaktif)",
    "it": "Ricerca con doppio clic (disattiva)", "ja": "ダブルクリック辞書検索（オフ）",
    "jv": "Panelusuran klik ganda (mati)", "ka": "ორმაგი დაწკაპუნების ძებნა (გამ.)",
    "kr": "더블클릭 단어 찾기 (꺼짐)", "ku": "Lêgerîna du car klîk (girtî)",
    "kw": "Hwilas gerow klik dewblek (marow)", "lt": "Paieška dvigubu spustelėjimu (išj.)",
    "lv": "Meklēšana ar dubultklikšķi (izsl.)", "mk": "Пребарување со двоен клик (искл.)",
    "ml": "ഇരട്ടക്ലിക്ക് തിരയൽ (ഓഫ്)", "mm": "နှစ်ချက်နှိပ်ရှာဖွေမှု (ပိ)",
    "my": "နှစ်ချက်နှိပ်ရှာဖွေမှု (ပိ)", "ne": "डबल-क्लिक शब्द खोज (निष्क्रिय)",
    "nl": "Dubbelklik woordopzoeking (uit)", "nn": "Oppslag ved dobbeltklikk (av)",
    "no": "Oppslag ved dobbeltklikk (av)", "pa": "ਡਬਲ-ਕਲਿਕ ਸ਼ਬਦ ਖੋਜ (ਬੰਦ)",
    "pl": "Wyszukiwanie dwuklikiem (wyłączone)", "pt": "Pesquisa com duplo clique (desativada)",
    "ro": "Căutare prin dublu clic (dezactivată)", "ru": "Поиск двойным щелчком (выкл.)",
    "sat": "ᱵᱟᱲᱟᱭ ᱠᱞᱤᱠ ᱟᱹᱨᱩ (band)", "si": "දෙවරක් ක්ලික් සෙවීම (අක්‍රීය)",
    "sk": "Vyhľadávanie dvojitým kliknutím (vyp.)", "sl": "Iskanje z dvojnim klikom (izklop.)",
    "sn": "Kutsvaga nekukanda kaviri (zvakavharwa)", "sp-rs": "Pretraga dvostrukim klikom (isključeno)",
    "sq": "Kërkim me klikim të dyfishtë (joaktiv)", "sr-rs": "Претрага двоструким кlikом (искл.)",
    "sv": "Orduppslag med dubbelklick (av)", "ta": "இரட்டை கிளிக் தேடல் (அண.)",
    "th": "ค้นหาคำด้วยดับเบิลคลิก (ปิด)", "tl": "Paghahanap sa double-click (naka-off)",
    "tr": "Çift tıklama arama (kapalı)", "tw": "雙擊查詞（已關閉）",
    "uk": "Пошук подвійним клацанням (вимк.)", "uz": "Ikki marta bosib qidirish (o‘ch.)",
    "vn": "Tra từ nhấp đúp (tắt)",
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


def replace_blocks(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    start_key = KEYS[0]
    end_key = ":Toggle Menu Bar"
    start = text.index(f":{start_key}")
    end = text.index(end_key, start)
    blocks = {
        KEYS[0]: WORD_TOGGLE,
        KEYS[1]: THEME_TOGGLE,
        KEYS[2]: DARK_THEME,
        KEYS[3]: LIGHT_THEME,
        KEYS[4]: ENABLED,
        KEYS[5]: DISABLED,
        KEYS[6]: THEME_TOGGLE,
    }
    new_section = "\n".join(format_block(k, blocks[k]) for k in KEYS) + "\n"
    path.write_text(text[:start] + new_section + text[end:], encoding="utf-8")


def main() -> None:
    replace_blocks(GOOD)
    replace_blocks(TXT)
    print(f"Updated translations with {len(LANGS)} languages x {len(KEYS)} strings")


if __name__ == "__main__":
    main()
