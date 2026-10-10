/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
#include <uxtheme.h>
#include "utils/BitManip.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "Annotation.h"
#include "EbookFontMenu.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "Translations.h"
#include "AnnotationNotesExport.h"
#include "SumatraConfig.h"
#include "GlobalPrefs.h"
#include "DisplayModel.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "MainWindow.h"
#include "Toolbar.h"
#include "WindowTab.h"
#include "EditAnnotations.h"
#include "SumatraPDF.h"
#include "SidebarThumbs.h"
#include "Canvas.h"
#include "Commands.h"
#include "DarkModeSubclass.h"
#include "EbookAnnotations.h"
#include "EditEbookAnnotations.h"
#include "Selection.h"
#include "RenderCache.h"

#include "utils/Log.h"

#include "theme.h"
#include "AppDialogTheme.h"
#include "AnnotationSidebar.h"
#include "FloatingPopupStyle.h"
#include "PdfDarkMode.h"

extern RenderCache* gRenderCache;

static HWND gFreeTextToolbarHwnd = nullptr;

static void RerenderPdfAnnotationChange(WindowTab* tab, Annotation* overlayAnnot) {
    if (!tab) {
        return;
    }
    Annotation* annot = overlayAnnot ? overlayAnnot : tab->selectedAnnotation;
    int pageNo = annot ? annot->pageNo : 0;
    Annotation* overlay = nullptr;
    if (overlayAnnot && IsPdfTextMarkupAnnotation(overlayAnnot)) {
        overlay = overlayAnnot;
    } else if (annot && IsPdfTextMarkupAnnotation(annot)) {
        overlay = annot;
    }
    MainWindowRerenderAnnotationChange(tab->win, pageNo, overlay);
}

static COLORREF ColorRefFromPdfColor(PdfColor c) {
    if (c == 0) {
        return MkColor(0xff, 0xff, 0x00);
    }
    u8 r, g, b, a;
    UnpackPdfColor(c, r, g, b, a);
    return MkColor(r, g, b);
}

COLORREF ColorRefFromPdfAnnotationColor(PdfColor c) {
    return ColorRefFromPdfColor(c);
}

PdfColor PdfAnnotationColorFromColorRef(COLORREF c) {
    u8 r, g, b;
    UnpackColor(c, r, g, b);
    return MkPdfColor(r, g, b, 0xff);
}

// Page pixels under a free-text box with annotations hidden. The stand-in
// blits this before drawing text, so the stale tile's glyphs are covered
// instead of showing through as a second copy.
static HBITMAP CaptureFreeTextBackdrop(DisplayModel* dm, int pageNo, Rect screen);

struct FreeTextOverlayCover {
    Annotation* annot = nullptr;
    HBITMAP bmp = nullptr;
    RectF bounds;
};

static FreeTextOverlayCover gFreeTextOverlayCover;

static void DiscardFreeTextOverlayCover() {
    if (gFreeTextOverlayCover.bmp) {
        DeleteObject(gFreeTextOverlayCover.bmp);
        gFreeTextOverlayCover.bmp = nullptr;
    }
    gFreeTextOverlayCover.annot = nullptr;
    gFreeTextOverlayCover.bounds = {};
}

static bool SameFreeTextCoverBounds(RectF a, RectF b) {
    return fabsf(a.x - b.x) < 0.05f && fabsf(a.y - b.y) < 0.05f && fabsf(a.dx - b.dx) < 0.05f &&
           fabsf(a.dy - b.dy) < 0.05f;
}

void RemovePdfMarkupOverlayAnnot(WindowTab* tab, Annotation* annot) {
    if (!tab || !annot) {
        return;
    }
    if (gFreeTextOverlayCover.annot == annot) {
        DiscardFreeTextOverlayCover();
    }
    for (int i = tab->pdfMarkupOverlays.size() - 1; i >= 0; i--) {
        if (tab->pdfMarkupOverlays.at(i).annot == annot) {
            tab->pdfMarkupOverlays.RemoveAt(i);
        }
    }
}

void ClearPdfMarkupOverlayForPage(WindowTab* tab, int pageNo) {
    if (!tab || pageNo <= 0) {
        return;
    }
    if (gFreeTextOverlayCover.annot && gFreeTextOverlayCover.annot->pageNo == pageNo) {
        DiscardFreeTextOverlayCover();
    }
    for (int i = tab->pdfMarkupOverlays.size() - 1; i >= 0; i--) {
        if (tab->pdfMarkupOverlays.at(i).pageNo == pageNo) {
            tab->pdfMarkupOverlays.RemoveAt(i);
        }
    }
}

// Same 6pt corner as pdf_write_square_appearance() and the drag preview.
// The stand-in used to stroke a sharp rectangle, so releasing the drag
// flashed square corners until the rounded page tile arrived.
static void DrawPdfRoundedSquare(Gdiplus::Graphics& gs, Gdiplus::Pen* pen, Gdiplus::Brush* fill, float x, float y,
                                 float w, float h, float zoom) {
    float radius = std::min(6.f * zoom, std::min(w, h) / 4.f);
    float diameter = radius * 2.f;
    if (diameter < 1.f || w < diameter || h < diameter) {
        if (fill) {
            gs.FillRectangle(fill, x, y, w, h);
        }
        if (pen) {
            gs.DrawRectangle(pen, x, y, w, h);
        }
        return;
    }
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180.f, 90.f);
    path.AddArc(x + w - diameter, y, diameter, diameter, 270.f, 90.f);
    path.AddArc(x + w - diameter, y + h - diameter, diameter, diameter, 0.f, 90.f);
    path.AddArc(x, y + h - diameter, diameter, diameter, 90.f, 90.f);
    path.CloseFigure();
    if (fill) {
        gs.FillPath(fill, &path);
    }
    if (pen) {
        gs.DrawPath(pen, &path);
    }
}

static void PaintPdfStrokeOverlay(HDC hdc, DisplayModel* dm, int pageNo, Annotation* annot) {
    float lw = BorderWidthF(annot);
    float zoom = dm->GetZoomReal(pageNo);
    if (zoom < 0.05f) {
        zoom = 1.f;
    }
    float strokePx = std::max(0.5f, lw * zoom);
    COLORREF color = ColorRefFromPdfColor(GetColor(annot));
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    Gdiplus::Graphics gs(hdc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(255, r, g, b), strokePx);
    AnnotationType type = annot->type;
    if (type == AnnotationType::Ink) {
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Vec<PointF> points;
        Vec<int> counts;
        GetInkStrokes(annot, points, counts);
        int at = 0;
        for (int n : counts) {
            if (n >= 2 && at + n <= points.Size()) {
                Point prev = dm->CvtToScreen(pageNo, points.at(at));
                for (int k = 1; k < n; k++) {
                    Point cur = dm->CvtToScreen(pageNo, points.at(at + k));
                    gs.DrawLine(&pen, prev.x, prev.y, cur.x, cur.y);
                    prev = cur;
                }
            }
            at += n;
        }
        return;
    }
    if (type == AnnotationType::Line) {
        PointF start, end;
        if (!GetLinePoints(annot, start, end)) {
            return;
        }
        Point sa = dm->CvtToScreen(pageNo, start);
        Point sb = dm->CvtToScreen(pageNo, end);
        gs.DrawLine(&pen, sa.x, sa.y, sb.x, sb.y);
        return;
    }
    if (type != AnnotationType::Square && type != AnnotationType::Circle) {
        return;
    }
    RectF page = GetBounds(annot);
    float inset = lw / 2.f;
    if (page.dx <= lw || page.dy <= lw) {
        inset = 0.f;
    }
    RectF inner = RectF(page.x + inset, page.y + inset, page.dx - inset * 2.f, page.dy - inset * 2.f);
    Rect screen = dm->CvtToScreen(pageNo, inner);
    if (screen.IsEmpty()) {
        return;
    }
    PdfColor interior = InteriorColor(annot);
    Gdiplus::SolidBrush fillBrush(Gdiplus::Color(255, 255, 255));
    Gdiplus::Brush* fill = nullptr;
    if (interior != 0) {
        COLORREF fillCol = ColorRefFromPdfColor(interior);
        u8 fr, fg, fb;
        UnpackColor(fillCol, fr, fg, fb);
        fillBrush.SetColor(Gdiplus::Color(255, fr, fg, fb));
        fill = &fillBrush;
    }
    if (type == AnnotationType::Circle) {
        if (fill) {
            gs.FillEllipse(fill, screen.x, screen.y, screen.dx, screen.dy);
        }
        gs.DrawEllipse(&pen, screen.x, screen.y, screen.dx, screen.dy);
    } else {
        DrawPdfRoundedSquare(gs, &pen, fill, (float)screen.x, (float)screen.y, (float)screen.dx, (float)screen.dy,
                             zoom);
    }
}

static COLORREF DeletedAnnotCoverColor();

// Device pixels of a PDF stroke: points times pixels-per-point. Width under
// half a point is the stored hairline that free text treats as no border.
static int FreeTextBorderPixels(float pt, float scale, int box) {
    if (pt < 0.5f || scale <= 0) {
        return 0;
    }
    int px = (int)(pt * scale + 0.5f);
    if (px < 1) {
        px = 1;
    }
    int limit = box > 8 ? box / 2 - 1 : px;
    if (px > limit) {
        px = limit;
    }
    return px;
}

// The PDF appearance strokes a rectangle inset by half the width, so the ink
// occupies the outer band. Fill that band instead of a centered pen.
static void FillOuterBorderBand(HDC hdc, const RECT& outer, int px, COLORREF col) {
    if (!hdc || px <= 0) {
        return;
    }
    int w = outer.right - outer.left;
    int h = outer.bottom - outer.top;
    if (w <= px * 2 || h <= px * 2) {
        return;
    }
    HBRUSH brush = CreateSolidBrush(col);
    RECT band{outer.left, outer.top, outer.right, outer.top + px};
    FillRect(hdc, &band, brush);
    band = {outer.left, outer.bottom - px, outer.right, outer.bottom};
    FillRect(hdc, &band, brush);
    band = {outer.left, outer.top + px, outer.left + px, outer.bottom - px};
    FillRect(hdc, &band, brush);
    band = {outer.right - px, outer.top + px, outer.right, outer.bottom - px};
    FillRect(hdc, &band, brush);
    DeleteObject(brush);
}

static void PaintPdfFreeTextOverlay(HDC hdc, DisplayModel* dm, int pageNo, Annotation* annot) {
    Rect screen = dm->CvtToScreen(pageNo, GetBounds(annot));
    if (screen.dx < 2 || screen.dy < 2) {
        return;
    }
    PdfColor fill = GetColor(annot);
    PdfColor textCol = DefaultAppearanceTextColor(annot);
    COLORREF fg = textCol == 0 ? RGB(0, 0, 0) : ColorRefFromPdfColor(textCol);
    RECT rc = ToRECT(screen);
    // 0 is a real transparent fill. ColorRefFromPdfColor maps 0 to yellow, which
    // flashed a solid block over the box while the page tile caught up.
    // The tile still has the previous glyphs until MuPDF finishes. Cover them
    // with the page (annotations hidden) before drawing this copy, or the two
    // sit on top of each other for as long as the tile takes.
    if (fill != 0) {
        COLORREF bg = fill == kColorUnset ? DeletedAnnotCoverColor() : ColorRefFromPdfColor(fill);
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
    } else {
        RectF bounds = GetBounds(annot);
        if (!gFreeTextOverlayCover.bmp || gFreeTextOverlayCover.annot != annot ||
            !SameFreeTextCoverBounds(gFreeTextOverlayCover.bounds, bounds)) {
            DiscardFreeTextOverlayCover();
            HBITMAP bmp = CaptureFreeTextBackdrop(dm, pageNo, screen);
            if (bmp) {
                gFreeTextOverlayCover.bmp = bmp;
                gFreeTextOverlayCover.annot = annot;
                gFreeTextOverlayCover.bounds = bounds;
            }
        }
        if (gFreeTextOverlayCover.bmp) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, gFreeTextOverlayCover.bmp);
            BITMAP bm{};
            GetObject(gFreeTextOverlayCover.bmp, sizeof(bm), &bm);
            if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                StretchBlt(hdc, screen.x, screen.y, screen.dx, screen.dy, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            }
            SelectObject(mem, old);
            DeleteDC(mem);
        }
    }

    float border = BorderWidthF(annot);
    int box = std::min(screen.dx, screen.dy);
    int penW = FreeTextBorderPixels(border, dm->GetZoomReal(pageNo), box);
    if (penW > 0) {
        COLORREF frame = ColorRefFromPdfColor(FreeTextBorderColor(annot));
        FillOuterBorderBand(hdc, rc, penW, frame);
    }
    int sizePt = DefaultAppearanceTextSize(annot);
    if (sizePt <= 0) {
        sizePt = 12;
    }
    Rect sized = dm->CvtToScreen(pageNo, RectF(0, 0, (float)sizePt, (float)sizePt));
    int px = sized.dy > 0 ? sized.dy : sizePt;
    int padPx = (int)((float)px * 0.4f + 0.5f);
    if (padPx < 1) {
        padPx = 1;
    }
    int inset = penW + padPx;
    int roomX = rc.right - rc.left;
    int roomY = rc.bottom - rc.top;
    int room = roomX < roomY ? roomX : roomY;
    if (inset * 2 >= room) {
        inset = room / 4;
        if (inset < 0) {
            inset = 0;
        }
    }
    InflateRect(&rc, -inset, -inset);
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    const WCHAR* face = FreeTextWindowsFace(DefaultAppearanceTextFont(annot));
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!font) {
        return;
    }
    HGDIOBJ oldFont = SelectObject(hdc, font);
    COLORREF prevFg = SetTextColor(hdc, fg);
    int prevBk = SetBkMode(hdc, TRANSPARENT);
    UINT fmt = DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX | DT_TOP;
    int align = Quadding(annot);
    if (align == 1) {
        fmt |= DT_CENTER;
    } else if (align == 2) {
        fmt |= DT_RIGHT;
    } else {
        fmt |= DT_LEFT;
    }
    TempStr text = Contents(annot);
    text = str::ReplaceTemp(text, "\r\n", "\n");
    text = str::ReplaceTemp(text, "\n", "\r\n");
    TempWStr wide = ToWStrTemp(text ? text : "");
    DrawTextW(hdc, wide, -1, &rc, fmt);
    SetBkMode(hdc, prevBk);
    SetTextColor(hdc, prevFg);
    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void PaintPdfMarkupOverlayPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo) {
    if (!tab || tab->hideAnnotations || !dm || !dm->PageVisible(pageNo) || tab->pdfMarkupOverlays.empty()) {
        return;
    }
    for (auto& entry : tab->pdfMarkupOverlays) {
        if (entry.pageNo != pageNo || !entry.annot) {
            continue;
        }
        if (entry.annot->type == AnnotationType::FreeText) {
            PaintPdfFreeTextOverlay(hdc, dm, pageNo, entry.annot);
            continue;
        }
        if (!IsPdfTextMarkupAnnotation(entry.annot)) {
            PaintPdfStrokeOverlay(hdc, dm, pageNo, entry.annot);
            continue;
        }
        Vec<RectF> pageRects = GetQuadPointsAsRect(entry.annot);
        if (pageRects.empty()) {
            RectF r = GetRect(entry.annot);
            if (!r.IsEmpty()) {
                pageRects.Append(r);
            }
        }
        NormalizeNearbyHighlightHeights(pageRects);
        Vec<Rect> screenRects;
        for (RectF rect : pageRects) {
            Rect screenRect = dm->CvtToScreen(pageNo, rect);
            if (!screenRect.IsEmpty()) {
                screenRects.Append(screenRect);
            }
        }
        COLORREF color = ColorRefFromPdfColor(GetColor(entry.annot));
        PaintTextMarkupOverlay(hdc, tab->win->canvasRc, entry.annot->type, color, screenRects);
    }
}

// The deleted annotation is already gone from the PDF, but the page tile still
// shows it until MuPDF finishes redrawing. Cover that shape with the page color
// so the mark is gone on the next paint. There is no fade.
struct PdfDeletedAnnotCover {
    int pageNo = 0;
    AnnotationType type = AnnotationType::Unknown;
    float borderWidth = 1.f;
    RectF bounds;
    bool hasLine = false;
    PointF lineA;
    PointF lineB;
    bool fillInterior = false;
    Vec<RectF> quads;
    Vec<PointF> inkPoints;
    Vec<int> inkCounts;
};

static COLORREF DeletedAnnotCoverColor() {
    COLORREF bg = RGB(255, 255, 255);
    ThemePageRenderColors(bg, true);
    return bg;
}

static PdfDeletedAnnotCover* RememberPdfDeletedAnnotCover(WindowTab* tab, Annotation* annot) {
    if (!tab || !annot || !annot->pdfannot) {
        return nullptr;
    }
    auto* cover = new PdfDeletedAnnotCover();
    cover->pageNo = annot->pageNo;
    cover->type = annot->type;
    cover->borderWidth = BorderWidthF(annot);
    cover->bounds = GetBounds(annot);
    switch (annot->type) {
        case AnnotationType::Ink:
            GetInkStrokes(annot, cover->inkPoints, cover->inkCounts);
            break;
        case AnnotationType::Line:
            cover->hasLine = GetLinePoints(annot, cover->lineA, cover->lineB);
            break;
        case AnnotationType::Square:
        case AnnotationType::Circle:
            cover->fillInterior = InteriorColor(annot) != 0;
            break;
        case AnnotationType::Highlight:
        case AnnotationType::Underline:
        case AnnotationType::Squiggly:
        case AnnotationType::StrikeOut:
        case AnnotationType::Redact:
            cover->quads = GetQuadPointsAsRect(annot);
            break;
        default:
            break;
    }
    tab->pdfDeletedAnnotCovers.Append(cover);
    return cover;
}

static void DropPdfDeletedAnnotCover(WindowTab* tab, PdfDeletedAnnotCover* cover) {
    if (!tab || !cover) {
        return;
    }
    int idx = tab->pdfDeletedAnnotCovers.Find(cover);
    if (idx >= 0) {
        tab->pdfDeletedAnnotCovers.RemoveAt(idx);
    }
    delete cover;
}

static void DeletePdfDeletedAnnotCover(PdfDeletedAnnotCover* cover) {
    delete cover;
}

void ClearPdfDeletedAnnotCovers(WindowTab* tab) {
    if (!tab) {
        return;
    }
    for (PdfDeletedAnnotCover* cover : tab->pdfDeletedAnnotCovers) {
        DeletePdfDeletedAnnotCover(cover);
    }
    tab->pdfDeletedAnnotCovers.Reset();
}

void ClearPdfDeletedAnnotCoversForPage(WindowTab* tab, int pageNo) {
    if (!tab || pageNo <= 0) {
        return;
    }
    for (int i = tab->pdfDeletedAnnotCovers.Size() - 1; i >= 0; i--) {
        PdfDeletedAnnotCover* cover = tab->pdfDeletedAnnotCovers.at(i);
        if (cover && cover->pageNo == pageNo) {
            DeletePdfDeletedAnnotCover(cover);
            tab->pdfDeletedAnnotCovers.RemoveAt(i);
        }
    }
}

static void PaintDeletedCoverStroke(Gdiplus::Graphics& gs, Gdiplus::Pen& pen, const Gdiplus::Color& color,
                                    DisplayModel* dm, int pageNo, PdfDeletedAnnotCover* cover) {
    AnnotationType type = cover->type;
    if (type == AnnotationType::Ink) {
        int at = 0;
        for (int n : cover->inkCounts) {
            if (n >= 2 && at + n <= cover->inkPoints.Size()) {
                Point prev = dm->CvtToScreen(pageNo, cover->inkPoints.at(at));
                for (int k = 1; k < n; k++) {
                    Point cur = dm->CvtToScreen(pageNo, cover->inkPoints.at(at + k));
                    gs.DrawLine(&pen, prev.x, prev.y, cur.x, cur.y);
                    prev = cur;
                }
            }
            at += n;
        }
        return;
    }
    if (type == AnnotationType::Line && cover->hasLine) {
        Point sa = dm->CvtToScreen(pageNo, cover->lineA);
        Point sb = dm->CvtToScreen(pageNo, cover->lineB);
        gs.DrawLine(&pen, sa.x, sa.y, sb.x, sb.y);
        return;
    }
    if (type != AnnotationType::Square && type != AnnotationType::Circle) {
        return;
    }
    RectF page = cover->bounds;
    float lw = cover->borderWidth;
    float inset = lw / 2.f;
    if (page.dx <= lw || page.dy <= lw) {
        inset = 0.f;
    }
    RectF inner = RectF(page.x + inset, page.y + inset, page.dx - inset * 2.f, page.dy - inset * 2.f);
    Rect screen = dm->CvtToScreen(pageNo, inner);
    if (screen.IsEmpty()) {
        return;
    }
    float zoom = dm->GetZoomReal(pageNo);
    if (zoom < 0.05f) {
        zoom = 1.f;
    }
    Gdiplus::SolidBrush brush(color);
    Gdiplus::Brush* fill = cover->fillInterior ? &brush : nullptr;
    if (type == AnnotationType::Circle) {
        if (fill) {
            gs.FillEllipse(fill, screen.x, screen.y, screen.dx, screen.dy);
        }
        gs.DrawEllipse(&pen, screen.x, screen.y, screen.dx, screen.dy);
    } else {
        DrawPdfRoundedSquare(gs, &pen, fill, (float)screen.x, (float)screen.y, (float)screen.dx, (float)screen.dy,
                             zoom);
    }
}

static void PaintDeletedMarkupCover(Gdiplus::Graphics& gs, const Gdiplus::Color& color, DisplayModel* dm, int pageNo,
                                    PdfDeletedAnnotCover* cover) {
    Vec<RectF> quads = cover->quads;
    if (quads.empty() && !cover->bounds.IsEmpty()) {
        quads.Append(cover->bounds);
    }
    Gdiplus::SolidBrush brush(color);
    for (RectF pageRect : quads) {
        Rect screen = dm->CvtToScreen(pageNo, pageRect);
        if (screen.IsEmpty()) {
            continue;
        }
        if (cover->type == AnnotationType::Highlight || cover->type == AnnotationType::Redact) {
            screen.Inflate(1, 1);
            gs.FillRectangle(&brush, screen.x, screen.y, screen.dx, screen.dy);
            continue;
        }
        float h = (float)screen.dy;
        float lineWidth = std::max(3.f, h / 6.f);
        Gdiplus::Pen pen(color, lineWidth);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float x1 = (float)screen.x;
        float x2 = (float)screen.x + (float)screen.dx;
        float yBot = (float)screen.y + (float)screen.dy;
        if (cover->type == AnnotationType::StrikeOut) {
            float y = yBot - h * 3.f / 7.f;
            gs.DrawLine(&pen, x1, y, x2, y);
        } else {
            float y = yBot - h / 7.f;
            gs.DrawLine(&pen, x1, y, x2, y);
        }
    }
}

void PaintPdfDeletedAnnotCoversPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo) {
    if (!tab || !dm || !dm->PageVisible(pageNo) || tab->pdfDeletedAnnotCovers.empty()) {
        return;
    }
    COLORREF bg = DeletedAnnotCoverColor();
    u8 r, g, b;
    UnpackColor(bg, r, g, b);
    Gdiplus::Color gdiBg(255, r, g, b);
    Gdiplus::Graphics gs(hdc);
    gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (PdfDeletedAnnotCover* cover : tab->pdfDeletedAnnotCovers) {
        if (!cover || cover->pageNo != pageNo) {
            continue;
        }
        AnnotationType type = cover->type;
        bool stroke = type == AnnotationType::Ink || type == AnnotationType::Line || type == AnnotationType::Square ||
                      type == AnnotationType::Circle;
        if (stroke && type == AnnotationType::Ink && cover->inkPoints.empty()) {
            stroke = false;
        }
        if (stroke && type == AnnotationType::Line && !cover->hasLine) {
            stroke = false;
        }
        if (stroke) {
            float zoom = dm->GetZoomReal(pageNo);
            if (zoom < 0.05f) {
                zoom = 1.f;
            }
            // Wider than the stored stroke so antialiased edges of the old tile are covered.
            float strokePx = std::max(1.f, cover->borderWidth * zoom) + 3.f;
            Gdiplus::Pen pen(gdiBg, strokePx);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            PaintDeletedCoverStroke(gs, pen, gdiBg, dm, pageNo, cover);
            continue;
        }
        if (type == AnnotationType::Highlight || type == AnnotationType::Underline ||
            type == AnnotationType::Squiggly || type == AnnotationType::StrikeOut || type == AnnotationType::Redact) {
            PaintDeletedMarkupCover(gs, gdiBg, dm, pageNo, cover);
            continue;
        }
        if (cover->bounds.IsEmpty()) {
            continue;
        }
        Rect screen = dm->CvtToScreen(pageNo, cover->bounds);
        if (screen.IsEmpty()) {
            continue;
        }
        screen.Inflate(2, 2);
        Gdiplus::SolidBrush brush(gdiBg);
        gs.FillRectangle(&brush, screen.x, screen.y, screen.dx, screen.dy);
    }
}

// Small Note pictogram (MuPDF-style bars) at the end of a markup line that has
// written contents — so 摘抄 (highlight/underline) and 批注 (with a note) differ on page.
static void PaintMarkupNoteBadge(HDC hdc, HWND hwndDpi, Rect lineRect, COLORREF color) {
    if (lineRect.IsEmpty()) {
        return;
    }
    int size = DpiScale(hwndDpi, 11);
    if (size < 9) {
        size = 9;
    }
    int x = lineRect.x + lineRect.dx - size / 2;
    int y = lineRect.y - size / 4;
    if (y < lineRect.y - size / 2) {
        y = lineRect.y - size / 2;
    }
    u8 r, g, b;
    UnpackColor(color, r, g, b);
    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush fill(Gdiplus::Color(230, r, g, b));
    Gdiplus::Pen frame(Gdiplus::Color(200, 40, 40, 40), 1.f);
    Gdiplus::SolidBrush glyph(Gdiplus::Color(220, 30, 30, 30));
    Gdiplus::Rect bounds(x, y, size - 1, size - 1);
    graphics.FillRectangle(&fill, bounds);
    graphics.DrawRectangle(&frame, bounds);
    float iconSize = (float)std::max(6, size / 2);
    float iconX = (float)x + ((float)size - iconSize) / 2.f;
    float iconY = (float)y + ((float)size - iconSize) / 2.f;
    for (int row = 0; row < 8; row += 2) {
        graphics.FillRectangle(&glyph, Gdiplus::RectF(iconX, iconY + row * iconSize / 8.f, iconSize, iconSize / 8.f));
    }
}

void PaintPdfMarkupNoteBadgesPage(WindowTab* tab, HDC hdc, DisplayModel* dm, int pageNo) {
    if (!tab || tab->hideAnnotations || !dm || !dm->PageVisible(pageNo)) {
        return;
    }
    EngineMupdf* engine = AsEngineMupdf(dm->GetEngine());
    if (!engine || !engine->pdfdoc) {
        return;
    }
    FzPageInfo* pi = engine->GetFzPageInfoCanFail(pageNo);
    if (!pi || pi->annotations.empty()) {
        return;
    }
    HWND hwndDpi = tab->win ? tab->win->hwndFrame : nullptr;
    for (Annotation* annot : pi->annotations) {
        if (!IsPdfTextMarkupAnnotation(annot)) {
            continue;
        }
        TempStr note = Contents(annot);
        if (str::IsEmptyOrWhiteSpace(note)) {
            continue;
        }
        Vec<RectF> pageRects = GetQuadPointsAsRect(annot);
        if (pageRects.empty()) {
            RectF r = GetRect(annot);
            if (!r.IsEmpty()) {
                pageRects.Append(r);
            }
        }
        if (pageRects.empty()) {
            continue;
        }
        RectF last = pageRects.Last();
        Rect screenRect = dm->CvtToScreen(pageNo, last);
        if (screenRect.IsEmpty()) {
            continue;
        }
        PaintMarkupNoteBadge(hdc, hwndDpi, screenRect, ColorRefFromPdfColor(GetColor(annot)));
    }
}

constexpr int borderWidthMin = 0;
constexpr int borderWidthMax = 12;

// clang-format off
static const char *gFileAttachmentUcons = "Graph\0Paperclip\0PushPin\0Tag\0";
static const char *gSoundIcons = "Speaker\0Mic\0";
// those are in order of pdf_line_ending enum in annot.h
static const char *gLineEndingStyles = "None\0Square\0Circle\0Diamond\0OpenArrow\0ClosedArrow\0Butt\0ROpenArrow\0RClosedArrow\0Slash\0";
static const char* gColors = "Transparent\0Aqua\0Black\0Blue\0Fuchsia\0Gray\0Green\0Lime\0Maroon\0Navy\0Olive\0Orange\0Purple\0Red\0Silver\0Teal\0White\0Yellow\0";
static const char* gQuaddingNames = "Left\0Center\0Right\0";

static PdfColor gColorsValues[] = {
	0x00000000, /* transparent */
	0xff00ffff, /* aqua */
	0xff000000, /* black */
	0xff0000ff, /* blue */
	0xffff00ff, /* fuchsia */
	0xff808080, /* gray */
	0xff008000, /* green */
	0xff00ff00, /* lime */
	0xff800000, /* maroon */
	0xff000080, /* navy */
	0xff808000, /* olive */
	0xffffa500, /* orange */
	0xff800080, /* purple */
	0xffff0000, /* red */
	0xffc0c0c0, /* silver */
	0xff008080, /* teal */
	0xffffffff, /* white */
	0xffffff00, /* yellow */
};

// list of annotations where GetColor() returns background color
// TODO: probably incomplete;
static AnnotationType gAnnotsIsColorBackground[] = {
    AnnotationType::FreeText,
};
// clang-format on

const char* GetPdfAnnotationColorNames() {
    return gColors;
}

const char* GetKnownColorName(PdfColor c) {
    int n = (int)dimof(gColorsValues);
    for (int i = 0; i < n; i++) {
        if (c == gColorsValues[i]) {
            const char* s = seqstrings::IdxToStr(gColors, i);
            return s;
        }
    }
    return nullptr;
}

struct AnnotColorSwatch : Wnd {
    COLORREF color = 0;
    bool has = false;

    void OnPaint(HDC hdc, PAINTSTRUCT*) override {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (HBRUSH bg = BackgroundBrush()) {
            FillRect(hdc, &rc, bg);
        }
        if (!has) {
            return;
        }
        int side = std::min(rc.right - rc.left, rc.bottom - rc.top);
        int d = std::max(4, side - DpiScale(hwnd, 2));
        int x = (rc.right - d) / 2;
        int y = (rc.bottom - d) / 2;
        ScopedGdiObj<HBRUSH> br(CreateSolidBrush(color));
        ScopedSelectObject selBr(hdc, br);
        ScopedSelectObject selPen(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, x, y, x + d, y + d);
    }

    Size GetIdealSize() override {
        int d = hwnd ? DpiScale(hwnd, 14) : 14;
        return {d, d};
    }
};

// Type or author stay semibold; page/geometry/date are regular + muted.
// titleColW aligns the muted column across the type and author rows.

struct EditAnnotationsWindow;

// Flex slot above the fixed save footer. No outer scrollbar: when the
// inspector is tight, shrink the note editor instead.
// Trackbar PREPAINT floods the client with a system grey band (especially
// obvious on Light-White). Fill the sidebar panel color and skip that flood;
// channel/thumb still draw via item notifications (DarkMode or default).
constexpr UINT_PTR kAnnotTrackbarBgNotifyId = 0xA11E;

static COLORREF AnnotTrackbarPanelColor() {
    return ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
}

static LRESULT CALLBACK AnnotTrackbarBgNotifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, AnnotTrackbarBgNotifyProc, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_NOTIFY) {
        auto* hdr = (NMHDR*)lp;
        if (hdr && hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom) {
            WCHAR cls[64]{};
            if (GetClassNameW(hdr->hwndFrom, cls, dimof(cls)) > 0 && str::EqI(cls, TRACKBAR_CLASS)) {
                auto* cd = (LPNMCUSTOMDRAW)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    // Let the parent panel show through between channel and ticks.
                    SetWindowLongPtrW(hdr->hwndFrom, GWL_STYLE,
                                      GetWindowLongPtrW(hdr->hwndFrom, GWL_STYLE) | TBS_TRANSPARENTBKGND);
                    RECT rc{};
                    GetClientRect(hdr->hwndFrom, &rc);
                    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(AnnotTrackbarPanelColor()));
                    FillRect(cd->hdc, &rc, br);
                    return CDRF_NOTIFYITEMDRAW | CDRF_SKIPDEFAULT;
                }
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void RenderPdfIconPreview(Annotation* annot, AnnotationIconPreviewRequest* request) {
    request->bitmap =
        RenderAnnotationIconPreviewBitmap(annot, request->name, request->size, request->lineEnding, request->lineStart);
}

static void PreparePdfIconPreviews(AnnotIconDropDown* control, Annotation* annot) {
    control->renderPreview = MkFunc1(RenderPdfIconPreview, annot);
    control->PreparePreviews(annot->type, GetColor(annot));
}

struct EditAnnotationsWindow : Wnd {
    WindowTab* tab = nullptr;
    LayoutBase* mainLayout = nullptr;

    ListBox* listBox = nullptr;
    AnnotHeadingLine* staticHeading = nullptr;
    AnnotHeadingLine* staticMeta = nullptr;
    Static* staticDetails = nullptr;
    AnnotColorSwatch* colorSwatch = nullptr;
    AnnotInspectorPane* inspectorPane = nullptr;
    ILayout* inspectorLayout = nullptr;
    ILayout* footerLayout = nullptr;
    HFONT headingFont = nullptr;
    bool authorEditing = false;
    Static* staticRect = nullptr;
    Static* staticAuthor = nullptr;
    Edit* editAuthor = nullptr;
    Static* staticModificationDate = nullptr;
    Static* staticPopup = nullptr;
    Static* staticContents = nullptr;
    AnnotNoteEdit* editContents = nullptr;
    Static* staticTextAlignment = nullptr;
    DropDown* dropDownTextAlignment = nullptr;
    Static* staticTextFont = nullptr;
    Button* buttonTextFont = nullptr;
    Static* staticTextSize = nullptr;
    Trackbar* trackbarTextSize = nullptr;
    AnnotResetButton* buttonRestoreFreeText = nullptr;
    HBox* resetAppearanceRow = nullptr;
    Static* staticTextColor = nullptr;
    DropDown* dropDownTextColor = nullptr;
    Static* staticBorderColor = nullptr;
    DropDown* dropDownBorderColor = nullptr;

    Static* staticLineStart = nullptr;
    AnnotIconDropDown* dropDownLineStart = nullptr;
    Static* staticLineEnd = nullptr;
    AnnotIconDropDown* dropDownLineEnd = nullptr;

    Static* staticIcon = nullptr;
    AnnotIconDropDown* dropDownIcon = nullptr;

    Static* staticBorder = nullptr;
    Trackbar* trackbarBorder = nullptr;

    Static* staticColor = nullptr;
    DropDown* dropDownColor = nullptr;
    Static* staticInteriorColor = nullptr;
    DropDown* dropDownInteriorColor = nullptr;

    Static* staticOpacity = nullptr;
    Trackbar* trackbarOpacity = nullptr;

    Button* buttonSaveAttachment = nullptr;
    Button* buttonEmbedAttachment = nullptr;

    Button* buttonExport = nullptr;

    Button* buttonSaveToCurrentFile = nullptr;
    Button* buttonSaveToNewFile = nullptr;

    // those are
    Vec<Annotation*> annotations;
    StrVec annotationExcerpts;
    int annotationLocationWidth = 0;

    bool skipGoToPage = false;
    bool updatingControls = false;
    int dpi = 0;

    StrBuilder currTextColor;
    StrBuilder currBorderColor;
    StrBuilder currCustomColor;
    StrBuilder currCustomInteriorColor;

    void OnSize(UINT msg, UINT type, SIZE size) override;
    void OnFocus() override;
    bool PreTranslateMessage(MSG&) override;

    void ListBoxSelectionChanged();

    ~EditAnnotationsWindow() override;
};

// Same slot the deleted row occupied. Deleting the first row selects the
// second; deleting the last row selects the new last. Matches EPUB.
static Annotation* PickNewSelectedAnnotation(EditAnnotationsWindow* ew, int prevIdx) {
    int nAnnots = ew->annotations.Size();
    if (nAnnots == 0) {
        return nullptr;
    }
    if (prevIdx >= nAnnots) {
        prevIdx = nAnnots - 1;
    }
    if (prevIdx < 0) {
        prevIdx = 0;
    }
    return ew->annotations.at(prevIdx);
}

static void UpdateUIForSelectedAnnotation(EditAnnotationsWindow* ew, Annotation* annot, bool isNew,
                                          EditAnnotFocus focus);

void DeleteAnnotationAndUpdateUI(WindowTab* tab, Annotation* annot) {
    EndFreeTextInPlaceEditForTab(tab, false);
    if (!tab || !annot) {
        return;
    }
    EditAnnotationsWindow* ew = tab->editAnnotsWindow;
    Annotation* selectNext = nullptr;
    int deletedIdx = -1;
    int pageNo = annot ? annot->pageNo : 0;
    if (annot != tab->selectedAnnotation) {
        // preserve current selection if we're not deleting it
        selectNext = tab->selectedAnnotation;
    } else if (ew) {
        deletedIdx = ew->annotations.Find(annot);
    }

    // Snapshot the shape first. After delete the PDF object is gone, and the
    // old page bitmap still shows it until the tile is rendered again.
    PdfDeletedAnnotCover* cover = RememberPdfDeletedAnnotCover(tab, annot);
    RemovePdfMarkupOverlayAnnot(tab, annot);
    DeleteAnnotation(annot);
    if (annot->pdfannot) {
        // Delete failed; the mark is still on the page. Do not advance the selection.
        DropPdfDeletedAnnotCover(tab, cover);
        cover = nullptr;
        deletedIdx = -1;
    }
    if (tab->selectedAnnotation == annot) {
        tab->selectedAnnotation = nullptr;
    }
    if (ew != nullptr) {
        // can be null if called from Menu.cpp and annotations window is not visible
        UpdateAnnotationsList(ew);
        if (!selectNext && deletedIdx >= 0) {
            selectNext = PickNewSelectedAnnotation(ew, deletedIdx);
        }
        // Set before the list update navigates, so the page paints this mark.
        tab->selectedAnnotation = selectNext;
        UpdateUIForSelectedAnnotation(ew, selectNext, false, EditAnnotFocus::Default);
    } else if (selectNext) {
        tab->selectedAnnotation = selectNext;
    }
    ToolbarUpdateStateForWindow(tab->win, false);
    // Only this page. SetSelectedAnnotation redraws every visible page, which
    // kept the deleted mark on the stale tile for seconds.
    MainWindowRerenderAnnotationChange(tab->win, pageNo, nullptr);
}

static void DeleteSelectedAnnotation(EditAnnotationsWindow* ew) {
    int idx = ew->listBox->GetCurrentSelection();
    if (idx < 0) {
        // can get out of sync e.g. after UpdateAnnotationsList during save/reload
        ew->tab->selectedAnnotation = nullptr;
        return;
    }
    Annotation* annot = ew->annotations.at(idx);
    if (ew->tab->selectedAnnotation != annot) {
        // can get out of sync if e.g. keyboard navigation in listbox
        // hasn't triggered ListBoxSelectionChanged yet
        ew->tab->selectedAnnotation = annot;
    }
    ew->skipGoToPage = true;
    DeleteAnnotationAndUpdateUI(ew->tab, annot);
    ew->skipGoToPage = false;
}

static void DeleteAnnotationListItem(EditAnnotationsWindow* ew, int idx) {
    if (!ew->annotations.isValidIndex(idx)) return;
    ew->skipGoToPage = true;
    DeleteAnnotationAndUpdateUI(ew->tab, ew->annotations.at(idx));
    ew->skipGoToPage = false;
}

static NO_INLINE EngineMupdf* GetEngineMupdf(EditAnnotationsWindow* ew) {
#if 0
    // TODO: shouldn't happen but seen in crash report
    if (!ew || !ew->tab) {
        return nullptr;
    }
#endif
    DisplayModel* dm = ew->tab->AsFixed();
#if 0
    if (!dm) {
        return nullptr;
    }
#endif
    return AsEngineMupdf(dm->GetEngine());
}

static void HidePerAnnotControls(EditAnnotationsWindow* ew) {
    ew->authorEditing = false;
    ew->staticHeading->SetIsVisible(false);
    ew->staticMeta->SetIsVisible(false);
    if (ew->staticDetails) {
        ew->staticDetails->SetIsVisible(false);
    }
    if (ew->staticRect) {
        ew->staticRect->SetIsVisible(false);
    }
    ew->staticAuthor->SetIsVisible(false);
    ew->editAuthor->SetIsVisible(false);
    ew->staticModificationDate->SetIsVisible(false);
    ew->staticPopup->SetIsVisible(false);
    ew->staticContents->SetIsVisible(false);
    if (ew->colorSwatch) {
        ew->colorSwatch->has = false;
        ew->colorSwatch->SetIsVisible(false);
    }
    ew->editContents->SetIsVisible(false);
    ew->staticTextAlignment->SetIsVisible(false);
    ew->dropDownTextAlignment->SetIsVisible(false);
    ew->staticTextFont->SetIsVisible(false);
    ew->buttonTextFont->SetIsVisible(false);
    ew->staticTextSize->SetIsVisible(false);
    ew->trackbarTextSize->SetIsVisible(false);
    if (ew->buttonRestoreFreeText) {
        ew->buttonRestoreFreeText->SetIsVisible(false);
    }
    if (ew->resetAppearanceRow) {
        ew->resetAppearanceRow->SetVisibility(Visibility::Collapse);
    }
    ew->staticTextColor->SetIsVisible(false);
    ew->dropDownTextColor->SetIsVisible(false);
    if (ew->staticBorderColor) {
        ew->staticBorderColor->SetIsVisible(false);
    }
    if (ew->dropDownBorderColor) {
        ew->dropDownBorderColor->SetIsVisible(false);
    }

    ew->staticLineStart->SetIsVisible(false);
    ew->dropDownLineStart->SetIsVisible(false);
    ew->staticLineEnd->SetIsVisible(false);
    ew->dropDownLineEnd->SetIsVisible(false);

    ew->staticIcon->SetIsVisible(false);
    ew->dropDownIcon->SetIsVisible(false);

    ew->staticBorder->SetIsVisible(false);
    ew->trackbarBorder->SetIsVisible(false);
    ew->staticColor->SetIsVisible(false);
    ew->dropDownColor->SetIsVisible(false);
    ew->staticInteriorColor->SetIsVisible(false);
    ew->dropDownInteriorColor->SetIsVisible(false);

    ew->staticOpacity->SetIsVisible(false);
    ew->trackbarOpacity->SetIsVisible(false);

    ew->buttonSaveAttachment->SetIsVisible(false);
    ew->buttonEmbedAttachment->SetIsVisible(false);
}

static int FindStringInArray(const char* items, const char* toFind, int valIfNotFound = -1) {
    int idx = seqstrings::StrToIdx(items, toFind);
    if (idx < 0) {
        idx = valIfNotFound;
    }
    return idx;
}

static bool IsAnnotationTypeInArray(AnnotationType* arr, size_t arrSize, AnnotationType toFind) {
    for (size_t i = 0; i < arrSize; i++) {
        if (toFind == arr[i]) {
            return true;
        }
    }
    return false;
}

// return true if closed the window, false if there was no window to close
bool CloseAndDeleteEditAnnotationsWindow(WindowTab* tab) {
    EndFreeTextInPlaceEditForTab(tab, true);
    if (!tab->editAnnotsWindow) {
        return false;
    }
    auto ew = tab->editAnnotsWindow;
    tab->editAnnotsWindow = nullptr;
    // this will trigger closing the window
    delete ew;
    return true;
}

EditAnnotationsWindow::~EditAnnotationsWindow() {
    // hacky: we want the position of the main window
    // but the size of client area
    tab->lastEditAnnotsWindowPos = WindowRect(hwnd);
    auto cr = ClientRect(hwnd);
    tab->lastEditAnnotsWindowPos.dx = cr.dx;
    tab->lastEditAnnotsWindowPos.dy = cr.dy;
    tab->lastEditAnnotsWindowDpi = dpi > 0 ? dpi : DpiGet(hwnd);
    tab->lastEditAnnotsWindowMainWidth = WindowRect(tab->win->hwndFrame).dx;

    if (tab->selectedAnnotation != nullptr) {
        tab->selectedAnnotation = nullptr;
        if (!tab->win->isBeingClosed) {
            MainWindowRerender(tab->win);
            ToolbarUpdateStateForWindow(tab->win, false);
        }
    }
    DeleteObject(headingFont);
    headingFont = nullptr;
    delete inspectorLayout;
    inspectorLayout = nullptr;
    inspectorPane = nullptr;
    footerLayout = nullptr;
    delete mainLayout;
}

static bool DidAnnotationsChange(EditAnnotationsWindow* ew) {
    EngineMupdf* engine = GetEngineMupdf(ew);
    if (!engine) { // maybe seen in crash report
        ReportIf(true);
        return false;
    }
    return EngineMupdfHasUnsavedAnnotations(engine);
}

static void EnableSaveIfAnnotationsChanged(EditAnnotationsWindow* ew) {
    bool didChange = DidAnnotationsChange(ew);
    ew->buttonSaveToCurrentFile->SetIsEnabled(didChange);
    ew->buttonSaveToNewFile->SetIsEnabled(didChange);
}

static void CacheAnnotationExcerpts(EditAnnotationsWindow* ew);

void NotifyAnnotationsChanged(EditAnnotationsWindow* ew) {
    if (!ew) {
        return;
    }
    EnableSaveIfAnnotationsChanged(ew);
    CacheAnnotationExcerpts(ew);
    InvalidateRect(ew->listBox->hwnd, nullptr, FALSE);
}

static void GetEditAnnotationsThemeColors(COLORREF& textOut, COLORREF& bgOut) {
    // Dark: same page colors as the Options dialog (BackgroundColor / text).
    textOut = ThemeWindowTextColor();
    bgOut = ThemeUsesDarkChrome() ? ThemeWindowBackgroundColor() : ThemeWindowControlBackgroundColor();
}

struct EditAnnotThemeColors {
    COLORREF text = 0;
    COLORREF bg = 0;
};

static BOOL CALLBACK ApplyThemeColorsToChildWnd(HWND hwnd, LPARAM lparam) {
    auto* colors = (EditAnnotThemeColors*)lparam;
    Wnd* wnd = WndListFindByHwnd(hwnd);
    if (wnd) {
        wnd->SetColors(colors->text, colors->bg);
    }
    return TRUE;
}

// Paint only the closed native color combo and the secondary save action.
// Native dropdown, keyboard handling and button commands remain unchanged.

static void ApplyEditAnnotationsWindowTheme(EditAnnotationsWindow* ew, bool installDarkMode) {
    if (!ew || !ew->hwnd) {
        return;
    }
    EditAnnotThemeColors colors;
    GetEditAnnotationsThemeColors(colors.text, colors.bg);
    ew->SetColors(colors.text, colors.bg);
    EnumChildWindows(ew->hwnd, ApplyThemeColorsToChildWnd, (LPARAM)&colors);
    ew->editContents->SetColors(colors.text, ThemeAnnotationContentsEditBackgroundColor());
    UpdateAnnotationContentsEditChrome(ew->editContents, ew->editContents);
    ew->editAuthor->SetColors(colors.text, ThemeAnnotationContentsEditBackgroundColor());
    UpdateAnnotationContentsEditChrome(ew->editAuthor);
    COLORREF secondary = ThemeInspectorSecondaryTextColor();
    if (ew->staticHeading) {
        ew->staticHeading->SetColors(colors.text, colors.bg);
    }
    if (ew->staticMeta) {
        ew->staticMeta->SetColors(colors.text, colors.bg);
    }
    if (ew->staticDetails) {
        ew->staticDetails->SetColors(secondary, colors.bg);
    }
    if (ew->staticRect) {
        ew->staticRect->SetColors(secondary, colors.bg);
    }
    if (ew->staticPopup) {
        ew->staticPopup->SetColors(secondary, colors.bg);
    }
    if (ew->colorSwatch) {
        ew->colorSwatch->SetColors(colors.text, colors.bg);
    }
    if (ew->listBox) {
        ew->listBox->SetColors(colors.text, colors.bg);
    }
    if (ew->inspectorPane) {
        ew->inspectorPane->SetColors(colors.text, colors.bg);
    }

    if (UseDarkModeLib()) {
        if (ThemeUsesDarkChrome()) {
            // Same DarkMode stack as Options (AppDialogApplyChrome), plus notify
            // on the inspector pane so Border trackbars custom-draw correctly.
            DarkMode::setDarkWndNotifySafe(ew->hwnd);
            DarkMode::setWindowEraseBgSubclass(ew->hwnd);
            if (ew->inspectorPane && ew->inspectorPane->hwnd) {
                DarkMode::setWindowNotifyCustomDrawSubclass(ew->inspectorPane->hwnd);
                DarkMode::setWindowCtlColorSubclass(ew->inspectorPane->hwnd);
                DarkMode::setChildCtrlsSubclassAndTheme(ew->inspectorPane->hwnd);
            }
        } else if (installDarkMode) {
            DarkMode::setDarkWndNotifySafe(ew->hwnd);
            DarkMode::setWindowEraseBgSubclass(ew->hwnd);
        } else {
            DarkMode::setWindowCtlColorSubclass(ew->hwnd);
            DarkMode::setChildCtrlsTheme(ew->hwnd);
        }
    }
    // Light-White (and Warm): kill the grey trackbar client band so only the
    // thin channel shows, matching the Warm sidebar look.
    if (ew->inspectorPane && ew->inspectorPane->hwnd) {
        RemoveWindowSubclass(ew->inspectorPane->hwnd, AnnotTrackbarBgNotifyProc, kAnnotTrackbarBgNotifyId);
        SetWindowSubclass(ew->inspectorPane->hwnd, AnnotTrackbarBgNotifyProc, kAnnotTrackbarBgNotifyId, 0);
    }
    UpdateWindowCaptionTheme(ew->hwnd);
    // Warm + dark: custom push paint so Delete/Export/Save match Options chrome.
    AppDialogSyncWarmPushButtons(ew->hwnd);
    RemoveWindowSubclass(ew->dropDownColor->hwnd, AnnotationSecondaryButtonProc, 0xA11D);
    RemoveWindowSubclass(ew->buttonSaveToNewFile->hwnd, AnnotationSecondaryButtonProc, 0xA11D);

    uint flags = RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN;
    RedrawWindow(ew->hwnd, nullptr, nullptr, flags);
}

void RefreshEditAnnotationsWindowsTheme() {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (tab->editAnnotsWindow) {
                ApplyEditAnnotationsWindowTheme(tab->editAnnotsWindow, false);
            }
        }
    }
}

