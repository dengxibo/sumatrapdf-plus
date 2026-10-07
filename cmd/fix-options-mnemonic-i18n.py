#!/usr/bin/env python3
"""Fill Options dialog keys that include mnemonic & (exact UI keys)."""
from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAJORS = ["cn", "tw", "ja", "kr", "de", "fr", "es", "it", "pt", "br", "ru", "uk", "pl", "nl", "tr", "vn"]
FILLS: dict[str, dict[str, str]] = {}


def add(key: str, **langs: str) -> None:
    FILLS[key] = langs


# --- Toolbar / sidebar tips ---
add(
    "Decrease Font Size",
    cn="减小字号", tw="縮小字級", ja="フォントサイズを縮小", kr="글꼴 크기 줄이기",
    de="Schrift verkleinern", fr="Réduire la taille de police", es="Reducir tamaño de fuente",
    it="Riduci dimensione carattere", pt="Diminuir tamanho da letra", br="Diminuir tamanho da fonte",
    ru="Уменьшить шрифт", uk="Зменшити шрифт", pl="Zmniejsz czcionkę", nl="Lettergrootte verkleinen",
    tr="Yazı tipini küçült", vn="Giảm cỡ chữ",
)
add(
    "Increase Font Size",
    cn="增大字号", tw="放大字級", ja="フォントサイズを拡大", kr="글꼴 크기 늘리기",
    de="Schrift vergrößern", fr="Augmenter la taille de police", es="Aumentar tamaño de fuente",
    it="Aumenta dimensione carattere", pt="Aumentar tamanho da letra", br="Aumentar tamanho da fonte",
    ru="Увеличить шрифт", uk="Збільшити шрифт", pl="Zwiększ czcionkę", nl="Lettergrootte vergroten",
    tr="Yazı tipini büyüt", vn="Tăng cỡ chữ",
)
add(
    "Thumbnails",
    cn="缩略图", tw="縮圖", ja="サムネイル", kr="미리보기", de="Miniaturansichten", fr="Vignettes",
    es="Miniaturas", it="Miniature", pt="Miniaturas", br="Miniaturas", ru="Эскизы", uk="Мініатюри",
    pl="Miniatury", nl="Miniaturen", tr="Küçük resimler", vn="Hình thu nhỏ",
)

