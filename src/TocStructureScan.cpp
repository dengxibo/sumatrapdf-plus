/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Body-structure TOC scan. See TocStructureScan.h for the design contract:
// local code owns the facts (original text, PDF page, bbox), the web AI only
// selects candidates and assigns levels.

#include "utils/BaseUtil.h"
#include "utils/Log.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "OcrService.h"
#include "EngineBase.h"
#include "ExtractPdfToc.h"
#include "TocExtraction.h"
#include "utils/JsonParser.h"
#include "TocStructureScan.h"

#include <algorithm>
#include <cmath>

// --- tuning constants -------------------------------------------------------

// Pages whose text layer yields fewer non-space glyphs are treated as scanned
// pages and OCRd. Mirrors the few-native threshold in CollectPageScanLines.
static constexpr int kMinNativeGlyphs = 20;
// Recall cap for a single AI round. When exceeded, the lowest-scoring
// candidates are dropped (never a page-range tail cut, so every page keeps
// its chance to contribute headings).
static constexpr int kMaxCandidates = 2000;
static constexpr int kMaxHeadingChars = 80;
static constexpr int kMaxShortChars = 34;

// --- small utf-8 helpers ----------------------------------------------------

static bool CpIsSpace(int cp) {
    return cp > 0 && cp <= 32 || cp == 0x3000; // includes ideographic space
}

static bool CpIsHanNumeral(int cp) {
    switch (cp) {
        case 0x3007: // 〇
        case 0x96F6: // 零
        case 0x4E00: // 一
        case 0x4E24: // 两
        case 0x4E8C: // 二
        case 0x4E09: // 三
        case 0x56DB: // 四
        case 0x4E94: // 五
        case 0x516D: // 六
        case 0x4E03: // 七
        case 0x516B: // 八
        case 0x4E5D: // 九
        case 0x5341: // 十
        case 0x767E: // 百
        case 0x5343: // 千
        case 0x4E07: // 万
            return true;
    }
    return false;
}

// Returns a newly allocated copy with leading/trailing spaces removed.
static char* DupTrimmed(const char* s) {
    if (!s) {
        return str::Dup("");
    }
    int len = (int)str::Len(s);
    int b = 0;
    while (b < len) {
        int next = b;
        int cp = Utf8CodepointNext(s, len, next);
        if (cp <= 0 || !CpIsSpace(cp)) {
            break;
        }
        b = next;
    }
    // Walk the rest codepoint by codepoint, tracking the end of the last
    // non-space run.
    int i = b;
    int contentEnd = b;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (!CpIsSpace(cp)) {
            contentEnd = i;
        }
    }
    return str::Dup(s + b, (size_t)(contentEnd - b));
}

static int CountChars(const char* s) {
    int len = (int)str::Len(s);
    int n = 0;
    for (int i = 0; i < len;) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        n++;
    }
    return n;
}

static int CountNonSpaceGlyphs(const Vec<ScanLine>& lines) {
    int n = 0;
    for (int i = 0; i < lines.Size(); i++) {
        const char* s = lines[i].text;
        if (!s) {
            continue;
        }
        int len = (int)str::Len(s);
        for (int b = 0; b < len;) {
            int cp = Utf8CodepointNext(s, len, b);
            if (cp <= 0) {
                break;
            }
            if (!CpIsSpace(cp)) {
                n++;
            }
        }
    }
    return n;
}

// --- heading numbering detection (recall-oriented local prior) --------------

// Skips spaces at s[i] and returns the number of bytes skipped.
static int SkipSpaces(const char* s, int len, int& i) {
    int start = i;
    while (i < len) {
        int next = i;
        int cp = Utf8CodepointNext(s, len, next);
        if (cp <= 0 || !CpIsSpace(cp)) {
            break;
        }
        i = next;
    }
    return i - start;
}

static int ConsumeHanNumeralRun(const char* s, int len, int& i) {
    int start = i;
    int n = 0;
    while (i < len && n < 8) {
        int next = i;
        int cp = Utf8CodepointNext(s, len, next);
        if (cp <= 0 || !CpIsHanNumeral(cp)) {
            break;
        }
        i = next;
        n++;
    }
    return n;
}

static int ConsumeDigits(const char* s, int len, int& i) {
    int start = i;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    return i - start;
}

