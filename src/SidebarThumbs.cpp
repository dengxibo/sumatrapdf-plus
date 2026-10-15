/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/FileUtil.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "DisplayModel.h"
#include "Theme.h"
#include "PdfDarkMode.h"
#include "RenderCache.h"
#include "FileHistory.h"
#include "Favorites.h"
#include "TableOfContents.h"
#include "SvgIcons.h"
#include "Translations.h"
#include "SidebarThumbs.h"
#include "InlineTranslate.h"
#include "EngineAll.h"
#include "EbookAnnotations.h"
#include "EditAnnotations.h"
#include "EditEbookAnnotations.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "Toolbar.h"
#include "AppDialogTheme.h"
#include "Menu.h"
#include "SearchAndDDE.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "OcrService.h"
#include "Commands.h"
#include "PdfPageSelection.h"

constexpr UINT_PTR kThumbRenderTimerId = 0x106;
constexpr const WCHAR* kThumbsClass = L"SUMATRA_SIDEBAR_THUMBS";

struct ThumbSlot {
    int pageNo = 0;
    u32 themeHash = 0;
    RenderedBitmap* bmp = nullptr;
};

struct SidebarThumbsWnd {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    Vec<int> selected;
    int anchor = 0;
    Point dragStart;
    bool dragging = false;
    int dropBefore = 0;
    bool busy = false;
    int pendingAction = 0;
    int pendingAt = 0;
    u32 generation = 1;
    Vec<ThumbSlot> slots;
    int scrollY = 0;
    int contentDy = 0;
    int cols = 1;
    int thumbDx = 0;
    int thumbDy = 0;
    int gap = 0;
    int pageCount = 0;
    Vec<int> shownPages;
    AutoFreeStr appliedFind;
    bool shownDirty = true;
    u32 themeHash = 0;
    bool rendering = false;
    // Last document page we scrolled into view. Relayout reports the same page
    // again; following it every time yanks the list while thumbnails load.
    int followedPage = 0;
    DocController* boundCtrl = nullptr;
};

static ATOM gThumbsClassAtom = 0;

SidebarView SidebarViewFromStr(const char* s) {
    if (str::EqI(s, "thumbnails")) {
        return SidebarView::Thumbnails;
    }
    if (str::EqI(s, "favorites")) {
        return SidebarView::Favorites;
    }
    if (str::EqI(s, "ai")) {
        return SidebarView::Ai;
    }
    if (str::EqI(s, "annotations")) {
        return SidebarView::Annotations;
    }
    return SidebarView::Bookmarks;
}

const char* SidebarViewToStr(SidebarView view) {
    switch (view) {
        case SidebarView::Thumbnails:
            return "thumbnails";
        case SidebarView::Favorites:
            return "favorites";
        case SidebarView::Ai:
            return "ai";
        case SidebarView::Annotations:
            return "annotations";
        default:
            return "bookmarks";
    }
}

SidebarView CurrentSidebarView(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab) {
        return SidebarView::Bookmarks;
    }
    int v = tab->sidebarView;
    if (v < 0 || v > (int)SidebarView::Annotations) {
        return SidebarView::Bookmarks;
    }
    return (SidebarView)v;
}

bool SidebarViewAvailable(MainWindow* win, SidebarView view) {
    if (!win || !win->IsDocLoaded() || !win->ctrl) {
        return false;
    }
    switch (view) {
        case SidebarView::Bookmarks:
            return win->ctrl->HasToc();
        case SidebarView::Thumbnails:
            return !win->AsChm() && win->ctrl->PageCount() > 0;
        case SidebarView::Favorites:
            return !gPluginMode && CanAccessDisk();
        case SidebarView::Ai:
            return gGlobalPrefs && gGlobalPrefs->enableAskAI && HasPermission(Perm::InternetAccess);
        case SidebarView::Annotations: {
            WindowTab* tab = win->CurrentTab();
            if (!tab) {
                return false;
            }
            if (EbookAnnotationsSupported(tab)) {
                return true;
            }
            return EngineSupportsAnnotations(tab->GetEngine());
        }
    }
    return false;
}

SidebarView FallbackSidebarView(MainWindow* win, SidebarView want) {
    if (SidebarViewAvailable(win, want)) {
        return want;
    }
    if (SidebarViewAvailable(win, SidebarView::Bookmarks)) {
        return SidebarView::Bookmarks;
    }
    if (SidebarViewAvailable(win, SidebarView::Thumbnails)) {
        return SidebarView::Thumbnails;
    }
    if (SidebarViewAvailable(win, SidebarView::Favorites)) {
        return SidebarView::Favorites;
    }
    return want;
}

void LoadTabSidebarView(WindowTab* tab) {
    if (!tab) {
        return;
    }
    tab->sidebarView = 0;
    if (!tab->filePath || !gGlobalPrefs->rememberStatePerDocument) {
        return;
    }
    FileState* fs = gFileHistory.FindByPath(tab->filePath);
    if (!fs || fs->useDefaultState || str::IsEmpty(fs->sidebarView)) {
        return;
    }
    tab->sidebarView = (int)SidebarViewFromStr(fs->sidebarView);
}

void RememberTabSidebarView(WindowTab* tab) {
    if (!tab || !tab->filePath) {
        return;
    }
    FileState* fs = gFileHistory.FindByPath(tab->filePath);
    if (!fs) {
        return;
    }
    str::ReplaceWithCopy(&fs->sidebarView, SidebarViewToStr((SidebarView)tab->sidebarView));
}

static SidebarThumbsWnd* ThumbsFromWin(MainWindow* win) {
    if (!win || !win->hwndSidebarThumbs) {
        return nullptr;
    }
    return (SidebarThumbsWnd*)GetWindowLongPtr(win->hwndSidebarThumbs, GWLP_USERDATA);
}

static void ClearThumbSlots(SidebarThumbsWnd* t) {
    for (ThumbSlot& s : t->slots) {
        delete s.bmp;
        s.bmp = nullptr;
    }
    t->slots.Reset();
    t->shownPages.Reset();
    t->shownDirty = true;
}

static RenderedBitmap* FindThumb(SidebarThumbsWnd* t, int pageNo) {
    for (ThumbSlot& s : t->slots) {
        if (s.pageNo == pageNo && s.themeHash == t->themeHash) {
            return s.bmp;
        }
    }
    return nullptr;
}

static void StoreThumb(SidebarThumbsWnd* t, int pageNo, RenderedBitmap* bmp) {
    for (ThumbSlot& s : t->slots) {
        if (s.pageNo == pageNo) {
            delete s.bmp;
            s.bmp = bmp;
            s.themeHash = t->themeHash;
            return;
        }
    }
    if (t->slots.Size() > 80) {
        delete t->slots.at(0).bmp;
        t->slots.RemoveAt(0);
    }
    ThumbSlot slot;
    slot.pageNo = pageNo;
    slot.themeHash = t->themeHash;
    slot.bmp = bmp;
    t->slots.Append(slot);
}

static void LayoutMetrics(SidebarThumbsWnd* t) {
    Rect rc = ClientRect(t->hwnd);
    t->gap = DpiScale(t->hwnd, 8);
    int pad = DpiScale(t->hwnd, 8);
    int inner = rc.dx - pad * 2;
    if (inner < 40) {
        inner = 40;
    }
    int minDx = DpiScale(t->hwnd, 96);
    t->cols = std::max(1, (inner + t->gap) / (minDx + t->gap));
    t->thumbDx = (inner - (t->cols - 1) * t->gap) / t->cols;
    if (t->thumbDx < 40) {
        t->thumbDx = 40;
    }
    t->thumbDy = t->thumbDx * 4 / 3;
    int n = t->shownPages.Size();
    int rows = n > 0 ? (n + t->cols - 1) / t->cols : 0;
    t->contentDy = rows > 0 ? pad + rows * (t->thumbDy + t->gap) : 0;
    int maxScroll = std::max(0, t->contentDy - rc.dy);
    if (t->scrollY > maxScroll) {
        t->scrollY = maxScroll;
    }
    if (t->scrollY < 0) {
        t->scrollY = 0;
    }
}

