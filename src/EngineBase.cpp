
/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"

#include "utils/Log.h"

static thread_local bool gCreateEngineForThumbnail = false;

void SetCreateEngineForThumbnail(bool value) {
    gCreateEngineForThumbnail = value;
}

bool IsCreateEngineForThumbnail() {
    return gCreateEngineForThumbnail;
}

Kind kindPageElementDest = "dest";
Kind kindPageElementImage = "image";
Kind kindPageElementComment = "comment";

Kind kindDestinationNone = "none";
Kind kindDestinationScrollTo = "scrollTo";
Kind kindDestinationLaunchURL = "launchURL";
Kind kindDestinationLaunchEmbedded = "launchEmbedded";
Kind kindDestinationAttachment = "launchAttachment";
Kind kindDestinationLaunchFile = "launchFile";
Kind kindDestinationDjVu = "destinationDjVu";
Kind kindDestinationMupdf = "destinationMupdf";

// clang-format off
static Kind destKinds[] = {
    kindDestinationNone,
    kindDestinationScrollTo,
    kindDestinationLaunchURL,
    kindDestinationLaunchEmbedded,
    kindDestinationAttachment,
    kindDestinationLaunchFile,
    kindDestinationDjVu,
    kindDestinationMupdf
};
// clang-format on

bool IsExternalUrl(const WCHAR* url) {
    return str::StartsWithI(url, L"http://") || str::StartsWithI(url, L"https://") || str::StartsWithI(url, L"mailto:");
}

bool IsExternalUrl(const char* url) {
    return str::StartsWithI(url, "http://") || str::StartsWithI(url, "https://") || str::StartsWithI(url, "mailto:");
}

void FreePageText(PageText* pageText) {
    str::Free(pageText->text);
    free((void*)pageText->coords);
    pageText->text = nullptr;
    pageText->coords = nullptr;
    pageText->len = 0;
}

void FreePageTextUtf8(PageTextUtf8* pageText) {
    str::Free(pageText->text);
    free((void*)pageText->coords);
    pageText->text = nullptr;
    pageText->coords = nullptr;
    pageText->len = 0;
}

PageDestination::~PageDestination() {
    free(value);
    free(name);
}

// string value associated with the destination (e.g. a path or a URL)
char* PageDestination::GetValue2() {
    return value;
}

// the name of this destination (reverses EngineBase::GetNamedDest) or nullptr
// (mainly applicable for links of type "LaunchFile" to PDF documents)
char* PageDestination::GetName2() {
    return name;
}

IPageDestination* NewSimpleDest(int pageNo, RectF rect, float zoom, const char* value) {
    if (value) {
        return new PageDestinationURL(value);
    }
    auto res = new PageDestination();
    res->pageNo = pageNo;
    res->rect = rect;
    res->kind = kindDestinationScrollTo;
    res->zoom = zoom;
    return res;
}

bool IPageElement::Is(Kind expectedKind) {
    return kind == expectedKind;
}

Kind kindTocFzOutline = "tocFzOutline";
Kind kindTocFzOutlineAttachment = "tocFzOutlineAttachment";
Kind kindTocFzLink = "tocFzLink";
Kind kindTocDjvu = "tocDjvu";

TocItem::TocItem(TocItem* parent, const char* title, int pageNo) {
    this->title = str::Dup(title);
    this->pageNo = pageNo;
    this->parent = parent;
}

TocItem::~TocItem() {
    delete child;
    if (!destNotOwned) {
        delete dest;
    }
    while (next) {
        TocItem* tmp = next->next;
        next->next = nullptr;
        delete next;
        next = tmp;
    }
    str::Free(title);
}

void TocItem::AddSibling(TocItem* sibling) {
    TocItem* currNext = next;
    next = sibling;
    sibling->next = currNext;
    sibling->parent = parent;
}

void TocItem::AddSiblingAtEnd(TocItem* sibling) {
    TocItem* item = this;
    while (item->next) {
        item = item->next;
    }
    item->next = sibling;
    sibling->parent = item->parent;
}

void TocItem::AddChild(TocItem* newChild) {
    TocItem* curr = child;
    child = newChild;
    newChild->parent = this;
    newChild->next = curr;
}

// regular delete is recursive, this deletes only this item
void TocItem::DeleteJustSelf() {
    child = nullptr;
    next = nullptr;
    parent = nullptr;
    delete this;
}