static bool StartsWithCp(const char* s, int len, int i, int cp) {
    int next = i;
    return Utf8CodepointNext(s, len, next) == cp && next <= len;
}

// Parses a heading numbering prefix. On match sets kind/depth and returns the
// number of prefix bytes consumed; returns 0 for plain body lines. Depth is a
// local prior only — the AI owns the final hierarchy.
static int DetectNumbering(const char* s, int len, BodyNumberingKind& kind, int& depth) {
    kind = BodyNumberingKind::None;
    depth = 0;
    int i = 0;
    SkipSpaces(s, len, i);

    // 第X章 / 第X编 / 第X部分 / 第X节 (X = han or arabic numerals)
    if (StartsWithCp(s, len, i, 0x7B2C)) { // 第
        int next = i;
        Utf8CodepointNext(s, len, next);
        int nHan = ConsumeHanNumeralRun(s, len, next);
        int nDig = 0;
        if (nHan == 0) {
            nDig = ConsumeDigits(s, len, next);
        }
        if (nHan > 0 || nDig > 0) {
            int tail = next;
            int cpTail = Utf8CodepointNext(s, len, tail);
            if (cpTail == 0x7AE0) { // 章
                kind = BodyNumberingKind::Chapter;
                depth = 1;
                return tail;
            }
            if (cpTail == 0x7F16) { // 编
                kind = BodyNumberingKind::Chapter;
                depth = 1;
                return tail;
            }
            if (cpTail == 0x8282 || cpTail == 0x7BC7) { // 节 / 節
                kind = BodyNumberingKind::Section;
                depth = 2;
                return tail;
            }
            // 部分: 部 (U+90E8) followed by 分 (U+5206)
            if (cpTail == 0x90E8) {
                int afterBu = tail;
                int cpFen = Utf8CodepointNext(s, len, afterBu);
                if (cpFen == 0x5206) {
                    kind = BodyNumberingKind::Chapter;
                    depth = 1;
                    return afterBu;
                }
            }
        }
    }

    // （一） (一) （1） (1), spaces inside tolerated ("（ 一）")
    {
        int afterOpen = i;
        int cpOpen = Utf8CodepointNext(s, len, afterOpen);
        if (cpOpen == 0xFF08 || cpOpen == '(') { // （ / (
            int p = afterOpen;
            SkipSpaces(s, len, p);
            int nHan = ConsumeHanNumeralRun(s, len, p);
            int nDig = 0;
            if (nHan == 0) {
                nDig = ConsumeDigits(s, len, p);
            }
            if (nHan > 0 || nDig > 0) {
                SkipSpaces(s, len, p);
                int afterClose = p;
                int cpClose = Utf8CodepointNext(s, len, afterClose);
                if (cpClose == 0xFF09 || cpClose == ')') { // ） / )
                    kind = BodyNumberingKind::Paren;
                    depth = nHan > 0 ? 2 : 3;
                    return afterClose;
                }
            }
        }
    }

    // 1.1 / 1.1.1 (dotted decimal; depth = number of groups)
    {
        int p = i;
        int nDig = ConsumeDigits(s, len, p);
        if (nDig > 0) {
            int groups = 1;
            int q = p;
            bool dotted = false;
            while (q < len && s[q] == '.') {
                int r = q + 1;
                if (ConsumeDigits(s, len, r) == 0) {
                    break;
                }
                q = r;
                groups++;
                dotted = true;
            }
            if (dotted) {
                kind = BodyNumberingKind::Dotted;
                depth = groups;
                return q;
            }
        }
    }

    // 1. / 1、 / 1． but not decimals like "1.5": the marker char must be
    // followed by a space, a han ideograph or a full-width punctuation.
    {
        int p = i;
        int nDig = ConsumeDigits(s, len, p);
        if (nDig > 0 && nDig <= 3 && p < len) {
            int afterMark = p;
            int cpMark = Utf8CodepointNext(s, len, afterMark);
            if (cpMark == '.' || cpMark == 0xFF0E || cpMark == 0x3001) { // . ． 、
                int afterSep = afterMark;
                int cpNext = Utf8CodepointNext(s, len, afterSep);
                bool followsTitle = CpIsSpace(cpNext) || cpNext >= 0x2E80 || cpNext == 0x3001 ||
                                    cpNext == 0x201C || cpNext == 0x300C;
                if (followsTitle) {
                    kind = BodyNumberingKind::Arabic;
                    depth = 3;
                    return afterMark;
                }
            }
        }
    }

    // 一、 (han numerals + ideographic comma): top-level official-doc heading
    {
        int p = i;
        int nHan = ConsumeHanNumeralRun(s, len, p);
        if (nHan > 0 && nHan <= 6 && p < len) {
            int afterDun = p;
            int cpDun = Utf8CodepointNext(s, len, afterDun);
            if (cpDun == 0x3001) { // 、
                kind = BodyNumberingKind::Dunhao;
                depth = 1;
                return afterDun;
            }
        }
    }

    return 0;
}

