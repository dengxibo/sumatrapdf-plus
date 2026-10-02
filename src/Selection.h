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
// Leave a modest vertical margin around glyph ink. Keep all interactive
// highlights on the same band geometry so selection, reading, and search do
// not appear to jump in height when switching actions.
constexpr float kSelectionHighlightBandRatio = 1.35f;
constexpr float kReadAloudHighlightBandRatio = 1.35f;
constexpr float kFindHighlightBandRatio = 1.35f;
// OCR cells tend to sit slightly above the visible glyph baseline. Shift the
// common band down so most of the extra breathing room is below the glyph.
constexpr float kHighlightBandCenterOffsetRatio = 0.075f;
// Default opacity when SelectionColor has no alpha (#rrggbb). Used by alpha overlays (e.g. search).
constexpr u8 kSelectionDefaultAlpha = 0x5f;
constexpr u8 kSelectionHighlightAlpha = kSelectionDefaultAlpha;

COLORREF GetSelectionHighlightColor();

// Find-match highlights on the page; defaults to yellow in a fresh settings file.
COLORREF GetFindMatchHighlightColor();

// Scale a highlight rect to the given band ratio (page coordinates).
RectF ScaleHighlightBandRect(RectF r, float bandRatio);

// OCR line boxes hug the x-height, so a marker through "that" misses the top of t/h
// and the tail of g/y. weight is the number of letters in the box (CJK weight 2).
// A box that is already about an em tall is left alone.
RectF ExpandOcrXHeightBand(RectF r, int weight);

// Merge two highlight rects on the same line (horizontal span, uniform band height).
RectF MergeHighlightLineRect(RectF a, RectF b);

// Build one highlight rect for a run of glyph boxes on the same line.
Rect BuildHighlightLineRect(Rect* c0, Rect* cEnd);

// True when c starts the next visual line. Joined CJK paragraph lines have no
// zero-width separator; a superscript still overlaps its base glyph.
bool GlyphJumpsToNextBandLine(const Rect& band, const Rect& c);

// Align highlight band height for rects on the same text line (page coordinates).
void NormalizeHighlightLineHeights(Vec<RectF>& rects);

// Use one band height for every highlight rect in a selection (page coordinates).
void NormalizeHighlightUniformHeight(Vec<RectF>& rects);

// Match band height only between consecutive lines with similar font size.
void NormalizeNearbyHighlightHeights(Vec<RectF>& rects);

void DeleteOldSelectionInfo(MainWindow* win, bool alsoTextSel = false);
// Drops the highlight when it is still this selection. A newer selection is left alone.
void ClearSelectionIfCurrent(MainWindow* win, Vec<SelectionOnPage>* sel);
void PaintTransparentRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF selectionColor,
                                u8 alpha = kSelectionDefaultAlpha, int pad = 2);
// Find highlights: alpha overlay in light mode; marker-style (yellow band, dark text) in dark mode.
void PaintFindMatchHighlightRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color, u8 alpha);

// Text selection / highlight: multiply blend with page pixels (MuPDF/Acrobat-style).
void PaintMultiplyRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color, int opacity = 100);
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
TempStr GetSelectedTextTemp(WindowTab* tab, const char* lineSep, bool& isTextOnlySelectionOut, bool mergeLines = false);
