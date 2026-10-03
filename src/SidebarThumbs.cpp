/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"

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
    return SidebarView::Bookmarks;
}

const char* SidebarViewToStr(SidebarView view) {
    switch (view) {
        case SidebarView::Thumbnails:
            return "thumbnails";
        case SidebarView::Favorites:
            return "favorites";
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
    if (v < 0 || v > 2) {
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
    if (job->bmp && job->themeHash == t->themeHash) {
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
    if (t->rendering) {
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
        HPEN use = pageNo == current ? (HPEN)penCur : (HPEN)pen;
        HGDIOBJ old = SelectObject(hdc, use);
        HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(hdc, r.x, r.y, r.x + r.dx, r.y + r.dy);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, old);
        DrawThumbPageLabel(hdc, t->hwnd, r, pageNo, font);
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
            SetFocus(hwnd);
            int pageNo = PageAtPoint(t, Point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            if (pageNo > 0 && t->win && t->win->ctrl && t->win->ctrl->ValidPageNo(pageNo)) {
                t->win->ctrl->GoToPage(pageNo, true);
                t->win->Focus();
            }
            return 0;
        }
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
        t->boundCtrl = win->ctrl;
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

// The three header icons switch the column. Closing it is the X button.
static void ShowSidebarView(MainWindow* win, SidebarView view) {
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
    ShowSidebarView(win, SidebarView::Bookmarks);
}

static void OnSidebarThumbnails(MainWindow* win) {
    ShowSidebarView(win, SidebarView::Thumbnails);
}

static void OnSidebarFavorites(MainWindow* win) {
    ShowSidebarView(win, SidebarView::Favorites);
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

void UpdateSidebarViewButtons(MainWindow* win) {
    if (!win || !win->tocLabelWithClose) {
        return;
    }
    SidebarView cur = CurrentSidebarView(win);
    bool shown = win->tocVisible;
    LabelViewButton btns[3];
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
    win->tocLabelWithClose->SetViewButtons(btns, 3);
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
