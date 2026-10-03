/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "FileHistory.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "DisplayModel.h"
#include "Theme.h"
#include "DarkModeSubclass.h"
#include "WindowTab.h"
#include "resource.h"
#include "Commands.h"
#include "Flags.h"
#include "AppSettings.h"
#include "Favorites.h"
#include "Menu.h"
#include "SumatraDialogs.h"
#include "Tabs.h"
#include "TableOfContents.h"
#include "SidebarThumbs.h"
#include "Translations.h"
#include "Accelerators.h"

void RememberFavTreeExpansionStateForAllWindows();

struct FavTreeItem {
    ~FavTreeItem();

    HTREEITEM hItem = nullptr;
    FavTreeItem* parent = nullptr;
    char* text = nullptr;
    bool isExpanded = false;

    // not owned by us
    Favorite* favorite = nullptr;

    Vec<FavTreeItem*> children;
};

FavTreeItem::~FavTreeItem() {
    str::Free(text);
    DeleteVecMembers(children);
}

struct FavTreeModel : TreeModel {
    ~FavTreeModel() override;

    TreeItem Root() override;

    char* Text(TreeItem) override;
    TreeItem Parent(TreeItem) override;
    int ChildCount(TreeItem) override;
    TreeItem ChildAt(TreeItem, int index) override;
    bool IsExpanded(TreeItem) override;
    bool IsChecked(TreeItem) override;
    void SetHandle(TreeItem, HTREEITEM) override;
    HTREEITEM GetHandle(TreeItem) override;

    FavTreeItem* root = nullptr;
};

FavTreeModel::~FavTreeModel() {
    delete root;
}

TreeItem FavTreeModel::Root() {
    return (TreeItem)root;
}

char* FavTreeModel::Text(TreeItem ti) {
    auto fti = (FavTreeItem*)ti;
    return fti->text;
}

TreeItem FavTreeModel::Parent(TreeItem ti) {
    auto fti = (FavTreeItem*)ti;
    return (TreeItem)fti->parent;
}

int FavTreeModel::ChildCount(TreeItem ti) {
    auto fti = (FavTreeItem*)ti;
    if (!fti) {
        return 0;
    }
    size_t n = fti->children.size();
    return (int)n;
}

TreeItem FavTreeModel::ChildAt(TreeItem ti, int idx) {
    auto fti = (FavTreeItem*)ti;
    auto res = fti->children[idx];
    return (TreeItem)res;
}

bool FavTreeModel::IsExpanded(TreeItem ti) {
    auto fti = (FavTreeItem*)ti;
    return fti->isExpanded;
}

bool FavTreeModel::IsChecked(TreeItem) {
    return false;
}

void FavTreeModel::SetHandle(TreeItem ti, HTREEITEM hItem) {
    ReportIf(ti < 0);
    FavTreeItem* treeItem = (FavTreeItem*)ti;
    treeItem->hItem = hItem;
}

HTREEITEM FavTreeModel::GetHandle(TreeItem ti) {
    ReportIf(ti < 0);
    FavTreeItem* treeItem = (FavTreeItem*)ti;
    return treeItem->hItem;
}

static Favorite* GetFavByMenuId(int menuId, FileState** dsOut) {
    FileState* ds;
    for (size_t i = 0; (ds = gFileHistory.Get(i)) != nullptr; i++) {
        for (size_t j = 0; j < ds->favorites->size(); j++) {
            if (menuId == ds->favorites->at(j)->menuId) {
                if (dsOut) {
                    *dsOut = ds;
                }
                return ds->favorites->at(j);
            }
        }
    }
    return nullptr;
}

static FileState* GetByFavorite(Favorite* fn) {
    FileState* ds;
    for (size_t i = 0; (ds = gFileHistory.Get(i)) != nullptr; i++) {
        if (ds->favorites->Contains(fn)) {
            return ds;
        }
    }
    return nullptr;
}

static void ResetFavMenuIds() {
    FileState* ds;
    for (size_t i = 0; (ds = gFileHistory.Get(i)) != nullptr; i++) {
        for (size_t j = 0; j < ds->favorites->size(); j++) {
            ds->favorites->at(j)->menuId = 0;
        }
    }
}

static size_t idxCache = (size_t)-1;

static FileState* GetFavByFilePath(const char* filePath) {
    // it's likely that we'll ask about the info for the same
    // file as in previous call, so use one element cache
    FileState* fs = gFileHistory.Get(idxCache);
    if (!fs || !str::Eq(fs->filePath, filePath)) {
        fs = gFileHistory.FindByName(filePath, &idxCache);
    }
    return fs;
}

bool IsPageInFavorites(const char* filePath, int pageNo) {
    FileState* fav = GetFavByFilePath(filePath);
    if (!fav) {
        return false;
    }
    for (size_t i = 0; i < fav->favorites->size(); i++) {
        if (pageNo == fav->favorites->at(i)->pageNo) {
            return true;
        }
    }
    return false;
}

