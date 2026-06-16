/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
class EngineBase;
struct TextSelection;

bool ShowChineseWordLookupAt(MainWindow* win, TextSelection* ts, EngineBase* engine, int pageNo, PointF pagePt,
                             Point screenPos);
void ShowWordLookup(MainWindow* win, const char* word, Point screenPos);
void CloseWordLookup();