// returns the destination this ToC item points to or nullptr
// (the result is owned by the TocItem and MUST NOT be deleted)
// TODO: rename to GetDestination()
IPageDestination* TocItem::GetPageDestination() const {
    return dest;
}

int TocItem::ChildCount() {
    int n = 0;
    auto node = child;
    while (node) {
        n++;
        node = node->next;
    }
    return n;
}

TocItem* TocItem::ChildAt(int n) {
    if (n == 0) {
        currChild = child;
        currChildNo = 0;
        return child;
    }
    // speed up sequential iteration over children
    if (currChild != nullptr && n == currChildNo + 1) {
        currChild = currChild->next;
        ++currChildNo;
        return currChild;
    }
    auto node = child;
    while (n > 0) {
        n--;
        node = node->next;
    }
    return node;
}

bool TocItem::IsExpanded() {
    // leaf items cannot be expanded
    if (child == nullptr) {
        return false;
    }
    // item is expanded when:
    // - expanded by default, not toggled (true, false)
    // - not expanded by default, toggled (false, true)
    // which boils down to:
    return isOpenDefault != isOpenToggled;
}

bool TocItem::PageNumbersMatch() const {
    int destPageNo = PageDestGetPageNo(dest);
    if (destPageNo <= 0) {
        return true; // TODO: should be false?
    }
    if (pageNo != destPageNo) {
        logf("pageNo: %d, dest->pageNo: %d\n", pageNo, destPageNo);
        return false;
    }
    return true;
}

TocTree::TocTree(TocItem* root) {
    this->root = root;
}

TocTree::~TocTree() {
    delete root;
}

TreeItem TocTree::Root() {
    return (TreeItem)root;
}

char* TocTree::Text(TreeItem ti) {
    auto tocItem = (TocItem*)ti;
    return tocItem->title;
}

TreeItem TocTree::Parent(TreeItem ti) {
    auto tocItem = (TocItem*)ti;
    return (TreeItem)tocItem->parent;
}

int TocTree::ChildCount(TreeItem ti) {
    auto tocItem = (TocItem*)ti;
    return tocItem->ChildCount();
}

TreeItem TocTree::ChildAt(TreeItem ti, int idx) {
    auto tocItem = (TocItem*)ti;
    return (TreeItem)tocItem->ChildAt(idx);
}

bool TocTree::IsExpanded(TreeItem ti) {
    auto tocItem = (TocItem*)ti;
    return tocItem->IsExpanded();
}

bool TocTree::IsChecked(TreeItem ti) {
    auto tocItem = (TocItem*)ti;
    return !tocItem->isUnchecked;
}

void TocTree::SetHandle(TreeItem ti, HTREEITEM hItem) {
    ReportIf(ti < 0);
    TocItem* tocItem = (TocItem*)ti;
    tocItem->hItem = hItem;
}

HTREEITEM TocTree::GetHandle(TreeItem ti) {
    ReportIf(ti < 0);
    TocItem* tocItem = (TocItem*)ti;
    return tocItem->hItem;
}

// TODO: speed up by removing recursion
static bool VisitTocTree(TocItem* ti, const VisitTocTreeCb& f) {
    bool cont;
    VisitTocTreeData d;
    while (ti) {
        d.ti = ti;
        f.Call(&d);
        cont = !d.stopTraversal;
        if (cont && ti->child) {
            cont = VisitTocTree(ti->child, f);
        }
        if (!cont) {
            return false;
        }
        ti = ti->next;
    }
    return true;
}

static bool VisitTocTreeWithParentRecursive(TocItem* ti, TocItem* parent, const VisitTocTreeCb& f) {
    bool cont;
    VisitTocTreeData d;
    while (ti) {
        d.ti = ti;
        d.parent = parent;
        f.Call(&d);
        cont = !d.stopTraversal;
        if (cont && ti->child) {
            cont = VisitTocTreeWithParentRecursive(ti->child, ti, f);
        }
        if (!cont) {
            return false;
        }
        ti = ti->next;
    }
    return true;
}

RenderPageArgs::RenderPageArgs(int pageNo, float zoom, int rotation, RectF* pageRect, RenderTarget target,
                               AbortCookie** cookie_out) {
    this->pageNo = pageNo;
    this->zoom = zoom;
    this->rotation = rotation;
    this->pageRect = pageRect;
    this->target = target;
    this->cookie_out = cookie_out;
}

int EngineBase::AddRef() {
    return AtomicRefCountAdd(&refCount);
}

