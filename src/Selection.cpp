/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/WinUtil.h"

#include "utils/Log.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "ProgressUpdateUI.h"
#include "TextSearch.h"
#include "Notifications.h"
#include "SumatraConfig.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "EbookAnnotations.h"
#include "Toolbar.h"
#include "Translations.h"
#include "uia/Provider.h"

static bool HighlightSameTextLine(const RectF& a, const RectF& b) {
    float lineH = std::max(a.dy, b.dy);
    if (lineH < 1.0f) {
        lineH = 1.0f;
    }
    float centerA = a.y + a.dy * 0.5f;
    float centerB = b.y + b.dy * 0.5f;
    return std::abs(centerA - centerB) < lineH * 0.45f;
}

RectF ScaleHighlightBandRect(RectF r, float bandRatio) {
    if (r.IsEmpty() || bandRatio <= 0) {
        return r;
    }

    float scale = bandRatio / kHighlightBandBaseRatio;
    float centerY = r.y + r.dy * 0.5f;
    float newDy = r.dy * scale;
    if (newDy < 1.0f) {
        newDy = 1.0f;
    }
    r.y = centerY - newDy * 0.5f;
    r.dy = newDy;
    return r;
}

RectF MergeHighlightLineRect(RectF a, RectF b) {
    if (a.IsEmpty()) {
        return b;
    }
    if (b.IsEmpty()) {
        return a;
    }

    float x0 = std::min(a.x, b.x);
    float x1 = std::max(a.x + a.dx, b.x + b.dx);
    float bandDy = std::max(a.dy, b.dy);
    float centerY = (a.y + a.dy * 0.5f + b.y + b.dy * 0.5f) * 0.5f;
    return RectF(x0, centerY - bandDy * 0.5f, x1 - x0, bandDy);
}

Rect BuildHighlightLineRect(Rect* c0, Rect* cEnd) {
    int x0 = INT_MAX;
    int x1 = INT_MIN;
    int lineDy = 0;
    double centerSum = 0;
    int n = 0;

    for (Rect* c = c0; c < cEnd; c++) {
        if (!c->x && !c->dx) {
            continue;
        }
        x0 = std::min(x0, c->x);
        x1 = std::max(x1, c->BR().x);
        if (c->dy >= 3) {
            lineDy = std::max(lineDy, c->dy);
        }
        centerSum += c->y + c->dy * 0.5;
        n++;
    }

    if (n == 0) {
        return Rect();
    }

    if (lineDy == 0) {
        for (Rect* c = c0; c < cEnd; c++) {
            if (!c->x && !c->dx) {
                continue;
            }
            lineDy = std::max(lineDy, c->dy);
        }
    }

    int y = (int)(centerSum / n - lineDy * 0.5 + 0.5);
    return Rect(x0, y, x1 - x0, lineDy);
}

void NormalizeHighlightLineHeights(Vec<RectF>& rects) {
    for (size_t i = 0; i < rects.size(); i++) {
        if (rects[i].IsEmpty()) {
            continue;
        }

        float targetDy = rects[i].dy;
        for (size_t j = 0; j < rects.size(); j++) {
            if (j == i || rects[j].IsEmpty()) {
                continue;
            }
            if (HighlightSameTextLine(rects[i], rects[j])) {
                targetDy = std::min(targetDy, rects[j].dy);
            }
        }

        for (size_t j = 0; j < rects.size(); j++) {
            if (rects[j].IsEmpty()) {
                continue;
            }
            if (!HighlightSameTextLine(rects[i], rects[j])) {
                continue;
            }
            float center = rects[j].y + rects[j].dy * 0.5f;
            rects[j].dy = targetDy;
            rects[j].y = center - targetDy * 0.5f;
        }
    }
}

void NormalizeHighlightUniformHeight(Vec<RectF>& rects) {
    NormalizeNearbyHighlightHeights(rects);
}

void NormalizeNearbyHighlightHeights(Vec<RectF>& rects) {
    for (size_t i = 0; i + 1 < rects.size(); i++) {
        RectF& a = rects[i];
        RectF& b = rects[i + 1];
        if (a.IsEmpty() || b.IsEmpty()) {
            continue;
        }

        float lo = std::min(a.dy, b.dy);
        float hi = std::max(a.dy, b.dy);
        if (lo < 1.0f || hi < 1.0f) {
            continue;
        }
        // Only pair lines with similar font size (e.g. English title + Chinese subtitle).
        if (lo / hi < 0.55f) {
            continue;
        }

        float target = (a.dy + b.dy) * 0.5f;
        float centerA = a.y + a.dy * 0.5f;
        float centerB = b.y + b.dy * 0.5f;
        a.dy = target;
        a.y = centerA - target * 0.5f;
        b.dy = target;
        b.y = centerB - target * 0.5f;
    }
}

