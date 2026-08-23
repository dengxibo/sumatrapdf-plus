/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dict.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include <math.h>

#include <CommCtrl.h>

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"
#include "wingui/LabelWithCloseWnd.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "TableOfContents.h"
#include "Selection.h"
#include "Notifications.h"
#include "Translations.h"
#include "GlobalPrefs.h"
#include "Toolbar.h"
#include "SvgIcons.h"
#include "Theme.h"
#include "DarkModeSubclass.h"
#include "Flags.h"
#include "ExtractPdfToc.h"
#include "ExtractBookToc.h"
#include "TocCalib.h"

#include "utils/Log.h"

static void TocCalibRestoreDisplayMode(MainWindow* win, TocCalibSession* s);

static bool TocCalibHasPrinted(int printed) {
    return printed > 0;
}

static int TocCalibRowPdf(const TocCalibRow* row);
static int TocCalibLabelPrinted(const TocCalibSession* s, int pdf);
static bool TocCalibLabelIsPlainPdf(const char* label, int pdf);
static bool TocCalibNoPrintedTitle(const char* s);

static int TocCalibMajorityOffset(const int* offs, int n) {
    if (!offs || n < 1) {
        return -1;
    }
    int sorted[64];
    int m = n < 64 ? n : 64;
    for (int i = 0; i < m; i++) {
        sorted[i] = offs[i];
    }
    for (int i = 0; i < m; i++) {
        for (int j = i + 1; j < m; j++) {
            if (sorted[j] < sorted[i]) {
                int t = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = t;
            }
        }
    }
    int bestVal = sorted[0];
    int bestCnt = 1;
    int curVal = sorted[0];
    int curCnt = 1;
    for (int i = 1; i < m; i++) {
        if (sorted[i] == curVal) {
            curCnt++;
        } else {
            if (curCnt > bestCnt) {
                bestCnt = curCnt;
                bestVal = curVal;
            }
            curVal = sorted[i];
            curCnt = 1;
        }
    }
    if (curCnt > bestCnt) {
        bestCnt = curCnt;
        bestVal = curVal;
    }
    int runners = 0;
    curVal = sorted[0];
    curCnt = 1;
    for (int i = 1; i <= m; i++) {
        if (i < m && sorted[i] == curVal) {
            curCnt++;
            continue;
        }
        if (curCnt == bestCnt) {
            runners++;
        }
        if (i < m) {
            curVal = sorted[i];
            curCnt = 1;
        }
    }
    if (bestCnt >= 2 && runners == 1) {
        return bestVal;
    }
    return sorted[m / 2];
}

int TocCalibSolveOffset(const Vec<TocCalibMapRow>& rows) {
    int offs[64];
    int n = 0;
    for (int i = 0; i < rows.Size() && n < 64; i++) {
        const TocCalibMapRow& r = rows[i];
        if (!TocCalibHasPrinted(r.printedPage)) {
            continue;
        }
        int src = r.pdfPage;
        if (r.pdfPinned && r.pdfPage > 0) {
            src = r.pdfPage;
        } else if (r.identPage > 0) {
            src = r.identPage;
        }
        if (src < 1) {
            continue;
        }
        int off = src - r.printedPage;
        if (off < 0) {
            continue;
        }
        offs[n++] = off;
    }
    return TocCalibMajorityOffset(offs, n);
}

void TocCalibApplyOffset(Vec<TocCalibMapRow>& rows, int offset, bool force) {
    if (offset < 0) {
        return;
    }
    for (int i = 0; i < rows.Size(); i++) {
        TocCalibMapRow& r = rows[i];
        if (!TocCalibHasPrinted(r.printedPage) || r.pdfPinned) {
            continue;
        }
        if (!force && r.bodyMatched && r.pdfPage > 0) {
            continue;
        }
        int pdf = r.printedPage + offset;
        if (pdf < 1) {
            pdf = 1;
        }
        r.pdfPage = pdf;
    }
}

static bool TocCalibSkipMatchCp(int cp) {
    if (cp <= 32 || cp == 0x3000 || cp == 0x00A0) {
        return true;
    }
    if (cp == '.' || cp == 0xFF0E || cp == 0x3002 || cp == 0x2026 || cp == 0x00B7 || cp == 0x30FB || cp == 0x2022) {
        return true;
    }
    if (cp == ',' || cp == 0xFF0C || cp == ':' || cp == 0xFF1A || cp == ';' || cp == 0xFF1B) {
        return true;
    }
    if (cp == '-' || cp == 0x2013 || cp == 0x2014 || cp == 0x2015 || cp == 0xFF0D || cp == 0x2500) {
        return true;
    }
    if (cp == '+' || cp == '*' || cp == '|' || cp == '/' || cp == 0xFF0F) {
        return true;
    }
    return false;
}

static int TocCalibGlyphCount(const char* s) {
    if (!s) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (!TocCalibSkipMatchCp(cp)) {
            n++;
        }
    }
    return n;
}

static bool TocCalibIsFindSpaceCp(int cp) {
    return cp <= 32 || cp == 0x3000 || cp == 0x00A0;
}

static bool TocCalibIsFindDigitCp(int cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19);
}

static bool TocCalibIsFindLeaderCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x3002 || cp == 0x2026 || cp == 0x2024 || cp == 0x2025 || cp == 0x22EF ||
           cp == 0x00B7 || cp == 0x30FB || cp == 0x2022 || cp == 0xFF65 || cp == 0x2500 || cp == 0x2013 ||
           cp == 0x2014 || cp == 0x2015 || cp == 0xFF0D || cp == 0x30A0;
}

// Strip trailing TOC leaders / printed page numbers. Keep "Chapter 12".
static void TocCalibCleanFindTitle(const char* title, char* out, int cap) {
    if (!out || cap < 2) {
        return;
    }
    out[0] = 0;
    if (!title || !title[0]) {
        return;
    }
    char tmp[1024]{};
    int nCopy = (int)str::Len(title);
    if (nCopy >= (int)sizeof(tmp)) {
        nCopy = (int)sizeof(tmp) - 1;
    }
    memcpy(tmp, title, (size_t)nCopy);
    tmp[nCopy] = 0;
    str::TrimWSInPlace(tmp, str::TrimOpt::Both);
    int len = (int)str::Len(tmp);
    if (len < 1) {
        return;
    }

    int starts[512];
    int ncp = 0;
    int pos = 0;
    while (pos < len && ncp < 512) {
        starts[ncp++] = pos;
        int cp = Utf8CodepointNext(tmp, len, pos);
        if (cp <= 0) {
            break;
        }
    }
    if (ncp < 1) {
        str::BufSet(out, cap, tmp);
        return;
    }

    int end = ncp;
    while (end > 0) {
        int t = starts[end - 1];
        if (!TocCalibIsFindSpaceCp(Utf8CodepointNext(tmp, len, t))) {
            break;
        }
        end--;
    }
    int afterNum = end;
    while (end > 0) {
        int t = starts[end - 1];
        if (!TocCalibIsFindDigitCp(Utf8CodepointNext(tmp, len, t))) {
            break;
        }
        end--;
    }
    bool hadDigits = end < afterNum;
    int spaces = 0;
    while (end > 0) {
        int t = starts[end - 1];
        if (!TocCalibIsFindSpaceCp(Utf8CodepointNext(tmp, len, t))) {
            break;
        }
        end--;
        spaces++;
    }
    bool hadLeaders = false;
    while (end > 0) {
        int t = starts[end - 1];
        if (!TocCalibIsFindLeaderCp(Utf8CodepointNext(tmp, len, t))) {
            break;
        }
        end--;
        hadLeaders = true;
    }
    while (end > 0) {
        int t = starts[end - 1];
        if (!TocCalibIsFindSpaceCp(Utf8CodepointNext(tmp, len, t))) {
            break;
        }
        end--;
    }

    bool stripNum = hadDigits && (hadLeaders || spaces >= 2);
    if (!stripNum) {
        end = ncp;
    }
    if (end < 1) {
        end = ncp;
    }
    int byteEnd = (end >= ncp) ? len : starts[end];
    if (byteEnd >= cap) {
        byteEnd = cap - 1;
    }
    memcpy(out, tmp, (size_t)byteEnd);
    out[byteEnd] = 0;
    str::TrimWSInPlace(out, str::TrimOpt::Both);
}

// First nGlyphs content glyphs from the original title, keeping punctuation between them.
static bool TocCalibFindTitlePrefix(const char* title, int nGlyphs, char* out, int cap) {
    if (!out || cap < 2) {
        return false;
    }
    out[0] = 0;
    if (!title || nGlyphs < 1) {
        return false;
    }
    int len = (int)str::Len(title);
    int i = 0;
    int w = 0;
    int n = 0;
    int lastContentEnd = 0;
    while (i < len && w < cap - 4) {
        int save = i;
        int cp = Utf8CodepointNext(title, len, i);
        if (cp <= 0) {
            break;
        }
        int nbytes = i - save;
        if (nbytes < 1 || w + nbytes >= cap) {
            break;
        }
        memcpy(out + w, title + save, (size_t)nbytes);
        w += nbytes;
        if (!TocCalibSkipMatchCp(cp)) {
            n++;
            lastContentEnd = w;
            if (n >= nGlyphs) {
                break;
            }
        }
    }
    out[lastContentEnd] = 0;
    return n >= nGlyphs && lastContentEnd > 0;
}

struct TocCalibFindQueries {
    char cleaned[512]{};
    char prefix12[256]{};
    char prefix8[256]{};
    char prefix6[256]{};
    const char* q[4]{};
    int n = 0;
};

static bool TocCalibAddFindQuery(TocCalibFindQueries* q, const char* s, int minGlyphs) {
    if (!q || !s || !s[0] || q->n >= 4) {
        return false;
    }
    if (TocCalibGlyphCount(s) < minGlyphs) {
        return false;
    }
    for (int i = 0; i < q->n; i++) {
        if (str::Eq(q->q[i], s)) {
            return false;
        }
    }
    q->q[q->n++] = s;
    return true;
}

static bool TocCalibBuildFindQueries(const char* title, TocCalibFindQueries* q) {
    if (!q) {
        return false;
    }
    *q = {};
    if (!title || !title[0]) {
        return false;
    }
    TocCalibCleanFindTitle(title, q->cleaned, (int)sizeof(q->cleaned));
    const char* full = q->cleaned[0] ? q->cleaned : title;
    TocCalibAddFindQuery(q, full, 4);
    if (TocCalibFindTitlePrefix(title, 12, q->prefix12, (int)sizeof(q->prefix12))) {
        TocCalibAddFindQuery(q, q->prefix12, 6);
    }
    if (TocCalibFindTitlePrefix(title, 8, q->prefix8, (int)sizeof(q->prefix8))) {
        TocCalibAddFindQuery(q, q->prefix8, 6);
    }
    if (TocCalibFindTitlePrefix(title, 6, q->prefix6, (int)sizeof(q->prefix6))) {
        TocCalibAddFindQuery(q, q->prefix6, 6);
    }
    return q->n > 0;
}

static void TocCalibCompact(const char* s, char* out, int cap) {
    if (!out || cap < 2) {
        return;
    }
    out[0] = 0;
    if (!s) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int w = 0;
    while (i < len && w < cap - 4) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (TocCalibSkipMatchCp(cp)) {
            continue;
        }
        int n = i - save;
        if (n < 1 || w + n >= cap) {
            break;
        }
        memcpy(out + w, s + save, (size_t)n);
        w += n;
    }
    out[w] = 0;
}

static int TocCalibInOrderCover(const char* body, const char* title) {
    if (!body || !title) {
        return 0;
    }
    int tlen = (int)str::Len(title);
    int blen = (int)str::Len(body);
    int ti = 0;
    int bi = 0;
    int n = 0;
    while (ti < tlen) {
        int tcp = Utf8CodepointNext(title, tlen, ti);
        if (tcp <= 0) {
            break;
        }
        if (TocCalibSkipMatchCp(tcp)) {
            continue;
        }
        int bstart = bi;
        bool found = false;
        while (bi < blen) {
            int bcp = Utf8CodepointNext(body, blen, bi);
            if (bcp <= 0) {
                break;
            }
            if (TocCalibSkipMatchCp(bcp)) {
                continue;
            }
            if (bcp == tcp) {
                found = true;
                break;
            }
        }
        if (!found) {
            bi = bstart;
            continue;
        }
        n++;
    }
    return n;
}

int TocCalibTitleMatchScore(const char* body, const char* title) {
    if (!body || !title || !title[0]) {
        return 0;
    }
    int tg = TocCalibGlyphCount(title);
    if (tg < 2) {
        return 0;
    }
    if (str::Eq(body, title) || str::StartsWith(body, title) || str::Find(body, title)) {
        return tg;
    }
    int bg = TocCalibGlyphCount(body);
    if (tg < 4) {
        char tbuf[128];
        char bbuf[256];
        TocCalibCompact(title, tbuf, (int)sizeof(tbuf));
        TocCalibCompact(body, bbuf, (int)sizeof(bbuf));
        if (tbuf[0] && (str::Eq(bbuf, tbuf) || str::StartsWith(bbuf, tbuf))) {
            return tg;
        }
        return 0;
    }
    if (bg >= 4 && bg * 2 >= tg && str::StartsWith(title, body)) {
        return bg;
    }
    char tbuf[512];
    char bbuf[768];
    TocCalibCompact(title, tbuf, (int)sizeof(tbuf));
    TocCalibCompact(body, bbuf, (int)sizeof(bbuf));
    if (tbuf[0] && (str::Eq(bbuf, tbuf) || str::StartsWith(bbuf, tbuf) || str::Find(bbuf, tbuf))) {
        return tg;
    }
    if (bg >= 4 && bg * 2 >= tg && tbuf[0] && str::StartsWith(tbuf, bbuf)) {
        return bg;
    }
    int cover = TocCalibInOrderCover(body, title);
    if (cover >= 4 && cover * 5 >= tg * 4) {
        return cover;
    }
    return 0;
}

bool TocCalibApplyNearHit(ExtractedTocItem* it, int hitPage, float x, float y, int score, int predPage) {
    if (!it || hitPage < 1 || score < 2) {
        return false;
    }
    int d = 0;
    if (predPage > 0) {
        d = hitPage - predPage;
        if (d < 0) {
            d = -d;
        }
        if (d > 4) {
            return false;
        }
        if (score < 4 && d > 0) {
            return false;
        }
    }
    it->pageNo = hitPage;
    it->x = x;
    it->y = y;
    it->bodyMatched = true;
    if (it->confidence < 70) {
        it->confidence = 70;
    }
    if (score >= 6 && it->confidence < 85) {
        it->confidence = 85;
    }
    if (score >= 6 && d <= 1) {
        it->verified = true;
    }
    return true;
}

struct TocCalibNearHit {
    int page = 0;
    int score = 0;
    float x = 0;
    float y = 0;
    float fontSize = 0;
};

static bool TocCalibSearchTextFindFallback(TocCalibSession* s, TocCalibRow* row, const char* title,
                                           TocCalibNearHit* hit);

static bool TocCalibPageInToc(const TocCalibSession* s, int page) {
    if (!s || page < 1) {
        return false;
    }
    int lo = s->tocPage;
    int hi = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    if (lo < 1) {
        return false;
    }
    if (hi < lo) {
        hi = lo;
    }
    return page >= lo && page <= hi;
}

static bool TocCalibIsContentsTitle(const char* s);
static void TocCalibEnsureTocRange(TocCalibSession* s);
static void TocCalibClearDestsOnTocPages(TocCalibSession* s);
static int TocCalibRowIndexOf(TocCalibSession* s, TocCalibRow* row);
static void TocCalibFillAllPrinted(TocCalibSession* s);
static int TocCalibGuessPrinted(const TocCalibSession* s, const TocCalibRow* row, int fallbackPdf = 0);
static void TocCalibPinRowToPage(TocCalibSession* s, TocCalibRow* row, int page, float x, float y, const char* label);
static void TocCalibSpinPrinted(TocCalibRow* row, int delta, TocCalibSession* s);

static const Vec<EngineMupdfPageLine>* TocCalibCachePage(TocCalibSession* s, int page, Vec<int>& pages,
                                                         Vec<Vec<EngineMupdfPageLine>*>& cache) {
    for (int i = 0; i < pages.Size(); i++) {
        if (pages[i] == page) {
            return cache[i];
        }
    }
    auto* lines = new Vec<EngineMupdfPageLine>;
    if (s && s->engine) {
        EngineMupdfCollectPageLines(s->engine, page, *lines);
    }
    pages.Append(page);
    cache.Append(lines);
    return lines;
}

static void TocCalibBm25AddDf(dict::MapStrToInt* df, const char* key) {
    if (!df || !key || !key[0]) {
        return;
    }
    int existing = 0;
    if (!df->Insert(key, 1, &existing)) {
        df->Remove(key, nullptr);
        df->Insert(key, existing + 1);
    }
}

static void TocCalibBm25Key(int a, int b, char* out, int cap) {
    if (!out || cap < 9) {
        return;
    }
    char* p = out;
    str::Utf8Encode(p, a);
    str::Utf8Encode(p, b);
    *p = 0;
}

static int TocCalibBm25CollectGlyphs(const char* s, int* cps, int cap) {
    if (!s || !cps || cap < 1) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    while (i < len && n < cap) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (TocCalibSkipMatchCp(cp)) {
            continue;
        }
        cps[n++] = cp;
    }
    return n;
}

static int TocCalibBm25Tf(const char* compact, int qa, int qb) {
    if (!compact) {
        return 0;
    }
    int cps[1024];
    int n = TocCalibBm25CollectGlyphs(compact, cps, dimof(cps));
    int tf = 0;
    for (int i = 0; i + 1 < n; i++) {
        if (cps[i] == qa && cps[i + 1] == qb) {
            tf++;
        }
    }
    return tf;
}

struct TocCalibBm25Page {
    int page = 0;
    int nTokens = 0;
    char* compact = nullptr;
    char* headingCompact = nullptr;
    float maxY = 1;
    float avgFont = 12;
};

struct TocCalibBm25Index {
    Vec<TocCalibBm25Page> pages;
    dict::MapStrToInt* df = nullptr;
    int nDocs = 0;
    float avgdl = 1;
};

static void TocCalibBm25Free(TocCalibBm25Index* idx) {
    if (!idx) {
        return;
    }
    for (int i = 0; i < idx->pages.Size(); i++) {
        str::Free(idx->pages[i].compact);
        str::Free(idx->pages[i].headingCompact);
        idx->pages[i].compact = nullptr;
        idx->pages[i].headingCompact = nullptr;
    }
    idx->pages.Reset();
    delete idx->df;
    idx->df = nullptr;
    idx->nDocs = 0;
    idx->avgdl = 1;
}

static void TocCalibBm25AddPage(TocCalibBm25Index* idx, int page, const char* compact, float maxY, float avgFont,
                                const char* headingCompact = nullptr) {
    if (!idx || !compact || page < 1) {
        return;
    }
    if (!idx->df) {
        idx->df = new dict::MapStrToInt(4096);
    }
    TocCalibBm25Page pg;
    pg.page = page;
    pg.compact = str::Dup(compact);
    pg.headingCompact = headingCompact && headingCompact[0] ? str::Dup(headingCompact) : nullptr;
    pg.maxY = maxY > 1 ? maxY : 1;
    pg.avgFont = avgFont > 1 ? avgFont : 12;
    int cps[1024];
    int n = TocCalibBm25CollectGlyphs(compact, cps, dimof(cps));
    dict::MapStrToInt seen(256);
    char key[12]{};
    for (int i = 0; i + 1 < n; i++) {
        TocCalibBm25Key(cps[i], cps[i + 1], key, (int)sizeof(key));
        pg.nTokens++;
        int dummy = 0;
        if (seen.Insert(key, 1, &dummy)) {
            TocCalibBm25AddDf(idx->df, key);
        }
    }
    idx->pages.Append(pg);
    idx->nDocs = idx->pages.Size();
    float sum = 0;
    for (int i = 0; i < idx->pages.Size(); i++) {
        sum += (float)idx->pages[i].nTokens;
    }
    idx->avgdl = idx->nDocs > 0 ? sum / (float)idx->nDocs : 1;
    if (idx->avgdl < 1) {
        idx->avgdl = 1;
    }
}

static bool TocCalibBm25Build(TocCalibSession* s, Vec<int>& pages, Vec<Vec<EngineMupdfPageLine>*>& cache,
                              TocCalibBm25Index* idx) {
    if (!s || !idx || s->nPages < 1) {
        return false;
    }
    TocCalibBm25Free(idx);
    for (int p = 1; p <= s->nPages; p++) {
        if (TocCalibPageInToc(s, p)) {
            continue;
        }
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (!lines || lines->Size() < 1) {
            continue;
        }
        char compact[4096];
        compact[0] = 0;
        int used = 0;
        float maxY = 1;
        float fontSum = 0;
        int fontN = 0;
        for (int i = 0; i < lines->Size(); i++) {
            const EngineMupdfPageLine& ln = lines->At(i);
            if (ln.y + ln.dy > maxY) {
                maxY = ln.y + ln.dy;
            }
            float fs = ln.fontSize > 0 ? ln.fontSize : ln.dy;
            if (fs > 1) {
                fontSum += fs;
                fontN++;
            }
            if (!ln.text || !ln.text[0]) {
                continue;
            }
            char piece[512];
            TocCalibCompact(ln.text, piece, (int)sizeof(piece));
            int add = (int)str::Len(piece);
            if (add < 1 || used + add >= (int)sizeof(compact) - 1) {
                continue;
            }
            memcpy(compact + used, piece, (size_t)add);
            used += add;
            compact[used] = 0;
        }
        if (used < 2) {
            continue;
        }
        float avgFont = fontN > 0 ? fontSum / (float)fontN : 12;
        char heading[2048];
        heading[0] = 0;
        int hused = 0;
        for (int i = 0; i < lines->Size(); i++) {
            const EngineMupdfPageLine& ln = lines->At(i);
            if (!ln.text || !ln.text[0]) {
                continue;
            }
            float fs = ln.fontSize > 0 ? ln.fontSize : ln.dy;
            bool top = maxY > 1 && ln.y < maxY * 0.28f;
            bool large = avgFont > 1 && fs > avgFont * 1.12f;
            if (!top && !large) {
                continue;
            }
            char piece[512];
            TocCalibCompact(ln.text, piece, (int)sizeof(piece));
            int add = (int)str::Len(piece);
            if (add < 1 || hused + add >= (int)sizeof(heading) - 1) {
                continue;
            }
            memcpy(heading + hused, piece, (size_t)add);
            hused += add;
            heading[hused] = 0;
        }
        TocCalibBm25AddPage(idx, p, compact, maxY, avgFont, heading);
    }
    return idx->nDocs > 0;
}

static bool TocCalibBm25LocateHit(const Vec<EngineMupdfPageLine>* lines, const char* title, float maxY, float avgFont,
                                  float* xOut, float* yOut, float* fontOut) {
    if (!lines || !title || !xOut || !yOut) {
        return false;
    }
    int bestRank = -1;
    int best = -1;
    for (int i = 0; i < lines->Size(); i++) {
        const EngineMupdfPageLine& ln = lines->At(i);
        if (!ln.text || !ln.text[0]) {
            continue;
        }
        int sc = TocCalibTitleMatchScore(ln.text, title);
        if (sc < 1) {
            continue;
        }
        int rank = sc * 10;
        float fs = ln.fontSize > 0 ? ln.fontSize : ln.dy;
        if (maxY > 1 && ln.y < maxY * 0.28f) {
            rank += 8;
        }
        if (avgFont > 1 && fs > avgFont * 1.12f) {
            rank += 8;
        }
        if (rank > bestRank) {
            bestRank = rank;
            best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < lines->Size(); i++) {
            const EngineMupdfPageLine& ln = lines->At(i);
            if (ln.text && ln.text[0]) {
                best = i;
                break;
            }
        }
    }
    if (best < 0) {
        return false;
    }
    const EngineMupdfPageLine& ln = lines->At(best);
    *xOut = ln.x;
    *yOut = ln.y;
    if (fontOut) {
        *fontOut = ln.fontSize > 0 ? ln.fontSize : ln.dy;
    }
    return true;
}