// --- candidate scoring ------------------------------------------------------

static bool CpIsSentenceEnd(int cp) {
    switch (cp) {
        case 0x3002: // 。
        case 0xFF01: // ！
        case 0xFF1F: // ？
        case 0xFF1B: // ；
        case '.':
        case '!':
        case '?':
        case ';':
            return true;
    }
    return false;
}

static int LastCp(const char* s, int len) {
    int cp = 0;
    for (int i = 0; i < len;) {
        cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
    }
    return cp;
}

// Figure/table captions ("图1 …", "表 2-1 …", "Fig. 3") are common short
// lines but never document headings.
static bool LooksLikeFigureCaption(const char* s, int len) {
    int i = 0;
    int cp0 = Utf8CodepointNext(s, len, i);
    if (cp0 == 0x56FE || cp0 == 0x8868) { // 图 / 表
        int j = i;
        SkipSpaces(s, len, j);
        if (j < len && (s[j] >= '0' && s[j] <= '9')) {
            return true;
        }
    }
    if (str::StartsWith(s, "Fig.") || str::StartsWith(s, "FIG.") || str::StartsWith(s, "Figure")) {
        return true;
    }
    return false;
}

// Dominant font size among substantive lines on the page = body text size.
static float PageBodyFontSize(const Vec<ScanLine>& lines) {
    static constexpr int kBuckets = 64;
    int counts[kBuckets]{};
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.fontSize <= 0.f) {
            continue;
        }
        if (CountChars(sl.text) < 8) {
            continue;
        }
        int bucket = (int)(sl.fontSize * 2.f + 0.5f);
        if (bucket >= 0 && bucket < kBuckets) {
            counts[bucket]++;
        }
    }
    int best = 0;
    int bestBucket = 0;
    for (int b = 0; b < kBuckets; b++) {
        if (counts[b] > best) {
            best = counts[b];
            bestBucket = b;
        }
    }
    return bestBucket > 0 ? bestBucket / 2.f : 0.f;
}

static bool LineAtTop(const ScanLine& sl, float pageH) {
    return pageH > 0.f && sl.y < pageH * 0.10f;
}

static bool LineCentered(const ScanLine& sl, float pageW) {
    if (pageW <= 0.f) {
        return false;
    }
    float center = sl.x + sl.dx / 2.f;
    return fabsf(center - pageW / 2.f) < pageW * 0.12f;
}

// --- repeated header/footer removal -----------------------------------------

struct HeaderNorm {
    char* norm = nullptr;
    int distinctPages = 0;
    int lastPage = 0;
    int edgeHits = 0; // seen in top/bottom band
};

// Strips digits, han page numerals, dot leaders and punctuation so a running
// header with a changing page number still normalizes to one key.
static char* NormalizeHeaderKey(const char* s) {
    int len = (int)str::Len(s);
    StrBuilder out;
    for (int i = 0; i < len;) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (cp >= '0' && cp <= '9') {
            continue;
        }
        if (cp >= 0xFF10 && cp <= 0xFF19) { // full-width digits
            continue;
        }
        if (CpIsHanNumeral(cp) || CpIsSpace(cp)) {
            continue;
        }
        switch (cp) {
            case '.':
            case 0xFF0E: // ．
            case 0x3002: // 。
            case 0x2026: // …
            case 0x00B7: // ·
            case '_':
            case 0x2014: // —
            case 0x2013: // –
            case '-':
            case ':':
            case 0xFF1A: // ：
            case '(':
            case ')':
            case 0xFF08:
            case 0xFF09:
            case '/':
            case 0xFF0F:
                continue;
        }
        char buf[8]{};
        char* dst = buf;
        str::Utf8Encode(dst, cp);
        int n = (int)(dst - buf);
        if (n > 0) {
            out.Append(buf, (size_t)n);
        }
    }
    return out.StealData();
}

