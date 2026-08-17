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
#include "AppTools.h"
#include "TableOfContents.h"
#include "Translations.h"
#include "Tabs.h"
#include "Menu.h"
#include "Accelerators.h"
#include "Theme.h"
#include "Notifications.h"

#include "utils/Log.h"

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

    // When PageDestGetValue is empty, path falls back to tocItem->title — same as
    // labelText — so don't append it again after the truncated-label line.
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

    // validate tab before dereferencing — it may have been freed
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
        // Chapter-start page numbers are shared by every #fragment entry in one spine
        // HTML file (common in anthology EPUBs). Only plain chapter links can fast-path.
        if (loaded && navPage > 0 && !hasFragment) {
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

void ClearTocBox(MainWindow* win) {
    if (!win->tocLoaded) {
        return;
    }

    // set tocLoaded to false before SetText("") because SetText triggers
    // EN_CHANGE synchronously which calls ApplyTocFilter() re-entrantly
    // and we need it to bail out early
    win->tocLoaded = false;

    WindowTab* tab = win->CurrentTab();
    if (tab) {
        tab->currToc = nullptr;
    }

    win->tocTreeView->Clear();

    // clear filter state
    delete win->tocFilteredTree;
    win->tocFilteredTree = nullptr;
    if (win->tocFilterEdit) {
        win->tocFilterEdit->SetText("");
    }

    win->currPageNo = 0;
}

void ClearTocBoxForTabSwitch(MainWindow* win) {
    if (!win->tocLoaded) {
        return;
    }

    win->tocLoaded = false;

    if (win->tocTreeView) {
        win->tocTreeView->treeModel = nullptr;
    }

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
    if (win->tocVisible) {
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
    for (TocItem* curr = item; curr; curr = curr->parent) {
        TocItem* first = curr->parent ? curr->parent->child : tab->currToc->root->child;
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

static void ReloadPdfTocAfterEdit(MainWindow* win, const Vec<int>& selectedPath) {
    if (!win) {
        return;
    }
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->currToc || !tab->currToc->root || selectedPath.empty()) {
        return;
    }
    TocItem* selected = PdfTocItemAtPath(tab->currToc->root->child, selectedPath);
    if (selected && win->tocTreeView) {
        win->tocKeepSelection = true;
        win->tocTreeView->SelectItem((TreeItem)selected);
        win->tocKeepSelection = false;
    }
}

static void ShowPdfTocEditError(MainWindow* win, const char* error) {
    TempWStr msg = ToWStrTemp(error ? error : "The PDF table of contents could not be modified.");
    MessageBoxW(win->hwndFrame, msg, L"PDF table of contents", MB_OK | MB_ICONERROR);
}

static bool ConfirmPdfTocDelete(MainWindow* win, bool hasChildren) {
    const char* content = hasChildren ? _TRA("Delete this TOC item and all of its child items?")
                                      : _TRA("Delete this TOC item?");
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
        if (!ConfirmPdfTocDelete(win, selected && selected->child)) {
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
    ReloadPdfTocAfterEdit(win, resultPath);
    ToolbarUpdateStateForWindow(win, false);
}

bool TryAddPdfTocFromSelection(MainWindow* win) {
    EngineBase* engine = PdfTocEditableEngine(win);
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    TextSelection* selection = dm ? dm->textSelection : nullptr;
    if (!engine || !selection || selection->result.len <= 0 || !HasPermission(Perm::CopySelection)) {
        return false;
    }

    // Once this is recognized as the PDF TOC shortcut, don't fall back to adding
    // a favorite if the edit is cancelled or fails.
    if (!ConfirmPdfTocSignatureEdit(win, engine)) {
        return true;
    }

    bool isTextOnlySelection = false;
    TempStr selectedText = GetSelectedTextTemp(tab, " ", isTextOnlySelection);
    AutoFreeStr title(str::Dup(selectedText));
    if (!isTextOnlySelection || !title) {
        return true;
    }
    str::TrimWSInPlace(title.Get(), str::TrimOpt::Both);
    if (str::IsEmpty(title.Get())) {
        return true;
    }

    int pageNo = selection->result.pages[0];
    Rect selectionRect = selection->result.rects[0];
    AutoFreeStr target(
        EngineMupdfFormatPdfTocTarget(engine, pageNo, (float)selectionRect.x, (float)selectionRect.y));
    if (!target) {
        ShowPdfTocEditError(win, "The selected PDF position could not be captured.");
        return true;
    }

    TocItem* selected = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
    selected = OriginalPdfTocItem(win, selected);
    if (!selected || !selected->dest || selected->dest->GetKind() != kindDestinationMupdf) {
        selected = nullptr;
    }
    Vec<int> path;
    if (selected && !PdfTocPathForItem(win, selected, path)) {
        return true;
    }

    Vec<int> resultPath;
    char* errorRaw = nullptr;
    bool ok = EngineMupdfEditPdfToc(engine, PdfTocEditAction::AddAfter, path, title.Get(), target.Get(),
                                    &resultPath, &errorRaw);
    AutoFreeStr error(errorRaw);
    if (!ok) {
        ShowPdfTocEditError(win, error);
        return true;
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
    if (!selected && action != PdfTocEditAction::AddAfter) {
        return true;
    }
    ExecutePdfTocEdit(win, selected, action);
    return true;
}

// clang-format off
static MenuDef menuDefContextToc[] = {
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
    TreeModel* tm = treeView->treeModel;
    TreeItem ti = GetOrSelectTreeItemAtPos(ev, pt);
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
    bool isPdfTocItem = pdfTocItem && pdfTocItem->dest && pdfTocItem->dest->GetKind() == kindDestinationMupdf;
    const int pdfTocCommands[] = {CmdPdfTocAddAfter, CmdPdfTocAddChild, CmdPdfTocEdit,    CmdPdfTocDelete,
                                  CmdPdfTocMoveUp,   CmdPdfTocMoveDown, CmdPdfTocPromote, CmdPdfTocDemote};
    if (!pdfTocEngine) {
        for (int command : pdfTocCommands) {
            MenuRemove(popup, command);
        }
    } else if (!isPdfTocItem) {
        for (int command : pdfTocCommands) {
            if (command != CmdPdfTocAddAfter) {
                MenuRemove(popup, command);
            }
        }
        MenuSetText(popup, CmdPdfTocAddAfter, _TRA("Add Root PDF TOC Item"));
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

    const char* path = nullptr;
    char* fileName = nullptr;
    Kind destKind = dest ? dest->GetKind() : nullptr;

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
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::Delete);
            break;
        case CmdPdfTocMoveUp:
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::MoveUp);
            break;
        case CmdPdfTocMoveDown:
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::MoveDown);
            break;
        case CmdPdfTocPromote:
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::Promote);
            break;
        case CmdPdfTocDemote:
            ExecutePdfTocEdit(win, pdfTocItem, PdfTocEditAction::Demote);
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
    RaiseDocumentLoadingNotification(win->hwndFrame, win->hwndCanvas);
}

void RestoreTocTreeForTab(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
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

    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    bool isHot = (cd->uItemState & CDIS_HOT) != 0;
    COLORREF textCol = tvcd->clrText;
    COLORREF bgCol = tvcd->clrTextBk;
    bool skipBgFill = ThemeUsesDarkChrome() && (isSelected || isHot);

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
    if (TreeWrapUpdatesSuspended()) {
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
    if (rcRow.right < rcClient.right) {
        rcRow.right = rcClient.right;
    }
}

static void DrawTocRowFill(NMCUSTOMDRAW* cd, HWND hwnd, HTREEITEM hItem, COLORREF col) {
    RECT rcRow;
    GetTocItemRowRect(hwnd, hItem, cd, rcRow);
    HBRUSH br = CreateSolidBrush(col);
    FillRect(cd->hdc, &rcRow, br);
    DeleteObject(br);
}

static void DrawTocSelectionFill(NMCUSTOMDRAW* cd, HWND hwnd, HTREEITEM hItem) {
    DrawTocRowFill(cd, hwnd, hItem, TocSelectionBgColor());
}

static void DrawTocHotTrackFill(NMCUSTOMDRAW* cd, HWND hwnd, HTREEITEM hItem) {
    DrawTocRowFill(cd, hwnd, hItem, TocHotTrackBgColor());
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

static void SetTocItemDrawColors(NMTVCUSTOMDRAW* tvcd, TreeView* treeView, TocItem* tocItem, MainWindow* win) {
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    bool isHot = (cd->uItemState & CDIS_HOT) != 0;
    bool hasFocus = (GetFocus() == treeView->hwnd);
    COLORREF bgCol = SidebarBackgroundColor(treeView->bgColor);

    if (isSelected) {
        if (ThemeUsesDarkChrome()) {
            tvcd->clrText = TocItemTextColor(tocItem, win, treeView);
            tvcd->clrTextBk = TocSelectionBgColor();
            return;
        }
        if (hasFocus) {
            tvcd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
            tvcd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
        } else {
            tvcd->clrTextBk = GetSysColor(COLOR_BTNFACE);
        }
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
            bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
            bool isHot = (cd->uItemState & CDIS_HOT) != 0;
            HTREEITEM hItem = (HTREEITEM)cd->dwItemSpec;
            SetTocItemDrawColors(tvcd, ev->treeView, tocItem, win);
            if (ThemeUsesDarkChrome() && (isSelected || isHot)) {
                if (isSelected) {
                    DrawTocSelectionFill(cd, ev->treeView->hwnd, hItem);
                } else {
                    DrawTocHotTrackFill(cd, ev->treeView->hwnd, hItem);
                }
                ev->result = CDRF_NOTIFYPOSTPAINT;
                return;
            }
            ev->result = CDRF_DODEFAULT;
            return;
        }
        if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
            TocItem* tocItem = (TocItem*)ev->treeItem;
            bool knownTree = win && IsKnownTocTreeModel(win, ev->treeView->treeModel);
            bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
            if (ThemeUsesDarkChrome() && isSelected && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
                HTREEITEM hItem = (HTREEITEM)cd->dwItemSpec;
                DrawTocSelectionFrame(cd, ev->treeView->hwnd, hItem);
            }
            if (filterActive && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
                DrawTocItemHighlight(ev, win);
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
        bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
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
        bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
        bool isHot = (cd->uItemState & CDIS_HOT) != 0;
        HTREEITEM hItem = (HTREEITEM)cd->dwItemSpec;
        if (ThemeUsesDarkChrome() && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
            if (isSelected) {
                DrawTocSelectionFill(cd, ev->treeView->hwnd, hItem);
            } else if (isHot) {
                DrawTocHotTrackFill(cd, ev->treeView->hwnd, hItem);
            }
        }
        if (tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
            DrawTocWrappedLabel(tvcd, ev->treeView, tocItem, win);
        }
        if (ThemeUsesDarkChrome() && isSelected && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
            DrawTocSelectionFrame(cd, ev->treeView->hwnd, hItem);
        }
        if (filterActive && tocItem && knownTree && win && win->tocLoaded && !win->isBeingClosed) {
            DrawTocItemHighlight(ev, win);
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
#if 0
    ev->didHandle = true;
    if (!ev->treeItem) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    ReportIf(!win);
    bool allowExternal = false;
    GoToTocTreeItem(win, ev->treeItem, allowExternal);
#endif
    ev->result = -1;
}

static void TocTreeSelectionChanged(TreeView::SelectionChangedEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    ReportIf(!win);

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
    bool allowExternal = ev->byMouse;
    GoToTocTreeItem(win, ev->selectedItem, allowExternal);
}

void TocTreeKeyDown2(TreeView::KeyDownEvent* ev) {
    // TODO: trying to fix https://github.com/sumatrapdfreader/sumatrapdf/issues/1841
    // doesn't work i.e. page up / page down seems to be processed anyway by TreeCtrl
#if 0
    if ((ev->keyCode == VK_PRIOR) || (ev->keyCode == VK_NEXT)) {
        // up/down in tree is not very useful, so instead
        // send it to frame so that it scrolls document instead
        MainWindow* win = FindMainWindowByHwnd(ev->hwnd);
        // this is sent as WM_NOTIFY to TreeCtrl but for frame it's WM_KEYDOWN
        // alternatively, we could call FrameOnKeydown(ev->wp, ev->lp, false);
        SendMessageW(win->hwndFrame, WM_KEYDOWN, ev->wp, ev->lp);
        ev->didHandle = true;
        ev->result = 1;
        return;
    }
#endif
    if (ev->keyCode != VK_TAB) {
        ev->result = 0;
        return;
    }

    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (win->tabsVisible && IsCtrlPressed()) {
        TabsOnCtrlTab(win, IsShiftPressed());
        ev->result = 1;
        return;
    }
    AdvanceFocus(win);
    ev->result = 1;
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
    UINT liveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW;
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
    if (treeView && treeView->hwnd) {
        place(treeView->hwnd, 0, y, rc.dx, dy);
    }
}

void RelayoutTocContainer(MainWindow* win) {
    if (!win) {
        return;
    }
    LayoutTocContainer(win);
}

static LRESULT CALLBACK WndProcTocTree(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR subclassId, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
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
    treeView->onKeyDown = MkFunc1Void(TocTreeKeyDown2);
    treeView->onGetTooltip = MkFunc1Void(TocCustomizeTooltip);
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
    HwndSetVisibility(win->tocFilterEdit->hwnd, win->tocVisible);
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
        if (ThemeUsesDarkChrome()) {
            if (isSelected) {
                tvcd->clrText = ThemeReadingTextColor();
                tvcd->clrTextBk = TocSelectionBgColor();
                DrawTocSelectionFill(cd, ev->treeView->hwnd, hItem);
            } else if (isHot) {
                tvcd->clrText = ThemeReadingTextColor();
                tvcd->clrTextBk = TocHotTrackBgColor();
                DrawTocHotTrackFill(cd, ev->treeView->hwnd, hItem);
            }
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
