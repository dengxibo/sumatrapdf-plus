/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/ThreadUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "PdfSync.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "AppTools.h"
#include "SearchAndDDE.h"
#include "Selection.h"
#include "Toolbar.h"
#include "FindBar.h"
#include "FindWindow.h"
#include "SumatraDialogs.h"
#include "Translations.h"

#include "OcrService.h"

#include "utils/Log.h"

bool gIsStartup = false;
StrVec gDdeOpenOnStartup;

// Chrome-style orange for non-active find matches on the visible page. The
// active (current) match uses FixedPageUI.FindMatchColor (default yellow).
constexpr COLORREF kFindOtherMatchColor = RGB(0xff, 0x96, 0x32);

struct FindMatchPaintPageRect {
    int pageNo = 0;
    Rect rect{};
};

void OnFindViewLayoutChanged(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (engine) {
        EngineMupdfInvalidateSearchTextCache(engine);
    }
    if (!dm->textSearch) {
        return;
    }
    TextSearch* ts = dm->textSearch;
    if (ts->result.len == 0) {
        return;
    }
    int startPage = ts->startPage;
    int startGlyph = ts->startGlyph;
    int endPage = ts->endPage;
    int endGlyph = ts->endGlyph;
    ts->Reset();
    ts->StartAt(startPage, startGlyph);
    ts->SelectUpTo(endPage, endGlyph);
}

Kind kNotifFindProgress = "findProgress";

// don't show the Search UI for document types that don't
// support extracting text and/or navigating to a specific
// text selection; default to showing it, since most users
// will never use a format that does not support search
bool NeedsFindUI(MainWindow* win) {
    if (!win->IsDocLoaded()) {
        return true;
    }
    if (!win->AsFixed()) {
        return false;
    }
    if (win->AsFixed()->GetEngine()->IsImageCollection()) {
        return false;
    }
    return true;
}

bool IsDocumentSearchReady(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    // Progressive ebooks publish a stable leading range of pages. Searching
    // that range is safe; OnEbookPageCountChanged() extends the same search as
    // more pages become available.
    return dm->PageCount() > 0;
}

void RefreshFindSearchBlockedStatus(MainWindow* win) {
    if (!win || !IsFindUIVisible(win)) {
        return;
    }
    if (!IsDocumentSearchReady(win)) {
        // Progressive documents start searching as soon as their first page is
        // published. Until then there is no special loading state in the find
        // UI; leave the counter area empty.
        FindBarSetStatus(win, "");
        return;
    }
    if (!win->findCountValid && !win->findThread && !win->findCountThread && !win->findStatusAnimating) {
        TempStr s = win->hwndFindEdit ? HwndGetTextTemp(win->hwndFindEdit) : nullptr;
        if (str::IsEmpty(s)) {
            FindBarSetStatus(win, "");
        }
    }
}

static bool EnsureDocumentSearchReady(MainWindow* win) {
    if (IsDocumentSearchReady(win)) {
        return true;
    }
    RefreshFindSearchBlockedStatus(win);
    return false;
}

static EngineBase* CurrentFindEngine(MainWindow* win) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    return dm ? dm->GetEngine() : nullptr;
}

// true when a find/count worker still belongs to a previous tab/document
static bool HasStaleFindWorkers(MainWindow* win) {
    if (!win) {
        return false;
    }
    EngineBase* cur = CurrentFindEngine(win);
    if ((win->findCountThread || win->findThread) && win->findCountEngine &&
        (!cur || (void*)cur != win->findCountEngine)) {
        return true;
    }
    return false;
}

static void AbortActiveFindWorkers(MainWindow* win, bool hideMessage = true, bool waitForWorkers = false,
                                   bool pumpMessages = true) {
    if (!win) {
        return;
    }
    AbortFinding(win, hideMessage, waitForWorkers, pumpMessages);
    uitask::DrainQueue();
}

void FindFirst(MainWindow* win) {
    if (win->AsChm()) {
        win->AsChm()->FindInCurrentPage();
        return;
    }

    if (!win->AsFixed() || !NeedsFindUI(win)) {
        return;
    }

    if (HasStaleFindWorkers(win)) {
        AbortActiveFindWorkers(win);
    }

    DisplayModel* dm = win->AsFixed();
    bool hadFindFocus = win->hwndFindEdit && HwndIsFocused(win->hwndFindEdit);
    ShowFindBar(win);
    if (!win->hwndFindEdit) {
        return;
    }

    // If focus was in the document (not find bar), copy selected text
    // to find edit only if it's different from current text
    if (!hadFindFocus && dm->textSelection->result.len > 0) {
        AutoFreeWStr selection(dm->textSelection->ExtractText(" "));
        str::NormalizeWSInPlace(selection);
        if (!str::IsEmpty(selection.Get())) {
            TempStr s = ToUtf8Temp(selection);
            TempStr current = HwndGetTextTemp(win->hwndFindEdit);
            if (!str::EqI(s, current)) {
                AbortFinding(win, false);
                dm->textSearch->SetLastResult(dm->textSelection);
                HwndSetText(win->hwndFindEdit, s);
            }
        }
    }

    HwndSetFocus(win->hwndFindEdit);
    Edit_SetSel(win->hwndFindEdit, 0, -1);
    RefreshFindSearchBlockedStatus(win);
}

// run the actual search; assumes there is non-empty find text
static void StartIncrementalFind(MainWindow* win) {
    FindTextOnThread(win, TextSearch::Direction::Forward);
}

static void StartFindCount(MainWindow* win, const WCHAR* text, bool matchCase, bool matchWholeWord);

// Called whenever the shared find edit changes. Replace an in-flight count with
// the newest term immediately; Enter remains pending so it can navigate to the
// first/next result without being required to make the list start updating.
void OnFindBarTextChanged(MainWindow* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    TempStr s = HwndGetTextTemp(win->hwndFindEdit);
    if (str::IsEmpty(s)) {
        // The count worker owns a private TextSearch and checks its epoch after
        // every page, so don't make the UI wait for it. A find worker uses the
        // document TextSearch and must be joined before clearing that selection.
        AbortFinding(win, true, win->findThread != nullptr);
        StopFindStatusAnimation(win);
        ClearSearchResult(win);
        FindBarSetStatus(win, "");
        ClearFindMatches(win);
        FindWindowRefreshResults(win); // empty the results list
        return;
    }
    win->findEnterPending = true;
    win->findPendingFromPage = 0;
    win->findCountValid = false;
    win->findCountPartial = false;
    FindBarSetStatus(win, "");
    ClearFindMatches(win);
    FindWindowRefreshResults(win, false);

    // A first-match worker shares dm->textSearch, so finish its cancellation
    // before starting a count worker. An existing count worker is canceled and
    // coalesced asynchronously by StartFindCount(), keeping typing responsive.
    if (win->findThread) {
        AbortFinding(win, false, true);
        win->findEnterPending = true;
    }
    TempWStr text = ToWStrTemp(s);
    StartFindCount(win, text, win->findMatchCase, win->findMatchWholeWord);
}

static TempStr FindStatusAnimTextTemp(int currentIdx, int totalKnown, int dotPhase) {
    int dots = (dotPhase % 3) + 1;
    if (currentIdx < 1 && totalKnown < 1) {
        return str::FormatTemp("%.*s", dots, "...");
    }
    if (currentIdx < 1) {
        return str::FormatTemp("0 / %d %.*s", totalKnown, dots, "...");
    }
    if (totalKnown < 1) {
        return str::FormatTemp("%d / %.*s", currentIdx, dots, "...");
    }
    return str::FormatTemp("%d / %d%.*s", currentIdx, totalKnown, dots, "...");
}

static int FindKnownMatchCount(MainWindow* win) {
    return std::max(win->findCountLatestFound, (int)win->findMatches.size());
}

static void UpdateFindStatusAnimDisplay(MainWindow* win) {
    if (!win) {
        return;
    }
    int cur = win->findStatusCurrentIndex;
    int total = FindKnownMatchCount(win);
    FindBarSetStatus(win, FindStatusAnimTextTemp(cur, total, win->findStatusDotPhase));
}

void StartFindStatusAnimation(MainWindow* win) {
    if (!win || !win->hwndFrame) {
        return;
    }
    if (!win->findStatusAnimating) {
        win->findStatusDotPhase = 0;
        win->findStatusCurrentIndex = 0;
    }
    win->findStatusAnimating = true;
    UpdateFindStatusAnimDisplay(win);
    SetTimer(win->hwndFrame, kFindStatusAnimateTimerId, kFindStatusAnimateMs, nullptr);
}

void StopFindStatusAnimation(MainWindow* win) {
    if (!win) {
        return;
    }
    if (win->hwndFrame) {
        KillTimer(win->hwndFrame, kFindStatusAnimateTimerId);
    }
    win->findStatusAnimating = false;
    win->findStatusCurrentIndex = 0;
    win->findCountLatestFound = 0;
    InterlockedExchange(&win->findCountAnimTaskPending, 0);
}

struct FindCountAnimTaskData {
    MainWindow* win = nullptr;
    LONG epoch = 0;
};

static void ShowMatchCount(MainWindow* win);

static void FindCountAnimTask(FindCountAnimTaskData* d) {
    AutoDelete delData(d);
    MainWindow* win = d->win;
    if (!IsMainWindowValid(win)) {
        return;
    }
    InterlockedExchange(&win->findCountAnimTaskPending, 0);
    if ((win->findCountValid && !win->findCountPartial) || win->findCountEpoch != d->epoch) {
        return;
    }
    if (win->findStatusAnimating) {
        win->findStatusDotPhase = (win->findStatusDotPhase + 1) % 3;
    }
    ShowMatchCount(win);
}

static void MaybePostFindCountAnimTask(MainWindow* win, LONG epoch) {
    if (!win || !win->findStatusAnimating || (win->findCountValid && !win->findCountPartial)) {
        return;
    }
    if (InterlockedCompareExchange(&win->findCountAnimTaskPending, 1, 0) != 0) {
        return;
    }
    auto d = new FindCountAnimTaskData;
    d->win = win;
    d->epoch = epoch;
    uitask::Post(MkFunc0<FindCountAnimTaskData>(FindCountAnimTask, d), "TaskFindCountAnim");
}

void FindStatusAnimateTimerFired(MainWindow* win) {
    if (!win || !win->findStatusAnimating || (win->findCountValid && !win->findCountPartial)) {
        StopFindStatusAnimation(win);
        return;
    }
    // During a full-document count the worker posts coalesced anim tasks instead;
    // the timer covers the shorter find-first-match phase before counting starts.
    if (win->findCountThread) {
        return;
    }
    win->findStatusDotPhase = (win->findStatusDotPhase + 1) % 3;
    UpdateFindStatusAnimDisplay(win);
}

static bool HasFindText(MainWindow* win) {
    return win->hwndFindEdit && HwndGetTextLen(win->hwndFindEdit) > 0;
}

static void ClearFindSearchProgressCb(MainWindow* win);

void ResetFindUIForTabSwitch(MainWindow* win) {
    CloseFindUI(win);
}

void CloseFindUI(MainWindow* win) {
    if (!win) {
        return;
    }
    if (!win->hwndFindEdit && !win->findThread && !win->findCountThread && !win->findBar && !win->findWindow) {
        return;
    }
    // drop pending work before any abort that pumps messages (CountEndTask must
    // not restart a scan for the previous tab while we're switching)
    str::FreePtr(&win->findCountPendingText);
    win->findCountPendingMatchCase = false;
    win->findCountPendingMatchWholeWord = false;
    InterlockedIncrement(&win->findCountEpoch);
    StealFocusFromFindUI(win);
    if (win->hwndFrame && ::IsWindow(win->hwndFrame)) {
        HwndSetFocus(win->hwndFrame);
    }
    StopFindStatusAnimation(win);
    // A tab switch must leave no search object or engine reference behind. Wait
    // for both workers, but do not dispatch window messages while waiting: a
    // nested tab notification here would re-enter LoadModelIntoTab with
    // currentTab and ctrl only half switched. Drain only our queued completion
    // tasks after the worker handles have signaled so their data is destroyed.
    AbortActiveFindWorkers(win, true, true, false);
    DestroyFindUI(win);
    win->findEnterPending = false;
    win->findPendingFromPage = 0;
    win->findCountValid = false;
    win->findCountPartial = false;
    str::FreePtr(&win->findCountText);
    win->findCountMatchCase = false;
    win->findCountMatchWholeWord = false;
    win->findCountPositions.Reset();
    win->findCountEngine = nullptr;
    win->findCountPageLimit = 0;
    win->findCountStartPage = 1;
    win->findCountContinuationPage = 0;
    win->findCountTextCacheGeneration = 0;
    win->findCountHasSnippets = false;
    win->findCountLatestFound = 0;
    win->findStatusAnimating = false;
    win->findStatusDotPhase = 0;
    win->findStatusCurrentIndex = 0;
    InterlockedExchange(&win->findCountAnimTaskPending, 0);
    ClearFindMatches(win);
    ClearFindSearchProgressCb(win);
    RemoveNotificationsForGroup(win->hwndCanvas, kNotifFindProgress);
    // ctrl still points at the tab being left when called from SelectionChanging
    // or early LoadModelIntoTab; clear its highlights before switching documents
    if (DisplayModel* dm = win->AsFixed()) {
        if (dm->textSearch) {
            dm->textSearch->Clear();
        }
    }
    ScheduleRepaint(win, 0);
}

bool FindFlushPendingSearch(MainWindow* win) {
    FindBarResyncActiveEdit(win);
    if (!EnsureDocumentSearchReady(win)) {
        return false;
    }
    if (HasStaleFindWorkers(win)) {
        AbortActiveFindWorkers(win);
    }
    if (!HasFindText(win)) {
        return false;
    }
    if (win->findEnterPending) {
        // Incremental counting already found navigable rows. The query is no
        // longer pending in the user's sense: let Enter move through that live
        // list instead of starting a second FindThread and waiting for the
        // full-document count to finish.
        if (win->findMatches.size() > 0) {
            win->findEnterPending = false;
            return false;
        }
        // The current query is already being scanned. Keep the UI responsive;
        // another Enter can navigate as soon as the first row is streamed.
        if (win->findCountThread) {
            return true;
        }
        win->findEnterPending = false;
        StartIncrementalFind(win);
        return true;
    }
    // A full-document count can still be running here. That does not mean the
    // query changed: an unchanged query must advance to the next result.
    return false;
}

static bool TryNavigateCachedFindMatch(MainWindow* win, TextSearch::Direction direction);

void FindNext(MainWindow* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    if (HasStaleFindWorkers(win)) {
        AbortActiveFindWorkers(win);
    }
    if (HasFindText(win)) {
        if (TryNavigateCachedFindMatch(win, TextSearch::Direction::Forward)) {
            return;
        }
        // The count worker owns text extraction until it finishes. Floating
        // results navigate through their streamed list; the compact bar waits
        // rather than starting a second, unsafe text-search worker.
        if (win->findCountThread) {
            return;
        }
        FindTextOnThread(win, TextSearch::Direction::Forward);
    }
}

void FindPrev(MainWindow* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    if (HasStaleFindWorkers(win)) {
        AbortActiveFindWorkers(win);
    }
    if (HasFindText(win)) {
        if (TryNavigateCachedFindMatch(win, TextSearch::Direction::Backward)) {
            return;
        }
        if (win->findCountThread) {
            return;
        }
        FindTextOnThread(win, TextSearch::Direction::Backward);
    }
}