static bool HeaderKeyIsRepeated(const Vec<HeaderNorm>& norms, const char* key) {
    for (int i = 0; i < norms.Size(); i++) {
        if (str::Eq(norms[i].norm, key)) {
            return true;
        }
    }
    return false;
}

// --- per-page scan ----------------------------------------------------------

struct PendingCandidate {
    HeadingCandidate* cand = nullptr;
    char* headerKey = nullptr;
    bool edge = false;
};

static void FreePending(PendingCandidate& pc) {
    delete pc.cand;
    str::Free(pc.headerKey);
}

static void CollectHeaderNorms(const Vec<ScanLine>& lines, float pageH, int pageNo, Vec<HeaderNorm>& norms) {
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (!sl.text) {
            continue;
        }
        AutoFreeStr key(NormalizeHeaderKey(sl.text));
        if (!key || CountChars(key) < 4) {
            continue;
        }
        bool edge = pageH > 0.f && (sl.y < pageH * 0.08f || sl.y > pageH * 0.92f);
        HeaderNorm* found = nullptr;
        for (int j = 0; j < norms.Size(); j++) {
            if (str::Eq(norms[j].norm, key)) {
                found = &norms[j];
                break;
            }
        }
        if (!found) {
            HeaderNorm hn;
            hn.norm = str::Dup(key);
            hn.lastPage = pageNo;
            hn.distinctPages = 1;
            hn.edgeHits = edge ? 1 : 0;
            norms.Append(hn);
        } else if (found->lastPage != pageNo) {
            found->lastPage = pageNo;
            found->distinctPages++;
            if (edge) {
                found->edgeHits++;
            }
        }
    }
}

static void ExtractPageCandidates(const Vec<ScanLine>& lines, int pageNo, float pageW, float pageH, bool fromOcr,
                                  Vec<PendingCandidate>& out) {
    float bodyFont = PageBodyFontSize(lines);

    // Sort line pointers top-to-bottom for whitespace signals; the text layer
    // is usually already ordered, OCR lines occasionally are not.
    Vec<const ScanLine*> ordered;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].text) {
            ordered.Append(&lines[i]);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const ScanLine* a, const ScanLine* b) {
        if (fabsf(a->y - b->y) > 1.f) {
            return a->y < b->y;
        }
        return a->x < b->x;
    });

    float leading = bodyFont > 0.f ? bodyFont * 1.6f : 16.f;

    for (int i = 0; i < ordered.Size(); i++) {
        const ScanLine& sl = *ordered[i];
        AutoFreeStr trimmed(DupTrimmed(sl.text));
        if (!trimmed) {
            continue;
        }
        int tlen = (int)str::Len(trimmed);
        int chars = CountChars(trimmed);
        if (chars < 2 || chars > kMaxHeadingChars) {
            continue;
        }
        if (LooksLikeFigureCaption(trimmed, tlen)) {
            continue;
        }

        BodyNumberingKind numKind;
        int numDepth = 0;
        bool numbered = DetectNumbering(trimmed, tlen, numKind, numDepth) > 0;
        bool sentenceEnd = CpIsSentenceEnd(LastCp(trimmed, tlen));

        float prevBottom = 0.f;
        float nextTop = 0.f;
        if (i > 0) {
            prevBottom = ordered[i - 1]->y + ordered[i - 1]->dy;
        }
        if (i + 1 < ordered.Size()) {
            nextTop = ordered[i + 1]->y;
        }
        bool gapAbove = i > 0 && (sl.y - prevBottom) > leading * 0.9f;
        bool gapBelow = i + 1 < ordered.Size() && (nextTop - (sl.y + sl.dy)) > leading * 0.9f;
        bool bigFont = bodyFont > 0.f && sl.fontSize >= bodyFont + 1.5f;
        bool centered = LineCentered(sl, pageW);
        bool atTop = LineAtTop(sl, pageH);

        int score = 0;
        if (numbered) {
            // Numbering is the strongest single signal. Long sentence-style
            // numbered runs are body enumerations; keep them (recall) but low.
            score = 45;
            if (sl.bold) {
                score += 10;
            }
            if (bigFont) {
                score += 10;
            }
            if (centered) {
                score += 5;
            }
            if (chars > 45 && sentenceEnd) {
                score -= 30;
            }
        } else {
            if (chars > kMaxShortChars) {
                continue;
            }
            if (sentenceEnd && chars > 24) {
                continue;
            }
            int signals = 0;
            if (sl.bold) {
                signals += 20;
            }
            if (bigFont) {
                signals += 25;
            }
            if (centered) {
                signals += 15;
            }
            if (atTop) {
                signals += 8;
            }
            if (gapAbove) {
                signals += 10;
            }
            if (gapBelow) {
                signals += 10;
            }
            // Need at least one typographic signal or two layout signals; a
            // bare short body line is noise.
            if (signals < 25) {
                continue;
            }
            score = 10 + signals;
        }
        if (score < 10) {
            continue;
        }
        if (score > 100) {
            score = 100;
        }

        auto* cand = new HeadingCandidate();
        cand->pdfPage = pageNo;
        cand->x = sl.x;
        cand->y = sl.y;
        cand->dx = sl.dx;
        cand->dy = sl.dy;
        cand->fontSize = sl.fontSize;
        cand->bold = sl.bold;
        cand->fromOcr = fromOcr;
        cand->numKind = numKind;
        cand->numDepth = numDepth;
        cand->localScore = score;
        cand->text = str::Dup(trimmed);

        PendingCandidate pc;
        pc.cand = cand;
        pc.headerKey = NormalizeHeaderKey(trimmed);
        pc.edge = pageH > 0.f && (sl.y < pageH * 0.08f || sl.y > pageH * 0.92f);
        out.Append(pc);
    }
}

