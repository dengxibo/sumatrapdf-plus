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
#include "resource.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "OcrService.h"
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
#include "RenderCache.h"
#include "FindBar.h"
#include "FindWindow.h"
#include "SearchAndDDE.h"

#include "utils/Log.h"
#include "utils/UITask.h"

static void TocCalibRestoreDisplayMode(MainWindow* win, TocCalibSession* s);

static bool TocCalibHasPrinted(int printed) {
    return printed > 0;
}

// OCR mangles printed page numbers ("113" can arrive as "70 3"); a printed
// page far beyond the document's page count is garbage. Feeding it into the
// printed->pdf mapping clamps the row to the last page, and the monotonic
// repair then drags every later row there too (a "703" on a 345-page book
// turned chapters 3-5 into page 345).
static bool TocCalibPrintedPlausible(int nPages, int printed) {
    if (!TocCalibHasPrinted(printed)) {
        return false;
    }
    if (nPages <= 0) {
        return true;
    }
    return printed <= nPages + nPages / 2 + 32;
}

static int TocCalibRowPdf(const TocCalibRow* row);
static int TocCalibOffsetForPrinted(const TocCalibSession* s, int printed);
static int TocCalibLabelPrinted(const TocCalibSession* s, int pdf);
static int TocCalibParseLabelPrinted(const char* label);
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
        // Rough AI seeds (lastToc+printed) all share the same wrong offset and
        // would lock calibration onto it. Only body/pin evidence may vote.
        if (!(r.bodyMatched || r.verified || r.pdfPinned)) {
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

// Presence bitmap over the low byte of each filtered codepoint. False
// positives are harmless (the precise scoring still runs); false negatives
// are impossible, which makes it safe as a prefilter for the cover test.
struct TocCalibCpBits {
    unsigned char b[32];

    void Add(int cp) {
        int v = cp & 0xFF;
        b[v >> 3] |= (unsigned char)(1 << (v & 7));
    }
    bool Has(int cp) const {
        int v = cp & 0xFF;
        return (b[v >> 3] >> (v & 7)) & 1;
    }
    void OrWith(const TocCalibCpBits& o) {
        for (int i = 0; i < 32; i++) {
            b[i] |= o.b[i];
        }
    }
};

static void TocCalibCpBitsFill(const char* s, int len, TocCalibCpBits* out) {
    int i = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (TocCalibSkipMatchCp(cp)) {
            continue;
        }
        out->Add(cp);
    }
}

// The cover test needs at least 4 in-order hits, so if the presence bitmap
// shows fewer than 4 of the title's glyphs can possibly appear in the body,
// the full evaluation would return 0 anyway.
static bool TocCalibCpBitsMayCover(const TocCalibCpBits& bits, const int* cps, int nCp) {
    int missing = 0;
    for (int k = 0; k < nCp; k++) {
        if (!bits.Has(cps[k])) {
            missing++;
            if (missing > nCp - 4) {
                return false;
            }
        }
    }
    return true;
}

// Prepared form of a TOC title for repeated matching against many body
// lines: glyph count, compacted text and the filtered codepoint array used
// by the in-order cover pass. Building it once per search instead of once
// per body line removes the dominant repeated cost of near-page scans.
struct TocCalibTitleCtx {
    const char* title; // not owned; must outlive the ctx
    char compact[512];
    int glyphCount;
    int cps[256];
    int nCp;
};

static void TocCalibTitleCtxInit(TocCalibTitleCtx* ctx, const char* title) {
    ctx->title = title;
    ctx->compact[0] = 0;
    ctx->glyphCount = 0;
    ctx->nCp = 0;
    if (!title || !title[0]) {
        return;
    }
    ctx->glyphCount = TocCalibGlyphCount(title);
    TocCalibCompact(title, ctx->compact, (int)sizeof(ctx->compact));
    int tlen = (int)str::Len(title);
    int ti = 0;
    while (ti < tlen && ctx->nCp < (int)dimof(ctx->cps)) {
        int tcp = Utf8CodepointNext(title, tlen, ti);
        if (tcp <= 0) {
            break;
        }
        if (TocCalibSkipMatchCp(tcp)) {
            continue;
        }
        ctx->cps[ctx->nCp++] = tcp;
    }
}