static bool TocCalibBm25Search(const TocCalibBm25Index* idx, const char* title, TocCalibNearHit* out) {
    if (!idx || !idx->df || !title || !out || idx->nDocs < 1) {
        return false;
    }
    int tg = TocCalibGlyphCount(title);
    if (tg < 4) {
        return false;
    }
    char tbuf[512];
    TocCalibCompact(title, tbuf, (int)sizeof(tbuf));
    int qcps[128];
    int qn = TocCalibBm25CollectGlyphs(tbuf, qcps, dimof(qcps));
    if (qn < 4) {
        return false;
    }
    const float k1 = 1.5f;
    const float b = 0.75f;
    float best = -1;
    float second = -1;
    int bestPage = 0;
    for (int i = 0; i < idx->pages.Size(); i++) {
        const TocCalibBm25Page& pg = idx->pages[i];
        if (!pg.compact || pg.nTokens < 1) {
            continue;
        }
        float score = 0;
        int matched = 0;
        char key[12]{};
        for (int t = 0; t + 1 < qn; t++) {
            TocCalibBm25Key(qcps[t], qcps[t + 1], key, (int)sizeof(key));
            int tf = TocCalibBm25Tf(pg.compact, qcps[t], qcps[t + 1]);
            if (tf < 1) {
                continue;
            }
            matched++;
            int dfv = 0;
            if (!idx->df->Get(key, &dfv) || dfv < 1) {
                dfv = 1;
            }
            float idf = (float)log(((double)idx->nDocs - dfv + 0.5) / (dfv + 0.5) + 1.0);
            if (idf < 0) {
                idf = 0;
            }
            float dl = (float)pg.nTokens;
            float tfNorm = (tf * (k1 + 1.f)) / (tf + k1 * (1.f - b + b * dl / idx->avgdl));
            score += idf * tfNorm;
        }
        int qTerms = qn - 1;
        if (matched < 3 && matched * 2 < qTerms) {
            continue;
        }
        if (tbuf[0] && str::Find(pg.compact, tbuf)) {
            score += 6.f;
        }
        if (pg.headingCompact && tbuf[0]) {
            if (str::Find(pg.headingCompact, tbuf)) {
                score += 4.f;
            } else {
                int hm = 0;
                for (int t = 0; t + 1 < qn; t++) {
                    if (TocCalibBm25Tf(pg.headingCompact, qcps[t], qcps[t + 1]) > 0) {
                        hm++;
                    }
                }
                if (hm >= 3) {
                    score += 2.f;
                }
            }
        }
        if (score > best) {
            second = best;
            best = score;
            bestPage = pg.page;
        } else if (score > second) {
            second = score;
        }
    }
    if (bestPage < 1 || best < 2.2f) {
        return false;
    }
    if (second > 0 && best < second * 1.28f + 0.8f) {
        return false;
    }
    out->page = bestPage;
    out->score = tg;
    out->x = 0;
    out->y = 0;
    out->fontSize = 0;
    return true;
}

static bool TocCalibSearchBm25(TocCalibSession* s, const char* title, Vec<int>& pages,
                               Vec<Vec<EngineMupdfPageLine>*>& cache, TocCalibBm25Index* idx, TocCalibNearHit* out) {
    if (!s || !title || !out) {
        return false;
    }
    if ((!idx->df || idx->nDocs < 1) && !TocCalibBm25Build(s, pages, cache, idx)) {
        return false;
    }
    if (!TocCalibBm25Search(idx, title, out)) {
        return false;
    }
    const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, out->page, pages, cache);
    float x = 0;
    float y = 0;
    float font = 0;
    float avg = 12;
    float maxY = 1;
    for (int i = 0; i < idx->pages.Size(); i++) {
        if (idx->pages[i].page == out->page) {
            avg = idx->pages[i].avgFont;
            maxY = idx->pages[i].maxY;
            break;
        }
    }
    if (TocCalibBm25LocateHit(lines, title, maxY, avg, &x, &y, &font)) {
        out->x = x;
        out->y = y;
        out->fontSize = font;
        if (maxY > 1 && y < maxY * 0.28f) {
            out->score += 2;
        }
        if (avg > 1 && font > avg * 1.12f) {
            out->score += 2;
        }
    }
    return true;
}

static void TocCalibFreePageCache(Vec<Vec<EngineMupdfPageLine>*>& cache) {
    for (int i = 0; i < cache.Size(); i++) {
        if (!cache[i]) {
            continue;
        }
        EngineMupdfFreePageLines(*cache[i]);
        delete cache[i];
        cache[i] = nullptr;
    }
    cache.Reset();
}

static bool TocCalibSearchNearPage(TocCalibSession* s, const char* title, int predPage, int radius, Vec<int>& pages,
                                   Vec<Vec<EngineMupdfPageLine>*>& cache, TocCalibNearHit* out) {
    if (!s || !title || !title[0] || predPage < 1 || !out) {
        return false;
    }
    Vec<TocCalibNearHit> hits;
    int lo = predPage - radius;
    int hi = predPage + radius;
    if (lo < 1) {
        lo = 1;
    }
    if (s->nPages > 0 && hi > s->nPages) {
        hi = s->nPages;
    }
    for (int p = lo; p <= hi; p++) {
        if (TocCalibPageInToc(s, p)) {
            continue;
        }
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (!lines) {
            continue;
        }
        int minScore = TocCalibGlyphCount(title) < 4 ? TocCalibGlyphCount(title) : 4;
        if (minScore < 2) {
            minScore = 2;
        }
        for (int i = 0; i < lines->Size(); i++) {
            const EngineMupdfPageLine& ln = lines->At(i);
            char joined[768];
            joined[0] = 0;
            int used = 0;
            float y0 = ln.y;
            float dy0 = ln.dy > 2 ? ln.dy : 12;
            for (int k = 0; k < 3 && i + k < lines->Size(); k++) {
                const EngineMupdfPageLine& part = lines->At(i + k);
                if (!part.text || !part.text[0]) {
                    break;
                }
                if (k > 0) {
                    float gap = part.y - y0;
                    if (gap < 0) {
                        gap = -gap;
                    }
                    if (gap > dy0 * 3.2f) {
                        break;
                    }
                }
                int add = (int)str::Len(part.text);
                if (used + add + 1 >= (int)sizeof(joined)) {
                    break;
                }
                memcpy(joined + used, part.text, (size_t)add);
                used += add;
                joined[used] = 0;
                y0 = part.y;
                if (part.dy > 2) {
                    dy0 = part.dy;
                }
                int sc = TocCalibTitleMatchScore(joined, title);
                if (sc < minScore) {
                    continue;
                }
                TocCalibNearHit h;
                h.page = p;
                h.score = sc;
                h.x = ln.x;
                h.y = ln.y;
                h.fontSize = ln.fontSize > 0 ? ln.fontSize : ln.dy;
                hits.Append(h);
                break;
            }
        }
    }
    if (hits.Size() < 1) {
        return false;
    }
    int bestScore = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].score > bestScore) {
            bestScore = hits[i].score;
        }
    }
    int nPagesHit = 0;
    int lastPage = 0;
    float maxFont = 0;
    int maxFontPage = 0;
    int nMaxFontPages = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].score < bestScore) {
            continue;
        }
        if (hits[i].page != lastPage) {
            nPagesHit++;
            lastPage = hits[i].page;
        }
        if (hits[i].fontSize > maxFont + 0.4f) {
            maxFont = hits[i].fontSize;
            maxFontPage = hits[i].page;
            nMaxFontPages = 1;
        } else if (hits[i].fontSize >= maxFont - 0.4f && hits[i].page != maxFontPage) {
            nMaxFontPages++;
        }
    }
    if (nPagesHit >= 3 && nMaxFontPages != 1) {
        return false;
    }
    int best = -1;
    int bestDist = 9999;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].score < bestScore) {
            continue;
        }
        if (nPagesHit >= 3 && hits[i].page != maxFontPage) {
            continue;
        }
        int d = hits[i].page - predPage;
        if (d < 0) {
            d = -d;
        }
        if (hits[i].page == predPage) {
            d = -1;
        }
        if (best < 0 || d < bestDist || (d == bestDist && hits[i].fontSize > hits[best].fontSize)) {
            best = i;
            bestDist = d;
        }
    }
    if (best < 0) {
        return false;
    }
    *out = hits[best];
    return true;
}

void TocCalibVerifyNearPredicted(TocCalibSession* s) {
    if (!s || !s->engine) {
        return;
    }
    TocCalibEnsureTocRange(s);
    TocCalibClearDestsOnTocPages(s);
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    TocCalibBm25Index bm25;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        if (s->rows[i].pdfPinned || s->rows[i].userSet) {
            continue;
        }
        const char* title = it->rawTitle && it->rawTitle[0] ? it->rawTitle : it->title;
        if (!title || TocCalibGlyphCount(title) < 2) {
            continue;
        }
        int pred = it->pageNo;
        if (pred < 1 && TocCalibHasPrinted(it->printedPage) && s->map.confidence > 0) {
            pred = it->printedPage + s->map.offset;
            if (pred < 1) {
                pred = 1;
            }
        }
        if (it->bodyMatched && it->pageNo > 0 && pred > 0) {
            int d = it->pageNo - pred;
            if (d < 0) {
                d = -d;
            }
            if (d <= 2) {
                if (it->confidence < 80) {
                    it->confidence = 80;
                }
                continue;
            }
        }
        TocCalibNearHit hit;
        bool found = false;
        if (pred > 0) {
            found = TocCalibSearchNearPage(s, title, pred, 2, pages, cache, &hit);
            if (!found && TocCalibGlyphCount(title) >= 6) {
                found = TocCalibSearchNearPage(s, title, pred, 4, pages, cache, &hit);
            }
        }
        if (found && TocCalibPageInToc(s, hit.page)) {
            found = false;
        }
        if (found) {
            TocCalibApplyNearHit(it, hit.page, hit.x, hit.y, hit.score, pred);
            s->rows[i].identPageNo = it->pageNo;
            continue;
        }
        if (TocCalibGlyphCount(title) >= 4 && TocCalibSearchBm25(s, title, pages, cache, &bm25, &hit) &&
            !TocCalibPageInToc(s, hit.page)) {
            if (TocCalibApplyNearHit(it, hit.page, hit.x, hit.y, hit.score, 0)) {
                s->rows[i].identPageNo = it->pageNo;
                if (it->confidence < 75) {
                    it->confidence = 75;
                }
            }
        } else if (TocCalibGlyphCount(title) >= 4 && TocCalibSearchTextFindFallback(s, &s->rows[i], title, &hit) &&
                   !TocCalibPageInToc(s, hit.page) && TocCalibApplyNearHit(it, hit.page, hit.x, hit.y, hit.score, 0)) {
            s->rows[i].identPageNo = it->pageNo;
            if (it->confidence < 75) {
                it->confidence = 75;
            }
        } else if (!it->bodyMatched && it->confidence > 60) {
            it->confidence = 60;
        }
    }
    TocCalibBm25Free(&bm25);
    TocCalibFreePageCache(cache);
}

static void TocCalibWriteDebugIfCli(const TocCalibSession* s) {
    if (!gCli || !gCli->extractTocDebug || !s || !s->engine || !s->engine->FilePath()) {
        return;
    }
    char* path = str::Join(path::GetPathNoExtTemp(s->engine->FilePath()), ".book-toc-debug.txt");
    TocCalibWriteDebug(s, path);
    str::Free(path);
}

static void TocCalibCollectRows(TocCalibSession* s);
static void TocCalibMarkConfirm(TocCalibSession* s);

void TocCalibWriteDebug(const TocCalibSession* s, const char* path) {
    if (!s || !path) {
        return;
    }
    FILE* f = fopen(path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "\n===== After calibration verify =====\n");
    fprintf(f, "offset: %d  confidence: %.2f  toc: %d-%d  rows: %d\n\n", s->map.offset, s->map.confidence, s->tocPage,
            s->tocEnd, s->rows.Size());
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        const char* title = it->rawTitle && it->rawTitle[0] ? it->rawTitle : (it->title ? it->title : "");
        fprintf(f, "%d. %s\n", i + 1, title);
        fprintf(f, "printed %d -> pdf %d (offset %d)\n", it->printedPage, it->pageNo,
                it->printedPage > 0 && it->pageNo > 0 ? it->pageNo - it->printedPage : 0);
        fprintf(f, "Body matched: %s\n", it->bodyMatched ? "YES" : "NO");
        fprintf(f, "Verified: %s\n", it->verified ? "YES" : "NO");
        fprintf(f, "confidence: %d\n", it->confidence);
        fprintf(f, "needsConfirm: %s\n\n", s->rows[i].needsConfirm ? "YES" : "NO");
    }
    fclose(f);
}

static int TocCalibDigitVal(int cp) {
    if (cp >= '0' && cp <= '9') {
        return cp - '0';
    }
    if (cp >= 0xFF10 && cp <= 0xFF19) {
        return cp - 0xFF10;
    }
    return -1;
}

static bool TocCalibHasLetterOrCjk(const char* s) {
    if (!s) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF)) {
            return true;
        }
    }
    return false;
}

static bool TocCalibIsLoosePagePunct(int cp) {
    return cp <= 32 || cp == 0x3000 || cp == '.' || cp == '*' || cp == '-' || cp == 0x00B7 || cp == 0x2022 ||
           cp == 0x2026 || cp == 0x3002 || cp == 0x30FB || cp == 0xFF0E || cp == 0x2013 || cp == 0x2014;
}

// Footer/header like "28", "· 28 ·", "1 0。" — digits plus leaders only.
static int TocCalibParseLoosePageNum(const char* s) {
    if (!s || !s[0] || TocCalibHasLetterOrCjk(s)) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    int page = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        int d = TocCalibDigitVal(cp);
        if (d >= 0) {
            page = page * 10 + d;
            n++;
            if (n > 3) {
                return 0;
            }
            continue;
        }
        if (!TocCalibIsLoosePagePunct(cp)) {
            return 0;
        }
    }
    if (n < 1 || page < 1 || page > 400) {
        return 0;
    }
    return page;
}

static int TocCalibFooterPrintedOnPage(EngineBase* engine, int pdf) {
    if (!engine || pdf < 1) {
        return 0;
    }
    Vec<EngineMupdfPageLine> lines;
    if (!EngineMupdfCollectPageLines(engine, pdf, lines)) {
        return 0;
    }
    float pageH = 0;
    for (int i = 0; i < lines.Size(); i++) {
        float b = lines[i].y + (lines[i].dy > 1 ? lines[i].dy : 10);
        if (b > pageH) {
            pageH = b;
        }
    }
    int footer = 0;
    int header = 0;
    for (int i = 0; i < lines.Size(); i++) {
        int pr = TocCalibParseLoosePageNum(lines[i].text);
        if (pr < 1) {
            continue;
        }
        float y = lines[i].y;
        if (pageH > 40 && y >= pageH * 0.78f) {
            footer = pr;
        } else if (pageH > 40 && y <= pageH * 0.12f) {
            header = pr;
        }
    }
    EngineMupdfFreePageLines(lines);
    int pr = footer > 0 ? footer : header;
    if (pr < 1 || pr == pdf) {
        return 0;
    }
    return pr;
}

// Keep TOC-extracted printed pages. For empty body hits, use the dest page
// label or the number printed in the header/footer (e.g. 28 on PDF 51).
static void TocCalibSeedPrintedFromPages(TocCalibSession* s) {
    if (!s || !s->engine) {
        return;
    }
    Vec<int> seenPdf;
    Vec<int> seenPr;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || TocCalibHasPrinted(it->printedPage)) {
            continue;
        }
        if (TocCalibNoPrintedTitle(it->title) || TocCalibNoPrintedTitle(it->rawTitle)) {
            continue;
        }
        int pdf = TocCalibRowPdf(&s->rows[i]);
        if (pdf < 1) {
            continue;
        }
        int lab = TocCalibLabelPrinted(s, pdf);
        if (TocCalibHasPrinted(lab)) {
            it->printedPage = lab;
            continue;
        }
        int pr = 0;
        bool found = false;
        for (int k = 0; k < seenPdf.Size(); k++) {
            if (seenPdf[k] == pdf) {
                pr = seenPr[k];
                found = true;
                break;
            }
        }
        if (!found) {
            pr = TocCalibFooterPrintedOnPage(s->engine, pdf);
            seenPdf.Append(pdf);
            seenPr.Append(pr);
        }
        if (TocCalibHasPrinted(pr)) {
            it->printedPage = pr;
        }
    }
}

static void TocCalibPrepareMapping(TocCalibSession* s, bool markConfirm) {
    if (!s) {
        return;
    }
    TocCalibCollectRows(s);
    TocCalibEnsureTocRange(s);
    TocCalibClearDestsOnTocPages(s);
    TocCalibSeedPrintedFromPages(s);
    s->editPdf = true;
    for (int i = 0; i < s->rows.Size(); i++) {
        if (s->rows[i].item && TocCalibHasPrinted(s->rows[i].item->printedPage)) {
            s->editPdf = false;
            break;
        }
    }
    TocCalibSolveSession(s);
    TocCalibVerifyNearPredicted(s);
    TocCalibSolveSession(s);
    if (markConfirm) {
        TocCalibMarkConfirm(s);
    }
    TocCalibWriteDebugIfCli(s);
}

void TocCalibRefineExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine) {
    TocCalibSession s;
    s.engine = engine;
    s.nPages = engine ? engine->PageCount() : 0;
    s.persistToDisk = false;
    for (int i = 0; i < roots.Size(); i++) {
        s.roots.Append(roots[i]);
    }
    TocCalibPrepareMapping(&s, false);
    s.roots.Reset();
    s.rows.Reset();
}

static bool TocCalibIsContentsTitle(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    return str::Eq(s, "目录") || str::Eq(s, "目次") || str::EqI(s, "Contents");
}

static void TocCalibClearDestsOnTocPages(TocCalibSession* s) {
    if (!s) {
        return;
    }
    TocCalibEnsureTocRange(s);
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || TocCalibIsContentsTitle(it->title)) {
            continue;
        }
        if (it->pageNo > 0 && TocCalibPageInToc(s, it->pageNo)) {
            it->pageNo = 0;
            it->bodyMatched = false;
            it->verified = false;
            s->rows[i].identPageNo = 0;
        }
    }
}

static constexpr int kTocCalibUndoMax = 40;

struct TocCalibUndoSnap {
    Vec<ExtractedTocItem*> roots;
    Vec<ExtractedTocItem*> extras;
    Vec<TocCalibRow> rows;
    PageMappingSegment map;
    bool offsetLocked = false;
    bool editPdf = false;
};

static ExtractedTocItem* TocCalibCloneExtracted(const ExtractedTocItem* src) {
    if (!src) {
        return nullptr;
    }
    auto* n = new ExtractedTocItem();
    n->title = str::Dup(src->title);
    n->rawTitle = str::Dup(src->rawTitle);
    n->pageNo = src->pageNo;
    n->x = src->x;
    n->y = src->y;
    n->level = src->level;
    n->confidence = src->confidence;
    n->source = src->source;
    n->printedPage = src->printedPage;
    n->printedLabel = str::Dup(src->printedLabel);
    n->tocPageNo = src->tocPageNo;
    n->tocX = src->tocX;
    n->tocY = src->tocY;
    n->verified = src->verified;
    n->bodyMatched = src->bodyMatched;
    n->expanded = src->expanded;
    n->treeHandle = src->treeHandle;
    for (int i = 0; i < src->children.Size(); i++) {
        ExtractedTocItem* ch = TocCalibCloneExtracted(src->children[i]);
        if (ch) {
            ch->parent = n;
            n->children.Append(ch);
        }
    }
    return n;
}

static void TocCalibCloneForest(const Vec<ExtractedTocItem*>& src, Vec<ExtractedTocItem*>& dst) {
    dst.Reset();
    for (int i = 0; i < src.Size(); i++) {
        dst.Append(TocCalibCloneExtracted(src[i]));
    }
}

static void TocCalibFreeUndoSnap(TocCalibUndoSnap* snap) {
    if (!snap) {
        return;
    }
    DeleteExtractedTocItems(snap->roots);
    DeleteExtractedTocItems(snap->extras);
    delete snap;
}

static void TocCalibFreeUndoStack(Vec<TocCalibUndoSnap*>& stack) {
    for (int i = 0; i < stack.Size(); i++) {
        TocCalibFreeUndoSnap(stack[i]);
    }
    stack.Reset();
}

static TocCalibUndoSnap* TocCalibCaptureSnap(const TocCalibSession* s) {
    if (!s) {
        return nullptr;
    }
    auto* snap = new TocCalibUndoSnap();
    TocCalibCloneForest(s->roots, snap->roots);
    TocCalibCloneForest(s->extras, snap->extras);
    snap->map = s->map;
    snap->offsetLocked = s->offsetLocked;
    snap->editPdf = s->editPdf;
    Vec<ExtractedTocItem*> oldFlat;
    Vec<ExtractedTocItem*> newFlat;
    FlattenExtractedTocItems(s->roots, oldFlat);
    FlattenExtractedTocItems(s->extras, oldFlat);
    FlattenExtractedTocItems(snap->roots, newFlat);
    FlattenExtractedTocItems(snap->extras, newFlat);
    for (int i = 0; i < s->rows.Size(); i++) {
        TocCalibRow r = s->rows[i];
        r.item = nullptr;
        for (int j = 0; j < oldFlat.Size() && j < newFlat.Size(); j++) {
            if (oldFlat[j] == s->rows[i].item) {
                r.item = newFlat[j];
                break;
            }
        }
        snap->rows.Append(r);
    }
    return snap;
}

static void TocCalibRemember(TocCalibSession* s) {
    if (!s || s->undoBusy) {
        return;
    }
    TocCalibUndoSnap* snap = TocCalibCaptureSnap(s);
    if (!snap) {
        return;
    }
    s->undo.Append(snap);
    while (s->undo.Size() > kTocCalibUndoMax) {
        TocCalibFreeUndoSnap(s->undo[0]);
        s->undo.RemoveAt(0);
    }
    TocCalibFreeUndoStack(s->redo);
}

static void TocCalibCollectRows(TocCalibSession* s);

static void TocCalibInstallSnap(TocCalibSession* s, TocCalibUndoSnap* snap) {
    if (!s || !snap) {
        return;
    }
    DeleteExtractedTocItems(s->roots);
    DeleteExtractedTocItems(s->extras);
    s->roots.Reset();
    s->extras.Reset();
    for (int i = 0; i < snap->roots.Size(); i++) {
        s->roots.Append(snap->roots[i]);
    }
    for (int i = 0; i < snap->extras.Size(); i++) {
        s->extras.Append(snap->extras[i]);
    }
    snap->roots.Reset();
    snap->extras.Reset();
    s->rows.Reset();
    for (int i = 0; i < snap->rows.Size(); i++) {
        s->rows.Append(snap->rows[i]);
    }
    s->map = snap->map;
    s->offsetLocked = snap->offsetLocked;
    s->editPdf = snap->editPdf;
    TocCalibCollectRows(s);
}