// --- public API -------------------------------------------------------------

void TocStructureScanResult::Reset() {
    for (int i = 0; i < candidates.Size(); i++) {
        delete candidates[i];
    }
    candidates.Reset();
    totalPages = 0;
    nativePages = 0;
    ocrPages = 0;
    rawLineCount = 0;
    repeatedHeaderCandidates = 0;
    engine = nullptr;
}

HeadingCandidate* TocStructureScanResult::FindById(int id) const {
    if (id < 1 || id > candidates.Size()) {
        return nullptr;
    }
    return candidates[id - 1];
}

bool RunTocStructureScan(EngineBase* engine, TocStructureScanResult& out, bool (*isCanceled)(void*), void* cancelCtx,
                         void (*onProgress)(void*, int, int), void* progressCtx) {
    out.Reset();
    if (!engine) {
        return false;
    }
    out.engine = engine;
    int nPages = engine->PageCount();
    out.totalPages = nPages;

    Vec<PendingCandidate> pending;
    Vec<HeaderNorm> norms;
    defer {
        for (int i = 0; i < norms.Size(); i++) {
            str::Free(norms[i].norm);
        }
    };

    for (int pageNo = 1; pageNo <= nPages; pageNo++) {
        if (isCanceled && isCanceled(cancelCtx)) {
            for (int i = 0; i < pending.Size(); i++) {
                FreePending(pending[i]);
            }
            out.Reset();
            return false;
        }
        if (onProgress) {
            onProgress(progressCtx, pageNo, nPages);
        }

        Vec<ScanLine> lines;
        PtocCollectPageScanLines(engine, pageNo, lines);
        bool fromOcr = false;
        if (CountNonSpaceGlyphs(lines) < kMinNativeGlyphs) {
            int before = lines.Size();
            if (PtocOcrAndCollectPageScanLines(engine, pageNo, lines) && lines.Size() > before) {
                fromOcr = true;
            }
        }
        if (fromOcr) {
            out.ocrPages++;
        } else {
            out.nativePages++;
        }
        out.rawLineCount += lines.Size();

        RectF box = engine->PageMediabox(pageNo);
        CollectHeaderNorms(lines, box.dy, pageNo, norms);
        ExtractPageCandidates(lines, pageNo, box.dx, box.dy, fromOcr, pending);
        PtocFreeScanLines(lines);
    }

    // Repeated headers/footers: same normalized text on at least 25% of the
    // pages (min 3), mostly hugging the top/bottom band.
    int minPages = nPages / 4;
    if (minPages < 3) {
        minPages = 3;
    }
    Vec<HeaderNorm> repeated;
    for (int i = 0; i < norms.Size(); i++) {
        const HeaderNorm& hn = norms[i];
        if (hn.distinctPages >= minPages && hn.edgeHits * 2 >= hn.distinctPages) {
            repeated.Append(hn);
        }
    }

    for (int i = 0; i < pending.Size(); i++) {
        PendingCandidate& pc = pending[i];
        bool isHeader = pc.edge && pc.headerKey && HeaderKeyIsRepeated(repeated, pc.headerKey);
        if (isHeader) {
            out.repeatedHeaderCandidates++;
            FreePending(pc);
            continue;
        }
        str::Free(pc.headerKey);
        pc.headerKey = nullptr;
        out.candidates.Append(pc.cand);
        pc.cand = nullptr;
    }

    // Recall cap: drop the weakest candidates globally, but restore reading
    // (page, y) order afterwards so the digest stays document-ordered.
    if (out.candidates.Size() > kMaxCandidates) {
        Vec<HeadingCandidate*> sorted = out.candidates;
        std::stable_sort(sorted.begin(), sorted.end(), [](HeadingCandidate* a, HeadingCandidate* b) {
            return a->localScore > b->localScore;
        });
        while (sorted.Size() > kMaxCandidates) {
            delete sorted.Last();
            sorted.RemoveAt(sorted.Size() - 1);
        }
        std::stable_sort(sorted.begin(), sorted.end(), [](HeadingCandidate* a, HeadingCandidate* b) {
            if (a->pdfPage != b->pdfPage) {
                return a->pdfPage < b->pdfPage;
            }
            if (fabsf(a->y - b->y) > 1.f) {
                return a->y < b->y;
            }
            return a->x < b->x;
        });
        out.candidates = sorted;
    }

    for (int i = 0; i < out.candidates.Size(); i++) {
        out.candidates[i]->id = i + 1;
    }
    logf("BodyTOC scan: pages=%d native=%d ocr=%d lines=%d candidates=%d headers=%d\n", nPages, out.nativePages,
         out.ocrPages, out.rawLineCount, out.candidates.Size(), out.repeatedHeaderCandidates);
    return true;
}

