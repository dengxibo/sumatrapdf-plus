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

void ShowEditAnnotationsWindow(WindowTab*, Annotation*, EditAnnotFocus focus = EditAnnotFocus::Default);
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
void ClearPdfMarkupOverlayForPage(WindowTab* tab, int pageNo);
void RemovePdfMarkupOverlayAnnot(WindowTab* tab, Annotation* annot);
bool IsPdfAnnotContentsEditFocused(HWND msgHwnd = nullptr);

const char* GetPdfAnnotationColorNames();
const char* GetKnownColorName(PdfColor c);
COLORREF ColorRefFromPdfAnnotationColor(PdfColor c);
PdfColor PdfAnnotationColorFromColorRef(COLORREF c);
void FillAnnotationColorDropDown(DropDown* w, COLORREF col, StrBuilder& customColor);
COLORREF GetAnnotationColorFromDropDown(const char* item);
COLORREF GetDefaultAnnotationColor(AnnotationType type);
