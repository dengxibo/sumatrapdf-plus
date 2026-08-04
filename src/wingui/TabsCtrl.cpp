/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/BitManip.h"
#include "utils/WinUtil.h"
#include "utils/Dpi.h"
#include "utils/WinDynCalls.h"

#include "wingui/UIModels.h"

#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Theme.h"

#include "utils/Log.h"

// Forward declaration - defined in MainWindow.cpp
struct MainWindow;
MainWindow* FindMainWindowByHwnd(HWND hwnd);

//--- Tabs

Kind kindTabs = "tabs";

// non-selected tabs narrower than this hide their close button so that
// clicks drag/select instead of accidentally closing the tab
constexpr int kMinTabWidthForClose = 64;

static int MinTabWidthForClose(HWND hwnd) {
    return DpiScale(hwnd, kMinTabWidthForClose);
}

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::CompositingQualityHighQuality;
using Gdiplus::Font;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::Ok;
using Gdiplus::PathData;
using Gdiplus::Pen;
using Gdiplus::Region;
using Gdiplus::SolidBrush;
using Gdiplus::Status;
using Gdiplus::StringAlignmentCenter;
using Gdiplus::StringFormat;
using Gdiplus::TextRenderingHintClearTypeGridFit;
using Gdiplus::UnitPixel;

// Match Font(hdc, hf) + UnitPixel: em size is abs(lfHeight), already scaled when the HFONT was created.
static float TabsFontEmSizePx(HWND hwnd, HFONT hf) {
    LOGFONTW lf{};
    if (!hf || GetObjectW(hf, sizeof(lf), &lf) == 0) {
        return (float)DpiScale(hwnd, 12);
    }
    int h = lf.lfHeight;
    if (h < 0) {
        h = -h;
    }
    if (h < 1) {
        return (float)DpiScale(hwnd, 12);
    }
    return (float)h;
}

static void HwndTabsSetItemSize(HWND hwnd, Size sz) {
    TabCtrl_SetItemSize(hwnd, sz.dx, sz.dy);
}

TabInfo::~TabInfo() {
    str::Free(text);
    str::Free(tooltip);
}

void TabsCtrl::ScheduleRepaint() {
    HwndScheduleRepaint(hwnd);
}

// Calculates tab's elements, based on its width and height.
// Generates a GraphicsPath, which is used for painting the tab, etc.
void TabsCtrl::LayoutTabs() {
    Rect rect = ClientRect(hwnd);
    int dy = rect.dy;
    int nTabs = TabCount();
    if (nTabs == 0) {
        // logfa("TabsCtrl::Layout size: (%d, %d), no tabs\n", rect.dx, rect.dy);
        HwndScheduleRepaint(hwnd);
        return;
    }

    HFONT hfont = GetFont();
    int textPad = DpiScale(hwnd, 8);
    // Home / pinned tabs: compact width from label (not the shared document max).
    // Document tabs stay equal and shrink together when crowded — no scroll arrows.
    int pinnedTotalDx = 0;
    int nDocTabs = 0;
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = GetTab(i);
        if (ti->isPinned) {
            Size ts = HwndMeasureText(hwnd, ti->text, hfont);
            int pinnedDx = ts.dx + 2 * textPad;
            int minPinned = DpiScale(hwnd, 48);
            if (pinnedDx < minPinned) {
                pinnedDx = minPinned;
            }
            // stash desired width in titleSize.dx until we assign rects
            ti->titleSize = ts;
            ti->r.dx = pinnedDx;
            pinnedTotalDx += pinnedDx;
        } else {
            nDocTabs++;
        }
    }

    int gap = 5;
    int availForDocs = rect.dx - gap - pinnedTotalDx;
    if (availForDocs < 0) {
        availForDocs = 0;
    }
    int docDx;
    if (tabWidthFrozen && frozenTabDx > 0) {
        docDx = frozenTabDx;
    } else if (nDocTabs > 0) {
        int maxDx = availForDocs / nDocTabs;
        docDx = std::min(tabDefaultDx, maxDx);
        int minDoc = DpiScale(hwnd, 40);
        if (docDx < minDoc && maxDx >= minDoc) {
            docDx = minDoc;
        }
        if (docDx < 1) {
            docDx = 1;
        }
    } else {
        docDx = tabDefaultDx;
    }
    tabSize = {docDx, dy};

    int closeDy = DpiScale(hwnd, 16);
    int closeDx = closeDy;
    int closeY = (dy - closeDy) / 2;
    // logfa("  closeDx: %d, closeDy: %d\n", closeDx, closeDy);

    bool isRtl = HwndIsRtl(hwnd);
    int closePad = DpiScale(hwnd, 8); // padding between close circle and tab edge

    int x = isRtl ? rect.dx : 0;
    int xEnd;
    TooltipInfo* tools = AllocArrayTemp<TooltipInfo>(nTabs);
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = GetTab(i);
        int dx = ti->isPinned ? ti->r.dx : docDx;
        if (isRtl) {
            xEnd = x - dx;
            ti->r = {xEnd, 0, dx, dy};
            ti->rClose = {xEnd + closePad, closeY, closeDx, closeDy};
            ti->rCloseHit = {xEnd, 0, closeDx + 2 * closePad, dy};
        } else {
            xEnd = x + dx;
            ti->r = {x, 0, dx, dy};
            ti->rClose = {xEnd - closeDx - closePad, closeY, closeDx, closeDy};
            ti->rCloseHit = {xEnd - closeDx - 2 * closePad, 0, closeDx + 2 * closePad, dy};
        }
        ti->titleSize = HwndMeasureText(hwnd, ti->text, hfont);
        int y = (dy - ti->titleSize.dy) / 2;
        // logfa("  ti->titleSize.dy: %d\n", ti->titleSize.dy);
        if (y < 0) {
            y = 0;
        }
        if (isRtl) {
            ti->titlePos = {xEnd + dx - textPad - ti->titleSize.dx, y};
        } else {
            ti->titlePos = {x + textPad, y};
        }
        if (withToolTips) {
            tools[i].s = ti->tooltip;
            tools[i].id = i;
            tools[i].r = ti->r;
        }
        x = xEnd;
    }
    if (withToolTips) {
        HWND ttHwnd = GetToolTipsHwnd();
        TooltipRemoveAll(ttHwnd);
        TooltipAddTools(ttHwnd, hwnd, tools, nTabs);
    }

    // Native WC_TABCONTROL assumes every tab is TCM_SETITEMSIZE wide. Home is
    // narrower, so nTabs*docDx can exceed the client and Windows draws UpDown
    // scroll arrows — hide them; we shrink tabs instead of scrolling.
    int nativeDx = nTabs > 0 ? (rect.dx - gap) / nTabs : docDx;
    if (nativeDx < 1) {
        nativeDx = 1;
    }
    HwndTabsSetItemSize(hwnd, {nativeDx, dy});
    HWND hwndUpDown = FindWindowExW(hwnd, nullptr, UPDOWN_CLASSW, nullptr);
    if (hwndUpDown) {
        ShowWindow(hwndUpDown, SW_HIDE);
    }
}