// --- digest -----------------------------------------------------------------

static void AppendXmlEscaped(StrBuilder& out, const char* s) {
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '&') {
            out.Append("&amp;");
        } else if (c == '<') {
            out.Append("&lt;");
        } else if (c == '>') {
            out.Append("&gt;");
        } else {
            out.Append(&c, 1);
        }
    }
}

char* BuildBodyTocDigest(const TocStructureScanResult& scan) {
    StrBuilder out;
    for (int i = 0; i < scan.candidates.Size(); i++) {
        HeadingCandidate* c = scan.candidates[i];
        out.AppendFmt("<CANDIDATE id=\"C%d\">\n", c->id);
        out.AppendFmt("pdf_page: %d\n", c->pdfPage);
        out.Append("text: ");
        AppendXmlEscaped(out, c->text);
        out.Append("\n</CANDIDATE>\n");
    }
    return out.StealData();
}

// --- v2 JSON parsing --------------------------------------------------------

bool IsBodyTocJsonCandidate(const char* text) {
    if (!text) {
        return false;
    }
    while (*text && *text <= ' ') {
        text++;
    }
    return *text == '{' && str::Find(text, "\"toc\"") && str::Find(text, "candidate_id");
}

struct RawBodySelection {
    char* idText = nullptr; // e.g. "C12"
    int level = 1;
    bool hasLevel = false;

    ~RawBodySelection() { str::Free(idText); }
};

struct BodyJsonVisitor : json::ValueVisitor {
    Vec<RawBodySelection*> sels;

    ~BodyJsonVisitor() {
        for (int i = 0; i < sels.Size(); i++) {
            delete sels[i];
        }
    }

    RawBodySelection& At(int idx) {
        while (sels.Size() <= idx) {
            sels.Append(new RawBodySelection());
        }
        return *sels[idx];
    }

