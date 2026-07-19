/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocProperties.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GlobalPrefs.h"
#include "Flags.h"
#include "Theme.h"
#include "PdfDarkMode.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"

#include "utils/Log.h"

static void PrintBitmapLuminanceStats(RenderedBitmap* bmp, Vec<Rect>* skipRects) {
    Size size = bmp->GetSize();
    int stepX = std::max(1, size.dx / 64);
    int stepY = std::max(1, size.dy / 64);
    BitmapPixels* px = GetBitmapPixels(bmp->GetBitmap());
    if (!px) {
        printf("  luminance stats unavailable\n");
        return;
    }
    u64 sumAll = 0, sumText = 0;
    int nAll = 0, nText = 0;
    auto inSkip = [&](int x, int y) -> bool {
        if (!skipRects) {
            return false;
        }
        for (Rect& sr : *skipRects) {
            if (sr.Contains(x, y)) {
                return true;
            }
        }
        return false;
    };
    for (int y = 0; y < size.dy; y += stepY) {
        for (int x = 0; x < size.dx; x += stepX) {
            COLORREF c = GetPixel(px, x, y);
            byte r, g, b;
            UnpackColor(c, r, g, b);
            int lum = (int)r + g + b;
            sumAll += lum;
            nAll++;
            if (!inSkip(x, y)) {
                sumText += lum;
                nText++;
            }
        }
    }
    FinalizeBitmapPixels(px);
    double avgAll = nAll ? (double)sumAll / (3.0 * nAll) : 0.0;
    double avgText = nText ? (double)sumText / (3.0 * nText) : 0.0;
    printf("  luminance avg(all)=%.1f avg(non-skip)=%.1f skipRects=%d\n", avgAll, avgText,
           skipRects ? skipRects->Size() : 0);
}

void TestRenderPage(const Flags& i) {
    if (i.showConsole) {
        RedirectIOToConsole();
    }

    if (i.pageNumber == -1) {
        printf("pageNumber is -1\n");
        return;
    }
    auto files = i.fileNames;
    if (files.Size() == 0) {
        printf("no file provided\n");
        return;
    }

    SetTheme("Black");
    SetPdfDocumentColorMode(PdfDocumentColorMode::Auto);
    (void)PdfDarkModeBuildPalette();

    float zoom = kZoomActualSize;
    if (i.startZoom != kInvalidZoom) {
        zoom = i.startZoom;
    }
    for (auto fileName : files) {
        printf("rendering page %d for '%s', zoom: %.2f\n", i.pageNumber, fileName, zoom);
        auto engine = CreateEngineFromFile(fileName, nullptr, true);
        if (engine == nullptr) {
            printf("failed to create engine\n");
            continue;
        }
        int pageNo = i.pageNumber;
        if (pageNo < 1 || pageNo > engine->PageCount()) {
            printf("invalid page number %d (document has %d pages)\n", pageNo, engine->PageCount());
            SafeEngineRelease(&engine);
            continue;
        }
        RenderPageArgs args(pageNo, zoom, 0);
        DarkModeProfile darkProfile;
        BuildViewDarkModeProfile(engine, &darkProfile);
        if (darkProfile.mode != PageColorMode::Normal) {
            args.darkProfile = &darkProfile;
        }
        auto bmp = engine->RenderPage(args);
        if (bmp == nullptr) {
            printf("failed to render page\n");
        } else if (DarkModeProfileUsesLegacyPostProcess(args.darkProfile)) {
            COLORREF bgCol = args.darkProfile ? args.darkProfile->pageBackground : 0;
            COLORREF textCol = args.darkProfile ? args.darkProfile->foreground : ThemePageRenderColors(bgCol, true);
            COLORREF linkCol = args.darkProfile ? args.darkProfile->linkColor : ThemeWindowLinkColor();
            Vec<Rect> skipRects;
            Vec<Rect>* skipRectsPtr = nullptr;
            bool preserve = args.darkProfile ? (args.darkProfile->mode == PageColorMode::PreserveImages)
                                             : GetPreservePdfImagesInDarkMode();
            if (preserve) {
                Size bmpSize = bmp->GetSize();
                RectF pageRect = engine->PageMediabox(pageNo);
                engine->GetBitmapRecolorSkipRects(pageNo, zoom, 0, pageRect, bmpSize, skipRects);
                if (skipRects.Size() > 0) {
                    skipRectsPtr = &skipRects;
                }
            }
            UpdateBitmapColors(bmp->GetBitmap(), textCol, bgCol, linkCol, skipRectsPtr);
            PrintBitmapLuminanceStats(bmp, skipRectsPtr);
        } else if (bmp) {
            PrintBitmapLuminanceStats(bmp, nullptr);
        }
        delete bmp;
        SafeEngineRelease(&engine);
    }
}