void FindToggleMatchCase(MainWindow* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    win->findMatchCase = !win->findMatchCase;
    win->AsFixed()->textSearch->SetMatchCase(win->findMatchCase);
    win->findCountValid = false;
    win->findCountPartial = false;
    FindBarSetMatchCaseChecked(win, win->findMatchCase);
    if (HasFindText(win) && EnsureDocumentSearchReady(win)) {
        FindTextOnThread(win, TextSearch::Direction::Forward);
    }
}

void FindToggleMatchWholeWord(MainWindow* win) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    win->findMatchWholeWord = !win->findMatchWholeWord;
    win->AsFixed()->textSearch->SetMatchWholeWord(win->findMatchWholeWord);
    win->findCountValid = false;
    win->findCountPartial = false;
    FindBarSetMatchWholeWordChecked(win, win->findMatchWholeWord);
    if (win->hwndFindEdit) {
        Edit_SetModify(win->hwndFindEdit, TRUE);
    }
    // re-run the search with the new whole-word setting
    if (HasFindText(win) && EnsureDocumentSearchReady(win)) {
        FindTextOnThread(win, TextSearch::Direction::Forward);
    }
}

void FindSelection(MainWindow* win, TextSearch::Direction direction) {
    if (!win->IsDocLoaded() || !NeedsFindUI(win)) {
        return;
    }
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!win->CurrentTab()->selectionOnPage || 0 == dm->textSelection->result.len) {
        return;
    }

    AutoFreeWStr selection(dm->textSelection->ExtractText(" "));
    str::NormalizeWSInPlace(selection);
    if (str::IsEmpty(selection.Get())) {
        return;
    }

    TempStr s = ToUtf8Temp(selection);
    HwndSetText(win->hwndFindEdit, s);
    AbortFinding(win, false);
    Edit_SetModify(win->hwndFindEdit, FALSE);
    dm->textSearch->SetLastResult(dm->textSelection);

    FindTextOnThread(win, direction);
}

static void ShowSearchResult(MainWindow* win, TextSel* result, bool addNavPt) {
    ReportIf(0 == result->len || !result->pages || !result->rects);
    if (0 == result->len || !result->pages || !result->rects) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    if (addNavPt || !dm->PageShown(result->pages[0]) ||
        (dm->GetZoomVirtual() == kZoomFitPage || dm->GetZoomVirtual() == kZoomFitContent)) {
        win->ctrl->GoToPage(result->pages[0], addNavPt);
    }

    // Find never changes the text selection: all matches (including the active
    // one) are highlighted independently by PaintAllFindMatches, so the user's
    // selection highlight is separate and survives searching (issue #5737).
    dm->ShowResultRectToScreen(result);
    ScheduleRepaint(win, 0);
}

void ClearSearchResult(MainWindow* win) {
    // clear only the find-match highlights, never the user's text selection:
    // find and selection highlights are tracked independently (issue #5737)
    ClearFindMatches(win); // also invalidates the find-match paint cache
    ScheduleRepaint(win, 0);
}

struct FindThreadData {
    MainWindow* win = nullptr;
    TextSearch::Direction direction = TextSearch::Direction::Forward;
    bool wasModified = false;
    AutoFreeWStr text;
    HANDLE thread = nullptr;
    LONG epochAtStart = 0;

    FindThreadData(MainWindow* win, TextSearch::Direction direction, const char* text, bool wasModified) {
        this->win = win;
        this->direction = direction;
        this->text = ToWStr(text);
        this->wasModified = wasModified;
        this->epochAtStart = win->findCountEpoch;
    }
    ~FindThreadData() { CloseHandle(thread); }

    void ShowUI() {
        SetToolbarButtonEnableState(win, CmdFindPrev, false);
        SetToolbarButtonEnableState(win, CmdFindNext, false);
        SetToolbarButtonEnableState(win, CmdFindToggleMatchCase, false);
        SetToolbarButtonEnableState(win, CmdFindToggleMatchWholeWord, false);
        StartFindStatusAnimation(win);
    }

    void HideUI(bool success, bool loopedAround) const {
        SetToolbarButtonEnableState(win, CmdFindPrev, true);
        SetToolbarButtonEnableState(win, CmdFindNext, true);
        SetToolbarButtonEnableState(win, CmdFindToggleMatchCase, true);
        SetToolbarButtonEnableState(win, CmdFindToggleMatchWholeWord, true);

        if (!success && !loopedAround) {
            // i.e. canceled
            StopFindStatusAnimation(win);
            FindBarSetStatus(win, "");
        } else if (!success && loopedAround) {
            StopFindStatusAnimation(win);
            // final "0 / 0" is set by FindEndTask (with tooltip and completion flash)
        }
        // else: a match was found; the "n / m" counter (set by UpdateMatchCount
        // after this) is the only feedback - no beep on wrap-around
    }

    bool WasCanceled() { return !IsMainWindowValid(win) || win->findCancelled; }
};

static void FreeMatchSnippets(Vec<FindMatch>* matches);

static void ClearFindSearchProgressCb(MainWindow* win) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    if (dm && dm->textSearch) {
        dm->textSearch->progressCb = {};
    }
}

struct FindEndTaskData {
    MainWindow* win = nullptr;
    FindThreadData* ftd = nullptr;
    TextSel* textSel = nullptr;
    bool wasModifiedCanceled = false;
    bool loopedAround = false;
    FindEndTaskData() = default;
    ~FindEndTaskData() {
        delete ftd;
        ftd = nullptr;
    }
};

// ---- find bar "n / m" match counter ----------------------------------------
//
// The find thread locates the first match quickly. Full-document counting (for
// the n/m counter, floating results list, and all-match painting) runs on a
// single background count thread via RunDocumentMatchScan.

static u64 MatchKey(int page, int offset) {
    return ((u64)(u32)page << 32) | (u32)offset;
}

static int CmpMatchKey(const void* a, const void* b) {
    u64 ka = *(const u64*)a;
    u64 kb = *(const u64*)b;
    if (ka < kb) {
        return -1;
    }
    if (ka > kb) {
        return 1;
    }
    return 0;
}

static int CmpFindMatch(const void* a, const void* b) {
    const FindMatch* ma = (const FindMatch*)a;
    const FindMatch* mb = (const FindMatch*)b;
    u64 ka = MatchKey(ma->startPage, ma->startGlyph);
    u64 kb = MatchKey(mb->startPage, mb->startGlyph);
    if (ka < kb) {
        return -1;
    }
    if (ka > kb) {
        return 1;
    }
    return 0;
}

static void SortMatchesDocumentOrder(Vec<u64>& positions, Vec<FindMatch>* matches) {
    if (positions.size() > 1) {
        positions.Sort(CmpMatchKey);
    }
    if (matches && matches->size() > 1) {
        matches->Sort(CmpFindMatch);
    }
}

