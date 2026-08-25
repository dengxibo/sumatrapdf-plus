/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <mupdf/helpers/pkcs7-windows.h>
#include "../mupdf/source/fitz/color-imp.h"
void fz_purge_stored_html(fz_context* ctx, void* doc);
void fz_purge_stored_html_chapter(fz_context* ctx, void* doc, int chapter);
void fz_reset_epub_html_font_set(fz_context* ctx, fz_document* doc);
void fz_htdoc_reparse_html(fz_context* ctx, fz_document* doc, fz_buffer* buf, float w, float h, float em);
}

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/GuessFileType.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"
#include "utils/Timer.h"
#include "utils/ThreadUtil.h"

#include "wingui/UIModels.h"

#include "Annotation.h"
#include "DocProperties.h"
#include "DocController.h"
#include "EngineBase.h"
#include "MdConvert.h"
#include "EngineMupdf.h"
#include "EngineAll.h"
#include "PdfTocEditModel.h"
#include "EbookBase.h"
#include "EbookFontConfig.h"
#include "EbookTypography.h"
#include "EbookDoc.h"
#include "EpubMeta.h"
#include "EpubPerfLog.h"
#include "SumatraConfig.h"
#include "Settings.h"

#include "Theme.h"
#include "GlobalPrefs.h"
#include "PdfDarkMode.h"
#include "PdfDarkModeInternal.h"
#include "PdfDarkModeV2.h"
#include "PdfCadDetect.h"
#include "PdfCadEnhanceDevice.h"
#include "PdfJoinSplitImages.h"
#include "DisplayModel.h"
#include "GlobalPrefs.h"
#include "AppTools.h"

#include "ExtractPdfToc.h"

#include "utils/Log.h"

void NotifyEbookPagesLoadingProgress(const char* filePath, bool reloadToc);
void NotifyPdfFollowThemeProbeComplete(const char* filePath);

static const DWORD kReflowBackgroundDelayMs = 50;
// Brief window after UI page access: throttle (not stop) background chapter counting.
static const DWORD kReflowActiveReadingMs = 450;
static const DWORD kReflowBackgroundYieldMs = 12;
static const int kReflowChaptersPerYield = 1;
// Notify UI after this many spine chapters are counted. Anthology EPUBs: ~10-15
// pages/chapter → 64 chapters ≈ 650-950 pages per burst. Tune 32-80; UI stays
// smooth while reading thanks to background yield + estimated mediabox layout.
static const int kReflowNotifyEveryNChapters = 64;
static const int kReflowInitialPages = 8;

// called from FinishLoading on the load thread; posts UI work to show the
// document as soon as the engine has initial pages (before full page count).
void NotifyEngineDisplayReady(EngineBase* engine);

// default flowed-ebook page sizes. EPUB uses reader-style virtual pages;
// other reflowable formats keep the legacy fallback unless configured.
static float layoutLatinEpubDxPt = 540.F;
static float layoutLatinEpubDyPt = 760.F;
static float layoutCjkEpubDxPt = 600.F;
static float layoutCjkEpubDyPt = 820.F;
static float layoutA5DxPt = 560.F;
static float layoutA5DyPt = 680.F;

// A4
static float layoutA4DxPt = 595.F;
static float layoutA4DyPt = 842.F;

static float layoutFontEm = 11.F;

static TempStr BuildEbookDarkCss(bool isEpub) {
    // EPUB: colors and backgrounds only — no layout properties (pagination must not change on theme).
    COLORREF bgCol;
    ThemePageRenderColors(bgCol, true);
    TempStr bgHex = str::FormatTemp("#%02x%02x%02x", GetRValue(bgCol), GetGValue(bgCol), GetBValue(bgCol));
    COLORREF linkCol = ThemeWindowLinkColor();
    TempStr linkHex = str::FormatTemp("#%02x%02x%02x", GetRValue(linkCol), GetGValue(linkCol), GetBValue(linkCol));
    if (isEpub) {
        return str::FormatTemp(R"(html {
  color-scheme: dark;
  background-color: %s !important;
  color: #e6e1d8 !important;
}
body, p, span, blockquote, h1, h2, h3, h4, h5, h6, li, td, th, div,
section, article, main, header, footer, pre, table, td, th,
b, strong, em, i,
.calibre,
.calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,
.calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, .calibre_10,
.calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, .calibre_19, .calibre_20 {
  background-color: transparent !important;
  color: #e6e1d8 !important;
}
body {
  background-color: %s !important;
}
a, a:link, a:visited, a:hover, a:active,
.footnote-link, .noteref, .note-ref,
.calibre_2 a, .calibre_3 a, .calibre_2 a span, .calibre_3 a span,
.sgc-toc-level a, .sgc-toc-level a span,
p a, sup a, li a {
  color: %s !important;
  text-decoration: none !important;
}
figcaption, caption, p.caption, div.caption, span.caption,
.figcaption, .figure-caption, .image-caption, .picture-caption, .pic-caption, .caption {
  color: #b8b1a6 !important;
}
)",
                               bgHex, bgHex, linkHex);
    }
    return str::FormatTemp(R"(html {
  color-scheme: dark;
  background-color: %s !important;
  color: #e8eaed !important;
}
body, p, span, blockquote, h1, h2, h3, h4, h5, h6, li, td, th, div,
section, article, main, header, footer, pre,
b, strong, em, i,
.calibre,
.calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,
.calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, .calibre_10,
.calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, .calibre_19, .calibre_20 {
  background-color: transparent !important;
  color: #e8eaed !important;
}
body {
  background-color: %s !important;
}
a, a:link, a:visited, a:hover, a:active,
.footnote-link, .noteref, .note-ref,
.calibre_2 a, .calibre_3 a, .calibre_2 a span, .calibre_3 a span,
.sgc-toc-level a, .sgc-toc-level a span,
p a, sup a, li a {
  color: %s !important;
  text-decoration: none !important;
}
)",
                           bgHex, bgHex, linkHex);
}

// in mupdf_load_system_font.c
extern "C" void install_load_windows_font_funcs(fz_context* ctx);

static AnnotationType AnnotationTypeFromPdfAnnot(enum pdf_annot_type tp) {
    return (AnnotationType)tp;
}

Kind kindEngineMupdf = "enginePdf";

EngineMupdf* AsEngineMupdf(EngineBase* engine) {
    if (!engine || !IsOfKind(engine, kindEngineMupdf)) {
        return nullptr;
    }
    return (EngineMupdf*)engine;
}

void EngineMupdfToggleCadEnhance(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (epdf) {
        epdf->ToggleCadEnhanceOverride();
    }
}

void EngineMupdfInvalidateSearchTextCache(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return;
    }
    fz_context* ctx = epdf->Ctx();
    EnterCriticalSection(&epdf->pagesLock);
    for (FzPageInfo* pi : epdf->pages) {
        if (pi && pi->searchStext) {
            fz_drop_stext_page(ctx, pi->searchStext);
            pi->searchStext = nullptr;
        }
    }
    LeaveCriticalSection(&epdf->pagesLock);
}

static void DropFollowThemePageBitmapCache(FzPageInfo* pi) {
    if (!pi) {
        return;
    }
    delete pi->followThemePageBitmap;
    pi->followThemePageBitmap = nullptr;
    pi->followThemePageBitmapZoom = 0.f;
    pi->followThemePageBitmapRotation = 0;
    pi->followThemePageBitmapProfileHash = 0;
    pi->followThemePageBitmapDevBounds = fz_empty_irect;
}

static void SyncFollowThemeLayoutTextbookFastRemap(EngineMupdf* engine, fz_context* ctx) {
    if (!engine || !engine->darkModeEngineCache) {
        return;
    }
    // LayoutPhoto textbooks (PageMaker/Acrobat) share class 2 with adobe RAZ producers.
    // Only skip PictureBook when this is NOT Image-Conversion picture-book metadata.
    bool fast = engine->followThemeDocBitmapRecolor == 2 && engine->pdfdoc &&
                !PdfDarkModePdfMetadataSuggestsImageConversionPictureBook(ctx, engine->pdfdoc);
    PdfDarkModeEngineCacheSetLayoutTextbookFastRemap(engine->darkModeEngineCache, fast);
}

static void ApplyFollowThemeDocClassFromMetadata(EngineMupdf* engine, fz_context* ctx) {
    if (!engine || !engine->pdfdoc) {
        if (engine) {
            engine->followThemeDocBitmapRecolor = 0;
            SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        }
        return;
    }
    // Content probe refines metadata; do not overwrite on theme invalidation.
    if (engine->followThemeContentProbed) {
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    if (InterlockedCompareExchange(&engine->pdfFollowThemeProbePending, 0, 0) != 0) {
        return;
    }
    // Doc class comes from PDF metadata only; do not tie it to the current UI theme.
    // Full-page scan packs (DuXiu/Pdg2Pic) must win over Acrobat producer → LayoutPhoto,
    // otherwise every page takes the heavy FollowTheme wrap path.
    if (PdfDarkModePdfMetadataSuggestsFullPageScanDoc(ctx, engine->pdfdoc) ||
        PdfDarkModePdfMetadataSuggestsBitmapRecolorDoc(ctx, engine->pdfdoc) ||
        PdfDarkModePdfMetadataSuggestsPrintToPdfScanDoc(ctx, engine->pdfdoc)) {
        engine->followThemeDocBitmapRecolor = 1;
        engine->followThemeContentProbed = true;
    } else if (PdfDarkModePdfMetadataSuggestsPaperCaptureDoc(ctx, engine->pdfdoc) ||
               PdfDarkModePdfMetadataSuggestsLayoutPhotoDoc(ctx, engine->pdfdoc)) {
        engine->followThemeDocBitmapRecolor = 2;
        engine->followThemeContentProbed = true;
    } else {
        engine->followThemeDocBitmapRecolor = 0;
    }
    SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
}

static void DetectFollowThemeDocBitmapRecolor(EngineMupdf* engine, fz_context* ctx);
static bool PdfPageObjHasImageXObject(fz_context* ctx, pdf_document* pdfdoc, int pageNo0);

void EngineMupdfInvalidateDarkMode(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return;
    }
    EnterCriticalSection(&epdf->pagesLock);
    fz_context* ctx = epdf->Ctx();
    if (epdf->darkModeEngineCache) {
        PdfDarkModeEngineCacheClear(ctx, epdf->darkModeEngineCache);
    }
    for (FzPageInfo* pi : epdf->pages) {
        if (pi) {
            PdfDarkModeInvalidatePage(ctx, pi);
            DropFollowThemePageBitmapCache(pi);
            if (pi->displayList) {
                fz_drop_display_list(ctx, pi->displayList);
                pi->displayList = nullptr;
            }
        }
    }
    ApplyFollowThemeDocClassFromMetadata(epdf, ctx);
    LeaveCriticalSection(&epdf->pagesLock);
}

bool EngineSupportsSmartDarkMode(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineMupdf) {
        return false;
    }
    if (!str::EqI(engine->defaultExt, ".pdf")) {
        return false;
    }
    EngineMupdf* epdf = AsEngineMupdf(engine);
    return epdf && epdf->pdfdoc;
}

class FitzAbortCookie : public AbortCookie {
  public:
    fz_cookie cookie;
    FitzAbortCookie() { memset(&cookie, 0, sizeof(cookie)); }
    void Abort() override { cookie.abort = 1; }
    void* GetData() override { return (void*)&cookie; }
};

// copy of fz_is_external_link without ctx
static bool IsExternalLink(const char* uri) {
    if (!uri) {
        return false;
    }
    while (*uri >= 'a' && *uri <= 'z') {
        ++uri;
    }
    return uri[0] == ':';
}

// MuPDF / freed-outline dests sometimes leave uri as (char*)-1, which is
// truthy and crashes strchr / *s.
static const char* MupdfSafeCStr(const char* s) {
    if (!s || s == (const char*)(intptr_t)-1) {
        return nullptr;
    }
    return s;
}

static char* FzGetURL(fz_link* link, fz_outline* outline) {
    const char* uri = nullptr;
    if (link) {
        uri = link->uri;
    } else if (outline) {
        uri = outline->uri;
    }
    return (char*)MupdfSafeCStr(uri);
}

struct PageDestinationMupdf : IPageDestination {
    fz_outline* outline = nullptr;
    fz_link* link = nullptr;
    // EPUB outlines keep page.chapter=-1; resolved once from URI spine match
    int reflowOutlineChapter = -1;

    char* value = nullptr;
    char* name = nullptr;
    // Owned copy so hover/click still work after the engine outline is replaced.
    char* uriOwned = nullptr;
    float outlineX = 0;
    float outlineY = 0;

    PageDestinationMupdf(fz_link* l, fz_outline* o) {
        // exactly one must be provided
        kind = kindDestinationMupdf;
        link = l;
        outline = o;
    }

    RectF GetRect2() override {
        if (outline || uriOwned) {
            // needed for -named-dest called from LinkHandler::ScrollTo
            RectF r{outlineX, outlineY, 0, 0};
            return r;
        }
        return rect;
    }
    ~PageDestinationMupdf() override {
        str::Free(value);
        str::Free(name);
        str::Free(uriOwned);
    }

    char* GetValue2() override;
    char* GetName2() override;
};

char* PageDestinationMupdf ::GetValue2() {
    if (value) {
        return value;
    }

    char* uri = FzGetURL(link, outline);
    if (uri && IsExternalLink(uri)) {
        value = str::Dup(uri);
    }
    return value;
}

char* PageDestinationMupdf ::GetName2() {
    if (name) {
        return name;
    }
    if (outline && outline->title) {
        name = str::Dup(outline->title);
    }
    return name;
}

static NO_INLINE RectF FzGetRectF(fz_link* link, fz_outline* outline) {
    if (link) {
        return ToRectF(link->rect);
    }
    return {};
}

static int ResolveLink(fz_context* ctx, fz_document* doc, const char* uri, float* xp, float* yp) {
    if (!uri) {
        return -1;
    }
    int pageNo = -1;
    fz_location loc;

    fz_var(loc);
    fz_var(pageNo);
    fz_try(ctx) {
        loc = fz_resolve_link(ctx, doc, uri, xp, yp);
        pageNo = fz_page_number_from_location(ctx, doc, loc);
    }
    fz_catch(ctx) {
        fz_warn(ctx, "fz_resolve_link failed");
        fz_report_error(ctx);
        pageNo = -1;
    }
    if (pageNo < 0) {
        return -1;
    }
    return pageNo + 1;
}

static int FzGetPageNo(fz_context* ctx, fz_document* doc, fz_link* link, fz_outline* outline) {
    float x, y;
    const char* uri = FzGetURL(link, outline);
    int pageNo = ResolveLink(ctx, doc, uri, &x, &y);
    return pageNo;
}

// EPUB NCX/nav outlines have page.chapter=-1; match URI (without #fragment) to spine
// chapter index. epub_resolve_link does not layout HTML when there is no fragment.
static int EpubUriChapterIndexNoLayout(fz_context* ctx, fz_document* doc, const char* uri) {
    if (!uri || !*uri) {
        return -1;
    }
    TempStr base = str::DupTemp(uri);
    char* hash = str::FindChar(base, '#');
    if (hash) {
        *hash = '\0';
    }
    int chapter = -1;
    fz_var(chapter);
    fz_try(ctx) {
        fz_link_dest dest = fz_resolve_link_dest(ctx, doc, base);
        chapter = dest.loc.chapter;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        chapter = -1;
    }
    return chapter;
}

static int ReflowOutlineChapterIndex(PageDestinationMupdf* link) {
    if (!link) {
        return -1;
    }
    if (link->outline && link->outline->page.chapter >= 0) {
        return link->outline->page.chapter;
    }
    return link->reflowOutlineChapter;
}

static int ResolveMupdfLinkPageNo1(EngineMupdf* e, const char* uri, fz_link_dest* ldestOut);
static bool ResolveReflowLinkDuringProgressiveLoad(EngineMupdf* e, const char* uri, int reflowOutlineChapter,
                                                   float destX, float destY, int* pageNo1Out, fz_link_dest* ldestOut);
static void KickReflowNavCountAsync(EngineMupdf* e);

static bool EpubMetaPageMapIsCurrent(const EngineMupdf* e) {
    return e && e->epubMetaLoaded && e->epubMeta.profile.pages > 0 && e->epubMeta.profile.pages == e->pageCount;
}

static int ReflowPageNoFromChapter(EngineMupdf* e, int ch, int pageInChapter) {
    if (!e || ch < 0) {
        return 0;
    }
    if (pageInChapter < 0) {
        pageInChapter = 0;
    }
    int chaptersCounted = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
    if (ch >= chaptersCounted) {
        return 0;
    }
    ScopedCritSec scope(&e->pagesLock);
    if (ch < e->reflowChapterStartPage.Size()) {
        return e->reflowChapterStartPage[ch] + pageInChapter + 1;
    }
    return 0;
}

static int ReflowChapterIndexForPageNo(EngineMupdf* e, int pageNo1) {
    if (!e || pageNo1 < 1) {
        return -1;
    }
    int pageIdx = pageNo1 - 1;
    int chapter = -1;
    {
        ScopedCritSec scope(&e->pagesLock);
        int n = e->reflowChapterStartPage.Size();
        if (n > 0) {
            int lo = 0;
            int hi = n - 1;
            int ans = 0;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (e->reflowChapterStartPage[mid] <= pageIdx) {
                    ans = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            chapter = ans;
        }
    }
    if (chapter >= 0) {
        return chapter;
    }

    fz_context* ctx = e->_ctx;
    fz_location loc = fz_make_location(-1, 0);
    fz_try(ctx) {
        loc = fz_location_from_page_number(ctx, e->_doc, pageIdx);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return -1;
    }
    return loc.chapter;
}

bool EngineMupdfGetReflowPageChapter(EngineBase* engine, int pageNo, int* chapterOut, int* chapterStartPageOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !str::EqI(engine->defaultExt, ".epub") || pageNo < 1 || !chapterOut || !chapterStartPageOut) {
        return false;
    }

    int pageIdx = pageNo - 1;
    ScopedCritSec scope(&e->pagesLock);
    int n = e->reflowChapterStartPage.Size();
    if (n <= 0 || pageIdx < e->reflowChapterStartPage[0]) {
        return false;
    }
    int lo = 0;
    int hi = n - 1;
    int chapter = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (e->reflowChapterStartPage[mid] <= pageIdx) {
            chapter = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    *chapterOut = chapter;
    *chapterStartPageOut = e->reflowChapterStartPage[chapter] + 1;
    return true;
}

bool EngineMupdfGetReflowChapterPageRange(EngineBase* engine, int chapter, int* startPageOut, int* endPageOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !str::EqI(engine->defaultExt, ".epub") || chapter < 0 || !startPageOut || !endPageOut) {
        return false;
    }
    ScopedCritSec scope(&e->pagesLock);
    if (!e->reflowChapterStartPage.isValidIndex(chapter)) {
        return false;
    }
    *startPageOut = e->reflowChapterStartPage[chapter] + 1;
    *endPageOut =
        e->reflowChapterStartPage.isValidIndex(chapter + 1) ? e->reflowChapterStartPage[chapter + 1] : e->pageCount;
    return *endPageOut >= *startPageOut;
}

// for reflowable docs, use outline->page (chapter+index) with cached chapter
// page counts instead of fz_resolve_link, which lays out HTML per bookmark
static int FastReflowableOutlinePageNo(EngineMupdf* e, fz_context* ctx, fz_document* doc, fz_outline* outline) {
    if (!outline || outline->page.chapter < 0) {
        return -1;
    }
    int ch = outline->page.chapter;
    int pageInChapter = outline->page.page;
    if (pageInChapter < 0) {
        pageInChapter = 0;
    }
    if (e) {
        ScopedCritSec scope(&e->pagesLock);
        if (ch < e->reflowChapterStartPage.Size()) {
            return e->reflowChapterStartPage[ch] + pageInChapter + 1;
        }
    }
    int pageNo = -1;
    fz_var(pageNo);
    fz_try(ctx) {
        pageNo = fz_page_number_from_location(ctx, doc, outline->page);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        pageNo = -1;
    }
    if (pageNo < 0) {
        return -1;
    }
    return pageNo + 1;
}

static int OutlinePageNo(EngineMupdf* e, fz_context* ctx, fz_document* doc, pdf_document* pdfdoc, fz_link* link,
                         fz_outline* outline) {
    if (!pdfdoc && outline) {
        int pageNo = FastReflowableOutlinePageNo(e, ctx, doc, outline);
        if (pageNo > 0) {
            return pageNo;
        }
        return 0;
    }
    return FzGetPageNo(ctx, doc, link, outline);
}

static IPageDestination* NewPageDestinationMupdf(EngineMupdf* e, fz_context* ctx, fz_document* doc, fz_link* link,
                                                 fz_outline* outline, int pageNoHint = 0) {
    ReportIf(link && outline);
    ReportIf(!link && !outline);
    char* uri = FzGetURL(link, outline);

    const char* maybePath = (const char*)uri;

    if (str::Skip(maybePath, "file:")) {
        // decode: file:path%20to_file.pdf#page=1

        // this is to handle file:// and
        // file:/// (which I assume is a mistake in PDF)
        str::Skip(maybePath, "/");
        str::Skip(maybePath, "/");
        str::Skip(maybePath, "/");

        TempStr path = str::DupTemp(maybePath);
        TempStr dest = str::FindChar(path, '#');
        if (dest) {
            *dest = 0;
            dest++;
        }
        // mupdf url-encodes paths so we un-decode them
        fz_urldecode(path);
        fz_cleanname(path);

        // mupdf does unix path, we want windows
        path = str::ReplaceTemp(path, "/", "\\");
        if (dest) {
            fz_urldecode(dest);
        }

        logf("NewPageDestinationMupdf: path='%s', dest='%s'\n", path, dest);
        auto res = new PageDestinationFile(path, dest);
        res->rect = FzGetRectF(link, outline);
        return res;
    }

    if (IsExternalUrl(uri) || IsExternalLink(uri)) {
        auto res = new PageDestinationURL(uri);
        res->rect = FzGetRectF(link, outline);
        return res;
    }

    // Markdown/HTML: relative paths like other.md or ./pic.png (MuPDF has no base dir for stream docs).
    if (e && uri && uri[0] != '#' && !pdf_specifics(ctx, doc)) {
        TempStr pathPart = str::DupTemp(uri);
        TempStr fragment = str::FindChar(pathPart, '#');
        if (fragment) {
            *fragment++ = '\0';
        }
        if (pathPart[0] && !IsExternalLink(pathPart)) {
            TempStr fullPath = pathPart;
            if (!path::IsAbsolute(pathPart)) {
                TempStr dir = path::GetDirTemp(e->FilePath());
                if (dir) {
                    fullPath = path::JoinTemp(dir, pathPart);
                }
            }
            fullPath = path::NormalizeTemp(fullPath);
            if (file::Exists(fullPath)) {
                auto res = new PageDestinationFile(fullPath, fragment);
                res->rect = FzGetRectF(link, outline);
                return res;
            }
        }
    }

    auto dest = new PageDestinationMupdf(link, outline);
    dest->rect = FzGetRectF(link, outline);
    if (uri) {
        dest->uriOwned = str::Dup(uri);
    }
    if (outline) {
        dest->outlineX = outline->x;
        dest->outlineY = outline->y;
    }
    if (e && !pdf_specifics(ctx, doc)) {
        if (outline && outline->page.chapter >= 0) {
            dest->reflowOutlineChapter = outline->page.chapter;
        } else {
            // Match URI (without #fragment) to spine chapter for progressive TOC.
            // epub_resolve_link_dest does not layout HTML when there is no fragment.
            // Callers must hold docLock during progressive reflow (GetToc does).
            dest->reflowOutlineChapter = EpubUriChapterIndexNoLayout(ctx, doc, uri);
        }
    }
    if (pageNoHint > 0) {
        dest->pageNo = pageNoHint;
    } else if (e && InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0) {
        dest->pageNo = 0;
    } else {
        dest->pageNo = OutlinePageNo(e, ctx, doc, pdf_specifics(ctx, doc), link, outline);
    }
    if (e && dest->pageNo > 0 && e->joinDisplayToEngine.Size() > 0) {
        dest->pageNo = EngineMupdfMapEnginePageToDisplay(e, dest->pageNo);
    }
    return dest;
}

static PageElementDestination* NewLinkDestination(int srcPageNo, EngineMupdf* e, fz_context* ctx, fz_document* doc,
                                                  fz_link* link, fz_outline* outline) {
    auto dest = NewPageDestinationMupdf(e, ctx, doc, link, outline);
    auto res = new PageElementDestination(dest);
    res->pageNo = srcPageNo;
    res->rect = dest->rect;
    return res;
}

struct LinkRectList {
    StrVec links;
    Vec<fz_rect> coords;
};

fz_rect ToFzRect(RectF rect) {
    fz_rect result = {(float)rect.x, (float)rect.y, (float)(rect.x + rect.dx), (float)(rect.y + rect.dy)};
    return result;
}

RectF ToRectF(fz_rect rect) {
    return RectF::FromXY(rect.x0, rect.y0, rect.x1, rect.y1);
}

static bool IsPointInRect(fz_rect rect, fz_point pt) {
    return ToRectF(rect).Contains(PointF(pt.x, pt.y));
}

fz_matrix FzCreateViewCtm(fz_rect mediabox, float zoom, int rotation) {
    fz_matrix ctm = fz_pre_scale(fz_rotate((float)rotation), zoom, zoom);

    // TODO: this is happening quite often so don't report it
    // not sure if it indicates an actual issue
    // ReportIf(0 != mediabox.x0 || 0 != mediabox.y0);
    rotation = (rotation + 360) % 360;
    if (90 == rotation) {
        ctm = fz_pre_translate(ctm, 0, -mediabox.y1);
    } else if (180 == rotation) {
        ctm = fz_pre_translate(ctm, -mediabox.x1, -mediabox.y1);
    } else if (270 == rotation) {
        ctm = fz_pre_translate(ctm, -mediabox.x1, 0);
    }

    ReportIf(fz_matrix_expansion(ctm) <= 0);
    if (fz_matrix_expansion(ctm) == 0) {
        return fz_identity;
    }

    return ctm;
}

// TODO: maybe make dpi a float as well
static float DpiScale(float x, int dpi) {
    ReportIf(dpi < 70.F);
    // TODO: maybe implement step scaling like mupdf
    float res = x * (float)dpi;
    res = res / 96.F;
    return res;
}

static float FzRectOverlap(fz_rect r1, fz_rect r2) {
    if (fz_is_empty_rect(r1)) {
        return 0.0F;
    }
    fz_rect isect = fz_intersect_rect(r1, r2);
    return (isect.x1 - isect.x0) * (isect.y1 - isect.y0) / ((r1.x1 - r1.x0) * (r1.y1 - r1.y0));
}

static float FzRectOverlap(fz_rect r1, RectF r2f) {
    if (fz_is_empty_rect(r1)) {
        return 0.0F;
    }
    fz_rect r2 = ToFzRect(r2f);
    fz_rect isect = fz_intersect_rect(r1, r2);
    return (isect.x1 - isect.x0) * (isect.y1 - isect.y0) / ((r1.x1 - r1.x0) * (r1.y1 - r1.y0));
}

static TempWStr PdfToWStrTemp(fz_context* ctx, pdf_obj* obj) {
    char* s = pdf_new_utf8_from_pdf_string_obj(ctx, obj);
    WCHAR* res = ToWStrTemp(s);
    fz_free(ctx, s);
    return res;
}

static TempStr PdfToUtf8Temp(fz_context* ctx, pdf_obj* obj) {
    char* s = pdf_new_utf8_from_pdf_string_obj(ctx, obj);
    TempStr res = str::DupTemp(s);
    fz_free(ctx, s);
    return res;
}

// some PDF documents contain control characters in outline titles or /Info properties
// we replace them with spaces and cleanup for display with NormalizeWSInPlace()
static WCHAR* PdfCleanStringInPlace(WCHAR* s) {
    if (!s) {
        return nullptr;
    }
    WCHAR* curr = s;
    while (*curr) {
        WCHAR c = *curr;
        if (c < 0x20) {
            *curr = ' ';
        } else if (c == 0xfffd) {
            // https://github.com/sumatrapdfreader/sumatrapdf/issues/4965
            // TODO: was there mupdf change that caused this?
            *curr = 0;
            break;
        }
        curr++;
    }
    str::NormalizeWSInPlace(s);
    return s;
}

struct istream_filter {
    IStream* stream;
    u8 buf[4096];
};

extern "C" int next_istream(fz_context* ctx, fz_stream* stm, size_t) {
    istream_filter* state = (istream_filter*)stm->state;
    ULONG cbRead = sizeof(state->buf);
    HRESULT res = state->stream->Read(state->buf, sizeof(state->buf), &cbRead);
    if (FAILED(res)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream read error: %x", res);
    }
    stm->rp = state->buf;
    stm->wp = stm->rp + cbRead;
    stm->pos += cbRead;

    return cbRead > 0 ? *stm->rp++ : EOF;
}

extern "C" void seek_istream(fz_context* ctx, fz_stream* stm, i64 offset, int whence) {
    istream_filter* state = (istream_filter*)stm->state;
    LARGE_INTEGER off;
    ULARGE_INTEGER n;
    off.QuadPart = offset;
    HRESULT res = state->stream->Seek(off, whence, &n);
    if (FAILED(res)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream seek error: %x", res);
    }
    if (n.HighPart != 0 || n.LowPart > INT_MAX) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "documents beyond 2GB aren't supported");
    }
    stm->pos = n.LowPart;
    stm->rp = stm->wp = state->buf;
}

extern "C" void drop_istream(fz_context* ctx, void* state_) {
    istream_filter* state = (istream_filter*)state_;
    state->stream->Release();
    fz_free(ctx, state);
}

static fz_stream* FzOpenIStream(fz_context* ctx, IStream* stream) {
    if (!stream) {
        return nullptr;
    }

    LARGE_INTEGER zero{};
    HRESULT res = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(res)) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream seek error: %x", res);
    }

    istream_filter* state = fz_malloc_struct(ctx, istream_filter);
    state->stream = stream;
    stream->AddRef();

    fz_stream* stm = fz_new_stream(ctx, state, next_istream, drop_istream);
    stm->seek = seek_istream;
    return stm;
}

static void* FzMemdup(fz_context* ctx, void* p, size_t size) {
    void* res = fz_malloc_no_throw(ctx, size);
    if (!res) {
        return nullptr;
    }
    memcpy(res, p, size);
    return res;
}

static fz_stream* FzStreamFromData(fz_context* ctx, const u8* data, int size) {
    fz_stream* stm = nullptr;
    // TODO: we copy so that the memory ends up in chunk allocated
    // by libmupdf so that it works across dll boundaries.
    // We can either use  fz_new_buffer_from_shared_data
    // and free the data on the side or create Allocator that
    // uses fz_malloc_no_throw and pass it to ReadFileWithAllocator
    void* dataCopy = FzMemdup(ctx, (void*)data, size);
    if (!dataCopy) {
        return nullptr;
    }

    fz_buffer* buf = fz_new_buffer_from_data(ctx, (u8*)dataCopy, size);
    fz_var(buf);
    fz_try(ctx) {
        stm = fz_open_buffer(ctx, buf);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        stm = nullptr;
        fz_report_error(ctx);
    }
    return stm;
}

// maximum size of a file that's entirely loaded into memory before parsed
// and displayed; larger files will be kept open while they're displayed
// so that their content can be loaded on demand in order to preserve memory
constexpr i64 kMaxMemoryFileSize = 32 * 1024 * 1024;

static fz_stream* FzReadFileIfSmall(fz_context* ctx, const char* path) {
    fz_stream* stm = nullptr;
    i64 fileSize = file::GetSize(path);
    // load small files entirely into memory so that they can be
    // overwritten even by programs that don't open files with FILE_SHARE_READ
    bool isSmallFile = fileSize > 0 && fileSize < kMaxMemoryFileSize;
    if (!isSmallFile) {
        return nullptr;
    }

    ByteSlice d = file::ReadFile(path);
    if (d.empty()) {
        // failed to read
        return nullptr;
    }

    stm = FzStreamFromData(ctx, d.data(), d.Size());
    d.Free();
    return stm;
}

/*
https://github.com/sumatrapdfreader/sumatrapdf/issues/4514
Some PDF files have garbage at the beginning, before the %PDF- marker
Sometimes removing this garbage fixes the file for mupdf
*/
static fz_stream* FzReadMaybeFixPDF(fz_context* ctx, const char* path) {
    fz_stream* stm;
    // fast fail: read enough to check if this is PDF file with garbage
    char buf[1024];
    size_t bufSize = dimof(buf);
    int n = file::ReadN(path, buf, bufSize);
    if (n < 1024) {
        return nullptr;
    }
    n = str::BufFind(buf, (int)bufSize, "%PDF-");
    if (n <= 0) {
        // not PDF or no garbage at the beginning
        return nullptr;
    }

    ByteSlice d = file::ReadFile(path);
    if (d.empty()) {
        // failed to read
        return nullptr;
    }

    // strip garbage
    const u8* data = d.data() + n;
    int size = d.Size() - n;
    stm = FzStreamFromData(ctx, data, size);
    d.Free();
    return stm;
}

static fz_stream* FzOpenOrReadFile(fz_context* ctx, const char* path) {
    fz_stream* stm = FzReadFileIfSmall(ctx, path);
    if (stm) {
        return stm;
    }
    WCHAR* pathW = ToWStrTemp(path);
    fz_try(ctx) {
        stm = fz_open_file_w(ctx, pathW);
    }
    fz_catch(ctx) {
        stm = nullptr;
        fz_report_error(ctx);
    }
    return stm;
}

static void FzStreamFingerprint(fz_context* ctx, fz_stream* stm, u8 digest[16]) {
    i64 fileLen = -1;
    fz_buffer* buf = nullptr;

    fz_try(ctx) {
        fz_seek(ctx, stm, 0, 2);
        fileLen = fz_tell(ctx, stm);
        fz_seek(ctx, stm, 0, 0);
        buf = fz_read_all(ctx, stm, fileLen);
    }
    fz_catch(ctx) {
        fz_warn(ctx, "couldn't read stream data, using a nullptr fingerprint instead");
        ZeroMemory(digest, 16);
        fz_report_error(ctx);
        return;
    }
    ReportIf(nullptr == buf);
    u8* data;
    size_t size = fz_buffer_extract(ctx, buf, &data);
    ReportIf((size_t)fileLen != size);
    fz_drop_buffer(ctx, buf);

    fz_md5 md5;
    fz_md5_init(&md5);
    fz_md5_update(&md5, data, size);
    fz_md5_final(&md5, digest);
}

static ByteSlice FzExtractStreamData(fz_context* ctx, fz_stream* stream) {
    fz_seek(ctx, stream, 0, 2);
    i64 fileLen = fz_tell(ctx, stream);
    fz_seek(ctx, stream, 0, 0);

    fz_buffer* buf = fz_read_all(ctx, stream, fileLen);

    u8* data = nullptr;
    size_t size = fz_buffer_extract(ctx, buf, &data);
    ReportIf((size_t)fileLen != size);
    fz_drop_buffer(ctx, buf);
    if (!data || size == 0) {
        return {};
    }
    // this was allocated inside mupdf, make a copy that can be free()d
    u8* res = (u8*)memdup(data, size);
    fz_free(ctx, data);
    return {res, size};
}

static inline int WcharsPerRune(int rune) {
    return rune > 0xFFFF ? 2 : 1;
}

static Rect SelectionRectFromChar(fz_stext_line* line, fz_stext_char* c) {
    fz_rect bbox = fz_rect_from_quad(c->quad);
    Rect r = ToRectF(bbox).Round();
    // Vertical reflow text needs the accurate glyph bounds. Replacing those
    // bounds with a horizontal font-size band shifts glyph picking by one cell.
    if (line && line->wmode != 0) {
        return r;
    }

    // Some CJK fallback fonts report outline bounds whose ascent/descent fill
    // most of the CSS line box. Using that height makes horizontal selection and
    // read-aloud highlights much taller than the visible glyphs. For horizontal
    // text, retain the established font-size band while keeping its real center.
    float bandH = c->size;
    if (bandH <= 0.01f || r.IsEmpty()) {
        return r;
    }
    float centerY = (float)r.y + (float)r.dy * 0.5f;
    int y = (int)(centerY - bandH * 0.5f + 0.5f);
    int h = std::max((int)(bandH + 0.5f), 1);
    r.y = y;
    r.dy = h;
    return r;
}

static bool RectsFormVerticalStep(const Rect& a, const Rect& b) {
    if (a.IsEmpty() || b.IsEmpty()) {
        return false;
    }
    int overlap = std::min(a.x + a.dx, b.x + b.dx) - std::max(a.x, b.x);
    int minW = std::min(a.dx, b.dx);
    int step = b.y - a.y;
    int maxH = std::max(a.dy, b.dy);
    return minW > 0 && overlap * 2 >= minW && step > 0 && step <= maxH * 3;
}

static Rect VerticalGlyphCell(const Rect& glyphRect, int centerOffset, int step) {
    Rect cell = glyphRect;
    int center = glyphRect.y + glyphRect.dy / 2 + centerOffset;
    cell.y = center - step / 2;
    cell.dy = step;
    return cell;
}

// Some reflowed vertical documents are emitted as one horizontal stext line per
// glyph. MuPDF associates each line's Unicode with the following painted glyph,
// so the text is correct but every quad is one visual cell late. Detect that
// shape from the empty line separators and re-associate rects at the common
// source used by selection, search, and lookup.
static void AlignVerticalGlyphRects(Vec<Rect>& rects) {
    int verticalPairs = 0;
    int separatedPairs = 0;
    int previousStart = -1;
    Rect previous{};
    bool separated = false;
    for (int i = 0; i < rects.Size();) {
        Rect current = rects[i];
        if (current.IsEmpty()) {
            separated = previousStart >= 0;
            i++;
            continue;
        }
        int end = i + 1;
        while (end < rects.Size() && rects[end] == current) {
            end++;
        }
        if (previousStart >= 0 && separated) {
            separatedPairs++;
            if (RectsFormVerticalStep(previous, current)) {
                verticalPairs++;
            }
        }
        previousStart = i;
        previous = current;
        separated = false;
        i = end;
    }
    if (verticalPairs < 6 || verticalPairs * 3 < separatedPairs * 2) {
        return;
    }

    previousStart = -1;
    int previousEnd = -1;
    Rect previousOriginal{};
    bool runStarted = false;
    separated = false;
    for (int i = 0; i < rects.Size();) {
        Rect original = rects[i];
        if (original.IsEmpty()) {
            separated = previousStart >= 0;
            i++;
            continue;
        }
        int end = i + 1;
        while (end < rects.Size() && rects[end] == original) {
            end++;
        }
        if (previousStart >= 0 && separated && RectsFormVerticalStep(previousOriginal, original)) {
            int step = original.y - previousOriginal.y;
            if (!runStarted) {
                Rect first = VerticalGlyphCell(previousOriginal, -step, step);
                for (int j = previousStart; j < previousEnd; j++) {
                    rects[j] = first;
                }
                runStarted = true;
            }
            Rect current = VerticalGlyphCell(previousOriginal, 0, step);
            for (int j = i; j < end; j++) {
                rects[j] = current;
            }
        } else {
            runStarted = false;
        }
        previousStart = i;
        previousEnd = end;
        previousOriginal = original;
        separated = false;
        i = end;
    }
}

static bool RuneIsWhitespace(int rune) {
    return rune >= 0 && rune <= 0xFFFF && str::IsWs((WCHAR)rune);
}

static bool RuneIsNonPrintable(int rune) {
    return rune <= 32 || (rune >= 0 && rune <= 0xFFFF && str::IsNonCharacter((WCHAR)rune));
}

static void AddChar(fz_stext_line* line, fz_stext_char* c, WStrBuilder& s, Vec<Rect>& rects) {
    Rect r = SelectionRectFromChar(line, c);

    int n = WcharsPerRune(c->c);
    if (n == 2) {
        WCHAR tmp[2];
        tmp[0] = 0xD800 | ((c->c - 0x10000) >> 10) & 0x3FF;
        tmp[1] = 0xDC00 | (c->c - 0x10000) & 0x3FF;
        s.Append(tmp, 2);
        rects.Append(r);
        rects.Append(r);
        return;
    }
    WCHAR wc = c->c;
    bool isNonPrintable = RuneIsNonPrintable(c->c);
    if (!isNonPrintable) {
        s.AppendChar(wc);
        rects.Append(r);
        return;
    }

    // non-printable or whitespace
    if (!RuneIsWhitespace(c->c)) {
        s.AppendChar(L'?');
        rects.Append(r);
        return;
    }

    // collapse multiple whitespace characters into one
    WCHAR prev = s.LastChar();
    if (!str::IsWs(prev)) {
        s.AppendChar(L' ');
        // MuPDF quads for whitespace can span paragraph indents; shrink the hit
        // target so clicking blank margin doesn't select following text.
        constexpr int kMaxSpaceHitW = 4;
        if (r.dx > kMaxSpaceHitW) {
            r.x = r.BR().x - kMaxSpaceHitW;
            r.dx = kMaxSpaceHitW;
        }
        rects.Append(r);
    }
}

static void AddLineSep(WStrBuilder& s, Vec<Rect>& rects, const WCHAR* lineSep, size_t lineSepLen) {
    if (lineSepLen == 0) {
        return;
    }
    // remove trailing spaces
    if (str::IsWs(s.LastChar())) {
        s.RemoveLast();
        rects.RemoveLast();
    }

    s.Append(lineSep);
    for (size_t i = 0; i < lineSepLen; i++) {
        rects.Append(Rect());
    }
}

static void RemoveLastUtf8Codepoint(StrBuilder& s, Vec<Rect>& rects) {
    if (s.IsEmpty()) {
        return;
    }
    int byteLen = (int)s.size();
    int byteIdx = byteLen;
    Utf8CodepointPrev(s.CStr(), byteLen, byteIdx);
    int n = byteLen - byteIdx;
    for (int i = 0; i < n; i++) {
        s.RemoveLast();
        rects.RemoveLast();
    }
}

static bool Utf8StrEndsWithWhitespace(const StrBuilder& s) {
    if (s.IsEmpty()) {
        return false;
    }
    int byteLen = (int)s.size();
    int byteIdx = byteLen;
    int cp = Utf8CodepointPrev(s.CStr(), byteLen, byteIdx);
    return str::IsWs((WCHAR)cp);
}

// UTF-8 variant: append `c` as up to 4 UTF-8 bytes to `s` and the same
// rect `r` for each byte, so rects.size() == s.size() holds.
static void AddCharUtf8(fz_stext_line* line, fz_stext_char* c, StrBuilder& s, Vec<Rect>& rects) {
    Rect r = SelectionRectFromChar(line, c);

    int rune = c->c;
    bool isWhitespace = RuneIsWhitespace(rune);
    bool isNonPrintable = RuneIsNonPrintable(rune);
    if (isNonPrintable && !isWhitespace) {
        s.AppendChar('?');
        rects.Append(r);
        return;
    }
    if (isWhitespace) {
        // collapse multiple whitespace characters into one (mirror AddChar WCHAR path)
        if (Utf8StrEndsWithWhitespace(s)) {
            return;
        }
        s.AppendChar(' ');
        rects.Append(r);
        return;
    }
    char buf[4];
    int n = fz_runetochar(buf, rune);
    s.Append(buf, (size_t)n);
    for (int i = 0; i < n; i++) {
        rects.Append(r);
    }
}

static void AddLineSepUtf8(StrBuilder& s, Vec<Rect>& rects, const char* lineSep) {
    size_t lineSepLen = str::Len(lineSep);
    if (lineSepLen == 0) {
        return;
    }
    // remove trailing whitespace (mirror AddLineSep WCHAR path)
    while (!s.IsEmpty()) {
        int byteLen = (int)s.size();
        int byteIdx = byteLen;
        int cp = Utf8CodepointPrev(s.CStr(), byteLen, byteIdx);
        if (!str::IsWs((WCHAR)cp)) {
            break;
        }
        RemoveLastUtf8Codepoint(s, rects);
    }
    s.Append(lineSep);
    for (size_t i = 0; i < lineSepLen; i++) {
        rects.Append(Rect());
    }
}

static float StextLineEm(fz_stext_line* line) {
    if (!line) {
        return 12.f;
    }
    float h = line->bbox.y1 - line->bbox.y0;
    if (h < 1.f && line->first_char) {
        h = line->first_char->size;
    }
    return h < 1.f ? 12.f : h;
}

static int StextLineFirstRune(fz_stext_line* line) {
    for (fz_stext_char* c = line ? line->first_char : nullptr; c; c = c->next) {
        if (c->c > 32) {
            return c->c;
        }
    }
    return 0;
}

static int StextLineLastRune(fz_stext_line* line) {
    int last = 0;
    for (fz_stext_char* c = line ? line->first_char : nullptr; c; c = c->next) {
        if (c->c > 32) {
            last = c->c;
        }
    }
    return last;
}

static bool StextStartsLikeHeading(fz_stext_line* line) {
    int n = 0;
    int buf[6]{};
    for (fz_stext_char* c = line ? line->first_char : nullptr; c && n < 6; c = c->next) {
        if (c->c <= 32) {
            continue;
        }
        buf[n++] = c->c;
    }
    if (n == 0) {
        return false;
    }
    int cp = buf[0];
    if (cp == 0x7B2C) {
        return true;
    }
    if (cp == 0x9644 && n > 1 && buf[1] == 0x4EF6) {
        return true;
    }
    if ((cp == 0xFF08 || cp == '(') && n > 2 && (buf[2] == 0xFF09 || buf[2] == ')')) {
        return true;
    }
    if (cp >= '1' && cp <= '9') {
        for (int i = 1; i < n; i++) {
            if (buf[i] == '.' || buf[i] == 0x3001 || buf[i] == 0xFF0E) {
                return true;
            }
            if (buf[i] < '0' || buf[i] > '9') {
                break;
            }
        }
    }
    if (cp == 0x4E00 || cp == 0x4E8C || cp == 0x4E09 || cp == 0x56DB || cp == 0x4E94 || cp == 0x516D || cp == 0x4E03 ||
        cp == 0x516B || cp == 0x4E5D || cp == 0x5341) {
        return n > 1 && buf[1] == 0x3001;
    }
    return false;
}

static float StextLineGap(fz_stext_line* a, fz_stext_line* b) {
    if (!a || !b) {
        return 0;
    }
    if (b->bbox.y0 >= a->bbox.y1) {
        return b->bbox.y0 - a->bbox.y1;
    }
    if (a->bbox.y0 >= b->bbox.y1) {
        return a->bbox.y0 - b->bbox.y1;
    }
    return 0;
}

static void CollectStextTextLines(fz_stext_page* text, Vec<fz_stext_line*>& lines) {
    if (!text) {
        return;
    }
    for (fz_stext_block* block = text->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) {
            continue;
        }
        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            lines.Append(line);
        }
    }
}

static float StextMedianGap(Vec<fz_stext_line*>& lines) {
    int n = (int)lines.size();
    if (n < 2) {
        return 0;
    }
    float* gaps = AllocArray<float>(n - 1);
    int ng = 0;
    for (int i = 1; i < n; i++) {
        float g = StextLineGap(lines[i - 1], lines[i]);
        if (g >= 0) {
            gaps[ng++] = g;
        }
    }
    float med = 0;
    if (ng > 0) {
        for (int i = 0; i < ng; i++) {
            for (int j = i + 1; j < ng; j++) {
                if (gaps[j] < gaps[i]) {
                    float t = gaps[i];
                    gaps[i] = gaps[j];
                    gaps[j] = t;
                }
            }
        }
        med = gaps[ng / 2];
    }
    free(gaps);
    return med;
}

static bool ShouldJoinStextLines(fz_stext_line* a, fz_stext_line* b, float gapMed) {
    const char* reason = "ok";
    bool join = true;
    float em = 0;
    float gap = 0;
    float aW = 0;
    float bW = 0;
    bool aFull = false;
    if (!a || !b) {
        join = false;
        reason = "null";
    } else if (StextStartsLikeHeading(b)) {
        join = false;
        reason = "heading";
    } else {
        em = StextLineEm(a);
        float aL = a->bbox.x0;
        float aR = a->bbox.x1;
        aW = aR - aL;
        float bL = b->bbox.x0;
        float bR = b->bbox.x1;
        bW = bR - bL;
        float bodyL = aL < bL ? aL : bL;
        float bodyR = aR > bR ? aR : bR;
        float measure = bodyR - bodyL;
        if (measure < em * 4) {
            measure = em * 8;
        }
        aFull = aR >= bodyR - em || aW >= bW * 0.82f;
        gap = StextLineGap(a, b);
        if (!aFull) {
            join = false;
            reason = "short";
        } else if (bL - aL >= em * 1.5f) {
            join = false;
            reason = "indent";
        } else if ((bL - bodyL) > em * 2 && (bodyR - bR) > em * 2 && bW < measure * 2 / 3) {
            join = false;
            reason = "center";
        } else if (gapMed > 0 && gap > gapMed * 2 + em / 2) {
            join = false;
            reason = "gap";
        }
    }
    return join;
}

static bool StextRuneIsAsciiWord(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static const char* StextJoinSepUtf8(fz_stext_line* a, fz_stext_line* b, float gapMed) {
    if (!ShouldJoinStextLines(a, b, gapMed)) {
        return "\n";
    }
    int last = StextLineLastRune(a);
    int first = StextLineFirstRune(b);
    if (StextRuneIsAsciiWord(last) && StextRuneIsAsciiWord(first)) {
        return " ";
    }
    return "";
}

static char* FzTextPageToUtf8(fz_stext_page* text, Rect** coordsOut) {
    StrBuilder content;
    Vec<Rect> rects;
    Vec<fz_stext_line*> lines;
    CollectStextTextLines(text, lines);
    float gapMed = StextMedianGap(lines);
    int nJoin = 0;
    int nBreak = 0;

    for (size_t i = 0; i < lines.size(); i++) {
        fz_stext_line* line = lines[i];
        for (fz_stext_char* c = line->first_char; c; c = c->next) {
            AddCharUtf8(line, c, content, rects);
        }
        if (i + 1 >= lines.size()) {
            continue;
        }
        const char* sep = StextJoinSepUtf8(line, lines[i + 1], gapMed);
        if (sep[0] == '\n') {
            nBreak++;
        } else {
            nJoin++;
        }
        AddLineSepUtf8(content, rects, sep);
    }

    ReportIf(content.size() != rects.size());
    AlignVerticalGlyphRects(rects);

    if (coordsOut) {
        *coordsOut = rects.StealData();
    }
    return content.StealData();
}

static WCHAR* FzTextPageToStr(fz_stext_page* text, Rect** coordsOut) {
    WStrBuilder content;
    // coordsOut is optional but we ask for it by default so we simplify the code
    // by always calculating it
    Vec<Rect> rects;
    Vec<fz_stext_line*> lines;
    CollectStextTextLines(text, lines);
    float gapMed = StextMedianGap(lines);
    int nJoin = 0;
    int nBreak = 0;

    for (size_t i = 0; i < lines.size(); i++) {
        fz_stext_line* line = lines[i];
        for (fz_stext_char* c = line->first_char; c; c = c->next) {
            AddChar(line, c, content, rects);
        }
        if (i + 1 >= lines.size()) {
            continue;
        }
        const char* sep = StextJoinSepUtf8(line, lines[i + 1], gapMed);
        if (sep[0] == '\n') {
            nBreak++;
            AddLineSep(content, rects, L"\n", 1);
        } else if (sep[0] == ' ') {
            nJoin++;
            AddLineSep(content, rects, L" ", 1);
        } else {
            nJoin++;
        }
    }

    ReportIf(content.size() != rects.size());
    AlignVerticalGlyphRects(rects);

    if (coordsOut) {
        *coordsOut = rects.StealData();
    }

    return content.StealData();
}

static fz_stext_options NewTextPageOptions(int flags = 0) {
    fz_stext_options opts{};
    // Use glyph outline bounds so text selection rectangles match visible text
    // instead of the looser line-height boxes from default MuPDF extraction.
    opts.flags = flags | FZ_STEXT_ACCURATE_BBOXES;
    return opts;
}

// Match HtmlFormatter::IsEmptyPage: spine items that layout to invisible-only pages
// should not consume a slot in the progressive reflow page map.
static bool ReflowChapterPageHasVisibleContent(fz_context* ctx, fz_document* doc, int chapter, int pageInChapter) {
    fz_page* page = nullptr;
    fz_stext_page* stext = nullptr;
    bool has = false;
    fz_var(page);
    fz_var(stext);
    fz_try(ctx) {
        page = fz_load_chapter_page(ctx, doc, chapter, pageInChapter);
        if (!page) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "reflow page missing");
        }
        fz_stext_options opts = NewTextPageOptions(FZ_STEXT_PRESERVE_IMAGES);
        stext = fz_new_stext_page_from_page(ctx, page, &opts);
        for (fz_stext_block* block = stext->first_block; block; block = block->next) {
            if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                has = true;
                break;
            }
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                for (fz_stext_char* c = line->first_char; c; c = c->next) {
                    int ch = c->c;
                    if (ch > 32 && ch != 0xA0) {
                        has = true;
                        break;
                    }
                }
                if (has) {
                    break;
                }
            }
            if (has) {
                break;
            }
        }
        if (!has) {
            fz_display_list* dl = fz_new_display_list_from_page(ctx, page);
            fz_rect bbox = fz_bound_display_list(ctx, dl);
            fz_drop_display_list(ctx, dl);
            if (!fz_is_empty_rect(bbox) && !fz_is_infinite_rect(bbox)) {
                float w = bbox.x1 - bbox.x0;
                float h = bbox.y1 - bbox.y0;
                if (w > 4.f && h > 4.f) {
                    has = true;
                }
            }
        }
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, stext);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        has = false;
    }
    return has;
}

static int EffectiveReflowChapterPageCount(fz_context* ctx, fz_document* doc, int chapter, int rawPages) {
    if (rawPages <= 0) {
        return 0;
    }
    // Only probe short spine items (anthology padding) to keep background load bounded.
    const int kMaxTrimProbe = 32;
    if (rawPages > kMaxTrimProbe) {
        return rawPages;
    }
    int trimmed = 0;
    while (trimmed < rawPages) {
        int pic = rawPages - 1 - trimmed;
        if (ReflowChapterPageHasVisibleContent(ctx, doc, chapter, pic)) {
            break;
        }
        trimmed++;
    }
    if (trimmed == rawPages) {
        return 0;
    }
    return rawPages - trimmed;
}

static bool LinkifyCheckMultiline(const WCHAR* pageText, const WCHAR* pos, Rect* coords) {
    // multiline links end in a non-alphanumeric character and continue on a line
    // that starts left and only slightly below where the current line ended
    // (and that doesn't start with http or a footnote numeral)
    return '\n' == *pos && pos > pageText && *(pos + 1) && !iswalnum(pos[-1]) && !str::IsWs(pos[1]) &&
           coords[pos - pageText + 1].BR().y > coords[pos - pageText - 1].y &&
           coords[pos - pageText + 1].y <= coords[pos - pageText - 1].BR().y + coords[pos - pageText - 1].dy * 0.35 &&
           coords[pos - pageText + 1].x < coords[pos - pageText - 1].BR().x &&
           coords[pos - pageText + 1].dy >= coords[pos - pageText - 1].dy * 0.85 &&
           coords[pos - pageText + 1].dy <= coords[pos - pageText - 1].dy * 1.2 && !str::StartsWith(pos + 1, L"http");
}

static bool EndsURL(WCHAR c) {
    if (c == 0 || str::IsWs(c)) {
        return true;
    }
    // https://github.com/sumatrapdfreader/sumatrapdf/issues/1313
    // 0xff0c is ","
    if (c == (WCHAR)0xff0c) {
        return true;
    }
    return false;
}

static const WCHAR* LinkifyFindEnd(const WCHAR* start, WCHAR prevChar) {
    const WCHAR* quote = nullptr;

    // look for the end of the URL (ends in a space preceded maybe by interpunctuation)
    const WCHAR* end = start;
    while (!EndsURL(*end)) {
        end++;
    }
    char prev = 0;
    if (end > start) {
        prev = end[-1];
    }
    if (',' == prev || '.' == prev || '?' == prev || '!' == prev) {
        end--;
    }

    prev = 0;
    if (end > start) {
        prev = end[-1];
    }
    // also ignore a closing parenthesis, if the URL doesn't contain any opening one
    if (')' == prev && (!str::FindChar(start, '(') || str::FindChar(start, '(') >= end)) {
        end--;
    }

    // cut the link at the first quotation mark, if it's also preceded by one
    if (('"' == prevChar || '\'' == prevChar) && (quote = str::FindChar(start, prevChar)) != nullptr && quote < end) {
        end = quote;
    }

    return end;
}

static const WCHAR* LinkifyMultilineText(LinkRectList* list, const WCHAR* pageText, const WCHAR* start,
                                         const WCHAR* next, Rect* coords) {
    int lastIx = list->coords.Size() - 1;
    char* uri = list->links.At(lastIx);
    const WCHAR* end = next;
    bool multiline = false;

    do {
        end = LinkifyFindEnd(next, start > pageText ? start[-1] : ' ');
        multiline = LinkifyCheckMultiline(pageText, end, coords);

        char* part = ToUtf8Temp(next, end - next);
        uri = str::JoinTemp(uri, part);
        Rect bbox = coords[next - pageText].Union(coords[end - pageText - 1]);
        list->coords.Append(ToFzRect(ToRectF(bbox)));

        next = end + 1;
    } while (multiline);

    // update the link URL for all partial links
    list->links.SetAt(lastIx, uri);
    for (int i = lastIx + 1; i < list->coords.Size(); i++) {
        list->links.Append(uri);
    }

    return end;
}

// cf. http://weblogs.mozillazine.org/gerv/archives/2011/05/html5_email_address_regexp.html
inline bool IsEmailUsernameChar(WCHAR c) {
    // explicitly excluding the '/' from the list, as it is more
    // often part of a URL or path than of an email address
    return iswalnum(c) || c && str::FindChar(L".!#$%&'*+=?^_`{|}~-", c);
}
inline bool IsEmailDomainChar(WCHAR c) {
    return iswalnum(c) || '-' == c;
}

static const WCHAR* LinkifyFindEmail(const WCHAR* pageText, const WCHAR* at) {
    const WCHAR* start;
    for (start = at; start > pageText && IsEmailUsernameChar(*(start - 1)); start--) {
        // do nothing
    }
    return start != at ? start : nullptr;
}

static const WCHAR* LinkifyEmailAddress(const WCHAR* start) {
    const WCHAR* end;
    for (end = start; IsEmailUsernameChar(*end); end++) {
        ;
    }
    if (end == start || *end != '@' || !IsEmailDomainChar(*(end + 1))) {
        return nullptr;
    }
    for (end++; IsEmailDomainChar(*end); end++) {
        ;
    }
    if ('.' != *end || !IsEmailDomainChar(*(end + 1))) {
        return nullptr;
    }
    do {
        for (end++; IsEmailDomainChar(*end); end++) {
            ;
        }
    } while ('.' == *end && IsEmailDomainChar(*(end + 1)));
    return end;
}

// caller needs to delete the result
// TODO: return Vec<IPageElement*> directly
static LinkRectList* LinkifyText(const WCHAR* pageText, Rect* coords) {
    LinkRectList* list = new LinkRectList;

    for (const WCHAR* start = pageText; *start; start++) {
        const WCHAR* end = nullptr;
        bool multiline = false;
        const WCHAR* protocol = nullptr;

        if ('@' == *start) {
            // potential email address without mailto:
            const WCHAR* email = LinkifyFindEmail(pageText, start);
            end = email ? LinkifyEmailAddress(email) : nullptr;
            protocol = L"mailto:";
            if (end != nullptr) {
                start = email;
            }
        } else if (start > pageText && ('/' == start[-1] || iswalnum(start[-1]))) {
            // hyperlinks must not be preceded by a slash (indicates a different protocol)
            // or an alphanumeric character (indicates part of a different protocol)
        } else if ('h' == *start && str::Parse(start, L"http%?s://")) {
            end = LinkifyFindEnd(start, start > pageText ? start[-1] : ' ');
            multiline = LinkifyCheckMultiline(pageText, end, coords);
        } else if ('w' == *start && str::StartsWith(start, L"www.")) {
            end = LinkifyFindEnd(start, start > pageText ? start[-1] : ' ');
            multiline = LinkifyCheckMultiline(pageText, end, coords);
            protocol = L"http://";
            // ignore www. links without a top-level domain
            if (end - start <= 4 || !multiline && (!wcschr(start + 5, '.') || wcschr(start + 5, '.') >= end)) {
                end = nullptr;
            }
        } else if ('m' == *start && str::StartsWith(start, L"mailto:")) {
            end = LinkifyEmailAddress(start + 7);
        }
        if (!end) {
            continue;
        }

        char* part = ToUtf8Temp(start, end - start);
        char* uri = part;
        if (protocol) {
            char* proto = ToUtf8Temp(protocol);
            uri = str::JoinTemp(proto, part);
        }
        list->links.Append(uri);
        Rect bbox = coords[start - pageText].Union(coords[end - pageText - 1]);
        list->coords.Append(ToFzRect(ToRectF(bbox)));
        if (multiline) {
            end = LinkifyMultilineText(list, pageText, start, end + 1, coords);
        }

        start = end;
    }

    return list;
}

// try to produce an 8-bit palette for saving some memory
static RenderedBitmap* TryRenderAsPaletteImage(fz_pixmap* pixmap) {
    int w = pixmap->w;
    int h = pixmap->h;
    int stride = ((w + 3) / 4) * 4;

    size_t sz = sizeof(BITMAPINFO) + (255 * sizeof(RGBQUAD));
    ScopedMem<BITMAPINFO> bmi((BITMAPINFO*)calloc(1, sz));
    if (!bmi.Get()) {
        return nullptr;
    }
    BITMAPINFOHEADER* bmih = &bmi.Get()->bmiHeader;
    bmih->biSize = sizeof(*bmih);
    bmih->biWidth = w;
    bmih->biHeight = -h;
    bmih->biPlanes = 1;
    bmih->biCompression = BI_RGB;
    bmih->biBitCount = 8;
    bmih->biSizeImage = h * stride;
    bmih->biClrUsed = 256;

    void* data = nullptr;
    HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bmih->biSizeImage, nullptr);
    HBITMAP hbmp = CreateDIBSection(nullptr, bmi, DIB_RGB_COLORS, &data, hMap, 0);
    if (!hbmp) {
        if (hMap) {
            CloseHandle(hMap);
        }
        return nullptr;
    }

    u32* palette = (u32*)bmi.Get()->bmiColors;

    // open-addressed hash table for color -> palette index lookup.
    // key is RGB in source byte order (R | G<<8 | B<<16); empty slot = -1.
    // kHashSize = 1,024 slots (4x the 256 max palette entries -> load factor <= 25%).
    // hashIdx is 1,024 * 2 = 2,048 bytes; hashKey is 1,024 * 4 = 4,096 bytes; 6 KB total on the stack.
    constexpr int kHashBits = 10;
    constexpr int kHashSize = 1 << kHashBits;
    constexpr u32 kHashMask = kHashSize - 1;
    i16 hashIdx[kHashSize];
    u32 hashKey[kHashSize];
    memset(hashIdx, 0xFF, sizeof(hashIdx));

    u8* dest = (u8*)data;
    u8* source = pixmap->samples;
    int paletteSize = 0;
    int padding = stride - w;
    // sentinel that can't equal any masked pixel (alpha bits would be 0)
    u32 lastPx = 0xFFFFFFFFu;
    int lastIdx = 0;

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            u32 px = *(u32*)source & 0x00FFFFFFu;
            source += 4;

            if (px == lastPx) {
                *dest++ = (u8)lastIdx;
                continue;
            }

            u32 slot = (px * 2654435761u) >> (32 - kHashBits);
            int k;
            for (;;) {
                int idx = hashIdx[slot];
                if (idx < 0) {
                    if (paletteSize >= 256) {
                        DeleteObject(hbmp);
                        if (hMap) {
                            CloseHandle(hMap);
                        }
                        return nullptr;
                    }
                    k = paletteSize++;
                    hashKey[slot] = px;
                    hashIdx[slot] = (i16)k;
                    // palette is BGR0 (RGBQUAD layout); source is RGBA, so swap R and B
                    palette[k] = ((px & 0xFFu) << 16) | (px & 0xFF00u) | ((px >> 16) & 0xFFu);
                    break;
                }
                if (hashKey[slot] == px) {
                    k = idx;
                    break;
                }
                slot = (slot + 1) & kHashMask;
            }
            lastPx = px;
            lastIdx = k;
            *dest++ = (u8)k;
        }
        dest += padding;
    }

    bmih->biClrUsed = paletteSize;
    // CreateDIBSection snapshotted the (empty) color table at call time, so push the
    // palette we just built into the DIB now via SetDIBColorTable.
    HDC hdc = CreateCompatibleDC(nullptr);
    if (hdc) {
        HGDIOBJ oldBmp = SelectObject(hdc, hbmp);
        SetDIBColorTable(hdc, 0, paletteSize, (RGBQUAD*)palette);
        SelectObject(hdc, oldBmp);
        DeleteDC(hdc);
    }
    return new RenderedBitmap(hbmp, Size(w, h), hMap);
}

// had to create a copy of fz_convert_pixmap to ensure we always get the alpha
static fz_pixmap* FzConvertPixmap2(fz_context* ctx, fz_pixmap* pix, fz_colorspace* ds, fz_colorspace* prf,
                                   fz_default_colorspaces* default_cs, fz_color_params color_params, int keep_alpha) {
    fz_pixmap* cvt;

    if (!ds && !keep_alpha) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "cannot both throw away and keep alpha");
    }

    cvt = fz_new_pixmap(ctx, ds, pix->w, pix->h, pix->seps, keep_alpha);

    cvt->xres = pix->xres;
    cvt->yres = pix->yres;
    cvt->x = pix->x;
    cvt->y = pix->y;
    if (pix->flags & FZ_PIXMAP_FLAG_INTERPOLATE) {
        cvt->flags |= FZ_PIXMAP_FLAG_INTERPOLATE;
    } else {
        cvt->flags &= ~FZ_PIXMAP_FLAG_INTERPOLATE;
    }

    fz_try(ctx) {
        fz_convert_pixmap_samples(ctx, pix, cvt, prf, default_cs, color_params, 1);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, cvt);
        fz_rethrow(ctx);
    }

    return cvt;
}

static RenderedBitmap* NewRenderedFzPixmap(fz_context* ctx, fz_pixmap* pixmap) {
    if (pixmap->n == 4 && fz_colorspace_is_rgb(ctx, pixmap->colorspace)) {
        RenderedBitmap* res = TryRenderAsPaletteImage(pixmap);
        if (res) {
            return res;
        }
    }

    ScopedMem<BITMAPINFO> bmi((BITMAPINFO*)calloc(1, sizeof(BITMAPINFO) + 255 * sizeof(RGBQUAD)));

    fz_pixmap* bgrPixmap = nullptr;
    fz_colorspace* csdest = nullptr;
    fz_color_params cp;

    fz_var(bgrPixmap);
    fz_var(csdest);
    fz_var(cp);

    /* BGRA is a GDI compatible format */
    fz_try(ctx) {
        csdest = fz_device_bgr(ctx);
        cp = fz_default_color_params;
        bgrPixmap = FzConvertPixmap2(ctx, pixmap, csdest, nullptr, nullptr, cp, 1);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return nullptr;
    }

    if (!bgrPixmap || !bgrPixmap->samples) {
        return nullptr;
    }

    int w = bgrPixmap->w;
    int h = bgrPixmap->h;
    int n = bgrPixmap->n;
    int imgSize = bgrPixmap->stride * h;
    int bitsCount = n * 8;

    BITMAPINFOHEADER* bmih = &bmi.Get()->bmiHeader;
    bmih->biSize = sizeof(*bmih);
    bmih->biWidth = w;
    bmih->biHeight = -h;
    bmih->biPlanes = 1;
    bmih->biCompression = BI_RGB;
    bmih->biBitCount = bitsCount;
    bmih->biSizeImage = imgSize;
    bmih->biClrUsed = 0;

    void* data = nullptr;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD fl = PAGE_READWRITE;
    HANDLE hMap = CreateFileMappingW(hFile, nullptr, fl, 0, imgSize, nullptr);
    uint usage = DIB_RGB_COLORS;
    HBITMAP hbmp = CreateDIBSection(nullptr, bmi, usage, &data, hMap, 0);
    if (data) {
        u8* samples = bgrPixmap->samples;
        memcpy(data, samples, imgSize);
    }
    fz_drop_pixmap(ctx, bgrPixmap);
    if (!hbmp) {
        return nullptr;
    }
    // return a RenderedBitmap even if hbmp is nullptr so that callers can
    // distinguish rendering errors from GDI resource exhaustion
    // (and in the latter case retry using smaller target rectangles)
    return new RenderedBitmap(hbmp, Size(w, h), hMap);
}

static TocItem* NewTocItemWithDestination(TocItem* parent, char* title, IPageDestination* dest) {
    auto res = new TocItem(parent, title, 0);
    res->dest = dest;
    return res;
}

// TODO: could be optimized
static bool RectFullyContains(RectF r1, RectF r2) {
    // if same size, we don't consider it that one covers another
    if (r1 == r2) {
        return false;
    }
    return r1.Contains(r2.TL()) && r1.Contains(r2.BR());
}

// if an elements fully obscures another, remove it from the list
static bool RemoveHeWhoFullyContains(Vec<IPageElement*>& els) {
    int n = els.Size();
    ReportIf(n < 2);
    for (int i = 0; i < n; i++) {
        RectF r1 = els[i]->GetRect();
        for (int j = 0; j < n; j++) {
            if (j == i) {
                continue; // skip checking against self
            }
            auto r2 = els[j]->GetRect();
            if (RectFullyContains(r1, r2)) {
                // logfa("el %d fully obscures %d\n", i, j);
                els.RemoveAtFast(i);
                return true;
            }
        }
    }
    return false;
}

// if we have multiple elements at the same position, pick the one
// that is fully obscured by all other elements
// if not fully obscured, return the first one
static IPageElement* PickBestElement(Vec<IPageElement*>& els) {
    int n = els.Size();
    if (n == 0) {
        return nullptr;
    }
    if (n == 1) {
        return els[0];
    }

    // for https://github.com/sumatrapdfreader/sumatrapdf/issues/5200
    // priority for destinations (e.g. links) over images
    for (IPageElement* el : els) {
        if (el->GetKind() == kindPageElementDest) {
            return el;
        }
    }
Encore:
    bool didRemove = RemoveHeWhoFullyContains(els);
    if (didRemove) {
        ReportIf(els.Size() != n - 1);
        n = els.Size();
        if (n == 1) {
            return els[0];
        }
        goto Encore;
    }
    return els[0];
}

// don't delete the result
NO_INLINE static IPageElement* FzGetElementAtPos(FzPageInfo* pageInfo, PointF pt) {
    if (!pageInfo) {
        return nullptr;
    }
    Vec<IPageElement*> res;

    for (auto pel : pageInfo->links) {
        if (pel->GetRect().Contains(pt)) {
            res.Append(pel);
        }
    }

    for (auto* pel : pageInfo->autoLinks) {
        if (pel->GetRect().Contains(pt)) {
            res.Append(pel);
        }
    }

    for (auto* pel : pageInfo->comments) {
        if (pel->GetRect().Contains(pt)) {
            res.Append(pel);
        }
    }

    fz_point p = {(float)pt.x, (float)pt.y};
    for (auto& img : pageInfo->images) {
        fz_rect ir = img->rect;
        if (IsPointInRect(ir, p)) {
            res.Append(img->imageElement);
        }
    }

    if (false) {
        int i = 0;
        for (auto&& el : res) {
            Rect r = el->GetRect().Round();
            logfa("el %d: pos: %d-%d, size: %d-%d, kind: %s\n", (int)i, r.x, r.y, r.dx, r.dy, el->GetKind());
            i++;
        }
    }
    return PickBestElement(res);
}

static void BuildElementsInfo(FzPageInfo* pageInfo) {
    if (!pageInfo || !pageInfo->elementsNeedRebuilding) {
        return;
    }
    pageInfo->elementsNeedRebuilding = false;
    auto& els = pageInfo->allElements;

    size_t total =
        pageInfo->images.size() + pageInfo->links.size() + pageInfo->autoLinks.size() + pageInfo->comments.size();
    els.Clear();
    els.EnsureCap(total);

    // since all elements lists are in last-to-first order, append
    // item types in inverse order and reverse the whole list at the end
    for (auto& img : pageInfo->images) {
        els.Append(img->imageElement);
    }
    for (auto& pel : pageInfo->links) {
        els.Append(pel);
    }
    for (auto& pel : pageInfo->autoLinks) {
        els.Append(pel);
    }
    for (auto& comment : pageInfo->comments) {
        els.Append(comment);
    }
    els.Reverse();
}

static void FzLinkifyPageText(FzPageInfo* pageInfo, fz_stext_page* stext) {
    if (!pageInfo || !stext) {
        return;
    }

    Rect* coords;
    WCHAR* pageText = FzTextPageToStr(stext, &coords);
    if (!pageText) {
        return;
    }

    LinkRectList* list = LinkifyText(pageText, coords);
    free(pageText);

    for (int i = 0; i < list->links.Size(); i++) {
        fz_rect bbox = list->coords.at(i);
        bool overlaps = false;
        for (auto pel : pageInfo->links) {
            overlaps = FzRectOverlap(bbox, pel->GetRect()) >= 0.25f;
        }
        if (overlaps) {
            continue;
        }

        char* uri = list->links[i];
        if (!uri) {
            continue;
        }

        // TODO: those leak on xps
        auto dest = new PageDestinationURL(uri);
        auto pel = new PageElementDestination(dest);
        pel->rect = ToRectF(bbox);
        pageInfo->autoLinks.Append(pel);
    }
    delete list;
    free(coords);
}

static int MinPreservePdfImageSizePx() {
    return GetPreservePdfImagesMinSize();
}

static void FzAppendPageImageRect(fz_context* ctx, Vec<FitzPageImageInfo*>& images, int pageNo, fz_rect bbox,
                                  fz_rect unclipped, fz_image* image) {
    if (fz_is_empty_rect(bbox) || fz_is_infinite_rect(bbox)) {
        return;
    }
    RectF rf = ToRectF(bbox);
    if (rf.IsEmpty()) {
        return;
    }
    for (FitzPageImageInfo* existing : images) {
        if (FzRectOverlap(existing->rect, rf) > 0.85f) {
            if (ctx && image && !existing->image) {
                existing->image = fz_keep_image(ctx, image);
            }
            if (fz_is_empty_rect(existing->unclipped) && !fz_is_empty_rect(unclipped)) {
                existing->unclipped = unclipped;
            }
            return;
        }
    }
    FitzPageImageInfo* img = new FitzPageImageInfo{bbox, fz_identity};
    if (ctx && image) {
        img->image = fz_keep_image(ctx, image);
    }
    img->unclipped = unclipped;
    auto pel = new PageElementImage();
    pel->pageNo = pageNo;
    pel->rect = rf;
    pel->imageID = images.Size();
    img->imageElement = pel;
    images.Append(img);
}

#define IMG_COLLECT_STACK_SIZE 96

typedef struct {
    fz_device super;
    Vec<FitzPageImageInfo*>* images;
    int pageNo;
    int top;
    fz_rect stack[IMG_COLLECT_STACK_SIZE];
} fz_image_collect_device;

static void fz_img_collect_add(fz_context* ctx, fz_device* dev, fz_rect rect, bool clip, fz_image* image) {
    fz_image_collect_device* d = (fz_image_collect_device*)dev;
    fz_rect unclipped = rect;
    if (d->top > 0 && d->top <= IMG_COLLECT_STACK_SIZE) {
        rect = fz_intersect_rect(rect, d->stack[d->top - 1]);
    }
    if (!clip && !fz_is_empty_rect(rect)) {
        FzAppendPageImageRect(ctx, *d->images, d->pageNo, rect, unclipped, image);
    }
    if (clip && ++d->top <= IMG_COLLECT_STACK_SIZE) {
        d->stack[d->top - 1] = rect;
    }
}

static void fz_img_collect_fill_image(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm, float alpha,
                                      fz_color_params color_params) {
    (void)alpha;
    (void)color_params;
    fz_img_collect_add(ctx, dev, fz_transform_rect(fz_unit_rect, ctm), false, image);
}

// Image masks are knockouts / stencil shapes, not photos, so dark-mode preserve
// must not treat them as artwork. Do not call fz_img_collect_add(..., clip=true):
// fill_image_mask is not a clipping operation and has no matching pop_clip
// (unlike clip_image_mask). Pushing an unpopped clip shrinks later image rects
// and can leave the collect device's clip stack unbalanced.
static void fz_img_collect_fill_image_mask(fz_context*, fz_device*, fz_image*, fz_matrix, fz_colorspace*, const float*,
                                           float, fz_color_params) {}

static void fz_img_collect_clip_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd, fz_matrix ctm,
                                     fz_rect scissor) {
    (void)scissor;
    fz_img_collect_add(ctx, dev, fz_bound_path(ctx, path, nullptr, ctm), true, nullptr);
}

static void fz_img_collect_clip_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path,
                                            const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    (void)scissor;
    fz_img_collect_add(ctx, dev, fz_bound_path(ctx, path, stroke, ctm), true, nullptr);
}

static void fz_img_collect_clip_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                                     fz_rect scissor) {
    (void)scissor;
    fz_img_collect_add(ctx, dev, fz_bound_text(ctx, text, nullptr, ctm), true, nullptr);
}

static void fz_img_collect_clip_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text,
                                            const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    (void)scissor;
    fz_img_collect_add(ctx, dev, fz_bound_text(ctx, text, stroke, ctm), true, nullptr);
}

static void fz_img_collect_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* image, fz_matrix ctm,
                                           fz_rect scissor) {
    (void)image;
    (void)scissor;
    fz_img_collect_add(ctx, dev, fz_transform_rect(fz_unit_rect, ctm), true, nullptr);
}

static void fz_img_collect_pop_clip(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    fz_image_collect_device* d = (fz_image_collect_device*)dev;
    if (d->top > 0) {
        d->top--;
    }
}

static void fz_img_collect_begin_mask(fz_context* ctx, fz_device* dev, fz_rect rect, int luminosity,
                                      fz_colorspace* colorspace, const float* color, fz_color_params color_params) {
    (void)luminosity;
    (void)colorspace;
    (void)color;
    (void)color_params;
    fz_img_collect_add(ctx, dev, rect, true, nullptr);
}

static void fz_img_collect_end_mask(fz_context* ctx, fz_device* dev, fz_function* tr) {
    (void)tr;
    fz_img_collect_pop_clip(ctx, dev);
}

static void fz_img_collect_begin_group(fz_context* ctx, fz_device* dev, fz_rect rect, fz_colorspace* cs, int isolated,
                                       int knockout, int blendmode, float alpha) {
    (void)cs;
    (void)isolated;
    (void)knockout;
    (void)blendmode;
    (void)alpha;
    fz_img_collect_add(ctx, dev, rect, true, nullptr);
}

static void fz_img_collect_end_group(fz_context* ctx, fz_device* dev) {
    fz_img_collect_pop_clip(ctx, dev);
}

static int fz_img_collect_begin_tile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep,
                                     float ystep, fz_matrix ctm, int id, int doc_id) {
    (void)view;
    (void)xstep;
    (void)ystep;
    (void)id;
    (void)doc_id;
    fz_img_collect_add(ctx, dev, fz_transform_rect(area, ctm), false, nullptr);
    return 0;
}

static void fz_img_collect_end_tile(fz_context* ctx, fz_device* dev) {
    (void)ctx;
    (void)dev;
}

static fz_device* FzNewImageCollectDevice(fz_context* ctx, Vec<FitzPageImageInfo*>* images, int pageNo) {
    fz_image_collect_device* dev = fz_new_derived_device(ctx, fz_image_collect_device);
    dev->super.fill_image = fz_img_collect_fill_image;
    dev->super.fill_image_mask = fz_img_collect_fill_image_mask;
    dev->super.clip_path = fz_img_collect_clip_path;
    dev->super.clip_stroke_path = fz_img_collect_clip_stroke_path;
    dev->super.clip_text = fz_img_collect_clip_text;
    dev->super.clip_stroke_text = fz_img_collect_clip_stroke_text;
    dev->super.clip_image_mask = fz_img_collect_clip_image_mask;
    dev->super.pop_clip = fz_img_collect_pop_clip;
    dev->super.begin_mask = fz_img_collect_begin_mask;
    dev->super.end_mask = fz_img_collect_end_mask;
    dev->super.begin_group = fz_img_collect_begin_group;
    dev->super.end_group = fz_img_collect_end_group;
    dev->super.begin_tile = fz_img_collect_begin_tile;
    dev->super.end_tile = fz_img_collect_end_tile;
    dev->images = images;
    dev->pageNo = pageNo;
    dev->top = 0;
    return &dev->super;
}

static bool PdfShouldCollectContentImages() {
    if (GetPdfDocumentColorMode() == PdfDocumentColorMode::Light) {
        return false;
    }
    if (!PdfSmartModePreservesEmbeddedImages()) {
        return false;
    }
    // Object-level Smart Dark builds its own analysis from the display list.
    if (PdfDarkModeUsesObjectLevel() && GetPdfDocumentColorMode() == PdfDocumentColorMode::Auto &&
        ThemeUsesDarkChrome()) {
        return false;
    }
    return true;
}

static void FzCollectImagesFromPageContent(fz_context* ctx, int pageNo, FzPageInfo* pageInfo, fz_page* page,
                                           fz_cookie* cookie) {
    fz_device* dev = nullptr;
    fz_var(dev);
    fz_try(ctx) {
        dev = FzNewImageCollectDevice(ctx, &pageInfo->images, pageNo);
        fz_run_page(ctx, page, dev, fz_identity, cookie);
    }
    fz_always(ctx) {
        if (dev) {
            fz_drop_device(ctx, dev);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
}

// Calibre photo books place the same image XObject on consecutive pages: one
// page shows most of the photo (clipped), the next only the leftover strip.
// Hide the strip page and paint the full unclipped image on the keeper page.
static bool PdfJoinSplitImagesMatch(fz_image* a, fz_image* b) {
    if (!a || !b) {
        return false;
    }
    if (a == b) {
        return true;
    }
    // Shared PDF XObjects normally share the fz_image store entry; fall back to
    // identical pixel dimensions when pointers differ.
    return a->w == b->w && a->h == b->h && a->w >= 64 && a->h >= 64;
}

struct JoinSplitPageImage {
    fz_image* image = nullptr;
    float coverage = 0.f;
    RectF vis{};
    RectF unclipped{};
};

static JoinSplitPageImage JoinSplitPickDominantImage(FzPageInfo* pi) {
    JoinSplitPageImage best{};
    if (!pi || pi->mediabox.IsEmpty()) {
        return best;
    }
    float pageArea = pi->mediabox.dx * pi->mediabox.dy;
    if (pageArea <= 0.f) {
        return best;
    }
    for (FitzPageImageInfo* info : pi->images) {
        if (!info || !info->image) {
            continue;
        }
        RectF visible = ToRectF(info->rect).Intersect(pi->mediabox);
        if (visible.IsEmpty()) {
            continue;
        }
        float cov = (visible.dx * visible.dy) / pageArea;
        if (cov > best.coverage) {
            best.coverage = cov;
            best.image = info->image;
            best.vis = visible;
            best.unclipped = ToRectF(info->unclipped);
        }
    }
    return best;
}

static void JoinSplitApplyKeeperDest(EngineMupdf* e, int keeperIdx, const RectF& dest, fz_image* image) {
    if (!e || keeperIdx < 0 || keeperIdx >= e->pages.Size() || dest.IsEmpty() || !image) {
        return;
    }
    FzPageInfo* kpi = e->pages[keeperIdx];
    if (!kpi) {
        return;
    }
    float destY1 = dest.y + dest.dy;
    float boxY1 = kpi->mediabox.y + kpi->mediabox.dy;
    if (destY1 > boxY1) {
        kpi->mediabox.dy = destY1 - kpi->mediabox.y;
    }
    if (dest.y < kpi->mediabox.y) {
        float extra = kpi->mediabox.y - dest.y;
        kpi->mediabox.y = dest.y;
        kpi->mediabox.dy += extra;
    }
    for (FitzPageImageInfo* info : kpi->images) {
        if (!info || !info->image || !PdfJoinSplitImagesMatch(info->image, image)) {
            continue;
        }
        info->rect = ToFzRect(dest);
        if (info->imageElement) {
            info->imageElement->rect = dest;
        }
    }
}

static void JoinSplitBlitOntoPixmap(fz_context* ctx, EngineMupdf* e, int enginePageNo, fz_pixmap* pix,
                                    fz_matrix viewCtm) {
    if (!e || !pix || e->joinExtend.Size() == 0) {
        return;
    }
    if (enginePageNo < 1 || enginePageNo > e->joinExtend.Size()) {
        return;
    }
    const EngineMupdf::JoinSplitExtend& ext = e->joinExtend[enginePageNo - 1];
    if (!ext.image || ext.dest.IsEmpty()) {
        return;
    }
    fz_device* d = nullptr;
    fz_var(d);
    fz_try(ctx) {
        d = fz_new_draw_device(ctx, viewCtm, pix);
        fz_matrix imgCtm = fz_make_matrix(ext.dest.dx, 0, 0, ext.dest.dy, ext.dest.x, ext.dest.y);
        fz_fill_image(ctx, d, ext.image, imgCtm, 1.f, fz_default_color_params);
        fz_close_device(ctx, d);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, d);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
}

static void ApplyJoinSplitPdfImages(EngineMupdf* e) {
    if (!e || !e->pdfdoc) {
        return;
    }
    if (!gGlobalPrefs || !gGlobalPrefs->fixedPageUI.joinSplitPdfImages) {
        return;
    }
    if (IsCreateEngineForThumbnail()) {
        return;
    }
    int rawN = e->pageCount;
    if (rawN < 4) {
        return;
    }

    fz_context* ctx = e->Ctx();
    // Acrobat/InDesign/Office layout PDFs are not Calibre strip albums. Running
    // fz_run_page on every page at open (e.g. 900+ page textbooks) made open
    // multi-minute. Skip the whole-document scan for those.
    if (PdfDarkModePdfMetadataSuggestsLayoutPhotoDoc(ctx, e->pdfdoc) ||
        PdfDarkModePdfMetadataSuggestsPaperCaptureDoc(ctx, e->pdfdoc)) {
        return;
    }

    Vec<JoinSplitPageImage> perPage;
    perPage.EnsureCap((size_t)rawN);
    // Calibre strip pairs show up early. If the first chunk has no candidates,
    // do not pay for parsing the rest of a large non-album PDF.
    constexpr int kJoinSplitProbePages = 32;
    int probeSkipCount = 0;
    for (int pageNo = 1; pageNo <= rawN; pageNo++) {
        JoinSplitPageImage info{};
        // Cheap reject: no image XObject → cannot be a strip page.
        if (!PdfPageObjHasImageXObject(ctx, e->pdfdoc, pageNo - 1)) {
            perPage.Append(info);
        } else {
            FzPageInfo* pi = e->GetFzPageInfo(pageNo, true, nullptr, false);
            if (pi && pi->page) {
                if (!pi->contentImagesCollected) {
                    ScopedCritSec rl(&e->renderLock);
                    FzCollectImagesFromPageContent(ctx, pageNo, pi, pi->page, nullptr);
                    pi->contentImagesCollected = true;
                }
                info = JoinSplitPickDominantImage(pi);
            }
            perPage.Append(info);
        }
        if (pageNo >= 2) {
            const JoinSplitPageImage& a = perPage[pageNo - 2];
            const JoinSplitPageImage& b = perPage[pageNo - 1];
            if (PdfJoinSplitImagesMatch(a.image, b.image) &&
                PdfJoinSplitShouldHideLowerCoverage(a.coverage, b.coverage)) {
                probeSkipCount++;
            }
        }
        if (pageNo == kJoinSplitProbePages && probeSkipCount == 0) {
            return;
        }
    }

    Vec<u8> skip;
    skip.SetSize((size_t)rawN);
    for (int i = 0; i < rawN; i++) {
        skip[i] = 0;
    }
    int skipCount = 0;
    for (int i = 0; i < rawN - 1; i++) {
        if (skip[i] || skip[i + 1]) {
            continue;
        }
        const JoinSplitPageImage& a = perPage[i];
        const JoinSplitPageImage& b = perPage[i + 1];
        if (!PdfJoinSplitImagesMatch(a.image, b.image)) {
            continue;
        }
        if (!PdfJoinSplitShouldHideLowerCoverage(a.coverage, b.coverage)) {
            continue;
        }
        if (a.coverage <= b.coverage) {
            skip[i] = 1;
        } else {
            skip[i + 1] = 1;
        }
        skipCount++;
    }

    // Require a clear signal so normal documents are not remapped by accident.
    if (skipCount < 4 && skipCount * 12 < rawN) {
        return;
    }

    e->joinRawPageCount = rawN;
    e->joinDisplayToEngine.Reset();
    e->joinEngineToDisplay.Reset();
    e->joinExtend.Reset();
    e->joinExtend.SetSize((size_t)rawN);
    e->joinDisplayToEngine.EnsureCap((size_t)(rawN - skipCount));
    e->joinEngineToDisplay.SetSize((size_t)rawN);
    for (int i = 0; i < rawN; i++) {
        e->joinEngineToDisplay[i] = 0;
    }
    {
        ScopedCritSec scope(&e->pagesLock);
        for (int i = 0; i < rawN - 1; i++) {
            if (!skip[i] && !skip[i + 1]) {
                continue;
            }
            if (skip[i] && skip[i + 1]) {
                continue;
            }
            int keeper = skip[i] ? i + 1 : i;
            int strip = skip[i] ? i : i + 1;
            bool stripAfter = strip > keeper;
            RectF dest =
                PdfJoinSplitKeeperDest(perPage[keeper].vis, perPage[keeper].unclipped, perPage[strip].vis, stripAfter);
            e->joinExtend[keeper].image = perPage[keeper].image;
            e->joinExtend[keeper].dest = dest;
            JoinSplitApplyKeeperDest(e, keeper, dest, perPage[keeper].image);
        }
    }
    for (int i = 0; i < rawN; i++) {
        if (skip[i]) {
            continue;
        }
        int displayNo = e->joinDisplayToEngine.Size() + 1;
        e->joinDisplayToEngine.Append(i + 1);
        e->joinEngineToDisplay[i] = displayNo;
    }
    int lastDisplay = 1;
    for (int i = 0; i < rawN; i++) {
        if (e->joinEngineToDisplay[i] > 0) {
            lastDisplay = e->joinEngineToDisplay[i];
        } else {
            e->joinEngineToDisplay[i] = lastDisplay;
        }
    }

    e->pageCount = e->joinDisplayToEngine.Size();
    logf("JoinSplitPdfImages: collapsed %d -> %d pages (joined %d strip pages onto keepers)\n", rawN, e->pageCount,
         skipCount);
}

int EngineMupdfMapDisplayPageToEngine(EngineBase* engine, int displayPageNo) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || e->joinDisplayToEngine.Size() == 0) {
        return displayPageNo;
    }
    if (displayPageNo < 1 || displayPageNo > e->joinDisplayToEngine.Size()) {
        return -1;
    }
    return e->joinDisplayToEngine[displayPageNo - 1];
}

int EngineMupdfMapEnginePageToDisplay(EngineBase* engine, int enginePageNo) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || e->joinEngineToDisplay.Size() == 0) {
        return enginePageNo;
    }
    if (enginePageNo < 1 || enginePageNo > e->joinEngineToDisplay.Size()) {
        return -1;
    }
    return e->joinEngineToDisplay[enginePageNo - 1];
}

bool EngineMupdfHasJoinSplitPdfImages(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    return e && e->joinDisplayToEngine.Size() > 0;
}

static int MapJoinDisplayPageNo(EngineMupdf* e, int displayPageNo) {
    if (!e || e->joinDisplayToEngine.Size() == 0) {
        return displayPageNo;
    }
    if (displayPageNo < 1 || displayPageNo > e->joinDisplayToEngine.Size()) {
        return -1;
    }
    return e->joinDisplayToEngine[displayPageNo - 1];
}

static void FzFindImagePositions(fz_context* ctx, int pageNo, Vec<FitzPageImageInfo*>& images, fz_stext_page* stext) {
    if (!stext) {
        return;
    }
    fz_stext_block* block = stext->first_block;
    fz_image* image;
    while (block) {
        if (block->type != FZ_STEXT_BLOCK_IMAGE) {
            block = block->next;
            continue;
        }
        image = block->u.i.image;
        if (!image) {
            block = block->next;
            continue;
        }
        {
            // https://github.com/sumatrapdfreader/sumatrapdf/issues/1480
            // fz_convert_pixmap_samples doesn't handle src without colorspace
            FitzPageImageInfo* img = new FitzPageImageInfo{block->bbox, block->u.i.transform};
            img->image = fz_keep_image(ctx, image);
            auto pel = new PageElementImage();
            pel->pageNo = pageNo;
            pel->rect = ToRectF(block->bbox);
            pel->imageID = images.Size();
            img->imageElement = pel;
            images.Append(img);
        }
        block = block->next;
    }
}

static fz_image* FzFindImageByRect(fz_context* ctx, FzPageInfo* pageInfo, fz_rect target);

static fz_image* FzGetKeptPageImage(fz_context* ctx, FzPageInfo* pageInfo, int idx) {
    if (!pageInfo || idx < 0 || idx >= pageInfo->images.Size()) {
        return nullptr;
    }
    FitzPageImageInfo* info = pageInfo->images.at(idx);
    if (info->image) {
        return fz_keep_image(ctx, info->image);
    }
    return FzFindImageByRect(ctx, pageInfo, info->rect);
}

static fz_image* FzFindImageByRect(fz_context* ctx, FzPageInfo* pageInfo, fz_rect target) {
    fz_stext_options opts = NewTextPageOptions(FZ_STEXT_PRESERVE_IMAGES);
    fz_stext_page* stext = nullptr;
    fz_var(stext);
    fz_try(ctx) {
        stext = fz_new_stext_page_from_page(ctx, pageInfo->page, &opts);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    if (!stext) {
        return nullptr;
    }
    float bestOverlap = 0.f;
    fz_image* best = nullptr;
    fz_stext_block* block = stext->first_block;
    while (block) {
        if (block->type != FZ_STEXT_BLOCK_IMAGE) {
            block = block->next;
            continue;
        }
        fz_image* image = block->u.i.image;
        if (!image) {
            block = block->next;
            continue;
        }
        float overlap = FzRectOverlap(block->bbox, target);
        if (overlap > bestOverlap) {
            if (best) {
                fz_drop_image(ctx, best);
            }
            bestOverlap = overlap;
            best = fz_keep_image(ctx, image);
        }
        block = block->next;
    }
    fz_drop_stext_page(ctx, stext);
    if (bestOverlap < 0.45f) {
        if (best) {
            fz_drop_image(ctx, best);
            best = nullptr;
        }
    }
    return best;
}

// Some EPUBs (e.g. bundled 棚车少年) ship with placeholder chapter-1 hrefs like
// "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" while chapter 2+ links are valid partNNNN.xhtml#aNNN.
static bool IsPlaceholderEpubHref(const char* uri) {
    if (!uri || !*uri) {
        return false;
    }
    size_t n = str::Len(uri);
    if (n < 8) {
        return false;
    }
    char c = uri[0];
    if (c != 'X' && c != 'x') {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        char ch = uri[i];
        if (ch != c && ch != (char)(c ^ 0x20)) {
            return false;
        }
    }
    return true;
}

static bool IsEpubPartFragmentHref(const char* uri) {
    return uri && str::StartsWith(uri, "part") && str::FindChar(uri, '#');
}

static char* DeriveChapter1HrefFromChapter2(const char* ch2uri) {
    if (!IsEpubPartFragmentHref(ch2uri)) {
        return nullptr;
    }
    const char* hash = str::FindChar(ch2uri, '#');
    if (!hash || hash[1] != 'a') {
        return nullptr;
    }
    int partNum = atoi(ch2uri + 4);
    int idNum = atoi(hash + 2);
    if (partNum <= 0 || idNum < 2) {
        return nullptr;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "part%04d.xhtml#a%03d", partNum - 1, idNum - 2);
    return str::Dup(buf);
}

static void HealPlaceholderEpubPageLinks(EngineMupdf* e, fz_context* ctx, fz_document* doc,
                                         Vec<PageElementDestination*>& links) {
    if (!e || !ctx || !doc) {
        return;
    }
    int n = (int)links.Size();
    for (int i = 0; i < n; i++) {
        PageElementDestination* pel = links.at(i);
        if (!pel || !pel->dest || pel->dest->GetKind() != kindDestinationMupdf) {
            continue;
        }
        auto* pd = (PageDestinationMupdf*)pel->dest;
        if (!pd->link || pd->value) {
            continue;
        }
        char* uri = pd->link->uri;
        if (!IsPlaceholderEpubHref(uri)) {
            continue;
        }
        for (int j = i + 1; j < n; j++) {
            PageElementDestination* pel2 = links.at(j);
            if (!pel2 || !pel2->dest || pel2->dest->GetKind() != kindDestinationMupdf) {
                continue;
            }
            auto* pd2 = (PageDestinationMupdf*)pel2->dest;
            if (!pd2->link) {
                continue;
            }
            char* uri2 = pd2->link->uri;
            if (!IsEpubPartFragmentHref(uri2)) {
                continue;
            }
            char* healed = DeriveChapter1HrefFromChapter2(uri2);
            if (!healed) {
                continue;
            }
            pd->value = healed;
            pd->reflowOutlineChapter = EpubUriChapterIndexNoLayout(ctx, doc, healed);
            break;
        }
    }
}

static fz_link* FixupPageLinks(fz_link* root) {
    // Links in PDF documents are added from bottom-most to top-most,
    // i.e. links that appear later in the list should be preferred
    // to links appearing before. Since we search from the start of
    // the (single-linked) list, we have to reverse the order of links
    // (http://code.google.com/p/sumatrapdf/issues/detail?id=1303 )
    fz_link* new_root = nullptr;
    while (root) {
        fz_link* tmp = root->next;
        root->next = new_root;
        new_root = root;
        root = tmp;

        // there are PDFs that have x,y positions in reverse order, so fix them up
        fz_link* link = new_root;
        if (link->rect.x0 > link->rect.x1) {
            std::swap(link->rect.x0, link->rect.x1);
        }
        if (link->rect.y0 > link->rect.y1) {
            std::swap(link->rect.y0, link->rect.y1);
        }
        ReportIf(link->rect.x1 < link->rect.x0);
        ReportIf(link->rect.y1 < link->rect.y0);
    }
    return new_root;
}

pdf_obj* PdfCopyStrDict(fz_context* ctx, pdf_document* doc, pdf_obj* dict) {
    pdf_obj* copy = pdf_copy_dict(ctx, dict);
    for (int i = 0; i < pdf_dict_len(ctx, copy); i++) {
        pdf_obj* val = pdf_dict_get_val(ctx, copy, i);
        // resolve all indirect references
        if (pdf_is_indirect(ctx, val)) {
            auto s = pdf_to_str_buf(ctx, val);
            auto slen = pdf_to_str_len(ctx, val);
            pdf_obj* val2 = pdf_new_string(ctx, s, slen);
            pdf_dict_put(ctx, copy, pdf_dict_get_key(ctx, copy, i), val2);
            pdf_drop_obj(ctx, val2);
        }
    }
    return copy;
}

// Note: make sure to only call with docLock
// PdfLoadAttachment && PdfLoadAttachments must traverse in the same order
static ByteSlice PdfLoadAttachment(fz_context* ctx, pdf_document* doc, int no) {
    pdf_obj* dict;
    fz_var(dict);
    ByteSlice res;

    fz_try(ctx) {
        dict = pdf_load_name_tree(ctx, doc, PDF_NAME(EmbeddedFiles));
        if (!dict) {
            break;
        }

        int n = pdf_dict_len(ctx, dict);
        for (int i = 0; i < n; i++) {
            pdf_obj* fs = pdf_dict_get_val(ctx, dict, i);

            // https://github.com/sumatrapdfreader/sumatrapdf/issues/1666
            if (false && !pdf_is_embedded_file(ctx, fs)) {
                continue;
            }
            if (no == i + 1) {
                fz_buffer* buf = pdf_load_embedded_file_contents(ctx, fs);
                res.d = (u8*)memdup(buf->data, buf->len);
                res.sz = buf->len;
                fz_drop_buffer(ctx, buf);
                i = n + 1; // exit for loop
            }
        }
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, dict);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logfa("PdfLoadAttachment() failed\n");
    }
    return res;
}

// load embedded file data from a file attachment annotation by PDF object number
static ByteSlice PdfLoadAnnotationAttachment(fz_context* ctx, pdf_document* doc, int objNum) {
    ByteSlice res;
    fz_try(ctx) {
        pdf_obj* obj = pdf_new_indirect(ctx, doc, objNum, 0);
        pdf_obj* fs = pdf_dict_get(ctx, obj, PDF_NAME(FS));
        if (!fs) {
            pdf_drop_obj(ctx, obj);
            break;
        }
        fz_buffer* buf = pdf_load_embedded_file_contents(ctx, fs);
        if (buf) {
            res.d = (u8*)memdup(buf->data, buf->len);
            res.sz = buf->len;
            fz_drop_buffer(ctx, buf);
        }
        pdf_drop_obj(ctx, obj);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logfa("PdfLoadAnnotationAttachment(objNum=%d) failed\n", objNum);
    }
    return res;
}

// Note: make sure to only call with docLock
static fz_outline* PdfLoadAttachments(fz_context* ctx, pdf_document* doc, const char* path) {
    fz_outline root{};
    pdf_obj* dict;

    fz_var(root);
    fz_var(dict);

    fz_try(ctx) {
        dict = pdf_load_name_tree(ctx, doc, PDF_NAME(EmbeddedFiles));
        if (!dict) {
            break;
        }

        fz_outline* curr = &root;
        for (int i = 0; i < pdf_dict_len(ctx, dict); i++) {
            pdf_obj* fs = pdf_dict_get_val(ctx, dict, i);

            // https://github.com/sumatrapdfreader/sumatrapdf/issues/1666
            if (false && !pdf_is_embedded_file(ctx, fs)) {
                continue;
            }
            pdf_filespec_params fileParams = {};
            pdf_get_filespec_params(ctx, fs, &fileParams);
            const char* nameStr = fileParams.filename;
            if (str::IsEmpty(nameStr) || (fileParams.size < 0)) {
                continue;
            }
            if (str::EndsWithI(nameStr, ".zst") || str::EndsWithI(nameStr, ".json") ||
                str::EndsWithI(nameStr, ".tar") || str::EndsWithI(nameStr, ".zip") ||
                str::FindI(nameStr, "annas_archive") || str::FindI(nameStr, "original_files")) {
                continue;
            }
            fz_outline* link = fz_new_outline(ctx);
            link->title = fz_strdup(ctx, nameStr);
            link->page.page = i + 1;
            link->uri = fz_strdup(ctx, nameStr);
            curr->next = link;
            curr = link;
        }
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, dict);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logfa("PdfLoadAttachments() failed for '%s'\n", path);
    }
    return root.next;
}

struct PageLabelInfo {
    int startAt = 0;
    int countFrom = 0;
    const char* type = nullptr;
    pdf_obj* prefix = nullptr;
};

int CmpPageLabelInfo(const void* a, const void* b) {
    return ((PageLabelInfo*)a)->startAt - ((PageLabelInfo*)b)->startAt;
}

// Some PDFs (often scanned ebooks) assign a separate PageLabels entry to
// almost every page with only a custom prefix and no numbering style (/S).
// These aren't meaningful page numbers and break the toolbar display.
static bool IsPerPagePrefixOnlyLabels(const Vec<PageLabelInfo>& data, int pageCount) {
    size_t n = data.size();
    if (n < 16 || pageCount <= 0 || (int)n < pageCount / 4) {
        return false;
    }
    int prefixOnly = 0;
    for (size_t i = 0; i < n; i++) {
        const PageLabelInfo& pli = data.at(i);
        if ((!pli.type || !*pli.type) && pli.prefix) {
            prefixOnly++;
        }
    }
    return prefixOnly * 4 >= (int)n * 3;
}

static bool PageLabelsContainInternalPdgNames(StrVec* labels, int pageCount) {
    int n = labels->Size();
    int samples = std::min(n, pageCount);
    samples = std::min(samples, 32);
    for (int i = 0; i < samples; i++) {
        const char* label = labels->At(i);
        if (label && str::ContainsI(label, ".pdg")) {
            return true;
        }
    }
    return false;
}

static TempStr FormatPageLabelTemp(const char* type, int pageNo, const char* prefix) {
    if (str::Eq(type, "D")) {
        return str::FormatTemp("%s%d", prefix, pageNo);
    }
    if (str::EqI(type, "R")) {
        // roman numbering style
        TempStr number = str::FormatRomanNumeralTemp(pageNo);
        if (*type == 'r') {
            str::ToLowerInPlace(number);
        }
        return str::FormatTemp("%s%s", prefix, number);
    }
    if (str::EqI(type, "A")) {
        // alphabetic numbering style (A..Z, AA..ZZ, AAA..ZZZ, ...)
        StrBuilder number;
        number.AppendChar('A' + (pageNo - 1) % 26);
        for (int i = 0; i < (pageNo - 1) / 26; i++) {
            number.AppendChar(number.at(0));
        }
        if (*type == 'a') {
            str::ToLowerInPlace(number.Get());
        }
        return str::FormatTemp("%s%s", prefix, number.Get());
    }
    return str::DupTemp(prefix);
}

void BuildPageLabelRec(fz_context* ctx, pdf_obj* node, int pageCount, Vec<PageLabelInfo>& data) {
    pdf_obj* obj;
    if ((obj = pdf_dict_gets(ctx, node, "Kids")) != nullptr && !pdf_mark_obj(ctx, node)) {
        int n = pdf_array_len(ctx, obj);
        for (int i = 0; i < n; i++) {
            auto arr = pdf_array_get(ctx, obj, i);
            BuildPageLabelRec(ctx, arr, pageCount, data);
        }
        pdf_unmark_obj(ctx, node);
        return;
    }
    obj = pdf_dict_gets(ctx, node, "Nums");
    if (obj == nullptr) {
        return;
    }
    int n = pdf_array_len(ctx, obj);
    for (int i = 0; i < n; i += 2) {
        pdf_obj* info = pdf_array_get(ctx, obj, i + 1);
        PageLabelInfo pli;
        pli.startAt = pdf_to_int(ctx, pdf_array_get(ctx, obj, i)) + 1;
        if (pli.startAt < 1) {
            continue;
        }

        pli.type = pdf_to_name(ctx, pdf_dict_gets(ctx, info, "S"));
        pli.prefix = pdf_dict_gets(ctx, info, "P");
        pli.countFrom = pdf_to_int(ctx, pdf_dict_gets(ctx, info, "St"));
        if (pli.countFrom < 1) {
            pli.countFrom = 1;
        }
        data.Append(pli);
    }
}

// TODO: maybe remove the code completely
// bugs: 3225 and 353
// not sure if we should do it, it's unexpected behavior
static bool gEnsureUniqueLabels = false;

static void EnsureLabelsUnique(StrVec* labels) {
    if (!gEnsureUniqueLabels) {
        return;
    }
    // ensure that all page labels are unique (by appending a number to duplicates)
    StrVec dups(*labels);
    Sort(&dups);
    int nDups = dups.Size();
    for (int i = 1; i < nDups; i++) {
        char* s = dups.At(i);
        if (!str::Eq(s, dups.At(i - 1))) {
            continue;
        }
        int idx = labels->Find(s), counter = 0;
        while ((idx = labels->Find(s, idx + 1)) != -1) {
            TempStr unique = nullptr;
            do {
                unique = str::FormatTemp("%s.%d", s, ++counter);
            } while (labels->Contains(unique));
            labels->SetAt(idx, unique);
        }
        nDups = dups.Size();
        for (; i + 1 < nDups && str::Eq(dups.At(i), dups.At(i + 1)); i++) {
            // no-op
        }
    }
}

static StrVec* BuildPageLabelVec(fz_context* ctx, pdf_obj* root, int pageCount) {
    Vec<PageLabelInfo> data;
    BuildPageLabelRec(ctx, root, pageCount, data);
    data.Sort(CmpPageLabelInfo);

    size_t n = data.size();
    if (n == 0) {
        return nullptr;
    }

    PageLabelInfo& pli = data.at(0);
    if (n == 1 && pli.startAt == 1 && pli.countFrom == 1 && !pli.prefix && str::Eq(pli.type, "D")) {
        // this is the default case, no need for special treatment
        return nullptr;
    }
    if (IsPerPagePrefixOnlyLabels(data, pageCount)) {
        return nullptr;
    }

    StrVec* labels = new StrVec();
    for (int i = 0; i < pageCount; i++) {
        labels->Append("");
    }

    for (size_t i = 0; i < n; i++) {
        pli = data.at(i);
        if (pli.startAt > pageCount) {
            break;
        }
        int secLen = pageCount + 1 - pli.startAt;
        if (i < n - 1 && data.at(i + 1).startAt <= pageCount) {
            secLen = data.at(i + 1).startAt - pli.startAt;
        }
        TempStr prefix = PdfToUtf8Temp(ctx, data.at(i).prefix);
        for (int j = 0; j < secLen; j++) {
            int idx = pli.startAt + j - 1;
            TempStr label = FormatPageLabelTemp(pli.type, pli.countFrom + j, prefix);
            labels->SetAt(idx, label);
        }
    }

    for (int idx = 0; (idx = labels->Find(nullptr, idx)) != -1; idx++) {
        labels->SetAt(idx, "");
    }

    EnsureLabelsUnique(labels);
    if (PageLabelsContainInternalPdgNames(labels, pageCount)) {
        delete labels;
        return nullptr;
    }
    return labels;
}
struct PageTreeStackItem {
    pdf_obj* kids = nullptr;
    int i = -1;
    int len = 0;
    int next_page_no = 0;

    PageTreeStackItem() = default;

    explicit PageTreeStackItem(fz_context* ctx, pdf_obj* kids, int next_page_no = 0) {
        this->kids = kids;
        this->len = pdf_array_len(ctx, kids);
        this->next_page_no = next_page_no;
    }
};

static void fz_lock_context_cs(void* user, int lock) {
    EngineMupdf* e = (EngineMupdf*)user;
    EnterCriticalSection(&e->fz_locks[lock]);
}

static void fz_unlock_context_cs(void* user, int lock) {
    EngineMupdf* e = (EngineMupdf*)user;
    LeaveCriticalSection(&e->fz_locks[lock]);
}

static void fz_print_cb(void* user, const char* msg) {
    static AtomicBool seenMsg = 0;
    if (str::Contains(msg, "generic error: couldn't find system font")) {
        // this floods the log in some files
        // it shows a font name like this:
        // generic error: couldn't find system font 'AngsanaUPC-Bold'
        // generic error: couldn't find system font 'AngsanaUPC'
        // we only show the first missed font. Could use StrVec() to log every
        // missing font
        if (AtomicBoolGet(&seenMsg)) {
            return;
        }
        AtomicBoolSet(&seenMsg, true);
    }
    if (!str::EndsWith(msg, "\n")) {
        msg = str::JoinTemp(msg, "\n");
    }
    log(msg);
    EngineMupdf* engine = (EngineMupdf*)user;
    if (engine) {
        engine->errors.Append(msg);
    }
}

static void InstallFitzErrorCallbacks(EngineMupdf* engine, fz_context* ctx) {
    fz_set_warning_callback(ctx, fz_print_cb, (void*)engine);
    fz_set_error_callback(ctx, fz_print_cb, (void*)engine);
}

struct ContextThreadID {
    EngineMupdf* engine = nullptr;
    fz_context* ctx = nullptr;
    DWORD threadID = 0;
};

static Vec<ContextThreadID>* gPerThreadContexts;
static CRITICAL_SECTION gPerThreadContextsCs;
static AtomicInt gEngineCount = 0;

static void InitializeEngineMupdf() {
    auto n = AtomicIntInc(&gEngineCount);
    if (n != 1) return;
    ReportIf(gPerThreadContexts);
    InitializeCriticalSection(&gPerThreadContextsCs);
    gPerThreadContexts = new Vec<ContextThreadID>();
}

static void DeInitializeEngineMupdf() {
    auto n = AtomicIntDec(&gEngineCount);
    if (n > 0) return;
    ReportIf(n < 0);
    DeleteCriticalSection(&gPerThreadContextsCs);
    delete gPerThreadContexts;
    gPerThreadContexts = nullptr;
}

fz_context* GetOrClonePerThreadContext(EngineMupdf* engine, fz_context* ctx) {
    DWORD threadID = GetCurrentThreadId();
    {
        ScopedCritSec cs(&gPerThreadContextsCs);
        for (auto& el : *gPerThreadContexts) {
            if (el.engine == engine && el.threadID == threadID) {
                return el.ctx;
            }
        }
    }
    // clone context without holding gPerThreadContextsCs to avoid deadlock
    // with threads that hold fz_locks (e.g. docLock) and then call Ctx()
    // safe because only current thread can create a context for its own threadID
    auto newCtx = fz_clone_context(ctx);
    {
        ScopedCritSec cs(&gPerThreadContextsCs);
        ContextThreadID el{engine, newCtx, threadID};
        gPerThreadContexts->Append(el);
    }
    return newCtx;
}

void ReleasePerThreadContext(EngineMupdf* engine) {
    DWORD threadID = GetCurrentThreadId();
    fz_context* ctxToDrop = nullptr;
    {
        ScopedCritSec cs(&gPerThreadContextsCs);
        auto n = gPerThreadContexts->Size();
        for (int i = 0; i < n; i++) {
            auto& el = gPerThreadContexts->at(i);
            if (el.engine == engine && el.threadID == threadID) {
                ctxToDrop = el.ctx;
                gPerThreadContexts->RemoveAtFast(i);
                break;
            }
        }
    }
    if (ctxToDrop) {
        fz_drop_context(ctxToDrop);
    }
}

static void ReleaseAllPerThreadContexts(EngineMupdf* engine) {
    Vec<fz_context*> ctxsToDrop;
    Vec<DWORD> tids;
    {
        ScopedCritSec cs(&gPerThreadContextsCs);
        for (int i = (int)gPerThreadContexts->Size() - 1; i >= 0; i--) {
            auto& el = gPerThreadContexts->at(i);
            if (el.engine == engine) {
                ctxsToDrop.Append(el.ctx);
                tids.Append(el.threadID);
                gPerThreadContexts->RemoveAtFast(i);
            }
        }
    }
    DWORD cur = GetCurrentThreadId();
    for (int i = 0; i < (int)ctxsToDrop.size(); i++) {
        fz_context* ctx = ctxsToDrop[i];
        int waited = 0;
        while (ctx && ctx->error.top != ctx->error.stack_base && waited < 120000) {
            Sleep(1);
            waited++;
        }
        if (ctx && ctx->error.top != ctx->error.stack_base) {
            continue;
        }
        fz_drop_context(ctx);
    }
}

EngineMupdf::EngineMupdf() {
    InitializeEngineMupdf();
    kind = kindEngineMupdf;
    defaultExt = str::Dup(".pdf");
    fileDPI = 72.0f;

    // pages Vec + its FzPageInfo elements live for the lifetime of the
    // engine, so bump-allocate them out of EngineBase::arena
    pages.allocator = arena;

    for (size_t i = 0; i < dimof(fz_locks); i++) {
        InitializeCriticalSection(&fz_locks[i]);
    }
    InitializeCriticalSection(&pagesLock);
    InitializeCriticalSection(&renderLock);
    InitializeCriticalSection(&docLock);
    InitializeCriticalSection(&reflowCountLock);
    InitializeCriticalSection(&tocBuildLock);
    InitializeCriticalSection(&pendingReflowNavLock);

    fz_locks_ctx.user = this;
    fz_locks_ctx.lock = fz_lock_context_cs;
    fz_locks_ctx.unlock = fz_unlock_context_cs;
    // 4x mupdf's default (256 MB). Laid-out EPUB chapter HTML lives in this
    // store; anthology EPUBs put an entire book in one spine chapter, and
    // evicting it means a multi-second re-layout on the next page render.
    // Trade memory for time (freed when the document closes).
    constexpr size_t kFzStoreBudget = (size_t)1 << 30; // 1 GB
    _ctx = fz_new_context(nullptr, &fz_locks_ctx, kFzStoreBudget);
    InstallFitzErrorCallbacks(this, _ctx);

    install_load_windows_font_funcs(_ctx);
    fz_register_document_handlers(_ctx);

    darkModeEngineCache = PdfDarkModeEngineCacheCreate();
}

fz_context* EngineMupdf::Ctx() const {
    return GetOrClonePerThreadContext(const_cast<EngineMupdf*>(this), _ctx);
}

EngineMupdf::~EngineMupdf() {
    InterlockedExchange(&reflowableLoadAbort, 1);
    for (int i = 0; i < 120000; i++) {
        if (InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) == 0) {
            break;
        }
        Sleep(1);
    }
    for (int i = 0; i < 120000; i++) {
        if (InterlockedCompareExchange(&reflowWarmActive, 0, 0) == 0) {
            break;
        }
        Sleep(1);
    }
    for (int i = 0; i < 120000; i++) {
        if (InterlockedCompareExchange(&reflowFragmentsAsyncActive, 0, 0) == 0 &&
            InterlockedCompareExchange(&reflowNavCountAsyncActive, 0, 0) == 0) {
            break;
        }
        Sleep(1);
    }

    EnterCriticalSection(&pagesLock);

    if (darkModeEngineCache) {
        PdfDarkModeEngineCacheFree(_ctx, darkModeEngineCache);
        darkModeEngineCache = nullptr;
    }

    ReleaseAllPerThreadContexts(this);
    auto ctx = _ctx;
    for (FzPageInfo* pi : pages) {
        DeleteVecMembers(pi->links);
        DeleteVecMembers(pi->autoLinks);
        DeleteVecMembers(pi->comments);
        for (FitzPageImageInfo* img : pi->images) {
            if (img && img->image) {
                fz_drop_image(ctx, img->image);
                img->image = nullptr;
            }
        }
        DeleteVecMembers(pi->images);
        if (pi->retainedLinks) {
            fz_drop_link(ctx, pi->retainedLinks);
        }
        if (pi->displayList) {
            fz_drop_display_list(ctx, pi->displayList);
        }
        DropFollowThemePageBitmapCache(pi);
        if (pi->searchStext) {
            fz_drop_stext_page(ctx, pi->searchStext);
        }
        PdfDarkModeInvalidatePage(ctx, pi);
        if (pi->page) {
            fz_drop_page(ctx, pi->page);
        }
        pi->~FzPageInfo();
    }

    for (fz_outline* ol : retiredOutlines) {
        fz_drop_outline(ctx, ol);
    }
    retiredOutlines.Reset();
    fz_drop_outline(ctx, outline);
    fz_drop_outline(ctx, attachments);

    if (pdfInfo) {
        pdf_drop_obj(ctx, pdfInfo);
    }

    if (pdfdoc) {
        pdf_drop_page_tree(ctx, pdfdoc);
    }

    fz_drop_document(ctx, _doc);
    fz_purge_glyph_cache(ctx);
    fz_drop_context(ctx);

    str::Free(pdfPassword);
    delete pageLabels;
    delete tocTree;
    reflowHtmlSource.Free();
    {
        ScopedCritSec scope(&pendingReflowNavLock);
        str::Free(pendingReflowNavUri);
        pendingReflowNavUri = nullptr;
        pendingReflowNavChapter = -1;
    }

    for (size_t i = 0; i < dimof(fz_locks); i++) {
        DeleteCriticalSection(&fz_locks[i]);
    }
    LeaveCriticalSection(&pagesLock);
    DeleteCriticalSection(&pagesLock);
    DeleteCriticalSection(&renderLock);
    DeleteCriticalSection(&docLock);
    DeleteCriticalSection(&reflowCountLock);
    DeleteCriticalSection(&tocBuildLock);
    DeleteCriticalSection(&pendingReflowNavLock);

    DeInitializeEngineMupdf();
}

class PasswordCloner : public PasswordUI {
    u8* cryptKey = nullptr;

  public:
    explicit PasswordCloner(u8* cryptKey) { this->cryptKey = cryptKey; }

    char* GetPassword(const char*, u8*, u8 decryptionKeyOut[32], bool* saveKey) override {
        memcpy(decryptionKeyOut, cryptKey, 32);
        *saveKey = true;
        return nullptr;
    }
};

EngineBase* EngineMupdf::Clone() {
    ScopedCritSec scope(&docLock);
    if (!FilePath()) {
        // before port we could clone streams but it's no longer possible
        logf("EngineMupdf::Clone() failed: no file path\n");
        return nullptr;
    }
    auto ctx = Ctx();
    // use this document's encryption key (if any) to load the clone
    PasswordCloner* pwdUI = nullptr;
    if (pdfdoc) {
        if (pdf_crypt_key(ctx, pdfdoc->crypt)) {
            pwdUI = new PasswordCloner(pdf_crypt_key(ctx, pdfdoc->crypt));
        }
    }

    EngineMupdf* clone = new EngineMupdf();
    bool ok = clone->Load(FilePath(), pwdUI);
    if (!ok) {
        logf("EngineMupdf::Clone() failed: Load('%s') failed\n", FilePath());
        delete clone;
        delete pwdUI;
        return nullptr;
    }
    delete pwdUI;

    clone->disableAntiAlias = disableAntiAlias;
    clone->cadEnhanceOverride = cadEnhanceOverride;
    clone->cadDetectEnable = cadDetectEnable;
    clone->cadDetectScore = cadDetectScore;
    clone->cadRasterDominant = cadRasterDominant;
    clone->cadHairlineVector = cadHairlineVector;
    clone->cadDetectDone = cadDetectDone;

    if (!decryptionKey.s && pdfdoc && pdfdoc->crypt) {
        clone->decryptionKey = Str();
    }

    return clone;
}

// File names ending in :<digits> are interpreted as containing
// embedded PDF documents (the digits is stream number of the embedded file stream)
// the caller must free()
const char* ParseEmbeddedStreamNumber(const char* path, int* streamNoOut) {
    int streamNo = -1;
    char* path2 = str::Dup(path);
    char* streamNoStr = (char*)FindEmbeddedPdfFileStreamNo(path2);
    if (streamNoStr) {
        char* rest = (char*)str::Parse(streamNoStr, ":%d", &streamNo);
        // there shouldn't be any left unparsed data
        ReportIf(!rest);
        if (!rest) {
            streamNo = -1;
        }
        // replace ':' with 0 to create a filesystem path
        *streamNoStr = 0;
    }
    *streamNoOut = streamNo;
    return path2;
}

ByteSlice EngineMupdf::LoadStreamFromPDFFile(const char* filePath) {
    auto ctx = Ctx();
    int streamNo = -1;
    AutoFreeStr fnCopy = ParseEmbeddedStreamNumber(filePath, &streamNo);
    if (streamNo < 0) {
        return {};
    }

    char* path = fnCopy.Get();
    bool ok = Load(path, nullptr);
    if (!ok) {
        return {};
    }

    if (!pdf_obj_num_is_stream(ctx, pdfdoc, streamNo)) {
        return {};
    }

    fz_buffer* buffer = nullptr;
    fz_var(buffer);
    fz_try(ctx) {
        buffer = pdf_load_stream_number(ctx, pdfdoc, streamNo);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return {};
    }
    auto dataSize = buffer->len;
    if (dataSize == 0) {
        return {};
    }
    auto data = (u8*)memdup(buffer->data, dataSize);
    fz_drop_buffer(ctx, buffer);

    return {data, dataSize};
}

// <filePath> should end with embed marks, which is a stream number
// inside pdf file
ByteSlice LoadEmbeddedPDFFile(const char* filePath) {
    EngineMupdf* engine = new EngineMupdf();
    auto res = engine->LoadStreamFromPDFFile(filePath);
    SafeEngineRelease(&engine);
    return res;
}

static ByteSlice TxtFileToHTML(const char* path) {
    ByteSlice fd = file::ReadFileWithAllocator(path, GetTempAllocator());
    if (fd.empty()) {
        return {};
    }

    InterlockedIncrement(&gAllowAllocFailure);
    defer {
        InterlockedDecrement(&gAllowAllocFailure);
    };

    TempStr data = strconv::UnknownToUtf8Temp((const char*)fd.data(), fd.size());
    if (!data) {
        return {};
    }
    data = str::ReplaceTemp(data, "&", "&amp;");
    data = str::ReplaceTemp(data, ">", "&gt;");
    if (!data) {
        return {};
    }
    data = str::ReplaceTemp(data, "<", "&lt;");
    if (!data) {
        return {};
    }

    StrBuilder d;
    d.Append(R"(<html>
    <head>
<style>
    html, body {
        background-color: #f7f3e8;
        color: #333333;
    }
    pre {
        white-space: pre-wrap;
    }
</style>
    </head>
<body>
    <pre>)");
    bool ok = d.Append(data);
    if (!ok) {
        return {};
    }
    d.Append(R"(</pre>
</body>
</html>)");
    size_t sz = d.size();
    return {(u8*)d.StealData(), sz};
}

static ByteSlice PalmDocToHTML(const char* path) {
    auto doc = PalmDoc::CreateFromFile(path);
    if (!doc) {
        return {};
    }
    ByteSlice html = doc->GetHtmlData();
    return html.Clone();
}

static void KeepReflowHtmlSource(EngineMupdf* e, ByteSlice& html);

static bool IsUnsupportedOfficeFilePath(const char* path) {
    TempStr ext = path::GetExtTemp(path);
    if (str::IsEmpty(ext)) {
        return false;
    }
    // MuPDF's office handler only converts OOXML (docx/xlsx/pptx/hwpx) to HTML.
    // Classic OLE .doc/.xls/.ppt and WPS are not supported.
    return str::EqI(ext, ".doc") || str::EqI(ext, ".wps") || str::EqI(ext, ".xls") || str::EqI(ext, ".ppt");
}

bool EngineMupdf::Load(const char* path, PasswordUI* pwdUI) {
    bool ok;
    const char* pathA = path;
    auto ctx = Ctx();
    ReportIf(FilePath() || _doc || !ctx);
    epubOpenStartTick = GetTickCount();
    SetFilePath(path);

    if (IsUnsupportedOfficeFilePath(pathA)) {
        return false;
    }

    auto ext = path::GetExtTemp(path);
    str::ReplaceWithCopy(&defaultExt, ext);

    int streamNo = -1;
    AutoFreeStr fnCopy = ParseEmbeddedStreamNumber(pathA, &streamNo);

    Kind kind = GuessFileTypeFromName(pathA);
    // show .txt, .xml and other text files as plain text
    // using html engine
    if (kind == kindFileTxt) {
        // synthesize a .html file from text file
        ByteSlice d = TxtFileToHTML(path);
        if (d.empty()) {
            return false;
        }
        KeepReflowHtmlSource(this, d);
        fz_buffer* buf =
            fz_new_buffer_from_copied_data(ctx, (const u8*)reflowHtmlSource.data(), reflowHtmlSource.size());
        fz_stream* file = fz_open_buffer(ctx, buf);
        fz_drop_buffer(ctx, buf);
        char* nameHint = str::JoinTemp(path, ".html");
        if (!LoadFromStream(file, nameHint, pwdUI)) {
            return false;
        }
        return FinishLoading();
    }

    if (kind == kindFileMd) {
        ByteSlice d = MdFileToHTML(path);
        if (d.empty()) {
            return false;
        }
        KeepReflowHtmlSource(this, d);
        fz_buffer* buf =
            fz_new_buffer_from_copied_data(ctx, (const u8*)reflowHtmlSource.data(), reflowHtmlSource.size());
        fz_stream* file = fz_open_buffer(ctx, buf);
        fz_drop_buffer(ctx, buf);
        char* nameHint = str::JoinTemp(path, ".html");
        if (!LoadFromStream(file, nameHint, pwdUI)) {
            return false;
        }
        return FinishLoading();
    }

    if (str::EqI(ext, ".pdb")) {
        // synthesize a .html file from pdb file
        ByteSlice d = PalmDocToHTML(pathA);
        if (d.empty()) {
            return false;
        }
        fz_buffer* buf = fz_new_buffer_from_copied_data(ctx, (const u8*)d.data(), d.size());
        fz_stream* file = fz_open_buffer(ctx, buf);
        fz_drop_buffer(ctx, buf);
        str::Free(d);
        TempStr nameHint = str::JoinTemp(path, ".html");
        if (!LoadFromStream(file, nameHint, pwdUI)) {
            return false;
        }
        return FinishLoading();
    }

    fz_stream* file = FzOpenOrReadFile(ctx, fnCopy);
    ok = LoadFromStream(file, FilePath(), pwdUI);
    if (!ok) {
        return false;
    }

    if (streamNo < 0) {
        ok = FinishLoading();
        if (ok) {
            return true;
        }
        fz_drop_document(ctx, _doc);
        _doc = nullptr;
        file = FzReadMaybeFixPDF(ctx, FilePath());
        if (!file) {
            return false;
        }
        ok = LoadFromStream(file, FilePath(), pwdUI);
        if (!ok) {
            return false;
        }
        return FinishLoading();
    }

    // load a stream from inside a pdf document
    pdfdoc = pdf_specifics(ctx, _doc);
    if (pdfdoc) {
        if (!pdf_obj_num_is_stream(ctx, pdfdoc, streamNo)) {
            return false;
        }

        fz_buffer* buffer = nullptr;
        fz_var(buffer);
        fz_try(ctx) {
            buffer = pdf_load_stream_number(ctx, pdfdoc, streamNo);
            file = fz_open_buffer(ctx, buffer);
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buffer);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            return false;
        }
    }

    fz_drop_document(ctx, _doc);
    _doc = nullptr;

    if (!LoadFromStream(file, FilePath(), pwdUI)) {
        return false;
    }

    return FinishLoading();
}

// TODO: need to do stuff to support .txt etc.
bool EngineMupdf::Load(IStream* stream, const char* nameHint, PasswordUI* pwdUI) {
    auto ctx = Ctx();
    ReportIf(FilePath() || _doc || !ctx);
    if (!ctx) {
        return false;
    }
    if (IsUnsupportedOfficeFilePath(nameHint)) {
        return false;
    }

    fz_stream* stm = nullptr;
    fz_var(stm);
    fz_try(ctx) {
        stm = FzOpenIStream(ctx, stream);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        stm = nullptr;
    }
    if (!stm) {
        return false;
    }
    if (!LoadFromStream(stm, nameHint, pwdUI)) {
        return false;
    }
    return FinishLoading();
}

// is implemented in SumatraPDF.exe, PdfFilter and PdfPreview
// TODO: allow setting per
extern EBookUI* GetEBookUI();

static bool IsCjkLangTag(const char* lang) {
    if (str::IsEmpty(lang)) {
        return false;
    }
    return str::StartsWith(lang, "zh") || str::StartsWith(lang, "ja") || str::StartsWith(lang, "ko") ||
           str::EqI(lang, "chi") || str::EqI(lang, "zho");
}

static bool IsLatinLangTag(const char* lang) {
    if (str::IsEmpty(lang)) {
        return false;
    }
    return str::StartsWith(lang, "en") || str::StartsWith(lang, "de") || str::StartsWith(lang, "fr") ||
           str::StartsWith(lang, "es") || str::StartsWith(lang, "it") || str::StartsWith(lang, "pt") ||
           str::StartsWith(lang, "nl") || str::StartsWith(lang, "sv") || str::StartsWith(lang, "ru");
}

static TempStr ExtractNextLanguageFromOpf(const char** cursor, const char* xmlEnd) {
    const char* markers[] = {"<dc:language", "<opf:language", "<language"};
    const char* best = nullptr;
    size_t bestOff = (size_t)-1;
    for (const char* marker : markers) {
        const char* p = *cursor;
        if (p < xmlEnd && (p = str::Find(p, marker))) {
            if (!best || (size_t)(p - *cursor) < bestOff) {
                best = p;
                bestOff = (size_t)(p - *cursor);
            }
        }
    }
    if (!best) {
        return nullptr;
    }
    const char* gt = (const char*)memchr(best, '>', xmlEnd - best);
    if (!gt) {
        return nullptr;
    }
    const char* val = gt + 1;
    const char* lt = (const char*)memchr(val, '<', xmlEnd - val);
    if (!lt || lt <= val) {
        return nullptr;
    }
    while (val < lt && str::IsWs(*val)) {
        val++;
    }
    const char* valEnd = lt;
    while (valEnd > val && str::IsWs(*(valEnd - 1))) {
        valEnd--;
    }
    if (valEnd <= val) {
        return nullptr;
    }
    *cursor = lt;
    return str::DupTemp(val, valEnd - val);
}

static EbookTypographyKind ClassifyOpfLanguages(const char* xml, size_t xmlLen) {
    bool hasCjk = false;
    bool hasLatin = false;
    const char* cursor = xml;
    const char* xmlEnd = xml + xmlLen;
    for (;;) {
        TempStr lang = ExtractNextLanguageFromOpf(&cursor, xmlEnd);
        if (!lang) {
            break;
        }
        if (IsCjkLangTag(lang)) {
            hasCjk = true;
        }
        if (IsLatinLangTag(lang)) {
            hasLatin = true;
        }
    }
    if (hasCjk && hasLatin) {
        return EbookTypographyKind::Bilingual;
    }
    if (hasCjk) {
        return EbookTypographyKind::Cjk;
    }
    if (hasLatin) {
        return EbookTypographyKind::Latin;
    }
    return EbookTypographyKind::Cjk;
}

static TempStr GetEpubOpfPath(MultiFormatArchive* arch) {
    auto* fi = arch->GetFileDataByName("META-INF/container.xml");
    if (!fi || !fi->data) {
        return nullptr;
    }
    const char* xml = fi->data;
    const char* key = "full-path=\"";
    char quote = '"';
    const char* p = strstr(xml, key);
    if (!p) {
        key = "full-path='";
        p = strstr(xml, key);
        quote = '\'';
    }
    if (!p) {
        return nullptr;
    }
    p += strlen(key);
    const char* end = strchr(p, quote);
    if (!end) {
        return nullptr;
    }
    TempStr path = str::DupTemp(p, end - p);
    url::DecodeInPlace(path);
    return path;
}

static void Utf8CountLetters(const char* s, size_t len, int* cjkOut, int* latinOut) {
    int cjk = 0;
    int latin = 0;
    if (!s || len == 0) {
        *cjkOut = 0;
        *latinOut = 0;
        return;
    }
    size_t i = 0;
    while (i < len && s[i]) {
        int rune = 0;
        int n = fz_chartorune(&rune, s + i);
        if (n <= 0) {
            break;
        }
        if ((rune >= 0x4E00 && rune <= 0x9FFF) || (rune >= 0x3400 && rune <= 0x4DBF)) {
            cjk++;
        } else if ((rune >= 'A' && rune <= 'Z') || (rune >= 'a' && rune <= 'z')) {
            latin++;
        }
        i += (size_t)n;
        if (cjk + latin >= 400) {
            break;
        }
    }
    *cjkOut = cjk;
    *latinOut = latin;
}

static bool PathHintsBilingual(const char* path) {
    if (str::IsEmpty(path)) {
        return false;
    }
    static const char* hints[] = {
        "\xe5\x8f\x8c\xe8\xaf\xad", // 双语
        "\xe8\x8b\xb1\xe6\xb1\x89", // 英汉
        "\xe6\xb1\x89\xe8\x8b\xb1", // 汉英
        "\xe4\xb8\xad\xe8\x8b\xb1", // 中英
        "\xe5\xaf\xb9\xe7\x85\xa7", // 对照
        "\xe4\xb9\xa6\xe8\x99\xab", // 书虫 (Oxford Bookworms bilingual editions)
        "bilingual",
        "Bilingual",
        "bookworm",
        "Bookworm",
        nullptr,
    };
    for (const char** h = hints; *h; h++) {
        if (str::Find(path, *h)) {
            return true;
        }
    }
    return false;
}

static EbookTypographyKind NormalizeTypographyKind(EbookTypographyKind kind) {
    return kind;
}

static EbookTypographyKind ClassifyLetterCounts(int cjk, int latin, EbookTypographyKind opfHint) {
    if (cjk >= 12 && latin >= 12 && cjk <= latin * 6 && latin <= cjk * 6) {
        return EbookTypographyKind::Bilingual;
    }
    if (opfHint == EbookTypographyKind::Latin) {
        // OPF says English; ignore short CJK runs in nav/TOC (common in Oxford volumes).
        if (cjk >= 12 && latin >= 12) {
            return EbookTypographyKind::Bilingual;
        }
        return EbookTypographyKind::Latin;
    }
    if (cjk >= 8 && cjk * 2 >= latin) {
        return EbookTypographyKind::Cjk;
    }
    if (latin >= 40 && latin > cjk * 3) {
        return EbookTypographyKind::Latin;
    }
    return cjk >= latin ? EbookTypographyKind::Cjk : EbookTypographyKind::Latin;
}

static EbookTypographyKind EpubArchiveDetectTypography(const char* path) {
    ArchiveExtractProgressCb emptyCb;
    MultiFormatArchive* arch = OpenArchiveFromFile(path, ArchiveLoadMode::Lazy, emptyCb);
    if (!arch) {
        return EbookTypographyKind::Cjk;
    }
    AutoDelete<MultiFormatArchive> scoped(arch);

    if (PathHintsBilingual(path)) {
        return EbookTypographyKind::Bilingual;
    }

    EbookTypographyKind opfHint = EbookTypographyKind::Cjk;
    TempStr opfPath = GetEpubOpfPath(arch);
    if (opfPath) {
        auto* opfFi = arch->GetFileDataByName(opfPath);
        if (opfFi && opfFi->data) {
            opfHint = ClassifyOpfLanguages(opfFi->data, opfFi->fileSizeUncompressed);
            if (opfHint == EbookTypographyKind::Bilingual) {
                return EbookTypographyKind::Bilingual;
            }
            if (opfHint == EbookTypographyKind::Cjk) {
                return EbookTypographyKind::Cjk;
            }
        }
    }

    for (auto* fi : arch->GetFileInfos()) {
        if (!fi || !fi->name || fi->isDir) {
            continue;
        }
        if (!str::EndsWithI(fi->name, ".html") && !str::EndsWithI(fi->name, ".xhtml") &&
            !str::EndsWithI(fi->name, ".htm")) {
            continue;
        }
        if (str::StartsWith(fi->name, "META-INF/")) {
            continue;
        }
        auto* htmlFi = arch->GetFileDataById(fi->fileId);
        if (!htmlFi || !htmlFi->data || htmlFi->fileSizeUncompressed < 64) {
            continue;
        }
        size_t n = htmlFi->fileSizeUncompressed;
        if (n > 8192) {
            n = 8192;
        }
        int cjk = 0;
        int latin = 0;
        Utf8CountLetters(htmlFi->data, n, &cjk, &latin);
        return ClassifyLetterCounts(cjk, latin, opfHint);
    }

    return opfHint == EbookTypographyKind::Cjk ? EbookTypographyKind::Cjk : EbookTypographyKind::Latin;
}

EbookTypographyKind DetectEbookTypographyKind(const char* filePath, const char* nameHint) {
    const char* path = filePath;
    if (str::IsEmpty(path)) {
        path = nameHint;
    }
    if (str::IsEmpty(path)) {
        return EbookTypographyKind::Latin;
    }

    if (PathHintsBilingual(path)) {
        return NormalizeTypographyKind(EbookTypographyKind::Bilingual);
    }

    if (str::EndsWithI(path, ".epub")) {
        return NormalizeTypographyKind(EpubArchiveDetectTypography(path));
    }

    int cjk = 0;
    int latin = 0;
    Utf8CountLetters(path, str::Len(path), &cjk, &latin);
    if (cjk + latin >= 4) {
        return NormalizeTypographyKind(ClassifyLetterCounts(cjk, latin, EbookTypographyKind::Cjk));
    }

    if (file::Exists(path)) {
        ByteSlice data = file::ReadFile(path);
        if (!data.empty()) {
            size_t n = data.size();
            if (n > 8192) {
                n = 8192;
            }
            Utf8CountLetters((const char*)data.data(), n, &cjk, &latin);
            data.Free();
            return NormalizeTypographyKind(ClassifyLetterCounts(cjk, latin, EbookTypographyKind::Cjk));
        }
    }

    return EbookTypographyKind::Latin;
}

bool EbookNeedsCjkTypography(const char* filePath, const char* nameHint) {
    return DetectEbookTypographyKind(filePath, nameHint) == EbookTypographyKind::Cjk;
}

// stm is either freed or retained via _doc
static void GetMupdfReflowLayoutPt(const char* nameHint, float* ldxPtOut, float* ldyPtOut, float* lfontDyPtOut);

static bool IsMarkdownReflowDocument(const char* nameHint, const char* filePath) {
    auto isMdPath = [](const char* path) -> bool {
        if (str::IsEmpty(path)) {
            return false;
        }
        TempStr ext = path::GetExtTemp(path);
        if (str::EqI(ext, ".md") || str::EqI(ext, ".markdown")) {
            return true;
        }
        return GuessFileTypeFromName(path) == kindFileMd;
    };
    if (isMdPath(filePath)) {
        return true;
    }
    if (!str::IsEmpty(nameHint)) {
        if (str::EndsWithI(nameHint, ".md.html") || str::EndsWithI(nameHint, ".markdown.html")) {
            return true;
        }
        return isMdPath(nameHint);
    }
    return false;
}

static bool IsEpubReflowNameHint(const char* nameHint) {
    return str::EndsWithI(nameHint, ".epub") || str::EndsWithI(nameHint, ".epub.html");
}

// Palette only: background, text, and link colors. No margins, line-height, page-break, or
// width/height rules — theme and PdfDocumentColorMode must not change pagination.
static TempStr BuildMupdfReflowPaletteCss(bool isEpub) {
    bool darkTheme = IsDarkThemeSelected();
    PdfDocumentColorMode docMode = GetPdfDocumentColorMode();
    bool injectThemeColors = docMode != PdfDocumentColorMode::Light;
    bool reflowThemeBitmapRecolor = ReflowEbookUsesThemeBitmapRecolor();
    bool useEyeCareLightCss =
        injectThemeColors && !darkTheme && !ThemeUsesOriginalPageColors() && !reflowThemeBitmapRecolor;
    bool useDarkCss = injectThemeColors && darkTheme && !reflowThemeBitmapRecolor;

    if (useDarkCss) {
        return BuildEbookDarkCss(isEpub);
    }
    if (useEyeCareLightCss) {
        static const char* kLightEyeCareCss = R"(html {
  background-color: #f7f3e8 !important;
}
body {
  background-color: #f7f3e8 !important;
  color: #333333;
}
pre {
  background-color: #f7f3e8 !important;
  color: #333333;
}
)";
        static const char* kEpubReaderLightCss = R"(html {
  background-color: #f7f3e8 !important;
  color: #333333 !important;
}
body, p, span, blockquote, li, td, th, div,
section, article, main, header, footer, pre, table,
.calibre,
.calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,
.calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, .calibre_10,
.calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, .calibre_19, .calibre_20 {
  background-color: transparent !important;
  color: #333333 !important;
}
.noindent-bodycontent-1-fangsong,
.bodycontent-1-fangsong,
.bodycontent-2-fangsong,
.bodycontent-1-fangsong-top,
.noindent-bodycontent-1-fangsong-top,
.bodycontent-1-top,
.noindent-bodycontent-1-top,
.bodycontent-1-fangsong-top1,
.bodycontent-2-fangsong-top,
.hang-bodycontent-1-fangsong,
.noindent-bodycontent,
.bodycontent,
.songti,
span.songti {
  background-color: transparent !important;
  color: #333333 !important;
}
p {
  color: #333333 !important;
}
h1, h2, h3, h4, h5, h6,
.title, .chapter, .chapter-title, .sgc-toc-title {
  color: #3f3a34 !important;
}
body, pre, table, td, th {
  background-color: #f7f3e8 !important;
}
a, a:link, a:visited, a:hover, a:active,
p a, sup a, li a {
  color: #315f9c !important;
}
figcaption, caption, p.caption, div.caption, span.caption,
.figcaption, .figure-caption, .image-caption, .picture-caption, .pic-caption, .caption {
  color: #68625a !important;
}
)";
        return str::DupTemp(isEpub ? kEpubReaderLightCss : kLightEyeCareCss);
    }
    return nullptr;
}

// MuPDF has no max-height; comic panels use explicit height in pt — same for cnepub alone* figures.
static TempStr BuildMupdfReflowEpubImageFitCss(float ldyPt) {
    float panelH = ldyPt - 16.f;
    if (panelH < 400.f) {
        panelH = 400.f;
    }
    float h90 = panelH * 0.88f;
    float h80 = panelH * 0.78f;
    float h70 = panelH * 0.68f;
    float h60 = panelH * 0.58f;
    float h50 = panelH * 0.48f;
    float h40 = panelH * 0.38f;
    return str::FormatTemp(
        R"(/* Sumatra: cnepub figures — one reflow page, explicit height (MuPDF ignores max-height). */
img.kindle-cn-bodycontent-image-alone100 {
  display: block !important;
  page-break-before: always !important;
  width: auto !important;
  max-width: 90%% !important;
  height: %.0fpt !important;
  margin: 0.8em auto !important;
}
img.kindle-cn-bodycontent-image-alone80,
img.kindle-cn-bodycontent-image-alone80-1 {
  display: block !important;
  page-break-before: always !important;
  width: auto !important;
  max-width: 72%% !important;
  height: %.0fpt !important;
  margin: 0.8em auto !important;
}
img.kindle-cn-bodycontent-image-alone70 {
  display: block !important;
  page-break-before: always !important;
  width: auto !important;
  max-width: 63%% !important;
  height: %.0fpt !important;
  margin: 0.8em auto !important;
}
img.kindle-cn-bodycontent-image-alone60 {
  display: block !important;
  page-break-before: always !important;
  width: auto !important;
  max-width: 54%% !important;
  height: %.0fpt !important;
  margin: 0.8em auto !important;
}
img.kindle-cn-bodycontent-image-alone50 {
  display: block !important;
  page-break-before: always !important;
  width: auto !important;
  max-width: 50%% !important;
  height: %.0fpt !important;
  margin: 0.8em auto !important;
}
img.kindle-cn-bodycontent-image-alone40,
img.kindle-cn-bodycontent-image-alone45 {
  display: block !important;
  page-break-before: always !important;
  width: auto !important;
  max-width: 45%% !important;
  height: %.0fpt !important;
  margin: 0.8em auto !important;
}
/* With-note: publisher %% width + auto height; page-break on large figures. */
img.kindle-cn-bodycontent-image-alone100-withnote,
img.kindle-cn-bodycontent-image-alone80-withnote,
img.kindle-cn-bodycontent-image-alone70-withnote,
img.kindle-cn-bodycontent-image-alone60-withnote,
img.kindle-cn-bodycontent-image-alone50-withnote,
img.kindle-cn-bodycontent-image-alone45-withnote,
img.kindle-cn-bodycontent-image-alone40-withnote {
  display: block !important;
  page-break-before: always !important;
  vertical-align: top !important;
  height: auto !important;
  margin: 0 auto 0 auto !important;
}
img.kindle-cn-bodycontent-image-alone30-withnote,
img.kindle-cn-bodycontent-image-alone20-withnote {
  display: block !important;
  page-break-before: auto !important;
  height: auto !important;
  margin: 0.4em auto 0 auto !important;
}
img.kindle-cn-bodycontent-image-alone100-withnote {
  max-width: 90%% !important;
}
img.kindle-cn-bodycontent-image-alone80-withnote {
  max-width: 72%% !important;
}
img.kindle-cn-bodycontent-image-alone70-withnote {
  max-width: 63%% !important;
}
img.kindle-cn-bodycontent-image-alone60-withnote {
  max-width: 54%% !important;
}
img.kindle-cn-bodycontent-image-alone50-withnote {
  max-width: 50%% !important;
}
img.kindle-cn-bodycontent-image-alone45-withnote,
img.kindle-cn-bodycontent-image-alone40-withnote {
  max-width: 45%% !important;
}
img.kindle-cn-bodycontent-image-alone30-withnote {
  max-width: 27%% !important;
}
img.kindle-cn-bodycontent-image-alone20-withnote {
  max-width: 18%% !important;
}
div.kindle-cn-bodycontent-div-alone100,
div.kindle-cn-bodycontent-div-alone100a {
  page-break-before: always !important;
  page-break-inside: avoid !important;
  margin: 0.4em auto !important;
}
div.kindle-cn-frame-zhijiao div.kindle-cn-bodycontent-div-alone100,
div.kindle-cn-frame-zhijiao div.kindle-cn-bodycontent-div-alone100a,
div.kindle-cn-frame-yuanjiao div.kindle-cn-bodycontent-div-alone100,
div.kindle-cn-frame-yuanjiao div.kindle-cn-bodycontent-div-alone100a {
  page-break-before: always !important;
}
h1 + div.kindle-cn-bodycontent-div-alone100,
h2 + div.kindle-cn-bodycontent-div-alone100,
h3 + div.kindle-cn-bodycontent-div-alone100,
h4 + div.kindle-cn-bodycontent-div-alone100,
h5 + div.kindle-cn-bodycontent-div-alone100,
h6 + div.kindle-cn-bodycontent-div-alone100,
h1 + div.kindle-cn-bodycontent-div-alone100a,
h2 + div.kindle-cn-bodycontent-div-alone100a,
h3 + div.kindle-cn-bodycontent-div-alone100a,
h4 + div.kindle-cn-bodycontent-div-alone100a,
h5 + div.kindle-cn-bodycontent-div-alone100a,
h6 + div.kindle-cn-bodycontent-div-alone100a {
  margin-top: 0.2em !important;
}
div.kindle-cn-bodycontent-div-alone100 p.kindle-cn-picture-txt-withmanycharactors,
div.kindle-cn-bodycontent-div-alone100 p.kindle-cn-picture-txt-withfewcharactors,
div.kindle-cn-bodycontent-div-alone100a p.kindle-cn-picture-txt-withmanycharactors,
div.kindle-cn-bodycontent-div-alone100a p.kindle-cn-picture-txt-withfewcharactors,
div.kindle-cn-bodycontent-div-alone100 + p.kindle-cn-picture-txt-withmanycharactors,
div.kindle-cn-bodycontent-div-alone100 + p.kindle-cn-picture-txt-withfewcharactors,
div.kindle-cn-bodycontent-div-alone100a + p.kindle-cn-picture-txt-withmanycharactors,
div.kindle-cn-bodycontent-div-alone100a + p.kindle-cn-picture-txt-withfewcharactors {
  margin-top: 0 !important;
  margin-bottom: 0.3em !important;
}
.image_full, .image_full_caption, .image_full_landscape, .image_full_caption_landscape {
  page-break-before: always !important;
}
.image_full img, .image_full_caption img, .image_full_landscape img, .image_full_caption_landscape img {
  display: block !important;
  width: auto !important;
  max-width: 100%% !important;
  height: %.0fpt !important;
  margin-left: auto !important;
  margin-right: auto !important;
}
figure, div.figure, div.image, div.picture, div.pic {
  page-break-before: always !important;
}
)",
        h90, h80, h70, h60, h50, h40, panelH);
}

static TempStr BuildMarkdownTableCss(bool darkTheme, bool eyeCareLight) {
    if (darkTheme) {
        return str::DupTemp(R"(table {
  background-color: transparent !important;
}
thead th {
  background-color: #21262d !important;
  color: #e6edf3 !important;
  font-weight: 600 !important;
}
tbody td {
  background-color: transparent !important;
}
)");
    }
    if (eyeCareLight) {
        return str::DupTemp(R"(table {
  background-color: transparent !important;
}
thead th {
  background-color: #ebe3d5 !important;
  color: #3f3a34 !important;
  font-weight: 600 !important;
}
tbody td {
  background-color: transparent !important;
}
)");
    }
    return str::DupTemp(R"(table {
  background-color: transparent !important;
}
thead th {
  background-color: #f6f8fa !important;
  color: #24292f !important;
  font-weight: 600 !important;
}
tbody td {
  background-color: transparent !important;
}
)");
}

static TempStr BuildMarkdownTableCss(bool darkTheme, bool eyeCareLight);
static TempStr BuildMarkdownCodeBlockCss(bool darkTheme, bool eyeCareLight);
static TempStr BuildMarkdownFontCss();
static TempStr BuildMarkdownUserCss(const char* nameHint, const char* filePath, float ldx, float ldy, float lfontDy);

static TempStr BuildMarkdownFontCss() {
    // MuPDF: use generic sans-serif/monospace (maps to Arial/Consolas on Windows). Universal
    // selector overrides any ebook Literata rules; keep this block last in user CSS.
    return str::DupTemp(R"(* {
  font-family: sans-serif !important;
}
h1, h2, h3, h4, h5, h6, strong, th {
  font-weight: 600 !important;
}
em, i, cite, dfn, var {
  font-style: italic !important;
}
pre, code, kbd, samp, tt, pre *, code * {
  font-family: monospace !important;
  font-style: normal !important;
  font-weight: normal !important;
}
)");
}

static const char* kMdInlineCodeCssLight =
    R"(p code, li code, td code, th code, h1 code, h2 code, h3 code, h4 code, h5 code, h6 code, dt code, dd code, blockquote code {
  background-color: #eff1f3 !important;
  color: #24292f !important;
  padding: 0.15em 0.35em !important;
  border-radius: 4px !important;
}
)";

static const char* kMdInlineCodeCssDark =
    R"(p code, li code, td code, th code, h1 code, h2 code, h3 code, h4 code, h5 code, h6 code, dt code, dd code, blockquote code {
  background-color: #3d444d !important;
  color: #e6edf3 !important;
  padding: 0.15em 0.35em !important;
  border-radius: 4px !important;
}
)";

static const char* kMdInlineCodeCssEyeCare =
    R"(p code, li code, td code, th code, h1 code, h2 code, h3 code, h4 code, h5 code, h6 code, dt code, dd code, blockquote code {
  background-color: #e0d8ca !important;
  color: #3f3a34 !important;
  padding: 0.15em 0.35em !important;
  border-radius: 4px !important;
}
)";

static TempStr BuildMarkdownUserCss(const char* nameHint, const char* filePath, float ldx, float ldy, float lfontDy) {
    (void)nameHint;
    (void)filePath;
    (void)ldx;
    (void)lfontDy;
    (void)ldy;

    bool darkTheme = IsDarkThemeSelected();
    PdfDocumentColorMode docMode = GetPdfDocumentColorMode();
    bool injectThemeColors = docMode != PdfDocumentColorMode::Light;
    bool reflowThemeBitmapRecolor = ReflowEbookUsesThemeBitmapRecolor();
    bool useEyeCareLightCss =
        injectThemeColors && !darkTheme && !ThemeUsesOriginalPageColors() && !reflowThemeBitmapRecolor;
    bool useDarkCss = injectThemeColors && darkTheme && !reflowThemeBitmapRecolor;

    TempStr css = nullptr;
    if (useDarkCss) {
        css = BuildEbookDarkCss(false);
    } else if (useEyeCareLightCss) {
        static const char* kLightEyeCareCss = R"(html {
  background-color: #f7f3e8 !important;
}
body {
  background-color: #f7f3e8 !important;
  color: #333333;
}
pre {
  background-color: #f7f3e8 !important;
  color: #333333;
}
)";
        css = str::DupTemp(kLightEyeCareCss);
    }

    TempStr mdTableCss = BuildMarkdownTableCss(useDarkCss, useEyeCareLightCss);
    if (mdTableCss) {
        css = css ? str::JoinTemp(css, "\n", mdTableCss) : mdTableCss;
    }
    TempStr mdCodeCss = BuildMarkdownCodeBlockCss(useDarkCss, useEyeCareLightCss);
    if (mdCodeCss) {
        css = css ? str::JoinTemp(css, "\n", mdCodeCss) : mdCodeCss;
    }
    TempStr mdFontCss = BuildMarkdownFontCss();
    if (mdFontCss) {
        css = css ? str::JoinTemp(css, "\n", mdFontCss) : mdFontCss;
    }
    return css;
}

static TempStr BuildMarkdownCodeBlockCss(bool darkTheme, bool eyeCareLight) {
    if (darkTheme) {
        TempStr css = str::DupTemp(R"(pre {
  display: block !important;
  background-color: #2d333b !important;
  color: #e6edf3 !important;
  border: none !important;
  padding: 0.85em 1em !important;
  border-radius: 6px !important;
  line-height: 1.45 !important;
  white-space: pre-wrap !important;
}
pre code {
  background-color: transparent !important;
  color: inherit !important;
}
)");
        return str::JoinTemp(css, "\n", kMdInlineCodeCssDark);
    }
    if (eyeCareLight) {
        TempStr css = str::DupTemp(R"(pre {
  display: block !important;
  background-color: #ebe3d5 !important;
  color: #3f3a34 !important;
  border: none !important;
  padding: 0.85em 1em !important;
  border-radius: 6px !important;
  line-height: 1.45 !important;
  white-space: pre-wrap !important;
}
pre code {
  background-color: transparent !important;
  color: inherit !important;
}
)");
        return str::JoinTemp(css, "\n", kMdInlineCodeCssEyeCare);
    }
    TempStr css = str::DupTemp(R"(pre {
  display: block !important;
  background-color: #f6f8fa !important;
  color: #24292f !important;
  border: none !important;
  padding: 0.85em 1em !important;
  border-radius: 6px !important;
  line-height: 1.45 !important;
  white-space: pre-wrap !important;
}
pre code {
  background-color: transparent !important;
  color: inherit !important;
}
)");
    return str::JoinTemp(css, "\n", kMdInlineCodeCssLight);
}

static const char* kEpubCnCompareTableCss =
    R"(/* cnepub 人物对比表：td width="40/45/10%" 定列宽（html-parse + layout_table） */
table {
  table-layout: fixed !important;
  width: 100% !important;
  border-collapse: collapse !important;
}
td.kindle-cn-table-rn1, td.rt1 {
  text-indent: 0 !important;
  vertical-align: top !important;
}
td.kindle-cn-table-rn1:nth-child(2) {
  text-align: center !important;
  vertical-align: middle !important;
}
/* text00039 等：表头行肖像 + 姓名 */
tr.kindle-cn-table-body td.rt1 {
  text-align: center !important;
  vertical-align: top !important;
  padding: 0 0 0.5em 0 !important;
  border: none !important;
}
tr.kindle-cn-table-body td.rt1:first-child,
tr.kindle-cn-table-body td.rt1:last-child {
  height: 11.5em !important;
}
tr.kindle-cn-table-body td.rt1 img.kindle-cn-inline-image2 {
  height: 9.5em !important;
  width: auto !important;
  max-width: 100% !important;
  vertical-align: top !important;
  display: block !important;
  margin: 0 auto 0.35em auto !important;
}
tr.kindle-cn-table-body td.rt1 .bodyContent_120,
tr.kindle-cn-table-body td.rt1 span.kindle-cn-bold {
  display: block !important;
  text-align: center !important;
  font-size: 0.95em !important;
  line-height: 1.35 !important;
}
tr.kindle-cn-table-body td.rt1:nth-child(2) {
  vertical-align: top !important;
  padding-top: 0 !important;
}
tr.kindle-cn-table-body td.rt1:nth-child(2) span {
  display: block !important;
  padding-top: 4em !important;
  font-size: 1.3em !important;
  line-height: 1.2 !important;
}
/* 正文行与表头行分离，避免压到肖像 */
table > tr:not(.kindle-cn-table-body) > td.kindle-cn-table-rn1 {
  padding: 0.55em 0.45em 0.45em 0.45em !important;
}
td.kindle-cn-table-rn1 .kindle-cn-hei {
  display: block !important;
  text-align: center !important;
}
)";

static TempStr BuildMupdfReflowUserCss(const char* nameHint, const char* filePath, float ldx, float ldy, float lfontDy,
                                       int displayDpi) {
    ReportIf(IsMarkdownReflowDocument(nameHint, filePath));
    bool isEpub = IsEpubReflowNameHint(nameHint);
    EbookTypographyKind typographyKind = GetEbookTypographyKind();
    TempStr ebookCss = nullptr;
    auto eBookUI = GetEBookUI();
    if (eBookUI) {
        // EPUB files ship with their own CSS (e.g. calibre stylesheets) - don't override
        // fonts or paragraph layout. Other formats get CJK fallbacks only when needed.
        // All ebooks: Literata primary; CJK glyphs fall back to songti via load_windows_fallback_font.
        static const char* kEpubFontCss = R"(html, body,
p, span, blockquote, h1, h2, h3, h4, h5, h6, li, td, th, div,
section, article, main, header, footer,
.calibre,
.calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,
.calibre11, .calibre12, .calibre13, .calibre14, .calibre15, .calibre16, .calibre17, .calibre18, .calibre19, .calibre20,
.calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, .calibre_10,
.calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, .calibre_19, .calibre_20 {
  font-family: Literata, Georgia, Charter, "Palatino Linotype", "Times New Roman", "Source Han Serif SC", "思源宋体", "NSimSun", "SimSun", "宋体", serif !important;
}
)";
        static const char* kEpubSharedCss = R"(body {
  line-height: 1.52;
}
p {
  line-height: 1.52;
  margin-top: 0.42em;
  margin-bottom: 0.42em;
}
li, blockquote {
  line-height: 1.5;
}
em, i, cite, dfn, var, .italic {
  font-style: italic !important;
}
a, a:link, a:visited, a:hover, a:active,
p a, .calibre_2 a, .calibre_3 a, .sgc-toc-level a {
  text-decoration: none !important;
  color: #0033cc;
}
.sgc-toc-level {
  text-align: left;
  text-indent: 0;
  line-height: 1.45;
}
)";
        static const char* kFallbackFontCss =
            R"(html, body, p, span, blockquote, h1, h2, h3, h4, h5, h6, li, td, th, div {
  font-family: Literata, Georgia, Charter, "Palatino Linotype", "Times New Roman", "Source Han Serif SC", "思源宋体", "NSimSun", "SimSun", "宋体", serif !important;
  line-height: 1.45 !important;
}
p {
  margin: 0.35em 0;
}
)";
        // Ebook dark-mode color remaps (light palette -> readable on dark page bg):
        //   #0033cc / #03c / blue  ->  #93c5fd  (links, TOC, footnotes)
        //   #000000 / #000         ->  #e8eaed  (body text, via body rule)
        static const char* kEpubReaderLatinFontCss =
            R"(/* Default body face only; book CSS (e.g. STKai for 书虫) keeps priority on styled elements. */
html, body {
  font-family: Literata, Georgia, Charter, "Palatino Linotype", "Times New Roman", "Source Han Serif SC", "思源宋体", "Source Han Serif", "Noto Serif CJK SC", "NSimSun", "SimSun", "宋体", serif !important;
}
)";
        static const char* kEpubReaderCjkFontCss =
            R"(/* Default body face only; book CSS (e.g. STKai / MKai PRC) keeps priority on styled elements. */
html, body {
  font-family: "Source Han Serif SC", "思源宋体", "Source Han Serif", "Noto Serif CJK SC", Literata, Georgia, "NSimSun", "SimSun", "宋体", serif !important;
}
)";
        static const char* kEpubReaderBaseCss = R"(html {
  color-scheme: light;
}
body {
  text-align: left;
  line-height: 1.54;
  margin: 0;
}
p {
  line-height: 1.52 !important;
  margin-top: 0.42em !important;
  margin-bottom: 0.42em !important;
}
p, li, blockquote, td, th, span, div, body,
section, article, main,
.calibre,
.calibre1, .calibre2, .calibre3, .calibre4, .calibre5, .calibre6, .calibre7, .calibre8, .calibre9, .calibre10,
.calibre11, .calibre12, .calibre13, .calibre14, .calibre15, .calibre16, .calibre17, .calibre18, .calibre19, .calibre20,
.calibre_1, .calibre_2, .calibre_3, .calibre_4, .calibre_5, .calibre_6, .calibre_7, .calibre_8, .calibre_9, .calibre_10,
.calibre_11, .calibre_12, .calibre_13, .calibre_14, .calibre_15, .calibre_16, .calibre_17, .calibre_18, .calibre_19, .calibre_20 {
  font-weight: 400 !important;
}
.noindent-bodycontent-1-fangsong,
.bodycontent-1-fangsong,
.bodycontent-2-fangsong,
.bodycontent-1-fangsong-top,
.noindent-bodycontent-1-fangsong-top,
.bodycontent-1-top,
.noindent-bodycontent-1-top,
.bodycontent-1-fangsong-top1,
.bodycontent-2-fangsong-top,
.hang-bodycontent-1-fangsong,
.noindent-bodycontent,
.bodycontent,
.songti,
span.songti {
  font-weight: 400 !important;
}
strong, b {
  font-weight: 700 !important;
}
li, blockquote {
  line-height: 1.5;
}
blockquote {
  margin: 0.8em 1.5em;
}
h1, h2 {
  text-align: center;
  line-height: 1.25;
  margin-top: 1.1em;
  margin-bottom: 0.8em;
}
h3, h4, h5, h6 {
  line-height: 1.3;
  margin-top: 0.9em;
  margin-bottom: 0.55em;
}
em, i, cite, dfn, var, .italic {
  font-style: italic !important;
}
img, svg {
  max-width: 100%;
  height: auto;
}
)" R"(/* Calibre titlepage.xhtml: SVG jacket should fill the page width. */
body > svg, body > div > svg {
  display: block !important;
  width: 100% !important;
  max-width: 100% !important;
  height: auto !important;
  margin: 0 auto !important;
}
img, div img, figure img {
  display: block !important;
  margin: 0.8em 0 !important;
}
/* p img omitted: Oxford Bookworm comic panels (p.picture > a > img) must stay block-level. */
/* Penguin trade EPUBs use portrait_xsmall (28% in the print stylesheet) for
   inline figures. Bump the screen size and keep caption width matched so the
   label aligns with the image. */
.portrait_xsmall, .landscape_xsmall {
  width: 50% !important;
}
.portrait_xsmall + .caption, .landscape_xsmall + .caption {
  width: 50% !important;
}
.portrait_small, .landscape_small {
  width: 65% !important;
}
.portrait_small + .caption, .landscape_small + .caption {
  width: 65% !important;
}
/* Penguin part/chapter dividers are full-page PNGs in .image_full; book CSS uses
   height:99vh and flex centering, which mupdf ignores, collapsing the wrapper. */
.image_full, .image_full_caption, .image_full_landscape, .image_full_caption_landscape {
  width: 100% !important;
  height: auto !important;
  margin: 0 auto !important;
  padding: 0 !important;
  text-align: center !important;
}
.image_full img, .image_full_caption img, .image_full_landscape img, .image_full_caption_landscape img {
  width: 100% !important;
  max-width: 100% !important;
  height: auto !important;
  margin: 0 auto !important;
  display: block !important;
}
/* Chinese trade EPUBs (e.g. ibook.178) use <p class="center"><img/></p> for cover.xhtml
   with a low-res thumbnail JPEG; mupdf renders at intrinsic size without width:100%.
   Do not use :only-child here - mupdf's implementation is broken for single elements. */
body > p.center {
  width: 100% !important;
  margin: 0 !important;
  padding: 0 !important;
  text-align: center !important;
  text-indent: 0 !important;
}
body > p.center img {
  width: 100% !important;
  max-width: 100% !important;
  height: auto !important;
  display: block !important;
  margin: 0 auto !important;
}
/* cnepub.com EPUBs stack img.cover + title/intro in coverpage.html without page breaks. */
img.cover {
  display: block !important;
  width: 100% !important;
  max-width: 100% !important;
  height: auto !important;
  margin: 0 auto !important;
}
img.cover + h1 {
  page-break-before: always !important;
}
/* Calibre+cnepub hybrids inject <body><img class="cover"/> before a <div>; pure cnepub
   wraps img.cover inside <div> instead. Hide the redundant direct-child copy. */
body > img.cover {
  display: none !important;
}
body > img.cover + div > h1 {
  page-break-before: auto !important;
}
/* Calibre duplicate jacket page: sole body.calibre content is 封面 + jacket img. */
body.calibre div.calibre1 > p.calibre2:only-of-type > img.calibre3 {
  display: none !important;
}
body.calibre div.calibre1 > p.calibre2:only-of-type {
  display: none !important;
  height: 0 !important;
  margin: 0 !important;
  padding: 0 !important;
  overflow: hidden !important;
}
figure img, div.figure img, div.fig img, div.image img, div.images img, div.pic img, div.illustration img, div.illus img,
p.figure img, p.fig img, p.image img, p.images img, p.pic img, p.illustration img, p.illus img {
  display: block !important;
  margin: 0.8em 0 !important;
}
/* HiResonator/KF8 dual-image EPUBs ship .squeeze-epub + .squeeze-amzn pairs.
   Put the most specific selectors first: mupdf uses the first matching
   selector's specificity in a comma group, and `div.image img { display:block
   !important }` otherwise beats `.squeeze-amzn`. */
div.image img.squeeze-amzn, div.figure img.squeeze-amzn, div.fig img.squeeze-amzn,
figure img.squeeze-amzn, div.images img.squeeze-amzn, div.picture img.squeeze-amzn,
img.squeeze-amzn,
.squeeze-amzn {
  display: none !important;
}
/* Book CSS uses inline-block .squeeze wrappers centered via text-align on the
   parent; mupdf treats inline-block as block, so use auto margins instead. */
.squeeze {
  display: block !important;
  margin-left: auto !important;
  margin-right: auto !important;
  text-align: center !important;
}
div.image img.squeeze-epub,
div.image1 img.squeeze-epub {
  display: block !important;
  margin-top: 0.8em !important;
  margin-bottom: 0.8em !important;
  margin-left: auto !important;
  margin-right: auto !important;
}
figure, div.figure, div.fig, div.image, div.images, div.picture, div.pic, div.illustration, div.illus,
p.figure, p.fig, p.image, p.images, p.picture, p.pic, p.illustration, p.illus {
  display: block;
  width: 100% !important;
  margin-left: auto !important;
  margin-right: auto !important;
  text-align: center !important;
  text-indent: 0 !important;
  clear: both;
}
figure p, div.figure p, div.fig p, div.image p, div.images p, div.picture p, div.pic p, div.illustration p, div.illus p {
  display: block !important;
  max-width: 92% !important;
  margin-left: auto !important;
  margin-right: auto !important;
  text-align: center !important;
  text-indent: 0 !important;
}
figcaption, caption, p.caption, div.caption, span.caption,
.figcaption, .figure-caption, .image-caption, .picture-caption, .pic-caption, .caption {
  font-size: 0.88em;
  line-height: 1.35;
  text-align: center !important;
  text-indent: 0 !important;
  opacity: 0.82;
}
/* cnepub/kindle-cn side floats: MuPDF ignores float; size on <img> (container % often ignored). */
div.kindle-cn-bodycontent-div-right,
div.kindle-cn-bodycontent-div-right1,
div.kindle-cn-bodycontent-div-right11,
div.kindle-cn-bodycontent-div-left,
div.kindle-cn-bodycontent-div-left1 {
  float: none !important;
  clear: both !important;
  width: auto !important;
  max-width: none !important;
  margin: 1em auto !important;
  text-align: center !important;
  text-indent: 0 !important;
}
img.kindle-cn-bodycontent-divright-image-withnote,
img.kindle-cn-bodycontent-divright-image-withoutnote,
img.kindle-cn-bodycontent-divleft-image-withnote,
img.kindle-cn-bodycontent-divleft-image-withoutnote {
  display: block !important;
  width: auto !important;
  max-width: 30% !important;
  height: auto !important;
  margin: 0 auto 0.5em auto !important;
}
div.kindle-cn-para-center img,
p.kindle-cn-para-center img {
  display: block !important;
  width: auto !important;
  max-height: 10.5em !important;
  height: auto !important;
  margin: 0.5em auto !important;
}
/* Firefly/cnepub reuse zhijiao1 for list boxes (bodyContent_101/187) as well as diary
 * quotes (bodyContent_136 + float). MuPDF has no float — center float wrappers instead of
 * table+rtl on the whole frame (that mis-laid out the Everest attempt list). */
div.kindle-cn-frame-zhijiao1 {
  display: block !important;
  width: 100% !important;
  direction: ltr !important;
  text-align: left !important;
}
div.kindle-cn-frame-zhijiao1 > p,
div.kindle-cn-frame-zhijiao1 > div {
  display: block !important;
  width: auto !important;
  max-width: 100% !important;
  text-align: left !important;
  text-indent: 0 !important;
}
div.kindle-cn-frame-zhijiao1 > p.bodyContent_136,
div.kindle-cn-frame-zhijiao1 > p.bodyContent_101 {
  font-weight: bold !important;
}
div.kindle-cn-frame-zhijiao1 > div.kindle-cn-bodycontent-div-right,
div.kindle-cn-frame-zhijiao1 > div.kindle-cn-bodycontent-div-right1 {
  float: none !important;
  clear: both !important;
  width: auto !important;
  max-width: none !important;
  margin: 1em auto !important;
  text-align: center !important;
}
div.kindle-cn-frame-zhijiao1 img.kindle-cn-bodycontent-divright-image-withnote,
div.kindle-cn-frame-zhijiao1 img.kindle-cn-bodycontent-divright-image-withoutnote {
  display: block !important;
  width: auto !important;
  max-width: 30% !important;
  height: auto !important;
  margin: 0 auto 0.5em auto !important;
}
div.kindle-cn-para-center,
p.kindle-cn-para-center {
  display: block !important;
  text-align: center !important;
  text-indent: 0 !important;
}
/* zhijiao2 info/team boxes: keep LTR; bios left, mini portraits centered. */
div.kindle-cn-frame-zhijiao2,
div.kindle-cn-frame-zhijiao3 {
  display: block !important;
  direction: ltr !important;
  text-align: left !important;
}
div.kindle-cn-frame-zhijiao2 > p,
div.kindle-cn-frame-zhijiao3 > p {
  text-align: left !important;
  text-indent: 0 !important;
}
div.kindle-cn-frame-zhijiao2 .kindle-cn-para-center,
div.kindle-cn-frame-zhijiao3 .kindle-cn-para-center {
  text-align: center !important;
}
/* cnepub 并列图文横排 (tourism table): keep two-up gallery like 稻壳/Kindle. */
table.kindle-cn-tourism-double_image_with_notes-1,
table.kindle-cn-tourism-triple_image_with_notes-1 {
  display: table !important;
  width: 100% !important;
  max-width: 100% !important;
  margin: 1em auto !important;
  border-collapse: collapse !important;
  table-layout: fixed !important;
  text-align: left !important;
}
table.kindle-cn-tourism-double_image_with_notes-1 tr,
table.kindle-cn-tourism-triple_image_with_notes-1 tr {
  display: table-row !important;
}
table.kindle-cn-tourism-double_image_with_notes-1 td.kindle-cn-tourism-double_image_with_notes-2,
table.kindle-cn-tourism-double_image_with_notes-1 td.kindle-cn-tourism-double_image_with_notes-3,
table.kindle-cn-tourism-triple_image_with_notes-1 td.kindle-cn-tourism-triple_image_with_notes_2,
table.kindle-cn-tourism-triple_image_with_notes-1 td.kindle-cn-tourism-triple_image_with_notes-3 {
  display: table-cell !important;
  vertical-align: top !important;
  text-indent: 0 !important;
  line-height: 1.42 !important;
}
table.kindle-cn-tourism-double_image_with_notes-1 td.kindle-cn-tourism-double_image_with_notes-2 {
  width: 50% !important;
  vertical-align: bottom !important;
}
table.kindle-cn-tourism-double_image_with_notes-1 td.kindle-cn-tourism-double_image_with_notes-3 {
  width: 50% !important;
  font-size: 0.85em !important;
  padding: 0.35em 0.6em 0.8em 0.6em !important;
  text-align: left !important;
}
table.kindle-cn-tourism-triple_image_with_notes-1 td.kindle-cn-tourism-triple_image_with_notes_2 {
  width: 33.33% !important;
  vertical-align: bottom !important;
}
table.kindle-cn-tourism-triple_image_with_notes-1 td.kindle-cn-tourism-triple_image_with_notes-3 {
  width: 33.33% !important;
  font-size: 0.85em !important;
  padding: 0.35em 0.5em 0.8em 0.5em !important;
  text-align: left !important;
}
img.kindle-cn-tourism-double_image_with_notes-4,
img.kindle-cn-tourism-triple_image_with_notes-4 {
  display: block !important;
  width: 100% !important;
  max-width: 100% !important;
  height: auto !important;
  margin: 0 !important;
}
p.kindle-cn-picture-txt-right {
  text-align: center !important;
  text-indent: 0 !important;
  margin-left: auto !important;
  margin-right: auto !important;
}
div.kindle-cn-bodycontent-div-alone100,
div.kindle-cn-bodycontent-div-alone100a {
  display: block !important;
  width: 100% !important;
  max-width: 100% !important;
  margin: 1.1em auto !important;
  text-align: center !important;
  text-indent: 0 !important;
  clear: both;
  page-break-before: always !important;
}
img.kindle-cn-bodycontent-image-alone80,
img.kindle-cn-bodycontent-image-alone80-1 {
  display: block !important;
  width: auto !important;
  max-width: 72% !important;
  margin: 0.8em auto !important;
  height: auto !important;
}
img.kindle-cn-bodycontent-image-alone80-withnote {
  display: block !important;
  width: auto !important;
  max-width: 72% !important;
  margin: 0.5em auto 0.05em auto !important;
  height: auto !important;
}
img.kindle-cn-bodycontent-image-alone100,
img.kindle-cn-bodycontent-image-alone100-withnote,
img.kindle-cn-bodycontent-image-alone70,
img.kindle-cn-bodycontent-image-alone70-withnote,
img.kindle-cn-bodycontent-image-alone60,
img.kindle-cn-bodycontent-image-alone60-withnote,
img.kindle-cn-bodycontent-image-alone50,
img.kindle-cn-bodycontent-image-alone50-withnote,
img.kindle-cn-bodycontent-image-alone45,
img.kindle-cn-bodycontent-image-alone45-withnote,
img.kindle-cn-bodycontent-image-alone40,
img.kindle-cn-bodycontent-image-alone40-withnote,
img.kindle-cn-bodycontent-image-alone30,
img.kindle-cn-bodycontent-image-alone30-withnote,
img.kindle-cn-bodycontent-image-alone20,
img.kindle-cn-bodycontent-image-alone20-withnote {
  display: block !important;
  width: auto !important;
  margin: 0.8em auto !important;
  height: auto !important;
}
img.kindle-cn-bodycontent-image-alone70,
img.kindle-cn-bodycontent-image-alone70-withnote {
  max-width: 63% !important;
}
img.kindle-cn-bodycontent-image-alone60,
img.kindle-cn-bodycontent-image-alone60-withnote {
  max-width: 54% !important;
}
img.kindle-cn-bodycontent-image-alone50,
img.kindle-cn-bodycontent-image-alone50-withnote {
  max-width: 45% !important;
}
img.kindle-cn-bodycontent-image-alone45,
img.kindle-cn-bodycontent-image-alone45-withnote {
  max-width: 40% !important;
}
img.kindle-cn-bodycontent-image-alone40,
img.kindle-cn-bodycontent-image-alone40-withnote {
  max-width: 36% !important;
}
img.kindle-cn-bodycontent-image-alone30,
img.kindle-cn-bodycontent-image-alone30-withnote {
  max-width: 27% !important;
}
img.kindle-cn-bodycontent-image-alone20,
img.kindle-cn-bodycontent-image-alone20-withnote {
  max-width: 18% !important;
}
img.kindle-cn-bodycontent-image-alone100,
img.kindle-cn-bodycontent-image-alone100-withnote {
  max-width: 90% !important;
}
p.kindle-cn-picture-txt-withmanycharactors,
p.kindle-cn-picture-txt-withfewcharactors {
  display: block !important;
  max-width: 84% !important;
  margin-left: auto !important;
  margin-right: auto !important;
  margin-top: 0.1em !important;
  margin-bottom: 0.3em !important;
  text-indent: 0 !important;
  text-align: center !important;
  font-size: 0.88em;
  line-height: 1.35;
  opacity: 0.82;
}
div.chatu_img, div.chatu-img {
  display: block !important;
  width: 100% !important;
  margin-left: auto !important;
  margin-right: auto !important;
  text-align: center !important;
  text-indent: 0 !important;
}
p.biaozhu, p.tuzhu, p.tuzhu-c, p.tuzhu-c1 {
  display: block !important;
  max-width: 84% !important;
  margin-left: auto !important;
  margin-right: auto !important;
  text-align: center !important;
  text-indent: 0 !important;
  font-size: 0.88em;
  line-height: 1.35;
  opacity: 0.82;
}
table {
  border-collapse: collapse;
  max-width: 100%;
}
td, th {
  line-height: 1.42;
  vertical-align: top;
}
a, a:link, a:visited, a:hover, a:active,
p a, .calibre_2 a, .calibre_3 a, .sgc-toc-level a {
  text-decoration: none !important;
}
.sgc-toc-level {
  text-align: left;
  text-indent: 0;
  line-height: 1.45;
}
)";
        static const char* kEpubReaderLatinCss = R"(body {
  line-height: 1.52;
}
p {
  line-height: 1.52 !important;
  margin-top: 0.42em !important;
  margin-bottom: 0.42em !important;
}
li, blockquote {
  line-height: 1.5;
}
)";
        static const char* kEpubReaderCjkCss = R"(body {
  line-height: 1.68;
}
p {
  line-height: 1.68 !important;
  margin-top: 0.24em !important;
  margin-bottom: 0.24em !important;
}
li, blockquote {
  line-height: 1.62;
}
h1, h2 {
  line-height: 1.35;
}
)";
        static const char* kEpubReaderMixedCss = R"(body {
  line-height: 1.62;
}
p {
  line-height: 1.62 !important;
  margin-top: 0.32em !important;
  margin-bottom: 0.32em !important;
}
li, blockquote {
  line-height: 1.56;
}
)";
        if (isEpub) {
            TempStr fontCss;
            bool forceFonts = UsesNonDefaultEbookReaderFonts() || typographyKind == EbookTypographyKind::Bilingual;
            if (forceFonts) {
                fontCss = BuildEbookForceFontCss(typographyKind);
            } else {
                fontCss = BuildEbookReaderFontCss(typographyKind);
            }
            const char* rhythmCss = kEpubReaderLatinCss;
            if (typographyKind == EbookTypographyKind::Cjk) {
                rhythmCss = kEpubReaderCjkCss;
            } else if (typographyKind == EbookTypographyKind::Bilingual) {
                rhythmCss = kEpubReaderMixedCss;
            }
            ebookCss = str::JoinTemp(fontCss, "\n", kEpubReaderBaseCss);
            ebookCss = str::JoinTemp(ebookCss, "\n", rhythmCss);
        }
        if (UsesNonDefaultEbookFontSize()) {
            TempStr sizeCss = BuildEbookForceFontSizeCss(displayDpi);
            if (sizeCss) {
                ebookCss = ebookCss ? str::JoinTemp(ebookCss, "\n", sizeCss) : sizeCss;
            }
        }
        if (!isEpub) {
            TempStr fallbackCss = BuildEbookFallbackFontCss();
            ebookCss = ebookCss ? str::JoinTemp(fallbackCss, "\n", ebookCss) : fallbackCss;
        }
        // An empty translated String setting can be serialized as a single-line
        // "# <description>" value. Do not feed that placeholder to MuPDF as CSS.
        TempStr customCss = str::DupTemp(eBookUI->customCSS);
        if (customCss) {
            str::TrimWSInPlace(customCss, str::TrimOpt::Both);
        }
        bool commentPlaceholder = customCss && customCss[0] == '#' && !str::Find(customCss, "{") &&
                                  !str::Find(customCss, "\n") && !str::Find(customCss, "\r");
        if (customCss && !commentPlaceholder) {
            ebookCss = ebookCss ? str::JoinTemp(ebookCss, "\n", customCss) : str::DupTemp(customCss);
        }
    }
    // Oxford Bookworms comic strips wrap each full-page panel in a link. Fit those
    // linked panels to a reflow page, but leave ordinary <p class="picture"><img>
    // .../></p> alone: the exercises in the omnibus edition use that same class
    // for tiny vocabulary pictures.
    if (isEpub) {
        float panelH = ldy - 16.f;
        if (panelH < 400.f) {
            panelH = 400.f;
        }
        TempStr comicCss = str::FormatTemp(R"(/* Sumatra: comic panels - one per reflow page, fill page height */
p.picture > a, p.picture1 > a {
  display: block !important;
  page-break-before: always !important;
  page-break-after: always !important;
  page-break-inside: avoid !important;
  width: 100%% !important;
  margin: 0 !important;
  padding: 0 !important;
}
p.picture > a > img, p.picture1 > a > img {
  display: block !important;
  width: auto !important;
  max-width: 100%% !important;
  height: %.0fpt !important;
  margin: 0 auto !important;
}
p.picture > img, p.picture1 > img {
  display: inline !important;
  width: auto !important;
  max-width: 100%% !important;
  height: auto !important;
  margin: 0.8em 0 !important;
}
)",
                                           panelH);
        ebookCss = ebookCss ? str::JoinTemp(ebookCss, "\n", comicCss) : comicCss;
        TempStr imgFitCss = BuildMupdfReflowEpubImageFitCss(ldy);
        ebookCss = str::JoinTemp(ebookCss, "\n", imgFitCss);
        ebookCss = str::JoinTemp(ebookCss, "\n", kEpubCnCompareTableCss);
    }
    TempStr paletteCss = BuildMupdfReflowPaletteCss(isEpub);
    if (paletteCss) {
        ebookCss = ebookCss ? str::JoinTemp(ebookCss, "\n", paletteCss) : str::DupTemp(paletteCss);
    }
    return ebookCss;
}

// mupdf 1.28: per-document styling via fz_style_document (replaces global
// fz_set_user_css / fz_set_use_document_css). Must run after the document is
// opened and before fz_layout_document / reparse.
static void StyleMupdfReflowDocument(fz_context* ctx, fz_document* doc, const char* nameHint, const char* filePath,
                                     float ldx, float ldy, float lfontDy, int displayDpi) {
    if (!doc) {
        return;
    }
    const char* userCss = nullptr;
    int usePublisherCss = 1;
    TempStr ownedCss = nullptr;
    if (IsMarkdownReflowDocument(nameHint, filePath)) {
        ownedCss = BuildMarkdownUserCss(nameHint, filePath, ldx, ldy, lfontDy);
        userCss = ownedCss;
    } else {
        auto eBookUI = GetEBookUI();
        if (eBookUI) {
            usePublisherCss = eBookUI->ignoreDocumentCSS ? 0 : 1;
        }
        ownedCss = BuildMupdfReflowUserCss(nameHint, filePath, ldx, ldy, lfontDy, displayDpi);
        userCss = ownedCss;
    }
    fz_style_document(ctx, doc, usePublisherCss, userCss);
}

static void LayoutMupdfReflowDocument(fz_context* ctx, fz_document* doc, int displayDPI, float ldx, float ldy,
                                      float lfontDy, float* dxOut, float* dyOut) {
    float dx = DpiScale(ldx, displayDPI);
    float dy = DpiScale(ldy, displayDPI);
    float fontDy = DpiScale(lfontDy, displayDPI);
    fz_layout_document(ctx, doc, dx, dy, fontDy);
    if (dxOut) {
        *dxOut = dx;
    }
    if (dyOut) {
        *dyOut = dy;
    }
}

// Theme / document-color: refresh user CSS and drop cached HTML pages. Layout rules in
// BuildMupdfReflowUserCss are identical for open and theme when nameHint and layout pt match;
// only BuildMupdfReflowPaletteCss varies with theme / document color mode.
static void ApplyMupdfThemeCssOnly(fz_context* ctx, fz_document* doc, const char* nameHint, const char* filePath,
                                   float ldxPt, float ldyPt, float lfontDyPt, int displayDpi) {
    StyleMupdfReflowDocument(ctx, doc, nameHint, filePath, ldxPt, ldyPt, lfontDyPt, displayDpi);
}

static void WaitForReflowUiDocLock(EngineMupdf* e);

// Persist per-chapter page counts after a full background count so the next
// open of the same file skips chapter layout entirely (fz_count_chapter_pages
// returns cached values).
static void GrowReflowPageCount(EngineMupdf* e, int newCount);

static void EngineMupdfTryLoadEpubMeta(EngineMupdf* e) {
    // Do not restore TOC metadata from a sidecar file. The loading state is
    // deliberately scoped to the current document session.
    (void)e;
}

static bool EngineMupdfApplyEpubMetaIfComplete(EngineMupdf* e) {
    if (!e || !e->epubMetaLoaded) {
        return false;
    }
    int chapters = e->epubMeta.profile.chapters;
    int pages = e->epubMeta.profile.pages;
    if (chapters <= 0 || pages <= 0 || e->epubMeta.chapterStartPage.Size() != chapters) {
        return false;
    }
    ScopedCritSec scope(&e->pagesLock);
    e->reflowChapterStartPage.Clear();
    for (int i = 0; i < chapters; i++) {
        e->reflowChapterStartPage.Append(e->epubMeta.chapterStartPage.At(i));
    }
    e->reflowPagesCounted = pages;
    e->reflowNumChapters = chapters;
    InterlockedExchange(&e->reflowChaptersCounted, chapters);
    // Grow before trusting pageCount: GrowReflowPageCountLocked is a no-op when
    // newCount <= pageCount, so setting pageCount first would skip allocation.
    GrowReflowPageCount(e, pages);
    return true;
}

static bool EngineMupdfFragmentInMeta(EngineMupdf* e, const char* uri) {
    return e && e->epubMetaLoaded && uri && EpubMetaLookupFragmentPage(e->epubMeta, uri) > 0;
}

static void EngineMupdfUpsertFragmentInMeta(EngineMupdf* e, const char* uri, int ch, int pageInChapter, float x,
                                            float y, int pageNo) {
    if (!e || !uri || pageNo <= 0) {
        return;
    }
    for (auto& f : e->epubMeta.fragments) {
        if (f.uri && str::Eq(f.uri, uri)) {
            f.chapter = ch;
            f.pageInChapter = pageInChapter;
            f.x = x;
            f.y = y;
            f.pageNo = pageNo;
            e->epubMetaLoaded = true;
            return;
        }
    }
    EpubMetaFragment frag;
    frag.uri = str::Dup(uri);
    frag.chapter = ch;
    frag.pageInChapter = pageInChapter;
    frag.x = x;
    frag.y = y;
    frag.pageNo = pageNo;
    e->epubMeta.fragments.Append(frag);
    e->epubMetaLoaded = true;
}

static void EngineMupdfTryResolveOutlineFragment(EngineMupdf* e, fz_outline* ol) {
    if (!e || !ol || !ol->uri || !str::FindChar(ol->uri, '#')) {
        return;
    }
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return;
    }
    int ch = ol->page.chapter;
    if (ch < 0) {
        ch = EpubUriChapterIndexNoLayout(e->Ctx(), e->_doc, ol->uri);
    }
    if (ch < 0) {
        return;
    }
    int counted = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
    if (counted <= ch) {
        return;
    }
    fz_link_dest ldest = fz_make_link_dest_none();
    {
        WaitForReflowUiDocLock(e);
        ScopedCritSec scope(&e->docLock);
        auto ctx = e->Ctx();
        fz_try(ctx) {
            ldest = fz_resolve_link_dest(ctx, e->_doc, ol->uri);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            ldest = fz_make_link_dest_none();
        }
    }
    int pageInChapter = ldest.loc.page >= 0 ? ldest.loc.page : 0;
    int resolveCh = ldest.loc.chapter >= 0 ? ldest.loc.chapter : ch;
    int pageNo = ReflowPageNoFromChapter(e, resolveCh, pageInChapter);
    if (pageNo <= 0) {
        return;
    }
    float x = isnan(ldest.x) ? 0.f : ldest.x;
    float y = isnan(ldest.y) ? 0.f : ldest.y;
    EngineMupdfUpsertFragmentInMeta(e, ol->uri, resolveCh, pageInChapter, x, y, pageNo);
}

static void EngineMupdfWalkOutlineFragments(EngineMupdf* e, fz_outline* ol) {
    while (ol) {
        if (e && InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return;
        }
        EngineMupdfTryResolveOutlineFragment(e, ol);
        if (ol->down) {
            EngineMupdfWalkOutlineFragments(e, ol->down);
        }
        ol = ol->next;
    }
}

static int EngineMupdfCollectOutlineTocMeta(EngineMupdf* e, fz_outline* ol, EpubMetaData& data, int idx) {
    while (ol) {
        if (e && InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return idx;
        }
        int pageNo = 0;
        if (ol->uri && str::FindChar(ol->uri, '#')) {
            pageNo = EpubMetaLookupFragmentPage(e->epubMeta, ol->uri);
            if (pageNo <= 0) {
                pageNo = ResolveMupdfLinkPageNo1(e, ol->uri, nullptr);
            }
        }
        if (pageNo <= 0) {
            int ch = ol->page.chapter;
            if (ch < 0 && ol->uri) {
                ch = EpubUriChapterIndexNoLayout(e->Ctx(), e->_doc, ol->uri);
            }
            int pic = ol->page.page >= 0 ? ol->page.page : 0;
            pageNo = ReflowPageNoFromChapter(e, ch, pic);
        }
        while (data.tocResolvedPageNo.Size() <= idx) {
            data.tocResolvedPageNo.Append(0);
        }
        data.tocResolvedPageNo[idx] = pageNo;
        idx++;
        if (ol->down) {
            idx = EngineMupdfCollectOutlineTocMeta(e, ol->down, data, idx);
        }
        ol = ol->next;
    }
    return idx;
}

static void EngineMupdfBuildAndSaveEpubMeta(EngineMupdf* e, int loadMs) {
    // Do not write .epubmeta sidecars. Chapter and TOC data remain in memory
    // for the lifetime of the currently open document only.
    (void)e;
    (void)loadMs;
}

static void EngineMupdfResolveFragmentsAsync(EngineMupdf* e) {
    if (!e || !e->outline) {
        InterlockedExchange(&e->reflowFragmentsAsyncActive, 0);
        return;
    }
    AtomicIntInc(&gDangerousThreadCount);
    defer {
        {
            WaitForReflowUiDocLock(e);
            ScopedCritSec scope(&e->docLock);
            ReleasePerThreadContext(e);
        }
        AtomicIntDec(&gDangerousThreadCount);
        InterlockedExchange(&e->reflowFragmentsAsyncActive, 0);
    };
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return;
    }
    EngineMupdfWalkOutlineFragments(e, e->outline);
    int loadMs = 0;
    if (e->epubOpenStartTick != 0) {
        loadMs = (int)(GetTickCount() - e->epubOpenStartTick);
    }
    EngineMupdfBuildAndSaveEpubMeta(e, loadMs);
}

bool EngineMupdf::LoadFromStream(fz_stream* stm, const char* nameHint, PasswordUI* pwdUI) {
    if (!stm) {
        return false;
    }
    if (IsUnsupportedOfficeFilePath(nameHint)) {
        return false;
    }
    auto ctx = Ctx();

#if 0
    /* a heuristic. a layout page size for .epub is A5 but that makes a font size too
       large for non-epub files like .txt or .xml, so for those use larger A4 */
    float ldx = layoutA4DxPt;
    float ldy = layoutA4DyPt;
    const char* ext = path::GetExtTemp(nameHint);
    if (str::EqI(ext, ".epub")) {
        ldx = layoutA5DxPt;
        ldy = layoutA5DyPt;
    }
#endif

    bool isEpub = str::EndsWithI(nameHint, ".epub");
    // Typography detection opens the source file when its path itself does not
    // provide a language hint. That is required for EPUB layout, but doing it
    // for a PDF reads the entire file once before MuPDF opens it, which makes
    // large PDFs noticeably slower to open.
    EbookTypographyKind typographyKind =
        isEpub ? DetectEbookTypographyKind(FilePath(), nameHint) : EbookTypographyKind::Latin;
    SetEbookTypographyKind(typographyKind);
    SetEbookReaderStyleMobi(false);

    float ldx = layoutA5DxPt;
    float ldy = layoutA5DyPt;
    float lfontDy = layoutFontEm;
    if (isEpub) {
        if (typographyKind == EbookTypographyKind::Cjk || typographyKind == EbookTypographyKind::Bilingual) {
            ldx = layoutCjkEpubDxPt;
            ldy = layoutCjkEpubDyPt;
        } else {
            ldx = layoutLatinEpubDxPt;
            ldy = layoutLatinEpubDyPt;
        }
    } else {
        lfontDy = 8.f;
    }

    float ldxPt, ldyPt, lfontDyPt;
    GetMupdfReflowLayoutPt(nameHint, &ldxPt, &ldyPt, &lfontDyPt);

    float dx, dy;
    _doc = nullptr;
    fz_var(dx);
    fz_var(dy);
    fz_try(ctx) {
        _doc = fz_open_accelerated_document_with_stream(ctx, nameHint, stm, nullptr);
        StyleMupdfReflowDocument(ctx, _doc, nameHint, FilePath(), ldxPt, ldyPt, lfontDyPt, displayDPI);
        pdfdoc = pdf_specifics(ctx, _doc);
        LayoutMupdfReflowDocument(ctx, _doc, displayDPI, ldxPt, ldyPt, lfontDyPt, &dx, &dy);
        reflowLayoutW = dx;
        reflowLayoutH = dy;
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, stm);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        _doc = nullptr;
    }
    if (!_doc) {
        return false;
    }

    isPasswordProtected = fz_needs_password(ctx, _doc);
    if (!isPasswordProtected) {
        return true;
    }

    if (!pwdUI) {
        return false;
    }

    // TODO: make this work for non-PDF formats?
    u8 digest[16 + 32]{};
    if (pdfdoc) {
        FzStreamFingerprint(ctx, pdfdoc->file, digest);
    }

    bool ok = false;
    bool saveKey = false;
    while (!ok) {
        u8* decryptKey = nullptr;
        if (pdfdoc) {
            decryptKey = pdf_crypt_key(ctx, pdfdoc->crypt);
        }
        AutoFreeStr pwd(pwdUI->GetPassword(FilePath(), digest, decryptKey, &saveKey));
        if (!pwd) {
            // password not given or encryption key has been remembered
            ok = saveKey;
            break;
        }

        // MuPDF expects passwords to be UTF-8 encoded
        TempStr pwdA = pwd.Get();
        ok = fz_authenticate_password(ctx, _doc, pwdA);
        // according to the spec (1.7 ExtensionLevel 3), the password
        // for crypt revisions 5 and above are in SASLprep normalization
        if (!ok) {
            // TODO: this is only part of SASLprep
            pwd.Set(NormalizeString(pwd, 5 /* NormalizationKC */));
            if (pwd) {
                pwdA = pwd.Get();
                ok = fz_authenticate_password(ctx, _doc, pwdA);
            }
        }
        // older Acrobat versions seem to have considered passwords to be in codepage 1252
        // note: such passwords aren't portable when stored as Unicode text
        if (!ok && GetACP() != 1252) {
            TempStr pwd_ansi = pwd.Get();
            TempWStr pwdCp1252 = strconv::StrCPToWStrTemp(pwd_ansi, 1252);
            pwdA = ToUtf8Temp(pwdCp1252);
            ok = fz_authenticate_password(ctx, _doc, pwdA);
        }
        if (ok) {
            str::ReplaceWithCopy(&pdfPassword, pwdA);
        }
    }

    if (pdfdoc && ok && saveKey) {
        memcpy(digest + 16, pdf_crypt_key(ctx, pdfdoc->crypt), 32);
        char* hex = _MemToHex(&digest);
        decryptionKey = StrDup(arena, Str(hex));
        free(hex);
    }
    // TODO: if !ok,
    return ok;
}

static PageLayout GetPreferredLayout(fz_context* ctx, fz_document* doc) {
    PageLayout layout(PageLayout::Type::Single);
    pdf_document* pdfdoc = pdf_specifics(ctx, doc);
    if (!pdfdoc) {
        return layout;
    }

    pdf_obj* root = nullptr;
    fz_var(root);
    fz_try(ctx) {
        root = pdf_dict_gets(ctx, pdf_trailer(ctx, pdfdoc), "Root");
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        root = nullptr;
    }
    if (!root) {
        return layout;
    }

    const char* name = nullptr;
    fz_var(name);
    fz_try(ctx) {
        name = pdf_to_name(ctx, pdf_dict_gets(ctx, root, "PageLayout"));
        if (str::EndsWith(name, "Right")) {
            layout.type = PageLayout::Type::Book;
        } else if (str::StartsWith(name, "Two")) {
            layout.type = PageLayout::Type::Facing;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }

    pdf_obj* prefs = nullptr;
    const char* direction = nullptr;
    fz_var(prefs);
    fz_var(direction);
    fz_try(ctx) {
        prefs = pdf_dict_gets(ctx, root, "ViewerPreferences");
        direction = pdf_to_name(ctx, pdf_dict_gets(ctx, prefs, "Direction"));
        if (str::Eq(direction, "R2L")) {
            layout.r2l = true;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }

    return layout;
}

static bool IsLinearizedFile(EngineMupdf* e) {
    if (!e->pdfdoc) {
        return false;
    }
    auto ctx = e->Ctx();

    ScopedCritSec scope(&e->docLock);
    int isLinear = 0;
    fz_try(ctx) {
        isLinear = pdf_doc_was_linearized(ctx, e->pdfdoc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        isLinear = 0;
    }
    return isLinear;
}

static void AcquireReflowUiDocLock(EngineMupdf* e);
static void ReleaseReflowUiDocLock(EngineMupdf* e);

static RectF LoadReflowPageMediabox(EngineMupdf* e, int pageNo) {
    auto ctx = e->Ctx();
    fz_rect mbox{};
    fz_page* page = nullptr;
    fz_var(page);
    fz_var(mbox);

    int reflowCh = -1;
    int reflowPageInChapter = 0;
    bool pageLoaded = false;

    auto loadOnce = [&]() {
        page = nullptr;
        mbox = {};
        pageLoaded = false;
        fz_try(ctx) {
            if (reflowCh >= 0) {
                page = fz_load_chapter_page(ctx, e->_doc, reflowCh, reflowPageInChapter);
            } else {
                page = fz_load_page(ctx, e->_doc, pageNo - 1);
            }
            if (page) {
                mbox = fz_bound_page(ctx, page);
                pageLoaded = true;
            }
        }
        fz_always(ctx) {
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            mbox = {};
            pageLoaded = false;
        }
    };

    {
        ScopedCritSec scope(&e->pagesLock);
        reflowCh = ReflowChapterIndexForPageNo(e, pageNo);
        if (reflowCh >= 0 && e->reflowChapterStartPage.isValidIndex(reflowCh)) {
            reflowPageInChapter = (pageNo - 1) - e->reflowChapterStartPage[reflowCh];
            if (reflowPageInChapter < 0) {
                reflowPageInChapter = 0;
            }
        } else {
            reflowCh = -1;
        }
    }

    AcquireReflowUiDocLock(e);
    {
        ScopedCritSec scope(&e->docLock);
        loadOnce();
        if (!pageLoaded && reflowCh >= 0) {
            fz_try(ctx) {
                fz_purge_stored_html_chapter(ctx, e->_doc, reflowCh);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
            }
            loadOnce();
        }
    }
    ReleaseReflowUiDocLock(e);

    if (!pageLoaded || fz_is_empty_rect(mbox)) {
        fz_warn(ctx, "cannot find page size for page %d", pageNo);
        mbox.x0 = 0;
        mbox.y0 = 0;
        mbox.x1 = e->reflowLayoutW;
        mbox.y1 = e->reflowLayoutH;
    }
    return ToRectF(mbox);
}

static RectF LoadNonPdfPageMediabox(EngineMupdf* e, int pageIdx) {
    return LoadReflowPageMediabox(e, pageIdx + 1);
}

static void FinishNonPDFLoading(EngineMupdf* e) {
    ScopedCritSec scope(&e->docLock);

    for (int i = 0; i < e->pageCount; i++) {
        FzPageInfo* pageInfo = e->pages.at(i);
        pageInfo->mediabox = LoadNonPdfPageMediabox(e, i);
        pageInfo->pageNo = i + 1;
    }

    auto ctx = e->Ctx();
    fz_try(ctx) {
        e->outline = fz_load_outline(ctx, e->_doc);
    }
    fz_catch(ctx) {
        // ignore errors from pdf_load_outline()
        // this information is not critical and checking the
        // error might prevent loading some pdfs that would
        // otherwise get displayed
        fz_report_error(ctx);
        fz_warn(ctx, "Couldn't load outline");
    }
}

static void GrowReflowPageCountLocked(EngineMupdf* e, int newCount) {
    if (newCount <= e->pageCount) {
        return;
    }
    for (int i = e->pageCount; i < newCount; i++) {
        auto pi = New<FzPageInfo>(e->arena);
        pi->pageNo = i + 1;
        // Leave mediabox empty so PageMediabox() measures fz_bound_page instead of
        // caching layout-sized placeholders during progressive EPUB loading.
        pi->mediabox = {};
        e->pages.Append(pi);
    }
    e->pageCount = newCount;
}

static void GrowReflowPageCount(EngineMupdf* e, int newCount) {
    for (int i = 0; i < 100; i++) {
        if (TryEnterCriticalSection(&e->pagesLock)) {
            defer {
                LeaveCriticalSection(&e->pagesLock);
            };
            GrowReflowPageCountLocked(e, newCount);
            return;
        }
        Sleep(1);
    }
    ScopedCritSec scope(&e->pagesLock);
    GrowReflowPageCountLocked(e, newCount);
}

static void GetMupdfReflowLayoutPt(const char* nameHint, float* ldxPtOut, float* ldyPtOut, float* lfontDyPtOut) {
    bool isEpub = IsEpubReflowNameHint(nameHint);
    EbookTypographyKind typographyKind = GetEbookTypographyKind();
    float ldx = layoutA5DxPt;
    float ldy = layoutA5DyPt;
    float lfontDy = layoutFontEm;
    if (isEpub) {
        if (typographyKind == EbookTypographyKind::Cjk || typographyKind == EbookTypographyKind::Bilingual) {
            ldx = layoutCjkEpubDxPt;
            ldy = layoutCjkEpubDyPt;
        } else {
            ldx = layoutLatinEpubDxPt;
            ldy = layoutLatinEpubDyPt;
        }
    } else {
        lfontDy = 8.f;
    }
    auto eBookUI = GetEBookUI();
    if (eBookUI) {
        if (eBookUI->layoutDx > 100 && eBookUI->layoutDx != 560) {
            ldx = eBookUI->layoutDx;
        }
        if (eBookUI->layoutDy > 100 && eBookUI->layoutDy != 680) {
            ldy = eBookUI->layoutDy;
        }
    }
    *ldxPtOut = ldx;
    *ldyPtOut = ldy;
    *lfontDyPtOut = lfontDy;
}

static void DropSingleFzPageCache(fz_context* ctx, FzPageInfo* pi) {
    if (!pi) {
        return;
    }
    DeleteVecMembers(pi->links);
    DeleteVecMembers(pi->autoLinks);
    DeleteVecMembers(pi->comments);
    for (FitzPageImageInfo* img : pi->images) {
        if (img && img->image) {
            fz_drop_image(ctx, img->image);
            img->image = nullptr;
        }
    }
    DeleteVecMembers(pi->images);
    if (pi->retainedLinks) {
        fz_drop_link(ctx, pi->retainedLinks);
        pi->retainedLinks = nullptr;
    }
    pi->linksLoaded = false;
    if (pi->displayList) {
        fz_drop_display_list(ctx, pi->displayList);
        pi->displayList = nullptr;
    }
    DropFollowThemePageBitmapCache(pi);
    if (pi->searchStext) {
        fz_drop_stext_page(ctx, pi->searchStext);
        pi->searchStext = nullptr;
    }
    PdfDarkModeInvalidatePage(ctx, pi);
    if (pi->page) {
        fz_drop_page(ctx, pi->page);
        pi->page = nullptr;
    }
    pi->fullyLoaded = false;
    pi->elementsNeedRebuilding = true;
}

// Drop cached fz pages/display lists for every UI page in a reflow chapter.
// MuPDF's HTML store is per-chapter: fz_count_chapter_pages() and store
// eviction can re-layout a chapter without touching our page caches, so the
// user keeps seeing an old bitmap until something else invalidates it.
static void DropReflowChapterPageCaches(EngineMupdf* e, fz_context* ctx, int chapter, bool stampThemeEpoch) {
    if (chapter < 0) {
        return;
    }
    ScopedCritSec scope(&e->pagesLock);
    if (!e->reflowChapterStartPage.isValidIndex(chapter)) {
        return;
    }
    int start = e->reflowChapterStartPage[chapter];
    int end =
        e->reflowChapterStartPage.isValidIndex(chapter + 1) ? e->reflowChapterStartPage[chapter + 1] : e->pages.Size();
    start = std::max(start, 0);
    end = std::min(end, e->pages.Size());
    for (int pageIdx = start; pageIdx < end; pageIdx++) {
        FzPageInfo* pi = e->pages[pageIdx];
        if (!pi) {
            continue;
        }
        DropSingleFzPageCache(ctx, pi);
        e->ClearTextCacheForPage(pi->pageNo);
        if (stampThemeEpoch) {
            pi->reflowThemeCssEpoch = e->reflowThemeCssEpoch;
        }
    }
}

static void RefreshReflowChapterForTheme(fz_context* ctx, EngineMupdf* e, int chapter) {
    if (chapter < 0) {
        return;
    }

    // MuPDF stores reflow layout per chapter, so invalidate it once and bring
    // every cached page from that chapter to the same CSS epoch. Otherwise
    // each visible page can purge and lay out the same (potentially enormous)
    // anthology chapter again during one theme switch.
    fz_purge_stored_html_chapter(ctx, e->_doc, chapter);

    DropReflowChapterPageCaches(e, ctx, chapter, true);
}

static void KeepReflowHtmlSource(EngineMupdf* e, ByteSlice& html) {
    e->reflowHtmlSource.Free();
    e->reflowHtmlSource = html;
    html = ByteSlice();
}

static void DropAllReflowPageCaches(fz_context* ctx, EngineMupdf* e) {
    ScopedCritSec scope(&e->pagesLock);
    for (int i = 0; i < e->pageCount; i++) {
        FzPageInfo* pi = e->pages[i];
        if (!pi) {
            continue;
        }
        DropSingleFzPageCache(ctx, pi);
        e->ClearTextCacheForPage(pi->pageNo);
        pi->reflowThemeCssEpoch = 0;
    }
}

static void RefreshSingleChapterReflowAfterReparse(EngineMupdf* e) {
    fz_context* ctx = e->Ctx();
    int newCount = 0;
    fz_var(newCount);
    fz_try(ctx) {
        newCount = fz_count_pages(ctx, e->_doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return;
    }
    if (newCount <= 0) {
        return;
    }
    GrowReflowPageCount(e, newCount);
    e->pageCount = newCount;
    for (int i = 0; i < e->pageCount; i++) {
        FzPageInfo* pi = e->pages[i];
        if (pi) {
            pi->mediabox = LoadNonPdfPageMediabox(e, i);
            pi->pageNo = i + 1;
        }
    }
    fz_drop_outline(ctx, e->outline);
    e->outline = nullptr;
    fz_try(ctx) {
        e->outline = fz_load_outline(ctx, e->_doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    e->DiscardTocTree();
    e->reflowTocNeedsUiReload = true;
}

static EngineMupdfRelayoutProgressFn gRelayoutProgressCb = nullptr;
static void* gRelayoutProgressUser = nullptr;

static void MaybeReportRelayoutProgress(int percent) {
    if (!gRelayoutProgressCb) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    gRelayoutProgressCb(percent, gRelayoutProgressUser);
}

static bool RelayoutSingleChapterReflowHtml(EngineMupdf* e, fz_context* ctx, const char* nameHint, const char* filePath,
                                            float ldxPt, float ldyPt, float lfontDyPt) {
    if (e->reflowHtmlSource.empty()) {
        return false;
    }
    fz_buffer* buf = nullptr;
    fz_var(buf);
    bool ok = false;
    fz_try(ctx) {
        MaybeReportRelayoutProgress(20);
        ApplyMupdfThemeCssOnly(ctx, e->_doc, nameHint, filePath, ldxPt, ldyPt, lfontDyPt, e->displayDPI);
        MaybeReportRelayoutProgress(35);
        buf = fz_new_buffer_from_copied_data(ctx, e->reflowHtmlSource.data(), e->reflowHtmlSource.size());
        float em = DpiScale(lfontDyPt, e->displayDPI);
        DropAllReflowPageCaches(ctx, e);
        MaybeReportRelayoutProgress(45);
        fz_htdoc_reparse_html(ctx, e->_doc, buf, e->reflowLayoutW, e->reflowLayoutH, em);
        MaybeReportRelayoutProgress(80);
        RefreshSingleChapterReflowAfterReparse(e);
        e->reflowThemeCssEpoch++;
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return ok;
}

static void AcquireReflowUiDocLock(EngineMupdf* e);
static bool TryAcquireReflowUiDocLock(EngineMupdf* e);
static void ReleaseReflowUiDocLock(EngineMupdf* e);
static void KickReflowLayoutWarmAsync(EngineMupdf* e);
static int CountReflowChaptersUpTo(EngineMupdf* e, int targetChapter, const char* notifyPath, bool isBackground,
                                   fz_context* ctxOverride, bool forThemeRecount = false);

static EngineMupdfThemeRecountProgressFn gThemeRecountProgressCb = nullptr;
static void* gThemeRecountProgressUser = nullptr;

void EngineMupdfSetThemeRecountProgressCb(EngineMupdfThemeRecountProgressFn cb, void* user) {
    gThemeRecountProgressCb = cb;
    gThemeRecountProgressUser = user;
}

void EngineMupdfSetRelayoutProgressCb(EngineMupdfRelayoutProgressFn cb, void* user) {
    gRelayoutProgressCb = cb;
    gRelayoutProgressUser = user;
}

static void MaybeReportThemeRecountProgress(EngineMupdf* e, int chaptersDone) {
    if (!gThemeRecountProgressCb || !e || e->reflowNumChapters <= 0) {
        return;
    }
    gThemeRecountProgressCb(chaptersDone, e->reflowNumChapters, gThemeRecountProgressUser);
}

static void WaitForReflowBackgroundYield(EngineMupdf* e) {
    if (!e || InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) == 0) {
        return;
    }
    for (int i = 0; i < 120000; i++) {
        if (InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) == 0) {
            return;
        }
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return;
        }
        // UI paused background loading for theme/font work; don't spin here.
        if (InterlockedCompareExchange(&e->reflowUiPaused, 0, 0) != 0) {
            return;
        }
        if (InterlockedCompareExchange(&e->reflowBackgroundYielding, 0, 0) != 0) {
            return;
        }
        Sleep(1);
    }
}

// Block background work only when the UI is waiting on docLock or paused.
static bool ReflowBackgroundMustYieldForUi(EngineMupdf* e) {
    if (!e || InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) == 0) {
        return false;
    }
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return true;
    }
    if (InterlockedCompareExchange(&e->reflowUiPaused, 0, 0) != 0) {
        return true;
    }
    if (InterlockedCompareExchange(&e->reflowSuspendBackgroundLoad, 0, 0) != 0) {
        return true;
    }
    return InterlockedCompareExchange(&e->reflowUiWantsDocLock, 0, 0) != 0;
}

static bool ReflowBackgroundUserReadingRecently(EngineMupdf* e) {
    DWORD lastAccess = (DWORD)InterlockedCompareExchange(&e->reflowLastPageAccessMs, 0, 0);
    if (lastAccess == 0) {
        return false;
    }
    return GetTickCount() - lastAccess < kReflowActiveReadingMs;
}

static void ReflowBackgroundYield(EngineMupdf* e) {
    InterlockedExchange(&e->reflowBackgroundYielding, 1);
    Sleep(kReflowBackgroundYieldMs);
    InterlockedExchange(&e->reflowBackgroundYielding, 0);
}

// Palette CSS: drop page handles so they re-render with the new colors, but do
// not fz_purge_stored_html() for every chapter here. That walks a fully counted
// anthology on the UI thread (20s+). GetFzPageInfo restyles one chapter lazily
// when the page is actually shown (reflowThemeCssEpoch == 0).
static void InvalidateReflowPageCachesForThemeCssLocked(EngineMupdf* e, fz_context* ctx) {
    if (!e || !ctx || e->pdfdoc || e->pageCount <= 0) {
        return;
    }
    ScopedCritSec scope(&e->pagesLock);
    for (int i = 0; i < e->pageCount; i++) {
        FzPageInfo* pi = e->pages[i];
        if (!pi) {
            continue;
        }
        DropSingleFzPageCache(ctx, pi);
        pi->reflowThemeCssEpoch = 0;
    }
}

static void ResyncReflowChapterPageMapFrom(EngineMupdf* e, fz_context* ctx, int fromChapter, bool forThemeRecount);
static void TruncateReflowChapterPageMapFrom(EngineMupdf* e, int fromChapter);
static void FinishReflowableLoadAsync(EngineMupdf* e);

static int ReflowSyncThroughChapterForProgressiveResync(EngineMupdf* e) {
    int pageNo = (int)InterlockedCompareExchange(&e->reflowLastAccessedPageNo, 0, 0);
    if (pageNo < 1) {
        pageNo = 1;
    }
    int syncCh = ReflowChapterIndexForPageNo(e, pageNo);
    if (syncCh < 0) {
        syncCh = 0;
    }
    int numCh = e->reflowNumChapters;
    if (numCh > 0) {
        int ahead = syncCh + 2;
        if (ahead >= numCh) {
            ahead = numCh - 1;
        }
        if (ahead > syncCh) {
            syncCh = ahead;
        }
    }
    return syncCh;
}

static void KickReflowChapterRecountAsync(EngineMupdf* e) {
    if (!e || e->pdfdoc || IsCreateEngineForThumbnail()) {
        return;
    }
    InterlockedExchange(&e->reflowableLoadAbort, 1);
    for (int i = 0; i < 5000; i++) {
        if (InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) == 0) {
            break;
        }
        Sleep(1);
    }
    InterlockedExchange(&e->reflowableLoadAbort, 0);
    InterlockedExchange(&e->reflowableLoadingInProgress, 1);
    auto fn = MkFunc0<EngineMupdf>(FinishReflowableLoadAsync, e);
    RunAsync(fn, "MupdfReflowRecount");
}

static bool ReflowBackgroundWorkersIdle(EngineMupdf* e) {
    return InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) == 0 &&
           InterlockedCompareExchange(&e->reflowFragmentsAsyncActive, 0, 0) == 0 &&
           InterlockedCompareExchange(&e->reflowNavCountAsyncActive, 0, 0) == 0;
}

static void AbortAndWaitForReflowBackgroundLoad(EngineMupdf* e) {
    if (!e || ReflowBackgroundWorkersIdle(e)) {
        return;
    }
    InterlockedExchange(&e->reflowableLoadAbort, 1);
    for (int i = 0; i < 120000; i++) {
        if (ReflowBackgroundWorkersIdle(e)) {
            break;
        }
        Sleep(1);
    }
    InterlockedExchange(&e->reflowableLoadAbort, 0);
}

void EngineMupdfAbortAndWaitBackgroundWork(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e) {
        return;
    }
    InterlockedExchange(&e->reflowableLoadAbort, 1);
    AbortAndWaitForReflowBackgroundLoad(e);
}

void EngineMupdfTouchReadingActivity(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || e->pdfdoc) {
        return;
    }
    InterlockedExchange(&e->reflowLastPageAccessMs, (LONG)GetTickCount());
}

static void ResyncReflowChapterPageMapProgressive(EngineMupdf* e, fz_context* ctx, int fromChapter,
                                                  int syncThroughChapter) {
    if (!e || e->pdfdoc || fromChapter < 0) {
        return;
    }
    AbortAndWaitForReflowBackgroundLoad(e);
    EngineMupdfSetReflowLoadingPaused(e, true);
    defer {
        EngineMupdfSetReflowLoadingPaused(e, false);
    };
    TruncateReflowChapterPageMapFrom(e, fromChapter);
    {
        ScopedCritSec countScope(&e->reflowCountLock);
        CountReflowChaptersUpTo(e, syncThroughChapter, nullptr, false, ctx, true);
    }
    GrowReflowPageCount(e, e->reflowPagesCounted);
    e->pageCount = e->reflowPagesCounted;
    e->reflowTocNeedsUiReload = true;
    e->InvalidateTocTree();
    const char* path = e->FilePath();
    if (path) {
        NotifyEbookPagesLoadingProgress(path, false);
    }
    KickReflowChapterRecountAsync(e);
}

bool EngineMupdfApplyReflowChange(EngineBase* engine, MupdfReflowChangeKind changeKind) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->_doc || e->pdfdoc) {
        return false;
    }

    const char* filePath = engine->FilePath();
    if (str::IsEmpty(filePath)) {
        return false;
    }

    bool paletteOnly = changeKind == MupdfReflowChangeKind::Palette;
    if (paletteOnly) {
        if (!TryAcquireReflowUiDocLock(e)) {
            return false;
        }
    }

    if (InterlockedCompareExchange(&e->reflowThemeRecountInProgress, 1, 0) != 0) {
        if (paletteOnly) {
            ReleaseReflowUiDocLock(e);
        }
        return false;
    }
    defer {
        InterlockedExchange(&e->reflowThemeRecountInProgress, 0);
    };

    const char* nameHint = filePath;
    float ldxPt, ldyPt, lfontDyPt;
    GetMupdfReflowLayoutPt(nameHint, &ldxPt, &ldyPt, &lfontDyPt);

    fz_context* ctx = e->_ctx;
    if (!paletteOnly) {
        AcquireReflowUiDocLock(e);
    }
    bool uiLockHeld = true;
    defer {
        if (uiLockHeld) {
            ReleaseReflowUiDocLock(e);
        }
    };
    if (!paletteOnly) {
        WaitForReflowBackgroundYield(e);
    }
    MaybeReportRelayoutProgress(10);
    bool ok = false;
    bool needChapterResync = false;
    bool geometryChange = changeKind != MupdfReflowChangeKind::Palette;
    bool fontFamilyChange = changeKind == MupdfReflowChangeKind::FontFamily;
    {
        ScopedCritSec scope(&e->docLock);
        fz_try(ctx) {
            if (fontFamilyChange) {
                fz_purge_fallback_font_cache(ctx);
                fz_reset_epub_html_font_set(ctx, e->_doc);
            }
            if (!e->reflowHtmlSource.empty()) {
                ok = RelayoutSingleChapterReflowHtml(e, ctx, nameHint, filePath, ldxPt, ldyPt, lfontDyPt);
            } else {
                MaybeReportRelayoutProgress(20);
                ApplyMupdfThemeCssOnly(ctx, e->_doc, nameHint, filePath, ldxPt, ldyPt, lfontDyPt, e->displayDPI);
                if (geometryChange) {
                    MaybeReportRelayoutProgress(40);
                    float dx, dy;
                    LayoutMupdfReflowDocument(ctx, e->_doc, e->displayDPI, ldxPt, ldyPt, lfontDyPt, &dx, &dy);
                    e->reflowLayoutW = dx;
                    e->reflowLayoutH = dy;
                }
                // Per-document fz_style_document stores CSS on the document;
                // cloned contexts see the same fz_document*. Never drop
                // contexts owned by other threads here: a renderer may already
                // have obtained its clone and be waiting for docLock.
                e->reflowThemeCssEpoch++;
                MaybeReportRelayoutProgress(75);
                InvalidateReflowPageCachesForThemeCssLocked(e, ctx);
                ok = true;
                needChapterResync = geometryChange;
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    MaybeReportRelayoutProgress(95);
    ReleaseReflowUiDocLock(e);
    uiLockHeld = false;
    if (needChapterResync && ok) {
        // Fragment and TOC page numbers were resolved against the previous
        // geometry. A progressive recount only rebuilds nearby chapters
        // synchronously, so retaining this map can make a distant anthology
        // TOC entry jump to its old page while the new recount is running.
        AbortAndWaitForReflowBackgroundLoad(e);
        e->epubMeta.Clear();
        e->epubMetaLoaded = false;
        bool progressive = gGlobalPrefs && gGlobalPrefs->eBookUI.epubProgressiveLoad;
        int syncThroughChapter = -1;
        if (progressive) {
            syncThroughChapter = ReflowSyncThroughChapterForProgressiveResync(e);
        }
        if (progressive && syncThroughChapter >= 0) {
            ResyncReflowChapterPageMapProgressive(e, ctx, 0, syncThroughChapter);
        } else {
            ResyncReflowChapterPageMapFrom(e, ctx, 0, true);
        }
        ok = e->pageCount > 0;
    }
    if (geometryChange && ok) {
        // GetTextForPage() owns a second cache of extracted text and glyph
        // coordinates outside the MuPDF page/search caches invalidated above.
        // Reflow geometry changes make those coordinates stale; keeping them
        // makes selection and cursor-based reading hit the previous layout.
        e->ClearTextCache();
    }
    if (ok) {
        InterlockedExchange(&e->reflowPaletteKeepsPageMap, paletteOnly ? 1 : 0);
    }
    return ok;
}

bool EngineMupdfRelayoutForThemeChange(EngineBase* engine) {
    return EngineMupdfApplyReflowChange(engine, MupdfReflowChangeKind::Palette);
}

bool EngineMupdfRelayoutForFontChange(EngineBase* engine) {
    return EngineMupdfApplyReflowChange(engine, MupdfReflowChangeKind::FontFamily);
}

bool EngineMupdfRelayoutForFontSizeChange(EngineBase* engine) {
    return EngineMupdfApplyReflowChange(engine, MupdfReflowChangeKind::FontSize);
}

void EngineMupdfSetReflowLoadingPaused(EngineBase* engine, bool paused) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e) {
        return;
    }
    if (paused) {
        InterlockedIncrement(&e->reflowUiPaused);
        return;
    }
    LONG depth = InterlockedDecrement(&e->reflowUiPaused);
    if (depth < 0) {
        InterlockedExchange(&e->reflowUiPaused, 0);
        depth = 0;
    }
    if (depth == 0) {
        InterlockedExchange(&e->reflowBackgroundYielding, 0);
    }
}

void EngineMupdfClearReflowUiWantsDocLock(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (e) {
        InterlockedExchange(&e->reflowUiWantsDocLock, 0);
    }
}

void EngineMupdfSetReflowLoadWhenForeground(EngineBase* engine, bool foreground) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || e->pdfdoc) {
        return;
    }
    InterlockedExchange(&e->reflowSuspendBackgroundLoad, foreground ? 0 : 1);
}

void EngineMupdfAbortBackgroundWork(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e) {
        return;
    }
    InterlockedExchange(&e->reflowableLoadAbort, 1);
}

bool EngineMupdfReflowTocNeedsUiReload(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    return e && e->reflowTocNeedsUiReload;
}

void EngineMupdfClearReflowTocNeedsUiReload(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (e) {
        e->reflowTocNeedsUiReload = false;
    }
}

static void AppendReflowChapterStartPage(EngineMupdf* e, int chapterStartPage, int chaptersCounted) {
    for (int i = 0; i < 100; i++) {
        if (TryEnterCriticalSection(&e->pagesLock)) {
            defer {
                LeaveCriticalSection(&e->pagesLock);
            };
            e->reflowChapterStartPage.Append(chapterStartPage);
            InterlockedExchange(&e->reflowChaptersCounted, chaptersCounted);
            return;
        }
        Sleep(1);
    }
    ScopedCritSec scope(&e->pagesLock);
    e->reflowChapterStartPage.Append(chapterStartPage);
    InterlockedExchange(&e->reflowChaptersCounted, chaptersCounted);
}

static void WaitForReflowUiDocLock(EngineMupdf* e) {
    for (;;) {
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return;
        }
        if (InterlockedCompareExchange(&e->reflowUiWantsDocLock, 0, 0) == 0) {
            return;
        }
        Sleep(1);
    }
}

static bool MupdfNeedsDocLock(EngineMupdf* engine, bool reflowLoading) {
    // Reflowable EPUB/FB2/HTML share one fz_archive stream across per-thread cloned
    // contexts; concurrent zip reads corrupt the heap (load_html_image/read_zip_entry).
    // Serialize for the whole document lifetime, not only during progressive reflow.
    (void)reflowLoading;
    return engine && !engine->pdfdoc;
}

static bool MupdfNeedsRenderLock(EngineMupdf* engine, bool reflowLoading) {
    // Reflowable docs rely on docLock alone; PDF uses renderLock for display-list replay.
    (void)reflowLoading;
    if (!engine || !engine->pdfdoc) {
        return false;
    }
    return true;
}

// Acquire docLock during progressive reflow without deadlock: only advertise
// reflowUiWantsDocLock while waiting. Setting wants before EnterCriticalSection
// blocked the UI on docLock while the background thread spun in WaitForReflowUiDocLock.
static void AcquireReflowUiDocLock(EngineMupdf* e) {
    bool backgroundBusy = InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0 ||
                          InterlockedCompareExchange(&e->reflowWarmActive, 0, 0) != 0;
    if (!backgroundBusy) {
        EnterCriticalSection(&e->docLock);
        return;
    }
    for (;;) {
        if (TryEnterCriticalSection(&e->docLock)) {
            InterlockedExchange(&e->reflowUiWantsDocLock, 0);
            return;
        }
        InterlockedExchange(&e->reflowUiWantsDocLock, 1);
        Sleep(0);
    }
}

static bool TryAcquireReflowUiDocLock(EngineMupdf* e) {
    if (TryEnterCriticalSection(&e->docLock)) {
        InterlockedExchange(&e->reflowUiWantsDocLock, 0);
        return true;
    }
    InterlockedExchange(&e->reflowUiWantsDocLock, 1);
    return false;
}

static void ReleaseReflowUiDocLock(EngineMupdf* e) {
    LeaveCriticalSection(&e->docLock);
    InterlockedExchange(&e->reflowUiWantsDocLock, 0);
}

// During progressive EPUB reflow the UI/render thread and MupdfReflowLoad both
// touch the same fz_document via per-thread cloned contexts. docLock must cover
// every mupdf call that reads/writes document state (not just fz_load_page).
struct ReflowUiDocLock {
    EngineMupdf* e = nullptr;
    bool active = false;
    bool uiPriority = true;

    ReflowUiDocLock(EngineMupdf* engine, bool reflowLoading, bool isBackground = false) {
        if (!MupdfNeedsDocLock(engine, reflowLoading)) {
            return;
        }
        e = engine;
        uiPriority = !isBackground;
        Acquire();
    }

    void Acquire() {
        if (!e || active) {
            return;
        }
        active = true;
        if (uiPriority) {
            AcquireReflowUiDocLock(e);
        } else {
            DWORD waitStart = GetTickCount();
            EnterCriticalSection(&e->docLock);
            DWORD waited = GetTickCount() - waitStart;
            if (waited >= 500) {
                logf("search: waited %lu ms for reflow docLock\n", (unsigned long)waited);
            }
        }
    }

    void Release() {
        if (!active) {
            return;
        }
        if (uiPriority) {
            ReleaseReflowUiDocLock(e);
        } else {
            LeaveCriticalSection(&e->docLock);
        }
        active = false;
    }

    ~ReflowUiDocLock() { Release(); }
};

// During progressive reflow, ReflowUiDocLock already serializes mupdf document
// access with background chapter counting. Skip renderLock then, or threads
// deadlock (GetFzPageInfo holds docLock then renderLock while a render worker
// waits on docLock).
struct ReflowRenderLock {
    EngineMupdf* e = nullptr;
    bool active = false;

    ReflowRenderLock(EngineMupdf* engine, bool reflowLoading) {
        if (!MupdfNeedsRenderLock(engine, reflowLoading)) {
            return;
        }
        e = engine;
        active = true;
        EnterCriticalSection(&e->renderLock);
    }

    ~ReflowRenderLock() {
        if (!active) {
            return;
        }
        LeaveCriticalSection(&e->renderLock);
    }
};

// Count reflowable EPUB chapters up to (and including) targetChapter, resuming
// from reflowChaptersCounted. targetChapter < 0 means "count all chapters".
// The reflowCountLock is held only per-chapter, so the background loader and an
// on-demand call from the UI thread (TOC navigation into an uncounted chapter)
// cooperate: whichever thread grabs the lock counts the next chapter and both
// advance the same shared reflowChaptersCounted, without double-counting or
// corrupting reflowChapterStartPage. Returns total pages counted so far.
// When forThemeRecount is true, the caller must already hold reflowCountLock.
static bool CountReflowChaptersUpToOneChapter(EngineMupdf* e, int targetChapter, fz_context* ctx,
                                              const char* notifyPath, bool forThemeRecount) {
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return true;
    }
    if (e->reflowNumChapters == 0) {
        int numChapters = 0;
        {
            WaitForReflowUiDocLock(e);
            ScopedCritSec scope(&e->docLock);
            fz_try(ctx) {
                numChapters = fz_count_chapters(ctx, e->_doc);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                numChapters = 0;
            }
        }
        if (numChapters <= 0) {
            return true;
        }
        e->reflowNumChapters = numChapters;
    }

    int target = targetChapter;
    if (target < 0 || target >= e->reflowNumChapters) {
        target = e->reflowNumChapters - 1;
    }
    int ch = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
    if (ch > target) {
        return true;
    }

    int chapterPages = 0;
    int rawChapterPages = 0;
    {
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return true;
        }
        WaitForReflowUiDocLock(e);
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return true;
        }
        ScopedCritSec scope(&e->docLock);
        if (forThemeRecount) {
            fz_try(ctx) {
                fz_purge_stored_html_chapter(ctx, e->_doc, ch);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
            }
            fz_page* warmPage = nullptr;
            fz_var(warmPage);
            fz_try(ctx) {
                warmPage = fz_load_chapter_page(ctx, e->_doc, ch, 0);
            }
            fz_always(ctx) {
                fz_drop_page(ctx, warmPage);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
            }
        }
        fz_try(ctx) {
            rawChapterPages = fz_count_chapter_pages(ctx, e->_doc, ch);
            chapterPages = EffectiveReflowChapterPageCount(ctx, e->_doc, ch, rawChapterPages);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            chapterPages = 0;
            rawChapterPages = 0;
        }
    }
    int startPage = e->reflowPagesCounted;
    AppendReflowChapterStartPage(e, startPage, ch + 1);
    if (chapterPages > 0) {
        e->reflowPagesCounted = startPage + chapterPages;
        if (e->reflowPagesCounted > e->pageCount) {
            GrowReflowPageCount(e, e->reflowPagesCounted);
        }
    }
    DropReflowChapterPageCaches(e, ctx, ch, false);
    if (forThemeRecount) {
        MaybeReportThemeRecountProgress(e, ch + 1);
    }
    if (notifyPath) {
        bool navPending = false;
        {
            ScopedCritSec navScope(&e->pendingReflowNavLock);
            navPending = e->pendingReflowNavUri != nullptr && e->pendingReflowNavChapter >= 0;
        }
        if (navPending || ((ch + 1) % kReflowNotifyEveryNChapters) == 0 || ch >= target) {
            NotifyEbookPagesLoadingProgress(notifyPath, false);
        }
    }
    return false;
}

static int CountReflowChaptersUpTo(EngineMupdf* e, int targetChapter, const char* notifyPath, bool isBackground,
                                   fz_context* ctxOverride, bool forThemeRecount) {
    if (!e) {
        return 0;
    }
    if (InterlockedCompareExchange(&e->reflowThemeRecountInProgress, 0, 0) != 0 && !forThemeRecount) {
        if (notifyPath != nullptr) {
            return e->reflowPagesCounted;
        }
        if (isBackground) {
            while (InterlockedCompareExchange(&e->reflowThemeRecountInProgress, 0, 0) != 0 &&
                   InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) == 0) {
                Sleep(1);
            }
            if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
                return e->reflowPagesCounted;
            }
        } else {
            return e->reflowPagesCounted;
        }
    }
    for (;;) {
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            break;
        }
        // Fully step aside while the UI thread performs a docLock-heavy operation
        // (theme change, annotation edit). Only the background loader pauses;
        // on-demand counting on the UI thread (TOC navigation) must not wait on
        // itself. Don't hold reflowCountLock while paused.
        if (isBackground) {
            while (ReflowBackgroundMustYieldForUi(e)) {
                InterlockedExchange(&e->reflowBackgroundYielding, 1);
                Sleep(1);
                InterlockedExchange(&e->reflowBackgroundYielding, 0);
                if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
                    break;
                }
            }
            if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
                break;
            }
            while (InterlockedCompareExchange(&e->reflowUiPaused, 0, 0) != 0 &&
                   InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) == 0) {
                InterlockedExchange(&e->reflowBackgroundYielding, 1);
                Sleep(1);
            }
            InterlockedExchange(&e->reflowBackgroundYielding, 0);
            if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
                break;
            }
        }
        // Resolve this thread's context for each iteration. Runtime CSS updates
        // preserve cloned contexts; only the owning thread or engine teardown
        // may release one.
        fz_context* ctx = ctxOverride ? ctxOverride : e->Ctx();
        bool done = false;
        if (forThemeRecount) {
            done = CountReflowChaptersUpToOneChapter(e, targetChapter, ctx, notifyPath, true);
        } else {
            ScopedCritSec countScope(&e->reflowCountLock);
            done = CountReflowChaptersUpToOneChapter(e, targetChapter, ctx, notifyPath, false);
        }
        if (done) {
            break;
        }
        if (forThemeRecount) {
            continue;
        }
        bool navPendingYield = false;
        {
            ScopedCritSec navScope(&e->pendingReflowNavLock);
            navPendingYield = e->pendingReflowNavUri != nullptr && e->pendingReflowNavChapter >= 0;
        }
        if (!navPendingYield) {
            if (isBackground && ReflowBackgroundUserReadingRecently(e)) {
                ReflowBackgroundYield(e);
            } else if ((int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0) % kReflowChaptersPerYield ==
                       0) {
                Sleep(0);
            }
        }
    }
    return e->reflowPagesCounted;
}

static int ReflowStoredChapterPageCount(EngineMupdf* e, int chapter) {
    ScopedCritSec scope(&e->pagesLock);
    if (!e->reflowChapterStartPage.isValidIndex(chapter)) {
        return -1;
    }
    int chStart = e->reflowChapterStartPage[chapter];
    int chEndPageIdx = e->reflowChapterStartPage.isValidIndex(chapter + 1) ? e->reflowChapterStartPage[chapter + 1] - 1
                                                                           : e->pageCount - 1;
    if (chEndPageIdx < chStart) {
        return 0;
    }
    return chEndPageIdx - chStart + 1;
}

static void TruncateReflowChapterPageMapFrom(EngineMupdf* e, int fromChapter) {
    ScopedCritSec countScope(&e->reflowCountLock);
    ScopedCritSec pagesScope(&e->pagesLock);
    if (fromChapter < 0 || !e->reflowChapterStartPage.isValidIndex(fromChapter)) {
        return;
    }
    int rewindGlobal = e->reflowChapterStartPage[fromChapter];
    e->reflowChapterStartPage.SetSize(fromChapter);
    e->reflowPagesCounted = rewindGlobal;
    InterlockedExchange(&e->reflowChaptersCounted, fromChapter);
    if (rewindGlobal < e->pageCount) {
        e->pageCount = rewindGlobal;
    }
}

static bool ReflowChapterSpanFinalizedInPageMap(EngineMupdf* e, int chapter) {
    ScopedCritSec scope(&e->pagesLock);
    return e->reflowChapterStartPage.isValidIndex(chapter + 1);
}

static bool ReflowChapterPageMapNeedsResync(EngineMupdf* e, fz_context* ctx, int chapter, int pageInChapter) {
    if (InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0) {
        return false;
    }
    if (InterlockedCompareExchange(&e->reflowPaletteKeepsPageMap, 0, 0) != 0) {
        return false;
    }
    if (!ReflowChapterSpanFinalizedInPageMap(e, chapter)) {
        return false;
    }
    int sumatraLen = ReflowStoredChapterPageCount(e, chapter);
    if (sumatraLen < 0) {
        return false;
    }
    int rawPages = 0;
    // GetFzPageInfo() calls this with docLock already held. Waiting for the UI
    // here deadlocks if a result click starts rendering at the same instant:
    // the UI waits for docLock while this thread waits for the UI-wants flag.
    fz_try(ctx) {
        rawPages = fz_count_chapter_pages(ctx, e->_doc, chapter);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return false;
    }
    int effPages = EffectiveReflowChapterPageCount(ctx, e->_doc, chapter, rawPages);
    if (pageInChapter >= rawPages) {
        return true;
    }
    if (effPages > 0 && sumatraLen != effPages) {
        return true;
    }
    return false;
}

static void ResyncReflowChapterPageMapFrom(EngineMupdf* e, fz_context* ctx, int fromChapter, bool forThemeRecount) {
    if (!e || e->pdfdoc || fromChapter < 0) {
        return;
    }
    AbortAndWaitForReflowBackgroundLoad(e);
    EngineMupdfSetReflowLoadingPaused(e, true);
    defer {
        EngineMupdfSetReflowLoadingPaused(e, false);
    };
    TruncateReflowChapterPageMapFrom(e, fromChapter);
    if (forThemeRecount) {
        ScopedCritSec countScope(&e->reflowCountLock);
        CountReflowChaptersUpTo(e, -1, nullptr, false, ctx, true);
    } else {
        CountReflowChaptersUpTo(e, -1, nullptr, false, ctx, false);
    }
    GrowReflowPageCount(e, e->reflowPagesCounted);
    e->pageCount = e->reflowPagesCounted;
    if (e->outline) {
        WaitForReflowUiDocLock(e);
        ScopedCritSec docScope(&e->docLock);
        EngineMupdfWalkOutlineFragments(e, e->outline);
    }
    e->reflowTocNeedsUiReload = true;
    e->InvalidateTocTree();
    const char* path = e->FilePath();
    if (path) {
        NotifyEbookPagesLoadingProgress(path, true);
    }
}

// Pre-lays out every chapter in the background after loading completes and
// keeps the results in the (enlarged) fz store. With the disk accelerator the
// page counts come from cache, so no chapter was actually laid out during
// "loading" - the first render of any page would pay a full chapter layout
// (seconds for anthology EPUBs where one chapter holds a whole book). After
// warming, page renders and #fragment jumps anywhere are instant.
static void ReflowLayoutWarmAsync(EngineMupdf* e) {
    AtomicIntInc(&gDangerousThreadCount);
    defer {
        AtomicIntDec(&gDangerousThreadCount);
        InterlockedExchange(&e->reflowWarmActive, 0);
    };

    auto ctx = e->Ctx();
    int nch = 0;
    {
        WaitForReflowUiDocLock(e);
        ScopedCritSec scope(&e->docLock);
        fz_try(ctx) {
            nch = fz_count_chapters(ctx, e->_doc);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            nch = 0;
        }
    }
    if (nch <= 0 || nch > 64 * 1024) {
        return;
    }
    // Reading-position-first order: before each chapter, re-center on the
    // chapter of the most recently accessed page and pick the nearest unwarmed
    // chapter (preferring forward). This dynamically follows the user if they
    // jump elsewhere mid-warm.
    Vec<bool> warmed;
    bool* warmedBuf = warmed.AppendBlanks(nch);
    if (!warmedBuf) {
        return;
    }
    memset(warmedBuf, 0, (size_t)nch * sizeof(bool));
    u32 cssEpoch = e->reflowThemeCssEpoch;
    int warmedCount = 0;
    int done = 0;
    while (done < nch && warmedCount < nch) {
        done++;
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            break;
        }
        // fully step aside while the UI performs a docLock-heavy operation
        // (theme change, annotation edit), same as the background loader
        while (InterlockedCompareExchange(&e->reflowUiPaused, 0, 0) != 0 &&
               InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) == 0) {
            Sleep(1);
        }
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            break;
        }
        // theme change purged all chapter layouts: start over with the new CSS
        if (e->reflowThemeCssEpoch != cssEpoch) {
            cssEpoch = e->reflowThemeCssEpoch;
            memset(warmedBuf, 0, (size_t)nch * sizeof(bool));
            done = 0;
            warmedCount = 0;
        }
        // Back off while the user is actively reading: a chapter layout holds
        // docLock for seconds and would stall the render threads, making the
        // document freeze every few pages. Only warm when page accesses have
        // been quiet for a moment.
        const DWORD kWarmQuiescentMs = 1500;
        for (;;) {
            if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
                break;
            }
            DWORD lastAccess = (DWORD)InterlockedCompareExchange(&e->reflowLastPageAccessMs, 0, 0);
            if (lastAccess == 0 || GetTickCount() - lastAccess >= kWarmQuiescentMs) {
                break;
            }
            Sleep(100);
        }
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            break;
        }
        int curPage = (int)InterlockedCompareExchange(&e->reflowLastAccessedPageNo, 0, 0);
        int center = curPage > 0 ? ReflowChapterIndexForPageNo(e, curPage) : 0;
        if (center < 0 || center >= nch) {
            center = 0;
        }
        // Only warm a window around the reading position. Warming the whole
        // book is counterproductive for huge anthologies: layouts beyond the
        // fz store budget just evict each other (including chapters the user
        // is reading), burning CPU and holding docLock for nothing.
        const int kWarmWindow = 16;
        int ch = -1;
        for (int off = 0; off <= kWarmWindow; off++) {
            int fwd = center + off;
            if (fwd < nch && !warmed[fwd]) {
                ch = fwd;
                break;
            }
            int back = center - off;
            if (off > 0 && back >= 0 && !warmed[back]) {
                ch = back;
                break;
            }
        }
        if (ch < 0) {
            // window fully warm: idle until the user moves elsewhere
            done--;
            Sleep(500);
            continue;
        }
        warmed[ch] = true;
        warmedCount++;
        // let the UI/render thread in between chapters; a warm chapter layout
        // can take seconds and docLock is engine-wide
        WaitForReflowUiDocLock(e);
        ScopedCritSec scope(&e->docLock);
        fz_page* page = nullptr;
        fz_var(page);
        fz_try(ctx) {
            // loading page 0 parses + lays out the chapter and stores the
            // laid-out HTML in the fz store; a store hit makes this a no-op
            page = fz_load_chapter_page(ctx, e->_doc, ch, 0);
        }
        fz_always(ctx) {
            fz_drop_page(ctx, page);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        Sleep(0);
    }
    {
        WaitForReflowUiDocLock(e);
        ScopedCritSec scope(&e->docLock);
        ReleasePerThreadContext(e);
    }
}

static void KickReflowLayoutWarmAsync(EngineMupdf* e) {
    // Disabled: pre-laying-out chapters in the background holds docLock for
    // tens of seconds per anthology chapter and blocks scroll rendering, and
    // layouts beyond the fz store budget just evict each other. On-demand
    // layout (one stall on first entry into a cold chapter, like upstream)
    // gives a much smoother reading experience overall.
    if (true) {
        return;
    }
    if (!e || e->pdfdoc) {
        return;
    }
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return;
    }
    if (InterlockedCompareExchange(&e->reflowWarmActive, 1, 0) != 0) {
        return;
    }
    auto fn = MkFunc0<EngineMupdf>(ReflowLayoutWarmAsync, e);
    RunAsync(fn, "ReflowLayoutWarm");
}

static void FinishReflowableLoadAsync(EngineMupdf* e) {
    AtomicIntInc(&gDangerousThreadCount);
    defer {
        AtomicIntDec(&gDangerousThreadCount);
    };
    // Clear only when this thread fully exits. Setting the flag earlier let
    // ~EngineMupdf drop fz_context while CountReflowChaptersUpTo still held
    // an active fz_try on a per-thread clone (context.c error-stack assert).
    defer {
        InterlockedExchange(&e->reflowableLoadingInProgress, 0);
    };

    // let the UI thread render the first pages before heavy background work
    Sleep(kReflowBackgroundDelayMs);
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return;
    }
    // Session restore opens every tab; only count chapters for the one the
    // user is actually looking at. Progressive load starts on first focus.
    while (InterlockedCompareExchange(&e->reflowSuspendBackgroundLoad, 0, 0) != 0 &&
           InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) == 0) {
        Sleep(50);
    }
    if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
        return;
    }

    AutoFreeStr path(str::Dup(e->FilePath()));
    int loadMs = 0;
    if (e->epubOpenStartTick != 0) {
        loadMs = (int)(GetTickCount() - e->epubOpenStartTick);
    }

    int totalPages = 0;
    if (EngineMupdfApplyEpubMetaIfComplete(e)) {
        totalPages = e->reflowPagesCounted;
    } else {
        totalPages = CountReflowChaptersUpTo(e, -1, path, /*isBackground*/ true, nullptr);
        bool abortedAfterCount = InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0;
        if (!abortedAfterCount && e->outline) {
            EngineMupdfWalkOutlineFragments(e, e->outline);
            e->epubMeta.tocResolvedPageNo.Clear();
            EngineMupdfCollectOutlineTocMeta(e, e->outline, e->epubMeta, 0);
            e->epubMeta.profile.pages = e->pageCount;
            e->epubMeta.profile.chapters = e->reflowNumChapters;
            e->epubMetaLoaded = e->epubMeta.fragments.Size() > 0 || e->epubMeta.tocResolvedPageNo.Size() > 0;
        }
    }

    bool aborted = InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0;
    if (!aborted && totalPages > 0 && totalPages < e->pageCount) {
        ScopedCritSec scope(&e->pagesLock);
        e->pageCount = totalPages;
    }

    if (!aborted) {
        EngineMupdfBuildAndSaveEpubMeta(e, loadMs);
    }

    if (!aborted) {
        // Mark stale only. Do NOT call GetToc() here: the UI TreeView still holds
        // TocItem* from the current tree (tooltips, custom draw, clicks). Rebuilding
        // on the background thread deletes that tree and causes use-after-free.
        // LoadTocTree on the UI thread calls GetToc() after ClearTocBox.
        e->InvalidateTocTree();
    }

    {
        WaitForReflowUiDocLock(e);
        ScopedCritSec scope(&e->docLock);
        ReleasePerThreadContext(e);
    }

    if (!aborted && path) {
        NotifyEbookPagesLoadingProgress(path, true);
    }

    if (!aborted) {
        // trade memory for time: pre-lay-out all chapters so later page
        // renders and #fragment jumps don't stall on chapter layout
        KickReflowLayoutWarmAsync(e);
    }
}

bool EngineMupdf::IsProgressiveEbookLoading() {
    return InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;
}

static void LoadPdfPageMediabox(EngineMupdf* e, fz_context* ctx, int pageNo);
static void FinishPdfDeferredWork(EngineMupdf* e, fz_context* ctx);

static void FinishPdfThemeProbeAsync(EngineMupdf* e) {
    AtomicIntInc(&gDangerousThreadCount);
    defer {
        AtomicIntDec(&gDangerousThreadCount);
    };
    auto ctx = e->Ctx();
    {
        ScopedCritSec scope(&e->docLock);
        DetectFollowThemeDocBitmapRecolor(e, ctx);
    }
    InterlockedExchange(&e->pdfFollowThemeProbePending, 0);
    const char* path = e->FilePath();
    if (path) {
        NotifyPdfFollowThemeProbeComplete(path);
    }
}

static void LoadPdfPageMediabox(EngineMupdf* e, fz_context* ctx, int pageNo) {
    // Use pages.Size() (raw PDF page count). After JoinSplitPdfImages, pageCount
    // is the collapsed display count and must not bound raw page indexes.
    ReportIf(pageNo < 0 || pageNo >= e->pages.Size());
    FzPageInfo* pageInfo = e->pages[pageNo];
    if (!pageInfo) {
        return;
    }
    pdf_obj* pageref = nullptr;
    fz_rect mbox{};
    fz_matrix page_ctm{};
    fz_var(pageref);
    fz_var(mbox);
    fz_try(ctx) {
        pageref = pdf_lookup_page_obj(ctx, e->pdfdoc, pageNo);
        pdf_page_obj_transform(ctx, pageref, &mbox, &page_ctm);
        mbox = fz_transform_rect(mbox, page_ctm);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        mbox = {};
    }
    if (fz_is_empty_rect(mbox)) {
        logfa("cannot find page size for page %d", pageNo);
        mbox.x0 = 0;
        mbox.y0 = 0;
        mbox.x1 = 612;
        mbox.y1 = 792;
    }
    pageInfo->mediabox = ToRectF(mbox);
    pageInfo->pageNo = pageNo + 1;
}

static void FinishPdfDeferredWork(EngineMupdf* e, fz_context* ctx) {
    ReportIf(!e->pdfdoc);

    fz_try(ctx) {
        e->outline = fz_load_outline(ctx, e->_doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logfa("Couldn't load outline for '%s'\n", e->FilePath());
    }

    e->attachments = PdfLoadAttachments(ctx, e->pdfdoc, e->FilePath());

    pdf_obj* origInfo = nullptr;
    fz_var(origInfo);
    fz_try(ctx) {
        origInfo = pdf_dict_gets(ctx, pdf_trailer(ctx, e->pdfdoc), "Info");

        if (origInfo) {
            e->pdfInfo = PdfCopyStrDict(ctx, e->pdfdoc, origInfo);
        }
        if (!e->pdfInfo) {
            e->pdfInfo = pdf_new_dict(ctx, e->pdfdoc, 4);
        }
        if (IsLinearizedFile(e)) {
            pdf_dict_puts_drop(ctx, e->pdfInfo, "Linearized", PDF_TRUE);
        }
        pdf_obj* trailer = pdf_trailer(ctx, e->pdfdoc);
        pdf_obj* marked = pdf_dict_getp(ctx, trailer, "Root/MarkInfo/Marked");
        bool isMarked = pdf_to_bool(ctx, marked);
        if (isMarked) {
            pdf_dict_puts_drop(ctx, e->pdfInfo, "Marked", PDF_TRUE);
        }
        pdf_obj* intents = pdf_dict_getp(ctx, trailer, "Root/OutputIntents");
        if (pdf_is_array(ctx, intents)) {
            int n = pdf_array_len(ctx, intents);
            pdf_obj* list = pdf_new_array(ctx, e->pdfdoc, n);
            for (int i = 0; i < n; i++) {
                pdf_obj* intent = pdf_dict_gets(ctx, pdf_array_get(ctx, intents, i), "S");
                if (pdf_is_name(ctx, intent) && !pdf_is_indirect(ctx, intent) &&
                    str::StartsWith(pdf_to_name(ctx, intent), "GTS_PDF")) {
                    pdf_array_push(ctx, list, intent);
                }
            }
            pdf_dict_puts_drop(ctx, e->pdfInfo, "OutputIntents", list);
        }
        pdf_obj* xfa = pdf_dict_getp(ctx, pdf_trailer(ctx, e->pdfdoc), "Root/AcroForm/XFA");
        if (pdf_is_array(ctx, xfa)) {
            pdf_dict_puts_drop(ctx, e->pdfInfo, "Unsupported_XFA", PDF_TRUE);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        fz_warn(ctx, "Couldn't load document properties");
        pdf_drop_obj(ctx, e->pdfInfo);
        e->pdfInfo = nullptr;
    }

    pdf_obj* labels = nullptr;
    fz_var(labels);
    fz_try(ctx) {
        labels = pdf_dict_getp(ctx, pdf_trailer(ctx, e->pdfdoc), "Root/PageLabels");
        if (labels) {
            e->pageLabels = BuildPageLabelVec(ctx, labels, e->PageCount());
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        fz_warn(ctx, "Couldn't load page labels");
    }
    if (e->pageLabels) {
        e->hasPageLabels = true;
    }

    ReportIf(pdf_js_supported(ctx, e->pdfdoc));

    e->RunCadDetection();

    e->followThemeContentProbed = false;
    ApplyFollowThemeDocClassFromMetadata(e, ctx);
}

bool EngineMupdf::FinishLoading() {
    auto ctx = Ctx();
    pdfdoc = pdf_specifics(ctx, _doc);

    preferredLayout = GetPreferredLayout(ctx, _doc);
    allowsPrinting = fz_has_permission(ctx, _doc, FZ_PERMISSION_PRINT);
    allowsCopyingText = fz_has_permission(ctx, _doc, FZ_PERMISSION_COPY);

    bool isReflowable = fz_is_document_reflowable(ctx, _doc);

    if (!pdfdoc && isReflowable) {
        {
            ScopedCritSec scope(&docLock);
            fz_try(ctx) {
                outline = fz_load_outline(ctx, _doc);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                outline = nullptr;
            }
        }

        pageCount = kReflowInitialPages;
        for (int i = 0; i < kReflowInitialPages; i++) {
            auto pi = New<FzPageInfo>(arena);
            pi->pageNo = i + 1;
            pi->mediabox = {};
            pages.Append(pi);
        }

        InterlockedExchange(&reflowableLoadAbort, 0);
        EngineMupdfTryLoadEpubMeta(this);
        bool metaApplied = EngineMupdfApplyEpubMetaIfComplete(this);
        bool progressive = gGlobalPrefs && gGlobalPrefs->eBookUI.epubProgressiveLoad;

        if (!progressive) {
            InterlockedExchange(&reflowableLoadingInProgress, 1);
            NotifyEngineDisplayReady(this);
            if (IsCreateEngineForThumbnail()) {
                InterlockedExchange(&reflowableLoadingInProgress, 0);
                return true;
            }
            CountReflowChaptersUpTo(this, -1, nullptr, /*isBackground*/ false, nullptr);
            if (outline) {
                EngineMupdfWalkOutlineFragments(this, outline);
            }
            InterlockedExchange(&reflowableLoadingInProgress, 0);
            int loadMs = 0;
            if (epubOpenStartTick != 0) {
                loadMs = (int)(GetTickCount() - epubOpenStartTick);
            }
            EngineMupdfBuildAndSaveEpubMeta(this, loadMs);
            return true;
        }

        if (metaApplied) {
            InterlockedExchange(&reflowableLoadingInProgress, 0);
            NotifyEngineDisplayReady(this);
            if (!IsCreateEngineForThumbnail() && outline) {
                InterlockedExchange(&reflowFragmentsAsyncActive, 1);
                auto fn = MkFunc0<EngineMupdf>(EngineMupdfResolveFragmentsAsync, this);
                RunAsync(fn, "EpubMetaFragments");
            }
            return true;
        }

        // Block TOC outline page resolution before the UI is notified; each resolved
        // bookmark can lay out every spine chapter via fz_page_number_from_location.
        InterlockedExchange(&reflowableLoadingInProgress, 1);

        // show the first pages on the UI thread before background chapter counting,
        // which contends on MuPDF locks with fz_load_page during initial render
        NotifyEngineDisplayReady(this);

        if (IsCreateEngineForThumbnail()) {
            InterlockedExchange(&reflowableLoadingInProgress, 0);
            return true;
        }

        auto fn = MkFunc0<EngineMupdf>(FinishReflowableLoadAsync, this);
        RunAsync(fn, "MupdfReflowLoad");
        return true;
    }

    pageCount = 0;
    fz_var(pageCount);
    fz_try(ctx) {
        // this call might throw the first time
        pageCount = fz_count_pages(ctx, _doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        pageCount = 0;
    }
    if (pageCount == 0) {
        fz_warn(ctx, "document has no pages");
        return false;
    }

    for (int i = 0; i < pageCount; i++) {
        auto pi = New<FzPageInfo>(arena);
        pages.Append(pi);
    }
    if (!pdfdoc) {
        FinishNonPDFLoading(this);
        return true;
    }

    {
        ScopedCritSec scope(&docLock);

        // Pre-build the page tree so per-page mediabox lookup stays O(n) instead of
        // repeatedly walking a broken /Pages tree (common in repackaged PDFs).
        fz_try(ctx) {
            pdf_load_page_tree(ctx, pdfdoc);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }

        if (IsCreateEngineForThumbnail()) {
            for (int pageNo = 0; pageNo < pageCount; pageNo++) {
                LoadPdfPageMediabox(this, ctx, pageNo);
            }
            FinishPdfDeferredWork(this, ctx);
            DetectFollowThemeDocBitmapRecolor(this, ctx);
            return true;
        }

        for (int pageNo = 0; pageNo < pageCount; pageNo++) {
            LoadPdfPageMediabox(this, ctx, pageNo);
        }
        FinishPdfDeferredWork(this, ctx);

        // Metadata may already classify LayoutPhoto / FullPageScan. Do not
        // schedule an async probe (and later InvalidateDarkMode) in that case.
        if (!followThemeContentProbed) {
            if (PdfFollowThemePreservesEmbeddedImageColors()) {
                InterlockedExchange(&pdfFollowThemeProbePending, 1);
            } else {
                DetectFollowThemeDocBitmapRecolor(this, ctx);
            }
        }
    }

    // Outside docLock: image collect acquires renderLock / pagesLock.
    ApplyJoinSplitPdfImages(this);
    return true;
}

static NO_INLINE IPageDestination* DestFromAttachment(EngineMupdf* engine, fz_outline* outline) {
    PageDestination* dest = new PageDestination();
    dest->kind = kindDestinationAttachment;
    // WCHAR* path = ToWStr(outline->uri);
    dest->name = str::Dup(outline->title);
    // page is really a stream number
    dest->value = str::Format("%s:%d", engine->FilePath(), outline->page.page);
    dest->pageNo = outline->page.page;
    return dest;
}

int EngineMupdf::OutlinePageNoForItem(fz_link* link, fz_outline* ol, int outlineIndex) {
    auto ctx = Ctx();
    if (!pdfdoc && ol) {
        bool loading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;
        const char* uri = MupdfSafeCStr(ol->uri);
        bool hasFragment = uri && str::FindChar(uri, '#');
        bool metaCurrent = EpubMetaPageMapIsCurrent(this);

        if (!loading && hasFragment && uri) {
            int livePage = ResolveMupdfLinkPageNo1(this, uri, nullptr);
            if (livePage > 0) {
                return livePage;
            }
        }

        if (outlineIndex >= 0 && epubMetaLoaded && metaCurrent) {
            int metaPage = EpubMetaLookupTocPage(epubMeta, outlineIndex);
            if (metaPage > 0) {
                return metaPage;
            }
        }

        int ch = ol->page.chapter;
        if (ch < 0 && uri) {
            ch = EpubUriChapterIndexNoLayout(ctx, _doc, uri);
        }
        int pageInChapter = ol->page.page >= 0 ? ol->page.page : 0;
        if (hasFragment && epubMetaLoaded && metaCurrent) {
            int metaPage = EpubMetaLookupFragmentPage(epubMeta, uri);
            if (metaPage > 0) {
                return metaPage;
            }
        }
        int pageNo = ReflowPageNoFromChapter(this, ch, pageInChapter);
        if (pageNo > 0 && !hasFragment) {
            return pageNo;
        }

        pageNo = FastReflowableOutlinePageNo(this, ctx, _doc, ol);
        if (pageNo > 0 && !hasFragment) {
            return pageNo;
        }

        if (loading || bulkBuildingToc) {
            // While loading, never resolve #fragment anchors here (it would lay
            // out the chapter); show the chapter-start page and let navigation
            // resolve the exact target asynchronously on click.
            return pageNo > 0 ? pageNo : 0;
        }

        // Many EPUBs (e.g. repackaged anthologies) put multiple TOC entries in one spine
        // HTML file with #fragment anchors. The chapter-start cache cannot distinguish them.
        if (uri && hasFragment) {
            pageNo = ResolveMupdfLinkPageNo1(this, uri, nullptr);
            if (pageNo > 0) {
                return pageNo;
            }
        }

        if (pageNo > 0) {
            return pageNo;
        }

        if (uri) {
            return ResolveMupdfLinkPageNo1(this, uri, nullptr);
        }
        return 0;
    }
    return FzGetPageNo(ctx, _doc, link, ol);
}

void EngineMupdf::InvalidateTocTree() {
    // Mark stale but keep alive until UI calls ClearTocBox + reloads via GetToc.
    // Deleting here (on the background load thread) races with TOC clicks that
    // still reference TocItems owned by this tree.
    tocTreeStale = true;
}

static void EngineMupdfDropRetiredOutlines(EngineMupdf* e) {
    if (!e) {
        return;
    }
    auto ctx = e->Ctx();
    for (fz_outline* ol : e->retiredOutlines) {
        fz_drop_outline(ctx, ol);
    }
    e->retiredOutlines.Reset();
}

// Caller holds docLock. Keep the previous outline alive until tocTree dests are deleted.
static void EngineMupdfReloadOutlineAfterWrite(EngineMupdf* e) {
    auto ctx = e->Ctx();
    if (e->outline) {
        e->retiredOutlines.Append(e->outline);
        e->outline = nullptr;
    }
    e->outline = fz_load_outline(ctx, e->_doc);
    e->InvalidateTocTree();
}

void EngineMupdf::DiscardTocTree() {
    delete tocTree;
    tocTree = nullptr;
    tocTreeStale = false;
    EngineMupdfDropRetiredOutlines(this);
}

TocItem* EngineMupdf::BuildTocTree(TocItem* parent, fz_outline* outline, int& idCounter, bool isAttachment,
                                   int& outlineIdx) {
    TocItem* root = nullptr;
    TocItem* curr = nullptr;

    auto ctx = Ctx();
    while (outline) {
        char* name = nullptr;
        WCHAR* nameW = nullptr;
        if (outline->title) {
            // must convert to Unicode because PdfCleanString() doesn't work on utf8
            nameW = ToWStr(outline->title);
            PdfCleanStringInPlace(nameW);
            name = ToUtf8(nameW);
            str::Free(nameW);
        }
        if (!name) {
            name = str::Dup("");
        }

        int pageNo = OutlinePageNoForItem(nullptr, outline, outlineIdx);
        outlineIdx++;

        IPageDestination* dest = nullptr;
        if (isAttachment) {
            dest = DestFromAttachment(this, outline);
        } else {
            dest = NewPageDestinationMupdf(this, ctx, _doc, nullptr, outline, pageNo);
        }
        TocItem* item = NewTocItemWithDestination(parent, name, dest);

        free(name);
        item->isOpenDefault = outline->is_open;
        item->id = ++idCounter;
        item->fontFlags = outline->flags;
        item->pageNo = pageNo;
        if (!isAttachment && InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) == 0) {
            int destPageNo = PageDestGetPageNo(dest);
            // JoinSplitPdfImages maps dest pages to the collapsed display list;
            // keep TocItem.pageNo in sync (otherwise PageNumbersMatch asserts).
            if (destPageNo > 0 && (pageNo <= 0 || destPageNo != pageNo)) {
                item->pageNo = destPageNo;
            }
            ReportIf(!item->PageNumbersMatch());
        }

        if (outline->r != 0 || outline->g != 0 || outline->b != 0) {
            item->color = RGB(outline->r, outline->g, outline->b);
        }

        if (outline->down) {
            item->child = BuildTocTree(item, outline->down, idCounter, isAttachment, outlineIdx);
        }

        if (!root) {
            root = item;
            curr = item;
        } else {
            ReportIf(!curr);
            if (curr) {
                curr->next = item;
            }
            curr = item;
        }

        outline = outline->next;
    }

    return root;
}

static bool IsIgnorableRepeatedTocItem(TocItem* item) {
    return item && !item->child && (str::IsEmpty(item->title) || str::EqI(item->title, "Untitled"));
}

struct TocTitleEntry {
    const char* title;
    int level;
};

static void CollectTocTitleEntries(TocItem* item, int level, Vec<TocTitleEntry>& entries) {
    for (; item; item = item->next) {
        if (!IsIgnorableRepeatedTocItem(item)) {
            entries.Append({item->title, level});
            CollectTocTitleEntries(item->child, level + 1, entries);
        }
    }
}

static bool TocSubtreesHaveSameTitles(TocItem* a, TocItem* b) {
    Vec<TocTitleEntry> aEntries;
    Vec<TocTitleEntry> bEntries;
    CollectTocTitleEntries(a, 0, aEntries);
    CollectTocTitleEntries(b, 0, bEntries);
    Vec<TocTitleEntry>* shorter = &aEntries;
    Vec<TocTitleEntry>* longer = &bEntries;
    if (shorter->size() > longer->size()) {
        Vec<TocTitleEntry>* tmp = shorter;
        shorter = longer;
        longer = tmp;
    }
    if (shorter->empty() || shorter->size() * 10 < longer->size() * 9) {
        return false;
    }
    size_t shorterIdx = 0;
    for (TocTitleEntry& entry : *longer) {
        TocTitleEntry& candidate = shorter->at(shorterIdx);
        if (entry.level == candidate.level && str::Eq(entry.title, candidate.title)) {
            shorterIdx++;
            if (shorterIdx == shorter->size()) {
                return true;
            }
        }
    }
    return false;
}

static TocItem* PruneTocItemsOutsidePageRange(TocItem* item, int firstPage, int lastPage) {
    TocItem* head = nullptr;
    TocItem* tail = nullptr;
    while (item) {
        TocItem* next = item->next;
        item->next = nullptr;
        item->child = PruneTocItemsOutsidePageRange(item->child, firstPage, lastPage);
        bool inRange = item->pageNo >= firstPage && item->pageNo <= lastPage;
        if (!IsIgnorableRepeatedTocItem(item) && (inRange || item->child)) {
            if (!head) {
                head = item;
            } else {
                tail->next = item;
            }
            tail = item;
        } else {
            item->DeleteJustSelf();
        }
        item = next;
    }
    return head;
}

static void CollapseInvalidTocContainers(TocItem* chapter, int firstPage, int lastPage) {
    while (chapter->child && !chapter->child->next && chapter->child->child) {
        TocItem* container = chapter->child;
        bool inRange = container->pageNo >= firstPage && container->pageNo <= lastPage;
        if (inRange) {
            return;
        }
        chapter->child = container->child;
        container->child = nullptr;
        for (TocItem* item = chapter->child; item; item = item->next) {
            item->parent = chapter;
        }
        container->DeleteJustSelf();
    }
}

static void PruneRepeatedTocSubtrees(TocItem* root, int pageCount) {
    for (TocItem* first = root; first && first->next && first->next->next; first = first->next) {
        TocItem* second = first->next;
        TocItem* third = second->next;
        if (!first->child || !second->child || !third->child ||
            !TocSubtreesHaveSameTitles(first->child, second->child) ||
            !TocSubtreesHaveSameTitles(first->child, third->child)) {
            continue;
        }
        int repeatedCount = 0;
        for (TocItem* chapter = first;
             chapter && chapter->child && TocSubtreesHaveSameTitles(first->child, chapter->child);
             chapter = chapter->next) {
            repeatedCount++;
        }
        TocItem* chapter = first;
        for (int i = 0; i < repeatedCount; i++) {
            int firstPage = chapter->pageNo;
            int lastPage = pageCount;
            if (chapter->next && chapter->next->pageNo > firstPage) {
                lastPage = chapter->next->pageNo - 1;
            }
            if (firstPage > 0) {
                chapter->child = PruneTocItemsOutsidePageRange(chapter->child, firstPage, lastPage);
                CollapseInvalidTocContainers(chapter, firstPage, lastPage);
            }
            chapter = chapter->next;
        }
        return;
    }
}

// TODO: maybe build in FinishLoading
TocTree* EngineMupdf::GetToc() {
    if (tocTree && !tocTreeStale) {
        return tocTree;
    }
    // Serialize building: the background reflow loader may pre-build the tree at
    // load completion while the UI thread also asks for it. Without this a second
    // caller would delete a tree the first is still building (use-after-free).
    ScopedCritSec buildScope(&tocBuildLock);
    // re-check after acquiring the lock: another thread may have just built it
    if (tocTree && !tocTreeStale) {
        return tocTree;
    }
    delete tocTree;
    tocTree = nullptr;
    tocTreeStale = false;
    EngineMupdfDropRetiredOutlines(this);
    if (outline == nullptr && attachments == nullptr) {
        if (!pdfdoc) {
            return nullptr;
        }
        ScopedCritSec docScope(&docLock);
        if (!pdf_has_permission(Ctx(), pdfdoc, (fz_permission)PDF_PERM_MODIFY)) {
            return nullptr;
        }
        TocItem* realRoot = new TocItem();
        tocTree = new TocTree(realRoot);
        return tocTree;
    }

    int idCounter = 0;
    int outlineIdx = 0;

    bulkBuildingToc = true;
    defer {
        bulkBuildingToc = false;
    };

    TocItem* root = nullptr;
    TocItem* att = nullptr;
    if (pdfdoc) {
        ScopedCritSec cs(&docLock);
        if (outline) {
            root = BuildTocTree(nullptr, outline, idCounter, false, outlineIdx);
        }
        if (!attachments) {
            goto MakeTree;
        }
        att = BuildTocTree(nullptr, attachments, idCounter, true, outlineIdx);
        if (root) {
            root->AddSiblingAtEnd(att);
        } else {
            root = att;
        }
    } else {
        ReflowUiDocLock docGuard(this, false);
        if (outline) {
            root = BuildTocTree(nullptr, outline, idCounter, false, outlineIdx);
        }
        if (!attachments) {
            goto MakeTree;
        }
        att = BuildTocTree(nullptr, attachments, idCounter, true, outlineIdx);
        if (root) {
            root->AddSiblingAtEnd(att);
        } else {
            root = att;
        }
    }
MakeTree:
    if (!root) {
        return nullptr;
    }
    PruneRepeatedTocSubtrees(root, PageCount());
    TocItem* realRoot = new TocItem();
    realRoot->child = root;
    tocTree = new TocTree(realRoot);
    return tocTree;
}

IPageDestination* EngineMupdf::GetNamedDest(const char* name) {
    if (!pdfdoc) {
        return nullptr;
    }
    auto ctx = Ctx();
    IPageDestination* pageDest = nullptr;
    ScopedCritSec scope2(&docLock);
    char* uri = str::JoinTemp("#nameddest=", name);
    float x, y, zoom = 0;
    int pageNo = ResolveLink(ctx, _doc, uri, &x, &y);
    if (pageNo < 0) {
        return nullptr;
    }

    RectF r{x, y, 0, 0};
    pageDest = NewSimpleDest(pageNo, r, zoom);
    return pageDest;
}

#if 0
IPageDestination* EngineMupdf::GetNamedDest(const char* name) {
    if (!pdfdoc) {
        return nullptr;
    }

    ScopedCritSec scope1(&pagesLock);
    ScopedCritSec scope2(&docLock);

    size_t nameLen = str::Len(name);
    pdf_obj* dest = nullptr;

    fz_var(dest);
    pdf_obj* nameobj = nullptr;
    fz_var(nameobj);
    fz_try(ctx) {
        nameobj = pdf_new_string(ctx, name, (int)nameLen);
        dest = pdf_lookup_dest(ctx, pdfdoc, nameobj);
        pdf_drop_obj(ctx, nameobj);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        dest = nullptr;
    }

    if (!dest) {
        return nullptr;
    }

    IPageDestination* pageDest = nullptr;
    char* uri = nullptr;

    fz_var(uri);
    fz_try(ctx) {
        uri = pdf_parse_link_dest(ctx, pdfdoc, dest);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        uri = nullptr;
    }

    if (!uri) {
        return nullptr;
    }

    float x, y, zoom = 0;
    int pageNo = ResolveLink(ctx, _doc, uri, &x, &y);

    RectF r{x, y, 0, 0};
    pageDest = NewSimpleDest(pageNo, r, zoom);
    fz_free(ctx, uri);
    return pageDest;
}
#endif

// return a page but only if is fully loaded
// pageNo is a raw engine page (JoinSplit mapping happens at public API entry points).
FzPageInfo* EngineMupdf::GetFzPageInfoFast(int pageNo) {
    ScopedCritSec scope(&pagesLock);
    ReportIf(pageNo < 1 || pageNo > pages.Size());
    if (pageNo < 1 || pageNo > pages.Size()) {
        return nullptr;
    }
    FzPageInfo* pageInfo = pages[pageNo - 1];
    if (!pageInfo || !pageInfo->page || !pageInfo->fullyLoaded) {
        return nullptr;
    }
    return pageInfo;
}

static IPageElement* NewFzComment(const char* comment, int pageNo, RectF rect) {
    auto res = new PageElementComment(comment);
    res->pageNo = pageNo;
    res->rect = rect;
    return res;
}

// must be called inside fz_try
static IPageElement* MakePdfCommentFromPdfAnnot(fz_context* ctx, int pageNo, pdf_annot* annot) {
    fz_rect rect = pdf_bound_annot(ctx, annot);
    const char* contents = pdf_annot_contents(ctx, annot);
    const char* label = pdf_annot_field_label(ctx, annot);
    const char* s = contents;
    // TODO: use separate classes for comments and tooltips?
    if (str::IsEmpty(contents)) {
        s = label;
    }
    RectF rd = ToRectF(rect);
    return NewFzComment(s, pageNo, rd);
}

// must be called inside fz_try
static void RebuildCommentsFromAnnotationsInner(fz_context* ctx, pdf_annot* annot, int pageNo,
                                                Vec<IPageElement*>& comments) {
    auto tp = pdf_annot_type(ctx, annot);
    const char* contents = pdf_annot_contents(ctx, annot); // don't free
    if (str::Len(contents) > 128) {
        contents = str::DupTemp(contents, 128);
    }
    bool isContentsEmpty = str::IsEmpty(contents);
    const char* label = pdf_annot_field_label(ctx, annot); // don't free
    bool isLabelEmpty = str::IsEmpty(label);
    int flags = pdf_annot_field_flags(ctx, annot);
    bool isEmpty = isContentsEmpty && isLabelEmpty;

    // const char* tpStr = pdf_string_from_annot_type(ctx, tp);
    //  logf("MakePageElementCommentsFromAnnotations: annot %d '%s', contents: '%s', label: '%s'\n", tp, tpStr,
    //  contents, abel);

    if (PDF_ANNOT_FILE_ATTACHMENT == tp) {
        logf("found file attachment annotation\n");

        pdf_filespec_params fileParams = {};
        pdf_obj* fs = pdf_annot_filespec(ctx, annot);
        int num = pdf_to_num(ctx, pdf_annot_obj(ctx, annot));
        pdf_get_filespec_params(ctx, fs, &fileParams);
        const char* attname = fileParams.filename;
        fz_rect rect = pdf_bound_annot(ctx, annot);
        if (str::IsEmpty(attname) || fz_is_empty_rect(rect) || !pdf_is_embedded_file(ctx, fs)) {
            return;
        }

        logf("attachment: %s, num: %d\n", attname, num);

        auto dest = new PageDestination();
        dest->kind = kindDestinationLaunchEmbedded;
        dest->value = str::Dup(attname);
        dest->embedObjNum = num;

        auto el = new PageElementDestination(dest);
        el->pageNo = pageNo;
        el->rect = ToRectF(rect);

        comments.Append(el);
        // TODO: need to implement https://github.com/sumatrapdfreader/sumatrapdf/issues/1336
        // for saving the attachment to a file
        // TODO: expose /Contents in addition to the file path
        return;
    }

    if (!isEmpty && tp != PDF_ANNOT_FREE_TEXT) {
        auto comment = MakePdfCommentFromPdfAnnot(ctx, pageNo, annot);
        comments.Append(comment);
        return;
    }

    if (PDF_ANNOT_WIDGET == tp && !isLabelEmpty) {
        bool isReadOnly = flags & PDF_FIELD_IS_READ_ONLY;
        if (!isReadOnly) {
            auto comment = MakePdfCommentFromPdfAnnot(ctx, pageNo, annot);
            comments.Append(comment);
        }
    }
}

static void RebuildCommentsFromAnnotations(fz_context* ctx, FzPageInfo* pageInfo) {
    DeleteVecMembers(pageInfo->comments);

    // TODO: can use pageInof->annotations
    Vec<IPageElement*>& comments = pageInfo->comments;

    auto page = pageInfo->page;
    if (!page) {
        return;
    }
    auto pdfpage = pdf_page_from_fz_page(ctx, page);
    int pageNo = pageInfo->pageNo;

    pdf_annot* annot;
    for (annot = pdf_first_annot(ctx, pdfpage); annot; annot = pdf_next_annot(ctx, annot)) {
        fz_try(ctx) {
            RebuildCommentsFromAnnotationsInner(ctx, annot, pageNo, comments);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }

    // re-order list into top-to-bottom order (i.e. last-to-first)
    comments.Reverse();
}

// like GetFzPageInfo() but fails if we can't acquire locks
// prevents blocking main thread due to render thread keeping the lock
// https://github.com/sumatrapdfreader/sumatrapdf/issues/4145
// https://github.com/sumatrapdfreader/sumatrapdf/issues/4187
FzPageInfo* EngineMupdf::GetFzPageInfoCanFail(int pageNo) {
    FzPageInfo* res = nullptr;
    if (!TryEnterCriticalSection(&pagesLock)) {
        return nullptr;
    }
    if (pageNo >= 1 && pageNo <= pages.Size()) {
        FzPageInfo* pageInfo = pages[pageNo - 1];
        if (pageInfo &&
            (pageInfo->fullyLoaded || pageInfo->retainedLinks || pageInfo->links.Size() > 0 || pageInfo->page)) {
            res = pageInfo;
        }
    }
    LeaveCriticalSection(&pagesLock);
    return res;
}

/* SumatraPDF */
fz_stext_page* fz_new_stext_page_from_page2(fz_context* ctx, fz_page* page, const fz_stext_options* options,
                                            fz_cookie* cookie) {
    fz_stext_page* text;
    fz_device* dev = NULL;

    fz_var(dev);

    if (page == NULL) return NULL;

    text = fz_new_stext_page(ctx, fz_bound_page(ctx, page));
    fz_try(ctx) {
        dev = fz_new_stext_device(ctx, text, options);
        fz_run_page_contents(ctx, page, dev, fz_identity, cookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
    }
    fz_catch(ctx) {
        fz_drop_stext_page(ctx, text);
        fz_rethrow(ctx);
    }

    return text;
}

// Maybe: handle FZ_ERROR_TRYLATER, which can happen when parsing from network.
// (I don't think we read from network now).
// Maybe: when loading fully, cache extracted text in FzPageInfo
// so that we don't have to re-do fz_new_stext_page_from_page() when doing search.
// Search uses FzPageInfo::searchStext (see ExtractPageTextFromFzPageInfo).
FzPageInfo* EngineMupdf::GetFzPageInfo(int pageNo, bool loadQuick, fz_cookie* cookie, bool loadLinks,
                                       bool backgroundAccess) {
    // pageNo is a raw engine page. Public EngineBase APIs map display->engine first.
    auto ctx = Ctx();
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;

    // TODO: minimize time spent under pagesLock when fully loading
    FzPageInfo* pageInfo = nullptr;
    {
        ScopedCritSec scope(&pagesLock);
        int n = pages.Size();
        ReportIf(pageNo < 1 || pageNo > n);
        if (pageNo < 1 || pageNo > n) {
            return nullptr;
        }
        pageInfo = pages[pageNo - 1];
        if (!pageInfo) {
            return nullptr;
        }
    }

    if (!pdfdoc && !backgroundAccess) {
        InterlockedExchange(&reflowLastAccessedPageNo, (LONG)pageNo);
        InterlockedExchange(&reflowLastPageAccessMs, (LONG)GetTickCount());
    }

    ReflowUiDocLock docGuard(this, reflowLoading, backgroundAccess);
    ReflowRenderLock renderGuard(this, reflowLoading);

    if (!pdfdoc && pageInfo->reflowThemeCssEpoch != reflowThemeCssEpoch) {
        fz_try(ctx) {
            int chapter = ReflowChapterIndexForPageNo(this, pageNo);
            RefreshReflowChapterForTheme(ctx, this, chapter);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        pageInfo->reflowThemeCssEpoch = reflowThemeCssEpoch;
    }

    // page-running operations on this specific page run under per-page lock.
    if (!pageInfo->page) {
        // For reflowable documents load by (chapter, page-in-chapter) using our
        // own stable chapter-start-page snapshot. The flat fz_load_page() maps
        // the page number through mupdf's *current* per-chapter page counts,
        // which drift whenever the CSS checksum changes (theme switch) or a
        // chapter gets re-laid out: page numbers near the end of the book then
        // fall beyond mupdf's total and fz_load_page() throws "invalid page
        // number" deterministically, permanently failing to render the page.
        // fz_load_chapter_page() doesn't validate against the (possibly stale)
        // count and is also O(1) instead of counting every chapter up front.
        int reflowCh = -1;
        int reflowPageInChapter = 0;
        if (!pdfdoc) {
            reflowCh = ReflowChapterIndexForPageNo(this, pageNo);
            if (reflowCh >= 0) {
                ScopedCritSec scopePages(&pagesLock);
                if (reflowCh < reflowChapterStartPage.Size()) {
                    reflowPageInChapter = (pageNo - 1) - reflowChapterStartPage[reflowCh];
                    if (reflowPageInChapter < 0) {
                        reflowPageInChapter = 0;
                    }
                } else {
                    reflowCh = -1;
                }
            }
        }
        if (reflowCh >= 0 && ReflowChapterPageMapNeedsResync(this, ctx, reflowCh, reflowPageInChapter)) {
            // Resync waits for background helpers and manages docLock itself.
            // Never call it while retaining this search/render acquisition: a
            // helper or a concurrently waiting UI render may need the same lock.
            docGuard.Release();
            ResyncReflowChapterPageMapFrom(this, ctx, reflowCh, false);
            docGuard.Acquire();
            reflowCh = ReflowChapterIndexForPageNo(this, pageNo);
            if (reflowCh >= 0) {
                ScopedCritSec scopePages(&pagesLock);
                if (reflowCh < reflowChapterStartPage.Size()) {
                    reflowPageInChapter = (pageNo - 1) - reflowChapterStartPage[reflowCh];
                    if (reflowPageInChapter < 0) {
                        reflowPageInChapter = 0;
                    }
                } else {
                    reflowCh = -1;
                }
            }
        }
        fz_try(ctx) {
            if (reflowCh >= 0) {
                pageInfo->page = fz_load_chapter_page(ctx, _doc, reflowCh, reflowPageInChapter);
            } else {
                pageInfo->page = fz_load_page(ctx, _doc, pageNo - 1);
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        if (!pageInfo->page && !pdfdoc) {
            // stale cached chapter layout: purge and retry once with a fresh layout
            fz_try(ctx) {
                if (reflowCh >= 0) {
                    fz_purge_stored_html_chapter(ctx, _doc, reflowCh);
                    DropReflowChapterPageCaches(this, ctx, reflowCh, false);
                    pageInfo->page = fz_load_chapter_page(ctx, _doc, reflowCh, reflowPageInChapter);
                } else {
                    pageInfo->page = fz_load_page(ctx, _doc, pageNo - 1);
                }
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
            }
        }
    }

    fz_page* page = pageInfo->page;
    if (!page) {
        return pageInfo;
    }

    if (!pdfdoc) {
        fz_rect mbox = fz_bound_page(ctx, page);
        if (!fz_is_empty_rect(mbox)) {
            pageInfo->mediabox = ToRectF(mbox);
        }
    }

    // build annotations info on first access
    if (pdfdoc && pageInfo->annotations.Size() == 0) {
        fz_try(ctx) {
            pdf_page* pdfpage = pdf_page_from_fz_page(ctx, pageInfo->page);
            pdf_annot* annot = pdf_first_annot(ctx, pdfpage);
            while (annot) {
                Annotation* a = MakeAnnotationWrapper(this, annot, pageNo);
                if (a) {
                    pageInfo->annotations.Append(a);
                }
                annot = pdf_next_annot(ctx, annot);
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        RebuildCommentsFromAnnotations(ctx, pageInfo);
    }

    if (loadLinks && loadQuick && !pageInfo->fullyLoaded && !pageInfo->linksLoaded) {
        fz_link* link = nullptr;
        fz_var(link);
        fz_try(ctx) {
            link = fz_load_links(ctx, page);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        link = FixupPageLinks(link);
        pageInfo->retainedLinks = link;
        pageInfo->linksLoaded = true;
        while (link) {
            auto pel = NewLinkDestination(pageNo, this, ctx, _doc, link, nullptr);
            pageInfo->links.Append(pel);
            link = link->next;
        }
        HealPlaceholderEpubPageLinks(this, ctx, _doc, pageInfo->links);
        if (pageInfo->links.Size() > 0) {
            pageInfo->elementsNeedRebuilding = true;
        }
    }

    if (loadQuick || pageInfo->fullyLoaded) {
        return pageInfo;
    }

    ReportIf(pageInfo->pageNo != pageNo);

    pageInfo->fullyLoaded = true;

    fz_stext_page* stext = nullptr;
    fz_var(stext);
    fz_stext_options opts = NewTextPageOptions(FZ_STEXT_PRESERVE_IMAGES);
    fz_try(ctx) {
        stext = fz_new_stext_page_from_page2(ctx, page, &opts, cookie);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }

    if (!pageInfo->linksLoaded) {
        fz_link* link = fz_load_links(ctx, page);
        link = FixupPageLinks(link); // TOOD: is this necessary?
        pageInfo->retainedLinks = link;
        pageInfo->linksLoaded = true;
        while (link) {
            auto pel = NewLinkDestination(pageNo, this, ctx, _doc, link, nullptr);
            pageInfo->links.Append(pel);
            link = link->next;
        }
        HealPlaceholderEpubPageLinks(this, ctx, _doc, pageInfo->links);
    }

    if (!stext) {
        return pageInfo;
    }

    FzLinkifyPageText(pageInfo, stext);
    FzFindImagePositions(ctx, pageNo, pageInfo->images, stext);
    if (pdfdoc && PdfShouldCollectContentImages()) {
        FzCollectImagesFromPageContent(ctx, pageNo, pageInfo, page, cookie);
        pageInfo->contentImagesCollected = true;
    }
    pageInfo->darkLegacySkipHash = 0;
    if (!pageInfo->searchStext) {
        pageInfo->searchStext = stext;
        stext = nullptr;
    }
    fz_drop_stext_page(ctx, stext);
    return pageInfo;
}

RectF EngineMupdf::PageMediabox(int pageNo) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return {};
    }
    FzPageInfo* pi = nullptr;
    RectF cached{};
    {
        ScopedCritSec scope(&pagesLock);
        int n = pages.Size();
        ReportIf(pageNo < 1 || pageNo > n);
        if (pageNo < 1 || pageNo > n) {
            return {};
        }
        pi = pages[pageNo - 1];
        if (!pi) {
            return {};
        }
        cached = pi->mediabox;
    }
    bool measure = cached.IsEmpty();
    if (measure && pi) {
        if (pdfdoc) {
            ScopedCritSec docScope(&docLock);
            LoadPdfPageMediabox(this, Ctx(), pageNo - 1);
            return pi->mediabox;
        }
        bool loading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;
        // Placeholder size for pages not laid out yet. Do not cache it: FXL
        // chapters honor <meta viewport> (this book's cover is 1398x2000) while
        // the reflow page is ~750x1025. Caching the estimate makes Fit Page
        // position a small box while render draws the real page — cover sits
        // to the right and looks like half an image. Always measure the first
        // screen of pages (they are on screen immediately).
        if (loading && reflowLayoutW > 0 && reflowLayoutH > 0 && pageNo > kReflowInitialPages) {
            return RectF(0, 0, reflowLayoutW, reflowLayoutH);
        }
        RectF box = LoadReflowPageMediabox(this, pageNo);
        ScopedCritSec scope(&pagesLock);
        if (pageNo >= 1 && pageNo <= pages.Size() && pages[pageNo - 1] == pi) {
            pi->mediabox = box;
        }
        return box;
    }
    return cached;
}

// returns a kept reference to the cached "View" display list for the page,
// building+caching it on first call. Caller must fz_drop_display_list when done.
// must be called with pi->renderLock held (this both protects pi->displayList
// and serializes the page-running done by fz_new_display_list_from_page).
static fz_display_list* GetOrBuildPageDisplayList(FzPageInfo* pi, fz_context* ctx) {
    if (!pi->displayList) {
        fz_display_list* list = nullptr;
        fz_try(ctx) {
            list = fz_new_display_list_from_page(ctx, pi->page);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            list = nullptr;
        }
        pi->displayList = list;
    }
    if (!pi->displayList) {
        return nullptr;
    }
    return fz_keep_display_list(ctx, pi->displayList);
}

static RenderedBitmap* BlitRegionFromFollowThemePageBitmap(const RenderedBitmap* pageBmp, fz_irect pageDevBounds,
                                                           fz_irect tileDevBounds) {
    if (!pageBmp || !pageBmp->GetBitmap()) {
        return nullptr;
    }
    fz_irect copy = fz_intersect_irect(tileDevBounds, pageDevBounds);
    if (fz_is_empty_irect(copy)) {
        return nullptr;
    }
    int w = copy.x1 - copy.x0;
    int h = copy.y1 - copy.y0;
    if (w <= 0 || h <= 0) {
        return nullptr;
    }
    HDC srcDc = CreateCompatibleDC(nullptr);
    if (!srcDc) {
        return nullptr;
    }
    HGDIOBJ srcOld = SelectObject(srcDc, pageBmp->GetBitmap());
    HDC dstDc = CreateCompatibleDC(nullptr);
    if (!dstDc) {
        SelectObject(srcDc, srcOld);
        DeleteDC(srcDc);
        return nullptr;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dstHb = CreateDIBSection(dstDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dstHb) {
        SelectObject(srcDc, srcOld);
        DeleteDC(srcDc);
        DeleteDC(dstDc);
        return nullptr;
    }
    HGDIOBJ dstOld = SelectObject(dstDc, dstHb);
    int srcX = copy.x0 - pageDevBounds.x0;
    int srcY = copy.y0 - pageDevBounds.y0;
    BitBlt(dstDc, 0, 0, w, h, srcDc, srcX, srcY, SRCCOPY);
    SelectObject(srcDc, srcOld);
    SelectObject(dstDc, dstOld);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    return new RenderedBitmap(dstHb, Size(w, h));
}

static void CacheLaTeXFollowThemePageProbe(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo, fz_page* page,
                                           fz_display_list* list);
static bool FollowThemePageUsesBitmapRecolor(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo, fz_page* page);
static bool FollowThemePageWantsLatexFigureSkipRects(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo,
                                                     fz_page* page);
static bool FollowThemeWholeTileBitmapBlockedForPaperScan(fz_context* ctx, EngineMupdf* engine, FzPageInfo* pageInfo,
                                                          fz_page* page);

static RenderedBitmap* GetOrBuildLaTeXFollowThemePageBitmap(EngineMupdf* engine, FzPageInfo* pageInfo, fz_context* ctx,
                                                            fz_page* page, fz_display_list* list, float zoom,
                                                            int rotation, const DarkModeProfile* darkProfile) {
    if (!engine || !pageInfo || !page || !list || !darkProfile) {
        return nullptr;
    }
    u32 profileHash = PdfDarkModeComputeProfileHash(darkProfile);
    if (pageInfo->followThemePageBitmap && pageInfo->followThemePageBitmapZoom == zoom &&
        pageInfo->followThemePageBitmapRotation == rotation &&
        pageInfo->followThemePageBitmapProfileHash == profileHash) {
        return pageInfo->followThemePageBitmap;
    }
    DropFollowThemePageBitmapCache(pageInfo);

    fz_rect pRect = fz_bound_page(ctx, page);
    fz_matrix ctm = engine->viewctm(page, zoom, rotation);
    fz_irect ibounds = fz_round_rect(fz_transform_rect(pRect, ctm));
    i64 pixels = (i64)(ibounds.x1 - ibounds.x0) * (ibounds.y1 - ibounds.y0);
    static constexpr i64 kMaxPixels = 28000000;
    if (pixels <= 0 || pixels > kMaxPixels) {
        return nullptr;
    }

    fz_colorspace* csRgb = fz_device_rgb(ctx);
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    RenderedBitmap* bitmap = nullptr;
    fz_var(dev);
    fz_var(pix);
    fz_var(bitmap);
    fz_try(ctx) {
        pix = fz_new_pixmap_with_bbox(ctx, csRgb, ibounds, nullptr, 1);
        fz_clear_pixmap_with_value(ctx, pix, 0xff);
        dev = fz_new_draw_device(ctx, ctm, pix);
        fz_run_display_list(ctx, list, dev, fz_identity, pRect, nullptr);
        fz_close_device(ctx, dev);
        dev = nullptr;
        if (engine->CadEnhanceActive() && engine->cadRasterDominant && pix) {
            PdfCadEnhancePixmap(ctx, pix, zoom, true);
        }
        bool textScanBinarize = FollowThemeWholeTileBitmapBlockedForPaperScan(ctx, engine, pageInfo, page);
        if (textScanBinarize) {
            fz_pixmap* bin = PdfDarkModeProcessGovernmentPaperPixmap(ctx, pix, darkProfile->palette);
            if (bin && bin != pix) {
                fz_drop_pixmap(ctx, pix);
                pix = bin;
            }
        }
        bitmap = NewRenderedFzPixmap(ctx, pix);
        if (bitmap && !textScanBinarize) {
            Vec<Rect> skipRects;
            Vec<Rect>* skipPtr = nullptr;
            if (PdfFollowThemePreservesEmbeddedImageColors()) {
                if (engine->followThemeDocBitmapRecolor == 1 && !pageInfo->contentImagesCollected) {
                    FzCollectImagesFromPageContent(ctx, pageInfo->pageNo, pageInfo, page, nullptr);
                    pageInfo->contentImagesCollected = true;
                    pageInfo->darkLegacySkipHash = 0;
                }
                RectF renderRect = ToRectF(pRect);
                Size bmpSize = bitmap->GetSize();
                engine->GetBitmapRecolorSkipRects(pageInfo->pageNo, zoom, rotation, renderRect, bmpSize, skipRects);
                if (skipRects.Size() > 0) {
                    skipPtr = &skipRects;
                }
            }
            UpdateBitmapColors(bitmap->GetBitmap(), darkProfile->foreground, darkProfile->pageBackground,
                               darkProfile->linkColor, skipPtr);
        }
    }
    fz_always(ctx) {
        if (dev) {
            fz_drop_device(ctx, dev);
        }
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        delete bitmap;
        return nullptr;
    }
    if (!bitmap) {
        return nullptr;
    }
    pageInfo->followThemePageBitmap = bitmap;
    pageInfo->followThemePageBitmapZoom = zoom;
    pageInfo->followThemePageBitmapRotation = rotation;
    pageInfo->followThemePageBitmapProfileHash = profileHash;
    pageInfo->followThemePageBitmapDevBounds = ibounds;
    return bitmap;
}

RectF EngineMupdf::PageContentBox(int pageNo, RenderTarget target) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return RectF();
    }
    auto ctx = Ctx();

    FzPageInfo* pageInfo = GetFzPageInfo(pageNo, false);
    if (!pageInfo) {
        // maybe should return a dummy size. not sure how this
        // will play with layout. The page should fail to render
        // since the doc is broken and page is missing
        return RectF();
    }

    RectF mediabox = pageInfo->mediabox;

    fz_rect pagerect;
    fz_display_list* keptList = nullptr;
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;
    {
        ReflowUiDocLock docGuard(this, reflowLoading);
        ReflowRenderLock renderGuard(this, reflowLoading);
        pagerect = fz_bound_page(ctx, pageInfo->page);
        keptList = GetOrBuildPageDisplayList(pageInfo, ctx);
    }
    if (!keptList) {
        return mediabox;
    }

    // Lock-free when reflow is done; during progressive reflow replay still hits
    // the shared fz_store (image decode) and must not run alongside chapter counting.
    fz_cookie fzcookie{};
    fz_rect rect = fz_empty_rect;
    fz_device* dev = nullptr;
    fz_var(dev);
    ReflowUiDocLock replayGuard(this, reflowLoading);
    ReflowRenderLock replayRenderGuard(this, reflowLoading);
    fz_try(ctx) {
        dev = fz_new_bbox_device(ctx, &rect);
        fz_run_display_list(ctx, keptList, dev, fz_identity, pagerect, &fzcookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_display_list(ctx, keptList);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return mediabox;
    }

    if (fz_is_infinite_rect(rect)) {
        return mediabox;
    }

    RectF rect2 = ToRectF(rect);
    return rect2.Intersect(mediabox);
}

RectF EngineMupdf::Transform(const RectF& rect, int pageNo, float zoom, int rotation, bool inverse) {
    if (zoom <= 0) {
        const char* name = FilePath();
        if (!name) {
            name = "";
        }
        logf("EngineMupdf::Transform: doc: %s, pageNo: %d, zoom: %.2f\n", name, pageNo, zoom);
        zoom = 1;
    }
    fz_matrix ctm = viewctm(pageNo, zoom, rotation);
    if (inverse) {
        ctm = fz_invert_matrix(ctm);
    }
    fz_rect rect2 = ToFzRect(rect);
    rect2 = fz_transform_rect(rect2, ctm);
    return ToRectF(rect2);
}

// Min rendered size (device pixels) for preserving original image colors in dark mode.
// Smaller embedded images (icons, bullets, ornaments) still use the page recolor filter.
// Minimum size for preserved PDF images (see GetPreservePdfImagesMinSize(), default 72).

// Keep only the largest preserve region per page, matching FinalizeTileSkipRects intent.
static void DarkLegacySkipKeepLargestArtwork(FzPageInfo* pageInfo) {
    Vec<Rect>& skipRects = pageInfo->darkLegacySkipDevAbs;
    if (skipRects.Size() <= 1) {
        return;
    }
    int bestIdx = 0;
    i64 bestArea = 0;
    for (int i = 0; i < skipRects.Size(); i++) {
        i64 a = (i64)skipRects.at(i).dx * skipRects.at(i).dy;
        if (a > bestArea) {
            bestArea = a;
            bestIdx = i;
        }
    }
    Rect keep = skipRects.at(bestIdx);
    skipRects.Clear();
    skipRects.Append(keep);
    pageInfo->darkLegacyArtworkPageBottom = 0.f;
}

static int DarkLegacyTileFindRoot(Vec<int>& parent, int i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

// Some PDFs slice a single illustration into a grid of image tiles. Judged one
// at a time each tile is just a small picture: the page-dominance rule never
// fires, the decorative-strip rule throws away the thin edge pieces, and
// DarkLegacySkipKeepLargestArtwork keeps only one tile of the set. Group tiles
// that touch into one region and let the rest of the code treat that region as
// the image.
//
// A group only counts as a tiling if its tiles actually fill its bounding box.
// Without that check, scattered figures on a busy page would chain together
// into one huge region covering unrelated page content.
//
// Fills groupOf (one entry per rect, indexing groupRect) and groupRect. Rects
// that don't group land in a group of their own, so callers see no difference
// from the ungrouped case.
static void DarkLegacyGroupImageTiles(const Vec<RectF>& rects, float tol, Vec<int>& groupOf, Vec<RectF>& groupRect) {
    groupOf.Clear();
    groupRect.Clear();
    int n = rects.Size();
    Vec<int> parent;
    for (int i = 0; i < n; i++) {
        parent.Append(i);
    }
    for (int i = 0; i < n; i++) {
        if (rects[i].IsEmpty()) {
            continue;
        }
        RectF grown = rects[i];
        grown.Inflate(tol, tol);
        for (int j = i + 1; j < n; j++) {
            if (rects[j].IsEmpty() || grown.Intersect(rects[j]).IsEmpty()) {
                continue;
            }
            int ri = DarkLegacyTileFindRoot(parent, i);
            int rj = DarkLegacyTileFindRoot(parent, j);
            if (ri != rj) {
                parent[ri] = rj;
            }
        }
    }

    Vec<RectF> bbox;
    Vec<float> tileArea;
    Vec<int> nTiles;
    for (int i = 0; i < n; i++) {
        bbox.Append(RectF());
        tileArea.Append(0.f);
        nTiles.Append(0);
    }
    for (int i = 0; i < n; i++) {
        if (rects[i].IsEmpty()) {
            continue;
        }
        int r = DarkLegacyTileFindRoot(parent, i);
        bbox[r] = nTiles[r] == 0 ? rects[i] : bbox[r].Union(rects[i]);
        tileArea[r] += rects[i].dx * rects[i].dy;
        nTiles[r]++;
    }
    for (int i = 0; i < n; i++) {
        if (nTiles[i] < 2) {
            continue;
        }
        float bboxArea = bbox[i].dx * bbox[i].dy;
        if (bboxArea <= 0.f || tileArea[i] < bboxArea * 0.85f) {
            nTiles[i] = 0; // scattered images, not a tiling - dissolve the group
        }
    }

    Vec<int> rootToGroup;
    for (int i = 0; i < n; i++) {
        rootToGroup.Append(-1);
    }
    for (int i = 0; i < n; i++) {
        int r = DarkLegacyTileFindRoot(parent, i);
        if (rects[i].IsEmpty() || nTiles[r] < 2) {
            groupOf.Append(groupRect.Size());
            groupRect.Append(rects[i]);
            continue;
        }
        if (rootToGroup[r] < 0) {
            rootToGroup[r] = groupRect.Size();
            groupRect.Append(bbox[r]);
        }
        groupOf.Append(rootToGroup[r]);
    }
}

static u32 DarkLegacySkipHash(FzPageInfo* pageInfo, float zoom, int rotation) {
    u32 h = PdfDarkModeComputeOptionsHash();
    h = h * 31 + (u32)(zoom * 1000.f);
    h = h * 31 + (u32)rotation;
    h = h * 31 + (u32)GetPreservePdfImagesMinSize();
    h = h * 31 + (u32)GetPreservePdfImagesInDarkMode();
    h = h * 31 + (u32)PdfFollowThemePreservesEmbeddedImageColors();
    h = h * 31 + (u32)(pageInfo ? pageInfo->images.Size() : 0);
    return h;
}

static void BuildPageDarkLegacySkipRects(EngineMupdf* engine, FzPageInfo* pageInfo, float zoom, int rotation,
                                         u32 hash) {
    pageInfo->darkLegacySkipDevAbs.Clear();
    pageInfo->darkLegacyArtworkPageBottom = 0.f;
    pageInfo->darkLegacySkipHash = hash;
    pageInfo->darkLegacySkipZoom = zoom;
    pageInfo->darkLegacySkipRotation = rotation;

    if (!pageInfo->page || pageInfo->images.Size() == 0) {
        return;
    }
    fz_context* ctx = engine->Ctx();
    fz_page* page = pageInfo->page;
    bool preserveAllImageColors = PdfFollowThemePreservesEmbeddedImageColors();

    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        pageArea = 1.f;
    }

    // Whole-tile bitmap recolor: skip only photo interiors on Image Conversion picture books.
    if (preserveAllImageColors && FollowThemePageUsesBitmapRecolor(engine, ctx, pageInfo, page)) {
        if (engine->pdfdoc && PdfDarkModePdfMetadataSuggestsImageConversionPictureBook(ctx, engine->pdfdoc)) {
            fz_matrix ctm = engine->viewctm(page, zoom, rotation);
            for (int imgIdx = 0; imgIdx < pageInfo->images.Size(); imgIdx++) {
                FitzPageImageInfo* img = pageInfo->images.at(imgIdx);
                RectF imgPage = ToRectF(img->rect);
                fz_image* image = FzGetKeptPageImage(ctx, pageInfo, imgIdx);
                if (image && image->w > 0 && image->h > 0) {
                    imgPage = PdfDarkModeClampImagePageRect(imgPage, image->w, image->h);
                } else {
                    imgPage = PdfDarkModeCapUnknownImagePageRect(imgPage, pageBounds.dy);
                }
                RectF imgOnPage = imgPage.Intersect(pageBounds);
                float coverage = (imgOnPage.dx * imgOnPage.dy) / pageArea;
                if (coverage >= kMaxPreserveImagePageCoverage && image &&
                    PdfDarkModeImageHasPreservablePhotoRects(ctx, image)) {
                    PdfDarkModeAppendImagePhotoSkipDevRects(ctx, image, imgOnPage, ctm, pageInfo->darkLegacySkipDevAbs);
                }
                if (image) {
                    fz_drop_image(ctx, image);
                }
            }
        }
        return;
    }
    fz_matrix ctm = engine->viewctm(page, zoom, rotation);
    bool latexFigureSkips =
        preserveAllImageColors && FollowThemePageWantsLatexFigureSkipRects(engine, ctx, pageInfo, page);
    int minDx = preserveAllImageColors ? 0 : MinPreservePdfImageSizePx();
    int minDy = minDx;

    int nImages = pageInfo->images.Size();
    Vec<RectF> imgPageRects;
    Vec<RectF> imgOnPageRects;
    for (int imgIdx = 0; imgIdx < nImages; imgIdx++) {
        RectF imgPage = ToRectF(pageInfo->images.at(imgIdx)->rect);
        fz_image* image = FzGetKeptPageImage(ctx, pageInfo, imgIdx);
        if (image && image->w > 0 && image->h > 0) {
            imgPage = PdfDarkModeClampImagePageRect(imgPage, image->w, image->h);
        } else {
            imgPage = PdfDarkModeCapUnknownImagePageRect(imgPage, pageBounds.dy);
        }
        if (image) {
            fz_drop_image(ctx, image);
        }
        imgPageRects.Append(imgPage);
        imgOnPageRects.Append(imgPage.Intersect(pageBounds));
    }
    // Tiles of one sliced illustration are judged as the region they form.
    // Latex figure skips keep per-image rects so neighboring figures don't merge.
    Vec<int> groupOf;
    Vec<RectF> groupRect;
    if (latexFigureSkips) {
        for (int i = 0; i < nImages; i++) {
            groupOf.Append(i);
            groupRect.Append(imgOnPageRects[i]);
        }
    } else {
        float tileTol = std::max(1.0f, std::min(pageBounds.dx, pageBounds.dy) * 0.005f);
        DarkLegacyGroupImageTiles(imgOnPageRects, tileTol, groupOf, groupRect);
    }
    Vec<int> groupAppended;
    for (int i = 0; i < groupRect.Size(); i++) {
        groupAppended.Append(0);
    }

    for (int imgIdx = 0; imgIdx < nImages; imgIdx++) {
        RectF imgPage = imgPageRects[imgIdx];
        int groupIdx = groupOf[imgIdx];
        RectF imgOnPage = groupRect[groupIdx];
        fz_image* image = FzGetKeptPageImage(ctx, pageInfo, imgIdx);
        float coverage = (imgOnPage.dx * imgOnPage.dy) / pageArea;
        if (latexFigureSkips) {
            fz_irect devLatex = fz_round_rect(fz_transform_rect(ToFzRect(imgOnPage), ctm));
            int latexDx = devLatex.x1 - devLatex.x0;
            int latexDy = devLatex.y1 - devLatex.y0;
            if (latexDx >= 12 && latexDy >= 12) {
                Rect r(devLatex.x0, devLatex.y0, latexDx, latexDy);
                if (!r.IsEmpty() && !groupAppended[groupIdx]) {
                    groupAppended[groupIdx] = 1;
                    pageInfo->darkLegacySkipDevAbs.Append(r);
                }
            }
            if (image) {
                fz_drop_image(ctx, image);
            }
            continue;
        }
        // Light eye-care match theme: preserve embedded photos at any coverage above min size.
        // Without this, UpdateBitmapColors + sharpenAchromatic floods white highlights in photos.
        if (!preserveAllImageColors && ThemeUsesEyeCareChrome() && image) {
            DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, false);
            bool isPhoto = analysis.kind == DarkImageKind::Photo || PdfDarkModeFeaturesLookLikePhoto(analysis.features);
            fz_irect devPhoto = fz_round_rect(fz_transform_rect(ToFzRect(imgOnPage), ctm));
            int photoDx = devPhoto.x1 - devPhoto.x0;
            int photoDy = devPhoto.y1 - devPhoto.y0;
            if (isPhoto && photoDx >= minDx && photoDy >= minDy) {
                Rect r(devPhoto.x0, devPhoto.y0, photoDx, photoDy);
                if (!r.IsEmpty() && !groupAppended[groupIdx]) {
                    groupAppended[groupIdx] = 1;
                    pageInfo->darkLegacySkipDevAbs.Append(r);
                }
                fz_drop_image(ctx, image);
                continue;
            }
        }
        if (!preserveAllImageColors && PdfDarkModePageDominantImageRecolors(ctx, image, coverage)) {
            // full-bleed backgrounds / scans recolor with the page; artwork
            // that happens to fill the page does not
            if (ThemeUsesEyeCareChrome() && image) {
                DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, false);
                if (analysis.kind == DarkImageKind::Photo || PdfDarkModeFeaturesLookLikePhoto(analysis.features)) {
                    fz_irect dev = fz_round_rect(fz_transform_rect(ToFzRect(imgOnPage), ctm));
                    Rect r(dev.x0, dev.y0, dev.x1 - dev.x0, dev.y1 - dev.y0);
                    if (!r.IsEmpty() && !groupAppended[groupIdx]) {
                        groupAppended[groupIdx] = 1;
                        pageInfo->darkLegacySkipDevAbs.Append(r);
                    }
                    fz_drop_image(ctx, image);
                    continue;
                }
            }
            if (image) {
                fz_drop_image(ctx, image);
            }
            continue;
        }
        if (!preserveAllImageColors && PdfDarkModeIsDecorativeStripImage(imgOnPage, pageBounds)) {
            if (image) {
                fz_drop_image(ctx, image);
            }
            continue;
        }
        if (preserveAllImageColors && PdfDarkModeIsDecorativeStripImage(imgOnPage, pageBounds)) {
            fz_drop_image(ctx, image);
            continue;
        }
        fz_irect fullDev = fz_round_rect(fz_transform_rect(ToFzRect(imgPage), ctm));
        int fullDx = fullDev.x1 - fullDev.x0;
        int fullDy = fullDev.y1 - fullDev.y0;
        if (fullDx < minDx || fullDy < minDy) {
            if (image) {
                fz_drop_image(ctx, image);
            }
            continue;
        }
        if (!preserveAllImageColors && !image) {
            continue;
        }
        if (preserveAllImageColors && !image) {
            continue;
        }
        if (!preserveAllImageColors &&
            !PdfDarkModeShouldPreserveEmbeddedImageRect(ctx, image, coverage, fullDx, fullDy)) {
            fz_drop_image(ctx, image);
            continue;
        }
        // Wide bboxes often span a layout column; only preserve if clearly a dark painting.
        if (!preserveAllImageColors && imgOnPage.dx > pageBounds.dx * 0.44f &&
            !PdfDarkModeImageLooksLikeDarkArtwork(ctx, image, coverage)) {
            fz_drop_image(ctx, image);
            continue;
        }
        if (preserveAllImageColors && coverage >= kMaxPreserveImagePageCoverage &&
            !PdfDarkModeImageIsConfirmedArtwork(ctx, image, coverage, fullDx, fullDy)) {
            fz_drop_image(ctx, image);
            continue;
        }
        fz_irect dev = fz_round_rect(fz_transform_rect(ToFzRect(imgOnPage), ctm));
        Rect r(dev.x0, dev.y0, dev.x1 - dev.x0, dev.y1 - dev.y0);
        // Tiles of one sliced illustration share a region - append it once.
        if (!r.IsEmpty() && !groupAppended[groupIdx]) {
            groupAppended[groupIdx] = 1;
            pageInfo->darkLegacySkipDevAbs.Append(r);
            if (!preserveAllImageColors) {
                float bottom = imgOnPage.y + imgOnPage.dy;
                if (bottom > pageInfo->darkLegacyArtworkPageBottom) {
                    pageInfo->darkLegacyArtworkPageBottom = bottom;
                }
            }
        }
        if (image) {
            fz_drop_image(ctx, image);
        }
    }
    if (!preserveAllImageColors) {
        DarkLegacySkipKeepLargestArtwork(pageInfo);
    }
}

static void BuildFollowThemeArtworkBounds(fz_context* ctx, FzPageInfo* pageInfo, fz_page* page, const RectF& pageBounds,
                                          Vec<RectF>& out) {
    out.Clear();
    if (!pageInfo || pageBounds.IsEmpty()) {
        return;
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        return;
    }
    if (!pageInfo->contentImagesCollected) {
        FzCollectImagesFromPageContent(ctx, pageInfo->pageNo, pageInfo, page, nullptr);
        pageInfo->contentImagesCollected = true;
    }
    for (int i = 0; i < pageInfo->images.Size(); i++) {
        FitzPageImageInfo* img = pageInfo->images.at(i);
        if (!img) {
            continue;
        }
        RectF imgOnPage = ToRectF(img->rect).Intersect(pageBounds);
        if (PdfDarkModeIsSubstantialFollowThemeArtwork(imgOnPage, pageArea)) {
            out.Append(imgOnPage);
        }
    }
}

void EngineMupdf::GetBitmapRecolorSkipRects(int pageNo, float zoom, int rotation, const RectF& renderPageRect,
                                            Size bmpSize, Vec<Rect>& skipRects) {
    // pageNo is a raw engine page (RenderPage maps display->engine before calling).
    skipRects.Clear();
    if (renderPageRect.IsEmpty() || bmpSize.dx <= 0 || bmpSize.dy <= 0) {
        return;
    }
    FzPageInfo* pageInfo = GetFzPageInfo(pageNo, false);
    if (!pageInfo || !pageInfo->page) {
        return;
    }
    bool preserveAllImageColors = PdfFollowThemePreservesEmbeddedImageColors();
    if ((PdfShouldCollectContentImages() || preserveAllImageColors ||
         (PdfSmartModePreservesEmbeddedImages() && !ThemeUsesDarkChrome())) &&
        !pageInfo->contentImagesCollected) {
        fz_context* ctx = Ctx();
        FzCollectImagesFromPageContent(ctx, pageNo, pageInfo, pageInfo->page, nullptr);
        pageInfo->contentImagesCollected = true;
        pageInfo->darkLegacySkipHash = 0;
    }
    if (pageInfo->images.Size() == 0) {
        return;
    }

    u32 hash = DarkLegacySkipHash(pageInfo, zoom, rotation);
    if (pageInfo->darkLegacySkipHash != hash || pageInfo->darkLegacySkipZoom != zoom ||
        pageInfo->darkLegacySkipRotation != rotation) {
        BuildPageDarkLegacySkipRects(this, pageInfo, zoom, rotation, hash);
    }

    if (!preserveAllImageColors && pageInfo->darkLegacyArtworkPageBottom > 0.f &&
        renderPageRect.y >= pageInfo->darkLegacyArtworkPageBottom) {
        return;
    }

    fz_page* page = pageInfo->page;
    fz_rect pRect = ToFzRect(renderPageRect);
    fz_matrix ctm = viewctm(page, zoom, rotation);
    fz_irect ibounds = fz_round_rect(fz_transform_rect(pRect, ctm));

    Rect tileAbs(ibounds.x0, ibounds.y0, ibounds.x1 - ibounds.x0, ibounds.y1 - ibounds.y0);
    for (Rect& skipAbs : pageInfo->darkLegacySkipDevAbs) {
        Rect clipped = skipAbs.Intersect(tileAbs);
        if (clipped.IsEmpty()) {
            continue;
        }
        Rect local(clipped.x - ibounds.x0, clipped.y - ibounds.y0, clipped.dx, clipped.dy);
        local.Inflate(3, 3);
        local = local.Intersect(Rect(0, 0, bmpSize.dx, bmpSize.dy));
        if (!local.IsEmpty()) {
            skipRects.Append(local);
        }
    }
}

bool EngineMupdf::CadEnhanceActive() const {
    if (!pdfdoc) {
        return false;
    }
    CadDetectResult detect;
    detect.enable = cadDetectEnable;
    detect.score = cadDetectScore;
    return CadEnhanceEnabledForEngine(detect, cadEnhanceOverride);
}

bool EngineMupdf::CadEnhanceUseHairlineBoost() const {
    return cadHairlineVector;
}

void EngineMupdf::RunCadDetection() {
    if (!pdfdoc || cadDetectDone) {
        return;
    }
    CadDetectResult res = DetectCadPdf(Ctx(), pdfdoc);
    cadDetectEnable = res.enable;
    cadDetectScore = res.score;
    cadRasterDominant = res.rasterDominant;
    cadHairlineVector = res.hairlineVector;
    cadDetectDone = true;
    if (cadDetectEnable) {
        logfa("CAD enhance detect: score=%d reason=%s raster=%d hairline=%d\n", cadDetectScore,
              CadEnhanceReasonName(res.reason), (int)cadRasterDominant, (int)cadHairlineVector);
    } else if (cadDetectScore >= 30) {
        logfa("CAD enhance not enabled: score=%d hairline=%d (auto threshold 60, or metadata+45)\n", cadDetectScore,
              (int)cadHairlineVector);
    }
}

void EngineMupdf::ToggleCadEnhanceOverride() {
    switch (cadEnhanceOverride) {
        case CadEnhanceOverride::Unset:
            cadEnhanceOverride = CadEnhanceActive() ? CadEnhanceOverride::ForceOff : CadEnhanceOverride::ForceOn;
            break;
        case CadEnhanceOverride::ForceOn:
            cadEnhanceOverride = CadEnhanceOverride::ForceOff;
            break;
        case CadEnhanceOverride::ForceOff:
            cadEnhanceOverride = CadEnhanceOverride::ForceOn;
            break;
    }
}

static fz_device* WrapViewRenderDevice(fz_context* ctx, EngineMupdf* engine, fz_device* baseDev,
                                       const DarkModeProfile* darkProfile, FzPageInfo* pageInfo,
                                       fz_display_list* keptList, DarkModeReplayState* replayState, float zoom) {
    fz_device* renderDev = baseDev;
    if (darkProfile && DarkModeProfileUsesObjectLevel(darkProfile) && engine->pdfdoc && keptList) {
        u32 optionsHash = darkProfile->hash;
        const DarkModePalette& palette = darkProfile->palette;
        DarkModePageAnalysis* analysis =
            PdfDarkModeGetOrBuildAnalysis(ctx, pageInfo, keptList, optionsHash, engine->darkModeEngineCache);
        if (analysis) {
            renderDev = PdfDarkModeWrapDevice(ctx, renderDev, analysis, &palette, replayState,
                                              engine->darkModeEngineCache, optionsHash, darkProfile->debugOverlay);
        }
    }
    if (engine->CadEnhanceActive()) {
        CadEnhanceRenderOpts opts{};
        opts.zoom = zoom;
        opts.hairlineVector = engine->CadEnhanceUseHairlineBoost();
        renderDev = PdfCadEnhanceWrapDevice(ctx, renderDev, opts);
    }
    return renderDev;
}

static bool FollowThemeProbeUsesBitmapRecolor(FollowThemeScanProbe probe) {
    return probe == FollowThemeScanProbe::PureScan || probe == FollowThemeScanProbe::BitmapRecolor;
}

// RAZ-style PDFs are one full-bleed colorful image per page (few text/vector ops), so the
// structural PureScan probe matches true paper scans. Whole-tile bitmap recolor then skips
// that full-page image → the page looks completely unprocessed ("original"). Treat colorful
// full-bleed art as Mixed so FollowTheme wrap + paper softening runs instead.
static bool FollowThemePureScanIsColorfulPictureBook(fz_context* ctx, EngineMupdf* engine, FzPageInfo* pageInfo,
                                                     fz_page* page, float maxImageCoverage) {
    if (!ctx || !engine || !pageInfo || !page) {
        return false;
    }
    DarkModeOptions opts = PdfDarkModeCurrentOptions();
    if (maxImageCoverage < opts.scanImageCoverageThreshold) {
        return false;
    }
    if (!pageInfo->contentImagesCollected) {
        FzCollectImagesFromPageContent(ctx, pageInfo->pageNo, pageInfo, page, nullptr);
        pageInfo->contentImagesCollected = true;
    }
    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        return false;
    }
    for (int i = 0; i < pageInfo->images.Size(); i++) {
        FitzPageImageInfo* img = pageInfo->images.at(i);
        if (!img) {
            continue;
        }
        RectF imgOnPage = ToRectF(img->rect).Intersect(pageBounds);
        float coverage = (imgOnPage.dx * imgOnPage.dy) / pageArea;
        if (coverage < opts.scanImageCoverageThreshold) {
            continue;
        }
        fz_image* image = FzGetKeptPageImage(ctx, pageInfo, i);
        if (!image) {
            continue;
        }
        // Classify with the 128px thumbnail first. Office/text scans must not
        // fall through to ConfirmedArtwork or the 1400px photo-rect probe —
        // a red 公文 header can look "colorful" and would send the page to
        // FollowThemeV2 wrap (native 1–6MP remap, "Please wait - rendering...").
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, true);
        if (analysis.kind == DarkImageKind::FullPageScan ||
            PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(analysis.features) ||
            PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(analysis.features) ||
            PdfDarkModeFeaturesLookLikeGovernmentPaperScan(analysis.features)) {
            fz_drop_image(ctx, image);
            continue;
        }
        bool art = PdfDarkModeImageIsConfirmedArtwork(ctx, image, coverage, (int)(imgOnPage.dx + 0.5f),
                                                      (int)(imgOnPage.dy + 0.5f));
        if (!art && analysis.kind != DarkImageKind::FullPageScan) {
            art = PdfDarkModeFeaturesLookLikePhoto(analysis.features) ||
                  PdfDarkModeShouldPreserveImageFeatures(analysis.features, coverage);
        }
        if (!art && PdfDarkModeImageHasPreservablePhotoRects(ctx, image)) {
            art = true;
        }
        fz_drop_image(ctx, image);
        if (art) {
            return true;
        }
    }
    return false;
}

static FollowThemeScanProbe FollowThemeRefinePureScanForPictureBook(fz_context* ctx, EngineMupdf* engine,
                                                                    FzPageInfo* pageInfo, fz_page* page,
                                                                    FollowThemeScanProbe probe,
                                                                    const FollowThemePageProbeStats& st) {
    if (probe != FollowThemeScanProbe::PureScan) {
        return probe;
    }
    if (engine->pdfdoc && PdfDarkModePdfMetadataSuggestsImageConversionPictureBook(ctx, engine->pdfdoc) &&
        st.maxImageCoverage >= 0.88f) {
        return FollowThemeScanProbe::Mixed;
    }
    if (FollowThemePureScanIsColorfulPictureBook(ctx, engine, pageInfo, page, st.maxImageCoverage)) {
        return FollowThemeScanProbe::Mixed;
    }
    return probe;
}

static fz_image* FollowThemePageDominantBleedImage(fz_context* ctx, EngineMupdf* engine, FzPageInfo* pageInfo,
                                                   fz_page* page, float minCoverage) {
    if (!ctx || !engine || !pageInfo || !page) {
        return nullptr;
    }
    if (!pageInfo->contentImagesCollected) {
        FzCollectImagesFromPageContent(ctx, pageInfo->pageNo, pageInfo, page, nullptr);
        pageInfo->contentImagesCollected = true;
    }
    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        return nullptr;
    }
    fz_image* best = nullptr;
    float bestCov = 0.f;
    for (int i = 0; i < pageInfo->images.Size(); i++) {
        FitzPageImageInfo* img = pageInfo->images.at(i);
        if (!img) {
            continue;
        }
        RectF imgOnPage = ToRectF(img->rect).Intersect(pageBounds);
        float coverage = (imgOnPage.dx * imgOnPage.dy) / pageArea;
        if (coverage < minCoverage || coverage <= bestCov) {
            continue;
        }
        fz_image* image = FzGetKeptPageImage(ctx, pageInfo, i);
        if (!image) {
            continue;
        }
        if (best) {
            fz_drop_image(ctx, best);
        }
        best = image;
        bestCov = coverage;
    }
    return best;
}

static bool FollowThemePageLooksLikeNotebookIllustration(fz_context* ctx, EngineMupdf* engine, FzPageInfo* pageInfo,
                                                         fz_page* page, float maxCoverage) {
    if (!ctx || !engine || !pageInfo || !page) {
        return false;
    }
    (void)maxCoverage;
    if (!pageInfo->contentImagesCollected) {
        FzCollectImagesFromPageContent(ctx, pageInfo->pageNo, pageInfo, page, nullptr);
        pageInfo->contentImagesCollected = true;
    }
    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        return false;
    }
    for (int i = 0; i < pageInfo->images.Size(); i++) {
        FitzPageImageInfo* img = pageInfo->images.at(i);
        if (!img) {
            continue;
        }
        RectF imgOnPage = ToRectF(img->rect).Intersect(pageBounds);
        float coverage = (imgOnPage.dx * imgOnPage.dy) / pageArea;
        if (coverage < 0.75f) {
            continue;
        }
        fz_image* image = FzGetKeptPageImage(ctx, pageInfo, i);
        if (!image) {
            continue;
        }
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, false);
        fz_drop_image(ctx, image);
        return PdfDarkModeFeaturesLookLikeNotebookIllustrationPage(analysis.features);
    }
    return false;
}

static bool FollowThemePageIsWholeTileScanRecolor(fz_context* ctx, EngineMupdf* engine, FzPageInfo* pageInfo,
                                                  fz_page* page, const FollowThemePageProbeStats& st,
                                                  FollowThemeScanProbe rawProbe) {
    if (st.textOps > 2 || st.vectorOps > 2 || st.imageOps < 1) {
        return false;
    }
    bool coverageOk = st.maxImageCoverage >= 0.88f || st.stackedImageCoverage >= 0.85f;
    if (!coverageOk) {
        return false;
    }
    bool colorfulPictureBook =
        FollowThemePureScanIsColorfulPictureBook(ctx, engine, pageInfo, page, st.maxImageCoverage);
    if (colorfulPictureBook) {
        // Mixed full-bleed with colored illustration blocks (小家越住越大): whole-tile recolor.
        // Keep wrap + photo-rect protect for B&W portrait pages (RAZ Abraham Lincoln).
        if (rawProbe == FollowThemeScanProbe::Mixed && st.imageOps <= 4) {
            fz_image* dominant = FollowThemePageDominantBleedImage(ctx, engine, pageInfo, page, 0.75f);
            bool portrait = dominant && PdfDarkModeImageDecodeLooksLikeGrayscalePortrait(ctx, dominant);
            if (dominant) {
                fz_drop_image(ctx, dominant);
            }
            return !portrait;
        }
        return false;
    }
    if (rawProbe == FollowThemeScanProbe::PureScan) {
        return true;
    }
    // Multi-layer print/scan packs (strip + bg + fg) may probe as Mixed.
    return st.imageOps <= 4;
}

static bool FollowThemePageWantsLatexFigureSkipRects(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo,
                                                     fz_page* page) {
    if (!engine || engine->followThemeDocBitmapRecolor != 1) {
        return false;
    }
    CacheLaTeXFollowThemePageProbe(engine, ctx, pageInfo, page, nullptr);
    u8 cached = pageInfo->followThemeScanProbe;
    if (cached == (u8)FollowThemeScanProbe::PureScan || cached == (u8)FollowThemeScanProbe::BitmapRecolor) {
        return false;
    }
    if (cached == (u8)FollowThemeScanProbe::Mixed) {
        return true;
    }
    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    FollowThemePageProbeStats st{};
    FollowThemeScanProbe rawProbe = PdfDarkModeProbeFollowThemeScanPage(ctx, page, pageBounds, &st);
    return !FollowThemePageIsWholeTileScanRecolor(ctx, engine, pageInfo, page, st, rawProbe);
}

static void CacheLaTeXFollowThemePageProbe(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo, fz_page* page,
                                           fz_display_list* list) {
    if (!engine || !pageInfo || !page) {
        return;
    }
    if (pageInfo->followThemeScanProbe != 0) {
        return;
    }
    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    FollowThemePageProbeStats st{};
    FollowThemeScanProbe probe = FollowThemeScanProbe::Mixed;
    if (list) {
        probe = PdfDarkModeProbeFollowThemeScanList(ctx, list, pageBounds, &st);
    } else {
        probe = PdfDarkModeProbeFollowThemeScanPage(ctx, page, pageBounds, &st);
    }
    probe = PdfDarkModeLaTeXRefineFollowThemeProbe(probe, st, PdfDarkModeCurrentOptions());
    probe = FollowThemeRefinePureScanForPictureBook(ctx, engine, pageInfo, page, probe, st);
    u8 cacheProbe = (u8)FollowThemeScanProbe::Mixed;
    if (probe == FollowThemeScanProbe::PureScan) {
        cacheProbe = (u8)FollowThemeScanProbe::PureScan;
    } else if (FollowThemeProbeUsesBitmapRecolor(probe)) {
        cacheProbe = (u8)FollowThemeScanProbe::BitmapRecolor;
    }
    pageInfo->followThemeScanProbe = cacheProbe;
}

// Pick body pages for follow-theme probing; skip cover/back scans on textbooks.
static int FollowThemeProbeBodyPageNo(int pageCount, int sampleIndex, int sampleCount) {
    if (pageCount <= 0) {
        return 1;
    }
    if (pageCount <= 3 || sampleCount <= 1) {
        return 1 + sampleIndex;
    }
    int lo = 2;
    int hi = pageCount - 1;
    if (pageCount >= 20) {
        lo = 5;
        hi = pageCount - 4;
    }
    if (lo > hi) {
        lo = hi;
    }
    return lo + (hi - lo) * sampleIndex / (sampleCount - 1);
}

static bool PdfDictHasImageXObject(fz_context* ctx, pdf_obj* res) {
    if (!pdf_is_dict(ctx, res)) {
        return false;
    }
    pdf_obj* xobj = pdf_dict_get(ctx, res, PDF_NAME(XObject));
    if (!pdf_is_dict(ctx, xobj)) {
        return false;
    }
    int n = pdf_dict_len(ctx, xobj);
    for (int i = 0; i < n; i++) {
        pdf_obj* val = pdf_dict_get_val(ctx, xobj, i);
        if (pdf_is_indirect(ctx, val)) {
            val = pdf_resolve_indirect(ctx, val);
        }
        pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
        if (pdf_name_eq(ctx, subtype, PDF_NAME(Image))) {
            return true;
        }
    }
    return false;
}

static bool PdfPageObjHasImageXObject(fz_context* ctx, pdf_document* pdfdoc, int pageNo0) {
    pdf_obj* pageref = nullptr;
    fz_var(pageref);
    bool has = false;
    fz_try(ctx) {
        pageref = pdf_lookup_page_obj(ctx, pdfdoc, pageNo0);
        pdf_obj* res = pdf_dict_get(ctx, pageref, PDF_NAME(Resources));
        if (pdf_is_indirect(ctx, res)) {
            res = pdf_resolve_indirect(ctx, res);
        }
        has = PdfDictHasImageXObject(ctx, res);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        has = true;
    }
    return has;
}

static bool PdfPageHasImageXObject(fz_context* ctx, fz_page* page) {
    if (!page) {
        return false;
    }
    pdf_page* pdfpage = nullptr;
    fz_var(pdfpage);
    fz_try(ctx) {
        pdfpage = pdf_page_from_fz_page(ctx, page);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        return true;
    }
    if (!pdfpage) {
        return false;
    }
    pdf_obj* res = pdf_page_resources(ctx, pdfpage);
    return PdfDictHasImageXObject(ctx, res);
}

static void DetectFollowThemeDocBitmapRecolor(EngineMupdf* engine, fz_context* ctx) {
    if (!engine->pdfdoc) {
        return;
    }
    if (engine->followThemeDocBitmapRecolor == 1 || engine->followThemeContentProbed) {
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    // Full-page image scans (DuXiu / Pdg2Pic / …): whole-tile recolor, skip content probe.
    if (PdfDarkModePdfMetadataSuggestsFullPageScanDoc(ctx, engine->pdfdoc) ||
        PdfDarkModePdfMetadataSuggestsPrintToPdfScanDoc(ctx, engine->pdfdoc)) {
        engine->followThemeDocBitmapRecolor = 1;
        engine->followThemeContentProbed = true;
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    // Paper Capture / Acrobat layout textbooks: wrap path for the whole document.
    // Do NOT content-probe here — body pages can have tens of thousands of vector ops
    // (e.g. Exploring Our World) and probing made Match-theme open multi-second.
    // Scan packs with Pdg2Pic/DuXiu are already handled above as FullPageScan → class 1.
    if (PdfDarkModePdfMetadataSuggestsPaperCaptureDoc(ctx, engine->pdfdoc) ||
        PdfDarkModePdfMetadataSuggestsLayoutPhotoDoc(ctx, engine->pdfdoc)) {
        engine->followThemeDocBitmapRecolor = 2;
        engine->followThemeContentProbed = true;
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    if (!PdfFollowThemePreservesEmbeddedImageColors()) {
        engine->followThemeDocBitmapRecolor = 2;
        engine->followThemeContentProbed = true;
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    if (PdfDarkModePdfMetadataSuggestsBitmapRecolorDoc(ctx, engine->pdfdoc)) {
        engine->followThemeDocBitmapRecolor = 1;
        engine->followThemeContentProbed = true;
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    int pageCount = engine->PageCount();
    if (pageCount <= 0) {
        engine->followThemeDocBitmapRecolor = 2;
        engine->followThemeContentProbed = true;
        SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
        return;
    }
    DarkModeOptions opts = PdfDarkModeCurrentOptions();
    int microTextVotes = 0;
    int bitmapProbeVotes = 0;
    int imageOnlyScanVotes = 0;
    int samples = pageCount < 3 ? pageCount : 3;
    for (int si = 0; si < samples; si++) {
        int pageNo = FollowThemeProbeBodyPageNo(pageCount, si, samples);
        int pageNo0 = pageNo - 1;
        if (!PdfPageObjHasImageXObject(ctx, engine->pdfdoc, pageNo0)) {
            FzPageInfo* pi = engine->pages[pageNo0];
            if (pi) {
                pi->followThemeScanProbe = (u8)FollowThemeScanProbe::Mixed;
            }
            microTextVotes++;
            continue;
        }
        FzPageInfo* pi = engine->GetFzPageInfo(pageNo, true, nullptr, false);
        if (!pi || !pi->page) {
            continue;
        }
        if (!PdfPageHasImageXObject(ctx, pi->page)) {
            pi->followThemeScanProbe = (u8)FollowThemeScanProbe::Mixed;
            microTextVotes++;
            continue;
        }
        RectF pageBounds = pi->mediabox;
        if (pageBounds.IsEmpty()) {
            pageBounds = ToRectF(fz_bound_page(ctx, pi->page));
        }
        FollowThemePageProbeStats st{};
        FollowThemeScanProbe probe = PdfDarkModeProbeFollowThemeScanPage(ctx, pi->page, pageBounds, &st);
        FollowThemeScanProbe rawProbe = probe;
        probe = FollowThemeRefinePureScanForPictureBook(ctx, engine, pi, pi->page, probe, st);
        if (probe == FollowThemeScanProbe::BitmapRecolor || probe == FollowThemeScanProbe::PureScan) {
            bitmapProbeVotes++;
        }
        if (FollowThemePageIsWholeTileScanRecolor(ctx, engine, pi, pi->page, st, rawProbe)) {
            imageOnlyScanVotes++;
        }
        if (FollowThemePageStatsMatchBitmapRecolor(st, opts)) {
            microTextVotes++;
        }
        u8 cacheProbe = FollowThemeProbeUsesBitmapRecolor(probe) ? (u8)FollowThemeScanProbe::BitmapRecolor
                                                                 : (u8)FollowThemeScanProbe::Mixed;
        if (probe == FollowThemeScanProbe::PureScan) {
            cacheProbe = (u8)FollowThemeScanProbe::PureScan;
        }
        pi->followThemeScanProbe = cacheProbe;
    }
    if (imageOnlyScanVotes >= 2 || (imageOnlyScanVotes >= 1 && bitmapProbeVotes >= 1 && microTextVotes == 0)) {
        engine->followThemeDocBitmapRecolor = 1;
    } else {
        engine->followThemeDocBitmapRecolor =
            (bitmapProbeVotes >= 2 || (bitmapProbeVotes >= 1 && microTextVotes >= 2)) ? 1 : 2;
    }
    engine->followThemeContentProbed = true;
    SyncFollowThemeLayoutTextbookFastRemap(engine, ctx);
}

static bool FollowThemeWholeTileBitmapBlockedForPaperScan(fz_context* ctx, EngineMupdf* engine, FzPageInfo* pageInfo,
                                                          fz_page* page) {
    if (pageInfo->followThemePaperBinarize == 1) {
        return false;
    }
    if (pageInfo->followThemePaperBinarize == 2) {
        return true;
    }
    if (!pageInfo->contentImagesCollected) {
        FzCollectImagesFromPageContent(ctx, pageInfo->pageNo, pageInfo, page, nullptr);
        pageInfo->contentImagesCollected = true;
    }
    RectF pageBounds = pageInfo->mediabox;
    if (pageBounds.IsEmpty()) {
        pageBounds = ToRectF(fz_bound_page(ctx, page));
    }
    float pageArea = pageBounds.dx * pageBounds.dy;
    if (pageArea <= 0.f) {
        pageInfo->followThemePaperBinarize = 1;
        return false;
    }
    bool block = false;
    for (int i = 0; i < pageInfo->images.Size(); i++) {
        FitzPageImageInfo* imgInfo = pageInfo->images.at(i);
        if (!imgInfo) {
            continue;
        }
        RectF imgOnPage = ToRectF(imgInfo->rect).Intersect(pageBounds);
        float coverage = (imgOnPage.dx * imgOnPage.dy) / pageArea;
        if (coverage < 0.50f) {
            continue;
        }
        fz_image* image = FzGetKeptPageImage(ctx, pageInfo, i);
        if (!image) {
            continue;
        }
        DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, image, coverage, true);
        fz_drop_image(ctx, image);
        // Gray photocopies: thumbnail lumVar ~0.001 (sparse ink) vs ~0.012 (more gray
        // table ink) flips FullPageTextScanForBinarize. Hard binarize is bright and
        // aliased; UpdateBitmapColors is dimmer but smooth. Keep one look per file.
        if (PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(analysis.features)) {
            if (analysis.features.saturatedPixelRatio < 0.06f && analysis.features.chromaticPixelRatio < 0.08f) {
                continue;
            }
            block = true;
            break;
        }
    }
    if (!block) {
        fz_image* dominant = FollowThemePageDominantBleedImage(ctx, engine, pageInfo, page, 0.50f);
        if (dominant) {
            DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, dominant, 0.97f, true);
            bool textScan = PdfDarkModeFeaturesLookLikeFullPageTextScanForBinarize(analysis.features);
            if (textScan &&
                (analysis.features.saturatedPixelRatio >= 0.06f || analysis.features.chromaticPixelRatio >= 0.08f)) {
                block = true;
            }
            fz_drop_image(ctx, dominant);
        }
    }
    pageInfo->followThemePaperBinarize = block ? 2 : 1;
    return block;
}

static bool FollowThemePageUsesBitmapRecolor(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo,
                                             fz_page* page) {
    // Adobe Image Conversion picture books (RAZ-Z): per-image picture-book remap + photo-rect
    // protect — whole-tile bitmap recolor inverts B&W portraits even with skip rects.
    if (engine->pdfdoc && PdfDarkModePdfMetadataSuggestsImageConversionPictureBook(ctx, engine->pdfdoc)) {
        return false;
    }
    if (engine->followThemeDocBitmapRecolor == 1) {
        CacheLaTeXFollowThemePageProbe(engine, ctx, pageInfo, page, nullptr);
        u8 cached = pageInfo->followThemeScanProbe;
        // Dense-text / PureScan pages: rasterize at view zoom, then either
        // whole-tile recolor or office-paper binarize. Sending paper scans to
        // FollowTheme wrap decodes the native image (~1.4k–6MP) and remaps
        // every pixel — same cost class as OCR, and why flipping scan pages
        // shows "Please wait - rendering...".
        if (cached == (u8)FollowThemeScanProbe::Mixed) {
            return false;
        }
        if (cached == (u8)FollowThemeScanProbe::PureScan || cached == (u8)FollowThemeScanProbe::BitmapRecolor) {
            return true;
        }
        return false;
    }

    if (engine->followThemeDocBitmapRecolor == 2) {
        CacheLaTeXFollowThemePageProbe(engine, ctx, pageInfo, page, nullptr);
        u8 cached = pageInfo->followThemeScanProbe;
        // PureScan only — Mixed keeps wrap for colorful chapter openers.
        if (cached == (u8)FollowThemeScanProbe::PureScan) {
            return true;
        }
        // Image-only white scans misclassified as Mixed: whole-tile recolor beats unprocessed original.
        if (cached == (u8)FollowThemeScanProbe::Mixed) {
            RectF pageBounds = pageInfo->mediabox;
            if (pageBounds.IsEmpty()) {
                pageBounds = ToRectF(fz_bound_page(ctx, page));
            }
            FollowThemePageProbeStats st{};
            FollowThemeScanProbe rawProbe = PdfDarkModeProbeFollowThemeScanPage(ctx, page, pageBounds, &st);
            // InDesign/PageMaker textbooks (Basic Grammar): many vector/text ops — wrap path
            // recolors type cleanly; whole-tile UpdateBitmapColors + sharpen grains anti-aliasing.
            if (st.textOps > 2 || st.vectorOps > 2) {
                return false;
            }
            // Single full-bleed raster with vector/handwritten overlay (小家越住越大): whole-tile recolor.
            if (st.maxImageCoverage >= 0.88f && st.imageOps <= 1) {
                fz_image* dominant = FollowThemePageDominantBleedImage(ctx, engine, pageInfo, page, 0.75f);
                bool portrait = dominant && PdfDarkModeImageDecodeLooksLikeGrayscalePortrait(ctx, dominant);
                bool govtPaper = false;
                if (dominant && !portrait) {
                    DarkImageAnalysis analysis = PdfDarkModeAnalyzeImage(ctx, dominant, st.maxImageCoverage, true);
                    govtPaper = PdfDarkModeFeaturesLookLikeOfficePaperForDarkBinarize(analysis.features);
                }
                if (dominant) {
                    fz_drop_image(ctx, dominant);
                }
                if (!portrait && !govtPaper) {
                    return true;
                }
            }
            if (FollowThemePageIsWholeTileScanRecolor(ctx, engine, pageInfo, page, st, rawProbe)) {
                return true;
            }
            if (FollowThemePageLooksLikeNotebookIllustration(ctx, engine, pageInfo, page, st.maxImageCoverage)) {
                return true;
            }
            return false;
        }
        return false;
    }

    CacheLaTeXFollowThemePageProbe(engine, ctx, pageInfo, page, nullptr);
    u8 cached = pageInfo->followThemeScanProbe;
    if (cached == (u8)FollowThemeScanProbe::PureScan || cached == (u8)FollowThemeScanProbe::BitmapRecolor) {
        return true;
    }
    if (cached == (u8)FollowThemeScanProbe::Mixed) {
        return false;
    }

    if (InterlockedCompareExchange(&engine->pdfFollowThemeProbePending, 0, 0) != 0) {
        RectF pageBounds = pageInfo->mediabox;
        if (pageBounds.IsEmpty()) {
            pageBounds = ToRectF(fz_bound_page(ctx, page));
        }
        FollowThemePageProbeStats st{};
        FollowThemeScanProbe rawProbe = PdfDarkModeProbeFollowThemeScanPage(ctx, page, pageBounds, &st);
        if (FollowThemePageIsWholeTileScanRecolor(ctx, engine, pageInfo, page, st, rawProbe)) {
            return true;
        }
    }

    return false;
}

// FollowThemeV2 otherwise wraps fill_image and remaps the native scan (up to
// 4096px). Office-paper pages should rasterize at view zoom like light mode,
// then binarize — that is the difference between a snappy flip and
// "Please wait - rendering...".
static bool FollowThemePageShouldRasterizeThenRecolor(EngineMupdf* engine, fz_context* ctx, FzPageInfo* pageInfo,
                                                      fz_page* page) {
    if (FollowThemePageUsesBitmapRecolor(engine, ctx, pageInfo, page)) {
        return true;
    }
    return FollowThemeWholeTileBitmapBlockedForPaperScan(ctx, engine, pageInfo, page);
}

RenderedBitmap* EngineMupdf::RenderPage(RenderPageArgs& args) {
    auto ctx = Ctx();
    auto pageNo = MapJoinDisplayPageNo(this, args.pageNo);
    if (pageNo < 1) {
        return nullptr;
    }
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;

    // GetFzPageInfo() returns a cached fz_page owned by the reflow document.
    // A theme relayout can purge and free that page. Keep docLock from before
    // acquiring the pointer until all page/display-list execution is finished,
    // so relayout cannot invalidate the object in the gap between the nested
    // GetFzPageInfo() lock and the later rendering locks.
    ReflowUiDocLock pageLifetimeGuard(this, reflowLoading);

    fz_cookie* fzcookie = nullptr;
    FitzAbortCookie* cookie = nullptr;
    if (args.cookie_out) {
        cookie = new FitzAbortCookie();
        *args.cookie_out = cookie;
        fzcookie = (fz_cookie*)cookie->GetData();
    }

    // Rendering doesn't need the structured-text page. For PDFs, eagerly
    // extracting the hidden text layer makes scanned OCR pages noticeably
    // slower to appear. For reflowable EPUBs it's worse: the stext device
    // walks the entire laid-out chapter (a whole book in anthologies) once
    // per page, making scrolling crawl in large chapters. Text selection and
    // search do a full load on demand.
    bool loadQuick = true;
    // Skip link extraction too: fz_load_html_links walks every flow node of
    // the whole chapter with no per-page pruning - O(chapter words) per page,
    // seconds for anthology chapters. GetElementAtPos loads links on demand
    // when the mouse actually interacts with the page.
    FzPageInfo* pageInfo = GetFzPageInfo(pageNo, loadQuick, fzcookie, /*loadLinks*/ false);
    if (!pageInfo || !pageInfo->page) {
        return nullptr;
    }
    fz_page* page = pageInfo->page;

    // AA level is per-thread-context state since Ctx() clones; no lock needed.
    if (disableAntiAlias) {
        fz_set_aa_level(ctx, 0);
    } else {
        // 8 seems to be the default
        fz_set_aa_level(ctx, 8);
    }

    auto pageRect = args.pageRect;
    auto zoom = args.zoom;
    auto rotation = args.rotation;

    CadMinLineWidthScope cadMinLineWidth(ctx, zoom, CadEnhanceActive(), CadEnhanceUseHairlineBoost());

    // A display list is needed for the object-level Smart Dark renderer, which
    // analyzes and then replays page operations. For normal, original, and
    // legacy recolor modes it only adds an extra full page traversal before the
    // first pixels can be displayed, so render the page directly as upstream
    // does.
    bool useSmartDarkList = (args.target == RenderTarget::View) && !hideAnnotations && args.darkProfile &&
                            args.darkProfile->mode == PageColorMode::SmartDark;
    const DarkModeProfile* darkProfileForPath = args.darkProfile;
    bool followDirectPath = DarkModeProfileUsesFollowThemeDirect(darkProfileForPath);
    bool followV2Path = DarkModeProfileUsesFollowThemeV2(darkProfileForPath);
    bool followThemeBitmapDoc =
        pdfdoc && followThemeDocBitmapRecolor == 1 && !InterlockedCompareExchange(&pdfFollowThemeProbePending, 0, 0);
    bool useBitmapTexList = false;
    bool usePageDisplayList = useSmartDarkList;

    fz_rect pRect;
    fz_matrix ctm;
    fz_irect ibounds;
    fz_display_list* keptList = nullptr;
    {
        ReflowUiDocLock docGuard(this, reflowLoading);
        ReflowRenderLock renderGuard(this, reflowLoading);

        if (pageRect) {
            pRect = ToFzRect(*pageRect);
        } else {
            // TODO(port): use pageInfo->mediabox?
            pRect = fz_bound_page(ctx, page);
        }
        ctm = viewctm(page, zoom, rotation);
        ibounds = fz_round_rect(fz_transform_rect(pRect, ctm));

        if (useSmartDarkList) {
            keptList = GetOrBuildPageDisplayList(pageInfo, ctx);
        } else if ((followDirectPath || followV2Path) && args.darkProfile) {
            bool bitmapRecolor = FollowThemePageShouldRasterizeThenRecolor(this, ctx, pageInfo, page);
            if (bitmapRecolor) {
                keptList = GetOrBuildPageDisplayList(pageInfo, ctx);
                useBitmapTexList = keptList != nullptr;
                usePageDisplayList = useBitmapTexList;
            }
        }
    }

    fz_colorspace* csRgb = fz_device_rgb(ctx);
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    RenderedBitmap* bitmap = nullptr;

    fz_var(dev);
    fz_var(pix);
    fz_var(bitmap);

    if (keptList) {
        // Display-list replay still decodes shared images (JBIG2 etc.) under
        // the hood, and mupdf's image store races on concurrent decode of the
        // same image -- crashes seen in template_image_compose_opt with use-
        // after-free. Hold renderLock to serialize; during progressive EPUB
        // reflow docLock alone serializes (see ReflowRenderLock).
        ReflowUiDocLock replayGuard(this, reflowLoading);
        ReflowRenderLock replayRenderGuard(this, reflowLoading);
        fz_try(ctx) {
            pix = fz_new_pixmap_with_bbox(ctx, csRgb, ibounds, nullptr, 1);
            const DarkModeProfile* darkProfile = args.darkProfile;
            if (useBitmapTexList && !useSmartDarkList) {
                bool blockOfficePaperBmp = FollowThemeWholeTileBitmapBlockedForPaperScan(ctx, this, pageInfo, page);
                if (followThemeBitmapDoc && !blockOfficePaperBmp) {
                    RenderedBitmap* pageBmp = GetOrBuildLaTeXFollowThemePageBitmap(this, pageInfo, ctx, page, keptList,
                                                                                   zoom, rotation, darkProfile);
                    if (pageBmp) {
                        bitmap = BlitRegionFromFollowThemePageBitmap(pageBmp, pageInfo->followThemePageBitmapDevBounds,
                                                                     ibounds);
                    }
                }
                if (!bitmap) {
                    fz_clear_pixmap_with_value(ctx, pix, 0xff);
                    dev = fz_new_draw_device(ctx, ctm, pix);
                    fz_run_display_list(ctx, keptList, dev, fz_identity, pRect, fzcookie);
                    fz_close_device(ctx, dev);
                    dev = nullptr;
                    if (CadEnhanceActive() && cadRasterDominant && pix) {
                        PdfCadEnhancePixmap(ctx, pix, zoom, true);
                    }
                    bool textScanBinarize = blockOfficePaperBmp;
                    if (textScanBinarize) {
                        fz_pixmap* bin = PdfDarkModeProcessGovernmentPaperPixmap(ctx, pix, darkProfile->palette);
                        if (bin && bin != pix) {
                            fz_drop_pixmap(ctx, pix);
                            pix = bin;
                        }
                    }
                    JoinSplitBlitOntoPixmap(ctx, this, pageNo, pix, ctm);
                    bitmap = NewRenderedFzPixmap(ctx, pix);
                    if (bitmap && darkProfile && !textScanBinarize) {
                        Vec<Rect> skipRects;
                        Vec<Rect>* skipPtr = nullptr;
                        if (PdfFollowThemePreservesEmbeddedImageColors()) {
                            RectF renderRect = pageRect ? *pageRect : ToRectF(pRect);
                            this->GetBitmapRecolorSkipRects(pageNo, zoom, rotation, renderRect, bitmap->GetSize(),
                                                            skipRects);
                            if (skipRects.Size() > 0) {
                                skipPtr = &skipRects;
                            }
                        }
                        UpdateBitmapColors(bitmap->GetBitmap(), darkProfile->foreground, darkProfile->pageBackground,
                                           darkProfile->linkColor, skipPtr);
                    }
                }
            } else {
                if (DarkModeProfileUsesObjectLevel(darkProfile)) {
                    PdfDarkModeClearPixmapToThemeBackground(ctx, pix, darkProfile->palette);
                } else {
                    fz_clear_pixmap_with_value(ctx, pix, 0xff);
                }
                fz_device* baseDev = fz_new_draw_device(ctx, ctm, pix);
                DarkModeReplayState replayState{};
                dev = WrapViewRenderDevice(ctx, this, baseDev, darkProfile, pageInfo, keptList, &replayState, zoom);
                fz_run_display_list(ctx, keptList, dev, fz_identity, pRect, fzcookie);
                fz_close_device(ctx, dev);
                if (CadEnhanceActive() && cadRasterDominant && pix &&
                    !(darkProfile && DarkModeProfileUsesObjectLevel(darkProfile))) {
                    PdfCadEnhancePixmap(ctx, pix, zoom, true);
                }
                JoinSplitBlitOntoPixmap(ctx, this, pageNo, pix, ctm);
                bitmap = NewRenderedFzPixmap(ctx, pix);
            }
        }
        fz_always(ctx) {
            if (dev) {
                fz_drop_device(ctx, dev);
            }
            if (pix) {
                fz_drop_pixmap(ctx, pix);
            }
            fz_drop_display_list(ctx, keptList);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            delete bitmap;
            return nullptr;
        }
        return bitmap;
    }

    // Fallback: Print or hideAnnotations
    // not what the cached display list captured), or display-list construction
    // failed. Run the page directly under per-page lock.
    ReflowUiDocLock docGuard(this, reflowLoading);
    ReflowRenderLock renderGuard(this, reflowLoading);

    const char* usage = "View";
    switch (args.target) {
        case RenderTarget::Print:
            usage = "Print";
            break;
    }

    pdf_page* pdfpage = nullptr;
    fz_var(pdfpage);
    if (pdfdoc) {
        const DarkModeProfile* darkProfile = args.darkProfile;
        bool followDirect = DarkModeProfileUsesFollowThemeDirect(darkProfile);
        bool followV2 = DarkModeProfileUsesFollowThemeV2(darkProfile);
        bool paperScanBinarize = FollowThemeWholeTileBitmapBlockedForPaperScan(ctx, this, pageInfo, page);
        bool followBitmapRecolor = (followDirect || followV2) &&
                                   (FollowThemePageUsesBitmapRecolor(this, ctx, pageInfo, page) || paperScanBinarize);
        bool usedFollowThemeWrap = followDirect && !followBitmapRecolor;
        fz_try(ctx) {
            pdfpage = pdf_page_from_fz_page(ctx, page);
            pix = fz_new_pixmap_with_bbox(ctx, csRgb, ibounds, nullptr, 1);
            if (followBitmapRecolor) {
                fz_clear_pixmap_with_value(ctx, pix, 0xff);
            } else if (DarkModeProfileUsesObjectLevel(darkProfile)) {
                PdfDarkModeClearPixmapToThemeBackground(ctx, pix, darkProfile->palette);
            } else {
                fz_clear_pixmap_with_value(ctx, pix, 0xff);
            }
            fz_device* baseDev = fz_new_draw_device(ctx, ctm, pix);
            dev = baseDev;
            // Full-page scan packs (FreePic2Pdf 连环画, DuXiu, …): whole-tile UpdateBitmapColors
            // like main — do not run FollowThemeV2 photo-rect protect (white patchwork on line art).
            if (followV2 && !followBitmapRecolor) {
                RectF pageBounds = pageInfo->mediabox;
                if (pageBounds.IsEmpty()) {
                    pageBounds = ToRectF(fz_bound_page(ctx, page));
                }
                dev = PdfDarkModeWrapV2Device(ctx, baseDev, &darkProfile->palette, pageBounds, darkModeEngineCache,
                                              darkProfile->hash);
            } else if (usedFollowThemeWrap) {
                RectF pageBounds = pageInfo->mediabox;
                if (pageBounds.IsEmpty()) {
                    pageBounds = ToRectF(fz_bound_page(ctx, page));
                }
                Vec<RectF> followArtworkBounds;
                BuildFollowThemeArtworkBounds(ctx, pageInfo, page, pageBounds, followArtworkBounds);
                dev = PdfDarkModeWrapFollowThemeDevice(ctx, baseDev, &darkProfile->palette, pageBounds,
                                                       darkModeEngineCache, darkProfile->hash, &followArtworkBounds);
            }
            if (hideAnnotations) {
                pdf_run_page_contents_with_usage(ctx, pdfpage, dev, fz_identity, usage, fzcookie);
                pdf_run_page_widgets_with_usage(ctx, pdfpage, dev, fz_identity, usage, fzcookie);
            } else {
                pdf_run_page_with_usage(ctx, pdfpage, dev, fz_identity, usage, fzcookie);
            }
            fz_close_device(ctx, dev);
            dev = nullptr;
            if (CadEnhanceActive() && cadRasterDominant && pix &&
                !(darkProfile && DarkModeProfileUsesObjectLevel(darkProfile) && !followBitmapRecolor)) {
                PdfCadEnhancePixmap(ctx, pix, zoom, true);
            }
            JoinSplitBlitOntoPixmap(ctx, this, pageNo, pix, ctm);
            if (followBitmapRecolor && paperScanBinarize && darkProfile) {
                fz_pixmap* bin = PdfDarkModeProcessGovernmentPaperPixmap(ctx, pix, darkProfile->palette);
                if (bin && bin != pix) {
                    fz_drop_pixmap(ctx, pix);
                    pix = bin;
                }
            }
            bitmap = NewRenderedFzPixmap(ctx, pix);
            if (bitmap && followBitmapRecolor && darkProfile && !paperScanBinarize) {
                UpdateBitmapColors(bitmap->GetBitmap(), darkProfile->foreground, darkProfile->pageBackground,
                                   darkProfile->linkColor, nullptr);
            }
        }
        fz_always(ctx) {
            if (dev) {
                fz_drop_device(ctx, dev);
                dev = nullptr;
            }
            fz_drop_pixmap(ctx, pix);
            pix = nullptr;
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            delete bitmap;
            bitmap = nullptr;
        }
        if (!bitmap && usedFollowThemeWrap && darkProfile) {
            bool scanRecolor = FollowThemePageUsesBitmapRecolor(this, ctx, pageInfo, page);
            fz_try(ctx) {
                if (!pdfpage) {
                    pdfpage = pdf_page_from_fz_page(ctx, page);
                }
                pix = fz_new_pixmap_with_bbox(ctx, csRgb, ibounds, nullptr, 1);
                fz_clear_pixmap_with_value(ctx, pix, 0xff);
                dev = fz_new_draw_device(ctx, ctm, pix);
                if (hideAnnotations) {
                    pdf_run_page_contents_with_usage(ctx, pdfpage, dev, fz_identity, usage, fzcookie);
                    pdf_run_page_widgets_with_usage(ctx, pdfpage, dev, fz_identity, usage, fzcookie);
                } else {
                    pdf_run_page_with_usage(ctx, pdfpage, dev, fz_identity, usage, fzcookie);
                }
                fz_close_device(ctx, dev);
                dev = nullptr;
                if (CadEnhanceActive() && cadRasterDominant && pix) {
                    PdfCadEnhancePixmap(ctx, pix, zoom, true);
                }
                JoinSplitBlitOntoPixmap(ctx, this, pageNo, pix, ctm);
                bitmap = NewRenderedFzPixmap(ctx, pix);
                if (bitmap) {
                    Vec<Rect> skipRects;
                    Vec<Rect>* skipPtr = nullptr;
                    if (PdfFollowThemePreservesEmbeddedImageColors() && !scanRecolor) {
                        RectF renderRect = pageRect ? *pageRect : ToRectF(fz_bound_page(ctx, page));
                        GetBitmapRecolorSkipRects(pageNo, zoom, rotation, renderRect, bitmap->GetSize(), skipRects);
                        if (skipRects.Size() > 0) {
                            skipPtr = &skipRects;
                        }
                    }
                    UpdateBitmapColors(bitmap->GetBitmap(), darkProfile->foreground, darkProfile->pageBackground,
                                       darkProfile->linkColor, skipPtr);
                }
            }
            fz_always(ctx) {
                if (dev) {
                    fz_drop_device(ctx, dev);
                    dev = nullptr;
                }
                fz_drop_pixmap(ctx, pix);
                pix = nullptr;
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                delete bitmap;
                bitmap = nullptr;
            }
        }
    } else {
        fz_try(ctx) {
            pix = fz_new_pixmap_with_bbox(ctx, csRgb, ibounds, nullptr, 1);
            fz_clear_pixmap_with_value(ctx, pix, 0xff);
            dev = fz_new_draw_device(ctx, ctm, pix);
            fz_run_page_contents(ctx, page, dev, fz_identity, NULL);
            fz_close_device(ctx, dev);
            fz_drop_device(ctx, dev);
            if (CadEnhanceActive() && cadRasterDominant && pix) {
                PdfCadEnhancePixmap(ctx, pix, zoom, true);
            }
            JoinSplitBlitOntoPixmap(ctx, this, pageNo, pix, ctm);
            bitmap = NewRenderedFzPixmap(ctx, pix);
        }
        fz_always(ctx) {
            fz_drop_pixmap(ctx, pix);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            delete bitmap;
            return nullptr;
        }
    }

    return bitmap;
}

// don't delete the result
IPageElement* EngineMupdf::GetElementAtPos(int pageNo, PointF pt) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return nullptr;
    }
    FzPageInfo* pageInfo = GetFzPageInfoCanFail(pageNo);
    bool reflowLoading = !pdfdoc && IsProgressiveEbookLoading();
    if (!pageInfo) {
        // PDF: never block the UI thread loading pages for link/cursor hit-testing.
        // GetFzPageInfoCanFail returns null when the page is not loaded yet or the
        // render thread holds pagesLock; the old code returned immediately.
        if (pdfdoc) {
            return nullptr;
        }
        // Same for reflowable docs while loading: GetFzPageInfo would wait on
        // docLock, which background counting / fragment resolution can hold for
        // seconds per chapter. Hit-testing catches up once the page is rendered.
        if (reflowLoading) {
            return nullptr;
        }
        pageInfo = GetFzPageInfo(pageNo, true, nullptr, /*loadLinks*/ false);
    }
    if (!pageInfo) {
        return nullptr;
    }
    return FzGetElementAtPos(pageInfo, pt);
}

void EngineMupdfEnsurePageLinksForHitTest(EngineBase* engine, int pageNo) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || pageNo < 1 || pageNo > e->pageCount) {
        return;
    }
    pageNo = MapJoinDisplayPageNo(e, pageNo);
    if (pageNo < 1) {
        return;
    }
    // Rendering uses loadQuick with loadLinks false; populate link hit-test data on first hover.
    e->GetFzPageInfo(pageNo, true, nullptr, /*loadLinks*/ true);
}

void EngineMupdfEnsurePageImagesForHitTest(EngineBase* engine, int pageNo) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || pageNo < 1 || pageNo > e->pageCount) {
        return;
    }
    pageNo = MapJoinDisplayPageNo(e, pageNo);
    if (pageNo < 1) {
        return;
    }
    FzPageInfo* pageInfo = e->GetFzPageInfo(pageNo, false, nullptr, false);
    if (!pageInfo || !pageInfo->page) {
        return;
    }
    if (e->pdfdoc && !pageInfo->contentImagesCollected) {
        fz_context* ctx = e->Ctx();
        FzCollectImagesFromPageContent(ctx, pageNo, pageInfo, pageInfo->page, nullptr);
        pageInfo->contentImagesCollected = true;
    }
}

// TOOD: optimize by returning reference or pointer so that
// we don't have to re-create the Vec every time
Vec<IPageElement*> EngineMupdf::GetElements(int pageNo) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return Vec<IPageElement*>();
    }
    auto pageInfo = GetFzPageInfoFast(pageNo);
    if (!pageInfo) {
        return Vec<IPageElement*>();
    }

    BuildElementsInfo(pageInfo);
    return pageInfo->allElements;
}

// returns 1-based page number, or 0 if unresolved
static int ResolveMupdfLinkPageNo1(EngineMupdf* e, const char* uri, fz_link_dest* ldestOut) {
    if (!e || !uri) {
        return 0;
    }
    // docLock only: must not take pagesLock before docLock (background reflow counting
    // uses docLock then pagesLock). fz_resolve_link_dest touches the same epub document
    // as fz_count_chapter_pages and must not run concurrently with it.
    // Use try-lock during progressive load so bookmark clicks don't freeze the UI.
    bool reflowLoading = InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0;
    if (e->pdfdoc) {
        EnterCriticalSection(&e->docLock);
    } else {
        AcquireReflowUiDocLock(e);
    }
    defer {
        if (e->pdfdoc) {
            LeaveCriticalSection(&e->docLock);
        } else {
            ReleaseReflowUiDocLock(e);
        }
    };

    int pageNo = -1;
    int resolvedReflowPage = 0;
    fz_link_dest ldest{};
    auto ctx = e->Ctx();
    fz_var(pageNo);
    fz_try(ctx) {
        ldest = fz_resolve_link_dest(ctx, e->_doc, uri);
        if (!e->pdfdoc) {
            int ch = ldest.loc.chapter;
            int pic = ldest.loc.page >= 0 ? ldest.loc.page : 0;
            if (ch < 0) {
                ch = EpubUriChapterIndexNoLayout(ctx, e->_doc, uri);
            }
            int mapped = ReflowPageNoFromChapter(e, ch, pic);
            if (mapped > 0) {
                if (ldestOut) {
                    *ldestOut = ldest;
                }
                resolvedReflowPage = mapped;
            } else {
                pageNo = -1;
            }
        } else {
            pageNo = fz_page_number_from_location(ctx, e->_doc, ldest.loc);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        pageNo = -1;
        resolvedReflowPage = 0;
    }
    if (resolvedReflowPage > 0) {
        return resolvedReflowPage;
    }
    if (pageNo < 0) {
        return 0;
    }
    int pageNo1 = pageNo + 1;
    if (!e->pdfdoc && pageNo1 > e->pageCount) {
        int ch = -1;
        {
            ScopedCritSec scope(&e->docLock);
            ch = EpubUriChapterIndexNoLayout(e->Ctx(), e->_doc, uri);
        }
        int alt = ReflowPageNoFromChapter(e, ch, 0);
        if (alt > 0 && alt <= e->pageCount) {
            pageNo1 = alt;
        } else if (e->pageCount > 0) {
            pageNo1 = e->pageCount;
        } else {
            return 0;
        }
    }
    if (ldestOut) {
        *ldestOut = ldest;
    }
    return pageNo1;
}

static const char* MupdfDestUri(PageDestinationMupdf* link) {
    if (!link) {
        return nullptr;
    }
    if (MupdfSafeCStr(link->uriOwned)) {
        return link->uriOwned;
    }
    if (link->value && *link->value) {
        return link->value;
    }
    const char* uri = nullptr;
    if (link->outline) {
        uri = link->outline->uri;
    } else if (link->link) {
        uri = link->link->uri;
    }
    return MupdfSafeCStr(uri);
}

void HandleLinkMupdf(EngineMupdf* e, IPageDestination* dest, ILinkHandler* linkHandler) {
    ReportIf(kindDestinationMupdf != dest->GetKind());
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    ReportIf(!(link->outline || link->link));
    const char* uri = MupdfDestUri(link);
    if (!uri) {
        return;
    }
    if (IsExternalLink(uri)) {
        linkHandler->LaunchURL(uri);
        return;
    }

    fz_link_dest ldest{};
    int pageNo1 = 0;
    if (InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0) {
        // Same fast path as sidebar TOC: never call fz_resolve_link_dest during
        // background chapter counting (built-in EPUB links would freeze the UI).
        if (!EngineMupdfIsOutlineDestReachable(e, dest)) {
            return;
        }
        float destX = DEST_USE_DEFAULT;
        float destY = DEST_USE_DEFAULT;
        if (link->outline) {
            destX = link->outline->x;
            destY = link->outline->y;
        }
        EngineMupdfNavigateUri(e, uri, ReflowOutlineChapterIndex(link), destX, destY, linkHandler);
        return;
    } else {
        pageNo1 = ResolveMupdfLinkPageNo1(e, uri, &ldest);
        if (pageNo1 <= 0) {
            return;
        }
        pageNo1 = EngineMupdfMapEnginePageToDisplay(e, pageNo1);
        if (pageNo1 <= 0) {
            return;
        }
    }

    auto ctrl = linkHandler->GetDocController();
    ctrl->PreparePageNavigation(pageNo1);
    if (pageNo1 < 1 || pageNo1 > e->PageCount()) {
        return;
    }

    // TODO: handle ldest.type like FZ_LINK_DEST_FIT_H ?
    float x = isnan(ldest.x) ? 0.f : ldest.x;
    float y = isnan(ldest.y) ? 0.f : ldest.y;
    float zoom = isnan(ldest.zoom) ? 0.f : ldest.zoom;
    zoom = zoom / 100; // mupdf uses 100 as 100% zoom, we use 1
    float w = isnan(ldest.w) ? DEST_USE_DEFAULT : ldest.w;
    float h = isnan(ldest.h) ? DEST_USE_DEFAULT : ldest.h;

    RectF r(x, y, w, h);
    ctrl->ScrollTo(pageNo1, r, zoom);
}

bool EngineMupdf::HandleLink(IPageDestination* dest, ILinkHandler* linkHandler) {
    Kind k = dest->GetKind();
    if (k == kindDestinationMupdf) {
        HandleLinkMupdf(this, dest, linkHandler);
        return true;
    }
    linkHandler->GotoLink(dest);
    return true;
}

RenderedBitmap* EngineMupdf::GetImageForPageElement(IPageElement* ipel) {
    ReportIf(kindPageElementImage != ipel->GetKind());
    auto pel = (PageElementImage*)ipel;
    auto r = pel->rect;
    int pageNo = pel->pageNo;
    int imageID = pel->imageID;
    return GetPageImage(pageNo, r, imageID);
}

bool EngineMupdf::BenchLoadPage(int pageNo) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return false;
    }
    return GetFzPageInfo(pageNo, false) != nullptr;
}

fz_matrix EngineMupdf::viewctm(int pageNo, float zoom, int rotation) {
    const fz_rect tmpRc = ToFzRect(PageMediabox(pageNo));
    return FzCreateViewCtm(tmpRc, zoom, rotation);
}

fz_matrix EngineMupdf::viewctm(fz_page* page, float zoom, int rotation) const {
    auto ctx = Ctx();

    fz_rect bounds;
    fz_var(bounds);
    fz_try(ctx) {
        bounds = fz_bound_page(ctx, page);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        bounds = {};
    }
    if (fz_is_empty_rect(bounds)) {
        bounds = {0, 0, 612, 792};
    }
    return FzCreateViewCtm(bounds, zoom, rotation);
}

RenderedBitmap* EngineMupdf::GetPageImage(int pageNo, RectF rect, int imageIdx) {
    auto ctx = Ctx();
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;
    ReflowUiDocLock docGuard(this, reflowLoading);
    ReflowRenderLock renderGuard(this, reflowLoading);

    FzPageInfo* pageInfo = GetFzPageInfo(pageNo, false);
    if (!pageInfo->page) {
        return nullptr;
    }
    const auto& images = pageInfo->images;
    bool outOfBounds = imageIdx >= images.Size();
    fz_rect imgRect = images.at(imageIdx)->rect;
    bool badRect = ToRectF(imgRect) != rect;
    ReportIf(outOfBounds);
    ReportIf(badRect);
    if (outOfBounds || badRect) {
        return nullptr;
    }

    fz_image* image = FzGetKeptPageImage(ctx, pageInfo, imageIdx);
    if (!image) {
        return nullptr;
    }

    RenderedBitmap* bmp = nullptr;
    fz_pixmap* pixmap = nullptr;
    fz_var(pixmap);
    fz_var(bmp);

    fz_try(ctx) {
        // TODO(port): not sure if should provide subarea, w and h
        pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
        if (!pixmap || !pixmap->samples || pixmap->w <= 0 || pixmap->h <= 0) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "invalid image pixmap");
        }
        // Match `extract -r`: normalize embedded images to RGB before creating
        // a Windows bitmap for copy/save operations.
        // https://github.com/sumatrapdfreader/sumatrapdf/issues/1480
        if (!pixmap->colorspace) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "image pixmap without colorspace");
        }
        if (!fz_colorspace_is_rgb(ctx, pixmap->colorspace)) {
            fz_pixmap* rgb =
                fz_convert_pixmap(ctx, pixmap, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 1);
            fz_drop_pixmap(ctx, pixmap);
            pixmap = rgb;
            if (!pixmap || !pixmap->samples || !pixmap->colorspace) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "RGB conversion failed");
            }
        }
        bmp = NewRenderedFzPixmap(ctx, pixmap);
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pixmap);
        fz_drop_image(ctx, image);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        bmp = nullptr;
    }

    return bmp;
}

static PageText ExtractPageTextFromFzPageInfo(EngineMupdf* e, FzPageInfo* pageInfo) {
    if (!pageInfo || !pageInfo->page) {
        return {};
    }
    auto ctx = e->Ctx();
    if (!pageInfo->searchStext) {
        fz_stext_page* stext = nullptr;
        fz_var(stext);
        fz_stext_options opts = NewTextPageOptions();
        fz_try(ctx) {
            stext = fz_new_stext_page_from_page(ctx, pageInfo->page, &opts);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        pageInfo->searchStext = stext;
    }
    if (!pageInfo->searchStext) {
        return {};
    }
    PageText res;
    WCHAR* text = FzTextPageToStr(pageInfo->searchStext, &res.coords);
    res.text = text;
    res.len = (int)str::Len(text);
    return res;
}

static PageTextUtf8 ExtractPageTextUtf8FromFzPageInfo(EngineMupdf* e, FzPageInfo* pageInfo) {
    if (!pageInfo || !pageInfo->page) {
        return {};
    }
    auto ctx = e->Ctx();
    if (!pageInfo->searchStext) {
        fz_stext_page* stext = nullptr;
        fz_var(stext);
        fz_stext_options opts = NewTextPageOptions();
        fz_try(ctx) {
            stext = fz_new_stext_page_from_page(ctx, pageInfo->page, &opts);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        pageInfo->searchStext = stext;
    }
    if (!pageInfo->searchStext) {
        return {};
    }
    PageTextUtf8 res;
    char* text = FzTextPageToUtf8(pageInfo->searchStext, &res.coords);
    res.text = text;
    res.len = (int)str::Len(text);
    return res;
}

PageText EngineMupdf::ExtractPageText(int pageNo) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return {};
    }
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;

    ReflowUiDocLock docGuard(this, reflowLoading);
    ReflowRenderLock renderGuard(this, reflowLoading);

    FzPageInfo* pageInfo = GetFzPageInfo(pageNo, true);
    return ExtractPageTextFromFzPageInfo(this, pageInfo);
}

bool EngineMupdf::TryExtractPageText(int pageNo, PageText* out) {
    *out = {};
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return false;
    }
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;

    if (pdfdoc) {
        if (!TryEnterCriticalSection(&pagesLock)) {
            return false;
        }
        if (!TryEnterCriticalSection(&renderLock)) {
            LeaveCriticalSection(&pagesLock);
            return false;
        }
        FzPageInfo* pageInfo = nullptr;
        if (pageNo >= 1 && pageNo <= pages.Size()) {
            pageInfo = pages[pageNo - 1];
        }
        if (!pageInfo || !pageInfo->page) {
            LeaveCriticalSection(&renderLock);
            LeaveCriticalSection(&pagesLock);
            return false;
        }
        *out = ExtractPageTextFromFzPageInfo(this, pageInfo);
        LeaveCriticalSection(&renderLock);
        LeaveCriticalSection(&pagesLock);
        return true;
    }

    if (MupdfNeedsDocLock(this, reflowLoading)) {
        if (!TryEnterCriticalSection(&docLock)) {
            return false;
        }
    }
    if (!TryEnterCriticalSection(&pagesLock)) {
        if (MupdfNeedsDocLock(this, reflowLoading)) {
            LeaveCriticalSection(&docLock);
        }
        return false;
    }
    FzPageInfo* pageInfo = nullptr;
    if (pageNo >= 1 && pageNo <= pages.Size()) {
        pageInfo = pages[pageNo - 1];
    }
    if (!pageInfo || !pageInfo->page) {
        LeaveCriticalSection(&pagesLock);
        if (MupdfNeedsDocLock(this, reflowLoading)) {
            LeaveCriticalSection(&docLock);
        }
        return false;
    }
    *out = ExtractPageTextFromFzPageInfo(this, pageInfo);
    LeaveCriticalSection(&pagesLock);
    if (MupdfNeedsDocLock(this, reflowLoading)) {
        LeaveCriticalSection(&docLock);
    }
    return true;
}

bool EngineMupdf::TryExtractPageTextUtf8(int pageNo, PageTextUtf8* out) {
    *out = {};
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return false;
    }
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;

    if (pdfdoc) {
        if (!TryEnterCriticalSection(&pagesLock)) {
            return false;
        }
        if (!TryEnterCriticalSection(&renderLock)) {
            LeaveCriticalSection(&pagesLock);
            return false;
        }
        FzPageInfo* pageInfo = nullptr;
        if (pageNo >= 1 && pageNo <= pages.Size()) {
            pageInfo = pages[pageNo - 1];
        }
        if (!pageInfo || !pageInfo->page) {
            LeaveCriticalSection(&renderLock);
            LeaveCriticalSection(&pagesLock);
            return false;
        }
        *out = ExtractPageTextUtf8FromFzPageInfo(this, pageInfo);
        LeaveCriticalSection(&renderLock);
        LeaveCriticalSection(&pagesLock);
        return true;
    }

    if (MupdfNeedsDocLock(this, reflowLoading)) {
        if (!TryEnterCriticalSection(&docLock)) {
            return false;
        }
    }
    if (!TryEnterCriticalSection(&pagesLock)) {
        if (MupdfNeedsDocLock(this, reflowLoading)) {
            LeaveCriticalSection(&docLock);
        }
        return false;
    }
    FzPageInfo* pageInfo = nullptr;
    if (pageNo >= 1 && pageNo <= pages.Size()) {
        pageInfo = pages[pageNo - 1];
    }
    if (!pageInfo || !pageInfo->page) {
        LeaveCriticalSection(&pagesLock);
        if (MupdfNeedsDocLock(this, reflowLoading)) {
            LeaveCriticalSection(&docLock);
        }
        return false;
    }
    *out = ExtractPageTextUtf8FromFzPageInfo(this, pageInfo);
    LeaveCriticalSection(&pagesLock);
    if (MupdfNeedsDocLock(this, reflowLoading)) {
        LeaveCriticalSection(&docLock);
    }
    return true;
}

PageTextUtf8 EngineMupdf::ExtractPageTextUtf8(int pageNo) {
    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return {};
    }
    bool reflowLoading = InterlockedCompareExchange(&reflowableLoadingInProgress, 0, 0) != 0;

    // UTF-8 extraction is the full-document search path. It must not advertise
    // itself as a waiting UI operation: doing so makes progressive pagination
    // yield to the search worker and can starve both jobs after a result click.
    ReflowUiDocLock docGuard(this, reflowLoading, true);
    ReflowRenderLock renderGuard(this, reflowLoading);

    FzPageInfo* pageInfo = GetFzPageInfo(pageNo, true, nullptr, false, true);
    return ExtractPageTextUtf8FromFzPageInfo(this, pageInfo);
}

static void AppendStextLineToPageLines(fz_context* ctx, fz_stext_line* line, Vec<EngineMupdfPageLine>& linesOut) {
    if (!line) {
        return;
    }
    StrBuilder sb;
    float sizeSum = 0;
    int nSize = 0;
    int nBold = 0;
    int nCh = 0;
    for (fz_stext_char* c = line->first_char; c; c = c->next) {
        if (c->c <= 32) {
            if (c->c == ' ' || c->c == 0x3000) {
                sb.AppendChar(' ');
            }
            continue;
        }
        char buf[8];
        int n = fz_runetochar(buf, c->c);
        if (n > 0) {
            sb.Append(buf, (size_t)n);
        }
        sizeSum += c->size;
        nSize++;
        nCh++;
        bool bold = (c->flags & FZ_STEXT_BOLD) != 0;
        if (!bold && c->font) {
            bold = fz_font_is_bold(ctx, c->font) != 0;
        }
        if (bold) {
            nBold++;
        }
    }
    if (nCh < 1 || sb.size() < 1) {
        return;
    }
    EngineMupdfPageLine pl;
    pl.text = sb.StealData();
    str::TrimWSInPlace(pl.text, str::TrimOpt::Both);
    if (!pl.text[0]) {
        str::Free(pl.text);
        return;
    }
    pl.x = line->bbox.x0;
    pl.y = line->bbox.y0;
    pl.dx = line->bbox.x1 - line->bbox.x0;
    pl.dy = line->bbox.y1 - line->bbox.y0;
    pl.fontSize = nSize > 0 ? sizeSum / (float)nSize : pl.dy;
    pl.bold = nBold * 2 >= nCh;
    linesOut.Append(pl);
}

bool EngineMupdfCollectPageLines(EngineBase* engine, int pageNo, Vec<EngineMupdfPageLine>& linesOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e) {
        return false;
    }
    pageNo = MapJoinDisplayPageNo(e, pageNo);
    if (pageNo < 1) {
        return false;
    }
    bool reflowLoading = InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0;
    ReflowUiDocLock docGuard(e, reflowLoading, true);
    ReflowRenderLock renderGuard(e, reflowLoading);
    FzPageInfo* pageInfo = e->GetFzPageInfo(pageNo, true);
    if (!pageInfo || !pageInfo->page) {
        return false;
    }
    auto ctx = e->Ctx();
    if (!pageInfo->searchStext) {
        fz_stext_page* stext = nullptr;
        fz_var(stext);
        fz_stext_options opts = NewTextPageOptions();
        fz_try(ctx) {
            stext = fz_new_stext_page_from_page(ctx, pageInfo->page, &opts);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        pageInfo->searchStext = stext;
    }
    if (!pageInfo->searchStext) {
        return false;
    }
    for (fz_stext_block* block = pageInfo->searchStext->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) {
            continue;
        }
        for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
            AppendStextLineToPageLines(ctx, line, linesOut);
        }
    }
    return linesOut.Size() > 0;
}

static void pdf_extract_fonts(fz_context* ctx, pdf_obj* res, Vec<pdf_obj*>& fontList, Vec<pdf_obj*>& resList) {
    if (!res || pdf_mark_obj(ctx, res)) {
        return;
    }
    resList.Append(res);

    pdf_obj* fonts = pdf_dict_gets(ctx, res, "Font");
    for (int k = 0; k < pdf_dict_len(ctx, fonts); k++) {
        pdf_obj* font = pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, fonts, k));
        if (font && !fontList.Contains(font)) {
            fontList.Append(font);
        }
    }
    // also extract fonts for all XObjects (recursively)
    pdf_obj* xobjs = pdf_dict_gets(ctx, res, "XObject");
    for (int k = 0; k < pdf_dict_len(ctx, xobjs); k++) {
        pdf_obj* xobj = pdf_dict_get_val(ctx, xobjs, k);
        pdf_obj* xres = pdf_dict_gets(ctx, xobj, "Resources");
        pdf_extract_fonts(ctx, xres, fontList, resList);
    }
}

TempStr EngineMupdf::ExtractFontListTemp() {
    Vec<pdf_obj*> fontList;
    Vec<pdf_obj*> resList;

    auto ctx = Ctx();

    // collect all fonts from all page objects
    int nPages = PageCount();
    for (int i = 1; i <= nPages; i++) {
        auto pageInfo = GetFzPageInfo(i, false);
        if (!pageInfo) {
            continue;
        }
        fz_page* fzpage = pageInfo->page;
        if (!fzpage) {
            continue;
        }

        ScopedCritSec scope(&docLock);
        pdf_page* page = pdf_page_from_fz_page(ctx, fzpage);
        fz_try(ctx) {
            pdf_obj* resources = pdf_page_resources(ctx, page);
            pdf_extract_fonts(ctx, resources, fontList, resList);
            pdf_annot* annot;
            for (annot = pdf_first_annot(ctx, page); annot; annot = pdf_next_annot(ctx, annot)) {
                pdf_obj* o = pdf_annot_ap(ctx, annot);
                if (o) {
                    // TODO(port): not sure this is the right thing
                    resources = pdf_xobject_resources(ctx, o);
                    pdf_extract_fonts(ctx, resources, fontList, resList);
                }
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }

    // start docLock scope here so that we don't also have to
    // ask for pagesLock (as is required for GetFzPage)
    ScopedCritSec scope(&docLock);

    for (pdf_obj* res : resList) {
        pdf_unmark_obj(ctx, res);
    }

    StrVec fonts;
    for (size_t i = 0; i < fontList.size(); i++) {
        const char *name = nullptr, *type = nullptr, *encoding = nullptr;
        bool embedded = false;
        fz_try(ctx) {
            pdf_obj* font = fontList.at(i);
            pdf_obj* font2 = pdf_array_get(ctx, pdf_dict_gets(ctx, font, "DescendantFonts"), 0);
            if (!font2) {
                font2 = font;
            }

            name = pdf_to_name(ctx, pdf_dict_getsa(ctx, font2, "BaseFont", "Name"));
            bool needAnonName = str::IsEmpty(name);
            if (needAnonName && font2 != font) {
                name = pdf_to_name(ctx, pdf_dict_getsa(ctx, font, "BaseFont", "Name"));
                needAnonName = str::IsEmpty(name);
            }
            if (needAnonName) {
                name = str::FormatTemp("<#%d>", pdf_obj_parent_num(ctx, font2));
            }
            embedded = false;
            pdf_obj* desc = pdf_dict_gets(ctx, font2, "FontDescriptor");
            if (desc && (pdf_dict_gets(ctx, desc, "FontFile") || pdf_dict_getsa(ctx, desc, "FontFile2", "FontFile3"))) {
                embedded = true;
            }
            if (embedded && str::Len(name) > 7 && name[6] == '+') {
                name += 7;
            }

            type = pdf_to_name(ctx, pdf_dict_gets(ctx, font, "Subtype"));
            if (font2 != font) {
                const char* type2 = pdf_to_name(ctx, pdf_dict_gets(ctx, font2, "Subtype"));
                if (str::Eq(type2, "CIDFontType0")) {
                    type = "Type1 (CID)";
                } else if (str::Eq(type2, "CIDFontType2")) {
                    type = "TrueType (CID)";
                }
            }
            if (str::Eq(type, "Type3")) {
                embedded = pdf_dict_gets(ctx, font2, "CharProcs") != nullptr;
            }

            encoding = pdf_to_name(ctx, pdf_dict_gets(ctx, font, "Encoding"));
            if (str::Eq(encoding, "WinAnsiEncoding")) {
                encoding = "Ansi";
            } else if (str::Eq(encoding, "MacRomanEncoding")) {
                encoding = "Roman";
            } else if (str::Eq(encoding, "MacExpertEncoding")) {
                encoding = "Expert";
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            continue;
        }
        ReportIf(!name || !type || !encoding);

        StrBuilder info;
        if (name[0] < 0 && MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0)) {
            TempStr s = strconv::ToMultiByteTemp(name, 936, CP_UTF8);
            info.Append(s);
        } else {
            info.Append(name);
        }
        if (!str::IsEmpty(encoding) || !str::IsEmpty(type) || embedded) {
            info.Append(" (");
            if (!str::IsEmpty(type)) {
                info.AppendFmt("%s; ", type);
            }
            if (!str::IsEmpty(encoding)) {
                info.AppendFmt("%s; ", encoding);
            }
            if (embedded) {
                info.Append("embedded; ");
            }
            info.RemoveAt(info.size() - 2, 2);
            info.Append(")");
        }

        if (info.IsEmpty()) {
            continue;
        }
        char* fontName = info.LendData();
        AppendIfNotExists(&fonts, fontName);
    }
    if (fonts.Size() == 0) {
        return nullptr;
    }

    SortNatural(&fonts);
    return JoinTemp(&fonts, "\n");
}

// clang-format off
static const char* mupdfPropsMap[] = {
    kPropTitle, FZ_META_INFO_TITLE,
    kPropAuthor, FZ_META_INFO_AUTHOR,
    kPropSubject, "info:Subject",
    kPropPdfProducer, FZ_META_INFO_PRODUCER,
    kPropCreatorApp, "info:Creator", // not sure if the same meaning
    kPropCreationDate, "info:CreationDate",
    kPropModificationDate, "info:ModDate",
    nullptr,
};
// clang-format on

TempStr EngineMupdf::GetPropertyTemp(const char* name) {
    auto ctx = Ctx();
    ScopedCritSec ctxScope(&docLock);

    const char* key = GetMatchingString(mupdfPropsMap, name);
    if (key) {
        char buf[1024]{};
        int bufSize = (int)dimof(buf);
        int n = fz_lookup_metadata(ctx, _doc, key, buf, bufSize);
        if (n > 0) {
            if (n > bufSize) {
                // can be bigger if output truncated
                n = bufSize - 1;
                buf[bufSize - 1] = 0; // not sure if necessary
            }
            char* s = str::DupTemp(buf, (size_t)n - 1);
            return s;
        }
    }
    if (!pdfdoc) {
        return nullptr;
    }

    if (str::Eq(kPropPdfVersion, name)) {
        int major = pdfdoc->version / 10, minor = pdfdoc->version % 10;
        pdf_crypt* crypt = pdfdoc->crypt;
        if (1 == major && 7 == minor && pdf_crypt_version(ctx, crypt) == 5) {
            if (pdf_crypt_revision(ctx, crypt) == 5) {
                return str::FormatTemp("%d.%d Adobe Extension Level %d", major, minor, 3);
            }
            if (pdf_crypt_revision(ctx, crypt) == 6) {
                return str::FormatTemp("%d.%d Adobe Extension Level %d", major, minor, 8);
            }
        }
        return str::FormatTemp("%d.%d", major, minor);
    }

    if (str::Eq(kPropPdfFileStructure, name)) {
        StrVec fstruct;
        if (pdf_to_bool(ctx, pdf_dict_gets(ctx, pdfInfo, "Linearized"))) {
            fstruct.Append("linearized");
        }
        if (pdf_to_bool(ctx, pdf_dict_gets(ctx, pdfInfo, "Marked"))) {
            fstruct.Append("tagged");
        }
        if (pdf_dict_gets(ctx, pdfInfo, "OutputIntents")) {
            int n = pdf_array_len(ctx, pdf_dict_gets(ctx, pdfInfo, "OutputIntents"));
            for (int i = 0; i < n; i++) {
                pdf_obj* intent = pdf_array_get(ctx, pdf_dict_gets(ctx, pdfInfo, "OutputIntents"), i);
                ReportIf(!str::StartsWith(pdf_to_name(ctx, intent), "GTS_"));
                const char* s = pdf_to_name(ctx, intent) + 4;
                fstruct.Append(s);
            }
        }
        if (fstruct.Size() == 0) {
            return nullptr;
        }
        return JoinTemp(&fstruct, ",");
    }

    if (str::Eq(kPropUnsupportedFeatures, name)) {
        if (pdf_to_bool(ctx, pdf_dict_gets(ctx, pdfInfo, "Unsupported_XFA"))) {
            return (TempStr) "XFA";
        }
        return nullptr;
    }

    if (str::Eq(kPropFontList, name)) {
        return ExtractFontListTemp();
    }

    static const char* pdfPropNames[] = {
        kPropTitle,        "Title",        kPropAuthor,           "Author",
        kPropSubject,      "Subject",      kPropCopyright,        "Copyright",
        kPropCreationDate, "CreationDate", kPropModificationDate, "ModDate",
        kPropCreatorApp,   "Creator",      kPropPdfProducer,      "Producer",
        nullptr,
    };
    const char* pdfPropName = GetMatchingString(pdfPropNames, name);
    if (!pdfPropName) {
        return nullptr;
    }

    // _info is guaranteed not to contain any indirect references,
    // so no need for docLock
    pdf_obj* obj = pdf_dict_gets(ctx, pdfInfo, pdfPropName);
    if (!obj) {
        return nullptr;
    }
    TempWStr ws = PdfToWStrTemp(ctx, obj);
    PdfCleanStringInPlace(ws);
    TempStr res = ToUtf8Temp(ws);
    return res;
};

static TempStr LookupMetadataTemp(fz_context* ctx, fz_document* doc, const char* key) {
    char buf[1024]{};
    int n = fz_lookup_metadata(ctx, doc, key, buf, (int)dimof(buf));
    if (n <= 0) {
        return nullptr;
    }
    if (n > (int)dimof(buf)) {
        n = (int)dimof(buf) - 1;
        buf[n] = 0;
    }
    return str::DupTemp(buf, (size_t)n - 1);
}

static void AppendSigDictText(fz_context* ctx, StrBuilder& s, pdf_obj* sigDict, const char* label, pdf_obj* key) {
    const char* val = nullptr;
    fz_try(ctx) {
        pdf_obj* obj = pdf_dict_get(ctx, sigDict, key);
        if (obj) {
            val = pdf_to_text_string(ctx, obj);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        val = nullptr;
    }
    if (val && *val) {
        s.AppendFmt("  %s: %s\n", label, val);
    }
}

static void AppendSigDictDate(fz_context* ctx, StrBuilder& s, pdf_obj* sigDict, const char* label, pdf_obj* key) {
    int64_t secs = 0;
    fz_try(ctx) {
        pdf_obj* obj = pdf_dict_get(ctx, sigDict, key);
        if (obj) {
            secs = pdf_to_date(ctx, obj);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        secs = 0;
    }
    if (secs <= 0) {
        return;
    }
    time_t t = (time_t)secs;
    struct tm tm;
    gmtime_s(&tm, &t);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm);
    s.AppendFmt("  %s: %s\n", label, buf);
}

static void AppendSignatureInfo(fz_context* ctx, StrBuilder& s, pdf_pkcs7_verifier* verifier, pdf_document* pdfdoc,
                                pdf_annot* widget, int sigNo, int pageNo) {
    if (!s.IsEmpty()) {
        s.AppendChar('\n');
    }
    s.AppendFmt("Signature %d (page %d):\n", sigNo, pageNo);
    pdf_obj* sigObj = pdf_annot_obj(ctx, widget);
    if (!pdf_signature_is_signed(ctx, pdfdoc, sigObj)) {
        s.Append("  not signed\n");
        return;
    }

    pdf_pkcs7_distinguished_name* dn = nullptr;
    char* name = nullptr;
    fz_var(dn);
    fz_var(name);
    fz_try(ctx) {
        dn = pdf_signature_get_widget_signatory(ctx, verifier, widget);
        if (dn) {
            name = pdf_signature_format_distinguished_name(ctx, dn);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    s.AppendFmt("  signer: %s\n", name ? name : "(unknown)");
    fz_free(ctx, name);
    pdf_signature_drop_distinguished_name(ctx, dn);

    // optional metadata the signer put in the /V dictionary (PDF 32000-1
    // §12.8.1). These are plain PDF text strings, so pdf_to_text_string
    // already hands us well-formed UTF-8 -- no mojibake risk.
    pdf_obj* vDict = pdf_dict_get(ctx, sigObj, PDF_NAME(V));
    if (!vDict) {
        vDict = sigObj;
    }
    AppendSigDictDate(ctx, s, vDict, "signing time", PDF_NAME(M));
    AppendSigDictText(ctx, s, vDict, "reason", PDF_NAME(Reason));
    AppendSigDictText(ctx, s, vDict, "location", PDF_NAME(Location));
    AppendSigDictText(ctx, s, vDict, "contact", PDF_NAME(ContactInfo));

    pdf_signature_error certErr = PDF_SIGNATURE_ERROR_UNKNOWN;
    fz_try(ctx) {
        certErr = pdf_check_widget_certificate(ctx, verifier, widget);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    s.AppendFmt("  certificate: %s\n", pdf_signature_error_description(certErr));

    pdf_signature_error digErr = PDF_SIGNATURE_ERROR_UNKNOWN;
    int edits = 0;
    fz_try(ctx) {
        digErr = pdf_check_widget_digest(ctx, verifier, widget);
        edits = pdf_signature_incremental_change_since_signing(ctx, pdfdoc, sigObj);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    if (digErr) {
        s.AppendFmt("  digest: %s\n", pdf_signature_error_description(digErr));
    } else if (edits) {
        s.Append("  document edited after signing\n");
    } else {
        s.Append("  document unchanged since signing\n");
    }
}

void EngineMupdf::GetProperties(StrVec& keyValOut) {
    EngineBase::GetProperties(keyValOut);

    auto ctx = Ctx();
    ScopedCritSec ctxScope(&docLock);

    TempStr val = LookupMetadataTemp(ctx, _doc, "info:Keywords");
    if (val) {
        AddProp(keyValOut, kPropKeywords, val);
    }

    val = LookupMetadataTemp(ctx, _doc, "encryption");
    if (val) {
        AddProp(keyValOut, kPropEncryption, val);
    }

    // pdf signatures (signed form widgets). Walks each page's widget set;
    // for each signature widget, pulls signer DN + cert/digest verdict via
    // the Windows CryptoAPI pdf_pkcs7_verifier.
    if (pdfdoc && pdf_count_signatures(ctx, pdfdoc) > 0) {
        StrBuilder sigs;
        pdf_pkcs7_verifier* verifier = nullptr;
        pdf_page* page = nullptr;
        fz_var(verifier);
        fz_var(page);
        fz_try(ctx) {
            verifier = pkcs7_windows_new_verifier(ctx);
            int totalPages = pdf_count_pages(ctx, pdfdoc);
            int sigNo = 0;
            for (int pageNo = 0; pageNo < totalPages; pageNo++) {
                page = pdf_load_page(ctx, pdfdoc, pageNo);
                for (pdf_annot* w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
                    if (pdf_widget_type(ctx, w) != PDF_WIDGET_TYPE_SIGNATURE) {
                        continue;
                    }
                    ++sigNo;
                    AppendSignatureInfo(ctx, sigs, verifier, pdfdoc, w, sigNo, pageNo + 1);
                }
                fz_drop_page(ctx, (fz_page*)page);
                page = nullptr;
            }
        }
        fz_always(ctx) {
            fz_drop_page(ctx, (fz_page*)page);
            pdf_drop_verifier(ctx, verifier);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
        if (!sigs.IsEmpty()) {
            AddProp(keyValOut, kPropSignatures, sigs.CStr());
        }
    }

    // for epub files, list all files in the archive
    const char* path = FilePath();
    if (path && str::EndsWithI(path, ".epub")) {
        ArchiveExtractProgressCb emptyCb;
        MultiFormatArchive* zip = OpenArchiveFromFile(path, ArchiveLoadMode::Lazy, emptyCb);
        if (zip) {
            StrBuilder filesStr;
            auto& fileInfos = zip->GetFileInfos();
            size_t n = fileInfos.size();
            for (size_t i = 0; i < n; i++) {
                auto* fi = fileInfos[i];
                if (str::IsEmpty(fi->name)) {
                    continue;
                }
                filesStr.AppendChar('\n');
                filesStr.Append(fi->name);
            }
            AddProp(keyValOut, kPropFiles, filesStr.CStr());
            delete zip;
        }
    }
}

ByteSlice EngineMupdf::GetFileData() {
    auto ctx = Ctx();

    if (!pdfdoc) {
        return {};
    }

    ByteSlice res;
    ScopedCritSec scope(&docLock);

    fz_var(res);
    fz_try(ctx) {
        res = FzExtractStreamData(ctx, pdfdoc->file);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        res = {};
    }

    if (!res.empty()) {
        return res;
    }

    auto path = FilePath();
    if (!path) {
        return {};
    }
    return file::ReadFile(path);
}

bool EngineMupdf::SaveFileAs(const char* dstPath) {
    ByteSlice d = GetFileData();
    if (!d.empty()) {
        bool ok = file::WriteFile(dstPath, d);
        d.Free();
        return ok;
    }
    auto srcPath = FilePath();
    if (!srcPath) {
        return false;
    }
    bool ok = file::Copy(dstPath, srcPath, false);
    return ok;
}

const pdf_write_options pdf_default_write_options2 = {
    0,  /* do_incremental */
    0,  /* do_pretty */
    0,  /* do_ascii */
    0,  /* do_compress */
    0,  /* do_compress_images */
    0,  /* do_compress_fonts */
    0,  /* do_decompress */
    0,  /* do_garbage */
    0,  /* do_linear */
    0,  /* do_clean */
    0,  /* do_sanitize */
    0,  /* do_appearance */
    0,  /* do_encrypt */
    0,  /* dont_regenerate_id */
    ~0, /* permissions */
    "", /* opwd_utf8[128] */
    "", /* upwd_utf8[128] */
};

bool EngineMupdfIsEncrypted(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->pdfdoc) {
        return false;
    }
    return epdf->pdfdoc->crypt != nullptr;
}

bool EngineMupdfIsReflowableLoadingInProgress(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return false;
    }
    return InterlockedCompareExchange(&epdf->reflowableLoadingInProgress, 0, 0) != 0;
}

int EngineGetProgressivePageCount(EngineBase* engine) {
    return EngineEbookGetFormattedPageCount(engine);
}

void EngineMupdfAckPdfDeferredUi(EngineBase* engine) {
    (void)engine;
}

bool EngineMupdfIsFollowThemeProbePending(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return false;
    }
    return InterlockedCompareExchange(&e->pdfFollowThemeProbePending, 0, 0) != 0;
}

void EngineMupdfEnsureFollowThemeProbeDone(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return;
    }
    if (InterlockedCompareExchange(&e->pdfFollowThemeProbePending, 0, 0) == 0) {
        return;
    }
    // Sync path for -render / tests: do not post UI notifications (no message loop).
    auto ctx = e->Ctx();
    {
        ScopedCritSec scope(&e->docLock);
        DetectFollowThemeDocBitmapRecolor(e, ctx);
    }
    InterlockedExchange(&e->pdfFollowThemeProbePending, 0);
    InterlockedExchange(&e->pdfFollowThemeProbeScheduled, 1);
}

int EngineMupdfGetFollowThemeDocClass(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return 0;
    }
    return e->followThemeDocBitmapRecolor;
}

int EngineMupdfGetFollowThemePageProbe(EngineBase* engine, int pageNo) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc || pageNo < 1 || pageNo > e->PageCount()) {
        return 0;
    }
    FzPageInfo* pi = e->pages[pageNo - 1];
    if (!pi) {
        return 0;
    }
    return (int)pi->followThemeScanProbe;
}

void EngineMupdfScheduleFollowThemeProbe(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return;
    }
    if (InterlockedCompareExchange(&e->pdfFollowThemeProbePending, 0, 0) == 0) {
        return;
    }
    if (!PdfFollowThemePreservesEmbeddedImageColors()) {
        InterlockedExchange(&e->pdfFollowThemeProbePending, 0);
        return;
    }
    if (InterlockedCompareExchange(&e->pdfFollowThemeProbeScheduled, 1, 0) != 0) {
        return;
    }
    auto fn = MkFunc0<EngineMupdf>(FinishPdfThemeProbeAsync, e);
    RunAsync(fn, "PdfThemeProbe");
}

bool EngineMupdfIsReflowWarmActive(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return false;
    }
    return InterlockedCompareExchange(&epdf->reflowWarmActive, 0, 0) != 0;
}

int EngineMupdfFastOutlinePageNo(EngineBase* engine, IPageDestination* dest) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !dest || dest->GetKind() != kindDestinationMupdf) {
        return 0;
    }
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    int ch = ReflowOutlineChapterIndex(link);
    int pageInChapter = 0;
    if (link->outline && link->outline->page.page >= 0) {
        pageInChapter = link->outline->page.page;
    }
    int pageNo = ReflowPageNoFromChapter(e, ch, pageInChapter);
    return pageNo > 0 ? pageNo : 0;
}

int EngineMupdfTocItemPageNoForSync(EngineBase* engine, IPageDestination* dest, int bakedPageNo) {
    if (!engine || engine->kind != kindEngineMupdf || EngineIsProgressiveEbookLoading(engine)) {
        return bakedPageNo > 0 ? bakedPageNo : 0;
    }
    if (!dest || dest->GetKind() != kindDestinationMupdf) {
        return bakedPageNo > 0 ? bakedPageNo : 0;
    }
    EngineMupdf* e = AsEngineMupdf(engine);
    // PDF dests keep a baked page; do not walk outline->uri (may be freed after TOC rewrite).
    if (e && e->pdfdoc && bakedPageNo > 0) {
        return bakedPageNo;
    }
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    const char* uri = MupdfDestUri(link);
    if (uri && str::FindChar(uri, '#') && e) {
        if (EpubMetaPageMapIsCurrent(e)) {
            int metaPage = EpubMetaLookupFragmentPage(e->epubMeta, uri);
            if (metaPage > 0) {
                return metaPage;
            }
        }
        int livePage = ResolveMupdfLinkPageNo1(e, uri, nullptr);
        if (livePage > 0) {
            return livePage;
        }
    }
    int live = EngineMupdfFastOutlinePageNo(engine, dest);
    if (live > 0) {
        return live;
    }
    return bakedPageNo > 0 ? bakedPageNo : 0;
}

int EngineMupdfResolveLinkPageNo(EngineBase* engine, IPageDestination* dest) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !dest || dest->GetKind() != kindDestinationMupdf) {
        return 0;
    }
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    const char* uri = MupdfDestUri(link);
    if (!uri || IsExternalLink(uri)) {
        return 0;
    }
    return ResolveMupdfLinkPageNo1(e, uri, nullptr);
}

bool EngineMupdfIsOutlineDestReachable(EngineBase* engine, IPageDestination* dest) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !dest || dest->GetKind() != kindDestinationMupdf) {
        return true;
    }
    if (!EngineMupdfIsReflowableLoadingInProgress(engine)) {
        return true;
    }
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    const char* uri = MupdfDestUri(link);
    if (uri && (IsExternalLink(uri) || IsExternalUrl(uri))) {
        return true;
    }
    // Called from the TOC paint path (custom draw): must not take docLock or do
    // any mupdf work here — the background loader can hold docLock for seconds
    // per chapter, which would freeze painting. All checks below are lock-free:
    // chapter index comes from the outline snapshot, counted is an atomic.
    // An entry is reachable once its chapter has been counted; until then it
    // paints grey and clicks are ignored.
    int ch = ReflowOutlineChapterIndex(link);
    if (ch >= 0) {
        int counted = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
        if (ch < counted) {
            return true;
        }
        if (uri && str::FindChar(uri, '#') && EngineMupdfFragmentInMeta(e, uri)) {
            return true;
        }
        return false;
    }
    if (link->pageNo > 0) {
        return link->pageNo <= engine->PageCount();
    }
    int pageNo = EngineMupdfFastOutlinePageNo(engine, dest);
    if (pageNo > 0) {
        return pageNo <= engine->PageCount();
    }
    // chapter unknown without resolving the uri (needs docLock): stay optimistic
    return true;
}

bool EngineMupdfSnapshotOutlineLink(IPageDestination* dest, char** uriOut, int* reflowChOut, float* xOut, float* yOut) {
    if (!dest || dest->GetKind() != kindDestinationMupdf || !uriOut) {
        return false;
    }
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    const char* uri = MupdfDestUri(link);
    if (link->outline || link->uriOwned) {
        if (xOut) {
            *xOut = link->outlineX;
        }
        if (yOut) {
            *yOut = link->outlineY;
        }
    } else {
        if (xOut) {
            *xOut = DEST_USE_DEFAULT;
        }
        if (yOut) {
            *yOut = DEST_USE_DEFAULT;
        }
    }
    if (str::IsEmpty(uri)) {
        return false;
    }
    *uriOut = str::Dup(uri);
    if (reflowChOut) {
        *reflowChOut = link->reflowOutlineChapter;
    }
    return true;
}

// During progressive EPUB load, navigate using chapter offsets. Fragment links
// need fz_resolve_link_dest after the target chapter is counted (its HTML is
// already laid out by fz_count_chapter_pages); map via loc.chapter/page so the
// result stays within pages counted so far.
static void ClearPendingReflowNavLocked(EngineMupdf* e) {
    if (!e) {
        return;
    }
    str::Free(e->pendingReflowNavUri);
    e->pendingReflowNavUri = nullptr;
    e->pendingReflowNavChapter = -1;
    e->pendingReflowNavDestX = DEST_USE_DEFAULT;
    e->pendingReflowNavDestY = DEST_USE_DEFAULT;
    e->pendingReflowNavResolved = false;
    e->pendingReflowNavResolvedPageNo = 0;
}

static void QueuePendingReflowNav(EngineMupdf* e, const char* uri, int ch, float destX, float destY) {
    if (!e || !uri || ch < 0) {
        return;
    }
    ScopedCritSec scope(&e->pendingReflowNavLock);
    if (e->pendingReflowNavUri && str::Eq(e->pendingReflowNavUri, uri)) {
        // same target already queued (possibly already resolved): keep state
        return;
    }
    ClearPendingReflowNavLocked(e);
    e->pendingReflowNavUri = str::Dup(uri);
    e->pendingReflowNavChapter = ch;
    e->pendingReflowNavDestX = destX;
    e->pendingReflowNavDestY = destY;
}

// Background worker for TOC/link navigation during progressive load. Counts
// chapters up to the target and, for #fragment links, lays out the target
// chapter to resolve the anchor. Both are too slow for the UI thread (a spine
// chapter of an anthology EPUB can take seconds to lay out); the UI completes
// the queued navigation from EbookPagesProgressUI when notified.
static void ReflowNavCountAsync(EngineMupdf* e) {
    if (!e) {
        return;
    }
    AtomicIntInc(&gDangerousThreadCount);
    defer {
        AtomicIntDec(&gDangerousThreadCount);
        InterlockedExchange(&e->reflowNavCountAsyncActive, 0);
    };

    AutoFreeStr path(str::Dup(e->FilePath()));
    for (;;) {
        // keep going even if progressive loading finished meanwhile: a nav
        // queued right before load-finish must still be resolved, otherwise
        // the user's TOC click is silently dropped
        if (InterlockedCompareExchange(&e->reflowableLoadAbort, 0, 0) != 0) {
            return;
        }
        AutoFreeStr uri;
        int target = -1;
        {
            ScopedCritSec scope(&e->pendingReflowNavLock);
            if (!e->pendingReflowNavUri || e->pendingReflowNavChapter < 0) {
                return;
            }
            if (e->pendingReflowNavResolved) {
                return;
            }
            uri.Set(str::Dup(e->pendingReflowNavUri));
            target = e->pendingReflowNavChapter;
        }
        int counted = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
        if (counted <= target) {
            CountReflowChaptersUpTo(e, target, path, /*isBackground*/ true, nullptr);
            NotifyEbookPagesLoadingProgress(path, false);
            continue;
        }
        if (!str::FindChar(uri, '#')) {
            // plain chapter link: UI can complete it cheaply now
            NotifyEbookPagesLoadingProgress(path, false);
            return;
        }
        // resolve the fragment anchor; this lays out the chapter HTML
        fz_link_dest ldest = fz_make_link_dest_none();
        {
            WaitForReflowUiDocLock(e);
            ScopedCritSec scope(&e->docLock);
            auto ctx = e->Ctx();
            fz_try(ctx) {
                ldest = fz_resolve_link_dest(ctx, e->_doc, uri);
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                ldest = fz_make_link_dest_none();
            }
        }
        int pageInChapter = ldest.loc.page >= 0 ? ldest.loc.page : 0;
        int resolveCh = ldest.loc.chapter >= 0 ? ldest.loc.chapter : target;
        int pageNo1 = ReflowPageNoFromChapter(e, resolveCh, pageInChapter);
        if (pageNo1 <= 0) {
            pageNo1 = ReflowPageNoFromChapter(e, target, 0);
        }
        if (pageNo1 > 0) {
            float x = isnan(ldest.x) ? 0.f : ldest.x;
            float y = isnan(ldest.y) ? 0.f : ldest.y;
            EngineMupdfUpsertFragmentInMeta(e, uri, resolveCh, pageInChapter, x, y, pageNo1);
        }
        {
            ScopedCritSec scope(&e->pendingReflowNavLock);
            if (e->pendingReflowNavUri && str::Eq(e->pendingReflowNavUri, uri)) {
                if (pageNo1 > 0) {
                    e->pendingReflowNavResolved = true;
                    e->pendingReflowNavResolvedPageNo = pageNo1;
                    e->pendingReflowNavResolvedDest = ldest;
                } else {
                    ClearPendingReflowNavLocked(e);
                }
            }
        }
        NotifyEbookPagesLoadingProgress(path, false);
        return;
    }
}

static void KickReflowNavCountAsync(EngineMupdf* e) {
    if (!e) {
        return;
    }
    if (InterlockedCompareExchange(&e->reflowNavCountAsyncActive, 1, 0) != 0) {
        return;
    }
    auto fn = MkFunc0<EngineMupdf>(ReflowNavCountAsync, e);
    RunAsync(fn, "ReflowNavCount");
}

static void NavigateMupdfToResolvedDest(EngineBase* engine, int pageNo1, const fz_link_dest& ldest, ILinkHandler* lh) {
    if (!engine || !lh || pageNo1 < 1) {
        return;
    }
    auto ctrl = lh->GetDocController();
    if (!ctrl) {
        return;
    }
    ctrl->PreparePageNavigation(pageNo1);
    if (pageNo1 > engine->PageCount()) {
        return;
    }
    float x = isnan(ldest.x) ? 0.f : ldest.x;
    float y = isnan(ldest.y) ? 0.f : ldest.y;
    float zoom = isnan(ldest.zoom) ? 0.f : ldest.zoom;
    zoom = zoom / 100;
    float w = isnan(ldest.w) ? DEST_USE_DEFAULT : ldest.w;
    float h = isnan(ldest.h) ? DEST_USE_DEFAULT : ldest.h;
    RectF r(x, y, w, h);
    ctrl->ScrollTo(pageNo1, r, zoom);
}

static bool ResolveReflowLinkDuringProgressiveLoad(EngineMupdf* e, const char* uri, int reflowOutlineChapter,
                                                   float destX, float destY, int* pageNo1Out, fz_link_dest* ldestOut) {
    if (!e || !uri || !pageNo1Out) {
        return false;
    }
    if (e->epubMetaLoaded) {
        int metaPage = EpubMetaLookupFragmentPage(e->epubMeta, uri);
        if (metaPage > 0) {
            *pageNo1Out = metaPage;
            if (ldestOut) {
                *ldestOut = fz_make_link_dest_xyz(0, 0, 0, 0, 0);
            }
            return true;
        }
    }
    int ch = reflowOutlineChapter;
    if (ch < 0) {
        // no fragment involved here, so this resolves the chapter by spine path
        // without laying anything out; AcquireReflowUiDocLock makes the
        // background loader yield instead of blocking us for seconds
        AcquireReflowUiDocLock(e);
        ch = EpubUriChapterIndexNoLayout(e->Ctx(), e->_doc, uri);
        ReleaseReflowUiDocLock(e);
    }
    if (ch < 0) {
        return false;
    }
    int counted = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
    if (counted <= ch) {
        QueuePendingReflowNav(e, uri, ch, destX, destY);
        KickReflowNavCountAsync(e);
        return false;
    }

    bool hasFragment = str::FindChar(uri, '#') != nullptr;
    if (!hasFragment) {
        int pageNo1 = ReflowPageNoFromChapter(e, ch, 0);
        if (pageNo1 <= 0) {
            return false;
        }
        *pageNo1Out = pageNo1;
        if (ldestOut) {
            *ldestOut = fz_make_link_dest_xyz(ch, 0, 0, 0, 0);
        }
        return true;
    }

    // Fragment anchors need a full chapter layout to locate; never do that on
    // the UI thread. Use a result the async worker already produced, otherwise
    // queue it and finish the navigation when the worker notifies us.
    {
        ScopedCritSec scope(&e->pendingReflowNavLock);
        if (e->pendingReflowNavResolved && e->pendingReflowNavUri && str::Eq(e->pendingReflowNavUri, uri)) {
            int pageNo1 = e->pendingReflowNavResolvedPageNo;
            fz_link_dest ldest = e->pendingReflowNavResolvedDest;
            ClearPendingReflowNavLocked(e);
            if (pageNo1 <= 0) {
                return false;
            }
            *pageNo1Out = pageNo1;
            if (ldestOut) {
                *ldestOut = ldest;
            }
            return true;
        }
    }
    QueuePendingReflowNav(e, uri, ch, destX, destY);
    KickReflowNavCountAsync(e);
    return false;
}

bool EngineMupdfHasPendingReflowNav(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e) {
        return false;
    }
    ScopedCritSec scope(&e->pendingReflowNavLock);
    return e->pendingReflowNavUri != nullptr && e->pendingReflowNavChapter >= 0;
}

bool EngineMupdfTryCompletePendingReflowNav(EngineBase* engine, ILinkHandler* lh) {
    // no loading-in-progress gate: a nav queued right before loading finished
    // must still complete, driven by the worker's final notification
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !lh) {
        return false;
    }
    AutoFreeStr uri;
    int ch = -1;
    float destX = DEST_USE_DEFAULT;
    float destY = DEST_USE_DEFAULT;
    {
        ScopedCritSec scope(&e->pendingReflowNavLock);
        if (!e->pendingReflowNavUri || e->pendingReflowNavChapter < 0) {
            return false;
        }
        int counted = (int)InterlockedCompareExchange(&e->reflowChaptersCounted, 0, 0);
        if (counted <= e->pendingReflowNavChapter) {
            return false;
        }
        // fragment targets must first be resolved by the async worker
        if (str::FindChar(e->pendingReflowNavUri, '#') && !e->pendingReflowNavResolved) {
            return false;
        }
        // don't clear yet: the resolved result is keyed on pendingReflowNavUri
        uri.Set(str::Dup(e->pendingReflowNavUri));
        ch = e->pendingReflowNavChapter;
        destX = e->pendingReflowNavDestX;
        destY = e->pendingReflowNavDestY;
    }

    int pageNo1 = 0;
    fz_link_dest ldest{};
    if (!ResolveReflowLinkDuringProgressiveLoad(e, uri, ch, destX, destY, &pageNo1, &ldest)) {
        return false;
    }
    {
        // fragment path already cleared inside Resolve; clear the plain path too
        ScopedCritSec scope(&e->pendingReflowNavLock);
        if (e->pendingReflowNavUri && str::Eq(e->pendingReflowNavUri, uri)) {
            ClearPendingReflowNavLocked(e);
        }
    }
    if (destX != DEST_USE_DEFAULT) {
        ldest.x = destX;
    }
    if (destY != DEST_USE_DEFAULT) {
        ldest.y = destY;
    }
    NavigateMupdfToResolvedDest(engine, pageNo1, ldest, lh);
    return true;
}

void EngineMupdfNavigateUri(EngineBase* engine, const char* uri, int reflowOutlineChapter, float destX, float destY,
                            ILinkHandler* lh) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || str::IsEmpty(uri) || !lh) {
        return;
    }
    if (IsExternalLink(uri)) {
        lh->LaunchURL(uri);
        return;
    }

    fz_link_dest ldest{};
    int pageNo1 = 0;
    bool reflowLoading = InterlockedCompareExchange(&e->reflowableLoadingInProgress, 0, 0) != 0;
    if (reflowLoading) {
        if (!ResolveReflowLinkDuringProgressiveLoad(e, uri, reflowOutlineChapter, destX, destY, &pageNo1, &ldest)) {
            if (EngineMupdfHasPendingReflowNav(e)) {
                NotifyEbookPagesLoadingProgress(e->FilePath(), false);
            }
            return;
        }
        if (destX != DEST_USE_DEFAULT) {
            ldest.x = destX;
        }
        if (destY != DEST_USE_DEFAULT) {
            ldest.y = destY;
        }
        NavigateMupdfToResolvedDest(engine, pageNo1, ldest, lh);
        return;
    } else {
        pageNo1 = ResolveMupdfLinkPageNo1(e, uri, &ldest);
        if (pageNo1 <= 0) {
            return;
        }
    }

    NavigateMupdfToResolvedDest(engine, pageNo1, ldest, lh);
}

static void CapturePdfTocEditNodes(fz_outline* outline, Vec<PdfTocEditNode*>& nodes) {
    for (; outline; outline = outline->next) {
        auto node = new PdfTocEditNode;
        node->title = str::Dup(outline->title ? outline->title : "");
        node->uri = str::Dup(MupdfSafeCStr(outline->uri));
        node->isOpen = outline->is_open != 0;
        node->flags = outline->flags;
        node->r = outline->r;
        node->g = outline->g;
        node->b = outline->b;
        CapturePdfTocEditNodes(outline->down, node->children);
        nodes.Append(node);
    }
}

static void InsertPdfTocEditNode(fz_context* ctx, fz_outline_iterator* iter, PdfTocEditNode* node) {
    fz_outline_item item{};
    item.title = node->title;
    item.uri = node->uri;
    item.is_open = node->isOpen;
    item.flags = node->flags;
    item.r = (float)node->r;
    item.g = (float)node->g;
    item.b = (float)node->b;
    fz_outline_iterator_insert(ctx, iter, &item);
    fz_outline_iterator_prev(ctx, iter);
    if (!node->children.empty()) {
        fz_outline_iterator_down(ctx, iter);
        for (PdfTocEditNode* child : node->children) {
            InsertPdfTocEditNode(ctx, iter, child);
            fz_outline_iterator_next(ctx, iter);
        }
        fz_outline_iterator_up(ctx, iter);
        // PDF inserts start childless and closed. Restore the source expanded
        // state only after the complete subtree has been inserted.
        fz_outline_iterator_update(ctx, iter, &item);
    }
}

static void RewritePdfTocFromModel(fz_context* ctx, pdf_document* doc, Vec<PdfTocEditNode*>& roots) {
    // Word/WPS thesis PDFs often store /Outlines or item /Parent as a dangling
    // indirect ref to a null object. Walking that tree to delete items throws
    // "not a dict (null)". Dropping the catalog key matches the empty-outline
    // insert path that already succeeded at runtime.
    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    if (pdf_is_dict(ctx, root)) {
        pdf_dict_del(ctx, root, PDF_NAME(Outlines));
    }
    fz_outline_iterator* iter = fz_new_outline_iterator(ctx, (fz_document*)doc);
    fz_var(iter);
    fz_try(ctx) {
        for (PdfTocEditNode* node : roots) {
            InsertPdfTocEditNode(ctx, iter, node);
            fz_outline_iterator_next(ctx, iter);
        }
    }
    fz_always(ctx) {
        fz_drop_outline_iterator(ctx, iter);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

bool EngineMupdfCanEditPdfToc(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return false;
    }
    ScopedCritSec scope(&e->docLock);
    return pdf_has_permission(e->Ctx(), e->pdfdoc, (fz_permission)PDF_PERM_MODIFY) != 0;
}

bool EngineMupdfPdfHasSignatures(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return false;
    }
    ScopedCritSec scope(&e->docLock);
    return pdf_count_signatures(e->Ctx(), e->pdfdoc) > 0;
}

char* EngineMupdfFormatPdfTocTarget(EngineBase* engine, int pageNo, float x, float y) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc || pageNo < 1 || pageNo > e->PageCount()) {
        return nullptr;
    }
    char* uri = nullptr;
    char* result = nullptr;
    auto ctx = e->Ctx();
    ScopedCritSec scope(&e->docLock);
    fz_try(ctx) {
        fz_link_dest dest;
        if (x <= 0 && y <= 0) {
            dest = fz_make_link_dest_xyz(0, pageNo - 1, NAN, NAN, NAN);
        } else {
            dest = fz_make_link_dest_xyz(0, pageNo - 1, x, y, 100);
        }
        uri = fz_format_link_uri(ctx, e->_doc, dest);
        result = str::Dup(uri);
    }
    fz_always(ctx) {
        fz_free(ctx, uri);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return result;
}

bool EngineMupdfUpdatePageDest(EngineBase* engine, IPageDestination* dest, int pageNo, float x, float y) {
    if (!dest || pageNo < 1) {
        return false;
    }
    dest->pageNo = pageNo;
    if (dest->GetKind() != kindDestinationMupdf) {
        return true;
    }
    PageDestinationMupdf* link = (PageDestinationMupdf*)dest;
    link->outlineX = x;
    link->outlineY = y;
    str::Free(link->uriOwned);
    link->uriOwned = EngineMupdfFormatPdfTocTarget(engine, pageNo, x, y);
    return true;
}

bool EngineMupdfEditPdfToc(EngineBase* engine, PdfTocEditAction action, const Vec<int>& path, const char* title,
                           const char* uri, Vec<int>* resultPathOut, char** errorOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (errorOut) {
        *errorOut = nullptr;
    }
    if (!e || !e->pdfdoc || !EngineMupdfCanEditPdfToc(engine)) {
        return false;
    }

    Vec<PdfTocEditNode*> roots;
    defer {
        DeletePdfTocEditNodes(roots);
    };

    bool ok = false;
    auto ctx = e->Ctx();
    ScopedCritSec scope(&e->docLock);
    fz_try(ctx) {
        CapturePdfTocEditNodes(e->outline, roots);
        if (!ApplyPdfTocEditToModel(roots, action, path, title, uri, resultPathOut)) {
            bool moveNoop = action == PdfTocEditAction::MoveUp || action == PdfTocEditAction::MoveDown ||
                            action == PdfTocEditAction::Promote || action == PdfTocEditAction::Demote;
            if (moveNoop) {
                ok = true;
            } else {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid PDF table of contents edit");
            }
        } else {
            pdf_begin_operation(ctx, e->pdfdoc, "Edit PDF table of contents");
            fz_try(ctx) {
                RewritePdfTocFromModel(ctx, e->pdfdoc, roots);
                pdf_end_operation(ctx, e->pdfdoc);
            }
            fz_catch(ctx) {
                pdf_abandon_operation(ctx, e->pdfdoc);
                fz_rethrow(ctx);
            }
            EngineMupdfReloadOutlineAfterWrite(e);
            e->modifiedPdfToc = true;
            ok = true;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (errorOut) {
            *errorOut = str::Dup(fz_caught_message(ctx));
        }
    }
    return ok;
}

bool EngineMupdfEditPdfTocMany(EngineBase* engine, PdfTocEditAction action, const Vec<PdfTocPath>& paths,
                               Vec<PdfTocPath>* resultPathsOut, char** errorOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (errorOut) {
        *errorOut = nullptr;
    }
    if (!e || !e->pdfdoc || !EngineMupdfCanEditPdfToc(engine) || paths.empty()) {
        return false;
    }

    Vec<PdfTocEditNode*> roots;
    defer {
        DeletePdfTocEditNodes(roots);
    };

    bool ok = false;
    auto ctx = e->Ctx();
    ScopedCritSec scope(&e->docLock);
    fz_try(ctx) {
        CapturePdfTocEditNodes(e->outline, roots);
        bool applied = false;
        switch (action) {
            case PdfTocEditAction::Delete:
                applied = PdfTocEditDeleteMany(roots, paths);
                break;
            case PdfTocEditAction::MoveUp:
                applied = PdfTocEditMoveUpMany(roots, paths, resultPathsOut);
                break;
            case PdfTocEditAction::MoveDown:
                applied = PdfTocEditMoveDownMany(roots, paths, resultPathsOut);
                break;
            case PdfTocEditAction::Promote:
                applied = PdfTocEditPromoteMany(roots, paths, resultPathsOut);
                break;
            case PdfTocEditAction::Demote:
                applied = PdfTocEditDemoteMany(roots, paths, resultPathsOut);
                break;
            default:
                applied = false;
                break;
        }
        if (!applied) {
            bool moveNoop = action == PdfTocEditAction::MoveUp || action == PdfTocEditAction::MoveDown ||
                            action == PdfTocEditAction::Promote || action == PdfTocEditAction::Demote;
            if (moveNoop) {
                ok = true;
            } else {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid PDF table of contents edit");
            }
        } else {
            pdf_begin_operation(ctx, e->pdfdoc, "Edit PDF table of contents");
            fz_try(ctx) {
                RewritePdfTocFromModel(ctx, e->pdfdoc, roots);
                pdf_end_operation(ctx, e->pdfdoc);
            }
            fz_catch(ctx) {
                pdf_abandon_operation(ctx, e->pdfdoc);
                fz_rethrow(ctx);
            }
            EngineMupdfReloadOutlineAfterWrite(e);
            e->modifiedPdfToc = true;
            ok = true;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (errorOut) {
            *errorOut = str::Dup(fz_caught_message(ctx));
        }
    }
    return ok;
}

bool EngineMupdfMovePdfTocItems(EngineBase* engine, const Vec<PdfTocPath>& srcPaths, const Vec<int>& destPath,
                                PdfTocDropPos pos, Vec<PdfTocPath>* resultPathsOut, char** errorOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (errorOut) {
        *errorOut = nullptr;
    }
    if (!e || !e->pdfdoc || !EngineMupdfCanEditPdfToc(engine) || srcPaths.empty()) {
        return false;
    }

    Vec<PdfTocEditNode*> roots;
    defer {
        DeletePdfTocEditNodes(roots);
    };

    bool ok = false;
    auto ctx = e->Ctx();
    ScopedCritSec scope(&e->docLock);
    fz_try(ctx) {
        CapturePdfTocEditNodes(e->outline, roots);
        if (!PdfTocEditMoveMany(roots, srcPaths, destPath, pos, resultPathsOut)) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "invalid PDF table of contents move");
        }
        pdf_begin_operation(ctx, e->pdfdoc, "Move PDF table of contents");
        fz_try(ctx) {
            RewritePdfTocFromModel(ctx, e->pdfdoc, roots);
            pdf_end_operation(ctx, e->pdfdoc);
        }
        fz_catch(ctx) {
            pdf_abandon_operation(ctx, e->pdfdoc);
            fz_rethrow(ctx);
        }
        EngineMupdfReloadOutlineAfterWrite(e);
        e->modifiedPdfToc = true;
        ok = true;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (errorOut) {
            *errorOut = str::Dup(fz_caught_message(ctx));
        }
    }
    return ok;
}

bool EngineMupdfHasStoredOutline(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e) {
        return false;
    }
    ScopedCritSec scope(&e->docLock);
    return e->outline != nullptr;
}

static PdfTocEditNode* PdfTocNodeFromExtracted(EngineBase* engine, ExtractedTocItem* src) {
    auto node = new PdfTocEditNode;
    const char* shown = nullptr;
    if (src) {
        shown = src->title && src->title[0] ? src->title : src->rawTitle;
    }
    node->title = str::Dup(shown ? shown : "");
    if (src) {
        node->uri = EngineMupdfFormatPdfTocTarget(engine, src->pageNo, src->x, src->y);
        node->isOpen = src->level <= 2;
        for (ExtractedTocItem* child : src->children) {
            node->children.Append(PdfTocNodeFromExtracted(engine, child));
        }
    }
    return node;
}

bool EngineMupdfReplacePdfToc(EngineBase* engine, Vec<ExtractedTocItem*>& roots, char** errorOut) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (errorOut) {
        *errorOut = nullptr;
    }
    if (!e || !e->pdfdoc || !EngineMupdfCanEditPdfToc(engine)) {
        return false;
    }

    Vec<PdfTocEditNode*> editRoots;
    defer {
        DeletePdfTocEditNodes(editRoots);
    };
    for (ExtractedTocItem* r : roots) {
        editRoots.Append(PdfTocNodeFromExtracted(engine, r));
    }

    bool ok = false;
    auto ctx = e->Ctx();
    ScopedCritSec scope(&e->docLock);
    fz_try(ctx) {
        pdf_begin_operation(ctx, e->pdfdoc, "Extract PDF table of contents");
        fz_try(ctx) {
            RewritePdfTocFromModel(ctx, e->pdfdoc, editRoots);
            pdf_end_operation(ctx, e->pdfdoc);
        }
        fz_catch(ctx) {
            pdf_abandon_operation(ctx, e->pdfdoc);
            fz_rethrow(ctx);
        }
        EngineMupdfReloadOutlineAfterWrite(e);
        e->modifiedPdfToc = true;
        ok = true;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (errorOut) {
            *errorOut = str::Dup(fz_caught_message(ctx));
        }
    }
    return ok;
}

bool EngineMupdfHasOutline(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return false;
    }
    if (epdf->tocTree) {
        return true;
    }
    return epdf->outline != nullptr || epdf->attachments != nullptr || EngineMupdfCanEditPdfToc(engine);
}

bool EngineMupdfFirstPageLooksFixedLayout(EngineBase* engine) {
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || e->pdfdoc) {
        return false;
    }
    if (e->defaultExt && str::EqI(e->defaultExt, ".pdf")) {
        return false;
    }
    if (e->reflowLayoutW <= 0 || e->reflowLayoutH <= 0) {
        return false;
    }
    RectF box = engine->PageMediabox(1);
    if (box.IsEmpty() || box.dx <= 0 || box.dy <= 0) {
        return false;
    }
    return box.dx > e->reflowLayoutW * 1.15f || box.dy > e->reflowLayoutH * 1.15f;
}

const char* EngineMupdfGetPassword(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return nullptr;
    }
    return epdf->pdfPassword;
}

static bool PdfSaveDocumentToPath(fz_context* ctx, pdf_document* doc, const char* dest, pdf_write_options* opts,
                                  const char** errOut) {
    bool ok = false;
    fz_var(ok);
    fz_try(ctx) {
        pdf_save_document(ctx, doc, dest, opts);
        ok = true;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (errOut) {
            *errOut = fz_caught_message(ctx);
        }
    }
    return ok;
}

// Sidecar next to the open PDF (same volume as dest so replace-after-close works).
// Do not use GetTempFileNameW / %TEMP%\smpXXXX.tmp: that name is what users then see
// if replacing the original file fails.
static char* DupPdfOverwriteSidecarPath(const char* destPath) {
    if (str::IsEmpty(destPath)) {
        return nullptr;
    }
    TempStr sidecar = str::JoinTemp(destPath, ".sumatra-tmp");
    sidecar = MakeUniqueFilePathTemp(sidecar);
    return str::Dup(sidecar);
}

static char* DupPdfTempDirFallbackPath(const char* destPath) {
    TempStr tmpDir = GetTempFilePathTemp(nullptr);
    if (str::IsEmpty(tmpDir)) {
        return nullptr;
    }
    const char* base = path::GetBaseNameTemp(destPath);
    if (str::IsEmpty(base)) {
        base = "saved.pdf";
    }
    if (!str::EndsWithI(base, ".pdf")) {
        base = str::JoinTemp(base, ".pdf");
    }
    TempStr candidate = path::JoinTemp(tmpDir, base);
    candidate = MakeUniqueFilePathTemp(candidate);
    return str::Dup(candidate);
}

// re-save current pdf document using mupdf (as opposed to just saving the data)
// this is used after the PDF was modified by the user (e.g. by adding / changing
// annotations).
// if filePath is not given, we save under the same name
// Overwriting the open file can fail (file locked, long path). If overwriteTempOut
// is set, fall back to a full rewrite next to dest (or %TEMP% as .pdf) and let the
// caller replace-and-reload (same pattern as OCR save).
bool EngineMupdfSaveUpdated(EngineBase* engine, const char* path, const ShowErrorCb& showErrorFunc,
                            char** overwriteTempOut) {
    ReportIf(!engine);
    if (overwriteTempOut) {
        *overwriteTempOut = nullptr;
    }
    if (!engine) {
        return false;
    }
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->pdfdoc) {
        return false;
    }
    if (!EngineMupdfHasUnsavedPdfChanges(engine)) {
        return false;
    }

    auto timeStart = TimeGet();
    const char* currPath = engine->FilePath();
    bool overwriteSelf = str::IsEmpty(path) || (currPath && path::IsSame(path, currPath));
    if (str::IsEmpty(path)) {
        path = currPath;
    }
    auto ctx = epdf->Ctx();
    ScopedCritSec scope(&epdf->docLock);

    pdf_write_options save_opts{};
    save_opts = pdf_default_write_options2;
    save_opts.do_incremental = pdf_can_be_saved_incrementally(ctx, epdf->pdfdoc);
    save_opts.do_compress = 1;
    save_opts.do_compress_images = 1;
    save_opts.do_compress_fonts = 1;
    if (epdf->pdfdoc->redacted) {
        save_opts.do_garbage = 1;
    }

    const char* mupdfErr = nullptr;
    bool ok = PdfSaveDocumentToPath(ctx, epdf->pdfdoc, path, &save_opts, &mupdfErr);
    if (ok) {
        auto dur = TimeSinceInMs(timeStart);
        logf("Saved PDF changes to '%s' in  %.2f ms, incremental: %d\n", path, dur, save_opts.do_incremental);
        epdf->modifiedAnnotations = false;
        epdf->modifiedPdfToc = false;
        return true;
    }
    logf("Saving '%s' failed with: '%s'\n", path, mupdfErr ? mupdfErr : "");

    if (overwriteSelf && overwriteTempOut) {
        save_opts.do_incremental = 0;
        save_opts.do_garbage = 1;
        for (int pass = 0; pass < 2; pass++) {
            char* tmp = (pass == 0) ? DupPdfOverwriteSidecarPath(path) : DupPdfTempDirFallbackPath(path);
            if (!tmp) {
                continue;
            }
            const char* tmpErr = nullptr;
            ok = PdfSaveDocumentToPath(ctx, epdf->pdfdoc, tmp, &save_opts, &tmpErr);
            if (ok) {
                *overwriteTempOut = tmp;
                auto dur = TimeSinceInMs(timeStart);
                logf("Saved PDF changes via temp '%s' in %.2f ms\n", tmp, dur);
                epdf->modifiedAnnotations = false;
                epdf->modifiedPdfToc = false;
                return true;
            }
            logf("Saving temp '%s' failed with: '%s'\n", tmp, tmpErr ? tmpErr : "");
            file::Delete(tmp);
            str::Free(tmp);
            if (tmpErr) {
                mupdfErr = tmpErr;
            }
        }
    }

    if (showErrorFunc.IsValid()) {
        showErrorFunc.Call(mupdfErr ? mupdfErr : "Could not save PDF.");
    }
    return false;
}

static fz_font* TryFontFile(fz_context* ctx, const char* path) {
    if (!path || !file::Exists(path)) {
        return nullptr;
    }
    fz_font* font = nullptr;
    fz_try(ctx) {
        font = fz_new_font_from_file(ctx, nullptr, path, 0, 0);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        font = nullptr;
    }
    return font;
}

static fz_font* LoadSearchablePdfFont(fz_context* ctx) {
    TempStr exeDir = path::GetDirTemp(GetSelfExePathTemp());
    fz_font* font = TryFontFile(ctx, path::JoinTemp(exeDir, "fonts", "SourceHanSerifSC-Regular.otf"));
    if (font) {
        return font;
    }
    font = TryFontFile(ctx, path::JoinTemp(exeDir, "fonts", "SourceHanSerif-Regular.ttc"));
    if (font) {
        return font;
    }
    WCHAR winDirW[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDirW, dimof(winDirW)) == 0) {
        return nullptr;
    }
    TempStr winFonts = path::JoinTemp(ToUtf8Temp(winDirW), "Fonts");
    font = TryFontFile(ctx, path::JoinTemp(winFonts, "msyh.ttc"));
    if (font) {
        return font;
    }
    font = TryFontFile(ctx, path::JoinTemp(winFonts, "msyh.ttf"));
    if (font) {
        return font;
    }
    font = TryFontFile(ctx, path::JoinTemp(winFonts, "simsun.ttc"));
    if (font) {
        return font;
    }
    font = TryFontFile(ctx, path::JoinTemp(winFonts, "simsun.ttf"));
    if (font) {
        return font;
    }
    return TryFontFile(ctx, path::JoinTemp(winFonts, "arialuni.ttf"));
}

static int OcrDropAllTextFilter(fz_context*, void*, int*, int, fz_matrix, fz_matrix, fz_rect, int, float, float) {
    return 1;
}

// Full-page (or nearly full-page) raster: safe to drop the old text layer.
static constexpr float kOcrScanImagePageOverlap = 0.4f;

static bool PdfPageHasLargeScanImage(fz_context* ctx, pdf_document* doc, int pageIndex) {
    pdf_page* page = nullptr;
    fz_device* dev = nullptr;
    Vec<FitzPageImageInfo*> images;
    bool large = false;
    fz_var(page);
    fz_var(dev);
    fz_try(ctx) {
        page = pdf_load_page(ctx, doc, pageIndex);
        fz_rect mbox = pdf_bound_page(ctx, page, FZ_CROP_BOX);
        float pageArea = (mbox.x1 - mbox.x0) * (mbox.y1 - mbox.y0);
        if (pageArea > 1.f) {
            dev = FzNewImageCollectDevice(ctx, &images, pageIndex + 1);
            fz_run_page(ctx, &page->super, dev, fz_identity, nullptr);
            for (FitzPageImageInfo* img : images) {
                if (img && FzRectOverlap(mbox, img->rect) >= kOcrScanImagePageOverlap) {
                    large = true;
                    break;
                }
            }
        }
    }
    fz_always(ctx) {
        if (dev) {
            fz_drop_device(ctx, dev);
        }
        if (page) {
            fz_drop_page(ctx, &page->super);
        }
        for (FitzPageImageInfo* img : images) {
            if (img && img->image) {
                fz_drop_image(ctx, img->image);
                img->image = nullptr;
            }
        }
        DeleteVecMembers(images);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        large = false;
    }
    return large;
}

// Invisible text (Tr 3) does not paint. A flat fill also has no contrast.
static bool PdfPageHasVisibleContrast(fz_context* ctx, pdf_document* doc, int pageIndex) {
    pdf_page* page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    bool contrast = false;
    fz_var(page);
    fz_var(pix);
    fz_var(dev);
    fz_try(ctx) {
        page = pdf_load_page(ctx, doc, pageIndex);
        fz_rect bounds = pdf_bound_page(ctx, page, FZ_CROP_BOX);
        float w = bounds.x1 - bounds.x0;
        float h = bounds.y1 - bounds.y0;
        float longest = w > h ? w : h;
        if (longest >= 1.f) {
            float scale = 72.f / longest;
            fz_matrix ctm = fz_scale(scale, scale);
            fz_irect ibounds = fz_round_rect(fz_transform_rect(bounds, ctm));
            int pw = ibounds.x1 - ibounds.x0;
            int ph = ibounds.y1 - ibounds.y0;
            if (pw > 0 && ph > 0) {
                pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), ibounds, nullptr, 0);
                fz_clear_pixmap_with_value(ctx, pix, 0xff);
                dev = fz_new_draw_device(ctx, ctm, pix);
                fz_run_page(ctx, &page->super, dev, fz_identity, nullptr);
                fz_close_device(ctx, dev);
                fz_drop_device(ctx, dev);
                dev = nullptr;
                int width = fz_pixmap_width(ctx, pix);
                int height = fz_pixmap_height(ctx, pix);
                const unsigned char* samples = fz_pixmap_samples(ctx, pix);
                int stride = pix->stride;
                int mn = 255;
                int mx = 0;
                for (int y = 0; y < height; y++) {
                    const unsigned char* row = samples + y * stride;
                    for (int x = 0; x < width; x++) {
                        int v = row[x];
                        if (v < mn) {
                            mn = v;
                        }
                        if (v > mx) {
                            mx = v;
                        }
                    }
                }
                contrast = (mx - mn) >= 12;
            }
        }
    }
    fz_always(ctx) {
        if (dev) {
            fz_drop_device(ctx, dev);
        }
        if (pix) {
            fz_drop_pixmap(ctx, pix);
        }
        if (page) {
            fz_drop_page(ctx, &page->super);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        // Failed to inspect: keep existing paint rather than blank the page.
        contrast = true;
    }
    return contrast;
}

// Drop existing PDF text (including a garbled OCR layer) but keep images/paths.
static void StripExistingPdfText(fz_context* ctx, pdf_document* doc, int pageIndex) {
    pdf_page* page = nullptr;
    pdf_filter_options options = {0};
    pdf_sanitize_filter_options sopts = {0};
    pdf_filter_factory list[2] = {0};
    sopts.text_filter = OcrDropAllTextFilter;
    options.recurse = 1;
    options.instance_forms = 1;
    options.filters = list;
    list[0].filter = pdf_new_sanitize_filter;
    list[0].options = &sopts;
    fz_try(ctx) {
        page = pdf_load_page(ctx, doc, pageIndex);
        pdf_filter_page_contents(ctx, doc, page, &options);
    }
    fz_always(ctx) {
        if (page) {
            fz_drop_page(ctx, &page->super);
        }
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

static bool AppendOcrTextLayer(fz_context* ctx, pdf_document* doc, pdf_obj* fontObj, int pageIndex, const char* text,
                               const Rect* coords, int len, bool visible) {
    if (!text || !coords || len <= 0) {
        return false;
    }
    pdf_obj* pageobj = pdf_lookup_page_obj(ctx, doc, pageIndex);
    if (!pageobj) {
        return false;
    }
    fz_rect mbox{};
    fz_matrix pageCtm{};
    pdf_page_obj_transform(ctx, pageobj, &mbox, &pageCtm);
    fz_matrix inv = fz_invert_matrix(pageCtm);

    pdf_obj* resources = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
    if (!resources) {
        resources = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Resources), resources);
        resources = pdf_dict_get(ctx, pageobj, PDF_NAME(Resources));
    }
    pdf_obj* fonts = pdf_dict_get(ctx, resources, PDF_NAME(Font));
    if (!fonts) {
        fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put_drop(ctx, resources, PDF_NAME(Font), fonts);
        fonts = pdf_dict_get(ctx, resources, PDF_NAME(Font));
    }
    pdf_dict_puts(ctx, fonts, "OCR0", fontObj);

    fz_buffer* buf = fz_new_buffer(ctx, 1024);
    fz_append_string(ctx, buf, "\nq\n");
    fz_append_printf(ctx, buf, "%g %g %g %g %g %g cm\n", inv.a, inv.b, inv.c, inv.d, inv.e, inv.f);
    if (visible) {
        fz_append_string(ctx, buf, "BT\n0 Tr\n0 g\n/OCR0 1 Tf\n");
    } else {
        fz_append_string(ctx, buf, "BT\n3 Tr\n/OCR0 1 Tf\n");
    }
    int idx = 0;
    while (idx < len) {
        int before = idx;
        int cp = Utf8CodepointNext(text, len, idx);
        if (idx <= before) {
            break;
        }
        if (cp <= 32 || str::IsWs((WCHAR)cp)) {
            continue;
        }
        Rect r = coords[before];
        if (r.IsEmpty() || r.dx < 1 || r.dy < 1) {
            continue;
        }
        float fs = (float)r.dy;
        if (fs < 1.f) {
            fs = 1.f;
        }
        // UniGB-UTF16-H: Tj bytes are UTF-16BE Unicode, not glyph IDs.
        if (cp > 0xFFFF) {
            unsigned int u = (unsigned int)cp - 0x10000;
            fz_append_printf(ctx, buf, "%g 0 0 %g %g %g Tm\n<%04X%04X> Tj\n", fs, fs, (float)r.x, (float)r.y,
                             0xD800 + (u >> 10), 0xDC00 + (u & 0x3FF));
        } else {
            fz_append_printf(ctx, buf, "%g 0 0 %g %g %g Tm\n<%04X> Tj\n", fs, fs, (float)r.x, (float)r.y, cp);
        }
    }
    fz_append_string(ctx, buf, "ET\nQ\n");

    pdf_obj* stm = pdf_add_new_dict(ctx, doc, 0);
    pdf_update_stream(ctx, doc, stm, buf, 0);
    fz_drop_buffer(ctx, buf);
    pdf_obj* contents = pdf_dict_get(ctx, pageobj, PDF_NAME(Contents));
    if (pdf_is_array(ctx, contents)) {
        pdf_array_push(ctx, contents, stm);
    } else if (contents) {
        pdf_obj* arr = pdf_new_array(ctx, doc, 2);
        pdf_array_push(ctx, arr, contents);
        pdf_array_push(ctx, arr, stm);
        pdf_dict_put_drop(ctx, pageobj, PDF_NAME(Contents), arr);
    } else {
        pdf_dict_put(ctx, pageobj, PDF_NAME(Contents), stm);
    }
    pdf_drop_obj(ctx, stm);
    return true;
}

bool EngineMupdfSaveSearchablePdf(EngineBase* engine, const char* destPath, char** errOut) {
    if (errOut) {
        *errOut = nullptr;
    }
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->pdfdoc || str::IsEmpty(destPath)) {
        if (errOut) {
            *errOut = str::Dup("Not a PDF document.");
        }
        return false;
    }
    const char* srcPath = engine->FilePath();
    if (str::IsEmpty(srcPath) || !file::Exists(srcPath)) {
        if (errOut) {
            *errOut = str::Dup("Original PDF path is not available.");
        }
        return false;
    }
    const char* openPath = srcPath;
    if (!str::IsEmpty(destPath) && !path::IsSame(srcPath, destPath) && file::Exists(destPath)) {
        openPath = destPath;
    }

    struct PageOcrDump {
        int pageNo = 0;
        char* text = nullptr;
        Rect* coords = nullptr;
        int len = 0;
        bool writeLayer = false;
        bool visibleLayer = false;
    };
    Vec<PageOcrDump> pages;
    int nPages = engine->PageCount();
    for (int pageNo = 1; pageNo <= nPages; pageNo++) {
        if (!engine->HasCachedOcrText(pageNo)) {
            continue;
        }
        int len = 0;
        Rect* coords = nullptr;
        const char* text = engine->GetTextForPageUtf8(pageNo, &len, &coords);
        if (!text || len <= 0 || !coords) {
            continue;
        }
        PageOcrDump p;
        p.pageNo = pageNo;
        p.len = len;
        p.text = str::Dup(text);
        p.coords = (Rect*)memdup(coords, (size_t)len * sizeof(Rect));
        pages.Append(p);
    }
    if (pages.Size() == 0) {
        if (errOut) {
            *errOut = str::Dup("No OCR text to save. Recognize pages first.");
        }
        return false;
    }

    auto ctx = epdf->Ctx();
    pdf_document* doc = nullptr;
    fz_font* font = nullptr;
    pdf_obj* fontObj = nullptr;
    bool ok = false;
    ScopedCritSec scope(&epdf->docLock);
    fz_var(doc);
    fz_var(font);
    fz_var(fontObj);
    fz_try(ctx) {
        doc = pdf_open_document(ctx, openPath);
        if (pdf_needs_password(ctx, doc) && epdf->pdfPassword) {
            pdf_authenticate_password(ctx, doc, epdf->pdfPassword);
        }
        font = LoadSearchablePdfFont(ctx);
        if (!font) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "Could not load a font for the hidden text layer.");
        }
        bool anyLayer = false;
        for (int i = 0; i < pages.Size(); i++) {
            PageOcrDump& p = pages[i];
            // Scan image: drop a garbled text layer, keep the picture, write invisible OCR.
            // Born-digital / Type3: do not strip — that was blanking the page.
            // Already-blank (or only Tr 3): write visible OCR so the file is readable.
            bool hasScanImage = PdfPageHasLargeScanImage(ctx, doc, p.pageNo - 1);
            if (hasScanImage) {
                p.writeLayer = true;
                p.visibleLayer = false;
            } else if (!PdfPageHasVisibleContrast(ctx, doc, p.pageNo - 1)) {
                p.writeLayer = true;
                p.visibleLayer = true;
            }
            if (p.writeLayer) {
                StripExistingPdfText(ctx, doc, p.pageNo - 1);
                anyLayer = true;
            }
        }
        if (anyLayer) {
            // Add the CID font after stripping so filter/GC cannot reuse its xref for a content stream.
            fontObj = pdf_add_cjk_font(ctx, doc, font, FZ_ADOBE_GB, 0, 1);
            for (int i = 0; i < pages.Size(); i++) {
                PageOcrDump& p = pages[i];
                if (p.writeLayer) {
                    AppendOcrTextLayer(ctx, doc, fontObj, p.pageNo - 1, p.text, p.coords, p.len, p.visibleLayer);
                }
            }
        }
        pdf_write_options saveOpts = pdf_default_write_options2;
        saveOpts.do_incremental = 0;
        saveOpts.do_compress = 1;
        saveOpts.do_garbage = 1;
        pdf_save_document(ctx, doc, destPath, &saveOpts);
        ok = true;
    }
    fz_always(ctx) {
        if (fontObj) {
            pdf_drop_obj(ctx, fontObj);
        }
        if (font) {
            fz_drop_font(ctx, font);
        }
        if (doc) {
            pdf_drop_document(ctx, doc);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        if (errOut) {
            const char* mupdfErr = fz_caught_message(ctx);
            *errOut = str::Dup(mupdfErr ? mupdfErr : "Could not save searchable PDF.");
        }
        ok = false;
    }
    for (int i = 0; i < pages.Size(); i++) {
        str::Free(pages[i].text);
        free(pages[i].coords);
    }
    return ok;
}

bool EngineMupdf::HasClipOptimizations(int pageNo) {
    if (!pdfdoc) {
        return false;
    }

    pageNo = MapJoinDisplayPageNo(this, pageNo);
    if (pageNo < 1) {
        return false;
    }

    FzPageInfo* pageInfo = GetFzPageInfoFast(pageNo);
    if (!pageInfo || !pageInfo->page) {
        return false;
    }

    fz_rect mbox = ToFzRect(pageInfo->mediabox);
    // check if any image covers at least 90% of the page
    for (auto& img : pageInfo->images) {
        fz_rect ir = img->rect;
        if (FzRectOverlap(mbox, ir) >= 0.9f) {
            return false;
        }
    }
    return true;
}

TempStr EngineMupdf::GetPageLabeTemp(int pageNo) const {
    // Collapsed display pages use sequential numbers; raw PDF labels would drift.
    if (joinDisplayToEngine.Size() > 0) {
        return EngineBase::GetPageLabeTemp(pageNo);
    }
    if (!pageLabels || pageNo < 1 || PageCount() < pageNo || pageNo > pageLabels->Size()) {
        return EngineBase::GetPageLabeTemp(pageNo);
    }

    char* res = pageLabels->At(pageNo - 1);
    if (str::IsEmpty(res) || str::ContainsI(res, ".pdg")) {
        return EngineBase::GetPageLabeTemp(pageNo);
    }
    return res;
}

int EngineMupdf::GetPageByLabel(const char* label) const {
    if (!pdfdoc) {
        // non-pdf documents don't have labels so label is just a page number as string
        return EngineBase::GetPageByLabel(label);
    }
    if (joinDisplayToEngine.Size() > 0) {
        return EngineBase::GetPageByLabel(label);
    }
    int pageNo = 0;
    if (pageLabels) {
        pageNo = pageLabels->Find(label) + 1;
    }

    if (!pageNo) {
        return EngineBase::GetPageByLabel(label);
    }

    return pageNo;
}

bool IsEngineMupdfSupportedFileType(Kind kind) {
    if (kind == kindFilePDF) {
        return true;
    }
    if (kind == kindFileEpub) {
        return true;
    }
    if (kind == kindFileFb2) {
        return true;
    }
    if (kind == kindFileFb2z) {
        return true;
    }
    if (kind == kindFileHTML) {
        return true;
    }
    if (kind == kindFileSvg) {
        return true;
    }
    if (kind == kindFileXps) {
        return true;
    }
    if (kind == kindFileTxt) {
        return true;
    }
    if (kind == kindFileMd) {
        return true;
    }
    if (kind == kindFileOffice) {
        return true;
    }
    if (kind == kindFilePalmDoc) {
        return true;
    }
    return false;
}

EngineBase* CreateEngineMupdfFromFile(const char* path, Kind kind, int displayDPI, PasswordUI* pwdUI) {
    if (str::IsEmpty(path)) {
        return nullptr;
    }
    if (kind == kindFileFb2z) {
        AutoDelete archive = OpenArchiveFromFile(path, ArchiveLoadMode::Eager, gArchiveProgressCb);
        if (!archive) {
            return nullptr;
        }
        auto files = archive->GetFileInfos();
        if (files.size() != 1) {
            return nullptr;
        }
        auto* fi = archive->GetFileDataById(0);
        if (!fi || !fi->data) {
            return nullptr;
        }
        ByteSlice d{(u8*)fi->data, fi->fileSizeUncompressed};
        IStream* strm = CreateStreamFromData(d);
        ScopedComPtr<IStream> stream(strm);
        if (!stream) {
            return nullptr;
        }
        EngineMupdf* engine = new EngineMupdf();
        if (displayDPI < 70) {
            displayDPI = 96;
        }
        engine->displayDPI = displayDPI;
        if (!engine->Load(stream, "foo.fb2", pwdUI)) {
            SafeEngineRelease(&engine);
            return nullptr;
        }
        engine->SetFilePath(path);
        return engine;
    }
    EngineMupdf* engine = new EngineMupdf();
    if (displayDPI < 70) {
        displayDPI = 96;
    }
    engine->displayDPI = displayDPI;
    if (!engine->Load(path, pwdUI)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    const char* ext = GetExtForKind(kind);
    if (ext) {
        str::ReplaceWithCopy(&engine->defaultExt, ext);
    }
    return engine;
}

EngineBase* CreateEngineMupdfFromStream(IStream* stream, const char* nameHint, PasswordUI* pwdUI) {
    EngineMupdf* engine = new EngineMupdf();
    if (!engine->Load(stream, nameHint, pwdUI)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

EngineBase* CreateEngineMupdfFromData(const ByteSlice& data, const char* nameHint, PasswordUI* pwdUI) {
    EngineMupdf* engine = new EngineMupdf();
    IStream* stream = CreateStreamFromData(data);
    if (!engine->Load(stream, nameHint, pwdUI)) {
        SafeEngineRelease(&engine);
        return nullptr;
    }
    return engine;
}

// it's fast because we only collect pointers from FzPageInfo
static bool PdfPageHasAnnotations(EngineMupdf* e, int pageNo) {
    fz_context* ctx = e->Ctx();
    bool has = false;
    ScopedCritSec scope(&e->docLock);
    fz_try(ctx) {
        pdf_obj* pageref = pdf_lookup_page_obj(ctx, e->pdfdoc, pageNo - 1);
        pdf_obj* annots = pdf_dict_get(ctx, pageref, PDF_NAME(Annots));
        has = annots && pdf_array_len(ctx, annots) > 0;
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return has;
}

void EngineMupdfGetAnnotations(EngineBase* engine, Vec<Annotation*>& annotsOut) {
    annotsOut.Clear();

    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e->pdfdoc) {
        return;
    }
    // IMPORTANT: do NOT hold pagesLock across this loop. GetFzPageInfo() acquires
    // renderLock (and, while holding it, resolves link destinations that re-take
    // pagesLock). If we held pagesLock here we'd have order pagesLock->renderLock
    // while a concurrent render thread holds renderLock and wants pagesLock,
    // which deadlocks (seen after save->reload->reopen of the annotation editor
    // on large PDFs). GetFzPageInfo() does its own per-page locking and
    // pi->annotations is stable once built, so no outer lock is needed.
    for (int i = 1; i <= e->pageCount; i++) {
        FzPageInfo* pi = e->GetFzPageInfoCanFail(i);
        if (pi && pi->page) {
            if (pi->annotations.Size() > 0) {
                annotsOut.Append(pi->annotations);
            }
            continue;
        }
        if (!PdfPageHasAnnotations(e, i)) {
            continue;
        }
        // Quick load only the page and its annotation list; skip link extraction
        // and full text/image collection (see GetFzPageInfo(..., true, nullptr, false)).
        pi = e->GetFzPageInfo(i, true, nullptr, false);
        if (!pi) {
            continue;
        }
        annotsOut.Append(pi->annotations);
    }
}

bool EngineMupdfHasUnsavedAnnotations(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf || !epdf->pdfdoc) {
        return false;
    }
#if 0
    // TODO: this fails in https://github.com/sumatrapdfreader/sumatrapdf/issues/3448
    // because pdf_has_unsaved_changes() returns true even though pdf_was_repaired()
    // returns false (even though the doc was modified in pdf_test_outline() becasue
    // "Bad or missing last pointer in outline tree, repairing"

    // pdf_has_unsaved_changes() also returns true if the file was auto-repaired
    // at loading time, which is not something we want, so only rely on it
    // when we know it wasn't repaired.
    if (!pdf_was_repaired(epdf->Ctx(), epdf->pdfdoc)) {
        return pdf_has_unsaved_changes(epdf->Ctx(), epdf->pdfdoc);
    }
#endif
    return epdf->modifiedAnnotations;
}

bool EngineMupdfHasUnsavedPdfChanges(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    return epdf && epdf->pdfdoc && (epdf->modifiedAnnotations || epdf->modifiedPdfToc);
}

bool EngineMupdfSupportsAnnotations(EngineBase* engine) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf) {
        return false;
    }
    return (epdf->pdfdoc != nullptr);
}

// caller must free
ByteSlice EngineMupdfLoadAttachment(EngineBase* engine, int attachmentNo) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf->pdfdoc) {
        return {};
    }

    ByteSlice res = PdfLoadAttachment(epdf->Ctx(), epdf->pdfdoc, attachmentNo);
    return res;
}

ByteSlice EngineMupdfLoadAnnotAttachment(EngineBase* engine, int objNum) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf->pdfdoc) {
        return {};
    }
    ScopedCritSec scope(&epdf->docLock);
    return PdfLoadAnnotationAttachment(epdf->Ctx(), epdf->pdfdoc, objNum);
}

// if an elements fully obscures another, remove it from the list
static Annotation* PickAnnotationAtPos(FzPageInfo* pi, PointF pos, Annotation* preferredAnnot) {
    if (!pi) {
        return nullptr;
    }
    Vec<Annotation*> els;
    for (auto& annot : pi->annotations) {
        RectF bounds = annot->bounds;
        if (!bounds.Contains(pos)) {
            continue;
        }
        els.Append(annot);
    }
    if (els.Size() == 0) {
        return nullptr;
    }
    for (const auto& a : els) {
        if (a == preferredAnnot) {
            return preferredAnnot;
        }
    }

    // pick the annotation with the smallest rect: if the click lands inside
    // a big highlight that also wraps a smaller annotation, the smaller one
    // is almost always what the user meant
    Annotation* best = els[0];
    RectF br = best->bounds;
    float bestArea = br.dx * br.dy;
    for (int i = 1; i < els.Size(); i++) {
        RectF r = els[i]->bounds;
        float area = r.dx * r.dy;
        if (area < bestArea) {
            best = els[i];
            bestArea = area;
        }
    }
    return best;
}

Annotation* EngineMupdfGetAnnotationAtPos(EngineBase* engine, int pageNo, PointF pos, Annotation* preferredAnnot) {
    EngineMupdf* epdf = AsEngineMupdf(engine);
    if (!epdf->pdfdoc) {
        return nullptr;
    }
    FzPageInfo* pi = epdf->GetFzPageInfoCanFail(pageNo);
    if (pi) {
        ScopedCritSec cs(&epdf->docLock);
        Annotation* hit = PickAnnotationAtPos(pi, pos, preferredAnnot);
        if (hit) {
            return hit;
        }
        if (pi->page) {
            return nullptr;
        }
    }
    if (!PdfPageHasAnnotations(epdf, pageNo)) {
        return nullptr;
    }
    // CanFail returns null while the render thread holds pagesLock or before the
    // page is loaded. Load only the page + annotation list (no links / stext).
    pi = epdf->GetFzPageInfo(pageNo, true, nullptr, false);
    if (!pi) {
        return nullptr;
    }

    ScopedCritSec cs(&epdf->docLock);
    return PickAnnotationAtPos(pi, pos, preferredAnnot);
}

// Note: this code is compiled in release mode even if debug build so
// DEBUG is not defined so we can't do #if defined(DEBUG) here
// so we use this runtime boolean instead
static bool gSkipAnnotatoinValidation = true;

// check that pageInfo->annotations has the same info as in mupdf
NO_INLINE void ValidateAnnotationsInSync(EngineMupdf* e, FzPageInfo* pageInfo) {
    if (gSkipAnnotatoinValidation) {
        return;
    }
    // TODO: write me
}

// in a function so that we can set a breakpoint or add logging
// to easily trace all places that modify annotations
NO_INLINE void MarkNotificationAsModified(EngineMupdf* e, Annotation* annot, AnnotationChange change) {
    e->modifiedAnnotations = true;
    if (!e->pdfdoc) {
        return;
    }
    int pageNo = annot->pageNo;
    ReportIf(pageNo < 1 || pageNo > e->pageCount);
    int pageIdx = pageNo - 1;

    // EngineMupdf is the ultimate source of truth for Annotation* list
    // all other places only get references to Annotation* created
    // inside EngineMupdf.
    // It would be easier to re-create Annotation* list after each change
    // to annotations inside mupdf but we don't want loose the identity
    // so on add /remove we update the list manually
    // on change we assume Annotation* lives inside EngineMupdf
    ScopedCritSec scope(&e->pagesLock);
    FzPageInfo* pageInfo = e->pages[pageIdx];

    if (change == AnnotationChange::Remove) {
        int sizeBefore = pageInfo->annotations.Size();
        int removedPos = pageInfo->annotations.Remove(annot);
        ReportIf(removedPos < 0); // must exist
        int sizeNow = pageInfo->annotations.Size();
        ReportIf(sizeBefore != sizeNow + 1);
        ValidateAnnotationsInSync(e, pageInfo);
    } else if (change == AnnotationChange::Add) {
        int sizeBefore = pageInfo->annotations.Size();
        int pos = pageInfo->annotations.Find(annot);
        ReportIf(pos >= 0); // shouldn't exist
        pageInfo->annotations.Append(annot);
        int sizeNow = pageInfo->annotations.Size();
        ReportIf(sizeBefore != sizeNow - 1);
        ValidateAnnotationsInSync(e, pageInfo);
    } else {
        ReportIf(change != AnnotationChange::Modify);
    }
    {
        auto ctx = e->Ctx();
        ScopedCritSec ctxScope(&e->docLock);
        RebuildCommentsFromAnnotations(ctx, pageInfo);
    }
    pageInfo->elementsNeedRebuilding = true;

    // cached display list captured the old annotations; drop it so the next
    // render rebuilds with the new state.
    {
        auto ctx = e->Ctx();
        ScopedCritSec rl(&e->renderLock);
        if (pageInfo->displayList) {
            fz_drop_display_list(ctx, pageInfo->displayList);
            pageInfo->displayList = nullptr;
        }
    }
}

// creates Annotation wrapper around pdf_annot
Annotation* MakeAnnotationWrapper(EngineMupdf* engine, pdf_annot* annot, int pageNo) {
    ReportIf(pageNo < 1);
    ReportIf(!engine->pdfdoc);
    ScopedCritSec cs(&engine->docLock);

    AnnotationType typ = AnnotationType::Unknown;
    fz_rect bounds;

    fz_context* ctx = engine->Ctx();
    fz_try(ctx) {
        auto tp = pdf_annot_type(ctx, annot);
        bounds = pdf_bound_annot(ctx, annot);
        typ = AnnotationTypeFromPdfAnnot(tp);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        // do nothing
    }

    if (typ == AnnotationType::Unknown) {
        // unsupported type or exception in fz_try
        return nullptr;
    }

    Annotation* res = new Annotation();
    res->engine = engine;
    res->pageNo = pageNo;
    res->pdfannot = annot;
    res->bounds = ToRectF(bounds);
    res->type = typ;
    return res;
}

extern "C" fz_buffer* pdfinfo_to_buffer(fz_context* ctx, const char* filename);

static void outline_to_buffer_rec(fz_context* ctx, fz_output* out, fz_outline* outline, int level) {
    while (outline) {
        for (int i = 0; i < level; i++) {
            fz_write_byte(ctx, out, '\t');
        }
        const char* uri = MupdfSafeCStr(outline->uri);
        fz_write_printf(ctx, out, "%s\t%s\n", outline->title ? outline->title : "", uri ? uri : "");
        if (outline->down) {
            outline_to_buffer_rec(ctx, out, outline->down, level + 1);
        }
        outline = outline->next;
    }
}

TempStr EngineMupdfGetPdfOutline(const char* path) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) {
        return nullptr;
    }
    fz_register_document_handlers(ctx);
    TempStr res = nullptr;
    fz_document* doc = nullptr;
    fz_outline* outline = nullptr;
    fz_buffer* buf = nullptr;
    fz_output* out = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, path);
        outline = fz_load_outline(ctx, doc);
        if (!outline) {
            res = str::DupTemp("(no outline)");
        } else {
            buf = fz_new_buffer(ctx, 1024);
            out = fz_new_output_with_buffer(ctx, buf);
            outline_to_buffer_rec(ctx, out, outline, 0);
            fz_close_output(ctx, out);
            unsigned char* data;
            size_t len = fz_buffer_storage(ctx, buf, &data);
            res = str::DupTemp((const char*)data, len);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    fz_drop_output(ctx, out);
    fz_drop_buffer(ctx, buf);
    fz_drop_outline(ctx, outline);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return res;
}

TempStr EngineMupdfGetPdfInfo(const char* path) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) {
        return nullptr;
    }
    fz_register_document_handlers(ctx);
    TempStr res = nullptr;
    fz_buffer* buf = nullptr;
    fz_try(ctx) {
        buf = pdfinfo_to_buffer(ctx, path);
        unsigned char* data;
        size_t len = fz_buffer_storage(ctx, buf, &data);
        res = str::DupTemp((const char*)data, len);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    fz_drop_buffer(ctx, buf);
    fz_drop_context(ctx);
    return res;
}
