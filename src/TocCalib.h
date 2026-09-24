/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;
struct ExtractedTocItem;
struct MainWindow;
struct WindowTab;
struct TocItem;

struct PageMappingSegment {
    int printedStart = 1;
    int printedEnd = 99999;
    int offset = 0;
    float confidence = 0;
};

struct TocCalibMapRow {
    int printedPage = 0;
    int pdfPage = 0;
    int identPage = 0;
    bool bodyMatched = false;
    bool verified = false;
    bool pdfPinned = false;
};

int TocCalibSolveOffset(const Vec<TocCalibMapRow>& rows);
void TocCalibApplyOffset(Vec<TocCalibMapRow>& rows, int offset, bool force = false);
// Estimate printed→PDF offset from page footers / headers after the TOC spread.
// Returns -1 when no consistent arabic body anchors are found.
int TocCalibEstimateArabicOffset(EngineBase* engine, int afterTocPdf);
bool TocCalibBuildPdgPrintedMap(EngineBase* engine, Vec<int>& pdfByPrinted);
// PDF page whose /PageLabels entry is the decimal printed page. 0 if none.
int TocCalibPdfForPrintedLabel(EngineBase* engine, int printed);
bool TocCalibTestPdgBodyPrinted();
bool TocCalibTestOffsetIgnoresRoughEstimate();
bool TocCalibTestPiecewiseOffset();
bool TocCalibTestEstimateArabicOffsetVotes();

struct TocCalibItemRef {
    int tocId = 0;
    int idx[16]{};
    int len = 0;
};

struct TocCalibRow {
    ExtractedTocItem* item = nullptr;
    int depth = 1;
    bool needsConfirm = false;
    bool userSet = false;
    bool pdfPinned = false;
    bool colChosen = false;
    bool editPdf = false;
    // pageNo was clamped to the document's last page during solve, so it
    // carries no reliable order information.
    bool clamped = false;
    int identPageNo = 0;
    int origPageNo = 0;
    TocCalibItemRef toc;
};

struct TocCalibUndoSnap;

// Cooperative chunked body-verification job (see StartTocCalibAsync). Owned
// by the session while running; opaque outside TocCalib.cpp.
struct TocCalibVerifyJob;

struct TocCalibSession {
    Vec<ExtractedTocItem*> roots;
    Vec<ExtractedTocItem*> extras;
    Vec<ExtractedTocItem*> backup;
    Vec<TocCalibRow> rows;
    Vec<TocCalibUndoSnap*> undo;
    Vec<TocCalibUndoSnap*> redo;
    PageMappingSegment map;
    // Extra pdf-printed offsets after a missing sheet. Each segment starts at
    // printedStart and runs until the next one. map.offset covers folios
    // before the first segment. Printed numbers are not rewritten to keep
    // a single book-wide offset.
    Vec<PageMappingSegment> offsetSegs;
    int tocPage = 0;
    int tocEnd = 0;
    // PDF pages the user confirmed and sent for AI TOC. Double-click searches
    // only these sheets. Empty means the range was guessed from the document.
    Vec<int> confirmedTocPages;
    int nPages = 0;
    bool persistToDisk = true;
    bool offsetLocked = false;
    // Footer/header scan built a printed-page index. Do not invent
    // pageNo = printed + offset for numbers that index did not contain.
    bool footerMapped = false;
    // Owned printed-page index (arabic + R1/roman labels). Kept after the
    // async footer scan so typed R20 / ResolvePrintedLabels can look up
    // without rescanning 1800 pages.
    struct TocCalibPrintedIndex* printedIdx = nullptr;
    bool editPdf = false;
    bool restoreDisplayMode = false;
    int savedDisplayMode = 0;
    bool undoBusy = false;
    // Value of the engine's "PDF outline was modified" flag when the session
    // started. Cancel restores it so a TOC session cannot leave a phantom
    // dirty state behind when the document was clean before the session.
    bool baselineModifiedToc = false;
    EngineBase* engine = nullptr;
    // Non-null while a chunked (async) body verification runs for this
    // session; DeleteTocCalibSession aborts it through this pointer.
    TocCalibVerifyJob* verifyJob = nullptr;
};

int TocCalibTitleMatchScore(const char* body, const char* title);
bool TocCalibApplyNearHit(ExtractedTocItem* it, int hitPage, float x, float y, int score, int predPage);
void TocCalibVerifyNearPredicted(TocCalibSession* s);
void TocCalibRefineExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine);
void TocCalibWriteDebug(const TocCalibSession* s, const char* path);

void DeleteTocCalibSession(TocCalibSession* s);
TocCalibSession* TocCalibSessionFromExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                                              bool scanBody = true, bool deferVerify = false);
void TocCalibSolveSession(TocCalibSession* s);
bool TocCalibCommitPrinted(TocCalibSession* s, int rowIdx, int printed);
bool TocCalibSetOffset(TocCalibSession* s, int offset);
bool TocCalibCommitRow(TocCalibSession* s, int rowIdx, int printed, int pdf, int offset, bool pinPdf);
int TocCalibDisplayPrinted(const TocCalibSession* s, int rowIdx);