// first index with key >= MatchKey(startPage, 0), or -1 if none (positions sorted)
static int FirstSortedIndexAtOrAfterPage(const Vec<u64>& pos, int startPage) {
    int n = (int)pos.size();
    if (n == 0) {
        return -1;
    }
    u64 key = MatchKey(startPage, 0);
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (pos[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < n ? lo : -1;
}

// 1-based document-order index of `key`, or 0 if not found.
// positions are sorted by (page, glyph) after the count finishes.
static int MatchIndexInCache(MainWindow* win, u64 key) {
    Vec<u64>& pos = win->findCountPositions;
    int n = (int)pos.size();
    if (n == 0) {
        return 0;
    }
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (pos[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < n && pos[lo] == key) {
        return lo + 1;
    }
    return 0;
}

// list index of the match the document is currently on (so prev/next can step
// through the cached results in document order), or -1 if it isn't in the cache
static int FindCurrentMatchIndex(MainWindow* win) {
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSearch) {
        return -1;
    }
    int page = dm->textSearch->startPage;
    int glyph = dm->textSearch->startGlyph;
    int n = (int)win->findMatches.size();
    for (int i = 0; i < n; i++) {
        const FindMatch& fm = win->findMatches[i];
        if (fm.startPage == page && fm.startGlyph == glyph) {
            return i;
        }
    }
    if (!win->findCountValid) {
        return -1;
    }
    int idx = MatchIndexInCache(win, MatchKey(page, glyph));
    return idx > 0 ? idx - 1 : -1;
}

static bool CountCacheIsComplete(MainWindow* win) {
    return win && win->findCountValid && !win->findCountPartial;
}

// update the find bar with "n / m" from the cache (or partial results while
// the count thread is still running) and the current match
static void ShowMatchCount(MainWindow* win) {
    if (!win) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    int n = 0;
    bool complete = CountCacheIsComplete(win);
    if (complete && dm && dm->textSearch) {
        u64 key = MatchKey(dm->textSearch->startPage, dm->textSearch->startGlyph);
        n = MatchIndexInCache(win, key);
    }
    if (complete && n < 1) {
        // The results list can already have the new query's current row while
        // dm->textSearch still describes the previous query. Use that row until
        // navigation synchronizes the document search state.
        n = FindWindowCurrentSelectionIndex(win);
    }

    if (complete) {
        StopFindStatusAnimation(win);
        int total = (int)win->findCountPositions.size();
        win->findStatusCurrentIndex = n;
        if (n >= 1) {
            TempStr s = str::FormatTemp("%d / %d", n, total);
            FindBarSetStatus(win, s);
        } else {
            FindBarSetStatus(win, str::FormatTemp("... / %d", total));
        }
        return;
    }

    // wrap-order index is not a document-order n; keep n unknown until sort
    int total = win->findCountValid ? std::max((int)win->findCountPositions.size(), FindKnownMatchCount(win))
                                    : FindKnownMatchCount(win);
    bool countActive =
        win->findCountThread || win->findCountLatestFound > 0 || win->findMatches.size() > 0 || win->findCountValid;
    if (total <= 0 && !countActive) {
        return;
    }
    win->findStatusCurrentIndex = 0;
    FindBarSetStatus(win, FindStatusAnimTextTemp(0, total, win->findStatusDotPhase));
}

static bool WantFindSnippets() {
    // The compact and expanded forms are the same find window. Keep the match
    // model populated while compact too, so changing form cannot change Enter,
    // Next/Previous, or streaming-search behavior.
    return true;
}

static void InstallCountCache(MainWindow* win, WCHAR* text, bool matchCase, bool matchWholeWord, void* engine,
                              Vec<u64>& positions, Vec<FindMatch>* matches, bool hasSnippets, int pageLimit,
                              int startPage, bool partial = false, int continuationPage = 0,
                              u32 textCacheGeneration = 0) {
    SortMatchesDocumentOrder(positions, matches);
    str::FreePtr(&win->findCountText);
    win->findCountText = text;
    win->findCountMatchCase = matchCase;
    win->findCountMatchWholeWord = matchWholeWord;
    win->findCountEngine = engine;
    win->findCountPositions = positions;
    win->findCountPageLimit = pageLimit;
    win->findCountStartPage = startPage;
    win->findCountContinuationPage = continuationPage;
    win->findCountTextCacheGeneration = textCacheGeneration;
    win->findCountValid = true;
    win->findCountPartial = partial;
    if (matches) {
        ClearFindMatches(win);
        win->findMatches = *matches;
        for (int i = 0; i < (int)matches->size(); i++) {
            (*matches)[i].snippet = nullptr;
        }
        win->findCountHasSnippets = hasSnippets;
        if (hasSnippets) {
            FindWindowRefreshResults(win, false);
        }
    }
    ShowMatchCount(win);
    if (!partial) {
        FindBarBeginStatusCompleteFlash(win);
    }
    ScheduleRepaint(win, 0);
}

static void InstallEmptyCountCache(MainWindow* win, const WCHAR* text) {
    DisplayModel* dm = win->AsFixed();
    void* engine = dm ? (void*)dm->GetEngine() : nullptr;
    WCHAR* owned = str::Dup(text);
    Vec<u64> empty;
    InstallCountCache(win, owned, win->findMatchCase, win->findMatchWholeWord, engine, empty, nullptr, false, 0, 1);
}

static void UpdateMatchCount(MainWindow* win, const WCHAR* text);
static bool NavigateFirstMatchFromPage(MainWindow* win, int startPage);

// cap on how many per-match snippets we build for the floating results list
// (matches beyond this still count toward "n / m", just aren't listed)
constexpr int kMaxFindResults = 5000;

void ClearFindMatches(MainWindow* win) {
    int n = (int)win->findMatches.size();
    for (int i = 0; i < n; i++) {
        str::Free(win->findMatches[i].snippet);
    }
    win->findMatches.Reset();
    win->findCountHasSnippets = false;
}

// how many glyphs to show in a floating find result line, scaled to the results
// list width when the find window is visible (fallback for early search passes)
static int FindSnippetMaxGlyphs(MainWindow* win) {
    if (win) {
        return FindWindowSnippetGlyphBudget(win);
    }
    for (size_t i = 0; i < gWindows.size(); i++) {
        MainWindow* w = gWindows.at(i);
        if (IsFindWindowVisible(w)) {
            return FindWindowSnippetGlyphBudget(w);
        }
    }
    return 72;
}

struct SnippetPageContext {
    TextSearch* ts = nullptr;
    int pageNo = 0;
    const char* text = nullptr;
    int byteLen = 0;
    int textLen = 0;
};

static int SnippetCodepointAt(const SnippetPageContext& ctx, int idx) {
    if (!ctx.text || idx < 0) {
        return 0;
    }
    return ctx.ts->PageCodepointAt(ctx.pageNo, idx);
}

static bool IsSnippetSentenceEnd(int cp) {
    return cp == '.' || cp == '!' || cp == '?' || cp == 0x3002 /* 。 */ || cp == 0xFF01 /* ！ */ ||
           cp == 0xFF1F /* ？ */;
}

static bool IsSnippetNewlineCp(int cp) {
    return cp == '\n' || cp == '\r';
}

// Skip a run of \r and \n (treat CRLF as one soft wrap, not two breaks).
static int SnippetNewlineRunEnd(const SnippetPageContext& ctx, int idx) {
    int i = idx;
    while (i < ctx.textLen && IsSnippetNewlineCp(SnippetCodepointAt(ctx, i))) {
        i++;
    }
    return i;
}

static bool IsSnippetParagraphBreakAt(const SnippetPageContext& ctx, int idx) {
    int cp = SnippetCodepointAt(ctx, idx);
    if (!IsSnippetNewlineCp(cp)) {
        return false;
    }
    // Soft wraps like "structure\r\nis" are not paragraph breaks.
    if (idx > 0) {
        int prev = SnippetCodepointAt(ctx, idx - 1);
        if (!IsSnippetNewlineCp(prev) && IsSnippetSentenceEnd(prev)) {
            return true;
        }
    }
    int runEnd = SnippetNewlineRunEnd(ctx, idx);
    for (int j = runEnd; j < ctx.textLen; j++) {
        int c = SnippetCodepointAt(ctx, j);
        if (c == ' ' || c == '\t') {
            continue;
        }
        if (IsSnippetNewlineCp(c)) {
            return true;
        }
        break;
    }
    return false;
}

static int SnippetPosAfterSegmentBreak(const SnippetPageContext& ctx, int idx) {
    int cp = SnippetCodepointAt(ctx, idx);
    if (IsSnippetNewlineCp(cp)) {
        return SnippetNewlineRunEnd(ctx, idx);
    }
    return idx + 1;
}

static bool IsSnippetSegmentBreakAt(const SnippetPageContext& ctx, int idx) {
    int cp = SnippetCodepointAt(ctx, idx);
    if (IsSnippetSentenceEnd(cp)) {
        return true;
    }
    if (IsSnippetNewlineCp(cp)) {
        return IsSnippetParagraphBreakAt(ctx, idx);
    }
    return false;
}

static bool IsSnippetClauseBreak(int cp) {
    return cp == ';' || cp == 0xFF1B /* ； */ || cp == ':' || cp == 0xFF1A /* ： */;
}

static bool IsCleanSnippetBoundaryBefore(const SnippetPageContext& ctx, int from) {
    if (from <= 0) {
        return true;
    }
    int i = from - 1;
    if (IsSnippetSegmentBreakAt(ctx, i)) {
        return true;
    }
    return IsSnippetClauseBreak(SnippetCodepointAt(ctx, i));
}

// Pick a snippet start inside [earliest, mStart]: prefer just after the nearest
// sentence end, so lines begin on a complete clause like "这倒提醒了我：…".
static int RefineSnippetStart(const SnippetPageContext& ctx, int mStart, int earliest) {
    earliest = std::max(0, earliest);
    for (int i = mStart - 1; i >= earliest; i--) {
        if (IsSnippetSegmentBreakAt(ctx, i)) {
            return SnippetPosAfterSegmentBreak(ctx, i);
        }
    }
    // Short lead-in before the match (e.g. "这倒提醒了我：") is better than a hard cut.
    constexpr int kMaxClauseLeadGlyphs = 20;
    for (int i = mStart - 1; i >= earliest; i--) {
        if (!IsSnippetClauseBreak(SnippetCodepointAt(ctx, i))) {
            continue;
        }
        int start = i + 1;
        if (mStart - start <= kMaxClauseLeadGlyphs) {
            return start;
        }
    }
    return earliest;
}

// Pick a snippet end inside [mEnd, latest]: prefer the next sentence boundary.
static int RefineSnippetEnd(const SnippetPageContext& ctx, int mEnd, int latest) {
    for (int i = mEnd; i < latest; i++) {
        if (IsSnippetSegmentBreakAt(ctx, i)) {
            return SnippetPosAfterSegmentBreak(ctx, i);
        }
    }
    for (int i = mEnd; i < latest; i++) {
        if (IsSnippetClauseBreak(SnippetCodepointAt(ctx, i))) {
            return i + 1;
        }
    }
    return latest;
}

// build a one-line "...context match context..." snippet (UTF-8) around a match
static char* BuildSnippet(TextSearch* ts, const FindMatch& m, int maxSnippetGlyphs) {
    if (!ts || maxSnippetGlyphs <= 0) {
        return nullptr;
    }
    SnippetPageContext ctx;
    ctx.ts = ts;
    ctx.pageNo = m.startPage;
    ctx.text = ts->PreparePageOffsetMap(m.startPage, &ctx.byteLen, &ctx.textLen);
    const char* pageText = ctx.text;
    int textByteLen = ctx.byteLen;
    if (!pageText || textByteLen <= 0) {
        return nullptr;
    }
    int textLen = ctx.textLen;
    int mStart = ts->GlyphToCodepoint(m.startPage, m.startGlyph);
    int mEnd = textLen;
    if (m.endPage == m.startPage) {
        mEnd = ts->GlyphToCodepoint(m.endPage, m.endGlyph);
    }
    mStart = limitValue(mStart, 0, textLen);
    mEnd = limitValue(mEnd, mStart, textLen);
    const int kMaxSnippetGlyphs = maxSnippetGlyphs;
    int matchLen = std::max(0, mEnd - mStart);

    // Prefer a clean sentence/clause start even if it uses more leading room.
    // Leading "..." feels wrong; trailing "..." is fine.
    constexpr int kMaxLookbackGlyphs = 96;
    int lookbackEarliest = std::max(0, mStart - kMaxLookbackGlyphs);
    int from = RefineSnippetStart(ctx, mStart, lookbackEarliest);
    bool cleanStart = IsCleanSnippetBoundaryBefore(ctx, from);

    // Find the natural sentence end first (may be past a soft wrap), then apply budget.
    int naturalTo = RefineSnippetEnd(ctx, mEnd, textLen);
    int maxTo = std::min(textLen, from + kMaxSnippetGlyphs);
    int to = std::min(naturalTo, maxTo);
    if (to - from > kMaxSnippetGlyphs) {
        to = from + kMaxSnippetGlyphs;
    }
    if (to < mEnd) {
        to = mEnd;
    }

    // Last resort: match does not fit after a clean start — shift start and accept leading "...".
    if (to - from > kMaxSnippetGlyphs) {
        int forcedEarliest = std::max(0, mEnd + matchLen - kMaxSnippetGlyphs);
        from = RefineSnippetStart(ctx, mStart, forcedEarliest);
        cleanStart = IsCleanSnippetBoundaryBefore(ctx, from);
        maxTo = std::min(textLen, from + kMaxSnippetGlyphs);
        naturalTo = RefineSnippetEnd(ctx, mEnd, textLen);
        to = std::max(mEnd, std::min(naturalTo, maxTo));
        if (to - from > kMaxSnippetGlyphs) {
            to = from + kMaxSnippetGlyphs;
        }
    }

    int fromByte = ts->PageCodepointByteOffset(m.startPage, from);
    int toByte = ts->PageCodepointByteOffset(m.startPage, to);
    if (fromByte < 0 || toByte <= fromByte || toByte > textByteLen) {
        return nullptr;
    }
    char* sub = str::Dup(pageText + fromByte, (size_t)(toByte - fromByte));
    if (!sub) {
        return nullptr;
    }
    str::NormalizeWSInPlace(sub);
    bool showLeadingEllipsis = from > 0 && !cleanStart;
    TempStr full = str::FormatTemp("%s%s%s", showLeadingEllipsis ? "..." : "", sub, to < textLen ? "..." : "");
    str::Free(sub);
    return str::Dup(full);
}

static char* BuildSnippet(MainWindow* win, TextSearch* ts, const FindMatch& m) {
    return BuildSnippet(ts, m, FindSnippetMaxGlyphs(win));
}

static void BuildSnippetsForMatchList(MainWindow* win, EngineBase* engine, Vec<FindMatch>* matches) {
    if (!win || !engine || !matches) {
        return;
    }
    TextSearch ts(engine);
    for (int i = 0; i < (int)matches->size(); i++) {
        FindMatch& fm = (*matches)[i];
        if (!fm.snippet) {
            fm.snippet = BuildSnippet(win, &ts, fm);
        }
    }
}

void RebuildFindMatchSnippets(MainWindow* win) {
    if (!win || win->findMatches.size() == 0) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    TextSearch ts(engine);
    for (size_t i = 0; i < win->findMatches.size(); i++) {
        FindMatch& fm = win->findMatches[i];
        char* s = BuildSnippet(win, &ts, fm);
        str::ReplacePtr(&fm.snippet, s);
    }
    win->findCountHasSnippets = true;
    FindWindowRefreshResults(win, false);
}

static Vec<FindMatch>* BuildFindMatchesFromPositions(MainWindow* win, EngineBase* engine, const WCHAR* text,
                                                     bool matchCase, bool matchWholeWord, const Vec<u64>& positions,
                                                     bool wantSnippets);

static void EnsureFindMatchList(MainWindow* win) {
    if (!win || !WantFindSnippets() || win->findCountHasSnippets) {
        return;
    }
    if (!win->findCountValid || win->findCountPositions.size() == 0 || !win->findCountText) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || win->findCountEngine != engine) {
        return;
    }
    TempWStr currentText = win->hwndFindEdit ? HwndGetTextWTemp(win->hwndFindEdit) : nullptr;
    if (!currentText || !str::Eq(currentText, win->findCountText)) {
        return;
    }
    if (win->findCountMatchCase != win->findMatchCase || win->findCountMatchWholeWord != win->findMatchWholeWord) {
        return;
    }
    if (win->findMatches.size() > 0) {
        RebuildFindMatchSnippets(win);
        return;
    }
    Vec<FindMatch>* matches =
        BuildFindMatchesFromPositions(win, engine, win->findCountText, win->findCountMatchCase,
                                      win->findCountMatchWholeWord, win->findCountPositions, true);
    if (!matches) {
        return;
    }
    ClearFindMatches(win);
    win->findMatches = *matches;
    for (int i = 0; i < (int)matches->size(); i++) {
        (*matches)[i].snippet = nullptr;
    }
    win->findCountHasSnippets = true;
    delete matches;
    FindWindowRefreshResults(win, false);
}

void SyncFindResultsList(MainWindow* win) {
    EnsureFindMatchList(win);
}

static Vec<FindMatch>* BuildFindMatchesFromPositions(MainWindow* win, EngineBase* engine, const WCHAR* text,
                                                     bool matchCase, bool matchWholeWord, const Vec<u64>& positions,
                                                     bool wantSnippets) {
    auto* matches = new Vec<FindMatch>();
    if (positions.size() == 0) {
        return matches;
    }
    TextSearch ts(engine);
    ts.SetMatchCase(matchCase);
    ts.SetMatchWholeWord(matchWholeWord);
    ts.SetText(text);
    int limit = (int)positions.size();
    if (limit > kMaxFindResults) {
        limit = kMaxFindResults;
    }
    for (int i = 0; i < limit; i++) {
        u64 key = positions[i];
        FindMatch fm;
        fm.startPage = (int)(key >> 32);
        fm.startGlyph = (int)(key & 0xffffffff);
        fm.endPage = fm.startPage;
        fm.endGlyph = fm.startGlyph;
        if (wantSnippets) {
            ts.findPage = fm.startPage;
            bool abortSearch = false;
            if (ts.LoadPageText(fm.startPage, nullptr, &abortSearch) && !abortSearch && ts.pageText) {
                int startOffset = ts.GlyphToCodepoint(fm.startPage, fm.startGlyph);
                TextSearch::PageAndOffset end = ts.MatchEnd(startOffset);
                if (end.page > 0) {
                    fm.endPage = end.page;
                    fm.endGlyph = ts.CodepointToGlyph(end.page, end.offset);
                }
            }
            fm.snippet = BuildSnippet(win, &ts, fm);
        }
        matches->Append(fm);
    }
    return matches;
}

struct CountThreadData {
    MainWindow* win = nullptr;
    EngineBase* engine = nullptr; // AddRef'd by the caller, released by the thread
    WCHAR* text = nullptr;
    bool matchCase = false;
    bool matchWholeWord = false;
    bool wantMatchList = false; // build findMatches (for all-match painting or the results list)
    bool wantSnippets = false;  // build per-match snippet strings for the results list
    int startPage = 1;          // scan from here (the current page), wrapping around
    int resumeFromPage = 0;     // continuation scans only this newly published tail
    int scannedPageLimit = 0;
    int continuationPage = 0;
    u32 textCacheGeneration = 0;
    DWORD lastAnimPostMs = 0;
    DWORD lastResultsPostMs = 0;
    int streamedMatchCount = 0;
    int snippetMaxGlyphs = 72;
    LONG epoch = 0;
    HANDLE thread = nullptr;
    Vec<u64>* seedPositions = nullptr;
    Vec<FindMatch>* seedMatches = nullptr;

    CountThreadData(MainWindow* win, EngineBase* engine, const WCHAR* text, bool matchCase, bool matchWholeWord,
                    bool wantMatchList, bool wantSnippets, int startPage, LONG epoch) {
        this->win = win;
        this->engine = engine;
        this->text = str::Dup(text);
        this->matchCase = matchCase;
        this->matchWholeWord = matchWholeWord;
        this->wantMatchList = wantMatchList;
        this->wantSnippets = wantSnippets;
        this->startPage = startPage;
        this->textCacheGeneration = engine->GetTextCacheGeneration();
        this->epoch = epoch;
    }
    ~CountThreadData();
};

static void FreeMatchSnippets(Vec<FindMatch>* matches) {
    if (!matches) {
        return;
    }
    for (int i = 0; i < (int)matches->size(); i++) {
        str::Free((*matches)[i].snippet);
    }
}

static Vec<FindMatch>* CloneFindMatches(const Vec<FindMatch>& source) {
    auto result = new Vec<FindMatch>();
    for (int i = 0; i < (int)source.size(); i++) {
        FindMatch fm = source[i];
        fm.snippet = str::Dup(fm.snippet);
        result->Append(fm);
    }
    return result;
}

static void RemoveMatchesStartingAtOrAfter(Vec<u64>* positions, Vec<FindMatch>* matches, int pageNo) {
    if (pageNo <= 0) {
        return;
    }
    if (positions) {
        for (int i = (int)positions->size() - 1; i >= 0; i--) {
            if ((int)((*positions)[i] >> 32) >= pageNo) {
                positions->RemoveAt(i);
            }
        }
    }
    if (matches) {
        for (int i = (int)matches->size() - 1; i >= 0; i--) {
            if ((*matches)[i].startPage >= pageNo) {
                str::Free((*matches)[i].snippet);
                matches->RemoveAt(i);
            }
        }
    }
}

CountThreadData::~CountThreadData() {
    str::Free(text);
    CloseHandle(thread);
    delete seedPositions;
    FreeMatchSnippets(seedMatches);
    delete seedMatches;
}

struct CountResultsTaskData {
    MainWindow* win = nullptr;
    LONG epoch = 0;
    bool reset = false;
    Vec<FindMatch>* matches = nullptr;
    ~CountResultsTaskData() {
        FreeMatchSnippets(matches);
        delete matches;
    }
};

static void CountResultsTask(CountResultsTaskData* d) {
    AutoDelete delData(d);
    MainWindow* win = d->win;
    if (!IsMainWindowValid(win) || win->findCountEpoch != d->epoch || !IsFindWindowVisible(win)) {
        return;
    }
    if (d->reset) {
        ClearFindMatches(win);
    }
    for (int i = 0; i < (int)d->matches->size(); i++) {
        FindMatch fm = (*d->matches)[i];
        win->findMatches.Append(fm);
        (*d->matches)[i].snippet = nullptr;
    }
    win->findCountHasSnippets = true;
    FindWindowRefreshResults(win, false);
    ShowMatchCount(win);
}

static void MaybePostCountResults(CountThreadData* d, Vec<FindMatch>* matches, bool force) {
    if (!d->wantSnippets || !matches || d->streamedMatchCount >= (int)matches->size()) {
        return;
    }
    DWORD now = GetTickCount();
    if (!force && d->streamedMatchCount > 0 && now - d->lastResultsPostMs < 75) {
        return;
    }
    auto partial = new Vec<FindMatch>();
    for (int i = d->streamedMatchCount; i < (int)matches->size(); i++) {
        FindMatch fm = (*matches)[i];
        fm.snippet = str::Dup(fm.snippet);
        partial->Append(fm);
    }
    auto data = new CountResultsTaskData;
    data->win = d->win;
    data->epoch = d->epoch;
    data->reset = d->streamedMatchCount == 0;
    data->matches = partial;
    d->streamedMatchCount = (int)matches->size();
    d->lastResultsPostMs = now;
    uitask::Post(MkFunc0<CountResultsTaskData>(CountResultsTask, data), "TaskFindCountResults");
}

struct CountEndTaskData {
    MainWindow* win = nullptr;
    CountThreadData* ctd = nullptr;
    Vec<u64>* positions = nullptr;
    Vec<FindMatch>* matches = nullptr; // nullptr unless snippets were requested
    ~CountEndTaskData() {
        delete ctd;
        delete positions;
        FreeMatchSnippets(matches); // frees any snippets not transferred to win
        delete matches;
    }
};

static bool TryInstallCountFromSession(MainWindow* win, DisplayModel* dm, EngineBase* engine, const WCHAR* text,
                                       bool matchCase, bool matchWholeWord) {
    SearchSessionEntry entry;
    if (!dm->searchSession.Lookup(engine, text, matchCase, matchWholeWord, dm->PageCount(), true, &entry)) {
        return false;
    }
    if (!entry.positions) {
        return false;
    }
    Vec<u64> positions = *entry.positions;
    SortMatchesDocumentOrder(positions, nullptr);
    WCHAR* owned = str::Dup(text);
    bool wantSnippets = WantFindSnippets();
    Vec<FindMatch>* matches =
        BuildFindMatchesFromPositions(win, engine, text, matchCase, matchWholeWord, positions, wantSnippets);
    InstallCountCache(win, owned, matchCase, matchWholeWord, engine, positions, matches, wantSnippets && matches,
                      entry.pageLimit, entry.scanStartPage, entry.partial, entry.continuationPage,
                      entry.textCacheGeneration);
    if (matches) {
        FreeMatchSnippets(matches);
        delete matches;
    }
    return true;
}

static void CountEndTask(CountEndTaskData* d) {
    AutoDelete delData(d);
    MainWindow* win = d->win;
    CountThreadData* ctd = d->ctd;
    if (!IsMainWindowValid(win)) {
        return;
    }
    if (ctd->thread) {
        if (win->findCountThread != ctd->thread) {
            return; // superseded (shouldn't happen with the single-worker model)
        }
        win->findCountThread = nullptr;
    }
    if (win->findCountEpoch != ctd->epoch) {
        // The scan was canceled because the edit changed. Its results are stale,
        // but the latest coalesced term must still start now that the single
        // worker slot is free. A tab switch/close clears pendingText first.
        if (win->findCountPendingText) {
            WCHAR* pending = win->findCountPendingText;
            win->findCountPendingText = nullptr;
            StartFindCount(win, pending, win->findCountPendingMatchCase, win->findCountPendingMatchWholeWord);
            str::Free(pending);
        }
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (engine && ctd->textCacheGeneration != engine->GetTextCacheGeneration()) {
        // Text or layout changed while this worker was extracting pages. Its
        // mixed-generation positions cannot seed a continuation.
        win->findCountValid = false;
        win->findCountPartial = false;
        StartFindCount(win, ctd->text, ctd->matchCase, ctd->matchWholeWord);
        return;
    }
    int pageLimit = ctd->scannedPageLimit;
    logf("search: end epoch=%ld scanned=%d matches=%d enginePages=%d modelPages=%d loading=%d continuation=%d\n",
         (long)ctd->epoch, pageLimit, d->positions ? (int)d->positions->size() : 0, engine ? engine->PageCount() : 0,
         dm ? dm->PageCount() : 0, engine ? EngineIsProgressiveEbookLoading(engine) : 0, ctd->continuationPage);
    str::FreePtr(&win->findCountText);
    WCHAR* text = ctd->text;
    ctd->text = nullptr;
    bool partial = dm && engine && (EngineIsProgressiveEbookLoading(engine) || dm->PageCount() > pageLimit);
    SortMatchesDocumentOrder(*d->positions, d->matches);
    if (dm && engine && text) {
        dm->searchSession.Store(engine, text, ctd->matchCase, ctd->matchWholeWord, ctd->startPage, partial, pageLimit,
                                ctd->scannedPageLimit, ctd->continuationPage, *d->positions);
    }
    if (d->matches && ctd->wantSnippets && engine) {
        BuildSnippetsForMatchList(win, engine, d->matches);
    }
    InstallCountCache(win, text, ctd->matchCase, ctd->matchWholeWord, ctd->engine, *d->positions, d->matches,
                      ctd->wantSnippets, pageLimit, ctd->startPage, partial, ctd->continuationPage,
                      ctd->textCacheGeneration);
    if (d->matches) {
        for (int i = 0; i < (int)d->matches->size(); i++) {
            (*d->matches)[i].snippet = nullptr; // transferred to win->findMatches
        }
    }
    if (WantFindSnippets() && !win->findCountHasSnippets && win->findCountPositions.size() > 0) {
        EnsureFindMatchList(win);
    }
    int pendingListIdx = FindWindowPendingNavigationIndex(win);
    FindWindowApplyPendingNavigation(win);
    if (pendingListIdx <= 0 && win->findPendingFromPage > 0) {
        int fromPage = win->findPendingFromPage;
        win->findPendingFromPage = 0;
        NavigateFirstMatchFromPage(win, fromPage);
    }
    // a newer term arrived while we were scanning: run it now (no worker running)
    if (win->findCountPendingText) {
        WCHAR* pending = win->findCountPendingText;
        win->findCountPendingText = nullptr;
        StartFindCount(win, pending, win->findCountPendingMatchCase, win->findCountPendingMatchWholeWord);
        str::Free(pending);
        return;
    }
    // Pages may have been published while this fixed-range scan was running.
    // Continue only the unscanned tail; never restart the already searched prefix.
    if (partial && dm && dm->PageCount() > pageLimit && win->findCountText) {
        StartFindCount(win, win->findCountText, win->findCountMatchCase, win->findCountMatchWholeWord);
    }
}

static void CountProgress(CountThreadData* d, ProgressUpdateData* data) {
    if (data->wasCancelled) {
        *data->wasCancelled = d->win->findCountEpoch != d->epoch;
        return;
    }
    DWORD now = GetTickCount();
    if (data->current > 0 && (data->current == 1 || data->current == data->total || data->current % 256 == 0)) {
        logf("search: epoch=%ld page=%d/%d matches=%ld\n", (long)d->epoch, data->current, data->total,
             (long)d->win->findCountLatestFound);
    }
    if (now - d->lastAnimPostMs >= kFindStatusAnimateMs) {
        d->lastAnimPostMs = now;
        MaybePostFindCountAnimTask(d->win, d->epoch);
    }
}

static void ApplySearchPageLimit(TextSearch* ts, DisplayModel* dm, EngineBase* engine) {
    // Freeze each progressive pass at the UI model's published page frontier.
    // The engine may grow again while the worker runs; that tail is handled by
    // a later OnEbookPageCountChanged() continuation.
    if (dm && engine && EngineIsProgressiveEbookLoading(engine)) {
        ts->SetMaxPageCount(dm->PageCount());
    } else {
        ts->SetMaxPageCount(0);
    }
}

struct DocMatchScanNav {
    bool found = false;
    int startPage = 0;
    int startGlyph = 0;
    int endPage = 0;
    int endGlyph = 0;
};

struct DocMatchScanOptions {
    EngineBase* engine = nullptr;
    DisplayModel* dm = nullptr;
    MainWindow* win = nullptr;
    const WCHAR* text = nullptr;
    bool matchCase = false;
    bool matchWholeWord = false;
    int startPage = 1;
    int resumeFromPage = 0;
    bool wantMatchList = false;
    bool wantSnippets = false;
    CountThreadData* countData = nullptr;
    int snippetMaxGlyphs = 72;
    LONG countEpoch = 0;
    ProgressUpdateCb progressCb{};
    DocMatchScanNav* navOut = nullptr;
};

static void RunDocumentMatchScan(DocMatchScanOptions& opt, Vec<u64>* positions, Vec<FindMatch>* matches,
                                 TextSearch* existingTs = nullptr, int* pagesScannedOut = nullptr,
                                 int* continuationPageOut = nullptr) {
    TextSearch tsOwned(opt.engine);
    TextSearch* ts = existingTs ? existingTs : &tsOwned;
    if (!existingTs) {
        ApplySearchPageLimit(ts, opt.dm, opt.engine);
        ts->SetMatchCase(opt.matchCase);
        ts->SetMatchWholeWord(opt.matchWholeWord);
        ts->SetDirection(TextSearch::Direction::Forward);
        if (opt.progressCb.IsValid()) {
            ts->progressCb = opt.progressCb;
        }
        ts->SetText(opt.text);
        ts->SyncPageCount();
    } else {
        ApplySearchPageLimit(ts, opt.dm, opt.engine);
        ts->SyncPageCount();
        if (opt.progressCb.IsValid()) {
            ts->progressCb = opt.progressCb;
        }
    }

    MainWindow* win = opt.win;
    int nPages = ts->nPages;
    if (pagesScannedOut) {
        *pagesScannedOut = nPages;
    }
    if (nPages <= 0) {
        return;
    }

    auto recordMatch = [&](const TextSearch::MatchSpan& ms, int insertAt = -1, int matchInsertAt = -1) -> bool {
        if (opt.countEpoch && win && win->findCountEpoch != opt.countEpoch) {
            return false;
        }
        if (win && win->findCancelled) {
            return false;
        }
        u64 key = MatchKey(ms.startPage, ms.startGlyph);
        if (positions->size() > 0 && positions->Last() == key) {
            return true;
        }
        if (insertAt >= 0) {
            positions->InsertAt(insertAt, key);
        } else {
            positions->Append(key);
        }
        if (opt.navOut && !opt.navOut->found) {
            opt.navOut->found = true;
            opt.navOut->startPage = ms.startPage;
            opt.navOut->startGlyph = ms.startGlyph;
            opt.navOut->endPage = ms.endPage;
            opt.navOut->endGlyph = ms.endGlyph;
        }
        if (matches && (int)matches->size() < kMaxFindResults) {
            FindMatch fm;
            fm.startPage = ms.startPage;
            fm.startGlyph = ms.startGlyph;
            fm.endPage = ms.endPage;
            fm.endGlyph = ms.endGlyph;
            if (opt.wantSnippets) {
                fm.snippet = BuildSnippet(ts, fm, opt.snippetMaxGlyphs);
            }
            if (matchInsertAt >= 0 && matchInsertAt <= (int)matches->size()) {
                matches->InsertAt(matchInsertAt, fm);
            } else if (insertAt >= 0 && insertAt <= (int)matches->size()) {
                matches->InsertAt(insertAt, fm);
            } else {
                matches->Append(fm);
            }
        }
        int n = len(*positions);
        if (opt.win && opt.countEpoch) {
            opt.win->findCountLatestFound = n;
            if (n == 1) {
                MaybePostFindCountAnimTask(opt.win, opt.countEpoch);
            }
        }
        return true;
    };

    auto scanPage = [&](int pageNo) -> bool {
        if (WasCanceled(ts->progressCb)) {
            return false;
        }
        UpdateProgress(ts->progressCb, pageNo, nPages);
        if (ts->pagesToSkip[pageNo - 1]) {
            return true;
        }
        if (!ts->PageMightContainAnchor(pageNo)) {
            ts->pagesToSkip[pageNo - 1] = true;
            return true;
        }

        Vec<TextSearch::MatchSpan> spans;
        ts->CollectMatchesOnPage(pageNo, &spans, continuationPageOut);
        if (WasCanceled(ts->progressCb)) {
            return false;
        }
        if (spans.size() == 0) {
            ts->pagesToSkip[pageNo - 1] = true;
            return true;
        }

        for (size_t i = 0; i < spans.size(); i++) {
            if (!recordMatch(spans[i])) {
                return false;
            }
        }
        if (opt.countData) {
            MaybePostCountResults(opt.countData, matches, false);
        }
        return true;
    };

    if (opt.resumeFromPage > 0) {
        int first = std::max(1, opt.resumeFromPage);
        for (int pageNo = first; pageNo <= nPages; pageNo++) {
            if (!scanPage(pageNo)) {
                return;
            }
        }
        return;
    }

    int startPage = opt.startPage;
    if (startPage < 1) {
        startPage = 1;
    }
    if (startPage > nPages) {
        startPage = nPages;
    }

    for (int pageNo = startPage; pageNo <= nPages; pageNo++) {
        if (!scanPage(pageNo)) {
            return;
        }
    }
    if (startPage > 1) {
        for (int pageNo = 1; pageNo < startPage; pageNo++) {
            if (!scanPage(pageNo)) {
                return;
            }
        }
    }
}

static void SetupCountScanOptions(DocMatchScanOptions& opt, CountThreadData* d, MainWindow* win) {
    opt.engine = d->engine;
    opt.dm = win->AsFixed();
    opt.win = win;
    opt.text = d->text;
    opt.matchCase = d->matchCase;
    opt.matchWholeWord = d->matchWholeWord;
    opt.startPage = d->startPage;
    opt.resumeFromPage = d->resumeFromPage;
    opt.wantMatchList = d->wantMatchList;
    opt.wantSnippets = d->wantSnippets;
    opt.countData = d;
    opt.snippetMaxGlyphs = d->snippetMaxGlyphs;
    opt.countEpoch = d->epoch;
    opt.progressCb = MkFunc1<CountThreadData, ProgressUpdateData*>(CountProgress, d);
}

static void CountThread(CountThreadData* d) {
    MainWindow* win = d->win;
    EngineBase* engine = d->engine;

    Vec<u64>* positions = d->seedPositions ? d->seedPositions : new Vec<u64>();
    d->seedPositions = nullptr;
    Vec<FindMatch>* matches = d->seedMatches;
    d->seedMatches = nullptr;
    if (d->wantMatchList && !matches) {
        matches = new Vec<FindMatch>();
    }
    d->streamedMatchCount = matches ? (int)matches->size() : 0;
    d->lastResultsPostMs = GetTickCount();
    d->win->findCountLatestFound = (int)positions->size();
    DocMatchScanOptions opt;
    SetupCountScanOptions(opt, d, win);
    RunDocumentMatchScan(opt, positions, matches, nullptr, &d->scannedPageLimit, &d->continuationPage);
    MaybePostCountResults(d, matches, true);
    SafeEngineRelease(&engine);

    // wait for StartFindCount to record the thread handle (mirrors FindThread)
    while (!win->findCountThread) {
        Sleep(1);
    }

    auto data = new CountEndTaskData;
    data->win = win;
    data->ctd = d;
    data->positions = positions;
    data->matches = matches;
    auto fn = MkFunc0<CountEndTaskData>(CountEndTask, data);
    uitask::Post(fn, "TaskFindCount");
    DestroyTempAllocator();
}

// Wait for a worker thread while still pumping UI messages so the find window
// (and the rest of the app) stays responsive during cancellation.
static void WaitForThreadWithMessagePump(HANDLE th) {
    if (!th) {
        return;
    }
    for (;;) {
        DWORD waitRes = MsgWaitForMultipleObjects(1, &th, FALSE, 50, QS_ALLINPUT);
        if (waitRes == WAIT_OBJECT_0) {
            return;
        }
        uitask::DrainQueue();
        if (waitRes == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (!PumpAppMessage(msg)) {
                    return;
                }
            }
        }
    }
}

// cancel any running/pending count and wait for the worker to exit. The find
// thread and the count thread must never use the engine's text extraction at
// the same time (mupdf isn't safe for concurrent page access), so a find must
// not start while a count is running. The wait is bounded: the worker checks
// the epoch after every match, so it exits within one page's work.
static void AbortCount(MainWindow* win, bool wait, bool pumpMessages = true) {
    InterlockedIncrement(&win->findCountEpoch);
    str::FreePtr(&win->findCountPendingText);
    HANDLE th = win->findCountThread;
    if (!th) {
        return;
    }
    if (wait) {
        if (pumpMessages) {
            WaitForThreadWithMessagePump(th);
        } else {
            WaitForSingleObject(th, INFINITE);
        }
        win->findCountThread = nullptr;
    }
    // When cancellation is asynchronous, keep the handle in the worker slot.
    // CountEndTask clears it and starts the latest coalesced request. Clearing it
    // here would allow a second text-extraction worker to start concurrently.
}

void SuspendFindEngineAccess(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    if (!win->findCountThread && !win->findThread) {
        return;
    }
    AbortFinding(win, false);
}

// (re)build the match-position cache on a background thread. Coalesces: if a
// scan is already running, remember only the latest request and let the running
// worker start it when it finishes, so rapid typing never piles up scans and
// the UI thread never blocks waiting on a scan.
static void StartFindCount(MainWindow* win, const WCHAR* text, bool matchCase, bool matchWholeWord) {
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    if (OcrAutoEnabled(win)) {
        OcrScheduleForPage(win, dm->CurrentPageNo());
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return;
    }
    if (win->findCountThread && win->findCountEngine && win->findCountEngine != engine) {
        AbortCount(win, false);
    }
    bool resume = win->findCountValid && win->findCountPartial && win->findCountText &&
                  str::Eq(win->findCountText, text) && win->findCountMatchCase == matchCase &&
                  win->findCountMatchWholeWord == matchWholeWord && win->findCountEngine == engine &&
                  win->findCountTextCacheGeneration == engine->GetTextCacheGeneration() &&
                  win->findCountPageLimit > 0 && win->findCountPageLimit < dm->PageCount();
    int resumeFromPage = resume && win->findCountContinuationPage > 0 ? win->findCountContinuationPage
                                                                      : (resume ? win->findCountPageLimit + 1 : 0);
    if (resume) {
        // The old boundary is no longer authoritative. Remove it from the
        // installed model before streaming replacement rows from this pass.
        RemoveMatchesStartingAtOrAfter(&win->findCountPositions, &win->findMatches, resumeFromPage);
        win->findCountLatestFound = (int)win->findCountPositions.size();
        FindWindowRefreshResults(win, false);
        ShowMatchCount(win);
    }
    Vec<u64>* seedPositions = resume ? new Vec<u64>(win->findCountPositions) : nullptr;
    Vec<FindMatch>* seedMatches = resume ? CloneFindMatches(win->findMatches) : nullptr;
    int originalStartPage = resume ? win->findCountStartPage : (win->ctrl ? win->ctrl->CurrentPageNo() : 1);

    if (!resume) {
        win->findCountValid = false;
        win->findCountPartial = false;
        win->findCountContinuationPage = 0;
    }
    win->findCountLatestFound = seedPositions ? (int)seedPositions->size() : 0;
    win->findCountEngine = engine;
    StartFindStatusAnimation(win);

    if (win->findCountThread) {
        delete seedPositions;
        FreeMatchSnippets(seedMatches);
        delete seedMatches;
        // a scan is in flight: cancel it and queue this request; the running
        // worker's CountEndTask will start it once it exits
        InterlockedIncrement(&win->findCountEpoch);
        str::FreePtr(&win->findCountPendingText);
        win->findCountPendingText = str::Dup(text);
        win->findCountPendingMatchCase = matchCase;
        win->findCountPendingMatchWholeWord = matchWholeWord;
        return;
    }

    if (TryInstallCountFromSession(win, dm, engine, text, matchCase, matchWholeWord)) {
        delete seedPositions;
        FreeMatchSnippets(seedMatches);
        delete seedMatches;
        if (win->findPendingFromPage > 0) {
            int fromPage = win->findPendingFromPage;
            win->findPendingFromPage = 0;
            NavigateFirstMatchFromPage(win, fromPage);
        }
        return;
    }

    engine->AddRef(); // released in CountThread
    // build per-match snippets only when the floating results list is showing;
    // also build the match list (without snippets) when painting all highlights
    bool wantSnippets = WantFindSnippets();
    LONG epoch = InterlockedIncrement(&win->findCountEpoch);
    auto d =
        new CountThreadData(win, engine, text, matchCase, matchWholeWord, true, wantSnippets, originalStartPage, epoch);
    d->resumeFromPage = resumeFromPage;
    d->seedPositions = seedPositions;
    d->seedMatches = seedMatches;
    d->snippetMaxGlyphs = FindSnippetMaxGlyphs(win);
    win->findCountThread = nullptr;
    auto fn = MkFunc0<CountThreadData>(CountThread, d);
    win->findCountThread = StartThread(fn, "FindCountThread");
    d->thread = win->findCountThread;
}

// update the n/m counter after a search settles on a match: instant from cache
// when the term/match-case/document are unchanged, otherwise rebuild it
static void UpdateMatchCount(MainWindow* win, const WCHAR* text) {
    DisplayModel* dm = win->AsFixed();
    void* engine = dm ? (void*)dm->GetEngine() : nullptr;
    bool wantSnippets = WantFindSnippets();
    if (wantSnippets && win->findCountValid && !win->findCountHasSnippets && win->findCountText &&
        str::Eq(win->findCountText, text) && win->findCountPositions.size() > 0 && engine == win->findCountEngine &&
        win->findCountMatchCase == win->findMatchCase && win->findCountMatchWholeWord == win->findMatchWholeWord) {
        EnsureFindMatchList(win);
    }
    bool cacheHit = win->findCountValid && win->findCountText && str::Eq(win->findCountText, text) &&
                    win->findCountMatchCase == win->findMatchCase &&
                    win->findCountMatchWholeWord == win->findMatchWholeWord && win->findCountEngine == engine &&
                    (!win->findCountPartial || !dm || win->findCountPageLimit >= dm->PageCount()) &&
                    (wantSnippets ? win->findCountHasSnippets : win->findMatches.size() > 0);
    if (cacheHit) {
        EnsureFindMatchList(win);
        // matches are unchanged: just refresh n/m. Don't rebuild the results
        // list here -- it's already populated and rebuilding clears the user's
        // selection (the list is rebuilt only when a new count installs matches).
        ShowMatchCount(win);
    } else {
        StartFindCount(win, text, win->findMatchCase, win->findMatchWholeWord);
    }
}

void RequestFindCount(MainWindow* win) {
    if (!win || !win->hwndFindEdit) {
        return;
    }
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    TempWStr text = HwndGetTextWTemp(win->hwndFindEdit);
    if (str::IsEmpty(text)) {
        return;
    }
    UpdateMatchCount(win, text);
}

void UpdateFindMatchCountDisplay(MainWindow* win) {
    ShowMatchCount(win);
}

void OnEbookPageCountChanged(MainWindow* win) {
    RefreshFindSearchBlockedStatus(win);
    if (!win || !IsFindUIVisible(win) || !win->hwndFindEdit) {
        return;
    }
    TempWStr text = HwndGetTextWTemp(win->hwndFindEdit);
    if (str::IsEmpty(text)) {
        return;
    }
    // The query may have been entered before the first stable page existed.
    // Start it as soon as the loader publishes a searchable prefix.
    if (!win->findCountValid) {
        if (!win->findCountThread && !win->findThread && IsDocumentSearchReady(win)) {
            RequestFindCount(win);
        }
        return;
    }
    if (!win->findCountPartial) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!dm || !engine || win->findCountEngine != engine) {
        return;
    }
    if (str::IsEmpty(text) || !win->findCountText || !str::Eq(text, win->findCountText)) {
        return;
    }
    int publishedPages = dm->PageCount();
    if (publishedPages > win->findCountPageLimit) {
        if (!win->findCountThread && !win->findThread) {
            StartFindCount(win, text, win->findCountMatchCase, win->findCountMatchWholeWord);
        }
        return;
    }
    if (!EngineIsProgressiveEbookLoading(engine)) {
        if (win->findCountPartial) {
            // The published prefix is stable. Any candidate that needed pages
            // beyond the final frontier is now a definite non-match, so the
            // accumulated incremental result is complete without a full rescan.
            win->findCountPartial = false;
            win->findCountContinuationPage = 0;
        }
        dm->searchSession.Store(engine, text, win->findCountMatchCase, win->findCountMatchWholeWord,
                                win->findCountStartPage, false, win->findCountPageLimit, win->findCountPageLimit, 0,
                                win->findCountPositions);
        ShowMatchCount(win);
        FindBarBeginStatusCompleteFlash(win);
    }
}