SelectionOnPage::SelectionOnPage(int pageNo, const RectF* const rect) {
    this->pageNo = pageNo;
    if (rect) {
        this->rect = *rect;
    } else {
        this->rect = RectF();
    }
}

Rect SelectionOnPage::GetRect(DisplayModel* dm) const {
    // if the page is not visible, we return an empty rectangle
    PageInfo* pageInfo = dm->GetPageInfo(pageNo);
    if (!pageInfo || pageInfo->visibleRatio <= 0.0) {
        return Rect();
    }

    return dm->CvtToScreen(pageNo, rect);
}

Vec<SelectionOnPage>* SelectionOnPage::FromRectangle(DisplayModel* dm, Rect rect) {
    Vec<SelectionOnPage>* sel = new Vec<SelectionOnPage>();

    for (int pageNo = dm->GetEngine()->PageCount(); pageNo >= 1; --pageNo) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        ReportIf(!(!pi || 0.0 == pi->visibleRatio || pi->isShown));
        if (!pi || !pi->isShown) {
            continue;
        }

        Rect intersect = rect.Intersect(pi->pageOnScreen);
        if (intersect.IsEmpty()) {
            continue;
        }

        /* selection intersects with a page <pageNo> on the screen */
        RectF isectD = dm->CvtFromScreen(intersect, pageNo);
        sel->Append(SelectionOnPage(pageNo, &isectD));
    }
    sel->Reverse();

    if (sel->size() == 0) {
        delete sel;
        return nullptr;
    }
    return sel;
}

Vec<SelectionOnPage>* SelectionOnPage::FromTextSelect(TextSel* textSel) {
    Vec<SelectionOnPage>* sel = new Vec<SelectionOnPage>(textSel->len);

    for (int i = textSel->len - 1; i >= 0; i--) {
        RectF rect = ToRectF(textSel->rects[i]);
        sel->Append(SelectionOnPage(textSel->pages[i], &rect));
    }
    sel->Reverse();

    if (sel->size() == 0) {
        delete sel;
        return nullptr;
    }
    return sel;
}

void DeleteOldSelectionInfo(MainWindow* win, bool alsoTextSel) {
    HideSelectionToolbar(win);
    win->showSelection = false;
    win->selectionMeasure = SizeF();
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        return;
    }

    delete tab->selectionOnPage;
    tab->selectionOnPage = nullptr;
    if (alsoTextSel && tab->AsFixed()) {
        tab->AsFixed()->textSelection->Reset();
    }
}

void PaintTransparentRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF selectionColor, u8 alpha, int pad) {
    // create path from rectangles
    Gdiplus::GraphicsPath path(Gdiplus::FillModeWinding);
    screenRc.Inflate(pad, pad);
    for (size_t i = 0; i < rects.size(); i++) {
        Rect rc = rects.at(i);
        if (pad > 0) {
            rc.Inflate(pad, pad);
        }
        rc = rc.Intersect(screenRc);
        if (!rc.IsEmpty()) {
            path.AddRectangle(ToGdipRect(rc));
        }
    }

    Gdiplus::Graphics gs(hdc);
    u8 r, g, b;
    UnpackColor(selectionColor, r, g, b);
    Gdiplus::Color c(alpha, r, g, b);
    Gdiplus::SolidBrush tmpBrush(c);
    gs.FillPath(&tmpBrush, &path);
}

static u8 MultiplyChannel(u8 backdrop, u8 highlight) {
    return (u8)(((unsigned)backdrop * highlight + 127) / 255);
}

void PaintMultiplyRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color, int opacity) {
    opacity = std::clamp(opacity, 0, 100);
    Rect bounds;
    Vec<Rect> clippedRects;
    for (Rect rect : rects) {
        rect = rect.Intersect(screenRc);
        if (rect.IsEmpty()) {
            continue;
        }
        bounds = bounds.IsEmpty() ? rect : bounds.Union(rect);
        clippedRects.Append(rect);
    }
    if (bounds.IsEmpty()) {
        return;
    }

    HBITMAP bitmap = CreateMemoryBitmap(bounds.Size());
    HDC bitmapDc = CreateCompatibleDC(hdc);
    if (!bitmap || !bitmapDc) {
        DeleteObject(bitmap);
        DeleteDC(bitmapDc);
        return;
    }
    HGDIOBJ prevBitmap = SelectObject(bitmapDc, bitmap);
    if (!BitBlt(bitmapDc, 0, 0, bounds.dx, bounds.dy, hdc, bounds.x, bounds.y, SRCCOPY)) {
        SelectObject(bitmapDc, prevBitmap);
        DeleteObject(bitmap);
        DeleteDC(bitmapDc);
        return;
    }

    DIBSECTION info{};
    int infoSize = GetObject(bitmap, sizeof(info), &info);
    if (infoSize < (int)sizeof(info.dsBm) || !info.dsBm.bmBits || info.dsBm.bmBitsPixel != 32) {
        SelectObject(bitmapDc, prevBitmap);
        DeleteObject(bitmap);
        DeleteDC(bitmapDc);
        return;
    }

    u8 hr, hg, hb;
    UnpackColor(color, hr, hg, hb);
    const u8 highlight[3] = {hb, hg, hr};

    u8* pixels = (u8*)info.dsBm.bmBits;
    for (Rect rect : clippedRects) {
        int xStart = rect.x - bounds.x;
        int yStart = rect.y - bounds.y;
        for (int y = yStart; y < yStart + rect.dy; y++) {
            u8* row = pixels + y * info.dsBm.bmWidthBytes;
            for (int x = xStart; x < xStart + rect.dx; x++) {
                u8* pixel = row + x * 4;
                u8 mul = MultiplyChannel(pixel[0], highlight[0]);
                pixel[0] = (u8)(pixel[0] + (mul - pixel[0]) * opacity / 100);
                mul = MultiplyChannel(pixel[1], highlight[1]);
                pixel[1] = (u8)(pixel[1] + (mul - pixel[1]) * opacity / 100);
                mul = MultiplyChannel(pixel[2], highlight[2]);
                pixel[2] = (u8)(pixel[2] + (mul - pixel[2]) * opacity / 100);
            }
        }
    }

    BitBlt(hdc, bounds.x, bounds.y, bounds.dx, bounds.dy, bitmapDc, 0, 0, SRCCOPY);
    SelectObject(bitmapDc, prevBitmap);
    DeleteObject(bitmap);
    DeleteDC(bitmapDc);
}

COLORREF GetSelectionHighlightColor() {
    ParsedColor* parsedCol = GetPrefsColor(gGlobalPrefs->fixedPageUI.selectionColor);
    return parsedCol->col;
}

void PaintSelection(MainWindow* win, HDC hdc) {
    ReportIf(!win->AsFixed());

    Vec<Rect> rects;

    if (win->mouseAction == MouseAction::Selecting) {
        // during rectangle selection
        Rect selRect = win->selectionRect;
        if (selRect.dx < 0) {
            selRect.x += selRect.dx;
            selRect.dx *= -1;
        }
        if (selRect.dy < 0) {
            selRect.y += selRect.dy;
            selRect.dy *= -1;
        }

        rects.Append(selRect);
    } else {
        // during text selection or after selection is done
        if (MouseAction::SelectingText == win->mouseAction) {
            UpdateTextSelection(win);
            if (!win->CurrentTab()->selectionOnPage) {
                // prevent the selection from disappearing while the
                // user is still at it (OnSelectionStop removes it
                // if it is still empty at the end)
                win->CurrentTab()->selectionOnPage = new Vec<SelectionOnPage>();
                win->showSelection = true;
            }
        }

        ReportDebugIf(!win->CurrentTab()->selectionOnPage);
        if (!win->CurrentTab()->selectionOnPage) {
            return;
        }

        DisplayModel* dm = win->AsFixed();
        bool tightenRects = dm->textSelection->result.len > 0;
        if (tightenRects) {
            int pageCount = dm->GetEngine()->PageCount();
            for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
                Vec<RectF> pageRects;
                for (SelectionOnPage& sel : *win->CurrentTab()->selectionOnPage) {
                    if (sel.pageNo != pageNo) {
                        continue;
                    }
                    PageInfo* pageInfo = dm->GetPageInfo(sel.pageNo);
                    if (!pageInfo || pageInfo->visibleRatio <= 0.0) {
                        continue;
                    }
                    pageRects.Append(sel.rect);
                }
                if (pageRects.Size() == 0) {
                    continue;
                }
                NormalizeNearbyHighlightHeights(pageRects);
                for (RectF& rf : pageRects) {
                    rf = ScaleHighlightBandRect(rf, kSelectionHighlightBandRatio);
                    Rect sr = dm->CvtToScreen(pageNo, rf);
                    if (!sr.IsEmpty()) {
                        rects.Append(sr);
                    }
                }
            }
        } else {
            for (SelectionOnPage& sel : *win->CurrentTab()->selectionOnPage) {
                rects.Append(sel.GetRect(dm));
            }
        }
    }

    PaintMultiplyRectangles(hdc, win->canvasRc, rects, GetSelectionHighlightColor());
}

