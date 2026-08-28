/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/BitManip.h"
#include "utils/WinDynCalls.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/Timer.h"
#include "utils/UITask.h"
#include "utils/WinUtil.h"
#include "utils/ScopedWin.h"
#include "utils/ThreadUtil.h"
#include "utils/HttpUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/GuessFileType.h"
#include <algorithm>
#include <shlobj.h>

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "wingui/FrameRateWnd.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "Annotation.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"

#include "DisplayModel.h"
#include "Theme.h"
#include "PdfDarkMode.h"
#include "GlobalPrefs.h"
#include "RenderCache.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "SumatraConfig.h"
#include "WindowTab.h"
#include "SumatraPDF.h"
#include "EditAnnotations.h"
#include "EbookAnnotations.h"
#include "EditEbookAnnotations.h"
#include "Notifications.h"
#include "OverlayScrollbar.h"
#include "MainWindow.h"
#include "resource.h"
#include "Commands.h"
#include "Canvas.h"
#include "Menu.h"
#include "uia/Provider.h"
#include "SearchAndDDE.h"
#include "Selection.h"
#include "SelectionToolbar.h"
#include "ReadAloudHighlight.h"
#include "TextToSpeech.h"
#include "SelectionToolbar.h"
#include "WordLookup.h"
#include "HomePage.h"
#include "Tabs.h"
#include "Toolbar.h"
#include "Translations.h"
#include "OcrService.h"

#include "utils/Log.h"

// if set instead of trying to render pages we don't have, we simply do nothing
// this reduces the flickering when going quickly through pages but creates
// impression of lag
bool gNoFlickerRender = true;

Kind kNotifAnnotation = "notifAnnotation";

// OLE drag-drop support for dragging selected text out of the window
class TextDropSource : public IDropSource {
    LONG refCount = 1;

