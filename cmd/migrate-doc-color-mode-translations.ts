// One-off migration: rename Document Color Mode translation keys and update all languages.
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const transPath = join(import.meta.dir, "..", "translations", "translations.txt");

type BlockMap = Record<string, string>;

const smartByLang: BlockMap = {
  af: "Dokumentkleurmodus: Slim (pas kleure intelligent aan)",
  am: "Փաստաթղթի գունային ռեժիմ՝ խելացի (գույները խելացի հարմարեցնել)",
  ar: "وضع ألوان المستند: ذكي (تكييف الألوان بذكاء)",
  az: "Sənəd rəng rejimi: Ağıllı (rəngləri ağıllı uyğunlaşdır)",
  bg: "Цветов режим на документа: Умен (интелигентно адаптиране на цветовете)",
  bn: "ডকুমেন্ট রঙ মোড: স্মার্ট (রঙ বুদ্ধিমত্তার সাথে মানিয়ে নেওয়া)",
  br: "Modo de cor do documento: Inteligente (adaptar cores com inteligência)",
  bs: "Način boja dokumenta: Pametan (inteligentno prilagođavanje boja)",
  by: "Рэжым колеру дакумента: Разумны (разумнае прыстасаванне колераў)",
  ca: "Mode de color del document: Intel·ligent (adapta els colors amb intel·ligència)",
  "ca-xv": "Mode de color del document: Intel·ligent (adapta els colors amb intel·ligència)",
  cn: "文档颜色模式：智能（智能适配颜色）",
  co: "Modu di culore di u documentu: Intelligente (adattà i colori in modu intelligente)",
  cy: "Modd lliw dogfen: Clyfar (addasu lliwiau yn glyfar)",
  cz: "Barevný režim dokumentu: Chytrý (inteligentní úprava barev)",
  de: "Dokumentfarbmodus: Smart (Farben intelligent anpassen)",
  dk: "Dokumentfarvetilstand: Smart (tilpas farver intelligent)",
  el: "Λειτουργία χρώματος εγγράφου: Έξυπνη (έξυπνη προσαρμογή χρωμάτων)",
  es: "Modo de color del documento: Inteligente (adaptar colores con inteligencia)",
  et: "Dokumendi värvirežiim: Nutikas (värvide nutikas kohandamine)",
  eu: "Dokumentuaren kolore modua: Adimentsua (koloreak adimentsuz egokitzea)",
  fa: "حالت رنگ سند: هوشمند (تطبیق هوشمند رنگ‌ها)",
  fi: "Asiakirjan väritila: Älykäs (säädä värit älykkäästi)",
  fo: "Litmóti í skjali: Kløkt (tilpassa litir kløkt)",
  fr: "Mode couleur du document : Intelligent (adapter les couleurs intelligemment)",
  "fy-nl": "Dokumintkleurmodus: Smart (kleuren smart oanpasse)",
  ga: "Mód dath an doiciméid: Cliste (oiriúnú dathanna go cliste)",
  gl: "Modo de cor do documento: Intelixente (adaptar cores con intelixencia)",
  he: "מצב צבע מסמך: חכם (התאמת צבעים בחוכמה)",
  hi: "दस्तावेज़ रंग मोड: स्मार्ट (रंगों को बुद्धिमत्ता से अनुकूलित करें)",
  hr: "Način boja dokumenta: Pametan (inteligentno prilagođavanje boja)",
  hu: "Dokumentumszín-mód: Okos (színek intelligens igazítása)",
  id: "Mode warna dokumen: Cerdas (sesuaikan warna secara cerdas)",
  it: "Modalità colore documento: Smart (adatta i colori in modo intelligente)",
  ja: "ドキュメントのカラーモード：スマート（色を智能的に調整）",
  jv: "Mode warna dokumen: Pinter (ngatur warna kanthi pinter)",
  ka: "დოკუმენტის ფერის რეჟიმი: ჭკვიანი (ფერების ჭკვიანად მორგება)",
  kr: "문서 색상 모드: 스마트(색상을 지능적으로 조정)",
  ku: "Moda rengê belgeyê: Jîr (rengan bi jîrane lihevanî bike)",
  kw: "Mod liw an dastelhyans: Synsi (kewera liwis yn synsi)",
  lt: "Dokumento spalvų režimas: Protingas (protingai pritaikyti spalvas)",
  lv: "Dokumenta krāsu režīms: Gudrs (gudri pielāgot krāsas)",
  mk: "Режим на боја на документ: Паметен (паметно прилагодување на бои)",
  ml: "ഡോക്യുമെന്റ് നിറ മോഡ്: സ്മാർട്ട് (നിറങ്ങൾ ബുദ്ധിപൂർവ്വം ക്രമീകരിക്കുക)",
  mm: "စာရွက်စာတမ်း အရောင်မုဒ်: စမတ် (အရောင်များကို ဉာဏ်ရည်ဖြင့် ညှိ)",
  my: "Mod warna dokumen: Pintar (laras warna secara pintar)",
  ne: "कागजात रङ मोड: स्मार्ट (रङहरू बुद्धिपूर्वक मिलाउनुहोस्)",
  nl: "Documentkleurmodus: Slim (kleuren intelligent aanpassen)",
  nn: "Dokumentfargemodus: Smart (tilpass fargar intelligent)",
  no: "Dokumentfargemodus: Smart (tilpass farger intelligent)",
  pa: "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ਸਮਾਰਟ (ਰੰਗਾਂ ਨੂੰ ਸਮਾਰਟ ਢੰਗ ਨਾਲ ਅਨੁਕੂਲ ਬਣਾਓ)",
  pl: "Tryb koloru dokumentu: Smart (inteligentnie dostosuj kolory)",
  pt: "Modo de cor do documento: Inteligente (adaptar cores com inteligência)",
  ro: "Mod culoare document: Inteligent (adaptează culorile inteligent)",
  ru: "Цветовой режим документа: Умный (интеллектуальная подстройка цветов)",
  sat: "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ᱥᱢᱟᱨᱴ (ᱨᱚᱝ ᱵᱩᱫᱽᱫᱤᱭᱟᱹᱜ ᱥᱟᱯᱷᱟᱭ)",
  si: "ලේඛන වර්ණ ප්‍රකාරය: ස්මාර්ට් (වර්ණ බුද්ධිමත් ලෙස සකසන්න)",
  sk: "Farebný režim dokumentu: Smart (inteligentne prispôsobiť farby)",
  sl: "Barvni način dokumenta: Pametno (barve pametno prilagodi)",
  sn: "Maitiro emavara edokumenti: Smart (gadzirisa mavara nehungwaru)",
  "sp-rs": "Režim boja dokumenta: Pametan (inteligentno prilagođavanje boja)",
  sq: "Modaliteti i ngjyrës së dokumentit: Smart (përshtat ngjyrat në mënyrë inteligjente)",
  "sr-rs": "Режим боје документа: Паметан (паметно прилагођавање боја)",
  sv: "Dokumentfärgläge: Smart (anpassa färger intelligent)",
  ta: "ஆவண நிற முறை: ஸ்மார்ட் (நிறங்களை புத்திசாலித்தனமாக மாற்றியமை)",
  th: "โหมดสีเอกสาร: สมาร์ท (ปรับสีอย่างชาญฉลาด)",
  tl: "Color mode ng dokumento: Smart (iayon ang mga kulay nang matalino)",
  tr: "Belge renk modu: Akıllı (renkleri akıllıca uyarla)",
  tw: "文件色彩模式：智慧（智慧調整色彩）",
  uk: "Колірний режим документа: Розумний (розумне підлаштування кольорів)",
  uz: "Hujjat rang rejimi: Aqlli (ranglarni aqllona moslashtirish)",
  vn: "Chế độ màu tài liệu: Thông minh (điều chỉnh màu một cách thông minh)",
};

