/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/FileUtil.h"
#include "utils/TgaReader.h"
#include "utils/Timer.h"

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
#include "ExtractPdfToc.h"
#include "OcrService.h"
#include "TocCalib.h"

#include "utils/Log.h"

#include <psapi.h>

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
    auto sampleRgb = [&](int x, int y, int* or_, int* og, int* ob) {
        COLORREF c = GetPixel(px, x, y);
        byte r, g, b;
        UnpackColor(c, r, g, b);
        *or_ = r;
        *og = g;
        *ob = b;
    };
    int midX = size.dx / 2;
    int midY = size.dy / 2;
    int tlR, tlG, tlB, trR, trG, trB, blR, blG, blB, brR, brG, brB, cR, cG, cB;
    sampleRgb(0, 0, &tlR, &tlG, &tlB);
    sampleRgb(size.dx - 1, 0, &trR, &trG, &trB);
    sampleRgb(0, size.dy - 1, &blR, &blG, &blB);
    sampleRgb(size.dx - 1, size.dy - 1, &brR, &brG, &brB);
    sampleRgb(midX, midY, &cR, &cG, &cB);
    FinalizeBitmapPixels(px);
    double avgAll = nAll ? (double)sumAll / (3.0 * nAll) : 0.0;
    double avgText = nText ? (double)sumText / (3.0 * nText) : 0.0;
    char line[512];
    snprintf(line, dimof(line), "  corners TL=(%d,%d,%d) TR=(%d,%d,%d) BL=(%d,%d,%d) BR=(%d,%d,%d) center=(%d,%d,%d)\n",
             tlR, tlG, tlB, trR, trG, trB, blR, blG, blB, brR, brG, brB, cR, cG, cB);
    printf("%s", line);
    snprintf(line, dimof(line), "  luminance avg(all)=%.1f avg(non-skip)=%.1f skipRects=%d size=%dx%d\n", avgAll,
             avgText, skipRects ? skipRects->Size() : 0, size.dx, size.dy);
    printf("%s", line);
}

static const char* PageColorModeLabel(PageColorMode mode) {
    switch (mode) {
        case PageColorMode::Normal:
            return "Normal";
        case PageColorMode::LegacyInvert:
            return "LegacyInvert";
        case PageColorMode::SmartDark:
            return "SmartDark";
        case PageColorMode::FollowThemeDirect:
            return "FollowThemeDirect";
        case PageColorMode::FollowThemeV2:
            return "FollowThemeV2";
        case PageColorMode::PreserveImages:
            return "PreserveImages";
        case PageColorMode::ScanDark:
            return "ScanDark";
        default:
            return "?";
    }
}

static const char* FollowThemePageProbeLabel(int probe) {
    switch (probe) {
        case (int)FollowThemeScanProbe::Unknown:
            return "unset";
        case (int)FollowThemeScanProbe::Mixed:
            return "Mixed";
        case (int)FollowThemeScanProbe::PureScan:
            return "PureScan";
        case (int)FollowThemeScanProbe::BitmapRecolor:
            return "BitmapRecolor";
        default:
            return "?";
    }
}