bool EngineBase::Release() {
    int rc = AtomicRefCountDec(&refCount);
    if (rc == 0) {
        delete this;
        return true;
    }
    return false;
}

EngineBase::EngineBase() {
    InitializeCriticalSection(&textCacheLock);
    arena = ArenaNew();
}

void EngineBase::EnsurePagesTextSize() {
    // Snapshot pageCount into a local: during progressive ebook loading the
    // background formatter mutates pageCount without holding textCacheLock.
    // Reading the member multiple times could see a larger value between
    // realloc and memset, causing memset to write past the end of the buffer
    // and corrupt the heap (manifesting as a crash in free() during teardown).
    int n = pageCount;
    if (n <= 0) {
        return;
    }
    if (!pagesText) {
        pagesText = AllocArray<PageText>(n);
        pagesTextSize = n;
        return;
    }
    if (pagesTextSize >= n) {
        return;
    }
    int oldSize = pagesTextSize;
    pagesText = (PageText*)realloc(pagesText, (size_t)n * sizeof(PageText));
    if (!pagesText) {
        pagesTextSize = 0;
        return;
    }
    memset(&pagesText[oldSize], 0, (size_t)(n - oldSize) * sizeof(PageText));
    pagesTextSize = n;
}

void EngineBase::EnsurePagesTextUtf8Size() {
    int n = pageCount;
    if (n <= 0) {
        return;
    }
    if (!pagesTextUtf8) {
        pagesTextUtf8 = AllocArray<PageTextUtf8>(n);
        pagesTextUtf8Size = n;
        return;
    }
    if (pagesTextUtf8Size >= n) {
        return;
    }
    int oldSize = pagesTextUtf8Size;
    pagesTextUtf8 = (PageTextUtf8*)realloc(pagesTextUtf8, (size_t)n * sizeof(PageTextUtf8));
    if (!pagesTextUtf8) {
        pagesTextUtf8Size = 0;
        return;
    }
    memset(&pagesTextUtf8[oldSize], 0, (size_t)(n - oldSize) * sizeof(PageTextUtf8));
    pagesTextUtf8Size = n;
}

EngineBase::~EngineBase() {
    if (pagesText) {
        for (int i = 0; i < pagesTextSize; i++) {
            PageText* pt = &pagesText[i];
            free(pt->coords);
            free(pt->text);
        }
        free(pagesText);
    }
    if (pagesTextUtf8) {
        for (int i = 0; i < pagesTextUtf8Size; i++) {
            PageTextUtf8* pt = &pagesTextUtf8[i];
            free(pt->coords);
            free(pt->text);
        }
        free(pagesTextUtf8);
    }
    DeleteCriticalSection(&textCacheLock);
    str::Free(defaultExt);
    ArenaDelete(arena);
}

bool EngineBase::HasTextForPage(int pageNo) {
    if (pageNo < 1 || pageNo > pageCount) {
        return false;
    }
    ScopedCritSec scope(&textCacheLock);
    if (!pagesText || pageNo > pagesTextSize) {
        return false;
    }
    PageText* pt = &pagesText[pageNo - 1];
    return pt->text != nullptr;
}

void EngineBase::ClearTextCache() {
    ScopedCritSec scope(&textCacheLock);
    if (!pagesText && !pagesTextUtf8) {
        textCacheGeneration++;
        return;
    }
    if (pagesText) {
        for (int i = 0; i < pagesTextSize; i++) {
            PageText* pt = &pagesText[i];
            free(pt->coords);
            free(pt->text);
            pt->text = nullptr;
            pt->coords = nullptr;
            pt->len = 0;
        }
    }
    if (pagesTextUtf8) {
        for (int i = 0; i < pagesTextUtf8Size; i++) {
            PageTextUtf8* pt = &pagesTextUtf8[i];
            free(pt->coords);
            free(pt->text);
            pt->text = nullptr;
            pt->coords = nullptr;
            pt->len = 0;
            pt->asciiLetterMask = 0;
        }
    }
    textCacheGeneration++;
}

bool EngineBase::TryExtractPageText(int pageNo, PageText* out) {
    *out = ExtractPageText(pageNo);
    return true;
}

bool EngineBase::TryExtractPageTextUtf8(int pageNo, PageTextUtf8* out) {
    *out = ExtractPageTextUtf8(pageNo);
    return true;
}