void DeleteTocCalibSession(TocCalibSession* s) {
    if (!s) {
        return;
    }
    s->engine = nullptr;
    TocCalibFreeUndoStack(s->undo);
    TocCalibFreeUndoStack(s->redo);
    DeleteExtractedTocItems(s->roots);
    DeleteExtractedTocItems(s->extras);
    DeleteExtractedTocItems(s->backup);
    delete s;
}

static bool TocCalibInsertAfter(Vec<ExtractedTocItem*>& nodes, ExtractedTocItem* after, ExtractedTocItem* n) {
    if (!after || !n) {
        return false;
    }
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i] == after) {
            n->parent = after->parent;
            return nodes.InsertAt((size_t)i + 1, n);
        }
        if (nodes[i] && TocCalibInsertAfter(nodes[i]->children, after, n)) {
            return true;
        }
    }
    return false;
}

static bool TocCalibRemoveItem(Vec<ExtractedTocItem*>& nodes, ExtractedTocItem* n) {
    if (!n) {
        return false;
    }
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i] == n) {
            nodes.RemoveAt(i);
            return true;
        }
        if (nodes[i] && TocCalibRemoveItem(nodes[i]->children, n)) {
            return true;
        }
    }
    return false;
}

static void TocCalibLinkParents(Vec<ExtractedTocItem*>& nodes, ExtractedTocItem* parent) {
    for (int i = 0; i < nodes.Size(); i++) {
        ExtractedTocItem* it = nodes[i];
        if (!it) {
            continue;
        }
        it->parent = parent;
        TocCalibLinkParents(it->children, it);
    }
}

static bool TocCalibItemVisible(ExtractedTocItem* it) {
    if (!it) {
        return false;
    }
    if (TocCalibIsContentsTitle(it->title)) {
        return false;
    }
    if (!TocCalibHasPrinted(it->printedPage) && it->pageNo < 1) {
        return false;
    }
    return true;
}

static int TocCalibRowIndex(TocCalibSession* s, ExtractedTocItem* it) {
    if (!s || !it) {
        return -1;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        if (s->rows[i].item == it) {
            return i;
        }
    }
    return -1;
}

static bool TocCalibRestoreRowFlags(const Vec<TocCalibRow>& prev, ExtractedTocItem* it, TocCalibRow& row) {
    for (int i = 0; i < prev.Size(); i++) {
        if (prev[i].item == it) {
            row.userSet = prev[i].userSet;
            row.pdfPinned = prev[i].pdfPinned;
            row.colChosen = prev[i].colChosen;
            row.editPdf = prev[i].editPdf;
            row.needsConfirm = prev[i].needsConfirm;
            row.identPageNo = prev[i].identPageNo;
            row.origPageNo = prev[i].origPageNo;
            row.toc = prev[i].toc;
            return true;
        }
    }
    return false;
}

static void TocCalibCollectOne(TocCalibSession* s, ExtractedTocItem* it, int depth, const Vec<TocCalibRow>& prev) {
    if (!s || !it) {
        return;
    }
    if (it->tocPageNo > 0) {
        if (s->tocPage < 1 || it->tocPageNo < s->tocPage) {
            s->tocPage = it->tocPageNo;
        }
        if (it->tocPageNo > s->tocEnd) {
            s->tocEnd = it->tocPageNo;
        }
    }
    bool skipRow = !TocCalibItemVisible(it);
    if (TocCalibIsContentsTitle(it->title) && it->pageNo > 0) {
        if (s->tocPage < 1 || it->pageNo < s->tocPage) {
            s->tocPage = it->pageNo;
        }
        if (it->pageNo > s->tocEnd) {
            s->tocEnd = it->pageNo;
        }
    }
    if (it->title) {
        int fromTitle = StripBookPrintedPageFromTitle(it->title);
        if (!TocCalibHasPrinted(it->printedPage) && fromTitle > 0) {
            it->printedPage = fromTitle;
        }
    }
    if (it->rawTitle) {
        StripBookPrintedPageFromTitle(it->rawTitle);
    }
    if (!skipRow) {
        TocCalibRow row;
        row.item = it;
        int show = depth < 1 ? 1 : depth;
        if (it->level > show) {
            show = it->level;
        }
        if (show > 6) {
            show = 6;
        }
        row.depth = show;
        if (!TocCalibRestoreRowFlags(prev, it, row)) {
            row.identPageNo = it->pageNo;
            row.origPageNo = it->pageNo;
        }
        if (row.identPageNo < 1 && it->pageNo > 0) {
            row.identPageNo = it->pageNo;
        }
        if (row.origPageNo < 1 && it->pageNo > 0) {
            row.origPageNo = it->pageNo;
        }
        s->rows.Append(row);
    }
    int childDepth = skipRow ? depth : depth + 1;
    for (int i = 0; i < it->children.Size(); i++) {
        TocCalibCollectOne(s, it->children[i], childDepth, prev);
    }
}

static void TocCalibCollectRows(TocCalibSession* s) {
    Vec<TocCalibRow> prev;
    for (int i = 0; i < s->rows.Size(); i++) {
        prev.Append(s->rows[i]);
    }
    s->rows.Reset();
    s->tocPage = 0;
    s->tocEnd = 0;
    TocCalibLinkParents(s->roots, nullptr);
    for (int i = 0; i < s->roots.Size(); i++) {
        TocCalibCollectOne(s, s->roots[i], 1, prev);
    }
}

static void TocCalibMarkConfirm(TocCalibSession* s) {
    int n = s->rows.Size();
    for (int i = 0; i < n; i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (s->rows[i].userSet) {
            s->rows[i].needsConfirm = false;
            if (it) {
                it->verified = true;
            }
            continue;
        }
        s->rows[i].needsConfirm = false;
        if (it && TocCalibHasPrinted(it->printedPage) && it->pageNo > 0) {
            it->verified = true;
        }
    }
}

static void TocCalibRowsToMap(const TocCalibSession* s, Vec<TocCalibMapRow>& mapRows) {
    mapRows.Reset();
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        TocCalibMapRow r;
        r.printedPage = it->printedPage;
        r.pdfPage = it->pageNo;
        r.identPage = s->rows[i].identPageNo > 0 ? s->rows[i].identPageNo : it->pageNo;
        r.bodyMatched = it->bodyMatched;
        r.verified = it->verified;
        r.pdfPinned = s->rows[i].pdfPinned;
        mapRows.Append(r);
    }
}

static int TocCalibPredPdf(int printed, int offset, int nPages) {
    int pred = printed + offset;
    if (pred < 1) {
        pred = 1;
    }
    if (nPages > 0 && pred > nPages) {
        pred = nPages;
    }
    return pred;
}

static bool TocCalibHaveOffset(const TocCalibSession* s) {
    return s && s->map.offset >= 0 && (s->map.confidence > 0 || s->offsetLocked);
}

static int TocCalibRowPdf(const TocCalibRow* row) {
    if (!row) {
        return 0;
    }
    if (row->identPageNo > 0) {
        return row->identPageNo;
    }
    if (row->item && row->item->pageNo > 0) {
        return row->item->pageNo;
    }
    if (row->origPageNo > 0) {
        return row->origPageNo;
    }
    return 0;
}

static int TocCalibParseLabelPrinted(const char* label) {
    if (!label || !label[0]) {
        return 0;
    }
    const char* p = label;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    bool neg = *p == '-';
    if (neg) {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return 0;
    }
    int n = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        n = n * 10 + (*p - '0');
        if (n > 99999) {
            return 0;
        }
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p) {
        return 0;
    }
    return neg ? -n : n;
}

static int TocCalibRomanValue(char c) {
    switch ((char)toupper((unsigned char)c)) {
        case 'I':
            return 1;
        case 'V':
            return 5;
        case 'X':
            return 10;
        case 'L':
            return 50;
        case 'C':
            return 100;
        case 'D':
            return 500;
        case 'M':
            return 1000;
        default:
            return 0;
    }
}

static int TocCalibParseRoman(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int n = 0;
    int prev = 0;
    for (int i = (int)str::Len(s) - 1; i >= 0; i--) {
        int v = TocCalibRomanValue(s[i]);
        if (v < 1) {
            return 0;
        }
        if (v < prev) {
            n -= v;
        } else {
            n += v;
        }
        prev = v;
    }
    if (n < 1 || n > 3999) {
        return 0;
    }
    return n;
}

static bool TocCalibIsAlphaToken(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int n = 0;
    for (const char* p = s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            n++;
            if (n > 6) {
                return false;
            }
            continue;
        }
        return false;
    }
    return n > 0;
}

bool TocCalibParsePrintedText(const char* s, int* printedOut, char** labelOut) {
    if (printedOut) {
        *printedOut = 0;
    }
    if (labelOut) {
        *labelOut = nullptr;
    }
    if (!s) {
        return true;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return true;
    }
    const char* end = s + str::Len(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    int len = (int)(end - s);
    if (len < 1) {
        return true;
    }
    char buf[32]{};
    if (len >= (int)sizeof(buf)) {
        return false;
    }
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;

    int n = TocCalibParseLabelPrinted(buf);
    if (n < 0) {
        return false;
    }
    if (n != 0 || (buf[0] == '0' && !buf[1])) {
        if (printedOut) {
            *printedOut = n;
        }
        return true;
    }
    if (!TocCalibIsAlphaToken(buf)) {
        return false;
    }
    if (labelOut) {
        *labelOut = str::Dup(buf);
    }
    return true;
}

bool TocCalibTestPrintedInput() {
    int pr = 99;
    char* lab = nullptr;
    bool ok = TocCalibParsePrintedText("", &pr, &lab) && pr == 0 && !lab;
    ok = ok && !TocCalibParsePrintedText("-2", &pr, &lab);
    ok = ok && TocCalibParsePrintedText("12", &pr, &lab) && pr == 12 && !lab;
    ok = ok && TocCalibParsePrintedText("II", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "II");
    str::Free(lab);
    lab = nullptr;
    ok = ok && TocCalibParsePrintedText("A", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "A");
    str::Free(lab);
    lab = nullptr;
    ok = ok && TocCalibParsePrintedText("xiv", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "xiv");
    str::Free(lab);
    ok = ok && !TocCalibParsePrintedText("12a", &pr, &lab);
    ok = ok && TocCalibParseRoman("II") == 2 && TocCalibParseRoman("xiv") == 14 && TocCalibParseRoman("A") == 0;
    return ok;
}

bool TocCalibTestBm25Locate() {
    const char* title = "过去也不是没有厌学的孩子";
    TocCalibBm25Index idx;
    TocCalibBm25AddPage(&idx, 12, "过去也不是没有厌学的孩子这一章讲课堂和成绩册", 200, 12, "过去也不是没有厌学的孩子");
    TocCalibBm25AddPage(&idx, 40, "成绩册成绩册成绩册厌学的孩子厌学的孩子课堂课堂课堂课堂", 200, 12, nullptr);
    TocCalibBm25AddPage(&idx, 80, "完全无关的一章讲天气和旅行见闻", 200, 12, nullptr);
    TocCalibNearHit hit;
    bool ok = TocCalibBm25Search(&idx, title, &hit) && hit.page == 12;
    TocCalibNearHit shortHit;
    ok = ok && !TocCalibBm25Search(&idx, "目录", &shortHit);
    TocCalibBm25Free(&idx);

    TocCalibBm25Index tied;
    TocCalibBm25AddPage(&tied, 5, "过去也不是没有厌学的孩子正文重复", 200, 12, "过去也不是没有厌学的孩子");
    TocCalibBm25AddPage(&tied, 6, "过去也不是没有厌学的孩子正文重复", 200, 12, "过去也不是没有厌学的孩子");
    TocCalibNearHit tieHit;
    ok = ok && !TocCalibBm25Search(&tied, title, &tieHit);
    TocCalibBm25Free(&tied);
    return ok;
}

bool TocCalibTestFindQuery() {
    char buf[256];
    TocCalibCleanFindTitle("  过去也不是没有厌学的孩子......12  ", buf, (int)sizeof(buf));
    bool ok = str::Eq(buf, "过去也不是没有厌学的孩子");
    TocCalibCleanFindTitle("过去也不是没有厌学的孩子  12", buf, (int)sizeof(buf));
    ok = ok && str::Eq(buf, "过去也不是没有厌学的孩子");
    TocCalibCleanFindTitle("Chapter 12", buf, (int)sizeof(buf));
    ok = ok && str::Eq(buf, "Chapter 12");
    TocCalibCleanFindTitle("Chapter    12", buf, (int)sizeof(buf));
    ok = ok && str::Eq(buf, "Chapter");
    TocCalibCleanFindTitle("12", buf, (int)sizeof(buf));
    ok = ok && str::Eq(buf, "12");

    const char* title = "过去也不是没有厌学的孩子";
    char p12[64];
    char p8[64];
    char p6[64];
    ok = ok && TocCalibFindTitlePrefix(title, 12, p12, (int)sizeof(p12)) && str::Eq(p12, title);
    ok = ok && TocCalibFindTitlePrefix(title, 8, p8, (int)sizeof(p8)) && TocCalibGlyphCount(p8) == 8;
    ok = ok && TocCalibFindTitlePrefix(title, 6, p6, (int)sizeof(p6)) && TocCalibGlyphCount(p6) == 6;
    ok = ok && TocCalibFindTitlePrefix("Hello, World", 6, buf, (int)sizeof(buf)) && str::Eq(buf, "Hello, W");
    ok = ok && !TocCalibFindTitlePrefix(title, 20, buf, (int)sizeof(buf));

    TocCalibFindQueries q;
    ok = ok && TocCalibBuildFindQueries("过去也不是没有厌学的孩子......12", &q);
    ok = ok && q.n == 3;
    ok = ok && str::Eq(q.q[0], "过去也不是没有厌学的孩子");
    ok = ok && TocCalibGlyphCount(q.q[1]) == 8;
    ok = ok && TocCalibGlyphCount(q.q[2]) == 6;
    return ok;
}

bool TocCalibTestInterpolatePrinted() {
    TocCalibSession s;
    ExtractedTocItem a;
    ExtractedTocItem b;
    ExtractedTocItem c;
    a.title = str::Dup("末章资料");
    a.printedPage = 160;
    b.title = str::Dup("一种误解：家长最了解自己的孩子");
    b.printedPage = 0;
    c.title = str::Dup("高中生的生活目的");
    c.printedPage = 162;
    TocCalibRow ra;
    TocCalibRow rb;
    TocCalibRow rc;
    ra.item = &a;
    rb.item = &b;
    rc.item = &c;
    s.rows.Append(ra);
    s.rows.Append(rb);
    s.rows.Append(rc);
    TocCalibFillAllPrinted(&s);
    bool ok = b.printedPage == 161 && TocCalibGuessPrinted(&s, &s.rows[1]) == 161;
    ExtractedTocItem d;
    d.title = str::Dup("远");
    d.printedPage = 0;
    TocCalibRow rd;
    rd.item = &d;
    c.printedPage = 165;
    s.rows.Append(rd);
    d.printedPage = 0;
    b.printedPage = 0;
    TocCalibFillAllPrinted(&s);
    ok = ok && b.printedPage == 0 && d.printedPage == 0;
    str::Free(a.title);
    str::Free(b.title);
    str::Free(c.title);
    str::Free(d.title);
    a.title = nullptr;
    b.title = nullptr;
    c.title = nullptr;
    d.title = nullptr;
    return ok;
}

bool TocCalibTestPinOverwritesPrinted() {
    TocCalibSession s;
    s.map.offset = 3;
    s.map.confidence = 1;
    s.nPages = 139;
    ExtractedTocItem it;
    it.title = str::Dup("第二课");
    it.printedPage = 56;
    it.pageNo = 56;
    TocCalibRow row;
    row.item = &it;
    s.rows.Append(row);
    TocCalibPinRowToPage(&s, &s.rows[0], 11, 0, 0, nullptr);
    bool ok = it.pageNo == 11 && it.printedPage == 8 && s.rows[0].pdfPinned && s.rows[0].userSet;
    TocCalibSpinPrinted(&s.rows[0], -1, &s);
    ok = ok && it.printedPage == 7 && it.pageNo == 10 && !s.rows[0].pdfPinned;
    str::Free(it.title);
    it.title = nullptr;
    return ok;
}

bool TocCalibTestFrontMatterUsesLabel() {
    auto* pref = new ExtractedTocItem;
    pref->title = str::Dup("译者序");
    pref->rawTitle = str::Dup("译者序");
    pref->printedPage = 0;
    pref->pageNo = 17;
    pref->source = ExtractedTocSource::PrintedToc;
    auto* ch = new ExtractedTocItem;
    ch->title = str::Dup("第一章");
    ch->rawTitle = str::Dup("第一章");
    ch->printedPage = 1;
    ch->pageNo = 21;
    ch->bodyMatched = true;
    ch->verified = true;
    ch->source = ExtractedTocSource::PrintedToc;
    auto* ch2 = new ExtractedTocItem;
    ch2->title = str::Dup("第二章");
    ch2->rawTitle = str::Dup("第二章");
    ch2->printedPage = 15;
    ch2->pageNo = 35;
    ch2->bodyMatched = true;
    ch2->verified = true;
    ch2->source = ExtractedTocSource::PrintedToc;
    Vec<ExtractedTocItem*> roots;
    roots.Append(pref);
    roots.Append(ch);
    roots.Append(ch2);
    TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
    bool ok = sess && sess->map.offset == 20 && ch->pageNo == 21 && pref->pageNo == 17 && pref->printedPage == 0 &&
              TocCalibDisplayPrinted(sess, 0) == 0;
    ok = ok && TocCalibSetOffset(sess, 21) && pref->pageNo == 17 && ch->pageNo == 22 && ch2->pageNo == 36;
    pref->printedLabel = str::Dup("iii");
    ok = ok && TocCalibDisplayPrinted(sess, 0) == 0 && pref->printedLabel && str::Eq(pref->printedLabel, "iii");
    TocCalibPinRowToPage(sess, &sess->rows[0], 17, 0, 0, "iii");
    ok = ok && pref->pageNo == 17 && pref->printedPage == 0 && pref->printedLabel && str::Eq(pref->printedLabel, "iii");
    DeleteTocCalibSession(sess);
    return ok;
}

bool TocCalibTestClearTocDests() {
    TocCalibSession s;
    s.tocPage = 10;
    s.tocEnd = 12;
    ExtractedTocItem* toc = new ExtractedTocItem();
    toc->title = str::Dup("目录");
    toc->pageNo = 10;
    ExtractedTocItem* ch = new ExtractedTocItem();
    ch->title = str::Dup("2.报考超过自己实力的大学行吗？");
    ch->pageNo = 12;
    ch->printedPage = 77;
    ch->bodyMatched = true;
    TocCalibRow a;
    a.item = toc;
    TocCalibRow b;
    b.item = ch;
    b.identPageNo = 12;
    s.rows.Append(a);
    s.rows.Append(b);
    TocCalibClearDestsOnTocPages(&s);
    bool ok = toc->pageNo == 10 && ch->pageNo == 0 && !ch->bodyMatched && s.rows[1].identPageNo == 0;
    delete toc;
    delete ch;
    return ok;
}

static int TocCalibLabelPrinted(const TocCalibSession* s, int pdf) {
    if (!s || !s->engine || pdf < 1 || !s->engine->HasPageLabels()) {
        return 0;
    }
    const char* label = s->engine->GetPageLabeTemp(pdf);
    if (!label || !label[0]) {
        return 0;
    }
    int n = TocCalibParseLabelPrinted(label);
    if (n <= 0 || n == pdf) {
        return 0;
    }
    return n;
}

static const char* TocCalibPageLabelText(const TocCalibSession* s, int pdf) {
    if (!s || !s->engine || pdf < 1) {
        return nullptr;
    }
    const char* label = s->engine->GetPageLabeTemp(pdf);
    if (!label || !label[0] || TocCalibLabelIsPlainPdf(label, pdf)) {
        return nullptr;
    }
    return label;
}

static void TocCalibSeedPrintedLabel(TocCalibSession* s, ExtractedTocItem* it, int pdf) {
    if (!it || TocCalibHasPrinted(it->printedPage)) {
        return;
    }
    if (it->printedLabel && it->printedLabel[0]) {
        return;
    }
    const char* lab = TocCalibPageLabelText(s, pdf);
    if (lab && lab[0]) {
        str::ReplaceWithCopy(&it->printedLabel, lab);
    }
}

static bool TocCalibRowHasPrinted(const TocCalibRow* row) {
    return row && row->item && !TocCalibIsContentsTitle(row->item->title) && TocCalibHasPrinted(row->item->printedPage);
}

// One empty printed slot between 160 and 162 → 161. Only fill when the gap
// matches the number of empty rows so we do not invent 161–164 for 160…165.
static int TocCalibInterpolatePrinted(const TocCalibSession* s, int rowIdx) {
    if (!s || rowIdx < 0 || rowIdx >= s->rows.Size()) {
        return 0;
    }
    if (TocCalibRowHasPrinted(&s->rows[rowIdx])) {
        return s->rows[rowIdx].item->printedPage;
    }
    int prev = -1;
    int next = -1;
    for (int i = rowIdx - 1; i >= 0; i--) {
        if (TocCalibRowHasPrinted(&s->rows[i])) {
            prev = i;
            break;
        }
    }
    for (int i = rowIdx + 1; i < s->rows.Size(); i++) {
        if (TocCalibRowHasPrinted(&s->rows[i])) {
            next = i;
            break;
        }
    }
    if (prev < 0 || next < 0) {
        return 0;
    }
    int prevPr = s->rows[prev].item->printedPage;
    int nextPr = s->rows[next].item->printedPage;
    if (nextPr <= prevPr) {
        return 0;
    }
    int nEmpty = 0;
    int myEmpty = -1;
    for (int i = prev + 1; i < next; i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || TocCalibIsContentsTitle(it->title)) {
            continue;
        }
        if (TocCalibHasPrinted(it->printedPage)) {
            return 0;
        }
        if (i == rowIdx) {
            myEmpty = nEmpty;
        }
        nEmpty++;
    }
    if (myEmpty < 0 || nEmpty < 1 || nextPr - prevPr != nEmpty + 1) {
        return 0;
    }
    return prevPr + 1 + myEmpty;
}

static int TocCalibGuessPrinted(const TocCalibSession* s, const TocCalibRow* row, int fallbackPdf) {
    if (row && row->item && TocCalibHasPrinted(row->item->printedPage)) {
        return row->item->printedPage;
    }
    int pdf = TocCalibRowPdf(row);
    if (pdf < 1) {
        pdf = fallbackPdf;
    }
    int fromLabel = pdf > 0 ? TocCalibLabelPrinted(s, pdf) : 0;
    if (TocCalibHasPrinted(fromLabel)) {
        return fromLabel;
    }
    if (pdf > 0 && TocCalibHaveOffset(s)) {
        int g = pdf - s->map.offset;
        if (g > 0) {
            return g;
        }
    }
    if (s && row) {
        int idx = -1;
        for (int i = 0; i < s->rows.Size(); i++) {
            if (&s->rows[i] == row || s->rows[i].item == row->item) {
                idx = i;
                break;
            }
        }
        int gap = TocCalibInterpolatePrinted(s, idx);
        if (gap != 0) {
            return gap;
        }
    }
    return 0;
}

