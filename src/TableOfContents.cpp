/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/BitManip.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/UITask.h"
#include "utils/Timer.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "GlobalPrefs.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "SumatraDialogs.h"
#include "Toolbar.h"
#include "MainWindow.h"
#include "EpubPerfLog.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "Selection.h"
#include "Favorites.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "ExtractPdfToc.h"
#include "TocCalib.h"
#include "AppTools.h"
#include "TableOfContents.h"
#include "Translations.h"
#include "Tabs.h"
#include "Menu.h"
#include "Accelerators.h"
#include "Theme.h"
#include "Notifications.h"

#include "utils/Log.h"
#include "utils/WinDynCalls.h"

#include <vssym32.h>

/* Define if you want page numbers to be displayed in the ToC sidebar */
// #define DISPLAY_TOC_PAGE_NUMBERS

#ifdef DISPLAY_TOC_PAGE_NUMBERS
#define WM_APP_REPAINT_TOC (WM_APP + 1)
#endif

static COLORREF SidebarBackgroundColor(COLORREF wndBgColor) {
    if (ThemeUsesDarkChrome()) {
        COLORREF bg, text;
        ThemeSidebarColors(bg, text);
        return bg;
    }
    if (!IsSpecialColor(wndBgColor)) {
        return wndBgColor;
    }
    COLORREF bg;
    ThemeDocumentColors(bg);
    return bg;
}

// Blend toward the sidebar, not the document page. ThemeReadingTextDisabledColor()
// uses the page background, which makes unread TOC entries look fully lit when the
// chrome is dark and the document is light.
static COLORREF TocItemDisabledTextColor(TreeView* treeView) {
    COLORREF txt;
    if (ThemeUsesDarkChrome()) {
        txt = ThemeReadingTextColor();
    } else if (treeView && !IsSpecialColor(treeView->textColor)) {
        txt = treeView->textColor;
    } else {
        txt = GetSysColor(COLOR_WINDOWTEXT);
    }
    COLORREF bg = SidebarBackgroundColor(treeView ? treeView->bgColor : kColorUnset);
    u8 r = (u8)((GetRValue(txt) + GetRValue(bg)) / 2);
    u8 g = (u8)((GetGValue(txt) + GetGValue(bg)) / 2);
    u8 b = (u8)((GetBValue(txt) + GetBValue(bg)) / 2);
    return RGB(r, g, b);
}

static void LayoutTocContainer(MainWindow* win);
static void TocRecalcAllItemHeights(MainWindow* win);

// TreeWrapLabels: full-tree integral updates are too heavy for live sidebar drag
// (WM_SIZE per mouse move). Suspend wrap while dragging; flush on mouse-up.
// Non-drag WM_SIZE (e.g. window resize) still debounces.
static constexpr UINT kTreeWrapHeightDebounceMs = 64;
static int gTreeWrapSuspendDepth = 0;

static bool TreeWrapUpdatesSuspended() {
    return gTreeWrapSuspendDepth > 0;
}

bool TreeWrapLiveResizeSuspended() {
    return gTreeWrapSuspendDepth > 0;
}

static void ScheduleTreeWrapHeightRecalc(HWND hwndHost) {
    if (!hwndHost || TreeWrapUpdatesSuspended()) {
        return;
    }
    SetTimer(hwndHost, kTreeWrapHeightTimerId, kTreeWrapHeightDebounceMs, nullptr);
}

static void KillTreeWrapHeightTimer(HWND hwndHost) {
    if (hwndHost) {
        KillTimer(hwndHost, kTreeWrapHeightTimerId);
    }
}

void SuspendTreeWrapLiveResize() {
    gTreeWrapSuspendDepth++;
}

void SuspendTreeWrapLiveResizeForWindow(MainWindow* win) {
    SuspendTreeWrapLiveResize();
    if (!win) {
        return;
    }
    // Cancel debounce armed before the drag; flush happens on mouse-up.
    KillTreeWrapHeightTimer(win->hwndTocBox);
    KillTreeWrapHeightTimer(win->hwndFavBox);
}

static bool IsKnownTocTreeModel(MainWindow* win, TreeModel* tm);

static bool IsTocMupdfPageReachable(EngineBase* engine, int pageNo) {
    if (!engine || engine->kind != kindEngineMupdf || pageNo < 1) {
        return false;
    }
    // engine->PageCount() grows as chapters are counted; a known page number
    // within it means the target chapter was counted and is safe to jump to
    return pageNo <= engine->PageCount();
}

void InvalidateTocTree(MainWindow* win) {
    if (!win || !win->tocLoaded || !win->tocVisible || !win->tocTreeView) {
        return;
    }
    HWND hwnd = win->tocTreeView->hwnd;
    if (hwnd) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

bool TreeWrapLabelsEnabled() {
    return gGlobalPrefs && gGlobalPrefs->treeWrapLabels;
}

void TreeWrapLabelsConfigureCreateArgs(TreeView::CreateArgs& args) {
    args.unevenItemHeight = TreeWrapLabelsEnabled();
}

void TreeItemTooltipIfTruncated(TreeView::GetTooltipEvent* ev) {
    if (!ev || !ev->treeView || !ev->treeItem || TreeWrapLabelsEnabled()) {
        return;
    }
    TreeModel* tm = ev->treeView->treeModel;
    if (!tm) {
        return;
    }
    char* text = tm->Text(ev->treeItem);
    if (!text) {
        return;
    }
    RECT rcLine, rcLabel;
    ev->treeView->GetItemRect(ev->treeItem, false, rcLine);
    ev->treeView->GetItemRect(ev->treeItem, true, rcLabel);
    if (rcLabel.right <= rcLine.right + 2) {
        return;
    }
    NMTVGETINFOTIPW* nm = ev->info;
    if (nm) {
        str::BufSet(nm->pszText, nm->cchTextMax, text);
    }
}

// set tooltip for this item but only if the text isn't fully shown
// TODO: I might have lost something in translation
static void TocCustomizeTooltip(TreeView::GetTooltipEvent* ev) {
    auto treeView = ev->treeView;
    auto tm = treeView->treeModel;
    auto ti = ev->treeItem;
    if (!treeView || !tm || !ti) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(treeView->hwnd);
    if (!win || win->isBeingClosed || !win->tocLoaded || !IsKnownTocTreeModel(win, tm)) {
        return;
    }
    TocItem* tocItem = (TocItem*)ti;
    if (TocCalibIsActive(win)) {
        POINT pt{};
        GetCursorPos(&pt);
        MapWindowPoints(HWND_DESKTOP, treeView->hwnd, &pt, 1);
        const char* calibTip = TocCalibRowControlTip(win, treeView->hwnd, pt);
        NMTVGETINFOTIPW* nmCalib = ev->info;
        if (nmCalib) {
            if (calibTip) {
                str::BufSet(nmCalib->pszText, nmCalib->cchTextMax, calibTip);
            } else {
                nmCalib->pszText[0] = 0;
            }
        }
        return;
    }
    IPageDestination* link = tocItem->GetPageDestination();
    if (!link) {
        return;
    }
    char* path = PageDestGetValue(link);
    if (!path) {
        path = tocItem->title;
    }
    if (!path) {
        return;
    }
    auto k = link->GetKind();
    // TODO: TocItem from Chm contain other types
    // we probably shouldn't set TocItem::dest there
    if (k == kindDestinationScrollTo) {
        return;
    }
    if (k == kindDestinationNone) {
        return;
    }

    bool isOk = (k == kindDestinationLaunchURL) || (k == kindDestinationLaunchFile) ||
                (k == kindDestinationLaunchEmbedded) || (k == kindDestinationMupdf) || (k == kindDestinationDjVu) ||
                (k == kindDestinationAttachment);
    ReportIf(!isOk);

    StrBuilder infotip;

    char* labelText = tm->Text(ti);
    bool truncated = false;
    // Display the item's full label when single-line mode truncates it
    if (!TreeWrapLabelsEnabled()) {
        RECT rcLine, rcLabel;
        treeView->GetItemRect(ev->treeItem, false, rcLine);
        treeView->GetItemRect(ev->treeItem, true, rcLabel);
        truncated = rcLabel.right > rcLine.right + 2;
        if (truncated && labelText) {
            infotip.Append(labelText);
        }
    }

    // When PageDestGetValue is empty, path falls back to tocItem->title 鈥?same as
    // labelText 鈥?so don't append it again after the truncated-label line.
    bool pathSameAsLabel = labelText && path && str::Eq(labelText, path);
    if (!truncated || !pathSameAsLabel) {
        if (truncated && infotip.size() > 0) {
            infotip.Append("\r\n");
        }
        if (kindDestinationLaunchEmbedded == k || kindDestinationAttachment == k) {
            TempStr tmp = str::FormatTemp(_TRA("Attachment: %s"), path);
            infotip.Append(tmp);
        } else {
            infotip.Append(path);
        }
    }

    auto nm = ev->info;
    if (!nm || infotip.size() == 0) {
        return;
    }
    str::BufSet(nm->pszText, nm->cchTextMax, infotip.Get());
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void RelayoutTocItem(LPNMTVCUSTOMDRAW ntvcd) {
    // code inspired by http://www.codeguru.com/cpp/controls/treeview/multiview/article.php/c3985/
    LPNMCUSTOMDRAW ncd = &ntvcd->nmcd;
    HWND hTV = ncd->hdr.hwndFrom;
    HTREEITEM hItem = (HTREEITEM)ncd->dwItemSpec;
    RECT rcItem;
    if (0 == ncd->rc.right - ncd->rc.left || 0 == ncd->rc.bottom - ncd->rc.top) return;
    if (!TreeView_GetItemRect(hTV, hItem, &rcItem, TRUE)) return;
    if (rcItem.right > ncd->rc.right) rcItem.right = ncd->rc.right;

    // Clear the label
    RECT rcFullWidth = rcItem;
    rcFullWidth.right = ncd->rc.right;
    FillRect(ncd->hdc, &rcFullWidth, GetSysColorBrush(COLOR_WINDOW));

    // Get the label's text
    WCHAR szText[MAX_PATH];
    TVITEM item;
    item.hItem = hItem;
    item.mask = TVIF_TEXT | TVIF_PARAM;
    item.pszText = szText;
    item.cchTextMax = MAX_PATH;
    TreeView_GetItem(hTV, &item);

    // Draw the page number right-aligned (if there is one)
    MainWindow* win = FindMainWindowByHwnd(hTV);
    TocItem* tocItem = (TocItem*)item.lParam;
    TempStr label = nullptr;
    if (tocItem->pageNo && win && win->IsDocLoaded()) {
        label = win->ctrl->GetPageLabeTemp(tocItem->pageNo);
        label = str::JoinTemp("  ", label);
    }
    if (label && str::EndsWith(item.pszText, label)) {
        RECT rcPageNo = rcFullWidth;
        InflateRect(&rcPageNo, -2, -1);

        SIZE txtSize;
        GetTextExtentPoint32(ncd->hdc, label, str::Len(label), &txtSize);
        rcPageNo.left = rcPageNo.right - txtSize.cx;

        SetTextColor(ncd->hdc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(ncd->hdc, GetSysColor(COLOR_WINDOW));
        DrawTextW(ncd->hdc, label, -1, &rcPageNo, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        // Reduce the size of the label and cut off the page number
        rcItem.right = std::max(rcItem.right - txtSize.cx, 0);
        szText[str::Len(szText) - str::Len(label)] = '\0';
    }

    SetTextColor(ncd->hdc, ntvcd->clrText);
    SetBkColor(ncd->hdc, ntvcd->clrTextBk);

    // Draw the focus rectangle (including proper background color)
    HBRUSH brushBg = CreateSolidBrush(ntvcd->clrTextBk);
    FillRect(ncd->hdc, &rcItem, brushBg);
    DeleteObject(brushBg);
    if ((ncd->uItemState & CDIS_FOCUS)) DrawFocusRect(ncd->hdc, &rcItem);

    InflateRect(&rcItem, -2, -1);
    DrawTextW(ncd->hdc, szText, -1, &rcItem, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_WORD_ELLIPSIS);
}
#endif

struct GoToTocLinkData {
    WindowTab* tab = nullptr;
    DocController* ctrl = nullptr;
    Kind destKind = nullptr;
    int pageNo = 0;
    char* url = nullptr;
    char* mobiScrollName = nullptr;
    char* mupdfUri = nullptr;
    float mupdfDestX = DEST_USE_DEFAULT;
    float mupdfDestY = DEST_USE_DEFAULT;
    int mupdfReflowOutlineChapter = -1;
};

static void FreeGoToTocLinkData(GoToTocLinkData* d) {
    if (!d) {
        return;
    }
    str::Free(d->url);
    str::Free(d->mobiScrollName);
    str::Free(d->mupdfUri);
}

static void CaptureGoToTocLinkData(GoToTocLinkData* data, TocItem* tocItem) {
    data->pageNo = tocItem->pageNo;
    IPageDestination* dest = tocItem->GetPageDestination();
    if (!dest) {
        return;
    }
    data->destKind = dest->GetKind();
    if (dest->GetKind() == kindDestinationMupdf) {
        EngineMupdfSnapshotOutlineLink(dest, &data->mupdfUri, &data->mupdfReflowOutlineChapter, &data->mupdfDestX,
                                       &data->mupdfDestY);
        if (data->pageNo <= 0 && data->ctrl) {
            DisplayModel* dm = data->ctrl->AsFixed();
            EngineBase* engine = dm ? dm->GetEngine() : nullptr;
            if (engine) {
                int pageNo = EngineMupdfFastOutlinePageNo(engine, dest);
                if (pageNo > 0) {
                    data->pageNo = pageNo;
                }
            }
        }
    } else if (dest->GetKind() == kindDestinationScrollTo) {
        char* name = PageDestGetName(dest);
        if (name && *name) {
            data->mobiScrollName = str::Dup(name);
        }
    } else if (dest->GetKind() == kindDestinationLaunchURL) {
        auto* urlDest = (PageDestinationURL*)dest;
        if (urlDest->url) {
            data->url = str::Dup(urlDest->url);
        }
    }
}

static bool IsScrollToLink(IPageDestination* link) {
    if (!link) {
        return false;
    }
    auto kind = link->GetKind();
    return kind == kindDestinationScrollTo;
}

static bool IsTocInternalPageItem(TocItem* tocItem, DocController* ctrl = nullptr) {
    if (!tocItem) {
        return false;
    }
    if (tocItem->pageNo > 0) {
        return true;
    }
    IPageDestination* dest = tocItem->GetPageDestination();
    if (!dest) {
        return false;
    }
    if (dest->GetKind() == kindDestinationMupdf) {
        return true;
    }
    if (ctrl && dest->GetKind() == kindDestinationScrollTo) {
        DisplayModel* dm = ctrl->AsFixed();
        EngineBase* engine = dm ? dm->GetEngine() : nullptr;
        if (engine && engine->kind == kindEngineMobi && PageDestGetName(dest)) {
            return true;
        }
    }
    return false;
}

static bool IsMobiEbookTocItemReachable(DocController* ctrl, TocItem* tocItem, EngineBase* engine) {
    if (!ctrl || !tocItem || !engine || engine->kind != kindEngineMobi) {
        return true;
    }
    if (!EngineIsProgressiveEbookLoading(engine)) {
        if (tocItem->pageNo > 0) {
            return ctrl->ValidPageNo(tocItem->pageNo);
        }
        return true;
    }
    IPageDestination* dest = tocItem->GetPageDestination();
    int filePos = EngineEbookParseTocLinkFilePos(engine, dest);
    if (filePos >= 0) {
        return EngineEbookIsTocFilePosReachable(engine, filePos);
    }
    if (tocItem->pageNo > 0) {
        return tocItem->pageNo <= EngineEbookGetFormattedPageCount(engine);
    }
    if (dest && dest->GetKind() == kindDestinationScrollTo && PageDestGetName(dest)) {
        return true;
    }
    return false;
}

bool IsInternalPageLinkReachable(DocController* ctrl, IPageDestination* dest) {
    if (!ctrl || !dest) {
        return true;
    }
    Kind kind = dest->GetKind();
    if (kind == kindDestinationLaunchURL || kind == kindDestinationLaunchFile) {
        return true;
    }
    DisplayModel* dm = ctrl->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || !EngineIsProgressiveEbookLoading(engine)) {
        return true;
    }
    if (kind == kindDestinationMupdf) {
        return EngineMupdfIsOutlineDestReachable(engine, dest);
    }
    if (engine->kind == kindEngineMobi) {
        int filePos = EngineEbookParseTocLinkFilePos(engine, dest);
        if (filePos >= 0) {
            return EngineEbookIsTocFilePosReachable(engine, filePos);
        }
        int pageNo = PageDestGetPageNo(dest);
        if (pageNo > 0) {
            return pageNo <= EngineEbookGetFormattedPageCount(engine);
        }
        if (kind == kindDestinationScrollTo && PageDestGetName(dest)) {
            return true;
        }
    }
    int pageNo = PageDestGetPageNo(dest);
    if (pageNo > 0) {
        return ctrl->ValidPageNo(pageNo);
    }
    return true;
}

bool IsPageElementLinkReachable(DocController* ctrl, IPageElement* el) {
    if (!ctrl || !el || !el->Is(kindPageElementDest)) {
        return true;
    }
    return IsInternalPageLinkReachable(ctrl, ((PageElementDestination*)el)->dest);
}

static bool IsTocPageReachable(DocController* ctrl, TocItem* tocItem) {
    if (!ctrl || !tocItem) {
        return true;
    }
    DisplayModel* dm = ctrl->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (engine && engine->kind == kindEngineMupdf) {
        if (EngineMupdfReflowTocNeedsUiReload(engine)) {
            return true;
        }
    }
    if (engine && engine->kind == kindEngineMobi) {
        // OnTocCustomDraw calls this while painting; never dereference dest here.
        return IsMobiEbookTocItemReachable(ctrl, tocItem, engine);
    }
    if (engine && engine->kind == kindEngineMupdf && !EngineIsProgressiveEbookLoading(engine)) {
        IPageDestination* dest = tocItem->GetPageDestination();
        int pageNo = EngineMupdfTocItemPageNoForSync(engine, dest, tocItem->pageNo);
        if (pageNo > 0) {
            return pageNo <= engine->PageCount();
        }
        return false;
    }
    IPageDestination* dest = tocItem->GetPageDestination();
    if (dest && dest->GetKind() == kindDestinationMupdf && engine && EngineIsProgressiveEbookLoading(engine)) {
        return IsInternalPageLinkReachable(ctrl, dest);
    }
    if (tocItem->pageNo <= 0) {
        if (engine && dest && dest->GetKind() == kindDestinationMupdf && IsTocInternalPageItem(tocItem)) {
            int pageNo = EngineMupdfFastOutlinePageNo(engine, dest);
            if (pageNo > 0) {
                return IsTocMupdfPageReachable(engine, pageNo);
            }
            if (EngineIsProgressiveEbookLoading(engine)) {
                // not counted yet: grey and unclickable until counting reaches it
                return false;
            }
            return false;
        }
        return true;
    }
    if (engine && engine->kind == kindEngineMupdf) {
        return IsTocMupdfPageReachable(engine, tocItem->pageNo);
    }
    if (engine && EngineIsProgressiveEbookLoading(engine)) {
        return tocItem->pageNo <= engine->PageCount();
    }
    return ctrl->ValidPageNo(tocItem->pageNo);
}

static void GoToTocLink(GoToTocLinkData* d) {
    AutoDelete delData(d);
    LARGE_INTEGER tocStart{};
    if (EpubPerfLogIsEnabled()) {
        tocStart = TimeGet();
    }
    defer {
        if (EpubPerfLogIsEnabled() && d->mupdfUri) {
            TempStr kv = str::FormatTemp("\"uri\":\"%s\",\"ms\":%.2f,\"page\":%d", d->mupdfUri, TimeSinceInMs(tocStart),
                                         d->pageNo);
            EpubPerfLogEmit("toc_jump", kv);
        }
        FreeGoToTocLinkData(d);
    };

    auto tab = d->tab;
    auto ctrl = d->ctrl;

    // validate tab before dereferencing 鈥?it may have been freed
    // while this task was queued (e.g. user closed the tab/window)
    if (!IsWindowTabValid(tab)) {
        return;
    }
    MainWindow* win = tab->win;
    if (!IsMainWindowValid(win) || win->CurrentTab() != tab || tab->ctrl != ctrl) {
        return;
    }

    win->tocKeepSelection = true;
    defer {
        win->tocKeepSelection = false;
    };

    DisplayModel* dm = ctrl->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    int navPage = d->pageNo;

    if (d->destKind == kindDestinationMupdf && d->mupdfUri && engine) {
        bool loaded = !EngineIsProgressiveEbookLoading(engine);
        bool hasFragment = str::FindChar(d->mupdfUri, '#') != nullptr;
        bool isPdf = engine->kind == kindEngineMupdf && str::EqI(engine->defaultExt, ".pdf");
        // Chapter-start page numbers are shared by every #fragment entry in one spine
        // HTML file (common in anthology EPUBs). Only plain chapter links can fast-path.
        // PDF #page= URIs are the dest page; after 对准印刷目录 pin, pageNo is newer
        // than a stale outline URI, so prefer pageNo.
        if (loaded && navPage > 0 && (!hasFragment || isPdf)) {
            ctrl->PreparePageNavigation(navPage);
            if (navPage >= 1 && navPage <= engine->PageCount()) {
                if (d->mupdfDestX != DEST_USE_DEFAULT || d->mupdfDestY != DEST_USE_DEFAULT) {
                    float x = d->mupdfDestX != DEST_USE_DEFAULT ? d->mupdfDestX : 0.f;
                    float y = d->mupdfDestY != DEST_USE_DEFAULT ? d->mupdfDestY : 0.f;
                    ctrl->ScrollTo(navPage, RectF(x, y, 0, 0), 0);
                } else {
                    ctrl->GoToPage(navPage, true);
                }
                return;
            }
        }
        EngineMupdfNavigateUri(engine, d->mupdfUri, d->mupdfReflowOutlineChapter, d->mupdfDestX, d->mupdfDestY,
                               win->linkHandler);
        return;
    }

    if (dm && navPage > 0) {
        ctrl->PreparePageNavigation(navPage);
    }

    if (d->mobiScrollName && engine && engine->kind == kindEngineMobi) {
        IPageDestination* resolvedDest = engine->GetNamedDest(d->mobiScrollName);
        if (resolvedDest) {
            ctrl->HandleLink(resolvedDest, win->linkHandler);
            delete resolvedDest;
        }
    } else if (d->url) {
        win->linkHandler->LaunchURL(d->url);
    } else if (navPage > 0) {
        ctrl->GoToPage(navPage, true);
    }
}

static void GoToTocTreeItem(MainWindow* win, TreeItem ti, bool allowExternal) {
    if (!ti) {
        return;
    }
    TocItem* tocItem = (TocItem*)ti;
    bool isInternalPage = IsTocInternalPageItem(tocItem, win->ctrl);
    bool isScroll = IsScrollToLink(tocItem->GetPageDestination());
    if (isInternalPage && !IsTocPageReachable(win->ctrl, tocItem)) {
        return;
    }
    if (isInternalPage || (allowExternal || isScroll)) {
        // delay changing the page until the tree messages have been handled
        auto data = new GoToTocLinkData;
        data->ctrl = win->ctrl;
        data->tab = win->CurrentTab();
        CaptureGoToTocLinkData(data, tocItem);
        auto fn = MkFunc0<GoToTocLinkData>(GoToTocLink, data);
        uitask::Post(fn, "TaskGoToTocTreeItem");
    }
}

static void ClearTocMultiSelect(MainWindow* win);
static void TocCancelDrag(MainWindow* win);
static void UpdateTocCalibrateHeader(MainWindow* win);

void ClearTocBox(MainWindow* win) {
    if (!win->tocLoaded) {
        return;
    }

    win->tocLabelEditItem = nullptr;
    win->tocLabelEditFromF2 = false;

    // set tocLoaded to false before SetText("") because SetText triggers
    // EN_CHANGE synchronously which calls ApplyTocFilter() re-entrantly
    // and we need it to bail out early
    win->tocLoaded = false;

    WindowTab* tab = win->CurrentTab();
    if (tab) {
        tab->currToc = nullptr;
    }

    win->tocTreeView->Clear();
    ClearTocMultiSelect(win);
    TocCancelDrag(win);

    // clear filter state
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    win->currPageNo = 0;
    UpdateTocCalibrateHeader(win);
}

void ClearTocBoxForTabSwitch(MainWindow* win) {
    HideTocCalib(win);
    if (!win->tocLoaded) {
        return;
    }

    win->tocLoaded = false;
    win->tocLabelEditItem = nullptr;
    win->tocLabelEditFromF2 = false;

    if (win->tocTreeView) {
        win->tocTreeView->treeModel = nullptr;
    }
    ClearTocMultiSelect(win);
    TocCancelDrag(win);

    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    win->currPageNo = 0;
}

void ToggleTocBox(MainWindow* win) {
    if (!win->IsDocLoaded()) {
        return;
    }
    if (win->tocVisible) {
        SetSidebarVisibility(win, false, gGlobalPrefs->showFavorites);
        return;
    }
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    if (win->tocVisible && win->tocTreeView) {
        HwndSetFocus(win->tocTreeView->hwnd);
    }
}

static int TocItemPageNoForMatch(TocItem* item, EngineBase* engine) {
    if (!item) {
        return 0;
    }
    if (!engine) {
        return item->pageNo > 0 ? item->pageNo : 0;
    }

    IPageDestination* dest = item->GetPageDestination();
    if (!dest) {
        return item->pageNo > 0 ? item->pageNo : 0;
    }

    Kind destKind = dest->GetKind();
    if (destKind == kindDestinationMupdf) {
        if (!EngineIsProgressiveEbookLoading(engine)) {
            int syncPage = EngineMupdfTocItemPageNoForSync(engine, dest, item->pageNo);
            if (syncPage > 0) {
                return syncPage;
            }
        }
        if (item->pageNo > 0) {
            return item->pageNo;
        }
        int pageNo = EngineMupdfFastOutlinePageNo(engine, dest);
        if (pageNo > 0) {
            return pageNo;
        }
        if (EngineIsProgressiveEbookLoading(engine)) {
            return 0;
        }
        return EngineMupdfResolveLinkPageNo(engine, dest);
    }

    if (item->pageNo > 0) {
        return item->pageNo;
    }

    if (engine->kind == kindEngineMobi) {
        int filePos = EngineEbookParseTocLinkFilePos(engine, dest);
        if (filePos >= 0) {
            return EngineEbookPageNoForTocFilePos(engine, filePos);
        }
    }

    if (destKind == kindDestinationScrollTo) {
        char* name = PageDestGetName(dest);
        if (name) {
            IPageDestination* resolved = engine->GetNamedDest(name);
            if (resolved) {
                int pageNo = PageDestGetPageNo(resolved);
                delete resolved;
                return pageNo;
            }
        }
    }

    return 0;
}

int CountTocItems(TocItem* item) {
    int n = 0;
    for (; item; item = item->next) {
        n++;
        n += CountTocItems(item->child);
    }
    return n;
}

// Pick the deepest TOC entry whose page is <= pageNo. Among siblings with the same
// page (common when EPUB fragments fail to resolve), keep the earliest entry so we
// do not jump ahead to a later chapter while still reading earlier content.
TocItem* TocItemBestMatchForPage(TocItem* item, int pageNo, EngineBase* engine) {
    TocItem* currItem = nullptr;

    for (; item; item = item->next) {
        int itemPage = TocItemPageNoForMatch(item, engine);
        if (1 <= itemPage && itemPage <= pageNo) {
            int currPage = currItem ? TocItemPageNoForMatch(currItem, engine) : 0;
            if (!currItem || itemPage > currPage) {
                currItem = item;
            }
        }
        if (itemPage > pageNo) {
            break;
        }

        TocItem* subItem = TocItemBestMatchForPage(item->child, pageNo, engine);
        if (subItem) {
            currItem = subItem;
        }
    }

    return currItem;
}

static bool IsKnownTocTreeModel(MainWindow* win, TreeModel* tm) {
    if (!win || !tm) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    if (tab && tab->currToc && tm == tab->currToc) {
        return true;
    }
    if (win->tocFilteredTree && tm == win->tocFilteredTree) {
        return true;
    }
    return false;
}

// find the closest item in tree view to a given page number
static TocItem* TreeItemForPageNo(TreeView* treeView, int pageNo) {
    if (!treeView) {
        return nullptr;
    }
    TreeModel* tm = treeView->treeModel;
    if (!tm) {
        return nullptr;
    }

    TocItem* root = (TocItem*)tm->Root();
    if (!root || !root->child) {
        return nullptr;
    }
    // if there's only one item, we want to unselect it so that it can
    // be selected by the user
    if (CountTocItems(root->child) < 2) {
        return nullptr;
    }
    MainWindow* win = FindMainWindowByHwnd(treeView->hwnd);
    EngineBase* engine = nullptr;
    if (win && win->ctrl) {
        DisplayModel* dm = win->ctrl->AsFixed();
        engine = dm ? dm->GetEngine() : nullptr;
    }
    return TocItemBestMatchForPage(root->child, pageNo, engine);
}

// TODO: I can't use TreeItem->IsExpanded() because it's not in sync with
// the changes user makes to TreeCtrl
static TocItem* FindVisibleParentTreeItem(TreeView* treeView, TocItem* ti) {
    if (!ti) {
        return nullptr;
    }
    while (true) {
        auto parent = ti->parent;
        if (parent == nullptr) {
            // ti is a root node
            return ti;
        }
        if (treeView->IsExpanded((TreeItem)parent)) {
            return ti;
        }
        ti = parent;
    }
    return nullptr;
}

void UpdateTocSelection(MainWindow* win, int currPageNo) {
    if (!win->tocLoaded || !win->tocVisible || win->tocKeepSelection) {
        return;
    }
    if (TocCalibIsActive(win)) {
        return;
    }
    if (win->tocDragging || win->tocSelectedIds.Size() > 1) {
        return;
    }

    auto treeView = win->tocTreeView;
    if (!treeView || !treeView->treeModel) {
        return;
    }
    if (!IsKnownTocTreeModel(win, treeView->treeModel)) {
        treeView->treeModel = nullptr;
        return;
    }
    WindowTab* tab = win->CurrentTab();
    DisplayModel* dm = tab && tab->ctrl ? tab->ctrl->AsFixed() : nullptr;
    if (dm && dm->ShouldSkipTocSelectionUpdate()) {
        return;
    }

    auto item = TreeItemForPageNo(treeView, currPageNo);
    // only select the items that are visible i.e. are top nodes or
    // children of expanded node
    TreeItem toSelect = (TreeItem)FindVisibleParentTreeItem(treeView, item);
    treeView->SelectItem(toSelect);
    if (toSelect != TreeModel::kNullItem) {
        TocItem* tocItem = (TocItem*)toSelect;
        win->tocSelectedIds.Reset();
        if (tocItem && tocItem->id) {
            win->tocSelectedIds.Append(tocItem->id);
            win->tocAnchorId = tocItem->id;
        }
        win->tocSelectionOwned = true;
        InvalidateTocTree(win);
    }
}

static void UpdateDocTocExpansionStateRecur(TreeView* treeView, Vec<int>& tocState, TocItem* tocItem) {
    while (tocItem) {
        // items without children cannot be toggled
        if (tocItem->child) {
            // we have to query the state of the tree view item because
            // isOpenToggled is not kept in sync
            // TODO: keep toggle state on TocItem in sync
            // by subscribing to the right notifications
            bool isExpanded = treeView->IsExpanded((TreeItem)tocItem);
            bool wasToggled = isExpanded != tocItem->isOpenDefault;
            if (wasToggled) {
                tocState.Append(tocItem->id);
            }
            UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem->child);
        }
        tocItem = tocItem->next;
    }
}

void UpdateTocExpansionState(Vec<int>& tocState, TreeView* treeView, TocTree* docTree) {
    if (treeView->treeModel != docTree) {
        // CrashMe();
        return;
    }
    tocState.Reset();
    TocItem* tocItem = docTree->root->child;
    UpdateDocTocExpansionStateRecur(treeView, tocState, tocItem);
}

static bool inRange(WCHAR c, WCHAR low, WCHAR hi) {
    return (low <= c) && (c <= hi);
}

// copied from mupdf/fitz/dev_text.c
// clang-format off
static bool isLeftToRightChar(WCHAR c) {
    return (
        inRange(c, 0x0041, 0x005A) ||
        inRange(c, 0x0061, 0x007A) ||
        inRange(c, 0xFB00, 0xFB06)
    );
}

static bool isRightToLeftChar(WCHAR c) {
    return (
        inRange(c, 0x0590, 0x05FF) ||
        inRange(c, 0x0600, 0x06FF) ||
        inRange(c, 0x0750, 0x077F) ||
        inRange(c, 0xFB50, 0xFDFF) ||
        inRange(c, 0xFE70, 0xFEFE)
    );
}
// clang-format off

static void GetLeftRightCounts(TocItem* node, int& l2r, int& r2l) {
next:
    if (!node) {
        return;
    }
    // short-circuit because this could overflow the stack due to recursion
    // (happened in doc from https://github.com/sumatrapdfreader/sumatrapdf/issues/1795)
    if (l2r + r2l > 1024) {
        return;
    }
    if (node->title) {
        TempWStr ws = ToWStrTemp(node->title);
        for (const WCHAR* c = ws; *c; c++) {
            if (isLeftToRightChar(*c)) {
                l2r++;
            } else if (isRightToLeftChar(*c)) {
                r2l++;
            }
        }
    }
    GetLeftRightCounts(node->child, l2r, r2l);
    // could be: GetLeftRightCounts(node->next, l2r, r2l);
    // but faster if not recursive
    node = node->next;
    goto next;
}

static void SetInitialExpandState(TocItem* item, Vec<int>& tocState) {
    while (item) {
        item->isOpenToggled = tocState.Contains(item->id);
        SetInitialExpandState(item->child, tocState);
        item = item->next;
    }
}

static void AddFavoriteFromToc(MainWindow* win, TocItem* dti) {
    int pageNo = 0;
    if (!dti) {
        return;
    }
    if (dti->dest) {
        pageNo = PageDestGetPageNo(dti->dest);
    }
    char* name = dti->title;
    TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel, name);
}

static void SaveAttachment(WindowTab* tab, const char* fileName, int attachmentNo) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    ByteSlice data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (data.empty()) {
        return;
    }
    char* dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data.data());
}