static Favorite* FindByPage(FileState* ds, int pageNo, const char* pageLabel = nullptr) {
    if (!ds || !ds->favorites) {
        return nullptr;
    }
    auto favs = ds->favorites;
    int n = favs->Size();
    if (pageLabel) {
        for (int i = 0; i < n; i++) {
            auto fav = favs->at(i);
            if (str::Eq(fav->pageLabel, pageLabel)) {
                return fav;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        auto fav = favs->at(i);
        if (pageNo == fav->pageNo) {
            return fav;
        }
    }
    return nullptr;
}

static int SortByPageNo(const void* a, const void* b) {
    Favorite* na = *(Favorite**)a;
    Favorite* nb = *(Favorite**)b;
    // sort lower page numbers first
    return na->pageNo - nb->pageNo;
}

static int gNextFavSeq = 1;
static bool gFavSeqReady = false;

// Old favorites have addedSeq 0. Number them once so later adds stay ordered.
static void EnsureFavoriteSeqs() {
    if (gFavSeqReady) {
        return;
    }
    if (!gFileHistory.Get(0)) {
        return;
    }
    int maxSeq = 0;
    bool anyZero = false;
    FileState* ds = nullptr;
    for (size_t i = 0; (ds = gFileHistory.Get(i)) != nullptr; i++) {
        if (!ds->favorites) {
            continue;
        }
        for (size_t j = 0; j < ds->favorites->size(); j++) {
            int seq = ds->favorites->at(j)->addedSeq;
            if (seq > maxSeq) {
                maxSeq = seq;
            }
            if (seq <= 0) {
                anyZero = true;
            }
        }
    }
    int next = maxSeq + 1;
    if (next < 1) {
        next = 1;
    }
    if (anyZero) {
        size_t n = 0;
        while (gFileHistory.Get(n)) {
            n++;
        }
        for (size_t i = n; i-- > 0;) {
            ds = gFileHistory.Get(i);
            if (!ds || !ds->favorites) {
                continue;
            }
            for (size_t j = 0; j < ds->favorites->size(); j++) {
                Favorite* fn = ds->favorites->at(j);
                if (fn->addedSeq <= 0) {
                    fn->addedSeq = next++;
                }
            }
        }
    }
    gNextFavSeq = next;
    gFavSeqReady = true;
}

static void AddOrReplaceFav(const char* filePath, int pageNo, const char* name, const char* pageLabel) {
    FileState* fav = GetFavByFilePath(filePath);
    if (!fav) {
        // we were asked to add a favorite for current file but couldn't find
        // history for this file
        fav = NewFileState(filePath);
        gFileHistory.Append(fav);
    }

    EnsureFavoriteSeqs();
    Favorite* fn = FindByPage(fav, pageNo, pageLabel);
    if (fn) {
        str::ReplaceWithCopy(&fn->name, name);
        ReportIf(fn->pageLabel && !str::Eq(fn->pageLabel, pageLabel));
        if (fn->addedSeq <= 0) {
            fn->addedSeq = gNextFavSeq++;
        }
    } else {
        fn = NewFavorite(pageNo, name, pageLabel);
        fn->addedSeq = gNextFavSeq++;
        fav->favorites->Append(fn);
    }
}

static void RemoveFav(const char* filePath, int pageNo) {
    FileState* fav = GetFavByFilePath(filePath);
    if (!fav) {
        return;
    }
    Favorite* fn = FindByPage(fav, pageNo);
    if (!fn) {
        return;
    }

    fav->favorites->Remove(fn);
    DeleteFavorite(fn);

    if (!SettingsRememberOpenedFiles() && 0 == fav->favorites->size()) {
        gFileHistory.Remove(fav);
        DeleteFileState(fav);
    }
}

static void RemoveAllFavForFile(const char* filePath) {
    FileState* fav = GetFavByFilePath(filePath);
    if (!fav) {
        return;
    }

    for (size_t i = 0; i < fav->favorites->size(); i++) {
        DeleteFavorite(fav->favorites->at(i));
    }
    fav->favorites->Reset();

    if (!SettingsRememberOpenedFiles()) {
        gFileHistory.Remove(fav);
        DeleteFileState(fav);
    }
}

// Note: those might be too big
#define MAX_FAV_SUBMENUS 10
#define MAX_FAV_MENUS 10

bool HasFavorites() {
    FileState* ds;
    for (size_t i = 0; (ds = gFileHistory.Get(i)) != nullptr; i++) {
        if (ds->favorites->size() > 0) {
            return true;
        }
    }
    return false;
}

// caller has to free() the result
static TempStr FavReadableNameTemp(Favorite* fn) {
    const char* label = fn->pageLabel;
    if (!label) {
        label = str::FormatTemp("%d", fn->pageNo);
    }
    char* res = nullptr;
    if (fn->name) {
        TempStr pageNo = str::FormatTemp(_TRA("(page %s)"), label);
        res = str::JoinTemp(fn->name, " ", pageNo);
    } else {
        res = str::FormatTemp(_TRA("Page %s"), label);
    }
    return res;
}

// caller has to free() the result
static TempStr FavCompactReadableNameTemp(FileState* fav, Favorite* fn, bool isCurrent = false) {
    TempStr rn = FavReadableNameTemp(fn);
    if (isCurrent) {
        return str::FormatTemp("%s : %s", _TRA("Current file"), rn);
    }
    TempStr fp = path::GetBaseNameTemp(fav->filePath);
    return str::FormatTemp("%s : %s", fp, rn);
}

static void AppendFavMenuItems(HMENU m, FileState* f, int& idx, bool combined, bool isCurrent) {
    ReportIf(!f);
    if (!f) {
        return;
    }
    Vec<Favorite*> items;
    for (size_t i = 0; i < f->favorites->size(); i++) {
        items.Append(f->favorites->at(i));
    }
    items.Sort(SortByPageNo);
    for (size_t i = 0; i < items.size(); i++) {
        if (i >= MAX_FAV_MENUS) {
            return;
        }
        Favorite* fn = items.at(i);
        fn->menuId = idx++;
        TempStr s;
        if (combined) {
            s = FavCompactReadableNameTemp(f, fn, isCurrent);
        } else {
            s = FavReadableNameTemp(fn);
        }
        auto safeStr = MenuToSafeStringTemp(s);
        TempWStr ws = ToWStrTemp(safeStr);
        AppendMenuW(m, MF_STRING, (UINT_PTR)fn->menuId, ws);
    }
}

static bool SortByBaseFileName(const char* s1, const char* s2) {
    if (str::IsEmpty(s1)) {
        if (str::IsEmpty(s2)) {
            return false;
        }
        return true;
    }
    if (str::IsEmpty(s2)) {
        return false;
    }
    TempStr base1 = path::GetBaseNameTemp(s1);
    TempStr base2 = path::GetBaseNameTemp(s2);
    int n = str::CmpNatural(base1, base2);
    return n < 0;
}

static void GetSortedFilePaths(StrVec& filePathsSortedOut, FileState* toIgnore = nullptr) {
    FileState* fs;
    for (size_t i = 0; (fs = gFileHistory.Get(i)) != nullptr; i++) {
        if (fs->favorites->size() > 0 && fs != toIgnore) {
            filePathsSortedOut.Append(fs->filePath);
        }
    }
    Sort(&filePathsSortedOut, SortByBaseFileName);
}

// For easy access, we try to show favorites in the menu, similar to a list of
// recently opened files.
// The first menu items are for currently opened file (up to MAX_FAV_MENUS), based
// on the assumption that user is usually interested in navigating current file.
// Then we have a submenu for each file for which there are bookmarks (up to
// MAX_FAV_SUBMENUS), each having up to MAX_FAV_MENUS menu items.
// If not all favorites can be shown, we also enable "Show all favorites" menu which
// will provide a way to see all favorites.
// Note: not sure if that's the best layout. Maybe we should always use submenu and
// put the submenu for current file as the first one (potentially named as "Current file"
// or some such, to make it stand out from other submenus)
static void AppendFavMenus(HMENU m, const char* currFilePath) {
    // To minimize mouse movement when navigating current file via favorites
    // menu, put favorites for current file first
    FileState* currFileFav = nullptr;
    if (currFilePath) {
        currFileFav = GetFavByFilePath(currFilePath);
    }

    // sort the files with favorites by base file name of file path
    StrVec filePathsSorted;
    if (CanAccessDisk()) {
        // only show favorites for other files, if we're allowed to open them
        GetSortedFilePaths(filePathsSorted, currFileFav);
    }
    if (currFileFav && currFileFav->favorites->size() > 0) {
        filePathsSorted.InsertAt(0, currFileFav->filePath);
    }

    if (filePathsSorted.Size() == 0) {
        return;
    }

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    ResetFavMenuIds();
    int menuId = CmdFavoriteFirst;

    int menusCount = filePathsSorted.Size();
    if (menusCount > MAX_FAV_MENUS) {
        menusCount = MAX_FAV_MENUS;
    }

    for (int i = 0; i < menusCount; i++) {
        const char* filePath = filePathsSorted.At(i);
        FileState* f = GetFavByFilePath(filePath);
        ReportIf(!f);
        if (!f) {
            continue;
        }
        HMENU sub = m;
        bool combined = (f->favorites->Size() == 1);
        if (!combined) {
            sub = CreateMenu();
        }
        AppendFavMenuItems(sub, f, menuId, combined, f == currFileFav);
        if (!combined) {
            const char* s = _TRA("Current file");
            if (f != currFileFav) {
                s = MenuToSafeStringTemp(path::GetBaseNameTemp(filePath));
            }
            AppendMenuW(m, MF_POPUP | MF_STRING, (UINT_PTR)sub, ToWStrTemp(s));
        }
    }
}

// Called when a user opens "Favorites" top-level menu. We need to construct
// the menu:
// - disable add/remove menu items if no document is opened
// - if a document is opened and the page is already bookmarked,
//   disable "add" menu item and enable "remove" menu item
// - if a document is opened and the page is not bookmarked,
//   enable "add" menu item and disable "remove" menu item
void RebuildFavMenu(MainWindow* win, HMENU menu) {
    if (!win->IsDocLoaded()) {
        MenuSetEnabled(menu, CmdFavoriteAdd, false);
        MenuSetEnabled(menu, CmdFavoriteDel, false);
        AppendFavMenus(menu, (const char*)nullptr);
    } else {
        TempStr label = win->ctrl->GetPageLabeTemp(win->currPageNo);
        bool isBookmarked = IsPageInFavorites(win->ctrl->GetFilePath(), win->currPageNo);
        if (isBookmarked) {
            MenuSetEnabled(menu, CmdFavoriteAdd, false);
            TempStr s = str::FormatTemp(_TRA("Remove page %s from favorites"), label);
            MenuSetText(menu, CmdFavoriteDel, s);
        } else {
            MenuSetEnabled(menu, CmdFavoriteDel, false);
            TempStr s = str::FormatTemp(_TRA("Add page %s to favorites"), label);
            s = AppendAccelKeyToMenuStringTemp(s, CmdFavoriteAdd);
            MenuSetText(menu, CmdFavoriteAdd, s);
        }
        AppendFavMenus(menu, win->ctrl->GetFilePath());
    }
    MenuSetEnabled(menu, CmdFavoriteToggle, HasFavorites());
}

void ToggleFavorites(MainWindow* win) {
    ShowOrToggleSidebarView(win, SidebarView::Favorites);
}

static void GoToFavoritePage(MainWindow* win, int pageNo) {
    if (!IsMainWindowValid(win)) {
        return;
    }
    if (win->IsDocLoaded() && win->ctrl->ValidPageNo(pageNo)) {
        win->ctrl->GoToPage(pageNo, true);
    }
    // we might have been invoked by clicking on a tree view
    // switch focus so that keyboard navigation works, which enables
    // a fluid experience
    win->Focus();
}

struct GoToFavoritePageData {
    MainWindow* win;
    int pageNo;
};

static void GoToFavoritePage(GoToFavoritePageData* d) {
    GoToFavoritePage(d->win, d->pageNo);
    delete d;
}

// A click in the favorites list navigates. The sidebar view stays favorites;
// loading the target used to restore that file's bookmark sidebar.
static void StayOnFavoritesSidebar(MainWindow* win) {
    if (!win || !win->tocVisible) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || (SidebarView)tab->sidebarView == SidebarView::Favorites) {
        return;
    }
    tab->sidebarView = (int)SidebarView::Favorites;
    ApplySidebarViewLayout(win);
}

// Going to a bookmark within current file scrolls to a given page.
// Going to a bookmark in another file, loads the file and scrolls to a page
// (similar to how invoking one of the recently opened files works)
static void GoToFavorite(MainWindow* win, FileState* fs, Favorite* fav) {
    ReportIf(!fs || !fav);
    if (!fs || !fav) {
        return;
    }
    bool keepFavs = win && win->tocVisible && CurrentSidebarView(win) == SidebarView::Favorites;

    char* fp = fs->filePath;
    MainWindow* existingWin = FindMainWindowByFile(fp, true);
    if (!existingWin && win && win->IsDocLoaded() && win->ctrl && path::IsSame(win->ctrl->GetFilePath(), fp)) {
        existingWin = win;
    }
    if (existingWin && existingWin->IsDocLoaded() && existingWin->ctrl && existingWin->ctrl->ValidPageNo(fav->pageNo)) {
        // Do this now. Posting it let the click return before the page moved,
        // and a later focus/restore could leave the document where it was.
        existingWin->ctrl->GoToPage(fav->pageNo, true);
        if (keepFavs) {
            StayOnFavoritesSidebar(existingWin);
        }
        return;
    }
    if (existingWin) {
        if (keepFavs) {
            StayOnFavoritesSidebar(existingWin);
        }
        auto data = new GoToFavoritePageData;
        data->pageNo = fav->pageNo;
        data->win = existingWin;
        auto fn = MkFunc0<GoToFavoritePageData>(GoToFavoritePage, data);
        uitask::Post(fn, "TaskGoToFavorite");
        return;
    }

    if (!CanAccessDisk()) {
        return;
    }

    // When loading a new document, go directly to selected page instead of
    // first showing last seen page stored in file history
    // A hacky solution because I don't want to add even more parameters to
    // LoadDocument() and LoadDocumentInto()
    int pageNo = fav->pageNo;
    FileState* ds = gFileHistory.FindByPath(fs->filePath);
    if (ds && !ds->useDefaultState && gGlobalPrefs->rememberStatePerDocument) {
        ds->pageNo = fav->pageNo;
        ds->scrollPos = PointF(-1, -1); // don't scroll the page
        pageNo = -1;
    }

    LoadArgs args(fs->filePath, win);
    win = LoadDocument(&args);
    if (win && keepFavs) {
        StayOnFavoritesSidebar(win);
    }
    if (win) {
        auto data = new GoToFavoritePageData;
        data->pageNo = pageNo;
        data->win = win;
        auto fn = MkFunc0<GoToFavoritePageData>(GoToFavoritePage, data);
        uitask::Post(fn, "TaskGoToFavorite2");
    }
}

void GoToFavoriteByMenuId(MainWindow* win, int cmdId) {
    FileState* f;
    Favorite* fn = GetFavByMenuId(cmdId, &f);
    if (fn) {
        GoToFavorite(win, f, fn);
    }
}

static void GoToFavForTreeItem(MainWindow* win, TreeItem ti) {
    if (!win || !ti) {
        return;
    }

    FavTreeItem* fti = (FavTreeItem*)ti;
    Favorite* fn = fti->favorite;
    if (!fn) {
        // can happen for top-level node which is not associated with a favorite
        // but only serves a parent node for favorites for a given file
        return;
    }
    FileState* f = GetByFavorite(fn);
    GoToFavorite(win, f, fn);
}

static TreeItem FavFindItemByHandle(FavTreeItem* node, HTREEITEM h) {
    if (!node || !h) {
        return 0;
    }
    if (node->hItem == h) {
        return (TreeItem)node;
    }
    int n = node->children.Size();
    for (int i = 0; i < n; i++) {
        TreeItem found = FavFindItemByHandle(node->children[i], h);
        if (found) {
            return found;
        }
    }
    return 0;
}

// The expander toggles the node. The rest of the row, including the empty
// stretch to the right of a short title, jumps. TVHT_ONITEMRIGHT is that
// stretch; TVHT_TORIGHT is outside the control.
static bool FavTreeHitOnRow(UINT flags) {
    if (flags & TVHT_ONITEMBUTTON) {
        return false;
    }
    UINT offControl = TVHT_ABOVE | TVHT_BELOW | TVHT_NOWHERE | TVHT_TOLEFT | TVHT_TORIGHT;
    if (flags & offControl) {
        return false;
    }
    UINT onRow = TVHT_ONITEM | TVHT_ONITEMINDENT | TVHT_ONITEMRIGHT;
    return (flags & onRow) != 0;
}

static TreeItem FavTreeItemAtPoint(TreeView* treeView, int x, int y) {
    if (!treeView || !treeView->hwnd) {
        return 0;
    }
    TVHITTESTINFO ht{};
    ht.pt = {x, y};
    TreeView_HitTest(treeView->hwnd, &ht);
    if (!ht.hItem || !FavTreeHitOnRow(ht.flags)) {
        return 0;
    }
    TreeItem ti = treeView->GetTreeItemByHandle(ht.hItem);
    if (ti || !treeView->treeModel) {
        return ti;
    }
    auto* model = (FavTreeModel*)treeView->treeModel;
    return FavFindItemByHandle(model->root, ht.hItem);
}

#if 0
static void GoToFavForTVItem(MainWindow* win, TreeCtrl* treeView, HTREEITEM hItem = nullptr) {
    TreeItem ti = nullptr;
    if (nullptr == hItem) {
        ti = treeView->GetSelection();
    } else {
        ti = treeView->GetTreeItemByHandle(hItem);
    }
    GoToFavForTreeItem(win, ti);
}
#endif

static FavTreeItem* MakeFavTopLevelItem(FileState* fs, bool isExpanded) {
    if (!fs->favorites || fs->favorites->Size() == 0) {
        return nullptr;
    }
    auto* res = new FavTreeItem();
    Favorite* fn = fs->favorites->at(0);
    res->favorite = fn;

    bool isCollapsed = fs->favorites->size() == 1;
    if (isCollapsed) {
        isExpanded = false;
    }
    res->isExpanded = isExpanded;

    TempStr text = nullptr;
    if (isCollapsed) {
        text = FavCompactReadableNameTemp(fs, fn);
    } else {
        char* fp = fs->filePath;
        text = path::GetBaseNameTemp(fp);
    }
    res->text = str::Dup(text);
    return res;
}

static void CollectFavsSorted(FileState* f, Vec<Favorite*>& out) {
    for (size_t i = 0; i < f->favorites->size(); i++) {
        out.Append(f->favorites->at(i));
    }
    out.Sort(SortByPageNo);
}

static void MakeFavSecondLevel(FavTreeItem* parent, FileState* f) {
    Vec<Favorite*> favs;
    CollectFavsSorted(f, favs);
    for (Favorite* fn : favs) {
        auto* ti = new FavTreeItem();
        ti->text = str::Dup(FavReadableNameTemp(fn));
        ti->parent = parent;
        ti->favorite = fn;
        parent->children.Append(ti);
    }
}

static bool FavTextMatches(const char* text, const char* filter) {
    if (str::IsEmpty(filter)) {
        return true;
    }
    return text && str::ContainsI(text, filter);
}

static void CollectFavFiles(Vec<FileState*>& out) {
    EnsureFavoriteSeqs();
    StrVec paths;
    GetSortedFilePaths(paths);
    for (char* path : paths) {
        FileState* fs = GetFavByFilePath(path);
        if (fs) {
            out.Append(fs);
        }
    }
}

static FavTreeModel* BuildFavTreeModel(MainWindow* win, const char* filter) {
    bool filtering = !str::IsEmpty(filter);
    auto* res = new FavTreeModel();
    res->root = new FavTreeItem();
    Vec<FileState*> favFiles;
    CollectFavFiles(favFiles);
    for (FileState* fs : favFiles) {
        if (!fs) {
            continue;
        }
        TempStr baseName = path::GetBaseNameTemp(fs->filePath);
        bool fileMatch = FavTextMatches(baseName, filter);
        bool isExpanded = win->expandedFavorites.Contains(fs);
        if (fs->favorites->size() <= 1) {
            Favorite* fn = fs->favorites->size() == 1 ? fs->favorites->at(0) : nullptr;
            TempStr compact = fn ? FavCompactReadableNameTemp(fs, fn) : baseName;
            if (filtering && !fileMatch && !FavTextMatches(compact, filter) &&
                !FavTextMatches(fn ? FavReadableNameTemp(fn) : nullptr, filter)) {
                continue;
            }
            FavTreeItem* ti = MakeFavTopLevelItem(fs, isExpanded);
            if (ti) {
                res->root->children.Append(ti);
            }
            continue;
        }
        FavTreeItem* ti = MakeFavTopLevelItem(fs, isExpanded || filtering);
        if (!ti) {
            continue;
        }
        if (!filtering || fileMatch) {
            MakeFavSecondLevel(ti, fs);
            res->root->children.Append(ti);
            continue;
        }
        Vec<Favorite*> matched;
        CollectFavsSorted(fs, matched);
        for (Favorite* fn : matched) {
            TempStr name = FavReadableNameTemp(fn);
            if (!FavTextMatches(name, filter)) {
                continue;
            }
            auto* child = new FavTreeItem();
            child->text = str::Dup(name);
            child->parent = ti;
            child->favorite = fn;
            ti->children.Append(child);
        }
        if (ti->children.Size() == 0) {
            delete ti;
            continue;
        }
        ti->isExpanded = true;
        res->root->children.Append(ti);
    }
    return res;
}

void PopulateFavTreeIfNeeded(MainWindow* win) {
    TreeView* treeView = win->favTreeView;
    if (treeView->treeModel) {
        return;
    }
    TreeModel* tm = BuildFavTreeModel(win, win->sidebarFindText[2].Get());
    treeView->SetTreeModel(tm);
}

void ApplyFavoritesFind(MainWindow* win, const char* filter) {
    if (!win || !win->favTreeView) {
        return;
    }
    TreeView* treeView = win->favTreeView;
    auto* prevModel = treeView->treeModel;
    TreeModel* newModel = BuildFavTreeModel(win, filter);
    treeView->SetTreeModel(newModel);
    delete prevModel;
    if (win->tocVisible && CurrentSidebarView(win) == SidebarView::Favorites) {
        treeView->ExpandAll();
        InvalidateRect(treeView->hwnd, nullptr, FALSE);
    }
}

void UpdateFavoritesTree(MainWindow* win) {
    TreeView* treeView = win->favTreeView;
    auto* prevModel = treeView->treeModel;
    TreeModel* newModel = BuildFavTreeModel(win, win->sidebarFindText[2].Get());
    treeView->SetTreeModel(newModel);
    delete prevModel;

    if (win->tocVisible && CurrentSidebarView(win) == SidebarView::Favorites) {
        ApplySidebarViewLayout(win);
    }
}

void UpdateFavoritesTreeForAllWindows() {
    for (MainWindow* win : gWindows) {
        UpdateFavoritesTree(win);
    }
}

void AddFavoriteWithLabelAndName(MainWindow* win, int pageNo, const char* pageLabel, const char* nameIn) {
    AutoFreeStr name = str::Dup(nameIn);
    bool shouldAdd = Dialog_AddFavorite(win->hwndFrame, pageLabel, name);
    if (!shouldAdd) {
        return;
    }

    TempStr plainLabel = str::FormatTemp("%d", pageNo);
    bool needsLabel = !str::Eq(plainLabel, pageLabel);

    RememberFavTreeExpansionStateForAllWindows();
    const char* pl = nullptr;
    if (needsLabel) {
        pl = pageLabel;
    }
    WindowTab* tab = win->CurrentTab();
    const char* path = tab->filePath;
    AddOrReplaceFav(path, pageNo, name, pl);
    // expand newly added favorites by default
    FileState* fav = GetFavByFilePath(path);
    if (fav && fav->favorites->size() == 2) {
        win->expandedFavorites.Append(fav);
    }
    UpdateFavoritesTreeForAllWindows();
    SaveSettings();
}

static void AddFavoriteForPage(MainWindow* win, int pageNo) {
    char* name = nullptr;
    auto tab = win->CurrentTab();
    auto* ctrl = tab->ctrl;
    if (ctrl->HasToc()) {
        // use the current ToC heading as default name
        auto* docTree = ctrl->GetToc();
        TocItem* root = docTree->root;
        EngineBase* engine = nullptr;
        DisplayModel* dm = ctrl->AsFixed();
        if (dm) {
            engine = dm->GetEngine();
        }
        TocItem* item = root && root->child ? TocItemBestMatchForPage(root->child, pageNo, engine) : nullptr;
        if (item) {
            name = item->title;
        }
    }
    TempStr pageLabel = ctrl->GetPageLabeTemp(pageNo);
    AddFavoriteWithLabelAndName(win, pageNo, pageLabel, name);
}

void AddFavoriteForCurrentPage(MainWindow* win) {
    if (!win->IsDocLoaded()) {
        return;
    }
    int pageNo = win->currPageNo;
    AddFavoriteForPage(win, pageNo);
}

void DelFavorite(const char* filePath, int pageNo) {
    if (!filePath) {
        return;
    }
    RememberFavTreeExpansionStateForAllWindows();
    RemoveFav(filePath, pageNo);
    UpdateFavoritesTreeForAllWindows();
    SaveSettings();
}

void RememberFavTreeExpansionState(MainWindow* win) {
    win->expandedFavorites.Reset();
    TreeView* treeView = win->favTreeView;
    TreeModel* tm = treeView ? treeView->treeModel : nullptr;
    if (!tm) {
        // TODO: remember all favorites as expanded
        return;
    }
    TreeItem root = tm->Root();
    int n = tm->ChildCount(root);
    for (int i = 0; i < n; i++) {
        TreeItem ti = tm->ChildAt(root, i);
        bool isExpanded = treeView->IsExpanded(ti);
        if (isExpanded) {
            FavTreeItem* fti = (FavTreeItem*)ti;
            Favorite* fn = fti->favorite;
            FileState* f = GetByFavorite(fn);
            win->expandedFavorites.Append(f);
        }
    }
}

void RememberFavTreeExpansionStateForAllWindows() {
    for (size_t i = 0; i < gWindows.size(); i++) {
        RememberFavTreeExpansionState(gWindows.at(i));
    }
}

static void FavTreeItemClicked(TreeView::ClickEvent* ev) {
    if (!ev || !ev->treeView) {
        return;
    }
    // Already-selected rows do not send TVN_SELCHANGED. A click anywhere
    // on the row still has to jump, same as a bookmark entry.
    TreeItem ti = ev->treeItem ? ev->treeItem : FavTreeItemAtPoint(ev->treeView, ev->mouseWindow.x, ev->mouseWindow.y);
    if (!ti) {
        return;
    }
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    if (!win) {
        return;
    }
    GoToFavForTreeItem(win, ti);
}

// Focus selects the first row by itself (action 0x1000). Drop that highlight
// after the notification returns; selecting during TVN_SELCHANGED re-enters.
// A click in the same turn clears the flag so it is not wiped afterwards.
static constexpr UINT kFavClearAutoSelMsg = WM_APP + 0x4f3;
static HWND gFavDropFocusSelHwnd = nullptr;

static void FavTreeSelectionChanged(TreeView::SelectionChangedEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->treeView->hwnd);
    ReportIf(!win);

    // When the focus is set to the toc window the first item in the treeview is automatically
    // selected and a TVN_SELCHANGEDW notification message is sent with the special code pnmtv->action ==
    // 0x00001000. We have to ignore this message to prevent the current page to be changed.
    // The case pnmtv->action==TVC_UNKNOWN is ignored because
    // it corresponds to a notification sent by
    // the function TreeView_DeleteAllItems after deletion of the item.
    UINT action = ev->nmtv ? ev->nmtv->action : 0;
    if (action == 0x1000) {
        gFavDropFocusSelHwnd = ev->treeView->hwnd;
        PostMessageW(ev->treeView->hwnd, kFavClearAutoSelMsg, 0, 0);
        return;
    }
    bool shouldHandle = ev->byKeyboard || ev->byMouse;
    if (shouldHandle && gFavDropFocusSelHwnd == ev->treeView->hwnd) {
        gFavDropFocusSelHwnd = nullptr;
    }
    if (!shouldHandle) {
        return;
    }
    GoToFavForTreeItem(win, ev->selectedItem);
    UpdateSidebarViewButtons(win);
}