const originalByLang: BlockMap = {
  af: "Dokumentkleurmodus: Origineel (dokumentkleure onveranderd)",
  am: "Փաստաթղթի գունային ռեժիմ՝ բնօրինակ (փաստաթղթի գույներն անփոփոխ)",
  ar: "وضع ألوان المستند: أصلي (ألوان المستند دون تغيير)",
  az: "Sənəd rəng rejimi: Orijinal (sənəd rəngləri dəyişməz)",
  bg: "Цветов режим на документа: Оригинален (цветовете на документа непроменени)",
  bn: "ডকুমেন্ট রঙ মোড: মূল (ডকুমেন্টের রঙ অপরিবর্তিত)",
  br: "Modo de cor do documento: Original (cores do documento inalteradas)",
  bs: "Način boja dokumenta: Originalni (boje dokumenta nepromijenjene)",
  by: "Рэжым колеру дакумента: Арыгінал (колеры дакумента без змен)",
  ca: "Mode de color del document: Original (colors del document sense canvis)",
  "ca-xv": "Mode de color del document: Original (colors del document sense canvis)",
  cn: "文档颜色模式：原版（保持文档原始颜色）",
  co: "Modu di culore di u documentu: Originale (colori di u documentu senza mudifiche)",
  cy: "Modd lliw dogfen: Gwreiddiol (lliwiau'r ddogfen heb eu newid)",
  cz: "Barevný režim dokumentu: Původní (barvy dokumentu beze změny)",
  de: "Dokumentfarbmodus: Original (Dokumentfarben unverändert)",
  dk: "Dokumentfarvetilstand: Original (dokumentfarver uændrede)",
  el: "Λειτουργία χρώματος εγγράφου: Πρωτότυπη (χρώματα εγγράφου αμετάβλητα)",
  es: "Modo de color del documento: Original (colores del documento sin cambios)",
  et: "Dokumendi värvirežiim: Algne (dokumendi värvid muutmata)",
  eu: "Dokumentuaren kolore modua: Jatorrizkoa (dokumentuaren koloreak aldatu gabe)",
  fa: "حالت رنگ سند: اصلی (رنگ‌های سند بدون تغییر)",
  fi: "Asiakirjan väritila: Alkuperäinen (asiakirjan värit muuttumattomina)",
  fo: "Litmóti í skjali: Upprunalegt (litir í skjali óbroyttir)",
  fr: "Mode couleur du document : Original (couleurs du document inchangées)",
  "fy-nl": "Dokumintkleurmodus: Origenel (dokumintkleuren ûnferoare)",
  ga: "Mód dath an doiciméid: Bunaidh (dathanna an doiciméid gan athrú)",
  gl: "Modo de cor do documento: Orixinal (cores do documento sen cambios)",
  he: "מצב צבע מסמך: מקורי (צבעי המסמך ללא שינוי)",
  hi: "दस्तावेज़ रंग मोड: मूल (दस्तावेज़ के रंग अपरिवर्तित)",
  hr: "Način boja dokumenta: Izvorni (boje dokumenta nepromijenjene)",
  hu: "Dokumentumszín-mód: Eredeti (dokumentum színek változatlanul)",
  id: "Mode warna dokumen: Asli (warna dokumen tidak berubah)",
  it: "Modalità colore documento: Originale (colori del documento invariati)",
  ja: "ドキュメントのカラーモード：オリジナル（ドキュメントの色をそのまま表示）",
  jv: "Mode warna dokumen: Asli (warna dokumen ora owah)",
  ka: "დოკუმენტის ფერის რეჟიმი: ორიგინალი (დოკუმენტის ფერები უცვლელი)",
  kr: "문서 색상 모드: 원본(문서 색상 그대로)",
  ku: "Moda rengê belgeyê: Resen (rengên belgeyê bê guhartin)",
  kw: "Mod liw an dastelhyans: Oris (liwis an dastelhyans heb chanj)",
  lt: "Dokumento spalvų režimas: Originalus (dokumento spalvos nekeičiamos)",
  lv: "Dokumenta krāsu režīms: Oriģināls (dokumenta krāsas nemainītas)",
  mk: "Режим на боја на документ: Оригинален (боите на документот непроменети)",
  ml: "ഡോക്യുമെന്റ് നിറ മോഡ്: യഥാർത്ഥ (ഡോക്യുമെന്റ് നിറങ്ങൾ മാറ്റമില്ല)",
  mm: "စာရွက်စာတမ်း အရောင်မုဒ်: မူရင်း (စာရွက်စာတမ်း အရောင်များ မပြောင်း)",
  my: "Mod warna dokumen: Asal (warna dokumen tidak berubah)",
  ne: "कागजात रङ मोड: मूल (कागजातका रङहरू अपरिवर्तित)",
  nl: "Documentkleurmodus: Origineel (documentkleuren ongewijzigd)",
  nn: "Dokumentfargemodus: Original (dokumentfargar uendra)",
  no: "Dokumentfargemodus: Original (dokumentfarger uendret)",
  pa: "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ਅਸਲ (ਦਸਤਾਵੇਜ਼ ਦੇ ਰੰਗ ਬਦਲੇ ਬਿਨਾਂ)",
  pl: "Tryb koloru dokumentu: Oryginalny (kolory dokumentu bez zmian)",
  pt: "Modo de cor do documento: Original (cores do documento inalteradas)",
  ro: "Mod culoare document: Original (culorile documentului neschimbate)",
  ru: "Цветовой режим документа: Оригинал (цвета документа без изменений)",
  sat: "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ᱢᱩᱞ (ᱫᱚᱞᱤᱞ ᱨᱚᱝ ᱵᱚᱫᱚᱞ ᱵᱟᱝ)",
  si: "ලේඛන වර්ණ ප්‍රකාරය: මුල් (ලේඛන වර්ණ නොවෙනස්)",
  sk: "Farebný režim dokumentu: Pôvodný (farby dokumentu nezmenené)",
  sl: "Barvni način dokumenta: Izvirno (barve dokumenta nespremenjene)",
  sn: "Maitiro emavara edokumenti: Chokutanga (mavara edokumenti asina kuchinja)",
  "sp-rs": "Režim boja dokumenta: Originalni (boje dokumenta nepromenjene)",
  sq: "Modaliteti i ngjyrës së dokumentit: Origjinal (ngjyrat e dokumentit pa ndryshim)",
  "sr-rs": "Режим боје документа: Оригинални (боје документа непромењене)",
  sv: "Dokumentfärgläge: Original (dokumentfärger oförändrade)",
  ta: "ஆவண நிற முறை: அசல் (ஆவண நிறங்கள் மாறாமல்)",
  th: "โหมดสีเอกสาร: ต้นฉบับ (สีเอกสารไม่เปลี่ยนแปลง)",
  tl: "Color mode ng dokumento: Orihinal (hindi binabago ang kulay ng dokumento)",
  tr: "Belge renk modu: Orijinal (belge renkleri değişmeden)",
  tw: "文件色彩模式：原版（保留文件原始色彩）",
  uk: "Колірний режим документа: Оригінал (кольори документа без змін)",
  uz: "Hujjat rang rejimi: Asl (hujjat ranglari o‘zgarmasdan)",
  vn: "Chế độ màu tài liệu: Gốc (màu tài liệu không đổi)",
};