void DockOpenEditAnnotationsWindows(MainWindow*) {
    // The editor is a child of the sidebar. Frame layout places it.
}

HWND EditAnnotationsSidebarHwnd(WindowTab* tab) {
    if (!tab || !tab->editAnnotsWindow) {
        return nullptr;
    }
    return tab->editAnnotsWindow->hwnd;
}

bool AnnotationsSidebarIsShowing(WindowTab* tab) {
    if (!tab || !tab->win || !tab->win->tocVisible) {
        return false;
    }
    HWND hwnd = EditAnnotationsSidebarHwnd(tab);
    if (!hwnd) {
        hwnd = EbookAnnotationsSidebarHwnd(tab);
    }
    return hwnd && IsWindowVisible(hwnd);
}

void SyncEditAnnotationsSidebar(MainWindow* win, bool show) {
    if (!win) {
        return;
    }
    WindowTab* cur = win->CurrentTab();
    for (WindowTab* tab : win->Tabs()) {
        EditAnnotationsWindow* ew = tab->editAnnotsWindow;
        if (!ew || !ew->hwnd) {
            continue;
        }
        bool on = show && tab == cur;
        ShowWindow(ew->hwnd, on ? SW_SHOWNA : SW_HIDE);
    }
}

void CloseEditAnnotationsWindowsForDpiMove(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (tab->editAnnotsWindow) {
            tab->reopenEditAnnotsAfterDpiMove = true;
            CloseAndDeleteEditAnnotationsWindow(tab);
        }
    }
}

void ReopenEditAnnotationsWindowsAfterDpiMove(MainWindow* win) {
    if (!win) {
        return;
    }
    for (WindowTab* tab : win->Tabs()) {
        if (!tab->reopenEditAnnotsAfterDpiMove) {
            continue;
        }
        tab->reopenEditAnnotsAfterDpiMove = false;
        ShowEditAnnotationsWindow(tab, nullptr, EditAnnotFocus::Default, false);
    }
}

static TempStr PdfAnnotationExcerptTemp(DisplayModel* dm, Annotation* annot) {
    if (annot->type == AnnotationType::FreeText) {
        return str::DupTemp(Contents(annot));
    }
    TempStr excerpt = MarkupTextTemp(annot);
    if ((annot->type == AnnotationType::Square || annot->type == AnnotationType::Circle) && dm &&
        dm->GetEngine() == annot->engine) {
        // Use actual page text only, without initiating OCR or borrowing notes.
        PageTextUtf8 text = annot->engine->ExtractPageTextUtf8(annot->pageNo);
        bool hasText = !str::IsEmptyOrWhiteSpace(text.text);
        FreePageTextUtf8(&text);
        if (hasText) {
            char* regionText = dm->GetTextInRegion(annot->pageNo, GetRect(annot), true);
            excerpt = str::DupTemp(regionText);
            str::Free(regionText);
        }
    }
    if (str::IsEmptyOrWhiteSpace(excerpt) &&
        (annot->type == AnnotationType::Ink || annot->type == AnnotationType::Line ||
         annot->type == AnnotationType::Square || annot->type == AnnotationType::Circle)) {
        RectF bounds = GetBounds(annot);
        excerpt =
            str::FormatTemp("x=%d y=%d dx=%d dy=%d", (int)bounds.x, (int)bounds.y, (int)bounds.dx, (int)bounds.dy);
    }
    return excerpt;
}

static void CacheAnnotationExcerpts(EditAnnotationsWindow* ew) {
    ew->annotationExcerpts.Reset();
    ew->annotationLocationWidth = DpiScale(ew->hwnd, 22);
    HFONT font = (HFONT)SendMessageW(ew->listBox->hwnd, WM_GETFONT, 0, 0);
    DisplayModel* dm = ew->tab->AsFixed();
    for (Annotation* annot : ew->annotations) {
        TempStr location = str::FormatTemp("%d", annot->pageNo);
        ew->annotationLocationWidth =
            std::max(ew->annotationLocationWidth, HwndMeasureText(ew->listBox->hwnd, location, font).dx);
        TempStr excerpt = PdfAnnotationExcerptTemp(dm, annot);
        if (excerpt) str::NormalizeWSInPlace(excerpt);
        ew->annotationExcerpts.Append(excerpt ? excerpt : "");
    }
}

static void RebuildAnnotationsListBox(EditAnnotationsWindow* ew) {
    CacheAnnotationExcerpts(ew);
    auto model = new ListBoxModelStrings();
    int n = 0;
    n = ew->annotations.Size();

    StrBuilder s;
    for (int i = 0; i < n; i++) {
        auto annot = ew->annotations.at(i);
        s.Reset();
        // Owner-draw reads the annotation. The string is the accessible fallback.
        const char* name = trans::GetTranslation(AnnotationReadableNameTemp(annot->type));
        s.AppendFmt("%d  %s", annot->pageNo, name);
        const char* excerpt = ew->annotationExcerpts.At(i);
        if (!str::IsEmpty(excerpt)) {
            s.AppendFmt("  %s", excerpt);
        }
        model->strings.Append(s.Get());
    }

    auto topIdx = ListBoxGetTopIndex(ew->listBox->hwnd);
    ew->listBox->SetModel(model);
    topIdx = std::min(ew->listBox->GetCount() - 1, topIdx);
    if (topIdx >= 0) {
        ListBoxSetTopIndex(ew->listBox->hwnd, topIdx);
    }
    EnableSaveIfAnnotationsChanged(ew);
    if (ew->buttonExport) {
        ew->buttonExport->SetIsEnabled(n > 0);
    }
}

struct PdfAnnotationSortItem {
    Annotation* annotation = nullptr;
    int pageNo = 0;
    float sortY = 0.f;
    float sortX = 0.f;
};

static void AppendUtcDateTime(StrBuilder& s, time_t secs) {
    if (secs <= 0) {
        return;
    }
    struct tm tm;
    gmtime_s(&tm, &secs);
    char buf[100];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    s.Append(buf);
}

static bool BuildPdfAnnotationsExport(WindowTab* tab, StrBuilder& out) {
    if (!tab || !EngineSupportsAnnotations(tab->GetEngine())) {
        return false;
    }
    Vec<Annotation*> annotations;
    EngineMupdfGetAnnotations(tab->GetEngine(), annotations);
    if (annotations.empty()) {
        return false;
    }

    Vec<PdfAnnotationSortItem> items;
    for (Annotation* annotation : annotations) {
        PdfAnnotationSortItem item;
        item.annotation = annotation;
        item.pageNo = PageNo(annotation);
        RectF bounds = GetBounds(annotation);
        item.sortY = bounds.y;
        item.sortX = bounds.x;
        items.Append(item);
    }
    std::sort(items.begin(), items.end(), [](const PdfAnnotationSortItem& a, const PdfAnnotationSortItem& b) {
        if (a.pageNo != b.pageNo) {
            return a.pageNo < b.pageNo;
        }
        if (a.sortY != b.sortY) {
            return a.sortY < b.sortY;
        }
        return a.sortX < b.sortX;
    });

    out.Append(UTF8_BOM);
    out.AppendFmt("# %s\n\n", tab->GetTabTitle());
    out.Append("<details>\n");
    out.AppendFmt("<summary>%s</summary>\n\n", _TRA("Source"));
    out.AppendFmt("%s: %s\n\n", _TRA("Source"), tab->filePath);
    out.Append(_TRA("Exported:"));
    out.Append(" ");
    AppendUtcDateTime(out, time(nullptr));
    out.Append("\n\n</details>\n\n");

    int group = -1;
    int number = 0;
    for (const PdfAnnotationSortItem& item : items) {
        Annotation* annotation = item.annotation;
        if (item.pageNo != group) {
            group = item.pageNo;
            number = 0;
            out.AppendFmt("## %s %d\n\n", _TRA("Page"), group);
        }
        const char* typeName = trans::GetTranslation(AnnotationReadableNameTemp(Type(annotation)));
        TempStr excerpt = PdfAnnotationExcerptTemp(tab->AsFixed(), annotation);
        TempStr note = Contents(annotation);
        bool geometry = excerpt && str::StartsWith(excerpt, "x=") &&
                        (annotation->type == AnnotationType::Square || annotation->type == AnnotationType::Circle ||
                         annotation->type == AnnotationType::Line || annotation->type == AnnotationType::Ink);
        AppendReadingNoteMarkdown(out, ++number, typeName, excerpt, note, Author(annotation),
                                  ModificationDate(annotation), geometry);
    }

    return true;
}

bool PdfAnnotationsExportNotes(WindowTab* tab, HWND hwndParent) {
    if (!tab || !EngineSupportsAnnotations(tab->GetEngine())) {
        return false;
    }
    StrBuilder out;
    if (!BuildPdfAnnotationsExport(tab, out)) {
        NotificationCreateArgs nargs;
        nargs.hwndParent = hwndParent;
        nargs.font = GetDefaultGuiFont();
        nargs.timeoutMs = 4000;
        nargs.msg = _TRA("No annotations to export.");
        ShowNotification(nargs);
        return false;
    }

    TempStr defaultPath =
        path::JoinTemp(path::GetDirTemp(tab->filePath),
                       str::JoinTemp(path::GetBaseNameTemp(path::GetPathNoExtTemp(tab->filePath)), "-notes.md"));
    if (!SaveDataToFile(hwndParent, defaultPath, ByteSlice((const u8*)out.Get(), out.size()))) {
        return false;
    }

    NotificationCreateArgs nargs;
    nargs.hwndParent = hwndParent;
    nargs.font = GetDefaultGuiFont();
    nargs.timeoutMs = 5000;
    nargs.msg = _TRA("Exported annotations.");
    ShowNotification(nargs);
    return true;
}

static void FlushContentsFromEdit(EditAnnotationsWindow* ew);
static void FlushAuthorFromEdit(EditAnnotationsWindow* ew);

static void ExportClicked(EditAnnotationsWindow* ew) {
    FlushAuthorFromEdit(ew);
    FlushContentsFromEdit(ew);
    PdfAnnotationsExportNotes(ew->tab, ew->hwnd);
}

// TODO: this should be OnDestroy()
static void OnClose(Wnd::CloseEvent* ev) {
    auto w = (EditAnnotationsWindow*)ev->e->self;
    FlushAuthorFromEdit(w);
    FlushContentsFromEdit(w);
    HWND toActivate = w->tab->win->hwndFrame;
    w->tab->editAnnotsWindow = nullptr;
    delete w; // TODO: sketchy
    SetActiveWindow(toActivate);
}

void EditAnnotationsWindow::OnFocus() {
    // Only a click on this panel should bring its document forward. Focus handed
    // over when another window closes must not change the current tab.
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0 && (GetKeyState(VK_RBUTTON) & 0x8000) == 0) {
        return;
    }
    SelectTabInWindow(tab);
}

extern bool SaveAnnotationsToMaybeNewPdfFile(WindowTab*);

static void ButtonSaveToNewFileHandler(EditAnnotationsWindow* ew) {
    FlushAuthorFromEdit(ew);
    FlushContentsFromEdit(ew);
    WindowTab* tab = ew->tab;
    bool ok = SaveAnnotationsToMaybeNewPdfFile(tab);
    if (!ok) {
        return;
    }
}

extern bool SaveAnnotationsToExistingFile(WindowTab* tab);

static void ButtonSaveToCurrentPDFHandler(EditAnnotationsWindow* ew) {
    FlushAuthorFromEdit(ew);
    FlushContentsFromEdit(ew);
    SaveAnnotationsToExistingFile(ew->tab);
}

constexpr int kMaxControls = 32;

static void AdvanceFocus(EditAnnotationsWindow* ew, bool forward) {
    HWND controls[kMaxControls];
    int n = 0;
    auto addIfVisible = [&](HWND h) {
        if (h && IsWindowVisible(h)) {
            ReportIf(n >= kMaxControls);
            controls[n++] = h;
        }
    };

    addIfVisible(ew->listBox->hwnd);
    addIfVisible(ew->editAuthor->hwnd);
    addIfVisible(ew->editContents->hwnd);
    addIfVisible(ew->buttonRestoreFreeText ? ew->buttonRestoreFreeText->hwnd : nullptr);
    addIfVisible(ew->dropDownTextAlignment->hwnd);
    addIfVisible(ew->buttonTextFont->hwnd);
    addIfVisible(ew->trackbarTextSize->hwnd);
    addIfVisible(ew->dropDownTextColor->hwnd);
    addIfVisible(ew->dropDownBorderColor ? ew->dropDownBorderColor->hwnd : nullptr);
    addIfVisible(ew->dropDownLineStart->hwnd);
    addIfVisible(ew->dropDownLineEnd->hwnd);
    addIfVisible(ew->dropDownIcon->hwnd);
    addIfVisible(ew->trackbarBorder->hwnd);
    addIfVisible(ew->dropDownColor->hwnd);
    addIfVisible(ew->dropDownInteriorColor->hwnd);
    addIfVisible(ew->trackbarOpacity->hwnd);
    addIfVisible(ew->buttonSaveAttachment->hwnd);
    addIfVisible(ew->buttonEmbedAttachment->hwnd);
    addIfVisible(ew->buttonExport->hwnd);
    addIfVisible(ew->buttonSaveToCurrentFile->hwnd);
    addIfVisible(ew->buttonSaveToNewFile->hwnd);

    if (n == 0) {
        return;
    }

    HWND focused = ::GetFocus();
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (controls[i] == focused || ::IsChild(controls[i], focused)) {
            idx = i;
            break;
        }
    }

    int next;
    if (forward) {
        next = (idx + 1) % n;
    } else {
        next = (idx <= 0) ? n - 1 : idx - 1;
    }
    HwndSetFocus(controls[next]);
}

static bool IsAnnotContentsEditActive(HWND msgHwnd, HWND editHwnd, HWND windowHwnd) {
    if (!editHwnd) {
        return false;
    }
    auto relatedToEdit = [&](HWND h) -> bool { return h && (h == editHwnd || ::IsChild(editHwnd, h)); };
    if (relatedToEdit(msgHwnd) || relatedToEdit(::GetFocus())) {
        return true;
    }
    HWND focus = ::GetFocus();
    if (focus && windowHwnd && ::IsChild(windowHwnd, focus)) {
        TempStr cls = HwndGetClassName(focus);
        if (str::EqI(cls, "Edit")) {
            return true;
        }
    }
    return false;
}

bool IsPdfAnnotContentsEditFocused(HWND msgHwnd) {
    if (IsFreeTextInPlaceEditFocused(msgHwnd)) {
        return true;
    }
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            EditAnnotationsWindow* ew = tab->editAnnotsWindow;
            if (!ew || !ew->editContents) {
                continue;
            }
            if (IsAnnotContentsEditActive(msgHwnd, ew->editContents->hwnd, ew->hwnd)) {
                return true;
            }
        }
    }
    return false;
}

bool EditAnnotationsWindow::PreTranslateMessage(MSG& msg) {
    if (msg.message == WM_KEYDOWN) {
        int key = (int)msg.wParam;
        bool inContentsEdit = IsAnnotContentsEditActive(msg.hwnd, editContents ? editContents->hwnd : nullptr, hwnd);
        if (key == VK_ESCAPE && tab->selectedAnnotation) {
            SetSelectedAnnotation(tab, nullptr);
            return true;
        }
        if (key == VK_TAB) {
            bool forward = !IsShiftPressed();
            AdvanceFocus(this, forward);
            return true;
        }
        if (inContentsEdit && (key == VK_BACK || key == VK_DELETE)) {
            if (!IsCtrlPressed() && !IsAltPressed()) {
                return EditDeleteChar(editContents->hwnd, key == VK_BACK);
            }
            return false;
        }
        if (key == VK_DELETE) {
            if (IsCtrlPressed()) {
                DeleteSelectedAnnotation(this);
                return true;
            }
            DeleteSelectedAnnotation(this);
            return true;
        }
        if (key == 'S' && IsShiftPressed() && IsCtrlPressed()) {
            // TODO: delay by posting a message?
            // TODO: the keybinding could be changed so this should
            // be more sophisticated and match the shortcut
            ButtonSaveToCurrentPDFHandler(this);
            return true;
        }
    }
    return false;
}

static void ItemsFromSeqstrings(StrVec& items, const char* strings) {
    while (strings) {
        items.Append(strings);
        seqstrings::Next(strings);
    }
}

static void DropDownFillColors(DropDown* w, PdfColor col, StrBuilder& customColor) {
    StrVec items;
    ItemsFromSeqstrings(items, gColors);
    const char* colorName = GetKnownColorName(col);
    int idx = seqstrings::StrToIdx(gColors, colorName);
    if (idx < 0) {
        customColor.Reset();
        SerializePdfColor(col, customColor);
        items.Append(customColor.LendData());
        idx = items.Size() - 1;
    }
    w->SetItems(items);
    w->SetCurrentSelection(idx);
}