static void OpenAttachment(WindowTab* tab, const char* fileName, int attachmentNo) {
    EngineBase* engine = tab->AsFixed()->GetEngine();
    ByteSlice data = EngineMupdfLoadAttachment(engine, attachmentNo);
    if (data.empty()) {
        return;
    }
    MainWindow* win = tab->win;
    EngineBase* newEngine = CreateEngineMupdfFromData(data, fileName, nullptr);
    DocController* ctrl = CreateControllerForEngineOrFile(newEngine, nullptr, nullptr, win);
    LoadArgs* args = new LoadArgs(tab->filePath, win);    
    args->ctrl = ctrl;
    LoadDocumentFinish(args);
    str::Free(data.data());
}

static void OpenEmbeddedFile(WindowTab* tab, IPageDestination* dest) {
    ReportIf(!tab || !dest);
    if (!tab || !dest) {
        return;
    }
    MainWindow* win = tab->win;
    PageDestinationFile *destFile = (PageDestinationFile*)dest;
    char* path = destFile->path;
    const char* tabPath = tab->filePath;
    if (!str::StartsWith(path, tabPath)) {
        return;
    }
    LoadArgs args(path, win);
    args.activateExisting = true;
    LoadDocument(&args);
}

static void SaveEmbeddedFile(WindowTab* tab, const char* srcPath, const char* fileName) {
    ByteSlice data = LoadEmbeddedPDFFile(srcPath);
    if (data.empty()) {
        // TODO: show an error message
        return;
    }
    char* dir = path::GetDirTemp(tab->filePath);
    fileName = path::GetBaseNameTemp(fileName);
    TempStr dstPath = path::JoinTemp(dir, fileName);
    SaveDataToFile(tab->win->hwndFrame, dstPath, data);
    str::Free(data.data());
}

static EngineBase* PdfTocEditableEngine(MainWindow* win) {
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    return engine && EngineMupdfCanEditPdfToc(engine) ? engine : nullptr;
}

static TocItem* FindTocItemById(TocItem* item, int id) {
    for (; item; item = item->next) {
        if (item->id == id) {
            return item;
        }
        TocItem* found = FindTocItemById(item->child, id);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static TocItem* OriginalPdfTocItem(MainWindow* win, TocItem* item) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!item || !tab || !tab->currToc || !tab->currToc->root) {
        return item;
    }
    return FindTocItemById(tab->currToc->root->child, item->id);
}

static bool PdfTocPathForItem(MainWindow* win, TocItem* item, Vec<int>& pathOut) {
    pathOut.Clear();
    item = OriginalPdfTocItem(win, item);
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!item || !tab || !tab->currToc || !tab->currToc->root) {
        return false;
    }
    Vec<int> reverse;
    for (TocItem* curr = item; curr && curr != tab->currToc->root; curr = curr->parent) {
        TocItem* first = (curr->parent && curr->parent != tab->currToc->root) ? curr->parent->child
                                                                             : tab->currToc->root->child;
        int idx = 0;
        while (first && first != curr) {
            idx++;
            first = first->next;
        }
        if (!first) {
            return false;
        }
        reverse.Append(idx);
    }
    for (int i = reverse.Size() - 1; i >= 0; i--) {
        pathOut.Append(reverse.At(i));
    }
    return true;
}

static TocItem* PdfTocItemAtPath(TocItem* root, const Vec<int>& path) {
    TocItem* item = root;
    for (int depth = 0; depth < path.Size(); depth++) {
        int idx = path.At(depth);
        for (int i = 0; item && i < idx; i++) {
            item = item->next;
        }
        if (!item) {
            return nullptr;
        }
        if (depth + 1 < path.Size()) {
            item = item->child;
        }
    }
    return item;
}

static bool HasTocFilter(MainWindow* win);
static Vec<TocItem*> TocVisibleItems(TreeView* tv);

static bool IsPdfTocBookmarkItem(MainWindow* win, TocItem* item) {
    item = OriginalPdfTocItem(win, item);
    if (!item) {
        return false;
    }
    if (item->dest && item->dest->GetKind() == kindDestinationMupdf) {
        return true;
    }
    return TocCalibIsActive(win) && item->id != 0;
}

static void TocInvalidateTree(MainWindow* win) {
    if (win && win->tocTreeView && win->tocTreeView->hwnd) {
        InvalidateRect(win->tocTreeView->hwnd, nullptr, FALSE);
    }
}

static void ClearTocMultiSelect(MainWindow* win) {
    if (!win) {
        return;
    }
    win->tocSelectedIds.Reset();
    win->tocAnchorId = 0;
    win->tocSelectionOwned = false;
}

static bool TocItemIdSelected(MainWindow* win, int id) {
    return win && id != 0 && win->tocSelectedIds.Contains(id);
}

static bool TocItemIsMultiSelected(MainWindow* win, TocItem* item) {
    return item && TocItemIdSelected(win, item->id);
}

static TocItem* TocItemFromId(MainWindow* win, int id) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !tab->currToc || !tab->currToc->root || id == 0) {
        return nullptr;
    }
    return FindTocItemById(tab->currToc->root->child, id);
}

static void TocFocusItem(MainWindow* win, TocItem* item, bool navigate) {
    if (!win || !win->tocTreeView || !win->tocTreeView->treeModel || !win->tocTreeView->hwnd) {
        return;
    }
    win->tocSuppressGoTo = true;
    win->tocKeepSelection = true;
    win->tocTreeView->SelectItem(item ? (TreeItem)item : TreeModel::kNullItem);
    win->tocKeepSelection = false;
    win->tocSuppressGoTo = false;
    if (navigate && item) {
        GoToTocTreeItem(win, (TreeItem)item, true);
    }
}

static void TocSetSelectedIds(MainWindow* win, const Vec<int>& ids, int anchorId, TocItem* focus, bool navigate) {
    if (!win) {
        return;
    }
    win->tocSelectedIds.Reset();
    for (int id : ids) {
        if (id && !win->tocSelectedIds.Contains(id)) {
            win->tocSelectedIds.Append(id);
        }
    }
    win->tocAnchorId = anchorId;
    win->tocSelectionOwned = true;
    if (focus) {
        TocFocusItem(win, focus, navigate);
    }
    TocInvalidateTree(win);
}

static void TocSelectOnly(MainWindow* win, TocItem* item, bool navigate) {
    Vec<int> ids;
    if (item) {
        ids.Append(item->id);
    }
    TocSetSelectedIds(win, ids, item ? item->id : 0, item, navigate);
}

static void CollectTocItemIds(TocItem* item, Vec<int>& ids) {
    for (; item; item = item->next) {
        if (item->id) {
            ids.Append(item->id);
        }
        CollectTocItemIds(item->child, ids);
    }
}

static Vec<TocItem*> TocSelectedBookmarkItems(MainWindow* win) {
    Vec<TocItem*> items;
    if (!win) {
        return items;
    }
    if (win->tocSelectedIds.empty()) {
        TocItem* selected = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
        selected = OriginalPdfTocItem(win, selected);
        if (IsPdfTocBookmarkItem(win, selected)) {
            items.Append(selected);
        }
        return items;
    }
    for (int id : win->tocSelectedIds) {
        TocItem* item = OriginalPdfTocItem(win, TocItemFromId(win, id));
        if (IsPdfTocBookmarkItem(win, item) && !items.Contains(item)) {
            items.Append(item);
        }
    }
    return items;
}

static bool TocCollectSelectedPaths(MainWindow* win, Vec<PdfTocPath>& pathsOut) {
    pathsOut.Clear();
    Vec<TocItem*> items = TocSelectedBookmarkItems(win);
    for (TocItem* item : items) {
        Vec<int> path;
        if (!PdfTocPathForItem(win, item, path)) {
            return false;
        }
        PdfTocPath p;
        PdfTocPathFromVec(p, path);
        pathsOut.Append(p);
    }
    return !pathsOut.empty();
}

constexpr UINT_PTR kTocDragExpandTimerId = 0x7160;

static void TocCancelDrag(MainWindow* win) {
    if (!win) {
        return;
    }
    HWND hwnd = win->tocTreeView ? win->tocTreeView->hwnd : nullptr;
    if (hwnd && GetCapture() == hwnd) {
        ReleaseCapture();
    }
    if (hwnd) {
        KillTimer(hwnd, kTocDragExpandTimerId);
    }
    bool wasDragging = win->tocDragging;
    win->tocDragArmed = false;
    win->tocDragging = false;
    win->tocDropItem = nullptr;
    win->tocDropPos = 1;
    if (wasDragging) {
        TocInvalidateTree(win);
    }
}

static bool ConfirmPdfTocSignatureEdit(MainWindow* win, EngineBase* engine) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || tab->acceptedPdfTocSignatureWarning || !EngineMupdfPdfHasSignatures(engine)) {
        return true;
    }
    int res = MessageBoxW(win->hwndFrame,
                          L"Editing the table of contents changes this PDF after it was digitally signed. The "
                          L"existing signature will remain, but viewers will report that the document was modified. "
                          L"Continue?",
                          L"Digitally signed PDF", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (res != IDYES) {
        return false;
    }
    tab->acceptedPdfTocSignatureWarning = true;
    return true;
}

static char* CurrentPdfTocTarget(MainWindow* win, EngineBase* engine) {
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    if (!dm) {
        return nullptr;
    }
    ScrollState state = dm->GetScrollState();
    float x = state.x >= 0 ? (float)state.x : 0.f;
    float y = state.y >= 0 ? (float)state.y : 0.f;
    return EngineMupdfFormatPdfTocTarget(engine, state.page, x, y);
}

struct TocTreeExpandNode {
    int path[16]{};
    int pathLen = 0;
    bool expanded = false;
};

struct TocTreeViewKeep {
    int firstVisibleIndex = 0;
    int firstVisiblePath[16]{};
    int firstVisiblePathLen = 0;
    bool scrollValid = false;
    Vec<TocTreeExpandNode> expand;
};

static void TocCopyPathToFixed(int* dst, int* dstLen, int dstCap, const Vec<int>& path) {
    int n = path.Size();
    if (n > dstCap) {
        n = dstCap;
    }
    for (int i = 0; i < n; i++) {
        dst[i] = path[i];
    }
    *dstLen = n;
}

static void TocFixedPathToVec(const int* src, int srcLen, Vec<int>& path) {
    path.Clear();
    for (int i = 0; i < srcLen; i++) {
        path.Append(src[i]);
    }
}

static int TocTreeVisibleIndex(HWND hwnd, HTREEITEM target) {
    if (!hwnd || !target) {
        return 0;
    }
    int n = 0;
    for (HTREEITEM h = TreeView_GetRoot(hwnd); h; h = TreeView_GetNextVisible(hwnd, h)) {
        if (h == target) {
            return n;
        }
        n++;
        if (n > 200000) {
            break;
        }
    }
    return 0;
}