const matchThemeByLang: BlockMap = {
  af: "Dokumentkleurmodus: Volg tema (volg huidige temakleure)",
  am: "Փաստաթղթի գունային ռեժիմ՝ համապատասխանել թեմային (հետևել ընթացիկ թեմայի գույներին)",
  ar: "وضع ألوان المستند: مطابقة السمة (اتباع ألوان السمة الحالية)",
  az: "Sənəd rəng rejimi: Temaya uyğun (cari tema rənglərinə uy)",
  bg: "Цветов режим на документа: Съответствие на темата (следване на текущите цветове на темата)",
  bn: "ডকুমেন্ট রঙ মোড: থিম মিল (বর্তমান থিমের রঙ অনুসরণ)",
  br: "Modo de cor do documento: Seguir tema (seguir cores do tema atual)",
  bs: "Način boja dokumenta: Prati temu (prati boje trenutne teme)",
  by: "Рэжым колеру дакумента: Пад тему (следуючы колерам бягучай темы)",
  ca: "Mode de color del document: Segueix el tema (segueix els colors del tema actual)",
  "ca-xv": "Mode de color del document: Segueix el tema (segueix els colors del tema actual)",
  cn: "文档颜色模式：匹配主题（按当前主题着色）",
  co: "Modu di culore di u documentu: Segue u tema (seguì i colori di u tema attuale)",
  cy: "Modd lliw dogfen: Cyfateb thema (dilyn lliwiau'r thema gyfredol)",
  cz: "Barevný režim dokumentu: Podle motivu (barvy podle aktuálního motivu)",
  de: "Dokumentfarbmodus: Theme folgen (Farben des aktuellen Themes)",
  dk: "Dokumentfarvetilstand: Følg tema (følg aktuelle temafarver)",
  el: "Λειτουργία χρώματος εγγράφου: Ακολούθηση θέματος (χρώματα τρέχοντος θέματος)",
  es: "Modo de color del documento: Seguir tema (seguir colores del tema actual)",
  et: "Dokumendi värvirežiim: Järgi teemat (järgi praeguse teema värve)",
  eu: "Dokumentuaren kolore modua: Jarraitu gaia (unean gaiko koloreak)",
  fa: "حالت رنگ سند: مطابق پوسته (پیروی از رنگ‌های پوسته فعلی)",
  fi: "Asiakirjan väritila: Seuraa teemaa (noudata nykyisen teeman värejä)",
  fo: "Litmóti í skjali: Fylg tema (fylg núverandi temulitum)",
  fr: "Mode couleur du document : Suivre le thème (couleurs du thème actuel)",
  "fy-nl": "Dokumintkleurmodus: Folgje tema (kleuren fan aktuele tema)",
  ga: "Mód dath an doiciméid: Meaitseáil téama (dathanna an téama reatha)",
  gl: "Modo de cor do documento: Seguir tema (seguir cores do tema actual)",
  he: "מצב צבע מסמך: התאמה לערכת נושא (עקוב אחר צבעי ערכת הנושא)",
  hi: "दस्तावेज़ रंग मोड: थीम मिलान (वर्तमान थीम के रंग अपनाएँ)",
  hr: "Način boja dokumenta: Prati temu (prati boje trenutne teme)",
  hu: "Dokumentumszín-mód: Téma követése (aktuális téma színei)",
  id: "Mode warna dokumen: Sesuai tema (ikuti warna tema saat ini)",
  it: "Modalità colore documento: Segui tema (colori del tema corrente)",
  ja: "ドキュメントのカラーモード：テーマに合わせる（現在のテーマ色に従う）",
  jv: "Mode warna dokumen: Ngetut tema (ngetut warna tema saiki)",
  ka: "დოკუმენტის ფერის რეჟიმი: თემის მიხედვით (მიჰყვე მიმდინარე თემის ფერებს)",
  kr: "문서 색상 모드: 테마 따름(현재 테마 색상 적용)",
  ku: "Moda rengê belgeyê: Li gor temayê (rengên temaya niha bişopîne)",
  kw: "Mod liw an dastelhyans: Herens an them (heul liwis an them a-lemmyn)",
  lt: "Dokumento spalvų režimas: Pagal temą (laikytis dabartinės temos spalvų)",
  lv: "Dokumenta krāsu režīms: Pēc tēmas (sekot pašreizējās tēmas krāsām)",
  mk: "Режим на боја на документ: Според тема (следење на боите на тековната тема)",
  ml: "ഡോക്യുമെന്റ് നിറ മോഡ്: തീം പിന്തുടരുക (നിലവിലെ തീം നിറങ്ങൾ)",
  mm: "စာရွက်စာတမ်း အရောင်မုဒ်: အပြင်အမူ လိုက်နာ (လက်ရှိ အပြင်အမူ အရောင်များ)",
  my: "Mod warna dokumen: Ikut tema (ikut warna tema semasa)",
  ne: "कागजात रङ मोड: थिम अनुसार (हालको थिमका रङहरू)",
  nl: "Documentkleurmodus: Thema volgen (kleuren van huidige thema)",
  nn: "Dokumentfargemodus: Følg tema (følg gjeldande temafargar)",
  no: "Dokumentfargemodus: Følg tema (følg gjeldende temafarger)",
  pa: "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ਥੀਮ ਅਨੁਸਾਰ (ਮੌਜੂਦਾ ਥੀਮ ਦੇ ਰੰਗ)",
  pl: "Tryb koloru dokumentu: Zgodnie z motywem (kolory bieżącego motywu)",
  pt: "Modo de cor do documento: Seguir tema (seguir cores do tema atual)",
  ro: "Mod culoare document: Potrivire temă (urmează culorile temei curente)",
  ru: "Цветовой режим документа: Как в теме (цвета текущей темы)",
  sat: "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ᱛᱤᱢ ᱞᱮᱠᱷᱟ (ᱱᱤᱛᱤᱞ ᱛᱤᱢ ᱨᱚᱝ)",
  si: "ලේඛන වර්ණ ප්‍රකාරය: තේමාව අනුගමනය (වත්මන් තේමා වර්ණ)",
  sk: "Farebný režim dokumentu: Podľa motívu (farby aktuálneho motívu)",
  sl: "Barvni način dokumenta: Po temi (barve trenutne teme)",
  sn: "Maitiro emavara edokumenti: Teama inotevera (mavara etheme yazvino)",
  "sp-rs": "Režim boja dokumenta: Prati temu (prati boje trenutne teme)",
  sq: "Modaliteti i ngjyrës së dokumentit: Përputh me temën (ndiq ngjyrat e temës aktuale)",
  "sr-rs": "Режим боје документа: По теми (прати боје тренутне теме)",
  sv: "Dokumentfärgläge: Följ tema (följ aktuella temafärger)",
  ta: "ஆவண நிற முறை: தீம் பொருந்த (தற்போதைய தீம் நிறங்கள்)",
  th: "โหมดสีเอกสาร: ตามธีม (ใช้สีของธีมปัจจุบัน)",
  tl: "Color mode ng dokumento: Sunod sa tema (sundin ang kulay ng kasalukuyang tema)",
  tr: "Belge renk modu: Temaya uy (geçerli tema renklerini uygula)",
  tw: "文件色彩模式：跟隨主題（依目前主題著色）",
  uk: "Колірний режим документа: Як у темі (кольори поточної теми)",
  uz: "Hujjat rang rejimi: Mavzuga mos (joriy mavzu ranglari)",
  vn: "Chế độ màu tài liệu: Theo giao diện (theo màu giao diện hiện tại)",
};