static PdfColor GetDropDownColor(const char* sv) {
    int idx = seqstrings::StrToIdx(gColors, sv);
    if (idx >= 0) {
        int nMaxColors = (int)dimof(gColorsValues);
        ReportIf(idx >= nMaxColors);
        if (idx < nMaxColors) {
            return gColorsValues[idx];
        }
        return 0;
    }
    ParsedColor col;
    ParseColor(col, sv);
    return col.pdfCol;
}

COLORREF GetAnnotationColorFromDropDown(const char* item) {
    return ColorRefFromPdfColor(GetDropDownColor(item));
}

void FillAnnotationColorDropDown(DropDown* w, COLORREF col, StrBuilder& customColor) {
    DropDownFillColors(w, PdfAnnotationColorFromColorRef(col), customColor);
}

COLORREF GetDefaultAnnotationColor(AnnotationType type) {
    auto& a = gGlobalPrefs->annotations;
    ParsedColor* col = nullptr;
    if (type == AnnotationType::Text) {
        col = GetParsedColor(a.textIconColor, a.textIconColorParsed);
    } else if (type == AnnotationType::Underline) {
        col = GetParsedColor(a.underlineColor, a.underlineColorParsed);
    } else if (type == AnnotationType::Highlight) {
        col = GetParsedColor(a.highlightColor, a.highlightColorParsed);
    } else if (type == AnnotationType::Squiggly) {
        col = GetParsedColor(a.squigglyColor, a.squigglyColorParsed);
    } else if (type == AnnotationType::StrikeOut) {
        col = GetParsedColor(a.strikeOutColor, a.strikeOutColorParsed);
    } else if (type == AnnotationType::FreeText) {
        col = GetParsedColor(a.freeTextColor, a.freeTextColorParsed);
    }
    if (col && col->parsedOk) {
        return col->col;
    }
    if (type == AnnotationType::Underline) {
        return ParseColor("#00ff00");
    }
    if (type == AnnotationType::Squiggly) {
        return ParseColor("#ff00ff");
    }
    if (type == AnnotationType::StrikeOut) {
        return ParseColor("#ff0000");
    }
    return ParseColor("#ffff00");
}

// TODO: mupdf shows it in 1.6 but not 1.7. Why?
bool gShowRect = true;

// TODO: only limit to widgets that have rect?
static void AppendPdfDate(StrBuilder& s, time_t secs);

static void RelayoutEditAnnotations(EditAnnotationsWindow* ew, bool paint = true) {
    if (!ew || !ew->mainLayout || !ew->hwnd) {
        return;
    }
    Rect rc = ClientRect(ew->hwnd);
    if (rc.dx <= 0 || rc.dy <= 0) {
        return;
    }
    LayoutAnnotationSidebarControls(ew->mainLayout, ew->inspectorPane, {rc.dx, rc.dy});
    if (!paint) {
        return;
    }
    // One paint after the controls are in their final places.
    HWND pane = ew->inspectorPane ? ew->inspectorPane->hwnd : ew->hwnd;
    RedrawWindow(pane, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void SyncAnnotHeadingColumns(EditAnnotationsWindow* ew) {
    if (!ew || !ew->staticHeading) {
        return;
    }
    int col = ew->staticHeading->MeasureTitlePx();
    if (ew->staticMeta && ew->staticMeta->IsVisible()) {
        col = std::max(col, ew->staticMeta->MeasureTitlePx());
    }
    if (ew->staticHeading->titleColW != col) {
        ew->staticHeading->titleColW = col;
        if (ew->staticHeading->hwnd && ew->staticHeading->IsVisible()) {
            InvalidateRect(ew->staticHeading->hwnd, nullptr, FALSE);
        }
    }
    if (ew->staticMeta && ew->staticMeta->titleColW != col) {
        ew->staticMeta->titleColW = col;
        if (ew->staticMeta->hwnd) {
            InvalidateRect(ew->staticMeta->hwnd, nullptr, FALSE);
        }
    }
}

static void RefreshMetadataLine(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!ew || !ew->staticMeta) {
        return;
    }
    const char* author = nullptr;
    if (annot && AnnotationHasAuthor(annot)) {
        author = Author(annot);
    }
    StrBuilder date;
    if (annot && ModificationDate(annot) != 0) {
        AppendPdfDate(date, ModificationDate(annot));
    }
    if (ew->authorEditing || (str::IsEmpty(author) && date.Size() == 0)) {
        ew->staticMeta->SetIsVisible(false);
        SyncAnnotHeadingColumns(ew);
        return;
    }
    // Author is the only emphasis on this line; date stays regular/muted.
    ew->staticMeta->SetParts(str::IsEmpty(author) ? nullptr : author, date.Size() > 0 ? date.Get() : nullptr);
    ew->staticMeta->SetIsVisible(true);
    SyncAnnotHeadingColumns(ew);
}

static void EndAuthorEdit(EditAnnotationsWindow* ew) {
    if (!ew || !ew->authorEditing) {
        return;
    }
    ew->authorEditing = false;
    FlushAuthorFromEdit(ew);
    ew->editAuthor->SetIsVisible(false);
    RefreshMetadataLine(ew, ew->tab ? ew->tab->selectedAnnotation : nullptr);
    RelayoutEditAnnotations(ew);
}

static void BeginAuthorEdit(EditAnnotationsWindow* ew) {
    Annotation* annot = ew && ew->tab ? ew->tab->selectedAnnotation : nullptr;
    if (!annot || !AnnotationHasAuthor(annot) || ew->authorEditing) {
        return;
    }
    ew->authorEditing = true;
    ew->staticMeta->SetIsVisible(false);
    ew->editAuthor->SetIsVisible(true);
    RelayoutEditAnnotations(ew);
    HwndSetFocus(ew->editAuthor->hwnd);
    ew->editAuthor->SelectAll();
}

static TempStr AnnotationHeadingTemp(Annotation* annot) {
    if (!annot) {
        return nullptr;
    }
    // Only the type name is emphasized; page and geometry follow in regular weight.
    return str::DupTemp(trans::GetTranslation(AnnotationReadableNameTemp(annot->type)));
}

static TempStr AnnotationBoundsTemp(Annotation* annot) {
    if (!annot) {
        return nullptr;
    }
    TempStr pageLabel = str::FormatTemp(_TRA("Page %d"), annot->pageNo);
    if (!gShowRect) {
        return pageLabel;
    }
    RectF rect = GetBounds(annot);
    return str::FormatTemp("%s · x=%d  y=%d  dx=%d  dy=%d", pageLabel, (int)rect.x, (int)rect.y, (int)rect.dx,
                           (int)rect.dy);
}

static void DoAuthor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationHasAuthor(annot)) {
        ew->editAuthor->SetIsVisible(false);
        return;
    }
    const char* author = Author(annot);
    ew->updatingControls = true;
    ew->editAuthor->SetText(author ? author : "");
    ew->updatingControls = false;
    ew->editAuthor->SetIsVisible(false);
}

static void FlushAuthorFromEdit(EditAnnotationsWindow* ew) {
    if (!ew || !ew->editAuthor || ew->updatingControls) {
        return;
    }
    Annotation* a = ew->tab->selectedAnnotation;
    if (!a || !a->engine || !a->pdfannot || !AnnotationHasAuthor(a)) {
        return;
    }
    if (ew->annotations.Find(a) < 0) {
        return;
    }
    if (SetAuthor(a, ew->editAuthor->GetTextTemp())) {
        EnableSaveIfAnnotationsChanged(ew);
    }
}

static void AppendPdfDate(StrBuilder& s, time_t secs) {
    struct tm tm;
    gmtime_s(&tm, &secs);
    char buf[100];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    s.Append(buf);
}

static void DoModificationDate(EditAnnotationsWindow* ew, Annotation* annot) {
    // Author and date share one quiet metadata line. The old date field stays hidden.
    (void)ew;
    (void)annot;
}

static void FlushContentsFromEdit(EditAnnotationsWindow* ew) {
    if (!ew || !ew->editContents || ew->updatingControls) {
        return;
    }
    Annotation* a = ew->tab->selectedAnnotation;
    if (!a || !a->engine || !a->pdfannot) {
        return;
    }
    if (ew->annotations.Find(a) < 0) {
        return;
    }
    auto txt = ew->editContents->GetTextTemp();
    txt = str::ReplaceTemp(txt, "\r\n", "\n");
    SetContents(a, txt);
    EnableSaveIfAnnotationsChanged(ew);
}

static void DoContents(EditAnnotationsWindow* ew, Annotation* annot) {
    TempStr s = Contents(annot);
    // don't replace if already is "\r\n"
    s = str::ReplaceTemp(s, "\r\n", "\n");
    s = str::ReplaceTemp(s, "\n", "\r\n");
    ew->staticContents->SetText(_TRA("Note"));
    ew->staticContents->SetIsVisible(true);
    ew->editContents->SetIsVisible(true);
    ew->updatingControls = true;
    ew->editContents->SetText(s);
    ew->updatingControls = false;
}

static void DoTextAlignment(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    int itemNo = Quadding(annot);
    const char* items = gQuaddingNames;
    ew->dropDownTextAlignment->SetItemsSeqStrings(items);
    ew->dropDownTextAlignment->SetCurrentSelection(itemNo);
    ew->staticTextAlignment->SetIsVisible(true);
    ew->dropDownTextAlignment->SetIsVisible(true);
}

static void RefreshInPlaceFreeTextStyle(Annotation* annot);

static void TextAlignmentSelectionChanged(EditAnnotationsWindow* ew) {
    if (ew->updatingControls) {
        return;
    }
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextAlignment->GetCurrentSelection();
    int newQuadding = idx;
    SetQuadding(annot, newQuadding);
    RememberPdfDrawStyle(annot);
    RefreshInPlaceFreeTextStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void RefreshInPlaceFreeTextStyle(Annotation* annot);

static void DoTextFont(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    const char* fontName = DefaultAppearanceTextFont(annot);
    ew->buttonTextFont->SetText(FreeTextFontLabel(fontName));
    ew->staticTextFont->SetIsVisible(true);
    ew->buttonTextFont->SetIsVisible(true);
}

static void OnFreeTextFontPicked(const char* family, void* ctx) {
    auto* ew = (EditAnnotationsWindow*)ctx;
    if (!ew || !IsWindow(ew->hwnd) || str::IsEmpty(family)) {
        return;
    }
    auto annot = ew->tab ? ew->tab->selectedAnnotation : nullptr;
    if (!annot || !annot->engine || Type(annot) != AnnotationType::FreeText) {
        return;
    }
    SetDefaultAppearanceTextFont(annot, family);
    RememberFreeTextPreset(family, 0, 0, 0);
    RememberPdfDrawStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RefreshInPlaceFreeTextStyle(annot);
    DoTextFont(ew, annot);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void ButtonPickFreeTextFont(EditAnnotationsWindow* ew) {
    if (!ew || !ew->tab || !ew->tab->selectedAnnotation) {
        return;
    }
    const char* current = DefaultAppearanceTextFont(ew->tab->selectedAnnotation);
    HWND owner = GetAncestor(ew->hwnd, GA_ROOT);
    if (!owner) {
        owner = ew->hwnd;
    }
    ShowFreeTextFontPicker(owner, current, OnFreeTextFontPicked, ew);
}

static void SyncSidebarNoteToInPlace(Annotation* annot, const char* textLf);

static void DoTextSize(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    int fontSize = DefaultAppearanceTextSize(annot);
    TempStr s = str::FormatTemp(_TRA("Text Size: %d"), fontSize);
    ew->staticTextSize->SetText(s);
    // TODO: DoTextSize() shouldn't modify the annotation but I'm not sure
    // if it's not needed to be called for free text annotations
    // at some point (i.e. when creating)
    // SetDefaultAppearanceTextSize(ew->tab->selectedAnnotation, fontSize);
    ew->trackbarTextSize->SetValue(fontSize);
    ew->staticTextSize->SetIsVisible(true);
    ew->trackbarTextSize->SetIsVisible(true);
}

static void DoTextColor(EditAnnotationsWindow* ew, Annotation* annot);
static void DoBorderColor(EditAnnotationsWindow* ew, Annotation* annot);
static void DoBorder(EditAnnotationsWindow* ew, Annotation* annot);
static void DoColor(EditAnnotationsWindow* ew, Annotation* annot);
static void SyncInPlaceFreeTextBorder(Annotation* annot, float borderWidth);

static void UpdateUIForSelectedAnnotation(EditAnnotationsWindow* ew, Annotation* annot, bool isNew = false,
                                          EditAnnotFocus focus = EditAnnotFocus::Default);

static void ButtonRestoreFreeTextDefaults(EditAnnotationsWindow* ew) {
    ResetFreeTextPreset();
    auto annot = ew->tab ? ew->tab->selectedAnnotation : nullptr;
    if (!annot || Type(annot) != AnnotationType::FreeText) {
        return;
    }
    SetDefaultAppearanceTextFont(annot, "Helv");
    SetDefaultAppearanceTextSize(annot, 21);
    SetDefaultAppearanceTextColor(annot, 0xff000000);
    SetQuadding(annot, 0);
    SetBorderWidth(annot, 1);
    SetFreeTextBorderColor(annot, 0xff000000);
    SetColor(annot, 0);
    SetOpacity(annot, 255);
    SyncInPlaceFreeTextBorder(annot, 1);
    EnableSaveIfAnnotationsChanged(ew);
    RefreshInPlaceFreeTextStyle(annot);
    ew->updatingControls = true;
    DoTextAlignment(ew, annot);
    DoTextFont(ew, annot);
    DoTextSize(ew, annot);
    DoTextColor(ew, annot);
    DoBorderColor(ew, annot);
    DoBorder(ew, annot);
    DoColor(ew, annot);
    ew->updatingControls = false;
    RememberPdfDrawStyle(annot);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void ButtonRestoreAnnotationDefaults(EditAnnotationsWindow* ew) {
    auto annot = ew->tab ? ew->tab->selectedAnnotation : nullptr;
    if (!annot || !annot->engine) {
        return;
    }
    AnnotationType type = Type(annot);
    if (type == AnnotationType::FreeText) {
        ButtonRestoreFreeTextDefaults(ew);
        return;
    }
    if (AnnotationSupportsColor(type)) {
        SetColor(annot, PdfAnnotationColorFromColorRef(FactoryAnnotationColor(type)));
    }
    if (AnnotationSupportsBorder(type)) {
        float width = 1.f;
        if (type == AnnotationType::Ink || type == AnnotationType::Line || type == AnnotationType::Square ||
            type == AnnotationType::Circle) {
            DisplayModel* dm = ew->tab ? ew->tab->AsFixed() : nullptr;
            int pageNo = PageNo(annot);
            float zoom = (dm && dm->ValidPageNo(pageNo)) ? dm->GetZoomReal(pageNo) : 1.f;
            width = NewStrokeWidthPoints(zoom);
        }
        SetBorderWidthFloat(annot, width);
    }
    if (AnnotationSupportsInteriorColor(type)) {
        SetInteriorColor(annot, 0);
    }
    if (type == AnnotationType::Line) {
        SetLineStartStyles(annot, 0);
        SetLineEndStyles(annot, 0);
    }
    if (type == AnnotationType::Text) {
        SetIconName(annot, "Comment");
    } else if (type == AnnotationType::Stamp) {
        SetIconName(annot, "Final");
    } else if (type == AnnotationType::FileAttachment) {
        SetIconName(annot, "PushPin");
    } else if (type == AnnotationType::Sound) {
        SetIconName(annot, "Speaker");
    }
    if (type == AnnotationType::Highlight) {
        SetOpacity(annot, 255);
    }
    EnableSaveIfAnnotationsChanged(ew);
    UpdateUIForSelectedAnnotation(ew, annot);
    RememberPdfDrawStyle(annot);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void TextFontSizeChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    if (ew->updatingControls) {
        return;
    }
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int fontSize = ev->pos;
    SetDefaultAppearanceTextSize(annot, fontSize);
    RememberFreeTextPreset(nullptr, fontSize, 0, 0);
    RememberPdfDrawStyle(annot);
    TempStr s = str::FormatTemp(_TRA("Text Size: %d"), fontSize);
    ew->staticTextSize->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RefreshInPlaceFreeTextStyle(annot);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void DoTextColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    PdfColor col = DefaultAppearanceTextColor(annot);
    DropDownFillColors(ew->dropDownTextColor, col, ew->currTextColor);
    ew->staticTextColor->SetIsVisible(true);
    ew->dropDownTextColor->SetIsVisible(true);
}

static void TextColorSelectionChanged(EditAnnotationsWindow* ew) {
    if (ew->updatingControls) {
        return;
    }
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextColor->GetCurrentSelection();
    char* item = ew->dropDownTextColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetDefaultAppearanceTextColor(annot, col);
    RememberPdfDrawStyle(annot);
    RefreshInPlaceFreeTextStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void DoBorderColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!ew->dropDownBorderColor || Type(annot) != AnnotationType::FreeText) {
        return;
    }
    PdfColor col = FreeTextBorderColor(annot);
    DropDownFillColors(ew->dropDownBorderColor, col, ew->currBorderColor);
    ew->staticBorderColor->SetIsVisible(true);
    ew->dropDownBorderColor->SetIsVisible(true);
}

static void BorderColorSelectionChanged(EditAnnotationsWindow* ew) {
    if (ew->updatingControls) {
        return;
    }
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine || Type(annot) != AnnotationType::FreeText) {
        return;
    }
    auto idx = ew->dropDownBorderColor->GetCurrentSelection();
    char* item = ew->dropDownBorderColor->items.At(idx);
    auto col = GetDropDownColor(item);
    if (col == 0) {
        col = 0xff000000;
    }
    SetFreeTextBorderColor(annot, col);
    RememberPdfDrawStyle(annot);
    RefreshInPlaceFreeTextStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void DoBorder(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationSupportsBorder(annot->type)) {
        return;
    }
    if (ClearFreeTextHairlineBorder(annot)) {
        RerenderPdfAnnotationChange(ew->tab, nullptr);
    }
    int borderWidth = BorderWidth(annot);
    borderWidth = std::clamp(borderWidth, borderWidthMin, borderWidthMax);
    TempStr s = str::FormatTemp(_TRA("Border: %d"), borderWidth);
    ew->staticBorder->SetText(s);
    ew->trackbarBorder->SetValue(borderWidth);
    ew->staticBorder->SetIsVisible(true);
    ew->trackbarBorder->SetIsVisible(true);
}

static void SyncInPlaceFreeTextBorder(Annotation* annot, float borderWidth);

static void BorderWidthChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    if (ew->updatingControls) {
        return;
    }
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int borderWidth = ev->pos;
    SetBorderWidth(annot, borderWidth);
    RememberPdfDrawStyle(annot);
    TempStr s = str::FormatTemp(_TRA("Border: %d"), borderWidth);
    ew->staticBorder->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    SyncInPlaceFreeTextBorder(annot, borderWidth);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void DoLineStartEnd(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::Line) {
        return;
    }
    int start = 0;
    int end = 0;
    GetLineEndingStyles(annot, &start, &end);
    ew->dropDownLineStart->SetItemsSeqStrings(gLineEndingStyles);
    PreparePdfIconPreviews(ew->dropDownLineStart, annot);
    ew->dropDownLineStart->SetCurrentSelection(start);
    ew->dropDownLineEnd->SetItemsSeqStrings(gLineEndingStyles);
    PreparePdfIconPreviews(ew->dropDownLineEnd, annot);
    ew->dropDownLineEnd->SetCurrentSelection(end);
    ew->staticLineStart->SetIsVisible(true);
    ew->dropDownLineStart->SetIsVisible(true);
    ew->staticLineEnd->SetIsVisible(true);
    ew->dropDownLineEnd->SetIsVisible(true);
}

static void LineStartSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto start = ew->dropDownLineStart->GetCurrentSelection();
    if (start < 0) {
        return;
    }
    SetLineStartStyles(annot, start);
    RememberPdfDrawStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void LineEndSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto end = ew->dropDownLineEnd->GetCurrentSelection();
    if (end < 0) {
        return;
    }
    SetLineEndStyles(annot, end);
    RememberPdfDrawStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoIcon(EditAnnotationsWindow* ew, Annotation* annot) {
    const char* itemName = IconName(annot);
    const char* items = nullptr;
    switch (Type(annot)) {
        case AnnotationType::Text:
            items = gAnnotationTextIcons;
            break;
        case AnnotationType::FileAttachment:
            items = gFileAttachmentUcons;
            break;
        case AnnotationType::Sound:
            items = gSoundIcons;
            break;
        case AnnotationType::Stamp:
            items = gStampIcons;
            break;
        default:
            // no-op
            break;
    }
    if (!items || str::IsEmpty(itemName)) {
        return;
    }
    ew->dropDownIcon->SetItemsSeqStrings(items);
    PreparePdfIconPreviews(ew->dropDownIcon, annot);
    int idx = FindStringInArray(items, itemName, 0);
    ew->dropDownIcon->SetCurrentSelection(idx);
    ew->staticIcon->SetIsVisible(true);
    ew->dropDownIcon->SetIsVisible(true);
}

static void IconSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownIcon->GetCurrentSelection();
    auto item = ew->dropDownIcon->items.At(idx);
    SetIconName(annot, item);
    RememberPdfDrawStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationSupportsColor(annot->type)) {
        return;
    }
    PdfColor col = GetColor(annot);
    DropDownFillColors(ew->dropDownColor, col, ew->currCustomColor);
    int n = dimof(gAnnotsIsColorBackground);
    bool isBgCol = IsAnnotationTypeInArray(gAnnotsIsColorBackground, n, Type(annot));
    if (isBgCol) {
        ew->staticColor->SetText(_TRA("Background Color:"));
    } else {
        ew->staticColor->SetText(_TRA("Color"));
    }
    ew->staticColor->SetIsVisible(true);
    ew->dropDownColor->SetIsVisible(true);
    if (ew->colorSwatch) {
        ew->colorSwatch->has = true;
        ew->colorSwatch->color = ColorRefFromPdfColor(col);
        ew->colorSwatch->SetIsVisible(false);
        HwndScheduleRepaint(ew->dropDownColor->hwnd);
    }
}

