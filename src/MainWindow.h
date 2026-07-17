/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct DoubleBuffer;
struct Edit;
struct LinkHandler;
struct StressTest;
class SumatraUIAutomationProvider;
struct FrameRateWnd;
struct LabelWithCloseWnd;
struct Splitter;
struct Tooltip;
struct TreeView;
struct TabsCtrl;
struct TocTree;
struct SelectionToolbar;
struct FindBarWnd;
struct FindWindowWnd;
struct EbookAnnotation;
struct FindWindowWnd;

// one search match with a text snippet around it, for the floating results list
struct FindMatch {
    int startPage = 0;
    int startGlyph = 0;
    int endPage = 0;
    int endGlyph = 0;
    char* snippet = nullptr; // UTF-8, owned (freed when findMatches is rebuilt)
};

// factor by how large the non-maximized caption should be in relation to the tabbar
#define kCaptionTabBarDyFactor 1.0f

// gap in pixels between top of caption and tabs; this area allows dragging the window
#define kCaptionTopPadding 8

enum CaptionButtons {
    CB_BTN_FIRST = 0,
    CB_MINIMIZE = CB_BTN_FIRST,
    CB_MAXIMIZE,
    CB_RESTORE,
    CB_CLOSE,
    CB_MENU,
    CB_SYSTEM_MENU,
    CB_BTN_COUNT
};

struct ButtonInfo {
    int id = -1; // CaptionButtons value
    Rect rect{};
    bool highlighted = false;
    bool pressed = false;
    bool inactive = false;
    bool visible = true;
    ButtonInfo() = default;
};

struct IPageElement;
struct PageDestination;
struct TocItem;
struct DocController;
struct DocControllerCallback;
struct ChmModel;
struct DisplayModel;
struct WindowTab;

struct Annotation;
struct ILinkHandler;

// Current action being performed with a mouse
enum class MouseAction {
    None = 0,
    Dragging,
    Selecting,
    Scrolling,
    SelectingText
};

enum PresentationMode {
    PM_DISABLED = 0,
    PM_ENABLED,
    PM_BLACK_SCREEN,
    PM_WHITE_SCREEN
};

// WM_GESTURE handling
struct TouchState {
    bool panStarted = false;
    POINTS panPos{};
    int panScrollOrigX = 0;
    float zoomIntermediate = 0;
};

/* Describes position, the target (URL or file path) and infotip of a "hyperlink" */
struct StaticLink {
    Rect rect;
    char* target = nullptr;
    char* tooltip = nullptr;

    explicit StaticLink(Rect rect, const char* target, const char* infotip = nullptr);
    StaticLink() = default;
    ~StaticLink();
};

/* Describes information related to one window with (optional) a document
   on the screen */
struct MainWindow {
    explicit MainWindow(HWND hwnd);
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;
    ~MainWindow();

    // TODO: error windows currently have
    //       !IsAboutWindow() && !IsDocLoaded()
    //       which doesn't allow distinction between PDF, XPS, etc. errors
    bool IsCurrentTabAbout() const;
    bool IsDocLoaded() const;
    bool HasDocsLoaded() const;

    DisplayModel* AsFixed() const;
    ChmModel* AsChm() const;

    // TODO: use CurrentTab()->ctrl instead
    DocController* ctrl = nullptr; // owned by CurrentTab()

    WindowTab* currentTabTemp = nullptr; // points into tabs
    WindowTab* CurrentTab() const;
    int TabCount() const;
    Vec<WindowTab*> Tabs() const;
    WindowTab* GetTab(int idx) const;
    int GetTabIdx(WindowTab*) const;

    HWND hwndFrame = nullptr;
    HWND hwndCanvas = nullptr;