static void DeleteFavTreeItem(FavTreeItem* fti) {
    if (!fti || !fti->favorite) {
        return;
    }
    RememberFavTreeExpansionStateForAllWindows();
    Favorite* toDelete = fti->favorite;
    FileState* f = GetByFavorite(toDelete);
    if (!f) {
        return;
    }
    if (fti->parent) {
        RemoveFav(f->filePath, toDelete->pageNo);
    } else {
        // A top-level node stands for every favorite of that file.
        RemoveAllFavForFile(f->filePath);
    }
    UpdateFavoritesTreeForAllWindows();
    SaveSettings();
}

static FavTreeItem* SelectedFavItem(MainWindow* win) {
    if (!win || !win->favTreeView) {
        return nullptr;
    }
    return (FavTreeItem*)win->favTreeView->GetSelection();
}

bool CanDeleteSelectedFavorite(MainWindow* win) {
    FavTreeItem* fti = SelectedFavItem(win);
    return fti && fti->favorite;
}

void DeleteSelectedFavorite(MainWindow* win) {
    DeleteFavTreeItem(SelectedFavItem(win));
}

static void FavTreeContextMenu(ContextMenuEvent* ev);

void ShowFavoritesContextMenu(MainWindow* win, int screenX, int screenY) {
    if (!win || !win->favTreeView) {
        return;
    }
    POINT ptWindow = {screenX, screenY};
    if (screenX != -1 || screenY != -1) {
        MapWindowPoints(HWND_DESKTOP, win->favTreeView->hwnd, &ptWindow, 1);
    }
    ContextMenuEvent ev;
    ev.w = win->favTreeView;
    ev.mouseScreen = Point(screenX, screenY);
    ev.mouseWindow = Point(ptWindow.x, ptWindow.y);
    FavTreeContextMenu(&ev);
}

