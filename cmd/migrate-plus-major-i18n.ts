/**
 * Backfill Plus UI strings for 17 major languages (cn/tw/ja/kr/de/fr/es/it/pt/br/ru/uk/pl/nl/tr/vn).
 * Preserves existing translations; fills missing majors; adds newly _TRA-wrapped keys.
 * Rebuilds translations.txt.lzsa from translations-good.txt.
 */
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const root = join(import.meta.dir, "..");
const majors = [
  "cn",
  "tw",
  "ja",
  "kr",
  "de",
  "fr",
  "es",
  "it",
  "pt",
  "br",
  "ru",
  "uk",
  "pl",
  "nl",
  "tr",
  "vn",
] as const;

type Lang = (typeof majors)[number];
type LangMap = Partial<Record<Lang, string>>;

/** New / sparse Plus keys → major-language strings. cn/tw optional (kept from file if omitted). */
const fills: Record<string, LangMap> = {
  "Copy this batch's JSON": {
    cn: "请复制这一批的 JSON",
    tw: "請複製這一批的 JSON",
    ja: "このバッチの JSON をコピー",
    kr: "이 배치의 JSON 복사",
    de: "JSON dieses Stapels kopieren",
    fr: "Copier le JSON de ce lot",
    es: "Copiar el JSON de este lote",
    it: "Copia il JSON di questo lotto",
    pt: "Copiar o JSON deste lote",
    br: "Copiar o JSON deste lote",
    ru: "Скопируйте JSON этой партии",
    uk: "Скопіюйте JSON цієї партії",
    pl: "Skopiuj JSON tej partii",
    nl: "JSON van deze batch kopiëren",
    tr: "Bu partinin JSON’unu kopyala",
    vn: "Sao chép JSON của lô này",
  },
  "Batch %d of %d has been sent. Copy the JSON in that reply.\r\nThe app joins the batches in order. The AI is not asked to merge them.":
    {
      cn: "第 %d / %d 批已发送。复制这一批回复里的 JSON。\r\n程序按顺序自己接上各批，不再让 AI 合并。",
      tw: "第 %d / %d 批已傳送。請複製這一批回覆裡的 JSON。\r\n程式會依序接上各批，不再請 AI 合併。",
      ja: "バッチ %d / %d を送信しました。その返信の JSON をコピーしてください。\r\nアプリが順に結合します。AI に結合は依頼しません。",
      kr: "%d / %d 배치를 보냈습니다. 해당 답변의 JSON을 복사하세요.\r\n앱이 순서대로 합칩니다. AI에 병합을 요청하지 않습니다.",
      de: "Stapel %d von %d wurde gesendet. Kopieren Sie das JSON aus dieser Antwort.\r\nDie App fügt die Stapel der Reihe nach zusammen. Die KI soll sie nicht zusammenführen.",
      fr: "Le lot %d sur %d a été envoyé. Copiez le JSON de cette réponse.\r\nL’application assemble les lots dans l’ordre. L’IA n’est pas invitée à les fusionner.",
      es: "Se envió el lote %d de %d. Copie el JSON de esa respuesta.\r\nLa aplicación une los lotes en orden. No se pide a la IA que los fusione.",
      it: "Il lotto %d di %d è stato inviato. Copia il JSON di quella risposta.\r\nL’app unisce i lotti in ordine. All’IA non viene chiesto di unirli.",
      pt: "O lote %d de %d foi enviado. Copie o JSON dessa resposta.\r\nA aplicação junta os lotes por ordem. A IA não é pedida para os unir.",
      br: "O lote %d de %d foi enviado. Copie o JSON dessa resposta.\r\nO app une os lotes em ordem. A IA não é pedida para mesclá-los.",
      ru: "Партия %d из %d отправлена. Скопируйте JSON из этого ответа.\r\nПриложение соединит партии по порядку. ИИ не просят объединять их.",
      uk: "Партію %d з %d надіслано. Скопіюйте JSON із цієї відповіді.\r\nПрограма з’єднає партії по порядку. ІІ не просять об’єднувати їх.",
      pl: "Wysłano partię %d z %d. Skopiuj JSON z tej odpowiedzi.\r\nAplikacja połączy partie w kolejności. AI nie jest proszone o scalanie.",
      nl: "Batch %d van %d is verzonden. Kopieer de JSON uit dat antwoord.\r\nDe app voegt de batches op volgorde samen. De AI hoeft ze niet samen te voegen.",
      tr: "%d / %d parti gönderildi. Bu yanıttaki JSON’u kopyalayın.\r\nUygulama partileri sırayla birleştirir. Yapay zekâdan birleştirmesi istenmez.",
      vn: "Đã gửi lô %d / %d. Hãy sao chép JSON trong phản hồi đó.\r\nỨng dụng nối các lô theo thứ tự. Không yêu cầu AI gộp chúng.",
    },
  "Waiting for batch %d of %d JSON…": {
    cn: "等待第 %d / %d 批 JSON…",
    tw: "等待第 %d / %d 批 JSON…",
    ja: "バッチ %d / %d の JSON を待機中…",
    kr: "%d / %d 배치 JSON 대기 중…",
    de: "Warte auf JSON von Stapel %d von %d…",
    fr: "En attente du JSON du lot %d sur %d…",
    es: "Esperando el JSON del lote %d de %d…",
    it: "In attesa del JSON del lotto %d di %d…",
    pt: "A aguardar o JSON do lote %d de %d…",
    br: "Aguardando o JSON do lote %d de %d…",
    ru: "Ожидание JSON партии %d из %d…",
    uk: "Очікування JSON партії %d з %d…",
    pl: "Oczekiwanie na JSON partii %d z %d…",
    nl: "Wachten op JSON van batch %d van %d…",
    tr: "%d / %d parti JSON’u bekleniyor…",
    vn: "Đang chờ JSON lô %d / %d…",
  },
  "Sending batch %d…": {
    cn: "正在发送第 %d 批…",
    tw: "正在傳送第 %d 批…",
    ja: "バッチ %d を送信中…",
    kr: "%d 배치 전송 중…",
    de: "Sende Stapel %d…",
    fr: "Envoi du lot %d…",
    es: "Enviando el lote %d…",
    it: "Invio del lotto %d…",
    pt: "A enviar o lote %d…",
    br: "Enviando o lote %d…",
    ru: "Отправка партии %d…",
    uk: "Надсилання партії %d…",
    pl: "Wysyłanie partii %d…",
    nl: "Batch %d verzenden…",
    tr: "%d. parti gönderiliyor…",
    vn: "Đang gửi lô %d…",
  },
  "The JSON contains no importable table-of-contents items.": {
    cn: "JSON 中没有可导入的目录项。",
    tw: "JSON 中沒有可匯入的目錄項目。",
    ja: "JSON にインポート可能な目次項目がありません。",
    kr: "JSON에 가져올 수 있는 목차 항목이 없습니다.",
    de: "Das JSON enthält keine importierbaren Inhaltsverzeichnis-Einträge.",
    fr: "Le JSON ne contient aucun élément de table des matières importable.",
    es: "El JSON no contiene elementos de tabla de contenido importables.",
    it: "Il JSON non contiene voci di sommario importabili.",
    pt: "O JSON não contém itens de índice importáveis.",
    br: "O JSON não contém itens de sumário importáveis.",
    ru: "В JSON нет элементов оглавления для импорта.",
    uk: "У JSON немає елементів змісту для імпорту.",
    pl: "JSON nie zawiera elementów spisu treści do zaimportowania.",
    nl: "De JSON bevat geen importeerbare inhoudsopgave-items.",
    tr: "JSON içe aktarılabilir içindekiler öğesi içermiyor.",
    vn: "JSON không chứa mục lục nào có thể nhập.",
  },
  "Editing the table of contents changes this PDF after it was digitally signed. The existing signature will remain, but viewers will report that the document was modified. Continue?":
    {
      cn: "编辑目录会在数字签名之后修改此 PDF。现有签名仍会保留，但阅读器会提示文档已被修改。是否继续？",
      tw: "編輯目錄會在數位簽章之後修改此 PDF。現有簽章仍會保留，但閱讀器會提示文件已被修改。是否繼續？",
      ja: "目次を編集すると、デジタル署名後の PDF が変更されます。署名は残りますが、ビューアは文書が変更されたと報告します。続行しますか？",
      kr: "목차를 편집하면 디지털 서명 이후 PDF가 변경됩니다. 기존 서명은 유지되지만 뷰어는 문서가 수정되었다고 표시합니다. 계속할까요?",
      de: "Das Bearbeiten des Inhaltsverzeichnisses ändert dieses PDF nach der digitalen Signatur. Die Signatur bleibt, aber Viewer melden eine Änderung. Fortfahren?",
      fr: "Modifier la table des matières modifie ce PDF après sa signature numérique. La signature reste, mais les lecteurs indiqueront une modification. Continuer ?",
      es: "Editar la tabla de contenido cambia este PDF después de firmarlo. La firma permanece, pero los visores indicarán que se modificó. ¿Continuar?",
      it: "La modifica del sommario altera questo PDF dopo la firma digitale. La firma resta, ma i visualizzatori segnaleranno una modifica. Continuare?",
      pt: "Editar o índice altera este PDF após a assinatura digital. A assinatura permanece, mas os leitores reportarão alteração. Continuar?",
      br: "Editar o sumário altera este PDF após a assinatura digital. A assinatura permanece, mas os leitores reportarão alteração. Continuar?",
      ru: "Изменение оглавления меняет PDF после цифровой подписи. Подпись останется, но программы сообщат об изменении. Продолжить?",
      uk: "Редагування змісту змінює PDF після цифрового підпису. Підпис залишиться, але переглядачі повідомлять про зміну. Продовжити?",
      pl: "Edycja spisu treści zmienia ten PDF po podpisie cyfrowym. Podpis pozostanie, ale przeglądarki zgłoszą modyfikację. Kontynuować?",
      nl: "Het bewerken van de inhoudsopgave wijzigt deze PDF na digitale ondertekening. De handtekening blijft, maar viewers melden een wijziging. Doorgaan?",
      tr: "İçindekileri düzenlemek, dijital imzadan sonra bu PDF’yi değiştirir. İmza kalır ancak görüntüleyiciler belgenin değiştiğini bildirir. Devam?",
      vn: "Chỉnh mục lục sẽ thay đổi PDF sau khi đã ký số. Chữ ký vẫn còn nhưng trình xem sẽ báo tài liệu đã bị sửa. Tiếp tục?",
    },
  "Digitally signed PDF": {
    cn: "已数字签名的 PDF",
    tw: "已數位簽章的 PDF",
    ja: "デジタル署名付き PDF",
    kr: "디지털 서명된 PDF",
    de: "Digital signiertes PDF",
    fr: "PDF signé numériquement",
    es: "PDF firmado digitalmente",
    it: "PDF firmato digitalmente",
    pt: "PDF assinado digitalmente",
    br: "PDF assinado digitalmente",
    ru: "PDF с цифровой подписью",
    uk: "PDF із цифровим підписом",
    pl: "PDF podpisany cyfrowo",
    nl: "Digitaal ondertekende PDF",
    tr: "Dijital imzalı PDF",
    vn: "PDF đã ký số",
  },
  "PDF table of contents": {
    cn: "PDF 目录",
    tw: "PDF 目錄",
    ja: "PDF の目次",
    kr: "PDF 목차",
    de: "PDF-Inhaltsverzeichnis",
    fr: "Table des matières PDF",
    es: "Tabla de contenido del PDF",
    it: "Sommario PDF",
    pt: "Índice do PDF",
    br: "Sumário do PDF",
    ru: "Оглавление PDF",
    uk: "Зміст PDF",
    pl: "Spis treści PDF",
    nl: "PDF-inhoudsopgave",
    tr: "PDF içindekiler",
    vn: "Mục lục PDF",
  },
  "The PDF table of contents could not be modified.": {
    cn: "无法修改 PDF 目录。",
    tw: "無法修改 PDF 目錄。",
    ja: "PDF の目次を変更できませんでした。",
    kr: "PDF 목차를 수정할 수 없습니다.",
    de: "Das PDF-Inhaltsverzeichnis konnte nicht geändert werden.",
    fr: "Impossible de modifier la table des matières du PDF.",
    es: "No se pudo modificar la tabla de contenido del PDF.",
    it: "Impossibile modificare il sommario del PDF.",
    pt: "Não foi possível modificar o índice do PDF.",
    br: "Não foi possível modificar o sumário do PDF.",
    ru: "Не удалось изменить оглавление PDF.",
    uk: "Не вдалося змінити зміст PDF.",
    pl: "Nie można zmodyfikować spisu treści PDF.",
    nl: "De PDF-inhoudsopgave kon niet worden gewijzigd.",
    tr: "PDF içindekileri değiştirilemedi.",
    vn: "Không thể sửa mục lục PDF.",
  },
  "Unpin Document": {
    cn: "取消固定文档",
    tw: "取消釘選文件",
    ja: "文書のピン留めを解除",
    kr: "문서 고정 해제",
    de: "Dokument lösen",
    fr: "Désépingler le document",
    es: "Desanclar documento",
    it: "Rimuovi documento dai preferiti",
    pt: "Desafixar documento",
    br: "Desafixar documento",
    ru: "Открепить документ",
    uk: "Відкріпити документ",
    pl: "Odepnij dokument",
    nl: "Document losmaken",
    tr: "Belgeyi sabitlemeyi kaldır",
    vn: "Bỏ ghim tài liệu",
  },
  "Printed page": {
    cn: "印刷页码",
    tw: "印刷頁碼",
    ja: "印刷ページ",
    kr: "인쇄 쪽번호",
    de: "Gedruckte Seite",
    fr: "Page imprimée",
    es: "Página impresa",
    it: "Pagina stampata",
    pt: "Página impressa",
    br: "Página impressa",
    ru: "Печатная страница",
    uk: "Друкована сторінка",
    pl: "Strona drukowana",
    nl: "Gedrukte pagina",
    tr: "Basılı sayfa",
    vn: "Trang in",
  },
  "PDF page": {
    cn: "PDF 页码",
    tw: "PDF 頁碼",
    ja: "PDF ページ",
    kr: "PDF 쪽",
    de: "PDF-Seite",
    fr: "Page PDF",
    es: "Página PDF",
    it: "Pagina PDF",
    pt: "Página PDF",
    br: "Página PDF",
    ru: "Страница PDF",
    uk: "Сторінка PDF",
    pl: "Strona PDF",
    nl: "PDF-pagina",
    tr: "PDF sayfası",
    vn: "Trang PDF",
  },
  "Failed to save image": {
    cn: "无法保存图片",
    tw: "無法儲存圖片",
    ja: "画像を保存できませんでした",
    kr: "이미지를 저장하지 못했습니다",
    de: "Bild konnte nicht gespeichert werden",
    fr: "Échec de l’enregistrement de l’image",
    es: "No se pudo guardar la imagen",
    it: "Impossibile salvare l’immagine",
    pt: "Falha ao guardar a imagem",
    br: "Falha ao salvar a imagem",
    ru: "Не удалось сохранить изображение",
    uk: "Не вдалося зберегти зображення",
    pl: "Nie udało się zapisać obrazu",
    nl: "Afbeelding opslaan mislukt",
    tr: "Görüntü kaydedilemedi",
    vn: "Không lưu được ảnh",
  },
  "Failed to create cropped image": {
    cn: "无法创建裁剪后的图片",
    tw: "無法建立裁切後的圖片",
    ja: "切り抜き画像を作成できませんでした",
    kr: "자른 이미지를 만들지 못했습니다",
    de: "Zugeschnittenes Bild konnte nicht erstellt werden",
    fr: "Échec de la création de l’image recadrée",
    es: "No se pudo crear la imagen recortada",
    it: "Impossibile creare l’immagine ritagliata",
    pt: "Falha ao criar a imagem recortada",
    br: "Falha ao criar a imagem recortada",
    ru: "Не удалось создать обрезанное изображение",
    uk: "Не вдалося створити обрізане зображення",
    pl: "Nie udało się utworzyć przyciętego obrazu",
    nl: "Bijgesneden afbeelding maken mislukt",
    tr: "Kırpılmış görüntü oluşturulamadı",
    vn: "Không tạo được ảnh đã cắt",
  },
  "Failed to create resized image": {
    cn: "无法创建缩放后的图片",
    tw: "無法建立縮放後的圖片",
    ja: "リサイズ画像を作成できませんでした",
    kr: "크기 조정된 이미지를 만들지 못했습니다",
    de: "Skaliertes Bild konnte nicht erstellt werden",
    fr: "Échec de la création de l’image redimensionnée",
    es: "No se pudo crear la imagen redimensionada",
    it: "Impossibile creare l’immagine ridimensionata",
    pt: "Falha ao criar a imagem redimensionada",
    br: "Falha ao criar a imagem redimensionada",
    ru: "Не удалось создать изображение с новым размером",
    uk: "Не вдалося створити зображення зі зміненим розміром",
    pl: "Nie udało się utworzyć przeskalowanego obrazu",
    nl: "Herschalen van afbeelding mislukt",
    tr: "Yeniden boyutlandırılmış görüntü oluşturulamadı",
    vn: "Không tạo được ảnh đã đổi kích thước",
  },
  "Failed to bake PDF file.": {
    cn: "无法固化 PDF 文件。",
    tw: "無法固化 PDF 檔案。",
    ja: "PDF ファイルをベイクできませんでした。",
    kr: "PDF 파일을 베이크하지 못했습니다.",
    de: "PDF-Datei konnte nicht gebacken werden.",
    fr: "Échec du « bake » du fichier PDF.",
    es: "No se pudo consolidar el archivo PDF.",
    it: "Impossibile consolidare il file PDF.",
    pt: "Falha ao consolidar o ficheiro PDF.",
    br: "Falha ao consolidar o arquivo PDF.",
    ru: "Не удалось «запечь» PDF-файл.",
    uk: "Не вдалося закріпити PDF-файл.",
    pl: "Nie udało się utrwalić pliku PDF.",
    nl: "PDF-bestand bakken mislukt.",
    tr: "PDF dosyası sabitlenemedi.",
    vn: "Không thể cố định tệp PDF.",
  },
  "Failed to extract text.": {
    cn: "无法提取文字。",
    tw: "無法擷取文字。",
    ja: "テキストを抽出できませんでした。",
    kr: "텍스트를 추출하지 못했습니다.",
    de: "Text konnte nicht extrahiert werden.",
    fr: "Échec de l’extraction du texte.",
    es: "No se pudo extraer el texto.",
    it: "Impossibile estrarre il testo.",
    pt: "Falha ao extrair o texto.",
    br: "Falha ao extrair o texto.",
    ru: "Не удалось извлечь текст.",
    uk: "Не вдалося витягти текст.",
    pl: "Nie udało się wyodrębnić tekstu.",
    nl: "Tekst extraheren mislukt.",
    tr: "Metin ayıklanamadı.",
    vn: "Không trích xuất được văn bản.",
  },
  "Failed to compress PDF file.": {
    cn: "无法压缩 PDF 文件。",
    tw: "無法壓縮 PDF 檔案。",
    ja: "PDF ファイルを圧縮できませんでした。",
    kr: "PDF 파일을 압축하지 못했습니다.",
    de: "PDF-Datei konnte nicht komprimiert werden.",
    fr: "Échec de la compression du fichier PDF.",
    es: "No se pudo comprimir el archivo PDF.",
    it: "Impossibile comprimere il file PDF.",
    pt: "Falha ao comprimir o ficheiro PDF.",
    br: "Falha ao compactar o arquivo PDF.",
    ru: "Не удалось сжать PDF-файл.",
    uk: "Не вдалося стиснути PDF-файл.",
    pl: "Nie udało się skompresować pliku PDF.",
    nl: "PDF-bestand comprimeren mislukt.",
    tr: "PDF dosyası sıkıştırılamadı.",
    vn: "Không nén được tệp PDF.",
  },
  "Failed to decompress PDF file.": {
    cn: "无法解压 PDF 文件。",
    tw: "無法解壓 PDF 檔案。",
    ja: "PDF ファイルを展開できませんでした。",
    kr: "PDF 파일 압축을 풀지 못했습니다.",
    de: "PDF-Datei konnte nicht dekomprimiert werden.",
    fr: "Échec de la décompression du fichier PDF.",
    es: "No se pudo descomprimir el archivo PDF.",
    it: "Impossibile decomprimere il file PDF.",
    pt: "Falha ao descomprimir o ficheiro PDF.",
    br: "Falha ao descompactar o arquivo PDF.",
    ru: "Не удалось распаковать PDF-файл.",
    uk: "Не вдалося розпакувати PDF-файл.",
    pl: "Nie udało się zdekompresować pliku PDF.",
    nl: "PDF-bestand decomprimeren mislukt.",
    tr: "PDF dosyasının sıkıştırması açılamadı.",
    vn: "Không giải nén được tệp PDF.",
  },
  "Failed to encrypt PDF file.": {
    cn: "无法加密 PDF 文件。",
    tw: "無法加密 PDF 檔案。",
    ja: "PDF ファイルを暗号化できませんでした。",
    kr: "PDF 파일을 암호화하지 못했습니다.",
    de: "PDF-Datei konnte nicht verschlüsselt werden.",
    fr: "Échec du chiffrement du fichier PDF.",
    es: "No se pudo cifrar el archivo PDF.",
    it: "Impossibile crittografare il file PDF.",
    pt: "Falha ao encriptar o ficheiro PDF.",
    br: "Falha ao criptografar o arquivo PDF.",
    ru: "Не удалось зашифровать PDF-файл.",
    uk: "Не вдалося зашифрувати PDF-файл.",
    pl: "Nie udało się zaszyfrować pliku PDF.",
    nl: "PDF-bestand versleutelen mislukt.",
    tr: "PDF dosyası şifrelenemedi.",
    vn: "Không mã hóa được tệp PDF.",
  },
  "Failed to decrypt PDF file.": {
    cn: "无法解密 PDF 文件。",
    tw: "無法解密 PDF 檔案。",
    ja: "PDF ファイルを復号できませんでした。",
    kr: "PDF 파일을 복호화하지 못했습니다.",
    de: "PDF-Datei konnte nicht entschlüsselt werden.",
    fr: "Échec du déchiffrement du fichier PDF.",
    es: "No se pudo descifrar el archivo PDF.",
    it: "Impossibile decrittografare il file PDF.",
    pt: "Falha ao desencriptar o ficheiro PDF.",
    br: "Falha ao descriptografar o arquivo PDF.",
    ru: "Не удалось расшифровать PDF-файл.",
    uk: "Не вдалося розшифрувати PDF-файл.",
    pl: "Nie udało się odszyfrować pliku PDF.",
    nl: "PDF-bestand ontsleutelen mislukt.",
    tr: "PDF dosyasının şifresi çözülemedi.",
    vn: "Không giải mã được tệp PDF.",
  },
};