    HWND hwndReBar = nullptr;
    HWND hwndToolbar = nullptr;
    HWND hwndMenuReBar = nullptr;
    HWND hwndMenuToolbar = nullptr;
    HWND hwndFindEdit = nullptr;
    FindBarWnd* findBar = nullptr;
    FindWindowWnd* findWindow = nullptr;
    // Kept for compatibility with toolbar theming code; the floating bar owns
    // the actual edit control.
    HWND hwndFindLabel = nullptr;
    HWND hwndFindBg = nullptr;
    HWND hwndPageLabel = nullptr;
    HWND hwndPageEdit = nullptr;
    HWND hwndPageBg = nullptr;
    HWND hwndPageTotal = nullptr;

    // state related to table of contents (PDF bookmarks etc.)
    HWND hwndTocBox = nullptr;
    UINT_PTR tocBoxSubclassId = 0;
    UINT_PTR tocTreeSubclassId = 0;

    LabelWithCloseWnd* tocLabelWithClose = nullptr;
    Edit* tocFilterEdit = nullptr;
    TreeView* tocTreeView = nullptr;
    TocTree* tocFilteredTree = nullptr;

    // whether the current tab's ToC has been loaded into the tree
    bool tocLoaded = false;
    // whether the ToC sidebar is currently visible
    bool tocVisible = false;
    // set to temporarily disable UpdateTocSelection
    bool tocKeepSelection = false;

    // state related to favorites
    HWND hwndFavBox = nullptr;
    LabelWithCloseWnd* favLabelWithClose = nullptr;
    TreeView* favTreeView = nullptr;
    Vec<FileState*> expandedFavorites;

    // vertical splitter for resizing left side panel
    Splitter* sidebarSplitter = nullptr;

    // horizontal splitter for resizing favorites and bookmars parts
    Splitter* favSplitter = nullptr;

    TabsCtrl* tabsCtrl = nullptr;
    bool tabsVisible = false;
    bool tabsInTitlebar = false;
    // keeps the sequence of tab selection. This is needed for restoration
    // of the previous tab when the current one is closed. (Points into tabs.)
    Vec<WindowTab*>* tabSelectionHistory = nullptr;

    ButtonInfo captionBtn[CB_BTN_COUNT];
    bool isMenuOpen = false;
    Rect captionRect{};

    Tooltip* infotip = nullptr;

    HMENU menu = nullptr;

    DoubleBuffer* buffer = nullptr;

    MouseAction mouseAction = MouseAction::None;
    bool dragRightClick = false; // if true, drag was initiated with right mouse click
    bool dragStartPending = false;
    bool textDragPending = false;  // true when mouse down on selected text, waiting for drag
    bool imageDragPending = false; // true when mouse down on image, waiting for drag
    IPageElement* imageDragElement = nullptr;

    /* when dragging the document around, this is previous position of the
       cursor. A delta between previous and current is by how much we
       moved */
    Point dragPrevPos;
    /* when dragging, mouse x/y position when dragging was started */
    Point dragStart;

    Size annotationBeingMovedSize;
    Point annotationBeingMovedOffset;
    HBITMAP bmpMovePattern = nullptr;
    HBRUSH brMovePattern = nullptr;
    Annotation* annotationBeingDragged = nullptr;
    EbookAnnotation* ebookAnnotationBeingDragged = nullptr;
    EbookAnnotation* ebookAnnotationDragPending = nullptr;

    // Vars for resizing annotations
    int resizeHandle = 0; // ResizeHandle enum casted to int
    bool annotationBeingResized = false;
    RectF annotationOriginalRect;

    /* when moving the document by smooth scrolling, this keeps track of
       the speed at which we should scroll, which depends on the distance
       of the mouse from the point where the user middle clicked. */
    int xScrollSpeed = 0;
    int yScrollSpeed = 0;

    // true while selecting and when CurrentTab()->selectionOnPage != nullptr
    bool showSelection = false;
    // selection rectangle in screen coordinates (only needed while selecting)
    Rect selectionRect;
    // size of the current rectangular selection in document units
    SizeF selectionMeasure;