static void TocCalibFillAllPrinted(TocCalibSession* s) {
    if (!s) {
        return;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        if (it->printedPage < 0) {
            it->printedPage = 0;
        }
        if (TocCalibHasPrinted(it->printedPage)) {
            continue;
        }
        if (it->printedLabel && it->printedLabel[0]) {
            continue;
        }
        int g = TocCalibGuessPrinted(s, &s->rows[i]);
        if (g == 0) {
            g = TocCalibInterpolatePrinted(s, i);
        }
        if (g > 0) {
            it->printedPage = g;
            continue;
        }
        TocCalibSeedPrintedLabel(s, it, TocCalibRowPdf(&s->rows[i]));
    }
}

int TocCalibDisplayPrinted(const TocCalibSession* s, int rowIdx) {
    if (!s || rowIdx < 0 || rowIdx >= s->rows.Size()) {
        return 0;
    }
    return TocCalibGuessPrinted(s, &s->rows[rowIdx]);
}

static void TocCalibEnforceReadingOrder(TocCalibSession* s) {
    if (!s) {
        return;
    }
    int prevPr = 0;
    int prevPdf = 0;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || TocCalibIsContentsTitle(it->title)) {
            continue;
        }
        if (it->printedPage > 0) {
            if (prevPr > 0 && it->printedPage < prevPr) {
                if (!s->rows[i].userSet) {
                    it->printedPage = 0;
                    it->verified = false;
                    if (!s->rows[i].pdfPinned && s->rows[i].origPageNo > 0) {
                        it->pageNo = s->rows[i].origPageNo;
                    }
                }
                s->rows[i].needsConfirm = true;
            } else {
                prevPr = it->printedPage;
            }
        }
        if (s->rows[i].pdfPinned) {
            if (it->pageNo > 0) {
                if (prevPdf > 0 && it->pageNo < prevPdf) {
                    s->rows[i].needsConfirm = true;
                } else {
                    prevPdf = it->pageNo;
                }
            }
            continue;
        }
        int pdf = it->pageNo;
        if (pdf > 0 && prevPdf > 0 && pdf < prevPdf) {
            int pred = 0;
            if (it->printedPage > 0 && TocCalibHaveOffset(s)) {
                pred = TocCalibPredPdf(it->printedPage, s->map.offset, s->nPages);
            }
            it->pageNo = pred >= prevPdf ? pred : prevPdf;
            it->bodyMatched = false;
            s->rows[i].needsConfirm = true;
            pdf = it->pageNo;
        }
        if (pdf > 0) {
            prevPdf = pdf;
        }
    }
}

void TocCalibSolveSession(TocCalibSession* s) {
    if (!s) {
        return;
    }
    Vec<TocCalibMapRow> mapRows;
    TocCalibRowsToMap(s, mapRows);
    int offset = s->map.offset;
    if (s->offsetLocked && offset >= 0) {
        s->map.confidence = 1;
    } else {
        offset = TocCalibSolveOffset(mapRows);
        if (offset >= 0) {
            s->map.offset = offset;
            s->map.confidence = 0.9f;
        } else {
            s->map.confidence = 0;
        }
    }
    bool haveOffset = s->map.confidence > 0 && s->map.offset >= 0;
    TocCalibFillAllPrinted(s);
    int pMin = 0;
    int pMax = 0;
    bool havePrinted = false;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        if (!s->rows[i].pdfPinned) {
            if (TocCalibHasPrinted(it->printedPage) && haveOffset) {
                it->pageNo = TocCalibPredPdf(it->printedPage, s->map.offset, s->nPages);
            } else if (!TocCalibHasPrinted(it->printedPage) && s->rows[i].origPageNo > 0) {
                it->pageNo = s->rows[i].origPageNo;
            }
        }
        if (s->nPages > 0 && it->pageNo > s->nPages) {
            it->pageNo = s->nPages;
        }
        if (TocCalibHasPrinted(it->printedPage)) {
            if (!havePrinted || it->printedPage < pMin) {
                pMin = it->printedPage;
            }
            if (!havePrinted || it->printedPage > pMax) {
                pMax = it->printedPage;
            }
            havePrinted = true;
        }
    }
    if (havePrinted) {
        s->map.printedStart = pMin;
        s->map.printedEnd = pMax;
    }
    TocCalibEnforceReadingOrder(s);
}

bool TocCalibCommitPrinted(TocCalibSession* s, int rowIdx, int printed) {
    if (!s || rowIdx < 0 || rowIdx >= s->rows.Size()) {
        return false;
    }
    ExtractedTocItem* it = s->rows[rowIdx].item;
    if (!it) {
        return false;
    }
    if (printed < 0) {
        printed = 0;
    }
    it->printedPage = printed;
    it->verified = TocCalibHasPrinted(printed);
    s->rows[rowIdx].needsConfirm = false;
    s->rows[rowIdx].userSet = true;
    if (s->offsetLocked && TocCalibHasPrinted(printed)) {
        it->pageNo = TocCalibPredPdf(printed, s->map.offset, s->nPages);
        it->bodyMatched = false;
        TocCalibVerifyNearPredicted(s);
        return true;
    }
    TocCalibSolveSession(s);
    TocCalibVerifyNearPredicted(s);
    TocCalibSolveSession(s);
    return true;
}

bool TocCalibSetOffset(TocCalibSession* s, int offset) {
    if (!s || offset < 0) {
        return false;
    }
    s->map.offset = offset;
    s->map.confidence = 1;
    s->offsetLocked = true;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || !TocCalibHasPrinted(it->printedPage) || s->rows[i].pdfPinned) {
            continue;
        }
        it->pageNo = TocCalibPredPdf(it->printedPage, offset, s->nPages);
        it->verified = s->rows[i].userSet || it->verified;
    }
    TocCalibVerifyNearPredicted(s);
    TocCalibEnforceReadingOrder(s);
    return true;
}

bool TocCalibCommitRow(TocCalibSession* s, int rowIdx, int printed, int pdf, int offset, bool pinPdf) {
    if (!s || rowIdx < 0 || rowIdx >= s->rows.Size()) {
        return false;
    }
    ExtractedTocItem* it = s->rows[rowIdx].item;
    if (!it) {
        return false;
    }
    s->rows[rowIdx].pdfPinned = pinPdf;
    if (TocCalibHasPrinted(printed) || printed == 0) {
        it->printedPage = printed;
        it->verified = TocCalibHasPrinted(printed);
        s->rows[rowIdx].userSet = true;
        s->rows[rowIdx].needsConfirm = false;
    }
    if (pdf > 0) {
        it->pageNo = pdf;
        if (s->nPages > 0 && it->pageNo > s->nPages) {
            it->pageNo = s->nPages;
        }
        it->verified = true;
        if (pinPdf) {
            it->bodyMatched = false;
        }
    }
    if (!pinPdf && offset >= 0) {
        s->map.offset = offset;
        s->map.confidence = 1;
        s->offsetLocked = true;
        for (int i = 0; i < s->rows.Size(); i++) {
            ExtractedTocItem* row = s->rows[i].item;
            if (!row || !TocCalibHasPrinted(row->printedPage) || s->rows[i].pdfPinned) {
                continue;
            }
            row->pageNo = TocCalibPredPdf(row->printedPage, offset, s->nPages);
        }
    } else if (offset >= 0 && offset != s->map.offset) {
        s->map.offset = offset;
        s->map.confidence = 1;
        s->offsetLocked = true;
        for (int i = 0; i < s->rows.Size(); i++) {
            ExtractedTocItem* row = s->rows[i].item;
            if (!row || !TocCalibHasPrinted(row->printedPage) || s->rows[i].pdfPinned) {
                continue;
            }
            row->pageNo = TocCalibPredPdf(row->printedPage, offset, s->nPages);
        }
    }
    if (pinPdf && pdf > 0) {
        it->pageNo = pdf;
        if (s->nPages > 0 && it->pageNo > s->nPages) {
            it->pageNo = s->nPages;
        }
    }
    TocCalibVerifyNearPredicted(s);
    TocCalibEnforceReadingOrder(s);
    return true;
}

int TocCalibAddManualItem(TocCalibSession* s, const char* title, int pageNo, float x, float y, int afterRow,
                          bool asChild) {
    if (!s || !title || !title[0] || pageNo < 1) {
        return -1;
    }
    auto* n = new ExtractedTocItem();
    n->title = str::Dup(title);
    n->rawTitle = str::Dup(title);
    n->pageNo = pageNo;
    n->x = x;
    n->y = y;
    n->level = 1;
    if (afterRow >= 0 && afterRow < s->rows.Size() && s->rows[afterRow].item) {
        n->level = s->rows[afterRow].item->level;
        if (asChild) {
            n->level += 1;
        }
        if (n->level < 1) {
            n->level = 1;
        }
    }
    n->confidence = 100;
    n->source = ExtractedTocSource::Unknown;
    n->verified = true;
    n->bodyMatched = true;
    if (s->map.offset >= 0) {
        int pr = pageNo - s->map.offset;
        if (pr > 0) {
            n->printedPage = pr;
        }
    }

    ExtractedTocItem* after = nullptr;
    if (afterRow >= 0 && afterRow < s->rows.Size()) {
        after = s->rows[afterRow].item;
    }
    bool placed = false;
    if (asChild && after) {
        n->parent = after;
        after->children.InsertAt(0, n);
        placed = true;
    } else if (after) {
        placed = TocCalibInsertAfter(s->roots, after, n);
    }
    if (!placed) {
        n->parent = nullptr;
        s->roots.Append(n);
    }
    TocCalibLinkParents(s->roots, nullptr);

    TocCalibCollectRows(s);
    int idx = -1;
    for (int i = 0; i < s->rows.Size(); i++) {
        if (s->rows[i].item == n) {
            s->rows[i].userSet = true;
            s->rows[i].pdfPinned = true;
            s->rows[i].colChosen = true;
            s->rows[i].editPdf = true;
            s->rows[i].needsConfirm = false;
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        TocCalibRemoveItem(s->roots, n);
        delete n;
    }
    return idx;
}

static void TocCalibNormalizeItemTitles(ExtractedTocItem* n) {
    if (!n) {
        return;
    }
    NormalizeTocNumberingDotsHalfwidth(&n->title);
    NormalizeTocNumberingDotsHalfwidth(&n->rawTitle);
    for (int i = 0; i < n->children.Size(); i++) {
        TocCalibNormalizeItemTitles(n->children[i]);
    }
}

static void TocCalibNormalizeForestTitles(const Vec<ExtractedTocItem*>& roots) {
    for (int i = 0; i < roots.Size(); i++) {
        TocCalibNormalizeItemTitles(roots[i]);
    }
}

TocCalibSession* TocCalibSessionFromExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk) {
    auto* s = new TocCalibSession;
    s->engine = engine;
    s->persistToDisk = persistToDisk;
    s->nPages = engine ? engine->PageCount() : 0;
    for (int i = 0; i < roots.Size(); i++) {
        s->roots.Append(roots[i]);
    }
    roots.Reset();
    TocCalibNormalizeForestTitles(s->roots);
    TocCalibPrepareMapping(s, true);
    return s;
}

static void TocCalibCopyRef(TocCalibItemRef& dst, const Vec<int>& path) {
    dst.len = path.Size();
    if (dst.len > 16) {
        dst.len = 16;
    }
    for (int i = 0; i < dst.len; i++) {
        dst.idx[i] = path[i];
    }
}

static void TocCalibRefToVec(const TocCalibItemRef& src, Vec<int>& path) {
    path.Clear();
    for (int i = 0; i < src.len; i++) {
        path.Append(src.idx[i]);
    }
}

static TocItem* TocCalibItemAtPath(TocItem* first, const Vec<int>& path) {
    TocItem* item = first;
    for (int depth = 0; depth < path.Size(); depth++) {
        int idx = path[depth];
        for (int i = 0; item && i < idx; i++) {
            item = item->next;
        }
        if (!item) {
            return nullptr;
        }
        if (depth + 1 < path.Size()) {
            item = item->child;
        }
    }
    return item;
}

static TocItem* TocCalibFindById(TocItem* n, int id) {
    for (; n; n = n->next) {
        if (n->id == id) {
            return n;
        }
        TocItem* c = TocCalibFindById(n->child, id);
        if (c) {
            return c;
        }
    }
    return nullptr;
}

static bool TocCalibPathForItem(TocItem* dummyRoot, TocItem* item, Vec<int>& pathOut) {
    pathOut.Clear();
    if (!dummyRoot || !item) {
        return false;
    }
    Vec<int> reverse;
    for (TocItem* curr = item; curr && curr->parent; curr = curr->parent) {
        TocItem* sib = curr->parent->child;
        int idx = 0;
        while (sib && sib != curr) {
            idx++;
            sib = sib->next;
        }
        if (!sib) {
            return false;
        }
        reverse.Append(idx);
    }
    for (int i = reverse.Size() - 1; i >= 0; i--) {
        pathOut.Append(reverse[i]);
    }
    return pathOut.Size() > 0;
}

static TocCalibRow* TocCalibFindRowById(TocCalibSession* s, int id) {
    if (!s || id == 0) {
        return nullptr;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        if (s->rows[i].toc.tocId == id) {
            return &s->rows[i];
        }
    }
    return nullptr;
}

TocCalibRow* TocCalibRowForTocItem(MainWindow* win, TocItem* item) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !tab->tocCalib || !item) {
        return nullptr;
    }
    if (item->id) {
        TocCalibRow* row = TocCalibFindRowById(tab->tocCalib, item->id);
        if (row) {
            return row;
        }
    }
    return nullptr;
}

static ExtractedTocItem* TocCalibCloneTocItem(TocItem* t) {
    auto* n = new ExtractedTocItem();
    n->title = str::Dup(t && t->title ? t->title : "");
    n->rawTitle = str::Dup(n->title);
    n->pageNo = t && t->pageNo > 0 ? t->pageNo : 0;
    n->level = 1;
    if (t && t->dest) {
        char* uri = nullptr;
        EngineMupdfSnapshotOutlineLink(t->dest, &uri, nullptr, &n->x, &n->y);
        str::Free(uri);
    }
    if (t) {
        for (TocItem* c = t->child; c; c = c->next) {
            ExtractedTocItem* ch = TocCalibCloneTocItem(c);
            ch->parent = n;
            n->children.Append(ch);
        }
    }
    return n;
}

static void TocCalibCloneOutline(TocTree* toc, Vec<ExtractedTocItem*>& out) {
    if (!toc || !toc->root) {
        return;
    }
    for (TocItem* t = toc->root->child; t; t = t->next) {
        out.Append(TocCalibCloneTocItem(t));
    }
}

static void TocCalibBindPair(TocCalibSession* s, ExtractedTocItem* e, TocItem* t, Vec<int>& path) {
    if (e && t && t->id) {
        e->treeHandle = (void*)(intptr_t)t->id;
    }
    int idx = TocCalibRowIndex(s, e);
    if (idx >= 0 && t) {
        s->rows[idx].toc.tocId = t->id;
        TocCalibCopyRef(s->rows[idx].toc, path);
    }
    TocItem* c = t ? t->child : nullptr;
    for (int i = 0; i < (e ? e->children.Size() : 0); i++) {
        path.Append(i);
        TocCalibBindPair(s, e->children[i], c, path);
        path.RemoveLast();
        if (c) {
            c = c->next;
        }
    }
}

static ExtractedTocItem* TocCalibNewFromToc(TocItem* t) {
    auto* n = TocCalibCloneTocItem(t);
    if (n) {
        n->printedPage = 0;
        DeleteExtractedTocItems(n->children);
        n->children.Reset();
    }
    return n;
}

static void TocCalibAddMissingRows(TocCalibSession* s, TocItem* n, Vec<int>& path) {
    for (int i = 0; n; n = n->next, i++) {
        path.Append(i);
        if (n->id && !TocCalibFindRowById(s, n->id)) {
            ExtractedTocItem* e = TocCalibNewFromToc(n);
            if (e) {
                s->extras.Append(e);
                TocCalibRow row;
                row.item = e;
                row.pdfPinned = true;
                row.userSet = true;
                row.colChosen = true;
                row.editPdf = true;
                row.identPageNo = n->pageNo;
                row.origPageNo = n->pageNo;
                row.toc.tocId = n->id;
                TocCalibCopyRef(row.toc, path);
                s->rows.Append(row);
            }
        }
        if (n->child) {
            TocCalibAddMissingRows(s, n->child, path);
        }
        path.RemoveLast();
    }
}

void TocCalibRebind(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !tab->currToc || !tab->currToc->root) {
        return;
    }
    TocItem* first = tab->currToc->root->child;
    for (int i = 0; i < s->rows.Size(); i++) {
        Vec<int> path;
        TocCalibRefToVec(s->rows[i].toc, path);
        TocItem* item = nullptr;
        if (s->rows[i].toc.tocId) {
            item = TocCalibFindById(first, s->rows[i].toc.tocId);
        }
        if (!item && path.Size() > 0) {
            item = TocCalibItemAtPath(first, path);
        }
        if (item) {
            s->rows[i].toc.tocId = item->id;
            if (s->rows[i].item) {
                s->rows[i].item->treeHandle = (void*)(intptr_t)item->id;
            }
            Vec<int> fresh;
            if (TocCalibPathForItem(tab->currToc->root, item, fresh)) {
                TocCalibCopyRef(s->rows[i].toc, fresh);
            }
        } else {
            s->rows[i].toc.tocId = 0;
        }
    }
    Vec<int> walk;
    TocCalibAddMissingRows(s, first, walk);
}

static void TocCalibBindToTree(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !tab->currToc || !tab->currToc->root) {
        return;
    }
    TocItem* t = tab->currToc->root->child;
    Vec<int> path;
    for (int i = 0; i < s->roots.Size(); i++) {
        path.Append(i);
        TocCalibBindPair(s, s->roots[i], t, path);
        path.RemoveLast();
        if (t) {
            t = t->next;
        }
    }
    TocCalibRebind(win);
}

static void TocCalibReloadTree(MainWindow* win) {
    if (!win) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    ExtractedTocItem* keepExtracted = nullptr;
    Vec<int> keepPath;
    int keepId = win->tocAnchorId;
    if (s && tab && tab->currToc && tab->currToc->root) {
        TocItem* oldKeep = keepId ? TocCalibFindById(tab->currToc->root->child, keepId) : nullptr;
        if (!oldKeep && win->tocTreeView) {
            oldKeep = (TocItem*)win->tocTreeView->GetSelection();
        }
        TocCalibRow* kr = oldKeep ? TocCalibRowForTocItem(win, oldKeep) : nullptr;
        if (kr) {
            keepExtracted = kr->item;
        }
        if (oldKeep) {
            TocCalibPathForItem(tab->currToc->root, oldKeep, keepPath);
        }
    }
    TocTreeViewKeep* viewKeep = TocTreeViewKeepStart(win);
    HWND hwndTv = win->tocTreeView ? win->tocTreeView->hwnd : nullptr;
    HWND hwndBox = hwndTv ? GetParent(hwndTv) : nullptr;
    if (hwndTv) {
        SendMessageW(hwndTv, WM_SETREDRAW, FALSE, 0);
    }
    if (hwndBox) {
        SendMessageW(hwndBox, WM_SETREDRAW, FALSE, 0);
    }
    bool prevSuppress = win->tocSuppressGoTo;
    win->tocSuppressGoTo = true;
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
    TocCalibBindToTree(win);
    tab = win->CurrentTab();
    s = tab ? tab->tocCalib : nullptr;
    TocItem* keep = nullptr;
    if (s && tab && tab->currToc && tab->currToc->root) {
        if (keepExtracted) {
            for (int i = 0; i < s->rows.Size(); i++) {
                if (s->rows[i].item == keepExtracted && s->rows[i].toc.tocId) {
                    keep = TocCalibFindById(tab->currToc->root->child, s->rows[i].toc.tocId);
                    break;
                }
            }
        }
        if (!keep && keepPath.Size() > 0) {
            keep = TocCalibItemAtPath(tab->currToc->root->child, keepPath);
        }
    }
    if (keep && win->tocTreeView) {
        win->tocTreeView->SelectItem((TreeItem)keep);
        if (keep->id) {
            win->tocSelectedIds.Reset();
            win->tocSelectedIds.Append(keep->id);
            win->tocAnchorId = keep->id;
        }
    }
    TocTreeViewKeepFinish(win, viewKeep);
    win->tocSuppressGoTo = prevSuppress;
    hwndTv = win->tocTreeView ? win->tocTreeView->hwnd : nullptr;
    hwndBox = hwndTv ? GetParent(hwndTv) : nullptr;
    if (hwndBox) {
        SendMessageW(hwndBox, WM_SETREDRAW, TRUE, 0);
    }
    if (hwndTv) {
        SendMessageW(hwndTv, WM_SETREDRAW, TRUE, 0);
        RedrawWindow(hwndTv, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    if (hwndBox) {
        RedrawWindow(hwndBox, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

static void TocCalibSyncTitlesFromTree(MainWindow* win, TocCalibSession* s) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocTree* toc = tab && tab->currToc ? tab->currToc : nullptr;
    if (!s || !toc || !toc->root) {
        return;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        TocItem* t = nullptr;
        if (s->rows[i].toc.tocId) {
            t = TocCalibFindById(toc->root->child, s->rows[i].toc.tocId);
        }
        if (!t || !t->title || !t->title[0]) {
            continue;
        }
        if (it->title && it->rawTitle && str::Eq(it->title, t->title) && str::Eq(it->rawTitle, t->title)) {
            continue;
        }
        // F2 / Ctrl+Shift+B write the session first; the tree still shows the last
        // pushed title. Copying from the tree would drop that edit.
        if (s->rows[i].userSet && it->title && it->title[0]) {
            continue;
        }
        str::ReplaceWithCopy(&it->title, t->title);
        str::ReplaceWithCopy(&it->rawTitle, t->title);
    }
}

bool TocCalibRenameItem(MainWindow* win, TocItem* item, const char* title) {
    if (!win || !item || !title || !title[0] || !TocCalibIsActive(win)) {
        return false;
    }
    TocCalibRow* row = TocCalibRowForTocItem(win, item);
    if (!row) {
        TocCalibRebind(win);
        row = TocCalibRowForTocItem(win, item);
    }
    if (!row || !row->item) {
        return false;
    }
    TocCalibRemember(win->CurrentTab() ? win->CurrentTab()->tocCalib : nullptr);
    str::ReplaceWithCopy(&row->item->title, title);
    str::ReplaceWithCopy(&row->item->rawTitle, title);
    row->userSet = true;
    return true;
}

static void TocCalibFixLevels(ExtractedTocItem* n, int level);

static ExtractedTocItem* TocCalibExtractedForTocItem(TocCalibSession* s, TocItem* t,
                                                     const Vec<ExtractedTocItem*>& flat) {
    if (!s || !t) {
        return nullptr;
    }
    if (t->id) {
        TocCalibRow* row = TocCalibFindRowById(s, t->id);
        if (row && row->item) {
            return row->item;
        }
        for (int i = 0; i < flat.Size(); i++) {
            ExtractedTocItem* e = flat[i];
            if (e && e->treeHandle && (int)(intptr_t)e->treeHandle == t->id) {
                return e;
            }
        }
    }
    return nullptr;
}

static bool TocCalibAttachFromTocItems(TocCalibSession* s, TocItem* first, Vec<ExtractedTocItem*>& dest,
                                       ExtractedTocItem* parent, int level, const Vec<ExtractedTocItem*>& flat,
                                       Vec<ExtractedTocItem*>& used) {
    dest.Reset();
    for (TocItem* t = first; t; t = t->next) {
        ExtractedTocItem* e = TocCalibExtractedForTocItem(s, t, flat);
        if (!e) {
            e = TocCalibNewFromToc(t);
            if (!e) {
                continue;
            }
            if (t->id) {
                e->treeHandle = (void*)(intptr_t)t->id;
            }
            s->extras.Append(e);
        }
        e->parent = parent;
        if (t->id) {
            e->treeHandle = (void*)(intptr_t)t->id;
        }
        dest.Append(e);
        if (!used.Contains(e)) {
            used.Append(e);
        }
        if (!TocCalibAttachFromTocItems(s, t->child, e->children, e, level + 1, flat, used)) {
            return false;
        }
        TocCalibFixLevels(e, level);
    }
    return true;
}

// Rewrite s->roots parent/child links to match the tree the user sees.
static bool TocCalibSyncHierarchyFromTree(MainWindow* win, TocCalibSession* s) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocTree* toc = tab && tab->currToc ? tab->currToc : nullptr;
    if (!s || !toc || !toc->root || !toc->root->child) {
        return false;
    }
    Vec<ExtractedTocItem*> flat;
    FlattenExtractedTocItems(s->roots, flat);
    FlattenExtractedTocItems(s->extras, flat);
    for (int i = 0; i < flat.Size(); i++) {
        if (flat[i]) {
            flat[i]->parent = nullptr;
            flat[i]->children.Reset();
        }
    }
    Vec<ExtractedTocItem*> used;
    Vec<ExtractedTocItem*> newRoots;
    if (!TocCalibAttachFromTocItems(s, toc->root->child, newRoots, nullptr, 1, flat, used) || newRoots.empty()) {
        return false;
    }
    s->roots.Reset();
    for (int i = 0; i < newRoots.Size(); i++) {
        s->roots.Append(newRoots[i]);
    }
    s->extras.Reset();
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* e = flat[i];
        if (e && !used.Contains(e)) {
            s->extras.Append(e);
        }
    }
    TocCalibCollectRows(s);
    return true;
}

// syncFromTree: write-bookmarks / tree-first add. Session-first merge/delete/move
// must pass false — the visible tree still has the old nodes and would undo them.
static bool TocCalibPushOutline(MainWindow* win, TocCalibSession* s, bool syncFromTree = true) {
    if (!win || !s || !s->engine || s->roots.Size() < 1) {
        return false;
    }
    if (syncFromTree) {
        TocCalibSyncHierarchyFromTree(win, s);
        TocCalibSyncTitlesFromTree(win, s);
    }
    char* err = nullptr;
    bool ok = EngineMupdfReplacePdfToc(s->engine, s->roots, &err);
    str::Free(err);
    return ok;
}

static void TocCalibApplyPins(TocCalibSession* s) {
    if (!s) {
        return;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        if (s->rows[i].pdfPinned) {
            continue;
        }
        if (!TocCalibHasPrinted(it->printedPage)) {
            if (s->rows[i].origPageNo > 0) {
                it->pageNo = s->rows[i].origPageNo;
            }
            continue;
        }
        if (s->map.confidence <= 0) {
            continue;
        }
        it->pageNo = TocCalibPredPdf(it->printedPage, s->map.offset, s->nPages);
    }
}

static void TocCalibJumpToPdfPage(MainWindow* win, int page);

static void TocCalibSyncTreeDests(MainWindow* win, TocCalibSession* s) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocTree* toc = tab && tab->currToc ? tab->currToc : nullptr;
    if (!s || !toc || !toc->root) {
        return;
    }
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || !s->rows[i].toc.tocId) {
            continue;
        }
        TocItem* t = TocCalibFindById(toc->root->child, s->rows[i].toc.tocId);
        if (!t) {
            continue;
        }
        t->pageNo = it->pageNo;
        if (t->dest) {
            t->dest->pageNo = it->pageNo;
            EngineMupdfUpdatePageDest(engine, t->dest, it->pageNo, it->x, it->y);
        }
    }
}

