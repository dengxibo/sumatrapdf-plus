/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct DisplayModel;
struct EbookAnnotation;
struct EbookAnnotations;
struct WindowTab;
enum class AnnotationType;

bool EbookAnnotationsSupported(WindowTab* tab);
EbookAnnotation* EbookAnnotationsCreateFromSelection(WindowTab* tab, AnnotationType type, COLORREF color);
EbookAnnotation* EbookAnnotationsCreateAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint, AnnotationType type,
                                          COLORREF color);
EbookAnnotation* EbookAnnotationsCreateDragShape(WindowTab* tab, DisplayModel* dm, Point canvasStart,
                                                   Point canvasEnd, AnnotationType type);
EbookAnnotation* EbookAnnotationsCreateInkStroke(WindowTab* tab, DisplayModel* dm, int pageNo, PointF* points,
                                                 int nPoints, COLORREF color);
EbookAnnotation* EbookAnnotationsCreateText(WindowTab* tab, DisplayModel* dm, Point canvasPoint, COLORREF color);
EbookAnnotation* EbookAnnotationsGetAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint);
bool EbookAnnotationsHitTest(WindowTab* tab, DisplayModel* dm, Point canvasPoint);
bool EbookAnnotationsDeleteAt(WindowTab* tab, DisplayModel* dm, Point canvasPoint);
bool EbookAnnotationsDelete(WindowTab* tab, EbookAnnotation* annotation);
bool EbookAnnotationGetPageBounds(WindowTab* tab, DisplayModel* dm, EbookAnnotation* annotation, int* pageNoOut,
                                  RectF* boundsOut);
bool EbookAnnotationSetPageBounds(WindowTab* tab, DisplayModel* dm, EbookAnnotation* annotation, int pageNo,
                                  RectF bounds, bool save);
void EbookAnnotationsGetAll(WindowTab* tab, Vec<EbookAnnotation*>& annotationsOut);
AnnotationType EbookAnnotationGetType(EbookAnnotation* annotation);
const char* EbookAnnotationGetText(EbookAnnotation* annotation);
const char* EbookAnnotationGetNote(EbookAnnotation* annotation);
const char* EbookAnnotationGetIcon(EbookAnnotation* annotation);
const char* EbookAnnotationGetAuthor(EbookAnnotation* annotation);
time_t EbookAnnotationGetCreated(EbookAnnotation* annotation);
time_t EbookAnnotationGetModified(EbookAnnotation* annotation);
COLORREF EbookAnnotationGetColor(EbookAnnotation* annotation);
int EbookAnnotationGetOpacity(EbookAnnotation* annotation);
bool EbookAnnotationSetNote(WindowTab* tab, EbookAnnotation* annotation, const char* note);
bool EbookAnnotationSetIcon(WindowTab* tab, EbookAnnotation* annotation, const char* icon);
bool EbookAnnotationSetColor(WindowTab* tab, EbookAnnotation* annotation, COLORREF color);
bool EbookAnnotationSetOpacity(WindowTab* tab, EbookAnnotation* annotation, int opacity);
int EbookAnnotationGetFreeTextAlignment(EbookAnnotation* annotation);
const char* EbookAnnotationGetFreeTextFont(EbookAnnotation* annotation);
int EbookAnnotationGetFreeTextSize(EbookAnnotation* annotation);
int EbookAnnotationGetFreeTextBorderWidth(EbookAnnotation* annotation);
bool EbookAnnotationGetFreeTextBackground(EbookAnnotation* annotation, COLORREF* colorOut);
bool EbookAnnotationSetFreeTextAlignment(WindowTab* tab, EbookAnnotation* annotation, int alignment);
bool EbookAnnotationSetFreeTextFont(WindowTab* tab, EbookAnnotation* annotation, const char* font);
bool EbookAnnotationSetFreeTextSize(WindowTab* tab, EbookAnnotation* annotation, int size);
bool EbookAnnotationSetFreeTextBorderWidth(WindowTab* tab, EbookAnnotation* annotation, int width);
bool EbookAnnotationSetFreeTextBackground(WindowTab* tab, EbookAnnotation* annotation, bool transparent,
                                          COLORREF color);
int EbookAnnotationGetBorderWidth(EbookAnnotation* annotation);
int EbookAnnotationGetLineStart(EbookAnnotation* annotation);
int EbookAnnotationGetLineEnd(EbookAnnotation* annotation);
bool EbookAnnotationGetInteriorColor(EbookAnnotation* annotation, COLORREF* colorOut);
bool EbookAnnotationSetBorderWidth(WindowTab* tab, EbookAnnotation* annotation, int width);
bool EbookAnnotationSetLineEnds(WindowTab* tab, EbookAnnotation* annotation, int start, int end);
bool EbookAnnotationSetInteriorColor(WindowTab* tab, EbookAnnotation* annotation, bool transparent, COLORREF color);
int EbookAnnotationGetPageNo(WindowTab* tab, EbookAnnotation* annotation);
bool EbookAnnotationsExportNotes(WindowTab* tab, HWND hwndParent);
void EbookAnnotationsPaintPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo);
void PaintTextMarkupOverlay(HDC hdc, Rect canvasRc, AnnotationType type, COLORREF color, Vec<Rect>& screenRects,
                            int opacity = 100);
// Drop cached page/anchor maps after reflow or theme changes.
void EbookAnnotationsInvalidateLayoutCaches(WindowTab* tab);
void EbookAnnotationsFree(EbookAnnotations* annotations);
