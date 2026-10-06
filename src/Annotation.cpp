/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "utils/FileUtil.h"

#include "wingui/UIModels.h"

#include "Annotation.h"
#include "Settings.h"
#include "Theme.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineMupdf.h"
#include "GlobalPrefs.h"
#include "Commands.h"
#include "LookupAudio.h"
#include "PdfPageAudio.h"

#include "utils/Log.h"

/*
Vec<RectF> GetQuadPointsAsRect(Annotation*);
time_t CreationDate(Annotation*);

const char* AnnotationName(AnnotationType);
*/

// spot checks the definitions are the same
static_assert((int)AnnotationType::Link == (int)PDF_ANNOT_LINK);
static_assert((int)AnnotationType::ThreeD == (int)PDF_ANNOT_3D);
static_assert((int)AnnotationType::Sound == (int)PDF_ANNOT_SOUND);
static_assert((int)AnnotationType::Unknown == (int)PDF_ANNOT_UNKNOWN);

// clang-format off
const char* gAnnotationTextIcons = "Comment\0Caret\0Help\0Insert\0Key\0NewParagraph\0Note\0Paragraph\0";
const char* gStampIcons =
    "Approved\0AsIs\0Confidential\0Departmental\0Draft\0Experimental\0Expired\0Final\0ForComment\0"
    "ForPublicRelease\0NotApproved\0NotForPublicRelease\0Sold\0TopSecret\0";
// clang-format on

static char gLastStampIcon[32] = "Final";

void RememberStampIconName(const char* name) {
    if (str::IsEmpty(name)) {
        return;
    }
    if (seqstrings::StrToIdxIS(gStampIcons, name) < 0) {
        return;
    }
    str::BufSet(gLastStampIcon, dimof(gLastStampIcon), name);
}

const char* DefaultStampIconName() {
    if (str::IsEmpty(gLastStampIcon)) {
        return "Final";
    }
    return gLastStampIcon;
}

SizeF GetDefaultStampSize() {
    // Display-space size for click-to-place. Wide enough to read on a form;
    // appearance writer keeps it horizontal on /Rotate 90/270 pages.
    return {280, 74};
}

// clang format-off

#if 0
// must match the order of enum class AnnotationType
static const char* gAnnotNames =
    "Text\0"
    "Link\0"
    "FreeText\0"
    "Line\0"
    "Square\0"
    "Circle\0"
    "Polygon\0"
    "PolyLine\0"
    "Highlight\0"
    "Underline\0"
    "Squiggly\0"
    "StrikeOut\0"
    "Redact\0"
    "Stamp\0"
    "Caret\0"
    "Ink\0"
    "Popup\0"
    "FileAttachment\0"
    "Sound\0"
    "Movie\0"
    "RichMedia\0"
    "Widget\0"
    "Screen\0"
    "PrinterMark\0"
    "TrapNet\0"
    "Watermark\0"
    "3D\0"
    "Projection\0";
#endif

static const char* gAnnotReadableNames =
    "Text\0"
    "Link\0"
    "Free Text\0"
    "Line\0"
    "Square\0"
    "Circle\0"
    "Polygon\0"
    "Poly Line\0"
    "Highlight\0"
    "Underline\0"
    "Squiggly\0"
    "StrikeOut\0"
    "Redact\0"
    "Stamp\0"
    "Caret\0"
    "Ink\0"
    "Popup\0"
    "File Attachment\0"
    "Sound\0"
    "Movie\0"
    "RichMedia\0"
    "Widget\0"
    "Screen\0"
    "Printer Mark\0"
    "Trap Net\0"
    "Watermark\0"
    "3D\0"
    "Projection\0";
// clang format-on

/*
const char* AnnotationName(AnnotationType tp) {
    int n = (int)tp;
    ReportIf(n < -1 || n > (int)AnnotationType::ThreeD);
    if (n < 0) {
        return "Unknown";
    }
    const char* s = seqstrings::IdxToStr(gAnnotNames, n);
    ReportIf(!s);
    return s;
}
*/

static bool gDebugAnnotDestructor = false;
Annotation::~Annotation() {
    if (gDebugAnnotDestructor) {
        logf("deleting an annotation\n");
    }
}

TempStr AnnotationReadableNameTemp(AnnotationType tp) {
    int n = (int)tp;
    if (n < 0) {
        return (char*)"Unknown";
    }
    char* s = (char*)seqstrings::IdxToStr(gAnnotReadableNames, n);
    ReportIf(!s);
    return s;
}

AnnotationType Type(Annotation* annot) {
    ReportIf((int)annot->type < 0);
    return annot->type;
}

int PageNo(Annotation* annot) {
    ReportIf(annot->pageNo < 1);
    return annot->pageNo;
}