    bool Visit(const char* path, const char* value, json::Type type) override {
        if (!str::StartsWith(path, "/toc[")) {
            return true; // ignore suspected_gaps and anything else
        }
        const char* p = path + str::Len("/toc[");
        int idx = 0;
        bool hasIndex = false;
        while (*p >= '0' && *p <= '9') {
            if (idx > 100000) {
                return false;
            }
            idx = idx * 10 + (*p - '0');
            p++;
            hasIndex = true;
        }
        if (!hasIndex || idx > 100000 || *p++ != ']' || *p++ != '/') {
            return true;
        }
        RawBodySelection& sel = At(idx);
        if (str::Eq(p, "candidate_id") && type == json::Type::String) {
            str::Free(sel.idText);
            sel.idText = str::Dup(value);
        } else if (str::Eq(p, "level") && type == json::Type::Number) {
            sel.level = ParseInt(value);
            sel.hasLevel = true;
        }
        return true;
    }
};

static bool ParseCandidateId(const char* s, int& idOut) {
    if (!s || s[0] != 'C' || !(s[1] >= '0' && s[1] <= '9')) {
        return false;
    }
    int id = 0;
    for (const char* p = s + 1; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        id = id * 10 + (*p - '0');
        if (id > 1000000) {
            return false;
        }
    }
    idOut = id;
    return id > 0;
}

bool ParseBodyTocSelections(const char* text, const TocStructureScanResult& scan, Vec<BodyTocSelection>& out) {
    if (!text) {
        return false;
    }
    // Tolerate markdown fences and surrounding prose.
    const char* start = strchr(text, '{');
    const char* lastBrace = strrchr(text, '}');
    if (!start || !lastBrace || lastBrace < start) {
        return false;
    }
    char* body = str::Dup(start, (size_t)(lastBrace - start + 1));
    defer {
        str::Free(body);
    };
    BodyJsonVisitor visitor;
    if (!json::Parse(body, &visitor)) {
        return false;
    }

    bool seen[kMaxCandidates] = {false};
    for (int i = 0; i < visitor.sels.Size(); i++) {
        RawBodySelection* raw = visitor.sels[i];
        if (!raw->idText) {
            continue;
        }
        int id = 0;
        if (!ParseCandidateId(raw->idText, id)) {
            continue;
        }
        if (!scan.FindById(id)) {
            continue; // whitelist: unknown ids are rejected, never guessed
        }
        if (id <= kMaxCandidates && seen[id - 1]) {
            continue;
        }
        if (id <= kMaxCandidates) {
            seen[id - 1] = true;
        }
        BodyTocSelection sel;
        sel.candidateId = id;
        sel.level = raw->hasLevel && raw->level >= 1 ? raw->level : 1;
        out.Append(sel);
    }
    return out.Size() > 0;
}

// --- bookmark tree construction ---------------------------------------------

bool BuildBodyTocItems(TocStructureScanResult& scan, const Vec<BodyTocSelection>& sel,
                       Vec<ExtractedTocItem*>& roots) {
    Vec<ExtractedTocItem*> stack;
    for (int i = 0; i < sel.Size(); i++) {
        HeadingCandidate* c = scan.FindById(sel[i].candidateId);
        if (!c) {
            continue;
        }
        int wantLevel = sel[i].level < 1 ? 1 : sel[i].level;
        if (wantLevel > stack.Size() + 1) {
            wantLevel = stack.Size() + 1;
        }
        while (stack.Size() >= wantLevel) {
            stack.RemoveAt(stack.Size() - 1);
        }
        auto* item = new ExtractedTocItem();
        item->title = str::Dup(c->text);
        item->rawTitle = str::Dup(c->text);
        item->pageNo = c->pdfPage;
        item->x = c->x;
        item->y = c->y;
        item->level = stack.Size() + 1;
        // GB/T 9704-2012: the second hierarchy ordinal uses full-width
        // Chinese parentheses "（一）". Source text commonly mixes half-width
        // "(一)"; unify only the level-2 ordinal wrapper. Other levels and the
        // rest of the title (English phrases etc.) stay verbatim, and
        // rawTitle keeps the original text for body anchoring.
        if (item->level == 2) {
            NormalizeTocNumberingParens(&item->title);
        }
        item->confidence = 60;
        item->source = ExtractedTocSource::BodyInference;
        item->destinationSource = TocDestinationSource::Unknown;
        item->expanded = true;
        if (stack.Size() > 0) {
            item->parent = stack.Last();
            item->parent->children.Append(item);
        } else {
            roots.Append(item);
        }
        stack.Append(item);
    }
    return roots.Size() > 0;
}