// rebuildTree: merge / delete / add / move. Page ± / pin only patch dests so
// ClearTocBox does not blank the sidebar for a few hundred milliseconds.
static bool TocCalibLiveApply(MainWindow* win, bool rebuildTree = true) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    TocCalibSolveSession(s);
    TocCalibApplyPins(s);
    if (!rebuildTree) {
        TocCalibSyncTreeDests(win, s);
        InvalidateTocTree(win);
        return true;
    }
    if (!TocCalibPushOutline(win, s, false)) {
        return false;
    }
    TocCalibReloadTree(win);
    InvalidateTocTree(win);
    return true;
}

static bool TocCalibApplyUndoOrRedo(MainWindow* win, TocCalibSession* s, Vec<TocCalibUndoSnap*>& from,
                                    Vec<TocCalibUndoSnap*>& to) {
    if (!win || !s || from.Size() < 1) {
        return false;
    }
    TocCalibClosePageEdit(false);
    TocCalibUndoSnap* cur = TocCalibCaptureSnap(s);
    if (cur) {
        to.Append(cur);
        while (to.Size() > kTocCalibUndoMax) {
            TocCalibFreeUndoSnap(to[0]);
            to.RemoveAt(0);
        }
    }
    TocCalibUndoSnap* snap = from.Pop();
    s->undoBusy = true;
    TocCalibInstallSnap(s, snap);
    TocCalibFreeUndoSnap(snap);
    bool ok = TocCalibLiveApply(win);
    s->undoBusy = false;
    return ok;
}

bool TocCalibUndo(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    return TocCalibApplyUndoOrRedo(win, s, s->undo, s->redo);
}

bool TocCalibRedo(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    return TocCalibApplyUndoOrRedo(win, s, s->redo, s->undo);
}

static bool TocCalibFocusIsTextEdit(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    TempStr cls = HwndGetClassName(hwnd);
    return cls && (str::EqI(cls, "Edit") || str::StartsWithI(cls, "RichEdit"));
}

bool TocCalibHandleUndoShortcut(MainWindow* win, HWND focus, int vk, bool ctrl, bool shift) {
    if (!win || !ctrl || !TocCalibIsActive(win)) {
        return false;
    }
    if (TocCalibFocusIsTextEdit(focus)) {
        return false;
    }
    if (vk == 'Z' && !shift) {
        return TocCalibUndo(win);
    }
    if ((vk == 'Z' && shift) || (vk == 'Y' && !shift)) {
        return TocCalibRedo(win);
    }
    return false;
}

static bool TocCalibCurrentDest(MainWindow* win, int* pageOut, float* xOut, float* yOut) {
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    if (!dm || !pageOut || !xOut || !yOut) {
        return false;
    }
    TextSelection* sel = dm->textSelection;
    if (sel && sel->result.len > 0 && sel->result.pages && sel->result.rects) {
        int page = sel->result.pages[0];
        if (page > 0) {
            *pageOut = page;
            *xOut = (float)sel->result.rects[0].x;
            *yOut = (float)sel->result.rects[0].y;
            return true;
        }
    }
    int page = win->ctrl->CurrentPageNo();
    if (page < 1) {
        return false;
    }
    ScrollState st = dm->GetScrollState();
    *pageOut = page;
    *xOut = st.x >= 0 ? (float)st.x : 0.f;
    *yOut = st.y >= 0 ? (float)st.y : 0.f;
    return true;
}

static bool TocCalibLabelIsPlainPdf(const char* label, int pdf) {
    if (!label || pdf < 1) {
        return true;
    }
    int n = TocCalibParseLabelPrinted(label);
    return n == pdf;
}

static void TocCalibClearPrintedLabel(ExtractedTocItem* it) {
    if (it) {
        str::Free(it->printedLabel);
        it->printedLabel = nullptr;
    }
}

static void TocCalibPinRowToPage(TocCalibSession* s, TocCalibRow* row, int page, float x, float y, const char* label) {
    if (!s || !row || !row->item || page < 1) {
        return;
    }
    row->item->pageNo = page;
    row->item->x = x;
    row->item->y = y;
    row->pdfPinned = true;
    row->userSet = true;
    row->colChosen = true;
    row->editPdf = false;
    row->needsConfirm = false;
    TocCalibClearPrintedLabel(row->item);
    if (label && label[0]) {
        str::ReplaceWithCopy(&row->item->printedLabel, label);
    } else if (s->engine) {
        TempStr lab = s->engine->GetPageLabeTemp(page);
        if (lab && !TocCalibLabelIsPlainPdf(lab, page)) {
            str::ReplaceWithCopy(&row->item->printedLabel, lab);
        }
    }
    // Arabic printed page from offset / label / gap. Front matter (序, i, ii)
    // keeps printedPage empty and shows the real page label instead of pdf-offset.
    int pr = 0;
    if (TocCalibHaveOffset(s)) {
        int g = page - s->map.offset;
        if (g > 0) {
            pr = g;
        }
    }
    if (!TocCalibHasPrinted(pr)) {
        int lab = TocCalibLabelPrinted(s, page);
        if (TocCalibHasPrinted(lab)) {
            pr = lab;
        }
    }
    if (!TocCalibHasPrinted(pr)) {
        int idx = TocCalibRowIndexOf(s, row);
        pr = TocCalibInterpolatePrinted(s, idx);
    }
    row->item->printedPage = TocCalibHasPrinted(pr) ? pr : 0;
}

bool TocCalibPinSelectedToView(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    int page = 0;
    float x = 0;
    float y = 0;
    if (!TocCalibCurrentDest(win, &page, &x, &y)) {
        return false;
    }
    TocCalibRebind(win);
    TocCalibRow* row = nullptr;
    if (win->tocSelectedIds.Size() > 0) {
        int id = win->tocSelectedIds[0];
        TocItem* t = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, id) : nullptr;
        row = t ? TocCalibRowForTocItem(win, t) : nullptr;
    }
    if (!row) {
        TocItem* sel = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
        row = sel ? TocCalibRowForTocItem(win, sel) : nullptr;
    }
    if (!row || !row->item) {
        DisplayModel* dm = tab->AsFixed();
        TextSelection* sel = dm ? dm->textSelection : nullptr;
        if (!sel || sel->result.len <= 0) {
            return false;
        }
        bool textOnly = false;
        TempStr title = GetSelectedTextTemp(tab, " ", textOnly);
        if (!textOnly || !title || !title[0]) {
            return false;
        }
        AutoFreeStr trimmed(str::Dup(title));
        str::TrimWSInPlace(trimmed.Get(), str::TrimOpt::Both);
        if (str::IsEmpty(trimmed.Get())) {
            return false;
        }
        int after = -1;
        for (int i = 0; i < s->rows.Size(); i++) {
            if (s->rows[i].toc.tocId && win->tocSelectedIds.Contains(s->rows[i].toc.tocId)) {
                after = i;
            }
        }
        TocCalibRemember(s);
        int idx = TocCalibAddManualItem(s, trimmed.Get(), page, x, y, after);
        if (idx < 0) {
            return false;
        }
        return TocCalibLiveApply(win);
    }
    TocCalibRemember(s);
    TocCalibPinRowToPage(s, row, page, x, y, nullptr);
    int jump = page;
    TocCalibLiveApply(win, false);
    TocCalibJumpToPdfPage(win, jump);
    return true;
}

static TocCalibRow* TocCalibSelectedRow(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return nullptr;
    }
    if (win->tocSelectedIds.Size() > 0) {
        int id = win->tocSelectedIds[0];
        TocItem* t = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, id) : nullptr;
        TocCalibRow* row = t ? TocCalibRowForTocItem(win, t) : nullptr;
        if (row) {
            return row;
        }
    }
    TocItem* sel = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
    return sel ? TocCalibRowForTocItem(win, sel) : nullptr;
}

bool TocCalibAddSelectionUnderCurrent(MainWindow* win, const char* title, int pageNo, float x, float y) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !title || !title[0] || pageNo < 1) {
        return false;
    }
    TocCalibRebind(win);
    int after = -1;
    TocCalibRow* row = TocCalibSelectedRow(win);
    if (row) {
        for (int i = 0; i < s->rows.Size(); i++) {
            if (&s->rows[i] == row || s->rows[i].item == row->item) {
                after = i;
                break;
            }
        }
    }
    bool asChild = row && row->item && row->item->children.Size() > 0;
    if (!asChild && row && row->toc.tocId && tab && tab->currToc && tab->currToc->root) {
        TocItem* t = TocCalibFindById(tab->currToc->root->child, row->toc.tocId);
        asChild = t && t->child;
    }
    TocCalibRemember(s);
    int idx = TocCalibAddManualItem(s, title, pageNo, x, y, after, asChild);
    if (idx < 0) {
        return false;
    }
    return TocCalibLiveApply(win);
}

bool TocCalibReplaceSelectedFromSelection(MainWindow* win, const char* title, int pageNo, float x, float y) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !title || !title[0]) {
        return false;
    }
    TocCalibRebind(win);
    TocCalibRow* row = TocCalibSelectedRow(win);
    if (!row || !row->item) {
        return false;
    }
    TocCalibRemember(s);
    str::ReplaceWithCopy(&row->item->title, title);
    str::ReplaceWithCopy(&row->item->rawTitle, title);
    row->userSet = true;
    if (pageNo > 0) {
        TocCalibPinRowToPage(s, row, pageNo, x, y, nullptr);
    }
    return TocCalibLiveApply(win);
}

static int TocCalibLocatePredPage(const TocCalibSession* s, const TocCalibRow* row) {
    int pdf = TocCalibRowPdf(row);
    if (pdf > 0 && !TocCalibPageInToc(s, pdf)) {
        return pdf;
    }
    if (s && row && row->item && TocCalibHasPrinted(row->item->printedPage) && TocCalibHaveOffset(s)) {
        int pred = TocCalibPredPdf(row->item->printedPage, s->map.offset, s->nPages);
        if (pred > 0 && !TocCalibPageInToc(s, pred)) {
            return pred;
        }
    }
    return 0;
}

static bool TocCalibSearchTextFind(TocCalibSession* s, const char* query, int predPage, TocCalibNearHit* out) {
    if (!s || !s->engine || !query || !query[0] || !out) {
        return false;
    }
    if (TocCalibGlyphCount(query) < 4) {
        return false;
    }
    WCHAR* w = ToWStrTemp(query);
    if (!w || !w[0]) {
        return false;
    }
    TextSearch ts(s->engine);
    ts.SetMatchCase(false);
    ts.SetMatchWholeWord(false);
    TextSel* sel = ts.FindFirst(1, w);
    int nHits = 0;
    int nScan = 0;
    int bestPage = 0;
    float bestX = 0;
    float bestY = 0;
    int bestDist = 0;
    while (sel && sel->len > 0 && nHits < 32 && nScan < 80) {
        nScan++;
        int page = ts.GetSearchHitStartPageNo();
        if (page < 1 && sel->pages) {
            page = sel->pages[0];
        }
        if (page < 1) {
            page = ts.startPage;
        }
        if (page < 1 || TocCalibPageInToc(s, page)) {
            sel = ts.FindNext();
            continue;
        }
        nHits++;
        float x = 0;
        float y = 0;
        if (sel->rects) {
            x = (float)sel->rects[0].x;
            y = (float)sel->rects[0].y;
        }
        int dist = predPage > 0 ? page - predPage : page;
        if (dist < 0) {
            dist = -dist;
        }
        bool take = false;
        if (bestPage < 1) {
            take = true;
        } else if (predPage > 0) {
            take = dist < bestDist || (dist == bestDist && y < bestY);
        } else if (page == bestPage && y < bestY) {
            take = true;
        }
        if (take) {
            bestPage = page;
            bestDist = dist;
            bestX = x;
            bestY = y;
        }
        sel = ts.FindNext();
    }
    if (bestPage < 1) {
        return false;
    }
    out->page = bestPage;
    out->x = bestX;
    out->y = bestY;
    out->score = TocCalibGlyphCount(query);
    out->fontSize = 0;
    return true;
}

static bool TocCalibSearchTextFindFallback(TocCalibSession* s, TocCalibRow* row, const char* title,
                                           TocCalibNearHit* hit) {
    if (!s || !s->engine || !title || !hit) {
        return false;
    }
    TocCalibFindQueries q;
    if (!TocCalibBuildFindQueries(title, &q) || q.n < 1) {
        return false;
    }
    int pred = TocCalibLocatePredPage(s, row);
    for (int i = 0; i < q.n; i++) {
        if (TocCalibSearchTextFind(s, q.q[i], pred, hit)) {
            return true;
        }
    }
    return false;
}

static bool TocCalibSearchRowInBody(TocCalibSession* s, TocCalibRow* row, TocCalibNearHit* hit) {
    if (!s || !row || !row->item || !hit) {
        return false;
    }
    const char* title = row->item->rawTitle && row->item->rawTitle[0] ? row->item->rawTitle : row->item->title;
    if (!title || !title[0]) {
        return false;
    }
    TocCalibEnsureTocRange(s);
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    TocCalibBm25Index bm25;
    *hit = {};
    bool found = false;
    if (TocCalibGlyphCount(title) >= 4) {
        found = TocCalibSearchBm25(s, title, pages, cache, &bm25, hit);
        if (found && TocCalibPageInToc(s, hit->page)) {
            found = false;
        }
        if (!found) {
            found = TocCalibSearchTextFindFallback(s, row, title, hit);
            if (found && TocCalibPageInToc(s, hit->page)) {
                found = false;
            }
        }
    } else {
        int bestSc = 0;
        int secondSc = 0;
        int bestPage = 0;
        float bestX = 0;
        float bestY = 0;
        for (int p = 1; p <= s->nPages; p++) {
            if (TocCalibPageInToc(s, p)) {
                continue;
            }
            const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
            if (!lines) {
                continue;
            }
            for (int i = 0; i < lines->Size(); i++) {
                const EngineMupdfPageLine& ln = lines->At(i);
                if (!ln.text || !ln.text[0]) {
                    continue;
                }
                int sc = TocCalibTitleMatchScore(ln.text, title);
                if (sc > bestSc) {
                    secondSc = bestSc;
                    bestSc = sc;
                    bestPage = p;
                    bestX = ln.x;
                    bestY = ln.y;
                } else if (sc > secondSc) {
                    secondSc = sc;
                }
            }
        }
        if (bestPage > 0 && bestSc >= 2 && (secondSc < 1 || bestSc > secondSc)) {
            hit->page = bestPage;
            hit->x = bestX;
            hit->y = bestY;
            hit->score = bestSc;
            found = true;
        }
    }
    TocCalibBm25Free(&bm25);
    TocCalibFreePageCache(cache);
    return found && hit->page > 0;
}

static bool TocCalibNotifyNoBodyHit(MainWindow* win) {
    if (win && win->hwndCanvas) {
        ShowTemporaryNotification(win->hwndCanvas, _TRA("Could not find this heading in the body"));
    }
    return false;
}

bool TocCalibLocateSelectedInBody(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    TocCalibRebind(win);
    TocCalibRow* row = TocCalibSelectedRow(win);
    if (!row || !row->item) {
        return false;
    }
    TocCalibNearHit hit;
    if (!TocCalibSearchRowInBody(s, row, &hit)) {
        return TocCalibNotifyNoBodyHit(win);
    }
    TocCalibJumpToPdfPage(win, hit.page);
    return true;
}

static bool TocCalibLocateRowInBody(MainWindow* win, TocCalibRow* row) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !row || !row->item) {
        return false;
    }
    TocCalibNearHit hit;
    if (!TocCalibSearchRowInBody(s, row, &hit)) {
        return TocCalibNotifyNoBodyHit(win);
    }
    TocCalibJumpToPdfPage(win, hit.page);
    return true;
}

static bool TocCalibPinRowToView(MainWindow* win, TocCalibRow* row) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !row || !row->item) {
        return false;
    }
    int page = 0;
    float x = 0;
    float y = 0;
    if (!TocCalibCurrentDest(win, &page, &x, &y)) {
        return false;
    }
    if (row->toc.tocId) {
        win->tocSelectedIds.Reset();
        win->tocSelectedIds.Append(row->toc.tocId);
        win->tocAnchorId = row->toc.tocId;
    }
    TocCalibRemember(s);
    TocCalibPinRowToPage(s, row, page, x, y, nullptr);
    return TocCalibLiveApply(win, false);
}

static bool TocCalibItemContains(ExtractedTocItem* a, ExtractedTocItem* b) {
    if (!a || !b) {
        return false;
    }
    if (a == b) {
        return true;
    }
    for (int i = 0; i < a->children.Size(); i++) {
        if (TocCalibItemContains(a->children[i], b)) {
            return true;
        }
    }
    return false;
}

static bool TocCalibUnlinkItem(TocCalibSession* s, ExtractedTocItem* n) {
    if (!s || !n) {
        return false;
    }
    if (TocCalibRemoveItem(s->roots, n) || TocCalibRemoveItem(s->extras, n)) {
        n->parent = nullptr;
        return true;
    }
    return false;
}

static void TocCalibFixLevels(ExtractedTocItem* n, int level) {
    if (!n) {
        return;
    }
    if (level < 1) {
        level = 1;
    }
    n->level = level;
    for (int i = 0; i < n->children.Size(); i++) {
        TocCalibFixLevels(n->children[i], level + 1);
    }
}

static Vec<ExtractedTocItem*>* TocCalibOwningList(TocCalibSession* s, ExtractedTocItem* n);

static bool TocCalibMoveItems(TocCalibSession* s, const Vec<ExtractedTocItem*>& moving, ExtractedTocItem* dest,
                              int dropPos) {
    if (!s || moving.empty()) {
        return false;
    }
    if (!dest && dropPos == 2) {
        return false;
    }
    for (int i = 0; i < moving.Size(); i++) {
        if (dest && TocCalibItemContains(moving[i], dest)) {
            return false;
        }
    }
    for (int i = 0; i < moving.Size(); i++) {
        if (!TocCalibUnlinkItem(s, moving[i])) {
            return false;
        }
    }
    if (dropPos == 2 && dest) {
        for (int i = 0; i < moving.Size(); i++) {
            ExtractedTocItem* n = moving[i];
            n->parent = dest;
            dest->children.Append(n);
            TocCalibFixLevels(n, dest->level + 1);
        }
        return true;
    }
    Vec<ExtractedTocItem*>* sibs = dest ? TocCalibOwningList(s, dest) : &s->roots;
    if (!sibs) {
        sibs = &s->roots;
    }
    int insertAt = 0;
    if (dest) {
        insertAt = sibs->Find(dest);
        if (insertAt < 0) {
            insertAt = sibs->Size();
        } else if (dropPos != 0) {
            insertAt++;
        }
    } else if (dropPos != 0) {
        insertAt = sibs->Size();
    }
    ExtractedTocItem* parent = dest ? dest->parent : nullptr;
    int level = parent ? parent->level + 1 : 1;
    for (int i = 0; i < moving.Size(); i++) {
        ExtractedTocItem* n = moving[i];
        n->parent = parent;
        sibs->InsertAt((size_t)(insertAt + i), n);
        TocCalibFixLevels(n, level);
    }
    return true;
}

static Vec<ExtractedTocItem*>* TocCalibOwningList(TocCalibSession* s, ExtractedTocItem* n) {
    if (!s || !n) {
        return nullptr;
    }
    if (n->parent) {
        return &n->parent->children;
    }
    if (s->extras.Find(n) >= 0) {
        return &s->extras;
    }
    return &s->roots;
}