// navigate to a match chosen from the floating results list and select it, so
// Find Next/Prev and the n/m counter continue from there
void GoToFindMatch(MainWindow* win, int startPage, int startGlyph, int endPage, int endGlyph) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSearch || !dm->GetEngine()) {
        return;
    }
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    // A first-match worker shares dm->textSearch and must be joined. The count
    // worker owns a private TextSearch; navigation from its streamed rows uses
    // text already cached for that match and can safely continue concurrently.
    // findEnterPending only means Enter has not started that first-match
    // worker. AbortFinding would also AbortCount, which permanently stops
    // progressive full-document search after a results-list click.
    if (win->findThread) {
        AbortFinding(win, true);
    } else {
        win->findEnterPending = false;
    }
    ClearFindSearchProgressCb(win);
    TextSearch* ts = dm->textSearch;
    TempWStr searchText = win->hwndFindEdit ? HwndGetTextWTemp(win->hwndFindEdit) : nullptr;

    auto trySelect = [&]() -> bool {
        ts->Reset();
        ts->StartAt(startPage, startGlyph);
        ts->SelectUpTo(endPage, endGlyph);
        return ts->result.len > 0;
    };

    if (!trySelect() && searchText && searchText[0]) {
        ts->Reset();
        ts->SetMatchCase(win->findMatchCase);
        ts->SetMatchWholeWord(win->findMatchWholeWord);
        ts->SetText(searchText);
        bool abortSearch = false;
        if (ts->LoadPageText(startPage, nullptr, &abortSearch) && !abortSearch) {
            int cp = ts->GlyphToCodepoint(startPage, startGlyph);
            TextSearch::PageAndOffset end = ts->MatchEnd(cp);
            if (end.page > 0 && end.offset > cp) {
                int sg = ts->CodepointToGlyph(startPage, cp);
                int eg = ts->CodepointToGlyph(end.page, end.offset);
                ts->StartAt(startPage, sg);
                ts->SelectUpTo(end.page, eg);
            }
        }
    }

    if (ts->result.len == 0) {
        if (win->ctrl) {
            win->ctrl->GoToPage(startPage, true);
        }
        return;
    }
    // navigate to the match while ts->result is still populated. SetLastResult()
    // below calls SetText(), which clears ts->result whenever the matched text
    // differs from the last search text (e.g. a case-insensitive find where
    // "the" matched "The"), so ShowSearchResult() must run first
    ShowSearchResult(win, &ts->result, true);
    // hand the selection to TextSearch as its "last result" so Find Next/Prev
    // continue from here; SetLastResult owns the findPage/findIndex/pageText
    // bookkeeping (so we don't poke internals or leave pageText null). The match's
    // glyph range (start/end) survives this, so the bookkeeping stays correct.
    ts->SetLastResult(ts);
    ShowMatchCount(win);
    // Find Next/Prev may be invoked by the compact find bar while the floating
    // results window is visible. Keep that list's selected row in sync with the
    // match we just navigated to.
    FindWindowRefreshResults(win, false);
}