static void ColorSelectionChanged(EditAnnotationsWindow* ew) {
    if (ew->updatingControls) {
        return;
    }
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownColor->GetCurrentSelection();
    auto item = ew->dropDownColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetColor(annot, col);
    RememberPdfDrawStyle(annot);
    RefreshInPlaceFreeTextStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    if (ew->colorSwatch) {
        ew->colorSwatch->has = true;
        ew->colorSwatch->color = ColorRefFromPdfColor(col);
        HwndScheduleRepaint(ew->dropDownColor->hwnd);
    }
    if (ew->listBox && ew->listBox->hwnd) {
        InvalidateRect(ew->listBox->hwnd, nullptr, FALSE);
    }
    RerenderPdfAnnotationChange(ew->tab, nullptr);
    if (gFreeTextToolbarHwnd) {
        InvalidateRect(gFreeTextToolbarHwnd, nullptr, FALSE);
    }
}

static void DoInteriorColor(EditAnnotationsWindow* ew, Annotation* annot) {
    if (!AnnotationSupportsInteriorColor(annot->type)) {
        return;
    }
    PdfColor col = InteriorColor(annot);
    DropDownFillColors(ew->dropDownInteriorColor, col, ew->currCustomInteriorColor);
    ew->staticInteriorColor->SetIsVisible(true);
    ew->dropDownInteriorColor->SetIsVisible(true);
}

static void InteriorColorSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownInteriorColor->GetCurrentSelection();
    auto item = ew->dropDownInteriorColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetInteriorColor(annot, col);
    RememberPdfDrawStyle(annot);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoOpacity(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::Highlight) {
        return;
    }
    int opacity = Opacity(ew->tab->selectedAnnotation);
    TempStr s = str::FormatTemp(_TRA("Opacity: %d"), opacity);
    ew->staticOpacity->SetText(s);
    ew->staticOpacity->SetIsVisible(true);
    ew->trackbarOpacity->SetIsVisible(true);
    ew->trackbarOpacity->SetValue(opacity);
}

static void DoSaveEmbed(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FileAttachment) {
        return;
    }
    ew->buttonSaveAttachment->SetIsVisible(true);
    ew->buttonEmbedAttachment->SetIsVisible(true);
}

static void OpacityChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int opacity = ev->pos;
    SetOpacity(annot, opacity);
    RememberPdfDrawStyle(annot);
    TempStr s = str::FormatTemp(_TRA("Opacity: %d"), opacity);
    ew->staticOpacity->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

// TODO: maybe use ew->tab->selectedAnnotation instead of annot
static void UpdateUIForSelectedAnnotation(EditAnnotationsWindow* ew, Annotation* annot, bool isNew,
                                          EditAnnotFocus focus) {
    InspectorUpdateLock hold(ew->hwnd, ew->inspectorPane ? ew->inspectorPane->hwnd : nullptr);
    Vec<HWND> visHwnds;
    Vec<u8> visBefore;
    CaptureInspectorChildVis(ew->inspectorPane ? ew->inspectorPane->hwnd : nullptr, visHwnds, visBefore);
    HidePerAnnotControls(ew);
    if (annot) {
        int itemNo = ew->annotations.Find(annot);
        if (itemNo < 0) {
            // can happen if annotations list is out of sync (e.g. after reload)
            hold.erase = true;
            RelayoutEditAnnotations(ew, false);
            return;
        }

        DoAuthor(ew, annot);
        DoModificationDate(ew, annot);
        DoContents(ew, annot);

        DoTextAlignment(ew, annot);
        DoTextFont(ew, annot);
        DoTextSize(ew, annot);
        DoTextColor(ew, annot);
        DoBorderColor(ew, annot);

        DoLineStartEnd(ew, annot);

        DoIcon(ew, annot);

        DoBorder(ew, annot);
        DoColor(ew, annot);
        DoInteriorColor(ew, annot);

        DoOpacity(ew, annot);
        DoSaveEmbed(ew, annot);
        if (ew->buttonRestoreFreeText) {
            ew->buttonRestoreFreeText->SetIsVisible(true);
        }
        if (ew->resetAppearanceRow) {
            ew->resetAppearanceRow->SetVisibility(Visibility::Visible);
        }
        // Types without a border start this block at the color row. Pull that
        // row up by the same 6px; leave the gap under a visible border alone.
        if (ew->staticColor && ew->dropDownColor && ew->staticBorder && !ew->staticBorder->IsVisible()) {
            ew->staticColor->SetInsetsPt(12, 0, 0, 0);
            ew->dropDownColor->SetInsetsPt(12, 0, 0, 0);
            ew->staticColor->insets.top -= 6;
            ew->dropDownColor->insets.top -= 6;
        } else if (ew->staticColor && ew->dropDownColor) {
            ew->staticColor->SetInsetsPt(12, 0, 0, 0);
            ew->dropDownColor->SetInsetsPt(12, 0, 0, 0);
        }

        ew->staticHeading->SetParts(AnnotationHeadingTemp(annot), AnnotationBoundsTemp(annot));
        ew->staticHeading->SetIsVisible(false);
        RefreshMetadataLine(ew, annot);
        SyncAnnotHeadingColumns(ew);

        ew->listBox->SetCurrentSelection(itemNo);

        if (focus == EditAnnotFocus::Edit) {
            HwndSetFocus(ew->editContents->hwnd);
            ew->editContents->SelectAll();
        } else if (focus == EditAnnotFocus::List) {
            HwndSetFocus(ew->listBox->hwnd);
        } else if (isNew && annot->type == AnnotationType::FreeText) {
            HwndSetFocus(ew->editContents->hwnd);
            // ew->editContents->SetCursorPositionAtEnd();
            ew->editContents->SelectAll();
        } else {
            HwndSetFocus(ew->listBox->hwnd);
        }
    }

    // Same controls: do not relayout. That pass parks the reset button and
    // then nudges it, and the erase redraw flashes the author line.
    HWND pane = ew->inspectorPane ? ew->inspectorPane->hwnd : nullptr;
    if (!InspectorChildVisUnchanged(pane, visHwnds, visBefore)) {
        // Outer size is often unchanged when switching annotation types, so the
        // inspector pane gets no WM_SIZE. Relayout its children explicitly or the
        // newly shown Line/Ink fields stay piled at their create-time positions.
        // Paint once after the update lock drops, not between hide and show.
        hold.erase = true;
        RelayoutEditAnnotations(ew, false);
    }

    if (!annot) {
        // The sidebar stays open, so the list highlight is the edit state.
        // Clear it when the page selection is cleared.
        if (ew->listBox && ew->listBox->GetCurrentSelection() >= 0) {
            ew->listBox->SetCurrentSelection(-1);
        }
        return;
    }
    if (ew->skipGoToPage) {
        ew->skipGoToPage = false;
        return;
    }

    int annotPageNo = annot->pageNo;
    DisplayModel* dm = ew->tab->AsFixed();
    int nPages = dm->PageCount();
    if (annotPageNo > nPages) {
        // see https://github.com/sumatrapdfreader/sumatrapdf/issues/1701
        logf("UpdateUIForSelectedAnnotation: invalid annotPageNo (%d), should be <= than nPages (%d)\n", annotPageNo,
             nPages);
        ReportIf(annotPageNo > nPages);
        return;
    }

    // don't switch pages if already visible. needed for cases where
    // we show more than one page at a time and GoToPage() scrolls
    // to top page
    // TODO: this is not perfect. We should skipGoToPage if this
    // is caused by creating an annotation. by definition the page
    // was visible when user created an annotation.
    // but that requires passing down more stuff
    if (!dm->PageVisible(annotPageNo)) {
        dm->GoToPage(annotPageNo, true);
    }
}

static void ButtonSaveAttachment(EditAnnotationsWindow* ew) {
    Annotation* annot = ew->tab->selectedAnnotation;
    ReportIf(!annot);
    if (!annot || annot->type != AnnotationType::FileAttachment) {
        return;
    }
    EngineMupdf* engine = GetEngineMupdf(ew);
    if (!engine) {
        return;
    }
    fz_context* ctx = engine->Ctx();
    pdf_annot* pdfannot = annot->pdfannot;
    if (!pdfannot) {
        return;
    }

    int objNum = pdf_to_num(ctx, pdf_annot_obj(ctx, pdfannot));
    ByteSlice data = EngineMupdfLoadAnnotAttachment((EngineBase*)engine, objNum);
    if (data.empty()) {
        return;
    }

    const char* fileName = nullptr;
    pdf_obj* fs = pdf_annot_filespec(ctx, pdfannot);
    if (fs) {
        pdf_filespec_params fileParams = {};
        pdf_get_filespec_params(ctx, fs, &fileParams);
        fileName = fileParams.filename;
    }
    if (str::IsEmpty(fileName)) {
        fileName = "attachment";
    }

    TempStr dir = path::GetDirTemp(ew->tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(ew->hwnd, dstPath, data);
    str::Free(data.data());
}

static void ButtonEmbedAttachment(EditAnnotationsWindow* ew) {
    ReportIf(!ew->tab->selectedAnnotation);
    // TODO: implement me
    MessageBoxNYI(ew->hwnd);
}

void SetSelectedAnnotation(WindowTab* tab, Annotation* annot, bool isNew, EditAnnotFocus focus) {
    // Leaving this free text for another annotation commits the page editor
    // first, so the Note still has what was just typed.
    if (annot != tab->selectedAnnotation && IsEditingFreeTextInPlace(tab->win)) {
        EndFreeTextInPlaceEdit(true);
    }
    // when we delete an annotation we automatically pick one to
    // set as selected and it might end up as currently selected
    // we still want to redraw to not show deleted annotation
    // but not do the rest of the logic as it triggers infinite loop
    // TODO: maybe if we already have selected annotation, do not auto-pick
    MainWindow* win = tab->win;
    auto ew = tab->editAnnotsWindow;
    if (annot == tab->selectedAnnotation) {
        MainWindowRerender(win);
        if (ew) {
            UpdateUIForSelectedAnnotation(ew, annot, isNew, focus);
        }
        ToolbarUpdateStateForWindow(win, false);
        return;
    }
    if (ew) {
        FlushAuthorFromEdit(ew);
        FlushContentsFromEdit(ew);
    }
    tab->selectedAnnotation = annot;
    tab->didScrollToSelectedAnnotation = false;
    // go to page with a given annotations before triggering repaint
    if (ew) {
        UpdateUIForSelectedAnnotation(ew, annot, isNew, focus);
        // Only the annotations page owns this window. Raising it while
        // bookmarks, thumbnails, favorites, or AI are showing stacks the
        // inspector on top of that page.
        if (win->tocVisible && CurrentSidebarView(win) == SidebarView::Annotations) {
            HwndMakeVisible(ew->hwnd);
        }
    }
    MainWindowRerender(win);
    ToolbarUpdateStateForWindow(win, false);
}

void UpdateAnnotationsList(EditAnnotationsWindow* ew) {
    if (!ew) {
        return;
    }
    auto engine = GetEngineMupdf(ew);
    EngineMupdfGetAnnotations(engine, ew->annotations);
    for (int i = ew->annotations.Size() - 1; i >= 0; i--) {
        if (AnnotationSupportsMediaPlayback(ew->annotations.at(i)->type)) {
            ew->annotations.RemoveAt(i);
        }
    }
    RebuildAnnotationsListBox(ew);
}

static void ListBoxSelectionChanged(EditAnnotationsWindow* ew) {
    ew->ListBoxSelectionChanged();
}

void EditAnnotationsWindow::ListBoxSelectionChanged() {
    int itemNo = listBox->GetCurrentSelection();
    if (itemNo < 0) {
        // an item has been deselected because e.g. selected annotation was deleted
        return;
    }
    if (!annotations.isValidIndex(itemNo)) {
        logfa("EditAnnotationsWindow::ListBoxSelectionChanged: invalid itemNo=%d, annotations.size()=%d\n", itemNo,
              annotations.Size());
        ReportDebugIf(true);
        return;
    }
    Annotation* annot = annotations.at(itemNo);
    SetSelectedAnnotation(tab, annot);
}

static UINT_PTR gMainWindowRerenderTimer = 0;
static MainWindow* gMainWindowForRender = nullptr;

// TODO: there seems to be a leak
static void AuthorChanged(EditAnnotationsWindow* ew) {
    FlushAuthorFromEdit(ew);
}

static void ContentsChanged(EditAnnotationsWindow* ew) {
    if (ew->updatingControls) {
        return;
    }
    auto a = ew->tab->selectedAnnotation;
    // TODO: saw a crash when this was null
    ReportDebugIf(!a);
    if (!a) {
        return;
    }
    auto txt = ew->editContents->GetTextTemp();
    txt = str::ReplaceTemp(txt, "\r\n", "\n");
    SetContents(a, txt);
    SyncSidebarNoteToInPlace(a, txt);
    EnableSaveIfAnnotationsChanged(ew);
    if (a->type == AnnotationType::FreeText) {
        InvalidateRect(ew->listBox->hwnd, nullptr, FALSE);
    }

    MainWindow* win = ew->tab->win;
    // Free text is drawn on the page bitmap, so waiting for a full re-render made
    // each phrase show up seconds later. Paint the new words immediately and let
    // the tile catch up after typing pauses.
    if (a->type == AnnotationType::FreeText) {
        MarkPdfAnnotationStandIn(ew->tab, a);
        DisplayModel* dm = win ? win->AsFixed() : nullptr;
        if (dm && a->pageNo > 0) {
            gRenderCache->Invalidate(dm, a->pageNo, GetBounds(a));
        }
    }
    if (win && win->hwndCanvas) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
    }
    if (!win || !win->hwndCanvas) {
        return;
    }
    if (gMainWindowRerenderTimer != 0) {
        KillTimer(win->hwndCanvas, gMainWindowRerenderTimer);
        gMainWindowRerenderTimer = 0;
    }
    gMainWindowForRender = win;
    gMainWindowRerenderTimer = SetTimer(win->hwndCanvas, 1, 120, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
        KillTimer(hwnd, id);
        gMainWindowRerenderTimer = 0;
        if (IsMainWindowValid(gMainWindowForRender)) {
            WindowTab* tab = gMainWindowForRender->CurrentTab();
            if (tab) {
                RerenderPdfAnnotationChange(tab, tab->selectedAnnotation);
            }
        }
    });
}

void EditAnnotationsWindow::OnSize(UINT msg, UINT, SIZE size) {
    if (msg == WM_SIZE) {
        LayoutAnnotationSidebar(hwnd, mainLayout, listBox, inspectorPane, footerLayout, (int)size.cx, (int)size.cy,
                                IsSidebarSplitterLiveDrag());
    }
}

static void DrawAnnotListItem(EditAnnotationsWindow* ew, ListBox::DrawItemEvent* ev) {
    if (!ew || !ev || !ev->hdc || ev->itemIndex < 0 || ev->itemIndex >= ew->annotations.Size()) return;
    Annotation* annot = ew->annotations.at(ev->itemIndex);
    PdfColor color = GetColor(annot);
    bool hasColor = color != 0;
    // /C is the fill. The list bar should match the text the reader sees.
    if (annot->type == AnnotationType::FreeText) {
        color = DefaultAppearanceTextColor(annot);
        if (color == 0) {
            color = 0xff000000;
        }
        hasColor = true;
    }
    const char* excerpt = ev->itemIndex < ew->annotationExcerpts.Size() ? ew->annotationExcerpts.At(ev->itemIndex) : "";
    if (annot->type == AnnotationType::FreeText) {
        TempStr text = str::DupTemp(Contents(annot));
        if (text) str::NormalizeWSInPlace(text);
        excerpt = text ? text : "";
    }
    DrawAnnotationSidebarRow(ew->hwnd, ew->listBox->hwnd, ev, ev->selected || annot == ew->tab->selectedAnnotation,
                             hasColor, ColorRefFromPdfColor(color), str::FormatTemp("%d", annot->pageNo),
                             trans::GetTranslation(AnnotationReadableNameTemp(annot->type)), excerpt,
                             ew->annotationLocationWidth);
}

static Static* CreateStatic(HWND parent, HFONT font, const char* s = nullptr) {
    auto w = new Static();
    Static::CreateArgs args;
    args.parent = parent;
    args.text = s;
    args.isRtl = IsUIRtl();
    args.font = font;
    HWND hwnd = w->Create(args);
    ReportIf(!hwnd);
    return w;
}

static void CreateMainLayout(EditAnnotationsWindow* ew) {
    HWND parent = ew->hwnd;

    int dpi = ew->dpi > 0 ? ew->dpi : DpiGet(parent);
    HFONT fnt = GetAppFontForDpi(dpi);
    LOGFONTW lf{};
    if (GetObjectW(fnt, sizeof(lf), &lf) == sizeof(lf)) {
        lf.lfWeight = FW_SEMIBOLD;
        ew->headingFont = CreateFontIndirectW(&lf);
    }
    HFONT headFont = ew->headingFont ? ew->headingFont : fnt;
    COLORREF panelText = ThemeWindowTextColor();
    COLORREF panel = ThemeWindowControlBackgroundColor();
    GetEditAnnotationsThemeColors(panelText, panel);
    COLORREF secondary = ThemeInspectorSecondaryTextColor();

    auto shell = CreateAnnotationSidebarShell(parent, fnt, headFont, IsUIRtl(), MkFunc1(DrawAnnotListItem, ew),
                                              MkFunc0(ListBoxSelectionChanged, ew), _TRA("Annotations"));
    auto vbox = shell.root;
    auto box = shell.inspector;
    ew->listBox = shell.list;
    shell.list->onDelete = MkFunc1(DeleteAnnotationListItem, ew);
    auto pane = shell.pane;
    ew->inspectorPane = pane;
    ew->inspectorLayout = pane->inner;
    HWND sidebarParent = parent;
    parent = pane->hwnd;

    {
        auto w = new AnnotHeadingLine();
        w->titleFont = headFont;
        w->metaFont = fnt;
        CreateCustomArgs args;
        args.parent = parent;
        args.style = WS_CHILD | WS_VISIBLE;
        args.bgColor = panel;
        args.font = headFont;
        args.pos = {0, 0, 10, 10};
        w->CreateCustom(args);
        w->SetColors(ThemeWindowTextColor(), panel);
        w->SetInsetsPt(4, 0, 0, 0);
        ew->staticHeading = w;
        box->AddChild(w);
    }

    {
        auto w = new AnnotHeadingLine();
        w->titleFont = headFont;
        w->metaFont = fnt;
        w->onClick = MkFunc0(BeginAuthorEdit, ew);
        CreateCustomArgs args;
        args.parent = parent;
        args.style = WS_CHILD | WS_VISIBLE;
        args.bgColor = panel;
        args.font = headFont;
        args.pos = {0, 0, 10, 10};
        w->CreateCustom(args);
        w->SetColors(ThemeWindowTextColor(), panel);
        w->SetInsetsPt(0, 0, 0, 0);
        ew->staticMeta = w;
        box->AddChild(w);
    }

    {
        // Kept collapsed. Position/size is on the heading line.
        auto w = CreateStatic(parent, fnt);
        ew->staticRect = w;
        w->SetIsVisible(false);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Author:"));
        ew->staticAuthor = w;
    }

    {
        Edit::CreateArgs args;
        args.parent = parent;
        args.idealSizeLines = 1;
        args.font = fnt;
        args.isRtl = IsUIRtl();
        args.withBorder = ThemeUsesDarkChrome();
        auto w = new Edit();
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);
        w->maxDx = MulDiv(150, dpi, 96);
        w->SetColors(ThemeWindowTextColor(), ThemeAnnotationContentsEditBackgroundColor());
        w->onTextChanged = MkFunc0(AuthorChanged, ew);
        w->onLostFocus = MkFunc0(EndAuthorEdit, ew);
        w->SetInsetsPt(4, 0, 0, 0);
        ew->editAuthor = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt);
        ew->staticModificationDate = w;
    }

    {
        auto w = CreateStatic(parent, fnt);
        w->SetColors(secondary, panel);
        w->SetInsetsPt(4, 0, 0, 0);
        ew->staticPopup = w;
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Note"));
        ew->staticContents = w;
        w->SetInsetsPt(12, 0, 4, 0);
        box->AddChild(w);
    }

    {
        Edit::CreateArgs args;
        args.parent = parent;
        args.isMultiLine = true;
        args.cueText = _TRA("Write a note…");
        args.idealSizeLines = 4;
        args.font = fnt;
        args.isRtl = IsUIRtl();
        args.withBorder = false;
        auto w = new AnnotNoteEdit();
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);
        int pad = DpiScale(hwnd, 8);
        SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad, pad));
        w->maxDx = MulDiv(150, dpi, 96);
        w->SetColors(ThemeWindowTextColor(), ThemeAnnotationContentsEditBackgroundColor());
        w->onTextChanged = MkFunc0(ContentsChanged, ew);
        ew->editContents = w;
        pane->note = w;
        box->AddChild(w);
    }

    {
        // Under the note, trailing edge: this resets every appearance control
        // below and leaves the note text alone.
        auto row = new HBox();
        row->alignMain = MainAxisAlign::MainEnd;
        row->alignCross = CrossAxisAlign::CrossCenter;
        auto reset = new AnnotResetButton();
        // Sit on the note's bottom-right corner, just left of its scrollbar.
        reset->SetInsetsPt(-2, 0, 4, 0);
        ReportIf(!reset->Create(parent));
        reset->onClick = MkFunc0(ButtonRestoreAnnotationDefaults, ew);
        row->AddChild(reset);
        box->AddChild(row);
        ew->buttonRestoreFreeText = reset;
        ew->resetAppearanceRow = row;
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Alignment:"));
        w->SetInsetsPt(2, 0, 0, 0);
        ew->staticTextAlignment = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->SetItemsSeqStrings(gQuaddingNames);
        w->onSelectionChanged = MkFunc0(TextAlignmentSelectionChanged, ew);
        ew->dropDownTextAlignment = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Font:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticTextFont = w;
        box->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Helvetica");
        args.font = fnt;
        args.isRtl = IsUIRtl();
        auto w = new Button();
        w->SetInsetsPt(4, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);
        w->onClick = MkFunc0(ButtonPickFreeTextFont, ew);
        SetPropW(hwnd, L"AnnotLeftAligned", (HANDLE)1);
        SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | BS_LEFT);
        ew->buttonTextFont = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Size:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticTextSize = w;
        box->AddChild(w);
    }

    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 8;
        args.rangeMax = 36;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Trackbar();
        w->SetInsetsPt(4, 0, 0, 0);

        w->Create(args);

        w->onPositionChanging = MkFunc1(TextFontSizeChanging, ew);
        ew->trackbarTextSize = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Text Color:"));
        ew->staticTextColor = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new AnnotColorDropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->SetItemsSeqStrings(gColors);
        w->onSelectionChanged = MkFunc0(TextColorSelectionChanged, ew);
        ew->dropDownTextColor = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Border Color:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticBorderColor = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new AnnotColorDropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);
        w->SetItemsSeqStrings(gColors);
        w->onSelectionChanged = MkFunc0(BorderColorSelectionChanged, ew);
        ew->dropDownBorderColor = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Line Start:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticLineStart = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new AnnotIconDropDown();
        w->lineEndPreview = true;
        w->lineStartPreview = true;
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->onSelectionChanged = MkFunc0(LineStartSelectionChanged, ew);
        ew->dropDownLineStart = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Line End:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticLineEnd = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new AnnotIconDropDown();
        w->lineEndPreview = true;
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->onSelectionChanged = MkFunc0(LineEndSelectionChanged, ew);
        ew->dropDownLineEnd = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Icon:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticIcon = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new AnnotIconDropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->onSelectionChanged = MkFunc0(IconSelectionChanged, ew);
        ew->dropDownIcon = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, "Border:");
        w->SetInsetsPt(8, 0, 0, 0);
        // The border / color / fill block sits 6px higher for every annotation type.
        w->insets.top -= 6;
        ew->staticBorder = w;
        box->AddChild(w);
    }

    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = borderWidthMin;
        args.rangeMax = borderWidthMax;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Trackbar();
        w->Create(args);
        w->onPositionChanging = MkFunc1(BorderWidthChanging, ew);
        ew->trackbarBorder = w;
        box->AddChild(w);
    }

    {
        auto property = AddAnnotationColorProperty(box, parent, fnt, IsUIRtl(), _TRA("Color"), gColors,
                                                   MkFunc0(ColorSelectionChanged, ew));
        ew->staticColor = property.label;
        ew->dropDownColor = property.value;
        // Existing PDF metadata helper; not an extra visible swatch.
        auto swatch = new AnnotColorSwatch();
        CreateCustomArgs args;
        args.parent = parent;
        args.style = WS_CHILD;
        args.pos = {0, 0, 14, 14};
        swatch->CreateCustom(args);
        swatch->SetColors(ThemeWindowTextColor(), panel);
        ew->colorSwatch = swatch;
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Interior Color:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticInteriorColor = w;
        box->AddChild(w);
    }

    {
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new AnnotColorDropDown();
        w->SetInsetsPt(4, 0, 0, 0);
        w->Create(args);

        w->SetItemsSeqStrings(gColors);
        w->onSelectionChanged = MkFunc0(InteriorColorSelectionChanged, ew);
        ew->dropDownInteriorColor = w;
        box->AddChild(w);
    }

    {
        auto w = CreateStatic(parent, fnt, _TRA("Opacity:"));
        w->SetInsetsPt(8, 0, 0, 0);
        ew->staticOpacity = w;
        box->AddChild(w);
    }

    {
        Trackbar::CreateArgs args;
        args.parent = parent;
        args.rangeMin = 0;
        args.rangeMax = 255;
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Trackbar();
        w->Create(args);

        w->onPositionChanging = MkFunc1(OpacityChanging, ew);
        ew->trackbarOpacity = w;
        box->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Save...");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->onClick = MkFunc0(ButtonSaveAttachment, ew);
        ew->buttonSaveAttachment = w;
        box->AddChild(w);
    }

    {
        Button::CreateArgs args;
        args.parent = parent;
        args.text = _TRA("Embed...");
        args.font = fnt;
        args.isRtl = IsUIRtl();

        auto w = new Button();
        w->SetInsetsPt(8, 0, 0, 0);
        HWND hwnd = w->Create(args);
        ReportIf(!hwnd);

        w->onClick = MkFunc0(ButtonEmbedAttachment, ew);
        ew->buttonEmbedAttachment = w;
        box->AddChild(w);
    }

    {
        // Collapsed leftovers: author edit / date / popup stay parented but
        // take no space. Position/size lives on the heading; Details is gone.
        auto w = CreateStatic(parent, fnt);
        ew->staticDetails = w;
        w->SetIsVisible(false);
        box->AddChild(w);
        box->AddChild(ew->staticRect);
        box->AddChild(ew->staticPopup);
        box->AddChild(ew->staticAuthor);
        box->AddChild(ew->staticModificationDate);
    }

    parent = sidebarParent;
    auto footer = shell.footer;
    ew->footerLayout = footer;
    auto commands = AddAnnotationCommandBar(footer);
    ew->buttonExport = AddAnnotationFooterAction(commands, parent, fnt, IsUIRtl(), _TRA("Export Notes"), false,
                                                 MkFunc0(ExportClicked, ew));
    ew->buttonSaveToCurrentFile = AddAnnotationFooterAction(commands, parent, fnt, IsUIRtl(), _TRA("Save"), false,
                                                            MkFunc0(ButtonSaveToCurrentPDFHandler, ew));
    ew->buttonSaveToNewFile = AddAnnotationFooterAction(commands, parent, fnt, IsUIRtl(), _TRA("Save as…"), false,
                                                        MkFunc0(ButtonSaveToNewFileHandler, ew));
    commands->exportButton = ew->buttonExport;
    commands->saveButton = ew->buttonSaveToCurrentFile;
    commands->copyButton = ew->buttonSaveToNewFile;
    ((AnnotCommandButton*)commands->exportButton)->tooltipText = _TRA("Export Notes");
    ((AnnotCommandButton*)commands->saveButton)->tooltipText = _TRA("Save to this PDF");
    ((AnnotCommandButton*)commands->saveButton)->reserveSaveWidth = true;
    ((AnnotCommandButton*)commands->copyButton)->tooltipText = _TRA("Save as…");

    auto padding = new Padding(vbox, DpiScaledInsets(parent, 8, 12));
    ew->mainLayout = padding;
    HidePerAnnotControls(ew);
}

