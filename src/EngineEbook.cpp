/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// engines which render flowed ebook formats into fixed pages through the EngineBase API
// flowed ebook pages use A5 dimensions (420pt x 595pt), matching the MuPDF engine

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Archive.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "mui/Mui.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"

#include "wingui/UIModels.h"

#include "GumboHelpers.h"

#include "DocProperties.h"
#include "DocController.h"
#include "FzImgReader.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SumatraConfig.h"
#include "EbookBase.h"
#include "EbookTypography.h"
#include "PalmDbReader.h"
#include "EbookDoc.h"
#include "HtmlFormatter.h"
#include "EbookFormatter.h"
#include "Settings.h"
#include "Theme.h"
#include "utils/ThreadUtil.h"

#include "utils/Log.h"

void NotifyEngineDisplayReady(EngineBase* engine);

static Vec<HtmlPage*>* FormatFirstHtmlPage(HtmlFormatter& formatter) {
    auto* result = new Vec<HtmlPage*>();
    HtmlPage* page = formatter.Next(false);
    if (page) {
        result->Append(page);
    }
    return result;
}

static Vec<HtmlPage*>* FormatInitialHtmlPages(HtmlFormatter* formatter, int maxPages) {
    auto* result = new Vec<HtmlPage*>();
    for (int i = 0; i < maxPages; i++) {
        HtmlPage* page = formatter->Next(false);
        if (!page) {
            break;
        }
        result->Append(page);
        if (result->size() >= 1) {
            break;
        }
    }
    return result;
}
void NotifyEbookPagesLoadingProgress(const char* filePath, bool reloadToc);

// pages formatted per batch before yielding during background MOBI pagination
static const int kEbookFormatBatchPages = 16;
// let the UI render the first page(s) before the background formatter starts
// hammering the CPU (mirrors the EPUB/MuPDF reflowable loader)
static const DWORD kEbookBackgroundDelayMs = 50;
// minimum gap between "more pages available" notifications. Each notification
// triggers a full O(pageCount) relayout on the UI thread, so we throttle by
// time (not page count) to keep the cost bounded on very large books.
static const DWORD kEbookNotifyIntervalMs = 200;

extern EBookUI* GetEBookUI();

Kind kindEngineEpub = "engineEpub";
Kind kindEngineFb2 = "engineFb2";
Kind kindEngineMobi = "engineMobi";
Kind kindEnginePdb = "enginePdb";
Kind kindEngineChm = "engineChm";
Kind kindEngineHtml = "engineHtml";
Kind kindEngineTxt = "engineTxt";

static AutoFreeStr gDefaultFontName;
static float gDefaultFontSize = 10.f;

static float layoutLatinMobiDxPt = 540.F;
static float layoutLatinMobiDyPt = 760.F;
static float layoutCjkMobiDxPt = 600.F;
static float layoutCjkMobiDyPt = 820.F;
static float layoutLegacyMobiDxPt = 560.F;
static float layoutLegacyMobiDyPt = 680.F;
// MOBI/AZW HtmlFormatter default is 10pt; MuPDF EPUB uses layoutFontEm (11pt). Scale MOBI
// body text up so it matches EPUB readability without changing the EPUB engine path.
static const float kMobiReaderFontScale = 1.12f;

static bool IsReaderStyledMobiPath(const char* filePath) {
    if (str::IsEmpty(filePath)) {
        return false;
    }
    const char* ext = path::GetExtTemp(filePath);
    return str::EqI(ext, ".mobi") || str::EqI(ext, ".azw") || str::EqI(ext, ".azw3");
}

static void CountHtmlLetters(const char* s, size_t len, int* cjkOut, int* latinOut) {
    int cjk = 0;
    int latin = 0;
    bool inTag = false;
    if (!s) {
        *cjkOut = 0;
        *latinOut = 0;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '<') {
            inTag = true;
            continue;
        }
        if (c == '>') {
            inTag = false;
            continue;
        }
        if (inTag) {
            continue;
        }
        if (c < 0x80) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                latin++;
            }
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            i += 1;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            unsigned char c1 = (unsigned char)s[i + 1];
            unsigned char c2 = (unsigned char)s[i + 2];
            uint cp = ((uint)(c & 0x0F) << 12) | ((uint)(c1 & 0x3F) << 6) | (uint)(c2 & 0x3F);
            if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)) {
                cjk++;
            }
            i += 2;
        } else if ((c & 0xF8) == 0xF0) {
            i += 3;
        }
        if (cjk + latin >= 600) {
            break;
        }
    }
    *cjkOut = cjk;
    *latinOut = latin;
}

static EbookTypographyKind ClassifyHtmlLetters(int cjk, int latin) {
    if (cjk >= 12 && latin >= 12 && cjk <= latin * 6 && latin <= cjk * 6) {
        return EbookTypographyKind::Bilingual;
    }
    if (cjk >= 8 && cjk * 2 >= latin) {
        return EbookTypographyKind::Cjk;
    }
    if (latin >= 40 && latin > cjk * 3) {
        return EbookTypographyKind::Latin;
    }
    return cjk >= latin ? EbookTypographyKind::Cjk : EbookTypographyKind::Latin;
}

static EbookTypographyKind DetectHtmlTypographyKind(const ByteSlice& html) {
    size_t n = html.size();
    if (n > 384 * 1024) {
        n = 384 * 1024;
    }
    int cjk = 0;
    int latin = 0;
    CountHtmlLetters((const char*)html.data(), n, &cjk, &latin);
    return ClassifyHtmlLetters(cjk, latin);
}

static void SetupHtmlFormatterFont(HtmlFormatterArgs& args, const char* /*filePath*/,
                                   EbookTypographyKind typographyKind = EbookTypographyKind::Latin) {
    SetEbookTypographyKind(typographyKind);
    char* s = gDefaultFontName.Get();
    if (s) {
        args.SetFontName(ToWStrTemp(s));
    } else if (typographyKind == EbookTypographyKind::Cjk || typographyKind == EbookTypographyKind::Bilingual) {
        args.SetFontName(L"Source Han Serif SC");
    } else {
        args.SetFontName(L"Literata");
    }
}

static float GetDefaultFontSize() {
    // fonts are scaled at higher DPI settings,
    // undo this here for (mostly) consistent results
    float size = gDefaultFontSize;
    if (size == 0) {
        size = 11.f;
    }
    auto eBookUI = GetEBookUI();
    if (eBookUI && eBookUI->fontSize > 6 && eBookUI->fontSize < 30) {
        size = eBookUI->fontSize;
    }
    return size * 96.0f / (float)DpiGetForHwnd(HWND_DESKTOP);
}

void SetDefaultEbookFont(const char* name, float size) {
    // intentionally don't validate the input
    if (str::Eq(name, "default")) {
        // "default" is used for mupdf engine to indicate
        // we should use the font as given in css
        name = "Source Han Serif SC";
    }
    gDefaultFontName.SetCopy(name);
    // use a somewhat smaller size than in the EbookUI, since fit page/width
    // is likely to be above 100% for the paperback page dimensions
    gDefaultFontSize = size * 0.8f;
}

/* common classes for EPUB, FictionBook2, Mobi, PalmDOC, CHM, HTML and TXT engines */

struct PageAnchor {
    DrawInstr* instr;
    int pageNo;

    explicit PageAnchor(DrawInstr* instr = nullptr, int pageNo = -1) : instr(instr), pageNo(pageNo) {}
};

class EbookAbortCookie : public AbortCookie {
  public:
    bool abort = false;
    EbookAbortCookie() {}
    void Abort() override { abort = true; }
    void* GetData() override { return nullptr; }
};

class EngineEbook : public EngineBase {
  public:
    EngineEbook();
    ~EngineEbook() override;

    RectF PageMediabox(int pageNo) override;
    RectF PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override;

    RenderedBitmap* RenderPage(RenderPageArgs& args) override;