static constexpr int kMaxReselectTextChars = 2000;
static constexpr int kReselectPageRadius = 3;

static bool ReselectTextAfterLayoutChange(DisplayModel* dm, TextSelection* ts, int nearPage, const WCHAR* text) {
    EngineBase* engine = dm->GetEngine();
    if (!engine || str::IsEmpty(text)) {
        return false;
    }
    if (!dm->ValidPageNo(nearPage)) {
        nearPage = dm->FirstVisiblePageNo();
    }
    if (!dm->ValidPageNo(nearPage)) {
        nearPage = 1;
    }

    int pageCount = dm->PageCount();
    for (int delta = 0; delta <= kReselectPageRadius; delta++) {
        int candidates[2] = {nearPage + delta, nearPage - delta};
        int n = delta == 0 ? 1 : 2;
        for (int i = 0; i < n; i++) {
            int pageNo = candidates[i];
            if (!dm->ValidPageNo(pageNo) || pageNo > pageCount) {
                continue;
            }
            TextSearch search(engine);
            TextSel* sel = search.FindFirst(pageNo, text);
            if (sel && sel->len > 0) {
                ts->Reset();
                ts->CopySelection(&search);
                return true;
            }
        }
    }
    return false;
}

void RefreshTextSelectionAfterLayoutChange(WindowTab* tab, MainWindow* win) {
    if (!tab) {
        return;
    }
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return;
    }
    TextSelection* ts = dm->textSelection;
    if (!ts || ts->startPage < 0 || ts->startGlyph < 0 || ts->result.len == 0) {
        return;
    }

    int nearPage = ts->startPage;
    WCHAR* savedText = ts->ExtractText(" ");
    if (str::IsEmpty(savedText)) {
        str::Free(savedText);
        if (win && win->CurrentTab() == tab) {
            DeleteOldSelectionInfo(win, true);
        }
        return;
    }
    str::NormalizeWSInPlace(savedText);
    if (str::Len(savedText) > kMaxReselectTextChars) {
        str::Free(savedText);
        if (win && win->CurrentTab() == tab) {
            DeleteOldSelectionInfo(win, true);
        }
        return;
    }

    EbookAnnotationsInvalidateLayoutCaches(tab);

    ts->Reset();
    ts->startPage = ts->endPage = ts->startGlyph = ts->endGlyph = -1;

    bool ok = ReselectTextAfterLayoutChange(dm, ts, nearPage, savedText);
    str::Free(savedText);
    if (!ok) {
        if (win && win->CurrentTab() == tab) {
            DeleteOldSelectionInfo(win, true);
        }
        return;
    }

    delete tab->selectionOnPage;
    tab->selectionOnPage = SelectionOnPage::FromTextSelect(&ts->result);
    if (!tab->selectionOnPage) {
        if (win && win->CurrentTab() == tab) {
            DeleteOldSelectionInfo(win, true);
        }
        return;
    }

    if (win && win->CurrentTab() == tab) {
        win->showSelection = true;
        UpdateSelectionToolbarPosition(win);
        if (win->uiaProvider) {
            win->uiaProvider->OnSelectionChanged();
        }
        ScheduleRepaint(win, 0);
    }
}

void UpdateTextSelection(MainWindow* win, bool select) {
    if (!win->AsFixed()) {
        return;
    }

    // logf("UpdateTextSelection: select: %d\n", (int)select);
    DisplayModel* dm = win->AsFixed();
    if (select) {
        int pageNo = dm->GetPageNoByPoint(win->selectionRect.BR());
        if (win->ctrl->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(win->selectionRect.BR(), pageNo);
            dm->textSelection->SelectUpTo(pageNo, pt.x, pt.y);
        }
    }

    DeleteOldSelectionInfo(win);
    win->CurrentTab()->selectionOnPage = SelectionOnPage::FromTextSelect(&dm->textSelection->result);
    win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;

    if (win->uiaProvider) {
        win->uiaProvider->OnSelectionChanged();
    }
    ToolbarUpdateStateForWindow(win, false);
}