static void RevealAnnotationsSidebar(WindowTab* tab, bool reveal) {
    if (!tab || !tab->win) {
        return;
    }
    if (reveal) {
        ShowSidebarPage(tab->win, SidebarView::Annotations);
    }
    ApplySidebarViewLayout(tab->win);
}

void ShowEditAnnotationsWindow(WindowTab* tab, Annotation* annot, EditAnnotFocus focus, bool revealInSidebar) {
    if (!tab) return;
    auto engine = tab->GetEngine();
    auto canAnnotate = EngineSupportsAnnotations(engine);
    if (!canAnnotate) {
        ReportDebugIf(true);
        return;
    }
    EditAnnotationsWindow* ew = tab->editAnnotsWindow;
    if (ew) {
        bool isNew = annot && annot != ew->tab->win->annotationUnderCursor;
        if (annot) {
            SetSelectedAnnotation(tab, annot, isNew, focus);
        } else if (ew->listBox && ew->listBox->hwnd && focus != EditAnnotFocus::Edit) {
            HwndSetFocus(ew->listBox->hwnd);
        }
        RevealAnnotationsSidebar(tab, revealInSidebar);
        return;
    }
    HWND parentHwnd = tab->win->hwndTocBox;
    if (!parentHwnd) {
        return;
    }
    ew = new EditAnnotationsWindow();
    ew->onClose = MkFunc1Void(OnClose);
    CreateCustomArgs args;
    HMODULE h = GetModuleHandleW(nullptr);
    WCHAR* iconName = MAKEINTRESOURCEW(GetAppIconID());
    args.icon = LoadIconW(h, iconName);
    {
        COLORREF textCol = ThemeWindowTextColor();
        COLORREF bgCol = ThemeWindowControlBackgroundColor();
        GetEditAnnotationsThemeColors(textCol, bgCol);
        args.bgColor = bgCol;
    }

    args.title = str::JoinTemp(_TRA("Annotations"), ": ", tab->GetTabTitle());
    args.visible = false;
    args.parent = parentHwnd;
    args.style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    args.pos = {0, 0, 10, 10};
    int parentDpi = tab->win->frameDpi > 0 ? tab->win->frameDpi : DpiGet(parentHwnd);
    args.font = GetAppFontForDpi(parentDpi);

    ew->CreateCustom(args);
    ew->dpi = parentDpi > 0 ? parentDpi : DpiGet(ew->hwnd);

    // Build the full inspector off-screen, then show once themed. Otherwise
    // Warm/Light startup paints WHITE_BRUSH then the cream panel.
    ew->SuspendRedraw();
    CreateMainLayout(ew);
    ew->tab = tab;
    tab->editAnnotsWindow = ew;

    UpdateAnnotationsList(ew);

    if (!annot) annot = ew->tab->selectedAnnotation;
    ew->skipGoToPage = (annot != nullptr);
    if (annot) {
        bool isNew = annot != ew->tab->win->annotationUnderCursor;
        SetSelectedAnnotation(tab, annot, isNew, focus);
    }
    ApplyEditAnnotationsWindowTheme(ew, true);
    ew->ResumeRedraw();
    RevealAnnotationsSidebar(tab, revealInSidebar);
}

//--- in-place free text editing

// A plain edit control sits exactly over the free text annotation while you
// type: same font, size, background and border as the rendered annotation.
// Enter makes a new line; Ctrl+Enter, Esc or clicking away ends it and the
// rendered annotation comes back.
struct FreeTextInPlaceEdit {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;
    Annotation* annot = nullptr;
    EbookAnnotation* ebookAnnot = nullptr;
    Size size;
    Size minSize;
    int padding = 0;
    // what MuPDF will lay the text out with
    int textSize = 12;
    float borderWidth = 0;
    int borderPx = 0;
    int fontPx = 0;
    float scale = 1.f;
    bool composing = false;
    bool canvasClippedChildren = false;
    bool suspendClose = false;
    COLORREF textCol = RGB(0, 0, 0);
    COLORREF borderCol = RGB(0, 0, 0);
    bool bgTransparent = false;
    COLORREF bgCol = RGB(255, 255, 255);
    HBRUSH bgBrush = nullptr;
    HBITMAP bgBitmap = nullptr;
};

static FreeTextInPlaceEdit gInPlace;
static void UpdateFreeTextPropertyToolbar(MainWindow* win);
static void SizeInPlaceEditToText();