// Finds the index of the tab, which contains the given point.
TabsCtrl::MouseState TabsCtrl::TabStateFromMousePosition(const Point& p) {
    TabsCtrl::MouseState res;
    // WS_EX_LAYOUTRTL mirrors client coordinates in mouse messages,
    // but GDI+ (used for painting) doesn't respect DC mirroring.
    // Un-mirror so mouse coords match our manually laid out tab rects.
    Point pt = p;
    if (HwndIsRtl(hwnd)) {
        Rect rc = ClientRect(hwnd);
        pt.x = rc.dx - 1 - pt.x;
    }
    if (pt.x < 0 || pt.y < 0) {
        return res;
    }
    int nTabs = TabCount();
    for (int i = 0; i < nTabs; i++) {
        TabInfo* ti = tabs[i];
        Rect r = ti->r;
        // logfa("testing i=%d rect: %d %d %d %d pt: %d %d\n", i, ti->r.x, ti->r.y, ti->r.dx, ti->r.dy, pt.x, pt.y);
        if (!r.Contains(pt)) {
            continue;
        }
        res.tabIdx = i;
        bool isSelected = (i == GetSelected());
        bool closeActive = isSelected || r.dx >= MinTabWidthForClose(hwnd);
        res.overClose = closeActive && ti->rCloseHit.Contains(pt);
        res.tabInfo = ti;
        Rect rightHalf = r;
        int halfDx = r.dx / 2;
        rightHalf.x = r.x + halfDx;
        rightHalf.dx = halfDx;
        res.inRightHalf = rightHalf.Contains(pt);
        return res;
    }

    return res;
}

Gdiplus::Color GdipCol(COLORREF c) {
    return GdiRgbFromCOLORREF(c);
}

bool TabsCtrl::IsValidIdx(int idx) {
    return idx >= 0 && idx < TabCount();
}