int TocCalibAddManualItem(TocCalibSession* s, const char* title, int pageNo, float x, float y, int afterRow,
                          bool asChild = false);
bool TocCalibAddSelectionUnderCurrent(MainWindow* win, const char* title, int pageNo, float x, float y);
bool TocCalibReplaceSelectedFromSelection(MainWindow* win, const char* title, int pageNo, float x, float y);
bool TocCalibTestAddChildManual();
bool TocCalibMergeWithNext(TocCalibSession* s, TocCalibRow* row);
bool TocCalibTestMergeWithNext();

bool StartTocCalib(MainWindow* win, Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                   bool scanBody = true);
// Same outcome as StartTocCalib, but the expensive body-verification pass
// (per-title text search across document pages) runs in cooperative ~40ms
// slices on the UI thread instead of blocking. Callbacks run on the UI
// thread: onProgress(doneRows, totalRows) during verification, onDone(ok)
// once the outline is committed and the calib bar is shown. On early
// failure the function returns false without invoking any callback.
typedef void (*TocCalibVerifyProgressFn)(int done, int total, void* ctx);
typedef void (*TocCalibVerifyDoneFn)(bool ok, void* ctx);
bool StartTocCalibAsync(MainWindow* win, Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                        TocCalibVerifyProgressFn onProgress, TocCalibVerifyDoneFn onDone, void* ctx);
// Remember the TOC sheets the user already checked. tocPage/tocEnd become
// that span, and a later text-score scan must not replace it.
void TocCalibSetConfirmedTocPages(TocCalibSession* s, const Vec<int>& pages);
bool StartTocCalibFromExisting(MainWindow* win);
void ShowTocCalib(MainWindow* win);
void HideTocCalib(MainWindow* win);
void TocCalibUpdateTheme(MainWindow* win);
void DeleteTocCalibUi(MainWindow* win);
bool TocCalibIsActive(MainWindow* win);
bool TocCalibBarVisible(MainWindow* win);
void RelayoutTocCalib(MainWindow* win);
int TocCalibBarDy(MainWindow* win);
void TocCalibOnTabSwitch(MainWindow* win);
void CloseTocCalibForTab(WindowTab* tab);
void TocCalibRebind(MainWindow* win);
TocCalibRow* TocCalibRowForTocItem(MainWindow* win, TocItem* item);
bool TocCalibRenameItem(MainWindow* win, TocItem* item, const char* title);
int TocCalibColumnsDx(HWND hwnd);
void TocCalibDrawColumns(HDC hdc, HWND hwnd, const RECT& rcRow, TocItem* item, MainWindow* win, bool selected);
bool TocCalibHandleRowClick(MainWindow* win, TocItem* item, int x, int y, const RECT& rcRow);
bool TocCalibHandleTreeClick(MainWindow* win, HWND hwnd, POINT pt);
bool TocCalibHandleDrop(MainWindow* win, TocItem* dest, int dropPos);
enum class TocCalibOutlineOp {
    MoveUp,
    MoveDown,
    Promote,
    Demote
};
bool TocCalibHandleOutlineOp(MainWindow* win, TocCalibOutlineOp op);
bool TocCalibHandleDelete(MainWindow* win);
bool TocCalibUndo(MainWindow* win);
bool TocCalibRedo(MainWindow* win);
bool TocCalibHandleUndoShortcut(MainWindow* win, HWND focus, int vk, bool ctrl, bool shift);
bool TocCalibTestUndo();
bool TocCalibTestPromoteDemote();
bool TocCalibDeleteAndPromote(TocCalibSession* s, ExtractedTocItem* n);
bool TocCalibTestDeletePromotesChildren();
bool TocCalibTestDropMoveAndNest();
bool TocCalibParsePrintedText(const char* s, int* printedOut, char** labelOut);
bool TocCalibPinSelectedToView(MainWindow* win);
bool TocCalibLocateSelectedInBody(MainWindow* win);
// Selected calib row has a printed page and a PDF page, so later rows can
// reuse pdf - printed. False outside enhance mode.
bool TocCalibCanApplyOffsetBelow(MainWindow* win);
// From the selected row downward, set printed + that offset. Rows above stay.
// A later row the user already edited stays as they left it.
bool TocCalibApplyOffsetBelow(MainWindow* win);
bool TocCalibTestPrintedInput();
bool TocCalibTestPrintedIndexLookup();
bool TocCalibTestBm25Locate();
bool TocCalibTestFindQuery();
bool TocCalibTestInterpolatePrinted();
bool TocCalibTestPinOverwritesPrinted();
bool TocCalibTestFrontMatterUsesLabel();
bool TocCalibTestClearTocDests();
bool TocCalibIsPageFieldAt(MainWindow* win, HWND hwnd, POINT pt);
bool TocCalibIsPageControlAt(MainWindow* win, HWND hwnd, POINT pt);
const char* TocCalibRowControlTip(MainWindow* win, HWND hwnd, POINT pt);
bool TocCalibColorPageEdit(HWND edit, HDC hdc, HBRUSH* brOut);
void TocCalibClosePageEdit(bool commit);
void TocCalibJumpToContents(MainWindow* win);
void TocCalibJumpToItemContents(MainWindow* win, TocItem* item);