static HTREEITEM TocTreeItemAtVisibleIndex(HWND hwnd, int index) {
    if (!hwnd || index < 0) {
        return nullptr;
    }
    HTREEITEM h = TreeView_GetRoot(hwnd);
    while (h && index > 0) {
        h = TreeView_GetNextVisible(hwnd, h);
        index--;
    }
    return h;
}

static void TocCollectExpandNodes(MainWindow* win, TocItem* item, Vec<TocTreeExpandNode>& out) {
    if (!win || !win->tocTreeView) {
        return;
    }
    for (; item; item = item->next) {
        if (item->child) {
            Vec<int> path;
            if (PdfTocPathForItem(win, item, path)) {
                TocTreeExpandNode n{};
                TocCopyPathToFixed(n.path, &n.pathLen, dimof(n.path), path);
                n.expanded = win->tocTreeView->IsExpanded((TreeItem)item);
                out.Append(n);
            }
            TocCollectExpandNodes(win, item->child, out);
        }
    }
}

TocTreeViewKeep* TocTreeViewKeepStart(MainWindow* win) {
    auto* keep = new TocTreeViewKeep;
    if (!win || !win->tocLoaded || !win->tocTreeView || !win->tocTreeView->hwnd) {
        return keep;
    }
    WindowTab* tab = win->CurrentTab();
    if (tab && tab->currToc && tab->currToc->root) {
        TocCollectExpandNodes(win, tab->currToc->root->child, keep->expand);
    }
    HWND hwnd = win->tocTreeView->hwnd;
    HTREEITEM hFirst = TreeView_GetFirstVisible(hwnd);
    if (!hFirst) {
        return keep;
    }
    keep->firstVisibleIndex = TocTreeVisibleIndex(hwnd, hFirst);
    TocItem* item = (TocItem*)win->tocTreeView->GetTreeItemByHandle(hFirst);
    Vec<int> path;
    if (item && PdfTocPathForItem(win, item, path)) {
        TocCopyPathToFixed(keep->firstVisiblePath, &keep->firstVisiblePathLen, dimof(keep->firstVisiblePath), path);
    }
    keep->scrollValid = true;
    return keep;
}

void TocTreeViewKeepFinish(MainWindow* win, TocTreeViewKeep* keep) {
    if (!keep) {
        return;
    }
    if (win && win->tocLoaded && win->tocTreeView && win->tocTreeView->hwnd) {
        HWND hwnd = win->tocTreeView->hwnd;
        WindowTab* tab = win->CurrentTab();
        TocItem* root = tab && tab->currToc && tab->currToc->root ? tab->currToc->root->child : nullptr;
        if (root && keep->expand.Size() > 0) {
            for (int i = 0; i < keep->expand.Size(); i++) {
                const TocTreeExpandNode& n = keep->expand[i];
                Vec<int> path;
                TocFixedPathToVec(n.path, n.pathLen, path);
                TocItem* item = PdfTocItemAtPath(root, path);
                if (!item || !item->child) {
                    continue;
                }
                HTREEITEM h = win->tocTreeView->GetHandleByTreeItem((TreeItem)item);
                if (h) {
                    TreeView_Expand(hwnd, h, n.expanded ? TVE_EXPAND : TVE_COLLAPSE);
                }
            }
        }
        if (keep->scrollValid) {
            HTREEITEM hFirst = nullptr;
            if (root && keep->firstVisiblePathLen > 0) {
                Vec<int> path;
                TocFixedPathToVec(keep->firstVisiblePath, keep->firstVisiblePathLen, path);
                TocItem* item = PdfTocItemAtPath(root, path);
                if (item) {
                    hFirst = win->tocTreeView->GetHandleByTreeItem((TreeItem)item);
                }
            }
            if (!hFirst) {
                hFirst = TocTreeItemAtVisibleIndex(hwnd, keep->firstVisibleIndex);
            }
            if (hFirst) {
                TreeView_SelectSetFirstVisible(hwnd, hFirst);
            }
        }
    }
    delete keep;
}

static void ReloadPdfTocAfterEdit(MainWindow* win, const Vec<PdfTocPath>& selectedPaths) {
    if (!win) {
        return;
    }
    TocTreeViewKeep* keep = TocTreeViewKeepStart(win);
    bool prevSuppress = win->tocSuppressGoTo;
    win->tocSuppressGoTo = true;
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
    WindowTab* tab = win->CurrentTab();
    if (tab && tab->tocCalib) {
        TocCalibRebind(win);
    }
    if (tab && tab->currToc && tab->currToc->root) {
        Vec<int> ids;
        TocItem* focus = nullptr;
        for (int i = 0; i < selectedPaths.Size(); i++) {
            Vec<int> path;
            PdfTocPathToVec(selectedPaths.At(i), path);
            TocItem* item = PdfTocItemAtPath(tab->currToc->root->child, path);
            if (!item) {
                continue;
            }
            if (!ids.Contains(item->id)) {
                ids.Append(item->id);
            }
            if (!focus) {
                focus = item;
            }
        }
        if (focus) {
            TocSetSelectedIds(win, ids, focus->id, focus, false);
        }
    }
    TocTreeViewKeepFinish(win, keep);
    win->tocSuppressGoTo = prevSuppress;
}

static void ReloadPdfTocAfterEdit(MainWindow* win, const Vec<int>& selectedPath) {
    Vec<PdfTocPath> paths;
    if (!selectedPath.empty()) {
        PdfTocPath p;
        PdfTocPathFromVec(p, selectedPath);
        paths.Append(p);
    }
    ReloadPdfTocAfterEdit(win, paths);
}

static void ShowPdfTocEditError(MainWindow* win, const char* error) {
    TempWStr msg = ToWStrTemp(error ? error : "The PDF table of contents could not be modified.");
    MessageBoxW(win->hwndFrame, msg, L"PDF table of contents", MB_OK | MB_ICONERROR);
}

static bool ConfirmPdfTocDelete(MainWindow* win, int count, bool hasChildren, bool promoteChildren = false) {
    const char* content;
    if (promoteChildren && hasChildren) {
        content = count > 1 ? str::FormatTemp(_TRA("Delete %d TOC items? Child items will be moved up one level."), count)
                            : _TRA("Delete this TOC item? Child items will be moved up one level.");
    } else if (count > 1) {
        content = hasChildren ? str::FormatTemp(_TRA("Delete %d TOC items and their child items?"), count)
                              : str::FormatTemp(_TRA("Delete %d TOC items?"), count);
    } else {
        content = hasChildren ? _TRA("Delete this TOC item and all of its child items?")
                              : _TRA("Delete this TOC item?");
    }
    TASKDIALOG_BUTTON buttons[2]{};
    buttons[0].nButtonID = IDYES;
    buttons[0].pszButtonText = ToWStrTemp(_TRA("Yes"));
    buttons[1].nButtonID = IDNO;
    buttons[1].pszButtonText = ToWStrTemp(_TRA("No"));

    DWORD flags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    if (trans::IsCurrLangRtl()) {
        flags |= TDF_RTL_LAYOUT;
    }
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = win->hwndFrame;
    config.dwFlags = flags;
    config.pszWindowTitle = ToWStrTemp(_TRA("Delete PDF TOC Item"));
    config.pszContent = ToWStrTemp(content);
    config.pszMainIcon = TD_WARNING_ICON;
    config.nDefaultButton = IDNO;
    config.cButtons = dimof(buttons);
    config.pButtons = buttons;

    int pressed = 0;
    HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    return hr == S_OK && pressed == IDYES;
}

static void ExecutePdfTocEdit(MainWindow* win, TocItem* selected, PdfTocEditAction action) {
    if (TocCalibIsActive(win)) {
        if (action == PdfTocEditAction::Delete) {
            TocCalibHandleDelete(win);
            ToolbarUpdateStateForWindow(win, false);
            return;
        }
        TocCalibOutlineOp op;
        bool structural = false;
        if (action == PdfTocEditAction::MoveUp) {
            op = TocCalibOutlineOp::MoveUp;
            structural = true;
        } else if (action == PdfTocEditAction::MoveDown) {
            op = TocCalibOutlineOp::MoveDown;
            structural = true;
        } else if (action == PdfTocEditAction::Promote) {
            op = TocCalibOutlineOp::Promote;
            structural = true;
        } else if (action == PdfTocEditAction::Demote) {
            op = TocCalibOutlineOp::Demote;
            structural = true;
        }
        if (structural) {
            TocCalibHandleOutlineOp(win, op);
            ToolbarUpdateStateForWindow(win, false);
            return;
        }
    }
    EngineBase* engine = PdfTocEditableEngine(win);
    if (!engine || !ConfirmPdfTocSignatureEdit(win, engine)) {
        return;
    }
    selected = OriginalPdfTocItem(win, selected);
    Vec<int> path;
    if (selected && !PdfTocPathForItem(win, selected, path)) {
        return;
    }

    AutoFreeStr title;
    AutoFreeStr target;
    bool setTargetToCurrentView = false;
    bool needsTitle = action == PdfTocEditAction::AddAfter || action == PdfTocEditAction::AddChild ||
                      action == PdfTocEditAction::Update;
    if (needsTitle) {
        if (selected && action == PdfTocEditAction::Update) {
            title.SetCopy(selected->title);
        } else {
            TempStr pageLabel = win->ctrl->GetPageLabeTemp(win->ctrl->CurrentPageNo());
            title.SetCopy(str::FormatTemp(_TRA("Page %s"), pageLabel));
        }
        const char* dialogTitle =
            action == PdfTocEditAction::Update ? _TRA("Edit PDF TOC Item") : _TRA("Add PDF TOC Item");
        const char* prompt = _TRA("Title (target: current view):");
        TempStr editPrompt;
        if (action == PdfTocEditAction::Update) {
            const char* currentTarget = selected && selected->dest ? PageDestGetValue(selected->dest) : nullptr;
            editPrompt = str::FormatTemp(_TRA("Title (current target: %s):"),
                                         currentTarget ? currentTarget : _TRA("none"));
            prompt = editPrompt;
        }
        bool* setTarget = action == PdfTocEditAction::Update ? &setTargetToCurrentView : nullptr;
        if (!Dialog_PdfTocTitle(win->hwndFrame, dialogTitle, prompt, title, setTarget)) {
            return;
        }
    }

    if (action == PdfTocEditAction::AddAfter || action == PdfTocEditAction::AddChild) {
        target.Set(CurrentPdfTocTarget(win, engine));
        if (!target) {
            ShowPdfTocEditError(win, "The current PDF position could not be captured.");
            return;
        }
    } else if (action == PdfTocEditAction::Update) {
        if (setTargetToCurrentView) {
            target.Set(CurrentPdfTocTarget(win, engine));
            if (!target) {
                ShowPdfTocEditError(win, "The current PDF position could not be captured.");
                return;
            }
        }
    } else if (action == PdfTocEditAction::Delete) {
        if (!ConfirmPdfTocDelete(win, 1, selected && selected->child)) {
            return;
        }
    }

    Vec<int> resultPath;
    char* errorRaw = nullptr;
    const char* uri = target ? target.Get() : nullptr;
    bool ok = EngineMupdfEditPdfToc(engine, action, path, title.Get(), uri, &resultPath, &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return;
    }
    bool structuralMove = action == PdfTocEditAction::MoveUp || action == PdfTocEditAction::MoveDown ||
                          action == PdfTocEditAction::Promote || action == PdfTocEditAction::Demote;
    if (structuralMove && resultPath.empty()) {
        return;
    }
    ReloadPdfTocAfterEdit(win, resultPath);
    ToolbarUpdateStateForWindow(win, false);
}

static bool PdfTocEditIsBatchAction(PdfTocEditAction action) {
    return action == PdfTocEditAction::Delete || action == PdfTocEditAction::MoveUp ||
           action == PdfTocEditAction::MoveDown || action == PdfTocEditAction::Promote ||
           action == PdfTocEditAction::Demote;
}

static void ExecutePdfTocEditMany(MainWindow* win, PdfTocEditAction action) {
    Vec<TocItem*> items = TocSelectedBookmarkItems(win);
    if (TocCalibIsActive(win)) {
        if (action == PdfTocEditAction::Delete) {
            TocCalibHandleDelete(win);
            ToolbarUpdateStateForWindow(win, false);
            return;
        }
        TocCalibOutlineOp op;
        bool structural = false;
        if (action == PdfTocEditAction::MoveUp) {
            op = TocCalibOutlineOp::MoveUp;
            structural = true;
        } else if (action == PdfTocEditAction::MoveDown) {
            op = TocCalibOutlineOp::MoveDown;
            structural = true;
        } else if (action == PdfTocEditAction::Promote) {
            op = TocCalibOutlineOp::Promote;
            structural = true;
        } else if (action == PdfTocEditAction::Demote) {
            op = TocCalibOutlineOp::Demote;
            structural = true;
        }
        if (structural) {
            TocCalibHandleOutlineOp(win, op);
            ToolbarUpdateStateForWindow(win, false);
            return;
        }
    }
    if (items.Size() <= 1) {
        ExecutePdfTocEdit(win, items.empty() ? nullptr : items.At(0), action);
        return;
    }
    if (!PdfTocEditIsBatchAction(action)) {
        ExecutePdfTocEdit(win, items.At(0), action);
        return;
    }

    EngineBase* engine = PdfTocEditableEngine(win);
    if (!engine || !ConfirmPdfTocSignatureEdit(win, engine)) {
        return;
    }
    Vec<PdfTocPath> paths;
    if (!TocCollectSelectedPaths(win, paths)) {
        return;
    }
    if (action == PdfTocEditAction::Delete) {
        bool hasChildren = false;
        for (TocItem* item : items) {
            if (item && item->child) {
                hasChildren = true;
                break;
            }
        }
        if (!ConfirmPdfTocDelete(win, paths.Size(), hasChildren)) {
            return;
        }
    }

    Vec<PdfTocPath> resultPaths;
    char* errorRaw = nullptr;
    bool ok = EngineMupdfEditPdfTocMany(engine, action, paths, &resultPaths, &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return;
    }
    if (action != PdfTocEditAction::Delete && resultPaths.empty()) {
        return;
    }
    ReloadPdfTocAfterEdit(win, resultPaths);
    ToolbarUpdateStateForWindow(win, false);
}

static void ExecutePdfTocDrop(MainWindow* win, TocItem* dest, PdfTocDropPos pos) {
    if (!win || HasTocFilter(win)) {
        return;
    }
    if (TocCalibIsActive(win) && TocCalibHandleDrop(win, dest, (int)pos)) {
        return;
    }
    EngineBase* engine = PdfTocEditableEngine(win);
    if (!engine || !ConfirmPdfTocSignatureEdit(win, engine)) {
        return;
    }
    Vec<PdfTocPath> srcPaths;
    if (!TocCollectSelectedPaths(win, srcPaths)) {
        return;
    }
    dest = OriginalPdfTocItem(win, dest);
    Vec<int> destPath;
    if (dest && !PdfTocPathForItem(win, dest, destPath)) {
        return;
    }
    Vec<PdfTocPath> resultPaths;
    char* errorRaw = nullptr;
    bool ok = EngineMupdfMovePdfTocItems(engine, srcPaths, destPath, pos, &resultPaths, &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return;
    }
    ReloadPdfTocAfterEdit(win, resultPaths);
    ToolbarUpdateStateForWindow(win, false);
}

static bool PdfTocTakeSelection(MainWindow* win, AutoFreeStr& title, int* pageOut, float* xOut, float* yOut) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    TextSelection* selection = dm ? dm->textSelection : nullptr;
    if (!selection || selection->result.len <= 0 || !HasPermission(Perm::CopySelection)) {
        return false;
    }
    bool isTextOnlySelection = false;
    TempStr selectedText = GetSelectedTextTemp(tab, " ", isTextOnlySelection);
    if (!isTextOnlySelection || !selectedText) {
        return false;
    }
    title.SetCopy(selectedText);
    if (!title) {
        return false;
    }
    str::TrimWSInPlace(title.Get(), str::TrimOpt::Both);
    str::NormalizeWSInPlace(title.Get());
    if (str::IsEmpty(title.Get())) {
        return false;
    }
    if (pageOut) {
        *pageOut = selection->result.pages[0];
    }
    Rect rc = selection->result.rects[0];
    if (xOut) {
        *xOut = (float)rc.x;
    }
    if (yOut) {
        *yOut = (float)rc.y;
    }
    return true;
}

static TocItem* PdfTocCurrentItem(MainWindow* win) {
    TocItem* selected = win && win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
    if (!selected && win && win->tocSelectedIds.Size() > 0) {
        Vec<TocItem*> items = TocSelectedBookmarkItems(win);
        if (!items.empty()) {
            selected = items.At(0);
        }
    }
    return OriginalPdfTocItem(win, selected);
}

bool TryAddPdfTocFromSelection(MainWindow* win) {
    AutoFreeStr title;
    int pageNo = 0;
    float x = 0;
    float y = 0;
    if (!PdfTocTakeSelection(win, title, &pageNo, &x, &y)) {
        return false;
    }
    if (TocCalibIsActive(win)) {
        return TocCalibAddSelectionUnderCurrent(win, title.Get(), pageNo, x, y);
    }
    EngineBase* engine = PdfTocEditableEngine(win);
    if (!engine) {
        return false;
    }
    // Once this is recognized as the PDF TOC shortcut, don't fall back to adding
    // a favorite if the edit is cancelled or fails.
    if (!ConfirmPdfTocSignatureEdit(win, engine)) {
        return true;
    }
    AutoFreeStr target(EngineMupdfFormatPdfTocTarget(engine, pageNo, x, y));
    if (!target) {
        ShowPdfTocEditError(win, "The selected PDF position could not be captured.");
        return true;
    }

    TocItem* selected = PdfTocCurrentItem(win);
    if (selected && (!selected->dest || selected->dest->GetKind() != kindDestinationMupdf)) {
        selected = nullptr;
    }
    Vec<int> path;
    if (selected && !PdfTocPathForItem(win, selected, path)) {
        return true;
    }

    PdfTocEditAction action = PdfTocEditAction::AddAfter;
    if (selected && selected->child) {
        action = PdfTocEditAction::AddChild;
    }
    Vec<int> resultPath;
    char* errorRaw = nullptr;
    bool ok = EngineMupdfEditPdfToc(engine, action, path, title.Get(), target.Get(), &resultPath, &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return true;
    }
    ReloadPdfTocAfterEdit(win, selected ? path : resultPath);
    ToolbarUpdateStateForWindow(win, false);
    return true;
}

bool TryReplacePdfTocFromSelection(MainWindow* win) {
    AutoFreeStr title;
    int pageNo = 0;
    float x = 0;
    float y = 0;
    if (!PdfTocTakeSelection(win, title, &pageNo, &x, &y)) {
        return false;
    }
    if (TocCalibIsActive(win)) {
        return TocCalibReplaceSelectedFromSelection(win, title.Get(), pageNo, x, y);
    }
    EngineBase* engine = PdfTocEditableEngine(win);
    if (!engine) {
        return false;
    }
    if (!ConfirmPdfTocSignatureEdit(win, engine)) {
        return true;
    }
    TocItem* selected = PdfTocCurrentItem(win);
    if (!selected || str::IsEmpty(selected->title)) {
        return false;
    }
    Vec<int> path;
    if (!PdfTocPathForItem(win, selected, path)) {
        return false;
    }
    AutoFreeStr target(EngineMupdfFormatPdfTocTarget(engine, pageNo, x, y));
    if (!target) {
        ShowPdfTocEditError(win, "The selected PDF position could not be captured.");
        return true;
    }
    Vec<int> resultPath;
    char* errorRaw = nullptr;
    bool ok = EngineMupdfEditPdfToc(engine, PdfTocEditAction::Update, path, title.Get(), target.Get(), &resultPath,
                                    &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return true;
    }
    ReloadPdfTocAfterEdit(win, path);
    ToolbarUpdateStateForWindow(win, false);
    return true;
}

bool HandlePdfTocFindInBody(MainWindow* win) {
    return TocCalibLocateSelectedInBody(win);
}

bool HandlePdfTocSetCurrentPage(MainWindow* win) {
    if (!win) {
        return false;
    }
    if (TocCalibIsActive(win)) {
        return TocCalibPinSelectedToView(win);
    }
    EngineBase* engine = PdfTocEditableEngine(win);
    if (!engine || !ConfirmPdfTocSignatureEdit(win, engine)) {
        return false;
    }
    TocItem* selected = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
    if (!selected) {
        Vec<TocItem*> items = TocSelectedBookmarkItems(win);
        if (!items.empty()) {
            selected = items.At(0);
        }
    }
    selected = OriginalPdfTocItem(win, selected);
    if (!selected || str::IsEmpty(selected->title)) {
        return false;
    }
    Vec<int> path;
    if (!PdfTocPathForItem(win, selected, path)) {
        return false;
    }
    AutoFreeStr target(CurrentPdfTocTarget(win, engine));
    if (!target) {
        ShowPdfTocEditError(win, "The current PDF position could not be captured.");
        return false;
    }
    Vec<int> resultPath;
    char* errorRaw = nullptr;
    bool ok = EngineMupdfEditPdfToc(engine, PdfTocEditAction::Update, path, selected->title, target.Get(),
                                    &resultPath, &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return false;
    }
    ReloadPdfTocAfterEdit(win, resultPath);
    ToolbarUpdateStateForWindow(win, false);
    return true;
}

bool HandlePdfTocEditCommand(MainWindow* win, int commandId) {
    PdfTocEditAction action;
    switch (commandId) {
        case CmdPdfTocAddAfter:
            action = PdfTocEditAction::AddAfter;
            break;
        case CmdPdfTocAddChild:
            action = PdfTocEditAction::AddChild;
            break;
        case CmdPdfTocEdit:
            action = PdfTocEditAction::Update;
            break;
        case CmdPdfTocDelete:
            action = PdfTocEditAction::Delete;
            break;
        case CmdPdfTocMoveUp:
            action = PdfTocEditAction::MoveUp;
            break;
        case CmdPdfTocMoveDown:
            action = PdfTocEditAction::MoveDown;
            break;
        case CmdPdfTocPromote:
            action = PdfTocEditAction::Promote;
            break;
        case CmdPdfTocDemote:
            action = PdfTocEditAction::Demote;
            break;
        default:
            return false;
    }
    TocItem* selected = nullptr;
    if (win && win->tocTreeView) {
        selected = (TocItem*)win->tocTreeView->GetSelection();
    }
    if (PdfTocEditIsBatchAction(action) && win && win->tocSelectedIds.Size() > 1) {
        ExecutePdfTocEditMany(win, action);
        return true;
    }
    if (!selected && action != PdfTocEditAction::AddAfter) {
        Vec<TocItem*> items = TocSelectedBookmarkItems(win);
        if (!items.empty()) {
            selected = items.At(0);
        }
    }
    if (!selected && action != PdfTocEditAction::AddAfter) {
        return true;
    }
    ExecutePdfTocEdit(win, selected, action);
    return true;
}