static int TocCalibInOrderCoverCps(const int* tcps, int nT, const char* body) {
    if (!body || nT < 1) {
        return 0;
    }
    int blen = (int)str::Len(body);
    int bi = 0;
    int n = 0;
    for (int k = 0; k < nT; k++) {
        int tcp = tcps[k];
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

// Same semantics as TocCalibTitleMatchScore(body, ctx->title) with the
// title-side work (glyph count, compact, codepoint filtering) already done.
int TocCalibTitleMatchScoreCtx(const TocCalibTitleCtx* ctx, const char* body) {
    if (!ctx || ctx->glyphCount < 2 || !body || !body[0]) {
        return 0;
    }
    const char* title = ctx->title;
    int tg = ctx->glyphCount;
    if (str::Eq(body, title) || str::StartsWith(body, title) || str::Find(body, title)) {
        return tg;
    }
    int bg = TocCalibGlyphCount(body);
    if (tg < 4) {
        char bbuf[256];
        TocCalibCompact(body, bbuf, (int)sizeof(bbuf));
        if (ctx->compact[0] && (str::Eq(bbuf, ctx->compact) || str::StartsWith(bbuf, ctx->compact))) {
            return tg;
        }
        return 0;
    }
    if (bg >= 4 && bg * 2 >= tg && str::StartsWith(title, body)) {
        return bg;
    }
    char bbuf[768];
    TocCalibCompact(body, bbuf, (int)sizeof(bbuf));
    if (ctx->compact[0] &&
        (str::Eq(bbuf, ctx->compact) || str::StartsWith(bbuf, ctx->compact) || str::Find(bbuf, ctx->compact))) {
        return tg;
    }
    if (bg >= 4 && bg * 2 >= tg && ctx->compact[0] && str::StartsWith(ctx->compact, bbuf)) {
        return bg;
    }
    int cover = TocCalibInOrderCoverCps(ctx->cps, ctx->nCp, body);
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
    it->destinationSource = TocDestinationSource::BodyMatch;
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
                                           TocCalibNearHit* hit, int startPage = 0, bool stopAtFirst = false,
                                           DWORD budgetMs = 40);

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
static void TocCalibKeepExistingDests(TocCalibSession* s);

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

// Incrementally indexes pages [*pNext..nPages) into idx, processing pages
// until the time budget (budgetMs, or UINT_MAX for "no budget") is used up.
// Returns true when indexing is complete (regardless of nDocs), false when
// the budget ran out and the caller should call again with the advanced
// *pNext cursor. Caller owns the initial TocCalibBm25Free.
static bool TocCalibBm25BuildChunk(TocCalibSession* s, Vec<int>& pages, Vec<Vec<EngineMupdfPageLine>*>& cache,
                                   TocCalibBm25Index* idx, int* pNext, DWORD budgetMs) {
    if (!s || !idx || s->nPages < 1) {
        return false;
    }
    DWORD t0 = ::GetTickCount();
    int p = *pNext;
    for (; p <= s->nPages; p++) {
        *pNext = p + 1;
        // Image-only / empty text layer: stop after a short probe. Indexing every
        // page loads fz_page for the whole book and can exhaust GDI/memory so the
        // canvas paints blank; Find is skipped for sparse indexes anyway.
        if (budgetMs != UINT_MAX && idx->nDocs < 1 && p > 32) {
            *pNext = s->nPages + 1;
            return true;
        }
        if (TocCalibPageInToc(s, p)) {
            if (budgetMs != UINT_MAX && ::GetTickCount() - t0 > budgetMs) {
                return false;
            }
            continue;
        }
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (!lines || lines->Size() < 1) {
            // empty / image pages still cost GetPageLines; honor the slice budget
            if (budgetMs != UINT_MAX && ::GetTickCount() - t0 > budgetMs) {
                return false;
            }
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
            if (budgetMs != UINT_MAX && ::GetTickCount() - t0 > budgetMs) {
                return false;
            }
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
        if (budgetMs != UINT_MAX && ::GetTickCount() - t0 > budgetMs) {
            return false;
        }
    }
    *pNext = s->nPages + 1;
    return true;
}

static bool TocCalibBm25Build(TocCalibSession* s, Vec<int>& pages, Vec<Vec<EngineMupdfPageLine>*>& cache,
                              TocCalibBm25Index* idx) {
    if (!s || !idx || s->nPages < 1) {
        return false;
    }
    TocCalibBm25Free(idx);
    int next = 1;
    TocCalibBm25BuildChunk(s, pages, cache, idx, &next, UINT_MAX);
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
    TocCalibTitleCtx tctx;
    TocCalibTitleCtxInit(&tctx, title);
    if (tctx.glyphCount < 2) {
        return false;
    }
    int tg = tctx.glyphCount;
    int minScore = tg < 4 ? tg : 4;
    if (minScore < 2) {
        minScore = 2;
    }
    // the bitmap prefilter models the cover test, which only exists for
    // titles of 4+ glyphs; shorter titles must always be scored directly
    bool usePrefilter = tctx.nCp >= 4;
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
        int nL = lines->Size();
        // per-line presence bitmaps, built once per page and OR-combined as
        // the joined-line window grows; lets the inner loop skip line combos
        // that cannot possibly reach the in-order-cover threshold
        Vec<TocCalibCpBits> bits;
        if (nL > 0 && usePrefilter) {
            bits.SetSize(nL);
            memset(bits.LendData(), 0, (size_t)nL * sizeof(TocCalibCpBits));
            for (int i = 0; i < nL; i++) {
                const EngineMupdfPageLine& ln = lines->At(i);
                if (ln.text && ln.text[0]) {
                    TocCalibCpBitsFill(ln.text, (int)str::Len(ln.text), &bits[i]);
                }
            }
        }
        for (int i = 0; i < nL; i++) {
            const EngineMupdfPageLine& ln = lines->At(i);
            char joined[768];
            joined[0] = 0;
            int used = 0;
            float y0 = ln.y;
            float dy0 = ln.dy > 2 ? ln.dy : 12;
            TocCalibCpBits combo;
            if (usePrefilter) {
                combo = bits[i];
            }
            for (int k = 0; k < 3 && i + k < nL; k++) {
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
                    if (usePrefilter) {
                        combo.OrWith(bits[i + k]);
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
                if (usePrefilter && !TocCalibCpBitsMayCover(combo, tctx.cps, tctx.nCp)) {
                    continue;
                }
                int sc = TocCalibTitleMatchScoreCtx(&tctx, joined);
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

enum class TocCalibRowVerifyPhase {
    Skip,     // row needs no work (pinned / too short / consistent already)
    Done,     // matched via near-page search
    NeedFull, // needs the full-document BM25 / Find fallback passes
};

// Verify one row using only the cheap near-page search around the predicted
// page. Shared by the synchronous and the chunked (async) verify drivers.
static TocCalibRowVerifyPhase TocCalibVerifyRowNear(TocCalibSession* s, int i, Vec<int>& pages,
                                                    Vec<Vec<EngineMupdfPageLine>*>& cache) {
    ExtractedTocItem* it = s->rows[i].item;
    if (!it) {
        return TocCalibRowVerifyPhase::Skip;
    }
    if (s->rows[i].pdfPinned || s->rows[i].userSet) {
        return TocCalibRowVerifyPhase::Skip;
    }
    const char* title = it->rawTitle && it->rawTitle[0] ? it->rawTitle : it->title;
    if (!title || TocCalibGlyphCount(title) < 2) {
        return TocCalibRowVerifyPhase::Skip;
    }
    int pred = it->pageNo;
    // footerMapped: the printed-page index already assigned destinations.
    // Do not fall back to printed+offset — that one offset is wrong when a
    // contents block splits the arabic sequence.
    if (pred < 1 && !s->footerMapped && TocCalibPrintedPlausible(s->nPages, it->printedPage)) {
        int rowOff = TocCalibOffsetForPrinted(s, it->printedPage);
        if (rowOff >= 0) {
            pred = it->printedPage + rowOff;
            if (pred < 1) {
                pred = 1;
            }
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
            return TocCalibRowVerifyPhase::Skip;
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
        return TocCalibRowVerifyPhase::Done;
    }
    return TocCalibRowVerifyPhase::NeedFull;
}

// Sparse BM25 (few pages with a text layer) means TextSearch::Find will walk
// / OCR most of the document per title — minutes of UI freeze on a 350-page
// scan. Near-page search is enough; skip Find in that case.
static bool TocCalibBm25IndexSparse(const TocCalibBm25Index* idx, int nPages) {
    if (!idx || idx->nDocs < 1) {
        return true;
    }
    if (idx->nDocs < 5) {
        return true;
    }
    return nPages > 0 && idx->nDocs * 20 < nPages;
}

// Full-document fallback pass for one row (BM25 index must be complete when
// nPages <= 800; the >800 case only demotes confidence). Shared by both
// verify drivers.
static void TocCalibVerifyRowFull(TocCalibSession* s, int i, Vec<int>& pages, Vec<Vec<EngineMupdfPageLine>*>& cache,
                                  TocCalibBm25Index* bm25) {
    ExtractedTocItem* it = s->rows[i].item;
    if (!it) {
        return;
    }
    const char* title = it->rawTitle && it->rawTitle[0] ? it->rawTitle : it->title;
    // Indexing every page for BM25/Find freezes a 900-page textbook.
    // Near-page search above is enough; skip the full-document pass.
    if (s->nPages > 800) {
        if (!it->bodyMatched && it->confidence > 60) {
            it->confidence = 60;
        }
        return;
    }
    TocCalibNearHit hit;
    bool sparse = TocCalibBm25IndexSparse(bm25, s->nPages);
    if (TocCalibGlyphCount(title) >= 4 && TocCalibSearchBm25(s, title, pages, cache, bm25, &hit) &&
        !TocCalibPageInToc(s, hit.page)) {
        if (TocCalibApplyNearHit(it, hit.page, hit.x, hit.y, hit.score, 0)) {
            s->rows[i].identPageNo = it->pageNo;
            if (it->confidence < 75) {
                it->confidence = 75;
            }
        }
    } else if (!sparse && TocCalibGlyphCount(title) >= 4 &&
               TocCalibSearchTextFindFallback(s, &s->rows[i], title, &hit) && !TocCalibPageInToc(s, hit.page) &&
               TocCalibApplyNearHit(it, hit.page, hit.x, hit.y, hit.score, 0)) {
        s->rows[i].identPageNo = it->pageNo;
        if (it->confidence < 75) {
            it->confidence = 75;
        }
    } else if (!it->bodyMatched && it->confidence > 60) {
        it->confidence = 60;
    }
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
        TocCalibRowVerifyPhase ph = TocCalibVerifyRowNear(s, i, pages, cache);
        if (ph == TocCalibRowVerifyPhase::NeedFull) {
            TocCalibVerifyRowFull(s, i, pages, cache, &bm25);
        }
    }
    TocCalibBm25Free(&bm25);
    TocCalibFreePageCache(cache);
}

// Chunked body-verification state, driven by ~40ms UI-thread slices (see
// StartTocCalibAsync). Owned by the session (s->verifyJob) while running;
// the job frees itself when its slice chain ends.
struct TocCalibPrintedIndex;
struct TocCalibVerifyJob {
    MainWindow* win = nullptr;
    TocCalibSession* s = nullptr;
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    TocCalibBm25Index bm25;
    int row = 0;
    int total = 0;
    // wall-clock start of the whole verify job (for stage timing logs)
    DWORD tStart = 0;
    // incremental BM25 build cursor (valid while bm25Building)
    int bm25Page = 1;
    bool bm25Building = false;
    // True once the (possibly sparse / early-aborted) index pass finishes so
    // NeedFull rows do not restart a full-document probe when nDocs stays 0.
    bool bm25Ready = false;
    bool aborted = false;
    // verify cost statistics (logged when the job completes)
    int nSkip = 0;
    int nNear = 0;
    int nNeedFull = 0;
    int nFull = 0;
    DWORD bm25BuildMs = 0;
    TocCalibVerifyProgressFn onProgress = nullptr;
    TocCalibVerifyDoneFn onDone = nullptr;
    void* ctx = nullptr;
    // Scheduling goes through a one-shot timer on this message-only window
    // (NOT a self-reposting uitask): back-to-back posted tasks starve the
    // message pump and make the dialog impossible to drag. The timer gap
    // guarantees the pump processes input between slices.
    HWND timerWnd = nullptr;
    // First slices walk page headers/footers and assign PDF pages from the
    // printed number. Row verification starts only after that scan finishes.
    bool footerScanning = false;
    int footerPage = 1;
    TocCalibPrintedIndex* footerIdx = nullptr;
};

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

static void TocCalibReadFooterMark(EngineBase* engine, int pdf, int* arabicOut, char* labOut, int labCap,
                                   bool footerOnly = false);

// Same folio rules as the printed index (tight band + trailing R1). The old
// 0.78 loose parse swallowed credit years and skewed arabicOffset toward the
// reference section on multi-schema books.
// 2024 in a margin is a year. A feasibility report's own page numbers run
// past 400, so the old hard cap dropped every later folio and the row showed
// "?" or a leftover header digit.
static bool TocCalibIsCalendarYear(int n) {
    return n >= 1900 && n <= 2099;
}

static bool TocCalibFolioNumberOk(int arabic, int nPages) {
    if (arabic < 1 || TocCalibIsCalendarYear(arabic)) {
        return false;
    }
    int cap = nPages > 0 ? nPages : 400;
    if (cap < 400) {
        cap = 400;
    }
    if (cap > 9999) {
        cap = 9999;
    }
    return arabic <= cap;
}

static int TocCalibFooterPrintedOnPage(EngineBase* engine, int pdf) {
    int arabic = 0;
    TocCalibReadFooterMark(engine, pdf, &arabic, nullptr, 0);
    int nPages = engine ? engine->PageCount() : 0;
    if (!TocCalibFolioNumberOk(arabic, nPages)) {
        return 0;
    }
    return arabic;
}

int TocCalibEstimateArabicOffset(EngineBase* engine, int afterTocPdf) {
    if (!engine) {
        return -1;
    }
    int nPages = engine->PageCount();
    if (nPages < 1) {
        return -1;
    }
    int start = afterTocPdf > 0 ? afterTocPdf + 1 : 1;
    if (start < 1) {
        start = 1;
    }
    if (start > nPages) {
        return -1;
    }
    // Long front matter (roman / unnumbered) often sits between the TOC and
    // arabic page 1. Probe far enough to reach the body (G11 ≈ +41 pages).
    int end = start + 160;
    if (end > nPages) {
        end = nPages;
    }
    int offs[64];
    int n = 0;
    for (int pdf = start; pdf <= end && n < 64; pdf++) {
        int pr = TocCalibFooterPrintedOnPage(engine, pdf);
        if (!TocCalibFolioNumberOk(pr, nPages)) {
            continue;
        }
        int off = pdf - pr;
        if (off < 0) {
            continue;
        }
        offs[n++] = off;
    }
    return TocCalibMajorityOffset(offs, n);
}

// "000076.pdg" is printed page 76. "!00001.pdg" / "cov001.pdg" / "fow002.pdg"
// are front-matter sheet names, not body pages.
static int TocCalibParsePdgBodyPrinted(const char* label) {
    if (!label || !label[0]) {
        return 0;
    }
    const char* dot = str::FindI(label, ".pdg");
    if (!dot || dot[4] != 0 || dot == label) {
        return 0;
    }
    int nDig = 0;
    int page = 0;
    for (const char* p = label; p < dot; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        nDig++;
        page = page * 10 + (*p - '0');
        if (nDig > 6 || page > 9999) {
            return 0;
        }
    }
    return page >= 1 ? page : 0;
}

// pdfByPrinted[printed] = PDF page. Index 0 is unused. True when any body
// sheet (000NNN.pdg) was found. Scans with no text layer still carry the
// printed page in this filename.
bool TocCalibBuildPdgPrintedMap(EngineBase* engine, Vec<int>& pdfByPrinted) {
    pdfByPrinted.Reset();
    if (!engine) {
        return false;
    }
    bool any = false;
    int nPages = engine->PageCount();
    for (int pdf = 1; pdf <= nPages; pdf++) {
        int printed = TocCalibParsePdgBodyPrinted(EngineMupdfRawPageLabelTemp(engine, pdf));
        if (printed < 1) {
            continue;
        }
        while (pdfByPrinted.Size() <= printed) {
            pdfByPrinted.Append(0);
        }
        if (pdfByPrinted[printed] < 1) {
            pdfByPrinted[printed] = pdf;
            any = true;
        }
    }
    return any;
}

static int TocCalibPdgMapPdf(const Vec<int>& pdfByPrinted, int printed) {
    if (printed < 1 || printed >= pdfByPrinted.Size()) {
        return 0;
    }
    return pdfByPrinted[printed];
}

int TocCalibPdfForPrintedLabel(EngineBase* engine, int printed) {
    if (!engine || printed < 1 || printed > 9999) {
        return 0;
    }
    TempStr buf = str::FormatTemp("%d", printed);
    int pdf = EngineMupdfPageForExactLabel(engine, buf);
    if (pdf < 1) {
        return 0;
    }
    int nPages = engine->PageCount();
    if (nPages > 0 && pdf > nPages) {
        return 0;
    }
    return pdf;
}

bool TocCalibTestPdgBodyPrinted() {
    bool ok = TocCalibParsePdgBodyPrinted("000076.pdg") == 76 && TocCalibParsePdgBodyPrinted("000001.pdg") == 1 &&
              TocCalibParsePdgBodyPrinted("000323.pdg") == 323 && TocCalibParsePdgBodyPrinted("000466.pdg") == 466;
    ok = ok && TocCalibParsePdgBodyPrinted("!00001.pdg") == 0 && TocCalibParsePdgBodyPrinted("cov001.pdg") == 0 &&
         TocCalibParsePdgBodyPrinted("fow002.pdg") == 0 && TocCalibParsePdgBodyPrinted("bok001.pdg") == 0;
    ok = ok && TocCalibParsePdgBodyPrinted("Anna's Archive") == 0 && TocCalibParsePdgBodyPrinted("000076.pdg ") == 0 &&
         TocCalibParsePdgBodyPrinted("") == 0;
    return ok;
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
    // The same digit on sheets far apart is a running header ("11" on PDF 210
    // and 233), not a folio. A real restart (preface 1, then body 1) still
    // begins a 1,2,3 run and is kept. Decide every row before clearing, so
    // dropping the first copy does not hide the span from the next one.
    Vec<int> drop;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || !TocCalibHasPrinted(it->printedPage) || it->pageNo < 1) {
            continue;
        }
        int pr = it->printedPage;
        int minPdf = 0;
        int maxPdf = 0;
        bool startsRun = false;
        for (int j = 0; j < s->rows.Size(); j++) {
            ExtractedTocItem* o = s->rows[j].item;
            if (!o || o->printedPage != pr || o->pageNo < 1) {
                continue;
            }
            if (minPdf < 1 || o->pageNo < minPdf) {
                minPdf = o->pageNo;
            }
            if (o->pageNo > maxPdf) {
                maxPdf = o->pageNo;
            }
        }
        if (maxPdf - minPdf <= 4) {
            continue;
        }
        for (int j = 0; j < s->rows.Size(); j++) {
            ExtractedTocItem* o = s->rows[j].item;
            if (!o || o->printedPage != pr + 1 || o->pageNo <= it->pageNo || o->pageNo > it->pageNo + 8) {
                continue;
            }
            startsRun = true;
            break;
        }
        if (!startsRun) {
            drop.Append(i);
        }
    }
    for (int i = 0; i < drop.Size(); i++) {
        ExtractedTocItem* it = s->rows[drop[i]].item;
        if (it) {
            it->printedPage = 0;
        }
    }
}

// scanBody: extract printed-TOC pages, footers, and full-document BM25/Find.
// Opening an existing outline must not take that path. See TocCalibKeepExistingDests.

// Resolve roman / R-page / RA-page printedLabel. Defined after footer parsers.
// endBookRScan: probe the last ~500 pages for R1… (reopen path). Skip during
// AI import — the full footer index covers handbook pages instead.
static void TocCalibResolvePrintedLabels(TocCalibSession* s, bool endBookRScan);
static int TocCalibFillPageFromChildren(ExtractedTocItem* item);
int TocCalibLookupLabel(const TocCalibPrintedIndex* idx, int afterTocPdf, const char* label);

static void TocCalibSeedArabicOffsetFromFooters(TocCalibSession* s) {
    if (!s || !s->engine || s->offsetLocked) {
        return;
    }
    if (s->map.confidence > 0 && s->map.offset >= 0) {
        return;
    }
    // 000NNN.pdg already names each printed page. A single footer offset
    // (or a guess when the scan has no text) would move every bookmark.
    Vec<int> pdg;
    if (TocCalibBuildPdgPrintedMap(s->engine, pdg)) {
        return;
    }
    int after = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    int off = TocCalibEstimateArabicOffset(s->engine, after);
    if (off < 0) {
        return;
    }
    s->map.offset = off;
    s->map.confidence = 0.55f;
    logf("TocCalib: footer arabic offset=%d afterToc=%d\n", off, after);
}

static void TocCalibPrepareMapping(TocCalibSession* s, bool markConfirm, bool scanBody, bool deferVerify = false) {
    if (!s) {
        return;
    }
    TocCalibCollectRows(s);
    if (scanBody) {
        TocCalibEnsureTocRange(s);
        TocCalibClearDestsOnTocPages(s);
        TocCalibSeedPrintedFromPages(s);
    } else {
        // Existing outline: one footer read per bookmark that already has a
        // destination. Do not scan the book. Pin those destinations so a later
        // save does not replace them with printed+offset, and so the page
        // field can show the folio again instead of "?".
        TocCalibKeepExistingDests(s);
    }
    TocCalibResolvePrintedLabels(s, !scanBody);
    // R1 / roman children may have just gained a PDF dest; re-fill parents
    // such as "Reference Section" so the page field is not left as "?".
    for (int i = 0; i < s->roots.Size(); i++) {
        TocCalibFillPageFromChildren(s->roots[i]);
    }
    // Parents that inherited an R1/roman label still need GetPageByLabel / scan.
    TocCalibResolvePrintedLabels(s, !scanBody);
    for (int i = 0; i < s->roots.Size(); i++) {
        TocCalibFillPageFromChildren(s->roots[i]);
    }
    s->editPdf = true;
    for (int i = 0; i < s->rows.Size(); i++) {
        if (s->rows[i].item && TocCalibHasPrinted(s->rows[i].item->printedPage)) {
            s->editPdf = false;
            break;
        }
    }
    // Before body verify, seed offset from footers so predicted pages land near
    // the real body (critical for 1000+ page scans where NeedFull is skipped).
    if (scanBody) {
        TocCalibSeedArabicOffsetFromFooters(s);
    }
    TocCalibSolveSession(s);
    if (scanBody) {
        // deferVerify: the caller runs the (expensive) verification pass in
        // chunked slices via StartTocCalibAsync, then re-solves + marks.
        if (!deferVerify) {
            TocCalibVerifyNearPredicted(s);
            TocCalibSolveSession(s);
        }
    }
    if (markConfirm && !(deferVerify && scanBody)) {
        TocCalibMarkConfirm(s);
    }
    if (!deferVerify) {
        TocCalibWriteDebugIfCli(s);
    }
}

void TocCalibRefineExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine) {
    TocCalibSession s;
    s.engine = engine;
    s.nPages = engine ? engine->PageCount() : 0;
    s.persistToDisk = false;
    for (int i = 0; i < roots.Size(); i++) {
        s.roots.Append(roots[i]);
    }
    TocCalibPrepareMapping(&s, false, true);
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
        if (!it || it->destinationSource == TocDestinationSource::PdfLink || TocCalibIsContentsTitle(it->title)) {
            continue;
        }
        // Only clear items that came from the printed TOC (they only have
        // print-page numbers, not PDF page numbers). BodyInference items come
        // from heading detection on body pages and already have the correct PDF
        // pageNo -- clearing them here would lose that anchor info.
        if (it->source != ExtractedTocSource::PrintedToc) {
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
    Vec<PageMappingSegment> offsetSegs;
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
    n->destinationSource = src->destinationSource;
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
    for (int i = 0; i < s->offsetSegs.Size(); i++) {
        snap->offsetSegs.Append(s->offsetSegs[i]);
    }
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
    s->offsetSegs.Reset();
    for (int i = 0; i < snap->offsetSegs.Size(); i++) {
        s->offsetSegs.Append(snap->offsetSegs[i]);
    }
    s->editPdf = snap->editPdf;
    TocCalibCollectRows(s);
}

struct TocCalibPrintedIndex;
static void TocCalibPrintedIndexFree(TocCalibPrintedIndex* idx);

void DeleteTocCalibSession(TocCalibSession* s) {
    if (!s) {
        return;
    }
    if (s->verifyJob) {
        // Detach the pending verify job; its next slice (already queued or
        // to be posted) sees s == nullptr and frees itself without touching
        // this session again.
        s->verifyJob->aborted = true;
        s->verifyJob->s = nullptr;
        s->verifyJob->win = nullptr;
        s->verifyJob = nullptr;
    }
    s->engine = nullptr;
    if (s->printedIdx) {
        TocCalibPrintedIndexFree(s->printedIdx);
        s->printedIdx = nullptr;
    }
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
    // Keep unresolved rows in the editing sequence. They are drawn gray, but
    // are still real adjacent TOC items: hiding them from s->rows made
    // "merge with next" skip a gray line and consume the following one.
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
            row.pdfPinned = it->destinationSource == TocDestinationSource::PdfLink;
        }
        if (row.identPageNo < 1 && it->pageNo > 0) {
            row.identPageNo = it->pageNo;
        }
        if (row.origPageNo < 1 && it->pageNo > 0) {
            row.origPageNo = it->pageNo;
            row.pdfPinned = it->destinationSource == TocDestinationSource::PdfLink;
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
    Vec<int> pdgMap;
    bool pdgBook = s->engine && TocCalibBuildPdgPrintedMap(s->engine, pdgMap);
    for (int i = 0; i < n; i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (s->rows[i].userSet) {
            s->rows[i].needsConfirm = false;
            if (it) {
                it->verified = true;
            }
            continue;
        }
        bool pdgHit = pdgBook && it && TocCalibPdgMapPdf(pdgMap, it->printedPage) == it->pageNo && it->pageNo > 0;
        bool labelHit = it && TocCalibPdfForPrintedLabel(s->engine, it->printedPage) == it->pageNo && it->pageNo > 0;
        // R1 / xxxvi rows (and parents that inherited that folio) are resolved by
        // the footer index, not by /PageLabels or arabic offset.
        bool folioOk = it && it->pageNo > 0 && s->footerMapped &&
                       (TocCalibHasPrinted(it->printedPage) || (it->printedLabel && it->printedLabel[0]));
        bool reliable = it && it->pageNo > 0 && !s->rows[i].clamped &&
                        (it->bodyMatched || s->rows[i].pdfPinned || pdgHit || labelHit || folioOk ||
                         it->destinationSource == TocDestinationSource::BodyMatch);
        s->rows[i].needsConfirm = s->rows[i].needsConfirm || !reliable;
        if (it) it->verified = reliable;
    }
}

static void TocCalibRowsToMap(const TocCalibSession* s, Vec<TocCalibMapRow>& mapRows) {
    mapRows.Reset();
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        if (!TocCalibPrintedPlausible(s->nPages, it->printedPage)) {
            // Garbage printed must not vote on the offset.
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

// Offset for this folio: the latest segment that starts at or before it, else
// the book-wide map.offset. -1 when neither is known.
static int TocCalibOffsetForPrinted(const TocCalibSession* s, int printed) {
    int off = -1;
    if (TocCalibHaveOffset(s)) {
        off = s->map.offset;
    }
    if (!s || printed < 1) {
        return off;
    }
    int bestStart = -1;
    for (int i = 0; i < s->offsetSegs.Size(); i++) {
        const PageMappingSegment& seg = s->offsetSegs[i];
        if (seg.offset < 0 || seg.printedStart < 1) {
            continue;
        }
        if (seg.printedStart <= printed && seg.printedStart >= bestStart) {
            bestStart = seg.printedStart;
            off = seg.offset;
        }
    }
    return off;
}

// A correction applies from this folio downward. Drop this start and any
// later segment, then keep the new one.
static void TocCalibRememberOffsetSeg(TocCalibSession* s, int printedStart, int offset) {
    if (!s || printedStart < 1 || offset < 0) {
        return;
    }
    for (int i = s->offsetSegs.Size() - 1; i >= 0; i--) {
        if (s->offsetSegs[i].printedStart >= printedStart) {
            s->offsetSegs.RemoveAt(i);
        }
    }
    PageMappingSegment seg;
    seg.printedStart = printedStart;
    seg.printedEnd = 99999;
    seg.offset = offset;
    seg.confidence = 1;
    s->offsetSegs.Append(seg);
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

// Handbook / atlas labels: R1, R20, RA1, RA36 (1–3 letters + digits).
// Kept as printedLabel so GetPageByLabel / footer lookup can resolve them.
static bool TocCalibIsLetterDigitLabel(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int letters = 0;
    const char* p = s;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
        letters++;
        if (letters > 3) {
            return false;
        }
        p++;
    }
    if (letters < 1) {
        return false;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    int digits = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        digits++;
        if (digits > 4) {
            return false;
        }
    }
    return !*p && digits >= 1;
}

// Printed label found in a page header or footer (arabic, roman, or R1).
// Built once per import so each TOC entry looks up its own PDF page instead
// of sharing one offset. Front-matter "1" and body "4" stay in separate runs
// when a contents block sits between them.
struct TocCalibPrintedIndex {
    Vec<int> pdf;
    Vec<int> arabic;
    Vec<char*> label;
};

static void TocCalibPrintedIndexFree(TocCalibPrintedIndex* idx) {
    if (!idx) {
        return;
    }
    for (int i = 0; i < idx->label.Size(); i++) {
        str::Free(idx->label[i]);
    }
    delete idx;
}

static void TocCalibPrintedIndexAdd(TocCalibPrintedIndex* idx, int pdf, int arabic, const char* label) {
    if (!idx || pdf < 1) {
        return;
    }
    if (arabic < 1 && (!label || !label[0])) {
        return;
    }
    idx->pdf.Append(pdf);
    idx->arabic.Append(arabic > 0 ? arabic : 0);
    idx->label.Append(label && label[0] ? str::Dup(label) : nullptr);
}

// Strip leaders ("· 28 ·", "xxxvi") down to one token. Arabic up to 4 digits.
// Roman and R-page labels only — a footer word like "CHOICE" is not a page.
// One alphanumeric word, with only punctuation around it. "28" and "· xxxvi ·"
// count; "i i", "1.5" and "UNIT 1" do not — joining those invents a page.
static bool TocCalibParseFooterToken(const char* s, int* arabicOut, char* labOut, int labCap) {
    if (arabicOut) {
        *arabicOut = 0;
    }
    if (labOut && labCap > 0) {
        labOut[0] = 0;
    }
    if (!s || !s[0] || !arabicOut || !labOut || labCap < 2) {
        return false;
    }
    const char* p = s;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            break;
        }
        if (c >= 128) {
            return false;
        }
        p++;
    }
    char buf[32]{};
    int n = 0;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            if (n >= (int)sizeof(buf) - 1) {
                return false;
            }
            buf[n++] = (char)c;
            p++;
            continue;
        }
        break;
    }
    buf[n] = 0;
    if (n < 1) {
        return false;
    }
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c >= 128) {
            return false;
        }
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            return false;
        }
        p++;
    }
    bool allDig = true;
    int page = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] < '0' || buf[i] > '9') {
            allDig = false;
            break;
        }
        page = page * 10 + (buf[i] - '0');
        if (page > 9999) {
            return false;
        }
    }
    if (allDig) {
        if (page < 1) {
            return false;
        }
        *arabicOut = page;
        return true;
    }
    if (TocCalibParseRoman(buf) < 1 && !TocCalibIsLetterDigitLabel(buf)) {
        return false;
    }
    int copy = n < labCap - 1 ? n : labCap - 1;
    memcpy(labOut, buf, (size_t)copy);
    labOut[copy] = 0;
    return true;
}

