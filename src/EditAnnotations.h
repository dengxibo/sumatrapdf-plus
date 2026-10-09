/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct EditAnnotationsWindow;
struct MainWindow;
struct WindowTab;
struct Annotation;
struct StrBuilder;
struct DropDown;
enum class AnnotationType;
using PdfColor = uint64_t;

enum class EditAnnotFocus {
    Default,
    Edit,
    List,
};

void ShowEditAnnotationsWindow(WindowTab*, Annotation*, EditAnnotFocus focus = EditAnnotFocus::Default,
                               bool revealInSidebar = true);
HWND EditAnnotationsSidebarHwnd(WindowTab* tab);
bool AnnotationsSidebarIsShowing(WindowTab* tab);
void SyncEditAnnotationsSidebar(MainWindow* win, bool show);
bool CloseAndDeleteEditAnnotationsWindow(WindowTab*);
void DeleteAnnotationAndUpdateUI(WindowTab*, Annotation*);
void SetSelectedAnnotation(WindowTab*, Annotation*, bool isNew = false, EditAnnotFocus focus = EditAnnotFocus::Default);
void UpdateAnnotationsList(EditAnnotationsWindow*);
void NotifyAnnotationsChanged(EditAnnotationsWindow*);
void RefreshEditAnnotationsWindowsTheme();
void DockOpenEditAnnotationsWindows(MainWindow* win);
void CloseEditAnnotationsWindowsForDpiMove(MainWindow* win);
void ReopenEditAnnotationsWindowsAfterDpiMove(MainWindow* win);
bool PdfAnnotationsExportNotes(WindowTab* tab, HWND hwndParent);
void PaintPdfMarkupOverlayPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo);
void PaintPdfMarkupNoteBadgesPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo);
void ClearPdfMarkupOverlayForPage(WindowTab* tab, int pageNo);
void RemovePdfMarkupOverlayAnnot(WindowTab* tab, Annotation* annot);
void ClearPdfDeletedAnnotCovers(WindowTab* tab);
void ClearPdfDeletedAnnotCoversForPage(WindowTab* tab, int pageNo);
void PaintPdfDeletedAnnotCoversPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo);
bool IsPdfAnnotContentsEditFocused(HWND msgHwnd = nullptr);

const char* GetPdfAnnotationColorNames();
const char* GetKnownColorName(PdfColor c);
COLORREF ColorRefFromPdfAnnotationColor(PdfColor c);
PdfColor PdfAnnotationColorFromColorRef(COLORREF c);
void FillAnnotationColorDropDown(DropDown* w, COLORREF col, StrBuilder& customColor);
COLORREF GetAnnotationColorFromDropDown(const char* item);
COLORREF GetDefaultAnnotationColor(AnnotationType type);

bool StartFreeTextInPlaceEdit(MainWindow*, Annotation*);
bool StartFreeTextInPlaceEditAt(MainWindow*, Point);
bool IsEditingFreeTextInPlace(MainWindow* win = nullptr);
bool IsFreeTextInPlaceEditFocused(HWND hwnd = nullptr);
void EndFreeTextInPlaceEdit(bool accept);
void EndFreeTextInPlaceEditForTab(WindowTab*, bool accept);
void RepositionFreeTextInPlaceEdit(MainWindow*);
HBRUSH FreeTextInPlaceEditCtlColor(HWND, HDC);

struct EbookAnnotation;
bool StartEbookFreeTextInPlaceEdit(MainWindow*, EbookAnnotation*);