static void SyncInPlaceFreeTextBorder(Annotation* annot, float borderWidth) {
    if (!gInPlace.hwnd || gInPlace.annot != annot) {
        return;
    }
    gInPlace.borderWidth = borderWidth;
    SetWindowPos(gInPlace.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SizeInPlaceEditToText();
}
static WNDPROC gInPlaceDefProc = nullptr;
static bool gInPlaceEnding = false;
// Set while copying text or style between the page editor and the sidebar,
// so the two updates do not call back into each other.
static bool gInPlaceSyncing = false;
// Nested while a key's WM_CHAR runs inside WM_KEYDOWN. Redraw stays off until
// the outermost mutation returns, then the whole box is erased and painted once.
static int gInPlaceRedrawFreeze = 0;

static void FreezeInPlaceRedraw(HWND hwnd) {
    if (gInPlaceRedrawFreeze++ == 0) {
        SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    }
}

static void ThawInPlaceRedraw(HWND hwnd) {
    if (gInPlaceRedrawFreeze <= 0) {
        return;
    }
    if (--gInPlaceRedrawFreeze == 0) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        // Backspace and Delete scroll the existing pixels. Over a transparent
        // box that leaves the previous glyphs in place, so the line shifts or
        // piles up. One full erase paints the page, then the current text.
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

static bool InPlaceTextMutation(UINT msg, WPARAM wp) {
    if (gInPlace.composing || !gInPlace.bgTransparent) {
        return false;
    }
    if (msg == WM_KEYDOWN) {
        return wp == VK_BACK || wp == VK_DELETE;
    }
    if (msg == WM_CHAR) {
        return wp != VK_ESCAPE && wp != 0x0A;
    }
    return msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO || msg == EM_REPLACESEL;
}

static const WCHAR* FreeTextFaceFromPdf(const char* fontPdf) {
    return FreeTextWindowsFace(fontPdf);
}

static bool HwndBelongsToAnnotationSidebar(HWND hwnd) {
    if (!hwnd || !gInPlace.tab || !gInPlace.tab->editAnnotsWindow) {
        return false;
    }
    HWND sidebar = gInPlace.tab->editAnnotsWindow->hwnd;
    if (!sidebar) {
        return false;
    }
    for (int i = 0; hwnd && i < 12; i++) {
        if (hwnd == sidebar || IsChild(sidebar, hwnd)) {
            return true;
        }
        HWND owner = GetWindow(hwnd, GW_OWNER);
        hwnd = owner ? owner : GetParent(hwnd);
    }
    return false;
}

// Sidebar Note text changed: keep the page editor on the same words.
static void SyncSidebarNoteToInPlace(Annotation* annot, const char* textLf) {
    if (gInPlaceSyncing || !gInPlace.hwnd || !annot || gInPlace.annot != annot) {
        return;
    }
    TempStr cur = str::ReplaceTemp(HwndGetTextTemp(gInPlace.hwnd), "\r\n", "\n");
    cur = str::ReplaceTemp(cur, "\r", "\n");
    if (str::Eq(cur ? cur : "", textLf ? textLf : "")) {
        return;
    }
    TempStr shown = str::ReplaceTemp(textLf ? textLf : "", "\n", "\r\n");
    gInPlaceSyncing = true;
    HwndSetText(gInPlace.hwnd, shown);
    int end = (int)SendMessageW(gInPlace.hwnd, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(gInPlace.hwnd, EM_SETSEL, end, end);
    SizeInPlaceEditToText();
    gInPlaceSyncing = false;
}

// Page editor changed: write the same words into the sidebar Note.
static void SyncInPlaceNoteToSidebar() {
    if (gInPlaceSyncing || !gInPlace.hwnd || !gInPlace.tab) {
        return;
    }
    if (gInPlace.ebookAnnot) {
        SyncEbookFreeTextDraft(gInPlace.tab, gInPlace.ebookAnnot, HwndGetTextTemp(gInPlace.hwnd));
        return;
    }
    if (!gInPlace.annot) return;
    EditAnnotationsWindow* ew = gInPlace.tab->editAnnotsWindow;
    if (!ew || !ew->editContents || ew->tab->selectedAnnotation != gInPlace.annot) {
        return;
    }
    TempStr text = str::ReplaceTemp(HwndGetTextTemp(gInPlace.hwnd), "\r\n", "\n");
    text = str::ReplaceTemp(text, "\r", "\n");
    const char* cur = Contents(gInPlace.annot);
    if (str::Eq(cur ? cur : "", text ? text : "")) {
        return;
    }
    TempStr shown = str::ReplaceTemp(text ? text : "", "\n", "\r\n");
    gInPlaceSyncing = true;
    ew->updatingControls = true;
    ew->editContents->SetText(shown);
    ew->updatingControls = false;
    gInPlaceSyncing = false;
    EnableSaveIfAnnotationsChanged(ew);
}

// Sidebar font or size changed: the page editor uses that face and size now.
static bool InPlacePageBounds(int* pageNo, RectF* bounds);

static void DiscardInPlaceBackdrop() {
    if (gInPlace.bgBrush) {
        DeleteObject(gInPlace.bgBrush);
        gInPlace.bgBrush = nullptr;
    }
    if (gInPlace.bgBitmap) {
        DeleteObject(gInPlace.bgBitmap);
        gInPlace.bgBitmap = nullptr;
    }
}

// Page pixels under a transparent free-text box, without annotation paint,
// so the editor is not a white card and does not double the glyphs.
static HBITMAP CaptureFreeTextBackdrop(DisplayModel* dm, int pageNo, Rect screen) {
    if (!dm || screen.IsEmpty()) {
        return nullptr;
    }
    EngineBase* engine = dm->GetEngine();
    EngineMupdf* mupdf = AsEngineMupdf(engine);
    if (!mupdf) {
        return nullptr;
    }
    RectF pageRect = dm->CvtFromScreen(screen, pageNo);
    if (pageRect.dx < 0.5f || pageRect.dy < 0.5f) {
        return nullptr;
    }
    bool savedHide = engine->hideAnnotations;
    engine->hideAnnotations = true;
    float zoom = dm->GetZoomReal(pageNo);
    RenderPageArgs args(pageNo, zoom, dm->GetRotation(), &pageRect);
    DarkModeProfile profile{};
    BuildViewDarkModeProfile(engine, &profile);
    if (profile.mode != PageColorMode::Normal) {
        args.darkProfile = &profile;
    }
    RenderedBitmap* bmp = mupdf->RenderPage(args);
    engine->hideAnnotations = savedHide;
    if (!bmp || !bmp->GetBitmap()) {
        delete bmp;
        return nullptr;
    }
    // Same tint as the page tile, so a transparent free-text box is not a white card.
    ApplyRenderThemePostColors(engine, bmp, pageNo, zoom, &pageRect, args.darkProfile);
    HBITMAP copy = (HBITMAP)CopyImage(bmp->GetBitmap(), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    delete bmp;
    return copy;
}

static void ApplyInPlaceColors(Annotation* annot) {
    EbookAnnotation* ebook = gInPlace.ebookAnnot;
    PdfColor pdfTextCol = annot ? DefaultAppearanceTextColor(annot)
                                : PdfAnnotationColorFromColorRef(ebook ? EbookAnnotationGetColor(ebook) : RGB(0, 0, 0));
    if (pdfTextCol != kColorUnset && pdfTextCol != 0) {
        u8 r, g, b, a;
        UnpackPdfColor(pdfTextCol, r, g, b, a);
        gInPlace.textCol = RGB(r, g, b);
    } else if (!annot && ebook) {
        gInPlace.textCol = EbookAnnotationGetColor(ebook);
    }
    if (annot) {
        gInPlace.borderCol = ColorRefFromPdfColor(FreeTextBorderColor(annot));
        PdfColor bg = GetColor(annot);
        gInPlace.bgTransparent = (bg == 0);
        if (!gInPlace.bgTransparent) {
            gInPlace.bgCol = ColorRefFromPdfColor(bg);
        }
    } else if (ebook) {
        gInPlace.borderCol = EbookAnnotationGetFreeTextBorderColor(ebook);
        COLORREF bg = 0;
        gInPlace.bgTransparent = !EbookAnnotationGetFreeTextBackground(ebook, &bg);
        if (!gInPlace.bgTransparent) {
            gInPlace.bgCol = bg;
        }
    }
    DiscardInPlaceBackdrop();
    if (!gInPlace.bgTransparent) {
        gInPlace.bgBrush = CreateSolidBrush(gInPlace.bgCol);
        return;
    }
    DisplayModel* dm = gInPlace.tab ? gInPlace.tab->AsFixed() : nullptr;
    int pageNo = 0;
    RectF bounds;
    if (dm && InPlacePageBounds(&pageNo, &bounds)) {
        Rect screen = dm->CvtToScreen(pageNo, bounds);
        gInPlace.bgBitmap = CaptureFreeTextBackdrop(dm, pageNo, screen);
    }
}

static void RefreshInPlaceFreeTextStyle(Annotation* annot) {
    if (!gInPlace.hwnd || gInPlace.annot != annot || (!annot && !gInPlace.ebookAnnot)) {
        return;
    }
    int textSize = (annot ? DefaultAppearanceTextSize(annot) : EbookAnnotationGetFreeTextSize(gInPlace.ebookAnnot));
    if (textSize <= 0) {
        textSize = 12;
    }
    const WCHAR* face = FreeTextFaceFromPdf(
        (annot ? DefaultAppearanceTextFont(annot) : EbookAnnotationGetFreeTextFont(gInPlace.ebookAnnot)));
    int fontPx = std::max(6, (int)(((float)textSize * gInPlace.scale) + 0.5f));
    LOGFONTW current{};
    bool haveFont = gInPlace.font && GetObjectW(gInPlace.font, sizeof(current), &current);
    bool fontChanged = !haveFont || current.lfHeight != -fontPx || wcscmp(current.lfFaceName, face) != 0;
    if (fontChanged) {
        HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        if (!font) {
            return;
        }
        HwndSetFont(gInPlace.hwnd, font);
        if (gInPlace.font) {
            DeleteObject(gInPlace.font);
        }
        gInPlace.font = font;
    }
    gInPlace.textSize = textSize;
    gInPlace.fontPx = fontPx;
    LONG style = GetWindowLongW(gInPlace.hwnd, GWL_STYLE);
    LONG next = style & ~(ES_CENTER | ES_RIGHT);
    int align = (annot ? Quadding(annot) : EbookAnnotationGetFreeTextAlignment(gInPlace.ebookAnnot));
    if (align == 1) {
        next |= ES_CENTER;
    } else if (align == 2) {
        next |= ES_RIGHT;
    }
    if (next != style) {
        SetWindowLongW(gInPlace.hwnd, GWL_STYLE, next);
    }
    ApplyInPlaceColors(annot);
    SetWindowPos(gInPlace.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(gInPlace.hwnd, nullptr, TRUE);
    SizeInPlaceEditToText();
}
bool IsEditingFreeTextInPlace(MainWindow* win) {
    if (!gInPlace.hwnd) {
        return false;
    }
    return !win || gInPlace.win == win;
}

// The persisted annotation rectangle is the only geometry authority. Native
// multiline editing wraps within it and scrolls vertically for overflow.
static bool InPlacePageBounds(int* pageNo, RectF* bounds) {
    if (!gInPlace.tab) return false;
    if (gInPlace.ebookAnnot) {
        return EbookAnnotationGetPageBounds(gInPlace.tab, gInPlace.tab->AsFixed(), gInPlace.ebookAnnot, pageNo, bounds);
    }
    if (!gInPlace.annot || !gInPlace.annot->pdfannot) return false;
    *pageNo = PageNo(gInPlace.annot);
    *bounds = GetRect(gInPlace.annot);
    return true;
}

static void SizeInPlaceEditToText() {
    if (!gInPlace.hwnd || !gInPlace.tab) return;
    DisplayModel* dm = gInPlace.tab->AsFixed();
    if (!dm) return;
    int pageNo;
    RectF bounds;
    if (!InPlacePageBounds(&pageNo, &bounds)) return;
    Rect r = dm->CvtToScreen(pageNo, bounds);
    gInPlace.size = r.Size();
    int box = std::min(r.dx, r.dy);
    gInPlace.borderPx = FreeTextBorderPixels(gInPlace.borderWidth, gInPlace.scale, box);
    SetWindowPos(gInPlace.hwnd, nullptr, r.x, r.y, r.dx, r.dy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RECT content;
    GetClientRect(gInPlace.hwnd, &content);
    // The non-client border already occupies borderPx. The rest of the PDF inset is the font margin.
    int total = (int)((gInPlace.borderWidth + gInPlace.textSize * 0.4f) * gInPlace.scale + 0.5f);
    int pad = std::max(0, total - gInPlace.borderPx);
    pad = std::min(pad, std::max(0, (int)(std::min(content.right, content.bottom) * 0.45f)));
    InflateRect(&content, -pad, -pad);
    SendMessageW(gInPlace.hwnd, EM_SETRECTNP, 0, (LPARAM)&content);
}

// Scrolling and zooming move the annotation out from under the box.
void RepositionFreeTextInPlaceEdit(MainWindow* win) {
    UpdateFreeTextPropertyToolbar(win);
    if (!IsEditingFreeTextInPlace(win)) {
        return;
    }
    DisplayModel* dm = gInPlace.win->AsFixed();
    Annotation* annot = gInPlace.annot;
    int pageNo = 0;
    RectF bounds;
    bool valid = InPlacePageBounds(&pageNo, &bounds);
    if (win->CurrentTab() != gInPlace.tab || !dm || !valid || !dm->PageVisible(pageNo)) {
        EndFreeTextInPlaceEdit(true);
        return;
    }
    int size = annot ? DefaultAppearanceTextSize(annot) : EbookAnnotationGetFreeTextSize(gInPlace.ebookAnnot);
    const WCHAR* face = FreeTextFaceFromPdf(annot ? DefaultAppearanceTextFont(annot)
                                                  : EbookAnnotationGetFreeTextFont(gInPlace.ebookAnnot));
    LOGFONTW currentFont{};
    GetObjectW(gInPlace.font, sizeof(currentFont), &currentFont);
    COLORREF color = annot ? ColorRefFromPdfAnnotationColor(DefaultAppearanceTextColor(annot))
                           : EbookAnnotationGetColor(gInPlace.ebookAnnot);
    COLORREF borderCol = annot ? ColorRefFromPdfColor(FreeTextBorderColor(annot))
                               : EbookAnnotationGetFreeTextBorderColor(gInPlace.ebookAnnot);
    COLORREF bg = 0;
    bool bgTransparent = annot ? GetColor(annot) == 0 : !EbookAnnotationGetFreeTextBackground(gInPlace.ebookAnnot, &bg);
    if (!bgTransparent && annot) {
        bg = ColorRefFromPdfColor(GetColor(annot));
    }
    int alignment = annot ? Quadding(annot) : EbookAnnotationGetFreeTextAlignment(gInPlace.ebookAnnot);
    LONG style = GetWindowLongW(gInPlace.hwnd, GWL_STYLE);
    int currentAlignment = (style & ES_CENTER) ? 1 : (style & ES_RIGHT) ? 2 : 0;
    if (size != gInPlace.textSize || wcscmp(face, currentFont.lfFaceName) != 0 || color != gInPlace.textCol ||
        borderCol != gInPlace.borderCol || bgTransparent != gInPlace.bgTransparent ||
        (!bgTransparent && bg != gInPlace.bgCol) || alignment != currentAlignment)
        RefreshInPlaceFreeTextStyle(annot);
    float borderWidth = annot ? BorderWidthF(annot) : (float)EbookAnnotationGetFreeTextBorderWidth(gInPlace.ebookAnnot);
    if (fabsf(borderWidth - gInPlace.borderWidth) > 0.01f) {
        gInPlace.borderWidth = borderWidth;
        SetWindowPos(gInPlace.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SizeInPlaceEditToText();
    }
    Rect r = dm->CvtToScreen(pageNo, bounds);
    float scale = gInPlace.ebookAnnot ? (float)DpiScale(win->hwndFrame, 96) / 96.f : dm->GetZoomReal(pageNo);
    bool scaleChanged = fabsf(scale - gInPlace.scale) > 0.001f;
    if (scaleChanged) {
        float ratio = scale / gInPlace.scale;
        gInPlace.size = Size((int)(gInPlace.size.dx * ratio), (int)(gInPlace.size.dy * ratio));
        gInPlace.minSize = r.Size();
        gInPlace.scale = scale;
        gInPlace.fontPx = std::max(6, (int)(gInPlace.textSize * scale + 0.5f));
        LOGFONTW lf{};
        GetObjectW(gInPlace.font, sizeof(lf), &lf);
        lf.lfHeight = -gInPlace.fontPx;
        HFONT font = CreateFontIndirectW(&lf);
        if (font) {
            HwndSetFont(gInPlace.hwnd, font);
            DeleteObject(gInPlace.font);
            gInPlace.font = font;
        }
        SetWindowPos(gInPlace.hwnd, nullptr, r.x, r.y, gInPlace.size.dx, gInPlace.size.dy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SizeInPlaceEditToText();
    }
    Rect cur = ChildPosWithinParent(gInPlace.hwnd);
    if (cur.Size() != r.Size()) {
        SizeInPlaceEditToText();
    }
    if (gInPlace.bgTransparent && (scaleChanged || cur.x != r.x || cur.y != r.y)) {
        ApplyInPlaceColors(annot);
        InvalidateRect(gInPlace.hwnd, nullptr, TRUE);
    }
    if (cur.x == r.x && cur.y == r.y) {
        return;
    }
    SetWindowPos(gInPlace.hwnd, nullptr, r.x, r.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EndFreeTextInPlaceEdit(bool accept) {
    if (!gInPlace.hwnd || gInPlaceEnding) {
        return;
    }
    gInPlaceEnding = true;
    HWND hwnd = gInPlace.hwnd;
    // Drop the highlight while redraw is off, then hide. The edit would
    // otherwise paint a solid run as it clears the selection, and that paint
    // stays on the page until the next tile lands.
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(hwnd, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    if (selStart != selEnd) {
        SendMessageW(hwnd, EM_SETSEL, selEnd, selEnd);
    }
    ShowWindow(hwnd, SW_HIDE);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    HFONT font = gInPlace.font;
    MainWindow* win = gInPlace.win;
    WindowTab* tab = gInPlace.tab;
    Annotation* annot = gInPlace.annot;
    EbookAnnotation* ebookAnnot = gInPlace.ebookAnnot;
    bool canvasClippedChildren = gInPlace.canvasClippedChildren;
    TempStr text{};
    if (accept) {
        text = str::DupTemp(HwndGetTextTemp(hwnd));
        text = str::ReplaceTemp(text, "\r\n", "\n");
        text = str::ReplaceTemp(text, "\r", "\n");
    }
    // clear the state and unsubclass before destroying, so the destroy-time
    // WM_KILLFOCUS doesn't come back through the commit path
    HBRUSH bgBrush = gInPlace.bgBrush;
    HBITMAP bgBitmap = gInPlace.bgBitmap;
    gInPlace.bgBrush = nullptr;
    gInPlace.bgBitmap = nullptr;
    gInPlace = {};
    gInPlaceRedrawFreeze = 0;
    if (bgBrush) {
        DeleteObject(bgBrush);
    }
    if (bgBitmap) {
        DeleteObject(bgBitmap);
    }
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)gInPlaceDefProc);
    DestroyWindow(hwnd);
    if (!canvasClippedChildren && IsWindow(win->hwndCanvas)) {
        LONG_PTR style = GetWindowLongPtrW(win->hwndCanvas, GWL_STYLE);
        SetWindowLongPtrW(win->hwndCanvas, GWL_STYLE, style & ~WS_CLIPCHILDREN);
    }
    if (font) {
        DeleteObject(font);
    }
    bool winOk = win && IsWindow(win->hwndCanvas);
    if (winOk && GetFocus() == nullptr) {
        HwndSetFocus(win->hwndCanvas);
    }
    bool painted = false;
    if (ebookAnnot) {
        // SetNote rerenders when the note changed. An unchanged close must not
        // throw the page tiles away.
        if (accept) {
            EbookAnnotationSetNote(tab, ebookAnnot, text);
        }
        if (tab->editEbookAnnotsWindow) UpdateEbookAnnotationsList(tab->editEbookAnnotsWindow, ebookAnnot);
    } else if (accept && (annot && annot->pdfannot)) {
        int pageNo = PageNo(annot);
        // Same text: the tile already shows it. Invalidating here stacks a
        // second GDI copy on that tile until MuPDF redraws (a few seconds).
        bool changed = SetContents(annot, text);
        NotifyAnnotationsChanged(tab->editAnnotsWindow);
        if (tab->editAnnotsWindow) {
            if (tab->selectedAnnotation == annot) {
                DoContents(tab->editAnnotsWindow, annot);
            }
        }
        if (changed && winOk && win->CurrentTab() == tab) {
            MainWindowRerenderAnnotationChange(win, pageNo, annot);
            ToolbarUpdateStateForWindow(win, false);
            painted = true;
        }
    } else if (winOk && tab && tab->editAnnotsWindow && tab->selectedAnnotation == annot) {
        DoContents(tab->editAnnotsWindow, annot);
    }
    if (winOk && !painted) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
        UpdateWindow(win->hwndCanvas);
    }
    gInPlaceEnding = false;
}

static LRESULT CALLBACK WndProcFreeTextInPlaceEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETDLGCODE:
            // Enter is a new line and Esc cancels, so we want every key
            return DLGC_WANTALLKEYS;
        case WM_IME_STARTCOMPOSITION:
            gInPlace.composing = true;
            break;
        case WM_IME_ENDCOMPOSITION:
            gInPlace.composing = false;
            break;
        case WM_KEYDOWN:
            if (gInPlace.composing) {
                break;
            }
            if (wp == 'A' && IsCtrlPressed()) {
                SendMessageW(hwnd, EM_SETSEL, 0, -1);
                return 0;
            }
            if (wp == VK_F6 && gFreeTextToolbarHwnd && IsWindowVisible(gFreeTextToolbarHwnd)) {
                HwndSetFocus(gFreeTextToolbarHwnd);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                EndFreeTextInPlaceEdit(false);
                return 0;
            }
            if (wp == VK_RETURN && IsCtrlPressed()) {
                EndFreeTextInPlaceEdit(true);
                return 0;
            }
            break;
        case WM_CHAR:
            // Ctrl+Enter reaches an edit control as LF. That, not the key-down
            // above, is what a real keyboard delivers here.
            if (wp == 0x0A) {
                EndFreeTextInPlaceEdit(true);
                return 0;
            }
            // Esc was handled on key down; don't also insert it
            if (wp == VK_ESCAPE) {
                return 0;
            }
            break;
        case WM_ERASEBKGND: {
            if (!gInPlace.bgTransparent) {
                break;
            }
            HDC hdc = (HDC)wp;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (gInPlace.bgBitmap) {
                HDC mem = CreateCompatibleDC(hdc);
                HGDIOBJ old = SelectObject(mem, gInPlace.bgBitmap);
                BITMAP bm{};
                GetObject(gInPlace.bgBitmap, sizeof(bm), &bm);
                StretchBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            } else {
                // No captured page: still cover the annotation that was on screen.
                HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
                FillRect(hdc, &rc, brush);
                DeleteObject(brush);
            }
            return 1;
        }
        case WM_NCCALCSIZE: {
            // Width 0 keeps the client full-bleed. A real border reserves the
            // same device pixels the PDF stroke occupies.
            LRESULT res = CallWindowProcW(gInPlaceDefProc, hwnd, msg, wp, lp);
            int px = gInPlace.borderPx;
            if (px <= 0) {
                return res;
            }
            RECT* rc = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
            if (rc->right - rc->left > px * 2 + 2 && rc->bottom - rc->top > px * 2 + 2) {
                InflateRect(rc, -px, -px);
            }
            return res;
        }
        case WM_NCPAINT: {
            CallWindowProcW(gInPlaceDefProc, hwnd, msg, wp, lp);
            if (gInPlace.borderPx <= 0) {
                return 0;
            }
            HDC hdc = GetWindowDC(hwnd);
            if (hdc) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                RECT outer{0, 0, wr.right - wr.left, wr.bottom - wr.top};
                FillOuterBorderBand(hdc, outer, gInPlace.borderPx, gInPlace.borderCol);
                ReleaseDC(hwnd, hdc);
            }
            return 0;
        }
        case WM_KILLFOCUS: {
            HWND next = (HWND)wp;
            if (!next || gInPlace.suspendClose) {
                break;
            }
            // The sidebar, floating toolbar and font picker edit this same annotation.
            if (next == gFreeTextToolbarHwnd || IsChild(gFreeTextToolbarHwnd, next) ||
                HwndBelongsToAnnotationSidebar(next) || HwndBelongsToFontPicker(next)) {
                break;
            }
            EndFreeTextInPlaceEdit(true);
            return 0;
        }
    }
    bool freezeRedraw = InPlaceTextMutation(msg, wp);
    if (freezeRedraw) {
        FreezeInPlaceRedraw(hwnd);
    }
    LRESULT res = CallWindowProcW(gInPlaceDefProc, hwnd, msg, wp, lp);
    if (freezeRedraw) {
        ThawInPlaceRedraw(hwnd);
    }
    if (msg == WM_PAINT && gInPlace.hwnd == hwnd && gInPlace.borderPx <= 0) {
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            COLORREF ink = FloatingPopupAccentColor();
            HPEN pen = CreatePen(PS_DOT, 1, ink);
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, 0, 0, rc.right, rc.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            ReleaseDC(hwnd, hdc);
        }
    }
    switch (msg) {
        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_PASTE:
        case WM_CUT:
        case WM_CLEAR:
        case WM_UNDO:
        case WM_SETTEXT:
        case EM_REPLACESEL:
            if (!gInPlaceSyncing) {
                SyncInPlaceNoteToSidebar();
            }
            break;
    }
    return res;
}

static bool StartFreeTextEdit(MainWindow* win, Annotation* annot, EbookAnnotation* ebookAnnot) {
    if (!win || !win->hwndCanvas || (!ebookAnnot && !(annot && annot->pdfannot))) {
        return false;
    }
    if (annot && Type(annot) != AnnotationType::FreeText) {
        return false;
    }
    if (gInPlace.hwnd && gInPlace.annot == annot && gInPlace.ebookAnnot == ebookAnnot) {
        return true;
    }
    EndFreeTextInPlaceEdit(true);
    DisplayModel* dm = win->AsFixed();
    int pageNo = annot ? PageNo(annot) : 0;
    RectF pageRect;
    if (ebookAnnot && !EbookAnnotationGetPageBounds(win->CurrentTab(), dm, ebookAnnot, &pageNo, &pageRect))
        return false;
    if (!dm || !dm->ValidPageNo(pageNo) || !dm->PageVisible(pageNo)) {
        return false;
    }
    if (annot) pageRect = GetRect(annot);
    Rect rc = dm->CvtToScreen(pageNo, pageRect);
    if (rc.IsEmpty()) {
        return false;
    }

    // screen pixels per PDF point, so the box matches the rendered text
    float scale = ebookAnnot ? (float)DpiScale(win->hwndFrame, 96) / 96.f : dm->GetZoomReal(pageNo);
    int textSize = (annot ? DefaultAppearanceTextSize(annot) : EbookAnnotationGetFreeTextSize(ebookAnnot));
    if (textSize <= 0) {
        textSize = 12;
    }
    float borderWidth =
        annot ? BorderWidthF(annot) : (float)std::max(EbookAnnotationGetFreeTextBorderWidth(ebookAnnot), 0);
    int fontPx = std::max(6, (int)(((float)textSize * scale) + 0.5f));
    const char* fontPdf = (annot ? DefaultAppearanceTextFont(annot) : EbookAnnotationGetFreeTextFont(ebookAnnot));
    const WCHAR* face = FreeTextFaceFromPdf(fontPdf);
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    if (!font) {
        return false;
    }

    // Keep the annotation width fixed; long text wraps and overflow scrolls.
    // no WS_BORDER: WM_NCPAINT draws the real border color at the PDF stroke width
    DWORD style = WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL;
    int align = (annot ? Quadding(annot) : EbookAnnotationGetFreeTextAlignment(ebookAnnot));
    style |= align == 1 ? ES_CENTER : align == 2 ? ES_RIGHT : ES_LEFT;
    HMODULE hmod = GetModuleHandleW(nullptr);
    HWND hwnd =
        CreateWindowExW(0, WC_EDITW, L"", style, rc.x, rc.y, rc.dx, rc.dy, win->hwndCanvas, nullptr, hmod, nullptr);
    if (!hwnd) {
        DeleteObject(font);
        return false;
    }
    // a themed edit paints its own border over the one we draw in WM_NCPAINT
    SetWindowTheme(hwnd, L"", L"");
    HwndSetFont(hwnd, font);
    int pad = 0; // EM_SETRECT supplies the complete content inset.
    SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad, pad));
    TempStr text = str::DupTemp((annot ? Contents(annot) : EbookAnnotationGetNote(ebookAnnot)));
    text = str::ReplaceTemp(text, "\r\n", "\n");
    text = str::ReplaceTemp(text, "\r", "\n");
    text = str::ReplaceTemp(text, "\n", "\r\n");
    HwndSetText(hwnd, text);

    gInPlaceDefProc = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProcFreeTextInPlaceEdit);

    LONG_PTR canvasStyle = GetWindowLongPtrW(win->hwndCanvas, GWL_STYLE);
    gInPlace.canvasClippedChildren = (canvasStyle & WS_CLIPCHILDREN) != 0;
    SetWindowLongPtrW(win->hwndCanvas, GWL_STYLE, canvasStyle | WS_CLIPCHILDREN);
    gInPlace.hwnd = hwnd;
    gInPlace.font = font;
    gInPlace.win = win;
    gInPlace.tab = win->CurrentTab();
    gInPlace.annot = annot;
    gInPlace.ebookAnnot = ebookAnnot;
    gInPlace.size = rc.Size();
    gInPlace.minSize = rc.Size();
    gInPlace.padding = pad;
    gInPlace.textSize = textSize;
    gInPlace.borderWidth = borderWidth;
    gInPlace.fontPx = fontPx;
    gInPlace.scale = scale;
    ApplyInPlaceColors(annot);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    HwndSetFocus(hwnd);
    // caret at the end, nothing selected: this is editing what is there, not
    // replacing it
    int end = (int)SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0);
    SendMessageW(hwnd, EM_SETSEL,
                 str::Eq((annot ? Contents(annot) : EbookAnnotationGetNote(ebookAnnot)), "This is a text...") ? 0 : end,
                 end);
    SizeInPlaceEditToText();
    return true;
}

// Edit the free text annotation under `pt`, if there is one and we are in
// Edit PDF mode.
bool StartFreeTextInPlaceEdit(MainWindow* win, Annotation* annot) {
    return StartFreeTextEdit(win, annot, nullptr);
}
bool StartEbookFreeTextInPlaceEdit(MainWindow* win, EbookAnnotation* annot) {
    if (!win || !EbookAnnotationsSupported(win->CurrentTab()) ||
        EbookAnnotationGetType(annot) != AnnotationType::FreeText)
        return false;
    win->CurrentTab()->selectedEbookAnnotation = annot;
    return StartFreeTextEdit(win, nullptr, annot);
}

bool StartFreeTextInPlaceEditAt(MainWindow* win, Point pt) {
    if (!win || !win->AsFixed()) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    DisplayModel* dm = win->AsFixed();
    if (!tab || !dm) {
        return false;
    }
    if (EbookAnnotationsSupported(tab)) {
        EbookAnnotation* ebook = EbookAnnotationsGetAt(tab, dm, pt);
        return ebook && StartEbookFreeTextInPlaceEdit(win, ebook);
    }
    if (!EngineSupportsAnnotations(dm->GetEngine())) return false;
    Annotation* annot = dm->GetAnnotationAtPos(pt, nullptr, true);
    if (!annot || Type(annot) != AnnotationType::FreeText) {
        return false;
    }
    SetSelectedAnnotation(tab, annot);
    return StartFreeTextInPlaceEdit(win, annot);
}

// WM_CTLCOLOREDIT for the in-place box: the annotation's text color on its own
// background. A transparent background uses a hollow brush. nullptr if `edit`
// isn't the box.
HBRUSH FreeTextInPlaceEditCtlColor(HWND edit, HDC hdc) {
    if (!gInPlace.hwnd || edit != gInPlace.hwnd) {
        return nullptr;
    }
    SetTextColor(hdc, gInPlace.textCol);
    if (gInPlace.bgTransparent) {
        SetBkMode(hdc, TRANSPARENT);
        return (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    }
    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, gInPlace.bgCol);
    if (gInPlace.bgBrush) {
        return gInPlace.bgBrush;
    }
    return (HBRUSH)GetStockObject(WHITE_BRUSH);
}

void EndFreeTextInPlaceEditForTab(WindowTab* tab, bool accept) {
    if (gFreeTextToolbarHwnd && IsWindow(gFreeTextToolbarHwnd)) {
        ShowWindow(gFreeTextToolbarHwnd, SW_HIDE);
    }
    if (gInPlace.tab == tab) {
        EndFreeTextInPlaceEdit(accept);
    }
}
bool IsFreeTextInPlaceEditFocused(HWND hwnd) {
    HWND focus = hwnd ? hwnd : GetFocus();
    if (focus && focus == gFreeTextToolbarHwnd) return true;
    return gInPlace.hwnd && ((hwnd && hwnd == gInPlace.hwnd) || GetFocus() == gInPlace.hwnd);
}

// Same square for text, background, and border. Background is a checkerboard
// when transparent and a solid fill otherwise, always with a black frame.
// Border is a hollow frame in the border color.
static Rect FreeTextSwatchSquare(HWND hwnd, Rect chip) {
    int side = DpiScale(hwnd, 13);
    int limit = std::min(chip.dx, chip.dy) - DpiScale(hwnd, 8);
    if (limit > 4) {
        side = std::min(side, limit);
    }
    Rect sw;
    sw.dx = side;
    sw.dy = side;
    sw.x = chip.x + (chip.dx - side) / 2;
    sw.y = chip.y + (chip.dy - side) / 2;
    return sw;
}

static COLORREF FreeTextSwatchInk(COLORREF col) {
    int lum = (GetRValue(col) * 299 + GetGValue(col) * 587 + GetBValue(col) * 114) / 1000;
    return lum > 150 ? RGB(60, 60, 60) : RGB(255, 255, 255);
}

static void FillSolidRect(HDC dc, Rect r, COLORREF col) {
    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(col));
    RECT rc = ToRECT(r);
    FillRect(dc, &rc, br);
}

static void FrameSolidRect(HDC dc, Rect r, COLORREF col) {
    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(col));
    RECT rc = ToRECT(r);
    FrameRect(dc, &rc, br);
}

static void DrawHollowFrame(HDC dc, Rect r, COLORREF col, int thick) {
    thick = std::clamp(thick, 1, std::max(1, std::min(r.dx, r.dy) / 2));
    FillSolidRect(dc, Rect(r.x, r.y, r.dx, thick), col);
    FillSolidRect(dc, Rect(r.x, r.y + r.dy - thick, r.dx, thick), col);
    FillSolidRect(dc, Rect(r.x, r.y, thick, r.dy), col);
    FillSolidRect(dc, Rect(r.x + r.dx - thick, r.y, thick, r.dy), col);
}