static bool GoToCachedMatchIndex(MainWindow* win, int idx) {
    int n = (int)win->findCountPositions.size();
    if (idx < 0 || idx >= n) {
        return false;
    }
    if ((int)win->findMatches.size() == n) {
        const FindMatch& fm = win->findMatches[idx];
        GoToFindMatch(win, fm.startPage, fm.startGlyph, fm.endPage, fm.endGlyph);
        return true;
    }
    u64 key = win->findCountPositions[idx];
    int page = (int)(key >> 32);
    int glyph = (int)(key & 0xffffffff);
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || !win->findCountText) {
        return false;
    }
    TextSearch ts(engine);
    ts.SetMatchCase(win->findCountMatchCase);
    ts.SetMatchWholeWord(win->findCountMatchWholeWord);
    ts.SetText(win->findCountText);
    bool abortSearch = false;
    if (!ts.LoadPageText(page, nullptr, &abortSearch) || abortSearch) {
        return false;
    }
    int offset = ts.GlyphToCodepoint(page, glyph);
    TextSearch::PageAndOffset end = ts.MatchEnd(offset);
    if (end.page <= 0) {
        return false;
    }
    GoToFindMatch(win, page, glyph, end.page, ts.CodepointToGlyph(end.page, end.offset));
    return true;
}

static bool NavigateFirstMatchFromPage(MainWindow* win, int startPage) {
    if (!win->findCountValid) {
        return false;
    }
    int n = (int)win->findCountPositions.size();
    if (n == 0) {
        return false;
    }
    int idx = FirstSortedIndexAtOrAfterPage(win->findCountPositions, startPage);
    if (idx < 0) {
        idx = 0;
    }
    return GoToCachedMatchIndex(win, idx);
}

static bool ViewLeftCurrentFindHit(MainWindow* win) {
    DisplayModel* dm = win->AsFixed();
    if (!dm || !dm->textSearch || dm->textSearch->result.len == 0) {
        return true;
    }
    if (!win->ctrl) {
        return true;
    }
    return win->ctrl->CurrentPageNo() != dm->textSearch->startPage;
}

bool FindEnterFromCurrentPageIfNeeded(MainWindow* win) {
    if (!win || !win->IsDocLoaded() || !NeedsFindUI(win)) {
        return false;
    }
    if (!ViewLeftCurrentFindHit(win)) {
        return false;
    }
    int page = win->ctrl ? win->ctrl->CurrentPageNo() : 1;
    if (page < 1) {
        page = 1;
    }
    if (CountCacheIsComplete(win)) {
        if (win->findCountPositions.size() == 0) {
            return false;
        }
        win->findPendingFromPage = 0;
        return NavigateFirstMatchFromPage(win, page);
    }
    // count still running or incomplete: jump after the sorted cache lands
    win->findPendingFromPage = page;
    return true;
}

// step through the cached match list (same order as the n/m counter and the
// floating results list) instead of re-scanning the document
static bool TryNavigateCachedFindMatch(MainWindow* win, TextSearch::Direction direction) {
    if (!win->findCountValid) {
        return false;
    }
    int n = (int)win->findCountPositions.size();
    if (n == 0) {
        return false;
    }
    bool forward = direction == TextSearch::Direction::Forward;
    int cur = FindCurrentMatchIndex(win);
    int idx;
    if (cur < 0) {
        int page = win->ctrl ? win->ctrl->CurrentPageNo() : 1;
        int from = FirstSortedIndexAtOrAfterPage(win->findCountPositions, page);
        if (forward) {
            idx = from >= 0 ? from : 0;
        } else {
            idx = from > 0 ? from - 1 : n - 1;
        }
    } else if (forward) {
        idx = (cur + 1) % n;
    } else {
        idx = (cur - 1 + n) % n;
    }
    return GoToCachedMatchIndex(win, idx);
}

static void FindEndTask(FindEndTaskData* d) {
    auto win = d->win;
    auto ftd = d->ftd;
    auto textSel = d->textSel;
    auto wasModifiedCanceled = d->wasModifiedCanceled;
    auto loopedAround = d->loopedAround;

    AutoDelete delData(d);
    if (!IsMainWindowValid(win)) {
        return;
    }
    if (!win->ctrl || !win->AsFixed()) {
        // The tab/document changed after the worker was queued. This is a normal
        // cancellation race; only release the matching worker slot and data.
        if (win->findThread == ftd->thread) {
            win->findThread = nullptr;
        }
        return;
    }
    if (win->findThread != ftd->thread) {
        // Race condition: FindTextOnThread/AbortFinding was
        // called after the previous find thread ended but
        // before this FindEndTask could be executed
        return;
    }
    // This task owns the matching worker slot. Release it even when the epoch
    // below says its result is stale; otherwise a tab switch can permanently
    // leave findThread non-null and block later Ctrl+F searches.
    win->findThread = nullptr;
    if (win->findCountEpoch != ftd->epochAtStart) {
        return; // tab switched or search canceled while the worker was running
    }
    ClearFindSearchProgressCb(win);
    if (!win->IsDocLoaded()) {
        // the UI has already been disabled and hidden
    } else if (textSel) {
        ShowSearchResult(win, textSel, wasModifiedCanceled);
        ftd->HideUI(true, loopedAround);
        ShowMatchCount(win);
        if (!win->findCountValid) {
            RequestFindCount(win);
        }
    } else {
        ClearSearchResult(win);
        ftd->HideUI(false, !wasModifiedCanceled);
        if (!wasModifiedCanceled && loopedAround) {
            InstallEmptyCountCache(win, ftd->text);
        } else {
            RequestFindCount(win);
        }
    }
}