    // a list of static links (mainly used for About and Frequently Read pages)
    Vec<StaticLink*> staticLinks;

    // home page thumbnail scrolling
    int homePageScrollY = 0;
    int homePageScrollTargetY = 0;
    int homePageMaxScrollY = 0;
    int homePageThumbsVisibleDy = 0;
    Rect homePageThumbsArea;
    int homePageThumbsStartX = 0;
    int homePageThumbsTopY = 0;
    int homePageThumbsCols = 0;
    int homePageRowDy = 0;
    bool homePageListView = false;
    int homePagePaintScrollY = 0;
    bool homePageBlitScrollReady = false;
    Vec<FileState*> homePageFileStates;
    StrVec homePageFilterWords;
    Vec<u8> homePageHighlighted;
    UINT_PTR homePageScrollTimer = 0;

    // home page search filter
    HWND hwndHomeSearch = nullptr;

    bool isToolbarVisible = false;
    bool isFullScreen = false;
    PresentationMode presentation = PM_DISABLED;
    int windowStateBeforePresentation = 0;
    bool suppressFrameRedraw = false;

    long nonFullScreenWindowStyle = 0;
    Rect nonFullScreenFrameRect;

    Rect canvasRc; // size of the canvas (excluding any scroll bars)

    // state snapshot used to skip redundant RelayoutFrame calls
    struct LayoutState {
        Rect rc;
        int presentation = 0;
        bool tabsInTitlebar = false;
        bool isFullScreen = false;
        bool tabsVisible = false;
        bool isToolbarVisible = false;
        bool tocVisible = false;
        bool showFavorites = false;
        bool showMenuBarRebar = false;
    };
    LayoutState lastLayoutState;

    // last known DPI of hwndFrame; used to refresh UI after display topology changes
    int frameDpi = 0;
    int tocSidebarDpi = 0;
    int favSidebarDpi = 0;
    bool tocDpiRecreatePending = false;
    bool favDpiRecreatePending = false;
    bool deferDpiChromeRefresh = false;
    bool dpiChromeRefreshPending = false;

    int currPageNo = 0; // cached value, needed to determine when to auto-update the ToC selection

    // overlay scrollbars (used when scrollbars mode is "smart" or "overlay")
    struct OverlayScrollbar* overlayScrollV = nullptr;
    struct OverlayScrollbar* overlayScrollH = nullptr;

    int wheelAccumDelta = 0;
    UINT_PTR delayedRepaintTimer = 0;

    HANDLE printThread = nullptr;
    bool printCanceled = false;

    HANDLE findThread = nullptr;
    bool findCancelled = false;
    bool findMatchCase = false;
    bool findMatchWholeWord = false;
    // find-as-you-type is debounced: a WM_TIMER on hwndFrame fires the actual
    // search a short while after the last keystroke (see SearchAndDDE.cpp).
    // true while that timer is armed and hasn't fired yet.
    bool findDebouncePending = false;

    // find bar "n / m" match counter (see SearchAndDDE.cpp). The positions of all
    // matches for findCountText are cached so prev/next is instant; a background
    // thread (re)builds the cache when the search term or match-case changes.
    HANDLE findCountThread = nullptr;
    LONG findCountEpoch = 0;
    WCHAR* findCountText = nullptr;
    bool findCountMatchCase = false;
    bool findCountMatchWholeWord = false;
    bool findCountValid = false;
    // the scan stopped at kMaxFindCount matches; the real total is higher
    // (shown as "n / m+")
    bool findCountCapped = false;
    void* findCountEngine = nullptr; // engine the cache was built for (compared, never deref'd)
    // (page<<32 | startOffset) of each match, in scan order (the scan starts
    // at the page current at the time and wraps around)
    Vec<u64> findCountPositions;
    // a newer count request that arrived while a scan was running; the running
    // worker picks it up when it finishes (coalesces rapid typing to one scan)
    WCHAR* findCountPendingText = nullptr;
    bool findCountPendingMatchCase = false;
    bool findCountPendingMatchWholeWord = false;
    // per-match positions (and optional snippets for the floating results list);
    // also built when gShowAllMatches paints all highlights (see SearchAndDDE.cpp)
    Vec<FindMatch> findMatches;
    bool findCountHasSnippets = false;

