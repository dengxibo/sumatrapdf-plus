/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "utils/BaseUtil.h"
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
#include "EngineBase.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "Translations.h"
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
#include "Selection.h"
#include "RenderCache.h"

#include "utils/Log.h"

#include "theme.h"
#include "AppDialogTheme.h"
#include "AnnotationSidebar.h"

extern RenderCache* gRenderCache;

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

void RemovePdfMarkupOverlayAnnot(WindowTab* tab, Annotation* annot) {
    if (!tab || !annot) {
        return;
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
    for (int i = tab->pdfMarkupOverlays.size() - 1; i >= 0; i--) {
        if (tab->pdfMarkupOverlays.at(i).pageNo == pageNo) {
            tab->pdfMarkupOverlays.RemoveAt(i);
        }
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
    if (type == AnnotationType::Circle) {
        gs.DrawEllipse(&pen, screen.x, screen.y, screen.dx, screen.dy);
    } else {
        gs.DrawRectangle(&pen, screen.x, screen.y, screen.dx, screen.dy);
    }
}

static COLORREF DeletedAnnotCoverColor();

static void PaintPdfFreeTextOverlay(HDC hdc, DisplayModel* dm, int pageNo, Annotation* annot) {
    Rect screen = dm->CvtToScreen(pageNo, GetBounds(annot));
    if (screen.dx < 2 || screen.dy < 2) {
        return;
    }
    PdfColor fill = InteriorColor(annot);
    COLORREF bg = fill == 0 ? DeletedAnnotCoverColor() : ColorRefFromPdfColor(fill);
    PdfColor textCol = GetColor(annot);
    COLORREF fg = textCol == 0 ? RGB(0, 0, 0) : ColorRefFromPdfColor(textCol);
    RECT rc = ToRECT(screen);
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);

    float border = BorderWidthF(annot);
    int penW = 0;
    if (border > 0.1f) {
        Rect bw = dm->CvtToScreen(pageNo, RectF(0, 0, border, border));
        penW = bw.dy > 0 ? bw.dy : 1;
        HPEN pen = CreatePen(PS_SOLID, penW, fg);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
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
    const char* fontPdf = DefaultAppearanceTextFont(annot);
    const WCHAR* face = L"Arial";
    if (str::Eq(fontPdf, "TiRo")) {
        face = L"Times New Roman";
    } else if (str::Eq(fontPdf, "Cour")) {
        face = L"Courier New";
    }
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
    if (cover->fillInterior) {
        Gdiplus::SolidBrush brush(color);
        if (type == AnnotationType::Circle) {
            gs.FillEllipse(&brush, screen.x, screen.y, screen.dx, screen.dy);
        } else {
            gs.FillRectangle(&brush, screen.x, screen.y, screen.dx, screen.dy);
        }
    }
    if (type == AnnotationType::Circle) {
        gs.DrawEllipse(&pen, screen.x, screen.y, screen.dx, screen.dy);
    } else {
        gs.DrawRectangle(&pen, screen.x, screen.y, screen.dx, screen.dy);
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
static const char *gFontNames = "Cour\0Helv\0TiRo\0";
static const char *gFontReadableNames = "Courier\0Helvetica\0TimesRoman\0";
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
// DarkModeLib paints trackbar channel/thumb via NM_CUSTOMDRAW but the default
// PREPAINT still floods the client with white. Fill the page color and skip
// that default; item draw still reaches DarkMode through DefSubclassProc.
constexpr UINT_PTR kAnnotTrackbarBgNotifyId = 0xA11E;

static LRESULT CALLBACK AnnotTrackbarBgNotifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, AnnotTrackbarBgNotifyProc, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_NOTIFY && ThemeUsesDarkChrome()) {
        auto* hdr = (NMHDR*)lp;
        if (hdr && hdr->code == NM_CUSTOMDRAW && hdr->hwndFrom) {
            WCHAR cls[64]{};
            if (GetClassNameW(hdr->hwndFrom, cls, dimof(cls)) > 0 && str::EqI(cls, TRACKBAR_CLASS)) {
                auto* cd = (LPNMCUSTOMDRAW)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    RECT rc{};
                    GetClientRect(hdr->hwndFrom, &rc);
                    ScopedGdiObj<HBRUSH> br(CreateSolidBrush(ThemeWindowBackgroundColor()));
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
    DropDown* dropDownTextFont = nullptr;
    Static* staticTextSize = nullptr;
    Trackbar* trackbarTextSize = nullptr;
    Static* staticTextColor = nullptr;
    DropDown* dropDownTextColor = nullptr;

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
    int annotationTypeWidth = 0;

    bool skipGoToPage = false;
    bool updatingControls = false;
    int dpi = 0;

    StrBuilder currTextColor;
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
    ew->dropDownTextFont->SetIsVisible(false);
    ew->staticTextSize->SetIsVisible(false);
    ew->trackbarTextSize->SetIsVisible(false);
    ew->staticTextColor->SetIsVisible(false);
    ew->dropDownTextColor->SetIsVisible(false);

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
    UpdateAnnotationContentsEditChrome(ew->editContents);
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
                // Outermost: fill trackbar client before DarkMode item draw.
                RemoveWindowSubclass(ew->inspectorPane->hwnd, AnnotTrackbarBgNotifyProc, kAnnotTrackbarBgNotifyId);
                SetWindowSubclass(ew->inspectorPane->hwnd, AnnotTrackbarBgNotifyProc, kAnnotTrackbarBgNotifyId, 0);
            }
        } else if (installDarkMode) {
            DarkMode::setDarkWndNotifySafe(ew->hwnd);
            DarkMode::setWindowEraseBgSubclass(ew->hwnd);
        } else {
            DarkMode::setWindowCtlColorSubclass(ew->hwnd);
            DarkMode::setChildCtrlsTheme(ew->hwnd);
        }
    }
    if (ew->inspectorPane && ew->inspectorPane->hwnd && !ThemeUsesDarkChrome()) {
        RemoveWindowSubclass(ew->inspectorPane->hwnd, AnnotTrackbarBgNotifyProc, kAnnotTrackbarBgNotifyId);
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

static void CacheAnnotationExcerpts(EditAnnotationsWindow* ew) {
    ew->annotationExcerpts.Reset();
    ew->annotationTypeWidth = DpiScale(ew->hwnd, 80);
    HFONT font = (HFONT)SendMessageW(ew->listBox->hwnd, WM_GETFONT, 0, 0);
    DisplayModel* dm = ew->tab->win->AsFixed();
    Vec<int> nativeTextPages;
    Vec<int> emptyTextPages;
    for (Annotation* annot : ew->annotations) {
        TempStr name = AnnotationReadableNameTemp(annot->type);
        ew->annotationTypeWidth = std::max(ew->annotationTypeWidth, HwndMeasureText(ew->listBox->hwnd, name, font).dx);
        // MarkupTextTemp uses page text and QuadPoints, never annotation Contents.
        TempStr excerpt = MarkupTextTemp(annot);
        if ((annot->type == AnnotationType::Square || annot->type == AnnotationType::Circle) && dm &&
            dm->GetEngine() == annot->engine) {
            // Regional drawings need actual native page text. Do not initiate
            // OCR or borrow nearby text/Contents when the region is an image.
            // Check each page once per rebuild, outside the list paint path.
            if (!nativeTextPages.Contains(annot->pageNo) && !emptyTextPages.Contains(annot->pageNo)) {
                PageTextUtf8 text = annot->engine->ExtractPageTextUtf8(annot->pageNo);
                bool hasText = !str::IsEmptyOrWhiteSpace(text.text);
                FreePageTextUtf8(&text);
                (hasText ? nativeTextPages : emptyTextPages).Append(annot->pageNo);
            }
            if (nativeTextPages.Contains(annot->pageNo)) {
                // Reuse selection's overlap tolerance and reading-order/line
                // merging. No expansion of the annotation's actual rectangle.
                char* regionText = dm->GetTextInRegion(annot->pageNo, GetRect(annot), true);
                excerpt = str::DupTemp(regionText);
                str::Free(regionText);
            }
        }
        if (excerpt) {
            str::NormalizeWSInPlace(excerpt);
        }
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
        TempStr name = AnnotationReadableNameTemp(annot->type);
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

static void AppendMarkdownBlockquote(StrBuilder& out, const char* text) {
    if (str::IsEmpty(text)) {
        return;
    }
    const u8* p = (const u8*)text;
    bool lineStart = true;
    while (*p) {
        u8 c = *p++;
        if (lineStart) {
            out.Append("> ");
            lineStart = false;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out.AppendChar('\n');
            lineStart = true;
            continue;
        }
        out.AppendChar((char)c);
    }
    if (!lineStart) {
        out.AppendChar('\n');
    }
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
    out.AppendFmt("%s: %s\n", _TRA("Source"), tab->filePath);
    out.Append(_TRA("Exported:"));
    out.Append(" ");
    AppendUtcDateTime(out, time(nullptr));
    out.Append("\n\n---\n\n");

    for (const PdfAnnotationSortItem& item : items) {
        Annotation* annotation = item.annotation;
        TempStr typeName = AnnotationReadableNameTemp(Type(annotation));
        out.AppendFmt("## %s %d — %s\n\n", _TRA("Page"), item.pageNo, typeName);

        TempStr excerpt = MarkupTextTemp(annotation);
        if (!str::IsEmpty(excerpt)) {
            AppendMarkdownBlockquote(out, excerpt);
            out.AppendChar('\n');
        }

        TempStr note = Contents(annotation);
        if (!str::IsEmpty(note)) {
            out.AppendFmt("**%s**\n\n", _TRA("Note:"));
            out.Append(note);
            out.Append("\n\n");
        }

        const char* author = Author(annotation);
        if (!str::IsEmpty(author)) {
            out.AppendFmt("%s: %s\n", _TRA("Author"), author);
        }
        time_t date = ModificationDate(annotation);
        if (date > 0) {
            out.Append(_TRA("Date:"));
            out.Append(" ");
            AppendUtcDateTime(out, date);
            out.AppendChar('\n');
        }
        out.Append("\n---\n\n");
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

constexpr int kMaxControls = 24;

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
    addIfVisible(ew->dropDownTextAlignment->hwnd);
    addIfVisible(ew->dropDownTextFont->hwnd);
    addIfVisible(ew->trackbarTextSize->hwnd);
    addIfVisible(ew->dropDownTextColor->hwnd);
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

static void RelayoutEditAnnotations(EditAnnotationsWindow* ew) {
    if (!ew || !ew->mainLayout || !ew->hwnd) {
        return;
    }
    Rect rc = ClientRect(ew->hwnd);
    if (rc.dx <= 0 || rc.dy <= 0) {
        return;
    }
    LayoutToSize(ew->mainLayout, {rc.dx, rc.dy});
    if (ew->inspectorPane) {
        ew->inspectorPane->RelayoutInner();
    }
    // Collapsing inspector fields changes visibility via window styles. Erase
    // their old pixels as well as repainting the controls at their new positions.
    RedrawWindow(ew->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void SyncAnnotHeadingColumns(EditAnnotationsWindow* ew) {
    if (!ew || !ew->staticHeading) {
        return;
    }
    int col = ew->staticHeading->MeasureTitlePx();
    if (ew->staticMeta && ew->staticMeta->IsVisible()) {
        col = std::max(col, ew->staticMeta->MeasureTitlePx());
    }
    ew->staticHeading->titleColW = col;
    if (ew->staticMeta) {
        ew->staticMeta->titleColW = col;
        if (ew->staticMeta->hwnd) {
            InvalidateRect(ew->staticMeta->hwnd, nullptr, TRUE);
        }
    }
    if (ew->staticHeading->hwnd) {
        InvalidateRect(ew->staticHeading->hwnd, nullptr, TRUE);
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
    return AnnotationReadableNameTemp(annot->type);
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

static void TextAlignmentSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextAlignment->GetCurrentSelection();
    int newQuadding = idx;
    SetQuadding(annot, newQuadding);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

static void DoTextFont(EditAnnotationsWindow* ew, Annotation* annot) {
    if (Type(annot) != AnnotationType::FreeText) {
        return;
    }
    const char* fontName = DefaultAppearanceTextFont(annot);
    // TODO: might have other fonts, like "Symb" and "ZaDb"
    auto itemNo = seqstrings::StrToIdx(gFontNames, fontName);
    if (itemNo < 0) {
        return;
    }
    ew->dropDownTextFont->SetItemsSeqStrings(gFontReadableNames);
    ew->dropDownTextFont->SetCurrentSelection(itemNo);
    ew->staticTextFont->SetIsVisible(true);
    ew->dropDownTextFont->SetIsVisible(true);
}

static void TextFontSelectionChanged(EditAnnotationsWindow* ew) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextFont->GetCurrentSelection();
    const char* font = seqstrings::IdxToStr(gFontNames, idx);
    SetDefaultAppearanceTextFont(annot, font);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

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

static void TextFontSizeChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int fontSize = ev->pos;
    SetDefaultAppearanceTextSize(annot, fontSize);
    TempStr s = str::FormatTemp(_TRA("Text Size: %d"), fontSize);
    ew->staticTextSize->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
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
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownTextColor->GetCurrentSelection();
    char* item = ew->dropDownTextColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetDefaultAppearanceTextColor(annot, col);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
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

static void BorderWidthChanging(EditAnnotationsWindow* ew, Trackbar::PositionChangingEvent* ev) {
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    int borderWidth = ev->pos;
    SetBorderWidth(annot, borderWidth);
    TempStr s = str::FormatTemp(_TRA("Border: %d"), borderWidth);
    ew->staticBorder->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
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
    auto annot = ew->tab->selectedAnnotation;
    if (!annot || !annot->engine) {
        return;
    }
    auto idx = ew->dropDownColor->GetCurrentSelection();
    auto item = ew->dropDownColor->items.At(idx);
    auto col = GetDropDownColor(item);
    SetColor(annot, col);
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
    TempStr s = str::FormatTemp(_TRA("Opacity: %d"), opacity);
    ew->staticOpacity->SetText(s);
    EnableSaveIfAnnotationsChanged(ew);
    RerenderPdfAnnotationChange(ew->tab, nullptr);
}

// TODO: maybe use ew->tab->selectedAnnotation instead of annot
static void UpdateUIForSelectedAnnotation(EditAnnotationsWindow* ew, Annotation* annot, bool isNew = false,
                                          EditAnnotFocus focus = EditAnnotFocus::Default) {
    HidePerAnnotControls(ew);
    if (annot) {
        int itemNo = ew->annotations.Find(annot);
        if (itemNo < 0) {
            // can happen if annotations list is out of sync (e.g. after reload)
            return;
        }

        DoAuthor(ew, annot);
        DoModificationDate(ew, annot);
        DoContents(ew, annot);

        DoTextAlignment(ew, annot);
        DoTextFont(ew, annot);
        DoTextSize(ew, annot);
        DoTextColor(ew, annot);

        DoLineStartEnd(ew, annot);

        DoIcon(ew, annot);

        DoBorder(ew, annot);
        DoColor(ew, annot);
        DoInteriorColor(ew, annot);

        DoOpacity(ew, annot);
        DoSaveEmbed(ew, annot);

        ew->staticHeading->SetParts(AnnotationHeadingTemp(annot), AnnotationBoundsTemp(annot));
        ew->staticHeading->SetIsVisible(true);
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

    // Outer size is often unchanged when switching annotation types, so the
    // inspector pane gets no WM_SIZE. Relayout its children explicitly or the
    // newly shown Line/Ink fields stay piled at their create-time positions.
    RelayoutEditAnnotations(ew);

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
        HwndMakeVisible(ew->hwnd);
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
    EnableSaveIfAnnotationsChanged(ew);

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
    const char* excerpt = ev->itemIndex < ew->annotationExcerpts.Size() ? ew->annotationExcerpts.At(ev->itemIndex) : "";
    DrawAnnotationSidebarRow(ew->hwnd, ew->listBox->hwnd, ev, ev->selected || annot == ew->tab->selectedAnnotation,
                             color != 0, ColorRefFromPdfColor(color), str::FormatTemp("%d", annot->pageNo),
                             AnnotationReadableNameTemp(annot->type), excerpt, ew->annotationTypeWidth);
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
        auto w = CreateStatic(parent, fnt, _TRA("Text Alignment:"));
        w->SetInsetsPt(8, 0, 0, 0);
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
        DropDown::CreateArgs args;
        args.parent = parent;
        args.font = fnt;
        args.isRtl = IsUIRtl();
        auto w = new DropDown();
        w->SetInsetsPt(4, 0, 0, 0);

        w->Create(args);
        w->SetItemsSeqStrings(gQuaddingNames);
        w->onSelectionChanged = MkFunc0(TextFontSelectionChanged, ew);
        ew->dropDownTextFont = w;
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
    RevealAnnotationsSidebar(tab, revealInSidebar);
}