// Footers like "LITERARY TERMS HANDBOOK R1" / "READING HANDBOOK R21" are not a
// single token. Pull the trailing R1 / RA36 when the line ends with one.
static bool TocCalibExtractTrailingLetterDigitLabel(const char* s, char* out, int cap) {
    if (!s || !out || cap < 2) {
        return false;
    }
    out[0] = 0;
    const char* end = s + str::Len(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (end <= s) {
        return false;
    }
    const char* p = end;
    while (p > s && p[-1] >= '0' && p[-1] <= '9') {
        p--;
    }
    if (p >= end) {
        return false;
    }
    const char* digStart = p;
    while (p > s && ((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z'))) {
        p--;
    }
    if (p >= digStart) {
        return false;
    }
    // Require a separator before the label so "CHAPTER" alone is not R#.
    // Exception: handbook titles jammed against the folio ("HANDBOOKR1") —
    // MuPDF sometimes drops the space; still accept R# / RA#.
    if (p > s) {
        unsigned char c = (unsigned char)p[-1];
        bool sep = (c <= 32) || c == 0xA0 || c == '-' || c == 0xB7 || c == '.' || c == ':' || c == '/' || c == '|';
        if (!sep) {
            char probe[32]{};
            int pn = (int)(end - p);
            if (pn < 2 || pn >= (int)sizeof(probe)) {
                return false;
            }
            memcpy(probe, p, (size_t)pn);
            probe[pn] = 0;
            if (!TocCalibIsLetterDigitLabel(probe)) {
                return false;
            }
            char u0 = (char)toupper((unsigned char)probe[0]);
            if (u0 != 'R') {
                return false;
            }
        }
    }
    int n = (int)(end - p);
    if (n < 2 || n >= cap) {
        return false;
    }
    char buf[32]{};
    if (n >= (int)sizeof(buf)) {
        return false;
    }
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    if (!TocCalibIsLetterDigitLabel(buf)) {
        return false;
    }
    memcpy(out, buf, (size_t)n);
    out[n] = 0;
    return true;
}

// Printed folio on a page that already has a bookmark destination.
// A line counts only when it is itself a page token ("604", "xxxvi", "R20"),
// so a footer credit such as "UNIT 4" is not the page number.
static bool TocCalibLooksLikeTocLeaderLine(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    // Printed TOC rows: "Business Writing . . . . . . R42". Real footers are
    // "R42" or "LITERARY TERMS HANDBOOK R1" without leader dots.
    int dots = 0;
    for (const char* p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || c == 0xB7 || c == 0x2022 || c == 0x2027) {
            dots++;
            if (dots >= 3) {
                return true;
            }
            continue;
        }
        // UTF-8 ellipsis …
        if (c == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0xA6) {
            return true;
        }
    }
    return false;
}

static void TocCalibKeepExistingDests(TocCalibSession* s) {
    if (!s || !s->engine) {
        return;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        int pdf = TocCalibRowPdf(&s->rows[i]);
        if (pdf < 1) {
            continue;
        }
        s->rows[i].pdfPinned = true;
        if (s->rows[i].origPageNo < 1) {
            s->rows[i].origPageNo = pdf;
        }
        if (s->rows[i].identPageNo < 1) {
            s->rows[i].identPageNo = pdf;
        }
        if (TocCalibHasPrinted(it->printedPage) || (it->printedLabel && it->printedLabel[0])) {
            continue;
        }
        if (TocCalibNoPrintedTitle(it->title) || TocCalibNoPrintedTitle(it->rawTitle)) {
            continue;
        }
        int lab = TocCalibLabelPrinted(s, pdf);
        if (TocCalibHasPrinted(lab)) {
            it->printedPage = lab;
            continue;
        }
        Vec<EngineMupdfPageLine> lines;
        if (!EngineMupdfCollectPageLines(s->engine, pdf, lines)) {
            continue;
        }
        float pageH = 0;
        for (int li = 0; li < lines.Size(); li++) {
            float b = lines[li].y + (lines[li].dy > 1 ? lines[li].dy : 10);
            if (b > pageH) {
                pageH = b;
            }
        }
        int arabic = 0;
        char label[32]{};
        for (int li = 0; li < lines.Size(); li++) {
            float y = lines[li].y;
            bool band = pageH > 40 && (y >= pageH * 0.78f || y <= pageH * 0.12f);
            if (!band) {
                continue;
            }
            int pr = 0;
            char tok[32]{};
            if (!TocCalibParseFooterToken(lines[li].text, &pr, tok, (int)sizeof(tok))) {
                if (TocCalibLooksLikeTocLeaderLine(lines[li].text) ||
                    !TocCalibExtractTrailingLetterDigitLabel(lines[li].text, tok, (int)sizeof(tok))) {
                    continue;
                }
                pr = 0;
            }
            if (pr > 0 && pr != pdf) {
                arabic = pr;
                label[0] = 0;
            } else if (pr < 1 && tok[0] && arabic < 1) {
                int n = (int)str::Len(tok);
                if (n >= (int)sizeof(label)) {
                    n = (int)sizeof(label) - 1;
                }
                memcpy(label, tok, (size_t)n);
                label[n] = 0;
            }
        }
        EngineMupdfFreePageLines(lines);
        if (arabic > 0) {
            it->printedPage = arabic;
        } else if (label[0]) {
            str::ReplaceWithCopy(&it->printedLabel, label);
        }
    }
}

struct TocCalibArabicRun {
    int begin = 0;
    int end = 0;
    int printedMin = 0;
    int printedMax = 0;
    int pdfMin = 0;
    int pdfMax = 0;
};

// A new run starts when numbering restarts, a year or footnote jumps far
// ahead of the physical gap, or a long unnumbered block (the printed
// contents) sits between two arabic pages.
static bool TocCalibArabicContinues(int prevPdf, int prevPr, int pdf, int pr) {
    if (pdf <= prevPdf || pr < prevPr) {
        return false;
    }
    int pdfDelta = pdf - prevPdf;
    int prDelta = pr - prevPr;
    if (prDelta > pdfDelta + 2) {
        return false;
    }
    if (pdfDelta > prDelta + 8) {
        return false;
    }
    return true;
}

// Pull RA1 / RA36 from lines like "Reference Atlas • RA1" or "RA14 • Reference Atlas".
static bool TocCalibExtractAtlasFolioLabel(const char* text, char* out, int cap) {
    if (!text || !out || cap < 2) {
        return false;
    }
    out[0] = 0;
    if (!str::FindI(text, "Atlas") && !str::FindI(text, "Reference")) {
        return false;
    }
    for (const char* p = text; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
            continue;
        }
        char buf[32]{};
        int n = 0;
        const char* q = p;
        while (((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) && n < 3) {
            buf[n++] = *q++;
        }
        int dig = 0;
        while (*q >= '0' && *q <= '9' && n < (int)sizeof(buf) - 1) {
            buf[n++] = *q++;
            dig++;
            if (dig > 4) {
                break;
            }
        }
        buf[n] = 0;
        if (dig >= 1 && TocCalibIsLetterDigitLabel(buf)) {
            int copy = n < cap - 1 ? n : cap - 1;
            memcpy(out, buf, (size_t)copy);
            out[copy] = 0;
            return true;
        }
    }
    return false;
}

static void TocCalibResolvePrintedLabels(TocCalibSession* s, bool endBookRScan) {
    if (!s || !s->engine) {
        return;
    }
    int after = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    if (s->printedIdx) {
        for (int i = 0; i < s->rows.Size(); i++) {
            ExtractedTocItem* it = s->rows[i].item;
            if (!it || TocCalibHasPrinted(it->printedPage)) {
                continue;
            }
            if (!it->printedLabel || !it->printedLabel[0]) {
                continue;
            }
            if (s->rows[i].pdfPinned || it->bodyMatched) {
                continue;
            }
            int pdf = TocCalibLookupLabel(s->printedIdx, after, it->printedLabel);
            if (pdf > 0 && (s->nPages < 1 || pdf <= s->nPages)) {
                it->pageNo = pdf;
                s->rows[i].pdfPinned = true;
                s->rows[i].identPageNo = pdf;
            }
        }
    }
    bool needScan = false;
    bool needRScan = false;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || TocCalibHasPrinted(it->printedPage)) {
            continue;
        }
        if (!it->printedLabel || !it->printedLabel[0]) {
            continue;
        }
        if (s->rows[i].pdfPinned || it->bodyMatched) {
            continue;
        }
        if (s->engine->HasPageLabels()) {
            int pdf = s->engine->GetPageByLabel(it->printedLabel);
            if (pdf > 0 && (s->nPages < 1 || pdf <= s->nPages) && TocCalibParseLabelPrinted(it->printedLabel) < 1) {
                it->pageNo = pdf;
                s->rows[i].pdfPinned = true;
                s->rows[i].identPageNo = pdf;
                continue;
            }
        }
        if (TocCalibIsLetterDigitLabel(it->printedLabel)) {
            needRScan = true;
        } else {
            needScan = true;
        }
    }
    if (!needScan && !(needRScan && endBookRScan)) {
        return;
    }
    // Front matter + atlas usually sit early. Prefer atlas folio lines
    // ("Reference Atlas • RA1") over a contents page that lists every RA#.
    int limit = s->nPages > 0 ? s->nPages : s->engine->PageCount();
    int frontLimit = limit;
    if (frontLimit > 120) {
        frontLimit = 120;
    }
    Vec<char*> labs;
    Vec<int> labPdf;
    auto addLab = [&](const char* tok, int pdf) {
        if (!tok || !tok[0]) {
            return;
        }
        for (int k = 0; k < labs.Size(); k++) {
            if (labs[k] && str::EqI(labs[k], tok)) {
                // Prefer a later PDF for R# (handbook at end beats TOC listing).
                if (TocCalibIsLetterDigitLabel(tok) && pdf > labPdf[k]) {
                    labPdf[k] = pdf;
                }
                return;
            }
        }
        labs.Append(str::Dup(tok));
        labPdf.Append(pdf);
    };
    if (needScan) {
        for (int pdf = 1; pdf <= frontLimit; pdf++) {
            Vec<EngineMupdfPageLine> lines;
            if (!EngineMupdfCollectPageLines(s->engine, pdf, lines)) {
                continue;
            }
            float pageH = 0;
            for (int li = 0; li < lines.Size(); li++) {
                float b = lines[li].y + (lines[li].dy > 1 ? lines[li].dy : 10);
                if (b > pageH) {
                    pageH = b;
                }
            }
            for (int li = 0; li < lines.Size(); li++) {
                const char* text = lines[li].text;
                if (!text || !text[0]) {
                    continue;
                }
                char atlas[32]{};
                if (TocCalibExtractAtlasFolioLabel(text, atlas, (int)sizeof(atlas))) {
                    addLab(atlas, pdf);
                    continue;
                }
                float y = lines[li].y;
                bool band = pageH > 40 && (y >= pageH * 0.88f || y <= pageH * 0.10f);
                if (!band) {
                    continue;
                }
                int pr = 0;
                char tok[32]{};
                if (!TocCalibParseFooterToken(text, &pr, tok, (int)sizeof(tok))) {
                    continue;
                }
                // Roman / letter front matter in the true footer only. Skip lone
                // RA tokens here — those appear in printed TOC lists.
                if (pr < 1 && tok[0] && TocCalibParseRoman(tok) > 0) {
                    addLab(tok, pdf);
                }
            }
            EngineMupdfFreePageLines(lines);
        }
    }
    // R1… handbooks are near the end of G11-style books. Skip during AI
    // import (the full footer index covers them); use when reopening an
    // outline that already carries R labels but no index yet.
    if (needRScan && endBookRScan && limit > 0) {
        int rStart = after > 0 ? after + 1 : 1;
        int rFrom = limit;
        int budget = 500;
        if (rFrom - rStart + 1 > budget) {
            rStart = rFrom - budget + 1;
        }
        for (int pdf = rFrom; pdf >= rStart; pdf--) {
            int arabic = 0;
            char lab[16]{};
            TocCalibReadFooterMark(s->engine, pdf, &arabic, lab, (int)sizeof(lab));
            if (lab[0] && TocCalibIsLetterDigitLabel(lab)) {
                addLab(lab, pdf);
            }
        }
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || it->pageNo > 0 || TocCalibHasPrinted(it->printedPage)) {
            continue;
        }
        if (!it->printedLabel || !it->printedLabel[0]) {
            continue;
        }
        if (s->rows[i].pdfPinned || it->bodyMatched) {
            continue;
        }
        for (int k = 0; k < labs.Size(); k++) {
            if (labs[k] && str::EqI(labs[k], it->printedLabel)) {
                it->pageNo = labPdf[k];
                s->rows[i].pdfPinned = true;
                s->rows[i].identPageNo = labPdf[k];
                break;
            }
        }
    }
    for (int k = 0; k < labs.Size(); k++) {
        str::Free(labs[k]);
    }
}

static void TocCalibCollectArabicRuns(const TocCalibPrintedIndex* idx, Vec<TocCalibArabicRun>& runs) {
    runs.Reset();
    int prevPdf = 0;
    int prevPr = 0;
    bool open = false;
    for (int i = 0; i < idx->pdf.Size(); i++) {
        int pr = idx->arabic[i];
        if (pr < 1) {
            continue;
        }
        int pdf = idx->pdf[i];
        if (!open || !TocCalibArabicContinues(prevPdf, prevPr, pdf, pr)) {
            TocCalibArabicRun r;
            r.begin = i;
            r.end = i + 1;
            r.printedMin = pr;
            r.printedMax = pr;
            r.pdfMin = pdf;
            r.pdfMax = pdf;
            runs.Append(r);
            open = true;
        } else {
            TocCalibArabicRun& r = runs.Last();
            r.end = i + 1;
            if (pr < r.printedMin) {
                r.printedMin = pr;
            }
            if (pr > r.printedMax) {
                r.printedMax = pr;
            }
            if (pdf > r.pdfMax) {
                r.pdfMax = pdf;
            }
        }
        prevPdf = pdf;
        prevPr = pr;
    }
}

static int TocCalibRunPdfForPrinted(const TocCalibPrintedIndex* idx, const TocCalibArabicRun& run, int printed) {
    if (printed < run.printedMin || printed > run.printedMax) {
        return 0;
    }
    int anchorPdf = 0;
    int anchorPr = 0;
    for (int i = run.begin; i < run.end; i++) {
        int pr = idx->arabic[i];
        if (pr < 1) {
            continue;
        }
        if (pr == printed) {
            return idx->pdf[i];
        }
        if (pr < printed && (anchorPr < 1 || pr > anchorPr)) {
            anchorPr = pr;
            anchorPdf = idx->pdf[i];
        }
    }
    if (anchorPdf < 1) {
        return 0;
    }
    int pdf = anchorPdf + (printed - anchorPr);
    if (pdf < run.pdfMin) {
        pdf = run.pdfMin;
    }
    if (pdf > run.pdfMax) {
        pdf = run.pdfMax;
    }
    return pdf;
}

static bool TocCalibPdfAvailable(const Vec<u8>* usedPdf, int pdf) {
    if (pdf < 1) {
        return false;
    }
    if (!usedPdf || pdf >= usedPdf->Size()) {
        return true;
    }
    return (*usedPdf)[pdf] == 0;
}

static void TocCalibClaimPdf(Vec<u8>* usedPdf, int pdf) {
    if (!usedPdf || pdf < 1) {
        return;
    }
    if (pdf >= usedPdf->Size()) {
        int old = usedPdf->Size();
        usedPdf->SetSize(pdf + 1);
        for (int i = old; i <= pdf; i++) {
            (*usedPdf)[i] = 0;
        }
    }
    (*usedPdf)[pdf] = 1;
}

// First arabic run that extends past the printed TOC — the main body start.
// Do NOT pick the longest run: G11 skills/reference reprints restart at 1 with
// dense footers and out-length a body that was split by years / missed marks.
static int TocCalibPrimaryBodyRunIdx(const Vec<TocCalibArabicRun>& runs, int afterTocPdf) {
    int best = -1;
    int bestPdfMin = 0;
    for (int i = 0; i < runs.Size(); i++) {
        if (afterTocPdf > 0 && runs[i].pdfMax <= afterTocPdf) {
            continue;
        }
        if (best < 0 || runs[i].pdfMin < bestPdfMin) {
            best = i;
            bestPdfMin = runs[i].pdfMin;
        }
    }
    return best;
}

// Map a printed arabic folio to a PDF page. Prefer the earliest PDF page after
// the TOC (real body), then front matter, then later appendix reprints.
// usedPdf claims pages so TOC order consumes the early hit first and a later
// duplicate folio can take the reprint.
int TocCalibLookupArabic(const TocCalibPrintedIndex* idx, int afterTocPdf, int printed, const Vec<u8>* usedPdf) {
    if (!idx || printed < 1) {
        return 0;
    }
    Vec<TocCalibArabicRun> runs;
    TocCalibCollectArabicRuns(idx, runs);
    int primary = TocCalibPrimaryBodyRunIdx(runs, afterTocPdf);
    int bodyMin = primary >= 0 ? runs[primary].printedMin : 0;
    // Body often starts at 4 (G11). A post-TOC lone "1" from "Part 1" headers
    // must not beat front-matter maps at printed 1–2, and must not beat a later
    // real body folio when we are resolving UNIT pages (>= bodyMin).
    bool allowPostToc = bodyMin < 1 || printed >= bodyMin;

    // Exact hits — earliest eligible PDF wins even when that folio sits in a
    // short fragment and a long skills reprint also has the same number.
    int bestBody = 0;
    int bestFront = 0;
    for (int i = 0; i < idx->pdf.Size(); i++) {
        if (idx->arabic[i] != printed) {
            continue;
        }
        int pdf = idx->pdf[i];
        if (!TocCalibPdfAvailable(usedPdf, pdf)) {
            continue;
        }
        if (afterTocPdf > 0 && pdf > afterTocPdf) {
            if (!allowPostToc) {
                continue;
            }
            if (bestBody < 1 || pdf < bestBody) {
                bestBody = pdf;
            }
        } else if (bestFront < 1 || pdf < bestFront) {
            bestFront = pdf;
        }
    }
    if (bestBody > 0) {
        return bestBody;
    }
    if (bestFront > 0) {
        return bestFront;
    }

    // Interpolate gaps inside a run (sparse footer hits).
    auto tryInterp = [&](int runIdx) -> int {
        if (runIdx < 0 || runIdx >= runs.Size()) {
            return 0;
        }
        int pdf = TocCalibRunPdfForPrinted(idx, runs[runIdx], printed);
        return TocCalibPdfAvailable(usedPdf, pdf) ? pdf : 0;
    };
    if (allowPostToc) {
        int pdf = tryInterp(primary);
        if (pdf > 0) {
            return pdf;
        }
    }
    int bestFrontPdf = 0;
    int bestFrontMin = 0;
    for (int i = 0; i < runs.Size(); i++) {
        if (afterTocPdf > 0 && runs[i].pdfMax > afterTocPdf) {
            continue;
        }
        int p = tryInterp(i);
        if (p > 0 && (bestFrontPdf < 1 || runs[i].pdfMin < bestFrontMin)) {
            bestFrontPdf = p;
            bestFrontMin = runs[i].pdfMin;
        }
    }
    if (bestFrontPdf > 0) {
        return bestFrontPdf;
    }
    // Later appendix / skills arabic restarts (primary missed or claimed). Only
    // for folios that belong on the body scale — never promote a post-TOC
    // noise "1" / reprint when the body itself starts at 4+.
    if (!allowPostToc) {
        return 0;
    }
    int bestOtherPdf = 0;
    int bestOtherMin = 0;
    for (int i = 0; i < runs.Size(); i++) {
        if (i == primary) {
            continue;
        }
        if (afterTocPdf > 0 && runs[i].pdfMax <= afterTocPdf) {
            continue;
        }
        int p = tryInterp(i);
        if (p > 0 && (bestOtherPdf < 1 || runs[i].pdfMin < bestOtherMin)) {
            bestOtherPdf = p;
            bestOtherMin = runs[i].pdfMin;
        }
    }
    return bestOtherPdf;
}

