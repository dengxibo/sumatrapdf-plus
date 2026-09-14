/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma once

#include "utils/BaseUtil.h"

class EngineBase;
struct MainWindow;
struct ExtractedTocItem;

void ShowTocExtraction(MainWindow* win);
bool ConfirmTocPageSend(HWND parent, const WCHAR* message);
// Matches only unambiguous internal links intersecting the source entry.
int ApplyTocLinkEvidence(EngineBase* engine, Vec<ExtractedTocItem*>& roots);
void PrepareTocExtractionResult(EngineBase* engine, Vec<ExtractedTocItem*>& roots);
bool PreviewTocExtraction(MainWindow* win, EngineBase* engine, Vec<ExtractedTocItem*>& roots, bool persistToDisk,
                          HWND parent = nullptr);
