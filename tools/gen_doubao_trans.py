#!/usr/bin/env python3
"""Generate multilingual translations for Ask Doubao UI and prompts."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOOD = ROOT / "translations" / "translations-good.txt"
TXT = ROOT / "translations" / "translations.txt"

LANGS = """
af am ar az bg bn br bs by ca ca-xv cn co cy cz de dk el es et eu fa fi fo fr fy-nl ga gl he hi hr hu id it ja jv ka kr ku kw lt lv mk ml mm my ne nl nn no pa pl pt ro ru sat si sk sl sn sp-rs sq sr-rs sv ta th tl tr tw uk uz vn
""".split()

UI_KEYS = [
    "Ask &Doubao",
    "Ask Doubao",
    "Question sent to Doubao.",
]

PROMPT_KEYS = [
    "Please look up this word and give phonetic transcription, pronunciation, definition, examples, and mnemonics:",
    "Please explain what the following Chinese text means:",
    "Please translate this sentence and analyze its syntax and difficult vocabulary:",
]

ALL_KEYS = UI_KEYS + PROMPT_KEYS

# fmt: off
ASK_DOUBAO_MENU = {
    "af": "Vra &Doubao", "am": "Հարցնել &Doubao-ին", "ar": "اسأل &Doubao", "az": "&Doubao-ya sor",
    "bg": "Попитай &Doubao", "bn": "&Doubao-কে জিজ্ঞাসা করুন", "br": "Perguntar ao &Doubao",
    "bs": "Pitaj &Doubao", "by": "Запытаць &Doubao", "ca": "Pregunta a &Doubao",
    "ca-xv": "Pregunta a &Doubao", "cn": "问问豆包(&D)", "co": "Dì &Doubao",
    "cy": "Gofyn i &Doubao", "cz": "Zeptat se &Doubao", "de": "&Doubao fragen",
    "dk": "Spørg &Doubao", "el": "Ρώτησε το &Doubao", "es": "Preguntar a &Doubao",
    "et": "Küsi &Doubao-lt", "eu": "Galdetu &Doubao-ri", "fa": "از &Doubao بپرس",
    "fi": "Kysy &Doubaolta", "fo": "Spyr &Doubao", "fr": "Demander à &Doubao",
    "fy-nl": "Freeg &Doubao", "ga": "Fiafraigh de &Doubao", "gl": "Preguntar a &Doubao",
    "he": "שאל את &Doubao", "hi": "&Doubao से पूछें", "hr": "Pitaj &Doubao",
    "hu": "Kérdezd meg a &Doubao-t", "id": "Tanya &Doubao", "it": "Chiedi a &Doubao",
    "ja": "&Doubaoに質問", "jv": "Takon &Doubao", "ka": "ჰკითხე &Doubao-ს",
    "kr": "&Doubao에 질문", "ku": "Ji &Doubao bipirse", "kw": "Govyn &Doubao",
    "lt": "Klausk &Doubao", "lv": "Jautā &Doubao", "mk": "Прашај го &Doubao",
    "ml": "&Doubao-യോട് ചോദിക്കുക", "mm": "&Doubao ကို မေးပါ", "my": "Tanya &Doubao",
    "ne": "&Doubao लाई सोध्नुहोस्", "nl": "Vraag &Doubao", "nn": "Spør &Doubao",
    "no": "Spør &Doubao", "pa": "&Doubao ਨੂੰ ਪੁੱਛੋ", "pl": "Zapytaj &Doubao",
    "pt": "Perguntar ao &Doubao", "ro": "Întreabă &Doubao", "ru": "Спросить &Doubao",
    "sat": "&Doubao ᱠᱩᱠᱷᱟ ᱢᱮ", "si": "&Doubao අහන්න", "sk": "Opýtať sa &Doubao",
    "sl": "Vprašaj &Doubao", "sn": "Bvunza &Doubao", "sp-rs": "Pitaj &Doubao",
    "sq": "Pyet &Doubao", "sr-rs": "Питај &Doubao", "sv": "Fråga &Doubao",
    "ta": "&Doubao-வை கேள்", "th": "ถาม &Doubao", "tl": "Tanungin si &Doubao",
    "tr": "&Doubao'ya sor", "tw": "問問豆包(&D)", "uk": "Запитати &Doubao",
    "uz": "&Doubao-dan so'rang", "vn": "Hỏi &Doubao",
}

ASK_DOUBAO = {
    "af": "Vra Doubao", "am": "Հարցնել Doubao-ին", "ar": "اسأل Doubao", "az": "Doubao-ya sor",
    "bg": "Попитай Doubao", "bn": "Doubao-কে জিজ্ঞাসা করুন", "br": "Perguntar ao Doubao",
    "bs": "Pitaj Doubao", "by": "Запытаць Doubao", "ca": "Pregunta a Doubao",
    "ca-xv": "Pregunta a Doubao", "cn": "问问豆包", "co": "Dì Doubao",
    "cy": "Gofyn i Doubao", "cz": "Zeptat se Doubao", "de": "Doubao fragen",
    "dk": "Spørg Doubao", "el": "Ρώτησε το Doubao", "es": "Preguntar a Doubao",
    "et": "Küsi Doubao-lt", "eu": "Galdetu Doubao-ri", "fa": "از Doubao بپرس",
    "fi": "Kysy Doubaolta", "fo": "Spyr Doubao", "fr": "Demander à Doubao",
    "fy-nl": "Freeg Doubao", "ga": "Fiafraigh de Doubao", "gl": "Preguntar a Doubao",
    "he": "שאל את Doubao", "hi": "Doubao से पूछें", "hr": "Pitaj Doubao",
    "hu": "Kérdezd meg a Doubao-t", "id": "Tanya Doubao", "it": "Chiedi a Doubao",
    "ja": "Doubaoに質問", "jv": "Takon Doubao", "ka": "ჰკითხე Doubao-ს",
    "kr": "Doubao에 질문", "ku": "Ji Doubao bipirse", "kw": "Govyn Doubao",
    "lt": "Klausk Doubao", "lv": "Jautā Doubao", "mk": "Прашај го Doubao",
    "ml": "Doubao-യോട് ചോദിക്കുക", "mm": "Doubao ကို မေးပါ", "my": "Tanya Doubao",
    "ne": "Doubao लाई सोध्नुहोस्", "nl": "Vraag Doubao", "nn": "Spør Doubao",
    "no": "Spør Doubao", "pa": "Doubao ਨੂੰ ਪੁੱਛੋ", "pl": "Zapytaj Doubao",
    "pt": "Perguntar ao Doubao", "ro": "Întreabă Doubao", "ru": "Спросить Doubao",
    "sat": "Doubao ᱠᱩᱠᱷᱟ ᱢᱮ", "si": "Doubao අහන්න", "sk": "Opýtať sa Doubao",
    "sl": "Vprašaj Doubao", "sn": "Bvunza Doubao", "sp-rs": "Pitaj Doubao",
    "sq": "Pyet Doubao", "sr-rs": "Питај Doubao", "sv": "Fråga Doubao",
    "ta": "Doubao-வை கேள்", "th": "ถาม Doubao", "tl": "Tanungin si Doubao",
    "tr": "Doubao'ya sor", "tw": "問問豆包", "uk": "Запитати Doubao",
    "uz": "Doubao-dan so'rang", "vn": "Hỏi Doubao",
}

QUESTION_SENT = {
    "af": "Vraag na Doubao gestuur.", "am": "Հարցն ուղարկվել է Doubao-ին:",
    "ar": "تم إرسال السؤال إلى Doubao.", "az": "Sual Doubao-ya göndərildi.",
    "bg": "Въпросът е изпратен към Doubao.", "bn": "Doubao-তে প্রশ্ন পাঠানো হয়েছে।",
    "br": "Pergunta enviada ao Doubao.", "bs": "Pitanje poslano Doubao-u.",
    "by": "Пытанне адправлена ў Doubao.", "ca": "Pregunta enviada a Doubao.",
    "ca-xv": "Pregunta enviada a Doubao.", "cn": "已发送到豆包。",
    "co": "Domanda mandata à Doubao.", "cy": "Anfonwyd cwestiwn at Doubao.",
    "cz": "Otázka odeslána do Doubao.", "de": "Frage an Doubao gesendet.",
    "dk": "Spørgsmål sendt til Doubao.", "el": "Η ερώτηση στάλθηκε στο Doubao.",
    "es": "Pregunta enviada a Doubao.", "et": "Küsimus saadeti Doubao-sse.",
    "eu": "Galdera Doubao-ra bidali da.", "fa": "سؤال به Doubao ارسال شد.",
    "fi": "Kysymys lähetetty Doubaolle.", "fo": "Spurningur sendur til Doubao.",
    "fr": "Question envoyée à Doubao.", "fy-nl": "Frage nei Doubao stuurd.",
    "ga": "Ceist seolta chuig Doubao.", "gl": "Pregunta enviada a Doubao.",
    "he": "השאלה נשלחה ל-Doubao.", "hi": "Doubao को प्रश्न भेजा गया।",
    "hr": "Pitanje poslano Doubao-u.", "hu": "Kérdés elküldve a Doubao-nak.",
    "id": "Pertanyaan dikirim ke Doubao.", "it": "Domanda inviata a Doubao.",
    "ja": "Doubaoに質問を送信しました。", "jv": "Pitakon dikirim menyang Doubao.",
    "ka": "კითხვა გაგზავნილია Doubao-ში.", "kr": "Doubao에 질문을 보냈습니다.",
    "ku": "Pirs ji Doubao re hat şandin.", "kw": "Govyn danfonys dhe Doubao.",
    "lt": "Klausimas išsiųstas Doubao.", "lv": "Jautājums nosūtīts Doubao.",
    "mk": "Прашањето е испратено до Doubao.", "ml": "Doubao-യിലേക്ക് ചോദ്യം അയച്ചു.",
    "mm": "Doubao သို့ မေးခွန်း ပို့ပြီးပါပြီ။", "my": "Soalan dihantar ke Doubao.",
    "ne": "Doubao मा प्रश्न पठाइयो।", "nl": "Vraag verzonden naar Doubao.",
    "nn": "Spørsmål sendt til Doubao.", "no": "Spørsmål sendt til Doubao.",
    "pa": "Doubao ਨੂੰ ਸਵਾਲ ਭੇਜਿਆ ਗਿਆ।", "pl": "Pytanie wysłano do Doubao.",
    "pt": "Pergunta enviada ao Doubao.", "ro": "Întrebare trimisă către Doubao.",
    "ru": "Вопрос отправлен в Doubao.", "sat": "Doubao ᱨᱮ ᱠᱩᱠᱷᱟ ᱞᱟᱹᱜᱤᱫ ᱟ",
    "si": "Doubao වෙත ප්‍රශ්නය යවන ලදී.", "sk": "Otázka odoslaná do Doubao.",
    "sl": "Vprašanje poslano Doubao.", "sn": "Mubvunzo watumirwa kuDoubao.",
    "sp-rs": "Pitanje poslato Doubao-u.", "sq": "Pyetja u dërgua te Doubao.",
    "sr-rs": "Питање послато Doubao-у.", "sv": "Fråga skickad till Doubao.",
    "ta": "Doubao-க்கு கேள்வி அனுப்பப்பட்டது.", "th": "ส่งคำถามไปยัง Doubao แล้ว",
    "tl": "Naipadala ang tanong sa Doubao.", "tr": "Soru Doubao'ya gönderildi.",
    "tw": "已傳送到豆包。", "uk": "Запитання надіслано до Doubao.",
    "uz": "Savol Doubao-ga yuborildi.", "vn": "Đã gửi câu hỏi tới Doubao.",
}

PROMPT_WORD = {
    "cn": "请查一下这个单词，给出音标、读音、释义、例句和助记：",
    "tw": "請查一下這個單字，給出音標、讀音、釋義、例句和助記：",
    "ja": "この単語を調べて、発音記号、読み方、意味、例文、覚え方を教えてください：",
    "kr": "이 단어를 찾아 발음 기호, 발음, 뜻, 예문, 암기법을 알려 주세요:",
    "de": "Bitte nachschlagen: Lautschrift, Aussprache, Bedeutung, Beispiele und Merkhilfe für dieses Wort:",
    "fr": "Veuillez chercher ce mot et donner la phonétique, la prononciation, le sens, des exemples et une aide-mémoire :",
    "es": "Busca esta palabra y da transcripción fonética, pronunciación, significado, ejemplos y mnemotecnia:",
    "ru": "Найдите это слово: транскрипция, произношение, значение, примеры и подсказки для запоминания:",
    "pt": "Pesquise esta palavra e dê transcrição, pronúncia, significado, exemplos e mnemônicos:",
    "br": "Pesquise esta palavra e dê transcrição, pronúncia, significado, exemplos e mnemônicos:",
    "it": "Cerca questa parola e fornisci fonetica, pronuncia, significato, esempi e mnemonici:",
    "nl": "Zoek dit woord op en geef uitspraak, betekenis, voorbeelden en ezelsbruggetjes:",
    "pl": "Sprawdź to słowo: transkrypcja, wymowa, znaczenie, przykłady i wskazówki:",
    "vn": "Hãy tra từ này và cho phiên âm, cách đọc, nghĩa, ví dụ và mẹo ghi nhớ:",
    "th": "ช่วยค้นหาคำนี้ พร้อมสัญลักษณ์การออกเสียง การอ่าน ความหมาย ตัวอย่าง และเทคนิคจำ:",
    "ar": "ابحث عن هذه الكلمة وأعطِ النطق والمعنى والأمثلة وطرق الحفظ:",
    "he": "חפש את המילה הזו ותן תעתיק, הגייה, משמעות, דוגמאות ועזרי זיכרון:",
    "tr": "Bu kelimeyi ara ve ses bilgisi, telaffuz, anlam, örnekler ve akılda tutma ipuçları ver:",
    "uk": "Знайдіть це слово: транскрипція, вимова, значення, приклади та підказки:",
}

PROMPT_CHINESE = {
    "cn": "请解释一下下面这段中文是什么意思：",
    "tw": "請解釋一下下面這段中文是什麼意思：",
    "ja": "次の中国語の意味を説明してください：",
    "kr": "아래 중국어가 무슨 뜻인지 설명해 주세요:",
    "de": "Bitte erklären Sie, was der folgende chinesische Text bedeutet:",
    "fr": "Veuillez expliquer ce que signifie le texte chinois suivant :",
    "es": "Explique qué significa el siguiente texto en chino:",
    "ru": "Объясните, что означает следующий китайский текст:",
    "pt": "Explique o que significa o seguinte texto em chinês:",
    "br": "Explique o que significa o seguinte texto em chinês:",
    "it": "Spiega cosa significa il seguente testo cinese:",
    "nl": "Leg uit wat de volgende Chinese tekst betekent:",
    "pl": "Wyjaśnij, co oznacza poniższy tekst po chińsku:",
    "vn": "Hãy giải thích đoạn tiếng Trung sau nghĩa là gì:",
    "th": "โปรดอธิบายว่าข้อความภาษาจีนต่อไปนี้หมายความว่าอะไร:",
    "ar": "اشرح معنى النص الصيني التالي:",
    "he": "הסבר מה משמעות הטקסט הסיני הבא:",
    "tr": "Aşağıdaki Çince metnin ne anlama geldiğini açıkla:",
    "uk": "Поясніть, що означає наведений китайський текст:",
}

PROMPT_SENTENCE = {
    "cn": "请翻译这个句子，并分析句法结构和重点难点单词：",
    "tw": "請翻譯這個句子，並分析句法結構和重點難點單字：",
    "ja": "この文を翻訳し、文法構造と重要な語彙を分析してください：",
    "kr": "이 문장을 번역하고 문법 구조와 중요/어려운 단어를 분석해 주세요:",
    "de": "Bitte übersetzen Sie diesen Satz und analysieren Sie Syntax und schwierige Wörter:",
    "fr": "Veuillez traduire cette phrase et analyser la syntaxe et le vocabulaire difficile :",
    "es": "Traduce esta oración y analiza la sintaxis y el vocabulario difícil:",
    "ru": "Переведите это предложение и разберите синтаксис и сложные слова:",
    "pt": "Traduza esta frase e analise a sintaxe e vocabulário difícil:",
    "br": "Traduza esta frase e analise a sintaxe e vocabulário difícil:",
    "it": "Traduci questa frase e analizza sintassi e vocaboli difficili:",
    "nl": "Vertaal deze zin en analyseer de syntaxis en moeilijke woorden:",
    "pl": "Przetłumacz to zdanie i przeanalizuj składnię oraz trudne słowa:",
    "vn": "Hãy dịch câu này và phân tích cấu trúc ngữ pháp cùng từ khó:",
    "th": "แปลประโยคนี้และวิเคราะห์โครงสร้างไวยากรณ์และคำศัพท์สำคัญ:",
    "ar": "ترجم هذه الجملة وحلّل تركيبها والمفردات الصعبة:",
    "he": "תרגם את המשפט ונתח את המבנה התחבירי והמילים הקשות:",
    "tr": "Bu cümleyi çevir ve sözdizimi ile zor kelimeleri analiz et:",
    "uk": "Перекладіть це речення та проаналізуйте синтаксис і складні слова:",
}

EN_PROMPT_WORD = PROMPT_KEYS[0]
EN_PROMPT_CHINESE = PROMPT_KEYS[1]
EN_PROMPT_SENTENCE = PROMPT_KEYS[2]


def fill_prompt(base: dict[str, str], english: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for lang in LANGS:
        out[lang] = base.get(lang, english)
    return out


BLOCKS = {
    UI_KEYS[0]: ASK_DOUBAO_MENU,
    UI_KEYS[1]: ASK_DOUBAO,
    UI_KEYS[2]: QUESTION_SENT,
    PROMPT_KEYS[0]: fill_prompt(PROMPT_WORD, EN_PROMPT_WORD),
    PROMPT_KEYS[1]: fill_prompt(PROMPT_CHINESE, EN_PROMPT_CHINESE),
    PROMPT_KEYS[2]: fill_prompt(PROMPT_SENTENCE, EN_PROMPT_SENTENCE),
}
# fmt: on


def format_block(key: str, trans: dict[str, str]) -> str:
    lines = [f":{key}"]
    for lang in LANGS:
        val = trans.get(lang)
        if not val:
            raise KeyError(f"missing {lang} for {key!r}")
        lines.append(f"{lang}:{val}")
    return "\n".join(lines)


def replace_or_insert(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    new_section = "\n".join(format_block(k, BLOCKS[k]) for k in ALL_KEYS) + "\n"

    if ":Ask &Doubao" in text:
        start = text.index(":Ask &Doubao")
        end_key = ":Are you sure you want to uninstall SumatraPDF?"
        end = text.index(end_key, start)
        text = text[:start] + new_section + text[end:]
    else:
        end_key = ":Are you sure you want to uninstall SumatraPDF?"
        end = text.index(end_key)
        text = text[:end] + new_section + text[end:]

    path.write_text(text, encoding="utf-8")


def main() -> None:
    replace_or_insert(GOOD)
    replace_or_insert(TXT)
    print(f"Updated Doubao translations: {len(LANGS)} langs x {len(ALL_KEYS)} strings")


if __name__ == "__main__":
    main()