static void FavTreeContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    // TreeView* treeView = (TreeView*)ev->w;
    // HWND hwnd = treeView->hwnd;
    // MainWindow* win = FindMainWindowByHwnd(hwnd);

    POINT pt{};
    TreeItem ti = GetOrSelectTreeItemAtPos(ev, pt);
    if (!ti) {
        return;
    }
    // BuildMenuFromDef drops this command when the menu context is empty.
    // The item is the clicked favorite, so add it directly.
    HMENU popup = CreatePopupMenu();
    const char* title = trans::GetTranslation(_TRN("Remove from favorites"));
    AppendMenuW(popup, MF_STRING, CmdFavoriteDel, ToWStrTemp(title));
    MarkMenuOwnerDraw(popup);
    SetForegroundWindow(win->hwndFrame);
    uint flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    int cmd = TrackPopupMenu(popup, flags, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);

    // TODO: it would be nice to have a system for undo-ing things, like in Gmail,
    // so that we can do destructive operations without asking for permission via
    // invasive model dialog boxes but also allow reverting them if were done
    // by mistake
    if (CmdFavoriteDel == cmd) {
        DeleteFavTreeItem((FavTreeItem*)ti);
    }
}

static constexpr UINT kFavTipTimer = 0xFA71;
static int gFavHoverX = 0;
static int gFavHoverY = 0;