static Rect ThumbRect(SidebarThumbsWnd* t, int pageIndex) {
    int pad = DpiScale(t->hwnd, 8);
    int col = pageIndex % t->cols;
    int row = pageIndex / t->cols;
    return Rect(pad + col * (t->thumbDx + t->gap), pad + row * (t->thumbDy + t->gap) - t->scrollY, t->thumbDx,
                t->thumbDy);
}

static int PageAtPoint(SidebarThumbsWnd* t, Point pt) {
    for (int i = 0; i < t->shownPages.Size(); i++) {
        if (ThumbRect(t, i).Contains(pt)) {
            return t->shownPages[i];
        }
    }
    return 0;
}

static int FirstMissingVisiblePage(SidebarThumbsWnd* t) {
    Rect rc = ClientRect(t->hwnd);
    for (int i = 0; i < t->shownPages.Size(); i++) {
        Rect r = ThumbRect(t, i);
        if (r.y + r.dy < 0 || r.y > rc.dy) {
            continue;
        }
        if (!FindThumb(t, t->shownPages[i])) {
            return t->shownPages[i];
        }
    }
    return 0;
}

struct SidebarThumbJob {
    u32 generation = 0;
    HWND hwnd = nullptr;
    EngineBase* engine = nullptr;
    int pageNo = 0;
    float zoom = 0;
    RectF box;
    DarkModeProfile profile;
    bool hasProfile = false;
    u32 themeHash = 0;
    RenderedBitmap* bmp = nullptr;
};

static int ShownIndexOf(SidebarThumbsWnd* t, int pageNo) {
    for (int i = 0; i < t->shownPages.Size(); i++) {
        if (t->shownPages[i] == pageNo) {
            return i;
        }
    }
    return -1;
}

static void SidebarThumbDeliver(SidebarThumbJob* job);

static void SidebarThumbWorker(SidebarThumbJob* job) {
    RenderPageArgs args(job->pageNo, job->zoom, 0, nullptr, RenderTarget::Export);
    if (job->hasProfile) {
        args.darkProfile = &job->profile;
    }
    job->bmp = job->engine->RenderPage(args);
    uitask::Post(MkFunc0<SidebarThumbJob>(SidebarThumbDeliver, job), "SidebarThumb");
}

// Back on the UI thread. The page was rendered off this thread so scrolling
// and resizing are not stuck behind each bitmap.
static void SidebarThumbDeliver(SidebarThumbJob* job) {
    defer {
        if (job->engine) {
            job->engine->Release();
        }
        delete job->bmp;
        delete job;
    };
    if (!job->hwnd || !IsWindow(job->hwnd)) {
        return;
    }
    auto* t = (SidebarThumbsWnd*)GetWindowLongPtr(job->hwnd, GWLP_USERDATA);
    if (!t) {
        return;
    }
    t->rendering = false;
    ApplyRenderThemePostColors(job->engine, job->bmp, job->pageNo, job->zoom, &job->box,
                               job->hasProfile ? &job->profile : nullptr);
    WindowTab* tab = t->win->CurrentTab();
    if (job->bmp && job->themeHash == t->themeHash && job->generation == t->generation && tab &&
        tab->GetEngine() == job->engine) {
        StoreThumb(t, job->pageNo, job->bmp);
        job->bmp = nullptr;
        int idx = ShownIndexOf(t, job->pageNo);
        if (idx >= 0) {
            RECT rc = ToRECT(ThumbRect(t, idx));
            InflateRect(&rc, 2, 2);
            InvalidateRect(job->hwnd, &rc, FALSE);
        }
    }
    if (FirstMissingVisiblePage(t) > 0) {
        SetTimer(t->hwnd, kThumbRenderTimerId, 1, nullptr);
    }
}

static void RenderOneThumb(SidebarThumbsWnd* t) {
    if (t->rendering || t->busy || t->pendingAction) {
        return;
    }
    int pageNo = FirstMissingVisiblePage(t);
    if (pageNo <= 0) {
        KillTimer(t->hwnd, kThumbRenderTimerId);
        return;
    }
    MainWindow* win = t->win;
    if (!win || !win->IsDocLoaded() || !win->ctrl) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    EngineBase* engine = tab ? tab->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    // Same coloring as the printed-TOC page picker (RenderAiTocThumbnail):
    // the view profile, Export pixels, then post colors with the page box.
    // A second LegacyInvert on a reflow page that is already themed turns it light.
    RectF box = engine->PageMediabox(pageNo);
    if (box.IsEmpty()) {
        return;
    }
    float pagePx = box.dx > 1.f ? box.dx * 96.f / 72.f : (float)t->thumbDx;
    float zoom = pagePx > 1.f ? (float)t->thumbDx / pagePx : 0.2f;
    if (zoom < 0.05f) {
        zoom = 0.05f;
    }
    if (zoom > 1.5f) {
        zoom = 1.5f;
    }
    DarkModeProfile profile;
    BuildViewDarkModeProfile(engine, &profile);
    t->themeHash = profile.hash;

    auto* job = new SidebarThumbJob();
    job->hwnd = t->hwnd;
    job->generation = t->generation;
    job->engine = engine;
    job->pageNo = pageNo;
    job->zoom = zoom;
    job->box = box;
    job->profile = profile;
    job->hasProfile = profile.mode != PageColorMode::Normal;
    job->themeHash = profile.hash;
    engine->AddRef();
    t->rendering = true;
    RunAsync(MkFunc0<SidebarThumbJob>(SidebarThumbWorker, job), "SidebarThumb");
}

static void AddShownPage(Vec<int>& pages, int pageNo, int pageCount) {
    if (pageNo < 1 || pageNo > pageCount) {
        return;
    }
    for (int i = 0; i < pages.Size(); i++) {
        if (pages[i] == pageNo) {
            return;
        }
        if (pages[i] > pageNo) {
            pages.InsertAt((size_t)i, pageNo);
            return;
        }
    }
    pages.Append(pageNo);
}

// Same match as the bookmark filter: a heading title contains the query.
static void CollectTitlePages(TocItem* item, const char* query, Vec<int>& pages, int pageCount) {
    for (TocItem* si = item; si; si = si->next) {
        if (si->title && str::ContainsI(si->title, query)) {
            AddShownPage(pages, si->pageNo, pageCount);
        }
        if (si->child) {
            CollectTitlePages(si->child, query, pages, pageCount);
        }
    }
}

static void RebuildShownPages(SidebarThumbsWnd* t) {
    if (!t) {
        return;
    }
    int n = 0;
    TocItem* root = nullptr;
    if (t->win && t->win->IsDocLoaded() && t->win->ctrl) {
        n = t->win->ctrl->PageCount();
        WindowTab* tab = t->win->CurrentTab();
        if (tab && tab->currToc) {
            root = tab->currToc->root;
        }
    }
    t->pageCount = n;
    const char* query = t->win ? t->win->sidebarFindText[0].Get() : nullptr;
    t->shownPages.Reset();
    if (str::IsEmpty(query)) {
        for (int pageNo = 1; pageNo <= n; pageNo++) {
            t->shownPages.Append(pageNo);
        }
    } else if (root) {
        CollectTitlePages(root, query, t->shownPages, n);
    }
    t->shownDirty = false;
}

