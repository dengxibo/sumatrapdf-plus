/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;
struct DisplayModel;
struct MainWindow;
struct StrBuilder;
struct TextSelection;

struct ReadAloudByteLoc {
    int pageNo = -1;
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    // EPUB reflow: stable anchor across theme/layout changes
    int chapter = -1;
    int byteOff = -1;
};

struct ReadAloudHighlightMap {
    int len = 0;
    int cap = 0;
    ReadAloudByteLoc* locs = nullptr;
};

constexpr int kReadAloudBuildPagesPerBatch = 32;

void ReadAloudHighlightFree(ReadAloudHighlightMap* map);

bool ReadAloudHighlightBuildFromPage(EngineBase* engine, int pageNo, ReadAloudHighlightMap* map,
                                     StrBuilder& cleanedOut);

bool ReadAloudHighlightBuildFromTextSelection(TextSelection* ts, ReadAloudHighlightMap* map, StrBuilder& cleanedOut);

bool ReadAloudGetViewportStart(DisplayModel* dm, int* startPageOut, int* startGlyphOut);

bool ReadAloudCanReadFromCursor(DisplayModel* dm, Point screenPt);

bool ReadAloudGetCursorStart(DisplayModel* dm, Point screenPt, int* startPageOut, int* startGlyphOut);

bool ReadAloudGetStartBelowPoint(DisplayModel* dm, Point screenPt, int* startPageOut, int* startGlyphOut);

bool ReadAloudHighlightBuildFromDocument(DisplayModel* dm, int startPage, int startGlyph, int endPageInclusive,
                                         ReadAloudHighlightMap* map, StrBuilder& cleanedOut);

bool ReadAloudHighlightAppendDocumentPages(DisplayModel* dm, int startPage, int endPageInclusive,
                                           ReadAloudHighlightMap* map, char** textInOut);

bool ReadAloudShouldJoinAtWrappedLine(const char* beforeEnd, int beforeLen, const char* afterStart);

// decodes one utf8 codepoint, advancing s; returns false at end of string
bool ReadAloudDecodeUtf8One(const char*& s, char32_t* cpOut);

void ReadAloudHighlightTimerStart(MainWindow* win);
void ReadAloudHighlightTimerStop(MainWindow* win);

void ReadAloudOnUserViewChanged(MainWindow* win);
void ReadAloudUpdateAutoScroll(MainWindow* win);

bool ReadAloudGetProgressPage(WindowTab* tab, int* pageOut, int* pageCountOut);

void PaintReadAloudHighlight(MainWindow* win, HDC hdc);
// Rebuild read-aloud glyph coordinates after relayout (e.g. theme/document color change).
// Returns false when the highlight map could not be refreshed. When textRelocatedOut
// is set true, the caller should restart the current TTS chunk.
bool RefreshReadAloudHighlightAfterLayoutChange(WindowTab* tab, MainWindow* win = nullptr,
                                                bool* textRelocatedOut = nullptr);