int TocCalibLookupArabic(const TocCalibPrintedIndex* idx, int afterTocPdf, int printed) {
    return TocCalibLookupArabic(idx, afterTocPdf, printed, nullptr);
}

int TocCalibLookupLabel(const TocCalibPrintedIndex* idx, int afterTocPdf, const char* label) {
    if (!idx || !label || !label[0]) {
        return 0;
    }
    // R1 / RA36 handbooks sit at the end of the PDF. An early printed-TOC
    // listing that leaked into the index must not win over the real folio.
    bool preferLate = TocCalibIsLetterDigitLabel(label);
    int front = 0;
    int body = 0;
    for (int i = 0; i < idx->label.Size(); i++) {
        if (!idx->label[i] || !str::EqI(idx->label[i], label)) {
            continue;
        }
        int pdf = idx->pdf[i];
        if (afterTocPdf > 0 && pdf > afterTocPdf) {
            if (preferLate) {
                if (pdf > body) {
                    body = pdf;
                }
            } else if (body < 1) {
                body = pdf;
            }
        } else if (preferLate) {
            if (pdf > front) {
                front = pdf;
            }
        } else if (front < 1) {
            front = pdf;
        }
    }
    return body > 0 ? body : front;
}

static int TocCalibBodyPrintedMin(const TocCalibPrintedIndex* idx, int afterTocPdf) {
    Vec<TocCalibArabicRun> runs;
    TocCalibCollectArabicRuns(idx, runs);
    int primary = TocCalibPrimaryBodyRunIdx(runs, afterTocPdf);
    if (primary < 0) {
        return 0;
    }
    return runs[primary].printedMin;
}

// Structural parents (AI page:null / "Reference Section") keep the tree but
// often have no folio of their own. Point them at the first child that has a
// PDF dest and/or a printed folio / R1 label so the page field is not "?".
static int TocCalibFillPageFromChildren(ExtractedTocItem* item) {
    if (!item) {
        return 0;
    }
    int first = 0;
    int firstPr = 0;
    const char* firstLab = nullptr;
    for (int i = 0; i < item->children.Size(); i++) {
        ExtractedTocItem* child = item->children[i];
        int childPage = TocCalibFillPageFromChildren(child);
        if (first < 1 && childPage > 0) {
            first = childPage;
            firstPr = child->printedPage;
            firstLab = child->printedLabel;
        }
        if (!firstLab && child && child->printedLabel && child->printedLabel[0]) {
            firstLab = child->printedLabel;
            if (!TocCalibHasPrinted(firstPr) && TocCalibHasPrinted(child->printedPage)) {
                firstPr = child->printedPage;
            }
            if (first < 1 && child->pageNo > 0) {
                first = child->pageNo;
            }
        }
        if (!TocCalibHasPrinted(firstPr) && child && TocCalibHasPrinted(child->printedPage)) {
            firstPr = child->printedPage;
            if (first < 1 && child->pageNo > 0) {
                first = child->pageNo;
            }
            if (!firstLab && child->printedLabel && child->printedLabel[0]) {
                firstLab = child->printedLabel;
            }
        }
    }
    if (item->pageNo < 1 && first > 0) {
        item->pageNo = first;
    }
    if (!TocCalibHasPrinted(item->printedPage) && TocCalibHasPrinted(firstPr)) {
        item->printedPage = firstPr;
    }
    if ((!item->printedLabel || !item->printedLabel[0]) && firstLab && firstLab[0]) {
        str::ReplaceWithCopy(&item->printedLabel, firstLab);
    }
    return item->pageNo;
}

static void TocCalibApplyPrintedIndex(TocCalibSession* s, const TocCalibPrintedIndex* idx) {
    if (!s || !idx || idx->pdf.Size() < 1) {
        return;
    }
    int after = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    int bodyMin = TocCalibBodyPrintedMin(idx, after);
    int nHit = 0;
    Vec<u8> usedPdf;
    int nPages = s->nPages > 0 ? s->nPages : (s->engine ? s->engine->PageCount() : 0);
    if (nPages > 0) {
        usedPdf.SetSize(nPages + 1);
        for (int i = 0; i < usedPdf.Size(); i++) {
            usedPdf[i] = 0;
        }
    }
    // TOC order: claim early body pages first so a later appendix arabic restart
    // (skills / unit reprints) cannot steal UNIT ONE's printed 19.
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || s->rows[i].pdfPinned || s->rows[i].userSet) {
            continue;
        }
        if (TocCalibHasPrinted(it->printedPage)) {
            int pdf = TocCalibLookupArabic(idx, after, it->printedPage, nPages > 0 ? &usedPdf : nullptr);
            if (pdf > 0 && (nPages < 1 || pdf <= nPages)) {
                it->pageNo = pdf;
                s->rows[i].identPageNo = pdf;
                s->rows[i].pdfPinned = true;
                TocCalibClaimPdf(&usedPdf, pdf);
                nHit++;
            } else if (bodyMin > 1 && it->printedPage < bodyMin) {
                // Number belongs to the pre-TOC run and was not printed there.
                // printed+offset would land inside the contents.
                it->pageNo = 0;
                s->rows[i].identPageNo = 0;
            }
            continue;
        }
        if (it->printedLabel && it->printedLabel[0]) {
            int pdf = TocCalibLookupLabel(idx, after, it->printedLabel);
            // Do not claim label pages: Reference Section and Literary Terms
            // Handbook both print R1 and must share PDF 1394.
            if (pdf > 0 && (nPages < 1 || pdf <= nPages)) {
                it->pageNo = pdf;
                s->rows[i].identPageNo = pdf;
                s->rows[i].pdfPinned = true;
                nHit++;
            }
        }
    }
    for (int i = 0; i < s->roots.Size(); i++) {
        TocCalibFillPageFromChildren(s->roots[i]);
    }
    // Parents that just inherited an R1/roman label still need a PDF dest.
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it || s->rows[i].pdfPinned || s->rows[i].userSet || it->pageNo > 0) {
            continue;
        }
        if (!it->printedLabel || !it->printedLabel[0]) {
            continue;
        }
        int pdf = TocCalibLookupLabel(idx, after, it->printedLabel);
        if (pdf > 0 && (s->nPages < 1 || pdf <= s->nPages)) {
            it->pageNo = pdf;
            s->rows[i].identPageNo = pdf;
            s->rows[i].pdfPinned = true;
            nHit++;
        }
    }
    // AI page:0 leaves (e.g. "Literary Maps") have no folio. The printed TOC
    // literally lists 0 as a placeholder; the heading sits in front matter.
    // Search early pages by title — cheap, and 1800-page books skip full BM25.
    {
        int frontHi = after > 0 ? after : 48;
        if (frontHi < 24) {
            frontHi = 24;
        }
        if (frontHi > 80) {
            frontHi = 80;
        }
        Vec<int> pages;
        Vec<Vec<EngineMupdfPageLine>*> cache;
        for (int i = 0; i < s->rows.Size(); i++) {
            ExtractedTocItem* it = s->rows[i].item;
            if (!it || s->rows[i].pdfPinned || s->rows[i].userSet) {
                continue;
            }
            if (it->pageNo > 0 || TocCalibHasPrinted(it->printedPage) || (it->printedLabel && it->printedLabel[0])) {
                continue;
            }
            const char* title = it->rawTitle && it->rawTitle[0] ? it->rawTitle : it->title;
            if (!title || TocCalibGlyphCount(title) < 4) {
                continue;
            }
            TocCalibNearHit hit{};
            bool found = false;
            for (int center = 8; center <= frontHi && !found; center += 8) {
                found = TocCalibSearchNearPage(s, title, center, 4, pages, cache, &hit);
                if (found && TocCalibPageInToc(s, hit.page)) {
                    found = false;
                }
            }
            if (found && TocCalibApplyNearHit(it, hit.page, hit.x, hit.y, hit.score, 0)) {
                s->rows[i].identPageNo = it->pageNo;
                nHit++;
            }
        }
        TocCalibFreePageCache(cache);
        for (int i = 0; i < s->roots.Size(); i++) {
            TocCalibFillPageFromChildren(s->roots[i]);
        }
    }
    s->footerMapped = true;
    logf("TocCalib: footer index marks=%d afterToc=%d bodyMin=%d hits=%d\n", idx->pdf.Size(), after, bodyMin, nHit);
}

static void TocCalibReadFooterMark(EngineBase* engine, int pdf, int* arabicOut, char* labOut, int labCap,
                                   bool footerOnly) {
    if (arabicOut) {
        *arabicOut = 0;
    }
    if (labOut && labCap > 0) {
        labOut[0] = 0;
    }
    if (!engine || pdf < 1) {
        return;
    }
    Vec<EngineMupdfPageLine> lines;
    if (!EngineMupdfCollectPageLines(engine, pdf, lines)) {
        EngineMupdfTrimPageCaches(engine, 0, 0);
        return;
    }
    float pageH = 0;
    for (int i = 0; i < lines.Size(); i++) {
        float b = lines[i].y + (lines[i].dy > 1 ? lines[i].dy : 10);
        if (b > pageH) {
            pageH = b;
        }
    }
    int arabic = 0;
    char lab[16]{};
    bool have = false;
    float bestFooterY = -1e9f;
    float bestHeaderY = 1e9f;
    int headerArabic = 0;
    char headerLab[16]{};
    bool headerHave = false;
    // Real folio sits in the outer ~8% (this book: y≈748/783). The old 22%
    // band also swallowed timeline years and footnotes and split the run.
    for (int i = 0; i < lines.Size(); i++) {
        float y = lines[i].y;
        bool isFooter = pageH > 40 && y >= pageH * 0.92f;
        bool isHeader = pageH > 40 && y <= pageH * 0.08f;
        if (!isFooter && !isHeader) {
            continue;
        }
        if (TocCalibLooksLikeTocLeaderLine(lines[i].text)) {
            continue;
        }
        int pr = 0;
        char tok[16]{};
        if (!TocCalibParseFooterToken(lines[i].text, &pr, tok, (int)sizeof(tok))) {
            if (!TocCalibExtractTrailingLetterDigitLabel(lines[i].text, tok, (int)sizeof(tok))) {
                continue;
            }
            pr = 0;
        }
        if (isFooter) {
            if (have && y < bestFooterY) {
                continue;
            }
            bestFooterY = y;
            arabic = pr;
            lab[0] = 0;
            if (pr < 1) {
                int copy = (int)str::Len(tok);
                if (copy > (int)sizeof(lab) - 1) {
                    copy = (int)sizeof(lab) - 1;
                }
                memcpy(lab, tok, (size_t)copy);
                lab[copy] = 0;
            }
            have = true;
        } else if (!headerHave || y < bestHeaderY) {
            bestHeaderY = y;
            headerArabic = pr;
            headerLab[0] = 0;
            if (pr < 1) {
                int copy = (int)str::Len(tok);
                if (copy > (int)sizeof(headerLab) - 1) {
                    copy = (int)sizeof(headerLab) - 1;
                }
                memcpy(headerLab, tok, (size_t)copy);
                headerLab[copy] = 0;
            }
            headerHave = true;
        }
    }
    if (!have && headerHave && !footerOnly) {
        arabic = headerArabic;
        int copy = (int)str::Len(headerLab);
        if (copy > (int)sizeof(lab) - 1) {
            copy = (int)sizeof(lab) - 1;
        }
        memcpy(lab, headerLab, (size_t)copy);
        lab[copy] = 0;
        have = true;
    }
    EngineMupdfFreePageLines(lines);
    EngineMupdfTrimPageCaches(engine, 0, 0);
    if (!have) {
        return;
    }
    if (arabicOut) {
        *arabicOut = arabic;
    }
    if (labOut && labCap > 0 && lab[0]) {
        int copy = (int)str::Len(lab);
        if (copy > labCap - 1) {
            copy = labCap - 1;
        }
        memcpy(labOut, lab, (size_t)copy);
        labOut[copy] = 0;
    }
}

// Returns true when every page has been visited.
static bool TocCalibPrintedIndexScanChunk(EngineBase* engine, TocCalibPrintedIndex* idx, int* nextPage, int nPages,
                                          DWORD budgetMs) {
    if (!engine || !idx || !nextPage || nPages < 1) {
        return true;
    }
    DWORD t0 = ::GetTickCount();
    while (*nextPage <= nPages) {
        int pdf = *nextPage;
        int arabic = 0;
        char lab[16]{};
        TocCalibReadFooterMark(engine, pdf, &arabic, lab, (int)sizeof(lab));
        TocCalibPrintedIndexAdd(idx, pdf, arabic, lab);
        (*nextPage)++;
        if (budgetMs > 0 && ::GetTickCount() - t0 >= budgetMs) {
            return *nextPage > nPages;
        }
    }
    return true;
}

bool TocCalibTestPrintedIndexLookup() {
    TocCalibPrintedIndex idx;
    // G11 shape: maps 1–2, then a contents gap, then body starting at printed 4.
    TocCalibPrintedIndexAdd(&idx, 17, 0, "xxxvi");
    TocCalibPrintedIndexAdd(&idx, 22, 1, nullptr);
    TocCalibPrintedIndexAdd(&idx, 23, 2, nullptr);
    TocCalibPrintedIndexAdd(&idx, 45, 4, nullptr);
    TocCalibPrintedIndexAdd(&idx, 46, 5, nullptr);
    TocCalibPrintedIndexAdd(&idx, 60, 19, nullptr);
    TocCalibPrintedIndexAdd(&idx, 1700, 0, "R1");
    bool ok = TocCalibLookupArabic(&idx, 44, 4) == 45;
    ok = ok && TocCalibLookupArabic(&idx, 44, 19) == 60;
    ok = ok && TocCalibLookupArabic(&idx, 44, 1) == 22;
    ok = ok && TocCalibLookupArabic(&idx, 44, 3) == 0;
    ok = ok && TocCalibLookupArabic(&idx, 44, 6) == 47;
    ok = ok && TocCalibLookupLabel(&idx, 44, "xxxvi") == 17;
    ok = ok && TocCalibLookupLabel(&idx, 44, "XXXVI") == 17;
    ok = ok && TocCalibLookupLabel(&idx, 44, "R1") == 1700;
    // Early TOC listing must not beat the real handbook folio at the end.
    TocCalibPrintedIndexAdd(&idx, 8, 0, "R1");
    ok = ok && TocCalibLookupLabel(&idx, 44, "R1") == 1700;
    // Preface 1–2, then the body restarts at 1. Primary body run (past TOC) wins.
    TocCalibPrintedIndex idx2;
    TocCalibPrintedIndexAdd(&idx2, 3, 1, nullptr);
    TocCalibPrintedIndexAdd(&idx2, 4, 2, nullptr);
    TocCalibPrintedIndexAdd(&idx2, 30, 1, nullptr);
    TocCalibPrintedIndexAdd(&idx2, 31, 2, nullptr);
    TocCalibPrintedIndexAdd(&idx2, 32, 3, nullptr);
    ok = ok && TocCalibLookupArabic(&idx2, 10, 1) == 30;
    ok = ok && TocCalibLookupArabic(&idx2, 10, 2) == 31;
    // A year in the margin must not join the body run and drag later pages.
    TocCalibPrintedIndex idx3;
    TocCalibPrintedIndexAdd(&idx3, 45, 4, nullptr);
    TocCalibPrintedIndexAdd(&idx3, 46, 5, nullptr);
    TocCalibPrintedIndexAdd(&idx3, 48, 1778, nullptr);
    TocCalibPrintedIndexAdd(&idx3, 60, 19, nullptr);
    ok = ok && TocCalibLookupArabic(&idx3, 44, 4) == 45;
    ok = ok && TocCalibLookupArabic(&idx3, 44, 19) == 60;
    // Body folio 19 plus a late skills-section reprint of 19: primary body wins.
    // After claiming PDF 60, the reprint is available for a later TOC row.
    TocCalibPrintedIndex idx4;
    TocCalibPrintedIndexAdd(&idx4, 45, 4, nullptr);
    TocCalibPrintedIndexAdd(&idx4, 60, 19, nullptr);
    TocCalibPrintedIndexAdd(&idx4, 61, 20, nullptr);
    TocCalibPrintedIndexAdd(&idx4, 1550, 18, nullptr);
    TocCalibPrintedIndexAdd(&idx4, 1552, 19, nullptr);
    TocCalibPrintedIndexAdd(&idx4, 1553, 20, nullptr);
    ok = ok && TocCalibLookupArabic(&idx4, 44, 19) == 60;
    Vec<u8> used;
    used.SetSize(1600);
    for (int i = 0; i < used.Size(); i++) {
        used[i] = 0;
    }
    TocCalibClaimPdf(&used, 60);
    ok = ok && TocCalibLookupArabic(&idx4, 44, 19, &used) == 1552;
    // G11 regression: body footers shatter into 1–2 page fragments (years /
    // "Part 1" noise) while the skills reprint is one long run starting at 1.
    // Longest-run primary wrongly mapped UNIT ONE to Reference; earliest
    // post-TOC exact hit must still win.
    TocCalibPrintedIndex idx5;
    TocCalibPrintedIndexAdd(&idx5, 22, 1, nullptr);
    TocCalibPrintedIndexAdd(&idx5, 45, 4, nullptr);
    TocCalibPrintedIndexAdd(&idx5, 46, 5, nullptr);
    TocCalibPrintedIndexAdd(&idx5, 47, 1, nullptr); // header noise
    TocCalibPrintedIndexAdd(&idx5, 48, 7, nullptr);
    TocCalibPrintedIndexAdd(&idx5, 60, 19, nullptr);
    TocCalibPrintedIndexAdd(&idx5, 61, 20, nullptr);
    TocCalibPrintedIndexAdd(&idx5, 203, 162, nullptr);
    for (int p = 0; p < 35; p++) {
        TocCalibPrintedIndexAdd(&idx5, 1534 + p, 1 + (p % 3), nullptr);
    }
    TocCalibPrintedIndexAdd(&idx5, 1552, 19, nullptr);
    ok = ok && TocCalibLookupArabic(&idx5, 44, 4) == 45;
    ok = ok && TocCalibLookupArabic(&idx5, 44, 19) == 60;
    ok = ok && TocCalibLookupArabic(&idx5, 44, 162) == 203;
    ok = ok && TocCalibLookupArabic(&idx5, 44, 1) == 22;
    for (int i = 0; i < idx.label.Size(); i++) {
        str::Free(idx.label[i]);
    }
    for (int i = 0; i < idx2.label.Size(); i++) {
        str::Free(idx2.label[i]);
    }
    for (int i = 0; i < idx3.label.Size(); i++) {
        str::Free(idx3.label[i]);
    }
    for (int i = 0; i < idx4.label.Size(); i++) {
        str::Free(idx4.label[i]);
    }
    for (int i = 0; i < idx5.label.Size(); i++) {
        str::Free(idx5.label[i]);
    }
    return ok;
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
    if (TocCalibIsAlphaToken(buf) || TocCalibIsLetterDigitLabel(buf)) {
        if (labelOut) {
            *labelOut = str::Dup(buf);
        }
        return true;
    }
    return false;
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
    lab = nullptr;
    ok = ok && TocCalibParsePrintedText("R1", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "R1");
    str::Free(lab);
    lab = nullptr;
    ok = ok && TocCalibParsePrintedText("R20", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "R20");
    str::Free(lab);
    lab = nullptr;
    ok = ok && TocCalibParsePrintedText("RA1", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "RA1");
    str::Free(lab);
    lab = nullptr;
    ok = ok && TocCalibParsePrintedText("RA36", &pr, &lab) && pr == 0 && lab && str::Eq(lab, "RA36");
    str::Free(lab);
    ok = ok && !TocCalibParsePrintedText("12a", &pr, &lab);
    ok = ok && TocCalibParseRoman("II") == 2 && TocCalibParseRoman("xiv") == 14 && TocCalibParseRoman("A") == 0;
    char trail[32]{};
    ok = ok && TocCalibExtractTrailingLetterDigitLabel("LITERARY TERMS HANDBOOK R1", trail, (int)sizeof(trail)) &&
         str::Eq(trail, "R1");
    ok = ok && TocCalibExtractTrailingLetterDigitLabel("READING HANDBOOK R21", trail, (int)sizeof(trail)) &&
         str::Eq(trail, "R21");
    ok = ok && TocCalibExtractTrailingLetterDigitLabel("R20", trail, (int)sizeof(trail)) && str::Eq(trail, "R20");
    ok = ok && TocCalibExtractTrailingLetterDigitLabel("HANDBOOKR1", trail, (int)sizeof(trail)) && str::Eq(trail, "R1");
    ok = ok && !TocCalibExtractTrailingLetterDigitLabel("UNIT ONE", trail, (int)sizeof(trail));
    ok = ok && TocCalibLooksLikeTocLeaderLine("Business Writing . . . . . . . . R42");
    ok = ok && !TocCalibLooksLikeTocLeaderLine("LITERARY TERMS HANDBOOK R1");
    ok = ok && !TocCalibLooksLikeTocLeaderLine("R20");
    return ok;
}