void TabsCtrl::Paint(HDC hdc, const RECT& rc) {
    // verify the cursor is actually inside the tab control; if not, ignore stale lastMousePos
    Point cursorPos = HwndGetCursorPos(hwnd);
    Rect clientRc = ClientRect(hwnd);
    bool mouseInside = clientRc.Contains(cursorPos);
    TabsCtrl::MouseState tabState;
    if (mouseInside) {
        tabState = TabStateFromMousePosition(cursorPos);
    }
    int tabUnderMouse = tabState.tabIdx;
    bool overClose = tabState.overClose && tabState.tabInfo && tabState.tabInfo->canClose;
    int selectedIdx = GetSelected();
    if (IsValidIdx(tabForceShowSelected)) {
        selectedIdx = tabForceShowSelected;
    }

    // logfa("TabsCtrl::Paint, underMouse: %d, overClose: %d, selected: %d, rc: pos: (%d, %d), size: (%d, %d)\n",
    //  tabUnderMouse, (int)overClose, selectedIdx, rc.left, rc.top, RectDx(rc), RectDy(rc));

    Graphics gfx(hdc);
    gfx.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    gfx.SetCompositingQuality(CompositingQualityHighQuality);
    gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gfx.SetPageUnit(UnitPixel);

    SolidBrush br(GdipCol(ThemeChromeBackgroundColor()));

    // Font(hdc, hf) with UnitPixel uses abs(lfHeight) as the em size in pixels.
    HFONT hf = GetFont();
    LOGFONTW lf{};
    GetObjectW(hf, sizeof(lf), &lf);
    float sizePx = TabsFontEmSizePx(hwnd, hf);
    Gdiplus::FontFamily family(lf.lfFaceName);
    Gdiplus::FontFamily defaultFamily(L"Segoe UI");
    Gdiplus::FontFamily* familyPtr = family.IsAvailable() ? &family : &defaultFamily;
    int fontStyle = Gdiplus::FontStyleRegular;
    if (lf.lfWeight >= FW_BOLD) {
        fontStyle |= Gdiplus::FontStyleBold;
    }
    if (lf.lfItalic) {
        fontStyle |= Gdiplus::FontStyleItalic;
    }
    if (lf.lfUnderline) {
        fontStyle |= Gdiplus::FontStyleUnderline;
    }
    if (lf.lfStrikeOut) {
        fontStyle |= Gdiplus::FontStyleStrikeout;
    }
    Font f(familyPtr, sizePx, fontStyle, UnitPixel);

    Gdiplus::Rect gr = ToGdipRect(rc);
    gfx.FillRectangle(&br, gr);

    StringFormat sf(StringFormat::GenericDefault());
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    if (HwndIsRtl(hwnd)) {
        sf.SetAlignment(Gdiplus::StringAlignmentFar);
    }

    TabInfo* ti;
    int n = TabCount();
    Gdiplus::RectF rTxt;

    COLORREF textColor = ThemeWindowTextColor();
    COLORREF tabBaseBg = ThemeChromeBackgroundColor();
    COLORREF tabBgSelected = tabBaseBg;
    COLORREF tabBgHighlight;
    COLORREF tabBgBackground;
    if (ThemeUsesBlackChrome()) {
        tabBgBackground = tabBaseBg;
        tabBgHighlight = AccentColor(tabBaseBg, 14, 20);
        tabBgSelected = AccentColor(tabBaseBg, 28, 44);
    } else if (ThemeUsesDarkChrome()) {
        tabBgBackground = tabBaseBg;
        tabBgHighlight = AccentColor(tabBaseBg, 4, 8);
        tabBgSelected = AccentColor(tabBaseBg, 6, 14);
    } else {
        tabBgBackground = AccentColor(tabBaseBg, 25);
        tabBgHighlight = AccentColor(tabBaseBg, 35);
    }

    COLORREF tabBgCol;
    for (int i = 0; i < n; i++) {
        // Get the correct colors based on the state and the current theme
        tabBgCol = tabBgBackground;
        bool isSelected = selectedIdx == i;
        bool isUnderMouse = tabUnderMouse == i;
        if (isSelected) {
            tabBgCol = tabBgSelected;
        } else if (isUnderMouse) {
            tabBgCol = tabBgHighlight;
        }

        ti = GetTab(i);
        bool closeVisible = ti->canClose && (isSelected || (isUnderMouse && ti->r.dx >= MinTabWidthForClose(hwnd)));

        // use per-tab color if explicitly set
        constexpr COLORREF kUnset = (COLORREF)(0xfeffffff);
        if (ti->tabColor != kUnset) {
            tabBgCol = ti->tabColor;
            if (!isSelected) {
                tabBgCol = AccentColor(ti->tabColor, isUnderMouse ? 35 : 25);
            }
        }

        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);

        // draw background
        br.SetColor(GdipCol(tabBgCol));
        gr = ToGdipRect(ti->r);
        gfx.FillRectangle(&br, gr);

        if (isSelected && ThemeUsesBlackChrome()) {
            gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
            Pen pen(GdiRgbFromCOLORREF(AccentColor(tabBaseBg, 22, 38)), 1.0f);
            float y = (float)(ti->r.y + ti->r.dy - 1);
            gfx.DrawLine(&pen, (float)ti->r.x + 2, y, (float)(ti->r.x + ti->r.dx - 2), y);
        }

        // debug: paint close hit area in light green
        if (false && ti->canClose && (i == tabUnderMouse)) {
            Gdiplus::SolidBrush dbgBr(Gdiplus::Color(80, 0, 255, 0));
            gfx.FillRectangle(&dbgBr, ToGdipRect(ti->rCloseHit));
        }

        // draw text
        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        bool isRtl = HwndIsRtl(hwnd);
        int textPad = DpiScale(hwnd, 8);
        int elementGap = DpiScale(hwnd, 4);
        int dotDiameter = DpiScale(hwnd, 6);
        float contentLeft = (float)(ti->r.x + textPad);
        float contentRight = (float)(ti->r.x + ti->r.dx - textPad);
        if (closeVisible) {
            if (isRtl) {
                contentLeft = (float)(ti->rClose.x + ti->rClose.dx + elementGap);
            } else {
                contentRight = (float)(ti->rClose.x - elementGap);
            }
        }
        rTxt = ToGdipRectF(ti->r);
        rTxt.X = contentLeft;
        rTxt.Width = std::max(0.f, contentRight - contentLeft);
        if (ti->isDirty) {
            float dirtySlotDx = (float)(dotDiameter + elementGap);
            if (isRtl) {
                rTxt.X += dirtySlotDx;
            }
            rTxt.Width = std::max(0.f, rTxt.Width - dirtySlotDx);
        }
        br.SetColor(GdipCol(textColor));
        TempWStr ws = ToWStrTemp(ti->text);
        gfx.DrawString(ws, -1, &f, rTxt, &sf, &br);

        // draw red dot after tab text for dirty (unsaved) tabs
        if (ti->isDirty) {
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            // measure actual rendered text width (may be truncated with ellipsis)
            Gdiplus::RectF bounds;
            gfx.MeasureString(ws, -1, &f, rTxt, &sf, &bounds);
            int dotX;
            if (isRtl) {
                int minX = (int)contentLeft;
                dotX = (int)bounds.X - elementGap - dotDiameter;
                dotX = std::max(dotX, minX);
            } else {
                int maxX = (int)contentRight - dotDiameter;
                dotX = (int)(bounds.X + bounds.Width) + elementGap;
                dotX = std::min(dotX, maxX);
            }
            int dotY = ti->r.y + (ti->r.dy - dotDiameter) / 2;
            SolidBrush redBr(Color(255, 0xEE, 0x22, 0x22));
            gfx.FillEllipse(&redBr, dotX, dotY, dotDiameter, dotDiameter);
            gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        }
        if (closeVisible) {
            DrawCloseButtonArgs closeArgs;
            closeArgs.hdc = hdc;
            closeArgs.r = ti->rClose;
            closeArgs.isHover = overClose && isUnderMouse;
            closeArgs.colBg = tabBgCol;
            DrawCloseButton(closeArgs);
        }
    }
}

