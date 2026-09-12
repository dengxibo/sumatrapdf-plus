/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include <gdiplus.h>

#include "utils/GdiPlusUtil.h"
#include "wingui/UIModels.h"
#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "PrintedTocModel.h"
#include "PrintedTocOverlay.h"

// Colors match the PrintedTocModel.h contract: green titles, blue anchors,
// gray columns, level ticks in orange. Alpha keeps the page readable under
// the overlay.
using Gdiplus::ARGB;

static const ARGB kTitleColor = 0xC000B000;  // green
static const ARGB kAnchorColor = 0xC00050C8; // blue
static const ARGB kColumnColor = 0x90606060; // gray, dashed
static const ARGB kIndentColor = 0x90C06000; // orange ticks
static const ARGB kLabelColor = 0xD0000000;  // near-black text

// PtBoxF (x0/y0/x1/y1, page space) -> screen rect for pageNo
static Rect OverlayRectToScreen(DisplayModel* dm, int pageNo, const PtBoxF& b) {
    RectF rf(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    return dm->CvtToScreen(pageNo, rf);
}

static void DrawBoxRect(Gdiplus::Graphics& gs, const Rect& r, ARGB argb, bool dashed = false) {
    Gdiplus::Color col;
    col.SetValue(argb);
    Gdiplus::Pen pen(col, 1.5f);
    if (dashed) {
        pen.SetDashStyle(Gdiplus::DashStyleDash);
    }
    gs.DrawRectangle(&pen, r.x, r.y, r.dx, r.dy);
}

static void DrawLabelText(Gdiplus::Graphics& gs, const WCHAR* s, int x, int y, ARGB argb) {
    Gdiplus::Color col;
    col.SetValue(argb);
    // Solid fill (no antialias brush juggling): debug text must stay legible
    // over scanned ink, size is small and fixed.
    Gdiplus::SolidBrush brush(col);
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 9);
    gs.DrawString(s, -1, &font, Gdiplus::PointF((float)x, (float)y), &brush);
}

// vertical marker for one indent band inside a column (page space -> screen)
static void DrawIndentTick(Gdiplus::Graphics& gs, DisplayModel* dm, int pageNo, const PtBoxF& col,
                           float indentNormalized) {
    float xPage = col.x0 + indentNormalized * col.Dx();
    Rect top = dm->CvtToScreen(pageNo, RectF(xPage, col.y0, 0.f, 0.f));
    Rect bot = dm->CvtToScreen(pageNo, RectF(xPage, col.y1, 0.f, 0.f));
    Gdiplus::Color col2;
    col2.SetValue(kIndentColor);
    Gdiplus::Pen pen(col2, 1.f);
    pen.SetDashStyle(Gdiplus::DashStyleDot);
    gs.DrawLine(&pen, (float)top.x, (float)top.y, (float)bot.x, (float)bot.y);
}

void PaintPrintedTocOverlay(HDC hdc, DisplayModel* dm, int pageNo) {
    if (!dm || !PtocOverlayEnabled()) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    const char* pdfPath = engine ? engine->FilePath() : nullptr;
    if (!pdfPath) {
        return;
    }
    Vec<PtocOverlayPage*> pages;
    PtocOverlayCopyFor(pdfPath, pages);
    PtocOverlayPage* op = nullptr;
    for (int i = 0; i < pages.Size(); i++) {
        if (pages[i]->pageNo == pageNo) {
            op = pages[i];
            break;
        }
    }
    if (!op) {
        for (int i = 0; i < pages.Size(); i++) {
            delete pages[i];
        }
        return;
    }

    Gdiplus::Graphics gs(hdc);

    // column bounds + indent bands first (under the entry boxes)
    for (int i = 0; i < op->columns.Size(); i++) {
        const PtBoxF& c = op->columns[i];
        if (c.IsEmpty()) {
            continue;
        }
        Rect scr = OverlayRectToScreen(dm, pageNo, c);
        DrawBoxRect(gs, scr, kColumnColor, true);
        for (int k = 0; k < op->indentBands.Size(); k++) {
            DrawIndentTick(gs, dm, pageNo, c, op->indentBands[k]);
        }
    }

    for (int i = 0; i < op->entries.Size(); i++) {
        const PtocOverlayEntry& e = op->entries[i];
        if (!e.titleBox.IsEmpty()) {
            Rect scr = OverlayRectToScreen(dm, pageNo, e.titleBox);
            DrawBoxRect(gs, scr, kTitleColor);
            TempStr labelC =
                str::FormatTemp("L%d%s %.2f", e.level, e.mergedWithPrev > 0 ? "+" : "", (double)e.parseConf);
            TempWStr label = ToWStrTemp(labelC);
            DrawLabelText(gs, label, scr.x, scr.y - 16, kLabelColor);
        }
        if (!e.pageNumberBox.IsEmpty()) {
            Rect scr = OverlayRectToScreen(dm, pageNo, e.pageNumberBox);
            DrawBoxRect(gs, scr, kAnchorColor);
        }
    }

    // page summary at the top-left corner of the page
    {
        Rect scr = dm->CvtToScreen(pageNo, RectF(0, 0, 0, 0));
        TempStr labelC = str::FormatTemp("ptoc: toc=%d score=%.2f entries=%d", op->isTocPage ? 1 : 0,
                                         (double)op->pageScore, op->entries.Size());
        TempWStr label = ToWStrTemp(labelC);
        DrawLabelText(gs, label, scr.x, scr.y, kLabelColor);
    }

    for (int i = 0; i < pages.Size(); i++) {
        delete pages[i];
    }
}
