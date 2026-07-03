#!/usr/bin/env python3
"""Generate multilingual translations for Read Aloud / TTS UI."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOOD = ROOT / "translations" / "translations-good.txt"
TXT = ROOT / "translations" / "translations.txt"

LANGS = """
af am ar az bg bn br bs by ca ca-xv cn co cy cz de dk el es et eu fa fi fo fr fy-nl ga gl he hi hr hu id it ja jv ka kr ku kw lt lv mk ml mm my ne nl nn no pa pl pt ro ru sat si sk sl sn sp-rs sq sr-rs sv ta th tl tr tw uk uz vn
""".split()

KEYS = [
    "Read Aloud",
    "Pause Reading",
    "Continue Reading",
    "Stop Reading",
    "Start Reading From Top",
    "Start Reading Selection",
    "Start Reading From Cursor Position",
    "Read Aloud (TTS)",
    "Voice",
    "System default",
    "No text available to read aloud",
    "Selection",
    "From top",
    "From cursor",
    "Smart start",
    "document",
    "Reading \u00b7 %s \u00b7 page %d of %d \u00b7 %s",
    "Reading \u00b7 %s \u00b7 %s",
    "Resume",
    "Pause",
    "Stop",
    "Copying text is not allowed",
]

SPEED_KEYS = [
    "Speed",
    "Slowest (0.25×)",
    "Very slow (0.5×)",
    "Slow (0.75×)",
    "Normal (1.0×)",
    "Fast (1.25×)",
    "Faster (1.5×)",
    "Fastest (2.0×)",
]


def speed_row(
    speed: str,
    slowest: str,
    very_slow: str,
    slow: str,
    normal: str,
    fast: str,
    faster: str,
    fastest: str,
) -> tuple[str, ...]:
    return (
        speed,
        f"{slowest} (0.25×)",
        f"{very_slow} (0.5×)",
        f"{slow} (0.75×)",
        f"{normal} (1.0×)",
        f"{fast} (1.25×)",
        f"{faster} (1.5×)",
        f"{fastest} (2.0×)",
    )


SPEED_BY_LANG: dict[str, tuple[str, ...]] = {
    "af": speed_row("Spoed", "Stadigste", "Baie stadig", "Stadig", "Normaal", "Vinnig", "Vinniger", "Vinnigste"),
    "am": speed_row("Արագություն", "Ամենադանդաղ", "Շատ դանդաղ", "Դանդաղ", "Նորմալ", "Արագ", "Ավելի արագ", "Ամենաարագ"),
    "ar": speed_row("السرعة", "الأبطأ", "بطيء جداً", "بطيء", "عادي", "سريع", "أسرع", "الأسرع"),
    "az": speed_row("Sürət", "Ən yavaş", "Çox yavaş", "Yavaş", "Normal", "Sürətli", "Daha sürətli", "Ən sürətli"),
    "bg": speed_row("Скорост", "Най-бавно", "Много бавно", "Бавно", "Нормално", "Бързо", "По-бърzo", "Най-бърzo"),
    "bn": speed_row("গতি", "সবচেয়ে ধীর", "খুব ধীর", "ধীর", "স্বাভাবিক", "দ্রুত", "দ্রুততর", "সবচেয়ে দ্রুত"),
    "br": speed_row("Velocidade", "Mais lento", "Muito lento", "Lento", "Normal", "Rápido", "Mais rápido", "Mais rápido ainda"),
    "bs": speed_row("Brzina", "Najsporije", "Vrlo sporo", "Sporo", "Normalno", "Brzo", "Brže", "Najbrže"),
    "by": speed_row("Хуткасць", "Найпавольней", "Вельмі павольна", "Павольна", "Нармальная", "Хутка", "Хутчэй", "Найхутчейшая"),
    "ca": speed_row("Velocitat", "El més lent", "Molt lent", "Lent", "Normal", "Ràpid", "Més ràpid", "El més ràpid"),
    "ca-xv": speed_row("Velocitat", "El més lent", "Molt lent", "Lent", "Normal", "Ràpid", "Més ràpid", "El més ràpid"),
    "cn": speed_row("语速", "超慢", "极慢", "慢", "正常", "较快", "快", "最快"),
    "co": speed_row("Velocità", "U più lentu", "Assai lentu", "Lentu", "Normale", "Veloce", "Più veloce", "Velocissimu"),
    "cy": speed_row("Cyflymder", "Arafaf", "Araf iawn", "Araf", "Arferol", "Cyflym", "Cyflymach", "Cyflymaf"),
    "cz": speed_row("Rychlost", "Nejpomaleji", "Velmi pomalu", "Pomalu", "Normální", "Rychle", "Rychleji", "Nejrychleji"),
    "de": speed_row("Geschwindigkeit", "Am langsamsten", "Sehr langsam", "Langsam", "Normal", "Schnell", "Schneller", "Am schnellsten"),
    "dk": speed_row("Hastighed", "Langsomst", "Meget langsom", "Langsom", "Normal", "Hurtig", "Hurtigere", "Hurtigst"),
    "el": speed_row("Ταχύτητα", "Πιο αργά", "Πολύ αργά", "Αργά", "Κανονικά", "Γρήγορα", "Πιο γρήγορα", "Πιο γρήγορα ακόμα"),
    "es": speed_row("Velocidad", "Más lento", "Muy lento", "Lento", "Normal", "Rápido", "Más rápido", "Más rápido aún"),
    "et": speed_row("Kiirus", "Aeglasem", "Väga aeglane", "Aeglane", "Tavaline", "Kiire", "Kiirem", "Kiireim"),
    "eu": speed_row("Abiadura", "Motelen", "Oso motela", "Motela", "Normal", "Azkar", "Azkarrago", "Azkarrena"),
    "fa": speed_row("سرعت", "آهسته‌ترین", "خیلی آهسته", "آهسته", "عادی", "سریع", "سریع‌تر", "سریع‌ترین"),
    "fi": speed_row("Nopeus", "Hitain", "Erittäin hidas", "Hidas", "Normaali", "Nopea", "Nopeampi", "Nopein"),
    "fo": speed_row("Hasti", "Seinst", "Serliga seint", "Seint", "Vanligt", "Skjótt", "Skjótari", "Skjótast"),
    "fr": speed_row("Vitesse", "Le plus lent", "Très lent", "Lent", "Normal", "Rapide", "Plus rapide", "Le plus rapide"),
    "fy-nl": speed_row("Faas", "Stadichst", "Heul stadich", "Stadich", "Normaal", "Fluch", "Flugger", "Fluchst"),
    "ga": speed_row("Luas", "Is moille", "An-mhall", "Mall", "Gnáth", "Tapa", "Níos tapúla", "Is tapúla"),
    "gl": speed_row("Velocidade", "O máis lento", "Moi lento", "Lento", "Normal", "Rápido", "Máis rápido", "O máis rápido"),
    "he": speed_row("מהירות", "הכי איטי", "איטי מאוד", "איטי", "רגיל", "מהיר", "מהיר יותר", "הכי מהיר"),
    "hi": speed_row("गति", "सबसे धीमा", "बहुत धीमा", "धीमा", "सामान्य", "तेज़", "तेज़तर", "सबसे तेज़"),
    "hr": speed_row("Brzina", "Najsporije", "Vrlo sporo", "Sporo", "Normalno", "Brzo", "Brže", "Najbrže"),
    "hu": speed_row("Sebesség", "Leglassabb", "Nagyon lassú", "Lassú", "Normál", "Gyors", "Gyorsabb", "Leggyorsabb"),
    "id": speed_row("Kecepatan", "Paling lambat", "Sangat lambat", "Lambat", "Normal", "Cepat", "Lebih cepat", "Tercepat"),
    "it": speed_row("Velocità", "Più lento", "Molto lento", "Lento", "Normale", "Veloce", "Più veloce", "Velocissimo"),
    "ja": speed_row("速度", "最遅", "とても遅い", "遅い", "標準", "やや速い", "速い", "最速"),
    "jv": speed_row("Kacepetan", "Paling alon", "Banget alon", "Alon", "Normal", "Cepet", "Luwih cepet", "Paling cepet"),
    "ka": speed_row("სიჩქარე", "ყველაზე ნელი", "ძალიან ნელი", "ნელი", "ნორმალური", "სწრაფი", "უფრო სწრაფი", "ყველაზე სწრაფი"),
    "kr": speed_row("속도", "가장 느리게", "매우 느리게", "느리게", "보통", "빠르게", "더 빠르게", "가장 빠르게"),
    "ku": speed_row("Lezîn", "Herî hêdî", "Pir hêdî", "Hêdî", "Normal", "Bilez", "Bileztir", "Herî bilez"),
    "kw": speed_row("Sppyd", "Solabrysaf", "Mor solabrys", "Mor", "Normal", "Skwith", "Skwitha", "Skwithaf"),
    "lt": speed_row("Greitis", "Lėčiausiai", "Labai lėtai", "Lėtai", "Normalu", "Greitai", "Greičiau", "Greičiausiai"),
    "lv": speed_row("Ātrums", "Vislēnāk", "Ļoti lēni", "Lēni", "Normāli", "Ātri", "Ātrāk", "Visātrāk"),
    "mk": speed_row("Брзина", "Најбавно", "Многу бавно", "Бавно", "Нормално", "Брzo", "Побрzo", "Најбрzo"),
    "ml": speed_row("വേഗത", "ഏറ്റവും മന്ദം", "വളരെ മന്ദം", "മന്ദം", "സാധാരണ", "വേഗത്തിൽ", "കൂടുതൽ വേഗത്തിൽ", "ഏറ്റവും വേഗത്തിൽ"),
    "mm": speed_row("မြန်နှုန်း", "အနှေးဆုံး", "အလွန်နှေး", "နှေးကွေး", "ပုံမှန်", "မြန်", "ပိုမြန်", "အမြန်ဆုံး"),
    "my": speed_row("Kelajuan", "Paling perlahan", "Sangat perlahan", "Perlahan", "Normal", "Laju", "Lebih laju", "Paling laju"),
    "ne": speed_row("गति", "सबैभन्दा बिस्तारै", "धेरै बिस्तारै", "बिस्तारै", "सामान्य", "छिटो", "छिटो", "सबैभन्दा छिटो"),
    "nl": speed_row("Snelheid", "Langzaamst", "Zeer langzaam", "Langzaam", "Normaal", "Snel", "Sneller", "Snelst"),
    "nn": speed_row("Fart", "Saktast", "Veldig sakte", "Sakte", "Normal", "Rask", "Raskare", "Raskast"),
    "no": speed_row("Hastighet", "Langsomst", "Veldig langsom", "Langsom", "Normal", "Rask", "Raskere", "Raskest"),
    "pa": speed_row("ਰਫ਼ਤਾਰ", "ਸਭ ਤੋਂ ਹੌਲੀ", "ਬਹੁਤ ਹੌਲੀ", "ਹੌਲੀ", "ਸਧਾਰਨ", "ਤੇਜ਼", "ਹੋਰ ਤੇਜ਼", "ਸਭ ਤੋਂ ਤੇਜ਼"),
    "pl": speed_row("Szybkość", "Najwolniej", "Bardzo wolno", "Wolno", "Normalnie", "Szybko", "Szybciej", "Najszybciej"),
    "pt": speed_row("Velocidade", "Mais lento", "Muito lento", "Lento", "Normal", "Rápido", "Mais rápido", "Mais rápido ainda"),
    "ro": speed_row("Viteză", "Cel mai lent", "Foarte lent", "Lent", "Normal", "Rapid", "Mai rapid", "Cel mai rapid"),
    "ru": speed_row("Скорость", "Максимально медленно", "Очень медленно", "Медленно", "Обычная", "Быстро", "Быстрее", "Максимальная"),
    "sat": speed_row("Speed", "Slowest", "Very slow", "Slow", "Normal", "Fast", "Faster", "Fastest"),
    "si": speed_row("වේගය", "Slowest", "Very slow", "Slow", "Normal", "Fast", "Faster", "Fastest"),
    "sk": speed_row("Rýchlosť", "Najpomalšie", "Veľmi pomaly", "Pomalu", "Normálne", "Rýchlo", "Rýchlejšie", "Najrýchlejšie"),
    "sl": speed_row("Hitrost", "Najpočasneje", "Zelo počasi", "Počasi", "Normalno", "Hitro", "Hitreje", "Najhitreje"),
    "sn": speed_row("Speed", "Slowest", "Very slow", "Slow", "Normal", "Fast", "Faster", "Fastest"),
    "sp-rs": speed_row("Brzina", "Najsporije", "Veoma sporo", "Sporo", "Normalno", "Brzo", "Brže", "Najbrže"),
    "sq": speed_row("Shpejtësia", "Më ngadalë", "Shumë ngadalë", "Ngadalë", "Normal", "Shpejt", "Më shpejt", "Më shpejt"),
    "sr-rs": speed_row("Брzina", "Najsporije", "Veoma sporo", "Sporo", "Normalno", "Brzo", "Brže", "Najbrže"),
    "sv": speed_row("Hastighet", "Långsammast", "Mycket långsam", "Långsam", "Normal", "Snabb", "Snabbare", "Snabbast"),
    "ta": speed_row("வேகம்", "மிகக் குறைந்த வேகம்", "மிகவும் மெதுவாக", "மெதுவாக", "சாதாரணம்", "வேகமாக", "மிக வேகமாக", "அதிக வேகம்"),
    "th": speed_row("ความเร็ว", "ช้าที่สุด", "ช้ามาก", "ช้า", "ปกติ", "เร็ว", "เร็วขึ้น", "เร็วที่สุด"),
    "tl": speed_row("Bilis", "Pinakabagal", "Napakabagal", "Mabagal", "Normal", "Mabilis", "Mas mabilis", "Pinakamabilis"),
    "tr": speed_row("Hız", "En yavaş", "Çok yavaş", "Yavaş", "Normal", "Hızlı", "Daha hızlı", "En hızlı"),
    "tw": speed_row("語速", "超慢", "極慢", "慢", "正常", "較快", "快", "最快"),
    "uk": speed_row("Швидкість", "Найповільніше", "Дуже повільно", "Повільно", "Звичайна", "Швидко", "Швидше", "Максимальна"),
    "uz": speed_row("Tezlik", "Eng sekin", "Juda sekin", "Sekin", "Oddiy", "Tez", "Tezroq", "Eng tez"),
    "vn": speed_row("Tốc độ", "Chậm nhất", "Rất chậm", "Chậm", "Bình thường", "Nhanh", "Nhanh hơn", "Nhanh nhất"),
}

ALL_KEYS = KEYS + SPEED_KEYS

# Each tuple: lang code followed by translations in KEYS order.
# fmt: off
ROWS: list[tuple[str, ...]] = [
    ("af", "Lees hardop", "Lees hardop pouseer", "Gaan voort met lees", "Stop lees", "Begin lees van bo", "Lees seleksie", "Begin lees vanaf cursor", "Lees hardop (TTS)", "Stem", "Stelsel verstek", "Geen teks beskikbaar om hardop te lees nie", "Seleksie", "Van bo", "Vanaf cursor", "Slim begin", "dokument", "Lees \u00b7 %s \u00b7 bladsy %d van %d \u00b7 %s", "Lees \u00b7 %s \u00b7 %s", "Hervat", "Pouseer", "Stop", "Kopieer teks is nie toegelaat nie"),
    ("am", "Բարձրաձայն կարդալ", "Դադարեցնել կարդալը", "Շարունակել կարդալը", "Դադարեցնել կարդալը", "Սկսել կարդալ վերևից", "Կարդալ ընտրվածը", "Սկսել կարդալ կursor-ից", "Բարձրաձայն կարդալ (TTS)", "Ձայն", "Համակարգի լռելյայն", "Կարդալու տեքստ չկա", "Ընտրություն", "Վերևից", "Կursor-ից", "Խելացի սկիզբ", "փաստաթուղթ", "Կարդում \u00b7 %s \u00b7 էջ %d/%d \u00b7 %s", "Կարդում \u00b7 %s \u00b7 %s", "Շարունակել", "Դադար", "Դադարեցնել", "Տեքստի պատճենումն արգելված է"),
    ("ar", "القراءة بصوت", "إيقاف القراءة مؤقتًا", "متابعة القراءة", "إيقاف القراءة", "بدء القراءة من الأعلى", "قراءة التحديد", "بدء القراءة من المؤشر", "القراءة بصوت (TTS)", "الصوت", "الافتراضي للنظام", "لا يوجد نص للقراءة بصوت", "التحديد", "من الأعلى", "من المؤشر", "بدء ذكي", "مستند", "قراءة \u00b7 %s \u00b7 صفحة %d من %d \u00b7 %s", "قراءة \u00b7 %s \u00b7 %s", "استئناف", "إيقاف مؤقت", "إيقاف", "نسخ النص غير مسموح"),
    ("az", "Səsli oxu", "Oxumağı dayandır", "Oxumağa davam et", "Oxumağı dayandır", "Yuxarıdan oxumağa başla", "Seçimi oxu", "Kursor mövqeyindən oxumağa başla", "Səsli oxu (TTS)", "Səs", "Sistem standartı", "Oxumaq üçün mətn yoxdur", "Seçim", "Yuxarıdan", "Kursordan", "Ağıllı başlanğıc", "sənəd", "Oxunur \u00b7 %s \u00b7 səhifə %d/%d \u00b7 %s", "Oxunur \u00b7 %s \u00b7 %s", "Davam et", "Fasilə", "Dayandır", "Mətni kopyalamağa icazə verilmir"),
    ("bg", "Четене на глас", "Пауза на четенето", "Продължи четенето", "Спри четенето", "Започни четене отгоре", "Прочети избраното", "Започни четене от курсора", "Четене на глас (TTS)", "Глас", "Системно по подразбиране", "Няма текст за четене на глас", "Избор", "Отгоре", "От курсора", "Интелигентно начало", "документ", "Четене \u00b7 %s \u00b7 страница %d от %d \u00b7 %s", "Четене \u00b7 %s \u00b7 %s", "Продължи", "Пауза", "Спри", "Копирането на текст не е разрешено"),
    ("bn", "জোরে পড়ুন", "পড়া বিরতি", "পড়া চালিয়ে যান", "পড়া বন্ধ করুন", "উপর থেকে পড়া শুরু করুন", "নির্বাচিত অংশ পড়ুন", "কার্সার থেকে পড়া শুরু করুন", "জোরে পড়ুন (TTS)", "কণ্ঠ", "সিস্টেম ডিফল্ট", "পড়ার জন্য কোনো টেক্সট নেই", "নির্বাচন", "উপর থেকে", "কার্সার থেকে", "স্মার্ট শুরু", "নথি", "পড়ছে \u00b7 %s \u00b7 পৃষ্ঠা %d/%d \u00b7 %s", "পড়ছে \u00b7 %s \u00b7 %s", "পুনরায় শুরু", "বিরতি", "বন্ধ", "টেক্সট কপি অনুমোদিত নয়"),
    ("br", "Ler em voz alta", "Pausar leitura", "Continuar leitura", "Parar leitura", "Começar leitura do topo", "Ler seleção", "Começar leitura do cursor", "Ler em voz alta (TTS)", "Voz", "Padrão do sistema", "Nenhum texto disponível para leitura", "Seleção", "Do topo", "Do cursor", "Início inteligente", "documento", "Lendo \u00b7 %s \u00b7 página %d de %d \u00b7 %s", "Lendo \u00b7 %s \u00b7 %s", "Retomar", "Pausar", "Parar", "Copiar texto não é permitido"),
    ("bs", "Čitaj naglas", "Pauziraj čitanje", "Nastavi čitanje", "Zaustavi čitanje", "Počni čitanje od vrha", "Pročitaj odabir", "Počni čitanje od kursora", "Čitaj naglas (TTS)", "Glas", "Sistemski zadano", "Nema teksta za čitanje", "Odabir", "Od vrha", "Od kursora", "Pametan početak", "dokument", "Čitanje \u00b7 %s \u00b7 stranica %d od %d \u00b7 %s", "Čitanje \u00b7 %s \u00b7 %s", "Nastavi", "Pauza", "Zaustavi", "Kopiranje teksta nije dozvoljeno"),
    ("by", "Чытаць уголас", "Прыпыніць чытанне", "Працягнуць чытанне", "Спыніць чытанне", "Пачаць чытанне зверху", "Прачытай вылучанае", "Пачаць чытанне з курсора", "Чытаць уголас (TTS)", "Голас", "Сістэмнае па змаўчанні", "Няма тэксту для чытання", "Вылучэнне", "Зверху", "З курсора", "Разумны пачатак", "дакумент", "Чытанне \u00b7 %s \u00b7 старонка %d з %d \u00b7 %s", "Чытанне \u00b7 %s \u00b7 %s", "Узнавіць", "Паўза", "Спыніць", "Капіраванне тэксту забаронена"),
    ("ca", "Lectura en veu alta", "Pausa la lectura", "Continua la lectura", "Atura la lectura", "Comença a llegir des de dalt", "Llegeix la selecció", "Comença a llegir des del cursor", "Lectura en veu alta (TTS)", "Veu", "Per defecte del sistema", "No hi ha text per llegir en veu alta", "Selecció", "Des de dalt", "Des del cursor", "Inici intel·ligent", "document", "Llegint \u00b7 %s \u00b7 pàgina %d de %d \u00b7 %s", "Llegint \u00b7 %s \u00b7 %s", "Reprendre", "Pausa", "Atura", "No es permet copiar text"),
    ("ca-xv", "Lectura en veu alta", "Pausa la lectura", "Continua la lectura", "Atura la lectura", "Comença a llegir des de dalt", "Llegeix la selecció", "Comença a llegir des del cursor", "Lectura en veu alta (TTS)", "Veu", "Per defecte del sistema", "No hi ha text per llegir en veu alta", "Selecció", "Des de dalt", "Des del cursor", "Inici intel·ligent", "document", "Llegint \u00b7 %s \u00b7 pàgina %d de %d \u00b7 %s", "Llegint \u00b7 %s \u00b7 %s", "Reprendre", "Pausa", "Atura", "No es permet copiar text"),
    ("cn", "朗读", "暂停朗读", "继续朗读", "停止朗读", "从页首开始朗读", "朗读选中内容", "从光标处开始朗读", "朗读 (TTS)", "语音", "系统默认", "没有可朗读的文本", "选中内容", "从页首", "从光标", "智能开始", "文档", "朗读 \u00b7 %s \u00b7 第 %d/%d 页 \u00b7 %s", "朗读 \u00b7 %s \u00b7 %s", "继续", "暂停", "停止", "不允许复制文本"),
    ("co", "Leghje à voce alta", "Pausa a leghje", "Cuntinuà a leghje", "Fermà a leghje", "Cumincià à leghje da u principiu", "Leghje a selezzione", "Cumincià à leghje da u cursore", "Leghje à voce alta (TTS)", "Voce", "Predefinitu di u sistema", "Nisun testu dispunibule per leghje", "Selezzione", "Da u principiu", "Da u cursore", "Principiu intelligente", "documentu", "Leghje \u00b7 %s \u00b7 pagina %d di %d \u00b7 %s", "Leghje \u00b7 %s \u00b7 %s", "Riprende", "Pausa", "Ferma", "A copia di u testu ùn hè micca permessa"),
    ("cy", "Darllen yn uchel", "Oedi darllen", "Parhau i ddarllen", "Stopio darllen", "Dechrau darllen o'r brig", "Darllen y dewis", "Dechrau darllen o'r cyrnol", "Darllen yn uchel (TTS)", "Llais", "Rhagosodiad y system", "Dim testun i'w ddarllen", "Dewis", "O'r brig", "O'r cyrnol", "Dechrau smart", "dogfen", "Yn darllen \u00b7 %s \u00b7 tudalen %d o %d \u00b7 %s", "Yn darllen \u00b7 %s \u00b7 %s", "Ail-ddechrau", "Oedi", "Stopio", "Ni chaniateir copïo testun"),
    ("cz", "Přečíst nahlas", "Pozastavit čtení", "Pokračovat ve čtení", "Zastavit čtení", "Začít číst od začátku", "Přečíst výběr", "Začít číst od kurzoru", "Přečíst nahlas (TTS)", "Hlas", "Výchozí systémový", "Není k dispozici text ke čtení", "Výběr", "Od začátku", "Od kurzoru", "Chytrý start", "dokument", "Čtení \u00b7 %s \u00b7 strana %d z %d \u00b7 %s", "Čtení \u00b7 %s \u00b7 %s", "Pokračovat", "Pauza", "Zastavit", "Kopírování textu není povoleno"),
    ("de", "Vorlesen", "Lesen pausieren", "Lesen fortsetzen", "Lesen beenden", "Von oben vorlesen", "Auswahl vorlesen", "Ab Cursor vorlesen", "Vorlesen (TTS)", "Stimme", "Systemstandard", "Kein Text zum Vorlesen verfügbar", "Auswahl", "Von oben", "Ab Cursor", "Intelligenter Start", "Dokument", "Vorlesen \u00b7 %s \u00b7 Seite %d von %d \u00b7 %s", "Vorlesen \u00b7 %s \u00b7 %s", "Fortsetzen", "Pause", "Stopp", "Text kopieren ist nicht erlaubt"),
    ("dk", "Læs højt", "Pause læsning", "Fortsæt læsning", "Stop læsning", "Start læsning fra top", "Læs markering", "Start læsning fra markør", "Læs højt (TTS)", "Stemme", "Systemstandard", "Ingen tekst at læse højt", "Markering", "Fra top", "Fra markør", "Smart start", "dokument", "Læser \u00b7 %s \u00b7 side %d af %d \u00b7 %s", "Læser \u00b7 %s \u00b7 %s", "Genoptag", "Pause", "Stop", "Kopiering af tekst er ikke tilladt"),
    ("el", "Ανάγνωση φωνητικά", "Παύση ανάγνωσης", "Συνέχεια ανάγνωσης", "Διακοπή ανάγνωσης", "Έναρξη ανάγνωσης από την κορυφή", "Ανάγνωση επιλογής", "Έναρξη ανάγνωσης από τον δρομέα", "Ανάγνωση φωνητικά (TTS)", "Φωνή", "Προεπιλογή συστήματος", "Δεν υπάρχει κείμενο για ανάγνωση", "Επιλογή", "Από την κορυφή", "Από τον δρομέα", "Έξυπνη έναρξη", "έγγραφο", "Ανάγνωση \u00b7 %s \u00b7 σελίδα %d από %d \u00b7 %s", "Ανάγνωση \u00b7 %s \u00b7 %s", "Συνέχεια", "Παύση", "Διακοπή", "Η αντιγραφή κειμένου δεν επιτρέπεται"),
    ("es", "Leer en voz alta", "Pausar lectura", "Continuar lectura", "Detener lectura", "Empezar a leer desde arriba", "Leer selección", "Empezar a leer desde el cursor", "Leer en voz alta (TTS)", "Voz", "Predeterminado del sistema", "No hay texto disponible para leer", "Selección", "Desde arriba", "Desde el cursor", "Inicio inteligente", "documento", "Leyendo \u00b7 %s \u00b7 página %d de %d \u00b7 %s", "Leyendo \u00b7 %s \u00b7 %s", "Reanudar", "Pausa", "Detener", "No se permite copiar texto"),
    ("et", "Loe valjult", "Peata lugemine", "Jätka lugemist", "Lõpeta lugemine", "Alusta lugemist ülevalt", "Loe valikut", "Alusta lugemist kursorist", "Loe valjult (TTS)", "Hääl", "Süsteemi vaikimisi", "Lugemiseks pole teksti", "Valik", "Ülevalt", "Kursorist", "Nutikas algus", "dokument", "Loetakse \u00b7 %s \u00b7 leht %d/%d \u00b7 %s", "Loetakse \u00b7 %s \u00b7 %s", "Jätka", "Paus", "Lõpeta", "Teksti kopeerimine pole lubatud"),
    ("eu", "Ozen irakurri", "Irakurketa pausatu", "Jarraitu irakurtzen", "Gelditu irakurketa", "Goitik irakurtzen hasi", "Hautapena irakurri", "Kurtsorearen posiziotik hasi", "Ozen irakurri (TTS)", "Ahotsa", "Sistemaren lehenetsia", "Ez dago irakurtzeko testurik", "Hautapena", "Goitik", "Kurtsorearen posiziotik", "Hasiera adimentsua", "dokumentua", "Irakurtzen \u00b7 %s \u00b7 %d/%d orria \u00b7 %s", "Irakurtzen \u00b7 %s \u00b7 %s", "Berrekin", "Pausa", "Gelditu", "Testua kopiatzea ez da onartzen"),
    ("fa", "خواندن با صدا", "توقف موقت خواندن", "ادامه خواندن", "توقف خواندن", "شروع خواندن از بالا", "خواندن انتخاب", "شروع خواندن از مکان‌نما", "خواندن با صدا (TTS)", "صدا", "پیش‌فرض سیستم", "متنی برای خواندن موجود نیست", "انتخاب", "از بالا", "از مکان‌نما", "شروع هوشمند", "سند", "در حال خواندن \u00b7 %s \u00b7 صفحه %d از %d \u00b7 %s", "در حال خواندن \u00b7 %s \u00b7 %s", "ادامه", "مکث", "توقف", "کپی متن مجاز نیست"),
    ("fi", "Lue ääneen", "Keskeytä lukeminen", "Jatka lukemista", "Lopeta lukeminen", "Aloita lukeminen ylhäältä", "Lue valinta", "Aloita lukeminen kohdistimesta", "Lue ääneen (TTS)", "Ääni", "Järjestelmän oletus", "Ei luettavaa tekstiä", "Valinta", "Ylhäältä", "Kohdistimesta", "Älykäs aloitus", "asiakirja", "Luetaan \u00b7 %s \u00b7 sivu %d/%d \u00b7 %s", "Luetaan \u00b7 %s \u00b7 %s", "Jatka", "Tauko", "Lopeta", "Tekstin kopiointi ei ole sallittu"),
    ("fo", "Les upp", "Steðga lesing", "Halda á fram at lesa", "Steðga lesing", "Byrja at lesa frá topps", "Les val", "Byrja at lesa frá markaranum", "Les upp (TTS)", "Rødd", "Sjálvsett", "Eingin tekstur at lesa", "Val", "Frá topps", "Frá markaranum", "Smart byrjan", "skjal", "Lesur \u00b7 %s \u00b7 síða %d av %d \u00b7 %s", "Lesur \u00b7 %s \u00b7 %s", "Halda á fram", "Steðga", "Steðga", "Tað er ikki loyvt at avrita tekst"),
    ("fr", "Lecture à voix haute", "Mettre la lecture en pause", "Reprendre la lecture", "Arrêter la lecture", "Commencer la lecture depuis le haut", "Lire la sélection", "Commencer la lecture depuis le curseur", "Lecture à voix haute (TTS)", "Voix", "Par défaut du système", "Aucun texte à lire", "Sélection", "Depuis le haut", "Depuis le curseur", "Démarrage intelligent", "document", "Lecture \u00b7 %s \u00b7 page %d sur %d \u00b7 %s", "Lecture \u00b7 %s \u00b7 %s", "Reprendre", "Pause", "Arrêter", "La copie de texte n'est pas autorisée"),
    ("fy-nl", "Hardop lêze", "Lêzen pauzearje", "Trochgean mei lêzen", "Lêzen stopje", "Begjin boppe te lêzen", "Seleksje lêze", "Begjin fanôf kursor te lêzen", "Hardop lêze (TTS)", "Stim", "Systeemstandert", "Gjin tekst om te lêzen", "Seleksje", "Fan boppe", "Fan kursor", "Smart start", "dokumint", "Lêzen \u00b7 %s \u00b7 side %d fan %d \u00b7 %s", "Lêzen \u00b7 %s \u00b7 %s", "Fertsette", "Pauze", "Stopje", "Kopiearje fan tekst is net tastien"),
    ("ga", "Léigh os ard", "Cuir sos ar an léitheoireacht", "Lean ar aghaidh ag léamh", "Stop an léitheoireacht", "Tosaigh ag léamh ó bharr", "Léigh an roghnú", "Tosaigh ag léamh ón gcúrsóir", "Léigh os ard (TTS)", "Guth", "Réamhshocrú an chórais", "Níl aon téacs le léamh", "Roghnú", "Ó bharr", "Ón gcúrsóir", "Tús cliste", "doiciméad", "Ag léamh \u00b7 %s \u00b7 leathanach %d as %d \u00b7 %s", "Ag léamh \u00b7 %s \u00b7 %s", "Atosaigh", "Sos", "Stop", "Ní cheadaítear cóipeáil téacs"),
    ("gl", "Ler en voz alta", "Pausar lectura", "Continuar lectura", "Deter lectura", "Comezar a ler dende arriba", "Ler selección", "Comezar a ler dende o cursor", "Ler en voz alta (TTS)", "Voz", "Predeterminado do sistema", "Non hai texto dispoñible para ler", "Selección", "Dende arriba", "Dende o cursor", "Inicio intelixente", "documento", "Lendo \u00b7 %s \u00b7 páxina %d de %d \u00b7 %s", "Lendo \u00b7 %s \u00b7 %s", "Retomar", "Pausa", "Deter", "Non se permite copiar texto"),
    ("he", "קריאה בקול", "השהיית קריאה", "המשך קריאה", "עצירת קריאה", "התחל קריאה מלמעלה", "קרא את הבחירה", "התחל קריאה ממיקום הסמן", "קריאה בקול (TTS)", "קול", "ברירת מחדל של המערכת", "אין טקסט לקריאה", "בחירה", "מלמעלה", "מהסמן", "התחלה חכמה", "מסמך", "קורא \u00b7 %s \u00b7 עמוד %d מתוך %d \u00b7 %s", "קורא \u00b7 %s \u00b7 %s", "המשך", "השהה", "עצור", "העתקת טקסט אינה מותרת"),
    ("hi", "ज़ोर से पढ़ें", "पढ़ना रोकें", "पढ़ना जारी रखें", "पढ़ना बंद करें", "ऊपर से पढ़ना शुरू करें", "चयन पढ़ें", "कर्सर से पढ़ना शुरू करें", "ज़ोर से पढ़ें (TTS)", "आवाज़", "सिस्टम डिफ़ॉल्ट", "पढ़ने के लिए कोई पाठ नहीं", "चयन", "ऊपर से", "कर्सर से", "स्मार्ट शुरुआत", "दस्तावेज़", "पढ़ रहा है \u00b7 %s \u00b7 पृष्ठ %d/%d \u00b7 %s", "पढ़ रहा है \u00b7 %s \u00b7 %s", "फिर शुरू", "रोकें", "बंद", "पाठ कॉपी करने की अनुमति नहीं"),
    ("hr", "Čitaj naglas", "Pauziraj čitanje", "Nastavi čitanje", "Zaustavi čitanje", "Počni čitati od vrha", "Pročitaj odabir", "Počni čitati od kursora", "Čitaj naglas (TTS)", "Glas", "Sistemski zadano", "Nema teksta za čitanje", "Odabir", "Od vrha", "Od kursora", "Pametan početak", "dokument", "Čitanje \u00b7 %s \u00b7 stranica %d od %d \u00b7 %s", "Čitanje \u00b7 %s \u00b7 %s", "Nastavi", "Pauza", "Zaustavi", "Kopiranje teksta nije dopušteno"),
    ("hu", "Felolvasás", "Olvasás szüneteltetése", "Olvasás folytatása", "Olvasás leállítása", "Olvasás indítása felülről", "Kijelölés felolvasása", "Olvasás indítása a kurzortól", "Felolvasás (TTS)", "Hang", "Rendszer alapértelmezett", "Nincs felolvasható szöveg", "Kijelölés", "Felülről", "Kurzortól", "Okos indítás", "dokumentum", "Olvasás \u00b7 %s \u00b7 %d/%d. oldal \u00b7 %s", "Olvasás \u00b7 %s \u00b7 %s", "Folytatás", "Szünet", "Leállítás", "A szöveg másolása nem engedélyezett"),
    ("id", "Baca dengan suara", "Jeda pembacaan", "Lanjutkan pembacaan", "Hentikan pembacaan", "Mulai membaca dari atas", "Baca pilihan", "Mulai membaca dari kursor", "Baca dengan suara (TTS)", "Suara", "Default sistem", "Tidak ada teks untuk dibaca", "Pilihan", "Dari atas", "Dari kursor", "Mulai cerdas", "dokumen", "Membaca \u00b7 %s \u00b7 halaman %d dari %d \u00b7 %s", "Membaca \u00b7 %s \u00b7 %s", "Lanjutkan", "Jeda", "Hentikan", "Menyalin teks tidak diizinkan"),
    ("it", "Leggi ad alta voce", "Metti in pausa la lettura", "Continua la lettura", "Interrompi la lettura", "Inizia a leggere dall'inizio", "Leggi selezione", "Inizia a leggere dal cursore", "Leggi ad alta voce (TTS)", "Voce", "Predefinito di sistema", "Nessun testo da leggere", "Selezione", "Dall'inizio", "Dal cursore", "Avvio intelligente", "documento", "Lettura \u00b7 %s \u00b7 pagina %d di %d \u00b7 %s", "Lettura \u00b7 %s \u00b7 %s", "Riprendi", "Pausa", "Interrompi", "La copia del testo non è consentita"),
    ("ja", "読み上げ", "読み上げを一時停止", "読み上げを再開", "読み上げを停止", "先頭から読み上げ", "選択範囲を読み上げ", "カーソル位置から読み上げ", "読み上げ (TTS)", "音声", "システムの既定", "読み上げるテキストがありません", "選択範囲", "先頭から", "カーソルから", "スマート開始", "ドキュメント", "読み上げ中 \u00b7 %s \u00b7 %d/%d ページ \u00b7 %s", "読み上げ中 \u00b7 %s \u00b7 %s", "再開", "一時停止", "停止", "テキストのコピーは許可されていません"),
    ("jv", "Waca kanthi swara", "Ngasah maca", "Terusake maca", "Mungkasi maca", "Wiwiti maca saka ndhuwur", "Waca pilihan", "Wiwiti maca saka kursor", "Waca kanthi swara (TTS)", "Swara", "Default sistem", "Ora ana teks kanggo diwaca", "Pilihan", "Saka ndhuwur", "Saka kursor", "Wiwitan pinter", "dokumen", "Maca \u00b7 %s \u00b7 kaca %d saka %d \u00b7 %s", "Maca \u00b7 %s \u00b7 %s", "Terusake", "Ngasah", "Mungkasi", "Nyalin teks ora diidini"),
    ("ka", "ხმამაღლა წაკითხვა", "წაკითხვის паузა", "წაკითხვის გაგრძელება", "წაკითხვის შეწყვეტა", "წაკითხვა ზემოდან", "არჩეულის წაკითხვა", "წაკითხვა კურსორიდან", "ხმამაღლა წაკითხვა (TTS)", "ხმა", "სისტემის ნაგულისხმევი", "წასაკითხი ტექსტი არ არის", "არჩევა", "ზემოდან", "კურსორიდან", "ჭკვიანი დაწყება", "დოკუმენტი", "კითხვა \u00b7 %s \u00b7 გვერდი %d/%d \u00b7 %s", "კითხვა \u00b7 %s \u00b7 %s", "გაგრძელება", "პაუზა", "შეწყვეტა", "ტექსტის კოპირება აკრძალულია"),
    ("kr", "소리 내어 읽기", "읽기 일시 정지", "읽기 계속", "읽기 중지", "맨 위에서 읽기 시작", "선택 영역 읽기", "커서 위치에서 읽기 시작", "소리 내어 읽기 (TTS)", "음성", "시스템 기본값", "읽을 텍스트가 없습니다", "선택", "맨 위에서", "커서에서", "스마트 시작", "문서", "읽는 중 \u00b7 %s \u00b7 %d/%d 페이지 \u00b7 %s", "읽는 중 \u00b7 %s \u00b7 %s", "재개", "일시 정지", "중지", "텍스트 복사가 허용되지 않습니다"),
    ("ku", "Bi dengî bixwîne", "Xwendinê rawestîne", "Xwendinê bidomîne", "Xwendinê sekinîne", "Ji jorê dest pê bike", "Hilbijartinê bixwîne", "Ji cihê nîşanê dest pê bike", "Bi dengî bixwîne (TTS)", "Deng", "Defaulta pergala", "Nivîs ji bo xwendinê tune", "Hilbijartin", "Ji jorê", "Ji nîşanê", "Destpêka jîr", "belge", "Tê xwendin \u00b7 %s \u00b7 rûpel %d ji %d \u00b7 %s", "Tê xwendin \u00b7 %s \u00b7 %s", "Bidomîne", "Rawestîne", "Sekinîne", "Kopîkirina nivîsê ne destûr e"),
    ("kw", "Redya yn uhel", "Omdheg redya", "Pesya redya", "Hedhi redya", "Dalleth redya a'n penn", "Redya dewis", "Dalleth redya a'n kursor", "Redya yn uhel (TTS)", "Leuv", "Defowt an system", "Nyns eus testen dhe redya", "Dewis", "A'n penn", "A'n kursor", "Dalleth smart", "dokument", "Ow redya \u00b7 %s \u00b7 folen %d a %d \u00b7 %s", "Ow redya \u00b7 %s \u00b7 %s", "Pesya", "Omdheg", "Hedhi", "Ny aller kopi testen"),
    ("lt", "Skaityti garsiai", "Pristabdyti skaitymą", "Tęsti skaitymą", "Sustabdyti skaitymą", "Pradėti skaityti nuo viršaus", "Skaityti pažymėtą", "Pradėti skaityti nuo žymeklio", "Skaityti garsiai (TTS)", "Balsas", "Sistemos numatytasis", "Nėra teksto skaitymui", "Pažymėjimas", "Nuo viršaus", "Nuo žymeklio", "Protingas pradžia", "dokumentas", "Skaitoma \u00b7 %s \u00b7 puslapis %d iš %d \u00b7 %s", "Skaitoma \u00b7 %s \u00b7 %s", "Tęsti", "Pauzė", "Sustabdyti", "Teksto kopijavimas neleidžiamas"),
    ("lv", "Lasīt skaļi", "Pauzēt lasīšanu", "Turpināt lasīšanu", "Apturēt lasīšanu", "Sākt lasīt no augšas", "Lasīt atlasi", "Sākt lasīt no kursora", "Lasīt skaļi (TTS)", "Balss", "Sistēmas noklusējums", "Nav teksta lasīšanai", "Atlase", "No augšas", "No kursora", "Vieds sākums", "dokuments", "Lasīšana \u00b7 %s \u00b7 lapa %d no %d \u00b7 %s", "Lasīšana \u00b7 %s \u00b7 %s", "Turpināt", "Pauze", "Apturēt", "Teksta kopēšana nav atļauta"),
    ("mk", "Читај наглас", "Паузирај читање", "Продолжи читање", "Запри читање", "Почни читање од врвот", "Прочитај избор", "Почни читање од курсорот", "Читај наглас (TTS)", "Глас", "Системски стандард", "Нема текст за читање", "Избор", "Од врвот", "Од курсорот", "Паметен почеток", "документ", "Читање \u00b7 %s \u00b7 страница %d од %d \u00b7 %s", "Читање \u00b7 %s \u00b7 %s", "Продолжи", "Пауза", "Запри", "Копирање текст не е дозволено"),
    ("ml", "ശബ്ദമുള്ള വായന", "വായന നിർത്തുക", "വായന തുടരുക", "വായന നിർത്തുക", "മുകളിൽ നിന്ന് വായന ആരംഭിക്കുക", "തിരഞ്ഞെടുത്തത് വായിക്കുക", "കഴ്സർ സ്ഥാനത്ത് നിന്ന് വായന ആരംഭിക്കുക", "ശബ്ദമുള്ള വായന (TTS)", "ശബ്ദം", "സിസ്റ്റം ഡിഫോൾട്ട്", "വായിക്കാൻ ടെക്സ്റ്റ് ഇല്ല", "തിരഞ്ഞെടുപ്പ്", "മുകളിൽ നിന്ന്", "കഴ്സറിൽ നിന്ന്", "സ്മാർട്ട് ആരംഭം", "പ്രമാണം", "വായിക്കുന്നു \u00b7 %s \u00b7 പേജ് %d/%d \u00b7 %s", "വായിക്കുന്നു \u00b7 %s \u00b7 %s", "തുടരുക", "നിർത്തുക", "നിർത്തുക", "ടെക്സ്റ്റ് പകർപ്പ് അനുവദനീയമല്ല"),
    ("mm", "အသံထွက်ဖတ်ပါ", "ဖတ်ခြင်း ခဏရပ်ပါ", "ဖတ်ခြင်း ဆက်လုပ်ပါ", "ဖတ်ခြင်း ရပ်ပါ", "ထိပ်မှ ဖတ်ခြင်း စတင်ပါ", "ရွေးချယ်ထားသည်ကို ဖတ်ပါ", "ကursor နေရာမှ ဖတ်ခြင်း စတင်ပါ", "အသံထွက်ဖတ်ပါ (TTS)", "အသံ", "စနစ် ပုံသေ", "ဖတ်ရန် စာသား မရှိပါ", "ရွေးချယ်မှု", "ထိပ်မှ", "ကursor မှ", "Smart start", "စာရွက်", "ဖတ်နေသည် \u00b7 %s \u00b7 စာမျက်နှာ %d/%d \u00b7 %s", "ဖတ်နေသည် \u00b7 %s \u00b7 %s", "ဆက်လုပ်ပါ", "ခဏရပ်", "ရပ်", "စာသား ကူးယူခွင့် မရှိပါ"),
    ("my", "Baca dengan suara", "Jeda bacaan", "Sambung bacaan", "Hentikan bacaan", "Mula baca dari atas", "Baca pilihan", "Mula baca dari kursor", "Baca dengan suara (TTS)", "Suara", "Lalai sistem", "Tiada teks untuk dibaca", "Pilihan", "Dari atas", "Dari kursor", "Mula pintar", "dokumen", "Membaca \u00b7 %s \u00b7 halaman %d/%d \u00b7 %s", "Membaca \u00b7 %s \u00b7 %s", "Sambung", "Jeda", "Hentikan", "Menyalin teks tidak dibenarkan"),
    ("ne", "जोरले पढ्नुहोस्", "पढ्न रोक्नुहोस्", "पढ्न जारी राख्नुहोस्", "पढ्न बन्द गर्नुहोस्", "माथिबाट पढ्न सुरु गर्नुहोस्", "चयन पढ्नुहोस्", "कर्सरबाट पढ्न सुरु गर्नुहोस्", "जोरले पढ्नुहोस् (TTS)", "आवाज", "प्रणाली पूर्वनिर्धारित", "पढ्नका लागि पाठ छैन", "चयन", "माथिबाट", "कर्सरबाट", "स्मार्ट सुरुवात", "कागजात", "पढ्दै \u00b7 %s \u00b7 पृष्ठ %d/%d \u00b7 %s", "पढ्दै \u00b7 %s \u00b7 %s", "पुनः सुरु", "रोक", "बन्द", "पाठ प्रतिलिपि अनुमति छैन"),
    ("nl", "Voorlezen", "Lezen pauzeren", "Lezen hervatten", "Lezen stoppen", "Van boven beginnen met lezen", "Selectie voorlezen", "Van cursor beginnen met lezen", "Voorlezen (TTS)", "Stem", "Systeemstandaard", "Geen tekst om voor te lezen", "Selectie", "Van boven", "Van cursor", "Slimme start", "document", "Voorlezen \u00b7 %s \u00b7 pagina %d van %d \u00b7 %s", "Voorlezen \u00b7 %s \u00b7 %s", "Hervatten", "Pauze", "Stoppen", "Tekst kopiëren is niet toegestaan"),
    ("nn", "Les høgt", "Pause lesing", "Hald fram lesing", "Stopp lesing", "Start lesing frå toppen", "Les utval", "Start lesing frå markør", "Les høgt (TTS)", "Stemme", "Systemstandard", "Ingen tekst å lese", "Utval", "Frå toppen", "Frå markør", "Smart start", "dokument", "Les \u00b7 %s \u00b7 side %d av %d \u00b7 %s", "Les \u00b7 %s \u00b7 %s", "Hald fram", "Pause", "Stopp", "Kopiering av tekst er ikkje tillatt"),
    ("no", "Les høyt", "Pause lesing", "Fortsett lesing", "Stopp lesing", "Start lesing fra toppen", "Les utvalg", "Start lesing fra markør", "Les høyt (TTS)", "Stemme", "Systemstandard", "Ingen tekst å lese", "Utvalg", "Fra toppen", "Fra markør", "Smart start", "dokument", "Leser \u00b7 %s \u00b7 side %d av %d \u00b7 %s", "Leser \u00b7 %s \u00b7 %s", "Fortsett", "Pause", "Stopp", "Kopiering av tekst er ikke tillatt"),
    ("pa", "ਉੱਚੀ ਆਵਾਜ਼ ਵਿੱਚ ਪੜ੍ਹੋ", "ਪੜ੍ਹਨਾ ਰੋਕੋ", "ਪੜ੍ਹਨਾ ਜਾਰੀ ਰੱਖੋ", "ਪੜ੍ਹਨਾ ਬੰਦ ਕਰੋ", "ਉੱਪਰੋਂ ਪੜ੍ਹਨਾ ਸ਼ੁਰੂ ਕਰੋ", "ਚੋਣ ਪੜ੍ਹੋ", "ਕਰਸਰ ਤੋਂ ਪੜ੍ਹਨਾ ਸ਼ੁਰੂ ਕਰੋ", "ਉੱਚੀ ਆਵਾਜ਼ ਵਿੱਚ ਪੜ੍ਹੋ (TTS)", "ਆਵਾਜ਼", "ਸਿਸਟਮ ਡਿਫ਼ਾਲਟ", "ਪੜ੍ਹਨ ਲਈ ਟੈਕਸਟ ਨਹੀਂ", "ਚੋਣ", "ਉੱਪਰੋਂ", "ਕਰਸਰ ਤੋਂ", "ਸਮਾਰਟ ਸ਼ੁਰੂ", "ਦਸਤਾਵੇਜ਼", "ਪੜ੍ਹ ਰਿਹਾ ਹੈ \u00b7 %s \u00b7 ਸਫ਼ਾ %d/%d \u00b7 %s", "ਪੜ੍ਹ ਰਿਹਾ ਹੈ \u00b7 %s \u00b7 %s", "ਮੁੜ ਸ਼ੁਰੂ", "ਰੋਕੋ", "ਬੰਦ", "ਟੈਕਸਟ ਕਾਪੀ ਕਰਨ ਦੀ ਇਜਾਜ਼ਤ ਨਹੀਂ"),
    ("pl", "Czytaj na głos", "Wstrzymaj czytanie", "Kontynuuj czytanie", "Zatrzymaj czytanie", "Zacznij czytać od góry", "Czytaj zaznaczenie", "Zacznij czytać od kursora", "Czytaj na głos (TTS)", "Głos", "Domyślny systemowy", "Brak tekstu do odczytania", "Zaznaczenie", "Od góry", "Od kursora", "Inteligentny start", "dokument", "Czytanie \u00b7 %s \u00b7 strona %d z %d \u00b7 %s", "Czytanie \u00b7 %s \u00b7 %s", "Wznów", "Pauza", "Zatrzymaj", "Kopiowanie tekstu jest niedozwolone"),
    ("pt", "Ler em voz alta", "Pausar leitura", "Continuar leitura", "Parar leitura", "Começar a ler do topo", "Ler seleção", "Começar a ler do cursor", "Ler em voz alta (TTS)", "Voz", "Predefinição do sistema", "Nenhum texto disponível para leitura", "Seleção", "Do topo", "Do cursor", "Início inteligente", "documento", "A ler \u00b7 %s \u00b7 página %d de %d \u00b7 %s", "A ler \u00b7 %s \u00b7 %s", "Retomar", "Pausar", "Parar", "Copiar texto não é permitido"),
    ("ro", "Citește cu voce tare", "Pauzează citirea", "Continuă citirea", "Oprește citirea", "Începe citirea de sus", "Citește selecția", "Începe citirea de la cursor", "Citește cu voce tare (TTS)", "Voce", "Implicit sistem", "Nu există text de citit", "Selecție", "De sus", "De la cursor", "Pornire inteligentă", "document", "Citire \u00b7 %s \u00b7 pagina %d din %d \u00b7 %s", "Citire \u00b7 %s \u00b7 %s", "Reia", "Pauză", "Oprește", "Copierea textului nu este permisă"),
    ("ru", "Читать вслух", "Приостановить чтение", "Продолжить чтение", "Остановить чтение", "Начать чтение с начала", "Читать выделенное", "Начать чтение с курсора", "Читать вслух (TTS)", "Голос", "Системный по умолчанию", "Нет текста для чтения", "Выделение", "С начала", "С курсора", "Умный старт", "документ", "Чтение \u00b7 %s \u00b7 страница %d из %d \u00b7 %s", "Чтение \u00b7 %s \u00b7 %s", "Продолжить", "Пауза", "Стоп", "Копирование текста запрещено"),
    ("sat", "ᱡᱚᱨ ᱛᱮ ᱯᱟᱲᱦᱟᱣ", "ᱯᱟᱲᱦᱟᱣ ᱛᱷᱟᱹᱜ", "ᱯᱟᱲᱦᱟᱣ ᱜᱟᱹᱜᱤ", "ᱯᱟᱲᱦᱟᱣ ᱵᱚᱸᱫ", "ᱪᱮᱛᱟᱣ ᱠᱷᱚᱱ ᱯᱟᱲᱦᱟᱣ ᱮᱛᱚᱜ", "ᱵᱟᱪᱷᱟᱣ ᱯᱟᱲᱦᱟᱣ", "ᱠᱩᱨᱥᱚᱨ ᱠᱷᱚᱱ ᱯᱟᱲᱦᱟᱣ", "ᱡᱚᱨ ᱛᱮ ᱯᱟᱲᱦᱟᱣ (TTS)", "ᱥᱟᱲᱮ", "ᱥᱤᱥᱴᱟᱢ ᱰᱤᱯᱷᱚᱞᱴ", "ᱯᱟᱲᱦᱟᱣ ᱞᱟᱹᱜᱤᱫ ᱚᱱᱟ ᱵᱟᱹᱱᱤ", "ᱵᱟᱪᱷᱟᱣ", "ᱪᱮᱛᱟᱣ ᱠᱷᱚᱱ", "ᱠᱩᱨᱥᱚᱨ ᱠᱷᱚᱱ", "Smart start", "ᱫᱚᱞᱤ", "ᱯᱟᱲᱦᱟᱣ \u00b7 %s \u00b7 ᱥᱟᱦᱤᱱ %d/%d \u00b7 %s", "ᱯᱟᱲᱦᱟᱣ \u00b7 %s \u00b7 %s", "ᱜᱟᱹᱜᱤ", "ᱛᱷᱟᱹᱜ", "ᱵᱚᱸᱫ", "ᱚᱱᱟ ᱱᱚᱠᱚᱞ ᱵᱟᱭ ᱦᱩᱭ"),
    ("si", "හඬ නගා කියවන්න", "කියවීම නවත්වන්න", "කියවීම දිගටම", "කියවීම නවත්වන්න", "ඉහළින් කියවීම ආරම්භ කරන්න", "තෝරාගත් කියවන්න", "කර්සරයෙන් කියවීම ආරම්භ කරන්න", "හඬ නගා කියවන්න (TTS)", "හඬ", "පද්ධති පෙරනිමිය", "කියවීමට පෙළ නැත", "තෝරාගැනීම", "ඉහළින්", "කර්සරයෙන්", "Smart start", "ලේඛනය", "කියවමින් \u00b7 %s \u00b7 පිටු %d/%d \u00b7 %s", "කියවමින් \u00b7 %s \u00b7 %s", "නැවත", "නවත්වන්න", "නවත්වන්න", "පෙළ පිටපත් කිරීම අවසර නැත"),
    ("sk", "Prečítať nahlas", "Pozastaviť čítanie", "Pokračovať v čítaní", "Zastaviť čítanie", "Začať čítať od začiatku", "Prečítať výber", "Začať čítať od kurzora", "Prečítať nahlas (TTS)", "Hlas", "Systémové predvolené", "Nie je k dispozícii text na čítanie", "Výber", "Od začiatku", "Od kurzora", "Inteligentný štart", "dokument", "Čítanie \u00b7 %s \u00b7 strana %d z %d \u00b7 %s", "Čítanie \u00b7 %s \u00b7 %s", "Pokračovať", "Pauza", "Zastaviť", "Kopírovanie textu nie je povolené"),
    ("sl", "Glasno branje", "Premor branja", "Nadaljuj branje", "Ustavi branje", "Začni brati od vrha", "Preberi izbor", "Začni brati od kazalca", "Glasno branje (TTS)", "Glas", "Sistemsko privzeto", "Ni besedila za branje", "Izbor", "Od vrha", "Od kazalca", "Pameten začetek", "dokument", "Branje \u00b7 %s \u00b7 stran %d od %d \u00b7 %s", "Branje \u00b7 %s \u00b7 %s", "Nadaljuj", "Premor", "Ustavi", "Kopiranje besedila ni dovoljeno"),
    ("sn", "Verenga nezwi", "Misa kuverenga", "Enderera kuverenga", "Mira kuverenga", "Tanga kuverenga kubva pamusoro", "Verenga sarudzo", "Tanga kuverenga kubva pakursor", "Verenga nezwi (TTS)", "Izwi", "Default yesystem", "Hapana zvinyorwa zvekufanira kuverengwa", "Sarudzo", "Kubva pamusoro", "Kubva pakursor", "Smart start", "gwaro", "Kuverenga \u00b7 %s \u00b7 peji %d ye %d \u00b7 %s", "Kuverenga \u00b7 %s \u00b7 %s", "Enderera", "Misa", "Mira", "Kukopa zvinyorwa hazvibvumirwi"),
    ("sp-rs", "Čitaj naglas", "Pauziraj čitanje", "Nastavi čitanje", "Zaustavi čitanje", "Počni čitanje od vrha", "Pročitaj izbor", "Počni čitanje od kursora", "Čitaj naglas (TTS)", "Glas", "Sistemski podrazumevano", "Nema teksta za čitanje", "Izbor", "Od vrha", "Od kursora", "Pametan početak", "dokument", "Čitanje \u00b7 %s \u00b7 stranica %d od %d \u00b7 %s", "Čitanje \u00b7 %s \u00b7 %s", "Nastavi", "Pauza", "Zaustavi", "Kopiranje teksta nije dozvoljeno"),
    ("sq", "Lexo me zë të lartë", "Ndalo leximin", "Vazhdo leximin", "Ndalo leximin", "Fillo leximin nga lart", "Lexo përzgjedhjen", "Fillo leximin nga kursori", "Lexo me zë të lartë (TTS)", "Zëri", "Parazgjedhja e sistemit", "Nuk ka tekst për të lexuar", "Përzgjedhja", "Nga lart", "Nga kursori", "Fillim inteligjent", "dokument", "Duke lexuar \u00b7 %s \u00b7 faqja %d nga %d \u00b7 %s", "Duke lexuar \u00b7 %s \u00b7 %s", "Vazhdo", "Pauzë", "Ndalo", "Kopjimi i tekstit nuk lejohet"),
    ("sr-rs", "Читај наглас", "Паузирај читање", "Настави читање", "Заустави читање", "Почни читање од врха", "Прочитај избор", "Почни читање од курсора", "Читај наглас (TTS)", "Глас", "Системски подразумевано", "Нема текста за читање", "Избор", "Од врха", "Од курсора", "Паметан почетак", "документ", "Читање \u00b7 %s \u00b7 страница %d од %d \u00b7 %s", "Читање \u00b7 %s \u00b7 %s", "Настави", "Пауза", "Заустави", "Копирање текста није дозвољено"),
    ("sv", "Läs upp", "Pausa uppläsning", "Fortsätt läsa upp", "Stoppa uppläsning", "Börja läsa upp från toppen", "Läs markering", "Börja läsa upp från markören", "Läs upp (TTS)", "Röst", "Systemstandard", "Ingen text att läsa upp", "Markering", "Från toppen", "Från markören", "Smart start", "dokument", "Läser upp \u00b7 %s \u00b7 sida %d av %d \u00b7 %s", "Läser upp \u00b7 %s \u00b7 %s", "Fortsätt", "Paus", "Stopp", "Kopiering av text är inte tillåtet"),
    ("ta", "சத்தமாக படிக்க", "படிப்பை இடைநிறுத்து", "படிப்பை தொடர", "படிப்பை நிறுத்து", "மேலிருந்து படிக்க தொடங்கு", "தேர்வை படி", "கர்சரிலிருந்து படிக்க தொடங்கு", "சத்தமாக படிக்க (TTS)", "குரல்", "கணினி இயல்புநிலை", "படிக்க உரை இல்லை", "தேர்வு", "மேலிருந்து", "கர்சரிலிருந்து", "ஸ்மார்ட் தொடக்கம்", "ஆவணம்", "படிக்கிறது \u00b7 %s \u00b7 பக்கம் %d/%d \u00b7 %s", "படிக்கிறது \u00b7 %s \u00b7 %s", "தொடர", "இடைநிறுத்து", "நிறுத்து", "உரை நகலெடுப்பு அனுமதிக்கப்படவில்லை"),
    ("th", "อ่านออกเสียง", "หยุดอ่านชั่วคราว", "อ่านต่อ", "หยุดอ่าน", "เริ่มอ่านจากด้านบน", "อ่านส่วนที่เลือก", "เริ่มอ่านจากตำแหน่งเคอร์เซอร์", "อ่านออกเสียง (TTS)", "เสียง", "ค่าเริ่มต้นของระบบ", "ไม่มีข้อความให้อ่าน", "การเลือก", "จากด้านบน", "จากเคอร์เซอร์", "เริ่มอัจฉริยะ", "เอกสาร", "กำลังอ่าน \u00b7 %s \u00b7 หน้า %d/%d \u00b7 %s", "กำลังอ่าน \u00b7 %s \u00b7 %s", "ดำเนินต่อ", "หยุดชั่วคราว", "หยุด", "ไม่อนุญาตให้คัดลอกข้อความ"),
    ("tl", "Basahin nang malakas", "I-pause ang pagbabasa", "Ipagpatuloy ang pagbabasa", "Itigil ang pagbabasa", "Simulan ang pagbabasa mula sa itaas", "Basahin ang pinili", "Simulan ang pagbabasa mula sa cursor", "Basahin nang malakas (TTS)", "Boses", "Default ng system", "Walang tekstong mababasa", "Pinili", "Mula sa itaas", "Mula sa cursor", "Smart start", "dokumento", "Nagbabasa \u00b7 %s \u00b7 pahina %d ng %d \u00b7 %s", "Nagbabasa \u00b7 %s \u00b7 %s", "Ipagpatuloy", "I-pause", "Itigil", "Hindi pinapayagan ang pagkopya ng teksto"),
    ("tr", "Sesli oku", "Okumayı duraklat", "Okumaya devam et", "Okumayı durdur", "Yukarıdan okumaya başla", "Seçimi oku", "İmleçten okumaya başla", "Sesli oku (TTS)", "Ses", "Sistem varsayılanı", "Okunacak metin yok", "Seçim", "Yukarıdan", "İmleçten", "Akıllı başlangıç", "belge", "Okunuyor \u00b7 %s \u00b7 sayfa %d/%d \u00b7 %s", "Okunuyor \u00b7 %s \u00b7 %s", "Devam et", "Duraklat", "Durdur", "Metin kopyalamaya izin verilmiyor"),
    ("tw", "朗讀", "暫停朗讀", "繼續朗讀", "停止朗讀", "從頁首開始朗讀", "朗讀選取內容", "從游標處開始朗讀", "朗讀 (TTS)", "語音", "系統預設", "沒有可朗讀的文字", "選取內容", "從頁首", "從游標", "智慧開始", "文件", "朗讀 \u00b7 %s \u00b7 第 %d/%d 頁 \u00b7 %s", "朗讀 \u00b7 %s \u00b7 %s", "繼續", "暫停", "停止", "不允許複製文字"),
    ("uk", "Читати вголос", "Призупинити читання", "Продовжити читання", "Зупинити читання", "Почати читати зверху", "Читати виділене", "Почати читати з курсора", "Читати вголос (TTS)", "Голос", "Системний за замовчуванням", "Немає тексту для читання", "Виділення", "Зверху", "З курсора", "Розумний старт", "документ", "Читання \u00b7 %s \u00b7 сторінка %d з %d \u00b7 %s", "Читання \u00b7 %s \u00b7 %s", "Продовжити", "Пауза", "Зупинити", "Копіювання тексту заборонено"),
    ("uz", "Ovozli o'qish", "O'qishni pauza qilish", "O'qishni davom ettirish", "O'qishni to'xtatish", "Yuqoridan o'qishni boshlash", "Tanlovni o'qish", "Kursordan o'qishni boshlash", "Ovozli o'qish (TTS)", "Ovoz", "Tizim standarti", "O'qish uchun matn yo'q", "Tanlov", "Yuqoridan", "Kursordan", "Aqlli boshlash", "hujjat", "O'qilmoqda \u00b7 %s \u00b7 %d/%d bet \u00b7 %s", "O'qilmoqda \u00b7 %s \u00b7 %s", "Davom etish", "Pauza", "To'xtatish", "Matn nusxalashga ruxsat berilmaydi"),
    ("vn", "Đọc to", "Tạm dừng đọc", "Tiếp tục đọc", "Dừng đọc", "Bắt đầu đọc từ đầu", "Đọc phần chọn", "Bắt đầu đọc từ con trỏ", "Đọc to (TTS)", "Giọng", "Mặc định hệ thống", "Không có văn bản để đọc", "Lựa chọn", "Từ đầu", "Từ con trỏ", "Bắt đầu thông minh", "tài liệu", "Đang đọc \u00b7 %s \u00b7 trang %d/%d \u00b7 %s", "Đang đọc \u00b7 %s \u00b7 %s", "Tiếp tục", "Tạm dừng", "Dừng", "Không được phép sao chép văn bản"),
]
# fmt: on

PHRASES: dict[str, dict[str, str]] = {}
for row in ROWS:
    lang = row[0]
    values = row[1:]
    if len(values) != len(KEYS):
        raise ValueError(f"{lang}: expected {len(KEYS)} values, got {len(values)}")
    PHRASES[lang] = dict(zip(KEYS, values, strict=True))

if set(PHRASES) != set(LANGS):
    missing = set(LANGS) - set(PHRASES)
    extra = set(PHRASES) - set(LANGS)
    raise ValueError(f"missing langs: {sorted(missing)} extra: {sorted(extra)}")

for lang in LANGS:
    speed_vals = SPEED_BY_LANG.get(lang, tuple(SPEED_KEYS))
    if len(speed_vals) != len(SPEED_KEYS):
        raise ValueError(f"{lang}: expected {len(SPEED_KEYS)} speed values, got {len(speed_vals)}")
    for key, val in zip(SPEED_KEYS, speed_vals, strict=True):
        PHRASES[lang][key] = val


def format_block(key: str) -> str:
    lines = [f":{key}"]
    for lang in LANGS:
        lines.append(f"{lang}:{PHRASES[lang][key]}")
    return "\n".join(lines)


def append_blocks(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    for key in ALL_KEYS:
        marker = f":{key}\n"
        if marker in text:
            start = text.index(marker)
            end = start + 1
            while end < len(text) and not (text[end] == ":" and (end == 0 or text[end - 1] == "\n")):
                end = text.index("\n", end) + 1
            text = text[:start] + text[end:]
    blocks = "\n".join(format_block(key) for key in ALL_KEYS) + "\n"
    if not text.endswith("\n"):
        text += "\n"
    path.write_text(text + blocks, encoding="utf-8")


def main() -> None:
    append_blocks(GOOD)
    append_blocks(TXT)
    print(f"Appended {len(ALL_KEYS)} translation blocks x {len(LANGS)} languages")


if __name__ == "__main__":
    main()