static void UpdateSearchProgress(FindThreadData* ftd, ProgressUpdateData* data) {
    if (!ftd || !IsMainWindowValid(ftd->win)) {
        if (data && data->wasCancelled) {
            *data->wasCancelled = true;
        }
        return;
    }
    if (data->wasCancelled) {
        *data->wasCancelled = ftd->WasCanceled();
    }
}

static void FindThread(FindThreadData* ftd) {
    if (!ftd || !IsMainWindowValid(ftd->win) || !ftd->win->ctrl || !ftd->win->ctrl->AsFixed()) {
        // The UI can switch/close the tab between StartThread() and this entry
        // point. Treat that as cancellation instead of reporting an invariant
        // failure. Wait until the creator publishes the handle so the cleanup
        // task can close it through FindThreadData's destructor.
        if (ftd) {
            while (!ftd->thread) {
                Sleep(1);
            }
            auto data = new FindEndTaskData;
            data->win = ftd->win;
            data->ftd = ftd;
            uitask::Post(MkFunc0<FindEndTaskData>(FindEndTask, data), "TaskFindEndCanceled");
        }
        DestroyTempAllocator();
        return;
    }

    MainWindow* win = ftd->win;
    DisplayModel* dm = win->AsFixed();
    auto textSearch = dm->textSearch;
    auto ctrl = win->ctrl;

    auto engine = dm->GetEngine();
    engine->AddRef();
    defer {
        SafeEngineRelease(&engine);
    };

    TextSel* rect = nullptr;
    bool loopedAround = false;

    ApplySearchPageLimit(textSearch, dm, engine);
    textSearch->progressCb = MkFunc1<FindThreadData, ProgressUpdateData*>(UpdateSearchProgress, ftd);
    textSearch->SetDirection(ftd->direction);
    textSearch->SetMatchCase(win->findMatchCase);
    textSearch->SetMatchWholeWord(win->findMatchWholeWord);

    if (ftd->wasModified) {
        SearchSessionEntry entry;
        if (dm->searchSession.Lookup(engine, ftd->text, win->findMatchCase, win->findMatchWholeWord, dm->PageCount(),
                                     true, &entry)) {
            textSearch->SetText(ftd->text);
            textSearch->SetDirection(ftd->direction);
            if (entry.positions && textSearch->TryFindFromCachedPositions(*entry.positions, ctrl->CurrentPageNo())) {
                rect = &textSearch->result;
            }
        }
    }
    if (!rect) {
        if (ftd->wasModified || !ctrl->ValidPageNo(textSearch->GetCurrentPageNo()) ||
            !dm->GetPageInfo(textSearch->GetCurrentPageNo())->visibleRatio) {
            rect = textSearch->FindFirst(ctrl->CurrentPageNo(), ftd->text);
        } else {
            rect = textSearch->FindNext();
        }
    }

    if (!win->findCancelled && !rect) {
        // With no further findings, start over (unless this was a new search from the beginning)
        int startPage = (TextSearch::Direction::Forward == ftd->direction) ? 1 : ctrl->PageCount();
        if (!ftd->wasModified || ctrl->CurrentPageNo() != startPage) {
            loopedAround = true;
            rect = textSearch->FindFirst(startPage, ftd->text);
        }
    }

    // wait for FindTextOnThread to return so that
    // FindEndTask closes the correct handle to
    // the current find thread
    while (!win->findThread) {
        Sleep(1);
    }

    ClearFindSearchProgressCb(win);

    auto data = new FindEndTaskData;
    data->win = win;
    data->ftd = ftd;
    data->textSel = nullptr;
    data->loopedAround = false;

    if (!win->findCancelled && rect) {
        data->textSel = rect;
        data->wasModifiedCanceled = ftd->wasModified;
        data->loopedAround = loopedAround;
    } else {
        data->wasModifiedCanceled = win->findCancelled;
        if (!win->findCancelled && loopedAround) {
            data->loopedAround = true;
        }
    }
    auto fn = MkFunc0<FindEndTaskData>(FindEndTask, data);
    uitask::Post(fn, "TaskFindEnd");
    DestroyTempAllocator();
}

// returns true if did abort a thread or hidden the notification
bool AbortFinding(MainWindow* win, bool hideMessage, bool waitForWorkers, bool pumpMessages) {
    bool res = false;
    win->findEnterPending = false;
    AbortCount(win, waitForWorkers, pumpMessages);
    if (waitForWorkers) {
        StopFindStatusAnimation(win);
    }
    if (win->findThread) {
        res = true;
        win->findCancelled = true;
        HANDLE th = win->findThread;
        if (waitForWorkers) {
            if (pumpMessages) {
                WaitForThreadWithMessagePump(th);
            } else {
                WaitForSingleObject(th, INFINITE);
            }
            win->findThread = nullptr;
        }
        // For asynchronous cancellation, FindEndTask keeps ownership of and
        // clears the worker slot. Clearing it here would allow another shared
        // TextSearch worker to start before this one has actually exited.
        ClearFindSearchProgressCb(win);
    }
    win->findCancelled = false;

    if (hideMessage) {
        bool didRemove = RemoveNotificationsForGroup(win->hwndCanvas, kNotifFindProgress);
        if (didRemove) {
            res = true;
        }
    }
    return res;
}

// wasModified
//   if true, starting a search for new term
//   if false, searching for the next occurence of previous term
// TODO: should detect wasModified by comparing with the last search result
void FindTextOnThread(MainWindow* win, TextSearch::Direction direction, const char* text, bool wasModified) {
    if (!EnsureDocumentSearchReady(win)) {
        return;
    }
    if (OcrAutoEnabled(win)) {
        DisplayModel* dm = win->AsFixed();
        if (dm) {
            OcrScheduleForPage(win, dm->CurrentPageNo());
        }
    }
    AbortFinding(win, false);
    if (str::IsEmpty(text)) {
        return;
    }
    if (wasModified) {
        win->findCountValid = false;
        win->findCountPartial = false;
        DisplayModel* dm = win->AsFixed();
        if (dm) {
            dm->searchSession.Remove(ToWStrTemp(text), win->findMatchCase, win->findMatchWholeWord);
        }
    }
    FindThreadData* ftd = new FindThreadData(win, direction, text, wasModified);
    ftd->ShowUI();
    win->findThread = nullptr;
    auto fn = MkFunc0(FindThread, ftd);
    win->findThread = StartThread(fn, "FindThread");
    ftd->thread = win->findThread; // safe because only accesssed on ui thread
}

// TODO: for https://github.com/sumatrapdfreader/sumatrapdf/issues/2655
char* ReverseTextTemp(char* s) {
    TempWStr ws = ToWStrTemp(s);
    int n = str::Leni(ws);
    for (int i = 0; i < n / 2; i++) {
        WCHAR c1 = ws[i];
        WCHAR c2 = ws[n - 1 - i];
        ws[i] = c2;
        ws[n - 1 - i] = c1;
    }
    return ToUtf8Temp(ws);
}

void FindTextOnThread(MainWindow* win, TextSearch::Direction direction) {
    char* s = HwndGetTextTemp(win->hwndFindEdit);
    // if document is rtl, need to reverse the text
    // s = ReverseTextTemp(s);
    bool wasModified = Edit_GetModify(win->hwndFindEdit);
    if (!wasModified) {
        // check if the find text differs from the current tab's cached search text
        // this happens when switching tabs: the find edit box shows the current text
        // but the per-tab textSearch still has the old search text cached
        DisplayModel* dm = win->AsFixed();
        if (dm && dm->textSearch) {
            TempWStr ws = ToWStrTemp(s);
            if (!str::Eq(ws, dm->textSearch->lastText)) {
                wasModified = true;
            }
        }
    }
    Edit_SetModify(win->hwndFindEdit, FALSE);
    FindTextOnThread(win, direction, s, wasModified);
}

static bool FindMatchTouchesVisiblePages(const FindMatch& fm, int firstPage, int lastPage) {
    return fm.endPage >= firstPage && fm.startPage <= lastPage;
}

static void GetVisiblePageRange(DisplayModel* dm, int& firstOut, int& lastOut) {
    firstOut = dm->FirstVisiblePageNo();
    lastOut = firstOut;
    if (!dm->ValidPageNo(firstOut)) {
        firstOut = lastOut = 0;
        return;
    }
    int pageCount = dm->PageCount();
    for (int pageNo = pageCount; pageNo >= firstOut; pageNo--) {
        if (dm->PageVisible(pageNo)) {
            lastOut = pageNo;
            break;
        }
    }
}

static void AppendMatchPageRects(EngineBase* engine, const FindMatch& fm, Vec<FindMatchPaintPageRect>& out) {
    TextSelection ts(engine);
    ts.StartAt(fm.startPage, fm.startGlyph);
    ts.SelectUpTo(fm.endPage, fm.endGlyph);

    for (int i = 0; i < ts.result.len; i++) {
        FindMatchPaintPageRect pr;
        pr.pageNo = ts.result.pages[i];
        pr.rect = ts.result.rects[i];
        out.Append(pr);
    }
}

static void AppendPageRectsToScreen(DisplayModel* dm, const Rect& clipRc, const FindMatchPaintPageRect* pageRects,
                                    int nRects, Vec<Rect>& out) {
    if (!dm || !pageRects || nRects <= 0) {
        return;
    }
    for (int i = 0; i < nRects; i++) {
        const FindMatchPaintPageRect& pr = pageRects[i];
        if (!dm->ValidPageNo(pr.pageNo) || !dm->PageVisible(pr.pageNo)) {
            continue;
        }
        Rect rc = dm->CvtToScreen(pr.pageNo, ToRectF(pr.rect));
        rc = rc.Intersect(clipRc);
        if (!rc.IsEmpty()) {
            out.Append(rc);
        }
    }
}

static void AppendTextSelScreenRects(DisplayModel* dm, const Rect& clipRc, TextSel* sel, Vec<Rect>& out) {
    if (!sel || sel->len == 0 || !sel->pages || !sel->rects) {
        return;
    }
    for (int i = 0; i < sel->len; i++) {
        int pageNo = sel->pages[i];
        if (!dm->PageVisible(pageNo)) {
            continue;
        }
        Rect rc = dm->CvtToScreen(pageNo, ToRectF(sel->rects[i]));
        rc = rc.Intersect(clipRc);
        if (!rc.IsEmpty()) {
            out.Append(rc);
        }
    }
}

static void GetFindCurrentMatchHighlightStyle(COLORREF& col, u8& alpha) {
    ParsedColor* parsedCol = GetPrefsColor(gGlobalPrefs->fixedPageUI.findMatchColor);
    col = parsedCol->col;
    alpha = GetAlpha(parsedCol->col);
    if (alpha == 0) {
        alpha = kSelectionDefaultAlpha;
    }
}

static void GetFindOtherMatchHighlightStyle(COLORREF& col, u8& alpha) {
    col = kFindOtherMatchColor;
    alpha = kSelectionDefaultAlpha;
}

void PaintAllFindMatches(MainWindow* win, HDC hdc) {
    if (!win->IsDocLoaded() || !win->AsFixed()) {
        return;
    }
    if (!IsFindUIVisible(win)) {
        return;
    }
    if (!win->hwndFindEdit || HwndGetTextLen(win->hwndFindEdit) == 0) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    TextSearch* ts = dm->textSearch;
    if (!win->findCountValid && win->findMatches.size() == 0) {
        // count still running: at least highlight the current match
        if (!ts || ts->result.len == 0) {
            return;
        }
        Vec<Rect> currentRects;
        AppendTextSelScreenRects(dm, win->canvasRc, &ts->result, currentRects);
        if (currentRects.size() > 0) {
            COLORREF findCol;
            u8 alpha;
            GetFindCurrentMatchHighlightStyle(findCol, alpha);
            PaintFindMatchHighlightRectangles(hdc, win->canvasRc, currentRects, findCol, alpha);
        }
        return;
    }
    if (win->findMatches.size() == 0) {
        return;
    }
    int firstPage = 0;
    int lastPage = 0;
    GetVisiblePageRange(dm, firstPage, lastPage);
    if (!dm->ValidPageNo(firstPage)) {
        return;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return;
    }

    u64 currentKey = 0;
    if (ts && ts->result.len > 0) {
        currentKey = MatchKey(ts->startPage, ts->startGlyph);
    }

    COLORREF currentCol, otherCol;
    u8 currentAlpha, otherAlpha;
    GetFindCurrentMatchHighlightStyle(currentCol, currentAlpha);
    GetFindOtherMatchHighlightStyle(otherCol, otherAlpha);

    Vec<Rect> otherRects;
    Vec<Rect> currentRects;
    Vec<FindMatchPaintPageRect> liveRects;
    for (int i = 0; i < (int)win->findMatches.size(); i++) {
        const FindMatch& fm = win->findMatches[i];
        if (!FindMatchTouchesVisiblePages(fm, firstPage, lastPage)) {
            continue;
        }
        liveRects.Reset();
        AppendMatchPageRects(engine, fm, liveRects);
        if (liveRects.size() == 0) {
            continue;
        }
        u64 key = MatchKey(fm.startPage, fm.startGlyph);
        Vec<Rect>& out = (key == currentKey) ? currentRects : otherRects;
        AppendPageRectsToScreen(dm, win->canvasRc, liveRects.begin(), (int)liveRects.size(), out);
    }

    if (otherRects.size() > 0) {
        PaintFindMatchHighlightRectangles(hdc, win->canvasRc, otherRects, otherCol, otherAlpha);
    }
    if (currentRects.size() == 0 && ts && ts->result.len > 0) {
        AppendTextSelScreenRects(dm, win->canvasRc, &ts->result, currentRects);
    }
    if (currentRects.size() > 0) {
        PaintFindMatchHighlightRectangles(hdc, win->canvasRc, currentRects, currentCol, currentAlpha);
    }
}

void PaintForwardSearchMark(MainWindow* win, HDC hdc) {
    ReportIf(!win->AsFixed());
    DisplayModel* dm = win->AsFixed();
    int pageNo = win->fwdSearchMark.page;
    PageInfo* pageInfo = dm->GetPageInfo(pageNo);
    if (!pageInfo || 0.0 == pageInfo->visibleRatio) {
        return;
    }

    int hiLiWidth = gGlobalPrefs->forwardSearch.highlightWidth;
    int hiLiOff = gGlobalPrefs->forwardSearch.highlightOffset;

    // Draw the rectangles highlighting the forward search results
    Vec<Rect> rects;
    for (size_t i = 0; i < win->fwdSearchMark.rects.size(); i++) {
        Rect rect = win->fwdSearchMark.rects.at(i);
        rect = dm->CvtToScreen(pageNo, ToRectF(rect));
        if (hiLiOff > 0) {
            float zoom = dm->GetZoomReal(pageNo);
            rect.x = std::max(pageInfo->pageOnScreen.x, 0) + (int)(hiLiOff * zoom);
            rect.dx = (int)((hiLiWidth > 0 ? hiLiWidth : 15.0) * zoom);
            rect.y -= 4;
            rect.dy += 8;
        }
        rects.Append(rect);
    }

    BYTE alpha = (BYTE)(0x5f * 1.0f * (HIDE_FWDSRCHMARK_STEPS - win->fwdSearchMark.hideStep) / HIDE_FWDSRCHMARK_STEPS);
    ParsedColor* parsedCol = GetPrefsColor(gGlobalPrefs->forwardSearch.highlightColor);
    PaintTransparentRectangles(hdc, win->canvasRc, rects, parsedCol->col, alpha);
}