bool TocCalibDeleteAndPromote(TocCalibSession* s, ExtractedTocItem* n) {
    if (!s || !n) {
        return false;
    }
    int keepLevel = n->level < 1 ? 1 : n->level;
    ExtractedTocItem* parent = n->parent;
    Vec<ExtractedTocItem*> kids;
    for (int i = 0; i < n->children.Size(); i++) {
        kids.Append(n->children[i]);
    }
    n->children.Reset();
    Vec<ExtractedTocItem*>* sibs = TocCalibOwningList(s, n);
    int at = sibs ? sibs->Find(n) : -1;
    if (at < 0) {
        for (int i = 0; i < kids.Size(); i++) {
            n->children.Append(kids[i]);
        }
        return false;
    }
    sibs->RemoveAt(at);
    for (int i = 0; i < kids.Size(); i++) {
        ExtractedTocItem* ch = kids[i];
        ch->parent = parent;
        sibs->InsertAt((size_t)(at + i), ch);
        TocCalibFixLevels(ch, keepLevel);
    }
    delete n;
    return true;
}

static bool TocCalibAsciiWordCp(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static void TocCalibAppendTitle(char** dst, const char* extra) {
    if (!dst || !extra || !extra[0]) {
        return;
    }
    if (!*dst || !(*dst)[0]) {
        str::ReplaceWithCopy(dst, extra);
        return;
    }
    const char* a = *dst;
    size_t n = str::Len(a);
    unsigned char last = n > 0 ? (unsigned char)a[n - 1] : 0;
    unsigned char first = (unsigned char)extra[0];
    bool needSpace = TocCalibAsciiWordCp(last) && TocCalibAsciiWordCp(first);
    char* joined = needSpace ? str::Join(a, " ", extra) : str::Join(a, extra);
    str::ReplacePtr(dst, joined);
}

static int TocCalibRowIndexOf(TocCalibSession* s, TocCalibRow* row) {
    if (!s || !row) {
        return -1;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        if (&s->rows[i] == row || s->rows[i].item == row->item) {
            return i;
        }
    }
    return -1;
}

static TocCalibRow* TocCalibNextRow(TocCalibSession* s, TocCalibRow* row) {
    int i = TocCalibRowIndexOf(s, row);
    if (i < 0 || i + 1 >= s->rows.Size()) {
        return nullptr;
    }
    return &s->rows[i + 1];
}

bool TocCalibMergeWithNext(TocCalibSession* s, TocCalibRow* row) {
    if (!s || !row || !row->item) {
        return false;
    }
    TocCalibRow* next = TocCalibNextRow(s, row);
    if (!next || !next->item || next->item == row->item) {
        return false;
    }
    ExtractedTocItem* a = row->item;
    ExtractedTocItem* b = next->item;
    const char* extra = b->rawTitle && b->rawTitle[0] ? b->rawTitle : b->title;
    if (!a->rawTitle || !a->rawTitle[0]) {
        str::ReplaceWithCopy(&a->rawTitle, a->title);
    }
    if (a->title && extra && str::EndsWith(a->title, extra)) {
        return false;
    }
    TocCalibAppendTitle(&a->title, extra);
    TocCalibAppendTitle(&a->rawTitle, extra);
    row->userSet = true;
    if (!TocCalibDeleteAndPromote(s, b)) {
        return false;
    }
    TocCalibCollectRows(s);
    return true;
}

bool TocCalibTestMergeWithNext() {
    TocCalibSession s;
    auto* a = new ExtractedTocItem;
    a->title = str::Dup("认为高考竞争对孩子不利");
    a->rawTitle = str::Dup("认为高考竞争对孩子不利");
    a->level = 1;
    a->pageNo = 18;
    a->printedPage = 4;
    auto* b = new ExtractedTocItem;
    b->title = str::Dup("是错误的");
    b->rawTitle = str::Dup("是错误的");
    b->level = 1;
    b->pageNo = 19;
    b->printedPage = 5;
    s.roots.Append(a);
    s.roots.Append(b);
    TocCalibLinkParents(s.roots, nullptr);
    TocCalibCollectRows(&s);
    bool ok = TocCalibMergeWithNext(&s, &s.rows[0]);
    ok = ok && s.roots.Size() == 1 && s.roots[0] == a;
    ok = ok && str::Eq(a->title, "认为高考竞争对孩子不利是错误的");
    ok = ok && a->pageNo == 18 && a->printedPage == 4;
    ok = ok && !TocCalibMergeWithNext(&s, &s.rows[0]);
    DeleteExtractedTocItems(s.roots);
    return ok;
}

bool TocCalibTestUndo() {
    TocCalibSession s;
    auto* a = new ExtractedTocItem;
    a->title = str::Dup("认为高考竞争对孩子不利");
    a->rawTitle = str::Dup("认为高考竞争对孩子不利");
    a->level = 1;
    a->pageNo = 18;
    a->printedPage = 4;
    auto* b = new ExtractedTocItem;
    b->title = str::Dup("是错误的");
    b->rawTitle = str::Dup("是错误的");
    b->level = 1;
    b->pageNo = 19;
    b->printedPage = 5;
    s.roots.Append(a);
    s.roots.Append(b);
    TocCalibLinkParents(s.roots, nullptr);
    TocCalibCollectRows(&s);
    TocCalibRemember(&s);
    bool ok = TocCalibMergeWithNext(&s, &s.rows[0]);
    ok = ok && s.roots.Size() == 1 && str::Eq(s.roots[0]->title, "认为高考竞争对孩子不利是错误的");
    TocCalibUndoSnap* snap = s.undo.Size() > 0 ? s.undo.Pop() : nullptr;
    ok = ok && snap;
    if (snap) {
        TocCalibInstallSnap(&s, snap);
        TocCalibFreeUndoSnap(snap);
    }
    ok = ok && s.roots.Size() == 2;
    ok = ok && str::Eq(s.roots[0]->title, "认为高考竞争对孩子不利");
    ok = ok && str::Eq(s.roots[1]->title, "是错误的");
    DeleteExtractedTocItems(s.roots);
    DeleteExtractedTocItems(s.extras);
    TocCalibFreeUndoStack(s.undo);
    TocCalibFreeUndoStack(s.redo);
    return ok;
}

static int TocCalibItemDepth(ExtractedTocItem* n) {
    int d = 0;
    for (ExtractedTocItem* p = n; p; p = p->parent) {
        d++;
        if (d > 32) {
            break;
        }
    }
    return d;
}

bool TocCalibHandleDelete(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    TocCalibRebind(win);
    Vec<ExtractedTocItem*> doomed;
    if (win->tocSelectedIds.Size() > 0) {
        for (int id : win->tocSelectedIds) {
            TocItem* t = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, id) : nullptr;
            TocCalibRow* row = t ? TocCalibRowForTocItem(win, t) : nullptr;
            if (row && row->item && !doomed.Contains(row->item)) {
                doomed.Append(row->item);
            }
        }
    }
    if (doomed.empty()) {
        TocItem* sel = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
        TocCalibRow* row = sel ? TocCalibRowForTocItem(win, sel) : nullptr;
        if (row && row->item) {
            doomed.Append(row->item);
        }
    }
    if (doomed.empty()) {
        return false;
    }
    for (int i = doomed.Size() - 1; i >= 0; i--) {
        bool underSelected = false;
        for (int j = 0; j < doomed.Size(); j++) {
            if (i != j && doomed[j] != doomed[i] && TocCalibItemContains(doomed[j], doomed[i])) {
                underSelected = true;
                break;
            }
        }
        if (underSelected) {
            doomed.RemoveAt(i);
        }
    }
    for (int i = 0; i < doomed.Size(); i++) {
        for (int j = i + 1; j < doomed.Size(); j++) {
            if (TocCalibItemDepth(doomed[j]) < TocCalibItemDepth(doomed[i])) {
                ExtractedTocItem* t = doomed[i];
                doomed[i] = doomed[j];
                doomed[j] = t;
            }
        }
    }
    if (doomed.empty()) {
        return false;
    }
    TocCalibRemember(s);
    for (int i = 0; i < doomed.Size(); i++) {
        TocCalibDeleteAndPromote(s, doomed[i]);
    }
    TocCalibCollectRows(s);
    return TocCalibLiveApply(win);
}

bool TocCalibTestDeletePromotesChildren() {
    TocCalibSession s;
    auto* ch = new ExtractedTocItem;
    ch->title = str::Dup("第一章");
    ch->level = 1;
    auto* sec1 = new ExtractedTocItem;
    sec1->title = str::Dup("第一节");
    sec1->level = 2;
    auto* sec2 = new ExtractedTocItem;
    sec2->title = str::Dup("第二节");
    sec2->level = 2;
    auto* art = new ExtractedTocItem;
    art->title = str::Dup("一、目标");
    art->level = 3;
    sec1->children.Append(art);
    ch->children.Append(sec1);
    ch->children.Append(sec2);
    s.roots.Append(ch);
    TocCalibLinkParents(s.roots, nullptr);
    bool ok = TocCalibDeleteAndPromote(&s, ch);
    ok = ok && s.roots.Size() == 2 && s.roots[0] == sec1 && s.roots[1] == sec2;
    ok = ok && sec1->level == 1 && sec2->level == 1 && !sec1->parent && !sec2->parent;
    ok = ok && sec1->children.Size() == 1 && art->parent == sec1 && art->level == 2;
    bool mid = TocCalibDeleteAndPromote(&s, sec1);
    mid = mid && s.roots.Size() == 2 && s.roots[0] == art && s.roots[1] == sec2;
    mid = mid && art->level == 1 && !art->parent && art->children.Size() == 0;
    DeleteExtractedTocItems(s.roots);
    return ok && mid;
}

bool TocCalibTestAddChildManual() {
    TocCalibSession s;
    auto* ch = new ExtractedTocItem;
    ch->title = str::Dup("第一章");
    ch->level = 1;
    ch->pageNo = 10;
    auto* old = new ExtractedTocItem;
    old->title = str::Dup("旧小节");
    old->level = 2;
    old->pageNo = 11;
    ch->children.Append(old);
    s.roots.Append(ch);
    TocCalibLinkParents(s.roots, nullptr);
    TocCalibCollectRows(&s);
    int idx = TocCalibAddManualItem(&s, "新小节", 12, 8.f, 16.f, 0, true);
    bool ok = idx >= 0 && s.roots.Size() == 1 && ch->children.Size() == 2 && ch->children[0]->level == 2 &&
              ch->children[0]->parent == ch && str::Eq(ch->children[0]->title, "新小节") &&
              str::Eq(ch->children[1]->title, "旧小节");
    DeleteExtractedTocItems(s.roots);
    if (!ok) {
        return false;
    }

    TocCalibSession leaf;
    auto* a = new ExtractedTocItem;
    a->title = str::Dup("第一节");
    a->level = 1;
    a->pageNo = 4;
    auto* b = new ExtractedTocItem;
    b->title = str::Dup("第二节");
    b->level = 1;
    b->pageNo = 8;
    leaf.roots.Append(a);
    leaf.roots.Append(b);
    TocCalibLinkParents(leaf.roots, nullptr);
    TocCalibCollectRows(&leaf);
    idx = TocCalibAddManualItem(&leaf, "新条", 6, 8.f, 16.f, 0, false);
    ok = idx >= 0 && leaf.roots.Size() == 3 && leaf.roots[0] == a && leaf.roots[2] == b &&
         str::Eq(leaf.roots[1]->title, "新条") && leaf.roots[1]->level == 1 && !leaf.roots[1]->parent;
    DeleteExtractedTocItems(leaf.roots);
    return ok;
}

bool TocCalibTestDropMoveAndNest() {
    TocCalibSession s;
    auto* a = new ExtractedTocItem;
    a->title = str::Dup("第一章");
    a->level = 1;
    auto* b = new ExtractedTocItem;
    b->title = str::Dup("第二章");
    b->level = 1;
    auto* c = new ExtractedTocItem;
    c->title = str::Dup("第一节");
    c->level = 2;
    b->children.Append(c);
    s.roots.Append(a);
    s.roots.Append(b);
    TocCalibLinkParents(s.roots, nullptr);
    Vec<ExtractedTocItem*> moving;
    moving.Append(b);
    bool nest = TocCalibMoveItems(&s, moving, a, 2);
    nest = nest && s.roots.Size() == 1 && s.roots[0] == a && a->children.Size() == 1 && a->children[0] == b;
    nest = nest && b->parent == a && b->level == 2 && c->parent == b && c->level == 3;
    moving.Reset();
    moving.Append(c);
    bool before = TocCalibMoveItems(&s, moving, a, 0);
    before = before && s.roots.Size() == 2 && s.roots[0] == c && s.roots[1] == a;
    before = before && c->level == 1 && !c->parent && b->parent == a && b->children.Size() == 0;
    DeleteExtractedTocItems(s.roots);
    return nest && before;
}

bool TocCalibHandleDrop(MainWindow* win, TocItem* destToc, int dropPos) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    TocCalibRebind(win);
    Vec<ExtractedTocItem*> moving;
    if (win->tocSelectedIds.Size() > 0) {
        for (int id : win->tocSelectedIds) {
            TocItem* t = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, id) : nullptr;
            TocCalibRow* row = t ? TocCalibRowForTocItem(win, t) : nullptr;
            if (row && row->item && !moving.Contains(row->item)) {
                moving.Append(row->item);
            }
        }
    }
    if (moving.empty()) {
        TocItem* sel = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
        TocCalibRow* row = sel ? TocCalibRowForTocItem(win, sel) : nullptr;
        if (row && row->item) {
            moving.Append(row->item);
        }
    }
    if (moving.empty()) {
        return false;
    }
    ExtractedTocItem* dest = nullptr;
    if (destToc) {
        TocCalibRow* destRow = TocCalibRowForTocItem(win, destToc);
        dest = destRow ? destRow->item : nullptr;
    }
    TocCalibRemember(s);
    if (!TocCalibMoveItems(s, moving, dest, dropPos)) {
        return false;
    }
    TocCalibCollectRows(s);
    return TocCalibLiveApply(win);
}

static void TocCalibCollectSelectedExtracted(MainWindow* win, TocCalibSession* s, Vec<ExtractedTocItem*>& moving) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!s || !tab) {
        return;
    }
    if (win->tocSelectedIds.Size() > 0) {
        for (int id : win->tocSelectedIds) {
            TocItem* t = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, id) : nullptr;
            TocCalibRow* row = t ? TocCalibRowForTocItem(win, t) : nullptr;
            if (row && row->item && !moving.Contains(row->item)) {
                moving.Append(row->item);
            }
        }
    }
    if (moving.empty()) {
        TocItem* sel = win->tocTreeView ? (TocItem*)win->tocTreeView->GetSelection() : nullptr;
        TocCalibRow* row = sel ? TocCalibRowForTocItem(win, sel) : nullptr;
        if (row && row->item) {
            moving.Append(row->item);
        }
    }
}

static bool TocCalibOutlineMove(TocCalibSession* s, const Vec<ExtractedTocItem*>& moving, bool down) {
    if (!s || moving.empty()) {
        return false;
    }
    bool any = false;
    Vec<Vec<ExtractedTocItem*>*> seen;
    int i0 = down ? moving.Size() - 1 : 0;
    int step = down ? -1 : 1;
    for (int i = i0; i >= 0 && i < moving.Size(); i += step) {
        ExtractedTocItem* n = moving[i];
        Vec<ExtractedTocItem*>* sibs = TocCalibOwningList(s, n);
        if (!sibs || seen.Contains(sibs)) {
            continue;
        }
        seen.Append(sibs);
        int idx = sibs->Find(n);
        if (idx < 0) {
            continue;
        }
        int swap = down ? idx + 1 : idx - 1;
        if (swap < 0 || swap >= sibs->Size()) {
            continue;
        }
        ExtractedTocItem* other = sibs->At(swap);
        sibs->At(idx) = other;
        sibs->At(swap) = n;
        any = true;
    }
    return any;
}

static bool TocCalibOutlinePromote(TocCalibSession* s, const Vec<ExtractedTocItem*>& moving) {
    if (!s || moving.empty()) {
        return false;
    }
    bool any = false;
    Vec<ExtractedTocItem*> parents;
    for (int i = 0; i < moving.Size(); i++) {
        ExtractedTocItem* p = moving[i] ? moving[i]->parent : nullptr;
        if (p && !parents.Contains(p)) {
            parents.Append(p);
        }
    }
    for (int i = 0; i < parents.Size(); i++) {
        ExtractedTocItem* parent = parents[i];
        Vec<ExtractedTocItem*> group;
        for (int k = 0; k < moving.Size(); k++) {
            if (moving[k] && moving[k]->parent == parent) {
                group.Append(moving[k]);
            }
        }
        if (group.empty()) {
            continue;
        }
        if (TocCalibMoveItems(s, group, parent, 1)) {
            any = true;
        }
    }
    return any;
}

static bool TocCalibOutlineDemote(TocCalibSession* s, const Vec<ExtractedTocItem*>& moving) {
    if (!s || moving.empty()) {
        return false;
    }
    bool any = false;
    for (int i = 0; i < moving.Size(); i++) {
        ExtractedTocItem* n = moving[i];
        Vec<ExtractedTocItem*>* sibs = TocCalibOwningList(s, n);
        if (!sibs) {
            continue;
        }
        int idx = sibs->Find(n);
        if (idx <= 0) {
            continue;
        }
        int prev = idx - 1;
        while (prev >= 0 && moving.Contains(sibs->At(prev))) {
            prev--;
        }
        if (prev < 0) {
            continue;
        }
        Vec<ExtractedTocItem*> one;
        one.Append(n);
        if (TocCalibMoveItems(s, one, sibs->At(prev), 2)) {
            any = true;
        }
    }
    return any;
}

bool TocCalibHandleOutlineOp(MainWindow* win, TocCalibOutlineOp op) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return false;
    }
    TocCalibRebind(win);
    Vec<ExtractedTocItem*> moving;
    TocCalibCollectSelectedExtracted(win, s, moving);
    if (moving.empty()) {
        return false;
    }
    TocCalibRemember(s);
    bool ok = false;
    if (op == TocCalibOutlineOp::MoveUp) {
        ok = TocCalibOutlineMove(s, moving, false);
    } else if (op == TocCalibOutlineOp::MoveDown) {
        ok = TocCalibOutlineMove(s, moving, true);
    } else if (op == TocCalibOutlineOp::Promote) {
        ok = TocCalibOutlinePromote(s, moving);
    } else if (op == TocCalibOutlineOp::Demote) {
        ok = TocCalibOutlineDemote(s, moving);
    }
    if (!ok) {
        return false;
    }
    if (moving[0] && moving[0]->treeHandle) {
        win->tocAnchorId = (int)(intptr_t)moving[0]->treeHandle;
    }
    TocCalibCollectRows(s);
    return TocCalibLiveApply(win);
}

bool TocCalibTestPromoteDemote() {
    TocCalibSession s;
    auto* a = new ExtractedTocItem;
    a->title = str::Dup("A");
    a->level = 1;
    auto* b = new ExtractedTocItem;
    b->title = str::Dup("B");
    b->level = 2;
    a->children.Append(b);
    s.roots.Append(a);
    TocCalibLinkParents(s.roots, nullptr);
    Vec<ExtractedTocItem*> moving;
    moving.Append(b);
    bool ok = TocCalibOutlinePromote(&s, moving);
    ok = ok && s.roots.Size() == 2 && s.roots[0] == a && s.roots[1] == b && b->level == 1 && !b->parent;
    moving.Reset();
    moving.Append(b);
    ok = ok && TocCalibOutlineDemote(&s, moving);
    ok = ok && a->children.Size() == 1 && a->children[0] == b && b->parent == a && b->level == 2;
    DeleteExtractedTocItems(s.roots);
    return ok;
}

static bool TocCalibWriteBookmarks(MainWindow* win) {
    TocCalibClosePageEdit(true);
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !s->engine) {
        return false;
    }
    TocCalibSolveSession(s);
    TocCalibApplyPins(s);
    if (!TocCalibPushOutline(win, s)) {
        MessageBoxW(win->hwndFrame, ToWStrTemp(_TRA("Could not write the PDF table of contents.")),
                    L"PDF table of contents", MB_OK | MB_ICONERROR);
        return false;
    }
    if (s->persistToDisk) {
        tab->ignoreNextAutoReload = true;
        bool saved = EngineMupdfSaveUpdated(s->engine, nullptr, {});
        if (!saved) {
            tab->ignoreNextAutoReload = false;
            MessageBoxW(win->hwndFrame, ToWStrTemp(_TRA("Could not write the PDF table of contents.")),
                        L"PDF table of contents", MB_OK | MB_ICONERROR);
            return false;
        }
        s->engine->ClearUnsavedOcrText();
    }
    DeleteExtractedTocItems(s->backup);
    TocCalibCloneForest(s->roots, s->backup);
    ToolbarUpdateStateForWindow(win, false);
    const char* path = s->engine->FilePath();
    if (s->persistToDisk && path && path[0]) {
        ShowTemporaryNotification(win->hwndCanvas, str::FormatTemp(_TRA("Saved PDF changes to '%s'"), path),
                                  kNotif5SecsTimeOut);
    } else {
        ShowTemporaryNotification(win->hwndCanvas, _TRA("Saved."), kNotifDefaultTimeOut);
    }
    return true;
}

static void TocCalibCancel(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        HideTocCalib(win);
        return;
    }
    EngineBase* engine = s->engine;
    Vec<ExtractedTocItem*> backup;
    for (int i = 0; i < s->backup.Size(); i++) {
        backup.Append(s->backup[i]);
    }
    s->backup.Reset();
    TocCalibRestoreDisplayMode(win, s);
    tab->tocCalib = nullptr;
    DeleteTocCalibSession(s);
    HideTocCalib(win);
    if (engine) {
        char* err = nullptr;
        EngineMupdfReplacePdfToc(engine, backup, &err);
        str::Free(err);
    }
    DeleteExtractedTocItems(backup);
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
}

struct TocCalibBar : Wnd {
    MainWindow* win = nullptr;
    Button* jumpToc = nullptr;
    Button* done = nullptr;
    Button* cancel = nullptr;

    ~TocCalibBar() override;
    bool Create(MainWindow* mainWin);
    void LayoutIn(int x, int y, int dx, int dy);
    void OnJumpToc();
    void OnDone();
    void OnCancel();
    void UpdateTheme();
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;
};

TocCalibBar::~TocCalibBar() {
    delete jumpToc;
    delete done;
    delete cancel;
    jumpToc = nullptr;
    done = nullptr;
    cancel = nullptr;
}

bool TocCalibBar::Create(MainWindow* mainWin) {
    win = mainWin;
    if (!win || !win->hwndTocBox) {
        return false;
    }
    COLORREF colBg = 0;
    COLORREF colTxt = 0;
    ThemeSidebarColors(colBg, colTxt);
    CreateCustomArgs args;
    args.parent = win->hwndTocBox;
    args.visible = false;
    args.style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    args.isRtl = IsUIRtl();
    args.bgColor = colBg;
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    jumpToc =
        CreateButton(hwnd, _TRA("Contents page"), MkMethod0<TocCalibBar, &TocCalibBar::OnJumpToc>(this), IsUIRtl());
    done = CreateButton(hwnd, _TRA("Save"), MkMethod0<TocCalibBar, &TocCalibBar::OnDone>(this), IsUIRtl());
    cancel = CreateButton(hwnd, _TRA("Exit"), MkMethod0<TocCalibBar, &TocCalibBar::OnCancel>(this), IsUIRtl());
    UpdateTheme();
    return true;
}

