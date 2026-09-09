/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "OcrOnnx.h"

#include <windows.h>

struct MainWindow;
struct WindowTab;
class EngineBase;

bool OcrEngineKindSupported(EngineBase* engine);
// Auto OCR switch for the current tab. Toolbar checked state and the auto
// scheduling gates all read this; never use it as a feature-availability test.
bool OcrAutoEnabled(MainWindow* win);
void ApplyAutoOcrDefaultForTab(WindowTab* tab);
bool OcrDeferExtractUntilDocumentReady(MainWindow* win, bool persistToDisk);
bool OcrDocumentHasFileTextLayer(EngineBase* engine);
bool OcrPageLooksScanned(EngineBase* engine, int pageNo);
// tQueued: T1 timestamp (when the request was queued) for latency logs; pass
// a zero LARGE_INTEGER when the caller does not track queueing.
bool OcrRecognizeEnginePage(EngineBase* engine, int pageNo, bool forceOcr = false,
                            OcrOperation op = OcrOperation::CurrentPage, LARGE_INTEGER tQueued = {});
void OcrEnsurePageTextForSearch(EngineBase* engine, int pageNo);
void OcrScheduleForPage(MainWindow* win, int pageNo);
void OcrScheduleForPage(MainWindow* win, int pageNo, bool ignoreAutoPref);
void OcrScheduleDocument(MainWindow* win, bool extractTocIfMissing = false, bool forceOcrAll = false);
void OcrRerunAllPages(MainWindow* win, bool accurate);
void OcrSaveSearchablePdfAfterOcr(MainWindow* win, const char* destPath, bool extractTocWhenDone = false);
bool OcrSaveCachedSearchablePdf(MainWindow* win, const char* destPath);
void OcrExtractTocAfterDocumentOcr(MainWindow* win);
void OcrCancelQueued(MainWindow* win, bool quiet = false);
void OcrCancelForEngine(EngineBase* engine);
bool OcrHasQueuedJobs();
bool OcrPageIsPending(EngineBase* engine, int pageNo);
void OcrBeginRegionSelect(MainWindow* win);
void OcrCancelRegionSelect(MainWindow* win);
void OcrFinishRegionSelect(MainWindow* win, Rect screenRect);
void OcrNotifyMissingModels(HWND hwndCanvas, bool always = false);
int OcrRunFileBenchmark(const char* pdfPath, const char* outDir, int maxPages);
