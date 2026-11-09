#!/usr/bin/env python3
"""Update TOC-extraction UI translations (extract TOC flow, AI TOC dialog).

- Renames the menu msgid "Extract Table of Contents Locally" to the unified
  "Extract Table of Contents" (local + online entries share one chooser now).
- Adds cn/tw for every TOC-extraction UI string plus a few major languages
  for the most visible short labels; all other languages fall back to the
  English msgid (standard SumatraPDF behaviour for untranslated strings).
"""

from __future__ import annotations

import os

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GOOD = ROOT / "translations" / "translations-good.txt"
TXT = ROOT / "translations" / "translations.txt"

# Canonical language order used by translations-good.txt.
LANGS = """
af am ar az bg bn br bs by ca ca-xv cn co cy cz de dk el es et eu fa fi fo fr fy-nl ga gl he hi hr hu id it ja jv ka kr ku kw lt lv mk ml mm my ne nl nn no pa pl pt ro ru sat si sk sl sn sp-rs sq sr-rs sv ta th tl tr tw uk uz vn
""".split()

# Blocks to remove entirely (msgid replaced by the unified one below).
DELETE = [
    "Extract Table of Contents Locally",
]

# Existing blocks that get new/updated language lines.
UPDATE: dict[str, dict[str, str]] = {
    "Extract Table of Contents": {
        "cn": "提取目录",
        "tw": "擷取目錄",
        "de": "Inhaltsverzeichnis extrahieren",
        "es": "Extraer la tabla de contenidos",
        "fr": "Extraire la table des matières",
        "it": "Estrai il sommario",
        "ja": "目次を抽出",
        "kr": "목차 추출",
        "pt": "Extrair sumário",
        "ru": "Извлечь оглавление",
    },
    "AI Recognize Table of Contents": {
        "cn": "AI 提取目录书签",
        "tw": "AI 擷取目錄書籤",
    },
}