RectF GetBounds(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    fz_rect rc = {};

    fz_try(ctx) {
        rc = pdf_bound_annot(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("GetBounds(): pdf_bound_annot() failed\n");
    }
    annot->bounds = ToRectF(rc);
    return annot->bounds;
}

RectF GetRect(Annotation* annot) {
    return annot->bounds;
}

HBITMAP RenderAnnotationPreviewBitmap(Annotation* annot, float zoom, int rotation) {
    if (!annot || !annot->engine || !annot->pdfannot || zoom <= 0.f) {
        return nullptr;
    }
    EngineMupdf* e = annot->engine;
    fz_context* ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    fz_pixmap* pix = nullptr;
    HBITMAP hbmp = nullptr;
    fz_var(pix);
    fz_try(ctx) {
        fz_matrix ctm = e->viewctm(annot->pageNo, zoom, rotation);
        pix = pdf_new_pixmap_from_annot(ctx, annot->pdfannot, ctm, fz_device_rgb(ctx), nullptr, 1);
        if (pix && pix->n >= 2 && pix->alpha) {
            int w = fz_pixmap_width(ctx, pix);
            int h = fz_pixmap_height(ctx, pix);
            int comps = pix->n - 1;
            if (w > 0 && h > 0 && w <= 8000 && h <= 8000 && (comps == 1 || comps == 3)) {
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = w;
                bmi.bmiHeader.biHeight = -h;
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (hbmp && bits) {
                    const char* contents = pdf_annot_contents(ctx, annot->pdfannot);
                    bool remapSignature = str::Eq(contents, "Signature");
                    u8 tr = 0, tg = 0, tb = 0, br = 255, bgg = 255, bb = 255;
                    if (remapSignature) {
                        COLORREF bg = 0;
                        COLORREF text = ThemePageRenderColors(bg, true);
                        UnpackColor(text, tr, tg, tb);
                        UnpackColor(bg, br, bgg, bb);
                        remapSignature = (int)tr + (int)tg + (int)tb > (int)br + (int)bgg + (int)bb;
                    }
                    auto mapSample = [](int src, int textC, int bgC) -> u8 {
                        int diff = bgC - textC;
                        int x = src * diff + 128;
                        x += x >> 8;
                        x >>= 8;
                        int v = textC + x;
                        if (v < 0) {
                            v = 0;
                        }
                        if (v > 255) {
                            v = 255;
                        }
                        return (u8)v;
                    };
                    u8* dst = (u8*)bits;
                    const u8* src = fz_pixmap_samples(ctx, pix);
                    int stride = pix->stride;
                    for (int y = 0; y < h; y++) {
                        const u8* row = src + (ptrdiff_t)y * stride;
                        u8* out = dst + (ptrdiff_t)y * w * 4;
                        for (int x = 0; x < w; x++) {
                            const u8* p = row + x * pix->n;
                            u8 r, g, b, a;
                            if (comps == 1) {
                                r = g = b = p[0];
                                a = p[1];
                            } else {
                                r = p[0];
                                g = p[1];
                                b = p[2];
                                a = p[3];
                            }
                            if (remapSignature && a) {
                                int ur = (int)r * 255 / a;
                                int ug = (int)g * 255 / a;
                                int ub = (int)b * 255 / a;
                                int maxC = ur > ug ? (ur > ub ? ur : ub) : (ug > ub ? ug : ub);
                                int minC = ur < ug ? (ur < ub ? ur : ub) : (ug < ub ? ug : ub);
                                int srcR = ur;
                                int srcG = ug;
                                int srcB = ub;
                                if (maxC - minC <= 28) {
                                    int srcLum = (ur * 54 + ug * 183 + ub * 19) >> 8;
                                    int t;
                                    if (srcLum < 128) {
                                        t = (srcLum * srcLum) / 128;
                                    } else {
                                        int inv = 255 - srcLum;
                                        t = 255 - (inv * inv) / 127;
                                    }
                                    srcR = srcG = srcB = t;
                                }
                                r = (u8)((int)mapSample(srcR, tr, br) * a / 255);
                                g = (u8)((int)mapSample(srcG, tg, bgg) * a / 255);
                                b = (u8)((int)mapSample(srcB, tb, bb) * a / 255);
                            }
                            // MuPDF draw output is premultiplied; AlphaBlend wants the same, BGRA.
                            out[0] = b;
                            out[1] = g;
                            out[2] = r;
                            out[3] = a;
                            out += 4;
                        }
                    }
                }
            }
        }
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (hbmp) {
            DeleteObject(hbmp);
            hbmp = nullptr;
        }
    }
    return hbmp;
}

static HBITMAP RenderAnnotationIconPreviewImpl(EngineMupdf* engine, int pageNo, AnnotationType type, COLORREF swatch,
                                               const char* iconName, int size, int lineEnding, bool lineStart,
                                               Annotation* sourceColor) {
    if (!engine || !iconName || size < 1) {
        return nullptr;
    }
    fz_context* ctx = engine->Ctx();
    ScopedCritSec lock(&engine->docLock);
    pdf_document* doc = nullptr;
    pdf_obj* pageObj = nullptr;
    pdf_page* page = nullptr;
    HBITMAP bitmap = nullptr;
    fz_var(doc);
    fz_var(pageObj);
    fz_var(page);
    fz_var(bitmap);
    fz_try(ctx) {
        // Generate the actual MuPDF appearance in a disposable document, never
        // by changing the user's annotation or its dirty/save state.
        doc = pdf_create_document(ctx);
        pageObj = pdf_add_page(ctx, doc, {0, 0, 100, 100}, 0, nullptr, nullptr);
        pdf_insert_page(ctx, doc, -1, pageObj);
        page = pdf_load_page(ctx, doc, 0);
        pdf_annot* icon = pdf_create_annot(ctx, page, (enum pdf_annot_type)type);
        if (lineEnding >= 0) {
            pdf_set_annot_line(ctx, icon, {20, 30}, {50, 30});
            pdf_set_annot_border(ctx, icon, 1.5f);
            pdf_set_annot_line_ending_styles(ctx, icon, (enum pdf_line_ending)(lineStart ? lineEnding : 0),
                                             (enum pdf_line_ending)(lineStart ? 0 : lineEnding));
        } else {
            pdf_set_annot_rect(ctx, icon, {10, 10, 34, 34});
            pdf_set_annot_icon_name(ctx, icon, iconName);
        }
        u8 r, g, b;
        UnpackColor(swatch, r, g, b);
        float color[4] = {r / 255.f, g / 255.f, b / 255.f, 0};
        int n = 3;
        if (sourceColor) pdf_annot_color(ctx, sourceColor->pdfannot, &n, color);
        pdf_set_annot_color(ctx, icon, n, color);
        pdf_update_annot(ctx, icon);
        fz_rect bounds = pdf_bound_annot(ctx, icon);
        float extent = std::max(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
        if (extent > 0) {
            Annotation preview;
            preview.engine = engine;
            preview.pageNo = pageNo;
            preview.pdfannot = icon;
            bitmap = RenderAnnotationPreviewBitmap(&preview, size / extent, 0);
        }
    }
    fz_always(ctx) {
        pdf_drop_page(ctx, page);
        pdf_drop_obj(ctx, pageObj);
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return bitmap;
}

HBITMAP RenderAnnotationIconPreviewBitmap(EngineMupdf* engine, int pageNo, AnnotationType type, COLORREF color,
                                          const char* iconName, int size, int lineEnding, bool lineStart) {
    return RenderAnnotationIconPreviewImpl(engine, pageNo, type, color, iconName, size, lineEnding, lineStart, nullptr);
}

HBITMAP RenderAnnotationIconPreviewBitmap(Annotation* source, const char* iconName, int size, int lineEnding,
                                          bool lineStart) {
    if (!source) return nullptr;
    return RenderAnnotationIconPreviewImpl(source->engine, source->pageNo, source->type, 0, iconName, size, lineEnding,
                                           lineStart, source);
}

void SetLine(Annotation* annot, PointF a, PointF b) {
    EngineMupdf* e = annot->engine;
    auto pdfannot = annot->pdfannot;
    if (!pdfannot) {
        return;
    }
    bool failed = false;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            fz_point p1 = {a.x, a.y};
            fz_point p2 = {b.x, b.y};
            pdf_set_annot_line(ctx, pdfannot, p1, p2);
            pdf_update_annot(ctx, pdfannot);
            annot->bounds = ToRectF(pdf_bound_annot(ctx, pdfannot));
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            failed = true;
            logf("SetLine(): pdf_set_annot_line() failed\n");
        }
    }
    ReportIf(failed);
    if (failed) {
        return;
    }
    MarkNotificationAsModified(e, annot);
}

// Ink rects are derived from the stroke list, so moving the box has to move the strokes.
static void MoveInkListToRect(fz_context* ctx, pdf_annot* annot, RectF oldR, RectF newR) {
    int nstrokes = pdf_annot_ink_list_count(ctx, annot);
    if (nstrokes < 1) {
        return;
    }
    int* counts = nullptr;
    fz_point* pts = nullptr;
    fz_var(counts);
    fz_var(pts);
    fz_try(ctx) {
        counts = (int*)fz_malloc(ctx, sizeof(int) * (size_t)nstrokes);
        int total = 0;
        for (int i = 0; i < nstrokes; i++) {
            int n = pdf_annot_ink_list_stroke_count(ctx, annot, i);
            if (n < 0) {
                n = 0;
            }
            counts[i] = n;
            total += n;
        }
        if (total >= 1) {
            pts = (fz_point*)fz_malloc(ctx, sizeof(fz_point) * (size_t)total);
            int dst = 0;
            bool canMap = oldR.dx > 0.01f && oldR.dy > 0.01f && newR.dx > 0.01f && newR.dy > 0.01f;
            for (int i = 0; i < nstrokes; i++) {
                for (int k = 0; k < counts[i]; k++) {
                    fz_point p = pdf_annot_ink_list_stroke_vertex(ctx, annot, i, k);
                    if (canMap) {
                        float u = (p.x - oldR.x) / oldR.dx;
                        float v = (p.y - oldR.y) / oldR.dy;
                        p.x = newR.x + u * newR.dx;
                        p.y = newR.y + v * newR.dy;
                    }
                    pts[dst++] = p;
                }
            }
            pdf_set_annot_ink_list(ctx, annot, nstrokes, counts, pts);
        }
    }
    fz_always(ctx) {
        fz_free(ctx, pts);
        fz_free(ctx, counts);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

bool AnnotationContentsEqual(Annotation* annot, const char* text) {
    if (!annot || !annot->pdfannot || !annot->engine || !text) {
        return false;
    }
    EngineMupdf* e = annot->engine;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    const char* s = nullptr;
    fz_try(ctx) {
        s = pdf_annot_contents(ctx, annot->pdfannot);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return false;
    }
    return str::Eq(s, text);
}

void SetRect(Annotation* annot, RectF r) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    if (!a) {
        // pdfannot is nulled out by DeleteAnnotation; a stale reference
        // (e.g. annotationBeingDragged after a reload) must not reach mupdf
        return;
    }
    bool failed = false;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_rect rc = ToFzRect(r);
        fz_try(ctx) {
            if (annot->type == AnnotationType::Line) {
                // Keep the original diagonal / arrow direction. Mapping both ends
                // through a normalized rect (x0,y0)->(x1,y1) mirrors '/' into '\'.
                fz_point p1 = {}, p2 = {};
                pdf_annot_line(ctx, a, &p1, &p2);
                RectF old = annot->bounds;
                if (old.dx > 0.01f && old.dy > 0.01f) {
                    float u1 = (p1.x - old.x) / old.dx;
                    float v1 = (p1.y - old.y) / old.dy;
                    float u2 = (p2.x - old.x) / old.dx;
                    float v2 = (p2.y - old.y) / old.dy;
                    p1.x = r.x + u1 * r.dx;
                    p1.y = r.y + v1 * r.dy;
                    p2.x = r.x + u2 * r.dx;
                    p2.y = r.y + v2 * r.dy;
                } else {
                    p1 = {rc.x0, rc.y0};
                    p2 = {rc.x1, rc.y1};
                }
                pdf_set_annot_line(ctx, a, p1, p2);
            } else if (annot->type == AnnotationType::Ink) {
                MoveInkListToRect(ctx, a, annot->bounds, r);
            } else {
                pdf_set_annot_rect(ctx, a, rc);
            }
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            // can happen for non-moveable annotations
            failed = true;
            logf("SetRect(): pdf_set_annot_rect() or pdf_update_annot() failed\n");
        }
    }
    ReportIf(failed);
    if (failed) {
        return;
    }
    annot->bounds = r;
    // must be called outside docLock to avoid deadlock with pagesLock
    MarkNotificationAsModified(e, annot);
}

bool AnnotationHasAuthor(Annotation* annot) {
    if (!annot || !annot->engine || !annot->pdfannot) {
        return false;
    }
    EngineMupdf* e = annot->engine;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    int res = 0;
    fz_try(ctx) {
        res = pdf_annot_has_author(ctx, annot->pdfannot);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        res = 0;
    }
    return res != 0;
}

bool SetAuthor(Annotation* annot, const char* sv) {
    if (!annot || !annot->engine || !annot->pdfannot || !AnnotationHasAuthor(annot)) {
        return false;
    }
    const char* curr = Author(annot);
    const char* next = sv ? sv : "";
    const char* prev = curr ? curr : "";
    if (str::Eq(next, prev)) {
        return false;
    }
    EngineMupdf* e = annot->engine;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            pdf_set_annot_author(ctx, annot->pdfannot, next);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            return false;
        }
    }
    MarkNotificationAsModified(e, annot);
    return true;
}

const char* Author(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);

    const char* s = nullptr;

    fz_var(s);
    fz_try(ctx) {
        s = pdf_annot_author(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        s = nullptr;
    }
    if (!s || str::IsEmptyOrWhiteSpace(s)) {
        return {};
    }
    return s;
}

int Quadding(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    int res = 0;
    fz_try(ctx) {
        res = pdf_annot_quadding(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("Quadding(): pdf_annot_quadding() failed\n");
    }
    return res;
}

static bool IsValidQuadding(int i) {
    return i >= 0 && i <= 2;
}

// return true if changed
bool SetQuadding(Annotation* annot, int newQuadding) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        ReportIf(!IsValidQuadding(newQuadding));
        bool didChange = Quadding(annot) != newQuadding;
        if (!didChange) {
            return false;
        }
        fz_try(ctx) {
            pdf_set_annot_quadding(ctx, a, newQuadding);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            logf("SetQuadding(): pdf_set_annot_quadding or pdf_update_annot() failed\n");
        }
    }
    MarkNotificationAsModified(e, annot);
    return true;
}

void SetQuadPointsAsRect(Annotation* annot, const Vec<RectF>& rects) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_quad quads[512];
        int n = rects.Size();
        if (n == 0) {
            return;
        }
        constexpr int kMaxQuads = (int)dimof(quads);
        for (int i = 0; i < n && i < kMaxQuads; i++) {
            RectF rect = rects[i];
            fz_rect r = ToFzRect(rect);
            fz_quad q = fz_quad_from_rect(r);
            quads[i] = q;
        }
        fz_try(ctx) {
            pdf_clear_annot_quad_points(ctx, a);
            pdf_set_annot_quad_points(ctx, a, n, quads);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            logf("SetQuadPointsAsRect(): mupdf calls failed\n");
        }
    }
    MarkNotificationAsModified(e, annot);
}