// Load additional fills generated alongside this script when present.
const dataPath = join(import.meta.dir, "plus-major-i18n-data.json");
try {
  const extra = JSON.parse(readFileSync(dataPath, "utf8")) as Record<string, LangMap>;
  for (const [k, v] of Object.entries(extra)) {
    fills[k] = { ...(fills[k] || {}), ...v };
  }
  console.log("Loaded extra fills:", Object.keys(extra).length);
} catch {
  console.log("No plus-major-i18n-data.json (using built-in fills only)");
}

function parseBlocks(text: string): Map<string, Map<string, string>> {
  const blocks = new Map<string, Map<string, string>>();
  let key: string | null = null;
  let map: Map<string, string> | null = null;
  for (const line of text.split(/\r?\n/)) {
    if (line.startsWith(":")) {
      if (key && map) blocks.set(key, map);
      key = line.slice(1);
      map = new Map();
    } else if (map) {
      const i = line.indexOf(":");
      if (i > 0) map.set(line.slice(0, i), line.slice(i + 1));
    }
  }
  if (key && map) blocks.set(key, map);
  return blocks;
}

function serializeBlock(key: string, values: Map<string, string>): string {
  const lines = [...values.entries()].sort(([a], [b]) => a.localeCompare(b)).map(([l, v]) => `${l}:${v}`);
  return `:${key}\n${lines.join("\n")}\n`;
}