# New blocks (cn/tw for everything; major languages for the short labels).
NEW: dict[str, dict[str, str]] = {
    # --- fallback / waiting / phases -------------------------------------
    "Prepare Content": {
        "cn": "准备内容", "tw": "準備內容",
        "de": "Inhalt vorbereiten", "es": "Preparar contenido",
        "fr": "Préparer le contenu", "ja": "コンテンツを準備",
        "kr": "내용 준비", "ru": "Подготовка содержимого",
    },
    "AI Processing": {
        "cn": "AI 处理", "tw": "AI 處理",
        "de": "KI-Verarbeitung", "es": "Procesamiento IA",
        "fr": "Traitement IA", "ja": "AI 処理",
        "kr": "AI 처리", "ru": "Обработка ИИ",
    },
    "Import TOC": {
        "cn": "导入目录", "tw": "匯入目錄",
        "de": "Inhaltsverzeichnis importieren", "es": "Importar índice",
        "fr": "Importer le sommaire", "ja": "目次を読み込む",
        "kr": "목차 가져오기", "ru": "Импорт оглавления",
    },
    "No printed TOC found": {
        "cn": "未找到印刷目录页", "tw": "未找到印刷目錄頁",
        "de": "Kein gedrucktes Inhaltsverzeichnis gefunden",
        "es": "No se encontró un índice impreso",
        "fr": "Aucun sommaire imprimé trouvé",
        "ja": "印刷目次が見つかりません",
        "kr": "인쇄된 목차를 찾을 수 없음",
        "ru": "Печатное оглавление не найдено",
    },
    "This document may not have a separate printed TOC.\\r\\nYou can generate one from the body structure or specify the TOC pages manually.": {
        "cn": "这份文档可能没有独立的印刷目录。\\r\\n你可以从正文结构生成目录，或手动指定目录页。",
        "tw": "這份文件可能沒有獨立的印刷目錄。\\r\\n你可以從正文結構產生目錄，或手動指定目錄頁。",
    },
    "Generate TOC From Body": {
        "cn": "从正文生成目录", "tw": "從正文產生目錄",
        "de": "Inhaltsverzeichnis aus dem Fließtext erstellen",
        "es": "Generar índice desde el cuerpo",
        "fr": "Générer le sommaire depuis le corps",
        "ja": "本文構造から目次を生成",
        "kr": "본문 구조에서 목차 생성",
        "ru": "Создать оглавление по структуре текста",
    },
    "Specify TOC Pages": {
        "cn": "手工指定目录页", "tw": "手動指定目錄頁",
        "de": "Verzeichnisseiten angeben",
        "es": "Indicar páginas del índice",
        "fr": "Indiquer les pages du sommaire",
        "ja": "目次ページを指定",
        "kr": "목차 페이지 지정",
        "ru": "Указать страницы оглавления",
    },
    "Scans the full text and extracts heading candidates locally; the AI then organizes them into a TOC.": {
        "cn": "扫描全文，在本地提取疑似标题，再由 AI 整理目录结构。",
        "tw": "掃描全文，在本機擷取疑似標題，再由 AI 整理目錄結構。",
    },
    # --- waiting page ----------------------------------------------------
    "Automatic send incomplete": {
        "cn": "自动发送未完成", "tw": "自動傳送未完成",
    },
    "The heading-candidates prompt is on the clipboard. Paste and send it in the AI page.\\r\\nWhen the reply is ready, click Copy under the AI reply; the app reads it and imports the TOC.": {
        "cn": "标题候选提示词已放入剪贴板，可在 AI 网页粘贴发送。\\r\\n回复完成后，点击 AI 回复下方的“复制”，本程序会自动读取结果并导入目录。",
        "tw": "標題候選提示詞已放入剪貼簿，可在 AI 網頁貼上傳送。\\r\\n回覆完成後，點擊 AI 回覆下方的「複製」，本程式會自動讀取結果並匯入目錄。",
    },
    "The TOC page images and prompt are on the clipboard. Paste and send them in the AI page.\\r\\nWhen the reply is ready, click Copy under the AI reply; the app reads it and imports the TOC.": {
        "cn": "目录页图片与提示词已放入剪贴板，可在 AI 网页粘贴发送。\\r\\n回复完成后，点击 AI 回复下方的“复制”，本程序会自动读取结果并导入目录。",
        "tw": "目錄頁圖片與提示詞已放入剪貼簿，可在 AI 網頁貼上傳送。\\r\\n回覆完成後，點擊 AI 回覆下方的「複製」，本程式會自動讀取結果並匯入目錄。",
    },
    "Body structure sent to the web AI": {
        "cn": "正文结构已发送到网页版 AI", "tw": "正文結構已傳送到網頁版 AI",
    },
    "The AI is organizing a TOC from the full-text structure.\\r\\nWhen it finishes, click Copy under the AI reply; the app reads it and imports the TOC.\\r\\nOnly locally extracted heading candidates are sent — the PDF itself is not uploaded.": {
        "cn": "AI 正在根据全文结构整理目录。\\r\\n完成后，请在 AI 回复中点击“复制”，本程序会自动读取结果并导入目录。\\r\\n仅发送本地提取的结构化标题候选，不上传整份 PDF。",
        "tw": "AI 正在根據全文結構整理目錄。\\r\\n完成後，請在 AI 回覆中點擊「複製」，本程式會自動讀取結果並匯入目錄。\\r\\n僅傳送本機擷取的結構化標題候選，不上傳整份 PDF。",
    },
    "The AI is organizing a TOC from the full-text structure.\\r\\nWhen it finishes, click Copy under the AI reply; the app reads it and imports the TOC.": {
        "cn": "AI 正在根据全文结构整理目录。\\r\\n完成后，请在 AI 回复中点击“复制”，本程序会自动读取结果并导入目录。",
        "tw": "AI 正在根據全文結構整理目錄。\\r\\n完成後，請在 AI 回覆中點擊「複製」，本程式會自動讀取結果並匯入目錄。",
    },
    "TOC pages sent to the web AI": {
        "cn": "目录页已发送到网页版 AI", "tw": "目錄頁已傳送到網頁版 AI",
    },
    "The AI is recognizing the printed TOC.\\r\\nWhen it finishes, click Copy under the AI reply; the app reads it and imports the TOC.": {
        "cn": "AI 正在识别印刷目录。\\r\\n完成后，请在 AI 回复中点击“复制”，本程序会自动读取结果并导入目录。",
        "tw": "AI 正在辨識印刷目錄。\\r\\n完成後，請在 AI 回覆中點擊「複製」，本程式會自動讀取結果並匯入目錄。",
    },
    "Waiting for the AI reply. Click Copy when it is ready…": {
        "cn": "等待 AI 回复并复制…", "tw": "等待 AI 回覆並複製…",
    },
    # --- scanning / detection statuses ------------------------------------
    "Scanning for TOC pages": {
        "cn": "正在扫描目录页", "tw": "正在掃描目錄頁",
    },
    "Scanning document structure": {
        "cn": "正在扫描文档结构", "tw": "正在掃描文件結構",
    },
    "Analyzing the first pages for a printed TOC…": {
        "cn": "正在分析文档前段的印刷目录页…", "tw": "正在分析文件前段的印刷目錄頁…",
    },
    "Scan finished: %d likely headings found. Opening the AI page…": {
        "cn": "扫描完成，发现 %d 个疑似标题，正在打开 AI 网页…",
        "tw": "掃描完成，發現 %d 個疑似標題，正在開啟 AI 網頁…",
    },
    "Scan canceled.": {
        "cn": "已取消扫描。", "tw": "已取消掃描。",
    },
    "No obvious body headings detected. Enter the TOC PDF page numbers below.": {
        "cn": "未检测到明显的正文标题，可在下方手动填写目录页页码。",
        "tw": "未偵測到明顯的正文標題，可在下方手動填寫目錄頁頁碼。",
    },
    "✓ Likely TOC pages found. Please confirm the selection.": {
        "cn": "✓ 已找到疑似目录页，请确认选择。", "tw": "✓ 已找到疑似目錄頁，請確認選擇。",
    },
    "TOC pages (PDF page numbers, not printed page numbers)": {
        "cn": "目录所在的 PDF 页码（非书本印刷页码）",
        "tw": "目錄所在的 PDF 頁碼（非書本印刷頁碼）",
    },
    "e.g. 3-4, 7": {
        "cn": "例如：3-4, 7", "tw": "例如：3-4, 7",
    },
    "Page numbers and the thumbnails below are in sync.": {
        "cn": "页码与下方缩略图已同步。", "tw": "頁碼與下方縮圖已同步。",
    },
    "The entered PDF pages will be rendered and queued for sending.": {
        "cn": "将按输入的 PDF 页码逐页取图并等待发送。",
        "tw": "將按輸入的 PDF 頁碼逐頁取圖並等待傳送。",
    },
    "Enter the PDF pages of the TOC; matching thumbnails will be checked. You can also click thumbnails directly.": {
        "cn": "请输入目录所在的 PDF 页码，缩略图将同步勾选；也可直接点选缩略图。",
        "tw": "請輸入目錄所在的 PDF 頁碼，縮圖將同步勾選；也可直接點選縮圖。",
    },
    "The selected %d TOC page images will be sent to the configured web AI.\\n\\nPlease verify the pages and content before sending.": {
        "cn": "将把选中的 %d 页目录图片发送到当前配置的网页版 AI。\\n\\n请确认页码和内容无误后发送。",
        "tw": "將把選取的 %d 頁目錄圖片傳送到目前設定的網頁版 AI。\\n\\n請確認頁碼和內容無誤後傳送。",
    },
    # --- buttons ----------------------------------------------------------
    "Send TOC Pages": {
        "cn": "发送目录页", "tw": "傳送目錄頁",
        "de": "Verzeichnisseiten senden", "es": "Enviar páginas del índice",
        "fr": "Envoyer les pages du sommaire", "ja": "目次ページを送信",
        "kr": "목차 페이지 보내기", "ru": "Отправить страницы оглавления",
    },
    "Page %d": {
        "cn": "第 %d 页", "tw": "第 %d 頁",
        "de": "Seite %d", "es": "Página %d", "fr": "Page %d",
        "ja": "%d ページ", "kr": "%d 페이지", "ru": "Страница %d",
    },
    "Resend": {
        "cn": "重新发送", "tw": "重新傳送",
        "de": "Erneut senden", "es": "Reenviar",
        "fr": "Renvoyer", "ja": "再送信",
        "kr": "다시 보내기", "ru": "Отправить снова",
    },
    "Send": {
        "cn": "发送", "tw": "傳送",
    },
    # --- sending / import progress -----------------------------------------
    # --- extraction progress stages ----------------------------------------
    "Scanning contents pages… %d / %d": {
        "cn": "正在扫描目录页… %d / %d", "tw": "正在掃描目錄頁… %d / %d",
        "de": "Verzeichnisseiten werden gescannt… %d / %d",
        "es": "Escaneando páginas del índice… %d / %d",
        "fr": "Analyse des pages du sommaire… %d / %d",
        "ja": "目次ページを走査中… %d / %d",
        "kr": "목차 페이지 스캔 중… %d / %d",
        "ru": "Сканирование страниц оглавления… %d / %d",
    },
    "Analyzing body pages… %d / %d": {
        "cn": "正在分析正文页… %d / %d", "tw": "正在分析正文頁… %d / %d",
        "de": "Textseiten werden analysiert… %d / %d",
        "es": "Analizando páginas del cuerpo… %d / %d",
        "fr": "Analyse des pages du corps… %d / %d",
        "ja": "本文ページを解析中… %d / %d",
        "kr": "본문 페이지 분석 중… %d / %d",
        "ru": "Анализ страниц основного текста… %d / %d",
    },
    "Calibrating bookmark targets… %d / %d": {
        "cn": "正在校准书签目标… %d / %d", "tw": "正在校準書籤目標… %d / %d",
        "de": "Lesezeichenziele werden kalibriert… %d / %d",
        "es": "Calibrando destinos de marcadores… %d / %d",
        "fr": "Calibrage des cibles de signets… %d / %d",
        "ja": "しおりターゲットを校正中… %d / %d",
        "kr": "책갈피 대상 보정 중… %d / %d",
        "ru": "Калибровка целей закладок… %d / %d",
    },
    "Preparing %d TOC page images for sending…": {
        "cn": "正在准备 %d 张目录图片并发送…", "tw": "正在準備 %d 張目錄圖片並傳送…",
        "de": "%d Verzeichnisseiten werden zum Senden vorbereitet…",
        "es": "Preparando %d imágenes de páginas del índice para enviar…",
        "fr": "Préparation de l’envoi de %d images de pages de sommaire…",
        "ja": "目次ページ画像 %d 枚を送信準備中…",
        "kr": "목차 페이지 이미지 %d장을 보낼 준비 중…",
        "ru": "Подготовка к отправке изображений страниц оглавления (%d)…",
    },
    "Resending body structure…": {
        "cn": "正在重新发送正文结构…", "tw": "正在重新傳送正文結構…",
    },
    "Resending TOC pages…": {
        "cn": "正在重新发送目录页…", "tw": "正在重新傳送目錄頁…",
    },
    "Sending prompt…": {
        "cn": "正在发送提示词…", "tw": "正在傳送提示詞…",
    },
    "Importing TOC: parsing the AI reply…": {
        "cn": "正在导入目录：解析 AI 返回内容…", "tw": "正在匯入目錄：解析 AI 回傳內容…",
    },
    "Importing TOC: arranging bookmark levels…": {
        "cn": "正在导入目录：整理书签层级…", "tw": "正在匯入目錄：整理書籤層級…",
    },
    "Importing TOC: calibrating bookmark destinations…": {
        "cn": "正在导入目录：校准书签位置…", "tw": "正在匯入目錄：校準書籤位置…",
    },
    "Importing TOC: calibrating bookmark destinations… %d / %d": {
        "cn": "正在导入目录：校准书签位置… %d / %d", "tw": "正在匯入目錄：校準書籤位置… %d / %d",
    },
    "Importing TOC: restoring titles and page numbers…": {
        "cn": "正在导入目录：还原标题与页码…", "tw": "正在匯入目錄：還原標題與頁碼…",
    },
    "TOC import finished.": {
        "cn": "目录导入完成。", "tw": "目錄匯入完成。",
    },
    "TOC import not finished. Copy the AI reply again to retry.": {
        "cn": "目录导入未完成，可再次复制 AI 回复重试。",
        "tw": "目錄匯入未完成，可再次複製 AI 回覆重試。",
    },
    "TOC import failed. Copy the full AI TOC JSON and try again.": {
        "cn": "目录导入失败，请复制完整的 AI 目录 JSON 后重试。",
        "tw": "目錄匯入失敗，請複製完整的 AI 目錄 JSON 後重試。",
    },
    # --- error / warning boxes ---------------------------------------------
    "AI returned invalid JSON.": {
        "cn": "AI 返回的 JSON 无效。", "tw": "AI 傳回的 JSON 無效。",
    },
    "Choose between 1 and 20 PDF pages.": {
        "cn": "选择 1 到 20 个 PDF 页码。", "tw": "請選擇 1 到 20 個 PDF 頁碼。",
    },
    "Unable to paste the AI prompt into the browser.": {
        "cn": "无法将 AI 提示词粘贴到浏览器中。", "tw": "無法將 AI 提示詞貼上到瀏覽器中。",
    },
    "Cannot access the clipboard. Please retry.": {
        "cn": "无法访问剪贴板，请重试。", "tw": "無法存取剪貼簿，請重試。",
    },
    "Cannot open the configured AI page.": {
        "cn": "无法打开已配置的 AI 网页。", "tw": "無法開啟已設定的 AI 網頁。",
    },
    "The AI page opened, but the initial message could not be sent.": {
        "cn": "AI 网页已打开，但无法发送初始化消息。", "tw": "AI 網頁已開啟，但無法傳送初始化訊息。",
    },
    "Automatic send incomplete: the heading-candidates prompt is on the clipboard. Paste it into the AI input box and send.": {
        "cn": "自动发送未完成：标题候选提示词已放入剪贴板，可在 AI 输入框粘贴后发送。",
        "tw": "自動傳送未完成：標題候選提示詞已放入剪貼簿，可在 AI 輸入框貼上後傳送。",
    },
    "Automatic send incomplete. Paste the clipboard content into the AI input box and send.": {
        "cn": "自动发送未完成，可在 AI 输入框粘贴剪贴板内容后发送。",
        "tw": "自動傳送未完成，可在 AI 輸入框貼上剪貼簿內容後傳送。",
    },
    "Cannot create the image folder. Check disk space and try again.": {
        "cn": "无法创建图片文件夹，请检查磁盘空间后重试。",
        "tw": "無法建立圖片資料夾，請檢查磁碟空間後重試。",
    },
    "Failed to render the TOC page images. Try again or adjust the pages.": {
        "cn": "目录页图片生成失败，请重试或调整页码。",
        "tw": "目錄頁圖片產生失敗，請重試或調整頁碼。",
    },
    "Failed to save images. Check disk space and try again.": {
        "cn": "图片保存失败，请检查磁盘空间后重试。",
        "tw": "圖片儲存失敗，請檢查磁碟空間後重試。",
    },
    "Cannot watch the clipboard. Close the window and try again. The prompt was not sent.": {
        "cn": "无法监听剪贴板，请关闭窗口后重试。提示词尚未发送。",
        "tw": "無法監聽剪貼簿，請關閉視窗後重試。提示詞尚未傳送。",
    },
    "Clipboard is temporarily unreadable. Retrying; you can also copy the AI reply again.": {
        "cn": "剪贴板暂时不可读，正在重试。也可以再次复制 AI 回复。",
        "tw": "剪貼簿暫時不可讀，正在重試。也可以再次複製 AI 回覆。",
    },
    # --- chooser dialog ------------------------------------------------------
    "Local Extraction": {
        "cn": "本地提取", "tw": "本地擷取",
        "de": "Lokale Extraktion", "es": "Extracción local",
        "fr": "Extraction locale", "ja": "ローカル抽出",
        "kr": "로컬 추출", "ru": "Локальное извлечение",
    },
    "Online AI Recognition": {
        "cn": "联网识别", "tw": "網路辨識",
        "de": "Online-KI-Erkennung", "es": "Reconocimiento IA en línea",
        "fr": "Reconnaissance IA en ligne", "ja": "オンライン AI 認識",
        "kr": "온라인 AI 인식", "ru": "Онлайн-распознавание ИИ",
    },
    "Generate TOC From Document Structure": {
        "cn": "从文档结构生成目录", "tw": "從文件結構產生目錄",
        "de": "Inhaltsverzeichnis aus Dokumentstruktur erstellen",
        "es": "Generar índice desde la estructura del documento",
        "fr": "Générer le sommaire depuis la structure du document",
        "ja": "文書構造から目次を生成",
        "kr": "문서 구조에서 목차 생성",
        "ru": "Создать оглавление по структуре документа",
    },
    "AI Recognition for Printed TOC and Body Structure": {
        "cn": "AI 识别印刷目录与正文结构", "tw": "AI 辨識印刷目錄與正文結構",
        "de": "KI-Erkennung für gedrucktes Verzeichnis und Textstruktur",
        "es": "Reconocimiento IA de índice impreso y estructura del texto",
        "fr": "Reconnaissance IA du sommaire imprimé et de la structure du texte",
        "ja": "印刷目次と本文構造の AI 認識",
        "kr": "인쇄 목차·본문 구조 AI 인식",
        "ru": "ИИ-распознавание печатного оглавления и структуры текста",
    },
    "Analyzes text, heading levels, numbering and link targets to build the TOC locally and fast.\\nBest for official-document style PDFs; works even better when the PDF already has native text and a clickable TOC.": {
        "cn": "分析文档中的文字、标题层级、编号和跳转链接，本地快速生成目录。\\n特别适用于公文类文档；对已包含原生文字且具有可跳转目录的文档，识别效果更佳。",
        "tw": "分析文件中的文字、標題層級、編號和跳轉連結，在本機快速產生目錄。\\n特別適用於公文類文件；對已包含原生文字且具有可跳轉目錄的文件，辨識效果更佳。",
    },
    "Extracts the structure from printed TOC pages, or from the body text structure.\\nRecognized by the web AI; slower than local extraction.": {
        "cn": "支持从印刷版目录页提取结构，也可从中文正文文本结构提取内容生成目录。\\n由网页版 AI 识别，处理速度相对本地识别较慢。",
        "tw": "支援從印刷版目錄頁擷取結構，也可從中文正文文字結構擷取內容產生目錄。\\n由網頁版 AI 辨識，處理速度相對本地辨識較慢。",
    },
    "Processed locally and fast. Nothing is uploaded.": {
        "cn": "本地快速处理，全程无需上传文件。", "tw": "本機快速處理，全程無需上傳檔案。",
    },
    "Needs internet: only TOC page images or heading candidates are uploaded.": {
        "cn": "需联网上传：仅发送目录页图片或标题候选文本。",
        "tw": "需連線上傳：僅傳送目錄頁圖片或標題候選文字。",
    },
    "Confirm Sending TOC Pages": {
        "cn": "确认发送目录页", "tw": "確認傳送目錄頁",
    },
    "Choose an extraction method:": {
        "cn": "请选择提取方式：", "tw": "請選擇擷取方式：",
    },
}