# --- Read aloud bar tips ---
add(
    "Previous page recording",
    cn="上一页录音", tw="上一頁錄音", ja="前のページの録音", kr="이전 페이지 녹음",
    de="Aufnahme der vorherigen Seite", fr="Enregistrement de la page précédente",
    es="Grabación de la página anterior", it="Registrazione pagina precedente",
    pt="Gravação da página anterior", br="Gravação da página anterior",
    ru="Запись предыдущей страницы", uk="Запис попередньої сторінки",
    pl="Nagranie poprzedniej strony", nl="Opname vorige pagina", tr="Önceki sayfa kaydı",
    vn="Bản ghi trang trước",
)
add(
    "Next page recording",
    cn="下一页录音", tw="下一頁錄音", ja="次のページの録音", kr="다음 페이지 녹음",
    de="Aufnahme der nächsten Seite", fr="Enregistrement de la page suivante",
    es="Grabación de la página siguiente", it="Registrazione pagina successiva",
    pt="Gravação da página seguinte", br="Gravação da próxima página",
    ru="Запись следующей страницы", uk="Запис наступної сторінки",
    pl="Nagranie następnej strony", nl="Opname volgende pagina", tr="Sonraki sayfa kaydı",
    vn="Bản ghi trang sau",
)
add(
    "Play / Pause narration",
    cn="播放 / 暂停旁白", tw="播放 / 暫停旁白", ja="ナレーションの再生 / 一時停止",
    kr="나레이션 재생 / 일시정지", de="Erzählung wiedergeben / anhalten",
    fr="Lecture / Pause de la narration", es="Reproducir / Pausar narración",
    it="Riproduci / Pausa narrazione", pt="Reproduzir / Pausar narração",
    br="Reproduzir / Pausar narração", ru="Воспроизвести / пауза озвучки",
    uk="Відтворити / пауза озвучення", pl="Odtwórz / wstrzymaj narrację",
    nl="Vertelling afspelen / pauzeren", tr="Anlatımı oynat / duraklat", vn="Phát / Tạm dừng lời dẫn",
)
add(
    "Hide narration controls",
    cn="隐藏旁白控件", tw="隱藏旁白控制項", ja="ナレーションコントロールを隠す",
    kr="나레이션 컨트롤 숨기기", de="Erzählungssteuerung ausblenden",
    fr="Masquer les commandes de narration", es="Ocultar controles de narración",
    it="Nascondi controlli narrazione", pt="Ocultar controlos de narração",
    br="Ocultar controles de narração", ru="Скрыть управление озвучкой",
    uk="Сховати керування озвученням", pl="Ukryj sterowanie narracją",
    nl="Vertellingsbediening verbergen", tr="Anlatım denetimlerini gizle", vn="Ẩn điều khiển lời dẫn",
)
add(
    "Back 10 seconds",
    cn="后退 10 秒", tw="倒轉 10 秒", ja="10 秒戻る", kr="10초 뒤로", de="10 Sekunden zurück",
    fr="Reculer de 10 secondes", es="Retroceder 10 segundos", it="Indietro di 10 secondi",
    pt="Recuar 10 segundos", br="Voltar 10 segundos", ru="Назад на 10 секунд",
    uk="Назад на 10 секунд", pl="Cofnij o 10 sekund", nl="10 seconden terug",
    tr="10 saniye geri", vn="Lùi 10 giây",
)
add(
    "Forward 10 seconds",
    cn="前进 10 秒", tw="快轉 10 秒", ja="10 秒進む", kr="10초 앞으로", de="10 Sekunden vor",
    fr="Avancer de 10 secondes", es="Avanzar 10 segundos", it="Avanti di 10 secondi",
    pt="Avançar 10 segundos", br="Avançar 10 segundos", ru="Вперёд на 10 секунд",
    uk="Вперед на 10 секунд", pl="Do przodu o 10 sekund", nl="10 seconden vooruit",
    tr="10 saniye ileri", vn="Tiến 10 giây",
)
add(
    "Playback speed",
    cn="播放速度", tw="播放速度", ja="再生速度", kr="재생 속도", de="Wiedergabegeschwindigkeit",
    fr="Vitesse de lecture", es="Velocidad de reproducción", it="Velocità di riproduzione",
    pt="Velocidade de reprodução", br="Velocidade de reprodução", ru="Скорость воспроизведения",
    uk="Швидкість відтворення", pl="Prędkość odtwarzania", nl="Afspeelsnelheid",
    tr="Oynatma hızı", vn="Tốc độ phát",
)
add(
    "Scroll to the text being read",
    cn="滚动到正在朗读的文字", tw="捲動到正在朗讀的文字", ja="読み上げ中のテキストへスクロール",
    kr="읽는 중인 텍스트로 스크롤", de="Zum vorgelesenen Text scrollen",
    fr="Défiler jusqu’au texte lu", es="Desplazarse al texto que se lee",
    it="Scorri al testo in lettura", pt="Deslocar para o texto a ser lido",
    br="Rolar até o texto sendo lido", ru="Прокрутить к читаемому тексту",
    uk="Прокрутити до тексту, що читається", pl="Przewiń do czytanego tekstu",
    nl="Scrollen naar de gelezen tekst", tr="Okunan metne kaydır", vn="Cuộn tới chữ đang đọc",
)
add(
    "Read Aloud Settings",
    cn="朗读设置", tw="朗讀設定", ja="読み上げの設定", kr="소리 내어 읽기 설정",
    de="Vorlese-Einstellungen", fr="Paramètres de lecture à voix haute",
    es="Ajustes de lectura en voz alta", it="Impostazioni lettura ad alta voce",
    pt="Definições de leitura em voz alta", br="Configurações de leitura em voz alta",
    ru="Параметры чтения вслух", uk="Параметри читання вголос", pl="Ustawienia czytania na głos",
    nl="Instellingen voor voorlezen", tr="Sesli okuma ayarları", vn="Cài đặt đọc to",
)
add(
    "Follow",
    cn="跟随", tw="跟隨", ja="追従", kr="따라가기", de="Folgen", fr="Suivre", es="Seguir", it="Segui",
    pt="Seguir", br="Seguir", ru="Следовать", uk="Слідувати", pl="Śledź", nl="Volgen", tr="Takip et",
    vn="Theo dõi",
)
add(
    "Previous sentence",
    cn="上一句", tw="上一句", ja="前の文", kr="이전 문장", de="Vorheriger Satz", fr="Phrase précédente",
    es="Frase anterior", it="Frase precedente", pt="Frase anterior", br="Frase anterior",
    ru="Предыдущее предложение", uk="Попереднє речення", pl="Poprzednie zdanie",
    nl="Vorige zin", tr="Önceki cümle", vn="Câu trước",
)
add(
    "Next sentence",
    cn="下一句", tw="下一句", ja="次の文", kr="다음 문장", de="Nächster Satz", fr="Phrase suivante",
    es="Frase siguiente", it="Frase successiva", pt="Frase seguinte", br="Próxima frase",
    ru="Следующее предложение", uk="Наступне речення", pl="Następne zdanie",
    nl="Volgende zin", tr="Sonraki cümle", vn="Câu sau",
)
add(
    "Play / Pause reading",
    cn="播放 / 暂停朗读", tw="播放 / 暫停朗讀", ja="読み上げの再生 / 一時停止",
    kr="읽기 재생 / 일시정지", de="Vorlesen starten / anhalten", fr="Lecture / Pause",
    es="Reproducir / Pausar lectura", it="Riproduci / Pausa lettura",
    pt="Reproduzir / Pausar leitura", br="Reproduzir / Pausar leitura",
    ru="Воспроизвести / пауза чтения", uk="Відтворити / пауза читання",
    pl="Odtwórz / wstrzymaj czytanie", nl="Voorlezen afspelen / pauzeren",
    tr="Okumayı oynat / duraklat", vn="Phát / Tạm dừng đọc",
)
add(
    "Stop Reading",
    cn="停止朗读", tw="停止朗讀", ja="読み上げを停止", kr="읽기 중지", de="Vorlesen beenden",
    fr="Arrêter la lecture", es="Detener lectura", it="Interrompi lettura",
    pt="Parar leitura", br="Parar leitura", ru="Остановить чтение", uk="Зупинити читання",
    pl="Zatrzymaj czytanie", nl="Voorlezen stoppen", tr="Okumayı durdur", vn="Dừng đọc",
)
add(
    "Previous phrase",
    cn="上一短语", tw="上一片語", ja="前のフレーズ", kr="이전 구", de="Vorherige Phrase",
    fr="Phrase précédente", es="Frase anterior", it="Frase precedente", pt="Frase anterior",
    br="Frase anterior", ru="Предыдущая фраза", uk="Попередня фраза", pl="Poprzednia fraza",
    nl="Vorige zin", tr="Önceki ifade", vn="Cụm trước",
)
add(
    "Next phrase",
    cn="下一短语", tw="下一片語", ja="次のフレーズ", kr="다음 구", de="Nächste Phrase",
    fr="Phrase suivante", es="Frase siguiente", it="Frase successiva", pt="Frase seguinte",
    br="Próxima frase", ru="Следующая фраза", uk="Наступна фраза", pl="Następna fraza",
    nl="Volgende zin", tr="Sonraki ifade", vn="Cụm sau",
)