HBITMAP TabsCtrl::RenderForDragging(int idx) {
    TabInfo* ti = GetTab(idx);
    if (!ti) {
        return nullptr;
    }
    Bitmap bitmap(ti->r.dx, ti->r.dy);
    Graphics* gfx = Graphics::FromImage(&bitmap);
    // DrawString() on a bitmap does not work with CompositingModeSourceCopy - obscure bug.
    gfx->SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    gfx->SetCompositingQuality(CompositingQualityHighQuality);
    gfx->SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    gfx->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gfx->SetPageUnit(UnitPixel);

    StringFormat sf(StringFormat::GenericDefault());
    sf.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    COLORREF bgCol = tabSelectedBg;
    COLORREF textCol = tabSelectedText;

    SolidBrush br(GdipCol(bgCol));
    Gdiplus::Rect gr(0, 0, ti->r.dx, ti->r.dy);
    gfx->FillRectangle(&br, gr);

    HFONT hf = GetFont();
    LOGFONTW lf{};
    GetObjectW(hf, sizeof(lf), &lf);
    float sizePx = TabsFontEmSizePx(hwnd, hf);
    Gdiplus::FontFamily family(lf.lfFaceName);
    Gdiplus::FontFamily defaultFamily(L"Segoe UI");
    Gdiplus::FontFamily* familyPtr = family.IsAvailable() ? &family : &defaultFamily;
    int fontStyle = Gdiplus::FontStyleRegular;
    if (lf.lfWeight >= FW_BOLD) {
        fontStyle |= Gdiplus::FontStyleBold;
    }
    if (lf.lfItalic) {
        fontStyle |= Gdiplus::FontStyleItalic;
    }
    Font f(familyPtr, sizePx, fontStyle, UnitPixel);

    int textPad = DpiScale(hwnd, 8);
    Gdiplus::RectF rTxt(0, 0, ti->r.dx, ti->r.dy);
    rTxt.X += textPad;
    rTxt.Width -= (textPad + textPad);
    br.SetColor(GdipCol(textCol));
    TempWStr ws = ToWStrTemp(ti->text);
    gfx->DrawString(ws, -1, &f, rTxt, &sf, &br);

    HBITMAP ret;
    bitmap.GetHBITMAP(Color(255, 255, 255), &ret);
    delete gfx;
    return ret;
}

TabsCtrl::TabsCtrl() {
    kind = kindTabs;
}

// must be called after LayoutTabs()
static void TabsCtrlUpdateAfterChangingTabsCount(TabsCtrl* tabs) {
    HWND hwnd = tabs->hwnd;
    if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    tabs->tabBeingClosed = -1;
    Point mousePos = HwndGetCursorPos(hwnd);
    auto tabState = tabs->TabStateFromMousePosition(mousePos);
    bool canClose = tabState.tabInfo && tabState.tabInfo->canClose;
    bool overClose = tabState.overClose && canClose;
    int tabUnderMouse = tabState.tabIdx;
    tabs->tabHighlighted = tabUnderMouse;
    tabs->tabHighlightedClose = overClose ? tabUnderMouse : -1;
    if (tabs->draggingTab) {
        tabs->draggingTab = false;
        ImageList_EndDrag();
    }
}