bool TocCalibTestOffsetIgnoresRoughEstimate() {
    // lastToc(44)+printed yields a consistent wrong offset of 44; evidence from
    // a body hit must win so textbooks with long front matter map correctly.
    Vec<TocCalibMapRow> rows;
    for (int i = 0; i < 8; i++) {
        TocCalibMapRow r;
        r.printedPage = 4 + i * 10;
        r.pdfPage = 44 + r.printedPage; // poisoned lastToc+printed
        r.identPage = r.pdfPage;
        r.bodyMatched = false;
        rows.Append(r);
    }
    TocCalibMapRow hit;
    hit.printedPage = 4;
    hit.pdfPage = 45;
    hit.identPage = 45;
    hit.bodyMatched = true;
    rows.Append(hit);
    TocCalibMapRow hit2;
    hit2.printedPage = 19;
    hit2.pdfPage = 60;
    hit2.identPage = 60;
    hit2.bodyMatched = true;
    rows.Append(hit2);
    int off = TocCalibSolveOffset(rows);
    return off == 41;
}

bool TocCalibTestEstimateArabicOffsetVotes() {
    // Pure unit of MajorityOffset via the public estimator with no engine: the
    // estimator needs an engine, so validate the solve path only here.
    Vec<TocCalibMapRow> rows;
    TocCalibMapRow a;
    a.printedPage = 1;
    a.pdfPage = 42;
    a.identPage = 42;
    a.verified = true;
    rows.Append(a);
    TocCalibMapRow b;
    b.printedPage = 2;
    b.pdfPage = 43;
    b.identPage = 43;
    b.verified = true;
    rows.Append(b);
    TocCalibMapRow c;
    c.printedPage = 10;
    c.pdfPage = 51;
    c.identPage = 51;
    c.pdfPinned = true;
    rows.Append(c);
    return TocCalibSolveOffset(rows) == 41;
}