// returns true if inverse search was performed
bool OnInverseSearch(MainWindow* win, int x, int y) {
    if (!CanAccessDisk() || gPluginMode) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || tab->GetEngineType() != kindEngineMupdf) {
        return false;
    }
    DisplayModel* dm = tab->AsFixed();

    // Clear the last forward-search result
    win->fwdSearchMark.rects.Reset();
    InvalidateRect(win->hwndCanvas, nullptr, FALSE);

    // On double-clicking error message will be shown to the user
    // if the PDF does not have a synchronization file
    if (!dm->pdfSync) {
        const char* path = tab->filePath;
        int err = Synchronizer::Create(path, dm->GetEngine(), &dm->pdfSync);
        if (err == PDFSYNCERR_SYNCFILE_NOTFOUND) {
            // We used to warn that "No synchronization file found" at this
            // point if gGlobalPrefs->enableTeXEnhancements is set; we no longer
            // so do because a double-click has several other meanings
            // (selecting a word or an image, navigating quickly using links)
            // and showing an unrelated warning in all those cases seems wrong
            return false;
        }
        if (err != PDFSYNCERR_SUCCESS) {
            NotificationCreateArgs args;
            args.hwndParent = win->hwndCanvas;
            args.msg = _TRA("Synchronization file cannot be opened");
            ShowNotification(args);
            return true;
        }
        gGlobalPrefs->enableTeXEnhancements = true;
    }

    int pageNo = dm->GetPageNoByPoint(Point(x, y));
    if (!tab->ctrl->ValidPageNo(pageNo)) {
        return false;
    }

    Point pt = ToPoint(dm->CvtFromScreen(Point(x, y), pageNo));
    AutoFreeStr srcfilepath;
    int line = 0;
    int col = 0;
    int err = dm->pdfSync->DocToSource(pageNo, pt, srcfilepath, &line, &col);
    if (err != PDFSYNCERR_SUCCESS) {
        NotificationCreateArgs args;
        args.hwndParent = win->hwndCanvas;
        args.msg = _TRA("No synchronization info at this position");
        ShowNotification(args);
        return true;
    }

    if (!file::Exists(srcfilepath)) {
        // if the source file is missing, check if it's been moved to the same place as
        // the PDF document (which happens if all files are moved together)
        TempStr altsrcpath = path::GetDirTemp(tab->filePath);
        altsrcpath = path::JoinTemp(altsrcpath, path::GetBaseNameTemp(srcfilepath));
        if (!str::Eq(altsrcpath, srcfilepath) && file::Exists(altsrcpath)) {
            srcfilepath.SetCopy(altsrcpath);
        }
    }

    char* inverseSearch = gGlobalPrefs->inverseSearchCmdLine;
    if (!inverseSearch) {
        Vec<TextEditor*> editors;
        DetectTextEditors(editors);
        if (editors.Size() > 0) {
            inverseSearch = str::DupTemp(editors[0]->openFileCmd);
        }
    }

    AutoFreeStr cmdLine;
    if (inverseSearch) {
        cmdLine = BuildOpenFileCmd(inverseSearch, srcfilepath, line, col);
    }

    NotificationCreateArgs args;
    args.hwndParent = win->hwndCanvas;
    args.msg = _TRA("Cannot start inverse search command. Please check the command line in the settings.");
    if (!str::IsEmpty(cmdLine.Get())) {
        // resolve relative paths with relation to SumatraPDF.exe's directory
        char* appDir = GetSelfExeDirTemp();
        AutoCloseHandle process(LaunchProcessInDir(cmdLine, appDir));
        if (!process) {
            ShowNotification(args);
        }
    } else if (gGlobalPrefs->enableTeXEnhancements) {
        ShowNotification(args);
    }

    return true;
}

// Show the result of a PDF forward-search synchronization (initiated by a DDE command)
void ShowForwardSearchResult(MainWindow* win, const char* fileName, int line, int /* col */, int ret, int page,
                             Vec<Rect>& rects) {
    ReportIf(!win->AsFixed());
    DisplayModel* dm = win->AsFixed();
    win->fwdSearchMark.rects.Reset();
    const PageInfo* pi = dm->GetPageInfo(page);
    if ((ret == PDFSYNCERR_SUCCESS) && (rects.size() > 0) && (nullptr != pi)) {
        // remember the position of the search result for drawing the rect later on
        win->fwdSearchMark.rects = rects;
        win->fwdSearchMark.page = page;
        win->fwdSearchMark.show = true;
        win->fwdSearchMark.hideStep = 0;
        if (!gGlobalPrefs->forwardSearch.highlightPermanent) {
            SetTimer(win->hwndCanvas, HIDE_FWDSRCHMARK_TIMER_ID, HIDE_FWDSRCHMARK_DELAY_IN_MS, nullptr);
        }

        // Scroll to show the overall highlighted zone
        int pageNo = page;
        Rect overallrc = rects.at(0);
        for (size_t i = 1; i < rects.size(); i++) {
            overallrc = overallrc.Union(rects.at(i));
        }
        TextSel res = {1, 1, &pageNo, &overallrc};
        if (!dm->PageVisible(page)) {
            win->ctrl->GoToPage(page, true);
        }
        if (!dm->ShowResultRectToScreen(&res)) {
            ScheduleRepaint(win, 0);
        }
        if (IsIconic(win->hwndFrame)) {
            ShowWindowAsync(win->hwndFrame, SW_RESTORE);
        }
        return;
    }

    TempStr buf = nullptr;
    NotificationCreateArgs args{};
    args.hwndParent = win->hwndCanvas;
    if (ret == PDFSYNCERR_SYNCFILE_NOTFOUND) {
        args.msg = _TRA("No synchronization file found");
    } else if (ret == PDFSYNCERR_SYNCFILE_CANNOT_BE_OPENED) {
        args.msg = _TRA("Synchronization file cannot be opened");
    } else if (ret == PDFSYNCERR_INVALID_PAGE_NUMBER) {
        buf = str::FormatTemp(_TRA("Page number %u inexistant"), page);
    } else if (ret == PDFSYNCERR_NO_SYNC_AT_LOCATION) {
        args.msg = _TRA("No synchronization info at this position");
    } else if (ret == PDFSYNCERR_UNKNOWN_SOURCEFILE) {
        buf = str::FormatTemp(_TRA("Unknown source file (%s)"), fileName);
    } else if (ret == PDFSYNCERR_NORECORD_IN_SOURCEFILE) {
        buf = str::FormatTemp(_TRA("Source file %s has no synchronization point"), fileName);
    } else if (ret == PDFSYNCERR_NORECORD_FOR_THATLINE) {
        buf = str::FormatTemp(_TRA("No result found around line %u in file %s"), line, fileName);
    } else if (ret == PDFSYNCERR_NOSYNCPOINT_FOR_LINERECORD) {
        buf = str::FormatTemp(_TRA("No result found around line %u in file %s"), line, fileName);
    }
    if (buf) {
        args.msg = buf;
        ShowNotification(args);
    }
}

// DDE commands handling

/*
Forward search (synchronization) DDE command

[ForwardSearch(["<pdffilepath>",]"<sourcefilepath>",<line>,<column>[,<newwindow>, <setfocus>])]
eg:
[ForwardSearch("c:\file.pdf","c:\folder\source.tex",298,0)]

if pdffilepath is provided, the file will be opened if no open window can be found for it
if newwindow = 1 then a new window is created even if the file is already open
if focus = 1 then the focus is set to the window
*/
static const char* HandleSyncCmd(const char* cmd, bool* ack) {
    AutoFreeStr pdfFile, srcFile;
    BOOL line = 0, col = 0, newWindow = 0, setFocus = 0;
    const char* next = str::Parse(cmd, "[ForwardSearch(\"%s\",%? \"%s\",%u,%u)]", &pdfFile, &srcFile, &line, &col);
    if (!next) {
        next = str::Parse(cmd, "[ForwardSearch(\"%s\",%? \"%s\",%u,%u,%u,%u)]", &pdfFile, &srcFile, &line, &col,
                          &newWindow, &setFocus);
    }
    // allow to omit the pdffile path, so that editors don't have to know about
    // multi-file projects (requires that the PDF has already been opened)
    if (!next) {
        pdfFile.Reset();
        next = str::Parse(cmd, "[ForwardSearch(\"%s\",%u,%u)]", &srcFile, &line, &col);
        if (!next) {
            next = str::Parse(cmd, "[ForwardSearch(\"%s\",%u,%u,%u,%u)]", &srcFile, &line, &col, &newWindow, &setFocus);
        }
    }

    if (!next) {
        return nullptr;
    }

    MainWindow* win = nullptr;
    if (pdfFile) {
        // check if the PDF is already opened
        win = FindMainWindowByFile(pdfFile, !newWindow);
        // if not then open it
        if (newWindow || !win) {
            LoadArgs args(pdfFile, !newWindow ? win : nullptr);
            win = LoadDocument(&args);
        } else if (!win->IsDocLoaded()) {
            ReloadDocument(win, false);
        }
    } else {
        // check if any opened PDF has sync information for the source file
        win = FindMainWindowBySyncFile(srcFile, true);
        if (win && newWindow) {
            LoadArgs args(win->CurrentTab()->filePath, nullptr);
            win = LoadDocument(&args);
        }
    }

    if (!win || !win->CurrentTab() || win->CurrentTab()->GetEngineType() != kindEngineMupdf) {
        return next;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm->pdfSync) {
        return next;
    }

    int page;
    Vec<Rect> rects;
    int ret = dm->pdfSync->SourceToDoc(srcFile, line, col, &page, rects);
    ShowForwardSearchResult(win, srcFile, line, col, ret, page, rects);
    if (setFocus) {
        win->Focus();
    }

    *ack = true;
    return next;
}

/*
Search DDE command

[Search("<pdffile>","<search-term>")]
*/
static const char* HandleSearchCmd(const char* cmd, bool* ack) {
    AutoFreeStr pdfFile;
    AutoFreeStr term;
    const char* next = str::Parse(cmd, "[Search(\"%s\",\"%s\")]", &pdfFile, &term);
    // TODO: should un-quote text to allow searching text with '"' in them
    if (!next) {
        return nullptr;
    }
    if (str::IsEmpty(term.Get())) {
        return next;
    }
    // check if the PDF is already opened
    // TODO: prioritize window with HWND so that if we have the same file
    // opened in multiple tabs / windows, we operate on the one that got the message
    MainWindow* win = FindMainWindowByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }
    bool wasModified = true;
    FindTextOnThread(win, TextSearch::Direction::Forward, term, wasModified);
    win->Focus();
    *ack = true;
    return next;
}

/*
Open file DDE Command

[Open("<pdffilepath>"[,<newWindow>,<setFocus>,<forceRefresh>,<inCurrentTab>])]
    newWindow, setFocus, forceRefresh, inCurrentTab are flags that can be 0 or 1 (set)
if the flag is set to 1:
    newWindow    : new window is created even if the file is already open
    setFocus     : focus is set to the window
    forceRefresh : reloads document
    inCurrentTab : replaces document in current tab (if 0 loads in a new tab)
                   if newWindow != 0 => ignored
valid formats:
    [Open("c:\file.pdf")]
    [Open("c:\file.pdf",1,1,0)]
    [Open("c:\file.pdf",1,1,0,1)]
*/
static const char* HandleOpenCmd(const char* cmd, bool* ack) {
    AutoFreeStr filePath;
    int newWindow = 0;
    int setFocus = 0;
    int forceRefresh = 0;
    int inCurrentTab = 0;
    const char* next = str::Parse(cmd, "[Open(\"%s\")]", &filePath);
    if (!next) {
        const char* pat = "[Open(\"%s\",%u,%u,%u,%u)]";
        next = str::Parse(cmd, pat, &filePath, &newWindow, &setFocus, &forceRefresh, &inCurrentTab);
    }
    if (!next) {
        const char* pat = "[Open(\"%s\",%u,%u,%u)]";
        next = str::Parse(cmd, pat, &filePath, &newWindow, &setFocus, &forceRefresh);
    }
    if (!next) {
        return nullptr;
    }
    bool isCtrl = IsCtrlPressed();
    logf("HandleOpenCmd: '%s', newWindow: %d, setFocus: %d, forceRefresh: %d, inCurrentTab: %d, isCtrl: %d\n",
         filePath.CStr(), newWindow, setFocus, forceRefresh, inCurrentTab, isCtrl);
    // on startup this is called while LoadDocument is in progress, which causes
    // all sort of mayhem. Queue files to be loaded in a sequence
    if (gIsStartup) {
        logf("HandleOpenCmd: gIsStartup, appending to gDdeOpenOnStartup\n");
        gDdeOpenOnStartup.Append(filePath);
        return next;
    }

    if (newWindow != 0 && inCurrentTab != 0) {
        inCurrentTab = 0;
        logf("HandleOpenCmd: setting inCurrentTab to 0 because newWindow != 0\n");
    }

    bool focusTab = (newWindow == 0);

    // intelligently pick a window or create one
    MainWindow* win = nullptr;
    MainWindow* emptyExistingWin = nullptr;
    auto nWindows = gWindows.Size();
    for (auto& w : gWindows) {
        if (!w->HasDocsLoaded()) {
            emptyExistingWin = w;
            logf("HandleOpenCmd: found empty existing window\n");
            break;
        }
    }
    if (newWindow > 0) {
        if (emptyExistingWin) {
            // instead of opening new window, re-use exisitng open window
            win = emptyExistingWin;
            logf("HandleOpenCmd: newWindow > 0, using empty existing window\n");
        } else {
            win = CreateAndShowMainWindow(nullptr);
            logf("HandleOpenCmd: newWindow > 0, created new window\n");
        }
    }
    bool doLoad = true;
    if (!win) {
        win = FindMainWindowByFile(filePath, focusTab);
        if (win) {
            logf("HandleOpenCmd: found existing window with file '%s'\n", filePath.Get());
            doLoad = false;
            if (!win->IsDocLoaded()) {
                ReloadDocument(win, false);
                forceRefresh = 0;
                logf("HandleOpenCmd: existing tab was not loaded, so reloaded, set forceRefresh = 0\n");
            }
        }
    }
    if (!win) {
        if (nWindows == 1) {
            // of only one window, use that one
            win = gWindows[0];
            logf("HandleOpenCmd: using the only window\n");
        }
        if (!win) {
            // https://github.com/sumatrapdfreader/sumatrapdf/issues/2315
            // open in the last active window
            win = FindMainWindowByHwnd(gLastActiveFrameHwnd);
            if (win) {
                logf("HandleOpenCmd: found last active window\n");
            } else {
                logf("HandleOpenCmd: didn't find last active window\n");
            }
        }
        if (!win && nWindows > 0) {
            // if can't find active, using the first
            win = gWindows[0];
            logf("HandleOpenCmd: first window\n");
        }
    }

    if (doLoad) {
        LoadArgs args(filePath, win);
        args.activateExisting = !isCtrl;
        if (newWindow) {
            args.activateExisting = false;
        }
        if (inCurrentTab) {
            args.forceReuse = true;
        }
        logf("HandleOpenCmd: calling LoadDocument(), activateExisting: %d, forceReuse: %d\n",
             (int)args.activateExisting, (int)args.forceReuse);
        win = LoadDocument(&args);
        if (!win) {
            logf("HandleOpenCmd: LoadDocument() for '%s' failed\n", filePath.Get());
        }
    }

    // TODO: not sure why this triggers. Seems to happen when opening multiple files
    // via Open menu in explorer. The first one is opened via cmd-line arg, the
    // rest via DDE.
    // ReportIf(win && win->IsAboutWindow());
    if (win) {
        if (forceRefresh) {
            logf("HandleOpenCmd: forceRefresh != 0 so calling ReloadDocument()\n");
            ReloadDocument(win, true);
        }
        if (setFocus) {
            logf("HandleOpenCmd: setFocus != 0 so calling win->Focus()\n");
            win->Focus();
        }
    }

    *ack = true;
    return next;
}