static void SilenceFavBuiltinTip(HWND hwnd);
static void FavTreeOnMouseMove(MainWindow* win, HWND hwnd, int x, int y);
static void FavTreeOnMouseHover(MainWindow* win, HWND hwnd, int x, int y);
static void FavTreeOnMouseLeave();

static LRESULT CALLBACK WndProcFavTree(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if ((msg == WM_LBUTTONDOWN || msg == WM_NCLBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE ||
         msg == WM_SETCURSOR) &&
        HandleSidebarSplitterHit(win, hwnd, msg, lp)) {
        return msg == WM_SETCURSOR ? TRUE : 0;
    }
    if (msg == kFavClearAutoSelMsg) {
        if (gFavDropFocusSelHwnd == hwnd) {
            gFavDropFocusSelHwnd = nullptr;
            TreeView_SelectItem(hwnd, nullptr);
            UpdateSidebarViewButtons(win);
        }
        return 0;
    }
    if (msg == WM_MOUSEMOVE && win && win->favTreeView && win->favTreeView->hwnd == hwnd) {
        FavTreeOnMouseMove(win, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    }
    if (msg == WM_TIMER && wp == kFavTipTimer && win && win->favTreeView && win->favTreeView->hwnd == hwnd) {
        KillTimer(hwnd, kFavTipTimer);
        FavTreeOnMouseHover(win, hwnd, gFavHoverX, gFavHoverY);
        return 0;
    }
    if (msg == WM_MOUSEHOVER && win && win->favTreeView && win->favTreeView->hwnd == hwnd) {
        FavTreeOnMouseHover(win, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    if (msg == WM_MOUSELEAVE && win && win->favTreeView && win->favTreeView->hwnd == hwnd) {
        FavTreeOnMouseLeave();
    }
    if (msg == WM_RBUTTONUP && win && win->favTreeView && win->favTreeView->hwnd == hwnd) {
        FavTreeOnMouseLeave();
        // Showing the menu inside the button-up makes TrackPopupMenu close at
        // once. Post the context menu so it opens after this message returns,
        // and do not let the tree notify the bookmark menu.
        POINT ptScreen = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &ptScreen);
        PostMessageW(hwnd, WM_CONTEXTMENU, (WPARAM)hwnd, MAKELPARAM(ptScreen.x, ptScreen.y));
        return 0;
    }
    if (msg == WM_CONTEXTMENU && win && win->favTreeView && win->favTreeView->hwnd == hwnd) {
        POINT ptScreen = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ShowFavoritesContextMenu(win, ptScreen.x, ptScreen.y);
        return 0;
    }
    if (msg == WM_PAINT) {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            PaintFavTreeRowsOverTheme(hwnd, hdc);
            ReleaseDC(hwnd, hdc);
        }
        return r;
    }
    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP) && win && win->favTreeView &&
        win->favTreeView->hwnd == hwnd) {
        // Jump on the press, same as a bookmark. A click on the empty part of
        // the row never produced TVC_BYMOUSE, so selection changed and the
        // page did not. The second click of a double-click landed on the
        // title and did jump.
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TreeItem ti = FavTreeItemAtPoint(win->favTreeView, pt.x, pt.y);
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (ti) {
            GoToFavForTreeItem(win, ti);
        }
        return r;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static UINT_PTR gFavTreeSubclassId = 0;

// The tree's own infotip paints on the row. Detach it; the popup below the row stays.
static void SilenceFavBuiltinTip(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    if (style & TVS_INFOTIP) {
        SetWindowLongPtr(hwnd, GWL_STYLE, style & ~TVS_INFOTIP);
    }
    HWND tip = TreeView_GetToolTips(hwnd);
    if (!tip) {
        return;
    }
    SendMessageW(tip, TTM_ACTIVATE, FALSE, 0);
    ShowWindow(tip, SW_HIDE);
    TreeView_SetToolTips(hwnd, nullptr);
}

static void AttachFavTreeSplitter(MainWindow* win, HWND hwnd) {
    if (!win || !hwnd) {
        return;
    }
    // Id 1 is already taken by the first window subclass in the process, so
    // SetWindowSubclass failed and this proc never saw the click.
    if (gFavTreeSubclassId == 0) {
        gFavTreeSubclassId = NextSubclassId();
    }
    SetWindowSubclass(hwnd, WndProcFavTree, gFavTreeSubclassId, (DWORD_PTR)win);
}

static WNDPROC gWndProcFavBox = nullptr;
static LRESULT CALLBACK WndProcFavBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return CallWindowProc(gWndProcFavBox, hwnd, msg, wp, lp);
    }

    LRESULT res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }

    if (msg == WM_ERASEBKGND) {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        COLORREF bgCol = ThemeSidebarBackgroundColor();
        HBRUSH br = CreateSolidBrush(bgCol);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        return 1;
    }

    TreeView* treeView = win->favTreeView;
    switch (msg) {
        case WM_SIZE:
            // Favorites view reparents the tree into the bookmarks column.
            if (treeView && treeView->hwnd && GetParent(treeView->hwnd) == hwnd) {
                LayoutTreeContainer(win->favLabelWithClose, treeView->hwnd);
                ScheduleFavTreeWrapHeights(win);
            }
            break;

        case WM_TIMER:
            if (wp == kTreeWrapHeightTimerId) {
                FlushFavTreeWrapHeights(win);
                return 0;
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wp) == IDC_FAV_LABEL_WITH_CLOSE) {
                ToggleFavorites(win);
            }
            break;
    }
    return CallWindowProc(gWndProcFavBox, hwnd, msg, wp, lp);
}