bool TocCalibTestPiecewiseOffset() {
    TocCalibSession s;
    s.nPages = 450;
    s.map.offset = 11;
    s.map.confidence = 1;
    ExtractedTocItem a;
    ExtractedTocItem b;
    ExtractedTocItem c;
    ExtractedTocItem d;
    a.title = str::Dup("甲");
    a.printedPage = 46;
    a.pageNo = 57;
    b.title = str::Dup("乙");
    b.printedPage = 63;
    b.pageNo = 73;
    c.title = str::Dup("丙");
    c.printedPage = 75;
    c.pageNo = 86;
    d.title = str::Dup("丁");
    d.printedPage = 210;
    d.pageNo = 221;
    TocCalibRow ra;
    TocCalibRow rb;
    TocCalibRow rc;
    TocCalibRow rd;
    ra.item = &a;
    rb.item = &b;
    rb.pdfPinned = true;
    rc.item = &c;
    rd.item = &d;
    s.rows.Append(ra);
    s.rows.Append(rb);
    s.rows.Append(rc);
    s.rows.Append(rd);
    TocCalibRememberOffsetSeg(&s, 63, 10);
    TocCalibSolveSession(&s);
    bool ok = s.map.offset == 11;
    ok = ok && a.printedPage == 46 && b.printedPage == 63 && c.printedPage == 75 && d.printedPage == 210;
    ok = ok && a.pageNo == 57 && b.pageNo == 73 && c.pageNo == 85 && d.pageNo == 220;
    TocCalibRememberOffsetSeg(&s, 206, 9);
    TocCalibSolveSession(&s);
    ok = ok && s.map.offset == 11 && s.offsetSegs.Size() == 2;
    ok = ok && a.pageNo == 57 && b.pageNo == 73 && c.pageNo == 85 && d.pageNo == 219;
    ok = ok && a.printedPage == 46 && b.printedPage == 63 && c.printedPage == 75 && d.printedPage == 210;
    b.printedPage = 10;
    TocCalibSolveSession(&s);
    ok = ok && b.printedPage == 10 && b.pageNo == 73;
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
        if (!it || it->destinationSource == TocDestinationSource::PdfLink || TocCalibIsContentsTitle(it->title)) {
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
    // Do not invent a folio from pdf-offset: on multi-schema books (body + R#
    // + skills reprints) that paints the PDF index into the printed field and
    // makes Unit bookmarks look "numbered" while pointing at Reference.
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
        // A pinned row already has the folio the user or the AI kept. Do not
        // invent one from neighboring numbers.
        if (s->rows[i].pdfPinned) {
            continue;
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
        if (!it || it->destinationSource == TocDestinationSource::PdfLink || TocCalibIsContentsTitle(it->title)) {
            continue;
        }
        if (s->rows[i].clamped) {
            // A row clamped to the last page carries no order information;
            // it must not drag the remaining rows there.
            s->rows[i].needsConfirm = true;
            continue;
        }
        if (it->printedPage > 0) {
            if (prevPr > 0 && it->printedPage < prevPr) {
                // Pinned and hand-edited folios stay. Clearing them makes the
                // left column disagree with the number printed on the page.
                if (!s->rows[i].userSet && !s->rows[i].pdfPinned) {
                    it->printedPage = 0;
                    it->verified = false;
                    if (s->rows[i].origPageNo > 0) {
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
            if (it->printedPage > 0) {
                int rowOff = TocCalibOffsetForPrinted(s, it->printedPage);
                if (rowOff >= 0) {
                    pred = TocCalibPredPdf(it->printedPage, rowOff, s->nPages);
                }
            }
            if (it->bodyMatched) {
                // Direct body-text evidence outranks the running order when
                // the two conflict: keep the verified page (the earlier row
                // is the likely garbage) and re-anchor the sequence on it.
                s->rows[i].needsConfirm = true;
            } else {
                it->pageNo = pred >= prevPdf ? pred : prevPdf;
                it->bodyMatched = false;
                s->rows[i].needsConfirm = true;
                pdf = it->pageNo;
            }
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
    int offset = s->map.offset;
    if (s->offsetSegs.Size() > 0) {
        // Missing sheets are separate segments. A majority vote would fold
        // them back into one offset and then rewrite folios to match it.
    } else if (s->offsetLocked && offset >= 0) {
        s->map.confidence = 1;
    } else {
        Vec<TocCalibMapRow> mapRows;
        TocCalibRowsToMap(s, mapRows);
        offset = TocCalibSolveOffset(mapRows);
        if (offset >= 0) {
            s->map.offset = offset;
            s->map.confidence = 0.9f;
        } else if (!(s->map.confidence > 0 && s->map.offset >= 0)) {
            // Keep a footer-seeded offset when body evidence is not ready yet.
            s->map.confidence = 0;
        }
    }
    Vec<int> pdgMap;
    bool pdgBook = TocCalibBuildPdgPrintedMap(s->engine, pdgMap);
    TocCalibFillAllPrinted(s);
    int pMin = 0;
    int pMax = 0;
    bool havePrinted = false;
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        if (!s->rows[i].pdfPinned && !TocCalibPrintedPlausible(s->nPages, it->printedPage)) {
            // Garbage printed (OCR mangling): drop it and keep the extracted
            // pdf page instead of clamping a bogus printed+offset to nPages.
            // Pinned rows keep the folio the user or the AI already stored.
            it->printedPage = 0;
            it->verified = false;
        }
        if (!s->rows[i].pdfPinned) {
            int rowOff = TocCalibOffsetForPrinted(s, it->printedPage);
            int pdgPdf = pdgBook ? TocCalibPdgMapPdf(pdgMap, it->printedPage) : 0;
            int labelPdf = TocCalibPdfForPrintedLabel(s->engine, it->printedPage);
            if (pdgPdf > 0 && (s->nPages < 1 || pdgPdf <= s->nPages)) {
                // Sheet filename is the printed page. Do not add a global offset.
                it->pageNo = pdgPdf;
                s->rows[i].identPageNo = pdgPdf;
            } else if (labelPdf > 0) {
                // /PageLabels already names this printed page (front matter is
                // A, B, i, ii; body "1" is not PDF page 1).
                it->pageNo = labelPdf;
                s->rows[i].identPageNo = labelPdf;
            } else if (!pdgBook && TocCalibHasPrinted(it->printedPage) && rowOff >= 0) {
                it->pageNo = TocCalibPredPdf(it->printedPage, rowOff, s->nPages);
            } else if (!TocCalibHasPrinted(it->printedPage) && s->rows[i].origPageNo > 0) {
                it->pageNo = s->rows[i].origPageNo;
            } else if (pdgBook && TocCalibHasPrinted(it->printedPage)) {
                it->pageNo = 0;
            }
        }
        s->rows[i].clamped = false;
        if (s->nPages > 0 && it->pageNo > s->nPages) {
            it->pageNo = s->nPages;
            s->rows[i].clamped = true;
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
        if (!it || !TocCalibPrintedPlausible(s->nPages, it->printedPage) || s->rows[i].pdfPinned) {
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
            if (!row || !TocCalibPrintedPlausible(s->nPages, row->printedPage) || s->rows[i].pdfPinned) {
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
            if (!row || !TocCalibPrintedPlausible(s->nPages, row->printedPage) || s->rows[i].pdfPinned) {
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
    NormalizeTocNumberingParens(&n->title);
    NormalizeTocNumberingParens(&n->rawTitle);
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

TocCalibSession* TocCalibSessionFromExtracted(Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                                              bool scanBody, bool deferVerify) {
    auto* s = new TocCalibSession;
    s->engine = engine;
    s->persistToDisk = persistToDisk;
    s->nPages = engine ? engine->PageCount() : 0;
    for (int i = 0; i < roots.Size(); i++) {
        s->roots.Append(roots[i]);
    }
    roots.Reset();
    TocCalibNormalizeForestTitles(s->roots);
    TocCalibPrepareMapping(s, true, scanBody, deferVerify);
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

static bool TocCalibLiveApply(MainWindow* win, bool rebuildTree);
static void TocCalibSyncTreeDests(MainWindow* win, TocCalibSession* s);

static int TocCalibRowIndex(const TocCalibSession* s, const TocCalibRow* row) {
    if (!s || !row) {
        return -1;
    }
    for (int i = 0; i < s->rows.Size(); i++) {
        if (&s->rows[i] == row) {
            return i;
        }
    }
    return -1;
}

// pdf - printed for this row. Negative means the PDF page is before the folio.
static bool TocCalibRowPageOffset(const TocCalibRow* row, int* offsetOut) {
    if (!row || !row->item || !TocCalibHasPrinted(row->item->printedPage) || row->item->pageNo < 1) {
        return false;
    }
    int off = row->item->pageNo - row->item->printedPage;
    if (off < 0) {
        return false;
    }
    if (offsetOut) {
        *offsetOut = off;
    }
    return true;
}

bool TocCalibCanApplyOffsetBelow(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !win->tocTreeView) {
        return false;
    }
    TocItem* item = (TocItem*)win->tocTreeView->GetSelection();
    TocCalibRow* row = TocCalibRowForTocItem(win, item);
    return TocCalibRowPageOffset(row, nullptr);
}

bool TocCalibApplyOffsetBelow(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s || !win->tocTreeView) {
        return false;
    }
    TocItem* item = (TocItem*)win->tocTreeView->GetSelection();
    TocCalibRow* anchor = TocCalibRowForTocItem(win, item);
    int idx = TocCalibRowIndex(s, anchor);
    int offset = 0;
    if (idx < 0 || !TocCalibRowPageOffset(anchor, &offset)) {
        return false;
    }
    TocCalibRemember(s);
    for (int i = 0; i < s->rows.Size(); i++) {
        ExtractedTocItem* it = s->rows[i].item;
        if (!it) {
            continue;
        }
        // Freeze rows above, and later rows the user already edited, so the
        // next solve cannot pull them onto this new offset.
        if (i < idx || (i > idx && s->rows[i].userSet) || !TocCalibHasPrinted(it->printedPage)) {
            if (it->pageNo > 0) {
                s->rows[i].pdfPinned = true;
            }
            continue;
        }
        if (i == idx) {
            s->rows[i].pdfPinned = true;
            continue;
        }
        int pdf = TocCalibPredPdf(it->printedPage, offset, s->nPages);
        it->pageNo = pdf;
        s->rows[i].identPageNo = pdf;
        s->rows[i].pdfPinned = true;
        s->rows[i].needsConfirm = false;
        it->verified = true;
        it->bodyMatched = false;
    }
    // Record the segment and patch dests. Do not run the single-offset solve:
    // that pass rewrites printed folios so they no longer match the page.
    TocCalibRememberOffsetSeg(s, anchor->item->printedPage, offset);
    TocCalibSyncTreeDests(win, s);
    InvalidateTocTree(win);
    return true;
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
static bool TocCalibPushOutline(MainWindow* win, TocCalibSession* s, bool syncFromTree = true,
                                char** errOut = nullptr) {
    if (errOut) {
        *errOut = nullptr;
    }
    if (!win || !s || !s->engine || s->roots.Size() < 1) {
        return false;
    }
    if (syncFromTree) {
        TocCalibSyncHierarchyFromTree(win, s);
        TocCalibSyncTitlesFromTree(win, s);
    }
    char* err = nullptr;
    bool ok = EngineMupdfReplacePdfToc(s->engine, s->roots, &err);
    if (errOut) {
        *errOut = err; // caller owns (surface the mupdf reason in the UI)
    } else {
        str::Free(err);
    }
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
        int rowOff = TocCalibOffsetForPrinted(s, it->printedPage);
        if (rowOff < 0) {
            continue;
        }
        it->pageNo = TocCalibPredPdf(it->printedPage, rowOff, s->nPages);
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
    if (s && row && row->item && TocCalibHasPrinted(row->item->printedPage)) {
        int rowOff = TocCalibOffsetForPrinted(s, row->item->printedPage);
        int pred = rowOff >= 0 ? TocCalibPredPdf(row->item->printedPage, rowOff, s->nPages) : 0;
        if (pred > 0 && !TocCalibPageInToc(s, pred)) {
            return pred;
        }
    }
    return 0;
}

static void TocCalibFindDeadlineCb(DWORD* deadline, ProgressUpdateData* data) {
    if (deadline && data && data->wasCancelled && ::GetTickCount() >= *deadline) {
        *data->wasCancelled = true;
    }
}

static bool TocCalibSearchTextFind(TocCalibSession* s, const char* query, int predPage, TocCalibNearHit* out,
                                   bool stopAtFirst, DWORD budgetMs) {
    if (!s || !s->engine || !query || !query[0] || !out) {
        return false;
    }
    if (TocCalibGlyphCount(query) < 4) {
        return false;
    }
    if (budgetMs < 40) {
        budgetMs = 40;
    }
    WCHAR* w = ToWStrTemp(query);
    if (!w || !w[0]) {
        return false;
    }
    TextSearch ts(s->engine);
    ts.SetMatchCase(false);
    ts.SetMatchWholeWord(false);
    // Cap Find so a miss on a large / OCR book cannot monopolize the UI thread
    // (each LoadPageText may also trigger Auto-OCR).
    DWORD deadline = ::GetTickCount() + budgetMs;
    ts.progressCb = MkFunc1(TocCalibFindDeadlineCb, &deadline);
    int startPage = predPage > 0 ? predPage : 1;
    TextSel* sel = ts.FindFirst(startPage, w);
    int nHits = 0;
    int nScan = 0;
    int bestPage = 0;
    float bestX = 0;
    float bestY = 0;
    int bestDist = 0;
    while (sel && sel->len > 0 && nHits < 32 && nScan < 80) {
        if (::GetTickCount() >= deadline) {
            break;
        }
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
            if (stopAtFirst) {
                break;
            }
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
                                           TocCalibNearHit* hit, int startPage, bool stopAtFirst, DWORD budgetMs) {
    if (!s || !s->engine || !title || !hit) {
        return false;
    }
    TocCalibFindQueries q;
    if (!TocCalibBuildFindQueries(title, &q) || q.n < 1) {
        return false;
    }
    int pred = startPage > 0 ? startPage : TocCalibLocatePredPage(s, row);
    for (int i = 0; i < q.n; i++) {
        if (TocCalibSearchTextFind(s, q.q[i], pred, hit, stopAtFirst, budgetMs)) {
            return true;
        }
    }
    return false;
}

static bool TocCalibNotifyNoBodyHit(MainWindow* win) {
    if (win && win->hwndCanvas) {
        ShowTemporaryNotification(win->hwndCanvas, _TRA("Could not find this heading in the body"));
    }
    return false;
}

// Pin icon: open the find results list with this title so the user can pick
// among TOC / body hits. Do not auto-pin — the chain button links the page.
// Start from the current view page downward (sequential TOC correction).
static bool TocCalibLocateOpenFindList(MainWindow* win, TocCalibRow* row) {
    if (!win || !row || !row->item) {
        return false;
    }
    const char* title = row->item->rawTitle && row->item->rawTitle[0] ? row->item->rawTitle : row->item->title;
    TocCalibFindQueries q;
    if (!TocCalibBuildFindQueries(title, &q) || q.n < 1 || !q.q[0] || !q.q[0][0]) {
        return TocCalibNotifyNoBodyHit(win);
    }
    const char* query = q.q[0];
    ShowFindBar(win);
    if (!win->hwndFindEdit) {
        return false;
    }
    // Compact docked find hides the snippet list; float so hits are choosable.
    FindWindowSetDocked(win, false);
    // Fill the query without the edit's change handler, which only lists hits
    // and waits for Enter. FindBeginFromPage starts the scan and jumps to the
    // first hit at/after the current page as soon as it is found.
    FindWindowSetSuppressTextChanged(win, true);
    HwndSetText(win->hwndFindEdit, query);
    Edit_SetModify(win->hwndFindEdit, TRUE);
    FindWindowSetSuppressTextChanged(win, false);
    int page = win->ctrl ? win->ctrl->CurrentPageNo() : 1;
    if (page < 1) {
        page = 1;
    }
    FindBeginFromPage(win, page);
    return true;
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
    return TocCalibLocateOpenFindList(win, row);
}

static bool TocCalibLocateRowInBody(MainWindow* win, TocCalibRow* row) {
    // Stay on the page the user is already reviewing; the find list picks the
    // first hit at/after that page. Do not jump to a predicted early/TOC hit.
    return TocCalibLocateOpenFindList(win, row);
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
    // An unresolved (gray) row must remain the immediate merge target.
    b->pageNo = 0;
    b->printedPage = 0;
    auto* c = new ExtractedTocItem;
    c->title = str::Dup("后续条目");
    c->rawTitle = str::Dup("后续条目");
    c->level = 1;
    c->pageNo = 19;
    c->printedPage = 5;
    s.roots.Append(a);
    s.roots.Append(b);
    s.roots.Append(c);
    TocCalibLinkParents(s.roots, nullptr);
    TocCalibCollectRows(&s);
    bool ok = TocCalibMergeWithNext(&s, &s.rows[0]);
    ok = ok && s.roots.Size() == 2 && s.roots[0] == a && s.roots[1] == c;
    ok = ok && str::Eq(a->title, "认为高考竞争对孩子不利是错误的");
    ok = ok && a->pageNo == 18 && a->printedPage == 4;
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

struct TocCalibSaveErrCapture {
    char* err = nullptr;
};

static void TocCalibOnSaveError(TocCalibSaveErrCapture* ctx, const char* msg) {
    if (msg) {
        str::Free(ctx->err);
        ctx->err = str::Dup(msg);
    }
}

static void TocCalibReportWriteError(MainWindow* win, char* err) {
    // Surface the actual mupdf reason ("journaling", file lock, permissions…)
    // instead of the generic "Could not write the PDF table of contents."
    TempWStr detail = err ? ToWStrTemp(err) : nullptr;
    const WCHAR* msg = (detail && detail[0]) ? detail : ToWStrTemp(_TRA("Could not write the PDF table of contents."));
    MessageBoxW(win->hwndFrame, msg, L"PDF table of contents", MB_OK | MB_ICONERROR);
    str::Free(err);
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
    char* pushErr = nullptr;
    if (!TocCalibPushOutline(win, s, true, &pushErr)) {
        TocCalibReportWriteError(win, pushErr);
        return false;
    }
    str::Free(pushErr);
    if (s->persistToDisk) {
        tab->ignoreNextAutoReload = true;
        TocCalibSaveErrCapture saveErr;
        auto saveErrCb = MkFunc1<TocCalibSaveErrCapture, const char*>(TocCalibOnSaveError, &saveErr);
        char* tmp = nullptr;
        bool saved = EngineMupdfSaveUpdated(s->engine, nullptr, saveErrCb, &tmp);
        if (!saved) {
            tab->ignoreNextAutoReload = false;
            TocCalibReportWriteError(win, saveErr.err);
            return false;
        }
        s->engine->ClearUnsavedOcrText();
        if (tmp) {
            // Package was written to a sidecar because the open file is locked.
            // Replace it, or ask the user. Do not invent a second copy.
            const char* path = s->engine->FilePath();
            bool replaced = SwitchCurrentTabToSavedFile(win, path, tmp);
            str::Free(tmp);
            if (!replaced && IsMainWindowValid(win)) {
                HideTocCalib(win);
            }
            return replaced;
        }
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
    // Transaction rollback: restore both the outline contents and the engine's
    // TOC-dirty flag captured at session start. Other dirty sources
    // (annotations, unsaved OCR text) are untouched and stay as they were.
    bool baselineModifiedToc = s->baselineModifiedToc;
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
        // ReplacePdfToc re-marks the engine dirty; restore the baseline taken
        // before the session so a cancelled extraction leaves no phantom dirty.
        EngineMupdfSetPdfTocModified(engine, baselineModifiedToc);
    }
    DeleteExtractedTocItems(backup);
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
    // Refresh the tab dirty indicators right now: they must mirror the real
    // unsaved state without waiting for a tab switch.
    ToolbarUpdateStateForWindow(win, false);
}

void CancelTocExtractionPreview(MainWindow* win) {
    TocCalibCancel(win);
}

static void TocCalibFinish(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (!s) {
        HideTocCalib(win);
        return;
    }
    TocCalibRestoreDisplayMode(win, s);
    tab->tocCalib = nullptr;
    DeleteTocCalibSession(s);
    HideTocCalib(win);
    if (win->tocLoaded) {
        ClearTocBox(win);
    }
    LoadTocTree(win);
}

struct TocCalibBar : Wnd {
    MainWindow* win = nullptr;
    HWND panel = nullptr;
    HWND jumpToc = nullptr;
    HWND done = nullptr;
    HWND cancel = nullptr;

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
    if (panel) DestroyWindow(panel);

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
    panel = CreateDialogParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_TOC_FOOTER), hwnd,
        [](HWND dlg, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
            auto* bar = (TocCalibBar*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
            if (msg == WM_INITDIALOG) {
                SetWindowLongPtrW(dlg, GWLP_USERDATA, lp);
                if (UseDarkModeLib()) DarkMode::setDarkWndSafe(dlg);
                return TRUE;
            }
            if (msg == WM_COMMAND && bar && HIWORD(wp) == BN_CLICKED) {
                if (LOWORD(wp) == 100)
                    bar->OnJumpToc();
                else if (LOWORD(wp) == IDOK)
                    bar->OnDone();
                else if (LOWORD(wp) == IDCANCEL)
                    bar->OnCancel();
                else
                    return FALSE;
                return TRUE;
            }
            return FALSE;
        },
        (LPARAM)this);
    if (!panel) return false;
    jumpToc = GetDlgItem(panel, 100);
    done = GetDlgItem(panel, IDOK);
    cancel = GetDlgItem(panel, IDCANCEL);
    HwndSetText(jumpToc, _TRA("Contents page"));
    HwndSetText(done, _TRA("Save"));
    HwndSetText(cancel, _TRA("Cancel"));
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
    MoveWindow(panel, 0, 1, dx, std::max(1, dy - 1), TRUE);
    int pad = DpiScale(hwnd, 8);
    int gap = DpiScale(hwnd, 8);
    int inner = dx - 2 * pad;
    if (inner < 40) {
        inner = 40;
    }
    RECT buttonUnits{0, 0, 50, 14};
    MapDialogRect(panel, &buttonUnits);
    int btnDy = buttonUnits.bottom;
    int third = std::max(1, std::min((int)buttonUnits.right, (inner - 2 * gap) / 3));
    int top = std::max(0, (dy - btnDy) / 2);
    int jumpWidth = jumpToc ? ButtonGetIdealSize(jumpToc).dx : third;
    jumpWidth = std::max(1, std::min(jumpWidth, inner - 2 * third - 2 * gap));
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
        placeBtn(jumpToc, pad, top, jumpWidth, btnDy);
    }
    if (done) {
        placeBtn(done, dx - pad - 2 * third - gap, top, third, btnDy);
    }
    if (cancel) {
        placeBtn(cancel, dx - pad - third, top, third, btnDy);
    }
}

LRESULT TocCalibBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(ThemeSidebarBackgroundColor());
        FillRect((HDC)wparam, &rc, br);
        DeleteObject(br);
        rc.bottom = rc.top + 1;
        br = CreateSolidBrush(ThemeSidebarSeparatorColor(SidebarSeparatorState::Normal));
        FillRect((HDC)wparam, &rc, br);
        DeleteObject(br);
        return 1;
    }
    return Wnd::WndProc(hwnd, msg, wparam, lparam);
}

void TocCalibBar::OnJumpToc() {
    TocCalibJumpToContents(win);
}

void TocCalibBar::OnDone() {
    if (TocCalibWriteBookmarks(win)) {
        // Exit without restoring the old snapshot: the saved outline is now
        // the document state, so there is no post-save "Cancel" state.
        TocCalibFinish(win);
    }
}

void TocCalibBar::OnCancel() {
    TocCalibCancel(win);
}

void TocCalibBar::UpdateTheme() {
    COLORREF colBg = 0;
    COLORREF colTxt = 0;
    ThemeSidebarColors(colBg, colTxt);
    SetColors(colTxt, colBg);
    if (hwnd) {
        if (UseDarkModeLib()) DarkMode::setDarkWndSafe(panel);
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
    return DpiScale(win->hwndTocBox, 42);
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
    RECT arrow{};
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

static void TocCalibPlaceBox(RECT& field, int x, int yMid, int fieldDx, int fieldH) {
    field.left = x;
    field.right = x + fieldDx;
    field.top = yMid - fieldH / 2;
    field.bottom = field.top + fieldH;
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
    int fieldDx = DpiScale(hwnd, 48);
    int gap = DpiScale(hwnd, 4);
    int groupGap = DpiScale(hwnd, 6);
    int pad = DpiScale(hwnd, 4);
    int rowH = rcRow.bottom - rcRow.top;
    int fieldH = DpiScale(hwnd, 18);
    if (fieldH > rowH - 2) {
        fieldH = rowH - 2;
    }
    if (fieldH < 12) {
        fieldH = rowH > 2 ? rowH - 2 : rowH;
    }
    int yMid = (rcRow.top + rcRow.bottom) / 2;
    L.showPrinted = true;
    L.showPdf = true;
    int locDx = DpiScale(hwnd, 16);
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
    // [printed] → [pdf] locate associate merge delete
    TocCalibPlaceIcon(L.del, rcRow.right - pad, midDx, midDy, yMid);
    TocCalibPlaceIcon(L.merge, L.del.left - gap, midDx, midDy, yMid);
    TocCalibPlaceIcon(L.associate, L.merge.left - gap, midDx, midDy, yMid);
    TocCalibPlaceIcon(L.locate, L.associate.left - gap, locDx, locDy, yMid);
    int arrowDx = DpiScale(hwnd, 28);
    int xPdf = L.locate.left - groupGap - fieldDx;
    TocCalibPlaceBox(L.pdfField, xPdf, yMid, fieldDx, fieldH);
    int xArrow = xPdf - gap - arrowDx;
    L.arrow.left = xArrow;
    L.arrow.right = xArrow + arrowDx;
    L.arrow.top = yMid - fieldH / 2;
    L.arrow.bottom = L.arrow.top + fieldH;
    int xPr = L.arrow.left - gap - fieldDx;
    TocCalibPlaceBox(L.prField, xPr, yMid, fieldDx, fieldH);
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
    return rc.right - L.prField.left + pad;
}

static bool TocCalibPtInRect(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

static bool TocCalibEditingField(MainWindow* win, TocItem* item, bool printed) {
    return gCalibPageEdit.hwnd && gCalibPageEdit.win == win && item && item->id == gCalibPageEdit.tocId &&
           gCalibPageEdit.printed == printed;
}

static COLORREF TocCalibBlend(COLORREF a, COLORREF b, int bPct) {
    if (bPct < 0) {
        bPct = 0;
    }
    if (bPct > 100) {
        bPct = 100;
    }
    int aPct = 100 - bPct;
    auto ch = [](int ca, int cb, int ap, int bp) { return (ca * ap + cb * bp) / 100; };
    return RGB(ch(GetRValue(a), GetRValue(b), aPct, bPct), ch(GetGValue(a), GetGValue(b), aPct, bPct),
               ch(GetBValue(a), GetBValue(b), aPct, bPct));
}

static void TocCalibDrawPageField(HDC hdc, const RECT& rc, const WCHAR* text, bool empty, bool editing, bool enabled,
                                  bool muted) {
    if (rc.right <= rc.left || rc.bottom <= rc.top || editing) {
        return;
    }
    COLORREF bg = ThemeFindEditBackgroundColor();
    if (muted) {
        COLORREF side = 0;
        COLORREF sideTxt = 0;
        ThemeSidebarColors(side, sideTxt);
        bg = TocCalibBlend(bg, side, 42);
    }
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

static bool TocCalibUiZh() {
    const char* lang = trans::GetCurrentLangCode();
    return lang && (str::EqI(lang, "cn") || str::EqI(lang, "tw"));
}

static bool TocCalibDisplayOffset(const TocCalibRow* row, int* offOut) {
    if (!row || !row->item || !TocCalibHasPrinted(row->item->printedPage) || row->item->pageNo < 1) {
        return false;
    }
    if (offOut) {
        *offOut = row->item->pageNo - row->item->printedPage;
    }
    return true;
}

// True when this folio's pdf−printed differs from the previous row that has one.
static bool TocCalibOffsetBreaks(const TocCalibSession* s, const TocCalibRow* row, int off) {
    int idx = TocCalibRowIndex(s, row);
    if (idx < 1) {
        return false;
    }
    for (int i = idx - 1; i >= 0; i--) {
        int prev = 0;
        if (!TocCalibDisplayOffset(&s->rows[i], &prev)) {
            continue;
        }
        return prev != off;
    }
    return false;
}

// LineTo omits the end pixel, which was clipping the arrow tip and the lower stroke.
static void TocCalibStroke(HDC hdc, int x1, int y1, int x2, int y2, COLORREF color) {
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SetPixel(hdc, x2, y2, color);
}

static void TocCalibDrawShaftArrow(HDC hdc, int x1, int x2, int y, int head, COLORREF color) {
    if (x2 <= x1 || head < 2) {
        return;
    }
    int headUp = head / 2;
    int headDn = head - headUp;
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    TocCalibStroke(hdc, x1, y, x2, y, color);
    TocCalibStroke(hdc, x2 - head, y - headUp, x2, y, color);
    TocCalibStroke(hdc, x2, y, x2 - head, y + headDn, color);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

// Closer to body text than the disabled gray, on both light and dark sidebars.
static COLORREF TocCalibOffsetInk(bool emphasize) {
    COLORREF txt = ThemeWindowTextColor();
    if (emphasize) {
        return txt;
    }
    COLORREF side = 0;
    COLORREF sideTxt = 0;
    ThemeSidebarColors(side, sideTxt);
    return TocCalibBlend(txt, side, 22);
}

// +N above the shaft, with a clear gap. The pair is centered in the row.
static void TocCalibDrawOffsetArrow(HDC hdc, HWND hwnd, const RECT& col, const RECT& row, const WCHAR* offset,
                                    bool emphasize) {
    if (col.right <= col.left || row.bottom <= row.top) {
        return;
    }
    int rowH = row.bottom - row.top;
    if (rowH < 4) {
        return;
    }
    int inset = DpiScale(hwnd, 5);
    int x1 = col.left + inset;
    int x2 = col.right - inset;
    int head = (x2 - x1) / 5;
    if (head < 3) {
        head = 3;
    }
    if (head > 5) {
        head = 5;
    }
    int headUp = head / 2;
    int headDn = head - headUp;
    TEXTMETRIC tm{};
    int ascent = rowH * 2 / 3;
    if (GetTextMetrics(hdc, &tm) && tm.tmAscent > 0) {
        ascent = tm.tmAscent;
    }
    int gap = DpiScale(hwnd, 3);
    if (gap < 2) {
        gap = 2;
    }
    int block = ascent + gap + head;
    int textTop = row.top;
    if (block < rowH) {
        textTop = row.top + (rowH - block) / 2;
    }
    // Keep the whole arrowhead, including the lower stroke, inside the row.
    int limit = row.bottom - headDn - 1;
    int baseline = textTop + ascent;
    int arrowY = baseline + gap + headUp;
    if (arrowY > limit) {
        textTop -= arrowY - limit;
        if (textTop < row.top) {
            textTop = row.top;
        }
        baseline = textTop + ascent;
        arrowY = baseline + gap + headUp;
        if (arrowY > limit) {
            arrowY = limit;
        }
    }
    if (arrowY - headUp < baseline + 1 && baseline + 1 + headUp <= limit) {
        arrowY = baseline + 1 + headUp;
    }
    COLORREF ink = TocCalibOffsetInk(emphasize && offset && offset[0]);
    SetBkMode(hdc, TRANSPARENT);
    if (offset && offset[0]) {
        RECT offRc{};
        offRc.left = col.left;
        offRc.right = col.right;
        offRc.top = textTop;
        offRc.bottom = baseline + 1;
        if (offRc.bottom > arrowY - headUp) {
            offRc.bottom = arrowY - headUp;
        }
        if (offRc.bottom <= offRc.top) {
            offRc.bottom = offRc.top + 1;
        }
        SetTextColor(hdc, ink);
        // "+199" in the 28px arrow column was ellipsized to "+1...", so the
        // printed page and the PDF page looked unrelated.
        HFONT shrink = nullptr;
        HFONT prevFont = nullptr;
        int len = (int)wcsnlen(offset, 16);
        SIZE sz{};
        if (len > 0 && GetTextExtentPoint32W(hdc, offset, len, &sz)) {
            int colW = col.right - col.left;
            if (sz.cx > colW && sz.cx > 0 && colW > 4) {
                LOGFONTW lf{};
                HFONT cur = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
                if (cur && GetObjectW(cur, sizeof(lf), &lf)) {
                    lf.lfHeight = MulDiv(lf.lfHeight, colW, sz.cx);
                    int minPx = DpiScale(hwnd, 8);
                    if (lf.lfHeight < 0 && -lf.lfHeight < minPx) {
                        lf.lfHeight = -minPx;
                    } else if (lf.lfHeight > 0 && lf.lfHeight < minPx) {
                        lf.lfHeight = minPx;
                    }
                    shrink = CreateFontIndirectW(&lf);
                    if (shrink) {
                        prevFont = (HFONT)SelectObject(hdc, shrink);
                    }
                }
            }
        }
        DrawTextW(hdc, offset, len, &offRc, DT_SINGLELINE | DT_CENTER | DT_TOP | DT_NOPREFIX | DT_NOCLIP);
        if (prevFont) {
            SelectObject(hdc, prevFont);
        }
        if (shrink) {
            DeleteObject(shrink);
        }
    }
    TocCalibDrawShaftArrow(hdc, x1, x2, arrowY, head, ink);
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
    }
    WCHAR prBuf[16]{};
    const WCHAR* prText = L"";
    bool prEmpty = true;
    if (printedLab && printedLab[0] && !TocCalibHasPrinted(printed)) {
        prText = ToWStrTemp(printedLab);
        prEmpty = !prText || !prText[0];
    } else if (printed > 0) {
        _snwprintf(prBuf, dimof(prBuf), L"%d", printed);
        prText = prBuf;
        prEmpty = false;
    }
    if (prEmpty && row && row->needsConfirm) {
        prText = L"?";
        prEmpty = false;
    }
    WCHAR pdfBuf[16]{};
    const WCHAR* pdfText = L"";
    bool pdfEmpty = true;
    if (pdf > 0) {
        _snwprintf(pdfBuf, dimof(pdfBuf), L"%d", pdf);
        pdfText = pdfBuf;
        pdfEmpty = false;
    }
    bool zh = TocCalibUiZh();
    if (prEmpty) {
        prText = zh ? L"\u5370\u5237" : L"Print";
    }
    if (pdfEmpty) {
        pdfText = L"PDF";
    }
    TocCalibSpinLayout L = TocCalibMakeLayout(hwnd, rcRow, false);
    TocCalibDrawIconBtn(hdc, hwnd, L.locate, TbIcon::MapPin, true);
    TocCalibDrawIconBtn(hdc, hwnd, L.associate, TbIcon::Link, true);
    TocCalibDrawIconBtn(hdc, hwnd, L.merge, TbIcon::MergeUp, true);
    TocCalibDrawIconBtn(hdc, hwnd, L.del, TbIcon::Trash, true);
    WCHAR offBuf[16]{};
    const WCHAR* offText = nullptr;
    bool offBreak = false;
    int off = 0;
    if (TocCalibDisplayOffset(row, &off)) {
        if (off > 0) {
            _snwprintf(offBuf, dimof(offBuf), L"+%d", off);
        } else if (off < 0) {
            _snwprintf(offBuf, dimof(offBuf), L"%d", off);
        } else {
            _snwprintf(offBuf, dimof(offBuf), L"+0");
        }
        offText = offBuf;
        offBreak = TocCalibOffsetBreaks(s, row, off);
    }
    TocCalibDrawPageField(hdc, L.prField, prText, prEmpty, TocCalibEditingField(win, item, true), true, false);
    TocCalibDrawOffsetArrow(hdc, hwnd, L.arrow, rcRow, offText, offBreak);
    TocCalibDrawPageField(hdc, L.pdfField, pdfText, pdfEmpty, TocCalibEditingField(win, item, false), true, true);
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
    if (TocCalibPtInRect(L.prField, x, y)) {
        return TocCalibHit::PrintedField;
    }
    if (TocCalibPtInRect(L.pdfField, x, y)) {
        return TocCalibHit::PdfField;
    }
    return TocCalibHit::None;
}

// Arabic folio printed in the header/footer band (same token rules as
// KeepExistingDests). Used when the user types a printed page and we must
// find the PDF sheet that actually carries that number.
static int TocCalibBandArabicOnPage(EngineBase* engine, int pdf) {
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
    int arabic = 0;
    for (int i = 0; i < lines.Size(); i++) {
        float y = lines[i].y;
        if (pageH <= 40 || (y < pageH * 0.78f && y > pageH * 0.12f)) {
            continue;
        }
        int pr = 0;
        char tok[32]{};
        if (!TocCalibParseFooterToken(lines[i].text, &pr, tok, (int)sizeof(tok))) {
            continue;
        }
        if (pr > 0 && pr != pdf) {
            arabic = pr;
        }
    }
    EngineMupdfFreePageLines(lines);
    return arabic;
}

// Prefer the PDF page whose footer/header is exactly `printed`. A global
// offset can point at a sheet that still shows the previous folio (G11:
// printed 359 + offset 41 → PDF 400, but PDF 400 is still "329"; the real
// "359" is PDF 430).
static int TocCalibFindPdfForPrinted(TocCalibSession* s, int printed, int hint) {
    if (!s || !s->engine || printed < 1) {
        return 0;
    }
    int nPages = s->nPages > 0 ? s->nPages : s->engine->PageCount();
    if (nPages < 1) {
        return 0;
    }
    int start = hint;
    if (start < 1) {
        int rowOff = TocCalibOffsetForPrinted(s, printed);
        if (rowOff >= 0) {
            start = TocCalibPredPdf(printed, rowOff, nPages);
        }
    }
    if (start < 1) {
        start = printed;
    }
    if (start < 1) {
        start = 1;
    }
    if (start > nPages) {
        start = nPages;
    }
    if (TocCalibBandArabicOnPage(s->engine, start) == printed) {
        return start;
    }
    const int kRadius = 120;
    for (int rad = 1; rad <= kRadius; rad++) {
        int a = start + rad;
        int b = start - rad;
        if (a >= 1 && a <= nPages && TocCalibBandArabicOnPage(s->engine, a) == printed) {
            return a;
        }
        if (b >= 1 && b <= nPages && TocCalibBandArabicOnPage(s->engine, b) == printed) {
            return b;
        }
    }
    int rowOff = TocCalibOffsetForPrinted(s, printed);
    if (rowOff >= 0) {
        return TocCalibPredPdf(printed, rowOff, nPages);
    }
    return 0;
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
    TocCalibClearPrintedLabel(row->item);
    row->item->printedPage = printed;
    row->userSet = true;
    row->needsConfirm = false;
    row->item->verified = TocCalibHasPrinted(printed);
    row->item->x = 0;
    row->item->y = 0;
    if (!TocCalibHasPrinted(printed)) {
        row->pdfPinned = false;
        if (row->origPageNo > 0) {
            row->item->pageNo = row->origPageNo;
        }
        return;
    }
    int hint = 0;
    int rowOff = TocCalibOffsetForPrinted(s, printed);
    if (rowOff >= 0) {
        hint = TocCalibPredPdf(printed, rowOff, s->nPages);
    } else if (row->item->pageNo > 0) {
        hint = row->item->pageNo;
    }
    int pdf = TocCalibFindPdfForPrinted(s, printed, hint);
    if (pdf > 0) {
        row->item->pageNo = pdf;
        row->identPageNo = pdf;
        // Lock the destination the user asked for. printed+offset alone can
        // land on a page whose footer is still the old number (G11 Part 2).
        row->pdfPinned = true;
    } else {
        row->pdfPinned = false;
    }
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

static void TocCalibSyncPrintedFromDest(TocCalibSession* s, TocCalibRow* row) {
    if (!s || !row || !row->item) {
        return;
    }
    int kept = row->item->printedPage;
    char* keptLab = row->item->printedLabel ? str::Dup(row->item->printedLabel) : nullptr;
    TocCalibClearPrintedLabel(row->item);
    row->item->printedPage = 0;
    int pdf = row->item->pageNo;
    int arabic = 0;
    char lab[16]{};
    if (pdf >= 1 && s->engine) {
        TocCalibReadFooterMark(s->engine, pdf, &arabic, lab, (int)sizeof(lab), true);
    }
    if (arabic > 0) {
        row->item->printedPage = arabic;
        str::Free(keptLab);
        return;
    }
    if (lab[0]) {
        str::ReplaceWithCopy(&row->item->printedLabel, lab);
        str::Free(keptLab);
        return;
    }
    // A scan has no footer text. Keep the folio already on the row instead of
    // clearing it and letting the single-offset solve invent another one.
    row->item->printedPage = kept;
    if (keptLab && keptLab[0]) {
        str::ReplaceWithCopy(&row->item->printedLabel, keptLab);
    }
    str::Free(keptLab);
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
        row->identPageNo = value;
        row->pdfPinned = true;
        row->userSet = true;
        row->needsConfirm = false;
        TocCalibSyncPrintedFromDest(s, row);
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
        row->needsConfirm = false;
    } else if (value > 0) {
        row->item->printedPage = value;
        row->userSet = true;
        row->needsConfirm = false;
        row->item->verified = true;
        row->item->x = 0;
        row->item->y = 0;
        int hint = 0;
        int rowOff = TocCalibOffsetForPrinted(s, value);
        if (rowOff >= 0) {
            hint = TocCalibPredPdf(value, rowOff, s->nPages);
        } else if (row->item->pageNo > 0) {
            hint = row->item->pageNo;
        }
        int pdf = TocCalibFindPdfForPrinted(s, value, hint);
        if (pdf > 0) {
            row->item->pageNo = pdf;
            row->identPageNo = pdf;
            row->pdfPinned = true;
        } else {
            row->pdfPinned = false;
        }
    }
    TocCalibLiveApply(win, false);
}

static int TocCalibFindPdfForLabel(TocCalibSession* s, const char* label) {
    if (!s || !s->engine || !label || !label[0]) {
        return 0;
    }
    int after = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    if (s->printedIdx) {
        int pdf = TocCalibLookupLabel(s->printedIdx, after, label);
        if (pdf > 0) {
            return pdf;
        }
    }
    if (s->engine->HasPageLabels()) {
        int pdf = s->engine->GetPageByLabel(label);
        if (pdf > 0 && (s->nPages < 1 || pdf <= s->nPages)) {
            return pdf;
        }
    }
    if (!TocCalibIsLetterDigitLabel(label)) {
        return 0;
    }
    int nPages = s->nPages > 0 ? s->nPages : s->engine->PageCount();
    if (nPages < 1) {
        return 0;
    }
    int rStart = after > 0 ? after + 1 : 1;
    int budget = 500;
    if (nPages - rStart + 1 > budget) {
        rStart = nPages - budget + 1;
    }
    for (int pdf = nPages; pdf >= rStart; pdf--) {
        int arabic = 0;
        char lab[16]{};
        TocCalibReadFooterMark(s->engine, pdf, &arabic, lab, (int)sizeof(lab));
        if (lab[0] && str::EqI(lab, label)) {
            return pdf;
        }
    }
    return 0;
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
    int pdf = TocCalibFindPdfForLabel(s, label);
    bool sameLab = row->item->printedPage == 0 && row->item->printedLabel && str::Eq(row->item->printedLabel, label);
    if (sameLab && row->pdfPinned && row->item->pageNo > 0 && (pdf < 1 || pdf == row->item->pageNo)) {
        TocCalibChooseColumn(row, false);
        return;
    }
    TocCalibRemember(s);
    TocCalibChooseColumn(row, false);
    row->item->printedPage = 0;
    str::ReplaceWithCopy(&row->item->printedLabel, label);
    row->userSet = true;
    row->needsConfirm = false;
    if (pdf > 0) {
        row->item->pageNo = pdf;
        row->identPageNo = pdf;
        row->pdfPinned = true;
    } else if (!row->pdfPinned && row->origPageNo > 0) {
        row->item->pageNo = row->origPageNo;
        row->pdfPinned = false;
    } else {
        row->pdfPinned = false;
    }
    TocCalibLiveApply(win, false);
}

static int TocCalibNextVisibleTocId(MainWindow* win, int tocId);
static void TocCalibStartFieldEdit(MainWindow* win, int tocId, bool printed);

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
                TocCalibStartFieldEdit(win, nextId, true);
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
    const WCHAR* cue = printed ? (TocCalibUiZh() ? L"\u5370\u5237\u9875\u7801" : L"Printed page")
                               : (TocCalibUiZh() ? L"PDF \u9875\u7801" : L"PDF page");
    SendMessageW(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    TocCalibRow* row = TocCalibRowForTocItem(win, item);
    if (!row) {
        TocCalibRebind(win);
        row = TocCalibRowForTocItem(win, item);
    }
    int value = 0;
    const char* printedLab = nullptr;
    if (printed) {
        if (row && row->item && TocCalibHasPrinted(row->item->printedPage)) {
            value = row->item->printedPage;
        } else if (row && row->item && row->item->printedLabel && row->item->printedLabel[0]) {
            printedLab = row->item->printedLabel;
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

static void TocCalibStartFieldEdit(MainWindow* win, int tocId, bool printed) {
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
    TocCalibChooseColumn(TocCalibRowForTocItem(win, item), !printed);
    InvalidateRect(win->tocTreeView->hwnd, &rc, TRUE);
    TocCalibSpinLayout L = TocCalibMakeLayout(win->tocTreeView->hwnd, rc, false);
    TocCalibBeginPageEdit(win, item, printed ? L.prField : L.pdfField, printed);
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
    if (hit == TocCalibHit::PrintedField || hit == TocCalibHit::PdfField) {
        bool printed = hit == TocCalibHit::PrintedField;
        if (TocCalibEditingField(win, item, printed)) {
            return true;
        }
        int tocId = item->id;
        TocCalibClosePageEdit(true);
        TocCalibStartFieldEdit(win, tocId, printed);
        return true;
    }
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
    return hit == TocCalibHit::PrintedField || hit == TocCalibHit::PdfField;
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
        return _TRA("Search title (pick a hit, then link)");
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
    if (hit == TocCalibHit::PrintedField) {
        return TocCalibUiZh() ? "印刷页码" : _TRA("Printed page");
    }
    if (hit == TocCalibHit::PdfField) {
        return TocCalibUiZh() ? "PDF 页码" : _TRA("PDF page");
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

static int TocCalibPageTocScore(const Vec<EngineMupdfPageLine>* lines, bool* outHasWord = nullptr) {
    if (!lines) {
        if (outHasWord) *outHasWord = false;
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
    if (outHasWord) *outHasWord = hasWord;
    return sc;
}

void TocCalibSetConfirmedTocPages(TocCalibSession* s, const Vec<int>& pages) {
    if (!s) {
        return;
    }
    s->confirmedTocPages.Reset();
    for (int i = 0; i < pages.Size(); i++) {
        int page = pages[i];
        if (page < 1) {
            continue;
        }
        if (s->nPages > 0 && page > s->nPages) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < s->confirmedTocPages.Size(); j++) {
            if (s->confirmedTocPages[j] == page) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            s->confirmedTocPages.Append(page);
        }
    }
    if (s->confirmedTocPages.Size() < 1) {
        return;
    }
    s->tocPage = s->confirmedTocPages[0];
    s->tocEnd = s->confirmedTocPages.Last();
}

static void TocCalibEnsureTocRange(TocCalibSession* s) {
    if (!s || !s->engine) {
        return;
    }
    if (s->confirmedTocPages.Size() > 0) {
        s->tocPage = s->confirmedTocPages[0];
        s->tocEnd = s->confirmedTocPages.Last();
        return;
    }
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    int maxP = s->nPages < 30 ? s->nPages : 30;
    int start = s->tocPage;
    if (start < 1) {
        for (int p = 1; p <= maxP; p++) {
            const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
            bool hasWord = false;
            int sc = TocCalibPageTocScore(lines, &hasWord);
            // Require hasWord (目录/contents) for initial detection. A page with
            // only numbered lists can accidentally score >= 4 on content pages
            // of official documents without any real TOC.
            if (sc >= 4 && hasWord) {
                start = p;
                break;
            }
        }
    }
    int end = 0;
    if (start >= 1) {
        s->tocPage = start;
        end = s->tocEnd > start ? s->tocEnd : start;
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
    }
    // Leader pages often sit before the page that says "Contents"
    // (a book overview, then the worded contents). Fold that earlier run in
    // so body search does not treat those pages as the chapter text.
    int leadFirst = 0;
    int leadLast = 0;
    int leadGap = 0;
    int leadLimit = s->nPages < 48 ? s->nPages : 48;
    for (int p = 1; p <= leadLimit; p++) {
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (TocCalibPageTocScore(lines) >= 3) {
            if (leadFirst < 1) {
                leadFirst = p;
            }
            leadLast = p;
            leadGap = 0;
        } else if (leadFirst > 0) {
            leadGap++;
            if (leadGap >= 3) {
                break;
            }
        }
    }
    if (leadFirst > 0 && start >= 1) {
        if (leadFirst < start) {
            start = leadFirst;
        }
        if (leadLast > end) {
            end = leadLast;
        }
    }
    if (start >= 1) {
        s->tocPage = start;
        if (end > s->tocEnd) {
            s->tocEnd = end;
        }
    }
    TocCalibFreePageCache(cache);
}

static bool TocCalibConfirmedHas(const TocCalibSession* s, int page) {
    if (!s || page < 1 || s->confirmedTocPages.Size() < 1) {
        return false;
    }
    for (int i = 0; i < s->confirmedTocPages.Size(); i++) {
        if (s->confirmedTocPages[i] == page) {
            return true;
        }
    }
    return false;
}

static void TocCalibListTocSheets(TocCalibSession* s, Vec<int>& sheets) {
    sheets.Reset();
    if (!s) {
        return;
    }
    if (s->confirmedTocPages.Size() > 0) {
        for (int i = 0; i < s->confirmedTocPages.Size(); i++) {
            sheets.Append(s->confirmedTocPages[i]);
        }
        return;
    }
    TocCalibEnsureTocRange(s);
    int lo = s->tocPage;
    int hi = s->tocEnd > 0 ? s->tocEnd : s->tocPage;
    if (lo < 1) {
        return;
    }
    if (hi < lo) {
        hi = lo;
    }
    if (hi - lo > 24) {
        hi = lo + 24;
    }
    for (int p = lo; p <= hi; p++) {
        sheets.Append(p);
    }
}

// OCR text is one blob. Exact title wins; otherwise compare with spaces and
// punctuation removed, which is how a scanned contents line usually comes back.
static bool TocCalibUtf8HasTitle(const char* text, const char* title, int* byteOut) {
    if (byteOut) {
        *byteOut = -1;
    }
    if (!text || !text[0] || !title || !title[0]) {
        return false;
    }
    const char* hit = str::Find(text, title);
    if (hit) {
        if (byteOut) {
            *byteOut = (int)(hit - text);
        }
        return true;
    }
    int cap = (int)str::Len(text) + 8;
    if (cap < 16) {
        cap = 16;
    }
    if (cap > 16000) {
        cap = 16000;
    }
    char* page = AllocArray<char>(cap);
    char titleBuf[512];
    TocCalibCompact(text, page, cap);
    TocCalibCompact(title, titleBuf, (int)sizeof(titleBuf));
    bool ok = titleBuf[0] && page[0] && str::Find(page, titleBuf) != nullptr;
    free(page);
    return ok;
}

static bool TocCalibOcrTitleOnPage(EngineBase* engine, int page, const char* title, float* xOut, float* yOut) {
    if (!engine || page < 1 || !title || !title[0]) {
        return false;
    }
    int len = 0;
    Rect* coords = nullptr;
    const char* text = engine->GetTextForPageUtf8(page, &len, &coords);
    int byteAt = -1;
    bool hit = TocCalibUtf8HasTitle(text, title, &byteAt);
    if (!hit && !engine->PageHasUsableText(page)) {
        OcrRecognizeEnginePage(engine, page, true);
        text = engine->GetTextForPageUtf8(page, &len, &coords);
        hit = TocCalibUtf8HasTitle(text, title, &byteAt);
    }
    if (!hit) {
        return false;
    }
    if (xOut && yOut && byteAt >= 0 && coords && byteAt < len) {
        *xOut = (float)coords[byteAt].x;
        *yOut = (float)coords[byteAt].y;
    }
    return true;
}

static bool TocCalibFindTitleOnTocPages(TocCalibSession* s, const char* title, int* pageOut, float* xOut, float* yOut) {
    if (!s || !title || !title[0] || !pageOut) {
        return false;
    }
    if (TocCalibIsContentsTitle(title)) {
        return false;
    }
    Vec<int> sheets;
    TocCalibListTocSheets(s, sheets);
    if (sheets.Size() < 1) {
        return false;
    }
    Vec<int> pages;
    Vec<Vec<EngineMupdfPageLine>*> cache;
    Vec<int> needOcr;
    int bestSc = 0;
    int bestPage = 0;
    float bestX = 0;
    float bestY = 0;
    for (int si = 0; si < sheets.Size(); si++) {
        int p = sheets[si];
        const Vec<EngineMupdfPageLine>* lines = TocCalibCachePage(s, p, pages, cache);
        if (!lines || lines->Size() < 1) {
            if (s->confirmedTocPages.Size() > 0) {
                needOcr.Append(p);
            }
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
    if (bestPage > 0 && bestSc >= 2) {
        *pageOut = bestPage;
        if (xOut) {
            *xOut = bestX;
        }
        if (yOut) {
            *yOut = bestY;
        }
        return true;
    }
    for (int i = 0; i < needOcr.Size(); i++) {
        float x = 0;
        float y = 0;
        if (!TocCalibOcrTitleOnPage(s->engine, needOcr[i], title, &x, &y)) {
            continue;
        }
        *pageOut = needOcr[i];
        if (xOut) {
            *xOut = x;
        }
        if (yOut) {
            *yOut = y;
        }
        return true;
    }
    return false;
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
    // A stamped toc page is only a hit when it is one of the sheets the user
    // confirmed. The old stamp was "first item = first page, everything else
    // = last page", which skipped the per-title search.
    bool cachedSheet =
        it && it->tocPageNo > 0 && (s->confirmedTocPages.Size() < 1 || TocCalibConfirmedHas(s, it->tocPageNo));
    if (cachedSheet) {
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
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocCalibSession* s = tab ? tab->tocCalib : nullptr;
    if (s && s->confirmedTocPages.Size() > 0) {
        TocCalibJumpToPdfPoint(win, s->confirmedTocPages[0], 0, 0);
        return;
    }
    TocCalibJumpToRowContents(win, TocCalibSelectedRow(win));
}

void TocCalibJumpToItemContents(MainWindow* win, TocItem* item) {
    TocCalibJumpToRowContents(win, TocCalibRowForTocItem(win, item));
}

bool TocCalibBarVisible(MainWindow* win) {
    return win && win->tocCalibBar && win->tocCalibBar->IsVisible();
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
        DWORD tBar = ::GetTickCount();
        auto* w = new TocCalibBar();
        if (!w->Create(win)) {
            delete w;
            return;
        }
        win->tocCalibBar = w;
        logf("TocCalib showBar create=%ums\n", ::GetTickCount() - tBar);
    }
    DWORD tShow = ::GetTickCount();
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    HwndSetVisibility(win->tocCalibBar->hwnd, true);
    TocCalibUpdateTheme(win);
    RelayoutTocContainer(win);
    RelayoutTocCalib(win);
    FlushTocTreeWrapHeights(win);
    InvalidateTocTree(win);
    TocCalibEnterSinglePage(win);
    logf("TocCalib showBar layout=%ums\n", ::GetTickCount() - tShow);
}

static const char* TocCalibDebugSourceName(ExtractedTocSource source) {
    switch (source) {
        case ExtractedTocSource::PrintedToc:
            return "printed";
        case ExtractedTocSource::BodyInference:
            return "body";
        default:
            return "unknown";
    }
}

static void TocCalibDebugDumpExtracted(FILE* f, const Vec<ExtractedTocItem*>& nodes, int depth) {
    for (int i = 0; i < nodes.Size(); i++) {
        ExtractedTocItem* it = nodes[i];
        if (!it) {
            continue;
        }
        fprintf(f, "E depth=%d source=%s printed=%d pdf=%d title=%s\n", depth, TocCalibDebugSourceName(it->source),
                it->printedPage, it->pageNo, it->title ? it->title : "");
        TocCalibDebugDumpExtracted(f, it->children, depth + 1);
    }
}

static void TocCalibDebugDumpEngineTree(FILE* f, TocItem* first, int depth) {
    for (TocItem* it = first; it; it = it->next) {
        fprintf(f, "T depth=%d pdf=%d title=%s\n", depth, it->pageNo, it->title ? it->title : "");
        TocCalibDebugDumpEngineTree(f, it->child, depth + 1);
    }
}

static void TocCalibWriteFlowDebug(MainWindow* win, TocCalibSession* s, const char* stage) {
    if (!gCli || !gCli->extractTocDebug || !s || !s->engine || !s->engine->FilePath()) {
        return;
    }
    char* path = str::Join(path::GetPathNoExtTemp(s->engine->FilePath()), ".toc-flow-debug.txt");
    FILE* f = path ? fopen(path, "a") : nullptr;
    str::Free(path);
    if (!f) {
        return;
    }
    fprintf(f, "\n===== %s =====\n", stage ? stage : "TOC flow");
    TocCalibDebugDumpExtracted(f, s->roots, 1);
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    TocTree* tree = tab ? tab->currToc : nullptr;
    if (!tree || !tree->root) {
        fprintf(f, "T <tree not loaded>\n");
    } else {
        TocCalibDebugDumpEngineTree(f, tree->root->child, 1);
    }
    fclose(f);
}

// One ~40ms slice of the chunked body verification. Runs on the UI thread
// so it can safely touch the engine; yields between slices to keep the UI
// responsive and report progress.
static void TocCalibVerifySlice(TocCalibVerifyJob* job);

// Schedule the next verify slice with a short one-shot timer instead of a
// self-reposting task: the pump gap lets input messages (drag!) through.
static void TocCalibVerifyScheduleNext(TocCalibVerifyJob* job) {
    if (job->timerWnd) {
        SetTimer(job->timerWnd, 1, 25, nullptr);
    } else {
        uitask::Post(MkFunc0<TocCalibVerifyJob>(TocCalibVerifySlice, job), "TocCalibVerify");
    }
}

static LRESULT CALLBACK TocCalibVerifyTimerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    }
    if (msg == WM_TIMER && wp == 1) {
        KillTimer(hwnd, 1);
        auto* job = (TocCalibVerifyJob*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (job) {
            TocCalibVerifySlice(job);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND TocCalibVerifyTimerWindow(TocCalibVerifyJob* job) {
    static const wchar_t* kClass = L"SumatraTocCalibVerifyTimer";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TocCalibVerifyTimerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    // Message-only window: receives WM_TIMER, never shown, no taskbar.
    return CreateWindowExW(0, kClass, L"", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), job);
}

static void TocCalibVerifyJobFreeFooter(TocCalibVerifyJob* job) {
    if (!job || !job->footerIdx) {
        return;
    }
    TocCalibPrintedIndexFree(job->footerIdx);
    job->footerIdx = nullptr;
}

static void TocCalibVerifySlice(TocCalibVerifyJob* job) {
    DWORD t0 = ::GetTickCount();
    TocCalibSession* s = job->s;
    if (s && !job->aborted && job->footerScanning) {
        if (!job->footerIdx) {
            job->footerIdx = new TocCalibPrintedIndex;
            job->footerPage = 1;
        }
        bool done = TocCalibPrintedIndexScanChunk(s->engine, job->footerIdx, &job->footerPage, s->nPages, 40);
        if (!done) {
            if (job->onProgress) {
                int shown = job->footerPage > 0 ? job->footerPage - 1 : 0;
                job->onProgress(shown, s->nPages, job->ctx);
            }
            TocCalibVerifyScheduleNext(job);
            return;
        }
        TocCalibApplyPrintedIndex(s, job->footerIdx);
        if (s->printedIdx) {
            TocCalibPrintedIndexFree(s->printedIdx);
        }
        s->printedIdx = job->footerIdx;
        job->footerIdx = nullptr;
        job->footerScanning = false;
        if (job->onProgress) {
            job->onProgress(0, job->total, job->ctx);
        }
    }
    while (s && !job->aborted && job->row < job->total) {
        if (job->bm25Building) {
            // continue the incremental full-document index for this row
            DWORD tBm = ::GetTickCount();
            bool done = TocCalibBm25BuildChunk(s, job->pages, job->cache, &job->bm25, &job->bm25Page, 30);
            job->bm25BuildMs += ::GetTickCount() - tBm;
            if (!done) {
                if (job->onProgress) {
                    job->onProgress(job->row, job->total, job->ctx);
                }
                TocCalibVerifyScheduleNext(job);
                return;
            }
            job->bm25Building = false;
            job->bm25Ready = true;
            TocCalibVerifyRowFull(s, job->row, job->pages, job->cache, &job->bm25);
            job->nFull++;
            job->row++;
            // One full-doc row per slice — Find/OCR can otherwise freeze the UI
            // for minutes before onProgress runs (stuck at 0/N).
            break;
        } else {
            TocCalibRowVerifyPhase ph = TocCalibVerifyRowNear(s, job->row, job->pages, job->cache);
            if (ph == TocCalibRowVerifyPhase::NeedFull) {
                job->nNeedFull++;
                if (s->nPages > 800) {
                    // no full-document pass for huge books; just demote
                    TocCalibVerifyRowFull(s, job->row, job->pages, job->cache, &job->bm25);
                    job->nFull++;
                    job->row++;
                    break;
                } else if (!job->bm25Ready && (job->bm25Building || job->bm25.nDocs == 0)) {
                    // start (or continue) the incremental full-document index;
                    // it is document-wide and row-independent, so once the
                    // build completes it is reused by every later NeedFull row
                    // instead of being rebuilt from scratch each time.
                    if (!job->bm25Building) {
                        job->bm25Page = 1;
                        job->bm25Building = true;
                    }
                    continue;
                } else {
                    TocCalibVerifyRowFull(s, job->row, job->pages, job->cache, &job->bm25);
                    job->nFull++;
                    job->row++;
                    break;
                }
            } else {
                if (ph == TocCalibRowVerifyPhase::Skip) {
                    job->nSkip++;
                } else {
                    job->nNear++;
                }
                job->row++;
            }
        }
        if (::GetTickCount() - t0 > 40) {
            break;
        }
    }
    if (!s || job->aborted) {
        // session went away (tab closed / replaced); abandon silently
        if (job->onDone) {
            job->onDone(false, job->ctx);
        }
        if (job->timerWnd) {
            DestroyWindow(job->timerWnd);
        }
        TocCalibBm25Free(&job->bm25);
        TocCalibFreePageCache(job->cache);
        TocCalibVerifyJobFreeFooter(job);
        delete job;
        return;
    }
    if (job->onProgress) {
        job->onProgress(job->row, job->total, job->ctx);
    }
    if (job->row < job->total) {
        TocCalibVerifyScheduleNext(job);
        return;
    }
    // Verification complete: solve + mark, commit the working outline and
    // open the calibration bar (mirrors the tail of StartTocCalib).
    job->s->verifyJob = nullptr;
    MainWindow* win = job->win;
    int nCached = 0;
    for (Vec<EngineMupdfPageLine>* v : job->cache) {
        if (v) {
            nCached++;
        }
    }
    logf(
        "TocCalib verify stats rows=%d skip=%d near=%d needFull=%d full=%d bm25Build=%ums pagesCached=%d/%d "
        "elapsed=%ums\n",
        job->total, job->nSkip, job->nNear, job->nNeedFull, job->nFull, job->bm25BuildMs, nCached, job->cache.Size(),
        ::GetTickCount() - job->tStart);
    DWORD tCommit = ::GetTickCount();
    TocCalibSolveSession(s);
    TocCalibMarkConfirm(s);
    TocCalibWriteDebugIfCli(s);
    DWORD tSolved = ::GetTickCount();
    TocCalibWriteFlowDebug(win, s, "session before outline rewrite");
    char* err = nullptr;
    bool ok = EngineMupdfReplacePdfToc(s->engine, s->roots, &err);
    str::Free(err);
    DWORD tReplaced = ::GetTickCount();
    logf("TocCalib async commit rows=%d solveMark=%ums replaceToc=%ums total=%ums\n", job->total, tSolved - tCommit,
         tReplaced - tSolved, tReplaced - job->tStart);
    // Release pages loaded during BM25/near probe so paint has GDI/memory again.
    DisplayModel* dmRefresh = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    int keepPage = dmRefresh ? dmRefresh->CurrentPageNo() : 1;
    if (s->engine) {
        EngineMupdfTrimPageCaches(s->engine, keepPage, 4);
    }
    if (dmRefresh && gRenderCache) {
        gRenderCache->CancelRendering(dmRefresh);
        gRenderCache->FreeForDisplayModel(dmRefresh);
        dmRefresh->RenderVisibleParts();
    }
    if (win && win->hwndCanvas) {
        InvalidateRect(win->hwndCanvas, nullptr, TRUE);
    }
    if (ok) {
        if (win->tocLoaded) {
            ClearTocBox(win);
        }
        LoadTocTree(win);
        TocCalibWriteFlowDebug(win, s, "engine tree after outline rewrite");
    } else {
        WindowTab* tab = win->CurrentTab();
        if (tab && tab->tocCalib == s) {
            tab->tocCalib = nullptr;
            DeleteTocCalibSession(s);
        }
    }
    DWORD tLoaded = ::GetTickCount();
    if (ok) {
        TocCalibBindToTree(win);
        TocCalibWriteFlowDebug(win, s, "session after tree rebind");
        ShowTocCalib(win);
    }
    DWORD tShown = ::GetTickCount();
    logf("TocCalib async done ok=%d loadTree=%ums bind+show=%ums\n", (int)ok, tLoaded - tReplaced, tShown - tLoaded);
    if (job->onDone) {
        job->onDone(ok, job->ctx);
    }
    if (job->timerWnd) {
        DestroyWindow(job->timerWnd);
    }
    TocCalibBm25Free(&job->bm25);
    TocCalibFreePageCache(job->cache);
    TocCalibVerifyJobFreeFooter(job);
    delete job;
}

bool StartTocCalibAsync(MainWindow* win, Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                        TocCalibVerifyProgressFn onProgress, TocCalibVerifyDoneFn onDone, void* ctx) {
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
    DWORD tSession = ::GetTickCount();
    tab->tocCalib = TocCalibSessionFromExtracted(roots, engine, persistToDisk, true, /*deferVerify*/ true);
    logf("TocCalib async session rows=%d build=%ums\n", tab->tocCalib ? tab->tocCalib->rows.Size() : 0,
         ::GetTickCount() - tSession);
    if (tab->tocCalib) {
        // source mix: PdfLink rows are pinned and skip verification entirely,
        // so this line explains a slow verify at a glance
        int nPinned = 0, nBody = 0, nEst = 0, nUnknown = 0;
        for (int i = 0; i < tab->tocCalib->rows.Size(); i++) {
            ExtractedTocItem* it = tab->tocCalib->rows[i].item;
            switch (it ? it->destinationSource : TocDestinationSource::Unknown) {
                case TocDestinationSource::PdfLink:
                    nPinned++;
                    break;
                case TocDestinationSource::BodyMatch:
                    nBody++;
                    break;
                case TocDestinationSource::Estimated:
                    nEst++;
                    break;
                default:
                    nUnknown++;
                    break;
            }
        }
        logf("TocCalib async sources: pdfLink=%d bodyMatch=%d estimated=%d unknown=%d\n", nPinned, nBody, nEst,
             nUnknown);
    }
    if (!tab->tocCalib || tab->tocCalib->rows.Size() < 1) {
        DeleteTocCalibSession(tab->tocCalib);
        tab->tocCalib = nullptr;
        return false;
    }
    TocCalibSession* s = tab->tocCalib;
    TocTree* cur = tab->ctrl ? tab->ctrl->GetToc() : nullptr;
    TocCalibCloneOutline(cur, s->backup);
    // Begin the TOC transaction: remember the engine's dirty flag so Cancel
    // can restore it (same as StartTocCalib).
    s->baselineModifiedToc = EngineMupdfIsPdfTocModified(engine);
    auto* job = new TocCalibVerifyJob;
    job->win = win;
    job->s = s;
    job->total = s->rows.Size();
    job->tStart = ::GetTickCount();
    job->onProgress = onProgress;
    job->onDone = onDone;
    job->ctx = ctx;
    job->timerWnd = TocCalibVerifyTimerWindow(job);
    job->footerScanning = s->engine && s->nPages > 0;
    s->verifyJob = job;
    if (onProgress) {
        onProgress(0, job->total, ctx);
    }
    // Calibration rewrites the rows; showing the stale tree meanwhile looks
    // broken (rows half-bound, stale page fields). Hide it until the verify
    // completes, when LoadTocTree + ShowTocCalib present the finished result.
    ClearTocBox(win);
    TocCalibVerifyScheduleNext(job);
    return true;
}

bool StartTocCalib(MainWindow* win, Vec<ExtractedTocItem*>& roots, EngineBase* engine, bool persistToDisk,
                   bool scanBody) {
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
    tab->tocCalib = TocCalibSessionFromExtracted(roots, engine, persistToDisk, scanBody);
    if (!tab->tocCalib || tab->tocCalib->rows.Size() < 1) {
        DeleteTocCalibSession(tab->tocCalib);
        tab->tocCalib = nullptr;
        return false;
    }
    TocCalibWriteFlowDebug(win, tab->tocCalib, "session before outline rewrite");
    TocTree* cur = tab->ctrl ? tab->ctrl->GetToc() : nullptr;
    TocCalibCloneOutline(cur, tab->tocCalib->backup);
    // Begin the TOC transaction: remember the engine's dirty flag so Cancel
    // can restore it. The extracted outline below is committed to the PDF
    // model only as a working preview (Save/Cancel decides its fate).
    tab->tocCalib->baselineModifiedToc = EngineMupdfIsPdfTocModified(engine);
    // Opening an existing outline only binds the calib bar. Rewriting the PDF
    // outline and rebuilding the tree is for extracted TOCs (scanBody).
    if (scanBody) {
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
        TocCalibWriteFlowDebug(win, tab->tocCalib, "engine tree after outline rewrite");
    }
    TocCalibBindToTree(win);
    TocCalibWriteFlowDebug(win, tab->tocCalib, "session after tree rebind");
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
    if (!engine || !EngineMupdfCanEditToc(engine)) {
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
    return StartTocCalib(win, roots, engine, true, false);
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