static bool ReturnCachedPageText(PageText* pt, int* lenOut, Rect** coordsOut) {
    if (lenOut) {
        *lenOut = pt->len;
    }
    if (coordsOut) {
        *coordsOut = pt->coords;
    }
    return pt->text != nullptr;
}

bool EngineBase::TryGetTextForPage(int pageNo, int* lenOut, Rect** coordsOut) {
    auto emptyOk = [&]() {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return true;
    };

    if (pageNo < 1 || pageNo > pageCount) {
        return emptyOk();
    }

    {
        ScopedCritSec scope(&textCacheLock);
        if (pagesText && pageNo <= pagesTextSize) {
            PageText* pt = &pagesText[pageNo - 1];
            if (pt->text) {
                ReturnCachedPageText(pt, lenOut, coordsOut);
                return true;
            }
        }
    }

    if (IsProgressiveEbookLoading() && (pageNo < 1 || pageNo > pageCount)) {
        return emptyOk();
    }

    PageText extracted;
    if (!TryExtractPageText(pageNo, &extracted)) {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return false;
    }

    ScopedCritSec scope(&textCacheLock);
    EnsurePagesTextSize();
    if (!pagesText || pageNo > pagesTextSize) {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return true;
    }
    PageText* pt = &pagesText[pageNo - 1];
    if (!pt->text) {
        *pt = extracted;
        if (!pt->text) {
            pt->text = str::Dup(L"");
            pt->len = 0;
        }
        extracted = {};
    } else {
        free(extracted.text);
        free(extracted.coords);
    }
    ReturnCachedPageText(pt, lenOut, coordsOut);
    return true;
}

const WCHAR* EngineBase::GetTextForPage(int pageNo, int* lenOut, Rect** coordsOut) {
    auto emptyResult = [&]() {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return L"";
    };

    if (pageNo < 1 || pageNo > pageCount) {
        return emptyResult();
    }

    if (TryGetTextForPage(pageNo, lenOut, coordsOut)) {
        ScopedCritSec scope(&textCacheLock);
        if (pagesText && pageNo <= pagesTextSize) {
            PageText* pt = &pagesText[pageNo - 1];
            if (coordsOut) {
                *coordsOut = pt->coords;
            }
            return pt->text ? pt->text : L"";
        }
        return L"";
    }

    // Try-path failed (lock miss or page not loaded yet). Fall back to blocking
    // extraction so callers like selection still work; search uses TryGet first.
    PageText extracted = ExtractPageText(pageNo);

    ScopedCritSec scope(&textCacheLock);
    EnsurePagesTextSize();
    if (!pagesText || pageNo > pagesTextSize) {
        return emptyResult();
    }
    PageText* pt = &pagesText[pageNo - 1];
    if (!pt->text) {
        *pt = extracted;
        if (!pt->text) {
            pt->text = str::Dup(L"");
            pt->len = 0;
        }
        extracted = {};
    } else {
        free(extracted.text);
        free(extracted.coords);
    }

    if (lenOut) {
        *lenOut = pt->len;
    }
    if (coordsOut) {
        *coordsOut = pt->coords;
    }
    return pt->text ? pt->text : L"";
}

static bool ReturnCachedPageTextUtf8(PageTextUtf8* pt, int* lenOut, Rect** coordsOut) {
    if (lenOut) {
        *lenOut = pt->len;
    }
    if (coordsOut) {
        *coordsOut = pt->coords;
    }
    return pt->text != nullptr;
}

static u32 ComputeAsciiLetterMask(const char* text, int byteLen) {
    u32 mask = 0;
    if (!text || byteLen <= 0) {
        return 0;
    }
    for (int i = 0; i < byteLen; i++) {
        unsigned char b = (unsigned char)text[i];
        if (b >= 'A' && b <= 'Z') {
            mask |= (1u << (b - 'A'));
        } else if (b >= 'a' && b <= 'z') {
            mask |= (1u << (b - 'a'));
        }
    }
    return mask;
}

static void FinalizeCachedPageTextUtf8(PageTextUtf8* pt) {
    if (!pt->text) {
        pt->text = str::Dup("");
        pt->len = 0;
        pt->asciiLetterMask = 0;
    } else {
        int byteLen = pt->len;
        if (byteLen <= 0) {
            byteLen = (int)str::Len(pt->text);
            pt->len = byteLen;
        }
        pt->asciiLetterMask = ComputeAsciiLetterMask(pt->text, byteLen);
    }
}