// clang-format off
static MenuDef menuDefContextToc[] = {
    {
        _TRN("Extract Table of Contents"),
        CmdExtractPdfToc,
    },
    {
        _TRN("Calibrate TOC Pages"),
        CmdPdfTocCalibrate,
    },
    {
        _TRN("Find TOC Item in Body"),
        CmdPdfTocFindInBody,
    },
    {
        _TRN("Set TOC Item to Current Page"),
        CmdPdfTocSetCurrentPage,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Add PDF TOC Item After"),
        CmdPdfTocAddAfter,
    },
    {
        _TRN("Add PDF TOC Child Item"),
        CmdPdfTocAddChild,
    },
    {
        _TRN("Edit PDF TOC Item..."),
        CmdPdfTocEdit,
    },
    {
        _TRN("Delete PDF TOC Item"),
        CmdPdfTocDelete,
    },
    {
        _TRN("Move PDF TOC Item Up"),
        CmdPdfTocMoveUp,
    },
    {
        _TRN("Move PDF TOC Item Down"),
        CmdPdfTocMoveDown,
    },
    {
        _TRN("Promote PDF TOC Item"),
        CmdPdfTocPromote,
    },
    {
        _TRN("Demote PDF TOC Item"),
        CmdPdfTocDemote,
    },
    {
        kMenuSeparator,
        0,
    },
    {
        _TRN("Open Embedded PDF"),
        CmdOpenEmbeddedPDF,
    },
    {
        _TRN("Save Embedded File..."),
        CmdSaveEmbeddedFile,
    },
    {
        _TRN("Open Attachment"),
        CmdOpenAttachment,
    },
    {
        _TRN("Save Attachment..."),
        CmdSaveAttachment,
    },
    // note: strings cannot be "" or else items are not there
    {
        "Add to favorites",
        CmdFavoriteAdd,
    },
    {
        "Remove from favorites",
        CmdFavoriteDel,
    },
    {
        nullptr,
        0,
    },
};
// clang-format on

static void TocContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    const char* filePath = win->ctrl->GetFilePath();

    POINT pt{};

    TreeView* treeView = (TreeView*)ev->w;
    TreeItem ti = TreeModel::kNullItem;
    pt = {ev->mouseWindow.x, ev->mouseWindow.y};
    if (pt.x == -1 || pt.y == -1) {
        ti = treeView->GetSelection();
        if (ti != TreeModel::kNullItem) {
            RECT rcItem;
            if (treeView->GetItemRect(ti, true, rcItem)) {
                MapWindowPoints(treeView->hwnd, HWND_DESKTOP, (POINT*)&rcItem, 2);
                pt.x = rcItem.left;
                pt.y = rcItem.bottom;
            }
        }
    } else {
        ti = treeView->GetItemAt(pt.x, pt.y);
        TocItem* hit = (TocItem*)ti;
        if (hit && !TocItemIsMultiSelected(win, hit)) {
            TocSelectOnly(win, hit, false);
        }
        pt.x = ev->mouseScreen.x;
        pt.y = ev->mouseScreen.y;
    }
    if (ti == TreeModel::kNullItem) {
        pt = {ev->mouseScreen.x, ev->mouseScreen.y};
    }
    int pageNo = 0;
    TocItem* dti = (TocItem*)ti;
    IPageDestination* dest = dti ? dti->dest : nullptr;
    if (dest) {
        pageNo = PageDestGetPageNo(dti->dest);
    }

    WindowTab* tab = win->CurrentTab();
    BuildMenuCtx* menuCtx = NewBuildMenuCtx(tab, Point());
    defer {
        DeleteBuildMenuCtx(menuCtx);
    };
    HMENU popup = BuildMenuFromDef(menuDefContextToc, CreatePopupMenu(), menuCtx);

    EngineBase* pdfTocEngine = PdfTocEditableEngine(win);
    TocItem* pdfTocItem = OriginalPdfTocItem(win, dti);
    Kind destKind = dest ? dest->GetKind() : nullptr;
    bool isPdfTocItem = pdfTocItem && pdfTocItem->dest && pdfTocItem->dest->GetKind() == kindDestinationMupdf;
    bool tocMulti = win->tocSelectedIds.Size() > 1;
    const int pdfTocCommands[] = {CmdExtractPdfToc,  CmdPdfTocCalibrate, CmdPdfTocSetCurrentPage, CmdPdfTocFindInBody,
                                  CmdPdfTocAddAfter, CmdPdfTocAddChild,  CmdPdfTocEdit,           CmdPdfTocDelete,
                                  CmdPdfTocMoveUp,   CmdPdfTocMoveDown,  CmdPdfTocPromote,        CmdPdfTocDemote};
    bool canSetCurrentPage =
        dti && !tocMulti && destKind != kindDestinationLaunchEmbedded && destKind != kindDestinationAttachment;
    bool canFindInBody = TocCalibIsActive(win) && canSetCurrentPage;
    if (!pdfTocEngine) {
        for (int command : pdfTocCommands) {
            MenuRemove(popup, command);
        }
    } else if (!isPdfTocItem) {
        for (int command : pdfTocCommands) {
            if (command != CmdPdfTocAddAfter && command != CmdExtractPdfToc && command != CmdPdfTocCalibrate &&
                command != CmdPdfTocSetCurrentPage && command != CmdPdfTocFindInBody) {
                MenuRemove(popup, command);
            }
        }
        if (!canSetCurrentPage) {
            MenuRemove(popup, CmdPdfTocSetCurrentPage);
        }
        MenuSetText(popup, CmdPdfTocAddAfter, _TRA("Add Root PDF TOC Item"));
        TocTree* toc = tab && tab->currToc ? tab->currToc : nullptr;
        if (!toc || !toc->root || !toc->root->child) {
            MenuRemove(popup, CmdPdfTocCalibrate);
        }
    } else if (tocMulti) {
        MenuRemove(popup, CmdPdfTocAddChild);
        MenuRemove(popup, CmdPdfTocEdit);
        MenuRemove(popup, CmdPdfTocSetCurrentPage);
    } else {
        Vec<int> tocPath;
        bool hasPath = PdfTocPathForItem(win, pdfTocItem, tocPath);
        int idx = hasPath && !tocPath.empty() ? tocPath.Last() : -1;
        if (idx <= 0) {
            MenuRemove(popup, CmdPdfTocMoveUp);
            MenuRemove(popup, CmdPdfTocDemote);
        }
        TocItem* next = pdfTocItem->next;
        if (!next || !next->dest || next->dest->GetKind() != kindDestinationMupdf) {
            MenuRemove(popup, CmdPdfTocMoveDown);
        }
        if (!pdfTocItem->parent) {
            MenuRemove(popup, CmdPdfTocPromote);
        }
    }
    if (!canFindInBody) {
        MenuRemove(popup, CmdPdfTocFindInBody);
    }

    const char* path = nullptr;
    char* fileName = nullptr;

    // TODO: this is pontentially not used at all
    if (dest && destKind == kindDestinationLaunchEmbedded) {
        auto embeddedFile = (PageDestinationFile*)dest;
        // this is a path to a file on disk, e.g. a path to opened PDF
        // with the embedded stream number
        path = embeddedFile->path;
        // this is name of the file as set inside PDF file
        fileName = PageDestGetName(dest);
        bool canOpenEmbedded = str::EndsWithI(fileName, ".pdf");
        if (!canOpenEmbedded) {
            MenuRemove(popup, CmdOpenEmbeddedPDF);
        }
    } else {
        // TODO: maybe move this to BuildMenuFromMenuDef
        MenuRemove(popup, CmdSaveEmbeddedFile);
        MenuRemove(popup, CmdOpenEmbeddedPDF);
    }

    int attachmentNo = -1;
    if (dest && destKind == kindDestinationAttachment) {
        auto attachment = (PageDestinationFile*)dest;
        // this is a path to a file on disk, e.g. a path to opened PDF
        // with the embedded stream number
        path = attachment->path;
        // this is name of the file as set inside PDF file
        fileName = PageDestGetName(dest);
        // hack: attachmentNo is saved in pageNo see
        // PdfLoadAttachments and DestFromAttachment
        attachmentNo = pageNo;
        bool canOpenEmbedded = str::EndsWithI(fileName, ".pdf");
        if (!canOpenEmbedded) {
            MenuRemove(popup, CmdOpenAttachment);
        }
    } else {
        // TODO: maybe move this to BuildMenuFromMenuDef
        MenuRemove(popup, CmdSaveAttachment);
        MenuRemove(popup, CmdOpenAttachment);
    }

    if (pdfTocEngine) {
        MenuRemove(popup, CmdFavoriteAdd);
        MenuRemove(popup, CmdFavoriteDel);
    } else if (pageNo > 0) {
        TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
        bool isBookmarked = IsPageInFavorites(filePath, pageNo);
        if (isBookmarked) {
            MenuRemove(popup, CmdFavoriteAdd);

            // %s and not %d because re-using translation from RebuildFavMenu()
            const char* tr = _TRA("Remove page %s from favorites");
            TempStr s = str::FormatTemp(tr, pageLabel);
            MenuSetText(popup, CmdFavoriteDel, s);
        } else {
            MenuRemove(popup, CmdFavoriteDel);
            // %s and not %d because re-using translation from RebuildFavMenu()
            TempStr s = str::FormatTemp(_TRA("Add page %s to favorites"), pageLabel);
            s = AppendAccelKeyToMenuStringTemp(s, CmdFavoriteAdd);
            MenuSetText(popup, CmdFavoriteAdd, s);
        }
    } else {
        MenuRemove(popup, CmdFavoriteAdd);
        MenuRemove(popup, CmdFavoriteDel);
    }
    if (win->ctrl && GetMenuState(popup, CmdPdfTocSetCurrentPage, MF_BYCOMMAND) != (UINT)-1) {
        int viewPageNo = win->ctrl->CurrentPageNo();
        if (viewPageNo > 0) {
            TempStr viewLabel = win->ctrl->GetPageLabeTemp(viewPageNo);
            MenuSetText(popup, CmdPdfTocSetCurrentPage,
                        str::FormatTemp(_TRA("Link page %s to selected bookmark"), viewLabel));
        }
    }
    RemoveBadMenuSeparators(popup);
    MarkMenuOwnerDraw(popup);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);
    switch (cmd) {
        case CmdPdfTocAddAfter:
            ExecutePdfTocEdit(win, isPdfTocItem ? pdfTocItem : nullptr, PdfTocEditAction::AddAfter);
            break;
        case CmdPdfTocAddChild:
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::AddChild);
            break;
        case CmdPdfTocEdit:
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::Update);
            break;
        case CmdPdfTocDelete:
            ExecutePdfTocEditMany(win, PdfTocEditAction::Delete);
            break;
        case CmdPdfTocMoveUp:
            ExecutePdfTocEditMany(win, PdfTocEditAction::MoveUp);
            break;
        case CmdPdfTocMoveDown:
            ExecutePdfTocEditMany(win, PdfTocEditAction::MoveDown);
            break;
        case CmdPdfTocPromote:
            ExecutePdfTocEditMany(win, PdfTocEditAction::Promote);
            break;
        case CmdPdfTocDemote:
            ExecutePdfTocEditMany(win, PdfTocEditAction::Demote);
            break;
        case CmdExtractPdfToc:
            HandleExtractPdfTocCommand(win);
            break;
        case CmdPdfTocCalibrate:
            StartTocCalibFromExisting(win);
            break;
        case CmdPdfTocSetCurrentPage:
            HandlePdfTocSetCurrentPage(win);
            break;
        case CmdPdfTocFindInBody:
            HandlePdfTocFindInBody(win);
            break;
        case CmdFavoriteAdd:
            AddFavoriteFromToc(win, dti);
            break;
        case CmdFavoriteDel:
            DelFavorite(filePath, pageNo);
            break;
        case CmdSaveEmbeddedFile: {
            SaveEmbeddedFile(tab, path, fileName);
        } break;
        case CmdOpenEmbeddedPDF:
            // TODO: maybe also allow for a fileName hint
            OpenEmbeddedFile(tab, dest);
            break;
        case CmdSaveAttachment: {
            SaveAttachment(tab, fileName, attachmentNo);
            break;
        }
        case CmdOpenAttachment: {
            OpenAttachment(tab, fileName, attachmentNo);
        }
    }
}

void OnTocCustomDraw(TreeView::CustomDrawEvent*);

// auto-expand root level ToC nodes if there are at most two
static void AutoExpandTopLevelItems(TocItem* root) {
    if (!root) {
        return;
    }
    if (root->next && root->next->next) {
        return;
    }

    if (!root->IsExpanded()) {
        root->isOpenToggled = !root->isOpenToggled;
    }
    if (!root->next) {
        return;
    }
    if (!root->next->IsExpanded()) {
        root->next->isOpenToggled = !root->next->isOpenToggled;
    }
}

void LoadTocTree(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    ReportIf(!tab);

    if (win->tocLoaded) {
        return;
    }
    if (!tab->ctrl) {
        return;
    }

    // clear filter when loading new toc
    // null out currToc first so that SetText("") callback doesn't use stale pointer
    if (win->tocFilteredTree) {
        TreeView* tv = win->tocTreeView;
        if (tv && tv->treeModel == win->tocFilteredTree) {
            tv->treeModel = nullptr;
        }
        delete win->tocFilteredTree;
        win->tocFilteredTree = nullptr;
    }
    tab->currToc = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    auto* tocTree = tab->ctrl->GetToc();
    if (!tocTree || !tocTree->root) {
        return;
    }

    win->tocLoaded = true;
    tab->currToc = tocTree;

    // consider a ToC tree right-to-left if a more than half of the
    // alphabetic characters are in a right-to-left script
    int l2r = 0, r2l = 0;
    GetLeftRightCounts(tocTree->root, l2r, r2l);
    bool isRTL = r2l > l2r;

    TreeView* treeView = win->tocTreeView;
    if (!treeView || !treeView->hwnd) {
        return;
    }
    HWND hwnd = treeView->hwnd;
    HwndSetRtl(hwnd, isRTL);

    SetInitialExpandState(tocTree->root, tab->tocState);
    AutoExpandTopLevelItems(tocTree->root->child);

    treeView->SetTreeModel(tocTree);

    treeView->onCustomDraw = MkFunc1Void(OnTocCustomDraw);
    // Apply dark scrollbar theme after the model is set. Scrollbars are created
    // when content appears; theming before SetTreeModel leaves them light.
    UpdateControlsColors(win);
    LayoutTocContainer(win);
    DisplayModel* dm = win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (EngineIsProgressiveEbookLoading(engine)) {
        ScheduleTocTreeWrapHeights(win);
    } else {
        TocRecalcAllItemHeights(win);
    }
    tab->tocWrapHeightsReady = true;
    InvalidateTocTree(win);
    UpdateTocFilterForDocumentLoading(win);
    UpdateTocCalibrateHeader(win);
    RaiseDocumentLoadingNotification(win->hwndFrame, win->hwndCanvas);
}

void ReloadPdfTocTree(MainWindow* win) {
    if (!win) {
        return;
    }
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
}

void RestoreTocTreeForTab(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    if (tab && tab->tocCalib) {
        ShowTocCalib(win);
    } else {
        HideTocCalib(win);
    }
    if (!tab || win->tocLoaded || !tab->ctrl) {
        return;
    }

    TocTree* tocTree = tab->currToc;
    if (!tocTree) {
        tocTree = tab->ctrl->GetToc();
    }
    if (!tocTree || !tocTree->root) {
        return;
    }

    win->tocLoaded = true;
    tab->currToc = tocTree;

    int l2r = 0, r2l = 0;
    GetLeftRightCounts(tocTree->root, l2r, r2l);
    bool isRTL = r2l > l2r;

    TreeView* treeView = win->tocTreeView;
    if (!treeView || !treeView->hwnd) {
        return;
    }
    HWND hwnd = treeView->hwnd;
    HwndSetRtl(hwnd, isRTL);

    SetInitialExpandState(tocTree->root, tab->tocState);
    AutoExpandTopLevelItems(tocTree->root->child);

    treeView->SetTreeModel(tocTree);

    treeView->onCustomDraw = MkFunc1Void(OnTocCustomDraw);
    UpdateControlsColors(win);
    LayoutTocContainer(win);
    DisplayModel* dm = win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (tab->tocWrapHeightsReady) {
        InvalidateTocTree(win);
    } else if (EngineIsProgressiveEbookLoading(engine)) {
        ScheduleTocTreeWrapHeights(win);
        tab->tocWrapHeightsReady = true;
    } else {
        TocRecalcAllItemHeights(win);
        tab->tocWrapHeightsReady = true;
    }
    UpdateTocFilterForDocumentLoading(win);
    UpdateTocCalibrateHeader(win);
    RaiseDocumentLoadingNotification(win->hwndFrame, win->hwndCanvas);
}

// TODO: use https://docs.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getobject?redirectedfrom=MSDN
// to get LOGFONT from existing font and then create a derived font
static void UpdateFont(MainWindow* win, HWND hwndTree, HDC hdc, int fontFlags) {
    if (!win) {
        return;
    }
    int dpi = DpiGetForMonitorOfHwnd(hwndTree);
    if (dpi <= 0) {
        dpi = win->frameDpi > 0 ? win->frameDpi : DpiGet(win->hwndFrame);
    }
    dpi = RoundUp(dpi, 4);
    HFONT hfont = GetAppMenuFontForDpi(dpi);
    bool italic = bit::IsSet(fontFlags, fontBitItalic);
    bool bold = bit::IsSet(fontFlags, fontBitBold);
    if (bold || italic) {
        LOGFONTW lf{};
        if (GetObjectW(hfont, sizeof(lf), &lf) == sizeof(lf)) {
            if (bold) {
                lf.lfWeight = FW_BOLD;
            }
            if (italic) {
                lf.lfItalic = TRUE;
            }
            HFONT derived = CreateFontIndirectW(&lf);
            if (derived) {
                hfont = derived;
            }
        }
    }
    SelectObject(hdc, hfont);
}

static COLORREF TocItemTextColor(TocItem* tocItem, MainWindow* win, TreeView* treeView);
static void SetTocItemDrawColors(NMTVCUSTOMDRAW* tvcd, TreeView* treeView, TocItem* tocItem, MainWindow* win);
static bool TocDrawItemSelected(MainWindow* win, TocItem* tocItem, NMCUSTOMDRAW* cd);

static int TocTreeItemLevel(HWND hwnd, HTREEITEM hItem) {
    int level = 0;
    HTREEITEM p = TreeView_GetParent(hwnd, hItem);
    while (p) {
        level++;
        p = TreeView_GetParent(hwnd, p);
    }
    return level;
}

static int TocGetItemLabelLeft(HWND hwnd, HTREEITEM hItem) {
    RECT rcLabel;
    if (TreeView_GetItemRect(hwnd, hItem, &rcLabel, TRUE)) {
        return rcLabel.left;
    }
    int indent = TreeView_GetIndent(hwnd);
    return indent * (TocTreeItemLevel(hwnd, hItem) + 1);
}

static void DrawTreeWrappedLabel(NMTVCUSTOMDRAW* tvcd, TreeView* treeView, const WCHAR* textW, MainWindow* win,
                                 int fontFlags) {
    if (!textW || !*textW) {
        return;
    }
    HWND hwnd = treeView->hwnd;
    HTREEITEM hItem = (HTREEITEM)tvcd->nmcd.dwItemSpec;
    RECT rcLabel;
    bool gotItemRect = TreeView_GetItemRect(hwnd, hItem, &rcLabel, TRUE) != FALSE;
    if (!gotItemRect) {
        int labelLeft = TocGetItemLabelLeft(hwnd, hItem);
        int baseH = treeView->unevenItemBaseHeight;
        if (baseH <= 0) {
            baseH = TreeView_GetItemHeight(hwnd);
        }
        if (baseH <= 0) {
            baseH = 18;
        }
        rcLabel.left = labelLeft;
        rcLabel.top = 0;
        rcLabel.right = labelLeft + 100;
        rcLabel.bottom = baseH;
    }
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    rcLabel.right = rcClient.right - 2;
    if (TocCalibIsActive(win)) {
        rcLabel.right -= TocCalibColumnsDx(hwnd);
        if (rcLabel.right < rcLabel.left + 24) {
            rcLabel.right = rcLabel.left + 24;
        }
    }

    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    TocItem* tocItemForSel = (TocItem*)treeView->GetTreeItemByHandle(hItem);
    bool isSelected = TocDrawItemSelected(win, tocItemForSel, cd);
    bool isHot = (cd->uItemState & CDIS_HOT) != 0;
    COLORREF textCol = tvcd->clrText;
    COLORREF bgCol = tvcd->clrTextBk;
    bool skipBgFill = isSelected || (ThemeUsesDarkChrome() && isHot);

    if (!skipBgFill) {
        RECT rcFill = rcLabel;
        rcFill.right = rcClient.right;
        HBRUSH br = CreateSolidBrush(bgCol);
        FillRect(cd->hdc, &rcFill, br);
        DeleteObject(br);
    }

    if (fontFlags != 0 && win) {
        UpdateFont(win, hwnd, cd->hdc, fontFlags);
    }

    InflateRect(&rcLabel, -2, -1);
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, textCol);
    // During live sidebar resize, keep single-line clipped text so wrap paint
    // cannot spill and leave vertical ghost lines before heights are flushed.
    UINT dtFlags = DT_NOPREFIX | DT_LEFT;
    if (TocCalibIsActive(win) || !TreeWrapLabelsEnabled() || TreeWrapUpdatesSuspended()) {
        dtFlags |= DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER;
    } else {
        dtFlags |= DT_WORDBREAK | DT_EDITCONTROL;
    }
    DrawTextW(cd->hdc, textW, -1, &rcLabel, dtFlags);
}

static void DrawTocWrappedLabel(NMTVCUSTOMDRAW* tvcd, TreeView* treeView, TocItem* tocItem, MainWindow* win) {
    if (!tocItem || !tocItem->title) {
        return;
    }
    // TreeView default paint between prepaint and postpaint can clobber clrText
    // (visual styles). Recompute so unread chapters stay muted.
    SetTocItemDrawColors(tvcd, treeView, tocItem, win);
    DrawTreeWrappedLabel(tvcd, treeView, ToWStrTemp(tocItem->title), win, tocItem->fontFlags);
}

