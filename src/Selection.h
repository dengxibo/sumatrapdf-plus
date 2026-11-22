/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#define SMOOTHSCROLL_TIMER_ID 2
#define SMOOTHSCROLL_DELAY_IN_MS 20
#define SMOOTHSCROLL_SLOW_DOWN_FACTOR 10

struct DisplayModel;
struct MainWindow;
struct WindowTab;
struct TextSel;
class EngineBase;

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

// Highlight band height relative to the engine's glyph band.
constexpr float kHighlightBandBaseRatio = 1.0f;
// Native text follows glyph ink. OCR retains extra room for tight text boxes.
// Selection, find, and read-aloud share the same page-specific ratio.
constexpr float kSelectionHighlightBandRatio = 1.0f;
constexpr float kOcrSelectionHighlightBandRatio = 1.35f;
constexpr float kReadAloudHighlightBandRatio = 1.0f;
constexpr float kFindHighlightBandRatio = 1.0f;
// Preserve the hit area for existing ebook annotations, independently of paint.
constexpr float kEbookAnnotationHitBandRatio = 1.35f;
static_assert(kSelectionHighlightBandRatio == kReadAloudHighlightBandRatio);
static_assert(kSelectionHighlightBandRatio == kFindHighlightBandRatio);
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

// Use the independent OCR ratio for cached OCR and scanned PDF text layers.
// Both settings accept 0.5–1.5; invalid values use their respective defaults.
float HighlightBandRatio(EngineBase* engine, int pageNo);

// Raw PDF text selection -> final highlight quads. Apply only before creating
// a new highlight; saved annotation quads are already in their final geometry.
void PreparePdfHighlightRects(Vec<RectF>& rects, EngineBase* engine, int pageNo, bool vertical = false);
enum class AnnotationType;
// Convert native glyph bands to MuPDF's line-markup appearance coordinates.
void PreparePdfNativeLineRects(Vec<RectF>& rects, AnnotationType type, EngineBase* engine, int pageNo);
// Place new OCR underlines below the ink without changing the OCR hit boxes.
void PreparePdfOcrUnderlineRects(Vec<RectF>& rects, EngineBase* engine, int pageNo, bool vertical, bool squiggly);

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

// Text markup: Multiply on light paper; colored band with dark text on dark paper.
void PaintHighlightRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color, int opacity = 100);
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