Vec<RectF> GetQuadPointsAsRect(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    Vec<RectF> res;
    int n = pdf_annot_quad_point_count(ctx, annot->pdfannot);
    for (int i = 0; i < n; i++) {
        fz_quad q{};
        fz_rect r{};
        fz_try(ctx) {
            q = pdf_annot_quad_point(ctx, annot->pdfannot, i);
            r = fz_rect_from_quad(q);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        RectF rect = ToRectF(r);
        res.Append(rect);
    }
    return res;
}

TempStr Contents(Annotation* annot) {
    if (!annot || !annot->engine || !annot->pdfannot) {
        return nullptr;
    }
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    const char* s = nullptr;
    fz_try(ctx) {
        s = pdf_annot_contents(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        s = nullptr;
        logf("Contents(): pdf_annot_contents()\n");
    }
    return (TempStr)s;
}

bool SetContents(Annotation* annot, const char* sv) {
    ReportIf(!annot);
    if (!annot || !annot->engine || !annot->pdfannot) {
        return false;
    }
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    const char* currValue = Contents(annot);
    if (str::Eq(sv, currValue)) {
        return false;
    }
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            pdf_set_annot_contents(ctx, a, sv);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
    return true;
}

static bool IsAnnotationInEngine(EngineMupdf* e, Annotation* annot) {
    int pageNo = annot->pageNo;
    int pageIdx = pageNo - 1;
    if (pageIdx < 0 || pageIdx >= e->pages.Size()) {
        return false;
    }
    ScopedCritSec scope(&e->pagesLock);
    FzPageInfo* pageInfo = e->pages[pageIdx];
    return pageInfo->annotations.Contains(annot);
}

void DeleteAnnotation(Annotation* annot) {
    ReportIf(!annot);
    if (!annot) {
        return;
    }
    EngineMupdf* e = annot->engine;
    if (!e) {
        return;
    }
    auto a = annot->pdfannot;
    if (!a) {
        return;
    }
    if (!IsAnnotationInEngine(e, annot)) {
        logf("DeleteAnnotation: annotation not found in engine, skipping\n");
        return;
    }
    bool failed = false;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        pdf_page* page = nullptr;
        fz_try(ctx) {
            page = pdf_annot_page(ctx, a);
            pdf_delete_annot(ctx, page, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            failed = true;
        }
    }
    if (failed) {
        logf("failed to delete annotation on page %d\n", annot->pageNo);
        return;
    }
    annot->pdfannot = nullptr;
    MarkNotificationAsModified(e, annot, AnnotationChange::Remove);
}

// -1 if not exist
int PopupId(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    pdf_obj* obj = nullptr;
    int res = -1;
    fz_try(ctx) {
        obj = pdf_dict_get(ctx, pdf_annot_obj(ctx, a), PDF_NAME(Popup));
        if (obj) {
            res = pdf_to_num(ctx, obj);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return res;
}

/*
time_t CreationDate(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    auto pdf = annot->pdf;
    ScopedCritSec cs(&e->docLock);
    int64_t res = 0;
    fz_try(ctx)
    {
        res = pdf_annot_creation_date(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return res;
}
*/

time_t ModificationDate(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    int64_t res = 0;
    fz_try(ctx) {
        res = pdf_annot_modification_date(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return res;
}

// return empty() if no icon
const char* IconName(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    bool hasIcon = false;
    const char* iconName = nullptr;
    fz_try(ctx) {
        hasIcon = pdf_annot_has_icon_name(ctx, a);
        if (hasIcon) {
            // can only call if pdf_annot_has_icon_name() returned true
            iconName = pdf_annot_icon_name(ctx, a);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return iconName;
}

void SetIconName(Annotation* annot, const char* iconName) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            pdf_set_annot_icon_name(ctx, a, iconName);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    if (annot->type == AnnotationType::Stamp) {
        RememberStampIconName(iconName);
    }
    // TODO: only if the value changed
    MarkNotificationAsModified(e, annot);
}

void SetLineEndStyles(Annotation* annot, int end) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            pdf_set_annot_line_end_style(ctx, a, (pdf_line_ending)end);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
}

void SetLineStartStyles(Annotation* annot, int start) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            pdf_set_annot_line_start_style(ctx, a, (pdf_line_ending)start);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
}

static void PdfColorToFloat(PdfColor c, float rgb[3]) {
    u8 r, g, b, a;
    UnpackPdfColor(c, r, g, b, a);
    rgb[0] = (float)r / 255.0f;
    rgb[1] = (float)g / 255.0f;
    rgb[2] = (float)b / 255.0f;
}

static float GetOpacityFloat(PdfColor c) {
    u8 alpha = GetAlpha(c);
    return alpha / 255.0f;
}

static PdfColor MkPdfColorFromFloat(float rf, float gf, float bf) {
    u8 r = (u8)(rf * 255.0f);
    u8 g = (u8)(gf * 255.0f);
    u8 b = (u8)(bf * 255.0f);
    return MkPdfColor(r, g, b, 0xff);
}

// n = 1 (grey), 3 (rgb) or 4 (cmyk).
static PdfColor PdfColorFromFloat(fz_context* ctx, int n, float color[4]) {
    if (n == 0) {
        return 0; // transparent
    }
    if (n == 1) {
        return MkPdfColorFromFloat(color[0], color[0], color[0]);
    }
    if (n == 3) {
        return MkPdfColorFromFloat(color[0], color[1], color[2]);
    }
    if (n == 4) {
        float rgb[4]{};
        fz_try(ctx) {
            fz_convert_color(ctx, fz_device_cmyk(ctx), color, fz_device_rgb(ctx), rgb, nullptr,
                             fz_default_color_params);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        return MkPdfColorFromFloat(rgb[0], rgb[1], rgb[2]);
    }
    ReportIf(true);
    return 0;
}

PdfColor GetColor(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    float color[4]{};
    int n = -1;
    fz_try(ctx) {
        pdf_annot_color(ctx, a, &n, color);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        n = -1;
    }
    if (n == -1) {
        return 0;
    }
    PdfColor res = PdfColorFromFloat(ctx, n, color);
    return res;
}

// return true if color changed
bool SetColor(Annotation* annot, PdfColor c) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        bool didChange = false;
        float color[4]{};
        int n = -1;
        float oldOpacity = 0;
        fz_try(ctx) {
            pdf_annot_color(ctx, a, &n, color);
            oldOpacity = pdf_annot_opacity(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            n = -1;
        }
        if (n == -1) {
            return false;
        }
        float newColor[3];
        PdfColorToFloat(c, newColor);
        float opacity = GetOpacityFloat(c);
        didChange = (n != 3);
        if (!didChange) {
            for (int i = 0; i < n; i++) {
                if (color[i] != newColor[i]) {
                    didChange = true;
                }
            }
        }
        if (opacity != oldOpacity) {
            didChange = true;
        }
        if (!didChange) {
            return false;
        }
        fz_try(ctx) {
            if (c == 0) {
                pdf_set_annot_color(ctx, a, 0, newColor);
                // TODO: set opacity to 1?
                // pdf_set_annot_opacity(ctx, a, 1.f);
            } else {
                pdf_set_annot_color(ctx, a, 3, newColor);
                if (oldOpacity != opacity) {
                    pdf_set_annot_opacity(ctx, a, opacity);
                }
            }
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
    return true;
}

PdfColor InteriorColor(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    float color[4]{};
    int n = -1;
    fz_try(ctx) {
        pdf_annot_interior_color(ctx, a, &n, color);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        n = -1;
    }
    if (n == -1) {
        return 0;
    }
    PdfColor res = PdfColorFromFloat(ctx, n, color);
    return res;
}

bool SetInteriorColor(Annotation* annot, PdfColor c) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        bool didChange = false;
        float color[4]{};
        int n = -1;
        fz_try(ctx) {
            pdf_annot_interior_color(ctx, a, &n, color);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            n = -1;
        }
        float newColor[3]{};
        PdfColorToFloat(c, newColor);
        int newN = (c == 0) ? 0 : 3;
        didChange = (n != newN);
        if (!didChange) {
            for (int i = 0; i < n; i++) {
                if (color[i] != newColor[i]) {
                    didChange = true;
                }
            }
        }
        if (!didChange) {
            return false;
        }
        fz_try(ctx) {
            pdf_set_annot_interior_color(ctx, a, newN, newColor);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
    return true;
}

const char* DefaultAppearanceTextFont(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    const char* fontName = nullptr;
    float sizeF{0.0};
    int n = 0;
    float textColor[4]{};
    fz_try(ctx) {
        pdf_annot_default_appearance(ctx, a, &fontName, &sizeF, &n, textColor);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return fontName;
}

void SetDefaultAppearanceTextFont(Annotation* annot, const char* sv) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        const char* fontName = nullptr;
        float sizeF{0.0};
        int n = 0;
        float textColor[4]{};
        fz_try(ctx) {
            pdf_annot_default_appearance(ctx, a, &fontName, &sizeF, &n, textColor);
            pdf_set_annot_default_appearance(ctx, a, sv, sizeF, n, textColor);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
}

int DefaultAppearanceTextSize(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    const char* fontName = nullptr;
    float sizeF{0.0};
    int n = 0;
    float textColor[4]{};
    fz_try(ctx) {
        pdf_annot_default_appearance(ctx, a, &fontName, &sizeF, &n, textColor);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return (int)sizeF;
}

void SetDefaultAppearanceTextSize(Annotation* annot, int textSize) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        const char* fontName = nullptr;
        float sizeF{0.0};
        int n = 0;
        float textColor[4]{};
        fz_try(ctx) {
            pdf_annot_default_appearance(ctx, a, &fontName, &sizeF, &n, textColor);
            pdf_set_annot_default_appearance(ctx, a, fontName, (float)textSize, n, textColor);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
}

PdfColor DefaultAppearanceTextColor(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    const char* fontName = nullptr;
    float sizeF{0.0};
    int n = 0;
    float textColor[4]{};
    fz_try(ctx) {
        pdf_annot_default_appearance(ctx, a, &fontName, &sizeF, &n, textColor);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    PdfColor res = PdfColorFromFloat(ctx, n, textColor);
    return res;
}

void SetDefaultAppearanceTextColor(Annotation* annot, PdfColor col) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        const char* fontName = nullptr;
        float sizeF{0.0};
        int n = 0;
        float textColor[4]{}; // must be at least 4
        fz_try(ctx) {
            pdf_annot_default_appearance(ctx, a, &fontName, &sizeF, &n, textColor);
            PdfColorToFloat(col, textColor);
            pdf_set_annot_default_appearance(ctx, a, fontName, sizeF, 3, textColor);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MarkNotificationAsModified(e, annot);
}

void GetLineEndingStyles(Annotation* annot, int* start, int* end) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    pdf_line_ending leStart = PDF_ANNOT_LE_NONE;
    pdf_line_ending leEnd = PDF_ANNOT_LE_NONE;
    fz_try(ctx) {
        pdf_annot_line_ending_styles(ctx, a, &leStart, &leEnd);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("GetLineEndingStyles: pdf_annot_line_ending_styles() failed\n");
    }
    *start = (int)leStart;
    *end = (int)leEnd;
}

float BorderWidthF(Annotation* annot) {
    if (!annot || !annot->engine || !annot->pdfannot) {
        return 1.f;
    }
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    float res = 0;
    fz_try(ctx) {
        res = pdf_annot_border(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("BorderWidth: pdf_annot_border() failed\n");
    }
    // Free text uses 0 for no frame, matching EPUB. Other marks keep a hairline
    // so a zero-width stroke does not vanish.
    if (annot->type == AnnotationType::FreeText) {
        if (res < 0.5f) {
            res = 0;
        }
    } else if (res < 0.15f) {
        res = 0.15f;
    }
    return res;
}

int BorderWidth(Annotation* annot) {
    return (int)BorderWidthF(annot);
}

bool GetLinePoints(Annotation* annot, PointF& a, PointF& b) {
    if (!annot || !annot->engine || !annot->pdfannot || annot->type != AnnotationType::Line) {
        return false;
    }
    EngineMupdf* e = annot->engine;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    fz_point p1{}, p2{};
    bool ok = false;
    fz_try(ctx) {
        pdf_annot_line(ctx, annot->pdfannot, &p1, &p2);
        ok = true;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    if (!ok) {
        return false;
    }
    a = PointF(p1.x, p1.y);
    b = PointF(p2.x, p2.y);
    return true;
}

void GetInkStrokes(Annotation* annot, Vec<PointF>& points, Vec<int>& counts) {
    points.Reset();
    counts.Reset();
    if (!annot || !annot->engine || !annot->pdfannot || annot->type != AnnotationType::Ink) {
        return;
    }
    EngineMupdf* e = annot->engine;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    fz_try(ctx) {
        int nstrokes = pdf_annot_ink_list_count(ctx, annot->pdfannot);
        for (int i = 0; i < nstrokes; i++) {
            int n = pdf_annot_ink_list_stroke_count(ctx, annot->pdfannot, i);
            if (n < 1) {
                continue;
            }
            counts.Append(n);
            for (int k = 0; k < n; k++) {
                fz_point p = pdf_annot_ink_list_stroke_vertex(ctx, annot->pdfannot, i, k);
                points.Append(PointF(p.x, p.y));
            }
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        points.Reset();
        counts.Reset();
    }
}

static void SetBorderWidthFloatImpl(Annotation* annot, float newWidth) {
    ReportIf(!annot);
    if (!annot) {
        return;
    }
    if (annot->type == AnnotationType::FreeText) {
        if (newWidth < 0.5f) {
            newWidth = 0;
        }
    } else if (newWidth < 0.15f) {
        newWidth = 0.15f;
    }
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            pdf_set_annot_border_width(ctx, a, newWidth);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            logf("SetBorderWidth: SetBorderWidth() or pdf_update_annot() failed\n");
        }
    }
    MarkNotificationAsModified(e, annot);
}

void SetBorderWidth(Annotation* annot, int newWidth) {
    SetBorderWidthFloatImpl(annot, (float)newWidth);
}

bool ClearFreeTextHairlineBorder(Annotation* annot) {
    if (!annot || annot->type != AnnotationType::FreeText || !annot->engine || !annot->pdfannot) {
        return false;
    }
    EngineMupdf* e = annot->engine;
    float stored = 0;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        fz_try(ctx) {
            stored = pdf_annot_border(ctx, annot->pdfannot);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            return false;
        }
    }
    if (!(stored > 0.f && stored < 0.5f)) {
        return false;
    }
    SetBorderWidthFloatImpl(annot, 0);
    return true;
}

void SetBorderWidthFloat(Annotation* annot, float newWidth) {
    SetBorderWidthFloatImpl(annot, newWidth);
}

int Opacity(Annotation* annot) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    auto ctx = e->Ctx();
    ScopedCritSec cs(&e->docLock);
    float fopacity = 0;
    fz_try(ctx) {
        fopacity = pdf_annot_opacity(ctx, a);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("Opacity: pdf_annot_opacity() failed\n");
    }
    int res = (int)(fopacity * 255.f);
    return res;
}

void SetOpacity(Annotation* annot, int newOpacity) {
    EngineMupdf* e = annot->engine;
    auto a = annot->pdfannot;
    {
        auto ctx = e->Ctx();
        ScopedCritSec cs(&e->docLock);
        ReportIf(newOpacity < 0 || newOpacity > 255);
        newOpacity = std::clamp(newOpacity, 0, 255);
        float fopacity = (float)newOpacity / 255.f;

        fz_try(ctx) {
            pdf_set_annot_opacity(ctx, a, fopacity);
            pdf_update_annot(ctx, a);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            logf("SetOpacity: pdf_set_annot_opacity() or pdf_update_annot() failed\n");
        }
    }
    MarkNotificationAsModified(e, annot);
}

static const char* getuser(void) {
    const char* u;
    u = getenv("USER");
    if (!u) {
        u = getenv("USERNAME");
    }
    if (!u) {
        u = "user";
    }
    return u;
}

static TempStr GetAnnotationTextIconTemp() {
    char* s = str::DupTemp(gGlobalPrefs->annotations.textIconType);
    // this way user can use "new paragraph" and we'll match "NewParagraph"
    str::RemoveCharsInPlace(s, " ");
    int idx = seqstrings::StrToIdxIS(gAnnotationTextIcons, s);
    if (idx < 0) {
        return (char*)"Comment";
    }
    char* real = (char*)seqstrings::IdxToStr(gAnnotationTextIcons, idx);
    return real;
}

static AnnotationType supportsInteriorColor[] = {
    AnnotationType::Circle,  AnnotationType::Line,   AnnotationType::PolyLine,
    AnnotationType::Polygon, AnnotationType::Square,
};

// matches rect_subtypes in pdf-annot.c + Line (because special case it in SetRect())
// TODO: should include AnnotationType::ThreeD but mupdf doesn't
static AnnotationType moveableAnnotations[] = {
    AnnotationType::Text,           AnnotationType::FreeText, AnnotationType::Square, AnnotationType::Circle,
    AnnotationType::Redact,         AnnotationType::Stamp,    AnnotationType::Caret,  AnnotationType::Popup,
    AnnotationType::FileAttachment, AnnotationType::Sound,    AnnotationType::Movie,  AnnotationType::Widget,
    AnnotationType::Line,           AnnotationType::Ink,
};

static AnnotationType supportsBorder[] = {
    AnnotationType::FreeText, AnnotationType::Ink,     AnnotationType::Line,     AnnotationType::Square,
    AnnotationType::Circle,   AnnotationType::Polygon, AnnotationType::PolyLine,
};

static AnnotationType supportsColor[] = {
    AnnotationType::Stamp,     AnnotationType::Text,      AnnotationType::FileAttachment,
    AnnotationType::Sound,     AnnotationType::Caret,     AnnotationType::FreeText,
    AnnotationType::Ink,       AnnotationType::Line,      AnnotationType::Square,
    AnnotationType::Circle,    AnnotationType::Polygon,   AnnotationType::PolyLine,
    AnnotationType::Highlight, AnnotationType::Underline, AnnotationType::StrikeOut,
    AnnotationType::Squiggly,
};

static bool IsAnnotationInList(AnnotationType tp, AnnotationType* allowed, int nAllowed) {
    if (!allowed) {
        return true;
    }
    for (int i = 0; i < nAllowed; i++) {
        AnnotationType tp2 = allowed[i];
        if (tp2 == tp) {
            return true;
        }
    }
    return false;
}

bool AnnotationCanBeMoved(AnnotationType tp) {
    return IsAnnotationInList(tp, moveableAnnotations, dimofi(moveableAnnotations));
}

bool AnnotationCanBeResized(AnnotationType tp) {
    switch (tp) {
        // TODO: for now don't allow resizing text annotation because it's just an icon
        // would have to figure out how to change the size of the icon
        case AnnotationType::Text:
        case AnnotationType::Caret:
            return false;
    }
    return AnnotationCanBeMoved(tp);
}

bool AnnotationSupportsInteriorColor(AnnotationType tp) {
    return IsAnnotationInList(tp, supportsInteriorColor, dimofi(supportsInteriorColor));
}

bool AnnotationSupportsBorder(AnnotationType tp) {
    return IsAnnotationInList(tp, supportsBorder, dimofi(supportsBorder));
}

bool AnnotationSupportsColor(AnnotationType tp) {
    return IsAnnotationInList(tp, supportsColor, dimofi(supportsColor));
}

bool IsPdfTextMarkupAnnotation(AnnotationType tp) {
    switch (tp) {
        case AnnotationType::Highlight:
        case AnnotationType::Underline:
        case AnnotationType::StrikeOut:
        case AnnotationType::Squiggly:
            return true;
        default:
            return false;
    }
}

bool IsPdfTextMarkupAnnotation(Annotation* annot) {
    return annot && IsPdfTextMarkupAnnotation(annot->type);
}

TempStr MarkupTextTemp(Annotation* annot) {
    if (!IsPdfTextMarkupAnnotation(annot)) {
        return nullptr;
    }
    Vec<RectF> quads = GetQuadPointsAsRect(annot);
    if (quads.empty()) {
        return nullptr;
    }
    EngineMupdf* engine = annot->engine;
    if (!engine) {
        return nullptr;
    }
    // Use the same cache as selection, including OCR text already produced by
    // the user. GetTextForPageUtf8 extracts native text but never starts OCR.
    PageTextUtf8 pt;
    pt.text = (char*)engine->GetTextForPageUtf8(annot->pageNo, &pt.len, &pt.coords);
    if (!pt.text || pt.len <= 0 || !pt.coords) {
        return nullptr;
    }
    StrBuilder sb;
    /* Walk by UTF-8 codepoint — coords repeat per byte; including only some
     * bytes of a CJK character produces mojibake in the list/export. */
    bool pendingSpace = false;
    bool haveText = false;
    for (int i = 0; i < pt.len;) {
        int n = utf8RuneLen((const u8*)(pt.text + i));
        if (n < 1) {
            n = 1;
        }
        if (i + n > pt.len) {
            break;
        }
        RectF cr = ToRectF(pt.coords[i]);
        PointF center(cr.x + cr.dx / 2.f, cr.y + cr.dy / 2.f);
        bool hit = false;
        for (RectF& quad : quads) {
            if (quad.Contains(center)) {
                hit = true;
                break;
            }
        }
        if (hit) {
            if (pendingSpace && haveText) {
                sb.Append(" ");
            }
            sb.Append(pt.text + i, (size_t)n);
            pendingSpace = false;
            haveText = true;
        } else if (pt.text[i] == '\n' || pt.text[i] == '\r' || pt.text[i] == ' ' || pt.text[i] == '\t') {
            // Line separators have no glyph bounds, but selected lines must
            // remain separated when presented as a single-line excerpt.
            pendingSpace = true;
        }
        i += n;
    }
    TempStr res = sb.Get();
    if (str::IsEmptyOrWhiteSpace(res)) {
        return nullptr;
    }
    // StrBuilder::Get() points at sb's buffer; DupTemp so it survives sb's destructor.
    return str::DupTemp(res);
}

Annotation* EngineMupdfCreateAnnotation(EngineBase* engine, int pageNo, PointF pos, AnnotCreateArgs* args) {
    static const float black[3] = {0, 0, 0};

    EngineMupdf* epdf = AsEngineMupdf(engine);
    fz_context* ctx = epdf->Ctx();

    auto pageInfo = epdf->GetFzPageInfo(pageNo, true);
    pdf_annot* annot = nullptr;
    auto typ = args->annotType;
    auto col = args->col;
    auto bgCol = args->bgCol;
    auto interiorCol = args->interiorCol;
    {
        ScopedCritSec cs(&epdf->docLock);

        fz_try(ctx) {
            auto page = pdf_page_from_fz_page(ctx, pageInfo->page);
            enum pdf_annot_type atyp = (enum pdf_annot_type)typ;

            annot = pdf_create_annot(ctx, page, atyp);

            pdf_set_annot_modification_date(ctx, annot, time(nullptr));
            if (pdf_annot_has_author(ctx, annot)) {
                char* defAuthor = gGlobalPrefs->annotations.defaultAuthor;
                // if "(none)" we don't set it
                if (!str::Eq(defAuthor, "(none)")) {
                    const char* author = getuser();
                    if (!str::IsEmptyOrWhiteSpace(defAuthor)) {
                        author = defAuthor;
                    }
                    pdf_set_annot_author(ctx, annot, author);
                }
            }

            switch (typ) {
                case AnnotationType::Link:
                case AnnotationType::Polygon:
                case AnnotationType::Redact:
                case AnnotationType::Ink:
                case AnnotationType::Popup:
                case AnnotationType::PolyLine:
                case AnnotationType::Unknown:
                case AnnotationType::FileAttachment:
                case AnnotationType::Sound:
                case AnnotationType::Movie:
                case AnnotationType::RichMedia:
                case AnnotationType::Widget:
                case AnnotationType::Screen:
                case AnnotationType::PrinterMark:
                case AnnotationType::Watermark:
                case AnnotationType::TrapNet:
                case AnnotationType::ThreeD:
                case AnnotationType::Projection:
                    // do nothing
                    break;

                case AnnotationType::Highlight:
                case AnnotationType::Underline:
                case AnnotationType::Squiggly:
                case AnnotationType::StrikeOut: {
                    if (!str::IsEmptyOrWhiteSpace(args->content)) {
                        pdf_set_annot_contents(ctx, annot, args->content);
                    }
                } break;
                case AnnotationType::Text:
                case AnnotationType::FreeText:
                case AnnotationType::Caret:
                case AnnotationType::Square:
                case AnnotationType::Circle: {
                    fz_rect trect = pdf_annot_rect(ctx, annot);
                    float dx = trect.x1 - trect.x0;
                    trect.x0 = pos.x;
                    trect.x1 = trect.x0 + dx;
                    float dy = trect.y1 - trect.y0;
                    trect.y0 = pos.y;
                    trect.y1 = trect.y0 + dy;
                    pdf_set_annot_rect(ctx, annot, trect);
                } break;
                case AnnotationType::Stamp: {
                    SizeF sz = GetDefaultStampSize();
                    fz_rect trect;
                    trect.x0 = pos.x - sz.dx / 2.f;
                    trect.x1 = pos.x + sz.dx / 2.f;
                    trect.y0 = pos.y - sz.dy / 2.f;
                    trect.y1 = pos.y + sz.dy / 2.f;
                    pdf_set_annot_rect(ctx, annot, trect);
                    pdf_update_annot(ctx, annot);
                } break;
                case AnnotationType::Line: {
                    fz_point a{pos.x, pos.y};
                    fz_point b{pos.x + 100, pos.y + 50};
                    pdf_set_annot_line(ctx, annot, a, b);
                } break;
            }
            if (typ == AnnotationType::FreeText) {
                if (args->borderWidth >= 0) {
                    pdf_set_annot_border_width(ctx, annot, (float)args->borderWidth);
                }
                if (!str::IsEmptyOrWhiteSpace(args->content)) {
                    pdf_set_annot_contents(ctx, annot, args->content);
                } else {
                    pdf_set_annot_contents(ctx, annot, "This is a text...");
                }
                int fontSize = args->textSize;
                if (fontSize <= 0) {
                    fontSize = 21;
                }
                int nCol = 3;
                const float* fcol = black;
                float textColor[3]{};

                if (col.parsedOk) {
                    PdfColorToFloat(col.pdfCol, textColor);
                    fcol = textColor;
                }
                pdf_set_annot_default_appearance(ctx, annot, "Helv", (float)fontSize, nCol, fcol);
                if (bgCol.parsedOk) {
                    float bgColor[3]{};
                    PdfColorToFloat(bgCol.pdfCol, bgColor);
                    pdf_set_annot_color(ctx, annot, 3, bgColor);
                }
                // 100 is fuly opaque, the default
                if (args->opacity < 100) {
                    float fop = (float)args->opacity / 100.0f;
                    pdf_set_annot_opacity(ctx, annot, fop);
                }
            }

            if (interiorCol.parsedOk && AnnotationSupportsInteriorColor(typ)) {
                float interiorColor[3]{};
                PdfColorToFloat(interiorCol.pdfCol, interiorColor);
                pdf_set_annot_interior_color(ctx, annot, 3, interiorColor);
            }
            pdf_update_annot(ctx, annot);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            if (annot) {
                pdf_drop_annot(ctx, annot);
            }
        }
        if (!annot) {
            return nullptr;
        }
    }

    auto res = MakeAnnotationWrapper(epdf, annot, pageNo);
    MarkNotificationAsModified(epdf, res, AnnotationChange::Add);

    if (typ == AnnotationType::Text) {
        TempStr iconName = GetAnnotationTextIconTemp();
        if (!str::EqI(iconName, "Note")) {
            SetIconName(res, iconName);
        }
    }
    if (typ == AnnotationType::Stamp) {
        const char* iconName = DefaultStampIconName();
        if (!str::EqI(iconName, "Draft")) {
            SetIconName(res, iconName);
        }
    }
    if (col.parsedOk) {
        switch (typ) {
            case AnnotationType::FreeText:
                // do nothing. for free text we set text color via pdf_set_annot_default_appearance
                // and SetColor() sets background color
                break;
            default:
                SetColor(res, col.pdfCol);
                break;
        }
    }
    pdf_drop_annot(ctx, annot);
    return res;
}

Annotation* EngineMupdfCreateAnnotationInRect(EngineBase* engine, int pageNo, RectF rect, AnnotCreateArgs* args) {
    if (rect.dx < 1.f && rect.dy < 1.f) {
        return nullptr;
    }
    PointF pos{rect.x, rect.y};
    Annotation* annot = EngineMupdfCreateAnnotation(engine, pageNo, pos, args);
    if (!annot) {
        return nullptr;
    }
    SetRect(annot, rect);
    return annot;
}

Annotation* EngineMupdfCreateAnnotationInkStrokes(EngineBase* engine, int pageNo, PointF* pts, const int* counts,
                                                  int nstrokes, AnnotCreateArgs* args, float borderWidth) {
    if (!engine || !pts || !counts || nstrokes < 1 || !args) {
        return nullptr;
    }
    Vec<PointF> flat;
    Vec<int> validCounts;
    int src = 0;
    for (int i = 0; i < nstrokes; i++) {
        int n = counts[i];
        if (n < 0) {
            return nullptr;
        }
        if (n >= 2) {
            for (int k = 0; k < n; k++) {
                flat.Append(pts[src + k]);
            }
            validCounts.Append(n);
        }
        src += n;
    }
    if (validCounts.Size() < 1) {
        return nullptr;
    }
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return nullptr;
    }
    fz_context* ctx = epdf->Ctx();
    auto pageInfo = epdf->GetFzPageInfo(pageNo, true);
    if (!pageInfo || !pageInfo->page) {
        return nullptr;
    }
    pdf_annot* annot = nullptr;
    {
        ScopedCritSec cs(&epdf->docLock);
        fz_try(ctx) {
            auto page = pdf_page_from_fz_page(ctx, pageInfo->page);
            annot = pdf_create_annot(ctx, page, PDF_ANNOT_INK);
            pdf_set_annot_modification_date(ctx, annot, time(nullptr));
            if (pdf_annot_has_author(ctx, annot)) {
                char* defAuthor = gGlobalPrefs->annotations.defaultAuthor;
                if (!str::Eq(defAuthor, "(none)")) {
                    const char* author = getuser();
                    if (!str::IsEmptyOrWhiteSpace(defAuthor)) {
                        author = defAuthor;
                    }
                    pdf_set_annot_author(ctx, annot, author);
                }
            }
            int nFlat = flat.Size();
            fz_point* fzpts = (fz_point*)fz_malloc(ctx, sizeof(fz_point) * (size_t)nFlat);
            for (int i = 0; i < nFlat; i++) {
                fzpts[i].x = flat.at(i).x;
                fzpts[i].y = flat.at(i).y;
            }
            pdf_set_annot_ink_list(ctx, annot, validCounts.Size(), validCounts.LendData(), fzpts);
            fz_free(ctx, fzpts);
            if (borderWidth >= 0) {
                float black[3] = {0, 0, 0};
                pdf_set_annot_color(ctx, annot, 3, black);
                pdf_set_annot_border_width(ctx, annot, borderWidth);
                pdf_set_annot_contents(ctx, annot, "Signature");
            }
            pdf_update_annot(ctx, annot);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            if (annot) {
                pdf_drop_annot(ctx, annot);
                annot = nullptr;
            }
        }
    }
    if (!annot) {
        return nullptr;
    }
    auto res = MakeAnnotationWrapper(epdf, annot, pageNo);
    MarkNotificationAsModified(epdf, res, AnnotationChange::Add);
    if (args->col.parsedOk) {
        SetColor(res, args->col.pdfCol);
    }
    pdf_drop_annot(ctx, annot);
    return res;
}

Annotation* EngineMupdfCreateAnnotationInkStroke(EngineBase* engine, int pageNo, PointF* pts, int count,
                                                 AnnotCreateArgs* args) {
    if (!pts || count < 2) {
        return nullptr;
    }
    int strokeCounts[1] = {count};
    return EngineMupdfCreateAnnotationInkStrokes(engine, pageNo, pts, strokeCounts, 1, args, -1.f);
}

Annotation* EngineMupdfCreateAnnotationStampPng(EngineBase* engine, int pageNo, RectF rect, const ByteSlice& png) {
    if (!engine || png.empty() || rect.dx < 1.f || rect.dy < 1.f) {
        return nullptr;
    }
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return nullptr;
    }
    fz_context* ctx = epdf->Ctx();
    auto pageInfo = epdf->GetFzPageInfo(pageNo, true);
    if (!pageInfo || !pageInfo->page) {
        return nullptr;
    }
    pdf_annot* annot = nullptr;
    {
        ScopedCritSec cs(&epdf->docLock);
        fz_image* img = nullptr;
        fz_buffer* buf = nullptr;
        fz_var(img);
        fz_var(buf);
        fz_try(ctx) {
            auto page = pdf_page_from_fz_page(ctx, pageInfo->page);
            annot = pdf_create_annot(ctx, page, PDF_ANNOT_STAMP);
            pdf_set_annot_modification_date(ctx, annot, time(nullptr));
            if (pdf_annot_has_author(ctx, annot)) {
                char* defAuthor = gGlobalPrefs->annotations.defaultAuthor;
                if (!str::Eq(defAuthor, "(none)")) {
                    const char* author = getuser();
                    if (!str::IsEmptyOrWhiteSpace(defAuthor)) {
                        author = defAuthor;
                    }
                    pdf_set_annot_author(ctx, annot, author);
                }
            }
            fz_rect rc = ToFzRect(rect);
            pdf_set_annot_rect(ctx, annot, rc);
            buf = fz_new_buffer_from_copied_data(ctx, png.data(), png.size());
            img = fz_new_image_from_buffer(ctx, buf);
            pdf_set_annot_stamp_image(ctx, annot, img);
            pdf_set_annot_contents(ctx, annot, "Signature");
            pdf_update_annot(ctx, annot);
        }
        fz_always(ctx) {
            fz_drop_image(ctx, img);
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            if (annot) {
                pdf_drop_annot(ctx, annot);
                annot = nullptr;
            }
        }
    }
    if (!annot) {
        return nullptr;
    }
    auto res = MakeAnnotationWrapper(epdf, annot, pageNo);
    MarkNotificationAsModified(epdf, res, AnnotationChange::Add);
    pdf_drop_annot(ctx, annot);
    if (res) {
        res->bounds = rect;
    }
    return res;
}

AnnotationType CmdIdToAnnotationType(int cmdId) {
    // clang-format off
    switch (cmdId) {
        case CmdCreateAnnotText:           return AnnotationType::Text;
        case CmdCreateAnnotLink:           return AnnotationType::Link;
        case CmdCreateAnnotFreeText:       return AnnotationType::FreeText;
        case CmdCreateAnnotLine:           return AnnotationType::Line;
        case CmdCreateAnnotSquare:         return AnnotationType::Square;
        case CmdCreateAnnotCircle:         return AnnotationType::Circle;
        case CmdCreateAnnotPolygon:        return AnnotationType::Polygon;
        case CmdCreateAnnotPolyLine:       return AnnotationType::PolyLine;
        case CmdCreateAnnotHighlight:      return AnnotationType::Highlight;
        case CmdCreateAnnotUnderline:      return AnnotationType::Underline;
        case CmdCreateAnnotSquiggly:       return AnnotationType::Squiggly;
        case CmdCreateAnnotStrikeOut:      return AnnotationType::StrikeOut;
        case CmdCreateAnnotRedact:         return AnnotationType::Redact;
        case CmdCreateAnnotStamp:          return AnnotationType::Stamp;
        case CmdCreateAnnotCaret:          return AnnotationType::Caret;
        case CmdCreateAnnotInk:            return AnnotationType::Ink;
        case CmdCreateAnnotPopup:          return AnnotationType::Popup;
        case CmdCreateAnnotFileAttachment: return AnnotationType::FileAttachment;
    }
    // clang-format on
    return AnnotationType::Unknown;
}

enum class PdfSoundEncoding {
    Raw,
    Signed,
    MuLaw,
    ALaw,
    Unknown,
};

static void WriteLE16(u8* p, u16 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

static void WriteLE32(u8* p, u32 v) {
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static PdfSoundEncoding PdfSoundEncodingFromObj(fz_context* ctx, pdf_obj* enc) {
    if (!enc || !pdf_is_name(ctx, enc)) {
        return PdfSoundEncoding::Raw;
    }
    const char* n = pdf_to_name(ctx, enc);
    if (!n || !*n) {
        return PdfSoundEncoding::Raw;
    }
    if (str::EqI(n, "Raw")) {
        return PdfSoundEncoding::Raw;
    }
    if (str::EqI(n, "Signed")) {
        return PdfSoundEncoding::Signed;
    }
    if (str::EqI(n, "MuLaw")) {
        return PdfSoundEncoding::MuLaw;
    }
    if (str::EqI(n, "ALaw")) {
        return PdfSoundEncoding::ALaw;
    }
    return PdfSoundEncoding::Unknown;
}

static bool PdfSoundLooksLikeWav(const u8* data, size_t size, size_t* offsetOut) {
    if (!data || size < 12) {
        return false;
    }
    if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
        if (offsetOut) {
            *offsetOut = 0;
        }
        return true;
    }
    // Acrobat/iText quirk: first stream byte may be padding before "RIFF".
    if (size >= 13 && data[1] == 'R' && data[2] == 'I' && data[3] == 'F' && data[4] == 'F') {
        if (offsetOut) {
            *offsetOut = 1;
        }
        return true;
    }
    return false;
}

static bool PdfSoundLooksLikeMp3(const u8* data, size_t size) {
    if (!data || size < 3) {
        return false;
    }
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        return true;
    }
    if (size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        return true;
    }
    return false;
}

static pdf_obj* PdfResolveSoundObject(fz_context* ctx, pdf_obj* annotObj) {
    if (!annotObj) {
        return nullptr;
    }
    pdf_obj* sound = pdf_dict_get(ctx, annotObj, PDF_NAME(Sound));
    if (sound) {
        return sound;
    }
    auto tryAction = [&](pdf_obj* action) -> pdf_obj* {
        if (!action) {
            return nullptr;
        }
        if (!pdf_name_eq(ctx, pdf_dict_get(ctx, action, PDF_NAME(S)), PDF_NAME(Sound))) {
            return nullptr;
        }
        return pdf_dict_get(ctx, action, PDF_NAME(Sound));
    };
    sound = tryAction(pdf_dict_get(ctx, annotObj, PDF_NAME(A)));
    if (sound) {
        return sound;
    }
    pdf_obj* aa = pdf_dict_get(ctx, annotObj, PDF_NAME(AA));
    if (!aa) {
        return nullptr;
    }
    static const char* kActionKeys[] = {"U", "D", "E", "X"};
    for (const char* key : kActionKeys) {
        sound = tryAction(pdf_dict_gets(ctx, aa, key));
        if (sound) {
            return sound;
        }
    }
    return nullptr;
}

static u8* PdfBuildWavFromPcm(const u8* pcm, size_t pcmSize, int sampleRate, int channels, int bitsPerSample,
                              u16 audioFormat, size_t* outSize) {
    if (!pcm || pcmSize == 0 || sampleRate <= 0 || channels <= 0 || bitsPerSample <= 0 || !outSize) {
        return nullptr;
    }
    u32 blockAlign = (u32)channels * (u32)bitsPerSample / 8;
    u32 byteRate = (u32)sampleRate * blockAlign;
    size_t wavSize = 44 + pcmSize;
    u8* wav = AllocArray<u8>(wavSize);
    if (!wav) {
        return nullptr;
    }
    memcpy(wav, "RIFF", 4);
    WriteLE32(wav + 4, (u32)(wavSize - 8));
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    WriteLE32(wav + 16, 16);
    WriteLE16(wav + 20, audioFormat);
    WriteLE16(wav + 22, (u16)channels);
    WriteLE32(wav + 24, (u32)sampleRate);
    WriteLE32(wav + 28, byteRate);
    WriteLE16(wav + 32, (u16)blockAlign);
    WriteLE16(wav + 34, (u16)bitsPerSample);
    memcpy(wav + 36, "data", 4);
    WriteLE32(wav + 40, (u32)pcmSize);
    memcpy(wav + 44, pcm, pcmSize);
    *outSize = wavSize;
    return wav;
}

static u8* PdfSoundPcmToWav(fz_context* ctx, const u8* pcm, size_t pcmSize, pdf_obj* sound, size_t* outSize) {
    int sampleRate = pdf_dict_get_int(ctx, sound, PDF_NAME(R));
    if (sampleRate <= 0) {
        return nullptr;
    }
    int channels = pdf_dict_get(ctx, sound, PDF_NAME(C)) ? pdf_dict_get_int(ctx, sound, PDF_NAME(C)) : 1;
    if (channels <= 0) {
        channels = 1;
    }
    int bits = pdf_dict_get(ctx, sound, PDF_NAME(B)) ? pdf_dict_get_int(ctx, sound, PDF_NAME(B)) : 8;
    if (bits <= 0) {
        bits = 8;
    }
    PdfSoundEncoding enc = PdfSoundEncodingFromObj(ctx, pdf_dict_get(ctx, sound, PDF_NAME(E)));

    u16 audioFormat = 1;
    const u8* src = pcm;
    u8* swapped = nullptr;
    if (enc == PdfSoundEncoding::MuLaw) {
        audioFormat = 7;
        bits = 8;
    } else if (enc == PdfSoundEncoding::ALaw) {
        audioFormat = 6;
        bits = 8;
    } else if (enc == PdfSoundEncoding::Signed && bits == 16) {
        swapped = AllocArray<u8>(pcmSize);
        if (!swapped) {
            return nullptr;
        }
        for (size_t i = 0; i + 1 < pcmSize; i += 2) {
            swapped[i] = pcm[i + 1];
            swapped[i + 1] = pcm[i];
        }
        src = swapped;
    } else if (enc == PdfSoundEncoding::Unknown) {
        free(swapped);
        return nullptr;
    }

    u8* wav = PdfBuildWavFromPcm(src, pcmSize, sampleRate, channels, bits, audioFormat, outSize);
    free(swapped);
    return wav;
}

static bool CopySoundAnnotationAudio(fz_context* ctx, Annotation* annot, u8** dataOut, size_t* sizeOut) {
    pdf_annot* pdfannot = annot->pdfannot;
    pdf_obj* annotObj = pdf_annot_obj(ctx, pdfannot);
    pdf_obj* sound = PdfResolveSoundObject(ctx, annotObj);
    if (!sound) {
        return false;
    }

    fz_buffer* buf = nullptr;
    fz_var(buf);
    fz_try(ctx) {
        buf = pdf_load_stream(ctx, sound);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return false;
    }
    if (!buf) {
        return false;
    }

    unsigned char* data = nullptr;
    size_t size = fz_buffer_storage(ctx, buf, &data);
    if (!data || size == 0) {
        fz_drop_buffer(ctx, buf);
        return false;
    }

    size_t offset = 0;
    if (PdfSoundLooksLikeWav(data, size, &offset)) {
        size_t playSize = size - offset;
        u8* owned = AllocArray<u8>(playSize);
        if (!owned) {
            fz_drop_buffer(ctx, buf);
            return false;
        }
        memcpy(owned, data + offset, playSize);
        fz_drop_buffer(ctx, buf);
        *dataOut = owned;
        *sizeOut = playSize;
        return true;
    }
    if (PdfSoundLooksLikeMp3(data, size)) {
        u8* owned = AllocArray<u8>(size);
        if (!owned) {
            fz_drop_buffer(ctx, buf);
            return false;
        }
        memcpy(owned, data, size);
        fz_drop_buffer(ctx, buf);
        *dataOut = owned;
        *sizeOut = size;
        return true;
    }

    size_t wavSize = 0;
    u8* wav = PdfSoundPcmToWav(ctx, data, size, sound, &wavSize);
    fz_drop_buffer(ctx, buf);
    if (!wav) {
        return false;
    }
    *dataOut = wav;
    *sizeOut = wavSize;
    return true;
}

static bool PdfFilenameExtIsVideo(const char* ext) {
    if (!ext || !*ext) {
        return false;
    }
    return str::EqI(ext, ".mp4") || str::EqI(ext, ".mov") || str::EqI(ext, ".avi") || str::EqI(ext, ".webm") ||
           str::EqI(ext, ".m4v");
}

static bool PdfFilenameExtIsNonAudioMedia(const char* ext) {
    if (!ext || !*ext) {
        return false;
    }
    return str::EqI(ext, ".swf") || PdfFilenameExtIsVideo(ext);
}

static const char* PdfAudioExtWithoutDot(TempStr ext) {
    if (!ext || !*ext) {
        return nullptr;
    }
    return ext[0] == '.' ? ext + 1 : ext;
}

static const char* PdfGuessAudioExtFromBytes(const u8* data, size_t size) {
    size_t off = 0;
    if (PdfSoundLooksLikeWav(data, size, &off)) {
        return "wav";
    }
    if (PdfSoundLooksLikeMp3(data, size)) {
        return "mp3";
    }
    if (size >= 4 && data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C') {
        return "flac";
    }
    if (size >= 4 && data[0] == 0x4F && data[1] == 0x67 && data[2] == 0x67 && data[3] == 0x53) {
        return "ogg";
    }
    if (size >= 12 && data[4] == 'f' && data[5] == 't' && data[6] == 'y' && data[7] == 'p') {
        return "m4a";
    }
    return nullptr;
}

struct PdfEmbeddedAudioFindCtx {
    fz_context* ctx = nullptr;
    u8* data = nullptr;
    size_t size = 0;
    char* ext = nullptr;
    const char* preferredFilename = nullptr;
    bool requirePreferred = false;
};

static bool PdfEmbeddedFilenameMatchesPreferred(const char* filename, const char* preferred) {
    if (!preferred || !*preferred) {
        return true;
    }
    if (!filename || !*filename) {
        return false;
    }
    if (str::EqI(filename, preferred)) {
        return true;
    }
    TempStr baseName = path::GetBaseNameTemp(filename);
    return str::EqI(baseName, preferred);
}

static void PdfTryLoadEmbeddedAudioFilespec(PdfEmbeddedAudioFindCtx* fc, pdf_obj* fs) {
    if (!fc || fc->data || !fc->ctx || !fs || !pdf_is_embedded_file(fc->ctx, fs)) {
        return;
    }
    pdf_filespec_params params{};
    pdf_get_filespec_params(fc->ctx, fs, &params);
    if (fc->requirePreferred && !PdfEmbeddedFilenameMatchesPreferred(params.filename, fc->preferredFilename)) {
        return;
    }
    TempStr fileExt = path::GetExtTemp(params.filename);
    if (PdfFilenameExtIsNonAudioMedia(fileExt)) {
        return;
    }

    fz_buffer* buf = nullptr;
    fz_var(buf);
    fz_try(fc->ctx) {
        buf = pdf_load_embedded_file_contents(fc->ctx, fs);
        if (!buf) {
            fz_throw(fc->ctx, FZ_ERROR_GENERIC, "empty embedded file");
        }
        unsigned char* bytes = nullptr;
        size_t len = fz_buffer_storage(fc->ctx, buf, &bytes);
        if (!bytes || len == 0) {
            fz_throw(fc->ctx, FZ_ERROR_GENERIC, "empty embedded stream");
        }
        const char* playExt = PdfAudioExtWithoutDot(fileExt);
        if (!playExt) {
            playExt = PdfGuessAudioExtFromBytes(bytes, len);
        }
        if (!playExt) {
            fz_throw(fc->ctx, FZ_ERROR_GENERIC, "unknown embedded audio format");
        }
        u8* owned = AllocArray<u8>(len);
        if (!owned) {
            fz_throw(fc->ctx, FZ_ERROR_GENERIC, "alloc failed");
        }
        memcpy(owned, bytes, len);
        fc->data = owned;
        fc->size = len;
        fc->ext = str::Dup(playExt);
    }
    fz_always(fc->ctx) {
        fz_drop_buffer(fc->ctx, buf);
    }
    fz_catch(fc->ctx) {
        fz_report_error(fc->ctx);
    }
}

static void PdfWalkNameTreeNode(fz_context* ctx, pdf_obj* node, PdfEmbeddedAudioFindCtx* fc) {
    if (!node || fc->data) {
        return;
    }
    pdf_obj* names = pdf_dict_get(ctx, node, PDF_NAME(Names));
    if (names) {
        int n = pdf_array_len(ctx, names);
        for (int i = 1; i < n; i += 2) {
            PdfTryLoadEmbeddedAudioFilespec(fc, pdf_array_get(ctx, names, i));
            if (fc->data) {
                return;
            }
        }
    }
    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    if (kids) {
        int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n; i++) {
            PdfWalkNameTreeNode(ctx, pdf_array_get(ctx, kids, i), fc);
            if (fc->data) {
                return;
            }
        }
    }
}

static char* PdfRichMediaPreferredAudioFilename(fz_context* ctx, pdf_obj* annotObj) {
    pdf_obj* settings = pdf_dict_gets(ctx, annotObj, "RichMediaSettings");
    if (!settings) {
        return nullptr;
    }
    pdf_obj* activation = pdf_dict_gets(ctx, settings, "Activation");
    if (!activation) {
        return nullptr;
    }
    pdf_obj* config = pdf_dict_gets(ctx, activation, "Configuration");
    if (!config) {
        return nullptr;
    }
    pdf_obj* instances = pdf_dict_gets(ctx, config, "Instances");
    if (!instances || pdf_array_len(ctx, instances) == 0) {
        return nullptr;
    }
    pdf_obj* instance = pdf_array_get(ctx, instances, 0);
    if (!instance) {
        return nullptr;
    }
    pdf_obj* params = pdf_dict_gets(ctx, instance, "Params");
    if (!params) {
        return nullptr;
    }
    pdf_obj* flashVars = pdf_dict_gets(ctx, params, "FlashVars");
    if (!flashVars) {
        return nullptr;
    }
    const char* fv = pdf_to_text_string(ctx, flashVars);
    if (!fv) {
        return nullptr;
    }
    const char* source = str::Find(fv, "source=");
    if (!source) {
        source = str::Find(fv, "Source=");
    }
    if (!source) {
        return nullptr;
    }
    source += 7;
    size_t len = 0;
    while (source[len] && source[len] != '&') {
        len++;
    }
    if (len == 0) {
        return nullptr;
    }
    return str::Dup(source, len);
}

static bool CopyRichMediaAnnotationAudio(fz_context* ctx, pdf_obj* annotObj, u8** dataOut, size_t* sizeOut) {
    pdf_obj* content = pdf_dict_gets(ctx, annotObj, "RichMediaContent");
    if (!content) {
        return false;
    }
    pdf_obj* assets = pdf_dict_gets(ctx, content, "Assets");
    if (!assets) {
        return false;
    }
    char* preferred = PdfRichMediaPreferredAudioFilename(ctx, annotObj);
    PdfEmbeddedAudioFindCtx fc{};
    fc.ctx = ctx;
    fc.preferredFilename = preferred;
    fc.requirePreferred = preferred != nullptr;
    PdfWalkNameTreeNode(ctx, assets, &fc);
    if (!fc.data && preferred) {
        fc.requirePreferred = false;
        PdfWalkNameTreeNode(ctx, assets, &fc);
    }
    str::Free(preferred);
    if (!fc.data) {
        return false;
    }
    *dataOut = fc.data;
    *sizeOut = fc.size;
    str::Free(fc.ext);
    return true;
}

static bool PdfObjNameEquals(fz_context* ctx, pdf_obj* obj, const char* name) {
    return obj && pdf_is_name(ctx, obj) && str::Eq(pdf_to_name(ctx, obj), name);
}

static pdf_obj* PdfScreenRenditionFromAction(fz_context* ctx, pdf_obj* action) {
    if (!action) {
        return nullptr;
    }
    if (pdf_is_array(ctx, action)) {
        int n = pdf_array_len(ctx, action);
        for (int i = 0; i < n; i++) {
            pdf_obj* one = pdf_array_get(ctx, action, i);
            if (!PdfObjNameEquals(ctx, pdf_dict_get(ctx, one, PDF_NAME(S)), "Rendition")) {
                continue;
            }
            pdf_obj* r = pdf_dict_get(ctx, one, PDF_NAME(R));
            if (pdf_is_array(ctx, r)) {
                r = pdf_array_get(ctx, r, 0);
            }
            if (r) {
                return r;
            }
        }
        return nullptr;
    }
    if (!PdfObjNameEquals(ctx, pdf_dict_get(ctx, action, PDF_NAME(S)), "Rendition")) {
        return nullptr;
    }
    pdf_obj* r = pdf_dict_get(ctx, action, PDF_NAME(R));
    if (pdf_is_array(ctx, r)) {
        r = pdf_array_get(ctx, r, 0);
    }
    return r;
}

// Screen annotations often put the media rendition on an /A action
// (/S /Rendition /R <<...>>) rather than on the annotation itself.
static pdf_obj* PdfScreenResolveRendition(fz_context* ctx, pdf_obj* annotObj) {
    pdf_obj* rendition = pdf_dict_get(ctx, annotObj, PDF_NAME(R));
    if (pdf_is_array(ctx, rendition)) {
        rendition = pdf_array_get(ctx, rendition, 0);
    }
    if (rendition) {
        return rendition;
    }

    rendition = PdfScreenRenditionFromAction(ctx, pdf_dict_get(ctx, annotObj, PDF_NAME(A)));
    if (rendition) {
        return rendition;
    }

    // Additional actions (e.g. /AA /U for mouse-up)
    pdf_obj* aa = pdf_dict_get(ctx, annotObj, PDF_NAME(AA));
    if (!aa) {
        return nullptr;
    }
    const char* keys[] = {"U", "D", "E", "X", "Fo", "Bl", "PO", "PC", "PV", "PI"};
    for (const char* key : keys) {
        rendition = PdfScreenRenditionFromAction(ctx, pdf_dict_gets(ctx, aa, key));
        if (rendition) {
            return rendition;
        }
    }
    return nullptr;
}

// Media Rendition (/S /MR) -> Media Clip Data (/S /MCD) -> /D filespec
static pdf_obj* PdfScreenResolveFilespec(fz_context* ctx, pdf_obj* annotObj) {
    pdf_obj* rendition = PdfScreenResolveRendition(ctx, annotObj);
    if (!rendition) {
        return nullptr;
    }

    pdf_obj* clip = pdf_dict_get(ctx, rendition, PDF_NAME(C));
    if (clip) {
        // Media Clip Data: the embedded file is in /D
        pdf_obj* data = pdf_dict_get(ctx, clip, PDF_NAME(D));
        if (data && pdf_is_embedded_file(ctx, data)) {
            return data;
        }
        if (pdf_is_embedded_file(ctx, clip)) {
            return clip;
        }
        if (data) {
            return data;
        }
        return clip;
    }
    return pdf_dict_get(ctx, rendition, PDF_NAME(D));
}

static bool CopyScreenAnnotationAudio(fz_context* ctx, pdf_obj* annotObj, u8** dataOut, size_t* sizeOut) {
    pdf_obj* fs = PdfScreenResolveFilespec(ctx, annotObj);
    if (!fs) {
        return false;
    }
    PdfEmbeddedAudioFindCtx fc{};
    fc.ctx = ctx;
    PdfTryLoadEmbeddedAudioFilespec(&fc, fs);
    if (!fc.data) {
        return false;
    }
    *dataOut = fc.data;
    *sizeOut = fc.size;
    str::Free(fc.ext);
    return true;
}

bool AnnotationSupportsMediaPlayback(AnnotationType tp) {
    return tp == AnnotationType::Sound || tp == AnnotationType::RichMedia || tp == AnnotationType::Screen;
}

static u64 PdfMediaPlayToken(EngineMupdf* engine, int objNum) {
    return ((u64)(uintptr_t)engine) ^ (((u64)(u32)objNum << 1) | 1ull);
}

u64 AnnotationEmbeddedAudioToken(Annotation* annot) {
    if (!annot || !annot->pdfannot || !annot->engine) {
        return 0;
    }
    EngineMupdf* engine = annot->engine;
    fz_context* ctx = engine->Ctx();
    if (!ctx) {
        return 0;
    }
    int objNum = 0;
    ScopedCritSec cs(&engine->docLock);
    fz_try(ctx) {
        objNum = pdf_to_num(ctx, pdf_annot_obj(ctx, annot->pdfannot));
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return 0;
    }
    if (objNum <= 0) {
        return 0;
    }
    return PdfMediaPlayToken(engine, objNum);
}

bool AnnotationCopyEmbeddedAudio(Annotation* annot, u8** dataOut, size_t* sizeOut) {
    if (!annot || !dataOut || !sizeOut || !annot->pdfannot || !annot->engine) {
        return false;
    }
    *dataOut = nullptr;
    *sizeOut = 0;
    if (!AnnotationSupportsMediaPlayback(annot->type)) {
        return false;
    }
    EngineMupdf* engine = annot->engine;
    fz_context* ctx = engine->Ctx();
    if (!ctx || !engine->pdfdoc) {
        return false;
    }
    bool ok = false;
    ScopedCritSec cs(&engine->docLock);
    fz_try(ctx) {
        pdf_obj* annotObj = pdf_annot_obj(ctx, annot->pdfannot);
        switch (annot->type) {
            case AnnotationType::Sound:
                ok = CopySoundAnnotationAudio(ctx, annot, dataOut, sizeOut);
                break;
            case AnnotationType::RichMedia:
                ok = CopyRichMediaAnnotationAudio(ctx, annotObj, dataOut, sizeOut);
                break;
            case AnnotationType::Screen:
                ok = CopyScreenAnnotationAudio(ctx, annotObj, dataOut, sizeOut);
                break;
            default:
                break;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    if (!ok) {
        free(*dataOut);
        *dataOut = nullptr;
        *sizeOut = 0;
    }
    return ok && *dataOut != nullptr && *sizeOut > 0;
}

bool PlaySoundAnnotation(Annotation* annot) {
    if (!annot || !AnnotationSupportsMediaPlayback(annot->type)) {
        return false;
    }
    u64 token = AnnotationEmbeddedAudioToken(annot);
    if (token != 0 && LookupAudioIsPlaying() && LookupAudioPlayToken() == token) {
        LookupAudioStop();
        return true;
    }
    u8* data = nullptr;
    size_t size = 0;
    if (!AnnotationCopyEmbeddedAudio(annot, &data, &size)) {
        return false;
    }
    return PdfPageAudioPlayClip(annot, data, size);
}