static void DrawCheckerboard(HDC dc, Rect r, int cell) {
    if (r.dx < 1 || r.dy < 1) {
        return;
    }
    if (cell < 2) {
        cell = 2;
    }
    FillSolidRect(dc, r, RGB(255, 255, 255));
    for (int y = r.y; y < r.y + r.dy; y += cell) {
        for (int x = r.x; x < r.x + r.dx; x += cell) {
            if ((((x - r.x) / cell) + ((y - r.y) / cell)) % 2 == 0) {
                continue;
            }
            int w = std::min(cell, r.x + r.dx - x);
            int h = std::min(cell, r.y + r.dy - y);
            FillSolidRect(dc, Rect(x, y, w, h), RGB(196, 196, 196));
        }
    }
}

// Three bars, shared edge on the alignment side. Lengths stay in the same
// left / center / right pattern; thickness and spacing are even.
static void DrawFreeTextAlignIcon(HDC dc, HWND hwnd, Rect chip, int align) {
    if (align < 0 || align > 2) {
        align = 0;
    }
    int bar = std::max(1, DpiScale(hwnd, 1));
    int pitch = std::max(bar + 2, DpiScale(hwnd, 4));
    int iconW = DpiScale(hwnd, 13);
    int groupH = bar + pitch * 2;
    int x0 = chip.x + (chip.dx - iconW) / 2;
    int y0 = chip.y + (chip.dy - groupH) / 2;
    const int eighths[3][3] = {
        {8, 5, 7},
        {6, 8, 6},
        {7, 5, 8},
    };
    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(FloatingPopupTextColor()));
    for (int line = 0; line < 3; line++) {
        int len = std::max(bar * 4, iconW * eighths[align][line] / 8);
        int x = x0;
        if (align == 1) {
            x = x0 + (iconW - len) / 2;
        } else if (align == 2) {
            x = x0 + iconW - len;
        }
        int y = y0 + line * pitch;
        RECT rc{x, y, x + len, y + bar};
        FillRect(dc, &rc, br);
    }
}

// Adapted from upstream AnnotEditToolbar.cpp: a compact contextual row, state
// read from the selected annotation, and page-relative placement. The upstream
// VirtHost framework is not present in this branch; use its native Wnd layer.
struct FreeTextPropertyToolbar : Wnd {
    MainWindow* win = nullptr;
    Rect chips[7];
    HWND tip = nullptr;
    int hot = -1;
    int pressed = -1;
    int focusChip = 0;
    WindowTab* fontTab = nullptr;
    Annotation* fontPdf = nullptr;
    EbookAnnotation* fontEbook = nullptr;
    bool Live(WindowTab** outTab, Annotation** pdf, EbookAnnotation** ebook) {
        if (!win || !IsMainWindowValid(win)) return false;
        WindowTab* tab = win->CurrentTab();
        if (!tab) return false;
        *outTab = tab;
        *pdf = tab->selectedAnnotation;
        *ebook = tab->selectedEbookAnnotation;
        if (*pdf && (*pdf)->pdfannot && Type(*pdf) == AnnotationType::FreeText &&
            EngineSupportsAnnotations(tab->GetEngine())) {
            *ebook = nullptr;
            return true;
        }
        *pdf = nullptr;
        return *ebook && EbookAnnotationGetType(*ebook) == AnnotationType::FreeText;
    }
    void Changed() {
        WindowTab* tab;
        Annotation* pdf;
        EbookAnnotation* ebook;
        if (!Live(&tab, &pdf, &ebook)) return;
        if (pdf) {
            RefreshInPlaceFreeTextStyle(pdf);
            SyncInPlaceFreeTextBorder(pdf, BorderWidth(pdf));
            EditAnnotationsWindow* ew = tab->editAnnotsWindow;
            if (ew) {
                ew->updatingControls = true;
                DoTextFont(ew, pdf);
                DoTextSize(ew, pdf);
                DoTextColor(ew, pdf);
                DoBorderColor(ew, pdf);
                DoTextAlignment(ew, pdf);
                DoBorder(ew, pdf);
                DoColor(ew, pdf);
                ew->updatingControls = false;
                EnableSaveIfAnnotationsChanged(ew);
            }
            RememberPdfDrawStyle(pdf);
            MainWindowRerenderAnnotationChange(win, PageNo(pdf), pdf);
        } else {
            RememberEbookDrawStyle(ebook);
            RefreshInPlaceFreeTextStyle(nullptr);
            if (tab->editEbookAnnotsWindow) UpdateEbookAnnotationsList(tab->editEbookAnnotsWindow, ebook);
            MainWindowRerender(win);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    static void FontPicked(const char* family, void* ctx) {
        auto self = (FreeTextPropertyToolbar*)ctx;
        WindowTab* tab;
        Annotation* pdf;
        EbookAnnotation* ebook;
        if (!self->Live(&tab, &pdf, &ebook) || tab != self->fontTab || pdf != self->fontPdf || ebook != self->fontEbook)
            return;
        if (pdf) {
            SetDefaultAppearanceTextFont(pdf, family);
            RememberFreeTextPreset(family, 0, 0, 0);
        } else
            EbookAnnotationSetFreeTextFont(tab, ebook, family);
        self->Changed();
    }
    void Pick(int idx) {
        WindowTab* tab;
        Annotation* pdf;
        EbookAnnotation* ebook;
        if (!Live(&tab, &pdf, &ebook)) return;
        if (idx == 0) {
            fontTab = tab;
            fontPdf = pdf;
            fontEbook = ebook;
            gInPlace.suspendClose = true;
            ShowFreeTextFontPicker(hwnd, pdf ? DefaultAppearanceTextFont(pdf) : EbookAnnotationGetFreeTextFont(ebook),
                                   FontPicked, this);
            gInPlace.suspendClose = false;
            return;
        }
        HMENU menu = CreatePopupMenu();
        int selected = -1;
        const int sizes[] = {8, 10, 12, 14, 16, 18, 21, 24, 28, 32, 36, 48, 72};
        const COLORREF colors[] = {RGB(0, 0, 0),      RGB(255, 255, 255), RGB(200, 40, 40),
                                   RGB(40, 100, 200), RGB(40, 140, 70),   RGB(255, 240, 160)};
        if (idx == 1) {
            int value = pdf ? DefaultAppearanceTextSize(pdf) : EbookAnnotationGetFreeTextSize(ebook);
            for (int i = 0; i < dimof(sizes); i++) {
                AppendMenuW(menu, MF_STRING | (value == sizes[i] ? MF_CHECKED : 0), i + 1,
                            ToWStrTemp(str::FormatTemp("%d", sizes[i])));
            }
        } else if (idx == 2 || idx == 3 || idx == 4) {
            PdfColor color = kColorUnset;
            if (pdf) {
                color = idx == 2   ? DefaultAppearanceTextColor(pdf)
                        : idx == 3 ? GetColor(pdf)
                                   : FreeTextBorderColor(pdf);
            }
            COLORREF value = ColorRefFromPdfAnnotationColor(color);
            bool transparent = pdf && idx == 3 && color == 0;
            if (ebook && idx == 2) {
                value = EbookAnnotationGetColor(ebook);
            }
            if (ebook && idx == 3) {
                transparent = !EbookAnnotationGetFreeTextBackground(ebook, &value);
            }
            if (ebook && idx == 4) {
                value = EbookAnnotationGetFreeTextBorderColor(ebook);
            }
            const char* names[] = {_TRA("Black"), _TRA("White"), _TRA("Red"),
                                   _TRA("Blue"),  _TRA("Green"), _TRA("Yellow")};
            for (int i = 0; i < dimof(colors); i++)
                AppendMenuW(menu, MF_STRING | (!transparent && value == colors[i] ? MF_CHECKED : 0), i + 1,
                            ToWStrTemp(names[i]));
            if (idx == 3)
                AppendMenuW(menu, MF_STRING | (transparent ? MF_CHECKED : 0), 100, ToWStrTemp(_TRA("Transparent")));
        } else if (idx == 5) {
            selected = pdf ? BorderWidth(pdf) : EbookAnnotationGetFreeTextBorderWidth(ebook);
            for (int i = 0; i <= 12; i++)
                AppendMenuW(menu, MF_STRING | (selected == i ? MF_CHECKED : 0), i + 1,
                            ToWStrTemp(str::FormatTemp("%d", i)));
        } else {
            selected = pdf ? Quadding(pdf) : EbookAnnotationGetFreeTextAlignment(ebook);
            const char* names[] = {_TRA("Left"), _TRA("Center"), _TRA("Right")};
            for (int i = 0; i < 3; i++)
                AppendMenuW(menu, MF_STRING | (selected == i ? MF_CHECKED : 0), i + 1, ToWStrTemp(names[i]));
        }
        Rect r = chips[idx];
        POINT pt{r.x, r.y + r.dy};
        ClientToScreen(hwnd, &pt);
        auto expectedTab = tab;
        auto expectedPdf = pdf;
        auto expectedEbook = ebook;
        gInPlace.suspendClose = true;
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
        gInPlace.suspendClose = false;
        DestroyMenu(menu);
        if (!cmd || !Live(&tab, &pdf, &ebook) || tab != expectedTab || pdf != expectedPdf || ebook != expectedEbook)
            return;
        if (idx == 1) {
            if (pdf)
                SetDefaultAppearanceTextSize(pdf, sizes[cmd - 1]);
            else
                EbookAnnotationSetFreeTextSize(tab, ebook, sizes[cmd - 1]);
        } else if (idx == 2) {
            if (pdf)
                SetDefaultAppearanceTextColor(pdf, PdfAnnotationColorFromColorRef(colors[cmd - 1]));
            else
                EbookAnnotationSetColor(tab, ebook, colors[cmd - 1]);
        } else if (idx == 3) {
            if (pdf)
                SetColor(pdf, cmd == 100 ? 0 : PdfAnnotationColorFromColorRef(colors[cmd - 1]));
            else
                EbookAnnotationSetFreeTextBackground(tab, ebook, cmd == 100, cmd == 100 ? 0 : colors[cmd - 1]);
        } else if (idx == 4) {
            if (pdf)
                SetFreeTextBorderColor(pdf, PdfAnnotationColorFromColorRef(colors[cmd - 1]));
            else
                EbookAnnotationSetFreeTextBorderColor(tab, ebook, colors[cmd - 1]);
        } else if (idx == 5) {
            if (pdf)
                SetBorderWidth(pdf, cmd - 1);
            else
                EbookAnnotationSetFreeTextBorderWidth(tab, ebook, cmd - 1);
        } else {
            if (pdf)
                SetQuadding(pdf, cmd - 1);
            else
                EbookAnnotationSetFreeTextAlignment(tab, ebook, cmd - 1);
        }
        Changed();
    }
    static COLORREF ChipColor(Annotation* pdf, EbookAnnotation* ebook, int idx, bool* transparent) {
        *transparent = false;
        if (idx == 2) {
            if (pdf) {
                return ColorRefFromPdfColor(DefaultAppearanceTextColor(pdf));
            }
            return ebook ? EbookAnnotationGetColor(ebook) : RGB(0, 0, 0);
        }
        if (idx == 3) {
            if (pdf) {
                PdfColor c = GetColor(pdf);
                *transparent = c == 0;
                return *transparent ? RGB(255, 255, 255) : ColorRefFromPdfColor(c);
            }
            COLORREF bg = 0;
            *transparent = !ebook || !EbookAnnotationGetFreeTextBackground(ebook, &bg);
            return bg;
        }
        if (pdf) {
            return ColorRefFromPdfColor(FreeTextBorderColor(pdf));
        }
        return ebook ? EbookAnnotationGetFreeTextBorderColor(ebook) : RGB(0, 0, 0);
    }
    void OnPaint(HDC dc, PAINTSTRUCT*) override {
        WindowTab* tab;
        Annotation* pdf;
        EbookAnnotation* ebook;
        if (!Live(&tab, &pdf, &ebook)) return;
        Rect client = ClientRect(hwnd);
        int radius = DpiScale(hwnd, 10);
        int btnRadius = DpiScale(hwnd, 6);
        COLORREF bgCol = FloatingPopupBg();
        FillFloatingPopupRoundedRect(dc, client, radius, bgCol);
        StrokeFloatingPopupRoundedRect(dc, client, radius, FloatingPopupBorderColor());
        SetBkMode(dc, TRANSPARENT);
        HFONT font = GetAppFontForDpi(DpiGet(hwnd));
        HFONT oldFont = (HFONT)SelectObject(dc, font);
        const char* face =
            FreeTextFontLabel(pdf ? DefaultAppearanceTextFont(pdf) : EbookAnnotationGetFreeTextFont(ebook));
        const char* sizeLabel =
            str::FormatTemp("%d", pdf ? DefaultAppearanceTextSize(pdf) : EbookAnnotationGetFreeTextSize(ebook));
        int align = pdf ? Quadding(pdf) : EbookAnnotationGetFreeTextAlignment(ebook);
        int borderW = pdf ? BorderWidth(pdf) : EbookAnnotationGetFreeTextBorderWidth(ebook);
        for (int i = 0; i < 7; i++) {
            Rect chip = chips[i];
            if (chip.IsEmpty()) {
                continue;
            }
            if (i == pressed) {
                FillFloatingPopupRoundedRect(dc, chip, btnRadius, FloatingToolButtonPressedBg());
            } else if (i == hot) {
                FillFloatingPopupRoundedRect(dc, chip, btnRadius, FloatingPopupHoverBg(bgCol));
            }
            if (i == 0 || i == 1 || i == 5) {
                SetTextColor(dc, FloatingPopupTextColor());
                const char* label = face;
                if (i == 1) {
                    label = sizeLabel;
                } else if (i == 5) {
                    label = str::FormatTemp("%d", borderW);
                }
                DrawCenteredText(dc, chip, label);
            } else if (i == 2 || i == 3 || i == 4) {
                bool transparent = false;
                COLORREF col = ChipColor(pdf, ebook, i, &transparent);
                Rect sw = FreeTextSwatchSquare(hwnd, chip);
                if (i == 4) {
                    int thick = std::max(2, DpiScale(hwnd, 2));
                    DrawHollowFrame(dc, sw, col, thick);
                    if (FreeTextSwatchInk(col) == RGB(60, 60, 60)) {
                        FrameSolidRect(dc, sw, FloatingPopupBorderColor());
                    }
                } else if (i == 3) {
                    Rect inner = sw;
                    inner.x += 1;
                    inner.y += 1;
                    inner.dx -= 2;
                    inner.dy -= 2;
                    if (inner.dx < 2 || inner.dy < 2) {
                        inner = sw;
                    }
                    if (transparent) {
                        DrawCheckerboard(dc, inner, std::max(2, DpiScale(hwnd, 3)));
                    } else {
                        FillSolidRect(dc, inner, col);
                    }
                    FrameSolidRect(dc, sw, RGB(0, 0, 0));
                } else {
                    if (!transparent) {
                        FillSolidRect(dc, sw, col);
                    }
                    FrameSolidRect(dc, sw, FloatingPopupBorderColor());
                }
            } else {
                DrawFreeTextAlignIcon(dc, hwnd, chip, align);
            }
            if (GetFocus() == hwnd && focusChip == i) {
                RECT fr = ToRECT(chip);
                DrawFocusRect(dc, &fr);
            }
        }
        SelectObject(dc, oldFont);
    }
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
        if (msg == WM_KEYDOWN) {
            if (wp == VK_LEFT || (wp == VK_TAB && IsShiftPressed()))
                focusChip = (focusChip + 6) % 7;
            else if (wp == VK_RIGHT || wp == VK_TAB)
                focusChip = (focusChip + 1) % 7;
            else if (wp == VK_HOME)
                focusChip = 0;
            else if (wp == VK_END)
                focusChip = 6;
            else if (wp == VK_RETURN || wp == VK_SPACE)
                Pick(focusChip);
            else if (wp == VK_F6 || wp == VK_ESCAPE)
                HwndSetFocus(gInPlace.hwnd ? gInPlace.hwnd : win->hwndCanvas);
            else
                return 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) pressed = -1;
        if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
            Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            for (int i = 0; i < 7; i++)
                if (chips[i].Contains(pt)) idx = i;
            if (msg == WM_LBUTTONDOWN) {
                pressed = idx;
                if (idx >= 0) SetCapture(hwnd);
                return 0;
            }
            if (msg == WM_LBUTTONUP) {
                int wasPressed = pressed;
                pressed = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (idx >= 0 && idx == wasPressed) Pick(idx);
                return 0;
            }
            if (hot != idx) {
                hot = idx;
                if (!tip && hwnd) {
                    tip = CreateWindowExW(
                        WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
                    if (tip) {
                        TOOLINFOW info{};
                        info.cbSize = sizeof(info);
                        info.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                        info.hwnd = hwnd;
                        info.uId = (UINT_PTR)hwnd;
                        info.lpszText = const_cast<WCHAR*>(L"");
                        SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&info);
                    }
                }
                if (tip) {
                    WindowTab* tipTab = nullptr;
                    Annotation* tipPdf = nullptr;
                    EbookAnnotation* tipEbook = nullptr;
                    const char* text = "";
                    if (idx >= 0 && Live(&tipTab, &tipPdf, &tipEbook)) {
                        const char* tips[] = {_TRA("Text Font:"),        _TRA("Text Size:"),    _TRA("Text Color:"),
                                              _TRA("Background Color:"), _TRA("Border Color:"), _TRA("Border:"),
                                              _TRA("Text Alignment:")};
                        if (idx == 0) {
                            text = FreeTextFontLabel(tipPdf ? DefaultAppearanceTextFont(tipPdf)
                                                            : EbookAnnotationGetFreeTextFont(tipEbook));
                        } else {
                            text = tips[idx];
                        }
                    }
                    TOOLINFOW info{};
                    info.cbSize = sizeof(info);
                    info.hwnd = hwnd;
                    info.uId = (UINT_PTR)hwnd;
                    info.lpszText = (WCHAR*)ToWStrTemp(text);
                    SendMessageW(tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&info);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
        } else if (msg == WM_MOUSELEAVE) {
            hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return Wnd::WndProc(hwnd, msg, wp, lp);
    }
};
static FreeTextPropertyToolbar gFreeTextProperties;
static void UpdateFreeTextPropertyToolbar(MainWindow* win) {
    auto tb = &gFreeTextProperties;
    if (tb->hwnd && (!IsWindow(tb->hwnd) || tb->win != win)) {
        if (tb->tip) {
            DestroyWindow(tb->tip);
            tb->tip = nullptr;
        }
        if (IsWindow(tb->hwnd)) DestroyWindow(tb->hwnd);
        tb->hwnd = nullptr;
    }
    tb->win = win;
    WindowTab* tab;
    Annotation* pdf;
    EbookAnnotation* ebook;
    int pageNo = 0;
    RectF bounds;
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    bool valid = dm && tb->Live(&tab, &pdf, &ebook);
    if (valid && pdf) {
        pageNo = PageNo(pdf);
        bounds = GetRect(pdf);
    } else if (valid)
        valid = EbookAnnotationGetPageBounds(tab, dm, ebook, &pageNo, &bounds);
    if (!valid || !dm->PageVisible(pageNo) || win->mouseAction != MouseAction::None) {
        if (tb->hwnd) ShowWindow(tb->hwnd, SW_HIDE);
        return;
    }
    if (!tb->hwnd) {
        CreateCustomArgs args;
        args.parent = win->hwndCanvas;
        args.style = WS_CHILD | WS_TABSTOP;
        args.exStyle = WS_EX_NOACTIVATE;
        if (!tb->CreateCustom(args)) return;
        gFreeTextToolbarHwnd = tb->hwnd;
    }
    Rect canvas = ClientRect(win->hwndCanvas);
    Rect annot = dm->CvtToScreen(pageNo, bounds);
    int chipGap = DpiScale(tb->hwnd, 2);
    int annotGap = DpiScale(tb->hwnd, 10);
    int margin = DpiScale(tb->hwnd, 4);
    int h = DpiScale(tb->hwnd, 28);
    HFONT chipFont = GetAppFontForDpi(DpiGet(tb->hwnd));
    const char* face = FreeTextFontLabel(pdf ? DefaultAppearanceTextFont(pdf) : EbookAnnotationGetFreeTextFont(ebook));
    const char* sizeLabel =
        str::FormatTemp("%d", pdf ? DefaultAppearanceTextSize(pdf) : EbookAnnotationGetFreeTextSize(ebook));
    int fontW = HwndMeasureText(tb->hwnd, face, chipFont).dx + DpiScale(tb->hwnd, 12);
    fontW = std::clamp(fontW, DpiScale(tb->hwnd, 64), DpiScale(tb->hwnd, 160));
    int sizeW = HwndMeasureText(tb->hwnd, sizeLabel, chipFont).dx + DpiScale(tb->hwnd, 10);
    sizeW = std::max(sizeW, DpiScale(tb->hwnd, 26));
    const char* borderLabel =
        str::FormatTemp("%d", pdf ? BorderWidth(pdf) : EbookAnnotationGetFreeTextBorderWidth(ebook));
    int borderNumW = HwndMeasureText(tb->hwnd, borderLabel, chipFont).dx + DpiScale(tb->hwnd, 10);
    borderNumW = std::max(borderNumW, sizeW);
    int swatch = DpiScale(tb->hwnd, 20);
    const int widths[] = {fontW, sizeW, swatch, swatch, swatch, borderNumW, swatch};
    int x = margin, y = margin, rowW = 0, maxW = 0;
    for (int i = 0; i < 7; i++) {
        int w = std::min(widths[i], std::max(1, canvas.dx - 2 * margin));
        if (x + w + margin > canvas.dx && x > margin) {
            maxW = std::max(maxW, x);
            x = margin;
            y += h + chipGap;
        }
        tb->chips[i] = Rect(x, y, w, h);
        x += w + chipGap;
        rowW = x;
    }
    int width = std::max(maxW, rowW - chipGap) + margin;
    int height = y + h + margin;
    x = std::clamp(annot.x, 0, std::max(0, canvas.dx - width));
    int above = annot.y - annotGap - height;
    int below = annot.y + annot.dy + annotGap;
    y = above >= 0 ? above : below;
    if (y + height > canvas.dy) {
        y = std::clamp(above >= 0 ? above : 0, 0, std::max(0, canvas.dy - height));
    }
    Rect current = ChildPosWithinParent(tb->hwnd);
    Rect wanted(x, y, width, height);
    if (current != wanted) {
        SetWindowPos(tb->hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
        UpdateFloatingPopupWindowRgn(tb->hwnd, DpiScale(tb->hwnd, 10));
    }
    if (!IsWindowVisible(tb->hwnd)) {
        ShowWindow(tb->hwnd, SW_SHOWNOACTIVATE);
    }
    InvalidateRect(tb->hwnd, nullptr, FALSE);
}