u32 EngineBase::GetPageAsciiLetterMask(int pageNo) {
    if (pageNo < 1 || pageNo > pageCount) {
        return 0;
    }
    ScopedCritSec scope(&textCacheLock);
    if (!pagesTextUtf8 || pageNo > pagesTextUtf8Size) {
        return UINT32_MAX;
    }
    PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
    if (!pt->text) {
        return UINT32_MAX;
    }
    return pt->asciiLetterMask;
}

void EngineBase::ApplyAsciiMaskPageSkip(u32 anchorMask, int nPages, Vec<bool>& pagesToSkip) {
    if (anchorMask == 0 || nPages <= 0) {
        return;
    }
    ScopedCritSec scope(&textCacheLock);
    if (!pagesTextUtf8) {
        return;
    }
    int limit = pagesTextUtf8Size;
    if (limit > nPages) {
        limit = nPages;
    }
    for (int pageNo = 1; pageNo <= limit; pageNo++) {
        if (pagesToSkip[pageNo - 1]) {
            continue;
        }
        PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
        if (!pt->text) {
            continue;
        }
        if ((pt->asciiLetterMask & anchorMask) != anchorMask) {
            pagesToSkip[pageNo - 1] = true;
        }
    }
}

bool EngineBase::CachedPageContainsUtf8Bytes(int pageNo, const char* bytes, int byteLen) {
    if (!bytes || byteLen <= 0) {
        return true;
    }
    if (pageNo < 1 || pageNo > pageCount) {
        return false;
    }
    ScopedCritSec scope(&textCacheLock);
    if (!pagesTextUtf8 || pageNo > pagesTextUtf8Size) {
        return true;
    }
    PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
    if (!pt->text) {
        return true;
    }
    int textByteLen = pt->len;
    if (textByteLen <= 0) {
        textByteLen = (int)str::Len(pt->text);
    }
    if (textByteLen < byteLen) {
        return false;
    }
    for (int i = 0; i <= textByteLen - byteLen; i++) {
        if (memcmp(pt->text + i, bytes, (size_t)byteLen) == 0) {
            return true;
        }
    }
    return false;
}