void TocCalibBar::LayoutIn(int x, int y, int dx, int dy) {
    if (!hwnd) {
        return;
    }
    bool live = IsSidebarSplitterLiveDrag();
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (live) {
        // Drop stale bits, but still paint: NOREDRAW left 目录页/保存 smears.
        flags |= SWP_NOCOPYBITS;
    }
    SetWindowPos(hwnd, nullptr, x, y, dx, dy, flags);
    int pad = DpiScale(hwnd, 6);
    int gap = DpiScale(hwnd, 4);
    int inner = dx - 2 * pad;
    if (inner < 40) {
        inner = 40;
    }
    int btnDy = DpiScale(hwnd, 26);
    if (done) {
        Size sz = done->GetIdealSize();
        if (sz.dy > btnDy) {
            btnDy = sz.dy;
        }
    }
    if (jumpToc) {
        Size sz = jumpToc->GetIdealSize();
        if (sz.dy > btnDy) {
            btnDy = sz.dy;
        }
    }
    int third = (inner - 2 * gap) / 3;
    auto placeBtn = [&](HWND btn, int bx, int by, int bdx, int bdy) {
        if (!btn) {
            return;
        }
        if (live) {
            SetWindowPos(btn, nullptr, bx, by, bdx, bdy, flags);
        } else {
            MoveWindow(btn, bx, by, bdx, bdy, TRUE);
        }
    };
    if (jumpToc) {
        placeBtn(jumpToc->hwnd, pad, pad, third, btnDy);
    }
    if (done) {
        placeBtn(done->hwnd, pad + third + gap, pad, third, btnDy);
    }
    if (cancel) {
        placeBtn(cancel->hwnd, pad + 2 * (third + gap), pad, inner - 2 * (third + gap), btnDy);
    }
    if (live) {
        TocCalibFillLiveDrag(win);
        UpdateWindow(hwnd);
        Button* btns[] = {jumpToc, done, cancel};
        for (Button* b : btns) {
            if (b && b->hwnd) {
                UpdateWindow(b->hwnd);
            }
        }
    }
}

LRESULT TocCalibBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(ThemeSidebarBackgroundColor());
        FillRect((HDC)wparam, &rc, br);
        DeleteObject(br);
        return 1;
    }
    return Wnd::WndProc(hwnd, msg, wparam, lparam);
}

void TocCalibFillLiveDrag(MainWindow* win) {
    if (!win || !win->tocCalibBar || !win->tocCalibBar->hwnd) {
        return;
    }
    HWND hwnd = win->tocCalibBar->hwnd;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HDC hdc = GetDC(hwnd);
    HBRUSH br = CreateSolidBrush(ThemeSidebarBackgroundColor());
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    ReleaseDC(hwnd, hdc);
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
    Button* btns[] = {win->tocCalibBar->jumpToc, win->tocCalibBar->done, win->tocCalibBar->cancel};
    for (Button* b : btns) {
        if (b && b->hwnd) {
            InvalidateRect(b->hwnd, nullptr, FALSE);
            UpdateWindow(b->hwnd);
        }
    }
}

void TocCalibBar::OnJumpToc() {
    TocCalibJumpToContents(win);
}

void TocCalibBar::OnDone() {
    TocCalibWriteBookmarks(win);
}

void TocCalibBar::OnCancel() {
    TocCalibCancel(win);
}

void TocCalibBar::UpdateTheme() {
    COLORREF colBg = 0;
    COLORREF colTxt = 0;
    ThemeSidebarColors(colBg, colTxt);
    SetColors(colTxt, colBg);
    Button* btns[] = {jumpToc, done, cancel};
    for (Button* b : btns) {
        if (b) {
            b->SetColors(colTxt, colBg);
        }
    }
    if (hwnd) {
        DarkMode::setChildCtrlsTheme(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

void TocCalibUpdateTheme(MainWindow* win) {
    if (win && win->tocCalibBar) {
        win->tocCalibBar->UpdateTheme();
    }
}

int TocCalibBarDy(MainWindow* win) {
    if (!win || !win->hwndTocBox) {
        return 0;
    }
    return DpiScale(win->hwndTocBox, 40);
}

struct TocCalibSpinLayout {
    bool showPrinted = true;
    bool showPdf = false;
    bool editPdf = false;
    RECT locate{};
    RECT associate{};
    RECT merge{};
    RECT del{};
    RECT prPrev{};
    RECT prField{};
    RECT prNext{};
    RECT pdfPrev{};
    RECT pdfField{};
    RECT pdfNext{};
};

struct TocCalibPageEditState {
    HWND hwnd = nullptr;
    WNDPROC prev = nullptr;
    MainWindow* win = nullptr;
    int tocId = 0;
    bool printed = false;
    bool closing = false;
};

static TocCalibPageEditState gCalibPageEdit;

static void TocCalibPlaceField(RECT& prev, RECT& field, RECT& next, int x, int yMid, int arrowDx, int fieldDx,
                               int fieldH, int gap) {
    prev.left = x;
    prev.right = x + arrowDx;
    prev.top = yMid - fieldH / 2;
    prev.bottom = prev.top + fieldH;
    field.left = prev.right + gap;
    field.right = field.left + fieldDx;
    field.top = prev.top;
    field.bottom = prev.bottom;
    next.left = field.right + gap;
    next.right = next.left + arrowDx;
    next.top = prev.top;
    next.bottom = prev.bottom;
}

static void TocCalibPlaceIcon(RECT& rc, int right, int dx, int dy, int yMid) {
    rc.right = right;
    rc.left = right - dx;
    rc.top = yMid - dy / 2;
    rc.bottom = rc.top + dy;
}

static TocCalibSpinLayout TocCalibMakeLayout(HWND hwnd, const RECT& rcRow, bool editPdf) {
    TocCalibSpinLayout L;
    L.editPdf = editPdf;
    int arrowDx = DpiScale(hwnd, 18);
    int fieldDx = DpiScale(hwnd, 36);
    int gap = DpiScale(hwnd, 3);
    int groupGap = DpiScale(hwnd, 6);
    int pad = DpiScale(hwnd, 4);
    int rowH = rcRow.bottom - rcRow.top;
    int fieldH = DpiScale(hwnd, 18);
    if (fieldH > rowH - 4) {
        fieldH = rowH - 4;
    }
    if (fieldH < 12) {
        fieldH = rowH > 4 ? rowH - 4 : rowH;
    }
    int yMid = (rcRow.top + rcRow.bottom) / 2;
    L.showPrinted = true;
    L.showPdf = false;
    int locDx = DpiScale(hwnd, 15);
    int midDx = DpiScale(hwnd, 16);
    int locDy = locDx;
    int midDy = midDx;
    if (locDy > fieldH) {
        locDy = fieldH;
        locDx = locDy;
    }
    if (midDy > fieldH) {
        midDy = fieldH;
        midDx = midDy;
    }
    // [- page +] locate associate merge delete
    TocCalibPlaceIcon(L.del, rcRow.right - pad, midDx, midDy, yMid);
    TocCalibPlaceIcon(L.merge, L.del.left - gap, midDx, midDy, yMid);
    TocCalibPlaceIcon(L.associate, L.merge.left - gap, midDx, midDy, yMid);
    TocCalibPlaceIcon(L.locate, L.associate.left - gap, locDx, locDy, yMid);
    int xPr = L.locate.left - groupGap - (arrowDx + gap + fieldDx + gap + arrowDx);
    TocCalibPlaceField(L.prPrev, L.prField, L.prNext, xPr, yMid, arrowDx, fieldDx, fieldH, gap);
    return L;
}

static bool TocCalibNoPrintedTitle(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    const char* keys[] = {
        "译者序",   "作者序",   "前言", "序言",     "原序",     "自序",     "代序",     "再版序",   "修订版序",
        "中文版序", "英文版序", "总序", "出版说明", "内容提要", "编辑说明", "再版前言", "修订前言",
    };
    for (int i = 0; i < dimof(keys); i++) {
        if (str::Eq(s, keys[i]) || str::StartsWith(s, keys[i])) {
            return true;
        }
    }
    return str::Eq(s, "序");
}

static void TocCalibChooseColumn(TocCalibRow* row, bool pdf) {
    if (!row) {
        return;
    }
    row->colChosen = true;
    row->editPdf = pdf;
}

int TocCalibColumnsDx(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    RECT row = rc;
    row.top = 0;
    row.bottom = DpiScale(hwnd, 22);
    TocCalibSpinLayout L = TocCalibMakeLayout(hwnd, row, true);
    int pad = DpiScale(hwnd, 4);
    return rc.right - L.prPrev.left + pad;
}

static bool TocCalibPtInRect(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

static bool TocCalibEditingField(MainWindow* win, TocItem* item, bool printed) {
    return gCalibPageEdit.hwnd && gCalibPageEdit.win == win && item && item->id == gCalibPageEdit.tocId &&
           gCalibPageEdit.printed == printed;
}

static void TocCalibDrawSpinBtn(HDC hdc, HWND hwnd, const RECT& rc, bool plus, bool enabled) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    COLORREF txt = enabled ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor();
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txt);
    RECT tr = rc;
    int raise = DpiScale(hwnd, 2);
    tr.top -= raise;
    tr.bottom -= raise;
    DrawTextW(hdc, plus ? L"+" : L"-", 1, &tr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

static void TocCalibDrawPageField(HDC hdc, const RECT& rc, const WCHAR* text, bool empty, bool editing, bool enabled) {
    if (rc.right <= rc.left || rc.bottom <= rc.top || editing) {
        return;
    }
    COLORREF bg = ThemeFindEditBackgroundColor();
    COLORREF txt = (!enabled || empty) ? ThemeWindowTextDisabledColor() : ThemeWindowTextColor();
    COLORREF bd = AccentColor(ThemeWindowTextColor(), ThemeUsesDarkChrome() ? 0 : (!enabled || empty ? 58 : 40),
                              ThemeUsesDarkChrome() ? (!enabled || empty ? 42 : 28) : 0);
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(hdc, &rc, br);
    DeleteObject(br);
    HBRUSH bdBr = CreateSolidBrush(bd);
    FrameRect(hdc, &rc, bdBr);
    DeleteObject(bdBr);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txt);
    RECT inner = rc;
    inner.left += 2;
    inner.right -= 2;
    DrawTextW(hdc, text, -1, &inner, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

static void TocCalibDrawIconBtn(HDC hdc, HWND hwnd, const RECT& rc, TbIcon icon, bool enabled) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    COLORREF bg = GetPixel(hdc, rc.left > 0 ? rc.left - 1 : rc.left, (rc.top + rc.bottom) / 2);
    if (bg == CLR_INVALID) {
        COLORREF txtDummy = 0;
        ThemeSidebarColors(bg, txtDummy);
    }
    COLORREF fg = enabled ? ThemeWindowTextColor() : ThemeWindowTextDisabledColor();
    int pad = DpiScale(hwnd, 1);
    int dx = (rc.right - rc.left) - 2 * pad;
    int dy = (rc.bottom - rc.top) - 2 * pad;
    if (dx < 8 || dy < 8) {
        dx = rc.right - rc.left;
        dy = rc.bottom - rc.top;
        pad = 0;
    }
    DrawSvgIcon(hdc, Rect(rc.left + pad, rc.top + pad, dx, dy), icon, fg, bg);
}

static void TocCalibDrawPageGroup(HDC hdc, HWND hwnd, const RECT& prev, const RECT& field, const RECT& next, int value,
                                  const char* label, bool allowEmpty, bool editing, bool enabled) {
    TocCalibDrawSpinBtn(hdc, hwnd, prev, false, enabled);
    TocCalibDrawSpinBtn(hdc, hwnd, next, true, enabled);
    WCHAR buf[16];
    const WCHAR* text = L"";
    bool empty = true;
    if (label && label[0] && !TocCalibHasPrinted(value)) {
        text = ToWStrTemp(label);
        empty = !text || !text[0];
    } else if (allowEmpty ? value != 0 : value >= 1) {
        if (value > 0) {
            _snwprintf(buf, dimof(buf), L"%d", value);
            text = buf;
            empty = false;
        }
    }
    TocCalibDrawPageField(hdc, field, text, empty, editing, enabled);
}

void TocCalibDrawColumns(HDC hdc, HWND hwnd, const RECT& rcRow, TocItem* item, MainWindow* win, bool selected) {
    (void)selected;
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* live = dm ? dm->GetEngine() : nullptr;
    if (!s || !live || s->engine != live) {
        return;
    }
    TocCalibRow* row = TocCalibRowForTocItem(win, item);
    int pdf = row && row->item ? row->item->pageNo : (item ? item->pageNo : 0);
    int printed = 0;
    const char* printedLab = nullptr;
    if (row && row->item && TocCalibHasPrinted(row->item->printedPage)) {
        printed = row->item->printedPage;
    } else if (row && row->item && row->item->printedLabel && row->item->printedLabel[0]) {
        printedLab = row->item->printedLabel;
    } else {
        printed = TocCalibGuessPrinted(s, row, pdf);
        if (!TocCalibHasPrinted(printed)) {
            printedLab = TocCalibPageLabelText(s, pdf);
        }
    }
    TocCalibSpinLayout L = TocCalibMakeLayout(hwnd, rcRow, false);
    TocCalibDrawIconBtn(hdc, hwnd, L.locate, TbIcon::MapPin, true);
    TocCalibDrawIconBtn(hdc, hwnd, L.associate, TbIcon::Link, true);
    TocCalibDrawIconBtn(hdc, hwnd, L.merge, TbIcon::MergeUp, true);
    TocCalibDrawIconBtn(hdc, hwnd, L.del, TbIcon::Close, true);
    TocCalibDrawPageGroup(hdc, hwnd, L.prPrev, L.prField, L.prNext, printed, printedLab, true,
                          TocCalibEditingField(win, item, true), true);
}

enum class TocCalibHit {
    None,
    LocateBody,
    AssociateView,
    MergeNext,
    DeleteRow,
    PrintedUp,
    PrintedDown,
    PrintedField,
    PdfUp,
    PdfDown,
    PdfField,
};

static bool TocCalibHitEnabled(TocCalibHit hit, bool editPdf) {
    (void)editPdf;
    return hit != TocCalibHit::None;
}

static TocCalibHit TocCalibHitTest(HWND hwnd, int x, int y, const RECT& rcRow, bool editPdf) {
    (void)editPdf;
    TocCalibSpinLayout L = TocCalibMakeLayout(hwnd, rcRow, editPdf);
    if (TocCalibPtInRect(L.locate, x, y)) {
        return TocCalibHit::LocateBody;
    }
    if (TocCalibPtInRect(L.associate, x, y)) {
        return TocCalibHit::AssociateView;
    }
    if (TocCalibPtInRect(L.merge, x, y)) {
        return TocCalibHit::MergeNext;
    }
    if (TocCalibPtInRect(L.del, x, y)) {
        return TocCalibHit::DeleteRow;
    }
    if (TocCalibPtInRect(L.prNext, x, y)) {
        return TocCalibHit::PrintedUp;
    }
    if (TocCalibPtInRect(L.prPrev, x, y)) {
        return TocCalibHit::PrintedDown;
    }
    if (TocCalibPtInRect(L.prField, x, y)) {
        return TocCalibHit::PrintedField;
    }
    return TocCalibHit::None;
}

static void TocCalibSpinPrinted(TocCalibRow* row, int delta, TocCalibSession* s) {
    if (!row || !row->item || !s) {
        return;
    }
    if (row->item->printedLabel && row->item->printedLabel[0] && !TocCalibHasPrinted(row->item->printedPage)) {
        return;
    }
    int printed = TocCalibGuessPrinted(s, row);
    if (!TocCalibHasPrinted(printed)) {
        if (delta < 1) {
            return;
        }
        printed = 1;
        TocCalibClearPrintedLabel(row->item);
    } else {
        printed += delta;
        if (printed < 1) {
            printed = 0;
        }
    }
    int lim = s->nPages > 0 ? s->nPages : 99999;
    if (printed > lim) {
        printed = lim;
    }
    TocCalibChooseColumn(row, false);
    row->item->printedPage = printed;
    row->userSet = true;
    row->pdfPinned = false;
    row->item->x = 0;
    row->item->y = 0;
    if (!TocCalibHasPrinted(printed)) {
        if (row->origPageNo > 0) {
            row->item->pageNo = row->origPageNo;
        }
        return;
    }
    TocCalibClearPrintedLabel(row->item);
    if (!TocCalibHaveOffset(s)) {
        return;
    }
    row->item->pageNo = TocCalibPredPdf(printed, s->map.offset, s->nPages);
}

static int TocCalibParsePage(const char* s, bool allowEmpty) {
    if (!s) {
        return allowEmpty ? 0 : -1;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (!*s) {
        return allowEmpty ? 0 : -1;
    }
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
        if (!*s) {
            return -1;
        }
    }
    int n = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        n = n * 10 + (*p - '0');
        if (n > 99999) {
            return -1;
        }
    }
    if (neg) {
        n = -n;
    }
    if (!allowEmpty && n < 1) {
        return -1;
    }
    return n;
}

static void TocCalibJumpToPdfPage(MainWindow* win, int page) {
    if (!win || !win->ctrl || page < 1) {
        return;
    }
    int n = win->ctrl->PageCount();
    if (n > 0 && page > n) {
        page = n;
    }
    bool prevKeep = win->tocKeepSelection;
    win->tocKeepSelection = true;
    win->ctrl->GoToPage(page, false);
    win->tocKeepSelection = prevKeep;
}

static void TocCalibJumpToPdfPoint(MainWindow* win, int page, float x, float y) {
    if (!win || !win->ctrl || page < 1) {
        return;
    }
    int n = win->ctrl->PageCount();
    if (n > 0 && page > n) {
        page = n;
    }
    bool prevKeep = win->tocKeepSelection;
    win->tocKeepSelection = true;
    if (x != 0 || y != 0) {
        win->ctrl->ScrollTo(page, RectF(x, y, 0, 0), 0);
    } else {
        win->ctrl->GoToPage(page, true);
    }
    win->tocKeepSelection = prevKeep;
}

static void TocCalibApplyTypedPage(MainWindow* win, int tocId, bool printed, int value) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        return;
    }
    TocCalibRebind(win);
    TocItem* item = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, tocId) : nullptr;
    TocCalibRow* row = item ? TocCalibRowForTocItem(win, item) : nullptr;
    if (!row && tocId) {
        for (int i = 0; i < s->rows.Size(); i++) {
            if (s->rows[i].toc.tocId == tocId) {
                row = &s->rows[i];
                break;
            }
        }
    }
    if (!row || !row->item) {
        return;
    }
    if (!printed) {
        if (value < 1) {
            return;
        }
        if (value == row->item->pageNo) {
            TocCalibChooseColumn(row, true);
            return;
        }
        int lim = s->nPages > 0 ? s->nPages : 99999;
        if (value > lim) {
            value = lim;
        }
        TocCalibRemember(s);
        TocCalibChooseColumn(row, true);
        row->item->pageNo = value;
        row->userSet = true;
        TocCalibLiveApply(win, false);
        return;
    }
    int cur = row->item->printedPage;
    if (value == cur && !(row->item->printedLabel && row->item->printedLabel[0])) {
        TocCalibChooseColumn(row, false);
        return;
    }
    TocCalibRemember(s);
    TocCalibChooseColumn(row, false);
    TocCalibClearPrintedLabel(row->item);
    if (value == 0) {
        row->item->printedPage = 0;
        row->userSet = true;
        row->pdfPinned = false;
        row->item->x = 0;
        row->item->y = 0;
        if (row->origPageNo > 0) {
            row->item->pageNo = row->origPageNo;
        }
    } else if (value > 0) {
        TocCalibSpinPrinted(row, value - cur, s);
        row->item->printedPage = value;
        row->pdfPinned = false;
        row->item->x = 0;
        row->item->y = 0;
        if (TocCalibHaveOffset(s)) {
            row->item->pageNo = TocCalibPredPdf(value, s->map.offset, s->nPages);
        }
    }
    TocCalibLiveApply(win, false);
}

static void TocCalibApplyTypedPrintedLabel(MainWindow* win, int tocId, const char* label) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !label || !label[0]) {
        return;
    }
    TocCalibRebind(win);
    TocItem* item = tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, tocId) : nullptr;
    TocCalibRow* row = item ? TocCalibRowForTocItem(win, item) : nullptr;
    if (!row && tocId) {
        for (int i = 0; i < s->rows.Size(); i++) {
            if (s->rows[i].toc.tocId == tocId) {
                row = &s->rows[i];
                break;
            }
        }
    }
    if (!row || !row->item) {
        return;
    }
    if (row->item->printedPage == 0 && row->item->printedLabel && str::Eq(row->item->printedLabel, label)) {
        TocCalibChooseColumn(row, false);
        return;
    }
    TocCalibRemember(s);
    TocCalibChooseColumn(row, false);
    row->item->printedPage = 0;
    str::ReplaceWithCopy(&row->item->printedLabel, label);
    row->userSet = true;
    if (!row->pdfPinned && row->origPageNo > 0) {
        row->item->pageNo = row->origPageNo;
    }
    TocCalibLiveApply(win, false);
}

static int TocCalibNextVisibleTocId(MainWindow* win, int tocId);
static void TocCalibStartPrintedEdit(MainWindow* win, int tocId);

static LRESULT CALLBACK TocCalibPageEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC prev = gCalibPageEdit.prev;
    if (msg == WM_CHAR) {
        if (wp == VK_RETURN) {
            MainWindow* win = gCalibPageEdit.win;
            int nextId = 0;
            if (gCalibPageEdit.printed && win) {
                nextId = TocCalibNextVisibleTocId(win, gCalibPageEdit.tocId);
            }
            TocCalibClosePageEdit(true);
            if (nextId) {
                TocCalibStartPrintedEdit(win, nextId);
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            TocCalibClosePageEdit(false);
            return 0;
        }
        if (gCalibPageEdit.printed) {
            if (wp >= 32 && !((wp >= '0' && wp <= '9') || (wp >= 'A' && wp <= 'Z') || (wp >= 'a' && wp <= 'z'))) {
                return 0;
            }
        }
    }
    if (msg == WM_KILLFOCUS) {
        TocCalibClosePageEdit(true);
        return 0;
    }
    if (prev) {
        return CallWindowProc(prev, hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

bool TocCalibColorPageEdit(HWND edit, HDC hdc, HBRUSH* brOut) {
    if (!edit || edit != gCalibPageEdit.hwnd || !hdc || !brOut) {
        return false;
    }
    COLORREF bg = ThemeFindEditBackgroundColor();
    SetBkColor(hdc, bg);
    SetTextColor(hdc, ThemeWindowTextColor());
    static HBRUSH br = nullptr;
    static COLORREF last = (COLORREF)-1;
    if (bg != last) {
        if (br) {
            DeleteObject(br);
        }
        br = CreateSolidBrush(bg);
        last = bg;
    }
    *brOut = br;
    return br != nullptr;
}

void TocCalibClosePageEdit(bool commit) {
    if (gCalibPageEdit.closing || !gCalibPageEdit.hwnd) {
        return;
    }
    gCalibPageEdit.closing = true;
    MainWindow* win = gCalibPageEdit.win;
    int tocId = gCalibPageEdit.tocId;
    bool printed = gCalibPageEdit.printed;
    char buf[32]{};
    if (commit) {
        TempStr text = HwndGetTextTemp(gCalibPageEdit.hwnd);
        if (text) {
            str::BufSet(buf, dimof(buf), text);
        }
    }
    HWND h = gCalibPageEdit.hwnd;
    gCalibPageEdit.hwnd = nullptr;
    gCalibPageEdit.prev = nullptr;
    gCalibPageEdit.win = nullptr;
    gCalibPageEdit.tocId = 0;
    DestroyWindow(h);
    gCalibPageEdit.closing = false;
    if (!commit || !win) {
        return;
    }
    if (printed) {
        int pr = 0;
        char* lab = nullptr;
        if (!TocCalibParsePrintedText(buf, &pr, &lab)) {
            return;
        }
        if (lab && lab[0]) {
            TocCalibApplyTypedPrintedLabel(win, tocId, lab);
            str::Free(lab);
            return;
        }
        str::Free(lab);
        if (pr < 0) {
            return;
        }
        TocCalibApplyTypedPage(win, tocId, true, pr);
        return;
    }
    int value = TocCalibParsePage(buf, false);
    if (value < 0) {
        return;
    }
    TocCalibApplyTypedPage(win, tocId, false, value);
}

static void TocCalibBeginPageEdit(MainWindow* win, TocItem* item, const RECT& field, bool printed) {
    if (!win || !win->tocTreeView || !item) {
        return;
    }
    if (TocCalibEditingField(win, item, printed)) {
        return;
    }
    HWND parent = win->tocTreeView->hwnd;
    RECT rc = field;
    if (rc.right - rc.left < 8 || rc.bottom - rc.top < 8) {
        return;
    }
    HWND h = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL, rc.left,
                             rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, GetModuleHandle(nullptr),
                             nullptr);
    if (!h) {
        return;
    }
    HFONT font = (HFONT)SendMessageW(parent, WM_GETFONT, 0, 0);
    if (font) {
        SetWindowFont(h, font, TRUE);
    }
    WindowTab* tab = win->CurrentTab();
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    TocCalibRow* row = TocCalibRowForTocItem(win, item);
    if (!row) {
        TocCalibRebind(win);
        row = TocCalibRowForTocItem(win, item);
    }
    int value = 0;
    const char* printedLab = nullptr;
    if (printed) {
        int pdf = row && row->item ? row->item->pageNo : (item ? item->pageNo : 0);
        if (row && row->item && TocCalibHasPrinted(row->item->printedPage)) {
            value = row->item->printedPage;
        } else if (row && row->item && row->item->printedLabel && row->item->printedLabel[0]) {
            printedLab = row->item->printedLabel;
        } else {
            value = TocCalibGuessPrinted(s, row, pdf);
            if (!TocCalibHasPrinted(value)) {
                printedLab = TocCalibPageLabelText(s, pdf);
            }
        }
    } else if (row && row->item) {
        value = row->item->pageNo;
    } else if (item) {
        value = item->pageNo;
    }
    WCHAR buf[16]{};
    if (printed && printedLab && printedLab[0]) {
        SetWindowTextW(h, ToWStrTemp(printedLab));
    } else if (printed ? value > 0 : value > 0) {
        _snwprintf(buf, dimof(buf), L"%d", value);
        SetWindowTextW(h, buf);
    }
    gCalibPageEdit.hwnd = h;
    gCalibPageEdit.win = win;
    gCalibPageEdit.tocId = item->id;
    gCalibPageEdit.printed = printed;
    gCalibPageEdit.prev = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)TocCalibPageEditProc);
    SendMessageW(h, EM_SETSEL, 0, -1);
    SetFocus(h);
}

