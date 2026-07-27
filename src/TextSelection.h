/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// TODO: replace with Vec<TextSel>
struct TextSel {
    int len = 0;
    int cap = 0;
    int* pages = nullptr;
    Rect* rects = nullptr;
};

struct TextSelection {
    int startPage = -1;
    int endPage = -1;
    int startGlyph = -1;
    int endGlyph = -1;
    float startDragX = 0;
    int dragHoriz = 0;

    EngineBase* engine = nullptr;

    explicit TextSelection(EngineBase* engine);
    ~TextSelection();

    bool IsOverGlyph(int pageNo, double x, double y);
    void StartAt(int pageNo, int glyphIx);
    void StartAt(int pageNo, double x, double y);
    void SelectUpTo(int pageNo, int glyphIx);
    void SelectUpTo(int pageNo, double x, double y);
    void SelectWordAt(int pageNo, double x, double y);
    void SelectPageBbox(int pageNo, RectF bbox);
    int GlyphIndexAt(int pageNo, double x, double y);
    void SelectGlyphRange(int pageNo, int startGlyph, int endGlyph);
    char* ExtractWordAt(int pageNo, double x, double y);
    void CopySelection(TextSelection* orig);
    WCHAR* ExtractText(const char* lineSep);
    void Reset();

    TextSel result{};

    void GetGlyphRange(int* fromPage, int* fromGlyph, int* toPage, int* toGlyph) const;
};

uint distSq(int x, int y);
bool isCjkWordChar(WCHAR c);
bool isWordChar(WCHAR c);
bool isWordChar(int codepoint);
bool isNonCjkWordChar(WCHAR c);
