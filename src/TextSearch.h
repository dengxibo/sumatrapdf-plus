/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct TextSearch : public TextSelection {
    enum class Direction : bool {
        Backward = false,
        Forward = true
    };

    explicit TextSearch(EngineBase* engine);
    ~TextSearch();

    void SetMatchCase(bool sensitive);
    void SetMatchWholeWord(bool wholeWord);
    void SetDirection(Direction direction);
    // cap search range during progressive ebook loading (0 = use engine page count)
    void SetMaxPageCount(int maxPageCount);
    void SetLastResult(TextSelection* sel);
    TextSel* FindFirst(int page, const WCHAR* text);
    TextSel* FindNext();

    int GetCurrentPageNo() const;
    int GetSearchHitStartPageNo() const;

    ProgressUpdateCb progressCb;

    // Lightweight container for page and offset within the page to use as return value of MatchEnd
    struct PageAndOffset {
        int page;
        int offset;
    };

    struct MatchSpan {
        int startPage;
        int startGlyph;
        int endPage;
        int endGlyph;
    };

    // POD page match list: safe inside Vec<> (unlike Vec<Vec<MatchSpan>>)
    struct PageMatchList {
        MatchSpan* data = nullptr;
        int count = 0;
    };

    char* findText = nullptr;
    char* anchor = nullptr;
    int findTextLen = 0;
    int anchorLen = 0;
    u32 anchorAsciiMask = 0;
    int findPage = 0;
    int searchHitStartAt = 0; // when text found spans several pages, searchHitStartAt < findPage
    bool forward = true;
    bool matchCase = false;
    // when set, the search only matches complete words: it forces both
    // matchWordStart and matchWordEnd on regardless of leading/trailing spaces
    // (issue #4295)
    bool matchWholeWord = false;
    // these two options are implicitly set when the search text begins
    // resp. ends in a single space (many users already search that way),
    // combining them yields a 'Whole words' search
    bool matchWordStart = false;
    bool matchWordEnd = false;

    void SetText(const WCHAR* text);
    bool FindTextInPage(int pageNo, PageAndOffset* finalGlyph);
    bool FindStartingAtPage(int pageNo);
    // Find every match that starts on pageNo (single page-text load).
    void CollectMatchesOnPage(int pageNo, Vec<MatchSpan>* out);
    PageAndOffset MatchEnd(int startOff) const;
    // Search offsets are Unicode codepoints in UTF-8 text. Selection offsets are
    // UTF-16 code units in the engine's WCHAR text.
    int CodepointToGlyph(int pageNo, int codepointOffset) const;
    int GlyphToCodepoint(int pageNo, int glyphOffset) const;

    void Clear();
    void Reset();

    // keep in sync with engine page count (progressive ebook loading grows pages after ctor)
    void SyncPageCount();

    const char* pageText = nullptr;
    int pageTextLen = 0;
    int pageTextPage = 0;
    int findIndex = 0;

    WCHAR* lastText = nullptr;
    int nPages = 0;
    int maxPageCount = 0;
    Vec<bool> pagesToSkip;
    // per-page match lists built by CollectMatchesOnPage (reused by count scans)
    Vec<PageMatchList> pageMatchesCache;
    Vec<bool> pageMatchesCached;

    void EnsurePageMatchCacheSize();
    void SetPageMatchCache(int pageNo, const Vec<MatchSpan>& spans);
    void SetPageMatchCache(int pageNo, const PageMatchList& spans);
    void ApplyCachedAsciiPageSkip();
    void ApplyCachedUtf8AnchorPageSkip();
    const char* LoadPageText(int pageNo, int* lenOut, bool* abortSearch);
    bool TryGetCachedPageMatches(int pageNo, Vec<MatchSpan>* out) const;
    bool PageMightContainAnchor(int pageNo) const;
    // jump to a match using a cached session position list (wrap order from startPage)
    bool TryFindFromCachedPositions(const Vec<u64>& positions, int startPage);
    // discard per-page match lists after reflow/layout changes
    void InvalidatePageMatchCache();
};