TabsCtrl::~TabsCtrl() {}

static void TriggerSelectionChanged(TabsCtrl* tabs) {
    if (!tabs->onSelectionChanged.IsValid()) {
        return;
    }
    TabsCtrl::SelectionChangedEvent ev;
    ev.tabs = tabs;
    tabs->onSelectionChanged.Call(&ev);
}

static bool TriggerSelectionChanging(TabsCtrl* tabs) {
    if (!tabs->onSelectionChanging.IsValid()) {
        // allow changing
        return false;
    }

    TabsCtrl::SelectionChangingEvent ev;
    tabs->onSelectionChanging.Call(&ev);
    return (LRESULT)ev.preventChanging;
}

static void TriggerTabMigration(TabsCtrl* tabs, int tabIdx, Point p) {
    if (!tabs->onTabMigration.IsValid()) {
        return;
    }
    TabsCtrl::MigrationEvent ev;
    ev.tabs = tabs;
    ev.tabIdx = tabIdx;
    ev.releasePoint = p;
    tabs->onTabMigration.Call(&ev);
}

static void TriggerTabClosed(TabsCtrl* tabs, int tabIdx) {
    if ((tabIdx < 0) || !tabs->onTabClosed.IsValid()) {
        return;
    }
    TabsCtrl::ClosedEvent ev;
    ev.tabs = tabs;
    ev.tabIdx = tabIdx;
    tabs->onTabClosed.Call(&ev);
}

static void TriggerTabDragged(TabsCtrl* tabs, int tab1, int tab2) {
    if (!tabs->onTabDragged.IsValid()) {
        return;
    }
    TabsCtrl::DraggedEvent ev;
    ev.tabs = tabs;
    ev.tab1 = tab1;
    ev.tab2 = tab2;
    tabs->onTabDragged.Call(&ev);
}

static void UpdateAfterDrag(TabsCtrl* tabsCtrl, int tabIdxFrom, int tabIdxTo) {
    int nTabs = tabsCtrl->TabCount();
    bool badState =
        (tabIdxFrom == tabIdxTo) || (tabIdxFrom < 0) || (tabIdxTo < 0) || (tabIdxFrom >= nTabs) || (tabIdxTo > nTabs);
    if (badState) {
        logfa("tabIdxFrom: %d, tabIdxTo: %d, nTabs: %d\n", tabIdxFrom, tabIdxTo, nTabs);
        ReportDebugIf(true);
        return;
    }

    auto&& tabs = tabsCtrl->tabs;
    TabInfo* moved = tabs.At(tabIdxFrom);
    tabs.RemoveAt(tabIdxFrom);
    if (tabIdxFrom < tabIdxTo) {
        // we moved from left to right e.g. from 1 to 3
        // after removing 1 we insert not at 3 but 2
        tabIdxTo -= 1;
    }
    tabs.InsertAt(tabIdxTo, moved);
    tabsCtrl->SetSelected(tabIdxTo);
    tabsCtrl->LayoutTabs();
    TabsCtrlUpdateAfterChangingTabsCount(tabsCtrl);
}

LRESULT TabsCtrl::OnNotifyReflect(WPARAM wp, LPARAM lp) {
    NMHDR* hdr = (NMHDR*)lp;
    switch (hdr->code) {
        case TCN_SELCHANGING:
            return (LRESULT)TriggerSelectionChanging(this);

        case TCN_SELCHANGE:
            TriggerSelectionChanged(this);
            break;

        case TTN_GETDISPINFOA:
        case TTN_GETDISPINFOW:
            break;
    }
    return 0;
}

static bool CanDragTab(TabInfo* tab) {
    if (tab->isPinned) return false;
    return true;
}

