/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
enum class TbIcon;

void CreateToolbar(MainWindow*);
void ReCreateToolbar(MainWindow* win);
void ToolbarUpdateStateForWindow(MainWindow*, bool setButtonsVisibility);
void UpdateToolbarButtonsToolTipsForWindow(MainWindow*);
void UpdateToolbarFindText(MainWindow*);
void UpdateToolbarPageText(MainWindow*, int pageCount, bool updateOnly = false);
void UpdateFindbox(MainWindow*);
void SetToolbarButtonEnableState(MainWindow*, int cmdId, bool isEnabled);
void SetToolbarButtonCheckedState(MainWindow*, int cmdId, bool isChecked);
bool ShouldShowToolbar(MainWindow*);
void ShowOrHideToolbar(MainWindow*);
void UpdateAnnotToolToolbarButtons(MainWindow* win);
void UpdateToolbarState(MainWindow*);
void UpdateToolbarAfterThemeChange(MainWindow*);
HIMAGELIST BuildStdToolbarImageList(int dx);
void DrawSvgIcon(HDC hdc, const Rect& dest, TbIcon icon, COLORREF fgCol, COLORREF bgCol);
Rect GetToolbarButtonScreenRect(MainWindow*, int cmdId);
void UpdateDoubleClickWordLookupToolbarButton(MainWindow*);
void UpdateAutoOcrToolbarButton(MainWindow*);
void UpdateFullscreenToolbarButton(MainWindow*);
void UpdatePdfDocumentColorModeToolbarButton(MainWindow*);
bool NeedsDocumentColorModeUI(MainWindow* win);
bool NeedsPdfDocumentColorModeUI(MainWindow* win);

// Custom-draw a flat toolbar button (suppresses default white checked chrome).
// bgCol is the toolbar background; returns flags for NM_CUSTOMDRAW ITEMPREPAINT.
LRESULT PrepaintFlatToolbarItem(NMTBCUSTOMDRAW* custDraw, COLORREF bgCol);

void CreateMenuBarRebar(MainWindow*);
void DestroyMenuBarRebar(MainWindow*);
void ShowMenuBarRebar(MainWindow*);
void RebuildMenuBarButtons(MainWindow*);
bool IsShowingMenuBarRebar(MainWindow*);
bool HandleMenuBarCommand(MainWindow*, int cmdId);
bool ActivateMenuBarByAccel(MainWindow*, WCHAR accel);
void UpdateCustomMenuBarMenuSelect(MainWindow*, WPARAM, LPARAM);