static void extractPageText(EngineBase* engine, int pageNo) {
    PageTextUtf8 pageText = engine->ExtractPageTextUtf8(pageNo);
    if (!pageText.text) {
        return;
    }
    TempStr s = str::ReplaceTemp(pageText.text, "\n", "_");
    printf("text on page %d: '", pageNo);
    // print characters as hex because I don't know what kind of locale-specific mangling
    // printf() might do
    int idx = 0;
    while (s[idx] != 0) {
        char c = s[idx++];
        printf("%02x ", (u8)c);
    }
    printf("'\n");
    FreePageTextUtf8(&pageText);
}

static int CountMatchesFindNext(EngineBase* engine, const WCHAR* term) {
    TextSearch ts(engine);
    int n = 0;
    if (ts.FindFirst(1, term)) {
        n++;
        while (ts.FindNext()) {
            n++;
        }
    }
    return n;
}

static int CountMatchesCollect(EngineBase* engine, const WCHAR* term) {
    TextSearch ts(engine);
    ts.SetText(term);
    ts.SyncPageCount();
    int n = 0;
    for (int pageNo = 1; pageNo <= ts.nPages; pageNo++) {
        Vec<TextSearch::MatchSpan> spans;
        ts.CollectMatchesOnPage(pageNo, &spans);
        n += (int)spans.size();
    }
    return n;
}

static bool VerifyUtf8TextCache(EngineBase* engine) {
    int len1 = 0, len2 = 0;
    const char* t1 = engine->GetTextForPageUtf8(1, &len1);
    const char* t2 = engine->GetTextForPageUtf8(1, &len2);
    if (t1 != t2 || len1 != len2) {
        logf("UTF-8 cache FAIL: ptr %p vs %p, len %d vs %d\n", t1, t2, len1, len2);
        return false;
    }
    logf("UTF-8 cache OK: page 1 len=%d\n", len1);
    return true;
}

void TestSearchCollect(const Flags& ci) {
    if (ci.showConsole) {
        RedirectIOToConsole();
    }

    auto files = ci.fileNames;
    if (files.Size() == 0) {
        logf("search-collect: no file provided\n");
        return;
    }

    int failures = 0;
    const WCHAR* terms[] = {L"the", L"and", L"a", L"of", nullptr};
    for (auto fileName : files) {
        logf("search-collect test: '%s'\n", fileName);
        auto engine = CreateEngineFromFile(fileName, nullptr, true);
        if (!engine) {
            logf("FAIL: could not open '%s'\n", fileName);
            failures++;
            continue;
        }
        if (!VerifyUtf8TextCache(engine)) {
            failures++;
        }
        for (int i = 0; terms[i]; i++) {
            int viaFind = CountMatchesFindNext(engine, terms[i]);
            int viaCollect = CountMatchesCollect(engine, terms[i]);
            if (viaFind != viaCollect) {
                logf("FAIL term '%S': FindNext=%d Collect=%d\n", terms[i], viaFind, viaCollect);
                failures++;
            } else {
                logf("OK term '%S': %d matches\n", terms[i], viaFind);
            }
        }
        SafeEngineRelease(&engine);
    }
    if (failures) {
        logf("search-collect: %d failure(s)\n", failures);
        exit(1);
    }
    logf("search-collect: all checks passed\n");
}

void TestExtractPage(const Flags& ci) {
    if (ci.showConsole) {
        RedirectIOToConsole();
    }

    int pageNo = ci.pageNumber;

    auto files = ci.fileNames;
    if (files.Size() == 0) {
        printf("no file provided\n");
        return;
    }
    for (auto fileName : files) {
        auto engine = CreateEngineFromFile(fileName, nullptr, true);
        if (engine == nullptr) {
            printf("failed to create engine for file '%s'\n", fileName);
            continue;
        }
        if (pageNo < 0) {
            int nPages = engine->PageCount();
            for (int i = 1; i <= nPages; i++) {
                extractPageText(engine, i);
            }
        } else {
            extractPageText(engine, pageNo);
        }

        SafeEngineRelease(&engine);
    }
}