static int TocCalibNextVisibleTocId(MainWindow* win, int tocId) {
    if (!win || !win->tocTreeView || !tocId) {
        return 0;
    }
    WindowTab* tab = win->CurrentTab();
    TocItem* item =
        tab && tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, tocId) : nullptr;
    if (!item) {
        return 0;
    }
    HTREEITEM h = win->tocTreeView->GetHandleByTreeItem((TreeItem)item);
    if (!h) {
        return 0;
    }
    HTREEITEM next = TreeView_GetNextVisible(win->tocTreeView->hwnd, h);
    if (!next) {
        return 0;
    }
    TocItem* n = (TocItem*)win->tocTreeView->GetTreeItemByHandle(next);
    return n ? n->id : 0;
}

static void TocCalibStartPrintedEdit(MainWindow* win, int tocId) {
    if (!win || !win->tocTreeView || !tocId) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    TocItem* item =
        tab && tab->currToc && tab->currToc->root ? TocCalibFindById(tab->currToc->root->child, tocId) : nullptr;
    if (!item) {
        return;
    }
    HTREEITEM h = win->tocTreeView->GetHandleByTreeItem((TreeItem)item);
    if (!h) {
        return;
    }
    TreeView_EnsureVisible(win->tocTreeView->hwnd, h);
    RECT rc{};
    if (!TreeView_GetItemRect(win->tocTreeView->hwnd, h, &rc, FALSE)) {
        return;
    }
    win->tocSuppressGoTo = true;
    win->tocTreeView->SelectItem((TreeItem)item);
    win->tocSuppressGoTo = false;
    win->tocSelectedIds.Reset();
    if (item->id) {
        win->tocSelectedIds.Append(item->id);
        win->tocAnchorId = item->id;
    }
    TocCalibChooseColumn(TocCalibRowForTocItem(win, item), false);
    InvalidateRect(win->tocTreeView->hwnd, &rc, TRUE);
    TocCalibSpinLayout L = TocCalibMakeLayout(win->tocTreeView->hwnd, rc, false);
    TocCalibBeginPageEdit(win, item, L.prField, true);
}

bool TocCalibHandleRowClick(MainWindow* win, TocItem* item, int x, int y, const RECT& rcRow) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !item || !win->tocTreeView) {
        return false;
    }
    TocCalibRow* row = TocCalibRowForTocItem(win, item);
    if (!row) {
        TocCalibRebind(win);
        row = TocCalibRowForTocItem(win, item);
    }
    TocCalibHit hit = TocCalibHitTest(win->tocTreeView->hwnd, x, y, rcRow, true);
    if (!TocCalibHitEnabled(hit, true)) {
        return false;
    }
    if (!row) {
        return true;
    }
    TocCalibChooseColumn(row, false);
    if (hit == TocCalibHit::LocateBody) {
        int tocId = item->id;
        TocCalibClosePageEdit(true);
        s = win->CurrentTab() ? win->CurrentTab()->tocCalib : nullptr;
        TocItem* again = nullptr;
        if (win->CurrentTab() && win->CurrentTab()->currToc && win->CurrentTab()->currToc->root) {
            again = TocCalibFindById(win->CurrentTab()->currToc->root->child, tocId);
        }
        row = again ? TocCalibRowForTocItem(win, again) : nullptr;
        if (!row || !s) {
            return true;
        }
        TocCalibLocateRowInBody(win, row);
        return true;
    }
    if (hit == TocCalibHit::AssociateView) {
        int tocId = item->id;
        TocCalibClosePageEdit(true);
        s = win->CurrentTab() ? win->CurrentTab()->tocCalib : nullptr;
        TocItem* again = nullptr;
        if (win->CurrentTab() && win->CurrentTab()->currToc && win->CurrentTab()->currToc->root) {
            again = TocCalibFindById(win->CurrentTab()->currToc->root->child, tocId);
        }
        row = again ? TocCalibRowForTocItem(win, again) : nullptr;
        if (!row || !s) {
            return true;
        }
        TocCalibPinRowToView(win, row);
        return true;
    }
    if (hit == TocCalibHit::MergeNext || hit == TocCalibHit::DeleteRow) {
        int tocId = item->id;
        TocCalibClosePageEdit(true);
        s = win->CurrentTab() ? win->CurrentTab()->tocCalib : nullptr;
        TocItem* again = nullptr;
        if (win->CurrentTab() && win->CurrentTab()->currToc && win->CurrentTab()->currToc->root) {
            again = TocCalibFindById(win->CurrentTab()->currToc->root->child, tocId);
        }
        row = again ? TocCalibRowForTocItem(win, again) : nullptr;
        if (!row || !s) {
            return true;
        }
        if (hit == TocCalibHit::MergeNext) {
            if (row->toc.tocId) {
                win->tocAnchorId = row->toc.tocId;
            }
            TocCalibRemember(s);
            if (!TocCalibMergeWithNext(s, row)) {
                return true;
            }
            TocCalibLiveApply(win);
            return true;
        }
        TocCalibRemember(s);
        ExtractedTocItem* doomed = row->item;
        int keepId = 0;
        int idx = TocCalibRowIndexOf(s, row);
        if (idx > 0 && s->rows[idx - 1].toc.tocId) {
            keepId = s->rows[idx - 1].toc.tocId;
        } else if (idx + 1 < s->rows.Size() && s->rows[idx + 1].toc.tocId) {
            keepId = s->rows[idx + 1].toc.tocId;
        }
        if (!TocCalibDeleteAndPromote(s, doomed)) {
            return true;
        }
        TocCalibCollectRows(s);
        win->tocAnchorId = keepId;
        TocCalibLiveApply(win);
        return true;
    }
    if (hit == TocCalibHit::PrintedField) {
        if (TocCalibEditingField(win, item, true)) {
            return true;
        }
        int tocId = item->id;
        TocCalibClosePageEdit(true);
        TocCalibStartPrintedEdit(win, tocId);
        return true;
    }
    int tocId = item->id;
    TocCalibClosePageEdit(true);
    s = win->CurrentTab() ? win->CurrentTab()->tocCalib : nullptr;
    TocItem* again = nullptr;
    if (win->CurrentTab() && win->CurrentTab()->currToc && win->CurrentTab()->currToc->root) {
        again = TocCalibFindById(win->CurrentTab()->currToc->root->child, tocId);
    }
    row = again ? TocCalibRowForTocItem(win, again) : nullptr;
    if (!row || !s) {
        return true;
    }
    TocCalibRemember(s);
    if (hit == TocCalibHit::PrintedUp) {
        TocCalibSpinPrinted(row, 1, s);
    } else if (hit == TocCalibHit::PrintedDown) {
        TocCalibSpinPrinted(row, -1, s);
    }
    TocCalibLiveApply(win, false);
    return true;
}

static TocItem* TocCalibItemAtPoint(MainWindow* win, HWND hwnd, POINT pt, RECT* rcRowOut) {
    if (!win || !win->tocTreeView || win->tocTreeView->hwnd != hwnd) {
        return nullptr;
    }
    TVHITTESTINFO ht{};
    ht.pt = pt;
    TreeView_HitTest(hwnd, &ht);
    HTREEITEM h = ht.hItem;
    if (!h) {
        h = TreeView_GetFirstVisible(hwnd);
        while (h) {
            RECT r{};
            if (TreeView_GetItemRect(hwnd, h, &r, FALSE) && pt.y >= r.top && pt.y < r.bottom) {
                break;
            }
            h = TreeView_GetNextVisible(hwnd, h);
        }
    }
    if (!h) {
        return nullptr;
    }
    RECT rc{};
    if (!TreeView_GetItemRect(hwnd, h, &rc, FALSE)) {
        return nullptr;
    }
    if (rcRowOut) {
        *rcRowOut = rc;
    }
    return (TocItem*)win->tocTreeView->GetTreeItemByHandle(h);
}

bool TocCalibHandleTreeClick(MainWindow* win, HWND hwnd, POINT pt) {
    RECT rcRow{};
    TocItem* item = TocCalibItemAtPoint(win, hwnd, pt, &rcRow);
    if (!item) {
        return false;
    }
    return TocCalibHandleRowClick(win, item, pt.x, pt.y, rcRow);
}

bool TocCalibIsPageFieldAt(MainWindow* win, HWND hwnd, POINT pt) {
    RECT rcRow{};
    TocItem* item = TocCalibItemAtPoint(win, hwnd, pt, &rcRow);
    if (!item) {
        return false;
    }
    TocCalibHit hit = TocCalibHitTest(hwnd, pt.x, pt.y, rcRow, true);
    return hit == TocCalibHit::PrintedField;
}

bool TocCalibIsPageControlAt(MainWindow* win, HWND hwnd, POINT pt) {
    RECT rcRow{};
    TocItem* item = TocCalibItemAtPoint(win, hwnd, pt, &rcRow);
    if (!item) {
        return false;
    }
    return TocCalibHitEnabled(TocCalibHitTest(hwnd, pt.x, pt.y, rcRow, true), true);
}

const char* TocCalibRowControlTip(MainWindow* win, HWND hwnd, POINT pt) {
    RECT rcRow{};
    TocItem* item = TocCalibItemAtPoint(win, hwnd, pt, &rcRow);
    if (!item) {
        return nullptr;
    }
    TocCalibHit hit = TocCalibHitTest(hwnd, pt.x, pt.y, rcRow, true);
    if (hit == TocCalibHit::LocateBody) {
        return _TRA("Find TOC Item in Body");
    }
    if (hit == TocCalibHit::AssociateView) {
        return _TRA("Link to current page");
    }
    if (hit == TocCalibHit::MergeNext) {
        return _TRA("Merge with next TOC item");
    }
    if (hit == TocCalibHit::DeleteRow) {
        return _TRA("Delete");
    }
    return nullptr;
}

static bool TocCalibLineHasTocLeader(const char* t) {
    if (!t) {
        return false;
    }
    return str::Find(t, "......") || str::Find(t, "．．．．") || str::Find(t, "····") || str::Find(t, "\xE2\x80\xA6") ||
           str::Find(t, "\xE2\x8B\xAF") || str::Find(t, "……");
}

static bool TocCalibLineHasPrintedPageTok(const char* t) {
    if (!t || !t[0]) {
        return false;
    }
    int len = (int)str::Len(t);
    int i = len;
    int cp = Utf8CodepointPrev(t, len, i);
    if (cp == ')' || cp == 0xFF09) {
        cp = Utf8CodepointPrev(t, len, i);
    }
    int n = 0;
    while (cp >= '0' && cp <= '9' && n < 4) {
        n++;
        if (i < 1) {
            break;
        }
        cp = Utf8CodepointPrev(t, len, i);
    }
    return n >= 1 && n <= 3;
}

static int TocCalibPageTocScore(const Vec<EngineMupdfPageLine>* lines) {
    if (!lines) {
        return 0;
    }
    int leaders = 0;
    int numbered = 0;
    int pageTok = 0;
    bool hasWord = false;
    for (int i = 0; i < lines->Size(); i++) {
        const char* t = lines->At(i).text;
        if (!t || !t[0]) {
            continue;
        }
        if (str::Find(t, "目录") || str::Find(t, "目次") || str::FindI(t, "contents")) {
            hasWord = true;
        }
        if (TocCalibLineHasTocLeader(t)) {
            leaders++;
        }
        if (TocCalibLineHasPrintedPageTok(t)) {
            pageTok++;
        }
        int k = 0;
        int len = (int)str::Len(t);
        while (k < len && (t[k] == ' ' || t[k] == '\t')) {
            k++;
        }
        if (k < len && t[k] >= '1' && t[k] <= '9') {
            while (k < len && t[k] >= '0' && t[k] <= '9') {
                k++;
            }
            if (k < len && (t[k] == '.' || (unsigned char)t[k] == 0xEF)) {
                numbered++;
            }
        }
    }
    int sc = leaders + pageTok;
    if (numbered >= 4) {
        sc += 4;
    } else {
        sc += numbered / 2;
    }
    if (hasWord) {
        sc += 8;
    }
    return sc;
}

static void TocCalibEnsureTocRange(TocCalibSession* s) {
    if (!s || !s->engine) {
        return;
    }
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    int maxP = s->nPages < 30 ? s->nPages : 30;
    int start = s->tocPage;
    if (start < 1) {
        for (int p = 1; p <= maxP; p++) {
            const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
            if (TocCalibPageTocScore(lines) >= 4) {
                start = p;
                break;
            }
        }
    }
    if (start < 1) {
        TocCalibFreePageCache(cache);
        return;
    }
    s->tocPage = start;
    int end = s->tocEnd > start ? s->tocEnd : start;
    int miss = 0;
    int last = start + 16;
    if (s->nPages > 0 && last > s->nPages) {
        last = s->nPages;
    }
    for (int p = start; p <= last; p++) {
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (TocCalibPageTocScore(lines) >= 3) {
            end = p;
            miss = 0;
        } else if (p > end) {
            miss++;
            if (miss >= 2) {
                break;
            }
        }
    }
    if (end > s->tocEnd) {
        s->tocEnd = end;
    }
    TocCalibFreePageCache(cache);
}

static bool TocCalibFindTitleOnTocPages(TocCalibSession* s, const char* title, int* pageOut, float* xOut, float* yOut) {
    if (!s || !title || !title[0] || !pageOut) {
        return false;
    }
    if (TocCalibIsContentsTitle(title)) {
        return false;
    }
    TocCalibEnsureTocRange(s);
    int lo = s->tocPage;
    int hi = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    if (lo < 1) {
        return false;
    }
    if (hi < lo) {
        hi = lo;
    }
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    int bestSc = 0;
    int bestPage = 0;
    float bestX = 0;
    float bestY = 0;
    for (int p = lo; p <= hi; p++) {
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (!lines) {
            continue;
        }
        for (int i = 0; i < lines->Size(); i++) {
            const EngineMupdfPageLine& ln = lines->At(i);
            if (!ln.text || !ln.text[0]) {
                continue;
            }
            int sc = TocCalibTitleMatchScore(ln.text, title);
            if (sc > bestSc) {
                bestSc = sc;
                bestPage = p;
                bestX = ln.x;
                bestY = ln.y;
            }
        }
    }
    TocCalibFreePageCache(cache);
    if (bestPage < 1 || bestSc < 2) {
        return false;
    }
    *pageOut = bestPage;
    if (xOut) {
        *xOut = bestX;
    }
    if (yOut) {
        *yOut = bestY;
    }
    return true;
}

static void TocCalibJumpToRowContents(MainWindow* win, TocCalibRow* row) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !win->ctrl) {
        return;
    }
    ExtractedTocItem* it = row ? row->item : nullptr;
    int page = 0;
    float x = 0;
    float y = 0;
    if (it && it->tocPageNo > 0) {
        page = it->tocPageNo;
        x = it->tocX;
        y = it->tocY;
    } else if (it && TocCalibFindTitleOnTocPages(s, it->rawTitle && it->rawTitle[0] ? it->rawTitle : it->title, &page,
                                                 &x, &y)) {
        it->tocPageNo = page;
        it->tocX = x;
        it->tocY = y;
    } else {
        TocCalibEnsureTocRange(s);
        page = s->tocPage;
        if (page < 1) {
            page = s->tocEnd;
        }
        if (page < 1 && it && TocCalibIsContentsTitle(it->title) && it->pageNo > 0) {
            page = it->pageNo;
        }
    }
    if (page < 1 && s->nPages > 0) {
        page = 1;
    }
    if (page < 1) {
        return;
    }
    TocCalibJumpToPdfPoint(win, page, x, y);
}

void TocCalibJumpToContents(MainWindow* win) {
    TocCalibJumpToRowContents(win, TocCalibSelectedRow(win));
}

void TocCalibJumpToItemContents(MainWindow* win, TocItem* item) {
    TocCalibJumpToRowContents(win, TocCalibRowForTocItem(win, item));
}

bool TocCalibIsActive(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !tab->tocCalib) {
        return false;
    }
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    return engine && tab->tocCalib->engine == engine;
}

void RelayoutTocCalib(MainWindow* win) {
    if (!win || !win->tocCalibBar || !win->hwndTocBox || !win->tocLabelWithClose) {
        return;
    }
    if (!win->tocCalibBar->IsVisible()) {
        return;
    }
    Size labelSize = win->tocLabelWithClose->GetIdealSize();
    Rect rc = WindowRect(win->hwndTocBox);
    int barDy = TocCalibBarDy(win);
    int y = rc.dy - barDy;
    if (y < labelSize.dy) {
        y = labelSize.dy;
    }
    win->tocCalibBar->LayoutIn(0, y, rc.dx, barDy);
}

static void TocCalibEnterSinglePage(MainWindow* win) {
    if (!win || !win->ctrl || !win->IsDocLoaded()) {
        return;
    }
    DisplayMode cur = win->ctrl->GetDisplayMode();
    if (cur == DisplayMode::SinglePage) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (s && !s->restoreDisplayMode) {
        s->savedDisplayMode = (int)cur;
        s->restoreDisplayMode = true;
    }
    SwitchToDisplayMode(win, DisplayMode::SinglePage, false);
}

static void TocCalibRestoreDisplayMode(MainWindow* win, TocCalibSession* s) {
    if (!win || !s || !s->restoreDisplayMode || !win->ctrl || !win->IsDocLoaded()) {
        return;
    }
    DisplayMode want = (DisplayMode)s->savedDisplayMode;
    s->restoreDisplayMode = false;
    if (want != DisplayMode::Automatic && want != win->ctrl->GetDisplayMode()) {
        SwitchToDisplayMode(win, want, false);
    }
}

void HideTocCalib(MainWindow* win) {
    TocCalibClosePageEdit(false);
    if (!win || !win->tocCalibBar) {
        return;
    }
    HwndSetVisibility(win->tocCalibBar->hwnd, false);
    RelayoutTocContainer(win);
    FlushTocTreeWrapHeights(win);
}

void DeleteTocCalibUi(MainWindow* win) {
    if (!win || !win->tocCalibBar) {
        return;
    }
    delete win->tocCalibBar;
    win->tocCalibBar = nullptr;
}

void ShowTocCalib(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!win || !tab || !tab->tocCalib) {
        HideTocCalib(win);
        return;
    }
    if (!win->tocCalibBar) {
        auto* w = new TocCalibBar();
        if (!w->Create(win)) {
            delete w;
            return;
        }
        win->tocCalibBar = w;
    }
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    HwndSetVisibility(win->tocCalibBar->hwnd, true);
    TocCalibUpdateTheme(win);
    RelayoutTocContainer(win);
    RelayoutTocCalib(win);
    FlushTocTreeWrapHeights(win);
    InvalidateTocTree(win);
    TocCalibEnterSinglePage(win);
}

bool StartTocCalib(MainWindow* win, Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk) {
    if (!win) {
        DeleteExtractedTocItems(roots);
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        DeleteExtractedTocItems(roots);
        return false;
    }
    DeleteTocCalibSession(tab->tocCalib);
    tab->tocCalib = TocCalibSessionFromExtracted(roots, engine, persistToDisk);
    if (!tab->tocCalib || tab->tocCalib->rows.Size() < 1) {
        DeleteTocCalibSession(tab->tocCalib);
        tab->tocCalib = nullptr;
        return false;
    }
    TocTree* cur = tab->ctrl ? tab->ctrl->GetToc() : nullptr;
    TocCalibCloneOutline(cur, tab->tocCalib->backup);
    char* err = nullptr;
    bool ok = EngineMupdfReplacePdfToc(engine, tab->tocCalib->roots, &err);
    str::Free(err);
    if (!ok) {
        DeleteTocCalibSession(tab->tocCalib);
        tab->tocCalib = nullptr;
        return false;
    }
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
    TocCalibBindToTree(win);
    ShowTocCalib(win);
    return true;
}

bool StartTocCalibFromExisting(MainWindow* win) {
    if (!win) {
        return false;
    }
    if (TocCalibIsActive(win)) {
        ShowTocCalib(win);
        return true;
    }
    DisplayModel* dm = win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || !EngineMupdfCanEditPdfToc(engine)) {
        return false;
    }
    WindowTab* tab = win->CurrentTab();
    TocTree* cur = tab && tab->ctrl ? tab->ctrl->GetToc() : nullptr;
    if (!cur || !cur->root || !cur->root->child) {
        return false;
    }
    Vec<ExtractedTocItem*> roots;
    TocCalibCloneOutline(cur, roots);
    if (roots.Size() < 1) {
        return false;
    }
    return StartTocCalib(win, roots, engine, true);
}

void TocCalibOnTabSwitch(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (tab && tab->tocCalib) {
        ShowTocCalib(win);
    } else {
        HideTocCalib(win);
    }
}

void CloseTocCalibForTab(WindowTab* tab) {
    if (!tab) {
        return;
    }
    TocCalibSession* s = tab->tocCalib;
    tab->tocCalib = nullptr;
    DeleteTocCalibSession(s);
}
