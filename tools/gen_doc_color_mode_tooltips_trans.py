#!/usr/bin/env python3
"""Generate full multilingual blocks for PDF document color mode toolbar tooltips."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOOD = ROOT / "translations" / "translations-good.txt"
TXT = ROOT / "translations" / "translations.txt"

LANGS = """
af am ar az bg bn br bs by ca ca-xv cn co cy cz de dk el es et eu fa fi fo fr fy-nl ga gl he hi hr hu id it ja jv ka kr ku kw lt lv mk ml mm my ne nl nn no pa pl pt ro ru sat si sk sl sn sp-rs sq sr-rs sv ta th tl tr tw uk uz vn
""".split()

KEYS = [
    "Cycle Document Color Mode",
    "Document Color Mode: Auto (smart dark mode)",
    "Document Color Mode: Black (full dark)",
    "Document Color Mode: Light (original colors)",
]

# fmt: off
CYCLE = {
    "af": "Wissel dokumentkleurmodus", "am": "Փոխել փաստաթղթի գունային ռեժիմը",
    "ar": "تبديل وضع ألوان المستند", "az": "Sənəd rəng rejimini dəyiş",
    "bg": "Превключване на цветовия режим на документа", "bn": "ডকুমেন্ট রঙ মোড টগল করুন",
    "br": "Alternar modo de cor do documento", "bs": "Promijeni način boja dokumenta",
    "by": "Пераключыць рэжым колеру дакумента", "ca": "Commuta el mode de color del document",
    "ca-xv": "Commuta mode de color del document", "cn": "切换文档颜色模式",
    "co": "Cambià u modu di culore di u documentu", "cy": "Toglo modd lliw dogfen",
    "cz": "Přepnout barevný režim dokumentu", "de": "Dokumentfarbmodus wechseln",
    "dk": "Skift dokumentfarvetilstand", "el": "Εναλλαγή λειτουργίας χρώματος εγγράφου",
    "es": "Alternar modo de color del documento", "et": "Vaheta dokumendi värvirežiimi",
    "eu": "Txandakatu dokumentuaren kolore modua", "fa": "تغییر حالت رنگ سند",
    "fi": "Vaihda asiakirjan väritilaa", "fo": "Skifta litmóti í skjali",
    "fr": "Changer le mode couleur du document", "fy-nl": "Dokumintkleurmodus wikselje",
    "ga": "Scoránaigh mód dath an doiciméid", "gl": "Alternar modo de cor do documento",
    "he": "החלף מצב צבע מסמך", "hi": "दस्तावेज़ रंग मोड टॉगल करें",
    "hr": "Promijeni način boja dokumenta", "hu": "Dokumentumszín-mód váltása",
    "id": "Alihkan mode warna dokumen", "it": "Commuta modalità colore documento",
    "ja": "ドキュメントのカラーモードを切り替え", "jv": "Ganti mode warna dokumen",
    "ka": "დოკუმენტის ფერის რეჟიმის შეცვლა", "kr": "문서 색상 모드 전환",
    "ku": "Moda rengê belgeyê biguherîne", "kw": "Kestolya mod liw an dastelhyans",
    "lt": "Perjungti dokumento spalvų režimą", "lv": "Pārslēgt dokumenta krāsu režīmu",
    "mk": "Промени режим на боја на документ", "ml": "ഡോക്യുമെന്റ് നിറ മോഡ് മാറ്റുക",
    "mm": "စာရွက်စာတမ်း အရောင်မုဒ် ပြောင်းလဲပါ", "my": "Dokumen mod warna tukar",
    "ne": "कागजात रङ मोड टगल गर्नुहोस्", "nl": "Documentkleurmodus wisselen",
    "nn": "Byt dokumentfargemodus", "no": "Bytt dokumentfargemodus",
    "pa": "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ ਟੌਗਲ ਕਰੋ", "pl": "Przełącz tryb koloru dokumentu",
    "pt": "Alternar modo de cor do documento", "ro": "Comută modul de culoare al documentului",
    "ru": "Переключить цветовой режим документа", "sat": "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod ᱵᱚᱫᱚᱞ",
    "si": "ලේඛන වර්ණ ප්‍රකාරය මාරු කරන්න", "sk": "Prepnúť farebný režim dokumentu",
    "sl": "Preklopi barvni način dokumenta", "sn": "Chinja maitiro emavara edokumenti",
    "sp-rs": "Promeni režim boja dokumenta", "sq": "Ndrysho modalitetin e ngjyrës së dokumentit",
    "sr-rs": "Промени режим боје документа", "sv": "Växla dokumentfärgläge",
    "ta": "ஆவண நிற முறையை மாற்று", "th": "สลับโหมดสีเอกสาร",
    "tl": "I-toggle ang color mode ng dokumento", "tr": "Belge renk modunu değiştir",
    "tw": "切換文件色彩模式", "uk": "Перемкнути колірний режим документа",
    "uz": "Hujjat rang rejimini almashtirish", "vn": "Chuyển chế độ màu tài liệu",
}

AUTO = {
    "af": "Dokumentkleurmodus: Outo (slim donker modus)", "am": "Փաստաթղթի գունային ռեժիմ՝ ավտո (խելացի մուգ)",
    "ar": "وضع ألوان المستند: تلقائي (الوضع الداكن الذكي)", "az": "Sənəd rəng rejimi: Avtomatik (ağıllı qaranlıq rejimi)",
    "bg": "Цветов режим на документа: Автоматичен (интелигентен тъмен режим)", "bn": "ডকুমেন্ট রঙ মোড: স্বয়ং (স্মার্ট ডার্ক মোড)",
    "br": "Modo de cor do documento: Automático (modo escuro inteligente)", "bs": "Način boja dokumenta: Automatski (pametan tamni način)",
    "by": "Рэжым колеру дакумента: Аўта (разумны цёмны рэжым)", "ca": "Mode de color del document: Automàtic (mode fosc intel·ligent)",
    "ca-xv": "Mode de color del document: Automàtic (mode fosc intel·ligent)", "cn": "文档颜色模式：自动（智能深色模式）",
    "co": "Modu di culore di u documentu: Automaticu (modu scuru intelligente)", "cy": "Modd lliw dogfen: Awtomatig (modd tywyll clyfar)",
    "cz": "Barevný režim dokumentu: Automatický (chytrý tmavý režim)", "de": "Dokumentfarbmodus: Auto (intelligenter Dunkelmodus)",
    "dk": "Dokumentfarvetilstand: Auto (smart mørk tilstand)", "el": "Λειτουργία χρώματος εγγράφου: Αυτόματη (έξυπνη σκοτεινή λειτουργία)",
    "es": "Modo de color del documento: Automático (modo oscuro inteligente)", "et": "Dokumendi värvirežiim: Auto (nutikas tume režiim)",
    "eu": "Dokumentuaren kolore modua: Automatikoa (modu ilun adimentsua)", "fa": "حالت رنگ سند: خودکار (حالت تیره هوشمند)",
    "fi": "Asiakirjan väritila: Automaattinen (älykäs tumma tila)", "fo": "Litmóti í skjali: Sjálvvirkandi (kløkt myrkt móti)",
    "fr": "Mode couleur du document : Auto (mode sombre intelligent)", "fy-nl": "Dokumintkleurmodus: Automatysk (smart donker modus)",
    "ga": "Mód dath an doiciméid: Uathoibríoch (mód dorcha cliste)", "gl": "Modo de cor do documento: Automático (modo escuro intelixente)",
    "he": "מצב צבע מסמך: אוטומטי (מצב כהה חכם)", "hi": "दस्तावेज़ रंग मोड: ऑटो (स्मार्ट डार्क मोड)",
    "hr": "Način boja dokumenta: Automatski (pametan tamni način)", "hu": "Dokumentumszín-mód: Automatikus (okos sötét mód)",
    "id": "Mode warna dokumen: Otomatis (mode gelap cerdas)", "it": "Modalità colore documento: Auto (modalità scura intelligente)",
    "ja": "ドキュメントのカラーモード：自動（スマートダークモード）", "jv": "Mode warna dokumen: Otomatis (mode peteng pinter)",
    "ka": "დოკუმენტის ფერის რეჟიმი: ავტო (ჭკვიანი მუქი რეჟიმი)", "kr": "문서 색상 모드: 자동(스마트 다크 모드)",
    "ku": "Moda rengê belgeyê: Otomatîk (moda tarî ya jîr)", "kw": "Mod liw an dastelhyans: Emauto (mod tewl synsi)",
    "lt": "Dokumento spalvų režimas: Automatinis (protingas tamsus režimas)", "lv": "Dokumenta krāsu režīms: Automātisks (gudrs tumšais režīms)",
    "mk": "Режим на боја на документ: Автоматски (паметен темен режим)", "ml": "ഡോക്യുമെന്റ് നിറ മോഡ്: ഓട്ടോ (സ്മാർട്ട് ഡാർക്ക് മോഡ്)",
    "mm": "စာရွက်စာတမ်း အရောင်မုဒ်: အလိုအလျောက် (စမတ် အမှောင်မုဒ်)", "my": "Mod warna dokumen: Auto (mod gelap pintar)",
    "ne": "कागजात रङ मोड: स्वचालित (स्मार्ट गाढा मोड)", "nl": "Documentkleurmodus: Automatisch (slimme donkere modus)",
    "nn": "Dokumentfargemodus: Auto (smart mørk modus)", "no": "Dokumentfargemodus: Auto (smart mørk modus)",
    "pa": "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ਆਟੋ (ਸਮਾਰਟ ਡਾਰਕ ਮੋਡ)", "pl": "Tryb koloru dokumentu: Auto (inteligentny tryb ciemny)",
    "pt": "Modo de cor do documento: Automático (modo escuro inteligente)", "ro": "Mod culoare document: Auto (mod întunecat inteligent)",
    "ru": "Цветовой режим документа: Авто (умный тёмный режим)", "sat": "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ᱟᱹᱴᱚ (ᱥᱢᱟᱨᱴ ᱧᱩᱛ mod)",
    "si": "ලේඛන වර්ණ ප්‍රකාරය: ස්වයං (ස්මාර්ට් අඳුරු ප්‍රකාරය)", "sk": "Farebný režim dokumentu: Automatický (inteligentný tmavý režim)",
    "sl": "Barvni način dokumenta: Samodejno (pameten temni način)", "sn": "Maitiro emavara edokumenti: Otomatiki (modi rima rakanaka)",
    "sp-rs": "Režim boja dokumenta: Automatski (pametan tamni režim)", "sq": "Modaliteti i ngjyrës së dokumentit: Auto (modalitet i errët inteligjent)",
    "sr-rs": "Режим боје документа: Аутоматски (паметан тамни режим)", "sv": "Dokumentfärgläge: Auto (smart mörkt läge)",
    "ta": "ஆவண நிற முறை: தானியங்கி (ஸ்மார்ட் இருள் முறை)", "th": "โหมดสีเอกสาร: อัตโนมัติ (โหมดมืดอัจฉริยะ)",
    "tl": "Color mode ng dokumento: Auto (smart dark mode)", "tr": "Belge renk modu: Otomatik (akıllı karanlık mod)",
    "tw": "文件色彩模式：自動（智慧深色模式）", "uk": "Колірний режим документа: Авто (розумний темний режим)",
    "uz": "Hujjat rang rejimi: Avto (aqlli qorong‘i rejim)", "vn": "Chế độ màu tài liệu: Tự động (chế độ tối thông minh)",
}

BLACK = {
    "af": "Dokumentkleurmodus: Swart (vol donker)", "am": "Փաստաթղթի գունային ռեժիմ՝ սև (լիովին մուգ)",
    "ar": "وضع ألوان المستند: أسود (داكن بالكامل)", "az": "Sənəd rəng rejimi: Qara (tam qaranlıq)",
    "bg": "Цветов режим на документа: Черен (пълен тъмен режим)", "bn": "ডকুমেন্ট রঙ মোড: কালো (সম্পূর্ণ ডার্ক)",
    "br": "Modo de cor do documento: Preto (escuro total)", "bs": "Način boja dokumenta: Crni (potpuno tamni način)",
    "by": "Рэжым колеру дакумента: Чорны (поўны цёмны рэжым)", "ca": "Mode de color del document: Negre (fosc complet)",
    "ca-xv": "Mode de color del document: Negre (fosc complet)", "cn": "文档颜色模式：黑色（完全深色）",
    "co": "Modu di culore di u documentu: Neru (scuru cumpletu)", "cy": "Modd lliw dogfen: Du (tywyll llawn)",
    "cz": "Barevný režim dokumentu: Černý (plně tmavý režim)", "de": "Dokumentfarbmodus: Schwarz (vollständig dunkel)",
    "dk": "Dokumentfarvetilstand: Sort (fuld mørk)", "el": "Λειτουργία χρώματος εγγράφου: Μαύρη (πλήρως σκοτεινή)",
    "es": "Modo de color del documento: Negro (oscuro total)", "et": "Dokumendi värvirežiim: Must (täielik tume)",
    "eu": "Dokumentuaren kolore modua: Beltza (ilun osoa)", "fa": "حالت رنگ سند: سیاه (تیره کامل)",
    "fi": "Asiakirjan väritila: Musta (täysin tumma)", "fo": "Litmóti í skjali: Svart (fullt myrkt)",
    "fr": "Mode couleur du document : Noir (sombre complet)", "fy-nl": "Dokumintkleurmodus: Swart (folslein donker)",
    "ga": "Mód dath an doiciméid: Dubh (dorcha iomlán)", "gl": "Modo de cor do documento: Negro (escuro total)",
    "he": "מצב צבע מסמך: שחור (כהה מלא)", "hi": "दस्तावेज़ रंग मोड: काला (पूर्ण डार्क)",
    "hr": "Način boja dokumenta: Crni (potpuno tamni način)", "hu": "Dokumentumszín-mód: Fekete (teljes sötét)",
    "id": "Mode warna dokumen: Hitam (gelap penuh)", "it": "Modalità colore documento: Nero (scuro completo)",
    "ja": "ドキュメントのカラーモード：黒（完全ダーク）", "jv": "Mode warna dokumen: Ireng (peteng penuh)",
    "ka": "დოკუმენტის ფერის რეჟიმი: შავი (სრული მუქი)", "kr": "문서 색상 모드: 검정(완전 다크)",
    "ku": "Moda rengê belgeyê: Reş (tarîya tevahî)", "kw": "Mod liw an dastelhyans: Du (tewl leun)",
    "lt": "Dokumento spalvų režimas: Juodas (visiškai tamsus)", "lv": "Dokumenta krāsu režīms: Melns (pilnīgi tumšs)",
    "mk": "Режим на боја на документ: Црн (целосно темен)", "ml": "ഡോക്യുമെന്റ് നിറ മോഡ്: കറுப்ப് (പൂർണ്ണ ഡാർക്ക്)",
    "mm": "စာရွက်စာတမ်း အရောင်မုဒ်: အနက် (အမှောင်ပြည့်)", "my": "Mod warna dokumen: Hitam (gelap penuh)",
    "ne": "कागजात रङ मोड: कालो (पूर्ण गाढा)", "nl": "Documentkleurmodus: Zwart (volledig donker)",
    "nn": "Dokumentfargemodus: Svart (full mørk)", "no": "Dokumentfargemodus: Svart (full mørk)",
    "pa": "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ਕਾਲਾ (ਪੂਰਾ ਡਾਰਕ)", "pl": "Tryb koloru dokumentu: Czarny (pełny tryb ciemny)",
    "pt": "Modo de cor do documento: Preto (escuro total)", "ro": "Mod culoare document: Negru (întunecat complet)",
    "ru": "Цветовой режим документа: Чёрный (полностью тёмный)", "sat": "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ᱠᱟᱹᱞᱤ (ᱯᱩᱨᱟᱹ ᱧᱩᱛ)",
    "si": "ලේඛන වර්ණ ප්‍රකාරය: කala (සම්පූර්ණ අඳුරු)", "sk": "Farebný režim dokumentu: Čierny (plne tmavý režim)",
    "sl": "Barvni način dokumenta: Črn (popolnoma temen)", "sn": "Maitiro emavara edokumenti: Nhema (rima zvizere)",
    "sp-rs": "Režim boja dokumenta: Crni (potpuno tamni režim)", "sq": "Modaliteti i ngjyrës së dokumentit: I zi (i errët plotësisht)",
    "sr-rs": "Режим боје документа: Црни (потпуно тамни режим)", "sv": "Dokumentfärgläge: Svart (helt mörkt)",
    "ta": "ஆவண நிற முறை: கருப்பு (முழு இருள்)", "th": "โหมดสีเอกสาร: ดำ (มืดเต็มรูปแบบ)",
    "tl": "Color mode ng dokumento: Itim (buong dark)", "tr": "Belge renk modu: Siyah (tam karanlık)",
    "tw": "文件色彩模式：黑色（完全深色）", "uk": "Колірний режим документа: Чорний (повністю темний)",
    "uz": "Hujjat rang rejimi: Qora (to‘liq qorong‘i)", "vn": "Chế độ màu tài liệu: Đen (tối hoàn toàn)",
}

LIGHT = {
    "af": "Dokumentkleurmodus: Lig (originele kleure)", "am": "Փաստաթղթի գունային ռեժիմ՝ բաց (բնօրինակ գույներ)",
    "ar": "وضع ألوان المستند: فاتح (الألوان الأصلية)", "az": "Sənəd rəng rejimi: Açıq (orijinal rənglər)",
    "bg": "Цветов режим на документа: Светъл (оригинални цветове)", "bn": "ডকুমেন্ট রঙ মোড: হালকা (মূল রঙ)",
    "br": "Modo de cor do documento: Claro (cores originais)", "bs": "Način boja dokumenta: Svijetli (originalne boje)",
    "by": "Рэжым колеру дакумента: Светлы (аўрыгінальныя колеры)", "ca": "Mode de color del document: Clar (colors originals)",
    "ca-xv": "Mode de color del document: Clar (colors originals)", "cn": "文档颜色模式：浅色（原始颜色）",
    "co": "Modu di culore di u documentu: Chjaru (colori originali)", "cy": "Modd lliw dogfen: Golau (lliwiau gwreiddiol)",
    "cz": "Barevný režim dokumentu: Světlý (původní barvy)", "de": "Dokumentfarbmodus: Hell (Originalfarben)",
    "dk": "Dokumentfarvetilstand: Lys (originale farver)", "el": "Λειτουργία χρώματος εγγράφου: Φωτεινή (αρχικά χρώματα)",
    "es": "Modo de color del documento: Claro (colores originales)", "et": "Dokumendi värvirežiim: Hele (algvärvid)",
    "eu": "Dokumentuaren kolore modua: Argia (jatorrizko koloreak)", "fa": "حالت رنگ سند: روشن (رنگ‌های اصلی)",
    "fi": "Asiakirjan väritila: Vaalea (alkuperäiset värit)", "fo": "Litmóti í skjali: Ljóst (upprunalegir litir)",
    "fr": "Mode couleur du document : Clair (couleurs originales)", "fy-nl": "Dokumintkleurmodus: Ljocht (orizinele kleuren)",
    "ga": "Mód dath an doiciméid: Geal (dathanna bunaidh)", "gl": "Modo de cor do documento: Claro (cores orixinais)",
    "he": "מצב צבע מסמך: בהיר (צבעים מקוריים)", "hi": "दस्तावेज़ रंग मोड: हल्का (मूल रंग)",
    "hr": "Način boja dokumenta: Svijetli (izvorne boje)", "hu": "Dokumentumszín-mód: Világos (eredeti színek)",
    "id": "Mode warna dokumen: Terang (warna asli)", "it": "Modalità colore documento: Chiaro (colori originali)",
    "ja": "ドキュメントのカラーモード：ライト（元の色）", "jv": "Mode warna dokumen: Terang (warna asli)",
    "ka": "დოკუმენტის ფერის რეჟიმი: ღია (ორიგინალური ფერები)", "kr": "문서 색상 모드: 밝음(원본 색상)",
    "ku": "Moda rengê belgeyê: Ron (rengên orîjînal)", "kw": "Mod liw an dastelhyans: Golow (liwori gwreyth)",
    "lt": "Dokumento spalvų režimas: Šviesus (originalios spalvos)", "lv": "Dokumenta krāsu režīms: Gaišs (sākotnējās krāsas)",
    "mk": "Режим на боја на документ: Светол (оригинални бои)", "ml": "ഡോക്യുമെന്റ് നിറ മോഡ്: ലൈറ്റ് (യഥാർത്ഥ നിറങ്ങൾ)",
    "mm": "စာရွက်စာတမ်း အရောင်မုဒ်: အလင်း (မူရင်းအရောင်များ)", "my": "Mod warna dokumen: Terang (warna asal)",
    "ne": "कागजात रङ मोड: उज्यालो (मूल रङ)", "nl": "Documentkleurmodus: Licht (originele kleuren)",
    "nn": "Dokumentfargemodus: Lys (originale fargar)", "no": "Dokumentfargemodus: Lys (originale farger)",
    "pa": "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ਹਲਕਾ (ਮੂਲ ਰੰਗ)", "pl": "Tryb koloru dokumentu: Jasny (oryginalne kolory)",
    "pt": "Modo de cor do documento: Claro (cores originais)", "ro": "Mod culoare document: Deschis (culori originale)",
    "ru": "Цветовой режим документа: Светлый (исходные цвета)", "sat": "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ᱞᱟᱹᱜᱤᱡ (ᱯᱩᱨᱟᱹ ᱨᱚᱝ)",
    "si": "ලේඛන වර්ණ ප්‍රකාරය: දිවා (මුල් වර්ණ)", "sk": "Farebný režim dokumentu: Svetlý (pôvodné farby)",
    "sl": "Barvni način dokumenta: Svetlo (izvirne barve)", "sn": "Maitiro emavara edokumenti: Chena (mavara ekutanga)",
    "sp-rs": "Režim boja dokumenta: Svetli (originalne boje)", "sq": "Modaliteti i ngjyrës së dokumentit: I çelët (ngjyrat origjinale)",
    "sr-rs": "Режим боје документа: Светли (оригиналне боје)", "sv": "Dokumentfärgläge: Ljust (originalfärger)",
    "ta": "ஆவண நிற முறை: வெளிர் (அசல் நிறங்கள்)", "th": "โหมดสีเอกสาร: สว่าง (สีต้นฉบับ)",
    "tl": "Color mode ng dokumento: Maliwanag (orihinal na kulay)", "tr": "Belge renk modu: Açık (orijinal renkler)",
    "tw": "文件色彩模式：淺色（原始色彩）", "uk": "Колірний режим документа: Світлий (оригінальні кольори)",
    "uz": "Hujjat rang rejimi: Yorug‘ (asl ranglar)", "vn": "Chế độ màu tài liệu: Sáng (màu gốc)",
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


def insert_blocks(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    marker = ":&Light Theme"
    end_marker = ":Double-Click Word Lookup (enabled)"
    start = text.index(marker)
    end = text.index(end_marker, start)
    # find end of &Light Theme block (just before end_marker)
    insert_at = end
    blocks = {
        KEYS[0]: CYCLE,
        KEYS[1]: AUTO,
        KEYS[2]: BLACK,
        KEYS[3]: LIGHT,
    }
    new_section = "\n".join(format_block(k, blocks[k]) for k in KEYS) + "\n"
    path.write_text(text[:insert_at] + new_section + text[insert_at:], encoding="utf-8")


def main() -> None:
    insert_blocks(GOOD)
    insert_blocks(TXT)
    print(f"Updated translations with {len(LANGS)} languages x {len(KEYS)} strings")


if __name__ == "__main__":
    main()