/*
DDE command: jump to named destination in an already opened document.

[GoToNamedDest("<pdffilepath>","<destination name>")]
e.g.:
[GoToNamedDest("c:\file.pdf", "chapter.1")]
*/
static const char* HandleGotoCmd(const char* cmd, bool* ack) {
    AutoFreeStr pdfFile, destName;
    const char* next = str::Parse(cmd, "[GotoNamedDest(\"%s\",%? \"%s\")]", &pdfFile, &destName);
    if (!next) {
        return nullptr;
    }

    MainWindow* win = FindMainWindowByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    win->linkHandler->GotoNamedDest(destName);
    win->Focus();
    *ack = true;
    return next;
}

/*
DDE command: jump to a page in an already opened document.

[GoToPage("<pdffilepath>",<page number>)]

eg: [GoToPage("c:\file.pdf",37)]
*/
static const char* HandlePageCmd(HWND, const char* cmd, bool* ack) {
    AutoFreeStr pdfFile;
    uint page = 0;
    const char* next = str::Parse(cmd, "[GotoPage(\"%S\",%u)]", &pdfFile, &page);
    if (!next) {
        return nullptr;
    }

    // check if the PDF is already opened
    // TODO: prioritize window with HWND so that if we have the same file
    // opened in multiple tabs / windows, we operate on the one that got the message
    MainWindow* win = FindMainWindowByFile(pdfFile, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    if (!win->ctrl->ValidPageNo(page)) {
        return next;
    }

    win->ctrl->GoToPage(page, true);
    *ack = true;
    win->Focus();
    return next;
}

/*
Set view mode and zoom level DDE command

[SetView("<filepath>", "<view mode>", <zoom level>[, <scrollX>, <scrollY>])]

eg: [SetView("c:\file.pdf", "book view", -2)]

use -1 for kZoomFitPage, -2 for kZoomFitWidth and -3 for kZoomFitContent
*/
static const char* HandleSetViewCmd(const char* cmd, bool* ack) {
    AutoFreeStr filePath, viewMode;
    float zoom = kInvalidZoom;
    Point scroll(-1, -1);
    const char* next = str::Parse(cmd, "[SetView(\"%s\",%? \"%s\",%f)]", &filePath, &viewMode, &zoom);
    if (!next) {
        next =
            str::Parse(cmd, "[SetView(\"%s\",%? \"%s\",%f,%d,%d)]", &filePath, &viewMode, &zoom, &scroll.x, &scroll.y);
    }
    if (!next) {
        return nullptr;
    }

    MainWindow* win = FindMainWindowByFile(filePath, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    DisplayMode mode = DisplayModeFromString(viewMode, DisplayMode::Automatic);
    if (mode != DisplayMode::Automatic) {
        SwitchToDisplayMode(win, mode);
    }

    if (zoom != kInvalidZoom) {
        SmartZoom(win, zoom, nullptr, false);
    }

    if ((scroll.x != -1 || scroll.y != -1) && win->AsFixed()) {
        DisplayModel* dm = win->AsFixed();
        ScrollState ss = dm->GetScrollState();
        ss.x = scroll.x;
        ss.y = scroll.y;
        dm->SetScrollState(ss);
    }
    *ack = true;
    return next;
}

/*
Open new window.

[NewWindow]
*/
static const char* HandleNewWindowCmd(const char* cmd, bool* ack) {
    if (!str::StartsWith(cmd, "[NewWindow]")) {
        return nullptr;
    }
    logf("HandleNewWindowCmd\n");
    const char* next = cmd + str::Leni("[NewWindow]");
    CreateAndShowMainWindow(nullptr);
    *ack = true;
    return next;
}

/*
[GetFileState("<filepath>")]
[GetFileState()]
[GetFileState]
Return info about document <filepath> or currently viewed document if no
<filepath> given.
Returns info in the format:

path: c:\file.pdf
zoom: 1.34
view: continuous
sumver: 3.5

i.e. multiple lines, each line is
key: value
This should make parsing easy:
* split by `\n' to get the lines
* split each line by ':' to get key and value

Returns:
error: <error message>
if file doesn't exist or no opened file
*/
static const char* HandleGetFileStateCmd(HWND hwnd, const char* cmd, bool* ack, StrBuilder& res) {
    AutoFreeStr filePath;
    const char* next = str::Parse(cmd, "[GetFileState(\"%s\")]", &filePath);
    if (!next) {
        next = str::Parse(cmd, "[GetFileState()]");
    }
    if (!next) {
        next = str::Parse(cmd, "[GetFileState]");
    }
    if (!next) {
        return nullptr;
    }

    res.Append("error: hello");
    MainWindow* win = FindMainWindowByFile(filePath, true);
    if (!win) {
        return next;
    }
    if (!win->IsDocLoaded()) {
        ReloadDocument(win, false);
        if (!win->IsDocLoaded()) {
            return next;
        }
    }

    res.Append("error: hello");
    *ack = true;
    return next;
}

/*
Handle all commands as defined in Commands.h
eg: [CmdClose] or [CmdCreateAnnotHighlight #00ff00 openEdit]
*/
static const char* HandleCmdCommand(HWND hwnd, const char* cmd, bool* ack) {
    AutoFreeStr cmdContent;
    const char* next = str::Parse(cmd, "[%s]", &cmdContent);
    if (!next) {
        return nullptr;
    }
    // cmdContent is the full content between [ and ]
    // it might be just "CmdClose" or "CmdCreateAnnotHighlight #00ff00 openEdit"
    // extract the command name (first space-delimited token)
    char* s = cmdContent.Get();
    char* spacePos = str::FindChar(s, ' ');
    TempStr name = s;
    if (spacePos) {
        name = str::DupTemp(s, (size_t)(spacePos - s));
    }

    int cmdId = GetCommandIdByName(name);
    if (cmdId < 0) {
        return nullptr;
    }
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        logfa("HandleCmdCommand: not executing DDE becaues MainWindow for hwnd 0x%p not found\n", hwnd);
        return nullptr;
    }

    // if there are arguments after the command name, create a custom command with those args
    int idToSend = cmdId;
    if (spacePos) {
        CustomCommand* customCmd = CreateCommandFromDefinition(cmdContent);
        if (customCmd) {
            idToSend = customCmd->id;
        }
    }

    logfa("HandleCmdCommand: sending %d (%s) command\n", idToSend, cmdContent.Get());
    SendMessageW(win->hwndFrame, WM_COMMAND, idToSend, 0);
    *ack = true;
    return next;
}

// returns true if did handle a message
static bool HandleExecuteCmds(HWND hwnd, const char* cmd) {
    gMostRecentlyOpenedDoc = nullptr;

    bool didHandle = false;
    while (!str::IsEmpty(cmd)) {
        {
            logf("HandleExecuteCmds: '%s'\n", cmd);
        }

        const char* nextCmd = HandleSyncCmd(cmd, &didHandle);
        if (!nextCmd) {
            nextCmd = HandleOpenCmd(cmd, &didHandle);
        }
        if (!nextCmd) {
            nextCmd = HandleGotoCmd(cmd, &didHandle);
        }
        if (!nextCmd) {
            nextCmd = HandlePageCmd(hwnd, cmd, &didHandle);
        }
        if (!nextCmd) {
            nextCmd = HandleSetViewCmd(cmd, &didHandle);
        }
        if (!nextCmd) {
            nextCmd = HandleSearchCmd(cmd, &didHandle);
        }
        if (!nextCmd) {
            nextCmd = HandleCmdCommand(hwnd, cmd, &didHandle);
        }
        if (!nextCmd) {
            nextCmd = HandleNewWindowCmd(cmd, &didHandle);
        }
        if (!nextCmd) {
            // forwards compatibility: ignore unknown commands (maybe from newer version)
            AutoFreeStr tmp;
            nextCmd = str::Parse(cmd, "%s]", &tmp);
        }
        cmd = nextCmd;
    }
    return didHandle;
}

static bool HandleRequestCmds(HWND hwnd, const char* cmd, StrBuilder& rsp) {
    bool didHandle = false;
    while (!str::IsEmpty(cmd)) {
        {
            logf("HandleRequestCmds: '%s'\n", cmd);
        }

        const char* nextCmd = HandleGetFileStateCmd(hwnd, cmd, &didHandle, rsp);
        if (!nextCmd) {
            AutoFreeStr tmp;
            nextCmd = str::Parse(cmd, "%s]", &tmp);
        }
        cmd = nextCmd;
    }
    return didHandle;
}

LRESULT OnDDERequest(HWND hwnd, WPARAM wp, LPARAM lp) {
    // window that is sending us the message
    HWND hwndClient = (HWND)wp;

    UINT fmt = LOWORD(lp);
    switch (fmt) {
        case CF_TEXT:
        case CF_UNICODETEXT:
            // we handle those
            break;
        default:
            logf("OnDDERequest: invalid fmt '%s'\n", (int)fmt);
            return 0;
    }
    ATOM a = HIWORD(lp);
    TempStr cmd = AtomToStrTemp(a);
    if (!cmd) {
        return 0;
    }

    StrBuilder str;
    bool didHandle = HandleRequestCmds(hwnd, cmd, str);
    if (!didHandle) {
        str.Set("error: unknoqn command");
    }

    void* data;
    int cbData;
    if (fmt == CF_TEXT) {
        data = (void*)str.Get();
        cbData = str.Size() + 1;
    } else if (fmt == CF_UNICODETEXT) {
        TempWStr tmp = ToWStrTemp(str.Get());
        data = (void*)tmp;
        cbData = (str::Leni(tmp) + 1) * 2;
    } else {
        ReportIf(true);
        return 0;
    }

    int cbDdeData = sizeof(DDEDATA);
    u8* res = (u8*)AllocZero(GetTempAllocator(), cbDdeData + cbData);
    DDEDATA* ddeData = (DDEDATA*)res;
    ddeData->fRelease = 1; // tell client to free HGLOBAL
    ddeData->cfFormat = fmt;
    memcpy(res + cbDdeData, data, cbData);

    HGLOBAL h = MemToHGLOBAL(res, cbDdeData + cbData, GMEM_MOVEABLE | GMEM_DDESHARE);
    LPARAM lpres = MAKELPARAM(h, a);
    PostMessageW(hwndClient, WM_DDE_DATA, (WPARAM)hwnd, lpres);
    return 0;
}

LRESULT OnDDExecute(HWND hwnd, WPARAM wp, LPARAM lp) {
    HWND hwndClient = (HWND)wp;
    HGLOBAL hCommand = (HGLOBAL)lp;
    bool isUnicode = IsWindowUnicode(hwndClient);

    TempStr cmd = HGLOBALToStrTemp((HGLOBAL)hCommand, isUnicode);
    bool didHandle = HandleExecuteCmds(hwnd, cmd);
    DDEACK ack{};
    ack.fAck = didHandle ? 1 : 0;
    LPARAM lpres = PackDDElParam(WM_DDE_ACK, *(WORD*)&ack, (UINT_PTR)hCommand);
    PostMessageW(hwndClient, WM_DDE_ACK, (WPARAM)hwnd, lpres);
    return 0;
}

LRESULT OnDDEInitiate(HWND hwnd, WPARAM wp, LPARAM lp) {
    ATOM aServer = GlobalAddAtom(kSumatraDdeServer);
    ATOM aTopic = GlobalAddAtom(kSumatraDdeTopic);

    if (LOWORD(lp) == aServer && HIWORD(lp) == aTopic) {
        SendMessageW((HWND)wp, WM_DDE_ACK, (WPARAM)hwnd, MAKELPARAM(aServer, 0));
    } else {
        GlobalDeleteAtom(aServer);
        GlobalDeleteAtom(aTopic);
    }
    return 0;
}

LRESULT OnDDETerminate(HWND hwnd, WPARAM wp, LPARAM) {
    PostMessageW((HWND)wp, WM_DDE_TERMINATE, (WPARAM)hwnd, 0L);
    return 0;
}

// Payload for async Open command carried in kCopyDataOpen WM_COPYDATA
struct OpenCopyDataAsync {
    char* path; // heap-allocated, freed by OpenCopyDataAsyncRun
    u32 newWindow;
};

static void OpenCopyDataAsyncRun(OpenCopyDataAsync* d) {
    // Pick a target window the same way HandleOpenCmd would, then kick off
    // the load on a worker thread. We stay off the UI thread for the heavy
    // bit so the sender (already returned from SendMessageW by now) never
    // had to wait on us in the first place.
    MainWindow* win = nullptr;
    if (d->newWindow) {
        MainWindow* emptyExistingWin = nullptr;
        for (auto& w : gWindows) {
            if (!w->HasDocsLoaded()) {
                emptyExistingWin = w;
                break;
            }
        }
        win = emptyExistingWin ? emptyExistingWin : CreateAndShowMainWindow(nullptr);
    } else {
        win = FindMainWindowByFile(d->path, true);
        if (!win) {
            win = FindMainWindowByHwnd(gLastActiveFrameHwnd);
        }
        if (!win && gWindows.Size() > 0) {
            win = gWindows[0];
        }
    }
    LoadArgs args(d->path, win);
    args.activateExisting = d->newWindow == 0;
    // Match the legacy DDE Open(..., setFocus=1) behavior used by
    // shell/reuseInstance launches: opening into an existing instance should
    // bring that window to the foreground.
    if (win) {
        win->Focus();
    }
    StartLoadDocument(&args);

    str::Free(d->path);
    delete d;
}

LRESULT OnCopyData(HWND hwnd, WPARAM wp, LPARAM lp) {
    COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lp;
    if (!cds || wp) {
        return FALSE;
    }

    if (cds->dwData == kCopyDataOpen) {
        // Simple-open fast path used by the reuseInstance handshake: the
        // sibling SumatraPDF that Explorer just spawned is blocked in
        // SendMessageW. Copy the path out, post an async task, return
        // immediately so the sender unblocks and exits.
        if (cds->cbData < sizeof(SumatraOpenCopyData) + 1) {
            return FALSE;
        }
        auto* data = (const SumatraOpenCopyData*)cds->lpData;
        const char* path = (const char*)(data + 1);
        size_t pathMax = cds->cbData - sizeof(SumatraOpenCopyData);
        // require null-terminator within bounds
        if (strnlen_s(path, pathMax) >= pathMax) {
            return FALSE;
        }
        auto* d = new OpenCopyDataAsync;
        d->path = str::Dup(path);
        d->newWindow = data->newWindow;
        auto fn = MkFunc0<OpenCopyDataAsync>(OpenCopyDataAsyncRun, d);
        uitask::Post(fn, "OnCopyData/Open");
        return TRUE;
    }

    if (cds->dwData == kCopyDataDdeW) {
        const WCHAR* cmdW = (const WCHAR*)cds->lpData;
        if (cmdW[cds->cbData / sizeof(WCHAR) - 1]) {
            return FALSE;
        }
        // Legacy DDE grammar — callers expect synchronous handling.
        TempStr cmd = ToUtf8Temp(cmdW);
        bool didHandle = HandleExecuteCmds(hwnd, cmd);
        return didHandle ? TRUE : FALSE;
    }

    return FALSE;
}

#if 0
bool RegisterDDeServer() {
    DWORD ddeInst = (DWORD)-1;
    auto err = DdeInitializeW(&ddeInst, nullptr, APPCMD_CLIENTONLY | CBF_FAIL_ADVISES, 0);
    if (err != DMLERR_NO_ERROR) {
        // Handle initialization error
        logf("RegisterDDeServer: DdeInitializeW() failed with '%d'\n", (int)err);
        return false;
    }
    return true;
}
#endif
