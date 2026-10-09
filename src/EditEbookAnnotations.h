/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct EbookAnnotation;
struct EbookAnnotationsWindow;
struct WindowTab;
struct MainWindow;
enum class EditAnnotFocus;

void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation = nullptr);
void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation, EditAnnotFocus focus,
                                    bool revealInSidebar = true);
HWND EbookAnnotationsSidebarHwnd(WindowTab* tab);
void SyncEbookAnnotationsSidebar(MainWindow* win, bool show);
bool CloseAndDeleteEditEbookAnnotationsWindow(WindowTab* tab);
void FlushEbookAnnotationEdits(WindowTab* tab);
void UpdateEbookAnnotationsList(EbookAnnotationsWindow* window, EbookAnnotation* preferredSelection = nullptr);
void SyncEbookFreeTextDraft(WindowTab* tab, EbookAnnotation* annotation, const char* text);
void RefreshEbookAnnotationExcerpts(WindowTab* tab, bool loading);
void ClearSelectedEbookAnnotation(WindowTab* tab);
void RefreshEbookAnnotationsWindowsTheme();
void DockOpenEbookAnnotationsWindows(MainWindow* win);
void CloseEbookAnnotationsWindowsForDpiMove(MainWindow* win);
void ReopenEbookAnnotationsWindowsAfterDpiMove(MainWindow* win);
bool IsEbookAnnotContentsEditFocused(HWND msgHwnd = nullptr);