void TestRenderPage(const Flags& i) {
    if (i.showConsole) {
        RedirectIOToConsole();
    }

    FILE* bench = fopen("c:\\src\\sumatrapdf\\out\\dbg64\\render_bench.txt", "w");
    if (!bench) {
        bench = fopen("c:\\src\\sumatrapdf\\_toc_bench\\render_bench.txt", "w");
    }
    if (!bench) {
        logfa("TestRenderPage: failed to open bench file\n");
    }
    auto diag = [&](const char* line) {
        printf("%s", line);
        logfa("%s", line);
        if (bench) {
            fputs(line, bench);
            fflush(bench);
        }
    };

    if (i.pageNumber < 1) {
        diag("pageNumber invalid (use -render N file.pdf)\n");
        if (bench) {
            fclose(bench);
        }
        return;
    }
    auto files = i.fileNames;
    if (files.Size() == 0) {
        diag("no file provided\n");
        if (bench) {
            fclose(bench);
        }
        return;
    }

    // Prefer setting the theme index without full UI refresh (no main windows in -render).
    SetTheme("Dark-Black");
    SetPdfDocumentColorMode(PdfDocumentColorMode::Auto);
    (void)PdfDarkModeBuildPalette();

    float zoom = 1.f; // RenderPage expects scale (1 = 100%), not virtual zoom percent
    if (i.startZoom != kInvalidZoom) {
        // CLI -zoom is virtual percent (e.g. 50 or 50%); convert to render scale.
        zoom = i.startZoom >= 1.f ? (i.startZoom / 100.f) : i.startZoom;
        if (zoom < 0.05f) {
            zoom = 0.05f;
        }
        if (zoom > 4.f) {
            zoom = 4.f;
        }
    }
    for (auto fileName : files) {
        diag(str::FormatTemp("rendering page %d for '%s', zoom: %.2f\n", i.pageNumber, fileName, zoom));
        auto engine = CreateEngineFromFile(fileName, nullptr, true);
        if (engine == nullptr) {
            diag("failed to create engine\n");
            continue;
        }
        EngineMupdfEnsureFollowThemeProbeDone(engine);
        int pageNo = i.pageNumber;
        if (pageNo < 1 || pageNo > engine->PageCount()) {
            diag(str::FormatTemp("invalid page number %d (document has %d pages)\n", pageNo, engine->PageCount()));
            SafeEngineRelease(&engine);
            continue;
        }
        RenderPageArgs args(pageNo, zoom, 0);
        i64 tLight0 = GetTickCount64();
        auto bmpLight = engine->RenderPage(args);
        i64 lightMs = (i64)(GetTickCount64() - tLight0);
        diag(str::FormatTemp("  light RenderPage ms=%lld bmp=%d", lightMs, bmpLight ? 1 : 0));
        if (bmpLight) {
            Size sz = bmpLight->GetSize();
            diag(str::FormatTemp(" size=%dx%d\n", sz.dx, sz.dy));
        } else {
            diag("\n");
        }
        delete bmpLight;

        DarkModeProfile darkProfile;
        BuildViewDarkModeProfile(engine, &darkProfile);
        diag(str::FormatTemp("  docClass=%d pageProbe=%s(%d) mode=%s soft=%.2f\n",
                             EngineMupdfGetFollowThemeDocClass(engine),
                             FollowThemePageProbeLabel(EngineMupdfGetFollowThemePageProbe(engine, pageNo)),
                             EngineMupdfGetFollowThemePageProbe(engine, pageNo), PageColorModeLabel(darkProfile.mode),
                             darkProfile.options.preserveImagePaperSoftening));
        if (darkProfile.mode != PageColorMode::Normal) {
            args.darkProfile = &darkProfile;
        }
        i64 t0 = GetTickCount64();
        auto bmp = engine->RenderPage(args);
        i64 renderMs = (i64)(GetTickCount64() - t0);
        if (bmp) {
            Size sz = bmp->GetSize();
            diag(str::FormatTemp("  dark RenderPage ms=%lld size=%dx%d\n", renderMs, sz.dx, sz.dy));
        } else {
            diag(str::FormatTemp("  dark RenderPage ms=%lld\n", renderMs));
        }
        i64 tWarm0 = GetTickCount64();
        auto bmpWarm = engine->RenderPage(args);
        i64 warmMs = (i64)(GetTickCount64() - tWarm0);
        diag(str::FormatTemp("  dark RenderPage again ms=%lld\n", warmMs));
        delete bmpWarm;
        if (bmp == nullptr) {
            diag("failed to render page\n");
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
            diag(str::FormatTemp("  afterRender pageProbe=%s(%d)\n",
                                 FollowThemePageProbeLabel(EngineMupdfGetFollowThemePageProbe(engine, pageNo)),
                                 EngineMupdfGetFollowThemePageProbe(engine, pageNo)));
        }
        delete bmp;
        int nPages = engine->PageCount();
        int flipMax = nPages < 5 ? nPages : 5;
        for (int p = 1; p <= flipMax; p++) {
            if (p == pageNo) {
                continue;
            }
            RenderPageArgs flipLight(p, zoom, 0);
            i64 tFlipL = GetTickCount64();
            auto bmpL = engine->RenderPage(flipLight);
            i64 flipLightMs = (i64)(GetTickCount64() - tFlipL);
            delete bmpL;
            RenderPageArgs flipDark(p, zoom, 0);
            flipDark.darkProfile = args.darkProfile;
            i64 tFlipD = GetTickCount64();
            auto bmpD = engine->RenderPage(flipDark);
            i64 flipDarkMs = (i64)(GetTickCount64() - tFlipD);
            delete bmpD;
            diag(str::FormatTemp("  flip p%d light=%lldms dark=%lldms\n", p, flipLightMs, flipDarkMs));
        }
        SafeEngineRelease(&engine);
    }
    if (bench) {
        fclose(bench);
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

static int CountMatchesFindNext(EngineBase* engine, const WCHAR* term, Vec<u64>* positions = nullptr) {
    TextSearch ts(engine);
    int n = 0;
    if (ts.FindFirst(1, term)) {
        n++;
        if (positions) {
            positions->Append(((u64)(u32)ts.startPage << 32) | (u32)ts.startGlyph);
        }
        while (ts.FindNext()) {
            n++;
            if (positions) {
                positions->Append(((u64)(u32)ts.startPage << 32) | (u32)ts.startGlyph);
            }
        }
    }
    return n;
}

static void CollectMatchPositions(EngineBase* engine, const WCHAR* term, Vec<u64>& positions) {
    TextSearch ts(engine);
    ts.SetText(term);
    ts.SyncPageCount();
    for (int pageNo = 1; pageNo <= ts.nPages; pageNo++) {
        Vec<TextSearch::MatchSpan> spans;
        ts.CollectMatchesOnPage(pageNo, &spans);
        for (TextSearch::MatchSpan& span : spans) {
            u64 key = ((u64)(u32)span.startPage << 32) | (u32)span.startGlyph;
            positions.Append(key);
        }
    }
}

static void LogMissingFindNextPositions(const Vec<u64>& viaFind, const Vec<u64>& viaCollect) {
    int findIdx = 0;
    for (int i = 0; i < (int)viaCollect.size(); i++) {
        u64 expected = viaCollect[i];
        while (findIdx < (int)viaFind.size() && viaFind[findIdx] < expected) {
            findIdx++;
        }
        if (findIdx >= (int)viaFind.size() || viaFind[findIdx] != expected) {
            logf("  FindNext missing page=%d glyph=%d\n", (int)(expected >> 32), (int)(expected & 0xffffffff));
        }
    }
}

struct SearchBenchMemory {
    size_t workingSet = 0;
    size_t peakWorkingSet = 0;
};

static SearchBenchMemory GetSearchBenchMemory() {
    SearchBenchMemory result;
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    using GetProcessMemoryInfoFunc = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    auto getMemoryInfo =
        kernel32 ? (GetProcessMemoryInfoFunc)GetProcAddress(kernel32, "K32GetProcessMemoryInfo") : nullptr;
    if (getMemoryInfo && getMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        result.workingSet = counters.WorkingSetSize;
        result.peakWorkingSet = counters.PeakWorkingSetSize;
    }
    return result;
}

static bool EqualMatchPositions(const Vec<u64>& a, const Vec<u64>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (int i = 0; i < (int)a.size(); i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static void RemovePositionSuffix(Vec<u64>& positions, int pageNo) {
    for (int i = (int)positions.size() - 1; i >= 0; i--) {
        if ((int)(positions[i] >> 32) >= pageNo) {
            positions.RemoveAt(i);
        }
    }
}

static bool VerifyIncrementalMatchPositions(EngineBase* engine, const WCHAR* term) {
    Vec<u64> expected;
    CollectMatchPositions(engine, term, expected);

    int pageCount = engine->PageCount();
    int frontiers[] = {std::max(1, pageCount / 4), std::max(1, pageCount / 2), std::max(1, pageCount * 3 / 4),
                       pageCount};
    int resumeFromPage = 1;
    Vec<u64> actual;
    for (int frontier : frontiers) {
        RemovePositionSuffix(actual, resumeFromPage);
        TextSearch ts(engine);
        ts.SetMaxPageCount(frontier);
        ts.SetText(term);
        ts.SyncPageCount();
        int continuationPage = 0;
        for (int pageNo = resumeFromPage; pageNo <= frontier; pageNo++) {
            Vec<TextSearch::MatchSpan> spans;
            ts.CollectMatchesOnPage(pageNo, &spans, &continuationPage);
            for (TextSearch::MatchSpan& span : spans) {
                u64 key = ((u64)(u32)span.startPage << 32) | (u32)span.startGlyph;
                actual.Append(key);
            }
        }
        resumeFromPage = continuationPage > 0 ? continuationPage : frontier + 1;
    }
    bool same = EqualMatchPositions(expected, actual);
    logf("search-incremental term '%S': expected=%d actual=%d equal=%d\n", term, (int)expected.size(),
         (int)actual.size(), same);
    return same;
}

static double MedianOfThree(double* values) {
    if (values[0] > values[1]) {
        double tmp = values[0];
        values[0] = values[1];
        values[1] = tmp;
    }
    if (values[1] > values[2]) {
        double tmp = values[1];
        values[1] = values[2];
        values[2] = tmp;
    }
    if (values[0] > values[1]) {
        double tmp = values[0];
        values[0] = values[1];
        values[1] = tmp;
    }
    return values[1];
}

static bool BenchmarkSearchCacheStages(EngineBase* engine, const WCHAR* term, const char* termKind) {
    constexpr int kRounds = 3;
    double coldMs[kRounds]{};
    double hotMs[kRounds]{};
    double cachedMs[kRounds]{};
    bool ok = true;
    logf("search-bench term '%S' (%s)\n", term, termKind);
    for (int round = 0; round < kRounds; round++) {
        engine->ClearTextCache();
        Vec<u64> coldPositions;
        auto t = TimeGet();
        CollectMatchPositions(engine, term, coldPositions);
        coldMs[round] = TimeSinceInMs(t);
        SearchBenchMemory coldMem = GetSearchBenchMemory();

        Vec<u64> hotPositions;
        t = TimeGet();
        CollectMatchPositions(engine, term, hotPositions);
        hotMs[round] = TimeSinceInMs(t);
        SearchBenchMemory hotMem = GetSearchBenchMemory();

        volatile u64 checksum = 0;
        t = TimeGet();
        for (u64 key : hotPositions) {
            checksum ^= key;
        }
        cachedMs[round] = TimeSinceInMs(t);
        SearchBenchMemory cachedMem = GetSearchBenchMemory();

        bool same = EqualMatchPositions(coldPositions, hotPositions);
        ok = ok && same;
        logf(
            "  round %d: cold=%.2f ms hot=%.2f ms cached=%.4f ms matches=%d equal=%d checksum=%llu "
            "ws=%.1f/%.1f/%.1f MiB processPeak=%.1f MiB\n",
            round + 1, coldMs[round], hotMs[round], cachedMs[round], (int)hotPositions.size(), same,
            (unsigned long long)checksum, coldMem.workingSet / (1024.0 * 1024.0), hotMem.workingSet / (1024.0 * 1024.0),
            cachedMem.workingSet / (1024.0 * 1024.0), cachedMem.peakWorkingSet / (1024.0 * 1024.0));
    }
    logf("  median: cold=%.2f ms hot=%.2f ms cached=%.4f ms\n", MedianOfThree(coldMs), MedianOfThree(hotMs),
         MedianOfThree(cachedMs));
    return ok;
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
    // The process already owns the console when -console was parsed.

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
        // Standalone tests have no foreground tab to resume the progressive
        // MuPDF chapter counter. Treat this engine as foreground while waiting.
        EngineMupdfSetReflowLoadWhenForeground(engine, true);
        auto progressiveStart = TimeGet();
        while (EngineIsProgressiveEbookLoading(engine)) {
            Sleep(50);
        }
        logf("search-bench progressive load done: %.2f ms, pages=%d\n", TimeSinceInMs(progressiveStart),
             engine->PageCount());
        if (!VerifyUtf8TextCache(engine)) {
            failures++;
        }
        for (int i = 0; terms[i]; i++) {
            Vec<u64> findPositions;
            Vec<u64> collectPositions;
            int viaFind = CountMatchesFindNext(engine, terms[i], &findPositions);
            CollectMatchPositions(engine, terms[i], collectPositions);
            int viaCollect = (int)collectPositions.size();
            if (viaFind != viaCollect) {
                logf("FAIL term '%S': FindNext=%d Collect=%d\n", terms[i], viaFind, viaCollect);
                LogMissingFindNextPositions(findPositions, collectPositions);
                failures++;
            } else {
                logf("OK term '%S': %d matches\n", terms[i], viaFind);
            }
        }
        if (!BenchmarkSearchCacheStages(engine, L"the", "ordinary")) {
            failures++;
        }
        if (!BenchmarkSearchCacheStages(engine, L"a", "high-frequency")) {
            failures++;
        }
        if (!VerifyIncrementalMatchPositions(engine, L"the")) {
            failures++;
        }
        SafeEngineRelease(&engine);
    }
    if (failures) {
        logf("search-collect: %d failure(s)\n", failures);
        exit(1);
    }
    logf("search-collect: all checks passed\n");
}

static void TocJsonEscape(char* dst, int cap, const char* s) {
    int o = 0;
    for (int i = 0; s && s[i] && o < cap - 2; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)c;
            continue;
        }
        if (c < 32) {
            continue;
        }
        dst[o++] = (char)c;
    }
    dst[o] = 0;
}

static void FlattenExtractedToc(const Vec<ExtractedTocItem*>& roots, Vec<ExtractedTocItem*>& flat) {
    for (ExtractedTocItem* n : roots) {
        if (!n) {
            continue;
        }
        flat.Append(n);
        FlattenExtractedToc(n->children, flat);
    }
}

static void CompactText(const char* s, char* out, int cap) {
    int o = 0;
    for (int i = 0; s && s[i] && o < cap - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xA0) {
            continue;
        }
        if (c == 0xC2 && (unsigned char)s[i + 1] == 0xA8) {
            i++;
            continue;
        }
        if (c == 0xE3 && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) {
            i += 2;
            continue;
        }
        if (c == 0xEF && (unsigned char)s[i + 1] == 0xBC && (unsigned char)s[i + 2] >= 0x90 &&
            (unsigned char)s[i + 2] <= 0x99) {
            out[o++] = (char)('0' + ((unsigned char)s[i + 2] - 0x90));
            i += 2;
            continue;
        }
        if (c == 0xEF && (unsigned char)s[i + 1] == 0xBC && (unsigned char)s[i + 2] == 0x8E) {
            out[o++] = '.';
            i += 2;
            continue;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
}

static void TruncateUtf8Bytes(char* s, int maxBytes) {
    int i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        int n = 1;
        if ((c & 0x80) == 0) {
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        } else {
            n = 1;
        }
        if (i + n > maxBytes) {
            break;
        }
        i += n;
    }
    s[i] = 0;
}

static char* CollectPageCompact(EngineBase* engine, int pageNo) {
    Vec<EngineMupdfPageLine> raw;
    EngineMupdfCollectPageLines(engine, pageNo, raw);
    int n = 0;
    for (int i = 0; i < raw.Size(); i++) {
        if (raw[i].text) {
            n += (int)str::Len(raw[i].text) + 1;
        }
    }
    char* buf = AllocArray<char>(n + 8);
    int o = 0;
    for (int i = 0; i < raw.Size(); i++) {
        if (!raw[i].text) {
            continue;
        }
        int len = (int)str::Len(raw[i].text);
        memcpy(buf + o, raw[i].text, (size_t)len);
        o += len;
    }
    buf[o] = 0;
    EngineMupdfFreePageLines(raw);
    char compact[8000];
    CompactText(buf, compact, (int)sizeof(compact));
    str::Free(buf);
    return str::Dup(compact);
}

static bool TitleHitsPage(const char* title, const char* pageCompact) {
    if (!title || !pageCompact || !pageCompact[0]) {
        return false;
    }
    char t[512];
    CompactText(title, t, (int)sizeof(t));
    if (!t[0]) {
        return false;
    }
    int len = (int)str::Len(t);
    if (len > 72) {
        TruncateUtf8Bytes(t, 72);
    }
    if (str::Find(pageCompact, t) != nullptr) {
        return true;
    }
    int start = 0;
    while (t[start] && ((unsigned char)t[start] < 128) &&
           (t[start] == '.' || (t[start] >= '0' && t[start] <= '9') || t[start] == '(' || t[start] == ')')) {
        start++;
    }
    return start > 0 && t[start] && str::Find(pageCompact, t + start) != nullptr;
}

void TestExtractTocBench(const Flags& ci) {
    if (ci.showConsole) {
        RedirectIOToConsole();
    }
    CreateDirectoryW(L"c:\\src\\sumatrapdf\\_toc_bench", nullptr);
    StrVec files;
    for (auto f : ci.fileNames) {
        files.Append(f);
    }
    if (files.Size() == 0) {
        ByteSlice data = file::ReadFile("c:\\src\\sumatrapdf\\_toc_bench\\files.txt");
        if (data.data() && data.sz > 0) {
            char* txt = AllocArray<char>((int)data.sz + 2);
            memcpy(txt, data.data(), data.sz);
            txt[data.sz] = 0;
            char* start = txt;
            for (int i = 0;; i++) {
                if (txt[i] == '\n' || txt[i] == 0) {
                    char save = txt[i];
                    txt[i] = 0;
                    if (start[0]) {
                        if (start[0] == '\r') {
                            start++;
                        }
                        int n = (int)str::Len(start);
                        if (n > 0 && start[n - 1] == '\r') {
                            start[n - 1] = 0;
                        }
                        if (start[0]) {
                            files.Append(start);
                        }
                    }
                    if (save == 0) {
                        break;
                    }
                    txt[i] = save;
                    start = txt + i + 1;
                }
            }
            str::Free(txt);
        }
        data.Free();
    }
    FILE* out = fopen("c:\\src\\sumatrapdf\\_toc_bench\\report.jsonl", "w");
    FILE* sum = fopen("c:\\src\\sumatrapdf\\_toc_bench\\summary.txt", "w");
    int nOk = 0, nNoText = 0, nNoHead = 0, nFail = 0, nOpen = 0, nMiss = 0, nMono = 0;
    for (auto fileName : files) {
        logf("toc-bench: %s\n", fileName);
        auto engine = CreateEngineFromFile(fileName, nullptr, true);
        if (!engine) {
            nOpen++;
            if (out) {
                fprintf(out, "{\"file\":\"%s\",\"status\":\"open-fail\"}\n", fileName);
            }
            continue;
        }
        char forceOcr[8]{};
        if (GetEnvironmentVariableA("SUMATRA_TOC_BENCH_OCR", forceOcr, dimof(forceOcr)) > 0) {
            for (int pageNo = 1; pageNo <= engine->PageCount(); pageNo++) {
                OcrRecognizeEnginePage(engine, pageNo, true, OcrOperation::AllPages);
            }
        }
        Vec<ExtractedTocItem*> roots;
        int nItems = 0;
        ExtractPdfTocKind kind = ExtractPdfTocFromEngine(engine, roots, &nItems);
        const char* st = "ok";
        if (kind == ExtractPdfTocKind::NoText) {
            st = "notext";
            nNoText++;
        } else if (kind == ExtractPdfTocKind::NoHeadings) {
            st = "noheadings";
            nNoHead++;
        } else if (kind != ExtractPdfTocKind::Ok) {
            st = "fail";
            nFail++;
        } else {
            nOk++;
        }
        Vec<ExtractedTocItem*> flat;
        FlattenExtractedToc(roots, flat);
        int miss = 0;
        int mono = 0;
        int prevPage = 0;
        // Heap: a flat TOC can be large; a 256KB stack buffer overflows on some PDFs.
        const int kItemsCap = 256000;
        char* itemsBuf = AllocArray<char>(kItemsCap);
        if (!itemsBuf) {
            DeleteExtractedTocItems(roots);
            SafeEngineRelease(&engine);
            continue;
        }
        itemsBuf[0] = 0;
        int io = 0;
        for (int i = 0; i < flat.Size(); i++) {
            ExtractedTocItem* it = flat[i];
            if (prevPage > 0 && it->pageNo > 0 && it->pageNo < prevPage) {
                mono++;
            }
            if (it->pageNo > 0) {
                prevPage = it->pageNo;
            }
            char* pageTxt = it->pageNo > 0 ? CollectPageCompact(engine, it->pageNo) : nullptr;
            bool hit = TitleHitsPage(it->title, pageTxt);
            if (!hit) {
                miss++;
            }
            str::Free(pageTxt);
            char esc[400];
            TocJsonEscape(esc, (int)sizeof(esc), it->title ? it->title : "");
            if (io < kItemsCap - 80) {
                int n = snprintf(itemsBuf + io, (size_t)(kItemsCap - io),
                                 "%s{\"l\":%d,\"p\":%d,\"pp\":%d,\"tp\":%d,\"src\":%d,\"body\":%d,\"ver\":%d,\"hit\":%"
                                 "d,\"t\":\"%s\"}",
                                 io ? "," : "", it->level, it->pageNo, it->printedPage, it->tocPageNo, (int)it->source,
                                 it->bodyMatched ? 1 : 0, it->verified ? 1 : 0, hit ? 1 : 0, esc);
                if (n > 0) {
                    io += n;
                }
            }
        }
        nMiss += miss;
        nMono += mono;
        char fesc[800];
        TocJsonEscape(fesc, (int)sizeof(fesc), fileName);
        if (out) {
            fprintf(
                out,
                "{\"file\":\"%s\",\"status\":\"%s\",\"pages\":%d,\"n\":%d,\"miss\":%d,\"mono\":%d,\"items\":[%s]}\n",
                fesc, st, engine->PageCount(), nItems, miss, mono, itemsBuf);
        }
        if (sum) {
            fprintf(sum, "%s\t%s\tpages=%d n=%d miss=%d mono=%d\n", st, fileName, engine->PageCount(), nItems, miss,
                    mono);
        }
        free(itemsBuf);
        DeleteExtractedTocItems(roots);
        SafeEngineRelease(&engine);
    }
    if (sum) {
        fprintf(sum, "ok=%d notext=%d noheadings=%d fail=%d openfail=%d miss=%d mono=%d files=%d\n", nOk, nNoText,
                nNoHead, nFail, nOpen, nMiss, nMono, files.Size());
        fclose(sum);
    }
    if (out) {
        fclose(out);
    }
    logf("toc-bench: ok=%d notext=%d noheadings=%d fail=%d openfail=%d miss=%d mono=%d files=%d\n", nOk, nNoText,
         nNoHead, nFail, nOpen, nMiss, nMono, files.Size());
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