// Same bottom tag as the printed-TOC page picker: the page number on the theme
// control color, with a hairline frame, sitting on the thumbnail.
static void DrawThumbPageLabel(HDC hdc, HWND hwnd, const Rect& pageRc, int pageNo, HFONT font) {
    if (!font || pageRc.dx < 8 || pageRc.dy < 8) {
        return;
    }
    TempStr label = str::FormatTemp("%d", pageNo);
    WCHAR* text = ToWStrTemp(label);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    RECT textRc{0, 0, 0, 0};
    DrawTextW(hdc, text, -1, &textRc, DT_NOPREFIX | DT_CALCRECT | DT_SINGLELINE);
    int padX = DpiScale(hwnd, 6);
    int h = DpiScale(hwnd, 16);
    int inset = DpiScale(hwnd, 4);
    int maxW = pageRc.dx - inset * 2;
    if (maxW < 8 || pageRc.dy < h + inset) {
        SelectObject(hdc, oldFont);
        return;
    }
    int w = (textRc.right - textRc.left) + padX * 2;
    if (w > maxW) {
        w = maxW;
    }
    int x = pageRc.x + (pageRc.dx - w) / 2;
    int y = pageRc.y + pageRc.dy - inset - h;
    RECT rc{x, y, x + w, y + h};
    AutoDeleteBrush fill(CreateSolidBrush(ThemeWindowControlBackgroundColor()));
    FillRect(hdc, &rc, fill);
    bool dark = ThemeUsesDarkChrome();
    AutoDeleteBrush frame(CreateSolidBrush(dark ? RGB(78, 80, 88) : RGB(206, 208, 214)));
    FrameRect(hdc, &rc, frame);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
}

static void PaintThumbs(SidebarThumbsWnd* t, HDC hdc) {
    Rect rc = ClientRect(t->hwnd);
    COLORREF bg = ThemeSidebarBackgroundColor();
    AutoDeleteBrush br(CreateSolidBrush(bg));
    RECT fill = ToRECT(rc);
    FillRect(hdc, &fill, br);

    MainWindow* win = t->win;
    int current = 0;
    if (win && win->ctrl) {
        current = win->ctrl->CurrentPageNo();
    }
    if (t->shownDirty || (win && win->ctrl && win->ctrl->PageCount() != t->pageCount)) {
        RebuildShownPages(t);
    }
    LayoutMetrics(t);

    COLORREF border = ThemeWindowTextDisabledColor();
    COLORREF currentCol = ThemeSelectionFrameColor();
    AutoDeletePen pen(CreatePen(PS_SOLID, 1, border));
    AutoDeletePen penCur(CreatePen(PS_SOLID, DpiScale(t->hwnd, 2), currentCol));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ThemeWindowTextColor());
    HFONT font = CreateFontW(-DpiScale(t->hwnd, 11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei UI");

    for (int i = 0; i < t->shownPages.Size(); i++) {
        Rect r = ThumbRect(t, i);
        if (r.y + r.dy < 0 || r.y > rc.dy) {
            continue;
        }
        int pageNo = t->shownPages[i];
        RenderedBitmap* bmp = FindThumb(t, pageNo);
        COLORREF pageBg = ThemeUsesDarkChrome() ? bg : RGB(255, 255, 255);
        AutoDeleteBrush pageBr(CreateSolidBrush(pageBg));
        RECT pr = ToRECT(r);
        FillRect(hdc, &pr, pageBr);
        if (bmp) {
            bmp->Blit(hdc, r);
        }
        HPEN use =
            (t->selected.Contains(pageNo) || (t->selected.IsEmpty() && pageNo == current)) ? (HPEN)penCur : (HPEN)pen;
        HGDIOBJ old = SelectObject(hdc, use);
        HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, old);
        DrawThumbPageLabel(hdc, t->hwnd, r, pageNo, font);
    }
    if (t->dragging && t->dropBefore > 0) {
        int idx = ShownIndexOf(t, std::min(t->dropBefore, t->pageCount));
        if (idx >= 0) {
            Rect r = ThumbRect(t, idx);
            int x = t->dropBefore > t->pageCount ? r.Right() + 3 : r.x - 3;
            HGDIOBJ old = SelectObject(hdc, (HPEN)penCur);
            MoveToEx(hdc, x, r.y, nullptr);
            LineTo(hdc, x, r.Bottom());
            SelectObject(hdc, old);
        }
    }
    DeleteObject(font);
    if (FirstMissingVisiblePage(t) > 0) {
        SetTimer(t->hwnd, kThumbRenderTimerId, 1, nullptr);
    }
}

static void ScrollThumbsBy(SidebarThumbsWnd* t, int dy) {
    LayoutMetrics(t);
    int maxScroll = std::max(0, t->contentDy - ClientRect(t->hwnd).dy);
    int next = t->scrollY + dy;
    if (next < 0) {
        next = 0;
    }
    if (next > maxScroll) {
        next = maxScroll;
    }
    if (next == t->scrollY) {
        return;
    }
    t->scrollY = next;
    InvalidateRect(t->hwnd, nullptr, FALSE);
}

enum ThumbPageAction {
    ThumbCopy = 41001,
    ThumbCut,
    ThumbDelete,
    ThumbPasteBefore,
    ThumbPasteAfter,
    ThumbUndo,
    ThumbMove,
    ThumbSelectAll,
    ThumbInsertBefore,
    ThumbInsertAfter,
    ThumbPasteImage,
    ThumbExtract,
    ThumbOcr
};

static UINT PageClipboardFormat() {
    return RegisterClipboardFormatW(L"SumatraPDF-Plus.Pages.v1");
}

