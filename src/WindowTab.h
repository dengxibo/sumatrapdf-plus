/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct SelectionOnPage;
struct WatchedFile;
struct EditAnnotationsWindow;
struct EbookAnnotationsWindow;
struct EbookAnnotations;
struct EbookAnnotation;
struct MainWindow;
struct StrBuilder;
struct ReadAloudHighlightMap;

/* Data related to a single document loaded into a tab/window */
/* (none of these depend on MainWindow, so that a WindowTab could
   be moved between windows once this is supported) */
struct WindowTab {
    enum class Type {
        None,
        About,
        Document,
    };
    Type type = Type::None;
    const char* filePath = nullptr;
    MainWindow* win = nullptr;
    DocController* ctrl = nullptr;
    // text of win->hwndFrame when the tab is selected
    const char* frameTitle = nullptr;
    // state of the table of contents
    bool showToc = false;
    bool showTocPresentation = false;
    // an array of ids for ToC items that have been expanded/collapsed by user
    Vec<int> tocState;
    // canvas dimensions when the document was last visible
    Rect canvasRc;
    // whether to auto-reload the document when the tab is selected
    bool reloadOnFocus = false;
    // ebook font change on a background tab: full reload deferred until focus
    bool reloadForEbookFontChange = false;
    // distinguishes a metrics-only size/layout change from a font-family change
    bool reloadForEbookFontSizeChange = false;
    int restorePageAfterFontReload = 0;
    double restoreScrollXAfterFontReload = -1;
    double restoreScrollYAfterFontReload = -1;
    float restoreInPageScrollRatioAfterFontReload = -1.f;
    u32 lastDarkModeEpoch = 0;
    // FileWatcher token for unsubscribing
    WatchedFile* watcher = nullptr;
    // list of rectangles of the last rectangular, text or image selection
    // (split by page, in user coordinates)
    Vec<SelectionOnPage>* selectionOnPage = nullptr;
    // previous View settings, needed when unchecking the Fit Width/Page toolbar buttons
    float prevZoomVirtual{kInvalidZoom};
    DisplayMode prevDisplayMode{DisplayMode::Automatic};
    TocTree* currToc = nullptr; // not owned by us
    EditAnnotationsWindow* editAnnotsWindow = nullptr;
    EbookAnnotationsWindow* editEbookAnnotsWindow = nullptr;
    Rect lastEditAnnotsWindowPos = {};
    // DPI used when lastEditAnnotsWindowPos client size was saved; 0 = legacy/unknown.
    int lastEditAnnotsWindowDpi = 0;
    // Main window frame width when lastEditAnnotsWindowPos client size was saved; 0 = legacy/unknown.
    int lastEditAnnotsWindowMainWidth = 0;
    // Recreate annotation windows after a cross-monitor DPI move (close during drag).
    bool reopenEditAnnotsAfterDpiMove = false;
    bool reopenEbookAnnotsAfterDpiMove = false;

    // TODO: terrible hack
    bool askedToSaveAnnotations = false;
    bool acceptedPdfTocSignatureWarning = false;

    TabState* tabState = nullptr; // when lazy loading

    Annotation* selectedAnnotation = nullptr;
    EbookAnnotation* selectedEbookAnnotation = nullptr;
    bool didScrollToSelectedAnnotation = false; // only automatically scroll once

    bool hideAnnotations = false;
    EbookAnnotations* ebookAnnotations = nullptr;

    // PDF text markup drawn as overlay until page tiles catch up in the background.
    struct PdfMarkupOverlayAnnot {
        int pageNo = 0;
        Annotation* annot = nullptr;
    };
    Vec<PdfMarkupOverlayAnnot> pdfMarkupOverlays;

    HWND hwndPDFInfo = nullptr;
    HWND hwndPDFOutline = nullptr;

    // per-document background color from FileState; kColorUnset = use default
    COLORREF bgColor = kColorUnset;
    // true if per-document background is explicitly set to checkered pattern
    bool bgColorCheckered = false;
    // per-document tab color from FileState; kColorUnset = use default
    COLORREF tabColor = kColorUnset;

    // TODO: arguably a hack
    bool ignoreNextAutoReload = false;

    // read aloud: cleaned text that was being read and the utf8 offset
    // within it where the user stopped reading; enables "Continue reading"
    char* readAloudText = nullptr;
    int readAloudResumePos = -1;
    ReadAloudHighlightMap* readAloudHighlight = nullptr;
    int readAloudHighlightBase = 0;
    int readAloudChunkStart = 0;
    int readAloudChunkEnd = 0;
    int readAloudBuiltEndPage = 0;
    int readAloudStartPage = 0;
    int readAloudStartGlyph = 0;
    bool readAloudAutoScroll = false;
    bool readAloudAutoScrollHold = false;
    int readAloudAutoScrollHoldPageNo = -1;
    float readAloudAutoScrollHoldLineY = -1.f;

    enum ReadAloudScope {
        ReadAloudScopeSmart = 1,
        ReadAloudScopeViewport = 2,
        ReadAloudScopeSelection = 3,
        ReadAloudScopeCursor = 4,
    };
    int readAloudScope = 0;

    WindowTab(MainWindow* win);
    ~WindowTab();

    bool IsAboutTab() const;

    DisplayModel* AsFixed() const;

    void SetFilePath(const char* path);

    // only if AsFixed()
    EngineBase* GetEngine() const;
    Kind GetEngineType() const;

    ChmModel* AsChm() const;

    const char* GetTabTitle() const;
    bool IsDocLoaded() const;
    void MoveDocBy(int dx, int dy) const;
    void ToggleZoom() const;
};

bool SaveDataToFile(HWND hwndParent, char* fileName, ByteSlice data);