static bool HasTocFilter(MainWindow* win) {
    if (!win || !win->tocFilterEdit) {
        return false;
    }
    TempStr filter = win->tocFilterEdit->GetTextTemp();
    return filter && str::Len(filter) > 0;
}

static int TocMinItemLabelHeight(HDC hdc) {
    TEXTMETRIC tm{};
    if (!GetTextMetrics(hdc, &tm)) {
        return 18;
    }
    return tm.tmHeight + 4;
}

static int TocMeasureWrappedLabelHeight(HDC hdc, const WCHAR* text, int maxWidth, int minHeight) {
    if (!text || !*text || maxWidth <= 0) {
        return minHeight;
    }
    RECT rc = {0, 0, maxWidth, 0};
    DrawTextW(hdc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT | DT_EDITCONTROL);
    int h = rc.bottom - rc.top + 4;
    return h < minHeight ? minHeight : h;
}

static void TocSetTreeItemIntegral(HWND hwnd, HTREEITEM hItem, int integral) {
    TVITEMEXW item{};
    item.hItem = hItem;
    item.mask = TVIF_INTEGRAL;
    item.iIntegral = integral;
    TreeView_SetItem(hwnd, &item);
}

static void TreeWrapUpdateItemHeight(HWND hwnd, HTREEITEM hItem, HDC hdc, int clientWidth, int baseItemHeight,
                                     TreeView* treeView) {
    if (!hItem || !treeView) {
        return;
    }
    TreeItem ti = treeView->GetTreeItemByHandle(hItem);
    if (!ti || !treeView->treeModel) {
        return;
    }
    char* text = treeView->treeModel->Text(ti);
    if (!text) {
        return;
    }
    int labelLeft = TocGetItemLabelLeft(hwnd, hItem);
    int maxWidth = clientWidth - labelLeft - 4;
    MainWindow* wrapWin = FindMainWindowByHwnd(hwnd);
    if (TocCalibIsActive(wrapWin)) {
        TocSetTreeItemIntegral(hwnd, hItem, 1);
        return;
    }
    if (maxWidth < 24) {
        maxWidth = 24;
    }
    WCHAR* textW = ToWStrTemp(text);
    int minH = baseItemHeight > 0 ? baseItemHeight : TocMinItemLabelHeight(hdc);
    int pixelHeight = TocMeasureWrappedLabelHeight(hdc, textW, maxWidth, minH);
    int integral = baseItemHeight > 0 ? (pixelHeight + baseItemHeight - 1) / baseItemHeight : 1;
    if (integral < 1) {
        integral = 1;
    }
    TocSetTreeItemIntegral(hwnd, hItem, integral);
}

static void TreeWrapUpdateItemHeightsRecur(HWND hwnd, HTREEITEM hItem, HDC hdc, int clientWidth, int baseItemHeight,
                                           TreeView* treeView) {
    while (hItem) {
        TreeWrapUpdateItemHeight(hwnd, hItem, hdc, clientWidth, baseItemHeight, treeView);
        HTREEITEM child = TreeView_GetChild(hwnd, hItem);
        if (child) {
            TreeWrapUpdateItemHeightsRecur(hwnd, child, hdc, clientWidth, baseItemHeight, treeView);
        }
        hItem = TreeView_GetNextSibling(hwnd, hItem);
    }
}

static void TreeWrapRecalcAllItemHeights(TreeView* treeView, HWND hwndFrame) {
    if (!treeView || !treeView->hwnd || !treeView->unevenItemHeight) {
        return;
    }
    HWND hwnd = treeView->hwnd;
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    if (rcClient.right <= 0) {
        return;
    }
    int baseH = treeView->unevenItemBaseHeight;
    if (baseH <= 0) {
        baseH = TreeView_GetItemHeight(hwnd);
    }
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return;
    }
    HFONT font = GetAppTreeFontForHwnd(hwndFrame);
    if (font) {
        SelectObject(hdc, font);
    }
    HTREEITEM hItem = TreeView_GetRoot(hwnd);
    if (hItem) {
        TreeWrapUpdateItemHeightsRecur(hwnd, hItem, hdc, rcClient.right, baseH, treeView);
    }
    ReleaseDC(hwnd, hdc);
}

static void TocRecalcAllItemHeights(MainWindow* win) {
    if (!win) {
        return;
    }
    TreeWrapRecalcAllItemHeights(win->tocTreeView, win->hwndFrame);
}

void ScheduleTocTreeWrapHeights(MainWindow* win) {
    if (win) {
        ScheduleTreeWrapHeightRecalc(win->hwndTocBox);
    }
}

void FlushTocTreeWrapHeights(MainWindow* win) {
    if (!win) {
        return;
    }
    KillTreeWrapHeightTimer(win->hwndTocBox);
    TocRecalcAllItemHeights(win);
    InvalidateTocTree(win);
}

void ResumeTreeWrapLiveResizeAndFlush(MainWindow* win) {
    if (gTreeWrapSuspendDepth > 0) {
        gTreeWrapSuspendDepth--;
    }
    FlushTocTreeWrapHeights(win);
    FlushFavTreeWrapHeights(win);
}

static COLORREF TocSelectionBgColor() {
    return AccentColor(ThemeWindowControlBackgroundColor(), 25);
}

static COLORREF TocHotTrackBgColor() {
    return AccentColor(ThemeWindowControlBackgroundColor(), 12);
}

static COLORREF TocSelectionBorderColor() {
    return AccentColor(ThemeWindowLinkColor(), -20);
}

static void GetTocItemRowRect(HWND hwnd, HTREEITEM hItem, NMCUSTOMDRAW* cd, RECT& rcRow) {
    if (!TreeView_GetItemRect(hwnd, hItem, &rcRow, FALSE)) {
        rcRow = cd->rc;
    }
    TVITEMEXW item{};
    item.hItem = hItem;
    item.mask = TVIF_INTEGRAL;
    if (TreeView_GetItem(hwnd, &item) && item.iIntegral > 1) {
        int baseH = TreeView_GetItemHeight(hwnd);
        if (baseH > 0) {
            rcRow.bottom = rcRow.top + baseH * item.iIntegral;
        }
    }
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    rcRow.left = rcClient.left;
    if (rcRow.right < rcClient.right) {
        rcRow.right = rcClient.right;
    }
}

static void DrawTocExpandGlyph(HDC hdc, HWND hwnd, HTREEITEM hItem, const RECT& rcRow) {
    if (!TreeView_GetChild(hwnd, hItem)) {
        return;
    }
    RECT rcLabel{};
    if (!TreeView_GetItemRect(hwnd, hItem, &rcLabel, TRUE)) {
        return;
    }
    bool expanded = (TreeView_GetItemState(hwnd, hItem, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
    int indent = TreeView_GetIndent(hwnd);
    if (indent < 10) {
        indent = 16;
    }
    RECT rc;
    rc.right = rcLabel.left - 1;
    rc.left = rc.right - indent;
    if (rc.left < rcRow.left) {
        rc.left = rcRow.left;
    }
    int sz = (rc.right - rc.left);
    if (sz > indent) {
        sz = indent;
    }
    int midY = (rcRow.top + rcRow.bottom) / 2;
    rc.top = midY - sz / 2;
    rc.bottom = rc.top + sz;
    HTHEME theme = theme::OpenThemeData(hwnd, L"TREEVIEW");
    if (theme) {
        theme::DrawThemeBackground(theme, hdc, TVP_GLYPH, expanded ? GLPS_OPENED : GLPS_CLOSED, &rc, nullptr);
        theme::CloseThemeData(theme);
    }
}

static COLORREF TocSelectedRowFillColor(HWND) {
    return TocSelectionBgColor();
}

static void DrawTocSelectionFill(NMCUSTOMDRAW* cd, HWND hwnd, HTREEITEM hItem) {
    RECT rcRow;
    GetTocItemRowRect(hwnd, hItem, cd, rcRow);
    COLORREF col = TocSelectedRowFillColor(hwnd);
    HBRUSH br = CreateSolidBrush(col);
    FillRect(cd->hdc, &rcRow, br);
    DeleteObject(br);
    DrawTocExpandGlyph(cd->hdc, hwnd, hItem, rcRow);
}

static void DrawTocHotTrackFill(NMCUSTOMDRAW* cd, HWND hwnd, HTREEITEM hItem) {
    RECT rcRow;
    GetTocItemRowRect(hwnd, hItem, cd, rcRow);
    HBRUSH br = CreateSolidBrush(TocHotTrackBgColor());
    FillRect(cd->hdc, &rcRow, br);
    DeleteObject(br);
    DrawTocExpandGlyph(cd->hdc, hwnd, hItem, rcRow);
}

static void DrawTocSelectionFrame(NMCUSTOMDRAW* cd, HWND hwnd, HTREEITEM hItem) {
    RECT rcRow;
    GetTocItemRowRect(hwnd, hItem, cd, rcRow);
    RECT rcFrame = rcRow;
    InflateRect(&rcFrame, 1, 0);
    COLORREF borderCol = TocSelectionBorderColor();
    HPEN pen = CreatePen(PS_SOLID, 1, borderCol);
    HPEN oldPen = (HPEN)SelectObject(cd->hdc, pen);
    HGDIOBJ oldBr = SelectObject(cd->hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(cd->hdc, rcFrame.left, rcFrame.top, rcFrame.right, rcFrame.bottom);
    SelectObject(cd->hdc, oldBr);
    SelectObject(cd->hdc, oldPen);
    DeleteObject(pen);

    // Cover inactive-selection blue caps on the full row left/right edges.
    HPEN edgePen = CreatePen(PS_SOLID, 2, borderCol);
    oldPen = (HPEN)SelectObject(cd->hdc, edgePen);
    MoveToEx(cd->hdc, rcRow.left, rcRow.top, nullptr);
    LineTo(cd->hdc, rcRow.left, rcRow.bottom);
    MoveToEx(cd->hdc, rcRow.right - 1, rcRow.top, nullptr);
    LineTo(cd->hdc, rcRow.right - 1, rcRow.bottom);
    SelectObject(cd->hdc, oldPen);
    DeleteObject(edgePen);
}

static COLORREF TocItemTextColor(TocItem* tocItem, MainWindow* win, TreeView* treeView) {
    if (win->ctrl && IsTocInternalPageItem(tocItem, win->ctrl) && !IsTocPageReachable(win->ctrl, tocItem)) {
        return TocItemDisabledTextColor(treeView);
    }
    if (tocItem->color != kColorUnset) {
        return tocItem->color;
    }
    if (ThemeUsesDarkChrome()) {
        return ThemeReadingTextColor();
    }
    return IsSpecialColor(treeView->textColor) ? GetSysColor(COLOR_WINDOWTEXT) : treeView->textColor;
}

static bool TocDrawItemSelected(MainWindow* win, TocItem* tocItem, NMCUSTOMDRAW* cd) {
    if (win && win->tocSelectionOwned) {
        return TocItemIsMultiSelected(win, tocItem);
    }
    if (win && !win->tocSelectedIds.empty()) {
        return TocItemIsMultiSelected(win, tocItem);
    }
    return cd && (cd->uItemState & CDIS_SELECTED) != 0;
}

static void SetTocItemDrawColors(NMTVCUSTOMDRAW* tvcd, TreeView* treeView, TocItem* tocItem, MainWindow* win) {
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = TocDrawItemSelected(win, tocItem, cd);
    bool isHot = (cd->uItemState & CDIS_HOT) != 0;
    COLORREF bgCol = SidebarBackgroundColor(treeView->bgColor);

    if (isSelected) {
        tvcd->clrText = TocItemTextColor(tocItem, win, treeView);
        tvcd->clrTextBk = TocSelectionBgColor();
        return;
    }
    if (isHot && ThemeUsesDarkChrome()) {
        tvcd->clrText = TocItemTextColor(tocItem, win, treeView);
        tvcd->clrTextBk = TocHotTrackBgColor();
        return;
    }
    tvcd->clrTextBk = bgCol;
    if (win->ctrl && IsTocInternalPageItem(tocItem, win->ctrl) && !IsTocPageReachable(win->ctrl, tocItem)) {
        tvcd->clrText = TocItemDisabledTextColor(treeView);
    } else if (tocItem->color != kColorUnset) {
        tvcd->clrText = tocItem->color;
    } else if (ThemeUsesDarkChrome()) {
        tvcd->clrText = ThemeReadingTextColor();
    } else {
        tvcd->clrText = IsSpecialColor(treeView->textColor) ? GetSysColor(COLOR_WINDOWTEXT) : treeView->textColor;
    }
}

static void DrawTocItemHighlight(TreeView::CustomDrawEvent* ev, MainWindow* win) {
    TocItem* tocItem = (TocItem*)ev->treeItem;
    if (!tocItem || !tocItem->title) {
        return;
    }
    Edit* edit = win->tocFilterEdit;
    if (!edit) {
        return;
    }
    TempStr filter = edit->GetTextTemp();
    if (!filter || str::Len(filter) == 0) {
        return;
    }
    const char* title = tocItem->title;
    int titleLen = str::Leni(title);
    if (titleLen == 0) {
        return;
    }

    // mark which bytes are part of a match
    u8* highlighted = AllocArrayTemp<u8>(titleLen);
    int filterLen = str::Leni(filter);
    const char* p = title;
    while ((p = str::FindI(p, filter)) != nullptr) {
        int off = (int)(p - title);
        for (int k = 0; k < filterLen && off + k < titleLen; k++) {
            highlighted[off + k] = 1;
        }
        p += filterLen;
    }

    // collect contiguous highlighted ranges (up to 16)
    struct ByteRange {
        int start;
        int end;
    };
    ByteRange byteRanges[16];
    int nRanges = 0;
    {
        int pos = 0;
        while (pos < titleLen && nRanges < 16) {
            if (highlighted[pos]) {
                int start = pos;
                while (pos < titleLen && highlighted[pos]) {
                    pos++;
                }
                byteRanges[nRanges++] = {start, pos};
            } else {
                pos++;
            }
        }
    }
    if (nRanges == 0) {
        return;
    }

    // get the label rect for this tree item
    RECT labelRect;
    TreeView* tv = ev->treeView;
    if (!tv->GetItemRect(ev->treeItem, true, labelRect)) {
        return;
    }

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    HDC hdc = tvcd->nmcd.hdc;

    WCHAR* titleW = ToWStrTemp(title);

    // compute pixel rectangles for each highlighted range
    RECT highlightRects[16];
    for (int i = 0; i < nRanges; i++) {
        WCHAR* prefixToStart = ToWStrTemp(title, (size_t)byteRanges[i].start);
        int wStart = str::Leni(prefixToStart);
        WCHAR* prefixToEnd = ToWStrTemp(title, (size_t)byteRanges[i].end);
        int wEnd = str::Leni(prefixToEnd);

        SIZE szStart, szEnd;
        GetTextExtentPoint32W(hdc, titleW, wStart, &szStart);
        GetTextExtentPoint32W(hdc, titleW, wEnd, &szEnd);

        highlightRects[i].top = labelRect.top;
        highlightRects[i].bottom = labelRect.bottom;
        highlightRects[i].left = labelRect.left + szStart.cx;
        highlightRects[i].right = labelRect.left + szEnd.cx;
    }

    // erase the label area with the correct background color
    // so we can redraw text cleanly without double-draw artifacts
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    bool hasFocus = (GetFocus() == tv->hwnd);
    COLORREF bgCol;
    if (isSelected) {
        if (ThemeUsesDarkChrome()) {
            bgCol = TocSelectionBgColor();
        } else {
            bgCol = GetSysColor(hasFocus ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
        }
    } else {
        bgCol = SidebarBackgroundColor(tv->bgColor);
    }
    HBRUSH hbrBg = CreateSolidBrush(bgCol);
    FillRect(hdc, &labelRect, hbrBg);
    DeleteObject(hbrBg);

    // draw highlight background rectangles
    COLORREF highlightCol;
    if (!ThemeUsesDarkChrome()) {
        highlightCol = RGB(255, 255, 0);
    } else {
        highlightCol = AccentColor(bgCol, 40);
    }
    HBRUSH hbrHighlight = CreateSolidBrush(highlightCol);
    for (int i = 0; i < nRanges; i++) {
        FillRect(hdc, &highlightRects[i], hbrHighlight);
    }
    DeleteObject(hbrHighlight);

    // draw the text on top
    COLORREF txtCol = TocItemTextColor(tocItem, win, tv);
    if (isSelected && hasFocus && !ThemeUsesDarkChrome()) {
        txtCol = GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    COLORREF oldTxtCol = SetTextColor(hdc, txtCol);
    int oldBkMode = SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, titleW, -1, &labelRect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetBkMode(hdc, oldBkMode);
    SetTextColor(hdc, oldTxtCol);
}

// https://docs.microsoft.com/en-us/windows/win32/controls/about-custom-draw
// https://docs.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-nmtvcustomdraw
// While the user drags the TOC scrollbar thumb, skip per-item custom draw so
// Win32 can scroll natively; repaint with full styling when the drag ends.
static HWND gTocFastScrollHwnd = nullptr;

static bool TocIsEditingItem(MainWindow* win, HTREEITEM hItem);

void OnTocCustomDraw(TreeView::CustomDrawEvent* ev) {
#if defined(DISPLAY_TOC_PAGE_NUMBERS)
    if (false) return CDRF_DODEFAULT;
    switch (((LPNMCUSTOMDRAW)pnmtv)->dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT:
            return CDRF_DODEFAULT | CDRF_NOTIFYPOSTPAINT;
        case CDDS_ITEMPOSTPAINT:
            RelayoutTocItem((LPNMTVCUSTOMDRAW)pnmtv);
            // fall through
        default:
            return CDRF_DODEFAULT;
    }
    break;
#endif

    ev->result = CDRF_DODEFAULT;
    NMTVCUSTOMDRAW* tvcd = ev->nm;
    NMCUSTOMDRAW* cd = &(tvcd->nmcd);

    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (gTocFastScrollHwnd && gTocFastScrollHwnd == ev->treeView->hwnd && !EngineIsProgressiveEbookLoading(engine)) {
        return;
    }

    if (cd->dwDrawStage == CDDS_PREPAINT) {
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }

    bool filterActive = HasTocFilter(win);

    if (!TreeWrapLabelsEnabled()) {
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            TocItem* tocItem = (TocItem*)ev->treeItem;
            if (!tocItem) {
                return;
            }
            bool knownTree = win && IsKnownTocTreeModel(win, ev->treeView->treeModel);
            if (!knownTree || !win || win->isBeingClosed || !win->tocLoaded) {
                ev->result = CDRF_DODEFAULT;
                return;
            }
            bool isSelected = TocDrawItemSelected(win, tocItem, cd);
            bool isHot = (cd->uItemState & CDIS_HOT) != 0;
            SetTocItemDrawColors(tvcd, ev->treeView, tocItem, win);
            if (TocIsEditingItem(win, (HTREEITEM)cd->dwItemSpec)) {
                tvcd->clrText = tvcd->clrTextBk;
            }
            if (isSelected || (ThemeUsesDarkChrome() && isHot) || TocCalibIsActive(win)) {
                ev->result = CDRF_NOTIFYPOSTPAINT;
                return;
            }
            ev->result = CDRF_DODEFAULT;
            return;
        }
        if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
            TocItem* tocItem = (TocItem*)ev->treeItem;
            bool knownTree = win && IsKnownTocTreeModel(win, ev->treeView->treeModel);
            bool isSelected = TocDrawItemSelected(win, tocItem, cd);
            HTREEITEM hItem = (HTREEITEM)cd->dwItemSpec;
            if (isSelected && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
                DrawTocSelectionFill(cd, ev->treeView->hwnd, hItem);
                if (tocItem && !TocIsEditingItem(win, hItem)) {
                    DrawTocWrappedLabel(tvcd, ev->treeView, tocItem, win);
                }
                if (ThemeUsesDarkChrome()) {
                    DrawTocSelectionFrame(cd, ev->treeView->hwnd, hItem);
                }
            } else if (ThemeUsesDarkChrome() && (cd->uItemState & CDIS_HOT) && knownTree && win && win->tocLoaded &&
                       !win->isBeingClosed) {
                DrawTocHotTrackFill(cd, ev->treeView->hwnd, hItem);
                if (tocItem && !TocIsEditingItem(win, hItem)) {
                    DrawTocWrappedLabel(tvcd, ev->treeView, tocItem, win);
                }
            } else if (TocCalibIsActive(win) && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed &&
                       !TocIsEditingItem(win, hItem)) {
                DrawTocWrappedLabel(tvcd, ev->treeView, tocItem, win);
            }
            if (filterActive && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed &&
                !TocIsEditingItem(win, hItem)) {
                DrawTocItemHighlight(ev, win);
            }
            if (TocCalibIsActive(win) && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed &&
                !TocIsEditingItem(win, hItem)) {
                RECT rcRow{};
                if (TreeView_GetItemRect(ev->treeView->hwnd, hItem, &rcRow, FALSE)) {
                    TocCalibDrawColumns(cd->hdc, ev->treeView->hwnd, rcRow, tocItem, win, isSelected);
                }
            }
            ev->result = CDRF_DODEFAULT;
            return;
        }
        return;
    }

    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        TocItem* tocItem = (TocItem*)ev->treeItem;
        if (!tocItem) {
            return;
        }
        bool knownTree = win && IsKnownTocTreeModel(win, ev->treeView->treeModel);
        if (!knownTree || !win || win->isBeingClosed || !win->tocLoaded) {
            ev->result = CDRF_DODEFAULT;
            return;
        }
        SetTocItemDrawColors(tvcd, ev->treeView, tocItem, win);
        if (TocIsEditingItem(win, (HTREEITEM)cd->dwItemSpec)) {
            tvcd->clrText = tvcd->clrTextBk;
        }
        bool isSelected = TocDrawItemSelected(win, tocItem, cd);
        if (tocItem->fontFlags != 0) {
            UpdateFont(win, ev->treeView->hwnd, cd->hdc, tocItem->fontFlags);
        }
        // Default item paint runs between prepaint and postpaint; selection fill is
        // redrawn over the full row in postpaint before the wrapped label.
        LRESULT res = CDRF_NOTIFYPOSTPAINT;
        if (tocItem->fontFlags != 0) {
            res |= CDRF_NEWFONT;
        }
        ev->result = res;
        return;
    }

    if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
        TocItem* tocItem = (TocItem*)ev->treeItem;
        bool knownTree = win && IsKnownTocTreeModel(win, ev->treeView->treeModel);
        bool isSelected = TocDrawItemSelected(win, tocItem, cd);
        bool isHot = (cd->uItemState & CDIS_HOT) != 0;
        HTREEITEM hItem = (HTREEITEM)cd->dwItemSpec;
        if (knownTree && win && win->tocLoaded && !win->isBeingClosed) {
            if (isSelected) {
                DrawTocSelectionFill(cd, ev->treeView->hwnd, hItem);
            } else if (ThemeUsesDarkChrome() && isHot) {
                DrawTocHotTrackFill(cd, ev->treeView->hwnd, hItem);
            }
        }
        if (tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed && !TocIsEditingItem(win, hItem)) {
            DrawTocWrappedLabel(tvcd, ev->treeView, tocItem, win);
        }
        if (ThemeUsesDarkChrome() && isSelected && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
            DrawTocSelectionFrame(cd, ev->treeView->hwnd, hItem);
        }
        if (filterActive && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed &&
            !TocIsEditingItem(win, hItem)) {
            DrawTocItemHighlight(ev, win);
        }
        if (TocCalibIsActive(win) && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed &&
            !TocIsEditingItem(win, hItem)) {
            RECT rcRow{};
            if (TreeView_GetItemRect(ev->treeView->hwnd, hItem, &rcRow, FALSE)) {
                TocCalibDrawColumns(cd->hdc, ev->treeView->hwnd, rcRow, tocItem, win, isSelected);
            }
        }
        ev->result = CDRF_DODEFAULT;
        return;
    }
}

// disabled becaues of https://github.com/sumatrapdfreader/sumatrapdf/issues/2202
// it was added for https://github.com/sumatrapdfreader/sumatrapdf/issues/1716
// but unclear if its still needed
// this calls GoToTocLinkTask) which will eventually call GoToPage()
// which adds nav point. Maybe I should not add nav point
// if going to the same page?
void TocTreeClick(TreeView::ClickEvent* ev) {
    ev->result = 0;
    if (!ev || !ev->treeView || !ev->treeItem) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (!win || !TocCalibIsActive(win)) {
        return;
    }
    RECT rcRow{};
    if (!ev->treeView->GetItemRect(ev->treeItem, false, rcRow)) {
        return;
    }
    if (TocCalibHandleRowClick(win, (TocItem*)ev->treeItem, ev->mouseWindow.x, ev->mouseWindow.y, rcRow)) {
        ev->result = 1;
    }
}

static void TocTreeSelectionChanged(TreeView::SelectionChangedEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    ReportIf(!win);
    if (win->tocSuppressGoTo) {
        return;
    }

    // When the focus is set to the toc window the first item in the treeview is automatically
    // selected and a TVN_SELCHANGEDW notification message is sent with the special code pnmtv->action ==
    // 0x00001000. We have to ignore this message to prevent the current page to be changed.
    // The case pnmtv->action==TVC_UNKNOWN is ignored because
    // it corresponds to a notification sent by
    // the function TreeView_DeleteAllItems after deletion of the item.
    bool shouldHandle = ev->byKeyboard || ev->byMouse;
    if (!shouldHandle) {
        return;
    }
    if (win->tocTreeView == ev->treeView && ev->byKeyboard && !IsShiftPressed() && !IsCtrlPressed()) {
        TocItem* item = (TocItem*)ev->selectedItem;
        win->tocSelectedIds.Reset();
        if (item && item->id) {
            win->tocSelectedIds.Append(item->id);
            win->tocAnchorId = item->id;
        }
        win->tocSelectionOwned = true;
        TocInvalidateTree(win);
    }
    bool allowExternal = ev->byMouse;
    if (!TocCalibIsActive(win)) {
        GoToTocTreeItem(win, ev->selectedItem, allowExternal);
    }
}

static void TocSelectAllItems(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !tab->currToc || !tab->currToc->root) {
        return;
    }
    Vec<int> ids;
    if (HasTocFilter(win)) {
        Vec<TocItem*> visible = TocVisibleItems(win->tocTreeView);
        for (TocItem* item : visible) {
            if (item && item->id) {
                ids.Append(item->id);
            }
        }
    } else {
        CollectTocItemIds(tab->currToc->root->child, ids);
    }
    TocItem* focus = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
    if (!focus && !ids.empty()) {
        focus = TocItemFromId(win, ids.At(0));
    }
    TocSetSelectedIds(win, ids, focus ? focus->id : (ids.empty() ? 0 : ids.At(0)), focus, false);
}