void EngineBase::ApplyUtf8AnchorPageSkip(const char* anchor, int anchorByteLen, int nPages, Vec<bool>& pagesToSkip) {
    if (!anchor || anchorByteLen <= 0 || nPages <= 0) {
        return;
    }
    ScopedCritSec scope(&textCacheLock);
    if (!pagesTextUtf8) {
        return;
    }
    int limit = pagesTextUtf8Size;
    if (limit > nPages) {
        limit = nPages;
    }
    for (int pageNo = 1; pageNo <= limit; pageNo++) {
        if (pagesToSkip[pageNo - 1]) {
            continue;
        }
        PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
        if (!pt->text) {
            continue;
        }
        int textByteLen = pt->len;
        if (textByteLen <= 0) {
            textByteLen = (int)str::Len(pt->text);
        }
        if (textByteLen < anchorByteLen) {
            pagesToSkip[pageNo - 1] = true;
            continue;
        }
        bool found = false;
        for (int i = 0; i <= textByteLen - anchorByteLen; i++) {
            if (memcmp(pt->text + i, anchor, (size_t)anchorByteLen) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            pagesToSkip[pageNo - 1] = true;
        }
    }
}

bool EngineBase::TryGetTextForPageUtf8(int pageNo, int* lenOut, Rect** coordsOut, const char** textOut) {
    auto emptyOk = [&]() {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return true;
    };

    if (pageNo < 1 || pageNo > pageCount) {
        return emptyOk();
    }

    {
        ScopedCritSec scope(&textCacheLock);
        if (pagesTextUtf8 && pageNo <= pagesTextUtf8Size) {
            PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
            if (pt->text) {
                ReturnCachedPageTextUtf8(pt, lenOut, coordsOut);
                if (textOut) {
                    *textOut = pt->text;
                }
                return true;
            }
        }
    }

    if (IsProgressiveEbookLoading() && (pageNo < 1 || pageNo > pageCount)) {
        return emptyOk();
    }

    PageTextUtf8 extracted;
    if (!TryExtractPageTextUtf8(pageNo, &extracted)) {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return false;
    }

    ScopedCritSec scope(&textCacheLock);
    EnsurePagesTextUtf8Size();
    if (!pagesTextUtf8 || pageNo > pagesTextUtf8Size) {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return true;
    }
    PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
    if (!pt->text) {
        *pt = extracted;
        FinalizeCachedPageTextUtf8(pt);
        extracted = {};
    } else {
        free(extracted.text);
        free(extracted.coords);
    }
    ReturnCachedPageTextUtf8(pt, lenOut, coordsOut);
    if (textOut) {
        *textOut = pt->text ? pt->text : "";
    }
    return true;
}

const char* EngineBase::GetTextForPageUtf8(int pageNo, int* lenOut, Rect** coordsOut) {
    auto emptyResult = [&]() {
        if (lenOut) {
            *lenOut = 0;
        }
        if (coordsOut) {
            *coordsOut = nullptr;
        }
        return "";
    };

    if (pageNo < 1 || pageNo > pageCount) {
        return emptyResult();
    }

    if (TryGetTextForPageUtf8(pageNo, lenOut, coordsOut, nullptr)) {
        ScopedCritSec scope(&textCacheLock);
        if (pagesTextUtf8 && pageNo <= pagesTextUtf8Size) {
            PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
            if (coordsOut) {
                *coordsOut = pt->coords;
            }
            return pt->text ? pt->text : "";
        }
        return "";
    }

    PageTextUtf8 extracted = ExtractPageTextUtf8(pageNo);

    ScopedCritSec scope(&textCacheLock);
    EnsurePagesTextUtf8Size();
    if (!pagesTextUtf8 || pageNo > pagesTextUtf8Size) {
        return emptyResult();
    }
    PageTextUtf8* pt = &pagesTextUtf8[pageNo - 1];
    if (!pt->text) {
        *pt = extracted;
        FinalizeCachedPageTextUtf8(pt);
        extracted = {};
    } else {
        free(extracted.text);
        free(extracted.coords);
    }

    if (lenOut) {
        *lenOut = pt->len;
    }
    if (coordsOut) {
        *coordsOut = pt->coords;
    }
    return pt->text ? pt->text : "";
}

int EngineBase::PageCount() const {
    ReportIf(pageCount < 0);
    return pageCount;
}

RectF EngineBase::PageContentBox(int pageNo, RenderTarget) {
    return PageMediabox(pageNo);
}

bool EngineBase::IsImageCollection() const {
    return isImageCollection;
}

bool EngineBase::AllowsPrinting() const {
    return allowsPrinting;
}

bool EngineBase::AllowsCopyingText() const {
    return allowsCopyingText;
}

float EngineBase::GetFileDPI() const {
    return fileDPI;
}

IPageDestination* EngineBase::GetNamedDest(const char*) {
    return nullptr;
}

bool EngineBase::HasToc() {
    TocTree* tree = GetToc();
    return tree != nullptr;
}

TocTree* EngineBase::GetToc() {
    return nullptr;
}

#include "DocProperties.h"

// default implementation that just sets wanted keys
void EngineBase::GetProperties(StrVec& keyValueOut) {
    for (int i = 0;; i++) {
        const char* key = gAllProps[i];
        if (!key) {
            break;
        }
        TempStr val = GetPropertyTemp(key);
        if (val) {
            keyValueOut.Append(key);
            keyValueOut.Append(val);
        }
    }
}

bool EngineBase::HasPageLabels() const {
    return hasPageLabels;
}

TempStr EngineBase::GetPageLabeTemp(int pageNo) const {
    return str::FormatTemp("%d", pageNo);
}

int EngineBase::GetPageByLabel(const char* label) const {
    return atoi(label);
}

bool EngineBase::IsPasswordProtected() const {
    return isPasswordProtected;
}

const char* EngineBase::FilePath() const {
    return fileNameBase.s;
}

RenderedBitmap* EngineBase::GetImageForPageElement(IPageElement*) {
    CrashMe();
    return nullptr;
}

void EngineBase::GetBitmapRecolorSkipRects(int, float, int, const RectF&, Size, Vec<Rect>& skipRects) {
    skipRects.Clear();
}

void EngineBase::SetFilePath(const char* s) {
    fileNameBase = s ? StrDup(arena, Str((char*)s)) : Str();
}

PointF EngineBase::Transform(PointF pt, int pageNo, float zoom, int rotation, bool inverse) {
    RectF rc = RectF(pt, SizeF());
    RectF rect = Transform(rc, pageNo, zoom, rotation, inverse);
    return rect.TL();
}

bool EngineBase::HandleLink(IPageDestination*, ILinkHandler*) {
    // if not implemented in derived classes
    return false;
}