  public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refCount);
        if (r == 0) {
            delete this;
        }
        return r;
    }
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed) {
            return DRAGDROP_S_CANCEL;
        }
        if (!(grfKeyState & MK_LBUTTON)) {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(__unused DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

class TextDataObject : public IDataObject {
    LONG refCount = 1;
    HGLOBAL hText = nullptr;

  public:
    explicit TextDataObject(const WCHAR* text) {
        size_t cb = (str::Len(text) + 1) * sizeof(WCHAR);
        hText = GlobalAlloc(GMEM_MOVEABLE, cb);
        if (hText) {
            void* p = GlobalLock(hText);
            memcpy(p, text, cb);
            GlobalUnlock(hText);
        }
    }
    ~TextDataObject() {
        if (hText) {
            GlobalFree(hText);
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refCount);
        if (r == 0) {
            delete this;
        }
        return r;
    }

    STDMETHODIMP GetData(FORMATETC* pFE, STGMEDIUM* pMedium) override {
        if (!hText) {
            return E_UNEXPECTED;
        }
        if (pFE->cfFormat != CF_UNICODETEXT || !(pFE->tymed & TYMED_HGLOBAL)) {
            return DV_E_FORMATETC;
        }
        size_t cb = GlobalSize(hText);
        HGLOBAL hCopy = GlobalAlloc(GMEM_MOVEABLE, cb);
        if (!hCopy) {
            return E_OUTOFMEMORY;
        }
        void* src = GlobalLock(hText);
        void* dst = GlobalLock(hCopy);
        memcpy(dst, src, cb);
        GlobalUnlock(hCopy);
        GlobalUnlock(hText);
        pMedium->tymed = TYMED_HGLOBAL;
        pMedium->hGlobal = hCopy;
        pMedium->pUnkForRelease = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(__unused FORMATETC*, __unused STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* pFE) override {
        if (pFE->cfFormat == CF_UNICODETEXT && (pFE->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(__unused FORMATETC*, FORMATETC* pOut) override {
        pOut->ptd = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP SetData(__unused FORMATETC*, __unused STGMEDIUM*, __unused BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(__unused DWORD, __unused IEnumFORMATETC**) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(__unused FORMATETC*, __unused DWORD, __unused IAdviseSink*, __unused DWORD*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP DUnadvise(__unused DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDAdvise(__unused IEnumSTATDATA**) override { return E_NOTIMPL; }
};

static bool IsPointInSelection(MainWindow* win, Point pt) {
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->selectionOnPage) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    for (SelectionOnPage& sel : *tab->selectionOnPage) {
        Rect r = sel.GetRect(dm);
        if (r.Contains(pt)) {
            return true;
        }
    }
    return false;
}

static void StartTextDragDrop(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    bool isTextOnly = false;
    TempStr text = GetSelectedTextTemp(tab, "\r\n", isTextOnly);
    if (str::IsEmpty(text)) {
        return;
    }
    WCHAR* wtext = ToWStrTemp(text);
    TextDataObject* dataObj = new TextDataObject(wtext);
    TextDropSource* dropSrc = new TextDropSource();
    DWORD dwEffect = 0;
    DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY, &dwEffect);
    dropSrc->Release();
    dataObj->Release();
}

// encode HBITMAP to PNG in memory using GDI+ IStream
static HGLOBAL EncodeBitmapToPngGlobal(HBITMAP hbmp) {
    Gdiplus::Bitmap gdipBmp(hbmp, nullptr);
    if (gdipBmp.GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    CLSID pngClsid = GetGdiPlusEncoderClsid(L"image/png");
    IStream* stream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, FALSE, &stream);
    if (FAILED(hr) || !stream) {
        return nullptr;
    }
    Gdiplus::Status status = gdipBmp.Save(stream, &pngClsid, nullptr);
    HGLOBAL hMem = nullptr;
    if (status == Gdiplus::Ok) {
        GetHGlobalFromStream(stream, &hMem);
    }
    stream->Release();
    if (status != Gdiplus::Ok) {
        return nullptr;
    }
    return hMem;
}

// IDataObject that provides an image as a virtual file (CFSTR_FILEDESCRIPTOR + CFSTR_FILECONTENTS)
// and CF_DIB, without creating any temporary files on disk.
// Browsers and web apps accept virtual file drops just like real file drops.
class ImageDataObject : public IDataObject {
    LONG refCount = 1;
    HGLOBAL hPngData = nullptr; // PNG-encoded image data
    size_t pngSize = 0;
    UINT cfFileDescriptor = 0;
    UINT cfFileContents = 0;

  public:
    explicit ImageDataObject(HGLOBAL hPng) {
        hPngData = hPng;
        pngSize = GlobalSize(hPng);
        cfFileDescriptor = RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
        cfFileContents = RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    }
    ~ImageDataObject() {
        if (hPngData) {
            GlobalFree(hPngData);
        }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refCount);
        if (r == 0) {
            delete this;
        }
        return r;
    }

    STDMETHODIMP GetData(FORMATETC* pFE, STGMEDIUM* pMedium) override {
        if (!hPngData) {
            return E_UNEXPECTED;
        }

        // CFSTR_FILEDESCRIPTORW: describe one virtual file "image.png"
        if (pFE->cfFormat == cfFileDescriptor && (pFE->tymed & TYMED_HGLOBAL)) {
            size_t cb = sizeof(FILEGROUPDESCRIPTORW);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, cb);
            if (!h) {
                return E_OUTOFMEMORY;
            }
            auto* fgd = (FILEGROUPDESCRIPTORW*)GlobalLock(h);
            fgd->cItems = 1;
            fgd->fgd[0].dwFlags = FD_FILESIZE;
            fgd->fgd[0].nFileSizeLow = (DWORD)pngSize;
            fgd->fgd[0].nFileSizeHigh = 0;
            wcscpy_s(fgd->fgd[0].cFileName, MAX_PATH, L"image.png");
            GlobalUnlock(h);
            pMedium->tymed = TYMED_HGLOBAL;
            pMedium->hGlobal = h;
            pMedium->pUnkForRelease = nullptr;
            return S_OK;
        }

        // CFSTR_FILECONTENTS: provide the PNG data as an IStream
        if (pFE->cfFormat == cfFileContents && (pFE->tymed & TYMED_ISTREAM) && pFE->lindex == 0) {
            IStream* stream = nullptr;
            HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            if (FAILED(hr) || !stream) {
                return E_OUTOFMEMORY;
            }
            void* src = GlobalLock(hPngData);
            ULONG written = 0;
            stream->Write(src, (ULONG)pngSize, &written);
            GlobalUnlock(hPngData);
            // reset stream position to beginning
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            pMedium->tymed = TYMED_ISTREAM;
            pMedium->pstm = stream;
            pMedium->pUnkForRelease = nullptr;
            return S_OK;
        }

        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetDataHere(__unused FORMATETC*, __unused STGMEDIUM*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* pFE) override {
        if (pFE->cfFormat == cfFileDescriptor && (pFE->tymed & TYMED_HGLOBAL)) {
            return S_OK;
        }
        if (pFE->cfFormat == cfFileContents && (pFE->tymed & TYMED_ISTREAM) && pFE->lindex == 0) {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(__unused FORMATETC*, FORMATETC* pOut) override {
        pOut->ptd = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP SetData(__unused FORMATETC*, __unused STGMEDIUM*, __unused BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(__unused DWORD, __unused IEnumFORMATETC**) override { return E_NOTIMPL; }
    STDMETHODIMP DAdvise(__unused FORMATETC*, __unused DWORD, __unused IAdviseSink*, __unused DWORD*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP DUnadvise(__unused DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP EnumDAdvise(__unused IEnumSTATDATA**) override { return E_NOTIMPL; }
};

static void StartImageDragDrop(MainWindow* win) {
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    IPageElement* el = win->imageDragElement;
    if (!el) {
        return;
    }
    RenderedBitmap* bmp = dm->GetEngine()->GetImageForPageElement(el);
    if (!bmp) {
        return;
    }
    HGLOBAL hPng = EncodeBitmapToPngGlobal(bmp->GetBitmap());
    delete bmp;
    if (!hPng) {
        return;
    }
    ImageDataObject* dataObj = new ImageDataObject(hPng);
    TextDropSource* dropSrc = new TextDropSource();
    DWORD dwEffect = 0;
    DoDragDrop(dataObj, dropSrc, DROPEFFECT_COPY, &dwEffect);
    dropSrc->Release();
    dataObj->Release();
}

// Resize handle positions that used in resizing annotations
enum class ResizeHandle {
    None = 0,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left
};

// Size of resize handle hit area (in pixels)
constexpr int kResizeHandleSize = 8;

// Smooth scrolling factor. This is a value between 0 and 1.
// Each step, we scroll the needed delta times this factor.
// Therefore, a higher factor makes smooth scrolling faster.
static const double gSmoothScrollingFactor = 0.2;
// Read-aloud follow uses a slower factor so the view eases instead of snapping.
static const double kReadAloudSmoothScrollFactor = 0.2;

// these can be global, as the mouse wheel can't affect more than one window at once
static int gDeltaPerLine = 0;
// set when WM_MOUSEWHEEL has been passed on (to prevent recursion)
static bool gWheelMsgRedirect = false;

void UpdateDeltaPerLine() {
    ULONG ulScrollLines;
    BOOL ok = SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &ulScrollLines, 0);
    if (!ok) {
        return;
    }
    // ulScrollLines usually equals 3 or 0 (for no scrolling) or -1 (for page scrolling)
    // WHEEL_DELTA equals 120, so gDeltaPerLine will be 40
    gDeltaPerLine = 0;
    if (ulScrollLines == (ULONG)-1) {
        gDeltaPerLine = -1;
    } else if (ulScrollLines != 0) {
        gDeltaPerLine = WHEEL_DELTA / ulScrollLines;
    }
    // logf("SPI_GETWHEELSCROLLLINES: ulScrollLines=%d, gDeltaPerLine=%d\n", (int)ulScrollLines, gDeltaPerLine);
}

///// methods needed for FixedPageUI canvases with document loaded /////

const char* scrollMsgStr(USHORT msg) {
    switch (msg) {
        case SB_LINEDOWN:
            return "SB_LINEDOWN";
        case SB_LINEUP:
            return "SB_LINEUP";
        case SB_HALF_PAGEDOWN:
            return "SB_HALF_PAGEDOWN";
        case SB_HALF_PAGEUP:
            return "SB_HALF_PAGEUP";
        case SB_PAGEDOWN:
            return "SB_PAGEDOWN";
        case SB_PAGEUP:
            return "SB_PAGEUP";
    }
    return str::FormatTemp("%d", (int)msg);
}

static void OnVScroll(MainWindow* win, WPARAM wp) {
    ReportIf(!win->AsFixed());

    bool useOverlay = ScrollbarsUseOverlay() && IsOverlayScrollbarVisible(win->overlayScrollV);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (useOverlay) {
        OverlayScrollbarGetInfo(win->overlayScrollV, &si);
    } else {
        GetScrollInfo(win->hwndCanvas, SB_VERT, &si);
    }

    USHORT msg = LOWORD(wp);
    auto* ctrl = win->ctrl;
    bool dmIsSinglePage = (ctrl->GetDisplayMode() == DisplayMode::SinglePage);
    // scrollbarInSinglePage is false by default
    // if true, we show scrollbar in single page mode and make its position correspond to page number, so user can
    // scroll through pages using scrollbar even in single page mode
    bool singlePageWithScrollbar = gGlobalPrefs->scrollbarInSinglePage && dmIsSinglePage;

    int lineHeight = DpiScale(win->hwndCanvas, 16);
    bool isFitPage = (kZoomFitPage == ctrl->GetZoomVirtual());
    if (!IsContinuous(ctrl->GetDisplayMode()) && isFitPage) {
        lineHeight = 1;
    }
    // logf("OnVscroll: msg=%s, min: %d, max: %d, nPage: %d, pos: %d, fit page: %d, lineHeight: %d,
    // singlePageWithScrollbar: %d\n", scrollMsgStr(msg), si.nMin,
    //      si.nMax, si.nPage, si.nPos, isFitPage ? 1 : 0, lineHeight, singlePageWithScrollbar);

    if (singlePageWithScrollbar) {
        // In SinglePage mode, scrollbar position directly corresponds to page number
        int targetPage = ctrl->CurrentPageNo();

        switch (msg) {
            case SB_TOP:
                targetPage = 1;
                break;
            case SB_BOTTOM:
                targetPage = ctrl->PageCount();
                break;
            case SB_LINEUP:
                targetPage = std::max(1, targetPage - 1);
                break;
            case SB_LINEDOWN:
                targetPage = std::min(ctrl->PageCount(), targetPage + 1);
                break;
            case SB_HALF_PAGEUP:
                targetPage = std::max(1, targetPage - 1);
                break;
            case SB_HALF_PAGEDOWN:
                targetPage = std::min(ctrl->PageCount(), targetPage + 1);
                break;
            case SB_PAGEUP:
                targetPage = std::max(1, targetPage - 1);
                break;
            case SB_PAGEDOWN:
                targetPage = std::min(ctrl->PageCount(), targetPage + 1);
                break;
            case SB_THUMBTRACK:
                targetPage = si.nTrackPos + 1;
                break;
        }

        // Navigate to the target page
        if (targetPage != ctrl->CurrentPageNo()) {
            ctrl->GoToPage(targetPage, true);
        }
        return;
    }

    // Original logic for other display modes

    int currPos = si.nPos;
    int halfPage = si.nPage / 2;
    switch (msg) {
        case SB_TOP:
            si.nPos = si.nMin;
            break;
        case SB_BOTTOM:
            si.nPos = si.nMax;

            break;
        case SB_LINEUP:
            si.nPos -= lineHeight;
            break;
        case SB_LINEDOWN:
            si.nPos += lineHeight;
            break;
        case SB_HALF_PAGEUP:
            si.nPos -= halfPage;
            break;
        case SB_HALF_PAGEDOWN:
            si.nPos += halfPage;
            break;
        case SB_PAGEUP:
            si.nPos -= si.nPage;
            break;
        case SB_PAGEDOWN:
            si.nPos += si.nPage;
            break;
        case SB_THUMBTRACK:
            si.nPos = si.nTrackPos;
            break;
    }
    // logf("OnVScroll: nPos: %d\n", si.nPos);

    // Set the position and then retrieve it.  Due to adjustments
    // by Windows it may not be the same as the value set.
    si.fMask = SIF_POS;
    bool showScrollbar = !ScrollbarsAreHidden();
    BOOL showWinScrollbar = showScrollbar && !useOverlay;
    BOOL showOverScrollbar = showScrollbar && useOverlay;
    SetScrollInfo(win->hwndCanvas, SB_VERT, &si, showWinScrollbar);
    GetScrollInfo(win->hwndCanvas, SB_VERT, &si);
    OverlayScrollbarSetInfo(win->overlayScrollV, &si, showOverScrollbar);

    // If the position has changed or we're dealing with a touchpad scroll event,
    // scroll the window and update it
    if (si.nPos != currPos || msg == SB_THUMBTRACK) {
        if (gGlobalPrefs->smoothScroll) {
            win->scrollTargetY = si.nPos;
            SetTimer(win->hwndCanvas, kSmoothScrollTimerID, USER_TIMER_MINIMUM, nullptr);
        } else {
            win->AsFixed()->ScrollYTo(si.nPos);
            ReadAloudOnUserViewChanged(win);
        }
    }
}

static void OnHScroll(MainWindow* win, WPARAM wp) {
    ReportIf(!win->AsFixed());

    bool useOverlay = ScrollbarsUseOverlay() && IsOverlayScrollbarVisible(win->overlayScrollH);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (useOverlay) {
        OverlayScrollbarGetInfo(win->overlayScrollH, &si);
    } else {
        GetScrollInfo(win->hwndCanvas, SB_HORZ, &si);
    }

    int currPos = si.nPos;
    USHORT msg = LOWORD(wp);
    switch (msg) {
        case SB_LEFT:
            si.nPos = si.nMin;
            break;
        case SB_RIGHT:
            si.nPos = si.nMax;
            break;
        case SB_LINELEFT:
            si.nPos -= DpiScale(win->hwndCanvas, 16);
            break;
        case SB_LINERIGHT:
            si.nPos += DpiScale(win->hwndCanvas, 16);
            break;
        case SB_PAGELEFT:
            si.nPos -= si.nPage;
            break;
        case SB_PAGERIGHT:
            si.nPos += si.nPage;
            break;
        case SB_THUMBTRACK:
            si.nPos = si.nTrackPos;
            break;
    }

    // Set the position and then retrieve it.  Due to adjustments
    // by Windows it may not be the same as the value set.
    si.fMask = SIF_POS;
    SetScrollInfo(win->hwndCanvas, SB_HORZ, &si, !useOverlay);
    GetScrollInfo(win->hwndCanvas, SB_HORZ, &si);
    if (useOverlay) {
        OverlayScrollbarSetInfo(win->overlayScrollH, &si, TRUE);
    }

    // If the position has changed or we're dealing with a touchpad scroll event,
    // scroll the window and update it
    if (si.nPos != currPos || msg == SB_THUMBTRACK) {
        win->AsFixed()->ScrollXTo(si.nPos);
        ReadAloudOnUserViewChanged(win);
    }
}

static void DrawMovePattern(MainWindow* win, Point pt, Size size) {
    HWND hwnd = win->hwndCanvas;
    HDC hdc = GetDC(hwnd);
    auto [x, y] = pt;
    auto [dx, dy] = size;
    x += win->annotationBeingMovedOffset.x;
    y += win->annotationBeingMovedOffset.y;
    SetBrushOrgEx(hdc, x, y, nullptr);
    HBRUSH hbrushOld = (HBRUSH)SelectObject(hdc, win->brMovePattern);
    PatBlt(hdc, x, y, dx, dy, PATINVERT);
    SelectObject(hdc, hbrushOld);
    ReleaseDC(hwnd, hdc);
}

static void StartMouseDrag(MainWindow* win, int x, int y, bool right = false) {
    SetCapture(win->hwndCanvas);
    win->mouseAction = MouseAction::Dragging;
    win->dragRightClick = right;
    win->dragPrevPos = Point(x, y);
    if (GetCursor()) {
        SetCursor(gCursorDrag);
    }
}

// Get the resize handle at the given point for the selected annotation
static ResizeHandle GetResizeHandleAt(MainWindow* win, Point pt, Annotation* annot) {
    if (!annot) {
        return ResizeHandle::None;
    }

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return ResizeHandle::None;
    }

    int pageNo = annot->pageNo;
    if (!dm->PageVisible(pageNo)) {
        return ResizeHandle::None;
    }

    Rect rect = dm->CvtToScreen(pageNo, GetRect(annot));
    int hs = kResizeHandleSize;

    bool nearLeft = pt.x >= rect.x - hs && pt.x <= rect.x + hs;
    bool nearRight = pt.x >= rect.x + rect.dx - hs && pt.x <= rect.x + rect.dx + hs;
    bool nearTop = pt.y >= rect.y - hs && pt.y <= rect.y + hs;
    bool nearBottom = pt.y >= rect.y + rect.dy - hs && pt.y <= rect.y + rect.dy + hs;
    bool betweenX = pt.x >= rect.x + hs && pt.x <= rect.x + rect.dx - hs;
    bool betweenY = pt.y >= rect.y + hs && pt.y <= rect.y + rect.dy - hs;

    // clang-format off
    // corners have priority over edges
    if (nearLeft  && nearTop)    return ResizeHandle::TopLeft;
    if (nearRight && nearTop)    return ResizeHandle::TopRight;
    if (nearRight && nearBottom) return ResizeHandle::BottomRight;
    if (nearLeft  && nearBottom) return ResizeHandle::BottomLeft;
    // edges
    if (betweenX  && nearTop)    return ResizeHandle::Top;
    if (nearRight && betweenY)   return ResizeHandle::Right;
    if (betweenX  && nearBottom) return ResizeHandle::Bottom;
    if (nearLeft  && betweenY)   return ResizeHandle::Left;
    // clang-format on

    return ResizeHandle::None;
}

static ResizeHandle GetEbookResizeHandleAt(MainWindow* win, Point pt, EbookAnnotation* annotation) {
    DisplayModel* dm = win->AsFixed();
    WindowTab* tab = win->CurrentTab();
    if (!dm || !tab || !annotation || !AnnotationCanBeResized(EbookAnnotationGetType(annotation))) {
        return ResizeHandle::None;
    }
    int pageNo = 0;
    RectF bounds;
    if (!EbookAnnotationGetPageBounds(tab, dm, annotation, &pageNo, &bounds)) {
        return ResizeHandle::None;
    }
    Rect rect = dm->CvtToScreen(pageNo, bounds);
    int hs = kResizeHandleSize;
    bool nearLeft = pt.x >= rect.x - hs && pt.x <= rect.x + hs;
    bool nearRight = pt.x >= rect.BR().x - hs && pt.x <= rect.BR().x + hs;
    bool nearTop = pt.y >= rect.y - hs && pt.y <= rect.y + hs;
    bool nearBottom = pt.y >= rect.BR().y - hs && pt.y <= rect.BR().y + hs;
    bool betweenX = pt.x >= rect.x + hs && pt.x <= rect.BR().x - hs;
    bool betweenY = pt.y >= rect.y + hs && pt.y <= rect.BR().y - hs;
    if (nearLeft && nearTop) return ResizeHandle::TopLeft;
    if (nearRight && nearTop) return ResizeHandle::TopRight;
    if (nearRight && nearBottom) return ResizeHandle::BottomRight;
    if (nearLeft && nearBottom) return ResizeHandle::BottomLeft;
    if (betweenX && nearTop) return ResizeHandle::Top;
    if (nearRight && betweenY) return ResizeHandle::Right;
    if (betweenX && nearBottom) return ResizeHandle::Bottom;
    if (nearLeft && betweenY) return ResizeHandle::Left;
    return ResizeHandle::None;
}

// Get the appropriate cursor for a resize handle
static LPWSTR GetCursorForResizeHandle(ResizeHandle handle) {
    switch (handle) {
        case ResizeHandle::TopLeft:
        case ResizeHandle::BottomRight:
            return IDC_SIZENWSE;
        case ResizeHandle::TopRight:
        case ResizeHandle::BottomLeft:
            return IDC_SIZENESW;
        case ResizeHandle::Top:
        case ResizeHandle::Bottom:
            return IDC_SIZENS;
        case ResizeHandle::Left:
        case ResizeHandle::Right:
            return IDC_SIZEWE;
        default:
            return IDC_ARROW;
    }
}

// return true if this was annotation dragging
static bool StopDraggingAnnotation(MainWindow* win, int x, int y, bool aborted) {
    Annotation* annot = win->annotationBeingDragged;
    if (!annot) {
        return false;
    }
    DrawMovePattern(win, win->dragPrevPos, win->annotationBeingMovedSize);

    win->annotationBeingDragged = nullptr;
    if (aborted) {
        return true;
    }

    DisplayModel* dm = win->AsFixed();
    x += win->annotationBeingMovedOffset.x;
    y += win->annotationBeingMovedOffset.y;
    Point pt{x, y};
    int pageNo = dm->GetPageNoByPoint(pt);
    // we can only move annotation within the same page
    if (pageNo == PageNo(annot)) {
        Rect rScreen{x, y, 1, 1};
        RectF r = dm->CvtFromScreen(rScreen, pageNo);
        RectF ar = GetRect(annot);
        r.dx = ar.dx;
        r.dy = ar.dy;
        // logf("prev rect: x=%.2f, y=%.2f, dx=%.2f, dy=%.2f\n", ar.x, ar.y, ar.dx, ar.dy);
        // logf(" new rect: x=%.2f, y=%.2f, dx=%.2f, dy=%.2f\n", r.x, r.y, r.dx, r.dy);
        SetRect(annot, r);
        NotifyAnnotationsChanged(win->CurrentTab()->editAnnotsWindow);
        MainWindowRerenderAnnotationChange(win, annot->pageNo, IsPdfTextMarkupAnnotation(annot) ? annot : nullptr);
        ToolbarUpdateStateForWindow(win, true);
    }
    return true;
}

static bool StopDraggingEbookAnnotation(MainWindow* win, int x, int y, bool aborted) {
    EbookAnnotation* annotation = win->ebookAnnotationBeingDragged;
    if (!annotation || win->annotationBeingResized) {
        return false;
    }
    DrawMovePattern(win, win->dragPrevPos, win->annotationBeingMovedSize);
    win->ebookAnnotationBeingDragged = nullptr;
    if (aborted) {
        return true;
    }
    DisplayModel* dm = win->AsFixed();
    WindowTab* tab = win->CurrentTab();
    int pageNo = 0;
    RectF bounds;
    if (!EbookAnnotationGetPageBounds(tab, dm, annotation, &pageNo, &bounds)) {
        return true;
    }
    Point topLeft{x + win->annotationBeingMovedOffset.x, y + win->annotationBeingMovedOffset.y};
    if (dm->GetPageNoByPoint(topLeft) == pageNo) {
        RectF moved = dm->CvtFromScreen(Rect(topLeft.x, topLeft.y, 1, 1), pageNo);
        moved.dx = bounds.dx;
        moved.dy = bounds.dy;
        EbookAnnotationSetPageBounds(tab, dm, annotation, pageNo, moved, true);
        MainWindowRerender(win);
    }
    return true;
}

static void StopMouseDrag(MainWindow* win, int x, int y, bool aborted) {
    if (GetCapture() != win->hwndCanvas) {
        return;
    }

    if (GetCursor()) {
        SetCursorCached(IDC_ARROW);
    }
    ReleaseCapture();
    // A right-button drag is only a document/context-menu gesture. It must
    // never leak into a subsequent EPUB annotation move, where it used to
    // make the next right click act as an unintended "drop".
    win->dragRightClick = false;

    if (StopDraggingEbookAnnotation(win, x, y, aborted) || StopDraggingAnnotation(win, x, y, aborted)) {
        return;
    }

    if (aborted) {
        return;
    }

    Size drag(x - win->dragPrevPos.x, y - win->dragPrevPos.y);
    win->MoveDocBy(drag.dx, -2 * drag.dy);
}

static bool StopEbookAnnotationResize(MainWindow* win, bool aborted);

void CancelDrag(MainWindow* win) {
    auto pt = win->dragPrevPos;
    auto [x, y] = pt;
    if (win->ebookAnnotationBeingDragged && win->annotationBeingResized) {
        StopEbookAnnotationResize(win, true);
    } else {
        StopMouseDrag(win, x, y, true);
    }
    win->mouseAction = MouseAction::None;
    win->linkOnLastButtonDown = nullptr;
    win->annotationBeingDragged = nullptr;
    win->ebookAnnotationDragPending = nullptr;
    win->annotationBeingResized = false;
    win->ocrRegionPending = false;
    SetCursorCached(IDC_ARROW);
}

bool IsDragDistance(int x1, int x2, int y1, int y2) {
    int dx = abs(x1 - x2);
    int dragDx = GetSystemMetrics(SM_CXDRAG);
    if (dx > dragDx) {
        return true;
    }

    int dy = abs(y1 - y2);
    int dragDy = GetSystemMetrics(SM_CYDRAG);
    return dy > dragDy;
}

static bool gShowAnnotationNotification = true;

// Forward declaration
static RectF CalculateResizedRect(MainWindow* win, int x, int y);
static RectF CalculateResizedEbookRect(MainWindow* win, int x, int y);
static void StartEbookAnnotationDrag(MainWindow* win, EbookAnnotation* annotation, Point pt);

static void OnMouseMove(MainWindow* win, int x, int y, WPARAM) {
    DisplayModel* dm = win->AsFixed();
    // ReportIf(!dm); // can happen if reload fails, we delete DisplayModel
    if (!dm) return;

    if (win->InPresentation()) {
        if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
            // logf("OnMouseMove: hiding cursor because black screen or white screen\n");
            SetCursor((HCURSOR) nullptr);
            return;
        }

        bool showingCursor = (GetCursor() != nullptr);
        bool sameAsLastPos = win->dragPrevPos.Eq(x, y);
        // logf("OnMouseMove(): win->InPresentation() (%d, %d) showingCursor: %d, same as last pos: %d\n", x,
        // y,
        //     (int)showingCursor, (int)sameAsLastPos);
        if (!sameAsLastPos) {
            // shortly display the cursor if the mouse has moved and the cursor is hidden
            if (!showingCursor) {
                // logf("OnMouseMove: temporary showing cursor\n");
                if (win->mouseAction == MouseAction::None) {
                    SetCursorCached(IDC_ARROW);
                } else {
                    SendMessageW(win->hwndCanvas, WM_SETCURSOR, 0, 0);
                }
            }
            if (win->dragPrevPos.Eq(-2, -3)) {
                // hack: hide cursor immediately. see EnterFullScreen
                SetTimer(win->hwndCanvas, kHideCursorTimerID, 1, nullptr);
            } else {
                // logf("OnMouseMove: starting kHideCursorTimerID\n");
                SetTimer(win->hwndCanvas, kHideCursorTimerID, kHideCursorDelayInMs, nullptr);
            }
        }
    }

    Point pos{x, y};
    NotificationWnd* cursorPosNotif = GetNotificationForGroup(win->hwndCanvas, kNotifCursorPos);

    if (win->textDragPending) {
        if (!IsDragDistance(x, win->dragStart.x, y, win->dragStart.y)) {
            return;
        }
        // threshold met: initiate OLE drag-drop of selected text
        win->textDragPending = false;
        win->dragStartPending = false;
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        StartTextDragDrop(win);
        return;
    }

    if (win->imageDragPending) {
        if (!IsDragDistance(x, win->dragStart.x, y, win->dragStart.y)) {
            return;
        }
        win->imageDragPending = false;
        win->dragStartPending = false;
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        StartImageDragDrop(win);
        win->imageDragElement = nullptr;
        return;
    }

    if (win->ebookAnnotationDragPending) {
        if (!IsDragDistance(x, win->dragStart.x, y, win->dragStart.y)) {
            return;
        }
        EbookAnnotation* annotation = win->ebookAnnotationDragPending;
        win->ebookAnnotationDragPending = nullptr;
        win->dragStartPending = false;
        StartEbookAnnotationDrag(win, annotation, win->dragStart);
        Point dragPoint{x, y};
        DrawMovePattern(win, win->dragStart, win->annotationBeingMovedSize);
        DrawMovePattern(win, dragPoint, win->annotationBeingMovedSize);
        win->mouseAction = MouseAction::Dragging;
        win->dragPrevPos = dragPoint;
        return;
    }

    if (win->dragStartPending) {
        if (!IsDragDistance(x, win->dragStart.x, y, win->dragStart.y)) {
            return;
        }
        win->dragStartPending = false;
        win->linkOnLastButtonDown = nullptr;
    }

    Point prevPos = win->dragPrevPos;
    switch (win->mouseAction) {
        case MouseAction::None: {
            Annotation* annot = dm->GetAnnotationAtPos(pos, nullptr);
            Annotation* prev = win->annotationUnderCursor;
            if (annot != prev) {
#if 0
                TempStr name = annot ? AnnotationReadableNameTemp(annot->type) : (TempStr) "none";
                TempStr prevName = prev ? AnnotationReadableNameTemp(prev->type) : (TempStr) "none";
                logf("different annot under cursor. prev: %s, new: %s\n", prevName, name);
#endif
                if (gShowAnnotationNotification) {
                    if (annot && !AnnotationSupportsMediaPlayback(annot->type)) {
                        // auto r = annot->bounds;
                        // logf("new pos: %d-%d, size: %d-%d\n", (int)r.x, (int)r.y, (int)r.dx, (int)r.dy);
                        RemoveNotificationsForGroup(win->hwndCanvas, kNotifAnnotation);
                        NotificationCreateArgs args;
                        args.hwndParent = win->hwndCanvas;
                        args.groupId = kNotifAnnotation;
                        args.timeoutMs = 3000;
                        args.delayInMs = 1000;
                        args.noClose = true;
                        TempStr name = AnnotationReadableNameTemp(annot->type);
                        const char* fmt = _TRA("%s annotation. Ctrl+click to edit.");
                        args.msg = str::FormatTemp(fmt, name);
                        ShowNotification(args);
                    }
                }
            }
            if (!annot) {
                RemoveNotificationsForGroup(win->hwndCanvas, kNotifAnnotation);
            }
            win->annotationUnderCursor = annot;
            break;
        }

        case MouseAction::Scrolling: {
            win->annotationUnderCursor = nullptr;
            win->yScrollSpeed = (y - win->dragStart.y) / SMOOTHSCROLL_SLOW_DOWN_FACTOR;
            win->xScrollSpeed = (x - win->dragStart.x) / SMOOTHSCROLL_SLOW_DOWN_FACTOR;
            break;
        }
        case MouseAction::SelectingText:
            if (GetCursor()) {
                SetCursorCached(IDC_IBEAM);
            }
            [[fallthrough]];
        case MouseAction::Selecting: {
            win->annotationUnderCursor = nullptr;
            win->selectionRect.dx = x - win->selectionRect.x;
            win->selectionRect.dy = y - win->selectionRect.y;
            win->selectionMeasure = dm->CvtFromScreen(win->selectionRect).Size();
            OnSelectionEdgeAutoscroll(win, x, y);
            ScheduleRepaint(win, 0);
            break;
        }
        case MouseAction::CreatingAnnotation: {
            win->annotationUnderCursor = nullptr;
            if (win->annotCreateToolCmd != CmdCreateAnnotInk) {
                win->selectionRect.dx = x - win->selectionRect.x;
                win->selectionRect.dy = y - win->selectionRect.y;
            }
            if (win->annotCreateToolCmd == CmdCreateAnnotInk) {
                int pageNo = dm->GetPageNoByPoint(win->annotCreateDragStart);
                if (dm->ValidPageNo(pageNo)) {
                    PointF pagePt = dm->CvtFromScreen({x, y}, pageNo);
                    if (win->annotCreateInkPoints.len == 0) {
                        PointF startPt = dm->CvtFromScreen(win->annotCreateDragStart, pageNo);
                        win->annotCreateInkPoints.Append(startPt);
                    }
                    win->annotCreateInkPoints.Append(pagePt);
                }
            }
            ScheduleRepaint(win, 0);
            break;
        }
        case MouseAction::OcrRegion: {
            win->annotationUnderCursor = nullptr;
            win->selectionRect.dx = x - win->selectionRect.x;
            win->selectionRect.dy = y - win->selectionRect.y;
            ScheduleRepaint(win, 0);
            break;
        }
        case MouseAction::Dragging: {
            EbookAnnotation* ebookAnnotation = win->ebookAnnotationBeingDragged;
            if (ebookAnnotation) {
                if (win->annotationBeingResized) {
                    win->dragPrevPos = pos;
                    SetCursorCached(GetCursorForResizeHandle((ResizeHandle)win->resizeHandle));
                    int pageNo = 0;
                    RectF ignored;
                    EbookAnnotationGetPageBounds(win->CurrentTab(), dm, ebookAnnotation, &pageNo, &ignored);
                    RectF bounds = CalculateResizedEbookRect(win, x, y);
                    EbookAnnotationSetPageBounds(win->CurrentTab(), dm, ebookAnnotation, pageNo, bounds, false);
                    MainWindowRerender(win);
                } else {
                    Size size = win->annotationBeingMovedSize;
                    DrawMovePattern(win, prevPos, size);
                    DrawMovePattern(win, pos, size);
                }
                break;
            }
            Annotation* annot = win->annotationBeingDragged;
            if (annot) {
                if (win->annotationBeingResized) {
                    // During resize, calculate and apply new rectangle in real-time
                    win->dragPrevPos = pos;
                    // Keep the resize cursor active during resize
                    SetCursorCached(GetCursorForResizeHandle((ResizeHandle)win->resizeHandle));

                    // Calculate and apply the new rectangle based on current mouse position
                    RectF newRect = CalculateResizedRect(win, x, y);
                    SetRect(annot, newRect);

                    MainWindowRerender(win);
                } else {
                    Size size = win->annotationBeingMovedSize;
                    DrawMovePattern(win, prevPos, size);
                    DrawMovePattern(win, pos, size);
                }
            } else {
                win->MoveDocBy(win->dragPrevPos.x - x, win->dragPrevPos.y - y);
            }
            break;
        }
    }
    win->dragPrevPos = pos;

    if (ReadAloudCanReadFromCursor(dm, pos)) {
        win->readAloudLastTextPt = pos;
        win->readAloudLastTextPtValid = true;
    }

    if (cursorPosNotif) {
        UpdateCursorPositionHelper(win, pos, cursorPosNotif);
    }
}

static void StartAnnotationDrag(MainWindow* win, Annotation* annot, Point& pt) {
    win->annotationBeingDragged = annot;
    DisplayModel* dm = win->AsFixed();
    CreateMovePatternLazy(win);
    RectF r = GetRect(annot);
    int pageNo = PageNo(annot);
    Rect rScreen = dm->CvtToScreen(pageNo, r);
    win->annotationBeingMovedSize = {rScreen.dx, rScreen.dy};
    int offsetX = rScreen.x - pt.x;
    int offsetY = rScreen.y - pt.y;
    win->annotationBeingMovedOffset = Point{offsetX, offsetY};
    DrawMovePattern(win, pt, win->annotationBeingMovedSize);
}

static void StartEbookAnnotationDrag(MainWindow* win, EbookAnnotation* annotation, Point pt) {
    DisplayModel* dm = win->AsFixed();
    int pageNo = 0;
    RectF bounds;
    if (!EbookAnnotationGetPageBounds(win->CurrentTab(), dm, annotation, &pageNo, &bounds)) {
        return;
    }
    win->ebookAnnotationBeingDragged = annotation;
    // This path starts from the left button, but it bypasses StartMouseDrag().
    // Clear the flag left by a previous context-menu drag; otherwise the left
    // button-up is ignored and a following right click incorrectly drops it.
    win->dragRightClick = false;
    CreateMovePatternLazy(win);
    Rect screen = dm->CvtToScreen(pageNo, bounds);
    win->annotationBeingMovedSize = screen.Size();
    win->annotationBeingMovedOffset = {screen.x - pt.x, screen.y - pt.y};
    DrawMovePattern(win, pt, win->annotationBeingMovedSize);
}

static RectF CalculateResizedEbookRect(MainWindow* win, int x, int y) {
    DisplayModel* dm = win->AsFixed();
    EbookAnnotation* annotation = win->ebookAnnotationBeingDragged;
    int pageNo = 0;
    RectF ignored;
    if (!EbookAnnotationGetPageBounds(win->CurrentTab(), dm, annotation, &pageNo, &ignored)) {
        return win->annotationOriginalRect;
    }
    RectF pagePt = dm->CvtFromScreen(Rect(x, y, 1, 1), pageNo);
    RectF startPage = dm->CvtFromScreen(Rect(win->dragStart.x, win->dragStart.y, 1, 1), pageNo);
    float deltaX = pagePt.x - startPage.x;
    float deltaY = pagePt.y - startPage.y;
    RectF orig = win->annotationOriginalRect;
    RectF bounds = orig;
    ResizeHandle handle = (ResizeHandle)win->resizeHandle;
    bool moveLeft =
        handle == ResizeHandle::TopLeft || handle == ResizeHandle::Left || handle == ResizeHandle::BottomLeft;
    bool moveRight =
        handle == ResizeHandle::TopRight || handle == ResizeHandle::Right || handle == ResizeHandle::BottomRight;
    bool moveTop = handle == ResizeHandle::TopLeft || handle == ResizeHandle::Top || handle == ResizeHandle::TopRight;
    bool moveBottom =
        handle == ResizeHandle::BottomLeft || handle == ResizeHandle::Bottom || handle == ResizeHandle::BottomRight;
    constexpr float minSize = 10.f;
    if (moveLeft) {
        bounds.x = orig.x + deltaX;
        bounds.dx = orig.dx - deltaX;
        if (bounds.dx < minSize) {
            bounds.x = orig.x + orig.dx - minSize;
            bounds.dx = minSize;
        }
    }
    if (moveRight) {
        bounds.dx = std::max(minSize, orig.dx + deltaX);
    }
    if (moveTop) {
        bounds.y = orig.y + deltaY;
        bounds.dy = orig.dy - deltaY;
        if (bounds.dy < minSize) {
            bounds.y = orig.y + orig.dy - minSize;
            bounds.dy = minSize;
        }
    }
    if (moveBottom) {
        bounds.dy = std::max(minSize, orig.dy + deltaY);
    }
    return bounds;
}

static void StartEbookAnnotationResize(MainWindow* win, EbookAnnotation* annotation, Point pt, ResizeHandle handle) {
    int pageNo = 0;
    RectF bounds;
    if (!EbookAnnotationGetPageBounds(win->CurrentTab(), win->AsFixed(), annotation, &pageNo, &bounds)) {
        return;
    }
    win->ebookAnnotationBeingDragged = annotation;
    win->dragRightClick = false;
    win->annotationBeingResized = true;
    win->resizeHandle = (int)handle;
    win->dragStart = pt;
    win->annotationOriginalRect = bounds;
    SetCapture(win->hwndCanvas);
    win->mouseAction = MouseAction::Dragging;
    win->dragPrevPos = pt;
}

static bool StopEbookAnnotationResize(MainWindow* win, bool aborted) {
    EbookAnnotation* annotation = win->ebookAnnotationBeingDragged;
    if (!annotation || !win->annotationBeingResized) {
        return false;
    }
    int pageNo = 0;
    RectF bounds;
    EbookAnnotationGetPageBounds(win->CurrentTab(), win->AsFixed(), annotation, &pageNo, &bounds);
    win->annotationBeingResized = false;
    win->ebookAnnotationBeingDragged = nullptr;
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    if (aborted) {
        bounds = win->annotationOriginalRect;
    }
    EbookAnnotationSetPageBounds(win->CurrentTab(), win->AsFixed(), annotation, pageNo, bounds, !aborted);
    MainWindowRerender(win);
    return true;
}

// Helper function to calculate new rectangle during resize
static RectF CalculateResizedRect(MainWindow* win, int x, int y) {
    DisplayModel* dm = win->AsFixed();
    Annotation* annot = win->annotationBeingDragged;
    int pageNo = PageNo(annot);

    // Convert screen coordinates to page coordinates
    Rect screenPt{x, y, 1, 1};
    RectF pagePt = dm->CvtFromScreen(screenPt, pageNo);

    RectF orig = win->annotationOriginalRect;
    RectF r = orig;

    Point startPt = win->dragStart;
    Rect startScreen{startPt.x, startPt.y, 1, 1};
    RectF startPage = dm->CvtFromScreen(startScreen, pageNo);

    float deltaX = pagePt.x - startPage.x;
    float deltaY = pagePt.y - startPage.y;

    const float minSize = 10.0F;
    auto handle = (ResizeHandle)win->resizeHandle;

    bool moveLeft =
        handle == ResizeHandle::TopLeft || handle == ResizeHandle::Left || handle == ResizeHandle::BottomLeft;
    bool moveRight =
        handle == ResizeHandle::TopRight || handle == ResizeHandle::Right || handle == ResizeHandle::BottomRight;
    bool moveTop = handle == ResizeHandle::TopLeft || handle == ResizeHandle::Top || handle == ResizeHandle::TopRight;
    bool moveBottom =
        handle == ResizeHandle::BottomLeft || handle == ResizeHandle::Bottom || handle == ResizeHandle::BottomRight;

    if (moveLeft) {
        r.x = orig.x + deltaX;
        r.dx = orig.dx - deltaX;
        if (r.dx < minSize) {
            r.x = orig.x + orig.dx - minSize;
            r.dx = minSize;
        }
    }
    if (moveRight) {
        r.dx = orig.dx + deltaX;
        r.dx = std::max(r.dx, minSize);
    }
    if (moveTop) {
        r.y = orig.y + deltaY;
        r.dy = orig.dy - deltaY;
        if (r.dy < minSize) {
            r.y = orig.y + orig.dy - minSize;
            r.dy = minSize;
        }
    }
    if (moveBottom) {
        r.dy = orig.dy + deltaY;
        r.dy = std::max(r.dy, minSize);
    }

    return r;
}

static void StartAnnotationResize(MainWindow* win, Annotation* annot, Point& pt, ResizeHandle handle) {
    win->annotationBeingDragged = annot;
    win->annotationBeingResized = true;
    win->resizeHandle = (int)handle;
    win->dragStart = pt;
    RectF r = GetRect(annot);
    win->annotationOriginalRect = r;
    SetCapture(win->hwndCanvas);
    win->mouseAction = MouseAction::Dragging;
    win->dragPrevPos = pt;
}

static bool StopAnnotationResize(MainWindow* win, int x, int y, bool aborted) {
    if (!win->annotationBeingResized) {
        return false;
    }

    Annotation* annot = win->annotationBeingDragged;
    win->annotationBeingResized = false;
    win->annotationBeingDragged = nullptr;

    // Release mouse capture and reset cursor
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    SetCursorCached(IDC_ARROW);

    if (aborted || !annot) {
        return true;
    }

    // The annotation has already been updated during mouse move,
    // just notify and update toolbar
    NotifyAnnotationsChanged(win->CurrentTab()->editAnnotsWindow);
    MainWindowRerenderAnnotationChange(win, annot->pageNo, IsPdfTextMarkupAnnotation(annot) ? annot : nullptr);
    ToolbarUpdateStateForWindow(win, true);

    return true;
}

static Rect NormalizeScreenRect(Point a, Point b) {
    int x0 = std::min(a.x, b.x);
    int y0 = std::min(a.y, b.y);
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

static void PaintAnnotCreatePreview(MainWindow* win, DisplayModel* dm, HDC hdc) {
    bool ocrDrag = win->mouseAction == MouseAction::OcrRegion;
    if (win->mouseAction != MouseAction::CreatingAnnotation && !ocrDrag) {
        return;
    }
    int cmdId = win->annotCreateToolCmd;
    Gdiplus::Graphics gs(hdc);
    Gdiplus::Color col = ocrDrag ? Gdiplus::Color(200, 40, 120, 220) : Gdiplus::Color(200, 255, 140, 0);
    Gdiplus::Pen pen(col, 2);

    if (cmdId == CmdCreateAnnotText) {
        return;
    }
    if (cmdId == CmdCreateAnnotStamp) {
        Rect rc = win->selectionRect;
        if (rc.dx < 0) {
            rc.x += rc.dx;
            rc.dx = -rc.dx;
        }
        if (rc.dy < 0) {
            rc.y += rc.dy;
            rc.dy = -rc.dy;
        }
        if (rc.dx < 4 && rc.dy < 4) {
            return;
        }
        gs.DrawRectangle(&pen, rc.x, rc.y, rc.dx, rc.dy);
        return;
    }
    if (cmdId == CmdCreateAnnotInk) {
        if (win->annotCreateInkPoints.len < 2) {
            return;
        }
        int pageNo = dm->GetPageNoByPoint(win->annotCreateDragStart);
        if (!dm->ValidPageNo(pageNo)) {
            return;
        }
        PointF prev = win->annotCreateInkPoints[0];
        Point prevScreen = dm->CvtToScreen(pageNo, prev);
        for (size_t i = 1; i < win->annotCreateInkPoints.len; i++) {
            PointF cur = win->annotCreateInkPoints[i];
            Point curScreen = dm->CvtToScreen(pageNo, cur);
            gs.DrawLine(&pen, prevScreen.x, prevScreen.y, curScreen.x, curScreen.y);
            prevScreen = curScreen;
        }
        return;
    }

    if (cmdId == CmdCreateAnnotLine) {
        Point start = win->annotCreateDragStart;
        Point end{win->selectionRect.x + win->selectionRect.dx, win->selectionRect.y + win->selectionRect.dy};
        gs.DrawLine(&pen, start.x, start.y, end.x, end.y);
        return;
    }

    Rect rc = win->selectionRect;
    if (rc.dx < 0) {
        rc.x += rc.dx;
        rc.dx = -rc.dx;
    }
    if (rc.dy < 0) {
        rc.y += rc.dy;
        rc.dy = -rc.dy;
    }
    if (rc.dx <= 0 && rc.dy <= 0) {
        return;
    }
    if (ocrDrag) {
        gs.DrawRectangle(&pen, rc.x, rc.y, rc.dx, rc.dy);
        return;
    }
    if (cmdId == CmdCreateAnnotCircle) {
        gs.DrawEllipse(&pen, rc.x, rc.y, rc.dx, rc.dy);
    } else if (cmdId == CmdCreateAnnotSquare) {
        gs.DrawRectangle(&pen, rc.x, rc.y, rc.dx, rc.dy);
    }
}

static void ReleaseAnnotCreateToolIfUnlocked(MainWindow* win) {
    if (win && !win->annotCreateToolLocked) {
        SetAnnotCreateTool(win, 0);
    }
}

static void FinishEbookAnnotCreateDrag(MainWindow* win, WindowTab* tab, DisplayModel* dm, Point endCanvas) {
    int cmdId = win->annotCreateToolCmd;
    AnnotationType type = CmdIdToAnnotationType(cmdId);
    if (type == AnnotationType::Unknown) {
        return;
    }
    Point start = win->annotCreateDragStart;
    int pageNo = dm->GetPageNoByPoint(start);
    if (!dm->ValidPageNo(pageNo)) {
        return;
    }
    if (type == AnnotationType::Text) {
        EbookAnnotation* annotation =
            EbookAnnotationsCreateAt(tab, dm, start, type, GetDefaultEbookPointAnnotationColor(type));
        win->annotCreateInkPoints.Reset();
        if (!annotation) {
            return;
        }
        tab->selectedEbookAnnotation = annotation;
        UpdateEbookAnnotationsList(tab->editEbookAnnotsWindow, annotation);
        MainWindowRerender(win);
        ReleaseAnnotCreateToolIfUnlocked(win);
        return;
    }
    if (type == AnnotationType::Stamp) {
        Rect screenRect = NormalizeScreenRect(start, endCanvas);
        EbookAnnotation* annotation = nullptr;
        if (screenRect.dx < 4 && screenRect.dy < 4) {
            annotation = EbookAnnotationsCreateAt(tab, dm, start, type, GetDefaultEbookPointAnnotationColor(type));
        } else {
            annotation = EbookAnnotationsCreateDragShape(tab, dm, start, endCanvas, type);
        }
        win->annotCreateInkPoints.Reset();
        if (!annotation) {
            return;
        }
        tab->selectedEbookAnnotation = annotation;
        UpdateEbookAnnotationsList(tab->editEbookAnnotsWindow, annotation);
        MainWindowRerender(win);
        ReleaseAnnotCreateToolIfUnlocked(win);
        return;
    }
    if (type == AnnotationType::Ink) {
        if (win->annotCreateInkPoints.len < 2) {
            PointF a = dm->CvtFromScreen(start, pageNo);
            PointF b = dm->CvtFromScreen(endCanvas, pageNo);
            win->annotCreateInkPoints.Reset();
            win->annotCreateInkPoints.Append(a);
            win->annotCreateInkPoints.Append(b);
        }
        EbookAnnotation* annotation = EbookAnnotationsCreateInkStroke(tab, dm, pageNo, win->annotCreateInkPoints.els,
                                                                      (int)win->annotCreateInkPoints.len,
                                                                      GetDefaultEbookPointAnnotationColor(type));
        win->annotCreateInkPoints.Reset();
        if (!annotation) {
            return;
        }
        tab->selectedEbookAnnotation = annotation;
        UpdateEbookAnnotationsList(tab->editEbookAnnotsWindow, annotation);
        MainWindowRerender(win);
        ReleaseAnnotCreateToolIfUnlocked(win);
        return;
    }
    Rect screenRect = NormalizeScreenRect(start, endCanvas);
    if (screenRect.dx < 4 && screenRect.dy < 4) {
        return;
    }
    EbookAnnotation* annotation = EbookAnnotationsCreateDragShape(tab, dm, start, endCanvas, type);
    if (!annotation) {
        return;
    }
    tab->selectedEbookAnnotation = annotation;
    UpdateEbookAnnotationsList(tab->editEbookAnnotsWindow, annotation);
    MainWindowRerender(win);
    ReleaseAnnotCreateToolIfUnlocked(win);
}

static void FinishAnnotCreateDrag(MainWindow* win, Point endCanvas) {
    WindowTab* tab = win->CurrentTab();
    DisplayModel* dm = win->AsFixed();
    if (!tab || !dm) {
        return;
    }
    if (EbookAnnotationsSupported(tab)) {
        FinishEbookAnnotCreateDrag(win, tab, dm, endCanvas);
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine || !EngineSupportsAnnotations(engine)) {
        return;
    }
    int cmdId = win->annotCreateToolCmd;
    AnnotationType type = CmdIdToAnnotationType(cmdId);
    if (type == AnnotationType::Unknown) {
        return;
    }
    Point start = win->annotCreateDragStart;
    Rect screenRect = NormalizeScreenRect(start, endCanvas);
    if (cmdId != CmdCreateAnnotInk && cmdId != CmdCreateAnnotText && cmdId != CmdCreateAnnotStamp &&
        (screenRect.dx < 4 && screenRect.dy < 4)) {
        return;
    }
    int pageNo = dm->GetPageNoByPoint(start);
    if (!dm->ValidPageNo(pageNo)) {
        return;
    }
    AnnotCreateArgs args{type};
    Annotation* annot = nullptr;
    if (type == AnnotationType::Text) {
        PointF ptOnPage = dm->CvtFromScreen(start, pageNo);
        annot = EngineMupdfCreateAnnotation(engine, pageNo, ptOnPage, &args);
    } else if (type == AnnotationType::Ink) {
        if (win->annotCreateInkPoints.len < 2) {
            PointF a = dm->CvtFromScreen(start, pageNo);
            PointF b = dm->CvtFromScreen(endCanvas, pageNo);
            win->annotCreateInkPoints.Reset();
            win->annotCreateInkPoints.Append(a);
            win->annotCreateInkPoints.Append(b);
        }
        annot = EngineMupdfCreateAnnotationInkStroke(engine, pageNo, win->annotCreateInkPoints.els,
                                                     (int)win->annotCreateInkPoints.len, &args);
    } else if (type == AnnotationType::Line) {
        PointF a = dm->CvtFromScreen(start, pageNo);
        PointF b = dm->CvtFromScreen(endCanvas, pageNo);
        annot = EngineMupdfCreateAnnotation(engine, pageNo, a, &args);
        if (annot) {
            SetLine(annot, a, b);
        }
    } else if (type == AnnotationType::Stamp) {
        if (screenRect.dx < 4 && screenRect.dy < 4) {
            PointF ptOnPage = dm->CvtFromScreen(start, pageNo);
            annot = EngineMupdfCreateAnnotation(engine, pageNo, ptOnPage, &args);
        } else {
            PointF tl = dm->CvtFromScreen({screenRect.x, screenRect.y}, pageNo);
            PointF br = dm->CvtFromScreen({screenRect.x + screenRect.dx, screenRect.y + screenRect.dy}, pageNo);
            RectF pageRect = RectF::FromXY(tl, br);
            annot = EngineMupdfCreateAnnotationInRect(engine, pageNo, pageRect, &args);
        }
    } else {
        PointF tl = dm->CvtFromScreen({screenRect.x, screenRect.y}, pageNo);
        PointF br = dm->CvtFromScreen({screenRect.x + screenRect.dx, screenRect.y + screenRect.dy}, pageNo);
        RectF pageRect = RectF::FromXY(tl, br);
        annot = EngineMupdfCreateAnnotationInRect(engine, pageNo, pageRect, &args);
    }
    win->annotCreateInkPoints.Reset();
    if (!annot) {
        return;
    }
    SetSelectedAnnotation(tab, annot);
    UpdateAnnotationsList(tab->editAnnotsWindow);
    MainWindowRerenderAnnotationChange(win, pageNo, annot);
    ToolbarUpdateStateForWindow(win, true);
    ReleaseAnnotCreateToolIfUnlocked(win);
}

static void OnMouseLeftButtonDown(MainWindow* win, int x, int y, WPARAM key) {
    // lf("Left button clicked on %d %d", x, y);
    if (IsRightDragging(win)) {
        return;
    }

    if (MouseAction::Scrolling == win->mouseAction) {
        win->mouseAction = MouseAction::None;
        return;
    }

    if (win->mouseAction != MouseAction::None) {
        // this can be MouseAction::SelectingText (4)
        // can't reproduce it so far
        logf("OnMouseLeftButtonDown: win->mouseAction=%d\n", (int)win->mouseAction);
        // ReportIf(win->mouseAction != MouseAction::Idle);
        win->mouseAction = MouseAction::None;
        return;
    }

    HwndSetFocus(win->hwndFrame);
    DisplayModel* dm = win->AsFixed();
    ReportIf(!dm);
    Point pt{x, y};

    if (win->ocrRegionPending) {
        win->annotCreateDragStart = pt;
        win->dragStart = pt;
        win->selectionRect = Rect(pt.x, pt.y, 0, 0);
        win->mouseAction = MouseAction::OcrRegion;
        win->dragStartPending = true;
        SetCapture(win->hwndCanvas);
        return;
    }

    if (win->annotCreateToolCmd != 0) {
        WindowTab* tab = win->CurrentTab();
        bool canDragCreate = EngineSupportsAnnotations(dm->GetEngine());
        if (!canDragCreate && tab && EbookAnnotationsSupported(tab)) {
            canDragCreate = true;
        }
        if (canDragCreate) {
            win->annotCreateDragStart = pt;
            win->dragStart = pt;
            win->selectionRect = Rect(pt.x, pt.y, 0, 0);
            win->mouseAction = MouseAction::CreatingAnnotation;
            win->dragStartPending = true;
            win->annotCreateInkPoints.Reset();
            SetCapture(win->hwndCanvas);
            return;
        }
        SetAnnotCreateTool(win, 0);
    }

    if (ReadAloudCanReadFromCursor(dm, pt)) {
        win->readAloudLastClickTextPt = pt;
        win->readAloudLastClickTextPtValid = true;
        win->readAloudLastTextPt = pt;
        win->readAloudLastTextPtValid = true;
    }

    WindowTab* tab = win->CurrentTab();
    EbookAnnotation* ebookAnnotation = EbookAnnotationsGetAt(tab, dm, pt);
    ResizeHandle ebookResizeHandle = GetEbookResizeHandleAt(win, pt, tab->selectedEbookAnnotation);
    if (ebookResizeHandle != ResizeHandle::None) {
        StartEbookAnnotationResize(win, tab->selectedEbookAnnotation, pt, ebookResizeHandle);
    } else if (ebookAnnotation && AnnotationCanBeMoved(EbookAnnotationGetType(ebookAnnotation))) {
        tab->selectedEbookAnnotation = ebookAnnotation;
        win->ebookAnnotationDragPending = ebookAnnotation;
        win->dragStartPending = true;
        win->dragStart = pt;
        SetCapture(win->hwndCanvas);
        return;
    }
    Annotation* annot = dm->GetAnnotationAtPos(pt, tab->selectedAnnotation);
    bool isMoveableAnnot = annot && AnnotationCanBeMoved(annot->type);
    if (isMoveableAnnot) {
        if (annot == tab->selectedAnnotation) {
            // dragging the selected annotation. do nothing here, just start dragging in mouse move
        } else if (tab->editAnnotsWindow || tab->selectedAnnotation) {
            // clicking on a different annotation while edit annotations window is open. or
            // other annotation is selected, select the clicked annotation and start dragging yet
            SetSelectedAnnotation(tab, annot);
        } else {
            isMoveableAnnot = false;
        }
    }

    // Check if we're clicking on a resize handle of the selected annotation
    // must check selectedAnnotation directly (not annot) because resize handles
    // extend beyond annotation bounds and GetAnnotationAtPos() won't find them
    ResizeHandle resizeHandle = ResizeHandle::None;
    if (tab->selectedAnnotation && AnnotationCanBeResized(tab->selectedAnnotation->type)) {
        resizeHandle = GetResizeHandleAt(win, pt, tab->selectedAnnotation);
    }

    if (resizeHandle != ResizeHandle::None) {
        StartAnnotationResize(win, tab->selectedAnnotation, pt, resizeHandle);
    } else if (isMoveableAnnot) {
        StartAnnotationDrag(win, annot, pt);
    } else {
        ReportIf(win->linkOnLastButtonDown);
        IPageElement* pageEl = dm->GetElementAtPos(pt, nullptr, true);
        if (pageEl) {
            if (pageEl->Is(kindPageElementDest) && IsPageElementLinkReachable(win->ctrl, pageEl)) {
                win->linkOnLastButtonDown = pageEl;
            }
        }
    }

    win->dragStartPending = true;
    win->dragStart = pt;
    win->textDragPending = false;

    if (win->linkOnLastButtonDown) {
        StartMouseDrag(win, x, y);
        return;
    }

    if (!win->ebookAnnotationBeingDragged && EbookAnnotationsSupported(tab) && EbookAnnotationsHitTest(tab, dm, pt)) {
        StartMouseDrag(win, x, y);
        return;
    }
    if (win->ebookAnnotationBeingDragged) {
        SetCapture(win->hwndCanvas);
        win->mouseAction = MouseAction::Dragging;
        win->dragPrevPos = pt;
        return;
    }

    // - without modifiers, clicking on text starts a text selection
    //   and clicking somewhere else starts a drag
    // - pressing Shift forces dragging
    // - pressing Ctrl forces a rectangular selection
    // - pressing Ctrl+Shift forces text selection
    // - not having CopySelection permission forces dragging
    bool isShift = IsShiftPressed();
    bool isCtrl = IsCtrlPressed();
    bool canCopy = HasPermission(Perm::CopySelection);
    // A click may load text on demand so an EPUB page that has only been
    // rendered (not yet searched/selected) can still start a text selection.
    // Hover hit-testing remains cache-only to avoid blocking mouse movement.
    bool isOverText = win->AsFixed()->IsOverText(pt, true);

    bool inSel = win->showSelection && IsPointInSelection(win, pt);
    IPageElement* hitEl = dm->GetElementAtPos(pt, nullptr, true);
    const char* hitKind = (hitEl && hitEl->kind) ? hitEl->kind : "";
    bool hitImage = hitEl && hitEl->Is(kindPageElementImage);

    // Clicking the current selection (glyphs or the highlight over a page image)
    // prepares drag-out; a click without drag clears the selection.
    if (canCopy && !isShift && !isCtrl && inSel) {
        win->textDragPending = true;
        win->linkOnLastButtonDown = nullptr;
        SetCapture(win->hwndCanvas);
        return;
    }

    // if clicking on an image, prepare for image drag-out (but not over a media annotation)
    if (canCopy && !isShift && !isCtrl && !isOverText) {
        Annotation* mediaAnnot = dm->GetAnnotationAtPos(pt, tab->selectedAnnotation);
        if (!(mediaAnnot && AnnotationSupportsMediaPlayback(mediaAnnot->type))) {
            if (hitImage) {
                win->imageDragPending = true;
                win->imageDragElement = hitEl;
                win->linkOnLastButtonDown = nullptr;
                SetCapture(win->hwndCanvas);
                return;
            }
        }
    }

    const char* path = "selStart";
    if (resizeHandle != ResizeHandle::None || isMoveableAnnot || !canCopy || (isShift || !isOverText) && !isCtrl) {
        StartMouseDrag(win, x, y);
        path = "startDrag";
    } else {
        OnSelectionStart(win, x, y, key);
    }
}

static void OnMouseLeftButtonUp(MainWindow* win, int x, int y, WPARAM key) {
    DisplayModel* dm = win->AsFixed();
    ReportIf(!dm);

    if (win->ebookAnnotationDragPending) {
        EbookAnnotation* annotation = win->ebookAnnotationDragPending;
        win->ebookAnnotationDragPending = nullptr;
        win->dragStartPending = false;
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        ShowEditEbookAnnotationsWindow(win->CurrentTab(), annotation);
        return;
    }

    // click on selected text without dragging: clear selection
    if (win->textDragPending) {
        win->textDragPending = false;
        win->dragStartPending = false;
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        DeleteOldSelectionInfo(win, true);
        ScheduleRepaint(win, 0);
        return;
    }

    // click on image without dragging: play media annotation if one is on top, else cancel
    if (win->imageDragPending) {
        win->imageDragPending = false;
        win->imageDragElement = nullptr;
        win->dragStartPending = false;
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        Point pt(x, y);
        WindowTab* tab = win->CurrentTab();
        Annotation* annotAtClick = dm->GetAnnotationAtPos(pt, tab->selectedAnnotation);
        if (annotAtClick && AnnotationSupportsMediaPlayback(annotAtClick->type)) {
            PlaySoundAnnotation(annotAtClick);
            return;
        }
        if (win->showSelection) {
            DeleteOldSelectionInfo(win, true);
            ScheduleRepaint(win, 0);
        }
        return;
    }

    auto ma = win->mouseAction;
    if (ma == MouseAction::OcrRegion) {
        Point endCanvas{x, y};
        win->mouseAction = MouseAction::None;
        win->dragStartPending = false;
        Rect screenRect = NormalizeScreenRect(win->annotCreateDragStart, endCanvas);
        win->selectionRect = Rect();
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        OcrFinishRegionSelect(win, screenRect);
        ScheduleRepaint(win, 0);
        return;
    }
    if (ma == MouseAction::CreatingAnnotation) {
        Point endCanvas{x, y};
        win->mouseAction = MouseAction::None;
        win->dragStartPending = false;
        win->selectionRect = Rect();
        if (GetCapture() == win->hwndCanvas) {
            ReleaseCapture();
        }
        FinishAnnotCreateDrag(win, endCanvas);
        ScheduleRepaint(win, 0);
        return;
    }
    if (MouseAction::None == ma || IsRightDragging(win)) {
        return;
    }

    if (MouseAction::Scrolling == ma) {
        win->mouseAction = MouseAction::None;
        // TODO: I'm seeing this in crash reports. Can we get button up without button down?
        // maybe when down happens on a different hwnd? How can I add more logging.
        // logfa("OnMouseLeftButtonUp: unexpected MouseAction::Scrolling (%d)\n", ma);
        // ReportIf(true);
        return;
    }

    // TODO: should IsDrag() ever be true here? We should get mouse move first
    bool didDragMouse = !win->dragStartPending || IsDragDistance(x, win->dragStart.x, y, win->dragStart.y);
    if (MouseAction::Dragging == ma) {
        if (win->ebookAnnotationBeingDragged && win->annotationBeingResized) {
            StopEbookAnnotationResize(win, !didDragMouse);
            SendMessageW(win->hwndCanvas, WM_SETCURSOR, 0, 0);
        } else if (win->annotationBeingResized) {
            StopAnnotationResize(win, x, y, !didDragMouse);
            // Trigger cursor update after resize
            SendMessageW(win->hwndCanvas, WM_SETCURSOR, 0, 0);
        } else {
            StopMouseDrag(win, x, y, !didDragMouse);
        }
    } else {
        OnSelectionStop(win, x, y, !didDragMouse);
        if (MouseAction::Selecting == ma && win->showSelection) {
            win->selectionMeasure = dm->CvtFromScreen(win->selectionRect).Size();
        }
    }

    win->mouseAction = MouseAction::None;

    if (didDragMouse && win->showSelection && (ma == MouseAction::SelectingText || ma == MouseAction::Selecting)) {
        ShowSelectionToolbar(win);
    }

    Point pt(x, y);
    int pageNo = dm->GetPageNoByPoint(pt);
    PointF ptPage = dm->CvtFromScreen(pt, pageNo);

    // Re-hit-test at click time; linkOnLastButtonDown can dangle when progressive
    // MOBI/EPUB formatting rebuilds page elements between mouse down and up.
    win->linkOnLastButtonDown = nullptr;

    WindowTab* tab = win->CurrentTab();
    if (didDragMouse) {
        // no-op
        return;
    }

    IPageElement* link = nullptr;
    IPageElement* pageEl = dm->GetElementAtPos(pt, nullptr, true);
    if (pageEl && pageEl->Is(kindPageElementDest) && IsPageElementLinkReachable(win->ctrl, pageEl) &&
        pageEl->GetRect().Contains(ptPage)) {
        link = pageEl;
    }

    if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
        /* return from white/black screens in presentation mode */
        win->ChangePresentationMode(PM_ENABLED);
        return;
    }

    if (EbookAnnotationsSupported(tab)) {
        EbookAnnotation* annotation = EbookAnnotationsGetAt(tab, dm, pt);
        if (annotation) {
            ShowEditEbookAnnotationsWindow(tab, annotation);
            return;
        }
    }

    // Fresh hit-test at click time; annotationUnderCursor can be stale while the
    // page is re-rendering after creating or editing an annotation.
    Annotation* annotAtClick = dm->GetAnnotationAtPos(pt, tab->selectedAnnotation);
    if (annotAtClick) {
        if (AnnotationSupportsMediaPlayback(annotAtClick->type)) {
            PlaySoundAnnotation(annotAtClick);
            return;
        }
        if (tab->editAnnotsWindow) {
            SetSelectedAnnotation(tab, annotAtClick);
        } else {
            ShowEditAnnotationsWindow(tab, annotAtClick);
        }
        return;
    }

    if (link && link->GetRect().Contains(ptPage)) {
        /* follow an active link */
        IPageDestination* dest = link->AsLink();
        if (dest && !IsInternalPageLinkReachable(win->ctrl, dest)) {
            return;
        }
        // highlight the clicked link (as a reminder of the last action once the user returns)
        Kind kind = nullptr;
        if (dest) {
            kind = dest->GetKind();
        }
        if ((kindDestinationLaunchURL == kind || kindDestinationLaunchFile == kind)) {
            DeleteOldSelectionInfo(win, true);
            tab->selectionOnPage = SelectionOnPage::FromRectangle(dm, dm->CvtToScreen(pageNo, link->GetRect()));
            win->showSelection = tab->selectionOnPage != nullptr;
            ScheduleRepaint(win, 0);
        }
        SetCursorCached(IDC_ARROW);

        // Ctrl+click on internal link: open in new tab and navigate there
        bool isInternal = (kindDestinationLaunchURL != kind && kindDestinationLaunchFile != kind);
        if (IsCtrlPressed() && dest && isInternal && tab->filePath) {
            LoadArgs args(tab->filePath, win);
            args.showWin = true;
            args.noPlaceWindow = true;
            args.forceReuse = false;
            args.activateExisting = false;
            args.syncLoad = true;
            MainWindow* newWin = LoadDocument(&args);
            if (newWin && newWin->IsDocLoaded()) {
                newWin->linkHandler->ScrollTo(dest);
            }
            return;
        }

        win->ctrl->HandleLink(dest, win->linkHandler);
        return;
    }

    if (win->showSelection) {
        DeleteOldSelectionInfo(win, true);
        ScheduleRepaint(win, 0);
        return;
    }

    if (win->fwdSearchMark.show && gGlobalPrefs->forwardSearch.highlightPermanent) {
        /* if there's a permanent forward search mark, hide it */
        win->fwdSearchMark.show = false;
        ScheduleRepaint(win, 0);
        return;
    }

    if (PM_ENABLED == win->presentation) {
        /* in presentation mode, change pages on left/right-clicks */
        if ((key & MK_SHIFT)) {
            tab->ctrl->GoToPrevPage();
            ReadAloudOnUserViewChanged(win);
        } else {
            tab->ctrl->GoToNextPage();
            ReadAloudOnUserViewChanged(win);
        }
        return;
    }
}

bool gDisableInteractiveInverseSearch = false;

static void OnMouseLeftButtonDblClk(MainWindow* win, int x, int y, WPARAM key) {
    // lf("Left button clicked on %d %d", x, y);
    auto isLeft = bit::IsMaskSet(key, (WPARAM)MK_LBUTTON);
    if (gGlobalPrefs->enableTeXEnhancements && !gDisableInteractiveInverseSearch && isLeft) {
        bool dontSelect = OnInverseSearch(win, x, y);
        if (dontSelect) {
            return;
        }
    }

    DisplayModel* dm = win->AsFixed();
    // note: before 3.5 double-click used to turn 2 pages
    // OnMouseLeftButtonDown(win, x, y, key);
    Point mousePos = Point(x, y);
    // Double-click may load text on demand (same as single-click selection).
    bool isOverText = dm->IsOverText(mousePos, true);

    if (isLeft && (win->presentation || win->isFullScreen)) {
        // in fullscreen we allow to exit by tapping in upper right corner
        constexpr int kCornerSize = 64;
        Rect r = ClientRect(win->hwndCanvas);
        if (!isOverText && (x >= (r.dx - kCornerSize)) && (y < kCornerSize)) {
            ExitFullScreen(win);
            return;
        }
    }

    int elementPageNo = -1;
    IPageElement* pageEl = dm->GetElementAtPos(mousePos, &elementPageNo, true);
    if (isOverText && gGlobalPrefs->enableDoubleClickWordLookup) {
        SuspendFindEngineAccess(win);
        int pageNo = dm->GetPageNoByPoint(mousePos);
        if (win->ctrl->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(mousePos, pageNo);
            if (EngineIsFixedLayoutEbook(dm->GetEngine()) && ShowEbookWordLookupAt(win, dm, pageNo, pt, mousePos)) {
                UpdateTextSelection(win, false);
                ScheduleRepaint(win, 0);
                return;
            }
            if (ShowChineseWordLookupAt(win, dm->textSelection, dm->GetEngine(), pageNo, pt, mousePos)) {
                UpdateTextSelection(win, false);
                ScheduleRepaint(win, 0);
                return;
            }
            dm->textSelection->SelectWordAt(pageNo, pt.x, pt.y);
            UpdateTextSelection(win, false);
            ScheduleRepaint(win, 0);
            bool isTextOnly = false;
            TempStr selWord = GetSelectedTextTemp(win->CurrentTab(), "\r\n", isTextOnly);
            if (isTextOnly && selWord) {
                ShowWordLookup(win, selWord, mousePos);
            }
        }
        return;
    }

    if (!pageEl) {
        if (isLeft && !isOverText && !win->presentation) {
            ReadAloudHandleCanvasDoubleClick(win);
        }
        return;
    }
    if (pageEl->Is(kindPageElementDest)) {
        if (!IsPageElementLinkReachable(win->ctrl, pageEl)) {
            return;
        }
        // speed up navigation in a file where navigation links are in a fixed position
        OnMouseLeftButtonDown(win, x, y, key);
    } else if (pageEl->Is(kindPageElementImage)) {
        // select an image that could be copied to the clipboard
        Rect rc = dm->CvtToScreen(elementPageNo, pageEl->GetRect());

        // an image covering (almost) the whole page is likely a scanned page or
        // a background; treat it like empty area i.e. toggle read-aloud
        Rect rcPage = dm->CvtToScreen(elementPageNo, dm->GetEngine()->PageMediabox(elementPageNo));
        i64 imgArea = (i64)rc.dx * (i64)rc.dy;
        i64 pageArea = (i64)rcPage.dx * (i64)rcPage.dy;
        // full-bleed image: same as empty canvas for read-aloud pause/continue
        if (pageArea > 0 && imgArea * 100 >= pageArea * 85) {
            if (isLeft && !isOverText && !win->presentation) {
                ReadAloudHandleCanvasDoubleClick(win);
            }
            return;
        }

        DeleteOldSelectionInfo(win, true);
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(dm, rc);
        win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
        ScheduleRepaint(win, 0);
    }
}

static void OnMouseMiddleButtonDown(MainWindow* win, int x, int y, WPARAM) {
    // Handle message by recording placement then moving document as mouse moves.

    switch (win->mouseAction) {
        case MouseAction::None:
            win->mouseAction = MouseAction::Scrolling;

            win->dragStartPending = true;
            // record current mouse position, the farther the mouse is moved
            // from this position, the faster we scroll the document
            win->dragStart = Point(x, y);
            SetCursorCached(IDC_SIZEALL);
            break;

        case MouseAction::Scrolling:
            win->mouseAction = MouseAction::None;
            break;
    }
}

static void OnMouseMiddleButtonUp(MainWindow* win, int x, int y, WPARAM) {
    switch (win->mouseAction) {
        case MouseAction::Scrolling:
            if (!win->dragStartPending) {
                win->mouseAction = MouseAction::None;
                SetCursorCached(IDC_ARROW);
                break;
            }
    }
}

static void OnMouseRightButtonDown(MainWindow* win, int x, int y) {
    // lf("Right button clicked on %d %d", x, y);
    if (MouseAction::Scrolling == win->mouseAction) {
        win->mouseAction = MouseAction::None;
    } else if (win->mouseAction != MouseAction::None) {
        return;
    }
    ReportIf(!win->AsFixed());

    HwndSetFocus(win->hwndFrame);

    win->dragStartPending = true;
    win->dragStart = Point(x, y);

    StartMouseDrag(win, x, y, true);
}

static void OnMouseRightButtonUp(MainWindow* win, int x, int y, WPARAM key) {
    ReportIf(!win->AsFixed());
    if (!IsRightDragging(win)) {
        return;
    }

    int isDragXOrY = IsDragDistance(x, win->dragStart.x, y, win->dragStart.y);
    bool didDragMouse = !win->dragStartPending || isDragXOrY;
    StopMouseDrag(win, x, y, !didDragMouse);

    win->mouseAction = MouseAction::None;

    if (didDragMouse) {
        /* pass */;
    } else if (PM_ENABLED == win->presentation) {
        if ((key & MK_CONTROL)) {
            OnWindowContextMenu(win, x, y);
        } else if ((key & MK_SHIFT)) {
            win->ctrl->GoToNextPage();
            ReadAloudOnUserViewChanged(win);
        } else {
            win->ctrl->GoToPrevPage();
            ReadAloudOnUserViewChanged(win);
        }
    }
    /* return from white/black screens in presentation mode */
    else if (PM_BLACK_SCREEN == win->presentation || PM_WHITE_SCREEN == win->presentation) {
        win->ChangePresentationMode(PM_ENABLED);
    } else {
        OnWindowContextMenu(win, x, y);
    }
}

static void OnMouseRightButtonDblClick(MainWindow* win, int x, int y, WPARAM key) {
    if (win->presentation && !(key & ~MK_RBUTTON)) {
        // in presentation mode, right clicks turn the page,
        // make two quick right clicks (AKA one double-click) turn two pages
        OnMouseRightButtonDown(win, x, y);
        return;
    }
}

#ifdef DRAW_PAGE_SHADOWS
#define BORDER_SIZE 1
#define SHADOW_OFFSET 4
static void PaintPageFrameAndShadow(HDC hdc, Rect& bounds, Rect& pageRect, bool presentation, COLORREF colDocBg) {
    // Frame info
    Rect frame = bounds;
    frame.Inflate(BORDER_SIZE, BORDER_SIZE);

    // Shadow info
    Rect shadow = frame;
    shadow.Offset(SHADOW_OFFSET, SHADOW_OFFSET);
    if (frame.x < 0) {
        // the left of the page isn't visible, so start the shadow at the left
        int diff = std::min(-pageRect.x, SHADOW_OFFSET);
        shadow.x -= diff;
        shadow.dx += diff;
    }
    if (frame.y < 0) {
        // the top of the page isn't visible, so start the shadow at the top
        int diff = std::min(-pageRect.y, SHADOW_OFFSET);
        shadow.y -= diff;
        shadow.dy += diff;
    }

    // Draw shadow
    if (!presentation) {
        AutoDeleteBrush brush = CreateSolidBrush(COL_PAGE_SHADOW);
        FillRect(hdc, &shadow.ToRECT(), brush);
    }

    // Draw frame
    ScopedGdiObj<HPEN> pe(CreatePen(PS_SOLID, 1, presentation ? TRANSPARENT : COL_PAGE_FRAME));
    AutoDeleteBrush brush = CreateSolidBrush(ThemeMainWindowBackgroundColor());
    SelectObject(hdc, pe);
    SelectObject(hdc, brush);
    Rectangle(hdc, frame.x, frame.y, frame.x + frame.dx, frame.y + frame.dy);
}
#else
static void PaintPageFrameAndShadow(HDC hdc, Rect& bounds, Rect&, bool, COLORREF colDocBg) {
    AutoDeletePen pen(CreatePen(PS_NULL, 0, 0));
    AutoDeleteBrush brush(CreateSolidBrush(colDocBg));
    ScopedSelectPen restorePen(hdc, pen);
    ScopedSelectObject restoreBrush(hdc, brush);
    Rectangle(hdc, bounds.x, bounds.y, bounds.x + bounds.dx + 1, bounds.y + bounds.dy + 1);
}
#endif

/* debug code to visualize links (can block while rendering) */
static void PaintUnreachablePageLinks(MainWindow* win, HDC hdc, DisplayModel* dm) {
    if (!win || !win->ctrl || !dm) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine || !EngineIsProgressiveEbookLoading(engine)) {
        return;
    }

    Rect viewPortRect(Point(), dm->GetViewPort().Size());
    COLORREF disabledCol = ThemeReadingTextDisabledColor();
    Gdiplus::Graphics gs(hdc);
    Gdiplus::SolidBrush brush(
        Gdiplus::Color(160, GetRValue(disabledCol), GetGValue(disabledCol), GetBValue(disabledCol)));

    for (int pageNo = 1; pageNo <= dm->PageCount(); pageNo++) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || !pi->isShown || pi->visibleRatio == 0.0) {
            continue;
        }
        Vec<IPageElement*> els = engine->GetElements(pageNo);
        for (IPageElement* el : els) {
            if (!el || !el->Is(kindPageElementDest)) {
                continue;
            }
            if (IsPageElementLinkReachable(win->ctrl, el)) {
                continue;
            }
            Rect rect = dm->CvtToScreen(pageNo, el->GetRect());
            Rect isect = viewPortRect.Intersect(rect);
            if (isect.IsEmpty()) {
                continue;
            }
            gs.FillRectangle(&brush, (Gdiplus::REAL)isect.x, (Gdiplus::REAL)isect.y, (Gdiplus::REAL)isect.dx,
                             (Gdiplus::REAL)isect.dy);
        }
    }
}

static void DebugShowLinks(DisplayModel* dm, HDC hdc) {
    if (!gGlobalPrefs->showLinks) {
        return;
    }

    Rect viewPortRect(Point(), dm->GetViewPort().Size());

    ScopedSelectObject autoPen(hdc, CreatePen(PS_SOLID, 1, RGB(0x00, 0x00, 0xff)), true);

    for (int pageNo = dm->PageCount(); pageNo >= 1; --pageNo) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || !pi->isShown || 0.0 == pi->visibleRatio) {
            continue;
        }

        Vec<IPageElement*> els = dm->GetEngine()->GetElements(pageNo);

        for (auto& el : els) {
            if (el->Is(kindPageElementImage)) {
                continue;
            }
            Rect rect = dm->CvtToScreen(pageNo, el->GetRect());
            Rect isect = viewPortRect.Intersect(rect);
            if (!isect.IsEmpty()) {
                isect.Inflate(2, 2);
                DrawRect(hdc, isect);
            }
        }
    }

    if (false && dm->GetZoomVirtual() == kZoomFitContent) {
        // also display the content box when fitting content
        for (int pageNo = dm->PageCount(); pageNo >= 1; --pageNo) {
            PageInfo* pi = dm->GetPageInfo(pageNo);
            if (!pi->isShown || 0.0 == pi->visibleRatio) {
                continue;
            }

            auto cbbox = dm->GetEngine()->PageContentBox(pageNo);
            Rect rect = dm->CvtToScreen(pageNo, cbbox);
            DrawRect(hdc, rect);
        }
    }
}

// cf. https://web.archive.org/web/20140201011540/http://forums.fofou.org/sumatrapdf/topic?id=3183580&comments=15
static void GetGradientColor(COLORREF a, COLORREF b, float perc, TRIVERTEX* tv) {
    u8 ar, ag, ab;
    u8 br, bg, bb;
    UnpackColor(a, ar, ag, ab);
    UnpackColor(b, br, bg, bb);

    tv->Red = (COLOR16)((ar + perc * (br - ar)) * 256);
    tv->Green = (COLOR16)((ag + perc * (bg - ag)) * 256);
    tv->Blue = (COLOR16)((ab + perc * (bb - ab)) * 256);
}

// Draw a border around selected annotation
static bool gDrawOldStyleAnnotationRect = false;

NO_INLINE static void PaintCurrentEditAnnotationMark(WindowTab* tab, HDC hdc, DisplayModel* dm) {
    if (!tab || !tab->editAnnotsWindow) {
        return;
    }
    Annotation* annot = tab->selectedAnnotation;
    if (!annot) {
        return;
    }
    int pageNo = annot->pageNo;
    if (!dm->PageVisible(pageNo)) {
        // CvtToScreen() might not work if page is not visible because
        // it might not have zoom etc. calculated yet
        return;
    }
    bool canResize = AnnotationCanBeResized(annot->type);

    Rect rect = dm->CvtToScreen(pageNo, GetRect(annot));
    if (!tab->didScrollToSelectedAnnotation) {
        dm->ScrollScreenToRect(pageNo, rect);
        tab->didScrollToSelectedAnnotation = true;
    }
    rect.Inflate(4, 4);

    Gdiplus::Graphics gs(hdc);

    if (gDrawOldStyleAnnotationRect) {
        Gdiplus::Color col = GdiRgbFromCOLORREF(0xff3333); // blue
        Gdiplus::Color colHatch2((Gdiplus::ARGB)Gdiplus::Color::Yellow);
        Gdiplus::HatchBrush br(Gdiplus::HatchStyleCross, colHatch2, col);
        Gdiplus::Pen pen(&br, 4);
        gs.DrawRectangle(&pen, rect.x, rect.y, rect.dx, rect.dy);
    } else {
        Gdiplus::Color blue(255, 0, 80, 200);
        Gdiplus::Pen pen(blue, 2);
        pen.SetDashStyle(Gdiplus::DashStyleDot);
        gs.DrawRectangle(&pen, rect.x, rect.y, rect.dx, rect.dy);
    }

    if (!canResize) {
        return;
    }

    // Draw resize handles
    Gdiplus::SolidBrush handleBrush(Gdiplus::Color(255, 255, 255, 255)); // White
    Gdiplus::Pen handlePen(Gdiplus::Color(255, 0, 0, 0), 1);             // Black
    int hs = 6;                                                          // handle size
    int hh = hs / 2;                                                     // half handle

    int left = rect.x - hh;
    int midX = rect.x + rect.dx / 2 - hh;
    int right = rect.x + rect.dx - hh;
    int top = rect.y - hh;
    int midY = rect.y + rect.dy / 2 - hh;
    int bottom = rect.y + rect.dy - hh;

    auto drawHandle = [&](int x, int y) {
        gs.FillRectangle(&handleBrush, x, y, hs, hs);
        gs.DrawRectangle(&handlePen, x, y, hs, hs);
    };

    // corners
    drawHandle(left, top);
    drawHandle(right, top);
    drawHandle(right, bottom);
    drawHandle(left, bottom);
    // edges
    drawHandle(midX, top);
    drawHandle(right, midY);
    drawHandle(midX, bottom);
    drawHandle(left, midY);
}

static bool DrawDocument(MainWindow* win, HDC hdc, RECT* rcArea) {
    ReportIf(!win->AsFixed());
    if (!win->AsFixed()) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    // logf("DrawDocument RenderCache:\n");

    auto* engine = dm->GetEngine();
    bool isImage = engine->IsImageCollection();
    // draw comic books and single images on a black background
    // (without frame and shadow)
    bool paintOnBlackWithoutShadow = win->presentation || isImage;
    bool isEbook = engine->kind == kindEngineMupdf && !str::EqI(engine->defaultExt, ".pdf");
    bool isReflowableEbook = isEbook || engine->kind == kindEngineMobi || engine->kind == kindEngineEpub ||
                             engine->kind == kindEngineFb2 || engine->kind == kindEnginePdb ||
                             engine->kind == kindEngineHtml || engine->kind == kindEngineTxt;
    bool isPdf =
        (engine->kind == kindEngineMupdf && str::EqI(engine->defaultExt, ".pdf")) || engine->kind == kindEngineDjVu;
    COLORREF colDocBg;
    COLORREF colDocTxt = ThemeDocumentColors(colDocBg);
    if (isImage) {
        colDocBg = 0x0;
        colDocTxt = 0xffffff;
        // allow ComicBookUI/ImageUI WindowBgCol to override the default black
        ParsedColor* bgOverride = nullptr;
        if (engine->kind == kindEngineComicBooks) {
            bgOverride = GetPrefsColor(gGlobalPrefs->comicBookUI.windowBgCol);
        } else {
            bgOverride = GetPrefsColor(gGlobalPrefs->imageUI.windowBgCol);
        }
        if (bgOverride->parsedOk) {
            colDocBg = bgOverride->col;
        }
    } else if (isReflowableEbook) {
        if (engine->kind == kindEngineMobi) {
            // Match EngineEbook::RenderPage so canvas margins do not contrast with tiles.
            PdfDocumentColorMode docMode = GetPdfDocumentColorMode();
            if (docMode == PdfDocumentColorMode::Light) {
                colDocBg = RgbToCOLORREF(0xFFFFFF);
            } else if (IsDarkThemeSelected()) {
                ThemePageRenderColors(colDocBg, true);
            } else if (docMode != PdfDocumentColorMode::Light && !ThemeUsesOriginalPageColors()) {
                colDocBg = RgbToCOLORREF(0xF7F3E8);
            } else {
                colDocBg = RgbToCOLORREF(0xFFFFFF);
            }
        } else {
            PdfDocumentColorMode docMode = GetPdfDocumentColorMode();
            ParsedColor* bgOverride = GetPrefsColor(gGlobalPrefs->eBookUI.windowBgCol);
            if (bgOverride->parsedOk) {
                colDocBg = bgOverride->col;
            } else if (docMode == PdfDocumentColorMode::Light) {
                colDocBg = RgbToCOLORREF(0xFFFFFF);
            } else if (IsDarkThemeSelected()) {
                ThemePageRenderColors(colDocBg, true);
            } else if (!ThemeUsesOriginalPageColors()) {
                colDocBg = RgbToCOLORREF(0xF7F3E8);
            } else {
                colDocBg = RgbToCOLORREF(0xFFFFFF);
            }
        }
    } else if (isPdf) {
        ParsedColor* bgOverride = GetPrefsColor(gGlobalPrefs->fixedPageUI.windowBgCol);
        if (bgOverride->parsedOk) {
            colDocBg = bgOverride->col;
        }
    }

    // per-document background color from FileState overrides everything
    WindowTab* curTab = win->CurrentTab();
    if (curTab && curTab->bgColorCheckered) {
        colDocBg = kColorUnset;
    } else if (curTab && curTab->bgColor != kColorUnset) {
        colDocBg = curTab->bgColor;
    }

    bool shouldPaint = false;
    auto* gcols = gGlobalPrefs->fixedPageUI.gradientColors;
    auto nGCols = gcols->size();
    auto paintBgOrCheckerboard = [&](COLORREF col, RECT* rc) {
        if (col == kColorUnset) {
            PaintCheckerboard(hdc, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top);
        } else {
            AutoDeleteBrush brush = CreateSolidBrush(col);
            FillRect(hdc, rc, brush);
        }
    };

    // Always clear the full back-buffer first. The buffer is shared across tabs
    // and Flush() only blits ps.rcPaint to the screen; partial DrawDocument
    // updates used to leave a previous PDF page-frame row at a different Y than
    // the current EPUB layout when Flush copied the whole buffer.
    Rect canvas = win->canvasRc;
    if (!canvas.IsEmpty()) {
        RECT fullRc = ToRECT(canvas);
        paintBgOrCheckerboard(colDocBg, &fullRc);
    } else {
        paintBgOrCheckerboard(colDocBg, rcArea);
    }
    shouldPaint = true;

    if (!paintOnBlackWithoutShadow && colDocBg != kColorUnset && nGCols > 0) {
        COLORREF colors[3];
        colors[0] = ParseColor(gcols->at(0), WIN_COL_WHITE);
        if (nGCols == 1) {
            colors[1] = colors[2] = colors[0];
        } else if (nGCols == 2) {
            colors[2] = ParseColor(gcols->at(1), WIN_COL_WHITE);
            colors[1] =
                RGB((GetRed(colors[0]) + GetRed(colors[2])) / 2, (GetGreen(colors[0]) + GetGreen(colors[2])) / 2,
                    (GetBlue(colors[0]) + GetBlue(colors[2])) / 2);
        } else {
            colors[1] = ParseColor(gcols->at(1), WIN_COL_WHITE);
            colors[2] = ParseColor(gcols->at(2), WIN_COL_WHITE);
        }
        Size size = dm->GetCanvasSize();
        float percTop = 1.0F * dm->GetViewPort().y / size.dy;
        float percBot = 1.0F * dm->GetViewPort().BR().y / size.dy;
        if (!IsContinuous(dm->GetDisplayMode())) {
            percTop += dm->CurrentPageNo() - 1;
            percTop /= dm->PageCount();
            percBot += dm->CurrentPageNo() - 1;
            percBot /= dm->PageCount();
        }
        Size vp = dm->GetViewPort().Size();
        TRIVERTEX tv[4] = {{0, 0}, {vp.dx, vp.dy / 2}, {0, vp.dy / 2}, {vp.dx, vp.dy}};
        GRADIENT_RECT gr[2] = {{0, 1}, {2, 3}};

        COLORREF col0 = colors[0];
        COLORREF col1 = colors[1];
        COLORREF col2 = colors[2];
        if (percTop < 0.5F) {
            GetGradientColor(col0, col1, 2 * percTop, &tv[0]);
        } else {
            GetGradientColor(col1, col2, 2 * (percTop - 0.5F), &tv[0]);
        }

        if (percBot < 0.5f) {
            GetGradientColor(col0, col1, 2 * percBot, &tv[3]);
        } else {
            GetGradientColor(col1, col2, 2 * (percBot - 0.5F), &tv[3]);
        }

        bool needCenter = percTop < 0.5F && percBot > 0.5F;
        if (needCenter) {
            GetGradientColor(col1, col1, 0, &tv[1]);
            GetGradientColor(col1, col1, 0, &tv[2]);
            tv[1].y = tv[2].y = (LONG)((0.5F - percTop) / (percBot - percTop) * vp.dy);
        } else {
            gr[0].LowerRight = 3;
        }
        // TODO: disable for less than about two screen heights?
        ULONG nMesh = 1;
        if (needCenter) {
            nMesh = 2;
        }
        GradientFill(hdc, tv, dimof(tv), gr, nMesh, GRADIENT_FILL_RECT_V);
    }

    bool rendering = false;
    Rect screen(Point(), dm->GetViewPort().Size());
    int firstFrameY = -1;
    int visiblePageCount = 0;

    bool isRtl = IsUIRtl();
    int paintFrom = 1;
    int paintTo = dm->PageCount();
    if (dm->visibleScanTo >= dm->visibleScanFrom && dm->visibleScanFrom >= 1) {
        paintFrom = dm->visibleScanFrom;
        paintTo = dm->visibleScanTo;
    }
    for (int pageNo = paintFrom; pageNo <= paintTo; ++pageNo) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || 0.0F == pi->visibleRatio) {
            continue;
        }
        visiblePageCount++;
        if (firstFrameY < 0) {
            firstFrameY = pi->pageOnScreen.y;
        }
        ReportIf(!pi->isShown);
        if (!pi->isShown) {
            continue;
        }

        Rect bounds = pi->pageOnScreen.Intersect(screen);
        // don't paint the frame background for images
        // Reflowable ebooks paint edge-to-edge; a PDF-style page frame leaves a bright
        // row when theme/tiles disagree (e.g. MOBI dark tiles under light margins).
        if (!isReflowableEbook) {
            Rect r = pi->pageOnScreen;
            auto presMode = win->presentation;
            PaintPageFrameAndShadow(hdc, bounds, r, presMode, colDocBg);
        }

        // check if this page is known to have failed rendering
        if (pi->failedToRender) {
            shouldPaint = true;
            HFONT fontRightTxt = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
            HGDIOBJ hPrevFont = SelectObject(hdc, fontRightTxt);
            auto prevCol = SetTextColor(hdc, colDocTxt);
            DrawCenteredText(hdc, bounds, _TRA("Couldn't render the page"), isRtl);
            SetTextColor(hdc, prevCol);
            SelectObject(hdc, hPrevFont);
            continue;
        }

        bool renderOutOfDateCue = false;
        int renderDelay = gRenderCache->Paint(hdc, bounds, dm, pageNo, pi, &renderOutOfDateCue);
        if (renderDelay == 0) {
            shouldPaint = true;
        }
        if (renderDelay != 0) {
            HFONT fontRightTxt = CreateSimpleFont(hdc, "MS Shell Dlg", 14);
            HGDIOBJ hPrevFont = SelectObject(hdc, fontRightTxt);
            if (renderDelay != RENDER_DELAY_FAILED) {
                if (renderDelay < REPAINT_MESSAGE_DELAY_IN_MS) {
                    ScheduleRepaint(win, REPAINT_MESSAGE_DELAY_IN_MS / 4);
                } else {
                    SetTextColor(hdc, colDocTxt);
                    DrawCenteredText(hdc, bounds, _TRA("Please wait - rendering..."), isRtl);
                }
                rendering = true;
            } else {
                shouldPaint = true;
                auto prevCol = SetTextColor(hdc, colDocTxt);
                DrawCenteredText(hdc, bounds, _TRA("Couldn't render the page"), isRtl);
                SetTextColor(hdc, prevCol);
            }
            SelectObject(hdc, hPrevFont);
            continue;
        }

        EbookAnnotationsPaintPage(win->CurrentTab(), hdc, dm, pageNo);
        PaintPdfMarkupOverlayPage(win->CurrentTab(), hdc, dm, pageNo);
        if (win->CurrentTab() && !gRenderCache->PageNeedsMarkupOverlay(dm, pageNo)) {
            ClearPdfMarkupOverlayForPage(win->CurrentTab(), pageNo);
        }

        if (!renderOutOfDateCue) {
            continue;
        }

        HDC bmpDC = CreateCompatibleDC(hdc);
        if (!bmpDC) {
            continue;
        }
        SelectObject(bmpDC, gBitmapReloadingCue);
        int size = DpiScale(win->hwndFrame, 16);
        int cx = std::min(bounds.dx, 2 * size);
        int cy = std::min(bounds.dy, 2 * size);
        int x = bounds.x + bounds.dx - std::min((cx + size) / 2, cx);
        int y = bounds.y + std::max((cy - size) / 2, 0);
        int dxDest = std::min(cx, size);
        int dyDest = std::min(cy, size);
        StretchBlt(hdc, x, y, dxDest, dyDest, bmpDC, 0, 0, 16, 16, SRCCOPY);
        DeleteDC(bmpDC);
    }

    WindowTab* tab = win->CurrentTab();
    PaintCurrentEditAnnotationMark(tab, hdc, dm);

    // draw highlight rectangle around element under cursor during context menu
    if (win->contextMenuHighlightPageNo > 0 && dm->PageVisible(win->contextMenuHighlightPageNo)) {
        Rect rc = dm->CvtToScreen(win->contextMenuHighlightPageNo, win->contextMenuHighlightRect);
        Gdiplus::Graphics gs(hdc);
        Gdiplus::Color col(128, 0, 100, 255);
        Gdiplus::Pen pen(col, 2);
        gs.DrawRectangle(&pen, rc.x, rc.y, rc.dx, rc.dy);
    }

    PaintAnnotCreatePreview(win, dm, hdc);

    PaintAllFindMatches(win, hdc);
    // Search matches and the user's text selection are independent layers.
    // The latter must still be painted when all find matches are visible.
    if (win->showSelection) {
        PaintSelection(win, hdc);
    }
    // keep the floating selection toolbar aligned with the selection while
    // scrolling/zooming; hides itself when the selection is gone or off-screen
    UpdateSelectionToolbarPosition(win);

    PaintReadAloudHighlight(win, hdc);

    if (win->fwdSearchMark.show) {
        PaintForwardSearchMark(win, hdc);
    }

    if (!rendering) {
        PaintUnreachablePageLinks(win, hdc, dm);
        DebugShowLinks(dm, hdc);
    }
    return shouldPaint;
}

static void OnPaintDocument(MainWindow* win) {
    auto t = TimeGet();
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(win->hwndCanvas, &ps);

    if (IsSidebarSplitterLiveDrag()) {
        // Always cover the update region. Validating without a fill left
        // black/white ghosts; a 48px-only fill missed fast splitter moves.
        HBRUSH br = CreateSolidBrush(ThemeMainWindowBackgroundColor());
        FillRect(hdc, &ps.rcPaint, br);
        DeleteObject(br);
        EndPaint(win->hwndCanvas, &ps);
        return;
    }

    switch (win->presentation) {
        case PM_BLACK_SCREEN:
            FillRect(hdc, &ps.rcPaint, GetStockBrush(BLACK_BRUSH));
            break;
        case PM_WHITE_SCREEN:
            FillRect(hdc, &ps.rcPaint, GetStockBrush(WHITE_BRUSH));
            break;
        default:
            bool shouldPaint = DrawDocument(win, win->buffer->GetDC(), &ps.rcPaint);
            if (!gNoFlickerRender || shouldPaint) {
                win->buffer->Flush(hdc, &ps.rcPaint);
            }
    }

    EndPaint(win->hwndCanvas, &ps);
    if (gShowFrameRate) {
        win->frameRateWnd->ShowFrameRateDur(TimeSinceInMs(t));
    }
}

static void SetTextOrArrorCursor(DisplayModel* dm, Point pt) {
    if (dm->IsOverText(pt)) {
        SetCursorCached(IDC_IBEAM);
    } else {
        SetCursorCached(IDC_ARROW);
    }
}

// TODO: this gets called way too often
static LRESULT OnSetCursorMouseNone(MainWindow* win, HWND hwnd) {
    DisplayModel* dm = win->AsFixed();
    Point pt = HwndGetCursorPos(hwnd);
    if (!dm || !GetCursor() || pt.IsEmpty()) {
        win->DeleteToolTip();
        return FALSE;
    }
    if (win->annotCreateToolCmd != 0 || win->ocrRegionPending) {
        SetCursorCached(IDC_CROSS);
        return TRUE;
    }
    if (GetNotificationForGroup(win->hwndCanvas, kNotifCursorPos)) {
        SetCursorCached(IDC_CROSS);
        return TRUE;
    }

    WindowTab* tab = win->CurrentTab();
    Annotation* selected = tab->selectedAnnotation;

    ResizeHandle ebookHandle = GetEbookResizeHandleAt(win, pt, tab->selectedEbookAnnotation);
    if (ebookHandle != ResizeHandle::None) {
        SetCursorCached(GetCursorForResizeHandle(ebookHandle));
        return TRUE;
    }

    if (EbookAnnotationsSupported(tab) && EbookAnnotationsHitTest(tab, dm, pt)) {
        SetCursorCached(IDC_HAND);
        return TRUE;
    }

    // Check if hovering over resize handle of selected annotation
    if (selected && AnnotationCanBeResized(selected->type)) {
        ResizeHandle handle = GetResizeHandleAt(win, pt, selected);
        if (handle != ResizeHandle::None) {
            SetCursorCached(GetCursorForResizeHandle(handle));
            return TRUE;
        }
    }

    Annotation* annot = dm->GetAnnotationAtPos(pt, selected);
    if (annot) {
        SetCursorCached(IDC_HAND);
        return TRUE;
    }

    int pageNo = 0;
    // EPUB rendering deliberately skips expensive link extraction. Load it on
    // the first hover for this page, then use the cached hit-test data so links
    // get the hand cursor before they are clicked.
    IPageElement* pageEl = dm->GetElementAtPos(pt, &pageNo, true);
    if (!pageEl) {
        SetTextOrArrorCursor(dm, pt);
        win->DeleteToolTip();
        return TRUE;
    }
    char* text = pageEl->GetValue();
    if (!dm->ValidPageNo(pageNo)) {
        const char* kind = pageEl->GetKind();
        logf("OnSetCursorMouseIdle: page element '%s' of kind '%s' on invalid page %d\n", text, kind, pageNo);
        ReportIf(true);
        return TRUE;
    }
    auto r = pageEl->GetRect();
    Rect rc = dm->CvtToScreen(pageNo, r);
    win->ShowToolTip(text, rc, true);

    bool isLink = pageEl->Is(kindPageElementDest);
    bool linkReachable = !isLink || IsPageElementLinkReachable(win->ctrl, pageEl);

    if (isLink && linkReachable) {
        SetCursorCached(IDC_HAND);
    } else {
        SetTextOrArrorCursor(dm, pt);
    }
    return TRUE;
}

static LRESULT OnSetCursor(MainWindow* win, HWND hwnd) {
    ReportIf(win->hwndCanvas != hwnd);
    if (win->mouseAction != MouseAction::None) {
        win->DeleteToolTip();
    }

    switch (win->mouseAction) {
        case MouseAction::Dragging:
            if (win->annotationBeingResized) {
                SetCursorCached(GetCursorForResizeHandle((ResizeHandle)win->resizeHandle));
            } else {
                SetCursor(gCursorDrag);
            }
            return TRUE;
        case MouseAction::Scrolling:
            SetCursorCached(IDC_SIZEALL);
            return TRUE;
        case MouseAction::SelectingText:
            SetCursorCached(IDC_IBEAM);
            return TRUE;
        case MouseAction::Selecting:
            break;
        case MouseAction::CreatingAnnotation:
        case MouseAction::OcrRegion:
            SetCursorCached(IDC_CROSS);
            return TRUE;
        case MouseAction::None:
            return OnSetCursorMouseNone(win, hwnd);
    }
    return win->presentation ? TRUE : FALSE;
}

float ScaleZoomBy(MainWindow* win, float factor) {
    auto zoomVirt = win->ctrl->GetZoomVirtual(true);
    return factor * zoomVirt;
}

static bool gWheelZoomRelative = true;

constexpr float kWheelZoomPerNotch = 1.10f;
constexpr float kWheelZoomMaxNotchesPerMsg = 2.f;
constexpr float kWheelZoomVelocityMs = 300.f;
constexpr UINT kWheelZoomDebounceMs = 32;

static float WheelZoomFactorFromDelta(short delta, DWORD msgTime, DWORD* lastMsgTime) {
    float notches = (float)delta / (float)WHEEL_DELTA;
    notches = limitValue(notches, -kWheelZoomMaxNotchesPerMsg, kWheelZoomMaxNotchesPerMsg);
    float factor = powf(kWheelZoomPerNotch, notches);

    double elapsedMs = 100.0;
    if (*lastMsgTime != 0) {
        DWORD dt = msgTime - *lastMsgTime;
        if (dt < 500) {
            elapsedMs = (double)dt;
        }
    }
    *lastMsgTime = msgTime;

    float maxFactor = powf(2.f, (float)elapsedMs / kWheelZoomVelocityMs);
    maxFactor = limitValue(maxFactor, 1.04f, 2.f);
    if (factor > maxFactor) {
        factor = maxFactor;
    } else if (factor < 1.f / maxFactor) {
        factor = 1.f / maxFactor;
    }
    return factor;
}

static void AccumulateWheelZoom(MainWindow* win, WPARAM wp, Point pt, DWORD msgTime) {
    if (!win->ctrl) {
        return;
    }
    short delta = GET_WHEEL_DELTA_WPARAM(wp);
    float factor = WheelZoomFactorFromDelta(delta, msgTime, &win->wheelZoomLastMsgTime);
    float currZoom = win->ctrl->GetZoomVirtual(true);
    float base = win->wheelZoomPending ? win->wheelZoomTarget : currZoom;
    float newZoom = limitValue(base * factor, kZoomMin, kZoomMax);
    win->wheelZoomTarget = newZoom;
    win->wheelZoomPt = pt;
    win->wheelZoomPending = true;
    win->wheelZoomCoalesced++;
    ShowZoomNotification(win, newZoom);
}

static int DrainQueuedWheelZoom(MainWindow* win) {
    int n = 0;
    MSG msg;
    while (PeekMessage(&msg, nullptr, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_NOREMOVE)) {
        if (msg.hwnd && msg.hwnd != win->hwndCanvas && msg.hwnd != win->hwndFrame) {
            break;
        }
        bool isZoom = (LOWORD(msg.wParam) & MK_CONTROL) || (LOWORD(msg.wParam) & MK_RBUTTON);
        if (!isZoom) {
            break;
        }
        PeekMessage(&msg, nullptr, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE);
        Point pt{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
        HwndScreenToClient(win->hwndCanvas, pt);
        AccumulateWheelZoom(win, msg.wParam, pt, msg.time);
        n++;
    }
    return n;
}

void CancelPendingWheelZoom(MainWindow* win) {
    if (!win || win->wheelZoomApplying) {
        return;
    }
    win->wheelZoomPending = false;
    win->wheelZoomCoalesced = 0;
    if (win->hwndCanvas) {
        KillTimer(win->hwndCanvas, kWheelZoomTimerID);
    }
}

static void ApplyPendingWheelZoom(MainWindow* win) {
    if (!win || !IsMainWindowValid(win) || win->isBeingClosed || !win->IsDocLoaded()) {
        CancelPendingWheelZoom(win);
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        DrainQueuedWheelZoom(win);
        if (!win->wheelZoomPending) {
            KillTimer(win->hwndCanvas, kWheelZoomTimerID);
            return;
        }
        float newZoom = win->wheelZoomTarget;
        Point pt = win->wheelZoomPt;
        win->wheelZoomPending = false;
        win->wheelZoomCoalesced = 0;
        KillTimer(win->hwndCanvas, kWheelZoomTimerID);

        win->wheelZoomApplying = true;
        SmartZoom(win, newZoom, &pt, false);
        win->wheelZoomApplying = false;
    }
    DrainQueuedWheelZoom(win);
    if (win->wheelZoomPending) {
        SetTimer(win->hwndCanvas, kWheelZoomTimerID, kWheelZoomDebounceMs, nullptr);
    }
}

// Ctrl+wheel / right-button+wheel zoom. One WHEEL_DELTA notch is ~10%.
// Fast flicks used to explode (6000% / 20%): only exact ±120 was slowed, and
// deltas that arrived <150ms apart stacked from the gesture start without bound.
// Rapid ticks are coalesced: apply the net target once after a short idle so
// 1→2→3→4 becomes 1→4, and a reverse before apply cancels the overshoot.
static void ZoomByMouseWheel(MainWindow* win, WPARAM wp) {
    // don't show the context menu when zooming with the right mouse-button down
    win->dragStartPending = false;
    // Kill the smooth scroll timer when zooming
    // We don't want to move to the new updated y offset after zooming
    KillTimer(win->hwndCanvas, kSmoothScrollTimerID);

    short delta = GET_WHEEL_DELTA_WPARAM(wp);
    Point pt = HwndGetCursorPos(win->hwndCanvas);
    if (!gWheelZoomRelative) {
        // before 3.6 we were scrolling by steps
        float newZoom = win->ctrl->GetNextZoomStep(delta < 0 ? kZoomMin : kZoomMax);
        SmartZoom(win, newZoom, &pt, false);
        return;
    }

    AccumulateWheelZoom(win, wp, pt, GetMessageTime());
    DrainQueuedWheelZoom(win);
    SetTimer(win->hwndCanvas, kWheelZoomTimerID, kWheelZoomDebounceMs, nullptr);
}

static LRESULT CanvasOnMouseWheel(MainWindow* win, UINT msg, WPARAM wp, LPARAM lp) {
    // Scroll the ToC sidebar, if it's visible and the cursor is in it
    if (win->tocVisible && IsCursorOverWindow(win->tocTreeView->hwnd) && !gWheelMsgRedirect) {
        // Note: hwndTocTree's window procedure doesn't always handle
        //       WM_MOUSEWHEEL and when it's bubbling up, we'd return
        //       here recursively - prevent that
        gWheelMsgRedirect = true;
        LRESULT res = SendMessageW(win->tocTreeView->hwnd, msg, wp, lp);
        gWheelMsgRedirect = false;
        return res;
    }

    DisplayModel* dm = win->AsFixed();

    // Note: not all mouse drivers correctly report the Ctrl key's state
    // isCtrl is also set if this is pinch gestore from touchpad (on thinkpad x1 at least).
    bool isCtrl = (LOWORD(wp) & MK_CONTROL) || IsCtrlPressed();
    bool isAlt = (LOWORD(wp) & MK_ALT) || IsAltPressed();
    bool isRightButton = (LOWORD(wp) & MK_RBUTTON);
    bool isZooming = isCtrl || isRightButton;
    if (isZooming) {
        ZoomByMouseWheel(win, wp);
        return 0;
    }

    bool hScroll = (LOWORD(wp) & MK_SHIFT) || IsShiftPressed();
    bool vScroll = !hScroll;
    bool isCont = !IsContinuous(win->ctrl->GetDisplayMode());

    // logf("delta: %d, accumDelta: %d, hscroll: %d, continuous: %d, gDeltaPerLine: %d\n", (int)delta,
    // win->wheelAccumDelta,
    //      (int)hScroll, (int)isCont, gDeltaPerLine);

    // Alt speeds up scrolling but also triggers showing menu
    // this will suppress next menu trigger to avoid accidental triggering of menu
    if (isAlt) {
        gSupressNextAltMenuTrigger = true;
    }

    short delta = GET_WHEEL_DELTA_WPARAM(wp);

    // fit content: always flip page on wheel, regardless of scrollbar state
    if (vScroll && dm && dm->GetZoomVirtual() == kZoomFitContent && IsSingle(dm->GetDisplayMode())) {
        win->wheelAccumDelta += delta;
        if (win->wheelAccumDelta >= WHEEL_DELTA) {
            win->ctrl->GoToPrevPage();
            win->wheelAccumDelta -= WHEEL_DELTA;
        } else if (win->wheelAccumDelta <= -WHEEL_DELTA) {
            win->ctrl->GoToNextPage();
            win->wheelAccumDelta += WHEEL_DELTA;
        }
        return 0;
    }

    // Handle page-by-page navigation for non-continuous modes and SinglePage mode
    bool isSinglePageMode =
        gGlobalPrefs->scrollbarInSinglePage && (win->ctrl->GetDisplayMode() == DisplayMode::SinglePage);

    // For SinglePage mode with content requiring scrolling, use continuous scrolling behavior
    if (isSinglePageMode && vScroll) {
        if (dm && dm->NeedVScroll()) {
            // Content is larger than viewport, use continuous scrolling
            // Fall through to the default scrolling behavior below
        } else {
            // Content fits in viewport, use page-by-page navigation
            int pageFlipDelta = WHEEL_DELTA; // One wheel click = one page
            win->wheelAccumDelta += delta;
            if (win->wheelAccumDelta >= pageFlipDelta) {
                win->ctrl->GoToPrevPage();
                win->wheelAccumDelta -= pageFlipDelta;
                return 0;
            }
            if (win->wheelAccumDelta <= -pageFlipDelta) {
                win->ctrl->GoToNextPage();
                win->wheelAccumDelta += pageFlipDelta;
                return 0;
            }
            return 0;
        }
    }

    // Handle page-by-page navigation for other non-continuous modes (but not SinglePage mode)
    if (vScroll && !isCont && !isSinglePageMode) {
        float zoomVirt = win->ctrl->GetZoomVirtual();
        // in fit content we might show vert scrollbar but we want to flip the whole page on mouse wheel
        bool flipPage = zoomVirt == kZoomFitContent;
        if (dm && !dm->NeedVScroll()) {
            // if page/pages fully fit in window, flip the whole page
            // logf("  flipping page because !dm->NeedVScroll()\n");
            flipPage = true;
        }
        // fit content/page: one wheel click = one page; otherwise 3 clicks
        int pageFlipDelta = flipPage ? WHEEL_DELTA : WHEEL_DELTA * 3;

        // int scrolLPos = GetScrollPos(win->hwndCanvas, SB_VERT);
        //  Note: pre 3.6 didn't care about horizontallScroll and kZoomFitPage was handled below
        if (flipPage) {
            win->wheelAccumDelta += delta;
            if (win->wheelAccumDelta >= pageFlipDelta) {
                win->ctrl->GoToPrevPage();
                win->wheelAccumDelta -= pageFlipDelta;
                return 0;
            }
            if (win->wheelAccumDelta <= -pageFlipDelta) {
                win->ctrl->GoToNextPage();
                win->wheelAccumDelta += pageFlipDelta;
                return 0;
            }
            return 0;
        }
    }

    if (gDeltaPerLine == 0) {
        return 0;
    }

    // For SinglePage mode with zoomed content, use continuous scrolling with page transitions
    if (isSinglePageMode && vScroll && dm) {
        if (dm->NeedVScroll()) {
            // Use continuous scrolling that handles page transitions at boundaries
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_PAGE;
            GetScrollInfo(win->hwndCanvas, hScroll ? SB_HORZ : SB_VERT, &si);
            // Keep zoomed single-page scrolling controlled: one wheel notch moves
            // one third of the viewport instead of jumping many screens.
            int scrollBy = -MulDiv(si.nPage, delta, WHEEL_DELTA * 3);
            // on sensitive touchpads delta can be very small
            if (scrollBy == 0) return 0;
            if (hScroll) {
                dm->ScrollXBy(scrollBy);
            } else {
                dm->ScrollYBy(scrollBy, true);
                ReadAloudOnUserViewChanged(win);
            }
            return 0;
        }
    }

    if (gDeltaPerLine < 0 && dm) {
        // scroll by (fraction of a) page
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_PAGE;
        GetScrollInfo(win->hwndCanvas, hScroll ? SB_HORZ : SB_VERT, &si);
        int scrollBy = -MulDiv(si.nPage, delta, WHEEL_DELTA);
        // on sensitive touchpads delta can be very small
        if (scrollBy == 0) return 0;
        if (hScroll) {
            dm->ScrollXBy(scrollBy);
        } else {
            dm->ScrollYBy(scrollBy, true);
        }
        return 0;
    }

    // alt while scrolling will scroll by half a page per tick
    // usefull for browsing long files
    if (isAlt) {
        wp = (delta > 0) ? SB_HALF_PAGEUP : SB_HALF_PAGEDOWN;
        SendMessageW(win->hwndCanvas, WM_VSCROLL, wp, 0);
        return 0;
    }

    if (gGlobalPrefs->fastScrollOverScrollbar) {
        // scroll faster if the cursor is over the scroll bar
        if (IsCursorOverWindow(win->hwndCanvas)) {
            Point pt = HwndGetCursorPos(win->hwndCanvas);
            if (pt.x > win->canvasRc.dx) {
                wp = (delta > 0) ? SB_HALF_PAGEUP : SB_HALF_PAGEDOWN;
                SendMessageW(win->hwndCanvas, WM_VSCROLL, wp, 0);
                return 0;
            }
        }
    }

    win->wheelAccumDelta += delta;
    int prevScrollPos = GetScrollPos(win->hwndCanvas, SB_VERT);

    UINT scrollMsg = hScroll ? WM_HSCROLL : WM_VSCROLL;
    bool didScrollByLine = false;
    if (win->wheelAccumDelta < 0) {
        WPARAM scrollWp = hScroll ? SB_LINERIGHT : SB_LINEDOWN;
        while (win->wheelAccumDelta <= -gDeltaPerLine) {
            SendMessageW(win->hwndCanvas, scrollMsg, scrollWp, 0);
            win->wheelAccumDelta += gDeltaPerLine;
            // logf("  line down\n");
            didScrollByLine = true;
        }
    } else {
        WPARAM scrollWp = hScroll ? SB_LINELEFT : SB_LINEUP;
        while (win->wheelAccumDelta >= gDeltaPerLine) {
            SendMessageW(win->hwndCanvas, scrollMsg, scrollWp, 0);
            win->wheelAccumDelta -= gDeltaPerLine;
            // logf("  line up\n");
            didScrollByLine = true;
        }
    }
    // in non-continuous mode flip page if necessary
    if (!vScroll || !isCont) {
        return 0;
    }
    if (!didScrollByLine) {
        // we haven't reached accumulated delta to scroll by line
        return 0;
    }

    int currScrollPos = GetScrollPos(win->hwndCanvas, SB_VERT);
    bool didScroll = (currScrollPos != prevScrollPos);
    if (didScroll) {
        // we don't flip a page if we did scroll by line
        return 0;
    }
    // logf("  flip page: delta: %d, accumDelta: %d\n", (int)delta, (int)win->wheelAccumDelta);
    if (delta > 0) {
        win->ctrl->GoToPrevPage(true);
        ReadAloudOnUserViewChanged(win);
    } else {
        win->ctrl->GoToNextPage();
    }

    return 0;
}

static LRESULT CanvasOnMouseHWheel(MainWindow* win, UINT msg, WPARAM wp, LPARAM lp) {
    // Scroll the ToC sidebar, if it's visible and the cursor is in it
    if (win->tocVisible && IsCursorOverWindow(win->tocTreeView->hwnd) && !gWheelMsgRedirect) {
        // Note: hwndTocTree's window procedure doesn't always handle
        //       WM_MOUSEHWHEEL and when it's bubbling up, we'd return
        //       here recursively - prevent that
        gWheelMsgRedirect = true;
        LRESULT res = SendMessageW(win->tocTreeView->hwnd, msg, wp, lp);
        gWheelMsgRedirect = false;
        return res;
    }

    short delta = GET_WHEEL_DELTA_WPARAM(wp);

    if (gDeltaPerLine == 0) {
        return 0;
    }
    if (gDeltaPerLine < 0) {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_PAGE;
        GetScrollInfo(win->hwndCanvas, SB_HORZ, &si);
        int scrollBy = MulDiv(si.nPage, delta, WHEEL_DELTA);
        if (scrollBy != 0) {
            DisplayModel* dm = win->AsFixed();
            if (dm) {
                dm->ScrollXBy(scrollBy);
            }
        }
        return 0;
    }

    win->wheelAccumDelta += delta;

    while (win->wheelAccumDelta >= gDeltaPerLine) {
        SendMessageW(win->hwndCanvas, WM_HSCROLL, SB_LINERIGHT, 0);
        win->wheelAccumDelta -= gDeltaPerLine;
    }
    while (win->wheelAccumDelta <= -gDeltaPerLine) {
        SendMessageW(win->hwndCanvas, WM_HSCROLL, SB_LINELEFT, 0);
        win->wheelAccumDelta += gDeltaPerLine;
    }

    return TRUE;
}

static u32 LowerU64(ULONGLONG v) {
    u32 res = (u32)v;
    return res;
}

const char* GiFlagsToStr(DWORD flags) {
    switch (flags) {
        case 0:
            return "";
        case GF_BEGIN:
            return "GF_BEGIN";
        case GF_INERTIA:
            return "GF_INERTIA";
        case GF_END:
            return "GF_END";
        case GF_INERTIA | GF_END:
            return "GF_INERTIA  | GF_END";
    }
    return "unknown";
}

static LRESULT OnGesture(MainWindow* win, UINT msg, WPARAM wp, LPARAM lp) {
    DisplayModel* dm = win->AsFixed();
    if (!dm || !touch::SupportsGestures()) {
        return DefWindowProc(win->hwndFrame, msg, wp, lp);
    }

    HGESTUREINFO hgi = (HGESTUREINFO)lp;
    GESTUREINFO gi{};
    gi.cbSize = sizeof(GESTUREINFO);
    TouchState& touchState = win->touchState;

    BOOL ok = touch::GetGestureInfo(hgi, &gi);
    if (!ok) {
        touch::CloseGestureInfoHandle(hgi);
        return 0;
    }

    switch (gi.dwID) {
        case GID_ZOOM: {
            auto curr = (float)LowerU64(gi.ullArguments);
            bool isBegin = gi.dwFlags & GF_BEGIN;
            if (!isBegin) {
                auto prev = (float)touchState.zoomIntermediate;
                float factor = curr / prev;
                Point pt{gi.ptsLocation.x, gi.ptsLocation.y};
                HwndScreenToClient(win->hwndCanvas, pt);
                float newZoom = ScaleZoomBy(win, factor);
                SmartZoom(win, newZoom, &pt, false);
            }
            touchState.zoomIntermediate = curr;
            break;
        }

        case GID_PAN:
            // Flicking left or right changes the page,
            // panning moves the document in the scroll window
            if (gi.dwFlags == GF_BEGIN) {
                touchState.panStarted = true;
                touchState.panPos = gi.ptsLocation;
                touchState.panScrollOrigX = GetScrollPos(win->hwndCanvas, SB_HORZ);
                // logf("OnGesture: GID_PAN, GF_BEGIN, scrollX: %d\n", touchState.panScrollOrigX);
            } else if (touchState.panStarted) {
                int deltaX = touchState.panPos.x - gi.ptsLocation.x;
                int deltaY = touchState.panPos.y - gi.ptsLocation.y;
                touchState.panPos = gi.ptsLocation;

                // on left / right flick, go to next / prev page
                // unless we can pan/scroll the document
                bool isFlickX = (gi.dwFlags & GF_INERTIA) && (abs(deltaX) > abs(deltaY)) && (abs(deltaX) > 26);
                // logf("OnGesture: GID_PAN, flags: %d (%s), dx: %d, dy: %d, isFlick: %d\n", gi.dwFlags,
                // GiFlagsToStr(gi.dwFlags), deltaX, deltaY, (int)isFlickX);
                bool flipPage = false;
                if (!dm->NeedHScroll()) {
                    // if the page is fully visible
                    flipPage = true;
                    // logf("flipPage becaues !dm->NeedHScroll()");
                }
                if (deltaX > 0 && !dm->CanScrollRight()) {
                    flipPage = true;
                    // logf("flipPage becaues deltaX > 0 && !dm->CanScrollRight()");
                }
                if (deltaX < 0 && !dm->CanScrollLeft()) {
                    flipPage = true;
                    // logf("flipPage becaues deltaX < 0 && !dm->CanScrollLeft()");
                }

                if (isFlickX && flipPage) {
                    if (deltaX < 0) {
                        win->ctrl->GoToPrevPage();
                        // TODO: scroll to show the right-hand part
                        int x = dm->canvasSize.dx - dm->viewPort.dx;
                        // logf("x: %d\n");
                        dm->ScrollXTo(x);
                        ReadAloudOnUserViewChanged(win);
                    } else if (deltaX > 0) {
                        win->ctrl->GoToNextPage();
                        dm->ScrollXTo(0);
                        ReadAloudOnUserViewChanged(win);
                    }
                    // When we switch pages prevent further pan movement
                    // caused by the inertia
                    touchState.panStarted = false;
                } else {
                    // pan / scroll
                    bool canScrollRightBefore = dm->CanScrollRight();
                    bool canScrollLeftBefore = dm->CanScrollLeft();
                    win->MoveDocBy(deltaX, deltaY);

                    // if pan to the rigth edge, we want to "sticK" to it
                    // and only flip page on the next flick motion
                    bool stopPanning = false;
                    if (canScrollRightBefore != dm->CanScrollRight()) {
                        stopPanning = true;
                        // logf("stopPanning because canScrollRightBefore != dm->CanScrollRight()\n");
                    }
                    if (canScrollLeftBefore != dm->CanScrollLeft()) {
                        stopPanning = true;
                        // logf("stopPanning because canScrollLeftBefore != dm->CanScrollLeft()\n");
                    }
                    if (stopPanning) {
                        touchState.panStarted = false;
                    }
                }
            }
            break;

        case GID_ROTATE:
            // Rotate the PDF 90 degrees in one direction
            if (gi.dwFlags == GF_END && dm) {
                // This is in radians
                double rads = GID_ROTATE_ANGLE_FROM_ARGUMENT(LowerU64(gi.ullArguments));
                // The angle from the rotate is the opposite of the Sumatra rotate, thus the negative
                double degrees = -rads * 180 / M_PI;

                // Playing with the app, I found that I often didn't go quit a full 90 or 180
                // degrees. Allowing rotate without a full finger rotate seemed more natural.
                if (degrees < -120 || degrees > 120) {
                    dm->RotateBy(180);
                } else if (degrees < -45) {
                    dm->RotateBy(-90);
                } else if (degrees > 45) {
                    dm->RotateBy(90);
                }
            }
            break;

        case GID_TWOFINGERTAP:
            // Two-finger tap toggles fullscreen mode
            ToggleFullScreen(win);
            break;

        case GID_PRESSANDTAP:
            // Toggle between Fit Page, Fit Width and Fit Content (same as 'z')
            if (gi.dwFlags == GF_BEGIN) {
                win->ToggleZoom();
            }
            break;

        default:
            // A gesture was not recognized
            break;
    }

    touch::CloseGestureInfoHandle(hgi);
    return 0;
}

// WM_POINTER message support for pen/stylus input
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN 0x0246
#define WM_POINTERUP 0x0247
#define WM_POINTERUPDATE 0x0245
#endif

// POINTER_INPUT_TYPE values
#define SUMATRA_PT_PEN 3

// pointer message flags (in HIWORD of wParam)
#define SUMATRA_POINTER_MESSAGE_FLAG_INCONTACT 0x0004
#define SUMATRA_POINTER_MESSAGE_FLAG_FIRSTBUTTON 0x0010

// dynamically loaded pointer API (Windows 8+)
typedef BOOL(WINAPI* Sig_GetPointerType)(UINT32 pointerId, DWORD* pointerType);
static Sig_GetPointerType DynGetPointerType = nullptr;
static bool triedLoadPointerApi = false;

static void EnsurePointerApiLoaded() {
    if (triedLoadPointerApi) {
        return;
    }
    triedLoadPointerApi = true;
    HMODULE h = GetModuleHandleW(L"user32.dll");
    if (h) {
        DynGetPointerType = (Sig_GetPointerType)GetProcAddress(h, "GetPointerType");
    }
}

// handle WM_POINTER* messages for pen input by translating to mouse handlers
// pen input on Windows 8+ generates WM_POINTER* instead of WM_LBUTTON*
// and gesture configuration can prevent automatic promotion to mouse messages
static bool OnPointerMessage(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EnsurePointerApiLoaded();
    if (!DynGetPointerType) {
        return false;
    }

    UINT32 pointerId = LOWORD(wp);
    DWORD pointerType = 0;
    if (!DynGetPointerType(pointerId, &pointerType)) {
        return false;
    }
    // only handle pen input; let mouse and touch go through normal paths
    if (pointerType != SUMATRA_PT_PEN) {
        return false;
    }

    // WM_POINTER* lp contains screen coordinates
    POINT pt;
    pt.x = GET_X_LPARAM(lp);
    pt.y = GET_Y_LPARAM(lp);
    ScreenToClient(hwnd, &pt);
    int x = pt.x;
    int y = pt.y;

    // pointer message flags are in HIWORD(wParam)
    WORD flags = HIWORD(wp);
    WPARAM mouseWp = 0;

    if (msg == WM_POINTERDOWN) {
        mouseWp = MK_LBUTTON;
        OnMouseLeftButtonDown(win, x, y, mouseWp);
        return true;
    }
    if (msg == WM_POINTERUPDATE) {
        bool inContact = (flags & SUMATRA_POINTER_MESSAGE_FLAG_INCONTACT) != 0;
        if (inContact) {
            mouseWp = MK_LBUTTON;
        }
        OnMouseMove(win, x, y, mouseWp);
        return true;
    }
    if (msg == WM_POINTERUP) {
        OnMouseLeftButtonUp(win, x, y, mouseWp);
        return true;
    }
    return false;
}

static LRESULT WndProcCanvasFixedPageUI(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // DbgLogMsg("canvas:", hwnd, msg, wp, lp);

    if (!IsMainWindowValid(win)) {
        bool hwndValid = IsWindow(hwnd);
        logf("WndProcCanvasFixedPageUI: MainWindow win: 0x%p is no longer valid, msg: %d, hwnd valid: %d\n", win,
             (int)msg, (int)hwndValid);
        ReportIfFast(true);
        return 0;
    }

    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    switch (msg) {
        case WM_PAINT:
            if (gRedrawLog) {
                RECT urc;
                GetUpdateRect(hwnd, &urc, FALSE);
                logf("redraw: WM_PAINT hwnd=0x%p (canvas-fixed) rc=(%d,%d,%d,%d)\n", hwnd, urc.left, urc.top, urc.right,
                     urc.bottom);
            }
            OnPaintDocument(win);
            return 0;

        case WM_MOUSEMOVE:
            OnMouseMove(win, x, y, wp);
            return 0;

        case WM_LBUTTONDOWN:
            CloseWordLookup();
            OnMouseLeftButtonDown(win, x, y, wp);
            return 0;

        case WM_LBUTTONUP:
            OnMouseLeftButtonUp(win, x, y, wp);
            return 0;

        case WM_LBUTTONDBLCLK:
            OnMouseLeftButtonDblClk(win, x, y, wp);
            return 0;

        case WM_MBUTTONDOWN:
            SetTimer(hwnd, SMOOTHSCROLL_TIMER_ID, SMOOTHSCROLL_DELAY_IN_MS, nullptr);
            // TODO: Create window that shows location of initial click for reference
            OnMouseMiddleButtonDown(win, x, y, wp);
            return 0;

        case WM_MBUTTONUP:
            OnMouseMiddleButtonUp(win, x, y, wp);
            return 0;

        case WM_RBUTTONDOWN:
            CloseWordLookup();
            OnMouseRightButtonDown(win, x, y);
            return 0;

        case WM_RBUTTONUP:
            OnMouseRightButtonUp(win, x, y, wp);
            return 0;

        case WM_RBUTTONDBLCLK:
            OnMouseRightButtonDblClick(win, x, y, wp);
            return 0;

        case WM_VSCROLL:
            CloseWordLookup();
            OnVScroll(win, wp);
            return 0;

        case WM_HSCROLL:
            CloseWordLookup();
            OnHScroll(win, wp);
            return 0;

        case WM_MOUSEWHEEL:
            CloseWordLookup();
            if (MenuWheelScrollHandleWheel(wp)) {
                return 0;
            }
            return CanvasOnMouseWheel(win, msg, wp, lp);

        case WM_MOUSEHWHEEL:
            CloseWordLookup();
            return CanvasOnMouseHWheel(win, msg, wp, lp);

        case WM_SETCURSOR:
            if (OnSetCursor(win, hwnd)) {
                return TRUE;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_CONTEXTMENU:
            if (x == -1 || y == -1) {
                // if invoked with a keyboard (shift-F10) use current mouse position
                Point pt = HwndGetCursorPos(hwnd);
                x = pt.x;
                y = pt.y;
            }
            // super defensive
            if (x < 0) {
                x = 0;
            }
            if (y < 0) {
                y = 0;
            }
            OnWindowContextMenu(win, x, y);
            return 0;

        case WM_GESTURE:
            return OnGesture(win, msg, wp, lp);

        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
            if (OnPointerMessage(win, hwnd, msg, wp, lp)) {
                return 0;
            }
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_NCPAINT:
            // Do not call ShowScrollBar here. Visibility is owned by
            // UpdateScrollbars; ShowScrollBar mid-NCPAINT breaks native
            // scrollbar track hold-to-page auto-repeat (and can re-enter
            // uxtheme under dark themes). Match upstream behavior.
            goto def;
    }
def:
    return DefWindowProc(hwnd, msg, wp, lp);
}

///// methods needed for ChmUI canvases (should be subclassed by HtmlHwnd) /////

static LRESULT WndProcCanvasChmUI(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SETCURSOR:
            // TODO: make (re)loading a document always clear the infotip
            win->DeleteToolTip();
            return DefWindowProc(hwnd, msg, wp, lp);

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
}

///// methods needed for FixedPageUI canvases with loading error /////

static void OnPaintError(MainWindow* win) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(win->hwndCanvas, &ps);

    auto bgCol = ThemeMainWindowBackgroundColor();
    AutoDeleteBrush bgBrush = CreateSolidBrush(bgCol);
    FillRect(hdc, &ps.rcPaint, bgBrush);
    // Loading / error status is shown by the top-left notification banner.
    // Do not DrawCenteredText "Loading …" — same font on the full canvas looks
    // much larger than the banner (especially the last file of a multi-file open).

    EndPaint(win->hwndCanvas, &ps);
}

static LRESULT WndProcCanvasLoadError(MainWindow* win, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            if (gRedrawLog) {
                logf("redraw: WM_PAINT hwnd=0x%p (canvas-error)\n", hwnd);
            }
            OnPaintError(win);
            return 0;

        case WM_SETCURSOR:
            // TODO: make (re)loading a document always clear the infotip
            win->DeleteToolTip();
            return DefWindowProc(hwnd, msg, wp, lp);

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
}

///// methods needed for all types of canvas /////

struct RepaintTaskData {
    MainWindow* win = nullptr;
    int delayInMs = 0;
};

static void RepaintTask(RepaintTaskData* d) {
    AutoDelete delData(d);

    auto win = d->win;
    if (!IsMainWindowValid(win)) {
        return;
    }
    if (!d->delayInMs) {
        WndProcCanvas(win->hwndCanvas, WM_TIMER, REPAINT_TIMER_ID, 0);
    } else if (!win->delayedRepaintTimer) {
        win->delayedRepaintTimer = SetTimer(win->hwndCanvas, REPAINT_TIMER_ID, (uint)d->delayInMs, nullptr);
    }
}

void ScheduleRepaint(MainWindow* win, int delayInMs) {
    if (gRedrawLog) {
        logf("redraw: ScheduleRepaint delayMs=%d canvas=0x%p\n", delayInMs, win->hwndCanvas);
    }
    auto data = new RepaintTaskData;
    data->win = win;
    data->delayInMs = delayInMs;
    auto fn = MkFunc0<RepaintTaskData>(RepaintTask, data);
    // even though RepaintAsync is mostly called from the UI thread,
    // we depend on the repaint message to happen asynchronously
    uitask::Post(fn, nullptr);
}

static void OnTimer(MainWindow* win, HWND hwnd, WPARAM timerId) {
    Point pt;

    if (!win || !IsMainWindowValid(win) || win->isBeingClosed) {
        return;
    }

    switch (timerId) {
        case REPAINT_TIMER_ID:
            win->delayedRepaintTimer = 0;
            KillTimer(hwnd, REPAINT_TIMER_ID);
            win->RedrawAllIncludingNonClient();
            break;

        case SMOOTHSCROLL_TIMER_ID:
            if (MouseAction::Scrolling == win->mouseAction) {
                win->MoveDocBy(win->xScrollSpeed, win->yScrollSpeed);
            } else if (MouseAction::Selecting == win->mouseAction || MouseAction::SelectingText == win->mouseAction) {
                pt = HwndGetCursorPos(win->hwndCanvas);
                if (NeedsSelectionEdgeAutoscroll(win, pt.x, pt.y)) {
                    OnMouseMove(win, pt.x, pt.y, MK_CONTROL);
                }
            } else {
                KillTimer(hwnd, SMOOTHSCROLL_TIMER_ID);
                win->yScrollSpeed = 0;
                win->xScrollSpeed = 0;
            }
            break;

        case HOME_SCROLL_TIMER_ID:
            if (win->IsCurrentTabAbout()) {
                HomePageOnScrollTimer(win);
            } else {
                KillTimer(hwnd, HOME_SCROLL_TIMER_ID);
                win->homePageScrollTimer = 0;
            }
            break;

        case kHideCursorTimerID:
            // logf("got kHideCursorTimerID\n");
            KillTimer(hwnd, kHideCursorTimerID);
            if (win->InPresentation()) {
                // logf("hiding cursor because win->presentations\n");
                SetCursor((HCURSOR) nullptr);
            }
            break;

        case HIDE_FWDSRCHMARK_TIMER_ID:
            win->fwdSearchMark.hideStep++;
            if (1 == win->fwdSearchMark.hideStep) {
                SetTimer(hwnd, HIDE_FWDSRCHMARK_TIMER_ID, HIDE_FWDSRCHMARK_DECAYINTERVAL_IN_MS, nullptr);
            } else if (win->fwdSearchMark.hideStep >= HIDE_FWDSRCHMARK_STEPS) {
                KillTimer(hwnd, HIDE_FWDSRCHMARK_TIMER_ID);
                win->fwdSearchMark.show = false;
                ScheduleRepaint(win, 0);
            } else {
                ScheduleRepaint(win, 0);
            }
            break;

        case AUTO_RELOAD_TIMER_ID: {
            KillTimer(hwnd, AUTO_RELOAD_TIMER_ID);
            auto tab = win->CurrentTab();
            if (tab && tab->reloadOnFocus) {
                if (tab->ignoreNextAutoReload) {
                    tab->ignoreNextAutoReload = false;
                } else {
                    ApplyTabReloadOnFocus(win, tab, true);
                }
            }
            break;
        }

        case REFLOW_THEME_RETRY_TIMER_ID: {
            KillTimer(hwnd, REFLOW_THEME_RETRY_TIMER_ID);
            WindowTab* tab = win->CurrentTab();
            if (tab && tab->reloadOnFocus) {
                ApplyTabReloadOnFocus(win, tab, false);
            }
            break;
        }

        case READ_ALOUD_HIGHLIGHT_TIMER_ID: {
            WindowTab* raTab = GetReadAloudSourceTab();
            if (raTab && raTab->win == win && win->CurrentTab() == raTab) {
                TtsProcessEvents();
                ReadAloudUpdateAutoScroll(win);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                ReadAloudHighlightTimerStop(win);
            }
            break;
        }

        case kWheelZoomTimerID: {
            KillTimer(hwnd, kWheelZoomTimerID);
            ApplyPendingWheelZoom(win);
            break;
        }

        case kSmoothScrollTimerID: {
            DisplayModel* dm = win->AsFixed();
            // window might have been closed while the timer was running
            if (!dm) {
                return;
            }

            int current = dm->yOffset();
            int target = win->scrollTargetY;
            int delta = target - current;
            bool readAloudScroll = win->readAloudScrollFromCode;

            if (delta == 0) {
                KillTimer(hwnd, kSmoothScrollTimerID);
                if (readAloudScroll) {
                    win->readAloudScrollFromCode = false;
                }
            } else {
                // logf("Smooth scrolling from %d to %d (delta %d)\n", current, target, delta);

                double factor = readAloudScroll ? kReadAloudSmoothScrollFactor : gSmoothScrollingFactor;
                double step = delta * factor;

                // Round away from zero
                int dy = step < 0 ? (int)floor(step) : (int)ceil(step);
                dm->ScrollYTo(current + dy);
                if (!readAloudScroll) {
                    ReadAloudOnUserViewChanged(win);
                }
            }
            break;
        }
    }
}

static void GetDropFilesResolved(HDROP hDrop, bool dragFinish, StrVec& files) {
    int nFiles = DragQueryFile(hDrop, DRAGQUERY_NUMFILES, nullptr, 0);
    WCHAR pathW[MAX_PATH]{};
    char* path = nullptr;
    for (int i = 0; i < nFiles; i++) {
        DragQueryFile(hDrop, i, pathW, dimof(pathW));
        path = ToUtf8Temp(pathW);
        if (str::EndsWithI(path, ".lnk")) {
            char* resolved = ResolveLnkTemp(path);
            if (resolved) {
                path = resolved;
            }
        }
        files.Append(path);
    }
    if (dragFinish) {
        DragFinish(hDrop);
    }
}

static void OnDropFiles(MainWindow* win, HDROP hDrop, bool dragFinish) {
    StrVec files;
    bool isShift = IsShiftPressed();

    GetDropFilesResolved(hDrop, dragFinish, files);
    for (char* path : files) {
        // The first dropped document may override the current window
        LoadArgs args(path, win);
        SetUserOpenActivateExisting(args);
        if (isShift && !win) {
            win = CreateAndShowMainWindow(nullptr);
            args.win = win;
        }
        StartLoadDocument(&args);
    }
}

// returns true if url looks like it could be an image URL
static bool IsImageUrl(const char* url) {
    // strip query string / fragment for extension check
    const char* q = str::FindChar(url, '?');
    const char* h = str::FindChar(url, '#');
    int len = str::Leni(url);
    if (q && (int)(q - url) < len) {
        len = (int)(q - url);
    }
    if (h && (int)(h - url) < len) {
        len = (int)(h - url);
    }
    // check for common image extensions
    const char* exts[] = {".png",  ".jpg",  ".jpeg", ".gif", ".bmp", ".tiff", ".tif",
                          ".webp", ".avif", ".heic", ".jxr", ".jp2", ".tga"};
    for (auto ext : exts) {
        int extLen = str::Leni(ext);
        if (len >= extLen) {
            TempStr ending = str::DupTemp(url + len - extLen, extLen);
            if (str::EqI(ending, ext)) {
                return true;
            }
        }
    }
    return false;
}

// Get the user's Downloads folder path
static TempStr GetDownloadsDirTemp() {
    WCHAR* pathW = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pathW);
    if (FAILED(hr) || !pathW) {
        CoTaskMemFree(pathW);
        return nullptr;
    }
    TempStr res = ToUtf8Temp(pathW);
    CoTaskMemFree(pathW);
    return res;
}

// Extract a file name from a URL (last path component, without query/fragment)
static TempStr FileNameFromUrlTemp(const char* url) {
    // skip past scheme
    const char* s = str::FindChar(url, '/');
    if (s && s[1] == '/') {
        s += 2; // skip "//"
    }
    // find last '/' before any '?' or '#'
    const char* lastSlash = nullptr;
    const char* p = s ? s : url;
    while (*p && *p != '?' && *p != '#') {
        if (*p == '/') {
            lastSlash = p;
        }
        p++;
    }
    if (!lastSlash) {
        return nullptr;
    }
    int nameLen = (int)(p - lastSlash - 1);
    if (nameLen <= 0) {
        return nullptr;
    }
    return str::DupTemp(lastSlash + 1, nameLen);
}

struct DownloadAndOpenUrlData {
    char* url;
    HWND hwndCanvas;
};

static void DownloadAndOpenUrl(DownloadAndOpenUrlData* data) {
    TempStr url = data->url;
    HWND hwndCanvas = data->hwndCanvas;

    TempStr downloadsDir = GetDownloadsDirTemp();
    if (!downloadsDir) {
        logf("DownloadAndOpenUrl: failed to get Downloads folder\n");
        free(data->url);
        delete data;
        return;
    }

    TempStr fileName = FileNameFromUrlTemp(url);
    if (!fileName) {
        // generate a fallback name
        fileName = str::DupTemp("dropped_image.png");
    }

    TempStr destPath = path::JoinTemp(downloadsDir, fileName);

    // avoid overwriting: if file exists, add a numeric suffix
    if (file::Exists(destPath)) {
        TempStr ext = path::GetExtTemp(destPath);
        TempStr base = str::DupTemp(fileName, str::Leni(fileName) - str::Leni(ext));
        for (int i = 1; i < 1000; i++) {
            TempStr newName = str::FormatTemp("%s_%d%s", base, i, ext);
            destPath = path::JoinTemp(downloadsDir, newName);
            if (!file::Exists(destPath)) {
                break;
            }
        }
    }

    logf("DownloadAndOpenUrl: downloading '%s' to '%s'\n", url, destPath);

    Func1<HttpProgress*> emptyProgress;
    bool ok = HttpGetToFile(url, destPath, emptyProgress);
    if (!ok) {
        logf("DownloadAndOpenUrl: download failed for '%s'\n", url);
        free(data->url);
        delete data;
        return;
    }

    // verify the downloaded file is a supported image type
    Kind kind = GuessFileTypeFromContent(destPath);
    if (!IsEngineImageSupportedFileType(kind)) {
        logf("DownloadAndOpenUrl: downloaded file is not a supported image type: '%s'\n", destPath);
        file::Delete(destPath);
        free(data->url);
        delete data;
        return;
    }

    // ensure it has a good extension, some urls are like:
    // https://pbs.twimg.com/media/HEwit7bbQAAWiIO?format=jpg&name=large
    const char* ext = GetExtForKind(kind);
    if (!str::EndsWithI(destPath, ext)) {
        TempStr newDest = str::JoinTemp(destPath, ext);
        ok = file::Rename(newDest, destPath);
        if (ok) {
            destPath = newDest;
        }
    }

    // open the file on the UI thread
    char* pathDup = str::Dup(destPath);
    auto fn = MkFunc0<char>(
        [](char* path) {
            MainWindow* win = FindMainWindowByHwnd(GetForegroundWindow());
            if (!win && !gWindows.IsEmpty()) {
                win = gWindows.at(0);
            }
            if (win) {
                LoadArgs args(path, win);
                SetUserOpenActivateExisting(args);
                StartLoadDocument(&args);
            }
            free(path);
        },
        pathDup);
    uitask::Post(fn, "DownloadAndOpenUrl");

    free(data->url);
    delete data;
}

// Extract text from IDataObject (tries CF_UNICODETEXT, then CF_TEXT)
static TempStr GetTextFromDataObject(IDataObject* dataObj) {
    FORMATETC fmtUnicode = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    FORMATETC fmtAnsi = {CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    HRESULT hr = dataObj->GetData(&fmtUnicode, &medium);
    TempStr res;
    if (SUCCEEDED(hr) && medium.hGlobal) {
        WCHAR* w = (WCHAR*)GlobalLock(medium.hGlobal);
        res = w ? ToUtf8Temp(w) : nullptr;
        goto Cleanup;
    }
    hr = dataObj->GetData(&fmtAnsi, &medium);
    if (SUCCEEDED(hr) && medium.hGlobal) {
        char* s = (char*)GlobalLock(medium.hGlobal);
        res = s ? str::DupTemp(s) : nullptr;
        goto Cleanup;
    }
    return nullptr;
Cleanup:
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return res;
}

// Check if IDataObject contains a URL (registered format "UniformResourceLocatorW" or "UniformResourceLocator")
static TempStr GetUrlFromDataObject(IDataObject* dataObj) {
    // try wide URL format first
    static CLIPFORMAT cfUrlW = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocatorW");
    if (cfUrlW) {
        FORMATETC fmt = {cfUrlW, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        HRESULT hr = dataObj->GetData(&fmt, &medium);
        if (SUCCEEDED(hr) && medium.hGlobal) {
            WCHAR* w = (WCHAR*)GlobalLock(medium.hGlobal);
            TempStr res = w ? ToUtf8Temp(w) : nullptr;
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
            if (res && (str::StartsWithI(res, "http://") || str::StartsWithI(res, "https://"))) {
                return res;
            }
        }
    }
    // try ANSI URL format
    static CLIPFORMAT cfUrl = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocator");
    if (cfUrl) {
        FORMATETC fmt = {cfUrl, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        HRESULT hr = dataObj->GetData(&fmt, &medium);
        if (SUCCEEDED(hr) && medium.hGlobal) {
            char* s = (char*)GlobalLock(medium.hGlobal);
            TempStr res = s ? str::DupTemp(s) : nullptr;
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
            if (res && (str::StartsWithI(res, "http://") || str::StartsWithI(res, "https://"))) {
                return res;
            }
        }
    }
    return nullptr;
}

static bool DataObjectHasFiles(IDataObject* dataObj) {
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return dataObj->QueryGetData(&fmt) == S_OK;
}

static bool DataObjectHasUrl(IDataObject* dataObj) {
    TempStr url = GetUrlFromDataObject(dataObj);
    if (url && IsImageUrl(url)) {
        return true;
    }
    // also check plain text that looks like an image URL
    TempStr text = GetTextFromDataObject(dataObj);
    if (text && (str::StartsWithI(text, "http://") || str::StartsWithI(text, "https://")) && IsImageUrl(text)) {
        return true;
    }
    return false;
}

class CanvasDropTarget : public IDropTarget {
    LONG refCount = 1;
    HWND hwnd = nullptr;

  public:
    explicit CanvasDropTarget(HWND h) : hwnd(h) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refCount);
        if (r == 0) {
            delete this;
        }
        return r;
    }

    STDMETHODIMP DragEnter(IDataObject* dataObj, __unused DWORD grfKeyState, __unused POINTL pt,
                           DWORD* pdwEffect) override {
        if (DataObjectHasFiles(dataObj) || DataObjectHasUrl(dataObj)) {
            *pdwEffect = DROPEFFECT_COPY;
        } else {
            *pdwEffect = DROPEFFECT_NONE;
        }
        return S_OK;
    }

    STDMETHODIMP DragOver(__unused DWORD grfKeyState, __unused POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    STDMETHODIMP DragLeave() override { return S_OK; }

    STDMETHODIMP Drop(IDataObject* dataObj, DWORD grfKeyState, __unused POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;

        // first try file drops (CF_HDROP)
        FORMATETC fmtHDrop = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        HRESULT hr = dataObj->GetData(&fmtHDrop, &medium);
        if (SUCCEEDED(hr) && medium.hGlobal) {
            HDROP hDrop = (HDROP)medium.hGlobal;
            MainWindow* win = FindMainWindowByHwnd(hwnd);
            if (win) {
                OnDropFiles(win, hDrop, false);
            }
            ReleaseStgMedium(&medium);
            return S_OK;
        }

        // try URL drop
        TempStr url = GetUrlFromDataObject(dataObj);
        if (!url) {
            // fall back to plain text
            TempStr text = GetTextFromDataObject(dataObj);
            if (text && (str::StartsWithI(text, "http://") || str::StartsWithI(text, "https://"))) {
                url = text;
            }
        }

        if (url) {
            auto data = new DownloadAndOpenUrlData();
            data->url = str::Dup(url);
            data->hwndCanvas = hwnd;
            auto fn = MkFunc0<DownloadAndOpenUrlData>([](DownloadAndOpenUrlData* d) { DownloadAndOpenUrl(d); }, data);
            RunAsync(fn, "DownloadAndOpenUrl");
        }

        return S_OK;
    }
};

void RegisterCanvasDropTarget(HWND hwndCanvas) {
    auto* dt = new CanvasDropTarget(hwndCanvas);
    RegisterDragDrop(hwndCanvas, dt);
    dt->Release(); // RegisterDragDrop AddRef'd it
}

void RevokeCanvasDropTarget(HWND hwndCanvas) {
    RevokeDragDrop(hwndCanvas);
}

LRESULT CALLBACK WndProcCanvas(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // messages that don't require win

    if (msg == WM_NCCALCSIZE && wp == TRUE) {
        // When overlay/hidden scrollbars are active, SetScrollInfo still adds
        // WS_VSCROLL/WS_HSCROLL styles. Handle WM_NCCALCSIZE to prevent Windows
        // from reserving non-client space for native scrollbars.
        if (ScrollbarsAreHidden() || ScrollbarsUseOverlay()) {
            // strip scroll styles that SetScrollInfo may have added
            DWORD style = GetWindowLong(hwnd, GWL_STYLE);
            if (style & (WS_VSCROLL | WS_HSCROLL)) {
                SetWindowLong(hwnd, GWL_STYLE, style & ~(WS_VSCROLL | WS_HSCROLL));
            }
            // let DefWindowProc calculate NC size without scroll styles
            return DefWindowProc(hwnd, msg, wp, lp);
        }
    }

    MainWindow* win = FindMainWindowByHwnd(hwnd);
    switch (msg) {
        case WM_DROPFILES:
            ReportIf(lp != 0 && lp != 1);
            OnDropFiles(win, (HDROP)wp, !lp);
            return 0;

            // https://docs.microsoft.com/en-us/windows/win32/winmsg/wm-erasebkgnd
        case WM_ERASEBKGND: {
            if (gRedrawLog) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                logf("redraw: WM_ERASEBKGND hwnd=0x%p (canvas) rc=(%d,%d,%d,%d)\n", hwnd, rc.left, rc.top, rc.right,
                     rc.bottom);
            }
            // don't paint here; old content stays until WM_PAINT covers it
            // (CS_HREDRAW|CS_VREDRAW removed so no transparent flash)
            return 1;
        }

        case WM_NCHITTEST: {
            // return HTTRANSPARENT near frame edges so the parent frame
            // can handle resize hit-testing beyond kFrameBorderSize
            if (win && win->tabsInTitlebar && !IsZoomed(GetParent(hwnd))) {
                // Never steal scrollbar hits for frame resize — the vscroll
                // sits on the right edge where the resize strip would match.
                LRESULT ht = DefWindowProc(hwnd, msg, wp, lp);
                if (ht == HTVSCROLL || ht == HTHSCROLL || ht == HTGROWBOX) {
                    return ht;
                }
                int x = GET_X_LPARAM(lp);
                int y = GET_Y_LPARAM(lp);
                RECT wrc;
                GetWindowRect(GetParent(hwnd), &wrc);
                int b = kFrameResizeHitTest;
                if ((x - wrc.left) < b || (wrc.right - x) <= b || (y - wrc.top) < b || (wrc.bottom - y) <= b) {
                    return HTTRANSPARENT;
                }
                return ht;
            }
            break;
        }
    }

    if (!win) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    // messages that require win
    switch (msg) {
        case WM_NCLBUTTONDOWN:
            // Native track/arrow hold-to-repeat uses WM_TIMER. TOC paint storms used to
            // starve those timers; heights are recalculated outside paint now.
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_TIMER:
            OnTimer(win, hwnd, wp);
            return 0;

        case WM_SIZE:
            if (!IsIconic(win->hwndFrame)) {
                CloseWordLookup();
                if (gRedrawLog) {
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    logf("redraw: WM_SIZE hwnd=0x%p (canvas) size=(%d,%d)\n", hwnd, rc.right, rc.bottom);
                }
                if (IsSidebarSplitterLiveDrag()) {
                    return 0;
                }
                win->UpdateCanvasSize();
                // fully invalidate since layout depends on size
                // (replaces CS_HREDRAW | CS_VREDRAW which caused transparent flash)
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_GETOBJECT:
            // TODO: should we check for UiaRootObjectId, as in
            // http://msdn.microsoft.com/en-us/library/windows/desktop/ff625912.aspx ???
            // On the other hand
            // http://code.msdn.microsoft.com/windowsdesktop/UI-Automation-Clean-94993ac6/sourcecode?fileId=42883&pathId=2071281652
            // says that UiaReturnRawElementProvider() should be called regardless of lParam
            // Don't expose UIA automation in plugin mode yet. UIA is still too experimental
            if (gPluginMode) {
                return DefWindowProc(hwnd, msg, wp, lp);
            }
            // disable UIAutomation in release builds until concurrency issues and
            // memory leaks have been figured out and fixed
            if (!gIsDebugBuild) {
                return DefWindowProc(hwnd, msg, wp, lp);
            }
            if (!win->CreateUIAProvider()) {
                return DefWindowProc(hwnd, msg, wp, lp);
            }
            // TODO: should win->uiaProvider->Release() as in
            // http://msdn.microsoft.com/en-us/library/windows/desktop/gg712214.aspx
            // and http://www.code-magazine.com/articleprint.aspx?quickid=0810112&printmode=true ?
            // Maybe instead of having a single provider per win, we should always create a new one
            // like in this sample:
            // http://code.msdn.microsoft.com/windowsdesktop/UI-Automation-Clean-94993ac6/sourcecode?fileId=42883&pathId=2071281652
            // currently win->uiaProvider refCount is really out of wack in MainWindow::~MainWindow
            // from logging it seems that UiaReturnRawElementProvider() increases refCount by 1
            // and since WM_GETOBJECT is called many times, it accumulates
            return UiaReturnRawElementProvider(hwnd, wp, lp, win->uiaProvider);

        default:
            // TODO: achieve this split through subclassing or different window classes
            if (win->IsDocLoaded() && win->AsFixed()) {
                HomePageDestroySearch(win);
                return WndProcCanvasFixedPageUI(win, hwnd, msg, wp, lp);
            }

            if (win->AsChm()) {
                HomePageDestroySearch(win);
                return WndProcCanvasChmUI(win, hwnd, msg, wp, lp);
            }

            if (win->IsCurrentTabAbout()) {
                return WndProcCanvasAbout(win, hwnd, msg, wp, lp);
            }

            HomePageDestroySearch(win);
            return WndProcCanvasLoadError(win, hwnd, msg, wp, lp);
    }
}
