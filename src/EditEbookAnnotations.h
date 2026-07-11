/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct EbookAnnotation;
struct EbookAnnotationsWindow;
struct WindowTab;
struct MainWindow;
enum class EditAnnotFocus;

void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation = nullptr);
void ShowEditEbookAnnotationsWindow(WindowTab* tab, EbookAnnotation* annotation, EditAnnotFocus focus);
bool CloseAndDeleteEditEbookAnnotationsWindow(WindowTab* tab);
void UpdateEbookAnnotationsList(EbookAnnotationsWindow* window);
void RefreshEbookAnnotationsWindowsTheme();
void DockOpenEbookAnnotationsWindows(MainWindow* win);
void CloseEbookAnnotationsWindowsForDpiMove(MainWindow* win);
void ReopenEbookAnnotationsWindowsAfterDpiMove(MainWindow* win);