static Vec<TocItem*> TocVisibleItems(TreeView* tv) {
    Vec<TocItem*> items;
    if (!tv || !tv->hwnd) {
        return items;
    }
    HTREEITEM h = TreeView_GetRoot(tv->hwnd);
    while (h) {
        TocItem* item = (TocItem*)tv->GetTreeItemByHandle(h);
        if (item) {
            items.Append(item);
        }
        h = TreeView_GetNextVisible(tv->hwnd, h);
    }
    return items;
}

static void TocSelectVisibleRange(MainWindow* win, TocItem* from, TocItem* to) {
    if (!win || !win->tocTreeView || !to) {
        return;
    }
    Vec<TocItem*> visible = TocVisibleItems(win->tocTreeView);
    int iFrom = from ? visible.Find(from) : -1;
    int iTo = visible.Find(to);
    if (iTo < 0) {
        TocSelectOnly(win, to, false);
        return;
    }
    if (iFrom < 0) {
        iFrom = iTo;
    }
    if (iFrom > iTo) {
        int tmp = iFrom;
        iFrom = iTo;
        iTo = tmp;
    }
    Vec<int> ids;
    for (int i = iFrom; i <= iTo; i++) {
        TocItem* item = visible.At(i);
        if (item && item->id) {
            ids.Append(item->id);
        }
    }
    TocSetSelectedIds(win, ids, from ? from->id : to->id, to, false);
}

static constexpr UINT_PTR kTocLabelEditSubclassId = 0x70ced17;

static int TocInPlaceEditRight(MainWindow* win, HWND hwndTv, const RECT& rcLabel, const RECT& rcRow) {
    int right = rcRow.right - 4;
    if (TocCalibIsActive(win)) {
        right -= TocCalibColumnsDx(hwndTv);
    }
    if (right < rcLabel.left + 40) {
        right = rcLabel.left + 40;
    }
    return right;
}

// Same inset as DrawTreeWrappedLabel (InflateRect -2, -1) so F2 text does not jump.
static bool TocInPlaceEditRect(MainWindow* win, HWND hwndTv, RECT& rcOut) {
    rcOut = {};
    if (!win || !win->tocLabelEditItem) {
        return false;
    }
    RECT rcLabel{};
    RECT rcRow{};
    if (!TreeView_GetItemRect(hwndTv, win->tocLabelEditItem, &rcLabel, TRUE)) {
        return false;
    }
    if (!TreeView_GetItemRect(hwndTv, win->tocLabelEditItem, &rcRow, FALSE)) {
        rcRow = rcLabel;
    }
    int x = rcLabel.left + 2;
    int y = rcLabel.top + 1;
    int right = TocInPlaceEditRight(win, hwndTv, rcLabel, rcRow);
    int dx = right - x;
    int dy = rcLabel.bottom - 1 - y;
    if (dx < 40) {
        dx = 40;
    }
    if (dy < 16) {
        dy = rcRow.bottom - rcRow.top;
        if (dy < 16) {
            dy = 16;
        }
        y = rcRow.top + (rcRow.bottom - rcRow.top - dy) / 2;
    }
    rcOut.left = x;
    rcOut.top = y;
    rcOut.right = x + dx;
    rcOut.bottom = y + dy;
    return rcOut.right > rcOut.left && rcOut.bottom > rcOut.top;
}

static void TocStyleInPlaceEdit(HWND hwndEdit, HWND hwndTv) {
    if (DynSetWindowTheme) {
        DynSetWindowTheme(hwndEdit, L"", L"");
    }
    DWORD style = GetWindowLongW(hwndEdit, GWL_STYLE);
    if (style & WS_BORDER) {
        SetWindowLongW(hwndEdit, GWL_STYLE, style & ~WS_BORDER);
    }
    DWORD ex = GetWindowLongW(hwndEdit, GWL_EXSTYLE);
    if (ex & WS_EX_CLIENTEDGE) {
        SetWindowLongW(hwndEdit, GWL_EXSTYLE, ex & ~WS_EX_CLIENTEDGE);
    }
    SendMessageW(hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    HFONT hf = (HFONT)SendMessageW(hwndTv, WM_GETFONT, 0, 0);
    if (hf) {
        SendMessageW(hwndEdit, WM_SETFONT, (WPARAM)hf, FALSE);
    }
}

static void TocFitInPlaceEditToRow(MainWindow* win, HWND hwndEdit) {
    if (!win || !win->tocTreeView || !hwndEdit) {
        return;
    }
    RECT rc{};
    if (!TocInPlaceEditRect(win, win->tocTreeView->hwnd, rc)) {
        return;
    }
    SetWindowPos(hwndEdit, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOCOPYBITS);
}

static LRESULT CALLBACK TocLabelEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId,
                                                 DWORD_PTR data) {
    auto* win = (MainWindow*)data;
    if (msg == WM_WINDOWPOSCHANGING && win && win->tocTreeView && win->tocLabelEditItem) {
        auto* pos = (WINDOWPOS*)lp;
        if (pos && !((pos->flags & SWP_NOMOVE) && (pos->flags & SWP_NOSIZE))) {
            RECT rc{};
            if (TocInPlaceEditRect(win, win->tocTreeView->hwnd, rc)) {
                pos->x = rc.left;
                pos->y = rc.top;
                pos->cx = rc.right - rc.left;
                pos->cy = rc.bottom - rc.top;
                pos->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
            }
        }
    }
    if (msg == WM_NCPAINT) {
        return 0;
    }
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(TocSelectionBgColor());
        FillRect((HDC)wp, &rc, br);
        DeleteObject(br);
        return 1;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TocLabelEditSubclassProc, subclassId);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static bool TocColorInPlaceEdit(HWND hwndEdit, HDC hdc, HBRUSH* brOut) {
    if (!hwndEdit || !hdc || !brOut) {
        return false;
    }
    COLORREF bg = TocSelectionBgColor();
    COLORREF txt = ThemeUsesDarkChrome() ? ThemeReadingTextColor() : ThemeWindowTextColor();
    SetTextColor(hdc, txt);
    SetBkColor(hdc, bg);
    static HBRUSH br = nullptr;
    static COLORREF brBg = (COLORREF)-1;
    if (!br || brBg != bg) {
        if (br) {
            DeleteObject(br);
        }
        br = CreateSolidBrush(bg);
        brBg = bg;
    }
    *brOut = br;
    return true;
}

static void TocPrepareInPlaceEdit(MainWindow* win) {
    if (!win || !win->tocTreeView) {
        return;
    }
    HWND hwndTv = win->tocTreeView->hwnd;
    HWND hwndEdit = TreeView_GetEditControl(hwndTv);
    if (!hwndEdit) {
        return;
    }
    TocStyleInPlaceEdit(hwndEdit, hwndTv);
    RemoveWindowSubclass(hwndEdit, TocLabelEditSubclassProc, kTocLabelEditSubclassId);
    SetWindowSubclass(hwndEdit, TocLabelEditSubclassProc, kTocLabelEditSubclassId, (DWORD_PTR)win);
    TocFitInPlaceEditToRow(win, hwndEdit);
    SendMessageW(hwndEdit, EM_SETSEL, 0, -1);
}

static bool TocIsEditingItem(MainWindow* win, HTREEITEM hItem) {
    return win && hItem && win->tocLabelEditItem == hItem;
}

static void TocStartLabelEdit(MainWindow* win) {
    if (!win || !win->tocTreeView || HasTocFilter(win) || !PdfTocEditableEngine(win)) {
        return;
    }
    TocItem* item = (TocItem*)win->tocTreeView->GetSelection();
    if (!IsPdfTocBookmarkItem(win, item)) {
        return;
    }
    HTREEITEM h = win->tocTreeView->GetHandleByTreeItem((TreeItem)item);
    if (!h) {
        return;
    }
    win->tocLabelEditFromF2 = true;
    win->tocLabelEditItem = h;
    HWND hwndEdit = TreeView_EditLabel(win->tocTreeView->hwnd, h);
    if (!hwndEdit) {
        win->tocLabelEditFromF2 = false;
        win->tocLabelEditItem = nullptr;
    }
}

static void TocBeginLabelEdit(TreeView::LabelEditEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    ev->cancel = !(win && win->tocLabelEditFromF2);
    if (win) {
        win->tocLabelEditFromF2 = false;
        if (ev->cancel) {
            win->tocLabelEditItem = nullptr;
        } else {
            TocPrepareInPlaceEdit(win);
        }
    }
}

static bool TocCommitInPlaceLabelEdit(MainWindow* win) {
    if (!win || !win->tocTreeView || !win->tocLabelEditItem) {
        return false;
    }
    HWND hwnd = win->tocTreeView->hwnd;
    if (!hwnd || !IsWindow(hwnd)) {
        win->tocLabelEditItem = nullptr;
        return false;
    }
    HWND hwndEdit = TreeView_GetEditControl(hwnd);
    if (!hwndEdit || !IsWindow(hwndEdit)) {
        win->tocLabelEditItem = nullptr;
        return false;
    }
    TreeView_EndEditLabelNow(hwnd, FALSE);
    return true;
}

struct TocLabelCommitTask {
    MainWindow* win = nullptr;
    EngineBase* engine = nullptr;
    Vec<int> path;
    char* title = nullptr;
};

static void TocCommitLabelEditOnUi(TocLabelCommitTask* t) {
    if (!t) {
        return;
    }
    bool stillOpen = false;
    for (MainWindow* w : gWindows) {
        if (w == t->win) {
            stillOpen = true;
            break;
        }
    }
    if (stillOpen && t->engine && t->title && PdfTocEditableEngine(t->win) == t->engine) {
        Vec<int> resultPath;
        char* errorRaw = nullptr;
        bool ok = EngineMupdfEditPdfToc(t->engine, PdfTocEditAction::Update, t->path, t->title, nullptr, &resultPath,
                                        &errorRaw);
        AutoFreeStr error(errorRaw);
        if (ok) {
            ReloadPdfTocAfterEdit(t->win, resultPath);
            ToolbarUpdateStateForWindow(t->win, false);
        } else {
            ShowPdfTocEditError(t->win, error);
        }
    }
    str::Free(t->title);
    delete t;
}

static void TocEndLabelEdit(TreeView::LabelEditEvent* ev) {
    ev->result = FALSE;
    MainWindow* winEnd = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (winEnd) {
        winEnd->tocLabelEditItem = nullptr;
    }
    if (!ev->text) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    EngineBase* engine = PdfTocEditableEngine(win);
    TocItem* item = OriginalPdfTocItem(win, (TocItem*)ev->treeItem);
    if (!engine || !IsPdfTocBookmarkItem(win, item)) {
        return;
    }
    TempStr title = ToUtf8Temp(ev->text);
    str::TrimWSInPlace(title, str::TrimOpt::Both);
    if (str::IsEmpty(title)) {
        return;
    }
    if (TocCalibIsActive(win)) {
        str::ReplaceWithCopy(&item->title, title);
        TocCalibRenameItem(win, item, title);
        ev->result = TRUE;
        if (win->tocTreeView && win->tocTreeView->hwnd) {
            InvalidateRect(win->tocTreeView->hwnd, nullptr, FALSE);
        }
        return;
    }
    if (!ConfirmPdfTocSignatureEdit(win, engine)) {
        return;
    }
    str::ReplaceWithCopy(&item->title, title);
    ev->result = TRUE;
    Vec<int> path;
    if (!PdfTocPathForItem(win, item, path)) {
        return;
    }
    auto* task = new TocLabelCommitTask;
    task->win = win;
    task->engine = engine;
    task->path = path;
    task->title = str::Dup(title);
    uitask::Post(MkFunc0(TocCommitLabelEditOnUi, task), "TocCommitLabelEdit");
}

void TocTreeKeyDown2(TreeView::KeyDownEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (!win) {
        ev->result = 0;
        return;
    }
    bool isTocTree = win->tocTreeView == ev->treeView;

    if (ev->keyCode == VK_TAB) {
        if (win->tabsVisible && IsCtrlPressed()) {
            TabsOnCtrlTab(win, IsShiftPressed());
            ev->result = 1;
            return;
        }
        AdvanceFocus(win);
        ev->result = 1;
        return;
    }

    if (!isTocTree) {
        ev->result = 0;
        return;
    }

    if (win->tocDragging && ev->keyCode == VK_ESCAPE) {
        TocCancelDrag(win);
        ev->result = 1;
        return;
    }

    if (ev->keyCode == 'A' && IsCtrlPressed() && !IsShiftPressed()) {
        TocSelectAllItems(win);
        ev->result = 1;
        return;
    }

    if (TocCalibIsActive(win) && IsCtrlPressed() && !IsAltPressed() &&
        TocCalibHandleUndoShortcut(win, ev->treeView ? ev->treeView->hwnd : nullptr, ev->keyCode, true,
                                   IsShiftPressed())) {
        ev->result = 1;
        return;
    }

    if (ev->keyCode == VK_ESCAPE && win->tocSelectedIds.Size() > 1) {
        TocItem* focus = (TocItem*)win->tocTreeView->GetSelection();
        TocSelectOnly(win, focus, false);
        ev->result = 1;
        return;
    }

    EngineBase* engine = PdfTocEditableEngine(win);
    bool canEdit = engine && !HasTocFilter(win);
    // F2 must stay on the TOC item (in-place rename). Do not let it fall through
    // to the frame accelerator table (CmdRenameFile).
    if (ev->keyCode == VK_F2) {
        if (canEdit) {
            TocStartLabelEdit(win);
        }
        ev->result = 1;
        return;
    }
    if (canEdit) {
        if (ev->keyCode == VK_DELETE || ev->keyCode == VK_BACK) {
            ExecutePdfTocEditMany(win, PdfTocEditAction::Delete);
            ev->result = 1;
            return;
        }
        if (ev->keyCode == VK_INSERT && !IsCtrlPressed() && !IsShiftPressed()) {
            TocItem* selected = (TocItem*)win->tocTreeView->GetSelection();
            ExecutePdfTocEdit(win, selected, PdfTocEditAction::AddAfter);
            ev->result = 1;
            return;
        }
        if (IsCtrlPressed() && !IsShiftPressed()) {
            if (ev->keyCode == VK_UP) {
                ExecutePdfTocEditMany(win, PdfTocEditAction::MoveUp);
                ev->result = 1;
                return;
            }
            if (ev->keyCode == VK_DOWN) {
                ExecutePdfTocEditMany(win, PdfTocEditAction::MoveDown);
                ev->result = 1;
                return;
            }
            if (ev->keyCode == VK_LEFT) {
                ExecutePdfTocEditMany(win, PdfTocEditAction::Promote);
                ev->result = 1;
                return;
            }
            if (ev->keyCode == VK_RIGHT) {
                ExecutePdfTocEditMany(win, PdfTocEditAction::Demote);
                ev->result = 1;
                return;
            }
        }
    }

    if (IsShiftPressed() && !IsCtrlPressed() &&
        (ev->keyCode == VK_UP || ev->keyCode == VK_DOWN || ev->keyCode == VK_HOME || ev->keyCode == VK_END)) {
        TocItem* anchor = TocItemFromId(win, win->tocAnchorId);
        TocItem* focus = (TocItem*)win->tocTreeView->GetSelection();
        Vec<TocItem*> visible = TocVisibleItems(win->tocTreeView);
        if (!visible.empty()) {
            TocItem* dest = nullptr;
            if (ev->keyCode == VK_HOME) {
                dest = visible.At(0);
            } else if (ev->keyCode == VK_END) {
                dest = visible.Last();
            } else {
                int iFocus = focus ? visible.Find(focus) : -1;
                if (iFocus >= 0) {
                    int next = ev->keyCode == VK_UP ? iFocus - 1 : iFocus + 1;
                    if (next >= 0 && next < visible.Size()) {
                        dest = visible.At(next);
                    }
                }
            }
            if (dest) {
                if (!anchor) {
                    anchor = focus;
                }
                TocSelectVisibleRange(win, anchor, dest);
                ev->result = 1;
                return;
            }
        }
    }

    ev->result = 0;
}

#ifdef DISPLAY_TOC_PAGE_NUMBERS
static void TocTreeMsgFilter(WndEvent*) {
    switch (msg) {
        case WM_SIZE:
        case WM_HSCROLL:
            // Repaint the ToC so that RelayoutTocItem is called for all items
            PostMessageW(hwnd, WM_APP_REPAINT_TOC, 0, 0);
            break;
        case WM_APP_REPAINT_TOC:
            InvalidateRect(hwnd, nullptr, TRUE);
            UpdateWindow(hwnd);
            break;
    }
}
#endif

// Position label with close button and tree window within their parent.
// Used for toc and favorites.
void LayoutTreeContainer(LabelWithCloseWnd* l, HWND hwndTree) {
    HWND hwndContainer = GetParent(hwndTree);
    Size labelSize = l->GetIdealSize();
    Rect rc = WindowRect(hwndContainer);
    int dy = rc.dy;
    int y = 0;
    MoveWindow(l->hwnd, y, 0, rc.dx, labelSize.dy, TRUE);
    dy -= labelSize.dy;
    y += labelSize.dy;
    MoveWindow(hwndTree, 0, y, rc.dx, dy, TRUE);
}

