/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#define SMOOTHSCROLL_TIMER_ID 2
#define SMOOTHSCROLL_DELAY_IN_MS 20
#define SMOOTHSCROLL_SLOW_DOWN_FACTOR 10

struct DisplayModel;
struct MainWindow;
struct WindowTab;
struct TextSel;

/* Represents selected area on given page */
struct SelectionOnPage {
    explicit SelectionOnPage(int pageNo = 0, const RectF* const rect = nullptr);

    int pageNo; // page this selection is on
    RectF rect; // position of selection rectangle on page (in page coordinates)

    SelectionOnPage(const SelectionOnPage&) = default;
    SelectionOnPage& operator=(const SelectionOnPage&) = default;

    // position of selection rectangle in the view port
    Rect GetRect(DisplayModel* dm) const;

    static Vec<SelectionOnPage>* FromRectangle(DisplayModel* dm, Rect rect);
    static Vec<SelectionOnPage>* FromTextSelect(TextSel* textSel);
};

// Highlight band height as a fraction of font size (engine stores coords at kHighlightBandBaseRatio).
constexpr float kHighlightBandBaseRatio = 1.0f;
constexpr float kSelectionHighlightBandRatio = 1.10f;
constexpr float kReadAloudHighlightBandRatio = 0.80f;
// Default opacity when SelectionColor has no alpha (#rrggbb). Used by alpha overlays (e.g. search).
constexpr u8 kSelectionDefaultAlpha = 0x5f;
constexpr u8 kSelectionHighlightAlpha = kSelectionDefaultAlpha;

COLORREF GetSelectionHighlightColor();
// Read-aloud follow highlight: always yellow, independent of SelectionColor.
inline COLORREF GetReadAloudHighlightColor() {
    return RGB(255, 255, 0);
}

// Scale a highlight rect to the given band ratio (page coordinates).
RectF ScaleHighlightBandRect(RectF r, float bandRatio);

// Merge two highlight rects on the same line (horizontal span, uniform band height).
RectF MergeHighlightLineRect(RectF a, RectF b);

// Build one highlight rect for a run of glyph boxes on the same line.
Rect BuildHighlightLineRect(Rect* c0, Rect* cEnd);

// Align highlight band height for rects on the same text line (page coordinates).
void NormalizeHighlightLineHeights(Vec<RectF>& rects);

// Use one band height for every highlight rect in a selection (page coordinates).
void NormalizeHighlightUniformHeight(Vec<RectF>& rects);

// Match band height only between consecutive lines with similar font size.
void NormalizeNearbyHighlightHeights(Vec<RectF>& rects);

void DeleteOldSelectionInfo(MainWindow* win, bool alsoTextSel = false);
void PaintTransparentRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF selectionColor,
                                u8 alpha = kSelectionDefaultAlpha, int pad = 2);

// Text selection / highlight: multiply blend with page pixels (MuPDF/Acrobat-style).
void PaintMultiplyRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color);
void PaintSelection(MainWindow* win, HDC hdc);
void UpdateTextSelection(MainWindow* win, bool select = true);
// Rebuild text selection after relayout (e.g. theme/document color change) by
// re-searching the selected text near the original page.
void RefreshTextSelectionAfterLayoutChange(WindowTab* tab, MainWindow* win = nullptr);
void CopySelectionToClipboard(MainWindow* win);
void OnSelectAll(MainWindow* win, bool textOnly = false);
bool NeedsSelectionEdgeAutoscroll(MainWindow* win, int x, int y);
void OnSelectionEdgeAutoscroll(MainWindow* win, int x, int y);
void OnSelectionStart(MainWindow* win, int x, int y, WPARAM key);
void OnSelectionStop(MainWindow* win, int x, int y, bool aborted);
TempStr GetSelectedTextTemp(WindowTab* tab, const char* lineSep, bool& isTextOnlySelectionOut);