// isTextSelectionOut is set to true if this is text-only selection (as opposed to
// rectangular selection)
// caller needs to str::Free() the result
TempStr GetSelectedTextTemp(WindowTab* tab, const char* lineSep, bool& isTextOnlySelectionOut) {
    if (!tab || !tab->selectionOnPage) {
        return nullptr;
    }
    if (tab->selectionOnPage->size() == 0) {
        return nullptr;
    }
    DisplayModel* dm = tab->AsFixed();
    ReportIf(!dm);
    if (!dm) {
        return nullptr;
    }
    if (dm->GetEngine()->IsImageCollection()) {
        return nullptr;
    }

    isTextOnlySelectionOut = dm->textSelection->result.len > 0;
    if (isTextOnlySelectionOut) {
        WCHAR* s = dm->textSelection->ExtractText(lineSep);
        TempStr res = ToUtf8Temp(s);
        str::Free(s);
        return res;
    }
    StrVec selections;
    for (SelectionOnPage& sel : *tab->selectionOnPage) {
        // selection may reference pages that no longer exist after a reload
        if (!dm->ValidPageNo(sel.pageNo)) {
            continue;
        }
        char* text = dm->GetTextInRegion(sel.pageNo, sel.rect);
        if (!str::IsEmpty(text)) {
            selections.Append(text);
        }
        str::Free(text);
    }
    if (selections.Size() == 0) {
        return nullptr;
    }
    TempStr s = JoinTemp(&selections, lineSep);
    return s;
}

void CopySelectionToClipboard(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    ReportIf(tab->selectionOnPage->size() == 0 && win->mouseAction != MouseAction::SelectingText);

    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    defer {
        CloseClipboard();
    };

    DisplayModel* dm = win->AsFixed();
    TempStr selText = nullptr;
    bool isTextOnlySelectionOut = false;
    if (!gDisableDocumentRestrictions && (dm && !dm->GetEngine()->AllowsCopyingText())) {
        NotificationCreateArgs args;
        args.hwndParent = win->hwndCanvas;
        args.msg = _TRA("Copying text was denied (copying as image only)");
        ShowNotification(args);
    } else {
        selText = GetSelectedTextTemp(tab, "\r\n", isTextOnlySelectionOut);
    }

    if (!str::IsEmpty(selText)) {
        AppendTextToClipboard(selText);
    }

    if (isTextOnlySelectionOut) {
        // don't also copy the first line of a text selection as an image
        return;
    }

    if (!dm || !tab->selectionOnPage || tab->selectionOnPage->size() == 0) {
        return;
    }
    /* also copy a screenshot of the current selection to the clipboard */
    SelectionOnPage* selOnPage = &tab->selectionOnPage->at(0);
    if (!dm->ValidPageNo(selOnPage->pageNo)) {
        return;
    }
    float zoom = dm->GetZoomReal(selOnPage->pageNo);
    int rotation = dm->GetRotation();
    RenderPageArgs args(selOnPage->pageNo, zoom, rotation, &selOnPage->rect, RenderTarget::Export);
    RenderedBitmap* bmp = dm->GetEngine()->RenderPage(args);
    if (bmp) {
        CopyImageToClipboard(bmp->GetBitmap(), true);
    }
    delete bmp;
}

void OnSelectAll(MainWindow* win, bool textOnly) {
    if (!HasPermission(Perm::CopySelection)) {
        return;
    }

    if (HwndIsFocused(win->hwndFindEdit) || HwndIsFocused(win->hwndPageEdit)) {
        EditSelectAll(GetFocus());
        return;
    }

    if (win->AsChm()) {
        win->AsChm()->SelectAll();
        return;
    }
    if (!win->AsFixed()) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    if (textOnly) {
        int pageNo;
        for (pageNo = 1; !dm->PageShown(pageNo); pageNo++) {
            ;
        }
        dm->textSelection->StartAt(pageNo, 0);
        for (pageNo = win->ctrl->PageCount(); !dm->PageShown(pageNo); pageNo--) {
            ;
        }
        dm->textSelection->SelectUpTo(pageNo, -1);
        win->selectionRect = Rect::FromXY(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX);
        UpdateTextSelection(win);
    } else {
        DeleteOldSelectionInfo(win, true);
        win->selectionRect = Rect::FromXY(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX);
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(dm, win->selectionRect);
    }

    win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
    ScheduleRepaint(win, 0);
}