    ILinkHandler* linkHandler = nullptr;
    IPageElement* linkOnLastButtonDown = nullptr;
    AutoFreeStr urlOnLastButtonDown;
    Annotation* annotationUnderCursor = nullptr;
    // highlight rectangle for element under cursor during context menu (in page coordinates)
    RectF contextMenuHighlightRect{};
    int contextMenuHighlightPageNo = 0;
    Point contextMenuPt{};
    bool contextMenuPtValid = false;
    // last canvas position where the mouse was over document text (for read-aloud menu)
    Point readAloudLastTextPt{};
    bool readAloudLastTextPtValid = false;
    // last left-click on document text (preferred start for menubar/toolbar read-aloud)
    Point readAloudLastClickTextPt{};
    bool readAloudLastClickTextPtValid = false;
    // true when the read-aloud menu was opened from the document context menu
    bool readAloudMenuFromContext = false;
    HBRUSH brControlBgColor = nullptr;

    DocControllerCallback* cbHandler = nullptr;

    // The target y offset for smooth scrolling.
    // We use a timer to gradually scroll there.
    int scrollTargetY = 0;

    // suppress Read Aloud user-scroll detection during programmatic follow scrolling
    mutable bool readAloudScrollFromCode = false;

    /* when doing a forward search, the result location is highlighted with
     * rectangular marks in the document. These variables indicate the position of the markers
     * and whether they should be shown. */
    struct {
        bool show = false; // are the markers visible?
        Vec<Rect> rects;   // location of the markers in user coordinates
        int page = 0;
        int hideStep = 0; // value used to gradually hide the markers
    } fwdSearchMark;

    StressTest* stressTest = nullptr;

    TouchState touchState;

    FrameRateWnd* frameRateWnd = nullptr;

    // small floating toolbar shown after a text selection in PDF documents
    // that support annotations (controlled by Annotations.SelectionToolbar)
    SelectionToolbar* selectionToolbar = nullptr;

    // set at the beginning of CloseWindow() to prevent
    // processing commands while closing (e.g. reentrancy
    // via modal dialogs pumping messages)
    bool isBeingClosed = false;

    SumatraUIAutomationProvider* uiaProvider = nullptr;

    void UpdateCanvasSize();
    Size GetViewPortSize() const;
    void RedrawAll(bool update = false) const;
    void RedrawAllIncludingNonClient() const;

    void ChangePresentationMode(PresentationMode mode);
    bool InPresentation() const;

    void Focus() const;

    void ToggleZoom() const;
    void MoveDocBy(int dx, int dy) const;

    void ShowToolTip(const char* text, Rect& rc, bool multiline = false) const;
    void DeleteToolTip() const;

    bool CreateUIAProvider();
};

bool HasOpenedDocuments(MainWindow*);
void UpdateControlsColors(MainWindow*);
void ScheduleRepaint(MainWindow*, int delay);
void CreateMovePatternLazy(MainWindow*);
void ClearMouseState(MainWindow*);
bool IsRightDragging(MainWindow*);
MainWindow* FindMainWindowByTab(WindowTab*);
MainWindow* FindMainWindowByHwnd(HWND);
bool IsMainWindowValid(MainWindow*);
bool IsWindowTabValid(WindowTab*);
extern Vec<MainWindow*> gWindows;
void HighlightTab(MainWindow*, WindowTab*);
HWND GetHwndForNotification();

void RelayoutCaption(MainWindow* win);
void OpenSystemMenu(MainWindow* win);