    RectF Transform(const RectF& rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    ByteSlice GetFileData() override;

    bool SaveFileAs(const char* copyFileName) override;
    PageText ExtractPageText(int pageNo) override;
    PageTextUtf8 ExtractPageTextUtf8(int pageNo) override;
    // make RenderCache request larger tiles than per default
    bool HasClipOptimizations(int pageNo) override;

    Vec<IPageElement*> GetElements(int pageNo) override;
    IPageElement* GetElementAtPos(int pageNo, PointF pt) override;
    bool HandleLink(IPageDestination* dest, ILinkHandler* linkHandler) override;

    IPageDestination* GetNamedDest(const char* name) override;
    RenderedBitmap* GetImageForPageElement(IPageElement* el) override;

    bool BenchLoadPage(int pageNo) override;

    bool IsProgressiveEbookLoading() override {
        return IsEbookProgressiveLoadingInProgress();
    }

    bool HitTestText(int pageNo, PointF pagePt, EbookTextHit* hitOut);
    TempWStr GetRunTextTemp(int pageNo, int instrIndex);
    bool GetCharRangeBbox(int pageNo, int instrIndex, int charStart, int charEnd, RectF* out);

    bool IsEbookProgressiveLoadingInProgress() const {
        return InterlockedCompareExchange(&ebookLoadingInProgress, 0, 0) != 0;
    }

    int GetFormattedPageCount() {
        if (!pages) {
            return 0;
        }
        ScopedCritSec scope(&pagesAccess);
        return (int)pages->size();
    }

    bool IsTocFilePosFormatted(int filePos, const char* htmlStart, size_t htmlLen) const {
        if (filePos < 0 || !htmlStart || htmlLen == 0) {
            return false;
        }
        if (!IsEbookProgressiveLoadingInProgress()) {
            return true;
        }
        ScopedCritSec scope(&pagesAccess);
        if (!pages || pages->size() == 0) {
            return false;
        }
        int nFormatted = (int)pages->size();
        // which formatted page covers this filePos (same logic as GetNamedDestAtFilePos)
        int targetPage = 1;
        for (int i = 0; i < nFormatted; i++) {
            if (pages->at(i)->reparseIdx <= filePos) {
                targetPage = i + 1;
            } else {
                break;
            }
        }
        // earlier pages are fully formatted
        if (targetPage < nFormatted) {
            return true;
        }
        // on the last formatted page: reachable only if the formatter has passed filePos
        const char* htmlEnd = htmlStart + htmlLen;
        int maxOff = pages->at(nFormatted - 1)->reparseIdx;
        for (int i = 0; i < nFormatted; i++) {
            HtmlPage* page = pages->at(i);
            for (DrawInstr& instr : page->instructions) {
                if (instr.type != DrawInstrType::String && instr.type != DrawInstrType::RtlString) {
                    continue;
                }
                const char* s = instr.str.s;
                if (!s || s < htmlStart || s >= htmlEnd) {
                    continue;
                }
                int end = (int)(s - htmlStart) + (int)instr.str.len;
                if (end > maxOff) {
                    maxOff = end;
                }
            }
        }
        return filePos <= maxOff;
    }

  protected:
    Vec<HtmlPage*>* pages = nullptr;
    Vec<PageAnchor> anchors;
    // contains for each page the last anchor indicating
    // a break between two merged documents
    Vec<DrawInstr*> baseAnchors;
    // needed so that memory allocated by ResolveHtmlEntities isn't leaked
    Arena* allocator = nullptr;
    // TODO: still needed?
    mutable CRITICAL_SECTION pagesAccess;
    // page dimensions can vary between filetypes
    RectF pageRect;
    float pageBorder;
    EbookTypographyKind typographyKind = EbookTypographyKind::Latin;
    bool readerStyleMobi = false;

    void GetTransform(Matrix& m, float zoom, int rotation);
    bool ExtractPageAnchors();
    TempStr ExtractFontListTemp();
    void ConfigureMobiReaderStyle(const char* filePath, const ByteSlice& html);
    void InvalidateAllPageElements();

    virtual IPageElement* CreatePageLink(DrawInstr* link, Rect rect, int pageNo);

    Vec<DrawInstr>* GetHtmlPage(int pageNo);
    Vec<DrawInstr>* GetHtmlPageNoLock(int pageNo);
    HtmlPage* GetHtmlPage2(int pageNo);
    HtmlPage* GetHtmlPage2NoLock(int pageNo);

    mutable volatile LONG ebookLoadingInProgress = 0;
    mutable volatile LONG ebookLoadAbort = 0;
    HtmlFormatter* pendingFormatter = nullptr;
    // running state for incremental ExtractPageAnchors(): the last "page_marker"
    // base anchor seen so far. Persisted across calls so progressive loading
    // only scans newly formatted pages instead of rescanning everything.
    DrawInstr* lastBaseAnchor = nullptr;

    void AbortEbookLoading();
};

static IPageElement* NewEbookLink(DrawInstr* link, Rect rect, IPageDestination* dest, int pageNo = 0,
                                  bool showUrl = false) {
    if (!dest) {
        auto* pd = new PageDestination();
        pd->kind = kindDestinationLaunchURL;
        pd->rect = ToRectF(rect);
        // Keep the raw href/filepos so MOBI links can be resolved at click time.
        pd->value = str::Dup(link->str.s, link->str.len);
        pd->name = str::Dup(link->str.s, link->str.len);
        dest = pd;
    }

    auto res = new PageElementDestination(dest);
    res->pageNo = pageNo;
    res->rect = ToRectF(rect);

#if 0 // TODO: figure out
    if (showUrl) {
        res->value = strconv::FromHtmlUtf8(link->str.s, link->str.len);
    }
#endif
    return res;
}

static IPageElement* NewImageDataElement(int pageNo, Rect bbox, int imageID) {
    auto res = new PageElementImage();
    res->pageNo = pageNo;
    res->rect = ToRectF(bbox);
    res->imageID = imageID;
    return res;
}

static TocItem* newEbookTocItem(TocItem* parent, const char* title, IPageDestination* dest) {
    auto res = new TocItem(parent, title, 0);
    res->dest = dest;
    if (dest) {
        res->pageNo = PageDestGetPageNo(dest);
    }
    return res;
}

EngineEbook::EngineEbook() {
    pageCount = 0;
    // 560pt x 680pt - wider desktop page, closer to DaoKe-style readers
    float dpi = GetFileDPI();
    float w = 560.f * dpi / 72.f;
    float h = 680.f * dpi / 72.f;
    auto eBookUI = GetEBookUI();
    if (eBookUI) {
        if (eBookUI->layoutDx > 0) {
            w = eBookUI->layoutDx * dpi / 72.f;
        }
        if (eBookUI->layoutDy > 0) {
            h = eBookUI->layoutDy * dpi / 72.f;
        }
    }
    pageRect = RectF(0, 0, w, h);
    pageBorder = 0.2f * dpi;
    preferredLayout = preferredLayout = PageLayout(PageLayout::Type::Single);
    InitializeCriticalSection(&pagesAccess);
    allocator = ArenaNew();
}

void EngineEbook::ConfigureMobiReaderStyle(const char* filePath, const ByteSlice& html) {
    readerStyleMobi = IsReaderStyledMobiPath(filePath);
    if (!readerStyleMobi) {
        typographyKind = EbookTypographyKind::Latin;
        SetEbookTypographyKind(typographyKind);
        return;
    }

    typographyKind = DetectHtmlTypographyKind(html);
    SetEbookTypographyKind(typographyKind);

    float dpi = GetFileDPI();
    float w = layoutLatinMobiDxPt;
    float h = layoutLatinMobiDyPt;
    if (typographyKind == EbookTypographyKind::Cjk || typographyKind == EbookTypographyKind::Bilingual) {
        w = layoutCjkMobiDxPt;
        h = layoutCjkMobiDyPt;
    }

    auto eBookUI = GetEBookUI();
    if (eBookUI) {
        if (eBookUI->layoutDx > 100 && eBookUI->layoutDx != layoutLegacyMobiDxPt) {
            w = eBookUI->layoutDx;
        }
        if (eBookUI->layoutDy > 100 && eBookUI->layoutDy != layoutLegacyMobiDyPt) {
            h = eBookUI->layoutDy;
        }
    }

    pageRect = RectF(0, 0, w * dpi / 72.f, h * dpi / 72.f);
}

void EngineEbook::AbortEbookLoading() {
    InterlockedExchange(&ebookLoadAbort, 1);
    while (InterlockedCompareExchange(&ebookLoadingInProgress, 0, 0) != 0) {
        Sleep(10);
    }
    EnterCriticalSection(&pagesAccess);
    delete pendingFormatter;
    pendingFormatter = nullptr;
    LeaveCriticalSection(&pagesAccess);
}

EngineEbook::~EngineEbook() {
    AbortEbookLoading();

    EnterCriticalSection(&pagesAccess);

    if (pages) {
        for (HtmlPage* page : *pages) {
            DeleteVecMembers(page->elements);
        }
        DeleteVecMembers(*pages);
    }
    delete pages;

    LeaveCriticalSection(&pagesAccess);
    DeleteCriticalSection(&pagesAccess);
    ArenaDelete(allocator);
}

RectF EngineEbook::PageMediabox(int) {
    return pageRect;
}

RectF EngineEbook::PageContentBox(int pageNo, RenderTarget) {
    RectF mbox = PageMediabox(pageNo);
    mbox.Inflate(-pageBorder, -pageBorder);
    return mbox;
}

ByteSlice EngineEbook::GetFileData() {
    const char* fileName = FilePath();
    if (!fileName) {
        return {};
    }
    return file::ReadFile(fileName);
}

bool EngineEbook::SaveFileAs(const char* dstPath) {
    const char* srcPath = FilePath();
    if (!srcPath) {
        return false;
    }
    auto res = file::Copy(dstPath, srcPath, false);
    return res != 0;
}

// make RenderCache request larger tiles than per default
bool EngineEbook::HasClipOptimizations(int) {
    return false;
}

bool EngineEbook::BenchLoadPage(int) {
    return true;
}

void EngineEbook::GetTransform(Matrix& m, float zoom, int rotation) {
    GetBaseTransform(m, ToGdipRectF(pageRect), zoom, rotation);
}

Vec<DrawInstr>* EngineEbook::GetHtmlPageNoLock(int pageNo) {
    if (pageNo < 1 || !pages) {
        return nullptr;
    }
    int n = (int)pages->size();
    if (pageNo > n) {
        return nullptr;
    }
    return &pages->at(pageNo - 1)->instructions;
}

Vec<DrawInstr>* EngineEbook::GetHtmlPage(int pageNo) {
    ScopedCritSec scope(&pagesAccess);
    return GetHtmlPageNoLock(pageNo);
}

HtmlPage* EngineEbook::GetHtmlPage2NoLock(int pageNo) {
    if (pageNo < 1 || !pages) {
        return nullptr;
    }
    int n = (int)pages->size();
    if (pageNo > n) {
        return nullptr;
    }
    return pages->at(pageNo - 1);
}

HtmlPage* EngineEbook::GetHtmlPage2(int pageNo) {
    ScopedCritSec scope(&pagesAccess);
    return GetHtmlPage2NoLock(pageNo);
}

bool EngineEbook::ExtractPageAnchors() {
    ScopedCritSec scope(&pagesAccess);

    if (!pages) {
        return false;
    }

    // Incremental, append-only extraction. The pages vector only grows during
    // progressive (MOBI/AZW3) loading, so we resume from the first page that
    // hasn't been scanned yet. This keeps the total cost across all calls O(n)
    // instead of the O(n^2) of rescanning every page on each batch (which used
    // to freeze the UI on huge books because pagesAccess is held throughout).
    int n = (int)pages->size();
    for (int pageNo = (int)baseAnchors.size() + 1; pageNo <= n; pageNo++) {
        Vec<DrawInstr>* pageInstrs = &pages->at(pageNo - 1)->instructions;
        for (size_t k = 0; k < pageInstrs->size(); k++) {
            DrawInstr* i = &pageInstrs->at(k);
            if (DrawInstrType::Anchor != i->type) {
                continue;
            }
            anchors.Append(PageAnchor(i, pageNo));
            if (k < 2 && str::StartsWith(i->str.s + i->str.len, "\" page_marker />")) {
                lastBaseAnchor = i;
            }
        }
        baseAnchors.Append(lastBaseAnchor);
    }

    ReportIf(baseAnchors.size() != pages->size());
    return true;
}

RectF EngineEbook::Transform(const RectF& rect, int, float zoom, int rotation, bool inverse) {
    RectF rcF = rect; // TODO: un-needed conversion
    auto p1 = Gdiplus::PointF(rcF.x, rcF.y);
    auto p2 = Gdiplus::PointF(rcF.x + rcF.dx, rcF.y + rcF.dy);
    Gdiplus::PointF pts[2] = {p1, p2};
    Matrix m;
    GetTransform(m, zoom, rotation);
    if (inverse) {
        m.Invert();
    }
    m.TransformPoints(pts, 2);
    return RectF::FromXY(pts[0].X, pts[0].Y, pts[1].X, pts[1].Y);
}

RenderedBitmap* EngineEbook::RenderPage(RenderPageArgs& args) {
    auto pageNo = args.pageNo;
    auto zoom = args.zoom;
    auto rotation = args.rotation;

    RectF pageRc = args.pageRect ? *args.pageRect : PageMediabox(pageNo);
    Rect screen = Transform(pageRc, pageNo, zoom, rotation).Round();
    Point screenTL = screen.TL();
    screen.Offset(-screen.x, -screen.y);

    HANDLE hMap = nullptr;
    HBITMAP hbmp = CreateMemoryBitmap(screen.Size(), &hMap);
    HDC hDC = CreateCompatibleDC(nullptr);
    DeleteObject(SelectObject(hDC, hbmp));

    Graphics g(hDC);
    mui::InitGraphicsMode(&g);

    bool darkTheme = IsDarkThemeSelected();
    COLORREF bgCol;
    COLORREF textCol;
    if (readerStyleMobi && darkTheme) {
        bgCol = RgbToCOLORREF(0x000000);
        textCol = RgbToCOLORREF(0xE6E1D8);
    } else if (readerStyleMobi) {
        bgCol = RgbToCOLORREF(0xF7F3E8);
        textCol = RgbToCOLORREF(0x565047);
    } else if (darkTheme) {
        bgCol = RgbToCOLORREF(0x000000);
        textCol = RgbToCOLORREF(0xF9FAFB);
    } else {
        textCol = ThemePageRenderColors(bgCol);
    }
    Color pageBg = GdiRgbFromCOLORREF(bgCol);
    Color pageText = GdiRgbFromCOLORREF(textCol);
    Color pageLink = darkTheme ? Color(0xFF, 0x8F, 0xBC, 0xE6) : Color(0xFF, 0x31, 0x5F, 0x9C);
    if (!readerStyleMobi && !darkTheme) {
        pageLink = Color(0xFF, 0x00, 0x33, 0xCC);
    }
    SolidBrush tmpBrush(pageBg);
    Gdiplus::Rect screenR(ToGdipRect(screen));
    screenR.Inflate(1, 1);
    g.FillRectangle(&tmpBrush, screenR);

    Matrix m;
    GetTransform(m, zoom, rotation);
    m.Translate((float)-screenTL.x, (float)-screenTL.y, MatrixOrderAppend);
    g.SetTransform(&m);

    EbookAbortCookie* cookie = nullptr;
    if (args.cookie_out) {
        cookie = new EbookAbortCookie();
        *args.cookie_out = cookie;
    }

    ScopedCritSec scope(&pagesAccess);

    Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
    if (!pageInstrs) {
        DeleteDC(hDC);
        DeleteObject(hbmp);
        CloseHandle(hMap);
        return nullptr;
    }
    mui::ITextRender* textDraw = mui::TextRenderGdiplus::Create(&g);
    DrawHtmlPage(&g, textDraw, pageInstrs, pageBorder, pageBorder, false, pageText, cookie ? &cookie->abort : nullptr,
                 pageLink);
    delete textDraw;
    DeleteDC(hDC);

    if (cookie && cookie->abort) {
        DeleteObject(hbmp);
        CloseHandle(hMap);
        return nullptr;
    }

    return new RenderedBitmap(hbmp, screen.Size(), hMap);
}

static RectF GetInstrBboxF(DrawInstr& instr, float border) {
    RectF bbox = instr.bbox;
    bbox.Offset(border, border);
    return bbox;
}

static bool IsValidCharRelX(const float* charRelX, int charRelXLen, size_t strLen) {
    if (!charRelX || charRelXLen <= 0 || (size_t)charRelXLen != strLen) {
        return false;
    }
    if (charRelX[strLen] <= charRelX[0]) {
        return false;
    }
    for (size_t k = 0; k < strLen; k++) {
        if (charRelX[k + 1] <= charRelX[k]) {
            return false;
        }
    }
    return true;
}

static int WcharLenForDrawInstr(const DrawInstr& di) {
    WCHAR* buf = ToWStrTemp(di.str.s, di.str.len);
    int n = str::Leni(buf);
    n -= str::RemoveCharsInPlace(buf, L"\xad");
    return n;
}

static int CharIndexFromLocalX(const DrawInstr& di, float localX, int wlen, bool rtl) {
    if (wlen <= 0) {
        return 0;
    }
    if (localX < 0.f) {
        return 0;
    }
    if (IsValidCharRelX(di.charRelX, di.charRelXLen, (size_t)wlen)) {
        if (!rtl) {
            for (int k = 0; k < wlen; k++) {
                if (localX < di.charRelX[k + 1]) {
                    return k;
                }
            }
            return wlen - 1;
        }
        float xr = di.bbox.dx - localX;
        for (int k = 0; k < wlen; k++) {
            if (xr < di.charRelX[k + 1]) {
                return k;
            }
        }
        return wlen - 1;
    }
    if (di.bbox.dx <= 0.f) {
        return 0;
    }
    int k = (int)((localX / di.bbox.dx) * (float)wlen);
    if (k >= wlen) {
        k = wlen - 1;
    }
    if (k < 0) {
        k = 0;
    }
    return k;
}

static bool IsLatinLetter(WCHAR c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

static int LatinStartInRun(const WCHAR* run, int wlen) {
    for (int k = 0; k < wlen; k++) {
        if (IsLatinLetter(run[k])) {
            return k;
        }
    }
    return wlen;
}

// Latin in a mixed run is drawn with DrawString from charRelX[latinStart] to bbox.dx;
// per-char charRelX cells can be narrower than the ink, so map clicks in that tail
// proportionally across the drawn span instead of using compressed charRelX cells.
static int CharIndexFromLocalXMixed(const DrawInstr& di, float localX, int wlen, bool rtl, const WCHAR* run) {
    int idx = CharIndexFromLocalX(di, localX, wlen, rtl);
    if (rtl || !IsValidCharRelX(di.charRelX, di.charRelXLen, (size_t)wlen)) {
        return idx;
    }
    int latinStart = LatinStartInRun(run, wlen);
    if (latinStart >= wlen) {
        return idx;
    }
    float latinX0 = di.charRelX[latinStart];
    if (localX < latinX0) {
        return idx;
    }
    float span = di.bbox.dx - latinX0;
    if (span <= 0.f) {
        return idx;
    }
    float xIn = localX - latinX0;
    if (xIn >= span) {
        return wlen - 1;
    }
    int latinCount = wlen - latinStart;
    if (latinCount <= 0) {
        return idx;
    }
    int sub = (int)((xIn / span) * (float)latinCount);
    if (sub >= latinCount) {
        sub = latinCount - 1;
    }
    if (sub < 0) {
        sub = 0;
    }
    return latinStart + sub;
}

static bool RangeIsNonCjkLatin(const WCHAR* run, int start, int end) {
    if (end <= start) {
        return false;
    }
    for (int i = start; i < end; i++) {
        WCHAR c = run[i];
        if (!((IsCharAlphaNumeric(c) || c == L'_') && (unsigned short)c < 0x2E80)) {
            return false;
        }
    }
    return true;
}

static RectF LatinRangeBBoxFromInstr(const DrawInstr& di, int latinStart, int wlen, int charStart, int charEnd,
                                     float border) {
    RectF rb = GetInstrBboxF(const_cast<DrawInstr&>(di), border);
    float latinX0 = di.charRelX[latinStart];
    float span = di.bbox.dx - latinX0;
    if (span <= 0.f) {
        span = di.charRelX[wlen] - latinX0;
    }
    int latinCount = wlen - latinStart;
    if (latinCount <= 0 || span <= 0.f) {
        return rb;
    }
    float f0 = (float)(charStart - latinStart) / (float)latinCount;
    float f1 = (float)(charEnd - latinStart) / (float)latinCount;
    if (f0 < 0.f) {
        f0 = 0.f;
    }
    if (f1 > 1.f) {
        f1 = 1.f;
    }
    if (f1 < f0) {
        f1 = f0;
    }
    float x0 = rb.x + latinX0 + span * f0;
    float dx = span * (f1 - f0);
    if (dx < 1.f) {
        dx = 1.f;
    }
    return RectF(x0, rb.y, dx, rb.dy);
}

static RectF CharBBoxFromInstr(const DrawInstr& di, int charIdx, int wlen, float border, bool rtl) {
    RectF rb = GetInstrBboxF(const_cast<DrawInstr&>(di), border);
    if (wlen <= 0 || charIdx < 0) {
        return rb;
    }
    if (charIdx >= wlen) {
        charIdx = wlen - 1;
    }
    if (IsValidCharRelX(di.charRelX, di.charRelXLen, (size_t)wlen)) {
        float x0;
        float x1;
        if (!rtl) {
            x0 = rb.x + di.charRelX[charIdx];
            x1 = rb.x + di.charRelX[charIdx + 1];
        } else {
            x1 = rb.x + rb.dx - di.charRelX[charIdx];
            x0 = rb.x + rb.dx - di.charRelX[charIdx + 1];
        }
        if (x1 - x0 < 1.f) {
            x1 = x0 + 1.f;
        }
        return RectF(x0, rb.y, x1 - x0, rb.dy);
    }
    float cw = wlen > 0 ? rb.dx / (float)wlen : rb.dx;
    if (cw < 1.f) {
        cw = 1.f;
    }
    float x = rtl ? rb.x + (float)(wlen - charIdx - 1) * cw : rb.x + (float)charIdx * cw;
    return RectF(x, rb.y, cw, rb.dy);
}

static float LocalXInDrawInstr(float pagePtX, float pageBorder, float instrBboxX) {
    // The GDI+ per-glyph renderer draws glyph k at the float origin
    // (pageBorder + instrBboxX) + charRelX[k]; match it exactly (no int trunc)
    // so hit-test, highlight and on-screen glyphs share one coordinate origin.
    float drawOriginX = pageBorder + instrBboxX;
    return pagePtX - drawOriginX;
}

static uint DistSqPointToRectF(PointF pt, const RectF& r) {
    float dx = 0.f;
    float dy = 0.f;
    if (pt.x < r.x) {
        dx = r.x - pt.x;
    } else if (pt.x > r.x + r.dx) {
        dx = pt.x - (r.x + r.dx);
    }
    if (pt.y < r.y) {
        dy = r.y - pt.y;
    } else if (pt.y > r.y + r.dy) {
        dy = pt.y - (r.y + r.dy);
    }
    return (uint)(dx * dx + dy * dy);
}

bool EngineEbook::HitTestText(int pageNo, PointF pagePt, EbookTextHit* hitOut) {
    if (!hitOut) {
        return false;
    }
    *hitOut = {};

    ScopedCritSec scope(&pagesAccess);
    Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
    if (!pageInstrs) {
        return false;
    }

    int bestIdx = -1;
    int bestChar = 0;
    uint bestDist = UINT_MAX;
    RectF bestBox;

    size_t n = pageInstrs->size();
    for (size_t i = 0; i < n; i++) {
        DrawInstr& di = pageInstrs->at(i);
        if (di.type != DrawInstrType::String && di.type != DrawInstrType::RtlString) {
            continue;
        }
        RectF runBox = GetInstrBboxF(di, pageBorder);
        if (!runBox.Contains(pagePt)) {
            continue;
        }
        bool rtl = di.type == DrawInstrType::RtlString;
        int wlen = WcharLenForDrawInstr(di);
        if (wlen <= 0) {
            continue;
        }
        float localX = LocalXInDrawInstr(pagePt.x, pageBorder, di.bbox.x);
        WCHAR* run = ToWStrTemp(di.str.s, di.str.len);
        str::RemoveCharsInPlace(run, L"\xad");
        int charIdx = CharIndexFromLocalXMixed(di, localX, wlen, rtl, run);
        RectF charBox = CharBBoxFromInstr(di, charIdx, wlen, pageBorder, rtl);
        uint dist = DistSqPointToRectF(pagePt, charBox);
        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = (int)i;
            bestChar = charIdx;
            bestBox = charBox;
        }
    }

    if (bestIdx < 0) {
        return false;
    }

    DrawInstr& di = pageInstrs->at((size_t)bestIdx);
    hitOut->instrIndex = bestIdx;
    hitOut->charIndex = bestChar;
    hitOut->charLen = 1;
    hitOut->dirRtl = di.type == DrawInstrType::RtlString;
    hitOut->bbox = bestBox;
    return true;
}

TempWStr EngineEbook::GetRunTextTemp(int pageNo, int instrIndex) {
    ScopedCritSec scope(&pagesAccess);
    Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
    if (!pageInstrs || !pageInstrs->isValidIndex(instrIndex)) {
        return nullptr;
    }
    DrawInstr& di = pageInstrs->at(instrIndex);
    if (di.type != DrawInstrType::String && di.type != DrawInstrType::RtlString) {
        return nullptr;
    }
    WCHAR* buf = ToWStrTemp(di.str.s, di.str.len);
    str::RemoveCharsInPlace(buf, L"\xad");
    return buf;
}

bool EngineEbook::GetCharRangeBbox(int pageNo, int instrIndex, int charStart, int charEnd, RectF* out) {
    if (!out || charEnd <= charStart) {
        return false;
    }
    ScopedCritSec scope(&pagesAccess);
    Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
    if (!pageInstrs || !pageInstrs->isValidIndex(instrIndex)) {
        return false;
    }
    DrawInstr& di = pageInstrs->at(instrIndex);
    if (di.type != DrawInstrType::String && di.type != DrawInstrType::RtlString) {
        return false;
    }
    bool rtl = di.type == DrawInstrType::RtlString;
    int wlen = WcharLenForDrawInstr(di);
    if (charStart < 0) {
        charStart = 0;
    }
    if (charEnd > wlen) {
        charEnd = wlen;
    }
    WCHAR* run = ToWStrTemp(di.str.s, di.str.len);
    str::RemoveCharsInPlace(run, L"\xad");
    if (IsValidCharRelX(di.charRelX, di.charRelXLen, (size_t)wlen) && RangeIsNonCjkLatin(run, charStart, charEnd)) {
        int latinStart = LatinStartInRun(run, wlen);
        if (charStart >= latinStart) {
            *out = LatinRangeBBoxFromInstr(di, latinStart, wlen, charStart, charEnd, pageBorder);
            return true;
        }
    }
    RectF u;
    bool any = false;
    for (int i = charStart; i < charEnd; i++) {
        RectF b = CharBBoxFromInstr(di, i, wlen, pageBorder, rtl);
        u = any ? u.Union(b) : b;
        any = true;
    }
    if (!any) {
        return false;
    }
    *out = u;
    return true;
}

bool EngineIsFixedLayoutEbook(EngineBase* engine) {
    if (!engine) {
        return false;
    }
    Kind k = engine->kind;
    return k == kindEngineEpub || k == kindEngineMobi || k == kindEngineFb2 || k == kindEnginePdb ||
           k == kindEngineHtml || k == kindEngineTxt;
}

bool EngineEbookHitTestText(EngineBase* engine, int pageNo, PointF pagePt, EbookTextHit* hitOut) {
    if (!EngineIsFixedLayoutEbook(engine)) {
        return false;
    }
    return static_cast<EngineEbook*>(engine)->HitTestText(pageNo, pagePt, hitOut);
}

TempWStr EngineEbookGetRunTextTemp(EngineBase* engine, int pageNo, int instrIndex) {
    if (!EngineIsFixedLayoutEbook(engine) || instrIndex < 0) {
        return nullptr;
    }
    return static_cast<EngineEbook*>(engine)->GetRunTextTemp(pageNo, instrIndex);
}

bool EngineEbookGetCharRangeBbox(EngineBase* engine, int pageNo, int instrIndex, int charStart, int charEnd,
                                 RectF* out) {
    if (!EngineIsFixedLayoutEbook(engine)) {
        return false;
    }
    return static_cast<EngineEbook*>(engine)->GetCharRangeBbox(pageNo, instrIndex, charStart, charEnd, out);
}

static Rect GetInstrBbox(DrawInstr& instr, float pageBorder) {
    RectF bbox(instr.bbox.x, instr.bbox.y, instr.bbox.dx, instr.bbox.dy);
    bbox.Offset(pageBorder, pageBorder);
    return bbox.Round();
}

static void AppendStringCoords(Vec<Rect>& coords, Rect bbox, size_t strLen, bool rtl, const float* charRelX,
                               int charRelXLen) {
    bool useRelX = IsValidCharRelX(charRelX, charRelXLen, strLen);
    if (!rtl) {
        for (size_t k = 0; k < strLen; k++) {
            int x;
            int w;
            if (useRelX) {
                x = (int)(bbox.x + charRelX[k] + 0.5f);
                w = (int)(charRelX[k + 1] - charRelX[k] + 0.5f);
            } else {
                double cwidth = 1.0 * bbox.dx / strLen;
                x = (int)(bbox.x + k * cwidth);
                w = (int)cwidth;
            }
            if (w < 1) {
                w = 1;
            }
            coords.Append(Rect(x, bbox.y, w, bbox.dy));
        }
    } else {
        for (size_t k = 0; k < strLen; k++) {
            int x;
            int w;
            if (useRelX) {
                w = (int)(charRelX[k + 1] - charRelX[k] + 0.5f);
                x = (int)(bbox.x + bbox.dx - charRelX[k + 1] + 0.5f);
            } else {
                double cwidth = 1.0 * bbox.dx / strLen;
                w = (int)cwidth;
                x = (int)(bbox.x + (strLen - k - 1) * cwidth);
            }
            if (w < 1) {
                w = 1;
            }
            coords.Append(Rect(x, bbox.y, w, bbox.dy));
        }
    }
}

PageText EngineEbook::ExtractPageText(int pageNo) {
    const WCHAR* lineSep = L"\n";
    ScopedCritSec scope(&pagesAccess);

    InterlockedIncrement(&gAllowAllocFailure);
    defer {
        InterlockedDecrement(&gAllowAllocFailure);
    };

    WStrBuilder content;
    Vec<Rect> coords;
    bool insertSpace = false;

    Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
    if (!pageInstrs) {
        return {};
    }
    for (DrawInstr& i : *pageInstrs) {
        Rect bbox = GetInstrBbox(i, pageBorder);
        switch (i.type) {
            case DrawInstrType::String:
                if (coords.size() > 0 &&
                    (bbox.x < coords.Last().BR().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    ReportIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = bbox.x - coords.Last().BR().x;
                    if (swidth > 0) {
                        content.AppendChar(' ');
                        int hitW = swidth;
                        constexpr int kMaxSpaceHitW = 4;
                        if (hitW > kMaxSpaceHitW) {
                            hitW = kMaxSpaceHitW;
                        }
                        coords.Append(Rect(bbox.x - hitW, bbox.y, hitW, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    AutoFreeWStr s(strconv::FromHtmlUtf8(i.str.s, i.str.len));
                    content.Append(s);
                    size_t len = str::Len(s);
                    AppendStringCoords(coords, bbox, len, false, i.charRelX, i.charRelXLen);
                }
                break;
            case DrawInstrType::RtlString:
                if (coords.size() > 0 &&
                    (bbox.BR().x > coords.Last().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    ReportIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = coords.Last().x - bbox.BR().x;
                    if (swidth > 0) {
                        content.AppendChar(' ');
                        int hitW = swidth;
                        constexpr int kMaxSpaceHitW = 4;
                        if (hitW > kMaxSpaceHitW) {
                            hitW = kMaxSpaceHitW;
                        }
                        coords.Append(Rect(bbox.BR().x, bbox.y, hitW, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    AutoFreeWStr s(strconv::FromHtmlUtf8(i.str.s, i.str.len));
                    content.Append(s);
                    size_t len = str::Len(s);
                    AppendStringCoords(coords, bbox, len, true, i.charRelX, i.charRelXLen);
                }
                break;
            case DrawInstrType::ElasticSpace:
            case DrawInstrType::FixedSpace:
                insertSpace = true;
                break;
        }
    }
    if (content.size() > 0 && !str::EndsWith(content.Get(), lineSep)) {
        content.Append(lineSep);
        coords.AppendBlanks(str::Len(lineSep));
    }
    ReportIf(coords.size() != content.size());

    PageText res;
    res.len = (int)content.size();
    res.text = content.StealData();
    res.coords = coords.StealData();
    return res;
}

PageTextUtf8 EngineEbook::ExtractPageTextUtf8(int pageNo) {
    const char* lineSep = "\n";
    ScopedCritSec scope(&pagesAccess);

    InterlockedIncrement(&gAllowAllocFailure);
    defer {
        InterlockedDecrement(&gAllowAllocFailure);
    };

    StrBuilder content;
    Vec<Rect> coords;
    bool insertSpace = false;

    Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
    if (!pageInstrs) {
        return {};
    }
    for (DrawInstr& i : *pageInstrs) {
        Rect bbox = GetInstrBbox(i, pageBorder);
        switch (i.type) {
            case DrawInstrType::String:
                if (coords.size() > 0 &&
                    (bbox.x < coords.Last().BR().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    ReportIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = bbox.x - coords.Last().BR().x;
                    if (swidth > 0) {
                        content.AppendChar(' ');
                        int hitW = swidth;
                        constexpr int kMaxSpaceHitW = 4;
                        if (hitW > kMaxSpaceHitW) {
                            hitW = kMaxSpaceHitW;
                        }
                        coords.Append(Rect(bbox.x - hitW, bbox.y, hitW, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    TempStr s = strconv::FromHtmlUtf8Temp(i.str.s, i.str.len);
                    size_t len = str::Len(s);
                    content.Append(s);
                    if (len > 0) {
                        double cwidth = 1.0 * bbox.dx / (double)len;
                        for (size_t k = 0; k < len; k++) {
                            coords.Append(Rect((int)(bbox.x + (double)k * cwidth), bbox.y, (int)cwidth, bbox.dy));
                        }
                    }
                }
                break;
            case DrawInstrType::RtlString:
                if (coords.size() > 0 &&
                    (bbox.BR().x > coords.Last().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    ReportIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = coords.Last().x - bbox.BR().x;
                    if (swidth > 0) {
                        content.AppendChar(' ');
                        int hitW = swidth;
                        constexpr int kMaxSpaceHitW = 4;
                        if (hitW > kMaxSpaceHitW) {
                            hitW = kMaxSpaceHitW;
                        }
                        coords.Append(Rect(bbox.BR().x, bbox.y, hitW, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    TempStr s = strconv::FromHtmlUtf8Temp(i.str.s, i.str.len);
                    size_t len = str::Len(s);
                    content.Append(s);
                    if (len > 0) {
                        double cwidth = 1.0 * bbox.dx / (double)len;
                        for (size_t k = 0; k < len; k++) {
                            coords.Append(
                                Rect((int)(bbox.x + (double)(len - k - 1) * cwidth), bbox.y, (int)cwidth, bbox.dy));
                        }
                    }
                }
                break;
            case DrawInstrType::ElasticSpace:
            case DrawInstrType::FixedSpace:
                insertSpace = true;
                break;
        }
    }
    if (content.size() > 0 && !str::EndsWith(content.Get(), lineSep)) {
        content.Append(lineSep);
        coords.AppendBlanks(str::Len(lineSep));
    }
    ReportIf(coords.size() != content.size());

    PageTextUtf8 res;
    res.len = (int)content.size();
    res.text = content.StealData();
    res.coords = coords.StealData();
    return res;
}

static void SetEbookTocScrollName(PageDestination* pd, const char* url) {
    if (!pd || !url || !*url) {
        return;
    }
    str::Free(pd->name);
    pd->name = str::Dup(url);
}

static IPageDestination* NewDeferredEbookTocDest(const char* url) {
    auto* pd = static_cast<PageDestination*>(NewSimpleDest(0, RectF()));
    SetEbookTocScrollName(pd, url);
    return pd;
}

static bool IsExternalEbookUrl(const char* url) {
    return url::IsAbsolute(url) && !str::StartsWith(url, "kindle:pos:") && !str::StartsWith(url, "filepos:");
}

static bool IsMobiInternalLinkUrl(const char* url) {
    if (!url || !*url) {
        return false;
    }
    if (str::StartsWith(url, "kindle:pos:") || str::StartsWith(url, "filepos:")) {
        return true;
    }
    if (*url >= '0' && *url <= '9') {
        return true;
    }
    return !url::IsAbsolute(url);
}

IPageElement* EngineEbook::CreatePageLink(DrawInstr* link, Rect rect, int pageNo) {
    char* url = strconv::FromHtmlUtf8Temp(link->str.s, link->str.len);
    if (!url) {
        return nullptr;
    }

    // MOBI7 filepos digits / filepos:... / KF8 kindle:pos:... must not be treated as web URLs.
    if (kind == kindEngineMobi && IsMobiInternalLinkUrl(url)) {
        char* urlOwned = str::Dup(link->str.s, link->str.len);
        if (!urlOwned) {
            return nullptr;
        }
        IPageDestination* dest = NewDeferredEbookTocDest(urlOwned);
        str::Free(urlOwned);
        return NewEbookLink(link, rect, dest, pageNo);
    }

    if (IsExternalEbookUrl(url)) {
        IPageDestination* extDest = NewSimpleDest(0, RectF(), 0.f, url);
        return NewEbookLink(link, rect, extDest, pageNo);
    }

    // baseAnchors is mutated by ExtractPageAnchors() on the background load
    // thread during progressive loading; lock so we read a consistent view.
    ScopedCritSec scope(&pagesAccess);
    DrawInstr* baseAnchor = nullptr;
    if (baseAnchors.isValidIndex(pageNo - 1)) {
        baseAnchor = baseAnchors.at(pageNo - 1);
    }
    char* urlOwned = nullptr;
    if (baseAnchor) {
        char* basePath = str::DupTemp(baseAnchor->str.s, baseAnchor->str.len);
        TempStr relPath = ResolveHtmlEntitiesTemp(link->str.s, link->str.len);
        AutoFreeStr absPath = NormalizeURL(relPath, basePath);
        urlOwned = str::Dup(absPath.Get());
    } else {
        urlOwned = str::Dup(link->str.s, link->str.len);
    }
    if (!urlOwned) {
        return nullptr;
    }

    IPageDestination* dest = nullptr;
    if (kind == kindEngineMobi && IsMobiInternalLinkUrl(urlOwned)) {
        // Resolve at click time so progressive formatting and filepos/kindle:pos mapping stay current.
        dest = NewDeferredEbookTocDest(urlOwned);
    } else {
        dest = GetNamedDest(urlOwned);
    }
    str::Free(urlOwned);
    if (!dest) {
        return nullptr;
    }
    return NewEbookLink(link, rect, dest, pageNo);
}

Vec<IPageElement*> EngineEbook::GetElements(int pageNo) {
    HtmlPage* pi = GetHtmlPage2(pageNo);
    if (!pi) {
        return Vec<IPageElement*>();
    }
    if (pi->gotElements && kind == kindEngineMobi) {
        for (IPageElement* el : pi->elements) {
            if (!el->Is(kindPageElementDest)) {
                continue;
            }
            IPageDestination* d = el->AsLink();
            if (!d || d->GetKind() != kindDestinationLaunchURL) {
                continue;
            }
            char* v = PageDestGetValue(d);
            char* n = PageDestGetName(d);
            if ((!v || !*v) && (!n || !*n)) {
                for (IPageElement* e : pi->elements) {
                    delete e;
                }
                pi->elements.Reset();
                pi->gotElements = false;
                break;
            }
        }
    }
    if (pi->gotElements) {
        return pi->elements;
    }
    pi->gotElements = true;
    Vec<IPageElement*>& els = pi->elements;

    Vec<DrawInstr>* pageInstrs = &pi->instructions;
    size_t n = pageInstrs->size();
    for (size_t idx = 0; idx < n; idx++) {
        DrawInstr& i = pageInstrs->at(idx);
        if (DrawInstrType::Image == i.type) {
            auto box = GetInstrBbox(i, pageBorder);
            auto el = NewImageDataElement(pageNo, box, (int)idx);
            els.Append(el);
        } else if (DrawInstrType::LinkStart == i.type && !i.bbox.IsEmpty()) {
            IPageElement* link = CreatePageLink(&i, GetInstrBbox(i, pageBorder), pageNo);
            if (link) {
                els.Append(link);
            }
        }
    }

    return els;
}

static RenderedBitmap* getImageFromData(const ByteSlice& imageData) {
    HBITMAP hbmp = nullptr;
    Bitmap* bmp = BitmapFromData(imageData);
    if (!bmp || bmp->GetHBITMAP((ARGB)Color::White, &hbmp) != Ok) {
        delete bmp;
        return nullptr;
    }
    Size size(bmp->GetWidth(), bmp->GetHeight());
    delete bmp;
    return new RenderedBitmap(hbmp, size);
}

RenderedBitmap* EngineEbook::GetImageForPageElement(IPageElement* iel) {
    ReportIf(iel->GetKind() != kindPageElementImage);
    PageElementImage* el = (PageElementImage*)iel;
    int pageNo = el->pageNo;
    int idx = el->imageID;
    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    if (!pageInstrs || !pageInstrs->isValidIndex(idx)) {
        return nullptr;
    }
    auto&& i = pageInstrs->at(idx);
    ReportIf(i.type != DrawInstrType::Image);
    return getImageFromData(i.GetImage());
}

// don't delete the result
IPageElement* EngineEbook::GetElementAtPos(int pageNo, PointF pt) {
    auto els = GetElements(pageNo);

    for (auto& el : els) {
        if (el->GetRect().Contains(pt)) {
            return el;
        }
    }
    return nullptr;
}

IPageDestination* EngineEbook::GetNamedDest(const char* name) {
    // anchors/baseAnchors grow on the background load thread during progressive
    // loading; lock so concurrent appends (and possible reallocations) don't
    // corrupt the iteration below. CRITICAL_SECTION is recursive, so callers
    // that already hold pagesAccess (e.g. CreatePageLink) remain safe.
    ScopedCritSec scope(&pagesAccess);

    const char* id = name;
    if (str::FindChar(id, '#')) {
        id = str::FindChar(id, '#') + 1;
    }

    // if the name consists of both path and ID,
    // try to first skip to the page with the desired
    // path before looking for the ID to allow
    // for the same ID to be reused on different pages
    DrawInstr* baseAnchor = nullptr;
    int basePageNo = 0;
    if (id > name + 1) {
        size_t base_len = id - name - 1;
        for (size_t i = 0; i < baseAnchors.size(); i++) {
            DrawInstr* anchor = baseAnchors.at(i);
            if (anchor && base_len == anchor->str.len && str::EqNI(name, anchor->str.s, base_len)) {
                baseAnchor = anchor;
                basePageNo = (int)i + 1;
                break;
            }
        }
    }

    size_t id_len = str::Len(id);
    for (size_t i = 0; i < anchors.size(); i++) {
        PageAnchor* anchor = &anchors.at(i);
        if (baseAnchor) {
            if (anchor->instr == baseAnchor) {
                baseAnchor = nullptr;
            }
            continue;
        }
        // note: at least CHM treats URLs as case-independent
        if (id_len == anchor->instr->str.len && str::EqNI(id, anchor->instr->str.s, id_len)) {
            RectF rect(0, anchor->instr->bbox.y + pageBorder, pageRect.dx, 10);
            rect.Inflate(-pageBorder, 0);
            return NewSimpleDest(anchor->pageNo, rect);
        }
    }

    // don't fail if an ID doesn't exist in a merged document
    if (basePageNo != 0) {
        RectF rect(0, pageBorder, pageRect.dx, 10);
        rect.Inflate(-pageBorder, 0);
        return NewSimpleDest(basePageNo, rect);
    }

    return nullptr;
}

TempStr EngineEbook::ExtractFontListTemp() {
    ScopedCritSec scope(&pagesAccess);

    Vec<mui::CachedFont*> seenFonts;
    StrVec fonts;

    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr>* pageInstrs = GetHtmlPageNoLock(pageNo);
        if (!pageInstrs) {
            continue;
        }

        for (DrawInstr& i : *pageInstrs) {
            if (DrawInstrType::SetFont != i.type || !i.font || seenFonts.Contains(i.font)) {
                continue;
            }
            seenFonts.Append(i.font);

            // Use CachedFont::name (plain WCHAR string) instead of accessing
            // the Gdiplus::Font object. GDI+ Font objects have thread affinity:
            // they are created on the formatter thread, but this function can
            // be called from GetFontsThread (another background thread) via
            // GetPropertyTemp(kPropFontList). Calling font->GetFamily() from
            // the wrong thread crashes Gdiplus with 0xC0000005.
            const WCHAR* fontNameW = i.font->GetName();
            if (!fontNameW || !*fontNameW) {
                continue;
            }
            char* fontName = ToUtf8Temp(fontNameW);
            AppendIfNotExists(&fonts, fontName);
        }
    }
    if (fonts.Size() == 0) {
        return nullptr;
    }

    SortNatural(&fonts);
    return JoinTemp(&fonts, "\n");
}

static void AppendTocItem(TocItem*& root, TocItem* item, int level) {
    if (!root) {
        root = item;
        return;
    }
    // find the last child at each level, until finding the parent of the new item
    TocItem* r2 = root;
    while (--level > 0) {
        for (; r2->next; r2 = r2->next) {
            ;
        }
        if (r2->child) {
            r2 = r2->child;
        } else {
            r2->child = item;
            return;
        }
    }
    r2->AddSiblingAtEnd(item);
}

class EbookTocBuilder : public EbookTocVisitor {
    EngineBase* engine = nullptr;
    TocItem* root = nullptr;
    int idCounter = 0;
    bool isIndex = false;

  public:
    explicit EbookTocBuilder(EngineBase* engine) { this->engine = engine; }

    void Visit(const char* name, const char* url, int level) override;

    TocItem* GetRoot() { return root; }
    void SetIsIndex(bool value) { isIndex = value; }
};

void EngineEbook::InvalidateAllPageElements() {
    ScopedCritSec scope(&pagesAccess);
    if (!pages) {
        return;
    }
    for (HtmlPage* page : *pages) {
        if (!page || !page->gotElements) {
            continue;
        }
        for (IPageElement* el : page->elements) {
            delete el;
        }
        page->elements.Reset();
        page->gotElements = false;
    }
}

bool EngineEbook::HandleLink(IPageDestination* dest, ILinkHandler* linkHandler) {
    ReportIf(!dest || !linkHandler);
    if (!dest || !linkHandler) {
        return false;
    }

    Kind destKind = dest->GetKind();
    int destPage = PageDestGetPageNo(dest);
    char* url = PageDestGetName(dest);
    if (!url || !*url) {
        url = PageDestGetValue(dest);
    }

    if (kind == kindEngineMobi && url && *url && IsMobiInternalLinkUrl(url) &&
        (destPage <= 0 || destKind == kindDestinationLaunchURL)) {
        IPageDestination* resolved = GetNamedDest(url);
        if (resolved && PageDestGetPageNo(resolved) > 0) {
            linkHandler->GotoLink(resolved);
            delete resolved;
            return true;
        }
    } else if (destKind == kindDestinationScrollTo && destPage <= 0 && url && *url) {
        IPageDestination* resolved = GetNamedDest(url);
        if (resolved) {
            linkHandler->GotoLink(resolved);
            delete resolved;
            return true;
        }
    }
    linkHandler->GotoLink(dest);
    return true;
}

void EbookTocBuilder::Visit(const char* name, const char* url, int level) {
    IPageDestination* dest;
    if (!url) {
        dest = nullptr;
    } else if (IsExternalEbookUrl(url)) {
        dest = NewSimpleDest(0, RectF(), 0.f, url);
    } else {
        dest = engine->GetNamedDest(url);
        if (!dest && str::FindChar(url, '%')) {
            char* decodedUrl = str::DupTemp(url);
            url::DecodeInPlace(decodedUrl);
            dest = engine->GetNamedDest(decodedUrl);
        }
        // TOC is often built before all anchors/pages exist (progressive MOBI load).
        // Keep the raw link so GoToTocLink can resolve it at click time.
        if (!dest) {
            dest = NewDeferredEbookTocDest(url);
        }
    }

    // TODO: send parent to newEbookTocItem
    TocItem* item = newEbookTocItem(nullptr, name, dest);
    if (dest && url && dest->GetKind() == kindDestinationScrollTo) {
        auto* pd = static_cast<PageDestination*>(dest);
        if (!pd->name) {
            SetEbookTocScrollName(pd, url);
        }
    }
    item->id = ++idCounter;
    if (isIndex) {
        item->pageNo = 0;
        level++;
    }
    AppendTocItem(root, item, level);
}

/* EngineBase for handling EPUB documents */

class EngineEpub : public EngineEbook {
  public:
    EngineEpub();
    ~EngineEpub() override;
    EngineBase* Clone() override;

    ByteSlice GetFileData() override;
    bool SaveFileAs(const char* copyFileName) override;

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    TocTree* GetToc() override;

    static EngineBase* CreateFromFile(const char* fileName);
    static EngineBase* CreateFromStream(IStream* stream);

  protected:
    EpubDoc* doc = nullptr;
    IStream* stream = nullptr;
    TocTree* tocTree = nullptr;

    bool Load(const char* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

EngineEpub::EngineEpub() : EngineEbook() {
    kind = kindEngineEpub;
    str::ReplaceWithCopy(&defaultExt, ".epub");
}

EngineEpub::~EngineEpub() {
    delete doc;
    delete tocTree;
    if (stream) {
        stream->Release();
    }
}

EngineBase* EngineEpub::Clone() {
    if (stream) {
        auto res = CreateFromStream(stream);
        if (!res) {
            logf("EngineEpub::Clone() failed: CreateFromStream() failed\n");
        }
        return res;
    }
    const char* path = FilePath();
    if (path) {
        auto res = CreateFromFile(path);
        if (!res) {
            logf("EngineEpub::Clone() failed: CreateFromFile('%s') failed\n", path);
        }
        return res;
    }
    logf("EngineEpub::Clone() failed: no stream or file path\n");
    return nullptr;
}

bool EngineEpub::Load(const char* fileName) {
    SetFilePath(fileName);
    if (dir::Exists(fileName)) {
        // load uncompressed documents as a recompressed ZIP stream
        ScopedComPtr<IStream> zipStream(OpenDirAsZipStream(fileName, true));
        if (!zipStream) {
            return false;
        }
        return Load(zipStream);
    }
    doc = EpubDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool EngineEpub::Load(IStream* stream) {
    stream->AddRef();
    this->stream = stream;
    doc = EpubDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool EngineEpub::FinishLoading() {
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args{};
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), EbookTypographyKind::Latin);
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::GdiplusQuick;

    if (IsCreateEngineForThumbnail()) {
        EpubFormatter formatter(&args, doc);
        pages = FormatFirstHtmlPage(formatter);
        pageCount = (int)pages->size();
        return pageCount > 0;
    }

    pages = EpubFormatter(&args, doc).FormatAllPages(false);

    // must set pageCount before ExtractPageAnchors
    pageCount = (int)pages->size();
    if (!ExtractPageAnchors()) {
        return false;
    }

    preferredLayout = PageLayout(PageLayout::Type::Book);
    if (doc->IsRTL()) {
        preferredLayout.r2l = true;
    }

    return pageCount > 0;
}

ByteSlice EngineEpub::GetFileData() {
    const char* path = FilePath();
    return GetStreamOrFileData(stream, path);
}

bool EngineEpub::SaveFileAs(const char* dstPath) {
    if (stream) {
        ByteSlice d = GetDataFromStream(stream, nullptr);
        bool ok = !d.empty() && file::WriteFile(dstPath, d);
        d.Free();
        if (ok) {
            return true;
        }
    }
    const char* srcPath = FilePath();
    if (!srcPath) {
        return false;
    }
    return file::Copy(dstPath, srcPath, false);
}

TocTree* EngineEpub::GetToc() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    TocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

EngineBase* EngineEpub::CreateFromFile(const char* fileName) {
    EngineEpub* engine = new EngineEpub();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* EngineEpub::CreateFromStream(IStream* stream) {
    EngineEpub* engine = new EngineEpub();
    if (!engine->Load(stream)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineEpubFromFile(const char* fileName) {
    return EngineEpub::CreateFromFile(fileName);
}

EngineBase* CreateEngineEpubFromStream(IStream* stream) {
    return EngineEpub::CreateFromStream(stream);
}

/* EngineBase for handling FictionBook2 documents */

class EngineFb2 : public EngineEbook {
  public:
    EngineFb2() : EngineEbook() {
        kind = kindEngineFb2;
        str::ReplaceWithCopy(&defaultExt, ".fb2");
    }
    ~EngineFb2() override {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        const char* fileName = FilePath();
        if (!fileName) {
            return nullptr;
        }
        return CreateFromFile(fileName);
    }

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    TocTree* GetToc() override;

    static EngineBase* CreateFromFile(const char* fileName);
    static EngineBase* CreateFromStream(IStream* stream);

  protected:
    Fb2Doc* doc = nullptr;
    TocTree* tocTree = nullptr;

    bool Load(const char* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

bool EngineFb2::Load(const char* fileName) {
    SetFilePath(fileName);
    doc = Fb2Doc::CreateFromFile(fileName);
    return FinishLoading();
}

bool EngineFb2::Load(IStream* stream) {
    doc = Fb2Doc::CreateFromStream(stream);
    return FinishLoading();
}

bool EngineFb2::FinishLoading() {
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetXmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), EbookTypographyKind::Latin);
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::GdiplusQuick;

    if (doc->IsZipped()) {
        str::ReplaceWithCopy(&defaultExt, ".fb2z");
    }

    if (IsCreateEngineForThumbnail()) {
        Fb2Formatter formatter(&args, doc);
        pages = FormatFirstHtmlPage(formatter);
        pageCount = (int)pages->size();
        return pageCount > 0;
    }

    pages = Fb2Formatter(&args, doc).FormatAllPages(false);
    // must set pageCount before ExtractPageAnchors
    pageCount = (int)pages->size();
    if (!ExtractPageAnchors()) {
        return false;
    }
    return pageCount > 0;
}

TocTree* EngineFb2::GetToc() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    TocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

EngineBase* EngineFb2::CreateFromFile(const char* fileName) {
    EngineFb2* engine = new EngineFb2();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* EngineFb2::CreateFromStream(IStream* stream) {
    EngineFb2* engine = new EngineFb2();
    if (!engine->Load(stream)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineFb2FromFile(const char* fileName) {
    return EngineFb2::CreateFromFile(fileName);
}

EngineBase* CreateEngineFb2FromStream(IStream* stream) {
    return EngineFb2::CreateFromStream(stream);
}

/* EngineBase for handling Mobi documents */

#include "MobiDoc.h"

bool EngineEbookIsProgressiveLoadingInProgress(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineMobi) {
        return false;
    }
    return static_cast<EngineEbook*>(engine)->IsEbookProgressiveLoadingInProgress();
}

bool EngineIsProgressiveEbookLoading(EngineBase* engine) {
    return engine && engine->IsProgressiveEbookLoading();
}

int EngineEbookGetFormattedPageCount(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineMobi) {
        return engine ? engine->PageCount() : 0;
    }
    return static_cast<EngineEbook*>(engine)->GetFormattedPageCount();
}

static int ParseMobiNumericFilePos(const char* name);

class EngineMobi : public EngineEbook {
  public:
    EngineMobi() : EngineEbook() {
        kind = kindEngineMobi;
        str::ReplaceWithCopy(&defaultExt, ".mobi");
    }
    ~EngineMobi() override {
        // ~EngineEbook runs after this body; wait for the load thread and
        // destroy the formatter before deleting doc (formatter reads doc HTML).
        AbortEbookLoading();
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        const char* fileName = FilePath();
        if (!fileName) {
            return nullptr;
        }
        return CreateFromFile(fileName);
    }

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    IPageDestination* GetNamedDest(const char* name) override;
    TocTree* GetToc() override;

    int ParseTocUrlFilePos(const char* url) {
        if (!doc || !url || !*url) {
            return -1;
        }
        if (str::StartsWith(url, "kindle:pos:")) {
            return doc->ResolveKindlePos(url);
        }
        return ParseMobiNumericFilePos(url);
    }

    ByteSlice GetDocHtmlData() const { return doc ? doc->GetHtmlData() : ByteSlice(); }

    static EngineBase* CreateFromFile(const char* fileName);
    static EngineBase* CreateFromStream(IStream* stream);
    static void FinishFormatAsync(EngineMobi* e);

  protected:
    MobiDoc* doc = nullptr;
    TocTree* tocTree = nullptr;
    bool tocTreeStale = false;
    bool tocTreeBuilt = false;

    IPageDestination* GetNamedDestAtFilePos(int filePos);

    bool Load(const char* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

bool EngineMobi::Load(const char* fileName) {
    SetFilePath(fileName);
    doc = MobiDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool EngineMobi::Load(IStream* stream) {
    doc = MobiDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool EngineMobi::FinishLoading() {
    if (!doc || PdbDocType::Mobipocket != doc->GetDocType()) {
        return false;
    }

    ByteSlice htmlData = doc->GetHtmlData();
    ConfigureMobiReaderStyle(FilePath(), htmlData);

    HtmlFormatterArgs args;
    args.htmlStr = htmlData;
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), typographyKind);
    args.fontSize = GetDefaultFontSize() * kMobiReaderFontScale;
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::GdiplusQuick;

    pages = new Vec<HtmlPage*>();
    pendingFormatter = new MobiFormatter(&args, doc, typographyKind, readerStyleMobi);

    if (IsCreateEngineForThumbnail()) {
        delete pages;
        pages = FormatInitialHtmlPages(pendingFormatter, kEbookInitialPages);
        delete pendingFormatter;
        pendingFormatter = nullptr;
        pageCount = (int)pages->size();
        return pageCount > 0;
    }

    for (int i = 0; i < kEbookInitialPages; i++) {
        HtmlPage* page = pendingFormatter->Next(false);
        if (!page) {
            delete pendingFormatter;
            pendingFormatter = nullptr;
            {
                ScopedCritSec scope(&pagesAccess);
                pageCount = (int)pages->size();
            }
            if (pageCount <= 0) {
                return false;
            }
            if (!ExtractPageAnchors()) {
                return false;
            }
            logf("EngineMobi::FinishLoading: generated %d initial pages (all content)\n", pageCount);
            return true;
        }
        {
            ScopedCritSec scope(&pagesAccess);
            pages->Append(page);
            pageCount = (int)pages->size();
        }
    }
    logf("EngineMobi::FinishLoading: generated %d initial pages, starting async formatting\n", pageCount);
    ReportIf(pageCount <= 0);

    ExtractPageAnchors();

    InterlockedExchange(&ebookLoadAbort, 0);
    InterlockedExchange(&ebookLoadingInProgress, 1);
    NotifyEngineDisplayReady(this);

    // Run remaining page formatting on a separate thread so the load thread
    // can return and the UI stays responsive. The formatter's GDI Graphics
    // are re-created on the async thread via RecreateGfxForCurrentThread().
    auto fn = MkFunc0<EngineMobi>(FinishFormatAsync, this);
    RunAsync(fn, "MobiFormatAsync");
    return true;
}

void EngineMobi::FinishFormatAsync(EngineMobi* e) {
    AtomicIntInc(&gDangerousThreadCount);
    defer {
        AtomicIntDec(&gDangerousThreadCount);
    };

    // let the UI thread render the first page(s) before the background formatter
    // starts contending for CPU, exactly like the EPUB (MuPDF) reflowable loader
    Sleep(kEbookBackgroundDelayMs);

    DWORD t0 = GetTickCount();
    // copy the path: e->FilePath() returns engine-owned memory that we only
    // touch while the engine is alive, but a private copy avoids any surprises
    // if loading is aborted while we notify the UI.
    AutoFreeStr path(str::Dup(e->FilePath()));
    HtmlFormatter* formatter = e->pendingFormatter;
    if (!formatter) {
        logf("FinishFormatAsync: pendingFormatter is null for '%s', aborting (engine destroyed during Sleep?)\n",
             path.Get() ? path.Get() : "(null)");
        InterlockedExchange(&e->ebookLoadingInProgress, 0);
        return;
    }
    logf("FinishFormatAsync: starting for '%s'\n", path.Get() ? path.Get() : "(null)");

    // Re-create GDI Graphics on this thread since the formatter was created
    // on a different thread (the load thread) and GDI+ objects have thread affinity.
    formatter->RecreateGfxForCurrentThread();

    int batchCount = 0;
    long long _dbgIter = 0;
    DWORD lastNotify = GetTickCount();
    bool exitedByAbort = false;
    bool exitedByNullPage = false;
    for (;;) {
        if (InterlockedCompareExchange(&e->ebookLoadAbort, 0, 0) != 0) {
            exitedByAbort = true;
            break;
        }
        HtmlPage* page = formatter->Next(false);
        if (!page) {
            exitedByNullPage = true;
            break;
        }
        {
            ScopedCritSec scope(&e->pagesAccess);
            e->pages->Append(page);
            e->pageCount = (int)e->pages->size();
        }
        if (++batchCount >= kEbookFormatBatchPages) {
            batchCount = 0;
            // incremental: only scans the pages added since the last call
            e->ExtractPageAnchors();
            e->InvalidateAllPageElements();
            // throttle UI relayouts by wall-clock time so the notification
            // count (and thus total relayout work) stays bounded regardless
            // of how many pages the book has
            DWORD now = GetTickCount();
            if (path && (now - lastNotify) >= kEbookNotifyIntervalMs) {
                lastNotify = now;
                NotifyEbookPagesLoadingProgress(path, false);
            }
            Sleep(0);
        }
    }

    delete formatter;
    e->pendingFormatter = nullptr;

    bool aborted = InterlockedCompareExchange(&e->ebookLoadAbort, 0, 0) != 0;
    logf("FinishFormatAsync: loop exited for '%s' - abort:%d nullPage:%d abortedFlag:%d pages:%d\n",
         path.Get() ? path.Get() : "(null)", (int)exitedByAbort, (int)exitedByNullPage, (int)aborted, e->pageCount);
    if (!aborted) {
        e->ExtractPageAnchors();
        // Mark stale but keep alive until UI calls ClearTocBox + reloads via GetToc.
        e->tocTreeStale = true;
    }

    if (path && !aborted) {
        NotifyEbookPagesLoadingProgress(path, true);
    }
    DWORD t1 = GetTickCount();
    logf("EngineMobi::FinishFormatAsync: formatted %d pages in %u ms\n", e->pageCount, t1 - t0);
    e->InvalidateAllPageElements();
    InterlockedExchange(&e->ebookLoadingInProgress, 0);
}

static int ParseMobiNumericFilePos(const char* name) {
    if (!name || !*name) {
        return -1;
    }
    if (str::StartsWith(name, "filepos:")) {
        name += 8;
    }
    if (*name < '0' || *name > '9') {
        return -1;
    }
    const char* p = name;
    while (*p >= '0' && *p <= '9') {
        p++;
    }
    if (*p != '\0' && *p != '#') {
        return -1;
    }
    return atoi(name);
}

int EngineEbookParseTocLinkFilePos(EngineBase* engine, IPageDestination* dest) {
    if (!engine || engine->kind != kindEngineMobi || !dest) {
        return -1;
    }
    if (dest->GetKind() != kindDestinationScrollTo) {
        return -1;
    }
    char* name = PageDestGetName(dest);
    if (!name || !*name) {
        return -1;
    }
    auto* mobi = static_cast<EngineMobi*>(engine);
    return mobi->ParseTocUrlFilePos(name);
}

bool EngineEbookIsTocFilePosReachable(EngineBase* engine, int filePos) {
    if (!engine || engine->kind != kindEngineMobi || filePos < 0) {
        return true;
    }
    auto* ee = static_cast<EngineEbook*>(engine);
    if (!ee->IsEbookProgressiveLoadingInProgress()) {
        return true;
    }
    auto* mobi = static_cast<EngineMobi*>(engine);
    ByteSlice html = mobi->GetDocHtmlData();
    return ee->IsTocFilePosFormatted(filePos, (const char*)html.data(), html.size());
}

// Map a MOBI/KF8 HTML byte offset to a 1-based formatted page number.
static int MobiPageNoForFilePos(const Vec<HtmlPage*>* pages, int filePos) {
    if (!pages || pages->size() == 0) {
        return 0;
    }
    int pageNo = 1;
    int nPages = (int)pages->size();
    for (int i = 0; i < nPages; i++) {
        if (pages->at(i)->reparseIdx <= filePos) {
            pageNo = i + 1;
        } else {
            break;
        }
    }
    return pageNo;
}

IPageDestination* EngineMobi::GetNamedDestAtFilePos(int filePos) {
    if (filePos < 0 || !doc) {
        return nullptr;
    }
    ByteSlice htmlData = doc->GetHtmlData();
    size_t htmlLen = htmlData.size();
    const char* start = (const char*)htmlData.data();
    if (!start || (size_t)filePos > htmlLen) {
        return nullptr;
    }

    ScopedCritSec scope(&pagesAccess);
    if (!pages || pages->size() == 0) {
        return nullptr;
    }
    int pageNo = MobiPageNoForFilePos(pages, filePos);
    if (pageNo <= 0) {
        return nullptr;
    }

    HtmlPage* page = pages->at(pageNo - 1);
    float currY = (float)pageRect.dy;
    for (DrawInstr& instr : page->instructions) {
        if ((DrawInstrType::String == instr.type || DrawInstrType::RtlString == instr.type) && instr.str.s >= start &&
            instr.str.s <= start + htmlLen && instr.str.s - start >= filePos) {
            currY = instr.bbox.y;
            break;
        }
    }
    RectF rect(0, currY + pageBorder, pageRect.dx, 10);
    rect.Inflate(-pageBorder, 0);
    return NewSimpleDest(pageNo, rect);
}

IPageDestination* EngineMobi::GetNamedDest(const char* name) {
    if (!name || !*name) {
        return nullptr;
    }
    int filePos = -1;
    if (str::StartsWith(name, "kindle:pos:")) {
        filePos = doc->ResolveKindlePos(name);
        if (filePos < 0) {
            return nullptr;
        }
        return GetNamedDestAtFilePos(filePos);
    }
    filePos = ParseMobiNumericFilePos(name);
    if (filePos >= 0) {
        return GetNamedDestAtFilePos(filePos);
    }
    return EngineEbook::GetNamedDest(name);
}

TocTree* EngineMobi::GetToc() {
    // ParseToc() gumbo-parses up to the whole (potentially huge) document, so it
    // must not run on every call. Cache the result of a build attempt - including
    // a null result while the book is still loading - and only rebuild when the
    // tree is explicitly marked stale (e.g. after async formatting completes).
    if (tocTreeBuilt && !tocTreeStale) {
        return tocTree;
    }
    delete tocTree;
    tocTree = nullptr;
    tocTreeStale = false;
    tocTreeBuilt = true;
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    TocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

EngineBase* EngineMobi::CreateFromFile(const char* fileName) {
    EngineMobi* engine = new EngineMobi();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* EngineMobi::CreateFromStream(IStream* stream) {
    EngineMobi* engine = new EngineMobi();
    if (!engine->Load(stream)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineMobiFromFile(const char* fileName) {
    return EngineMobi::CreateFromFile(fileName);
}

EngineBase* CreateEngineMobiFromStream(IStream* stream) {
    return EngineMobi::CreateFromStream(stream);
}

/* EngineBase for handling PalmDOC documents (and extensions such as TealDoc) */

class EnginePdb : public EngineEbook {
  public:
    EnginePdb() : EngineEbook() {
        kind = kindEnginePdb;
        str::ReplaceWithCopy(&defaultExt, ".pdb");
    }
    ~EnginePdb() override {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        const char* fileName = FilePath();
        if (!fileName) {
            return nullptr;
        }
        return CreateFromFile(fileName);
    }

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    TocTree* GetToc() override;

    static EngineBase* CreateFromFile(const char* fileName);

  protected:
    PalmDoc* doc = nullptr;
    TocTree* tocTree = nullptr;

    bool Load(const char* fileName);
};

bool EnginePdb::Load(const char* fileName) {
    SetFilePath(fileName);

    doc = PalmDoc::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), EbookTypographyKind::Latin);
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::GdiplusQuick;

    pages = HtmlFormatter(&args).FormatAllPages();
    // must set pageCount before ExtractPageAnchors
    pageCount = (int)pages->size();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pageCount > 0;
}

TocTree* EnginePdb::GetToc() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    auto* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

EngineBase* EnginePdb::CreateFromFile(const char* fileName) {
    EnginePdb* engine = new EnginePdb();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEnginePdbFromFile(const char* fileName) {
    return EnginePdb::CreateFromFile(fileName);
}

/* formatting extensions for CHM */

#include "ChmFile.h"

class ChmDataCache {
    ChmFile* doc = nullptr; // owned by creator
    ByteSlice html;
    Vec<ImageData> images;

  public:
    ChmDataCache(ChmFile* doc, char* html) : doc(doc), html(html) {}

    ~ChmDataCache() {
        for (auto&& img : images) {
            str::Free(img.base);
            str::Free(img.fileName);
        }
        html.Free();
    }

    ByteSlice GetHtmlData() { return html; }

    ByteSlice* GetImageData(const char* id, const char* pagePath) {
        AutoFreeStr url = NormalizeURL(id, pagePath);
        for (size_t i = 0; i < images.size(); i++) {
            if (str::Eq(images.at(i).fileName, url)) {
                return &images.at(i).base;
            }
        }

        auto tmp = doc->GetData(url);
        if (tmp.empty()) {
            return nullptr;
        }

        ImageData data;
        data.base = tmp;

        data.fileName = url.Release();
        images.Append(data);
        return &images.Last().base;
    }

    ByteSlice GetFileData(const char* relPath, const char* pagePath) {
        AutoFreeStr url = NormalizeURL(relPath, pagePath);
        return doc->GetData(url);
    }
};

class ChmFormatter : public HtmlFormatter {
  protected:
    void HandleTagImg(HtmlToken* t) override;
    void HandleTagPagebreak(HtmlToken* t) override;
    void HandleTagLink(HtmlToken* t) override;

    ChmDataCache* chmDoc = nullptr;
    AutoFreeStr pagePath;

  public:
    ChmFormatter(HtmlFormatterArgs* args, ChmDataCache* doc) : HtmlFormatter(args), chmDoc(doc) {}
};

void ChmFormatter::HandleTagImg(HtmlToken* t) {
    ReportIf(!chmDoc);
    if (t->IsEndTag()) {
        return;
    }
    bool needAlt = true;
    AttrInfo* attr = t->GetAttrByName("src");
    if (attr) {
        AutoFreeStr src = str::Dup(attr->val, attr->valLen);
        url::DecodeInPlace(src);
        ByteSlice* img = chmDoc->GetImageData(src, pagePath);
        needAlt = !img || !EmitImage(img);
    }
    if (needAlt && (attr = t->GetAttrByName("alt")) != nullptr) {
        HandleText(attr->val, attr->valLen);
    }
}

void ChmFormatter::HandleTagPagebreak(HtmlToken* t) {
    AttrInfo* attr = t->GetAttrByName("page_path");
    if (!attr || pagePath) {
        ForceNewPage();
    }
    if (attr) {
        Gdiplus::RectF bbox(0, currY, pageDx, 0);
        currPage->instructions.Append(DrawInstr::Anchor(attr->val, attr->valLen, bbox));
        pagePath.Set(str::Dup(attr->val, attr->valLen));
        // reset CSS style rules for the new document
        styleRules.Reset();
    }
}

void ChmFormatter::HandleTagLink(HtmlToken* t) {
    ReportIf(!chmDoc);
    if (t->IsEndTag()) {
        return;
    }
    AttrInfo* attr = t->GetAttrByName("rel");
    if (!attr || !attr->ValIs("stylesheet")) {
        return;
    }
    attr = t->GetAttrByName("type");
    if (attr && !attr->ValIs("text/css")) {
        return;
    }
    attr = t->GetAttrByName("href");
    if (!attr) {
        return;
    }

    char* src = str::DupTemp(attr->val, attr->valLen);
    url::DecodeInPlace(src);
    ByteSlice data = chmDoc->GetFileData(src, pagePath);
    if (data.Get()) {
        ParseStyleSheet(data, data.size());
    }
    data.Free();
}

/* EngineBase for handling CHM documents */

class EngineChm : public EngineEbook {
  public:
    EngineChm() : EngineEbook() {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectF(0, 0, 8.27f * GetFileDPI(), 11.693f * GetFileDPI());
        kind = kindEngineChm;
        str::ReplaceWithCopy(&defaultExt, ".chm");
    }
    ~EngineChm() override {
        delete dataCache;
        delete doc;
        delete tocTree;
    }
    EngineBase* Clone() override {
        const char* fileName = FilePath();
        if (!fileName) {
            return nullptr;
        }
        return CreateFromFile(fileName);
    }

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    IPageDestination* GetNamedDest(const char* name) override;
    TocTree* GetToc() override;

    static EngineBase* CreateFromFile(const char* fileName);

  protected:
    ChmFile* doc = nullptr;
    ChmDataCache* dataCache = nullptr;
    TocTree* tocTree = nullptr;

    bool Load(const char* fileName);

    IPageElement* CreatePageLink(DrawInstr* link, Rect rect, int pageNo) override;
};

static uint CharsetNameToCodepage(const char* charset) {
    static struct {
        const char* name;
        uint codepage;
    } codepages[] = {
        {"ISO-8859-1", 1252}, {"Latin1", 1252}, {"CP1252", 1252},       {"Windows-1252", 1252}, {"ISO-8859-2", 28592},
        {"Latin2", 28592},    {"CP1251", 1251}, {"Windows-1251", 1251}, {"KOI8-R", 20866},      {"shift-jis", 932},
        {"x-euc", 932},       {"euc-kr", 949},  {"Big5", 950},          {"GB2312", 936},        {"UTF-8", CP_UTF8},
    };
    for (int i = 0; i < dimofi(codepages); i++) {
        if (str::EqI(charset, codepages[i].name)) {
            return codepages[i].codepage;
        }
    }
    return 0;
}

static uint FindHttpCharsetInNode(const GumboNode* node) {
    if (!node) {
        return 0;
    }
    if (node->type == GUMBO_NODE_ELEMENT && GumboTagNameIs(node, "meta")) {
        const GumboAttribute* httpEquiv = gumbo_get_attribute(&node->v.element.attributes, "http-equiv");
        if (httpEquiv && str::EqI(httpEquiv->value, "Content-Type")) {
            const GumboAttribute* content = gumbo_get_attribute(&node->v.element.attributes, "content");
            AutoFree mimetype, charset;
            if (content && str::Parse(content->value, "%S;%_charset=%S", &mimetype, &charset)) {
                uint cp = CharsetNameToCodepage(charset);
                if (cp) {
                    return cp;
                }
            }
        }
    }
    const GumboVector* children = nullptr;
    if (node->type == GUMBO_NODE_ELEMENT) {
        children = &node->v.element.children;
    } else if (node->type == GUMBO_NODE_DOCUMENT) {
        children = &node->v.document.children;
    }
    if (children) {
        for (unsigned int i = 0; i < children->length; i++) {
            uint cp = FindHttpCharsetInNode((const GumboNode*)children->data[i]);
            if (cp) {
                return cp;
            }
        }
    }
    return 0;
}

// cf. http://www.w3.org/TR/html4/charset.html#h-5.2.2
static uint ExtractHttpCharset(const char* html, size_t htmlLen) {
    if (!strstr(html, "charset=")) {
        return 0;
    }
    size_t parseLen = std::min(htmlLen, (size_t)1024);
    GumboOptions opts = GumboMakeOptions();
    GumboOutput* output = gumbo_parse_with_options(&opts, html, parseLen);
    if (!output) {
        return 0;
    }
    uint cp = FindHttpCharsetInNode(output->document);
    gumbo_destroy_output(&opts, output);
    return cp;
}

class ChmHtmlCollector : public EbookTocVisitor {
    ChmFile* doc = nullptr;
    StrVec added;
    StrBuilder html;

  public:
    explicit ChmHtmlCollector(ChmFile* doc) : doc(doc) {
        // can be big
    }

    char* GetHtml() {
        // first add the homepage
        const char* index = doc->GetHomePath();
        TempWStr urlW = strconv::StrCPToWStrTemp(index, doc->codepage);
        char* url = ToUtf8Temp(urlW);
        Visit(nullptr, url, 0);

        // then add all pages linked to from the table of contents
        doc->ParseToc(this);

        // finally add all the remaining HTML files
        StrVec paths;
        doc->GetAllPaths(&paths);
        for (char* path : paths) {
            if (str::EndsWithI(path, ".htm") || str::EndsWithI(path, ".html")) {
                if (*path == '/') {
                    path++;
                }
                urlW = ToWStr(path);
                url = ToUtf8Temp(urlW);
                str::Free(urlW);
                Visit(nullptr, url, -1);
            }
        }
        return html.StealData();
    }

    void Visit(const char*, const char* url, int) override {
        if (!url || url::IsAbsolute(url)) {
            return;
        }
        char* plainUrl = url::GetFullPathTemp(url);
        if (added.FindI(plainUrl) != -1) {
            return;
        }
        InterlockedIncrement(&gAllowAllocFailure);
        defer {
            InterlockedDecrement(&gAllowAllocFailure);
        };
        ByteSlice pageHtml = doc->GetData(plainUrl);
        if (!pageHtml) {
            return;
        }
        html.AppendFmt("<pagebreak page_path=\"%s\" page_marker />", plainUrl);
        uint charset = ExtractHttpCharset((const char*)pageHtml.Get(), pageHtml.size());
        if (!charset) {
            charset = doc->codepage;
        }
        TempStr s = SmartToUtf8Temp((const char*)pageHtml.Get(), charset);
        html.Append(s);
        added.Append(plainUrl);
        pageHtml.Free();
    }
};

bool EngineChm::Load(const char* fileName) {
    SetFilePath(fileName);
    doc = ChmFile::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    char* html = ChmHtmlCollector(doc).GetHtml();
    dataCache = new ChmDataCache(doc, html);

    HtmlFormatterArgs args;
    args.htmlStr = dataCache->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), EbookTypographyKind::Latin);
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::GdiplusQuick;

    pages = ChmFormatter(&args, dataCache).FormatAllPages(false);
    // must set pageCount before ExtractPageAnchors
    pageCount = (int)pages->size();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pageCount > 0;
}

IPageDestination* EngineChm::GetNamedDest(const char* name) {
    IPageDestination* dest = EngineEbook::GetNamedDest(name);
    if (dest) {
        return dest;
    }
    unsigned int topicID;
    if (str::Parse(name, "%u%$", &topicID)) {
        char* url = doc->ResolveTopicID(topicID);
        if (url) {
            dest = EngineEbook::GetNamedDest(url);
            str::Free(url);
        }
    }
    return dest;
}

TocTree* EngineChm::GetToc() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    if (doc->HasIndex()) {
        // TODO: ToC code doesn't work too well for displaying an index,
        //       so this should really become a tree of its own (which
        //       doesn't rely on entries being in the same order as pages)
        builder.Visit("Index", nullptr, 1);
        builder.SetIsIndex(true);
        doc->ParseIndex(&builder);
    }
    TocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

static IPageDestination* newChmEmbeddedDest(const char* path) {
    auto res = new PageDestination();
    res->kind = kindDestinationLaunchEmbedded;
    res->value = str::Dup(path::GetBaseNameTemp(path));
    return res;
}

IPageElement* EngineChm::CreatePageLink(DrawInstr* link, Rect rect, int pageNo) {
    IPageElement* linkEl = EngineEbook::CreatePageLink(link, rect, pageNo);
    if (linkEl) {
        return linkEl;
    }

    DrawInstr* baseAnchor = baseAnchors.at(pageNo - 1);
    AutoFreeStr basePath = str::Dup(baseAnchor->str.s, baseAnchor->str.len);
    AutoFreeStr url = str::Dup(link->str.s, link->str.len);
    url.Set(NormalizeURL(url, basePath));
    if (!doc->HasData(url)) {
        return nullptr;
    }

    IPageDestination* dest = newChmEmbeddedDest(url);
    return NewEbookLink(link, rect, dest, pageNo);
}

EngineBase* EngineChm::CreateFromFile(const char* fileName) {
    EngineChm* engine = new EngineChm();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineChmFromFile(const char* fileName) {
    return EngineChm::CreateFromFile(fileName);
}

/* EngineBase for handling HTML documents */
/* (mainly to allow creating minimal regression test testcases more easily) */

class EngineHtml : public EngineEbook {
  public:
    EngineHtml() : EngineEbook() {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectF(0, 0, 8.27f * GetFileDPI(), 11.693f * GetFileDPI());
        str::ReplaceWithCopy(&defaultExt, ".html");
    }
    ~EngineHtml() override { delete doc; }
    EngineBase* Clone() override {
        const char* fileName = FilePath();
        if (!fileName) {
            return nullptr;
        }
        return CreateFromFile(fileName);
    }

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    static EngineBase* CreateFromFile(const char* fileName);

  protected:
    HtmlDoc* doc = nullptr;

    bool Load(const char* fileName);

    IPageElement* CreatePageLink(DrawInstr* link, Rect rect, int pageNo) override;
};

bool EngineHtml::Load(const char* fileName) {
    SetFilePath(fileName);

    doc = HtmlDoc::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), EbookTypographyKind::Latin);
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::Gdiplus;

    if (IsCreateEngineForThumbnail()) {
        HtmlFileFormatter formatter(&args, doc);
        pages = FormatFirstHtmlPage(formatter);
        pageCount = (int)pages->size();
        return pageCount > 0;
    }

    pages = HtmlFileFormatter(&args, doc).FormatAllPages(false);
    // must set pageCount before ExtractPageAnchors
    pageCount = (int)pages->size();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pageCount > 0;
}

static IPageDestination* newRemoteHtmlDest(const char* relativeURL) {
    auto* res = new PageDestination();
    const char* id = str::FindChar(relativeURL, '#');
    if (id) {
        res->value = str::Dup(relativeURL, id - relativeURL);
        res->name = str::Dup(id);
    } else {
        res->value = str::Dup(relativeURL);
    }
    res->kind = kindDestinationLaunchFile;
    return res;
}

IPageElement* EngineHtml::CreatePageLink(DrawInstr* link, Rect rect, int pageNo) {
    if (0 == link->str.len) {
        return nullptr;
    }

    char* url = strconv::FromHtmlUtf8Temp(link->str.s, link->str.len);
    if (url::IsAbsolute(url) || '#' == *url) {
        return EngineEbook::CreatePageLink(link, rect, pageNo);
    }

    IPageDestination* dest = newRemoteHtmlDest(url);
    return NewEbookLink(link, rect, dest, pageNo, true);
}

EngineBase* EngineHtml::CreateFromFile(const char* fileName) {
    EngineHtml* engine = new EngineHtml();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineHtmlFromFile(const char* fileName) {
    return EngineHtml::CreateFromFile(fileName);
}

/* EngineBase for handling TXT documents */

class EngineTxt : public EngineEbook {
  public:
    EngineTxt() : EngineEbook() {
        kind = kindEngineTxt;
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectF(0, 0, 8.27f * GetFileDPI(), 11.693f * GetFileDPI());
        str::ReplaceWithCopy(&defaultExt, ".txt");
    }
    ~EngineTxt() override {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        const char* fileName = FilePath();
        if (!fileName) {
            return nullptr;
        }
        return CreateFromFile(fileName);
    }

    TempStr GetPropertyTemp(const char* name) override {
        if (str::Eq(name, kPropFontList)) {
            return ExtractFontListTemp();
        }
        return doc->GetPropertyTemp(name);
    }

    TocTree* GetToc() override;

    static EngineBase* CreateFromFile(const char* fileName);

  protected:
    TxtDoc* doc = nullptr;
    TocTree* tocTree = nullptr;

    bool Load(const char* fileName);
};

bool EngineTxt::Load(const char* fileName) {
    if (!fileName) {
        return false;
    }

    SetFilePath(fileName);

    str::ReplaceWithCopy(&defaultExt, path::GetExtTemp(fileName));

    doc = TxtDoc::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    if (doc->IsRFC()) {
        // RFCs are targeted at letter size pages
        pageRect = RectF(0, 0, 8.5f * GetFileDPI(), 11.f * GetFileDPI());
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    SetupHtmlFormatterFont(args, FilePath(), EbookTypographyKind::Latin);
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = allocator;
    args.textRenderMethod = mui::TextRenderMethod::Gdiplus;

    if (IsCreateEngineForThumbnail()) {
        TxtFormatter formatter(&args);
        pages = FormatFirstHtmlPage(formatter);
        pageCount = (int)pages->size();
        return pageCount > 0;
    }

    pages = TxtFormatter(&args).FormatAllPages(false);
    // must set pageCount before ExtractPageAnchors
    pageCount = (int)pages->size();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pageCount > 0;
}

TocTree* EngineTxt::GetToc() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    auto* root = builder.GetRoot();

    auto realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

EngineBase* EngineTxt::CreateFromFile(const char* fileName) {
    EngineTxt* engine = new EngineTxt();
    if (!engine->Load(fileName)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineTxtFromFile(const char* fileName) {
    return EngineTxt::CreateFromFile(fileName);
}

void EngineEbookCleanup() {
    gDefaultFontName.Reset();
}
