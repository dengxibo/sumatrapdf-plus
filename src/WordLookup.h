/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
class EngineBase;
struct TextSelection;
struct DisplayModel;

bool ShowChineseWordLookupAt(MainWindow* win, TextSelection* ts, EngineBase* engine, int pageNo, PointF pagePt,
                             Point screenPos);
bool ShowEbookWordLookupAt(MainWindow* win, DisplayModel* dm, int pageNo, PointF pagePt, Point screenPos);
void ShowWordLookup(MainWindow* win, const char* word, Point screenPos);
void CloseWordLookup();
bool IsWordLookupVisible();
void RefreshWordLookupTheme();
bool IsOfflineDictionaryAvailable();
bool CanLookupSelectionInTab(WindowTab* tab);
bool LookupSelectionInTab(MainWindow* win, WindowTab* tab);