static bool WritePageClipboard(HWND hwnd, ByteSlice data) {
    if (!data.size() || !OpenClipboard(hwnd)) return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, data.size() + sizeof(u64));
    bool ok = false;
    if (memory) {
        auto* dst = (u8*)GlobalLock(memory);
        if (dst) {
            u64 size = data.size();
            memcpy(dst, &size, sizeof(size));
            memcpy(dst + sizeof(size), data.data(), data.size());
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(PageClipboardFormat(), memory)) ok = true;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

static ByteSlice ReadPageClipboard(HWND hwnd) {
    ByteSlice result;
    if (!OpenClipboard(hwnd)) return result;
    HGLOBAL memory = GetClipboardData(PageClipboardFormat());
    SIZE_T total = memory ? GlobalSize(memory) : 0;
    auto* src = memory ? (u8*)GlobalLock(memory) : nullptr;
    if (src && total >= sizeof(u64)) {
        u64 size;
        memcpy(&size, src, sizeof(size));
        if (size && size <= total - sizeof(size) && size <= 256 * 1024 * 1024) {
            result = ByteSlice((u8*)memdup(src + sizeof(size), (size_t)size), (size_t)size);
        }
    }
    if (src) GlobalUnlock(memory);
    CloseClipboard();
    return result;
}

static ByteSlice ReadClipboardImage(HWND hwnd) {
    ByteSlice result;
    if (!OpenClipboard(hwnd)) return result;
    HGLOBAL memory = GetClipboardData(CF_DIBV5);
    if (!memory) memory = GetClipboardData(CF_DIB);
    SIZE_T size = memory ? GlobalSize(memory) : 0;
    auto* dib = memory ? (u8*)GlobalLock(memory) : nullptr;
    if (dib && size >= sizeof(BITMAPINFOHEADER) && size <= 256 * 1024 * 1024) {
        BITMAPINFOHEADER header;
        memcpy(&header, dib, sizeof(header));
        if (header.biSize >= sizeof(header) && header.biSize <= size && header.biBitCount > 0 &&
            header.biBitCount <= 32) {
            size_t colors =
                header.biClrUsed ? header.biClrUsed : (header.biBitCount <= 8 ? 1u << header.biBitCount : 0);
            size_t offset = header.biSize + colors * sizeof(RGBQUAD);
            if (header.biSize == sizeof(header) && header.biCompression == BI_BITFIELDS) offset += 12;
            if (offset <= size) {
                BITMAPFILEHEADER fileHeader{};
                fileHeader.bfType = 0x4d42;
                fileHeader.bfSize = (DWORD)(size + sizeof(fileHeader));
                fileHeader.bfOffBits = (DWORD)(offset + sizeof(fileHeader));
                auto* bytes = (u8*)malloc(fileHeader.bfSize);
                if (bytes) {
                    memcpy(bytes, &fileHeader, sizeof(fileHeader));
                    memcpy(bytes + sizeof(fileHeader), dib, size);
                    result = ByteSlice(bytes, fileHeader.bfSize);
                }
            }
        }
    }
    if (dib) GlobalUnlock(memory);
    CloseClipboard();
    return result;
}

static AutoFreeStr ChoosePageFile(HWND hwnd, bool extract) {
    WCHAR path[MAX_PATH]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = dimof(path);
    ofn.lpstrFilter = extract ? L"PDF documents\0*.pdf\0\0"
                              : L"PDF / Images\0*.pdf;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.webp\0All files\0*.*\0\0";
    ofn.lpstrDefExt = L"pdf";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (extract ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    bool ok = extract ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return AutoFreeStr(ok ? str::Dup(ToUtf8Temp(path)) : nullptr);
}

struct ThumbEditJob {
    HWND hwnd = nullptr, owner = nullptr;
    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;
    DisplayModel* dm = nullptr;
    EngineBase* engine = nullptr;
    Vec<int> selected, order;
    ByteSlice clipboard;
    AutoFreeStr filePath;
    int action = 0, at = 0, page = 1;
    bool ok = false, ownerEnabled = false, paused = false;
    ~ThumbEditJob() {
        clipboard.Free();
        engine->Release();
    }
};

static void ThumbStartAction(SidebarThumbsWnd*, int, int = 0);
static void ThumbEditFinished(ThumbEditJob* job) {
    AutoDelete cleanup(job);
    if (!IsMainWindowValid(job->win)) return;
    if (job->ownerEnabled) EnableWindow(job->owner, TRUE);
    auto* t = IsWindow(job->hwnd) ? (SidebarThumbsWnd*)GetWindowLongPtrW(job->hwnd, GWLP_USERDATA) : nullptr;
    if (!t || job->win->CurrentTab() != job->tab || job->tab->AsFixed() != job->dm) return;
    t->busy = false;
    job->dm->pauseRendering = job->paused;
    EnableWindow(job->hwnd, TRUE);
    if (job->action == ThumbCopy || job->action == ThumbCut) {
        job->ok = job->ok && WritePageClipboard(job->hwnd, job->clipboard);
        if (job->ok && job->action == ThumbCut) {
            ThumbStartAction(t, ThumbDelete);
            return;
        }
    } else if (job->action != ThumbExtract) {
        gRenderCache->FreeForDisplayModel(job->dm);
        job->dm->RefreshPageStructure(job->page);
        job->tab->askedToSaveAnnotations = false;
        ClearThumbSlots(t);
        t->generation++;
        t->selected.Reset();
        t->selected.Append(job->dm->CurrentPageNo());
        t->anchor = job->dm->CurrentPageNo();
        t->followedPage = 0;
        ClearTocBox(job->win);
        LoadTocTree(job->win);
        UpdateToolbarPageText(job->win, job->engine->PageCount());
        ToolbarUpdateStateForWindow(job->win, false);
        job->win->RedrawAll(true);
    }
    if (!job->ok) MessageBoxWarning(job->hwnd, _TRA("Unable to edit or copy the selected pages."), _TRA("Pages"));
    InvalidateRect(job->hwnd, nullptr, FALSE);
    SetFocus(job->hwnd);
    SetTimer(job->hwnd, kThumbRenderTimerId, 1, nullptr);
}

static void ThumbEditWorker(ThumbEditJob* job) {
    if (job->action == ThumbCopy || job->action == ThumbCut) {
        job->clipboard = EngineMupdfCopyPages(job->engine, job->selected);
        job->ok = job->clipboard.size() > 0;
    } else if (job->action == ThumbExtract) {
        job->clipboard = EngineMupdfCopyPages(job->engine, job->selected);
        job->ok = job->clipboard.size() && file::WriteFile(job->filePath, job->clipboard);
    } else {
        if (job->action == ThumbInsertBefore || job->action == ThumbInsertAfter || job->action == ThumbPasteImage) {
            ByteSlice source = job->action == ThumbPasteImage ? job->clipboard : file::ReadFile(job->filePath);
            ByteSlice imported = EngineMupdfImportPageData(job->engine, source);
            if (job->action != ThumbPasteImage) source.Free();
            job->clipboard.Free();
            job->clipboard = imported;
            if (!imported.size()) {
                uitask::Post(MkFunc0<ThumbEditJob>(ThumbEditFinished, job), "ThumbEditFinished");
                return;
            }
        }
        gRenderCache->WaitForRenderingComplete(job->dm);
        job->ok = job->action == ThumbUndo ? EngineMupdfUndoPageOrder(job->engine)
                                           : EngineMupdfEditPageOrder(job->engine, job->order, job->clipboard, job->at);
    }
    uitask::Post(MkFunc0<ThumbEditJob>(ThumbEditFinished, job), "ThumbEditFinished");
}

static void ThumbStartAction(SidebarThumbsWnd* t, int action, int at) {
    if (action < ThumbCopy || action > ThumbExtract || t->busy || !t->win || !t->win->AsFixed()) return;
    WindowTab* tab = t->win->CurrentTab();
    EngineBase* engine = tab->GetEngine();
    if (!EngineMupdfCanEditPdfToc(engine)) return;
    if (action == ThumbSelectAll) {
        t->selected = t->shownPages;
        InvalidateRect(t->hwnd, nullptr, FALSE);
        return;
    }
    if (t->selected.IsEmpty()) t->selected.Append(t->win->ctrl->CurrentPageNo());
    if (action == ThumbUndo && !EngineMupdfCanUndoPageOrder(engine)) return;
    if (action != ThumbCopy && engine->HasUnsavedOcrText()) {
        MessageBoxWarning(t->hwnd, _TRA("Save the recognized text before changing the page order."), _TRA("Pages"));
        return;
    }
    if ((action == ThumbDelete || action == ThumbCut) && t->selected.Size() >= engine->PageCount()) {
        MessageBoxWarning(t->hwnd, _TRA("Keep at least one page in the document."), _TRA("Pages"));
        return;
    }
    for (int p = 1; p <= engine->PageCount(); p++) {
        if (OcrPageIsPending(engine, p)) {
            MessageBoxWarning(t->hwnd, _TRA("Wait for text recognition to finish before editing pages."),
                              _TRA("Pages"));
            return;
        }
    }
    if (t->rendering) {
        t->pendingAction = action;
        t->pendingAt = at;
        SetTimer(t->hwnd, kThumbRenderTimerId + 1, 25, nullptr);
        return;
    }
    if (action == ThumbDelete &&
        AppMessageBoxW(t->hwnd, ToWStrTemp(str::FormatTemp(_TRA("Delete %d selected page(s)?"), t->selected.Size())),
                       _TRW("Delete pages"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    HWND sourceHwnd = t->hwnd;
    MainWindow* sourceWin = t->win;
    AbortFinding(sourceWin, true, true, true);
    if (!IsWindow(sourceHwnd) || !IsMainWindowValid(sourceWin)) return;
    if ((SidebarThumbsWnd*)GetWindowLongPtrW(sourceHwnd, GWLP_USERDATA) != t || sourceWin->CurrentTab() != tab ||
        tab->GetEngine() != engine)
        return;
    auto* job = new ThumbEditJob;
    job->hwnd = t->hwnd;
    job->owner = t->win->hwndFrame;
    job->win = t->win;
    job->tab = tab;
    job->dm = tab->AsFixed();
    job->engine = engine;
    engine->AddRef();
    job->action = action;
    for (int p = 1; p <= engine->PageCount(); p++)
        if (t->selected.Contains(p)) job->selected.Append(p);
    job->page = t->win->ctrl->CurrentPageNo();
    for (int p = 1; p <= engine->PageCount(); p++) {
        if ((action != ThumbDelete && action != ThumbMove) || !t->selected.Contains(p)) job->order.Append(p);
    }
    if (action == ThumbMove) {
        int count = engine->PageCount();
        int* order = AllocArray<int>(count);
        defer {
            free(order);
        };
        job->page = PdfMoveSelectedPages(count, job->selected.LendData(), job->selected.Size(), at, order);
        job->order.Reset();
        bool unchanged = true;
        for (int i = 0; i < count; i++) {
            job->order.Append(order[i]);
            if (order[i] != i + 1) unchanged = false;
        }
        if (unchanged) {
            delete job;
            return;
        }
    }
    if (action == ThumbPasteBefore || action == ThumbPasteAfter) {
        job->clipboard = ReadPageClipboard(t->hwnd);
        if (!job->clipboard.size()) {
            delete job;
            return;
        }
        job->at = job->page - (action == ThumbPasteBefore ? 1 : 0);
        job->page = job->at + 1;
    }
    if (action == ThumbInsertBefore || action == ThumbInsertAfter || action == ThumbExtract) {
        job->filePath = ChoosePageFile(t->win->hwndFrame, action == ThumbExtract);
        if (!job->filePath) {
            delete job;
            return;
        }
        if (action == ThumbExtract && path::IsSame(job->filePath, engine->FilePath())) {
            MessageBoxWarning(t->hwnd, _TRA("Choose a different file for extracted pages."), _TRA("Pages"));
            delete job;
            return;
        }
    }
    if (action == ThumbPasteImage) {
        job->clipboard = ReadClipboardImage(t->hwnd);
        if (!job->clipboard.size()) {
            delete job;
            return;
        }
    }
    if (action == ThumbInsertBefore || action == ThumbInsertAfter || action == ThumbPasteImage) {
        job->at = job->page - (action == ThumbInsertBefore ? 1 : 0);
        job->page = job->at + 1;
    }
    if (!IsMainWindowValid(job->win) || !IsWindow(job->hwnd) || job->win->CurrentTab() != job->tab ||
        job->tab->AsFixed() != job->dm) {
        delete job;
        return;
    }
    SendMessageW(t->win->hwndFrame, WM_COMMAND, CmdStopReadAloud, 0);
    DeleteSelectionToolbar(t->win);
    DeleteOldSelectionInfo(t->win, true);
    ClearPdfDeletedAnnotCovers(tab);
    t->win->annotationUnderCursor = nullptr;
    t->win->linkOnLastButtonDown = nullptr;
    t->win->imageDragElement = nullptr;
    EndFreeTextInPlaceEditForTab(tab, true);
    CloseAndDeleteEditAnnotationsWindow(tab);
    tab->selectedAnnotation = nullptr;
    ClearPdfMarkupOverlays(tab);
    t->win->DeleteToolTip();
    t->busy = true;
    t->generation++;
    job->paused = job->dm->pauseRendering;
    job->dm->pauseRendering = true;
    job->ownerEnabled = IsWindowEnabled(job->owner);
    EnableWindow(job->owner, FALSE);
    EnableWindow(t->hwnd, FALSE);
    if (action != ThumbCopy && action != ThumbCut && action != ThumbExtract) {
        EngineMupdfInvalidatePageStructurePreview(engine);
    }
    gRenderCache->CancelRendering(job->dm);
    RunAsync(MkFunc0<ThumbEditJob>(ThumbEditWorker, job), "ThumbEdit");
}

static void ThumbContextMenu(SidebarThumbsWnd* t, LPARAM lp) {
    if (t->busy || !t->win->AsFixed()) return;
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (pt.x == -1 && pt.y == -1) {
        pt = {DpiScale(t->hwnd, 20), DpiScale(t->hwnd, 20)};
        ClientToScreen(t->hwnd, &pt);
    } else {
        POINT local = pt;
        ScreenToClient(t->hwnd, &local);
        int page = PageAtPoint(t, Point(local.x, local.y));
        if (page > 0 && !t->selected.Contains(page)) {
            t->selected.Reset();
            t->selected.Append(page);
            t->anchor = page;
            t->win->ctrl->GoToPage(page, false);
        }
    }
    InvalidateRect(t->hwnd, nullptr, FALSE);
    HMENU menu = CreatePopupMenu();
    bool editable = EngineMupdfCanEditPdfToc(t->win->CurrentTab()->GetEngine());
    UINT enabled = MF_STRING | (editable ? 0 : MF_GRAYED);
    AppendMenuW(menu, enabled, ThumbCopy, _TRW("Copy pages\tCtrl+C"));
    AppendMenuW(menu, enabled, ThumbCut, _TRW("Cut pages\tCtrl+X"));
    bool canPaste = editable && IsClipboardFormatAvailable(PageClipboardFormat());
    AppendMenuW(menu, MF_STRING | (canPaste ? 0 : MF_GRAYED), ThumbPasteBefore, _TRW("Paste pages before"));
    AppendMenuW(menu, MF_STRING | (canPaste ? 0 : MF_GRAYED), ThumbPasteAfter, _TRW("Paste pages after\tCtrl+V"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, enabled, ThumbInsertBefore, _TRW("Insert PDF or image before…"));
    AppendMenuW(menu, enabled, ThumbInsertAfter, _TRW("Insert PDF or image after…"));
    bool canImage = editable && (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_DIBV5));
    AppendMenuW(menu, MF_STRING | (canImage ? 0 : MF_GRAYED), ThumbPasteImage, _TRW("Insert clipboard image after"));
    AppendMenuW(menu, enabled, ThumbExtract, _TRW("Extract selected pages…"));
    EngineBase* engine = t->win->CurrentTab()->GetEngine();
    AppendMenuW(menu, MF_STRING | (OcrEngineKindSupported(engine) ? 0 : MF_GRAYED), ThumbOcr,
                _TRW("Recognize selected pages"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, enabled, ThumbDelete, _TRW("Delete pages\tDelete"));
    AppendMenuW(menu, MF_STRING | (EngineMupdfCanUndoPageOrder(t->win->CurrentTab()->GetEngine()) ? 0 : MF_GRAYED),
                ThumbUndo, _TRW("Undo page edit\tCtrl+Z"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ThumbSelectAll, _TRW("Select all\tCtrl+A"));
    MarkMenuOwnerDraw(menu);
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, t->win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(menu);
    DestroyMenu(menu);
    if (command == ThumbOcr) {
        if (t->selected.IsEmpty()) {
            OcrScheduleForPage(t->win, t->win->ctrl->CurrentPageNo(), true);
        } else {
            OcrScheduleSelectedPages(t->win, t->selected);
        }
    } else if (command) {
        ThumbStartAction(t, command);
    }
}

bool SidebarThumbsHandleKey(MSG& msg) {
    if (msg.message != WM_KEYDOWN || msg.hwnd != GetFocus()) return false;
    WCHAR cls[64]{};
    GetClassNameW(msg.hwnd, cls, dimof(cls));
    if (!str::Eq(cls, kThumbsClass)) return false;
    auto* t = (SidebarThumbsWnd*)GetWindowLongPtrW(msg.hwnd, GWLP_USERDATA);
    int action = 0;
    if (IsCtrlPressed()) {
        switch (msg.wParam) {
            case 'A':
                action = ThumbSelectAll;
                break;
            case 'C':
                action = ThumbCopy;
                break;
            case 'X':
                action = ThumbCut;
                break;
            case 'V':
                action = IsClipboardFormatAvailable(PageClipboardFormat()) ? ThumbPasteAfter : ThumbPasteImage;
                break;
            case 'Z':
                action = ThumbUndo;
                break;
        }
    } else if (msg.wParam == VK_DELETE)
        action = ThumbDelete;
    if (!action) return false;
    ThumbStartAction(t, action);
    return true;
}

static LRESULT CALLBACK WndProcSidebarThumbs(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* t = (SidebarThumbsWnd*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lp;
        t = (SidebarThumbsWnd*)cs->lpCreateParams;
        t->hwnd = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)t);
    }
    if (!t) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    if (t->win &&
        (msg == WM_LBUTTONDOWN || msg == WM_NCLBUTTONDOWN || msg == WM_MOUSEMOVE || msg == WM_NCMOUSEMOVE ||
         msg == WM_SETCURSOR) &&
        HandleSidebarSplitterHit(t->win, hwnd, msg, lp)) {
        return msg == WM_SETCURSOR ? TRUE : 0;
    }
    switch (msg) {
        case WM_CONTEXTMENU:
            ThumbContextMenu(t, lp);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            LayoutMetrics(t);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Rect rc = ClientRect(hwnd);
            // Paint into a bitmap so a new thumbnail does not flash the whole grid.
            HDC mem = rc.dx > 0 && rc.dy > 0 ? CreateCompatibleDC(hdc) : nullptr;
            HBITMAP bmp = mem ? CreateCompatibleBitmap(hdc, rc.dx, rc.dy) : nullptr;
            if (mem && bmp) {
                HGDIOBJ old = SelectObject(mem, bmp);
                PaintThumbs(t, mem);
                BitBlt(hdc, 0, 0, rc.dx, rc.dy, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            } else {
                if (mem) {
                    DeleteDC(mem);
                }
                PaintThumbs(t, hdc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == kThumbRenderTimerId + 1) {
                if (!t->rendering) {
                    KillTimer(hwnd, kThumbRenderTimerId + 1);
                    int action = t->pendingAction, at = t->pendingAt;
                    t->pendingAction = 0;
                    ThumbStartAction(t, action, at);
                }
                return 0;
            }
            if (wp == kThumbRenderTimerId) {
                KillTimer(hwnd, kThumbRenderTimerId);
                RenderOneThumb(t);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            ScrollThumbsBy(t, -delta / 2);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (t->busy) return 0;
            SetFocus(hwnd);
            Point point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            int page = PageAtPoint(t, point);
            if (page > 0 && t->win->ctrl) {
                if (IsShiftPressed() && t->anchor > 0) {
                    if (!IsCtrlPressed()) t->selected.Reset();
                    int begin = ShownIndexOf(t, t->anchor), end = ShownIndexOf(t, page);
                    if (begin > end) std::swap(begin, end);
                    for (int i = std::max(0, begin); i <= end; i++) {
                        if (!t->selected.Contains(t->shownPages[i])) t->selected.Append(t->shownPages[i]);
                    }
                } else if (IsCtrlPressed()) {
                    if (t->selected.Contains(page))
                        t->selected.Remove(page);
                    else
                        t->selected.Append(page);
                    t->anchor = page;
                } else {
                    if (!t->selected.Contains(page)) {
                        t->selected.Reset();
                        t->selected.Append(page);
                    }
                    t->anchor = page;
                }
                t->dragStart = point;
                t->dragging = false;
                t->dropBefore = 0;
                SetCapture(hwnd);
                t->win->ctrl->GoToPage(page, true);
                SetFocus(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (GetCapture() != hwnd || t->busy) break;
            Point point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (!t->dragging && (abs(point.x - t->dragStart.x) > GetSystemMetrics(SM_CXDRAG) ||
                                 abs(point.y - t->dragStart.y) > GetSystemMetrics(SM_CYDRAG)))
                t->dragging = true;
            if (t->dragging) {
                int page = PageAtPoint(t, point);
                t->dropBefore = 0;
                if (page > 0) {
                    Rect rect = ThumbRect(t, ShownIndexOf(t, page));
                    t->dropBefore = page + (point.x > rect.x + rect.dx / 2 ? 1 : 0);
                }
                if (point.y < 20) ScrollThumbsBy(t, -16);
                if (point.y > ClientRect(hwnd).dy - 20) ScrollThumbsBy(t, 16);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (GetCapture() != hwnd) return 0;
            int target = t->dropBefore;
            bool move = t->dragging;
            t->dragging = false;
            ReleaseCapture();
            if (move && target > 0 && str::IsEmpty(t->appliedFind))
                ThumbStartAction(t, ThumbMove, target);
            else if (!move && !IsCtrlPressed() && !IsShiftPressed()) {
                int page = PageAtPoint(t, Point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
                t->selected.Reset();
                if (page > 0) t->selected.Append(page);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            t->dragging = false;
            t->dropBefore = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            ClearThumbSlots(t);
            if (t->win && t->win->hwndSidebarThumbs == hwnd) {
                t->win->hwndSidebarThumbs = nullptr;
            }
            delete t;
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void CreateSidebarThumbs(MainWindow* win) {
    if (!win || win->hwndSidebarThumbs) {
        return;
    }
    if (!gThumbsClassAtom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProcSidebarThumbs;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kThumbsClass;
        gThumbsClassAtom = RegisterClassExW(&wc);
    }
    auto* t = new SidebarThumbsWnd();
    t->win = win;
    t->hwnd = CreateWindowExW(0, kThumbsClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, win->hwndTocBox, nullptr,
                              GetModuleHandle(nullptr), t);
    win->hwndSidebarThumbs = t->hwnd;
}

void DestroySidebarThumbs(MainWindow* win) {
    if (!win || !win->hwndSidebarThumbs) {
        return;
    }
    HWND hwnd = win->hwndSidebarThumbs;
    win->hwndSidebarThumbs = nullptr;
    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
}

void SidebarThumbsOnThemeChanged(MainWindow* win) {
    SidebarThumbsWnd* t = ThumbsFromWin(win);
    if (!t) {
        return;
    }
    ClearThumbSlots(t);
    t->themeHash = 0;
    InvalidateRect(t->hwnd, nullptr, FALSE);
}

void SidebarThumbsSyncCurrentPage(MainWindow* win) {
    SidebarThumbsWnd* t = ThumbsFromWin(win);
    if (!t || !IsWindowVisible(t->hwnd) || !win->ctrl) {
        return;
    }
    if (win->ctrl != t->boundCtrl) {
        // Slots are keyed by page number and theme, so none can survive a document switch.
        ClearThumbSlots(t);
        t->boundCtrl = win->ctrl;
        t->selected.Reset();
        t->anchor = 0;
        t->generation++;
        t->pendingAction = 0;
        t->followedPage = 0;
        t->scrollY = 0;
    }
    if (t->shownDirty || win->ctrl->PageCount() != t->pageCount) {
        RebuildShownPages(t);
    }
    LayoutMetrics(t);
    int pageNo = win->ctrl->CurrentPageNo();
    if (pageNo == t->followedPage) {
        return;
    }
    if (GetFocus() != t->hwnd && GetCapture() != t->hwnd) {
        t->selected.Reset();
        t->selected.Append(pageNo);
        t->anchor = pageNo;
    }
    t->followedPage = pageNo;
    int idx = -1;
    for (int i = 0; i < t->shownPages.Size(); i++) {
        if (t->shownPages[i] == pageNo) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    Rect r = ThumbRect(t, idx);
    Rect rc = ClientRect(t->hwnd);
    if (r.y < 0) {
        t->scrollY += r.y - DpiScale(t->hwnd, 8);
    } else if (r.y + r.dy > rc.dy) {
        t->scrollY += (r.y + r.dy) - rc.dy + DpiScale(t->hwnd, 8);
    }
    if (t->scrollY < 0) {
        t->scrollY = 0;
    }
    InvalidateRect(t->hwnd, nullptr, FALSE);
}

// The header icons switch the column. Closing it is the X button.
void ShowSidebarPage(MainWindow* win, SidebarView view) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    if (!SidebarViewAvailable(win, view)) {
        if (view == SidebarView::Bookmarks && SidebarViewAvailable(win, SidebarView::Thumbnails)) {
            view = SidebarView::Thumbnails;
        } else {
            return;
        }
    }
    if (win->tocVisible && CurrentSidebarView(win) == view) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (tab) {
        tab->sidebarView = (int)view;
        RememberTabSidebarView(tab);
    }
    bool asToc = view != SidebarView::Favorites;
    SetSidebarVisibility(win, asToc, view == SidebarView::Favorites);
    if (!win->tocVisible) {
        return;
    }
    if (view == SidebarView::Bookmarks && win->tocTreeView) {
        HwndSetFocus(win->tocTreeView->hwnd);
    } else if (view == SidebarView::Thumbnails && win->hwndSidebarThumbs) {
        HwndSetFocus(win->hwndSidebarThumbs);
    } else if (view == SidebarView::Favorites && win->favTreeView) {
        HwndSetFocus(win->favTreeView->hwnd);
        // Focusing an empty tree selects the first row. That is not a choice.
        TreeView_SelectItem(win->favTreeView->hwnd, nullptr);
    }
}

void SidebarThumbsApplyFind(MainWindow* win, const char* query) {
    SidebarThumbsWnd* t = ThumbsFromWin(win);
    if (!t) {
        return;
    }
    const char* prev = t->appliedFind.Get();
    if (!prev) {
        prev = "";
    }
    const char* q = query ? query : "";
    if (!str::Eq(prev, q)) {
        t->appliedFind.SetCopy(q);
        t->scrollY = 0;
    }
    t->shownDirty = true;
    RebuildShownPages(t);
    if (t->hwnd) {
        InvalidateRect(t->hwnd, nullptr, FALSE);
    }
}

// Bookmarks and thumbnails are two ways to show one title search.
static int SidebarFindSlot(SidebarView view) {
    return view == SidebarView::Favorites ? 2 : 0;
}

static const char* SidebarFindCue(SidebarView view) {
    if (view == SidebarView::Favorites) {
        return _TRA("Search Favorites");
    }
    return _TRA("Search Bookmarks");
}

void ApplySidebarFindEdit(MainWindow* win) {
    if (!win || !win->tocFilterEdit) {
        return;
    }
    SidebarView view = CurrentSidebarView(win);
    const char* query = win->tocFilterEdit->GetTextTemp();
    if (!query) {
        query = "";
    }
    int slot = SidebarFindSlot(view);
    const char* prev = win->sidebarFindText[slot].Get();
    if (!prev) {
        prev = "";
    }
    bool changed = !str::Eq(prev, query);
    win->sidebarFindText[slot].SetCopy(query);
    if (view == SidebarView::Favorites) {
        ApplyFavoritesFind(win, query);
        return;
    }
    if (changed) {
        TocFilterChanged(win);
    }
    SidebarThumbsApplyFind(win, query);
}

void BindSidebarFindEdit(MainWindow* win, SidebarView view) {
    if (!win || !win->tocFilterEdit) {
        return;
    }
    // The AI and annotations pages have no title filter. Leave the bookmark query stored.
    if (view == SidebarView::Ai || view == SidebarView::Annotations) {
        win->sidebarFindBound = (int)view;
        return;
    }
    Edit* edit = win->tocFilterEdit;
    int next = (int)view;
    if (next < 0 || next > 2) {
        next = 0;
    }
    if (win->sidebarFindBound == next) {
        edit->SetCueText(SidebarFindCue(view));
        return;
    }
    int prev = win->sidebarFindBound;
    int prevSlot = -1;
    if (prev >= 0 && prev <= 2) {
        prevSlot = SidebarFindSlot((SidebarView)prev);
        const char* cur = edit->GetTextTemp();
        win->sidebarFindText[prevSlot].SetCopy(cur ? cur : "");
    }
    win->sidebarFindBound = next;
    edit->SetCueText(SidebarFindCue(view));
    int nextSlot = SidebarFindSlot(view);
    if (prevSlot == nextSlot) {
        const char* cur = edit->GetTextTemp();
        SidebarThumbsApplyFind(win, cur);
        return;
    }
    const char* saved = win->sidebarFindText[nextSlot].Get();
    if (!saved) {
        saved = "";
    }
    const char* cur = edit->GetTextTemp();
    if (!str::Eq(cur, saved)) {
        edit->SetText(saved);
    } else {
        ApplySidebarFindEdit(win);
    }
}

static void OnSidebarBookmarks(MainWindow* win) {
    ShowSidebarPage(win, SidebarView::Bookmarks);
}

static void OnSidebarThumbnails(MainWindow* win) {
    ShowSidebarPage(win, SidebarView::Thumbnails);
}

static void OnSidebarFavorites(MainWindow* win) {
    ShowSidebarPage(win, SidebarView::Favorites);
}

static void OnSidebarAi(MainWindow* win) {
    ShowSidebarPage(win, SidebarView::Ai);
}

static void OnFavAddCurrent(MainWindow* win) {
    AddFavoriteForCurrentPage(win);
}

static void OnFavRemoveCurrent(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    // A highlighted row is that entry. With none highlighted, remove this page.
    if (CanDeleteSelectedFavorite(win)) {
        DeleteSelectedFavorite(win);
        return;
    }
    DelFavorite(win->ctrl->GetFilePath(), win->currPageNo);
}

static void OnSidebarAnnotations(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab) {
        return;
    }
    if (EbookAnnotationsSupported(tab)) {
        ShowEditEbookAnnotationsWindow(tab, nullptr);
        return;
    }
    ShowEditAnnotationsWindow(tab, nullptr);
}

void UpdateSidebarViewButtons(MainWindow* win) {
    if (!win || !win->tocLabelWithClose) {
        return;
    }
    SidebarView cur = CurrentSidebarView(win);
    bool shown = win->tocVisible;
    LabelViewButton btns[5];
    btns[0].icon = TbIcon::SidebarBookmarks;
    btns[0].tooltip = _TRN("Bookmarks");
    btns[0].selected = shown && cur == SidebarView::Bookmarks;
    btns[0].enabled = SidebarViewAvailable(win, SidebarView::Bookmarks);
    btns[0].onClick = MkFunc0(OnSidebarBookmarks, win);
    btns[1].icon = TbIcon::HomeThumbnails;
    btns[1].tooltip = _TRN("Thumbnails");
    btns[1].selected = shown && cur == SidebarView::Thumbnails;
    btns[1].enabled = SidebarViewAvailable(win, SidebarView::Thumbnails);
    btns[1].onClick = MkFunc0(OnSidebarThumbnails, win);
    btns[2].icon = TbIcon::SidebarFavorites;
    btns[2].tooltip = _TRN("Favorites");
    btns[2].selected = shown && cur == SidebarView::Favorites;
    btns[2].enabled = SidebarViewAvailable(win, SidebarView::Favorites);
    btns[2].onClick = MkFunc0(OnSidebarFavorites, win);
    btns[3].icon = TbIcon::SidebarAnnotations;
    btns[3].tooltip = _TRN("Annotations");
    btns[3].selected = shown && cur == SidebarView::Annotations;
    btns[3].enabled = SidebarViewAvailable(win, SidebarView::Annotations);
    btns[3].onClick = MkFunc0(OnSidebarAnnotations, win);
    int nBtns = 4;
    if (gGlobalPrefs && gGlobalPrefs->enableAskAI) {
        btns[4].icon = TbIcon::SidebarAi;
        btns[4].tooltip = _TRN("Ask AI");
        btns[4].selected = shown && cur == SidebarView::Ai;
        btns[4].enabled = SidebarViewAvailable(win, SidebarView::Ai);
        btns[4].onClick = MkFunc0(OnSidebarAi, win);
        nBtns = 5;
    }
    win->tocLabelWithClose->SetViewButtons(btns, nBtns);
    win->tocLabelWithClose->SetHeaderActionsVisible(shown && cur == SidebarView::Bookmarks);
    if (shown && cur == SidebarView::Favorites) {
        LabelViewButton right[2];
        right[0].icon = TbIcon::FavAdd;
        right[0].tooltip = _TRN("Add current page to favorites");
        right[0].enabled = win->IsDocLoaded();
        right[0].chromeWell = false;
        right[0].onClick = MkFunc0(OnFavAddCurrent, win);
        right[1].icon = TbIcon::FavRemove;
        right[1].tooltip = _TRN("Remove from favorites");
        right[1].enabled = win->IsDocLoaded();
        right[1].chromeWell = false;
        right[1].onClick = MkFunc0(OnFavRemoveCurrent, win);
        win->tocLabelWithClose->SetRightButtons(right, 2);
    } else {
        win->tocLabelWithClose->SetRightButtons(nullptr, 0);
    }
}

void ApplySidebarViewLayout(MainWindow* win) {
    if (!win || !win->hwndTocBox) {
        return;
    }
    CreateSidebarThumbs(win);
    SidebarView view = FallbackSidebarView(win, CurrentSidebarView(win));
    WindowTab* tab = win->CurrentTab();
    if (tab) {
        tab->sidebarView = (int)view;
    }
    bool bookmarks = win->tocVisible && view == SidebarView::Bookmarks;
    bool thumbs = win->tocVisible && view == SidebarView::Thumbnails;
    bool favs = win->tocVisible && view == SidebarView::Favorites;
    bool ai = win->tocVisible && view == SidebarView::Ai;
    bool annots = win->tocVisible && view == SidebarView::Annotations;
    if (ai) {
        EnsureAskAiSidebar(win);
    }
    // Show* lays the sidebar out again. The flag stops that from creating a second editor.
    static bool creatingAnnotSidebar = false;
    if (annots && tab && !creatingAnnotSidebar && !EditAnnotationsSidebarHwnd(tab) &&
        !EbookAnnotationsSidebarHwnd(tab)) {
        creatingAnnotSidebar = true;
        if (EbookAnnotationsSupported(tab)) {
            ShowEditEbookAnnotationsWindow(tab, nullptr, EditAnnotFocus::Default, false);
        } else if (EngineSupportsAnnotations(tab->GetEngine())) {
            ShowEditAnnotationsWindow(tab, nullptr, EditAnnotFocus::Default, false);
        }
        creatingAnnotSidebar = false;
    }
    SyncEditAnnotationsSidebar(win, annots);
    SyncEbookAnnotationsSidebar(win, annots);

    if (win->tocFilterEdit && win->tocFilterEdit->hwnd) {
        ShowWindow(win->tocFilterEdit->hwnd, (bookmarks || thumbs || favs) ? SW_SHOW : SW_HIDE);
    }
    BindSidebarFindEdit(win, view);
    if (win->tocTreeView && win->tocTreeView->hwnd) {
        ShowWindow(win->tocTreeView->hwnd, bookmarks ? SW_SHOW : SW_HIDE);
    }
    if (win->hwndSidebarThumbs) {
        ShowWindow(win->hwndSidebarThumbs, thumbs ? SW_SHOW : SW_HIDE);
    }
    if (win->hwndAiSidebar) {
        ShowWindow(win->hwndAiSidebar, ai ? SW_SHOW : SW_HIDE);
    }
    if (win->favTreeView && win->favTreeView->hwnd) {
        HWND parent = favs ? win->hwndTocBox : win->hwndFavBox;
        if (parent && GetParent(win->favTreeView->hwnd) != parent) {
            SetParent(win->favTreeView->hwnd, parent);
        }
        ShowWindow(win->favTreeView->hwnd, favs ? SW_SHOW : SW_HIDE);
    }
    if (favs) {
        PopulateFavTreeIfNeeded(win);
    }
    UpdateSidebarViewButtons(win);
    RelayoutTocContainer(win);
    if (ai) {
        LayoutAskAiSidebar(win);
    }
    if (ai) {
        LayoutAskAiSidebar(win);
    }
    if (thumbs && win->hwndSidebarThumbs) {
        InvalidateRect(win->hwndSidebarThumbs, nullptr, TRUE);
        SidebarThumbsSyncCurrentPage(win);
    }
    if (favs && win->favTreeView && win->favTreeView->hwnd) {
        InvalidateRect(win->favTreeView->hwnd, nullptr, TRUE);
    }
}

void ShowOrToggleSidebarView(MainWindow* win, SidebarView view) {
    if (!win || !win->IsDocLoaded()) {
        return;
    }
    if (!SidebarViewAvailable(win, view)) {
        if (view == SidebarView::Bookmarks && SidebarViewAvailable(win, SidebarView::Thumbnails)) {
            view = SidebarView::Thumbnails;
        } else {
            return;
        }
    }
    bool showing = win->tocVisible && CurrentSidebarView(win) == view;
    WindowTab* tab = win->CurrentTab();
    if (showing) {
        SetSidebarVisibility(win, false, false);
        return;
    }
    if (tab) {
        tab->sidebarView = (int)view;
        RememberTabSidebarView(tab);
    }
    bool asToc = view != SidebarView::Favorites;
    SetSidebarVisibility(win, asToc, view == SidebarView::Favorites);
    if (!win->tocVisible) {
        return;
    }
    if (view == SidebarView::Bookmarks && win->tocTreeView) {
        HwndSetFocus(win->tocTreeView->hwnd);
    } else if (view == SidebarView::Thumbnails && win->hwndSidebarThumbs) {
        HwndSetFocus(win->hwndSidebarThumbs);
    } else if (view == SidebarView::Favorites && win->favTreeView) {
        HwndSetFocus(win->favTreeView->hwnd);
        // Focusing an empty tree selects the first row. That is not a choice.
        TreeView_SelectItem(win->favTreeView->hwnd, nullptr);
    }
}