// in TableOfContents.cpp
extern void TocTreeKeyDown2(TreeView::KeyDownEvent*);

// The tree infotip for a clipped label paints over the row. A bookmark tip
// sits just below the row, in the dark tooltip. This popup does the same.
static HWND gFavPopTip = nullptr;
static HWND gFavPopOwner = nullptr;
static TreeItem gFavHoverItem = 0;
static bool gFavHoverTracking = false;
static HWND gFavHoverHwnd = nullptr;

static void FavPopEnsure(HWND owner) {
    if (gFavPopTip && IsWindow(gFavPopTip) && gFavPopOwner == owner) {
        return;
    }
    if (gFavPopTip && IsWindow(gFavPopTip)) {
        DestroyWindow(gFavPopTip);
    }
    gFavPopTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    gFavPopOwner = owner;
    if (!gFavPopTip) {
        return;
    }
    SetWindowPos(gFavPopTip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = owner;
    ti.uId = 1;
    ti.lpszText = (WCHAR*)L"";
    SendMessageW(gFavPopTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    int maxDx = DpiScale(owner, 420);
    SendMessageW(gFavPopTip, TTM_SETMAXTIPWIDTH, 0, maxDx);
    if (UseDarkModeLib() && ThemeUsesDarkChrome()) {
        DarkMode::setDarkTooltips(gFavPopTip, (int)DarkMode::ToolTipsType::tooltip);
    }
}

static void FavPopHide() {
    if (!gFavPopTip || !gFavPopOwner) {
        return;
    }
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = gFavPopOwner;
    ti.uId = 1;
    SendMessageW(gFavPopTip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

static bool FavLabelTruncated(HWND hwnd, TreeItem ti, const char* text, RECT& rcLabel) {
    rcLabel = {};
    FavTreeItem* fti = (FavTreeItem*)ti;
    if (!hwnd || !fti || !fti->hItem || !text) {
        return false;
    }
    if (!TreeView_GetItemRect(hwnd, fti->hItem, &rcLabel, TRUE)) {
        return false;
    }
    RECT rcClient{};
    GetClientRect(hwnd, &rcClient);
    int avail = rcClient.right - rcLabel.left - DpiScale(hwnd, 8);
    if (avail < 8) {
        return false;
    }
    HDC hdc = GetDC(hwnd);
    if (!hdc) {
        return false;
    }
    HFONT font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    TempWStr ws = ToWStrTemp(text);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, ws, lstrlenW(ws), &sz);
    if (old) {
        SelectObject(hdc, old);
    }
    ReleaseDC(hwnd, hdc);
    return sz.cx > avail;
}

static void FavPopShow(HWND hwnd, const char* text, const RECT& rcLabel) {
    FavPopEnsure(hwnd);
    if (!gFavPopTip) {
        return;
    }
    TempWStr ws = ToWStrTemp(text);
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd;
    ti.uId = 1;
    ti.lpszText = (WCHAR*)ws;
    SendMessageW(gFavPopTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    POINT pt = {rcLabel.left + DpiScale(hwnd, 6), rcLabel.bottom + DpiScale(hwnd, 4)};
    MapWindowPoints(hwnd, HWND_DESKTOP, &pt, 1);
    SendMessageW(gFavPopTip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y));
    SendMessageW(gFavPopTip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

static void FavTreeOnMouseMove(MainWindow* win, HWND hwnd, int x, int y) {
    gFavHoverHwnd = hwnd;
    gFavHoverX = x;
    gFavHoverY = y;
    TreeItem ti = 0;
    if (win && win->favTreeView) {
        ti = FavTreeItemAtPoint(win->favTreeView, x, y);
    }
    if (ti != gFavHoverItem) {
        FavPopHide();
        gFavHoverItem = ti;
        gFavHoverTracking = false;
        KillTimer(hwnd, kFavTipTimer);
        if (ti) {
            SetTimer(hwnd, kFavTipTimer, 300, nullptr);
        }
        if (ThemeUsesDarkChrome()) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    if (gFavHoverTracking) {
        return;
    }
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_HOVER | TME_LEAVE;
    tme.hwndTrack = hwnd;
    tme.dwHoverTime = HOVER_DEFAULT;
    TrackMouseEvent(&tme);
    gFavHoverTracking = true;
}

static void FavTreeOnMouseHover(MainWindow* win, HWND hwnd, int x, int y) {
    gFavHoverTracking = false;
    if (TreeWrapLabelsEnabled() || !win || !win->favTreeView) {
        FavPopHide();
        return;
    }
    TreeItem ti = FavTreeItemAtPoint(win->favTreeView, x, y);
    gFavHoverItem = ti;
    char* text = (ti && win->favTreeView->treeModel) ? win->favTreeView->treeModel->Text(ti) : nullptr;
    RECT rcLabel{};
    if (!FavLabelTruncated(hwnd, ti, text, rcLabel)) {
        FavPopHide();
        return;
    }
    FavPopShow(hwnd, text, rcLabel);
}

static void FavTreeOnMouseLeave() {
    HWND hwnd = gFavHoverHwnd;
    if (hwnd) {
        KillTimer(hwnd, kFavTipTimer);
    }
    gFavHoverTracking = false;
    gFavHoverItem = 0;
    FavPopHide();
    if (ThemeUsesDarkChrome() && hwnd) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

static void FavCustomizeTooltip(TreeView::GetTooltipEvent* ev) {
    // Keep the in-place bar from covering the row. The hover popup is the tip.
    if (ev && ev->info && ev->info->pszText && ev->info->cchTextMax > 0) {
        ev->info->pszText[0] = 0;
    }
}

static void InitFavTreeViewHandlers(TreeView* treeView) {
    auto fn = MkFunc1Void(FavTreeContextMenu);
    treeView->onContextMenu = fn;
    treeView->onSelectionChanged = MkFunc1Void(FavTreeSelectionChanged);
    treeView->onKeyDown = MkFunc1Void(TocTreeKeyDown2);
    treeView->onClick = MkFunc1Void(FavTreeItemClicked);
    treeView->onCustomDraw = MkFunc1Void(FavTreeWrapOnCustomDraw);
    treeView->onGetTooltip = MkFunc1Void(FavCustomizeTooltip);
}

void ReCreateFavTreeView(MainWindow* win, HFONT font, int dpi) {
    if (!win || !win->hwndFavBox || !win->favTreeView) {
        return;
    }

    TreeView* oldTreeView = win->favTreeView;
    bool hadFocus = GetFocus() == oldTreeView->hwnd;
    bool hadModel = oldTreeView->treeModel != nullptr;
    RememberFavTreeExpansionState(win);

    TreeModel* oldModel = oldTreeView->treeModel;
    oldTreeView->treeModel = nullptr;
    delete oldTreeView;
    delete oldModel;
    win->favTreeView = nullptr;

    auto treeView = new TreeView();
    TreeView::CreateArgs args;
    args.parent = win->hwndFavBox;
    args.font = font;
    args.fullRowSelect = true;
    TreeWrapLabelsConfigureCreateArgs(args);
    args.exStyle = 0;
    args.isRtl = IsUIRtl();
    InitFavTreeViewHandlers(treeView);

    treeView->Create(args);
    ReportIf(!treeView->hwnd);
    win->favTreeView = treeView;
    AttachFavTreeSplitter(win, treeView->hwnd);

    if (hadModel) {
        TreeModel* newModel = BuildFavTreeModel(win, win->sidebarFindText[2].Get());
        treeView->SetTreeModel(newModel);
    }
    if (font) {
        HwndSetTreeFontForDpi(treeView->hwnd, font, dpi);
    }

    UpdateControlsColors(win);
    SilenceFavBuiltinTip(treeView->hwnd);
    LayoutTreeContainer(win->favLabelWithClose, treeView->hwnd);
    FavTreeWrapRecalcHeights(win);
    if (treeView->hwnd) {
        InvalidateRect(treeView->hwnd, nullptr, FALSE);
    }
    if (hadFocus) {
        SetFocus(treeView->hwnd);
    }
}

void CreateFavorites(MainWindow* win) {
    HMODULE h = GetModuleHandleW(nullptr);
    int dx = gGlobalPrefs->sidebarDx;
    DWORD dwStyle = WS_CHILD | WS_CLIPCHILDREN;
    win->hwndFavBox = CreateWindowW(WC_STATIC, L"", dwStyle, 0, 0, dx, 0, win->hwndFrame, (HMENU) nullptr, h, nullptr);

    auto l = new LabelWithCloseWnd();
    {
        LabelWithCloseWnd::CreateArgs args;
        args.parent = win->hwndFavBox;
        args.cmdId = IDC_FAV_LABEL_WITH_CLOSE;
        // TODO: use the same font size as in GetTreeFont()?
        args.font = GetAppFontForHwnd(win->hwndFrame);
        args.isRtl = IsUIRtl();
        l->Create(args);
    }

    win->favLabelWithClose = l;
    l->SetPaddingXY(2, 2);
    // label is set in UpdateToolbarSidebarText()

    auto treeView = new TreeView();
    TreeView::CreateArgs args;
    args.parent = win->hwndFavBox;
    args.font = GetAppTreeFontForHwnd(win->hwndFrame);
    args.fullRowSelect = true;
    TreeWrapLabelsConfigureCreateArgs(args);
    args.exStyle = 0;
    args.isRtl = IsUIRtl();

    InitFavTreeViewHandlers(treeView);

    // treeView->onChar = TocTreeCharHandler;
    // treeView->onMouseWheel = TocTreeMouseWheelHandler;

    treeView->Create(args);
    ReportIf(!treeView->hwnd);

    win->favTreeView = treeView;
    AttachFavTreeSplitter(win, treeView->hwnd);

    if (nullptr == gWndProcFavBox) {
        gWndProcFavBox = (WNDPROC)GetWindowLongPtr(win->hwndFavBox, GWLP_WNDPROC);
    }
    SetWindowLongPtr(win->hwndFavBox, GWLP_WNDPROC, (LONG_PTR)WndProcFavBox);

    UpdateControlsColors(win);
    SilenceFavBuiltinTip(treeView->hwnd);
}
