/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

void CreateToc(MainWindow*);
void ReCreateTocFilterEdit(MainWindow*, HFONT font);
void ReCreateTocTreeView(MainWindow*, HFONT font, int dpi);
void ClearTocBox(MainWindow*);
void ToggleTocBox(MainWindow*);
void LoadTocTree(MainWindow*);
void UpdateTocSelection(MainWindow*, int currPageNo);
void InvalidateTocTree(MainWindow* win);
void UpdateTocExpansionState(Vec<int>& tocState, TreeView*, TocTree*);
int CountTocItems(TocItem* item);
TocItem* TocItemBestMatchForPage(TocItem* item, int pageNo, EngineBase* engine);
void UnsubclassToc(MainWindow*);
void TocFilterChanged(MainWindow*);
bool HandlePdfTocEditCommand(MainWindow*, int commandId);
bool TryAddPdfTocFromSelection(MainWindow*);

// shared with Favorites.cpp
// void TocCustomizeTooltip(TreeItem::GetTooltipEvent*);
// LRESULT TocTreeKeyDown2(TreeKeyDownEvent*);

// void TocTreeCharHandler(CharEvent* ev);
// void TocTreeMouseWheelHandler(MouseWheelEvent* ev);
