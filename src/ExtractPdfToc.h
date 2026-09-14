/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

class EngineBase;
struct MainWindow;

struct EngineMupdfPageLine {
    char* text = nullptr;
    float x = 0;
    float y = 0;
    float dx = 0;
    float dy = 0;
    float fontSize = 0;
    bool bold = false;
};

// Shared document line for TOC extractors. Recognition strategy is not shared.
struct ScanLine {
    char* text = nullptr;
    int srcPage = 0;
    float x = 0;
    float y = 0;
    float dx = 0;
    float dy = 0;
    float fontSize = 0;
    bool bold = false;
};

// Shared progress/cancel context for asynchronous TOC extraction. The worker
// thread reports per-stage page counts; the UI thread shows them as a
// notification. hwnd may be null (CLI runs); cancelSeq == 0 disables
// cancellation checks.
struct TocExtractProgress {
    HWND hwnd = nullptr;
    LONG cancelSeq = 0;
};

bool TocExtractCancelled(const TocExtractProgress* prog);
// done/total cover the pages of the current stage; bodyPhase switches the
// notification text to the body-analysis message (the printed-TOC page scan
// reports through the "Extracting bookmarks…" message).
void TocExtractReportProgress(const TocExtractProgress* prog, int done, int total, bool bodyPhase);

enum class ExtractedTocSource {
    Unknown = 0,
    PrintedToc = 1,
    BodyInference = 2,
};

// Where the destination (page/x/y) of an extracted TOC entry came from.
enum class TocDestinationSource {
    Unknown = 0,
    Estimated = 1,  // printed page number mapped by offset, no body/link proof
    BodyMatch = 2,  // title text found on the target body page
    PdfLink = 3,    // clickable GoTo link on the printed Contents page
};

struct ExtractedTocItem {
    char* title = nullptr;
    char* rawTitle = nullptr;
    int pageNo = 0;
    float x = 0;
    float y = 0;
    int level = 1;
    int confidence = 0;
    ExtractedTocSource source = ExtractedTocSource::Unknown;
    TocDestinationSource destinationSource = TocDestinationSource::Unknown;
    int printedPage = 0;
    char* printedLabel = nullptr;
    int tocPageNo = 0;
    float tocX = 0;
    float tocY = 0;
    bool verified = false;
    bool bodyMatched = false;
    bool expanded = true;
    ExtractedTocItem* parent = nullptr;
    void* treeHandle = nullptr;
    Vec<ExtractedTocItem*> children;

    ~ExtractedTocItem();
};

void NormalizeTocNumberingDotsHalfwidth(char** titleOut);
// Rewrites the leading ordinal parentheses (e.g. "(一)" / "〈一〉" as read by
// OCR) to the GB/T 9704 fullwidth form "（一）". Titles without a paren
// ordinal prefix are left untouched.
void NormalizeTocNumberingParens(char** titleOut);
bool ExtractedHasPrintedBookCalib(const Vec<ExtractedTocItem*>& roots);
void FlattenExtractedTocItems(const Vec<ExtractedTocItem*>& nodes, Vec<ExtractedTocItem*>& flat);

void DeleteExtractedTocItems(Vec<ExtractedTocItem*>& roots);
void EngineMupdfFreePageLines(Vec<EngineMupdfPageLine>& lines);
bool EngineMupdfCollectPageLines(EngineBase* engine, int pageNo, Vec<EngineMupdfPageLine>& linesOut);
bool EngineMupdfHasStoredOutline(EngineBase* engine);
bool EngineMupdfReplacePdfToc(EngineBase* engine, Vec<ExtractedTocItem*>& roots, char** errorOut);

// Text-layer ScanLine collection for one page (shared with the second-pass
// body-anchor resolution in ExtractBookToc.cpp).
void PtocCollectPageScanLines(EngineBase* engine, int pageNo, Vec<ScanLine>& out);
// OCR and collect a page that contributed no usable file-text ScanLines. Used
// only after the book parser has established that the page lies inside a
// printed Contents span; do not use this as a general document-wide OCR pass.
bool PtocOcrAndCollectPageScanLines(EngineBase* engine, int pageNo, Vec<ScanLine>& out);
void PtocFindCachedTocPages(EngineBase* engine, Vec<int>& pageNosOut, bool includePrecisePages = false,
                            bool recognizeMissingPages = false, bool (*isCanceled)(void*) = nullptr,
                            void* cancelContext = nullptr, void (*onProgress)(void*, int, int) = nullptr,
                            void* progressContext = nullptr);
void PtocFreeScanLines(Vec<ScanLine>& lines);

enum class ExtractPdfTocKind {
    Ok,
    NoText,
    NoHeadings,
    Failed
};
ExtractPdfTocKind ExtractPdfTocFromEngine(EngineBase* engine, Vec<ExtractedTocItem*>& roots, int* nItemsOut);

bool WriteExtractedPdfToc(MainWindow* win, EngineBase* engine, Vec<ExtractedTocItem*>& roots, bool persistToDisk);
bool HandleExtractPdfTocCommand(MainWindow* win, bool skipConfirm = false, bool persistToDisk = true);
void CancelExtractPdfToc();
bool ExtractPdfTocIsRunning();

// If the stored PDF outline is debris from an older extract (dot-leader
// titles, leaked 目录 heading, cover attachment lists), re-extract in memory
// (never rewrites the file). Call once after the TOC tree is loaded.
void MaybeRebuildStaleOfficialPdfToc(MainWindow* win);