// Position label, filter edit, and tree window within toc container.
static void LayoutTocContainer(MainWindow* win) {
    LabelWithCloseWnd* l = win->tocLabelWithClose;
    Edit* edit = win->tocFilterEdit;
    TreeView* treeView = win->tocTreeView;
    HWND hwndContainer = win->hwndTocBox;
    Size labelSize = l->GetIdealSize();
    Rect rc = WindowRect(hwndContainer);
    int dy = rc.dy;
    int y = 0;
    BOOL liveDrag = TreeWrapLiveResizeSuspended() ? TRUE : FALSE;
    // NOCOPYBITS so old pixels are not smeared; do not use SWP_NOREDRAW
    // or the tree/filter leave white/black ghosts on every mouse-move.
    UINT liveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
    auto place = [&](HWND hwnd, int x, int y, int dx, int dy) {
        if (!hwnd) {
            return;
        }
        if (liveDrag) {
            SetWindowPos(hwnd, nullptr, x, y, dx, dy, liveFlags);
        } else {
            MoveWindow(hwnd, x, y, dx, dy, TRUE);
        }
    };
    place(l->hwnd, 0, y, rc.dx, labelSize.dy);
    dy -= labelSize.dy;
    y += labelSize.dy;
    HWND loadHwnd = GetDocumentLoadingNotificationHwnd(win->hwndFrame, win->hwndCanvas);
    bool loadInToc = loadHwnd && GetParent(loadHwnd) == hwndContainer;
    if (loadInToc) {
        Rect rn = WindowRect(loadHwnd);
        int loadDy = rn.dy;
        place(loadHwnd, 0, y, rc.dx, loadDy);
        dy -= loadDy;
        y += loadDy;
    }
    int editStyleVis = 0;
    int rowDy = 0;
    if (edit && edit->hwnd) {
        editStyleVis = (GetWindowLongW(edit->hwnd, GWL_STYLE) & WS_VISIBLE) ? 1 : 0;
        Size editSize = edit->GetIdealSize();
        // IsWindowVisible is false while RelayoutFrame has WM_SETREDRAW off on the
        // frame, even though the edit still has WS_VISIBLE. Using it here slides the
        // tree over the search row (first item vs. filter competing during splitter drag).
        if (editStyleVis) {
            rowDy = editSize.dy;
            place(edit->hwnd, 0, y, rc.dx, rowDy);
            dy -= rowDy;
            y += rowDy;
        }
    }
    int barDy = 0;
    if (TocCalibIsActive(win)) {
        barDy = TocCalibBarDy(win);
        if (barDy > dy - 40) {
            barDy = dy - 40;
        }
        if (barDy < 0) {
            barDy = 0;
        }
        dy -= barDy;
    }
    if (treeView && treeView->hwnd) {
        place(treeView->hwnd, 0, y, rc.dx, dy);
        y += dy;
    }
    if (barDy > 0) {
        RelayoutTocCalib(win);
    }
}

void RelayoutTocContainer(MainWindow* win) {
    if (!win) {
        return;
    }
    LayoutTocContainer(win);
}

static bool TocSidebarHasBookmarkItems(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !tab->ctrl) {
        return false;
    }
    EngineBase* engine = tab->GetEngine();
    TocTree* toc = tab->currToc;
    if (engine) {
        toc = engine->PeekCachedToc();
        // currToc is a non-owning alias of the engine tree. Theme/reflow rebuilds
        // and DiscardTocTree leave it dangling; never walk it if Peek disagrees.
        if (tab->currToc != toc) {
            tab->currToc = toc;
        }
    }
    TocItem* root = toc ? toc->root : nullptr;
    return root && root->child;
}

static bool TocSidebarShowEmptyHint(MainWindow* win) {
    return win && win->tocVisible && win->tocTreeView && !HasTocFilter(win) && !TocSidebarHasBookmarkItems(win);
}

static bool TocEmptyExtractActionRect(MainWindow* win, HWND hwnd, RECT* actionOut) {
    if (actionOut) {
        *actionOut = {};
    }
    if (!hwnd || !win || !TocSidebarShowEmptyHint(win) || !PdfTocEditableEngine(win)) {
        return false;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    int pad = DpiScale(hwnd, 16);
    rc.left += pad;
    rc.right -= pad;
    rc.top += pad;
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return false;
    }
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return false;
    }
    HFONT font = GetAppTreeFontForHwnd(win->hwndFrame);
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    const WCHAR* title = ToWStrTemp(_TRA("No bookmarks"));
    RECT titleRc = rc;
    DrawTextW(hdc, title, -1, &titleRc, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    int titleH = titleRc.bottom - titleRc.top;
    const WCHAR* action = ToWStrTemp(_TRA("Extract Table of Contents"));
    int actionTop = rc.top + titleH + DpiScale(hwnd, 8);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, action, lstrlenW(action), &sz);
    RECT actionRc;
    int avail = rc.right - rc.left;
    if (sz.cx > 0 && sz.cx <= avail) {
        int x = rc.left + (avail - sz.cx) / 2;
        actionRc = {x, actionTop, x + sz.cx, actionTop + sz.cy};
    } else {
        actionRc = rc;
        actionRc.top = actionTop;
        DrawTextW(hdc, action, -1, &actionRc, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    }
    int hitPad = DpiScale(hwnd, 4);
    InflateRect(&actionRc, hitPad, hitPad);
    if (old) {
        SelectObject(hdc, old);
    }
    ReleaseDC(hwnd, hdc);
    if (actionRc.right <= actionRc.left || actionRc.bottom <= actionRc.top) {
        return false;
    }
    if (actionOut) {
        *actionOut = actionRc;
    }
    return true;
}

static bool TocEmptyExtractHitTest(MainWindow* win, HWND hwnd, POINT pt) {
    RECT actionRc;
    if (!TocEmptyExtractActionRect(win, hwnd, &actionRc)) {
        return false;
    }
    return PtInRect(&actionRc, pt) != FALSE;
}

