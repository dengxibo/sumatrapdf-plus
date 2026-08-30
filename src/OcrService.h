/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "OcrOnnx.h"

struct MainWindow;
class EngineBase;

bool OcrEngineKindSupported(EngineBase* engine);
bool OcrAutoEnabled(EngineBase* engine);
bool OcrDeferExtractUntilDocumentReady(MainWindow* win, bool persistToDisk);
bool OcrDocumentHasFileTextLayer(EngineBase* engine);
bool OcrPageLooksScanned(EngineBase* engine, int pageNo);
bool OcrRecognizeEnginePage(EngineBase* engine, int pageNo, bool forceOcr = false,
                            OcrOperation op = OcrOperation::CurrentPage);
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
void OcrBeginRegionSelect(MainWindow* win);
void OcrCancelRegionSelect(MainWindow* win);
void OcrFinishRegionSelect(MainWindow* win, Rect screenRect);
void OcrNotifyMissingModels(HWND hwndCanvas, bool always = false);
int OcrRunFileBenchmark(const char* pdfPath, const char* outDir, int maxPages);
