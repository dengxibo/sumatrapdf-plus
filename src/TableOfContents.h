/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

void CreateToc(MainWindow*);
bool TreeWrapLabelsEnabled();
void TreeWrapLabelsConfigureCreateArgs(TreeView::CreateArgs& args);
void TreeItemTooltipIfTruncated(TreeView::GetTooltipEvent* ev);
void FavTreeWrapOnCustomDraw(TreeView::CustomDrawEvent* ev);
// Theme selection is painted after custom draw. Call after the tree's WM_PAINT
// so hover and selection match the bookmarks column.
void PaintFavTreeRowsOverTheme(HWND hwnd, HDC hdc);
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
void ClearTocBox(MainWindow*, bool preserveFilter = false);
void ClearTocBoxForTabSwitch(MainWindow*);
void RestoreTocTreeForTab(MainWindow*);
void ToggleTocBox(MainWindow*);
void LoadTocTree(MainWindow*);
// Dark-theme selection box: the live-resize scrollbar mask covers the tree's
// right stroke. Paint that stroke on the mask's left edge so the box stays closed.
void PaintTocSelectionEdgeOnScrollbarMask(HWND mask, HDC hdc);
void ReloadPdfTocTree(MainWindow* win);

// Capture TOC expand + first-visible before ClearTocBox/LoadTocTree; restore after.
struct TocTreeViewKeep;
TocTreeViewKeep* TocTreeViewKeepStart(MainWindow* win);
void TocTreeViewKeepFinish(MainWindow* win, TocTreeViewKeep* keep);
void UpdateTocSelection(MainWindow*, int currPageNo);
void InvalidateTocTree(MainWindow* win);
bool TocSidebarShowsEmptyHint(MainWindow* win);
void UpdateTocExpansionState(Vec<int>& tocState, TreeView*, TocTree*);
int CountTocItems(TocItem* item);
TocItem* TocItemBestMatchForPage(TocItem* item, int pageNo, EngineBase* engine);
void UnsubclassToc(MainWindow*);
void TocFilterChanged(MainWindow*);
bool HandlePdfTocEditCommand(MainWindow*, int commandId);
bool HandlePdfTocSetCurrentPage(MainWindow*);
bool HandlePdfTocFindInBody(MainWindow*);
bool TryAddPdfTocFromSelection(MainWindow*);
bool TryReplacePdfTocFromSelection(MainWindow*);

// shared with Favorites.cpp
// void TocCustomizeTooltip(TreeItem::GetTooltipEvent*);
// LRESULT TocTreeKeyDown2(TreeKeyDownEvent*);

// void TocTreeCharHandler(CharEvent* ev);
// void TocTreeMouseWheelHandler(MouseWheelEvent* ev);