static void DrawTocEmptyHint(MainWindow* win, HWND hwnd) {
    if (!hwnd || !TocSidebarShowEmptyHint(win)) {
        return;
    }
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    int pad = DpiScale(hwnd, 16);
    rc.left += pad;
    rc.right -= pad;
    rc.top += pad;
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        ReleaseDC(hwnd, hdc);
        return;
    }
    HFONT font = GetAppTreeFontForHwnd(win->hwndFrame);
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
    const WCHAR* title = ToWStrTemp(_TRA("No bookmarks"));
    RECT titleRc = rc;
    DrawTextW(hdc, title, -1, &titleRc, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    int titleH = titleRc.bottom - titleRc.top;
    titleRc = rc;
    titleRc.bottom = titleRc.top + titleH;
    DrawTextW(hdc, title, -1, &titleRc, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    bool canExtract = PdfTocEditableEngine(win) != nullptr;
    if (canExtract) {
        RECT actionRc = rc;
        actionRc.top = titleRc.bottom + DpiScale(hwnd, 8);
        SIZE sz{};
        const WCHAR* action = ToWStrTemp(_TRA("Extract Table of Contents"));
        GetTextExtentPoint32W(hdc, action, lstrlenW(action), &sz);
        int avail = rc.right - rc.left;
        if (sz.cx > 0 && sz.cx <= avail) {
            int x = rc.left + (avail - sz.cx) / 2;
            actionRc = {x, actionRc.top, x + sz.cx, actionRc.top + sz.cy};
        }
        SetTextColor(hdc, ThemeWindowLinkColor());
        DrawTextW(hdc, action, -1, &actionRc, DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    }
    if (old) {
        SelectObject(hdc, old);
    }
    ReleaseDC(hwnd, hdc);
}

static PdfTocDropPos TocDropPosFromInt(int pos) {
    if (pos == 0) {
        return PdfTocDropPos::Before;
    }
    if (pos == 2) {
        return PdfTocDropPos::Child;
    }
    return PdfTocDropPos::After;
}

static int TocDropLineLeft(HWND hwnd, HTREEITEM dest, const RECT& rcRow) {
    RECT rcText{};
    if (TreeView_GetItemRect(hwnd, dest, &rcText, TRUE) && rcText.left > rcRow.left) {
        return rcText.left;
    }
    int indent = (int)TreeView_GetIndent(hwnd);
    if (indent < 8) {
        indent = DpiScale(hwnd, 16);
    }
    int level = 0;
    for (HTREEITEM p = TreeView_GetParent(hwnd, dest); p; p = TreeView_GetParent(hwnd, p)) {
        level++;
    }
    return rcRow.left + DpiScale(hwnd, 4) + level * indent;
}

static int TocDropLineRight(MainWindow* win, HWND hwnd, const RECT& rcRow, int xLeft) {
    int right = rcRow.right - DpiScale(hwnd, 4);
    if (win && TocCalibIsActive(win)) {
        right -= TocCalibColumnsDx(hwnd);
    }
    int minRight = xLeft + DpiScale(hwnd, 24);
    if (right < minRight) {
        right = minRight;
    }
    return right;
}

static HTREEITEM TocLastVisibleDescendant(HWND hwnd, HTREEITEM item) {
    if (!hwnd || !item) {
        return item;
    }
    HTREEITEM last = item;
    HTREEITEM next = TreeView_GetNextVisible(hwnd, item);
    while (next) {
        bool desc = false;
        for (HTREEITEM p = TreeView_GetParent(hwnd, next); p; p = TreeView_GetParent(hwnd, p)) {
            if (p == item) {
                desc = true;
                break;
            }
        }
        if (!desc) {
            break;
        }
        last = next;
        next = TreeView_GetNextVisible(hwnd, next);
    }
    return last;
}

static COLORREF TocDropIndicatorColor() {
    COLORREF link = ThemeWindowLinkColor();
    int r = GetRValue(link);
    int g = GetGValue(link);
    int b = GetBValue(link);
    int y = (r * 30 + g * 59 + b * 11) / 100;
    r = (r + y * 2) / 3;
    g = (g + y * 2) / 3;
    b = (b + y * 2) / 3;
    COLORREF muted = RGB(r, g, b);
    return ThemeUsesDarkChrome() ? AccentColor(muted, 0, -18) : AccentColor(muted, 22);
}

static void DrawTocDropIndicator(MainWindow* win, HWND hwnd) {
    if (!win || !win->tocDragging || !win->tocDropItem) {
        return;
    }
    RECT rc;
    if (!TreeView_GetItemRect(hwnd, win->tocDropItem, &rc, FALSE)) {
        return;
    }
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return;
    }
    COLORREF col = TocDropIndicatorColor();
    int stroke = DpiScale(hwnd, 1);
    if (stroke < 1) {
        stroke = 1;
    }
    HPEN pen = CreatePen(PS_SOLID, stroke, col);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int x0 = TocDropLineLeft(hwnd, win->tocDropItem, rc);
    int x1 = TocDropLineRight(win, hwnd, rc, x0);
    if (win->tocDropPos == 2) {
        int boxL = x0 - DpiScale(hwnd, 4);
        if (boxL < rc.left + 2) {
            boxL = rc.left + 2;
        }
        HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, boxL, rc.top + 1, x1, rc.bottom - 1);
        SelectObject(hdc, oldBr);
        // First-child slot: a more-indented line under the title.
        int childIndent = (int)TreeView_GetIndent(hwnd);
        if (childIndent < 8) {
            childIndent = DpiScale(hwnd, 16);
        }
        int cx0 = x0 + childIndent;
        int cy = rc.bottom - 1;
        int tick = DpiScale(hwnd, 7);
        MoveToEx(hdc, cx0, cy - tick, nullptr);
        LineTo(hdc, cx0, cy);
        LineTo(hdc, x1, cy);
    } else if (win->tocDropPos == 0) {
        int y = rc.top + 1;
        int tick = DpiScale(hwnd, 7);
        MoveToEx(hdc, x0, y + tick, nullptr);
        LineTo(hdc, x0, y);
        LineTo(hdc, x1, y);
    } else {
        // After: same level as dest. If dest is expanded, the insert
        // is after the whole subtree (before the next sibling), so draw
        // there — a full-row line under the title looks like "first child".
        RECT lineRow = rc;
        HTREEITEM last = TocLastVisibleDescendant(hwnd, win->tocDropItem);
        if (last && last != win->tocDropItem) {
            RECT rcLast{};
            if (TreeView_GetItemRect(hwnd, last, &rcLast, FALSE)) {
                lineRow = rcLast;
            }
        }
        int y = lineRow.bottom - 1;
        int tick = DpiScale(hwnd, 7);
        if (last && last != win->tocDropItem && lineRow.bottom > rc.bottom) {
            MoveToEx(hdc, x0, rc.bottom, nullptr);
            LineTo(hdc, x0, y);
        }
        MoveToEx(hdc, x0, y - tick, nullptr);
        LineTo(hdc, x0, y);
        LineTo(hdc, x1, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    ReleaseDC(hwnd, hdc);
}

static TocItem* TocLastVisibleItem(TreeView* tv) {
    if (!tv || !tv->hwnd) {
        return nullptr;
    }
    HTREEITEM h = TreeView_GetRoot(tv->hwnd);
    HTREEITEM last = h;
    while (h) {
        last = h;
        h = TreeView_GetNextVisible(tv->hwnd, h);
    }
    return last ? (TocItem*)tv->GetTreeItemByHandle(last) : nullptr;
}

static TocItem* TocFirstVisibleItem(TreeView* tv) {
    if (!tv || !tv->hwnd) {
        return nullptr;
    }
    HTREEITEM h = TreeView_GetRoot(tv->hwnd);
    return h ? (TocItem*)tv->GetTreeItemByHandle(h) : nullptr;
}

static bool TocPathIsDescendantOf(const Vec<int>& dest, const Vec<int>& ancestor) {
    if (dest.Size() <= ancestor.Size() || ancestor.empty()) {
        return false;
    }
    for (int i = 0; i < ancestor.Size(); i++) {
        if (dest.At(i) != ancestor.At(i)) {
            return false;
        }
    }
    return true;
}

static bool TocDropAllowed(MainWindow* win, TocItem* dest) {
    if (!dest || !IsPdfTocBookmarkItem(win, dest)) {
        return false;
    }
    if (TocItemIsMultiSelected(win, dest)) {
        return false;
    }
    Vec<int> destPath;
    if (!PdfTocPathForItem(win, dest, destPath)) {
        return false;
    }
    Vec<TocItem*> moving = TocSelectedBookmarkItems(win);
    for (TocItem* item : moving) {
        Vec<int> src;
        if (!PdfTocPathForItem(win, item, src)) {
            continue;
        }
        if (TocPathIsDescendantOf(destPath, src)) {
            return false;
        }
    }
    return true;
}

static bool TocUpdateDropTarget(MainWindow* win, POINT pt) {
    TreeView* tv = win->tocTreeView;
    HWND hwnd = tv->hwnd;
    TVHITTESTINFO ht{};
    ht.pt = pt;
    HTREEITEM hItem = TreeView_HitTest(hwnd, &ht);
    int dropPos = 1;
    if (ht.flags & TVHT_ABOVE) {
        TocItem* first = TocFirstVisibleItem(tv);
        hItem = first ? tv->GetHandleByTreeItem((TreeItem)first) : nullptr;
        dropPos = 0;
    } else if (!hItem || (ht.flags & (TVHT_NOWHERE | TVHT_BELOW | TVHT_TOLEFT | TVHT_TORIGHT))) {
        TocItem* last = TocLastVisibleItem(tv);
        hItem = last ? tv->GetHandleByTreeItem((TreeItem)last) : nullptr;
        dropPos = 1;
    } else {
        RECT rc;
        if (TreeView_GetItemRect(hwnd, hItem, &rc, FALSE)) {
            int h = rc.bottom - rc.top;
            int y = pt.y - rc.top;
            if (h <= 0) {
                dropPos = 1;
            } else if (y < h / 3) {
                dropPos = 0;
            } else if (y > (2 * h) / 3) {
                dropPos = 1;
            } else {
                dropPos = 2;
            }
        }
    }
    TocItem* dest = hItem ? (TocItem*)tv->GetTreeItemByHandle(hItem) : nullptr;
    dest = OriginalPdfTocItem(win, dest);
    bool valid = TocDropAllowed(win, dest);
    HTREEITEM newItem = valid ? hItem : nullptr;
    int newPos = valid ? dropPos : 1;
    if (newItem != win->tocDropItem || newPos != win->tocDropPos) {
        win->tocDropItem = newItem;
        win->tocDropPos = newPos;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int margin = DpiScale(hwnd, 18);
    if (pt.y < rcClient.top + margin) {
        SendMessageW(hwnd, WM_VSCROLL, SB_LINEUP, 0);
    } else if (pt.y > rcClient.bottom - margin) {
        SendMessageW(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
    }
    if (valid && dropPos == 2 && dest && dest->child && !tv->IsExpanded((TreeItem)dest)) {
        SetTimer(hwnd, kTocDragExpandTimerId, 600, nullptr);
    } else {
        KillTimer(hwnd, kTocDragExpandTimerId);
    }
    return valid;
}

static bool TocTreeHandleMouse(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!win || !win->tocTreeView || win->tocTreeView->hwnd != hwnd) {
        return false;
    }
    if (TocSidebarShowEmptyHint(win)) {
        return false;
    }

    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    TreeView* tv = win->tocTreeView;

    if (msg == WM_LBUTTONDBLCLK && TocCalibIsActive(win)) {
        TVHITTESTINFO ht{};
        ht.pt = pt;
        TreeView_HitTest(hwnd, &ht);
        if (ht.flags & TVHT_ONITEMBUTTON) {
            return false;
        }
        if (TocCalibHandleTreeClick(win, hwnd, pt)) {
            return true;
        }
        TocItem* destItem = ht.hItem ? (TocItem*)tv->GetTreeItemByHandle(ht.hItem) : nullptr;
        if (destItem) {
            TocCalibJumpToItemContents(win, destItem);
        }
        return true;
    }

    if (msg == WM_MOUSEWHEEL && TocCalibIsActive(win)) {
        TocCalibClosePageEdit(true);
    }

    if (msg == WM_LBUTTONDOWN) {
        Vec<int> labelClickPath;
        bool endingLabel = win->tocLabelEditItem && TreeView_GetEditControl(hwnd);
        if (endingLabel) {
            TVHITTESTINFO ht0{};
            ht0.pt = pt;
            TreeView_HitTest(hwnd, &ht0);
            if (!(ht0.flags & TVHT_ONITEMBUTTON)) {
                TocItem* it0 = ht0.hItem ? (TocItem*)tv->GetTreeItemByHandle(ht0.hItem) : nullptr;
                if (it0) {
                    PdfTocPathForItem(win, it0, labelClickPath);
                }
            }
            TocCommitInPlaceLabelEdit(win);
            tv = win->tocTreeView;
            if (!tv || !tv->hwnd) {
                return true;
            }
            hwnd = tv->hwnd;
        }
        if (TocCalibIsActive(win)) {
            TocCalibClosePageEdit(true);
            tv = win->tocTreeView;
            if (!tv || !tv->hwnd) {
                return true;
            }
            hwnd = tv->hwnd;
        }
        if (endingLabel) {
            WindowTab* tab = win->CurrentTab();
            TocItem* next = nullptr;
            if (tab && tab->currToc && tab->currToc->root && !labelClickPath.empty()) {
                next = PdfTocItemAtPath(tab->currToc->root->child, labelClickPath);
            }
            if (next) {
                TocSelectOnly(win, next, true);
            }
            return true;
        }
        if (TocCalibIsActive(win) && TocCalibIsPageControlAt(win, hwnd, pt)) {
            TVHITTESTINFO htCalib{};
            htCalib.pt = pt;
            TreeView_HitTest(hwnd, &htCalib);
            TocItem* calibItem = htCalib.hItem ? (TocItem*)tv->GetTreeItemByHandle(htCalib.hItem) : nullptr;
            if (calibItem) {
                TocSelectOnly(win, calibItem, false);
            }
            TocCalibHandleTreeClick(win, hwnd, pt);
            return true;
        }
        TVHITTESTINFO ht{};
        ht.pt = pt;
        TreeView_HitTest(hwnd, &ht);
        if (ht.flags & TVHT_ONITEMBUTTON) {
            return false;
        }
        TocItem* item = ht.hItem ? (TocItem*)tv->GetTreeItemByHandle(ht.hItem) : nullptr;
        SetFocus(hwnd);
        bool ctrl = (wp & MK_CONTROL) != 0;
        bool shift = (wp & MK_SHIFT) != 0;
        if (!item) {
            if (!ctrl && !shift) {
                TocSelectOnly(win, nullptr, false);
            }
            return true;
        }
        if (ctrl && !shift) {
            Vec<int> ids = win->tocSelectedIds;
            if (ids.Contains(item->id)) {
                ids.Remove(item->id);
            } else if (item->id) {
                ids.Append(item->id);
            }
            TocSetSelectedIds(win, ids, item->id, item, false);
            return true;
        }
        if (shift) {
            TocItem* anchor = TocItemFromId(win, win->tocAnchorId);
            if (!anchor) {
                anchor = (TocItem*)tv->GetSelection();
            }
            TocSelectVisibleRange(win, anchor, item);
            return true;
        }
        bool already = TocItemIsMultiSelected(win, item) ||
                       (win->tocSelectedIds.Size() <= 1 && (TocItem*)tv->GetSelection() == item);
        win->tocDragArmed = PdfTocEditableEngine(win) && !HasTocFilter(win) && IsPdfTocBookmarkItem(win, item);
        win->tocDragging = false;
        win->tocDragWasSelected = already && win->tocSelectedIds.Size() > 1;
        win->tocDragStart = pt;
        win->tocDragItemId = item->id;
        if (!already) {
            TocSelectOnly(win, item, !TocCalibIsActive(win));
        }
        if (TocCalibIsActive(win)) {
            GoToTocTreeItem(win, (TreeItem)item, true);
        }
        // Capture only when a PDF TOC drag can start. EPUB/MOBI/AZW3 must not
        // keep capture after click: the tree view uses TVS_TRACKSELECT (hand
        // cursor), and an unreleased capture sends all clicks back to the TOC.
        if (win->tocDragArmed) {
            SetCapture(hwnd);
        }
        return true;
    }

    if (msg == WM_MOUSEMOVE && (win->tocDragArmed || win->tocDragging) && (wp & MK_LBUTTON)) {
        int dx = pt.x - win->tocDragStart.x;
        int dy = pt.y - win->tocDragStart.y;
        int threshX = GetSystemMetrics(SM_CXDRAG);
        int threshY = GetSystemMetrics(SM_CYDRAG);
        if (!win->tocDragging && (abs(dx) > threshX || abs(dy) > threshY)) {
            if (!win->tocDragArmed || HasTocFilter(win) || !PdfTocEditableEngine(win)) {
                win->tocDragArmed = false;
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                return true;
            }
            if (win->tocSelectedIds.empty() && win->tocDragItemId) {
                win->tocSelectedIds.Append(win->tocDragItemId);
            }
            win->tocDragging = true;
            SetCursorCached(IDC_ARROW);
        }
        if (win->tocDragging) {
            bool valid = TocUpdateDropTarget(win, pt);
            SetCursorCached(valid ? IDC_ARROW : IDC_NO);
        }
        return true;
    }

    if (msg == WM_LBUTTONUP) {
        bool armed = win->tocDragArmed;
        bool dragging = win->tocDragging;
        HTREEITEM dropItem = win->tocDropItem;
        int dropPos = win->tocDropPos;
        bool wasSelected = win->tocDragWasSelected;
        int clickId = win->tocDragItemId;
        POINT upPt = pt;
        win->tocDragArmed = false;
        win->tocDragging = false;
        win->tocDropItem = nullptr;
        KillTimer(hwnd, kTocDragExpandTimerId);
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (!armed && !dragging) {
            return false;
        }
        if (dragging && dropItem) {
            TocItem* dest = (TocItem*)tv->GetTreeItemByHandle(dropItem);
            ExecutePdfTocDrop(win, dest, TocDropPosFromInt(dropPos));
        } else if (!dragging && TocCalibIsActive(win) && TocCalibHandleTreeClick(win, hwnd, upPt)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        } else if (!dragging && wasSelected) {
            TocItem* item = TocItemFromId(win, clickId);
            TocSelectOnly(win, item, !TocCalibIsActive(win));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    if (msg == WM_CAPTURECHANGED) {
        if (win->tocDragging || win->tocDragArmed) {
            win->tocDragArmed = false;
            win->tocDragging = false;
            win->tocDropItem = nullptr;
            KillTimer(hwnd, kTocDragExpandTimerId);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return false;
    }

    if (msg == WM_TIMER && wp == kTocDragExpandTimerId) {
        KillTimer(hwnd, kTocDragExpandTimerId);
        if (win->tocDragging && win->tocDropItem && win->tocDropPos == 2) {
            TreeView_Expand(hwnd, win->tocDropItem, TVE_EXPAND);
        }
        return true;
    }

    if (msg == WM_RBUTTONDOWN) {
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        TVHITTESTINFO ht{};
        ht.pt = pt;
        TreeView_HitTest(hwnd, &ht);
        TocItem* item = ht.hItem ? (TocItem*)tv->GetTreeItemByHandle(ht.hItem) : nullptr;
        if (item && !TocItemIsMultiSelected(win, item)) {
            TocSelectOnly(win, item, false);
        }
        return false;
    }

    return false;
}

static LRESULT CALLBACK WndProcTocTree(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_PAINT) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        DrawTocEmptyHint(win, hwnd);
        DrawTocDropIndicator(win, hwnd);
        return r;
    }
    if (msg == WM_CTLCOLOREDIT) {
        HBRUSH br = nullptr;
        if (TocCalibColorPageEdit((HWND)lp, (HDC)wp, &br)) {
            return (LRESULT)br;
        }
        HWND hwndLabelEdit = TreeView_GetEditControl(hwnd);
        if (hwndLabelEdit && (HWND)lp == hwndLabelEdit && TocColorInPlaceEdit(hwndLabelEdit, (HDC)wp, &br)) {
            return (LRESULT)br;
        }
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && TocCalibIsActive(win)) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (TocCalibIsPageFieldAt(win, hwnd, pt)) {
            SetCursorCached(IDC_IBEAM);
            return TRUE;
        }
    }
    if (TocTreeHandleMouse(win, hwnd, msg, wp, lp)) {
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && win && win->tocDragging) {
        TocCancelDrag(win);
        return 0;
    }
    if (msg == WM_LBUTTONUP && TocSidebarShowEmptyHint(win) && PdfTocEditableEngine(win) && !ExtractPdfTocIsRunning()) {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        bool inHit = TocEmptyExtractHitTest(win, hwnd, pt);
        if (inHit) {
            HandleExtractPdfTocCommand(win);
            return 0;
        }
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && TocSidebarShowEmptyHint(win) && PdfTocEditableEngine(win)) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        bool inHit = TocEmptyExtractHitTest(win, hwnd, pt);
        SetCursorCached(inHit ? IDC_HAND : IDC_ARROW);
        return TRUE;
    }
    if (msg == WM_CONTEXTMENU && win && win->tocTreeView) {
        POINT ptScreen = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT ptWindow = ptScreen;
        if (ptScreen.x != -1 || ptScreen.y != -1) {
            MapWindowPoints(HWND_DESKTOP, hwnd, &ptWindow, 1);
        }
        ContextMenuEvent ev;
        ev.w = win->tocTreeView;
        ev.mouseScreen = Point(ptScreen.x, ptScreen.y);
        ev.mouseWindow = Point(ptWindow.x, ptWindow.y);
        TocContextMenu(&ev);
        return 0;
    }
    if (msg == WM_VSCROLL) {
        if (TocCalibIsActive(win)) {
            TocCalibClosePageEdit(true);
        }
        WORD code = LOWORD(wp);
        if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) {
            gTocFastScrollHwnd = hwnd;
        } else if (code == SB_ENDSCROLL) {
            if (gTocFastScrollHwnd == hwnd) {
                gTocFastScrollHwnd = nullptr;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && win && win->tocDragging) {
        return TRUE;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && win && win->ctrl && win->tocTreeView) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        TreeItem ti = win->tocTreeView->GetItemAt(pt.x, pt.y);
        if (ti != TreeModel::kNullItem) {
            TocItem* tocItem = (TocItem*)ti;
            if (IsTocInternalPageItem(tocItem, win->ctrl) && !IsTocPageReachable(win->ctrl, tocItem)) {
                SetCursorCached(IDC_ARROW);
                return TRUE;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProcTocBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId, DWORD_PTR data) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    if (msg == WM_CONTEXTMENU && win->tocTreeView) {
        POINT ptScreen = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT ptWindow = ptScreen;
        if (ptScreen.x != -1 || ptScreen.y != -1) {
            MapWindowPoints(HWND_DESKTOP, win->tocTreeView->hwnd, &ptWindow, 1);
        }
        ContextMenuEvent ev;
        ev.w = win->tocTreeView;
        ev.mouseScreen = Point(ptScreen.x, ptScreen.y);
        ev.mouseWindow = Point(ptWindow.x, ptWindow.y);
        TocContextMenu(&ev);
        return 0;
    }

    if (msg == WM_ERASEBKGND) {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        TreeView* tv = win->tocTreeView;
        COLORREF bgCol = SidebarBackgroundColor(tv ? tv->bgColor : kColorUnset);
        HBRUSH br = CreateSolidBrush(bgCol);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    LRESULT res = 0;
    res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    TreeView* treeView = win->tocTreeView;

    switch (msg) {
        case WM_SIZE:
            LayoutTocContainer(win);
            if (TreeWrapUpdatesSuspended()) {
                KillTreeWrapHeightTimer(hwnd);
                break;
            }
            // Window resize (not splitter drag): debounce wrap-height recalc.
            ScheduleTreeWrapHeightRecalc(hwnd);
            break;

        case WM_TIMER:
            if (wp == kTreeWrapHeightTimerId) {
                if (TreeWrapUpdatesSuspended()) {
                    KillTreeWrapHeightTimer(hwnd);
                    return 0;
                }
                KillTreeWrapHeightTimer(hwnd);
                TocRecalcAllItemHeights(win);
                InvalidateTocTree(win);
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_TOC_LABEL_WITH_CLOSE) {
                ToggleTocBox(win);
            }
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SubclassToc(MainWindow* win) {
    HWND hwndTocBox = win->hwndTocBox;

    if (win->tocBoxSubclassId == 0) {
        win->tocBoxSubclassId = NextSubclassId();
        BOOL ok = SetWindowSubclass(hwndTocBox, WndProcTocBox, win->tocBoxSubclassId, (DWORD_PTR)win);
        ReportIf(!ok);
    }

    HWND hwndTree = win->tocTreeView ? win->tocTreeView->hwnd : nullptr;
    if (hwndTree && win->tocTreeSubclassId == 0) {
        win->tocTreeSubclassId = NextSubclassId();
        BOOL ok = SetWindowSubclass(hwndTree, WndProcTocTree, win->tocTreeSubclassId, (DWORD_PTR)win);
        ReportIf(!ok);
    }
}

void UnsubclassToc(MainWindow* win) {
    if (win->tocTreeSubclassId != 0 && win->tocTreeView) {
        RemoveWindowSubclass(win->tocTreeView->hwnd, WndProcTocTree, win->tocTreeSubclassId);
        win->tocTreeSubclassId = 0;
    }
    if (win->tocBoxSubclassId != 0) {
        RemoveWindowSubclass(win->hwndTocBox, WndProcTocBox, win->tocBoxSubclassId);
        win->tocBoxSubclassId = 0;
    }
}

static void InitTocTreeViewHandlers(TreeView* treeView) {
    auto fn = MkFunc1Void(TocContextMenu);
    treeView->onContextMenu = fn;
    treeView->onSelectionChanged = MkFunc1Void(TocTreeSelectionChanged);
    treeView->onClick = MkFunc1Void(TocTreeClick);
    treeView->onKeyDown = MkFunc1Void(TocTreeKeyDown2);
    treeView->onGetTooltip = MkFunc1Void(TocCustomizeTooltip);
    treeView->onBeginLabelEdit = MkFunc1Void(TocBeginLabelEdit);
    treeView->onEndLabelEdit = MkFunc1Void(TocEndLabelEdit);
}

void ReCreateTocTreeView(MainWindow* win, HFONT font, int dpi) {
    if (!win || !win->hwndTocBox || !win->tocTreeView) {
        return;
    }

    TreeView* oldTreeView = win->tocTreeView;
    TreeModel* model = oldTreeView->treeModel;
    TreeItem selected = model ? oldTreeView->GetSelection() : TreeModel::kNullItem;
    bool hadFocus = GetFocus() == oldTreeView->hwnd;

    WindowTab* tab = win->CurrentTab();
    if (model && tab && tab->currToc && model == tab->currToc) {
        UpdateTocExpansionState(tab->tocState, oldTreeView, tab->currToc);
        SetInitialExpandState(tab->currToc->root, tab->tocState);
    }

    if (win->tocTreeSubclassId != 0) {
        RemoveWindowSubclass(oldTreeView->hwnd, WndProcTocTree, win->tocTreeSubclassId);
        win->tocTreeSubclassId = 0;
    }

    if (GetCapture() == oldTreeView->hwnd) {
        SendMessageW(oldTreeView->hwnd, WM_CANCELMODE, 0, 0);
    }

    oldTreeView->treeModel = nullptr;
    delete oldTreeView;
    win->tocTreeView = nullptr;

    auto treeView = new TreeView();
    TreeView::CreateArgs args;
    args.parent = win->hwndTocBox;
    args.font = font;
    args.fullRowSelect = true;
    TreeWrapLabelsConfigureCreateArgs(args);
    args.exStyle = 0;
    args.isRtl = IsUIRtl();
    args.editLabels = true;
    InitTocTreeViewHandlers(treeView);

    treeView->Create(args);
    ReportIf(!treeView->hwnd);
    win->tocTreeView = treeView;

    if (model && IsKnownTocTreeModel(win, model)) {
        treeView->SetTreeModel(model);
        treeView->onCustomDraw = MkFunc1Void(OnTocCustomDraw);
        if (selected != TreeModel::kNullItem) {
            treeView->SelectItem(selected);
        }
    }
    if (font) {
        HwndSetTreeFontForDpi(treeView->hwnd, font, dpi);
    }

    SubclassToc(win);
    UpdateControlsColors(win);
    LayoutTocContainer(win);
    TocRecalcAllItemHeights(win);
    InvalidateTocTree(win);
    if (hadFocus) {
        SetFocus(treeView->hwnd);
    }
}

// TODO: restore
#if 0
void TocTreeMouseWheelHandler(MouseWheelEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
    ReportIf(!win);
    if (!win) {
        return;
    }
    // scroll the canvas if the cursor isn't over the ToC tree
    if (!IsCursorOverWindow(ev->hwnd)) {
        ev->didHandle = true;
        ev->result = SendMessageW(win->hwndCanvas, ev->msg, ev->wp, ev->lp);
    }
}
#endif

// TODO: restore
#if 0
void TocTreeCharHandler(CharEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
    ReportIf(!win);
    if (!win) {
        return;
    }
    if (VK_ESCAPE != ev->keyCode) {
        return;
    }
    if (!gGlobalPrefs->escToExit) {
        return;
    }
    if (!CanCloseWindow(win)) {
        return;
    }

    CloseWindow(win, true, false);
    ev->didHandle = true;
}
#endif

// Recursively build a filtered copy of the TocItem tree.
// Includes items whose title matches the filter, plus ancestors needed to reach them.
// Returns nullptr if nothing matches.
static TocItem* FilterTocItemRec(TocItem* item, const char* filter) {
    if (!item) {
        return nullptr;
    }
    TocItem* resultFirst = nullptr;
    TocItem* resultLast = nullptr;
    for (TocItem* si = item; si; si = si->next) {
        // recursively filter children
        TocItem* filteredChildren = FilterTocItemRec(si->child, filter);
        bool titleMatches = si->title && str::ContainsI(si->title, filter);
        if (!titleMatches && !filteredChildren) {
            continue;
        }
        // create a copy of this item
        auto* copy = new TocItem();
        copy->title = str::Dup(si->title);
        copy->pageNo = si->pageNo;
        copy->id = si->id;
        copy->fontFlags = si->fontFlags;
        copy->color = si->color;
        copy->dest = si->dest;
        copy->destNotOwned = true;
        copy->isOpenDefault = true;
        copy->isOpenToggled = false;
        copy->child = filteredChildren;
        // set parent pointers on children
        for (TocItem* c = copy->child; c; c = c->next) {
            c->parent = copy;
        }
        if (!resultFirst) {
            resultFirst = copy;
            resultLast = copy;
        } else {
            resultLast->next = copy;
            resultLast = copy;
        }
    }
    return resultFirst;
}

static void ApplyTocFilter(MainWindow* win, const char* filter) {
    if (!win->tocLoaded) {
        return;
    }
    TocCancelDrag(win);
    ClearTocMultiSelect(win);
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->currToc) {
        return;
    }
    TreeView* treeView = win->tocTreeView;
    // free previous filtered tree; clear treeModel first if it still references it
    if (win->tocFilteredTree) {
        if (treeView && treeView->treeModel == win->tocFilteredTree) {
            treeView->treeModel = nullptr;
        }
        delete win->tocFilteredTree;
        win->tocFilteredTree = nullptr;
    }
    TocTree* origTree = tab->currToc;

    if (!filter || str::Len(filter) == 0) {
        // restore original tree
        SetInitialExpandState(origTree->root, tab->tocState);
        treeView->SetTreeModel(origTree);
        return;
    }

    TocItem* filteredRoot = FilterTocItemRec(origTree->root, filter);
    if (!filteredRoot) {
        treeView->Clear();
        return;
    }
    auto* filteredTree = new TocTree(filteredRoot);
    win->tocFilteredTree = filteredTree;
    treeView->SetTreeModel(filteredTree);
}

void TocFilterChanged(MainWindow* win) {
    Edit* edit = win->tocFilterEdit;
    if (!edit) {
        return;
    }
    TempStr filter = edit->GetTextTemp();
    ApplyTocFilter(win, filter);
}

static void OnTocFilterTextChanged(MainWindow* win) {
    TocFilterChanged(win);
}

static LRESULT CALLBACK WndProcTocFilterEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId,
                                             DWORD_PTR data) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        MainWindow* win = (MainWindow*)data;
        Edit* edit = win->tocFilterEdit;
        if (edit) {
            TempStr txt = edit->GetTextTemp();
            if (txt && str::Len(txt) > 0) {
                edit->SetText("");
                // onTextChanged will fire and restore the tree
                return 0;
            }
            // if already empty, move focus to tree
            SetFocus(win->tocTreeView->hwnd);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void InitTocFilterEdit(MainWindow* win, Edit* filterEdit) {
    filterEdit->onTextChanged = MkFunc0(OnTocFilterTextChanged, win);
    SetWindowSubclass(filterEdit->hwnd, WndProcTocFilterEdit, NextSubclassId(), (DWORD_PTR)win);
}

void UpdateTocFilterForDocumentLoading(MainWindow* win) {
    if (!win || !win->tocFilterEdit || !win->tocFilterEdit->hwnd) {
        return;
    }
    bool show = win->tocVisible && TocSidebarHasBookmarkItems(win);
    HwndSetVisibility(win->tocFilterEdit->hwnd, show);
    RelayoutTocContainer(win);
}

static Edit* CreateTocFilterEdit(MainWindow* win, HFONT font, const char* text) {
    auto filterEdit = new Edit();
    Edit::CreateArgs eargs;
    eargs.parent = win->hwndTocBox;
    eargs.withBorder = false;
    eargs.cueText = _TRA("Search Bookmarks");
    eargs.font = font;
    eargs.text = text;
    filterEdit->Create(eargs);
    InitTocFilterEdit(win, filterEdit);
    return filterEdit;
}

static void CollapseAllToc(MainWindow* win) {
    if (win && win->tocTreeView) {
        win->tocTreeView->CollapseAll();
    }
}

static void ExpandAllToc(MainWindow* win) {
    if (win && win->tocTreeView) {
        win->tocTreeView->ExpandAll();
    }
}

static void TocHeaderCalibrate(MainWindow* win) {
    StartTocCalibFromExisting(win);
}

static void UpdateTocCalibrateHeader(MainWindow* win) {
    if (!win || !win->tocLabelWithClose) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    bool hasItems = tab && tab->currToc && tab->currToc->root && tab->currToc->root->child;
    if (PdfTocEditableEngine(win) && hasItems) {
        win->tocLabelWithClose->SetThirdHeaderAction(MkFunc0(TocHeaderCalibrate, win), _TRN("Calibrate TOC"));
    } else {
        win->tocLabelWithClose->ClearThirdHeaderAction();
    }
    if (win->hwndTocBox) {
        RelayoutTocContainer(win);
    }
}

void ReCreateTocFilterEdit(MainWindow* win, HFONT font) {
    if (!win || !win->hwndTocBox || !win->tocFilterEdit) {
        return;
    }

    Edit* oldEdit = win->tocFilterEdit;
    AutoFreeStr text(str::Dup(oldEdit->GetTextTemp()));
    DWORD selStart = 0;
    DWORD selEnd = 0;
    SendMessageW(oldEdit->hwnd, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    bool hadFocus = GetFocus() == oldEdit->hwnd;

    delete oldEdit;
    win->tocFilterEdit = nullptr;

    Edit* filterEdit = CreateTocFilterEdit(win, font, text);
    win->tocFilterEdit = filterEdit;
    if (font) {
        HwndSetFont(filterEdit->hwnd, font);
    }
    filterEdit->SetSelection((int)selStart, (int)selEnd);

    UpdateTocFilterForDocumentLoading(win);
    UpdateControlsColors(win);
    LayoutTocContainer(win);
    TocRecalcAllItemHeights(win);
    InvalidateTocTree(win);
    if (hadFocus && IsWindowVisible(filterEdit->hwnd)) {
        SetFocus(filterEdit->hwnd);
    }
    RaiseDocumentLoadingNotification(win->hwndFrame, win->hwndCanvas);
}

void FavTreeWrapRecalcHeights(MainWindow* win) {
    if (!win) {
        return;
    }
    TreeWrapRecalcAllItemHeights(win->favTreeView, win->hwndFrame);
}

void ScheduleFavTreeWrapHeights(MainWindow* win) {
    if (win) {
        ScheduleTreeWrapHeightRecalc(win->hwndFavBox);
    }
}

void FlushFavTreeWrapHeights(MainWindow* win) {
    if (!win || !win->hwndFavBox) {
        return;
    }
    KillTreeWrapHeightTimer(win->hwndFavBox);
    FavTreeWrapRecalcHeights(win);
    if (win->favTreeView && win->favTreeView->hwnd) {
        InvalidateRect(win->favTreeView->hwnd, nullptr, FALSE);
    }
}

void FavTreeWrapOnCustomDraw(TreeView::CustomDrawEvent* ev) {
    ev->result = CDRF_DODEFAULT;
    if (!TreeWrapLabelsEnabled()) {
        return;
    }
    NMTVCUSTOMDRAW* tvcd = ev->nm;
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        if (!ev->treeItem || !ev->treeView->treeModel) {
            return;
        }
        char* text = ev->treeView->treeModel->Text(ev->treeItem);
        (void)text;
        ev->result = CDRF_NOTIFYPOSTPAINT;
        return;
    }
    if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
        if (!ev->treeItem || !ev->treeView->treeModel) {
            return;
        }
        bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
        bool isHot = (cd->uItemState & CDIS_HOT) != 0;
        HTREEITEM hItem = (HTREEITEM)cd->dwItemSpec;
        if (isSelected) {
            HWND hwnd = ev->treeView->hwnd;
            tvcd->clrText = ThemeUsesDarkChrome() ? ThemeReadingTextColor() : ThemeWindowTextColor();
            tvcd->clrTextBk = TocSelectedRowFillColor(hwnd);
            DrawTocSelectionFill(cd, hwnd, hItem);
        } else if (ThemeUsesDarkChrome() && isHot) {
            tvcd->clrText = ThemeReadingTextColor();
            tvcd->clrTextBk = TocHotTrackBgColor();
            DrawTocHotTrackFill(cd, ev->treeView->hwnd, hItem);
        }
        char* text = ev->treeView->treeModel->Text(ev->treeItem);
        DrawTreeWrappedLabel(tvcd, ev->treeView, ToWStrTemp(text), nullptr, 0);
        ev->result = CDRF_DODEFAULT;
    }
}

void CreateToc(MainWindow* win) {
    HMODULE hmod = GetModuleHandle(nullptr);
    int dx = gGlobalPrefs->sidebarDx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN;
    HWND parent = win->hwndFrame;
    win->hwndTocBox = CreateWindowExW(0, WC_STATIC, L"", style, 0, 0, dx, 0, parent, nullptr, hmod, nullptr);

    auto l = new LabelWithCloseWnd();
    {
        LabelWithCloseWnd::CreateArgs args;
        args.parent = win->hwndTocBox;
        args.cmdId = IDC_TOC_LABEL_WITH_CLOSE;
        args.isRtl = IsUIRtl();
        // TODO: use the same font size as in GetTreeFont()?
        args.font = GetAppSidebarLabelFontForHwnd(win->hwndFrame);
        l->Create(args);
    }
    win->tocLabelWithClose = l;
    l->SetPaddingXY(2, 2);
    // label is set in UpdateToolbarSidebarText()

    auto filterEdit = CreateTocFilterEdit(win, GetAppTreeFontForHwnd(win->hwndFrame), nullptr);
    win->tocFilterEdit = filterEdit;

    l->SetHeaderActions(MkFunc0(ExpandAllToc, win), _TRN("Expand All"), MkFunc0(CollapseAllToc, win),
                        _TRN("Collapse All"));

    auto treeView = new TreeView();
    TreeView::CreateArgs args;
    args.parent = win->hwndTocBox;
    args.font = GetAppTreeFontForHwnd(win->hwndFrame);
    args.fullRowSelect = true;
    TreeWrapLabelsConfigureCreateArgs(args);
    args.exStyle = 0;
    args.isRtl = IsUIRtl();
    args.editLabels = true;

    InitTocTreeViewHandlers(treeView);

    // treeView->onClick = TocTreeClick; // TODO: maybe not necessary
    // treeView->onChar = TocTreeCharHandler;
    // treeView->onMouseWheel = TocTreeMouseWheelHandler;

    treeView->Create(args);
    ReportIf(!treeView->hwnd);
    win->tocTreeView = treeView;

    SubclassToc(win);

    UpdateControlsColors(win);
}