const commandPrefixByLang: BlockMap = {
  af: "Stel dokumentkleurmodus: ",
  am: "Փաստաթղթի գունային ռեժիմ՝ ",
  ar: "تعيين وضع ألوان المستند: ",
  az: "Sənəd rəng rejimini təyin et: ",
  bg: "Задай цветов режим на документа: ",
  bn: "ডকুমেন্ট রঙ মোড সেট করুন: ",
  br: "Definir modo de cor do documento: ",
  bs: "Postavi način boja dokumenta: ",
  by: "Усталяваць рэжым колеру дакумента: ",
  ca: "Estableix mode de color del document: ",
  "ca-xv": "Estableix mode de color del document: ",
  cn: "设置文档颜色模式：",
  co: "Impostà modu di culore di u documentu: ",
  cy: "Gosod modd lliw dogfen: ",
  cz: "Nastavit barevný režim dokumentu: ",
  de: "Dokumentfarbmodus: ",
  dk: "Indstil dokumentfarvetilstand: ",
  el: "Ορισμός λειτουργίας χρώματος εγγράφου: ",
  es: "Modo de color del documento: ",
  et: "Määra dokumendi värvirežiim: ",
  eu: "Ezarri dokumentuaren kolore modua: ",
  fa: "تنظیم حالت رنگ سند: ",
  fi: "Aseta asiakirjan väritila: ",
  fo: "Set litmóti í skjali: ",
  fr: "Mode couleur du document : ",
  "fy-nl": "Dokumintkleurmodus: ",
  ga: "Socraigh mód dath an doiciméid: ",
  gl: "Modo de cor do documento: ",
  he: "הגדר מצב צבע מסמך: ",
  hi: "दस्तावेज़ रंग मोड सेट करें: ",
  hr: "Postavi način boja dokumenta: ",
  hu: "Dokumentumszín-mód: ",
  id: "Mode warna dokumen: ",
  it: "Modalità colore documento: ",
  ja: "ドキュメントのカラーモード：",
  jv: "Mode warna dokumen: ",
  ka: "დოკუმენტის ფერის რეჟიმი: ",
  kr: "문서 색상 모드: ",
  ku: "Moda rengê belgeyê: ",
  kw: "Set mod liw an dastelhyans: ",
  lt: "Dokumento spalvų režimas: ",
  lv: "Dokumenta krāsu režīms: ",
  mk: "Режим на боја на документ: ",
  ml: "ഡോക്യുമെന്റ് നിറ മോഡ്: ",
  mm: "စာရွက်စာတမ်း အရောင်မုဒ်: ",
  my: "Mod warna dokumen: ",
  ne: "कागजात रङ मोड: ",
  nl: "Documentkleurmodus: ",
  nn: "Dokumentfargemodus: ",
  no: "Dokumentfargemodus: ",
  pa: "ਦਸਤਾਵੇਜ਼ ਰੰਗ ਮੋਡ: ",
  pl: "Tryb koloru dokumentu: ",
  pt: "Modo de cor do documento: ",
  ro: "Mod culoare document: ",
  ru: "Цветовой режим документа: ",
  sat: "ᱫᱚᱞᱤᱞ ᱨᱚᱝ mod: ",
  si: "ලේඛන වර්ණ ප්‍රකාරය: ",
  sk: "Farebný režim dokumentu: ",
  sl: "Barvni način dokumenta: ",
  sn: "Seta maitiro emavara edokumenti: ",
  "sp-rs": "Režim boja dokumenta: ",
  sq: "Modaliteti i ngjyrës së dokumentit: ",
  "sr-rs": "Режим боје документа: ",
  sv: "Dokumentfärgläge: ",
  ta: "ஆவண நிற முறை: ",
  th: "โหมดสีเอกสาร: ",
  tl: "Color mode ng dokumento: ",
  tr: "Belge renk modu: ",
  tw: "設定文件色彩模式：",
  uk: "Колірний режим документа: ",
  uz: "Hujjat rang rejimi: ",
  vn: "Chế độ màu tài liệu: ",
};

