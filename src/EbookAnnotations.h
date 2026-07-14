/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct DisplayModel;
struct EbookAnnotation;
struct EbookAnnotations;
struct WindowTab;
enum class AnnotationType;

bool EbookAnnotationsSupported(WindowTab* tab);
EbookAnnotation* EbookAnnotationsCreateFromSelection(WindowTab* tab, AnnotationType type, COLORREF color);
EbookAnnotation* EbookAnnotationsCreateText(WindowTab* tab, DisplayModel* dm, Point canvasPoint, COLORREF color);
EbookAnnotation* EbookAnnotationsGetAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint);
bool EbookAnnotationsHitTest(WindowTab* tab, DisplayModel* dm, Point canvasPoint);
bool EbookAnnotationsDeleteAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint);
bool EbookAnnotationsDelete(WindowTab* tab, EbookAnnotation* annotation);
void EbookAnnotationsGetAll(WindowTab* tab, Vec<EbookAnnotation*>& annotationsOut);
AnnotationType EbookAnnotationGetType(EbookAnnotation* annotation);
const char* EbookAnnotationGetText(EbookAnnotation* annotation);
const char* EbookAnnotationGetNote(EbookAnnotation* annotation);
const char* EbookAnnotationGetAuthor(EbookAnnotation* annotation);
time_t EbookAnnotationGetCreated(EbookAnnotation* annotation);
time_t EbookAnnotationGetModified(EbookAnnotation* annotation);
COLORREF EbookAnnotationGetColor(EbookAnnotation* annotation);
bool EbookAnnotationSetNote(WindowTab* tab, EbookAnnotation* annotation, const char* note);
bool EbookAnnotationSetColor(WindowTab* tab, EbookAnnotation* annotation, COLORREF color);
int EbookAnnotationGetPageNo(WindowTab* tab, EbookAnnotation* annotation);
bool EbookAnnotationsExportNotes(WindowTab* tab, HWND hwndParent);
void EbookAnnotationsPaintPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo);
void PaintTextMarkupOverlay(HDC hdc, Rect canvasRc, AnnotationType type, COLORREF color, Vec<Rect>& screenRects);
// Drop cached page/anchor maps after reflow or theme changes.
void EbookAnnotationsInvalidateLayoutCaches(WindowTab* tab);
void EbookAnnotationsFree(EbookAnnotations* annotations);
