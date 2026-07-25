/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#define kSumatraDdeServer L"SUMATRA"
#define kSumatraDdeTopic L"control"

// WM_COPYDATA magic numbers (in COPYDATASTRUCT::dwData):
// - kCopyDataDdeW   : payload is a null-terminated UTF-16 DDE command string
//                    ("[Open(\"...\",...)]..."). Handled synchronously via
//                    the full DDE grammar in HandleExecuteCmds.
// - kCopyDataOpen   : payload is a SumatraOpenCopyData struct followed by the
//                    UTF-8 null-terminated path. Handled asynchronously so
//                    the sending instance (launched by Explorer for
//                    reuseInstance) can exit immediately without waiting for
//                    the receiver to finish loading the file.
#define kCopyDataDdeW 0x44646557 // 'DdeW'
#define kCopyDataOpen 0x4F70656E // 'Open'

struct SumatraOpenCopyData {
    u32 newWindow; // 0: reuse existing, non-zero: force new window
    // followed by UTF-8 path, null-terminated
};

LRESULT OnDDEInitiate(HWND hwnd, WPARAM wp, LPARAM lp);
LRESULT OnDDExecute(HWND hwnd, WPARAM wp, LPARAM lp);
LRESULT OnDDERequest(HWND hwnd, WPARAM wp, LPARAM lp);
LRESULT OnDDETerminate(HWND hwnd, WPARAM wp, LPARAM lp);
LRESULT OnCopyData(HWND hwnd, WPARAM wp, LPARAM lp);

#define HIDE_FWDSRCHMARK_TIMER_ID 4
#define HIDE_FWDSRCHMARK_DELAY_IN_MS 400
#define HIDE_FWDSRCHMARK_DECAYINTERVAL_IN_MS 100
#define HIDE_FWDSRCHMARK_STEPS 5

// animated "." / ".." / "..." while find/count is running (hwndFrame)
#define kFindStatusAnimateTimerId 0x101
#define kFindStatusAnimateMs 400
// brief status-text highlight when a full-document count finishes (hwndFrame)
#define kFindStatusCompleteFlashTimerId 0x102
#define kFindStatusCompleteFlashMs 450

bool NeedsFindUI(MainWindow* win);
// false while a reflowable ebook (EPUB/MOBI etc.) is still formatting pages
bool IsDocumentSearchReady(MainWindow* win);
void ClearSearchResult(MainWindow* win);
bool OnInverseSearch(MainWindow* win, int x, int y);
void ShowForwardSearchResult(MainWindow* win, const char* fileName, int line, int col, int ret, int page,
                             Vec<Rect>& rects);
void PaintForwardSearchMark(MainWindow* win, HDC hdc);
void PaintAllFindMatches(MainWindow* win, HDC hdc);
void OnFindViewLayoutChanged(MainWindow* win);

// when true, paint every visible search match (current match uses FindMatchColor,
// other matches on the page use a secondary orange)
void FindPrev(MainWindow* win);
void FindNext(MainWindow* win);
void FindFirst(MainWindow* win);
void FindToggleMatchCase(MainWindow* win);
void FindToggleMatchWholeWord(MainWindow* win);
// called when the user edits the find bar's text
void OnFindBarTextChanged(MainWindow* win);
void StartFindStatusAnimation(MainWindow* win);
void StopFindStatusAnimation(MainWindow* win);
void FindStatusAnimateTimerFired(MainWindow* win);
// if the current term has not been searched yet, start the search now (Enter).
// Returns true if a search was started.
bool FindFlushPendingSearch(MainWindow* win);
// navigate to and select a match chosen from the floating results list
void GoToFindMatch(MainWindow* win, int startPage, int startGlyph, int endPage, int endGlyph);
// start (or refresh) the deferred full-document match count
void RequestFindCount(MainWindow* win);
// clear the find box and in-flight/cached search state when switching tabs
// (destroys find bar/window HWNDs, aborts workers, resets MainWindow find state)
void ResetFindUIForTabSwitch(MainWindow* win);
// tear down find UI HWNDs and reset all search state (tab switch, Esc, close)
void CloseFindUI(MainWindow* win);
// update n/m from the valid count cache (no-op if count not ready)
void UpdateFindMatchCountDisplay(MainWindow* win);
// free the cached per-match snippets (win->findMatches)
void ClearFindMatches(MainWindow* win);
// rebuild snippet strings after the floating find window is resized wider
void RebuildFindMatchSnippets(MainWindow* win);
// populate the floating results list from the position cache (e.g. after
// expanding from the compact bar, or when a count finished without snippets)
void SyncFindResultsList(MainWindow* win);
// called when progressive ebook loading adds pages (or finishes)
void OnEbookPageCountChanged(MainWindow* win);
// update find status bar when ebook loading blocks search
void RefreshFindSearchBlockedStatus(MainWindow* win);
void FindSelection(MainWindow* win, TextSearch::Direction direction);
bool AbortFinding(MainWindow* win, bool hideMessage, bool waitForWorkers = true, bool pumpMessages = true);
// wait for the count/find worker before using the engine on the UI thread (lookup/tts)
void SuspendFindEngineAccess(MainWindow* win);
void FindTextOnThread(MainWindow* win, TextSearch::Direction direction);
void FindTextOnThread(MainWindow* win, TextSearch::Direction direction, const char* text, bool wasModified);
extern bool gIsStartup;
extern StrVec gDdeOpenOnStartup;
