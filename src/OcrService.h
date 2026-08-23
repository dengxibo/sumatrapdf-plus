/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
class EngineBase;

bool OcrEngineKindSupported(EngineBase* engine);
bool OcrDocumentHasFileTextLayer(EngineBase* engine);
bool OcrPageLooksScanned(EngineBase* engine, int pageNo);
bool OcrRecognizeEnginePage(EngineBase* engine, int pageNo, bool forceOcr = false);
void OcrEnsurePageTextForSearch(EngineBase* engine, int pageNo);
void OcrScheduleForPage(MainWindow* win, int pageNo);
void OcrScheduleForPage(MainWindow* win, int pageNo, bool ignoreAutoPref);
void OcrScheduleDocument(MainWindow* win, bool extractTocIfMissing = false, bool forceOcrAll = false);
void OcrSaveSearchablePdfAfterOcr(MainWindow* win, const char* destPath, bool extractTocWhenDone = false);
bool OcrSaveCachedSearchablePdf(MainWindow* win, const char* destPath);
void OcrExtractTocAfterDocumentOcr(MainWindow* win);
void OcrCancelQueued(MainWindow* win);
void OcrCancelForEngine(EngineBase* engine);
bool OcrHasQueuedJobs();
void OcrBeginRegionSelect(MainWindow* win);
void OcrCancelRegionSelect(MainWindow* win);
void OcrFinishRegionSelect(MainWindow* win, Rect screenRect);
void OcrNotifyMissingModels(HWND hwndCanvas, bool always = false);