LRESULT TabsCtrl::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Point mousePos = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (WM_MOUSELEAVE == msg) {
        mousePos = HwndGetCursorPos(hwnd);
    }

    TabsCtrl::MouseState tabState;

    bool overClose = false;
    bool canClose = true;
    int tabUnderMouse = -1;

    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg == WM_MOUSELEAVE)) {
        tabState = TabStateFromMousePosition(mousePos);
        tabUnderMouse = tabState.tabIdx;
        canClose = tabState.tabInfo && tabState.tabInfo->canClose;
        overClose = tabState.overClose && canClose;
        lastMousePos = mousePos;
        // TempStr msgName = WinMsgNameTemp(msg);
        //  logfa("msg; %s, tabUnderMouse: %d, overClose: %d\n", msgName, tabUnderMouse, (int)overClose);
    }

    if (draggingTab && msg == WM_MOUSEMOVE) {
        POINT p;
        p.x = mousePos.x;
        p.y = mousePos.y;
        MapWindowPoints(hwnd, NULL, &p, 1);
        // logfa("%s moving to: %d %d\n", WinMsgNameTemp(msg), p.x, p.y);
        ImageList_DragMove(p.x, p.y);
        return 0;
    }

    // Check if mouse has moved beyond system drag threshold
    bool beyondDragThreshold = false;
    if (msg == WM_MOUSEMOVE && GetCapture() == hwnd && !draggingTab) {
        if (tabHighlighted >= 0 && tabHighlighted < TabCount()) {
            int cxDrag = GetSystemMetrics(SM_CXDRAG);
            int cyDrag = GetSystemMetrics(SM_CYDRAG);
            beyondDragThreshold = (abs(mousePos.x - grabLocation.x - GetTab(tabHighlighted)->r.x) > cxDrag) ||
                                  (abs(mousePos.y - grabLocation.y - GetTab(tabHighlighted)->r.y) > cyDrag);
        }
    }

    switch (msg) {
        case WM_NCHITTEST: {
            if (false) {
                return HTCLIENT;
            }
            // parts that are HTTRANSPARENT are used to move the window
            if (!inTitleBar || hwnd == GetCapture()) {
                return HTCLIENT;
            }
            HwndScreenToClient(hwnd, mousePos);
            tabState = TabStateFromMousePosition(mousePos);
            if (tabState.tabIdx >= 0) {
                return HTCLIENT;
            }
            return HTTRANSPARENT;
        }

        case WM_SIZE:
            LayoutTabs();
            break;

        case WM_MOUSELEAVE:
            if (tabWidthFrozen) {
                tabWidthFrozen = false;
                LayoutTabs();
            }
            if (tabHighlighted != tabUnderMouse || tabHighlightedClose != -1) {
                tabHighlighted = tabUnderMouse;
                tabHighlightedClose = -1;
                HwndScheduleRepaint(hwnd);
            }
            break;

        case WM_MOUSEMOVE: {
            TrackMouseLeave(hwnd);
            bool isDragging = (GetCapture() == hwnd);
            int hl = tabHighlighted;
            if (isDragging && beyondDragThreshold) {
                if (hl < 0) {
                    return 0;
                }
                // move the tab out: draw it as a image and drag around the screen
                draggingTab = true;
                TabInfo* thl = GetTab(hl);
                HBITMAP hbmp = RenderForDragging(hl);
                if (!hbmp) {
                    logfa("TabsCtrl::WndProc: RenderForDragging failed for tab %d\n", hl);
                    return 0;
                }
                HIMAGELIST himl = ImageList_Create(thl->r.dx, thl->r.dy, 0, 1, 0);
                ImageList_Add(himl, hbmp, NULL);
                ImageList_BeginDrag(himl, 0, grabLocation.x, grabLocation.y);
                DeleteObject(hbmp);
                DeleteObject(himl);
                POINT p(mousePos.x, mousePos.y);
                MapWindowPoints(hwnd, NULL, &p, 1);
                ImageList_DragEnter(NULL, p.x, p.y);
                return 0;
            }

            if (hl != tabUnderMouse) {
                tabHighlighted = tabUnderMouse;
                // logf("tab: WM_MOUSEMOVE: tabHighlighted = tabUnderMouse: %d\n", tabHighlighted);
                // note: hl == -1 possible repro: we start drag, a file gets loaded via DDE etc.
                // which re-layouts tabs and mouse is no longer over a tab
                if (isDragging && hl != -1 && tabUnderMouse != -1) {
                    // send notification if the highlighted tab is dragged over another
                    if (!CanDragTab(GetTab(tabUnderMouse))) {
                        TriggerTabDragged(this, hl, tabUnderMouse);
                        UpdateAfterDrag(this, hl, tabUnderMouse);
                    }
                } else {
                    // highlight a different tab
                    HwndScheduleRepaint(hwnd);
                }
                return 0;
            }
            int xHl = -1;
            if (overClose && !isDragging) {
                xHl = hl;
            }
            // logfa("inX=%d, hl=%d, xHl=%d, xHighlighted=%d\n", (int)inX, hl, xHl, tab->xHighlighted);
            if (tabHighlightedClose != xHl) {
                // logfa("before invalidate, xHl=%d, xHighlited=%d\n", xHl, tab->xHighlighted);
                tabHighlightedClose = xHl;
                HwndScheduleRepaint(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            tabHighlighted = tabUnderMouse;
            if (overClose) {
                HwndScheduleRepaint(hwnd);
                tabBeingClosed = tabUnderMouse;
                return 0;
            }
            if (tabUnderMouse < 0) {
                return 0;
            }

            int selectedTab = GetSelected();
            if (tabUnderMouse != selectedTab) {
                bool stopChange = TriggerSelectionChanging(this);
                if (stopChange) {
                    return 0;
                }
                SetSelected(tabUnderMouse);
                TriggerSelectionChanged(this);
            }
            TabInfo* ti = GetTab(tabUnderMouse);
            if (ti->isPinned) {
                return 0;
            }

            int mx = mousePos.x;
            if (HwndIsRtl(hwnd)) {
                Rect rc = ClientRect(hwnd);
                mx = rc.dx - 1 - mx;
            }
            grabLocation.x = mx - ti->r.x;
            grabLocation.y = mousePos.y - ti->r.y;
            SetCapture(hwnd);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            tabHighlighted = tabUnderMouse;
            if (tabUnderMouse < 0 || !canClose) {
                return 0;
            }
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            if (overClose) {
                tabBeingClosed = tabUnderMouse;
                return 0;
            }
            if (tabUnderMouse != GetSelected()) {
                bool stopChange = TriggerSelectionChanging(this);
                if (stopChange) {
                    return 0;
                }
                SetSelected(tabUnderMouse);
                TriggerSelectionChanged(this);
            }
            tabBeingClosed = tabUnderMouse;
            TriggerTabClosed(this, tabBeingClosed);
            if (!FindMainWindowByHwnd(hwnd)) {
                return 0;
            }
            HwndScheduleRepaint(hwnd);
            tabBeingClosed = -1;
            return 0;
        }

        case WM_LBUTTONUP: {
            bool isDragging = (GetCapture() == hwnd);
            if (isDragging) {
                ReleaseCapture();
            }
            if (tabBeingClosed != -1 && tabUnderMouse == tabBeingClosed && overClose) {
                // freeze tab widths so next close button stays under cursor
                // unfreezes when mouse leaves the tab control
                frozenTabDx = tabSize.dx;
                tabWidthFrozen = true;
                // send notification that the tab is closed
                TriggerTabClosed(this, tabBeingClosed);
                // TriggerTabClosed() might have destroyed the window and this TabsCtrl
                if (!FindMainWindowByHwnd(hwnd)) {
                    return 0;
                }
                HwndScheduleRepaint(hwnd);
                tabBeingClosed = -1;
                return 0;
            }
            // we don't always get WM_MOUSEMOVE before WM_LBUTTONUP so
            // update tabHighlighted
            tabHighlighted = tabUnderMouse;

            if (!draggingTab) {
                return 0;
            }
            draggingTab = false;
            ImageList_EndDrag();
            int selectedTab = GetSelected();
            if (tabUnderMouse < 0) {
                // migrate to new/different window
                POINT p(mousePos.x, mousePos.y);
                ClientToScreen(hwnd, &p);
                Point scPoint(p.x, p.y);
                TriggerTabMigration(this, selectedTab, scPoint);
                return 0;
            }
            int dstIdx = tabUnderMouse;
            if (tabState.inRightHalf) {
                dstIdx++;
            }
            if (dstIdx == selectedTab) {
                return 0;
            }
            if ((dstIdx < TabCount()) && GetTab(dstIdx)->isPinned) {
                return 0;
            }
            TriggerTabDragged(this, selectedTab, dstIdx);
            UpdateAfterDrag(this, selectedTab, dstIdx);
            HwndScheduleRepaint(hwnd);
            return 0;
        }

        case WM_MBUTTONDOWN: {
            // middle-clicking unconditionally closes the tab

            tabBeingClosed = tabUnderMouse;
            if (tabBeingClosed < 0 || !canClose) {
                return 0;
            }
            TriggerTabClosed(this, tabBeingClosed);
            // TriggerTabClosed() might have destroyed the window and this TabsCtrl
            if (!FindMainWindowByHwnd(hwnd)) {
                return 0;
            }
            HwndScheduleRepaint(hwnd);
            return 0;
        }

        case WM_ERASEBKGND: {
            // just paint with the background color to avoid flickering, we will paint the tabs in WM_PAINT
            if (ThemeUsesDarkChrome()) {
                HDC hdc = (HDC)wp;
                RECT rc = ClientRECT(hwnd);
                HBRUSH hbr = CreateSolidBrush(ThemeChromeBackgroundColor());
                FillRect(hdc, &rc, hbr);
                DeleteObject(hbr);
            }
            return TRUE; // we handled it so don't erase
        }

        case WM_NCPAINT:
            return 0; // prevent native tab control from drawing its edge

        case WM_NCCALCSIZE:
            return 0; // remove non-client area so no edge is reserved

        case WM_PAINT: {
            PAINTSTRUCT ps;
            RECT rc = ClientRECT(hwnd);
            HDC hdc = BeginPaint(hwnd, &ps);
            DoubleBuffer buffer(hwnd, ToRect(rc));
            Paint(buffer.GetDC(), rc);
            buffer.Flush(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

#if 0
        case WM_PAINT: {
            RECT rc;
            GetUpdateRect(hwnd, &rc, FALSE);
            // TODO: when is wp != nullptr?
            hdc = wp ? (HDC)wp : BeginPaint(hwnd, &ps);
#if 1
            DoubleBuffer buffer(hwnd, ToRect(rc));
            Paint(buffer.GetDC(), rc);
            buffer.Flush(hdc);
#else
            Paint(hdc, rc);
#endif
            ValidateRect(hwnd, nullptr);
            if (!wp) {
                EndPaint(hwnd, &ps);
            }
            return 0;
        }
#endif
    }

    return WndProcDefault(hwnd, msg, wp, lp);
}

HWND TabsCtrl::Create(TabsCtrl::CreateArgs& args) {
    CreateControlArgs cargs;
    cargs.parent = args.parent;
    cargs.isRtl = args.isRtl;
    cargs.font = args.font;
    cargs.className = WC_TABCONTROLW;
    withToolTips = args.withToolTips;
    tabDefaultDx = args.tabDefaultDx;

    cargs.style = WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | TCS_FOCUSNEVER | TCS_FIXEDWIDTH | TCS_FORCELABELLEFT;
    if (withToolTips) {
        cargs.style |= TCS_TOOLTIPS;
    }

    HWND hwnd = CreateControl(cargs);
    if (!hwnd) {
        return nullptr;
    }

    if (withToolTips) {
        HWND ttHwnd = GetToolTipsHwnd();
        SetWindowStyle(ttHwnd, TTS_NOPREFIX, true);
    }
    return hwnd;
}

Size TabsCtrl::GetIdealSize() {
    Size sz{32, 128};
    return sz;
}

int TabsCtrl::TabCount() {
    int n = TabCtrl_GetItemCount(hwnd);
    return n;
}

// takes ownership of tab
int TabsCtrl::InsertTab(int idx, TabInfo* tab) {
    ReportIf(idx < 0);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = ToWStrTemp(tab->text);
    int res = TabCtrl_InsertItem(hwnd, idx, &item);
    if (res < 0) {
        return res;
    }
    tabs.InsertAt(idx, tab);
    // LayoutTabs() must be before SetSelected() because SetSelected()
    // triggers sync repaint which paints tab texts in wrong positions
    // because we didn't position them yet in layout.
    LayoutTabs();
    SetSelected(idx);
    TabsCtrlUpdateAfterChangingTabsCount(this);
    return idx;
}

void TabsCtrl::SetTextAndTooltip(int idx, const char* text, const char* tooltip) {
    TabInfo* tab = GetTab(idx);
    str::ReplaceWithCopy(&tab->text, text);
    str::ReplaceWithCopy(&tab->tooltip, tooltip);
    LayoutTabs();
    HwndScheduleRepaint(hwnd);
}

void TabsCtrl::SetTabDirty(int idx, bool dirty) {
    TabInfo* tab = GetTab(idx);
    if (tab && tab->isDirty != dirty) {
        tab->isDirty = dirty;
        LayoutTabs(); // rebuilds tooltips from current ti->tooltip values
        HwndScheduleRepaint(hwnd);
    }
}

// returns userData because it's not owned by TabsCtrl
UINT_PTR TabsCtrl::RemoveTab(int idx) {
    ReportIf(idx < 0);
    ReportIf(idx >= TabCount());
    BOOL ok = TabCtrl_DeleteItem(hwnd, idx);
    ReportIf(!ok);
    TabInfo* tab = tabs[idx];
    UINT_PTR userData = tab->userData;
    tabs.RemoveAt(idx);
    delete tab;
    int selectedTab = GetSelected();
    if (idx < selectedTab) {
        SetSelected(selectedTab - 1);
    } else if (idx == selectedTab) {
        int nTabs = TabCount();
        if (nTabs > 0) {
            int newSelected = idx;
            if (newSelected >= nTabs) {
                newSelected = nTabs - 1;
            }
            SetSelected(newSelected);
        }
    }
    LayoutTabs();
    TabsCtrlUpdateAfterChangingTabsCount(this);
    return userData;
}

void TabsCtrl::SwapTabs(int idx1, int idx2) {
    TabInfo* tmp = tabs[idx1];
    tabs[idx1] = tabs[idx2];
    tabs[idx2] = tmp;
}

// Note: the caller should take care of deleting userData
void TabsCtrl::RemoveAllTabs() {
    TabCtrl_DeleteAllItems(hwnd);
    DeleteVecMembers(tabs);
    tabs.Reset();
    LayoutTabs();
    TabsCtrlUpdateAfterChangingTabsCount(this);
}

TabInfo* TabsCtrl::GetTab(int idx) {
    return tabs[idx];
}

int TabsCtrl::GetSelected() {
    int idx = TabCtrl_GetCurSel(hwnd);
    return idx;
}

int TabsCtrl::SetSelected(int idx) {
    int nTabs = TabCount();
    if (idx < 0 || idx >= nTabs) {
        logf("TabsCtrl::SetSelected(): idx: %d, TabsCount(): %d\n", idx, nTabs);
    }
    ReportIf(idx < 0 || idx >= nTabs);
    int prevSelectedIdx = TabCtrl_SetCurSel(hwnd, idx);
    return prevSelectedIdx;
}

void TabsCtrl::SetHighlighted(int idx) {
    int oldSelectedIdx = GetSelected();
    if (IsValidIdx(tabForceShowSelected)) {
        oldSelectedIdx = tabForceShowSelected;
    }
    int newSelectedIdx = GetSelected();
    if (IsValidIdx(idx)) {
        newSelectedIdx = idx;
    }
    if (tabForceShowSelected == idx) {
        return;
    }
    tabForceShowSelected = idx;
    if (oldSelectedIdx == newSelectedIdx) {
        return;
    }
    HwndRepaintNow(hwnd);
}

HWND TabsCtrl::GetToolTipsHwnd() {
    HWND res = TabCtrl_GetToolTips(hwnd);
    return res;
}
