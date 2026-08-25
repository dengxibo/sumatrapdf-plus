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
    int identPageNo = 0;
    int origPageNo = 0;
    TocCalibItemRef toc;
};

struct TocCalibUndoSnap;

struct TocCalibSession {
    Vec<ExtractedTocItem*> roots;
    Vec<ExtractedTocItem*> extras;
    Vec<ExtractedTocItem*> backup;
    Vec<TocCalibRow> rows;
    Vec<TocCalibUndoSnap*> undo;
    Vec<TocCalibUndoSnap*> redo;
    PageMappingSegment map;
    int tocPage = 0;
    int tocEnd = 0;
    int nPages = 0;
    bool persistToDisk = true;
    bool offsetLocked = false;
    bool editPdf = false;
    bool restoreDisplayMode = false;
    int savedDisplayMode = 0;
    bool undoBusy = false;
    EngineBase* engine = nullptr;
};

int TocCalibTitleMatchScore(const char* body, const char* title);
bool TocCalibApplyNearHit(ExtractedTocItem* it, int hitPage, float x, float y, int score, int predPage);
void TocCalibVerifyNearPredicted(TocCalibSession* s);
void TocCalibRefineExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine);
void TocCalibWriteDebug(const TocCalibSession* s, const char* path);

void DeleteTocCalibSession(TocCalibSession* s);
TocCalibSession* TocCalibSessionFromExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                                              bool scanBody = true);
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
bool StartTocCalibFromExisting(MainWindow* win);
void ShowTocCalib(MainWindow* win);
void HideTocCalib(MainWindow* win);
void TocCalibUpdateTheme(MainWindow* win);
void DeleteTocCalibUi(MainWindow* win);
bool TocCalibIsActive(MainWindow* win);
void RelayoutTocCalib(MainWindow* win);
void TocCalibFillLiveDrag(MainWindow* win);
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
bool TocCalibTestPrintedInput();
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