function modeLabel(lang: string, byLang: BlockMap): string {
  const full = byLang[lang];
  if (!full) {
    return "";
  }
  let rest = full;
  for (const sep of ["：", ":", "՝"]) {
    const idx = rest.indexOf(sep);
    if (idx >= 0) {
      rest = rest.slice(idx + sep.length).trim();
      break;
    }
  }
  let cut = rest.length;
  for (const sep of ["(", "（"]) {
    const idx = rest.indexOf(sep);
    if (idx >= 0) {
      cut = Math.min(cut, idx);
    }
  }
  return rest.slice(0, cut).trim();
}

function buildCommandByLang(byLang: BlockMap): BlockMap {
  const out: BlockMap = {};
  for (const lang of Object.keys(byLang)) {
    const label = modeLabel(lang, byLang);
    const prefix = commandPrefixByLang[lang] ?? "Set Document Color Mode: ";
    out[lang] = `${prefix}${label}`;
  }
  return out;
}

const commandSmartByLang = buildCommandByLang(smartByLang);
const commandMatchThemeByLang = buildCommandByLang(matchThemeByLang);
const commandOriginalByLang = buildCommandByLang(originalByLang);

function rewriteBlock(lines: string[], oldKey: string, newKey: string, byLang: BlockMap): string[] {
  const out: string[] = [];
  let i = 0;
  while (i < lines.length) {
    if (lines[i] !== oldKey) {
      out.push(lines[i]);
      i++;
      continue;
    }
    out.push(newKey);
    i++;
    while (i < lines.length && !lines[i].startsWith(":")) {
      const colon = lines[i].indexOf(":");
      if (colon > 0) {
        const lang = lines[i].slice(0, colon);
        const text = byLang[lang];
        if (text) {
          out.push(`${lang}:${text}`);
        } else {
          out.push(lines[i]);
        }
      } else {
        out.push(lines[i]);
      }
      i++;
    }
  }
  return out;
}