#define SELECT_AUTOSCROLL_AREA_WIDTH DpiScale(win->hwndFrame, 15)
#define SELECT_AUTOSCROLL_STEP_LENGTH DpiScale(win->hwndFrame, 10)

bool NeedsSelectionEdgeAutoscroll(MainWindow* win, int x, int y) {
    return x < SELECT_AUTOSCROLL_AREA_WIDTH || x > win->canvasRc.dx - SELECT_AUTOSCROLL_AREA_WIDTH ||
           y < SELECT_AUTOSCROLL_AREA_WIDTH || y > win->canvasRc.dy - SELECT_AUTOSCROLL_AREA_WIDTH;
}

void OnSelectionEdgeAutoscroll(MainWindow* win, int x, int y) {
    int dx = 0, dy = 0;

    if (x < SELECT_AUTOSCROLL_AREA_WIDTH) {
        dx = -SELECT_AUTOSCROLL_STEP_LENGTH;
    } else if (x > win->canvasRc.dx - SELECT_AUTOSCROLL_AREA_WIDTH) {
        dx = SELECT_AUTOSCROLL_STEP_LENGTH;
    }
    if (y < SELECT_AUTOSCROLL_AREA_WIDTH) {
        dy = -SELECT_AUTOSCROLL_STEP_LENGTH;
    } else if (y > win->canvasRc.dy - SELECT_AUTOSCROLL_AREA_WIDTH) {
        dy = SELECT_AUTOSCROLL_STEP_LENGTH;
    }

    ReportIf(NeedsSelectionEdgeAutoscroll(win, x, y) != (dx != 0 || dy != 0));
    if (dx != 0 || dy != 0) {
        ReportIf(!win->AsFixed());
        DisplayModel* dm = win->AsFixed();
        Point oldOffset = dm->GetViewPort().TL();
        win->MoveDocBy(dx, dy);

        dx = dm->GetViewPort().x - oldOffset.x;
        dy = dm->GetViewPort().y - oldOffset.y;
        win->selectionRect.x -= dx;
        win->selectionRect.y -= dy;
        win->selectionRect.dx += dx;
        win->selectionRect.dy += dy;
    }
}

void OnSelectionStart(MainWindow* win, int x, int y, WPARAM) {
    ReportIf(!win->AsFixed());
    DeleteOldSelectionInfo(win, true);

    win->selectionRect = Rect(x, y, 0, 0);
    win->showSelection = true;
    win->mouseAction = MouseAction::Selecting;

    bool isShift = IsShiftPressed();
    bool isCtrl = IsCtrlPressed();

    // Ctrl+drag forces a rectangular selection
    if (!isCtrl || isShift) {
        DisplayModel* dm = win->AsFixed();
        int pageNo = dm->GetPageNoByPoint(Point(x, y));
        if (dm->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(Point(x, y), pageNo);
            dm->textSelection->StartAt(pageNo, pt.x, pt.y);
            win->mouseAction = MouseAction::SelectingText;
        }
    }

    SetCapture(win->hwndCanvas);
    SetTimer(win->hwndCanvas, SMOOTHSCROLL_TIMER_ID, SMOOTHSCROLL_DELAY_IN_MS, nullptr);
    ScheduleRepaint(win, 0);
}

void OnSelectionStop(MainWindow* win, int x, int y, bool aborted) {
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    KillTimer(win->hwndCanvas, SMOOTHSCROLL_TIMER_ID);

    // update the text selection before changing the selectionRect
    if (MouseAction::SelectingText == win->mouseAction) {
        UpdateTextSelection(win);
    }

    win->selectionRect = Rect::FromXY(win->selectionRect.x, win->selectionRect.y, x, y);
    if (aborted || (MouseAction::Selecting == win->mouseAction ? win->selectionRect.IsEmpty()
                                                               : !win->CurrentTab()->selectionOnPage)) {
        DeleteOldSelectionInfo(win, true);
    } else if (win->mouseAction == MouseAction::Selecting) {
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(win->AsFixed(), win->selectionRect);
        win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
    }
    ScheduleRepaint(win, 0);

    // show the floating annotation/Ask AI toolbar for a finished text selection
    // (the function self-guards: only PDF text selections that support annotations)
    if (!aborted) {
        ShowSelectionToolbar(win);
    }
}