ANCHOR = "Extract Table of Contents"  # keep TOC blocks together after this one


def parse(text: str) -> tuple[list[str], list[tuple[str, list[str]]]]:
    lines = text.split("\n")
    header = lines[:2]
    blocks: list[tuple[str, list[str]]] = []
    cur: tuple[str, list[str]] | None = None
    for line in lines[2:]:
        if not line:
            continue
        if line.startswith(":"):
            if cur:
                blocks.append(cur)
            cur = (line[1:], [])
        elif cur:
            cur[1].append(line)
    if cur:
        blocks.append(cur)
    return header, blocks


def serialize(header: list[str], blocks: list[tuple[str, list[str]]]) -> str:
    out = header[:]
    for msgid, trans in blocks:
        out.append(f":{msgid}")
        out.extend(trans)
    return "\n".join(out) + "\n"


def apply(path: Path) -> None:
    header, blocks = parse(path.read_text(encoding="utf-8"))

    # 1. delete
    blocks = [b for b in blocks if b[0] not in DELETE]

    # 2. update existing
    by_id = {b[0]: i for i, b in enumerate(blocks)}
    for msgid, trans in UPDATE.items():
        assert msgid in by_id, f"missing block {msgid!r}"
        i = by_id[msgid]
        id_, lines = blocks[i]
        lang_map = dict(line.split(":", 1) for line in lines if ":" in line)
        lang_map.update(trans)
        blocks[i] = (id_, [f"{k}:{lang_map[k]}" for k in LANGS if k in lang_map])

    # 3. insert new blocks after the anchor
    by_id = {b[0]: i for i, b in enumerate(blocks)}
    for msgid, trans in NEW.items():
        if msgid in by_id:
            i = by_id[msgid]
            id_, lines = blocks[i]
            lang_map = dict(line.split(":", 1) for line in lines if ":" in line)
            lang_map.update(trans)
            blocks[i] = (id_, [f"{k}:{lang_map[k]}" for k in LANGS if k in lang_map])
        else:
            assert ANCHOR in by_id, f"missing anchor {ANCHOR!r}"
            pos = by_id[ANCHOR] + 1
            block = (msgid, [f"{k}:{trans[k]}" for k in LANGS if k in trans])
            blocks.insert(pos, block)
            by_id = {b[0]: i for i, b in enumerate(blocks)}

    path.write_text(serialize(header, blocks), encoding="utf-8", newline="\n")
    print(f"{path.name}: {len(blocks)} blocks")


# Force the resource compiler to re-embed translations.txt.lzsa: SumatraPDF.rc
# has no tracked dependency on the archive, so without a timestamp bump the
# resource step is skipped and the exe keeps serving stale translations.

def touch_rc() -> None:
    rc = ROOT / "src" / "SumatraPDF.rc"
    if rc.exists():
        os.utime(rc, None)

def main() -> None:
    for path in (GOOD, TXT):
        apply(path)
    touch_rc()
    print(f"TOC extraction translations updated: {len(NEW)} new, {len(UPDATE)} updated, {len(DELETE)} removed")


if __name__ == "__main__":
    main()