function renameKeyOnly(lines: string[], oldKey: string, newKey: string): string[] {
  return lines.map((line) => (line === oldKey ? newKey : line));
}

const renames: { oldKey: string; newKey: string; byLang: BlockMap }[] = [
  {
    oldKey: ":Document Color Mode: Original (PDF colors unchanged)",
    newKey: ":Document Color Mode: Original (document colors unchanged)",
    byLang: originalByLang,
  },
  {
    oldKey: ":Set PDF Document Color Mode: Smart",
    newKey: ":Set Document Color Mode: Smart",
    byLang: commandSmartByLang,
  },
  {
    oldKey: ":Set PDF Document Color Mode: Match Theme",
    newKey: ":Set Document Color Mode: Match Theme",
    byLang: commandMatchThemeByLang,
  },
  {
    oldKey: ":Set PDF Document Color Mode: Original",
    newKey: ":Set Document Color Mode: Original",
    byLang: commandOriginalByLang,
  },
];

let lines = readFileSync(transPath, "utf8").split("\n");
for (const r of renames) {
  if (lines.includes(r.oldKey)) {
    lines = rewriteBlock(lines, r.oldKey, r.newKey, r.byLang);
  } else if (lines.includes(r.newKey)) {
    lines = rewriteBlock(lines, r.newKey, r.newKey, r.byLang);
  }
}
writeFileSync(transPath, lines.join("\n"), "utf8");
console.log("Updated translations/translations.txt");

function parseTranslations(d: string) {
  const lines = d.split("\n");
  const perLang = new Map<string, Map<string, string>>();
  const allStrings: string[] = [];
  let currString = "";
  for (const s of lines.slice(2)) {
    if (s.length === 0) {
      continue;
    }
    if (s.startsWith(":")) {
      currString = s.substring(1);
      allStrings.push(currString);
      continue;
    }
    const colonIdx = s.indexOf(":");
    if (colonIdx === -1) {
      continue;
    }
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
      if (trans) {
        out.push(`${lang}:${trans}`);
      }
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
