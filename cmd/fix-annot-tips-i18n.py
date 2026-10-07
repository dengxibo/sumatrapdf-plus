#!/usr/bin/env python3
"""Fill annotation types, colors (ensure), toolbar tips, signature pad, page-adjust dialog."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAJORS = [
    "cn", "tw", "ja", "kr", "de", "fr", "es", "it", "pt", "br", "ru", "uk", "pl", "nl", "tr", "vn",
]

# key -> {lang: text}
FILLS: dict[str, dict[str, str]] = {}


def add(key: str, **langs: str) -> None:
    FILLS[key] = langs


# --- Annotation readable names (sidebar list / menus) ---
add(
    "Text",
    cn="文本", tw="文字", ja="テキスト", kr="텍스트", de="Text", fr="Texte", es="Texto", it="Testo",
    pt="Texto", br="Texto", ru="Текст", uk="Текст", pl="Tekst", nl="Tekst", tr="Metin", vn="Văn bản",
)
add(
    "Free Text",
    cn="自由文本", tw="自由文字", ja="フリーテキスト", kr="자유 텍스트", de="Freitext", fr="Texte libre",
    es="Texto libre", it="Testo libero", pt="Texto livre", br="Texto livre", ru="Свободный текст",
    uk="Вільний текст", pl="Tekst swobodny", nl="Vrije tekst", tr="Serbest metin", vn="Chữ tự do",
)
add(
    "Stamp",
    cn="图章", tw="圖章", ja="スタンプ", kr="스탬프", de="Stempel", fr="Tampon", es="Sello", it="Timbro",
    pt="Carimbo", br="Carimbo", ru="Штамп", uk="Штамп", pl="Pieczątka", nl="Stempel", tr="Kaşe", vn="Con dấu",
)
add(
    "Highlight",
    cn="高亮", tw="螢光筆", ja="ハイライト", kr="형광", de="Hervorheben", fr="Surlignage", es="Resaltar",
    it="Evidenzia", pt="Realçar", br="Realçar", ru="Выделение", uk="Підсвічування", pl="Podświetlenie",
    nl="Markeren", tr="Vurgula", vn="Tô sáng",
)
add(
    "Underline",
    cn="下划线", tw="底線", ja="下線", kr="밑줄", de="Unterstreichen", fr="Souligner", es="Subrayar",
    it="Sottolinea", pt="Sublinhado", br="Sublinhado", ru="Подчёркивание", uk="Підкреслення", pl="Podkreślenie",
    nl="Onderstrepen", tr="Altı çizili", vn="Gạch dưới",
)
add(
    "StrikeOut",
    cn="删除线", tw="刪除線", ja="取り消し線", kr="취소선", de="Durchstreichen", fr="Barré", es="Tachado",
    it="Barrato", pt="Rasurado", br="Riscado", ru="Зачёркивание", uk="Закреслення", pl="Przekreślenie",
    nl="Doorhalen", tr="Üstü çizili", vn="Gạch ngang",
)
add(
    "Squiggly",
    cn="波浪线", tw="波浪線", ja="波線", kr="물결선", de="Wellenlinie", fr="Soulignement ondulé",
    es="Subrayado ondulado", it="Ondulato", pt="Ondulado", br="Ondulado", ru="Волнистое",
    uk="Хвилясте", pl="Faliste", nl="Golvend", tr="Dalgalı", vn="Gạch sóng",
)
add(
    "Caret",
    cn="插入符", tw="插入符", ja="キャレット", kr="캐럿", de="Caret", fr="Caret", es="Caret", it="Caret",
    pt="Caret", br="Caret", ru="Каретка", uk="Каретка", pl="Karetka", nl="Caret", tr="İmleç", vn="Dấu chèn",
)
add(
    "File Attachment",
    cn="文件附件", tw="檔案附件", ja="ファイル添付", kr="파일 첨부", de="Dateianhang", fr="Pièce jointe",
    es="Archivo adjunto", it="Allegato", pt="Anexo", br="Anexo", ru="Вложение", uk="Вкладення",
    pl="Załącznik", nl="Bijlage", tr="Dosya eki", vn="Tệp đính kèm",
)
add(
    "Poly Line",
    cn="折线", tw="折線", ja="折れ線", kr="꺾은선", de="Polylinie", fr="Polyligne", es="Polilínea",
    it="Polilinea", pt="Polilinha", br="Polilinha", ru="Ломаная", uk="Ламана", pl="Polilinia",
    nl="Polylijn", tr="Çoklu çizgi", vn="Đường gấp khúc",
)
add(
    "Popup",
    cn="弹出", tw="快顯", ja="ポップアップ", kr="팝업", de="Popup", fr="Popup", es="Emergente", it="Popup",
    pt="Popup", br="Popup", ru="Всплывающее", uk="Спливаюче", pl="Popup", nl="Popup", tr="Açılır", vn="Popup",
)

# --- Toolbar tips ---
add(
    "Handwritten Signature",
    cn="手写签名", tw="手寫簽名", ja="手書き署名", kr="손글씨 서명", de="Handschriftliche Unterschrift",
    fr="Signature manuscrite", es="Firma manuscrita", it="Firma autografa", pt="Assinatura manuscrita",
    br="Assinatura manuscrita", ru="Рукописная подпись", uk="Рукописний підпис", pl="Podpis odręczny",
    nl="Handgeschreven handtekening", tr="El yazısı imza", vn="Chữ ký viết tay",
)
add(
    "Enable Enhance Display",
    cn="启用显示增强", tw="啟用顯示增強", ja="表示強化を有効にする", kr="표시 향상 사용",
    de="Anzeigeverbesserung aktivieren", fr="Activer l’amélioration de l’affichage",
    es="Activar mejora de visualización", it="Abilita miglioramento visualizzazione",
    pt="Ativar melhoria de visualização", br="Ativar melhoria de exibição",
    ru="Включить улучшение отображения", uk="Увімкнути покращення відображення",
    pl="Włącz ulepszenie wyświetlania", nl="Weergaveverbetering inschakelen",
    tr="Görüntü geliştirmeyi aç", vn="Bật tăng cường hiển thị",
)
add(
    "Enhance Display",
    cn="显示增强", tw="顯示增強", ja="表示強化", kr="표시 향상", de="Anzeigeverbesserung",
    fr="Amélioration de l’affichage", es="Mejora de visualización", it="Miglioramento visualizzazione",
    pt="Melhoria de visualização", br="Melhoria de exibição", ru="Улучшение отображения",
    uk="Покращення відображення", pl="Ulepszenie wyświetlania", nl="Weergaveverbetering",
    tr="Görüntü geliştirme", vn="Tăng cường hiển thị",
)
add(
    "Enhance Display is enabled",
    cn="显示增强已启用", tw="顯示增強已啟用", ja="表示強化が有効です", kr="표시 향상이 켜져 있습니다",
    de="Anzeigeverbesserung ist aktiviert", fr="L’amélioration de l’affichage est activée",
    es="La mejora de visualización está activada", it="Miglioramento visualizzazione attivo",
    pt="Melhoria de visualização ativada", br="Melhoria de exibição ativada",
    ru="Улучшение отображения включено", uk="Покращення відображення увімкнено",
    pl="Ulepszenie wyświetlania jest włączone", nl="Weergaveverbetering is ingeschakeld",
    tr="Görüntü geliştirme açık", vn="Đã bật tăng cường hiển thị",
)
add(
    "Enhance Display is only available for PDF",
    cn="显示增强仅适用于 PDF", tw="顯示增強僅適用於 PDF", ja="表示強化は PDF のみ利用できます",
    kr="표시 향상은 PDF에서만 사용할 수 있습니다", de="Anzeigeverbesserung nur für PDF verfügbar",
    fr="L’amélioration de l’affichage n’est disponible que pour les PDF",
    es="La mejora de visualización solo está disponible para PDF",
    it="Miglioramento visualizzazione disponibile solo per PDF",
    pt="Melhoria de visualização disponível apenas para PDF",
    br="Melhoria de exibição disponível apenas para PDF",
    ru="Улучшение отображения доступно только для PDF",
    uk="Покращення відображення доступне лише для PDF",
    pl="Ulepszenie wyświetlania jest dostępne tylko dla PDF",
    nl="Weergaveverbetering is alleen beschikbaar voor PDF",
    tr="Görüntü geliştirme yalnızca PDF için kullanılabilir",
    vn="Tăng cường hiển thị chỉ dùng cho PDF",
)

# --- Signature pad ---
add(
    "Open Image",
    cn="打开图片", tw="開啟圖片", ja="画像を開く", kr="이미지 열기", de="Bild öffnen", fr="Ouvrir une image",
    es="Abrir imagen", it="Apri immagine", pt="Abrir imagem", br="Abrir imagem", ru="Открыть изображение",
    uk="Відкрити зображення", pl="Otwórz obraz", nl="Afbeelding openen", tr="Görüntü aç", vn="Mở ảnh",
)
add(
    "Clear",
    cn="清除", tw="清除", ja="消去", kr="지우기", de="Löschen", fr="Effacer", es="Borrar", it="Cancella",
    pt="Limpar", br="Limpar", ru="Очистить", uk="Очистити", pl="Wyczyść", nl="Wissen", tr="Temizle", vn="Xóa",
)
add(
    "Draw your signature here",
    cn="在此处书写签名", tw="在此書寫簽名", ja="ここに署名を書いてください", kr="여기에 서명을 쓰세요",
    de="Unterschrift hier zeichnen", fr="Dessinez votre signature ici", es="Dibuje su firma aquí",
    it="Disegna la firma qui", pt="Desenhe a assinatura aqui", br="Desenhe a assinatura aqui",
    ru="Нарисуйте подпись здесь", uk="Намалюйте підпис тут", pl="Narysuj podpis tutaj",
    nl="Teken hier uw handtekening", tr="İmzanızı buraya çizin", vn="Vẽ chữ ký tại đây",
)
add(
    "Paste or drop an image",
    cn="粘贴或拖放图片", tw="貼上或拖放圖片", ja="画像を貼り付けるかドロップ", kr="이미지를 붙여넣거나 놓기",
    de="Bild einfügen oder ablegen", fr="Collez ou déposez une image", es="Pegue o suelte una imagen",
    it="Incolla o rilascia un’immagine", pt="Colar ou largar uma imagem", br="Colar ou soltar uma imagem",
    ru="Вставьте или перетащите изображение", uk="Вставте або перетягніть зображення",
    pl="Wklej lub upuść obraz", nl="Plak of sleep een afbeelding", tr="Görüntü yapıştırın veya bırakın",
    vn="Dán hoặc thả ảnh",
)

# --- Manual page adjust ---
add(
    "Apply To",
    cn="应用于", tw="套用至", ja="適用先", kr="적용 대상", de="Anwenden auf", fr="Appliquer à",
    es="Aplicar a", it="Applica a", pt="Aplicar a", br="Aplicar a", ru="Применить к", uk="Застосувати до",
    pl="Zastosuj do", nl="Toepassen op", tr="Uygula", vn="Áp dụng cho",
)
add(
    "Any Angle",
    cn="任意角度", tw="任意角度", ja="任意の角度", kr="임의 각도", de="Beliebiger Winkel", fr="Angle libre",
    es="Cualquier ángulo", it="Angolo qualsiasi", pt="Qualquer ângulo", br="Qualquer ângulo",
    ru="Произвольный угол", uk="Довільний кут", pl="Dowolny kąt", nl="Elke hoek", tr="Herhangi açı",
    vn="Góc tùy ý",
)
add(
    "Left 90°",
    cn="向左 90°", tw="向左 90°", ja="左に 90°", kr="왼쪽 90°", de="90° nach links", fr="90° à gauche",
    es="90° a la izquierda", it="90° a sinistra", pt="90° à esquerda", br="90° à esquerda",
    ru="На 90° влево", uk="На 90° ліворуч", pl="90° w lewo", nl="90° links", tr="Sola 90°", vn="Trái 90°",
)
add(
    "Right 90°",
    cn="向右 90°", tw="向右 90°", ja="右に 90°", kr="오른쪽 90°", de="90° nach rechts", fr="90° à droite",
    es="90° a la derecha", it="90° a destra", pt="90° à direita", br="90° à direita",
    ru="На 90° вправо", uk="На 90° праворуч", pl="90° w prawo", nl="90° rechts", tr="Sağa 90°", vn="Phải 90°",
)
add(
    "Flip Horizontal",
    cn="水平翻转", tw="水平翻轉", ja="左右反転", kr="좌우 뒤집기", de="Horizontal spiegeln",
    fr="Retourner horizontalement", es="Voltear horizontalmente", it="Capovolgi orizzontalmente",
    pt="Inverter horizontalmente", br="Inverter horizontalmente", ru="Отразить по горизонтали",
    uk="Віддзеркалити горизонтально", pl="Odwróć w poziomie", nl="Horizontaal spiegelen",
    tr="Yatay çevir", vn="Lật ngang",
)
add(
    "Flip Vertical",
    cn="垂直翻转", tw="垂直翻轉", ja="上下反転", kr="상하 뒤집기", de="Vertikal spiegeln",
    fr="Retourner verticalement", es="Voltear verticalmente", it="Capovolgi verticalmente",
    pt="Inverter verticalmente", br="Inverter verticalmente", ru="Отразить по вертикали",
    uk="Віддзеркалити вертикально", pl="Odwróć w pionie", nl="Verticaal spiegelen",
    tr="Dikey çevir", vn="Lật dọc",
)
add(
    "Auto Crop",
    cn="自动裁切", tw="自動裁切", ja="自動切り抜き", kr="자동 자르기", de="Automatisch zuschneiden",
    fr="Recadrage automatique", es="Recorte automático", it="Ritaglio automatico",
    pt="Recorte automático", br="Recorte automático", ru="Автообрезка", uk="Автообрізання",
    pl="Auto przycinanie", nl="Automatisch bijsnijden", tr="Otomatik kırp", vn="Tự động cắt",
)
add(
    "Specified Pages",
    cn="指定页面", tw="指定頁面", ja="指定ページ", kr="지정 페이지", de="Angegebene Seiten",
    fr="Pages spécifiées", es="Páginas especificadas", it="Pagine specificate",
    pt="Páginas especificadas", br="Páginas especificadas", ru="Указанные страницы",
    uk="Вказані сторінки", pl="Wybrane strony", nl="Opgegeven pagina’s", tr="Belirtilen sayfalar",
    vn="Trang chỉ định",
)
add(
    "Current Page (Page %d)",
    cn="当前页（第 %d 页）", tw="目前頁（第 %d 頁）", ja="現在のページ（%d ページ）",
    kr="현재 페이지(%d페이지)", de="Aktuelle Seite (Seite %d)", fr="Page actuelle (page %d)",
    es="Página actual (página %d)", it="Pagina corrente (pagina %d)", pt="Página atual (página %d)",
    br="Página atual (página %d)", ru="Текущая страница (стр. %d)", uk="Поточна сторінка (стор. %d)",
    pl="Bieżąca strona (strona %d)", nl="Huidige pagina (pagina %d)", tr="Geçerli sayfa (Sayfa %d)",
    vn="Trang hiện tại (Trang %d)",
)
add(
    "All Pages (%d)",
    cn="全部页面（%d）", tw="全部頁面（%d）", ja="すべてのページ（%d）", kr="모든 페이지(%d)",
    de="Alle Seiten (%d)", fr="Toutes les pages (%d)", es="Todas las páginas (%d)",
    it="Tutte le pagine (%d)", pt="Todas as páginas (%d)", br="Todas as páginas (%d)",
    ru="Все страницы (%d)", uk="Усі сторінки (%d)", pl="Wszystkie strony (%d)",
    nl="Alle pagina’s (%d)", tr="Tüm sayfalar (%d)", vn="Tất cả trang (%d)",
)
add(
    "e.g. 2, 5-7, 13-",
    cn="例如 2, 5-7, 13-", tw="例如 2, 5-7, 13-", ja="例: 2, 5-7, 13-", kr="예: 2, 5-7, 13-",
    de="z. B. 2, 5-7, 13-", fr="ex. 2, 5-7, 13-", es="p. ej. 2, 5-7, 13-", it="es. 2, 5-7, 13-",
    pt="ex. 2, 5-7, 13-", br="ex. 2, 5-7, 13-", ru="напр. 2, 5-7, 13-", uk="напр. 2, 5-7, 13-",
    pl="np. 2, 5-7, 13-", nl="bijv. 2, 5-7, 13-", tr="örn. 2, 5-7, 13-", vn="vd. 2, 5-7, 13-",
)

# --- Annot create submenu (mnemonics) ---
add(
    "&Text",
    cn="文本(&T)", tw="文字(&T)", ja="テキスト(&T)", kr="텍스트(&T)", de="&Text", fr="&Texte",
    es="&Texto", it="&Testo", pt="&Texto", br="&Texto", ru="&Текст", uk="&Текст", pl="&Tekst",
    nl="&Tekst", tr="&Metin", vn="Văn &bản",
)
add(
    "&Free Text",
    cn="自由文本(&F)", tw="自由文字(&F)", ja="フリーテキスト(&F)", kr="자유 텍스트(&F)", de="&Freitext",
    fr="Texte &libre", es="Texto &libre", it="Testo &libero", pt="Texto &livre", br="Texto &livre",
    ru="&Свободный текст", uk="&Вільний текст", pl="Tekst &swobodny", nl="&Vrije tekst",
    tr="&Serbest metin", vn="Chữ tự &do",
)
add(
    "&Stamp",
    cn="图章(&S)", tw="圖章(&S)", ja="スタンプ(&S)", kr="스탬프(&S)", de="&Stempel", fr="&Tampon",
    es="&Sello", it="&Timbro", pt="&Carimbo", br="&Carimbo", ru="&Штамп", uk="&Штамп",
    pl="&Pieczątka", nl="&Stempel", tr="&Kaşe", vn="Con &dấu",
)
add(
    "&Highlight",
    cn="高亮(&H)", tw="螢光筆(&H)", ja="ハイライト(&H)", kr="형광(&H)", de="&Hervorheben",
    fr="&Surlignage", es="&Resaltar", it="&Evidenzia", pt="&Realçar", br="&Realçar",
    ru="&Выделение", uk="&Підсвічування", pl="&Podświetlenie", nl="&Markeren", tr="&Vurgula",
    vn="Tô &sáng",
)


def parse_blocks(text: str) -> tuple[list[str], dict[str, dict[str, str]]]:
    lines = text.splitlines()
    header: list[str] = []
    blocks: dict[str, dict[str, str]] = {}
    i = 0
    while i < len(lines) and not lines[i].startswith(":"):
        header.append(lines[i])
        i += 1
    key = None
    cur: dict[str, str] = {}
    for line in lines[i:]:
        if line.startswith(":"):
            if key is not None:
                blocks[key] = cur
            key = line[1:]
            cur = {}
        elif key is not None and ":" in line:
            lang, val = line.split(":", 1)
            cur[lang] = val
    if key is not None:
        blocks[key] = cur
    return header, blocks


def serialize(header: list[str], blocks: dict[str, dict[str, str]]) -> str:
    out = list(header)
    while len(out) < 2:
        out.append("AppTranslator: SumatraPDF")
    for key in sorted(blocks.keys(), key=lambda s: s.casefold()):
        out.append(":" + key)
        for lang in sorted(blocks[key].keys()):
            out.append(f"{lang}:{blocks[key][lang]}")
    return "\n".join(out) + "\n"


def upsert(path: Path) -> int:
    header, blocks = parse_blocks(path.read_text(encoding="utf-8"))
    changed = 0
    for key, langs in FILLS.items():
        cur = dict(blocks.get(key, {}))
        touched = False
        for lang, val in langs.items():
            if lang not in MAJORS:
                continue
            if cur.get(lang) != val:
                cur[lang] = val
                touched = True
        if touched:
            blocks[key] = cur
            changed += 1
    path.write_text(serialize(header, blocks), encoding="utf-8")
    return changed


if __name__ == "__main__":
    for name in ("translations-good.txt", "translations.txt"):
        n = upsert(ROOT / "translations" / name)
        print(f"Updated {name}: {n} keys")
    r = subprocess.run(["bun", str(ROOT / "cmd" / "sync-major-langs-to-good.ts")], cwd=ROOT)
    raise SystemExit(r.returncode)
