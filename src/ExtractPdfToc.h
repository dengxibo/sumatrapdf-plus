/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

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

enum class ExtractedTocSource {
    Unknown = 0,
    PrintedToc = 1,
    BodyInference = 2,
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
bool ExtractedHasPrintedBookCalib(const Vec<ExtractedTocItem*>& roots);
void FlattenExtractedTocItems(const Vec<ExtractedTocItem*>& nodes, Vec<ExtractedTocItem*>& flat);

void DeleteExtractedTocItems(Vec<ExtractedTocItem*>& roots);
void EngineMupdfFreePageLines(Vec<EngineMupdfPageLine>& lines);
bool EngineMupdfCollectPageLines(EngineBase* engine, int pageNo, Vec<EngineMupdfPageLine>& linesOut);
bool EngineMupdfHasStoredOutline(EngineBase* engine);
bool EngineMupdfReplacePdfToc(EngineBase* engine, Vec<ExtractedTocItem*>& roots, char** errorOut);

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
