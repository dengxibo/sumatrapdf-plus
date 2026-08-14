/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

void CreateToc(MainWindow*);
bool TreeWrapLabelsEnabled();
void TreeWrapLabelsConfigureCreateArgs(TreeView::CreateArgs& args);
void TreeItemTooltipIfTruncated(TreeView::GetTooltipEvent* ev);
void FavTreeWrapOnCustomDraw(TreeView::CustomDrawEvent* ev);
void FavTreeWrapRecalcHeights(MainWindow* win);
void ScheduleTocTreeWrapHeights(MainWindow* win);
void ScheduleFavTreeWrapHeights(MainWindow* win);
void FlushTocTreeWrapHeights(MainWindow* win);
void FlushFavTreeWrapHeights(MainWindow* win);
void SuspendTreeWrapLiveResize();
void SuspendTreeWrapLiveResizeForWindow(MainWindow* win);
void ResumeTreeWrapLiveResizeAndFlush(MainWindow* win);
// Shared by TOC/favorites host WM_TIMER handlers (debounce wrap-height recalc).
constexpr UINT_PTR kTreeWrapHeightTimerId = 0x7151;
void ReCreateTocFilterEdit(MainWindow*, HFONT font);
void UpdateTocFilterForDocumentLoading(MainWindow* win);
void RelayoutTocContainer(MainWindow* win);
void ReCreateTocTreeView(MainWindow*, HFONT font, int dpi);
void ClearTocBox(MainWindow*);
void ClearTocBoxForTabSwitch(MainWindow*);
void RestoreTocTreeForTab(MainWindow*);
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