# --- Options: exact mnemonic keys from SumatraDialogs.cpp ---
add(
    "Restore the last &session at startup",
    cn="启动时还原上次会话(&S)", tw="啟動時還原上次工作階段(&S)", ja="起動時に前回のセッションを復元(&S)",
    kr="시작할 때 마지막 세션 복원(&S)", de="Letzte &Sitzung beim Start wiederherstellen",
    fr="Restaurer la dernière &session au démarrage", es="Restaurar la última &sesión al iniciar",
    it="Ripristina l’ultima &sessione all’avvio", pt="Restaurar a última &sessão ao iniciar",
    br="Restaurar a última &sessão na inicialização", ru="Восстанавливать последний &сеанс при запуске",
    uk="Відновлювати останній &сеанс під час запуску", pl="Przywróć ostatnią &sesję przy starcie",
    nl="Laatste &sessie herstellen bij opstarten", tr="Başlangıçta son &oturumu geri yükle",
    vn="Khôi phục &phiên trước khi khởi động",
)
add(
    "Open new files in the existing &instance",
    cn="在现有实例中打开新文件(&I)", tw="在現有執行個體中開啟新檔案(&I)",
    ja="既存のインスタンスで新しいファイルを開く(&I)", kr="기존 인스턴스에서 새 파일 열기(&I)",
    de="Neue Dateien in bestehender &Instanz öffnen", fr="Ouvrir les nouveaux fichiers dans l’&instance existante",
    es="Abrir archivos nuevos en la &instancia existente", it="Apri nuovi file nell’&istanza esistente",
    pt="Abrir novos ficheiros na &instância existente", br="Abrir novos arquivos na &instância existente",
    ru="Открывать новые файлы в текущем &экземпляре", uk="Відкривати нові файли в наявному &екземплярі",
    pl="Otwieraj nowe pliki w istniejącej &instancji", nl="Nieuwe bestanden in bestaande &instantie openen",
    tr="Yeni dosyaları mevcut &örnekte aç", vn="Mở tệp mới trong &phiên bản đang chạy",
)
add(
    "Default &author:",
    cn="默认作者(&A)：", tw="預設作者(&A)：", ja="既定の作成者(&A):", kr="기본 작성자(&A):",
    de="Standard&autor:", fr="Auteur par &défaut :", es="&Autor predeterminado:", it="&Autore predefinito:",
    pt="&Autor predefinido:", br="&Autor padrão:", ru="Автор по &умолчанию:", uk="Автор за &замовчуванням:",
    pl="Domyślny &autor:", nl="Standaard&auteur:", tr="Varsayılan &yazar:", vn="Tác giả &mặc định:",
)
add(
    "Default document &colors:",
    cn="默认文档颜色(&C)：", tw="預設文件顏色(&C)：", ja="文書の既定の色(&C):", kr="기본 문서 색상(&C):",
    de="Standard-&Dokumentfarben:", fr="Couleurs de document par &défaut :",
    es="&Colores de documento predeterminados:", it="&Colori documento predefiniti:",
    pt="&Cores de documento predefinidas:", br="&Cores de documento padrão:",
    ru="Цвета документа по &умолчанию:", uk="Кольори документа за &замовчуванням:",
    pl="Domyślne &kolory dokumentu:", nl="Standaard document&kleuren:",
    tr="Varsayılan belge &renkleri:", vn="Màu tài liệu &mặc định:",
)
add(
    "Use &tabs (requires restart)",
    cn="使用标签页（需重启）(&T)", tw="使用分頁（需重新啟動）(&T)", ja="タブを使用（再起動が必要）(&T)",
    kr="탭 사용(다시 시작 필요)(&T)", de="&Registerkarten verwenden (Neustart erforderlich)",
    fr="Utiliser les &onglets (redémarrage requis)", es="Usar &pestañas (requiere reinicio)",
    it="Usa &schede (richiede riavvio)", pt="Usar &separadores (requer reinício)",
    br="Usar a&bas (requer reinício)", ru="Использовать &вкладки (нужен перезапуск)",
    uk="Використовувати &вкладки (потрібен перезапуск)", pl="Używaj &kart (wymaga restartu)",
    nl="&Tabbladen gebruiken (herstart vereist)", tr="&Sekmeleri kullan (yeniden başlatma gerekir)",
    vn="Dùng &tab (cần khởi động lại)",
)
add(
    "Keep a &Home tab (requires restart)",
    cn="保留主页标签（需重启）(&H)", tw="保留首頁分頁（需重新啟動）(&H)",
    ja="ホームタブを保持（再起動が必要）(&H)", kr="홈 탭 유지(다시 시작 필요)(&H)",
    de="&Startregisterkarte behalten (Neustart erforderlich)",
    fr="Conserver un onglet &Accueil (redémarrage requis)",
    es="Mantener pestaña de &inicio (requiere reinicio)", it="Mantieni scheda &Home (richiede riavvio)",
    pt="Manter separador &Início (requer reinício)", br="Manter aba &Início (requer reinício)",
    ru="Оставлять вкладку &«Домой» (нужен перезапуск)",
    uk="Залишати вкладку &«Домівка» (потрібен перезапуск)",
    pl="Zachowaj kartę &Start (wymaga restartu)", nl="&Starttabblad behouden (herstart vereist)",
    tr="&Ana sayfa sekmesini tut (yeniden başlatma gerekir)", vn="Giữ tab &Trang chủ (cần khởi động lại)",
)
add(
    "Show the &menu bar with tabs",
    cn="与标签一起显示菜单栏(&M)", tw="與分頁一起顯示功能表列(&M)", ja="タブと一緒にメニューバーを表示(&M)",
    kr="탭과 함께 메뉴 표시줄 보이기(&M)", de="&Menüleiste mit Registerkarten anzeigen",
    fr="Afficher la barre de &menu avec les onglets", es="Mostrar la barra de &menú con pestañas",
    it="Mostra la barra dei &menu con le schede", pt="Mostrar a barra de &menus com separadores",
    br="Mostrar a barra de &menus com abas", ru="Показывать строку &меню с вкладками",
    uk="Показувати рядок &меню з вкладками", pl="Pokazuj pasek &menu z kartami",
    nl="&Menubalk met tabbladen tonen", tr="Sekmelerle &menü çubuğunu göster",
    vn="Hiện thanh &menu cùng tab",
)
add(
    "Show the tool&bar",
    cn="显示工具栏(&B)", tw="顯示工具列(&B)", ja="ツールバーを表示(&B)", kr="도구 모음 표시(&B)",
    de="&Symbolleiste anzeigen", fr="Afficher la barre d’ou&tils", es="Mostrar la &barra de herramientas",
    it="Mostra la &barra degli strumenti", pt="Mostrar a &barra de ferramentas",
    br="Mostrar a &barra de ferramentas", ru="Показывать панель &инструментов",
    uk="Показувати панель &інструментів", pl="Pokazuj pasek &narzędzi", nl="&Werkbalk tonen",
    tr="Araç çu&buğunu göster", vn="Hiện thanh công &cụ",
)
add(
    "Ctrl+Tab uses most recently used &order",
    cn="Ctrl+Tab 使用最近使用顺序(&O)", tw="Ctrl+Tab 使用最近使用順序(&O)",
    ja="Ctrl+Tab で最近使用順に切り替え(&O)", kr="Ctrl+Tab을 최근 사용 순서로(&O)",
    de="Ctrl+Tab in Reihenfolge der letzten &Nutzung", fr="Ctrl+Tab selon l’&ordre d’utilisation récente",
    es="Ctrl+Tab usa el &orden de uso reciente", it="Ctrl+Tab usa l’&ordine di uso recente",
    pt="Ctrl+Tab usa a &ordem de uso recente", br="Ctrl+Tab usa a &ordem de uso recente",
    ru="Ctrl+Tab в порядке недавнего &использования", uk="Ctrl+Tab у порядку нещодавнього &використання",
    pl="Ctrl+Tab w kolejności ostatniego &użycia", nl="Ctrl+Tab in meest recent gebruikte &volgorde",
    tr="Ctrl+Tab en son kullanılan &sırayı kullanır", vn="Ctrl+Tab theo thứ tự dùng &gần đây",
)
add(
    "Use the &floating search window",
    cn="使用浮动搜索窗口(&F)", tw="使用浮動搜尋視窗(&F)", ja="フローティング検索ウィンドウを使用(&F)",
    kr="부동 검색 창 사용(&F)",
    de="&Schwebendes Suchfenster verwenden", fr="Utiliser la fenêtre de recherche &flottante",
    es="Usar la ventana de búsqueda &flotante", it="Usa la finestra di ricerca &mobile",
    pt="Usar a janela de pesquisa &flutuante", br="Usar a janela de pesquisa &flutuante",
    ru="Использовать &плавающее окно поиска", uk="Використовувати &плаваюче вікно пошуку",
    pl="Używaj &pływającego okna wyszukiwania", nl="&Zwevend zoekvenster gebruiken",
    tr="&Kayan arama penceresini kullan", vn="Dùng cửa sổ tìm kiếm &nổi",
)
add(
    "Tab fo&nt size (0 = auto, 6-72):",
    cn="标签字体大小（0=自动，6-72）(&N)：", tw="分頁字型大小（0=自動，6-72）(&N)：",
    ja="タブのフォントサイズ（0=自動、6-72）(&N):", kr="탭 글꼴 크기(0=자동, 6-72)(&N):",
    de="Register-&Schriftgröße (0 = auto, 6–72):", fr="Taille de police des o&nglets (0 = auto, 6-72) :",
    es="Tamaño de fue&nte de pestañas (0 = auto, 6-72):", it="Dimensione fo&nt schede (0 = auto, 6-72):",
    pt="Tama&nho da letra dos separadores (0 = auto, 6-72):",
    br="Tama&nho da fonte das abas (0 = auto, 6-72):",
    ru="Размер шрифта вкладок (0 = авто, 6–72) (&N):",
    uk="Розмір шрифту вкладок (0 = авто, 6–72) (&N):",
    pl="Rozmiar czcio&nki kart (0 = auto, 6-72):", nl="Tabbladletter&grootte (0 = auto, 6-72):",
    tr="Sekme ya&zı tipi boyutu (0 = otomatik, 6-72):", vn="Cỡ chữ &tab (0 = tự động, 6-72):",
)
add(
    "Tab bar &height (0 = auto, 16-128):",
    cn="标签栏高度（0=自动，16-128）(&H)：", tw="分頁列高度（0=自動，16-128）(&H)：",
    ja="タブバーの高さ（0=自動、16-128）(&H):", kr="탭 표시줄 높이(0=자동, 16-128)(&H):",
    de="Registerleisten&höhe (0 = auto, 16–128):", fr="&Hauteur de la barre d’onglets (0 = auto, 16-128) :",
    es="&Altura de la barra de pestañas (0 = auto, 16-128):",
    it="&Altezza barra schede (0 = auto, 16-128):",
    pt="&Altura da barra de separadores (0 = auto, 16-128):",
    br="&Altura da barra de abas (0 = auto, 16-128):",
    ru="&Высота панели вкладок (0 = авто, 16–128):",
    uk="&Висота панелі вкладок (0 = авто, 16–128):",
    pl="&Wysokość paska kart (0 = auto, 16-128):", nl="Tabbladbalk&hoogte (0 = auto, 16-128):",
    tr="Sekme çubuğu &yüksekliği (0 = otomatik, 16-128):", vn="C&hiều cao thanh tab (0 = tự động, 16-128):",
)
add(
    "Tree &font:",
    cn="树字体(&F)：", tw="樹狀字型(&F)：", ja="ツリーのフォント(&F):", kr="트리 글꼴(&F):",
    de="Baum&schriftart:", fr="Police de l’ar&borescence :", es="&Fuente del árbol:", it="&Carattere albero:",
    pt="&Tipo de letra da árvore:", br="&Fonte da árvore:", ru="&Шрифт дерева:", uk="&Шрифт дерева:",
    pl="&Czcionka drzewa:", nl="Boom&lettertype:", tr="Ağaç ya&zı tipi:", vn="Phông c&ây:",
)
add(
    "Si&ze (0 = auto, 6-72):",
    cn="大小（0=自动，6-72）(&Z)：", tw="大小（0=自動，6-72）(&Z)：", ja="サイズ（0=自動、6-72）(&Z):",
    kr="크기(0=자동, 6-72)(&Z):", de="Grö&ße (0 = auto, 6–72):", fr="Ta&ille (0 = auto, 6-72) :",
    es="Ta&maño (0 = auto, 6-72):", it="Dimen&sione (0 = auto, 6-72):",
    pt="Ta&manho (0 = auto, 6-72):", br="Ta&manho (0 = auto, 6-72):",
    ru="Ра&змер (0 = авто, 6–72):", uk="Ро&змір (0 = авто, 6–72):",
    pl="Ro&zmiar (0 = auto, 6-72):", nl="Groot&te (0 = auto, 6-72):",
    tr="Bo&yut (0 = otomatik, 6-72):", vn="Kíc&h thước (0 = tự động, 6-72):",
)
add(
    "Use s&mooth scrolling",
    cn="使用平滑滚动(&M)", tw="使用平滑捲動(&M)", ja="スムーズスクロールを使用(&M)",
    kr="부드러운 스크롤 사용(&M)", de="Sa&nftes Scrollen verwenden", fr="Utiliser le défilement flu&ide",
    es="Usar desplazamiento sua&ve", it="Usa scorrimento &fluido", pt="Usar deslocamento sua&ve",
    br="Usar rolagem sua&ve", ru="Плавная про&крутка", uk="Плавне про&кручування",
    pl="Używaj płynnego prze&wijania", nl="Soepele &scroll gebruiken", tr="Y&umuşak kaydırma kullan",
    vn="Dùng cuộn &mượt",
)
add(
    "Show a scrollbar in single-&page mode",
    cn="单页模式显示滚动条(&P)", tw="單頁模式顯示捲軸(&P)", ja="単一ページモードでスクロールバーを表示(&P)",
    kr="한 페이지 모드에서 스크롤 막대 표시(&P)", de="Bildlaufleiste im Einzel&seitenmodus anzeigen",
    fr="Afficher une barre de défilement en mode &page unique",
    es="Mostrar barra de desplazamiento en modo de &página única",
    it="Mostra barra di scorrimento in modalità &pagina singola",
    pt="Mostrar barra de deslocamento no modo de &página única",
    br="Mostrar barra de rolagem no modo de &página única",
    ru="Показывать полосу прокрутки в режиме одной &страницы",
    uk="Показувати смугу прокручування в режимі однієї &сторінки",
    pl="Pokazuj pasek przewijania w trybie jednej &strony",
    nl="Schuifbalk tonen in enkel&paginamodus", tr="Tek &sayfa modunda kaydırma çubuğu göster",
    vn="Hiện thanh cuộn ở chế độ một &trang",
)
add(
    "Fast page scrolling over the scroll&bar",
    cn="在滚动条上快速翻页(&B)", tw="在捲軸上快速翻頁(&B)", ja="スクロールバー上で高速ページ送り(&B)",
    kr="스크롤 막대에서 빠른 페이지 이동(&B)", de="Schnelles Blättern über die Bildlauf&leiste",
    fr="Défilement rapide des pages via la &barre", es="Desplazamiento rápido de páginas sobre la &barra",
    it="Scorrimento rapido pagine sulla &barra", pt="Deslocamento rápido de páginas na &barra",
    br="Rolagem rápida de páginas na &barra", ru="Быстрая прокрутка страниц по полосе про&крутки",
    uk="Швидке гортання сторінок смугою про&кручування", pl="Szybkie przewijanie stron na pa&sku",
    nl="Snel bladeren via de schuif&balk", tr="Kaydırma çu&buğunda hızlı sayfa kaydırma",
    vn="Lật trang nhanh trên thanh &cuộn",
)
add(
    "Engineering drawing &enhancement:",
    cn="工程图增强(&E)：", tw="工程圖增強(&E)：", ja="工学図面の強化(&E):", kr="공학 도면 향상(&E):",
    de="Verbesserung für technische &Zeichnungen:", fr="Amélioration des &dessins techniques :",
    es="Mejora de &dibujos de ingeniería:", it="Miglioramento &disegni tecnici:",
    pt="Melhoria de &desenhos de engenharia:", br="Melhoria de &desenhos de engenharia:",
    ru="Улучшение технических &чертежей:", uk="Покращення інженерних &креслень:",
    pl="Ulepszanie rysunków &technicznych:", nl="Verbetering technische &tekeningen:",
    tr="Mühendislik çizimi &geliştirme:", vn="Tăng cường bản vẽ kỹ &thuật:",
)
add(
    "Enable PDF &anti-aliasing",
    cn="启用 PDF 抗锯齿(&A)", tw="啟用 PDF 反鋸齒(&A)", ja="PDF のアンチエイリアスを有効にする(&A)",
    kr="PDF 안티앨리어싱 사용(&A)", de="PDF-&Antialiasing aktivieren", fr="Activer l’&anticrénelage PDF",
    es="Activar &antialiasing de PDF", it="Abilita &antialiasing PDF", pt="Ativar &anti-aliasing de PDF",
    br="Ativar &anti-aliasing de PDF", ru="Включить сглаживание &PDF", uk="Увімкнути згладжування &PDF",
    pl="Włącz wygładzanie &PDF", nl="PDF-&antialiasing inschakelen", tr="PDF &kenar yumuşatmayı aç",
    vn="Bật &khử răng cưa PDF",
)
add(
    "Look up words on &double-click",
    cn="双击查词(&D)", tw="按兩下查詞(&D)", ja="ダブルクリックで単語を調べる(&D)",
    kr="두 번 클릭으로 단어 찾기(&D)", de="Wörter per &Doppelklick nachschlagen",
    fr="Rechercher les mots par &double-clic", es="Buscar palabras con &doble clic",
    it="Cerca parole con &doppio clic", pt="Procurar palavras com &duplo clique",
    br="Pesquisar palavras com &clique duplo", ru="Искать слова по &двойному щелчку",
    uk="Шукати слова &подвійним клацанням", pl="Szukaj słów &podwójnym kliknięciem",
    nl="Woorden opzoeken met &dubbelklik", tr="&Çift tıklamayla sözcük ara", vn="Tra từ bằng &nhấp đôi",
)
add(
    "Prevent sleep in &fullscreen or presentation mode",
    cn="全屏或演示模式下防止休眠(&F)", tw="全螢幕或簡報模式下防止休眠(&F)",
    ja="全画面またはプレゼンテーション中はスリープしない(&F)",
    kr="전체 화면 또는 프레젠테이션에서 절전 방지(&F)",
    de="Ruhezustand im Vollbild- oder Präsentationsmodus &verhindern",
    fr="Empêcher la mise en veille en &plein écran ou présentation",
    es="Evitar suspensión en &pantalla completa o presentación",
    it="Impedisci sospensione in schermo &intero o presentazione",
    pt="Impedir suspensão em ecrã &inteiro ou apresentação",
    br="Impedir suspensão em tela &cheia ou apresentação",
    ru="Не уходить в сон в полноэкранном режиме или &презентации",
    uk="Не засинати в повноекранному режимі або &презентації",
    pl="Zapobiegaj uśpieniu w trybie &pełnoekranowym lub prezentacji",
    nl="Slaapstand voorkomen in &volledig scherm of presentatie",
    tr="Tam ekran veya sunumda uykuya geçmeyi &önle", vn="Chặn ngủ khi &toàn màn hình hoặc trình chiếu",
)
add(
    "Highlight co&lor:",
    cn="高亮颜色(&L)：", tw="螢光顏色(&L)：", ja="ハイライトの色(&L):", kr="형광 색상(&L):",
    de="Hervorhebungs&farbe:", fr="Cou&leur de surlignage :", es="Co&lor de resaltado:",
    it="Co&lore evidenziazione:", pt="Cor de rea&lce:", br="Cor de rea&lce:",
    ru="Цвет выделе&ния:", uk="Колір підсвічуван&ня:", pl="Ko&lor podświetlenia:",
    nl="Markeerk&leur:", tr="Vurgu ren&gi:", vn="Màu tô sá&ng:",
)
add(
    "&Reset",
    cn="重置(&R)", tw="重設(&R)", ja="リセット(&R)", kr="재설정(&R)", de="&Zurücksetzen", fr="&Réinitialiser",
    es="&Restablecer", it="&Reimposta", pt="&Repor", br="&Redefinir", ru="&Сброс", uk="&Скинути",
    pl="&Resetuj", nl="&Herstellen", tr="&Sıfırla", vn="&Đặt lại",
)
add(
    "Use the book's highlight c&olor",
    cn="使用书籍的高亮颜色(&O)", tw="使用書籍的螢光顏色(&O)", ja="書籍のハイライト色を使用(&O)",
    kr="책의 형광 색상 사용(&O)", de="Hervorhebungsfarbe des Buches &verwenden",
    fr="Utiliser la c&ouleur de surlignage du livre", es="Usar el c&olor de resaltado del libro",
    it="Usa il c&olore evidenziazione del libro", pt="Usar a c&or de realce do livro",
    br="Usar a c&or de realce do livro", ru="Использовать цвет выделения &книги",
    uk="Використовувати колір підсвічування &книги", pl="Używaj kol&oru podświetlenia książki",
    nl="Markeerkleur van het b&oek gebruiken", tr="Kitabın vurgu rengin&i kullan",
    vn="Dùng màu tô sáng của sác&h",
)
add(
    "Extraction &detail:",
    cn="提取详细程度(&D)：", tw="擷取詳細程度(&D)：", ja="抽出の詳細度(&D):", kr="추출 상세도(&D):",
    de="Extraktions&detail:", fr="Niveau de &détail d’extraction :", es="&Detalle de extracción:",
    it="&Dettaglio estrazione:", pt="&Detalhe de extração:", br="&Detalhe de extração:",
    ru="&Детализация извлечения:", uk="&Деталізація витягування:", pl="&Szczegółowość ekstrakcji:",
    nl="Extractie&detail:", tr="Çıkarma &ayrıntısı:", vn="Mức chi &tiết trích xuất:",
)
add(
    "Target &language:",
    cn="目标语言(&L)：", tw="目標語言(&L)：", ja="翻訳先の言語(&L):", kr="대상 언어(&L):",
    de="Ziel&sprache:", fr="&Langue cible :", es="&Idioma de destino:", it="&Lingua di destinazione:",
    pt="&Idioma de destino:", br="&Idioma de destino:", ru="Целевой &язык:", uk="Цільова &мова:",
    pl="&Język docelowy:", nl="Doel&taal:", tr="Hedef &dil:", vn="&Ngôn ngữ đích:",
)
add(
    "Volc &Access Key:",
    cn="Volc Access Key(&A)：", tw="Volc Access Key(&A)：", ja="Volc Access Key(&A):",
    kr="Volc Access Key(&A):", de="Volc-&Access-Key:", fr="Clé d’&accès Volc :",
    es="Clave de &acceso Volc:", it="Chiave di &accesso Volc:", pt="Chave de &acesso Volc:",
    br="Chave de &acesso Volc:", ru="Volc Access &Key:", uk="Volc Access &Key:",
    pl="Klucz &dostępu Volc:", nl="Volc-&toegangssleutel:", tr="Volc &Erişim Anahtarı:",
    vn="Volc Access &Key:",
)
add(
    "Volc &Secret Key:",
    cn="Volc Secret Key(&S)：", tw="Volc Secret Key(&S)：", ja="Volc Secret Key(&S):",
    kr="Volc Secret Key(&S):", de="Volc-&Secret-Key:", fr="Clé &secrète Volc :",
    es="Clave &secreta Volc:", it="Chiave &segreta Volc:", pt="Chave &secreta Volc:",
    br="Chave &secreta Volc:", ru="Volc Secret &Key:", uk="Volc Secret &Key:",
    pl="Klucz &tajny Volc:", nl="Volc-&geheime sleutel:", tr="Volc &Gizli Anahtar:",
    vn="Volc Secret &Key:",
)
add(
    "&Platform:",
    cn="平台(&P)：", tw="平台(&P)：", ja="プラットフォーム(&P):", kr="플랫폼(&P):", de="&Plattform:",
    fr="&Plateforme :", es="&Plataforma:", it="&Piattaforma:", pt="&Plataforma:", br="&Plataforma:",
    ru="&Платформа:", uk="&Платформа:", pl="&Platforma:", nl="&Platform:", tr="&Platform:",
    vn="&Nền tảng:",
)
add(
    "&Add",
    cn="添加(&A)", tw="新增(&A)", ja="追加(&A)", kr="추가(&A)", de="&Hinzufügen", fr="&Ajouter",
    es="&Añadir", it="&Aggiungi", pt="&Adicionar", br="&Adicionar", ru="&Добавить", uk="&Додати",
    pl="&Dodaj", nl="&Toevoegen", tr="&Ekle", vn="T&hêm",
)
add(
    "&Remove",
    cn="移除(&R)", tw="移除(&R)", ja="削除(&R)", kr="제거(&R)", de="&Entfernen", fr="&Supprimer",
    es="&Quitar", it="&Rimuovi", pt="&Remover", br="&Remover", ru="&Удалить", uk="&Видалити",
    pl="&Usuń", nl="&Verwijderen", tr="&Kaldır", vn="Xóa (&R)",
)
add(
    "&Name:",
    cn="名称(&N)：", tw="名稱(&N)：", ja="名前(&N):", kr="이름(&N):", de="&Name:", fr="&Nom :",
    es="&Nombre:", it="&Nome:", pt="&Nome:", br="&Nome:", ru="&Имя:", uk="&Ім’я:", pl="&Nazwa:",
    nl="&Naam:", tr="&Ad:", vn="Tê&n:",
)
add(
    "API &base URL:",
    cn="API 基址(&B)：", tw="API 基底 URL(&B)：", ja="API ベース URL(&B):", kr="API 기본 URL(&B):",
    de="API-&Basis-URL:", fr="URL de &base de l’API :", es="URL &base de la API:",
    it="URL &base API:", pt="URL &base da API:", br="URL &base da API:", ru="Базовый URL &API:",
    uk="Базовий URL &API:", pl="Bazowy URL &API:", nl="API-&basis-URL:", tr="API &taban URL’si:",
    vn="URL &gốc API:",
)
add(
    "API &key:",
    cn="API 密钥(&K)：", tw="API 金鑰(&K)：", ja="API キー(&K):", kr="API 키(&K):", de="API-&Schlüssel:",
    fr="Clé &API :", es="Clave de &API:", it="Chiave &API:", pt="Chave &API:", br="Chave &API:",
    ru="Ключ &API:", uk="Ключ &API:", pl="Klucz &API:", nl="API-&sleutel:", tr="API &anahtarı:",
    vn="Khóa &API:",
)
add(
    "&Model:",
    cn="模型(&M)：", tw="模型(&M)：", ja="モデル(&M):", kr="모델(&M):", de="&Modell:", fr="&Modèle :",
    es="&Modelo:", it="&Modello:", pt="&Modelo:", br="&Modelo:", ru="&Модель:", uk="&Модель:",
    pl="&Model:", nl="&Model:", tr="&Model:", vn="&Mô hình:",
)
add(
    "&Choose...",
    cn="选择(&C)…", tw="選擇(&C)…", ja="選択(&C)...", kr="선택(&C)...", de="&Auswählen...",
    fr="&Choisir...", es="&Elegir...", it="&Scegli...", pt="&Escolher...", br="&Escolher...",
    ru="&Выбрать...", uk="&Вибрати...", pl="&Wybierz...", nl="&Kiezen...", tr="&Seç...", vn="&Chọn...",
)
add(
    "&Parallel (1-8):",
    cn="并行数（1-8）(&P)：", tw="平行數（1-8）(&P)：", ja="並列数（1-8）(&P):", kr="병렬(1-8)(&P):",
    de="&Parallel (1–8):", fr="&Parallèle (1-8) :", es="&Paralelo (1-8):", it="&Parallelo (1-8):",
    pt="&Paralelo (1-8):", br="&Paralelo (1-8):", ru="&Параллельность (1–8):",
    uk="&Паралельність (1–8):", pl="&Równolegle (1-8):", nl="&Parallel (1-8):",
    tr="&Paralel (1-8):", vn="&Song song (1-8):",
)
add(
    "&Test",
    cn="测试(&T)", tw="測試(&T)", ja="テスト(&T)", kr="테스트(&T)", de="&Testen", fr="&Tester",
    es="&Probar", it="&Prova", pt="&Testar", br="&Testar", ru="&Проверить", uk="&Перевірити",
    pl="&Testuj", nl="&Testen", tr="&Test", vn="&Thử",
)
add(
    "Window",
    cn="窗口", tw="視窗", ja="ウィンドウ", kr="창", de="Fenster", fr="Fenêtre", es="Ventana", it="Finestra",
    pt="Janela", br="Janela", ru="Окно", uk="Вікно", pl="Okno", nl="Venster", tr="Pencere", vn="Cửa sổ",
)
add(
    "Display",
    cn="显示", tw="顯示", ja="表示", kr="표시", de="Anzeige", fr="Affichage", es="Pantalla", it="Schermo",
    pt="Ecrã", br="Exibição", ru="Экран", uk="Екран", pl="Wyświetlanie", nl="Weergave", tr="Görüntü",
    vn="Hiển thị",
)
add(
    "PDF",
    cn="PDF", tw="PDF", ja="PDF", kr="PDF", de="PDF", fr="PDF", es="PDF", it="PDF", pt="PDF", br="PDF",
    ru="PDF", uk="PDF", pl="PDF", nl="PDF", tr="PDF", vn="PDF",
)
add(
    "E&xit the application with Esc",
    cn="按 Esc 退出程序(&X)", tw="按 Esc 結束程式(&X)", ja="Esc でアプリケーションを終了(&X)",
    kr="Esc로 프로그램 종료(&X)", de="Anwendung mit Esc &beenden", fr="Quitter l’application avec &Échap",
    es="Salir de la aplicación con &Esc", it="Esci dall’applicazione con &Esc",
    pt="Sair da aplicação com &Esc", br="Sair do aplicativo com &Esc",
    ru="Выходить из программы по &Esc", uk="Виходити з програми за &Esc",
    pl="Zakończ aplikację klawiszem &Esc", nl="Applicatie afsluiten met &Esc",
    tr="Esc ile uygulamadan çı&k", vn="Thoát ứng dụng bằng &Esc",
)
add(
    "Show the full file &path in the title bar",
    cn="在标题栏显示完整文件路径(&P)", tw="在標題列顯示完整檔案路徑(&P)",
    ja="タイトルバーに完全なファイルパスを表示(&P)", kr="제목 표시줄에 전체 파일 경로 표시(&P)",
    de="Vollständigen Datei&pfad in der Titelleiste anzeigen",
    fr="Afficher le &chemin complet dans la barre de titre",
    es="Mostrar la &ruta completa en la barra de título",
    it="Mostra il &percorso completo nella barra del titolo",
    pt="Mostrar o &caminho completo na barra de título",
    br="Mostrar o &caminho completo na barra de título",
    ru="Показывать полный &путь в заголовке", uk="Показувати повний &шлях у заголовку",
    pl="Pokazuj pełną &ścieżkę na pasku tytułu", nl="Volledig bestands&pad in titelbalk tonen",
    tr="Başlık çubuğunda tam dosya &yolunu göster", vn="Hiện đường dẫn đầy đủ trên thanh tiêu đề(&P)",
)
add(
    "Custom &DPI (0 = automatic):",
    cn="自定义 DPI（0=自动）(&D)：", tw="自訂 DPI（0=自動）(&D)：", ja="カスタム DPI（0=自動）(&D):",
    kr="사용자 DPI(0=자동)(&D):", de="Benutzerdefiniertes &DPI (0 = automatisch):",
    fr="&DPI personnalisé (0 = automatique) :", es="&DPI personalizado (0 = automático):",
    it="&DPI personalizzato (0 = automatico):", pt="&DPI personalizado (0 = automático):",
    br="&DPI personalizado (0 = automático):", ru="Свой &DPI (0 = автоматически):",
    uk="Власний &DPI (0 = автоматично):", pl="Niestandardowe &DPI (0 = automatycznie):",
    nl="Aangepaste &DPI (0 = automatisch):", tr="Özel &DPI (0 = otomatik):",
    vn="DPI tùy chỉnh (0 = tự động)(&D):",
)
add(
    "Show &link borders",
    cn="显示链接边框(&L)", tw="顯示連結邊框(&L)", ja="リンクの枠を表示(&L)", kr="링크 테두리 표시(&L)",
    de="&Linkrahmen anzeigen", fr="Afficher les bordures de &liens", es="Mostrar bordes de &enlaces",
    it="Mostra bordi dei &collegamenti", pt="Mostrar contornos de &ligações",
    br="Mostrar bordas de &links", ru="Показывать рамки &ссылок", uk="Показувати рамки &посилань",
    pl="Pokazuj obramowania &łączy", nl="&Koppelingsranden tonen", tr="&Bağlantı kenarlıklarını göster",
    vn="Hiện viền &liên kết",
)
add(
    "Open &Advanced Options File...",
    cn="打开高级选项文件(&A)…", tw="開啟進階選項檔案(&A)…", ja="上級オプションファイルを開く(&A)...",
    kr="고급 옵션 파일 열기(&A)...", de="&Erweiterte Optionsdatei öffnen...",
    fr="Ouvrir le fichier d’options &avancées...", es="Abrir archivo de opciones &avanzadas...",
    it="Apri file opzioni &avanzate...", pt="Abrir ficheiro de opções &avançadas...",
    br="Abrir arquivo de opções &avançadas...", ru="Открыть файл &дополнительных настроек...",
    uk="Відкрити файл &додаткових параметрів...", pl="Otwórz plik opcji &zaawansowanych...",
    nl="Bestand met &geavanceerde opties openen...", tr="&Gelişmiş seçenekler dosyasını aç...",
    vn="Mở tệp tùy chọn nâng &cao...",
)
add(
    "Automatically &reload changed documents",
    cn="自动重新加载已更改的文档(&R)", tw="自動重新載入已變更的文件(&R)",
    ja="変更された文書を自動的に再読み込み(&R)", kr="변경된 문서 자동 다시 로드(&R)",
    de="Geänderte Dokumente automatisch neu &laden", fr="Recharger automatiquement les documents &modifiés",
    es="Recargar automáticamente documentos &modificados", it="Ricarica automaticamente i documenti &modificati",
    pt="Recarregar automaticamente documentos &alterados", br="Recarregar automaticamente documentos &alterados",
    ru="Автоматически перезагружать изменённые &документы",
    uk="Автоматично перезавантажувати змінені &документи",
    pl="Automatycznie przeładowuj zmienione &dokumenty", nl="Gewijzigde documenten automatisch he&rladen",
    tr="Değişen belgeleri otomatik yeniden &yükle", vn="Tự động tải lại tài liệu đã &đổi",
)

def parse_blocks(text: str):
    lines = text.splitlines()
    header, blocks, i = [], {}, 0
    while i < len(lines) and not lines[i].startswith(":"):
        header.append(lines[i])
        i += 1
    key, cur = None, {}
    for line in lines[i:]:
        if line.startswith(":"):
            if key is not None:
                blocks[key] = cur
            key, cur = line[1:], {}
        elif key is not None and ":" in line:
            lang, val = line.split(":", 1)
            cur[lang] = val
    if key is not None:
        blocks[key] = cur
    return header, blocks


def serialize(header, blocks):
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
    raise SystemExit(subprocess.run(["bun", str(ROOT / "cmd" / "sync-major-langs-to-good.ts")], cwd=ROOT).returncode)
