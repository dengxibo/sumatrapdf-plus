/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/WinUtil.h"

#include "Mui.h"
#include "EbookFontConfig.h"

#include "utils/Log.h"

/*
MUI is a simple UI library for win32.
MUI stands for nothing, it's just ui and gui are overused.

MUI is intended to allow building UIs that have modern
capabilities not supported by the standard win32 HWND
architecture:
- overlapping, alpha-blended windows
- animations
- a saner layout

It's inspired by WPF, WDL (http://www.cockos.com/wdl/),
DirectUI (https://github.com/kjk/directui).

MUI is minimal - it only supports stuff needed for Sumatra.
I got burned trying to build the whole toolkit at once with DirectUI.
Less code there is, the easier it is to change or extend.

The basic architectures is that of a tree of "virtual" (not backed
by HWND) windows. Each window can have children (making it a container).
Children windows are positioned relative to its parent window and can
be positioned outside of parent's bounds.

There must be a parent window backed by HWND which handles windows
messages and paints child windows on WM_PAINT.

Event handling is loosly coupled.
*/

using Gdiplus::Bitmap;
using Gdiplus::CompositingQualityHighQuality;
using Gdiplus::Font;
using Gdiplus::Image;
using Gdiplus::Ok;
using Gdiplus::SmoothingModeAntiAlias;
using Gdiplus::Status;
using Gdiplus::TextRenderingHintClearTypeGridFit;
using Gdiplus::UnitPixel;

namespace mui {

// a critical section for everything that needs protecting in mui
// we use only one for simplicity as long as contention is not a problem
static CRITICAL_SECTION gMuiCs;

static void EnterMuiCriticalSection() {
    EnterCriticalSection(&gMuiCs);
}

static void LeaveMuiCriticalSection() {
    LeaveCriticalSection(&gMuiCs);
}

class ScopedMuiCritSec {
  public:
    ScopedMuiCritSec() { EnterMuiCriticalSection(); }

    ~ScopedMuiCritSec() { LeaveMuiCriticalSection(); }
};

class FontListItem {
  public:
    DWORD threadId;

    FontListItem(const WCHAR* name, float sizePt, FontStyle style, Font* font, HFONT hFont) : next(nullptr) {
        threadId = GetCurrentThreadId();
        cf.name = str::Dup(name);
        cf.sizePt = sizePt;
        cf.style = style;
        cf.font = font;
        cf.hFont = hFont;
    }
    ~FontListItem() {
        str::Free(cf.name);
        delete cf.font;
        DeleteObject(cf.hFont);
        delete next;
    }

    CachedFont cf;
    FontListItem* next;
};

// Global font cache keyed by (thread, name, size, style). GDI+ Graphics objects are
// per-thread; bundled/private fonts must be measured on the same thread that created them.
static FontListItem* gFontsCache = nullptr;

// Graphics objects cannot be used across threads. We have a per-thread
// cache so that it's easy to grab Graphics object to be used for
// measuring text
struct GraphicsCacheEntry {
    enum {
        bmpDx = 32,
        bmpDy = 4,
        stride = bmpDx * 4,
    };

    DWORD threadId;
    int refCount;

    Graphics* gfx;
    Bitmap* bmp;
    BYTE data[bmpDx * bmpDy * 4];