function upsertFile(path: string): number {
  let text = readFileSync(path, "utf8");
  const existing = parseBlocks(text);
  let changed = 0;
  for (const [key, langMap] of Object.entries(fills)) {
    const values = new Map(existing.get(key) || []);
    let touched = false;
    for (const lang of majors) {
      const next = langMap[lang];
      if (!next) continue;
      if (values.get(lang) !== next) {
        values.set(lang, next);
        touched = true;
      }
    }
    if (!touched && existing.has(key)) continue;
    changed++;
    const block = serializeBlock(key, values);
    const start = text.indexOf(`:${key}\n`);
    if (start >= 0) {
      const end = text.indexOf("\n:", start + 1);
      text = text.slice(0, start) + block + (end < 0 ? "" : text.slice(end + 1));
    } else {
      text = text.trimEnd() + "\n\n" + block;
    }
    existing.set(key, values);
  }
  writeFileSync(path, text.endsWith("\n") ? text : text + "\n");
  return changed;
}

for (const name of ["translations-good.txt", "translations.txt"]) {
  const n = upsertFile(join(root, "translations", name));
  console.log(`Updated ${name}: ${n} keys`);
}

const archive = join(root, "translations", "translations.txt.lzsa");
const good = join(root, "translations", "translations-good.txt");
const result = spawnSync(join(root, "bin", "MakeLZSA.exe"), [archive, `${good}:translations-good.txt`], {
  stdio: "inherit",
});
if (result.status !== 0) {
  process.exit(result.status ?? 1);
}
console.log("Rebuilt translations.txt.lzsa");