    bool Create();
    void Free() const;
};

static Vec<GraphicsCacheEntry>* gGraphicsCache = nullptr;

// set consistent mode for our graphics objects so that we get
// the same results when measuring text
void InitGraphicsMode(Graphics* g) {
    g->SetCompositingQuality(CompositingQualityHighQuality);
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    // g.SetSmoothingMode(SmoothingModeHighQuality);
    g->SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g->SetPageUnit(UnitPixel);
}

bool GraphicsCacheEntry::Create() {
    memset(data, 0, sizeof(data));
    refCount = 1;
    threadId = GetCurrentThreadId();
    // using a small bitmap under assumption that Graphics used only
    // for measuring text doesn't need the actual bitmap
    bmp = new Bitmap(bmpDx, bmpDy, stride, PixelFormat32bppARGB, data);
    if (!bmp) {
        return false;
    }
    gfx = new Graphics((Image*)bmp);
    if (!gfx) {
        return false;
    }
    InitGraphicsMode(gfx);
    return true;
}

void GraphicsCacheEntry::Free() const {
    ReportIf(0 != refCount);
    delete gfx;
    delete bmp;
}

void Initialize() {
    InitializeCriticalSection(&gMuiCs);
    InstallBundledReaderFonts();
    gGraphicsCache = new Vec<GraphicsCacheEntry>();
    // allocate the first entry in gGraphicsCache for UI thread, ref count
    // ensures it stays alive forever
    AllocGraphicsForMeasureText();
}

void ClearCachedFonts() {
    ScopedMuiCritSec muiCs;
    delete gFontsCache;
    gFontsCache = nullptr;
    ClearMeasureTextQuickFontCache();
}

void Destroy() {
    FreeGraphicsForMeasureText(gGraphicsCache->at(0).gfx);
    for (GraphicsCacheEntry& e : *gGraphicsCache) {
        e.Free();
    }
    delete gGraphicsCache;
    gGraphicsCache = nullptr;
    ClearCachedFonts();
    DeleteCriticalSection(&gMuiCs);
}

bool CachedFont::SameAs(const WCHAR* otherName, float otherSizePt, FontStyle otherStyle) const {
    if (sizePt != otherSizePt) {
        return false;
    }
    if (style != otherStyle) {
        return false;
    }
    return str::Eq(name, otherName);
}

HFONT CachedFont::GetHFont() {
    LOGFONTW lf;
    EnterMuiCriticalSection();
    if (!hFont) {
        // TODO: Graphics is probably only used for metrics,
        // so this might not be 100% correct (e.g. 2 monitors with different DPIs?)
        // but previous code wasn't much better
        Graphics* gfx = AllocGraphicsForMeasureText();
        Status status = font->GetLogFontW(gfx, &lf);
        FreeGraphicsForMeasureText(gfx);
        ReportIf(status != Ok);
        hFont = CreateFontIndirectW(&lf);
        ReportIf(!hFont);
    }
    LeaveMuiCriticalSection();
    return hFont;
}

static const WCHAR* gCjkFallbackFonts[] = {
    L"Source Han Serif SC", L"思源宋体", L"NSimSun", L"SimSun", L"宋体", nullptr,
};

static bool IsResolvedFontAcceptable(const WCHAR* requested, Gdiplus::Font* font) {
    if (!requested || !font) {
        return false;
    }
    LOGFONTW lf{};
    Gdiplus::Bitmap bmp(1, 1);
    Gdiplus::Graphics g(&bmp);
    if (font->GetLogFontW(&g, &lf) != Gdiplus::Status::Ok) {
        return false;
    }
    if (str::EqI(lf.lfFaceName, L"Microsoft Sans Serif")) {
        return false;
    }
    if (IsEbookCjkFontRequestW(requested)) {
        if (IsEbookCjkFontRequestW(lf.lfFaceName) || str::EqI(requested, lf.lfFaceName)) {
            return true;
        }
        if (!IsBundledCjkFontFamily(GetEbookCjkFontFamily())) {
            // Installed fonts may resolve to a different GDI face name than the menu label.
            return true;
        }
        return str::EqI(lf.lfFaceName, L"SimSun") || str::EqI(lf.lfFaceName, L"NSimSun") ||
               str::EqI(lf.lfFaceName, L"宋体");
    }
    if (str::EqI(requested, GetEbookLatinFontFamilyW())) {
        return str::Find(lf.lfFaceName, GetEbookLatinFontFamilyW()) != nullptr;
    }
    return true;
}

static bool IsLatinReaderFontRequest(const WCHAR* name) {
    return name && (str::EqI(name, GetEbookLatinFontFamilyW()) || str::StartsWithI(name, L"Literata") ||
                    str::StartsWithI(name, L"Source Serif"));
}

static Gdiplus::Font* CreateFontWithFallback(const WCHAR* name, float sizePt, Gdiplus::FontStyle style) {
    if (IsLatinReaderFontRequest(name)) {
        Gdiplus::Font* latinFont = TryCreateReaderLatinFromSystemFiles(sizePt, style);
        if (latinFont) {
            return latinFont;
        }
    }

    Gdiplus::Font* font = TryCreateBundledFont(name, sizePt, style);
    if (font) {
        return font;
    }

    font = new Gdiplus::Font(name, sizePt, style);
    if (font->GetLastStatus() == Gdiplus::Status::Ok && IsResolvedFontAcceptable(name, font)) {
        return font;
    }
    delete font;

    if (IsLatinReaderFontRequest(name)) {
        Gdiplus::FontStyle useStyle = style;
        if (EbookReaderStyleMobi() && (style & Gdiplus::FontStyleBold)) {
            useStyle = (Gdiplus::FontStyle)(useStyle & ~Gdiplus::FontStyleBold);
        }
        font = new Gdiplus::Font(L"Georgia", sizePt, useStyle);
        if (font->GetLastStatus() == Gdiplus::Status::Ok) {
            return font;
        }
        delete font;
        font = new Gdiplus::Font(L"Times New Roman", sizePt, useStyle);
        if (font->GetLastStatus() == Gdiplus::Status::Ok) {
            return font;
        }
        delete font;
        return nullptr;
    }

    const WCHAR* cjkFont = GetEbookCjkFontFamilyW();
    font = TryCreateBundledFont(cjkFont, sizePt, style);
    if (font) {
        return font;
    }
    font = new Gdiplus::Font(cjkFont, sizePt, style);
    if (font->GetLastStatus() == Gdiplus::Status::Ok && IsResolvedFontAcceptable(cjkFont, font)) {
        return font;
    }
    delete font;

    for (int i = 0; gCjkFallbackFonts[i]; i++) {
        font = TryCreateBundledFont(gCjkFallbackFonts[i], sizePt, style);
        if (font) {
            return font;
        }
        font = new Gdiplus::Font(gCjkFallbackFonts[i], sizePt, style);
        if (font->GetLastStatus() == Gdiplus::Status::Ok && IsResolvedFontAcceptable(gCjkFallbackFonts[i], font)) {
            return font;
        }
        delete font;
    }

    font = new Gdiplus::Font(L"Times New Roman", sizePt, style);
    if (font->GetLastStatus() == Gdiplus::Status::Ok) {
        return font;
    }
    delete font;
    return nullptr;
}

// convenience function: given cached style, get a Font object matching the font
// properties.
// Caller should not delete the font - it's cached for performance and deleted at exit
CachedFont* GetCachedFont(const WCHAR* name, float sizePt, FontStyle style) {
    ScopedMuiCritSec muiCs;

    DWORD threadId = GetCurrentThreadId();
    for (FontListItem* item = gFontsCache; item; item = item->next) {
        if (item->threadId == threadId && item->cf.SameAs(name, sizePt, style) && item->cf.font != nullptr) {
            return &item->cf;
        }
    }

    Font* font = CreateFontWithFallback(name, sizePt, style);
    if (!font) {
        if (gFontsCache) {
            return &gFontsCache->cf;
        }
        return nullptr;
    }

    FontListItem* item = new FontListItem(name, sizePt, style, font, nullptr);
    ListInsertFront(&gFontsCache, item);
    return &item->cf;
}

Graphics* AllocGraphicsForMeasureText() {
    ScopedMuiCritSec muiCs;

    DWORD threadId = GetCurrentThreadId();
    for (GraphicsCacheEntry& e : *gGraphicsCache) {
        if (e.threadId == threadId) {
            e.refCount++;
            return e.gfx;
        }
    }
    GraphicsCacheEntry ce;
    ce.Create();
    gGraphicsCache->Append(ce);
    if (gGraphicsCache->size() < 64) {
        return ce.gfx;
    }

    // try to limit the size of cache by evicting the oldest entries, but don't remove
    // first (for ui thread) or last (one we just added) entries
    for (size_t i = 1; i < gGraphicsCache->size() - 1; i++) {
        GraphicsCacheEntry e = gGraphicsCache->at(i);
        if (0 == e.refCount) {
            e.Free();
            gGraphicsCache->RemoveAt(i);
            return ce.gfx;
        }
    }
    // evict entries owned by threads that have exited (e.g. finished thumbnail/load threads)
    for (size_t i = 1; i < gGraphicsCache->size() - 1; i++) {
        GraphicsCacheEntry& e = gGraphicsCache->at(i);
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.threadId);
        if (!th) {
            delete e.gfx;
            delete e.bmp;
            gGraphicsCache->RemoveAt(i);
            return ce.gfx;
        }
        CloseHandle(th);
    }
    // entries are tiny; allow growth instead of tripping ReportIf when many load threads overlap
    return ce.gfx;
}

void FreeGraphicsForMeasureText(Graphics* gfx) {
    ScopedMuiCritSec muiCs;

    DWORD threadId = GetCurrentThreadId();
    for (GraphicsCacheEntry& e : *gGraphicsCache) {
        if (e.gfx == gfx) {
            ReportIf(e.threadId != threadId);
            e.refCount--;
            ReportIf(e.refCount < 0);
            return;
        }
    }
    ReportIf(true);
}

void FreeGraphicsForMeasureTextAnyThread(Graphics* gfx) {
    ScopedMuiCritSec muiCs;

    for (GraphicsCacheEntry& e : *gGraphicsCache) {
        if (e.gfx == gfx) {
            e.refCount--;
            return;
        }
    }
    // not in cache, just delete
    delete gfx;
}

} // namespace mui
