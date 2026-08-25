/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/ThreadUtil.h"
#include "utils/UITask.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "TableOfContents.h"
#include "Notifications.h"
#include "Translations.h"
#include "GlobalPrefs.h"
#include "Toolbar.h"
#include "Flags.h"
#include "ExtractPdfToc.h"
#include "ExtractBookToc.h"
#include "TocCalib.h"
#include "OcrService.h"

#include "utils/Log.h"

// Tunable Auto TOC parameters. Adjust with real PDFs; keep documented.
struct ExtractPdfTocConfig {
    int tocSearchPct;      // front of book to search for printed TOC
    int tocSearchMinPages; // floor for that window
    int tocSearchMaxPages; // cap (also used as CONTENTS search bound)
    int tocPageScoreMin;   // heading word / dotted leaders / page-number column
    int tocMaxSpanPages;   // continue consecutive TOC pages at most this far
    int printedMinHits;    // give up printed route below this many entries
    int headingMaxGlyphs;  // reject body lines longer than this as headings
    int matchContainMinGlyphs;
    int lineYTolPct;       // same-line merge: |midY| <= this % of median line height
    int lineMaxGapEm;      // max x-gap (in line heights) to join fragments
    int lineNumTitleGapEm; // extra x-gap allowed between "1.1" and title
};

static const ExtractPdfTocConfig kExtractPdfToc = {15, 8, 60, 30, 40, 8, 80, 4, 55, 4, 12};

// Printed-TOC search window. Official uses kExtractPdfToc; book uses a local copy.
struct PrintedTocOpts {
    int searchPct = 0;
    int searchMinPages = 0;
    int searchMaxPages = 0;
    int pageScoreMin = 0;
    int maxSpanPages = 0;
    int minHits = 0;
    bool acceptTocHeadingPage = false;
    bool requireDestMatch = true;
    bool repairMissingSectionNumbers = true;
    bool bookHierarchy = false;
    bool acceptPartTitles = false;
};

static PrintedTocOpts OfficialPrintedTocOpts() {
    PrintedTocOpts o;
    o.searchPct = kExtractPdfToc.tocSearchPct;
    o.searchMinPages = kExtractPdfToc.tocSearchMinPages;
    o.searchMaxPages = kExtractPdfToc.tocSearchMaxPages;
    o.pageScoreMin = kExtractPdfToc.tocPageScoreMin;
    o.maxSpanPages = kExtractPdfToc.tocMaxSpanPages;
    o.minHits = kExtractPdfToc.printedMinHits;
    o.acceptTocHeadingPage = true;
    o.requireDestMatch = false;
    return o;
}

static PrintedTocOpts PaperPrintedTocOpts() {
    PrintedTocOpts o = OfficialPrintedTocOpts();
    o.acceptTocHeadingPage = true;
    o.repairMissingSectionNumbers = false;
    return o;
}

enum class ExtractTocDocClass {
    Official,
    Book,
    Paper,
    Contract
};
enum class TocProfileKind {
    Book,
    Paper,
    Contract
};

void DeleteExtractedTocItems(Vec<ExtractedTocItem*>& roots) {
    for (ExtractedTocItem* n : roots) {
        delete n;
    }
    roots.Reset();
}

ExtractedTocItem::~ExtractedTocItem() {
    str::Free(title);
    str::Free(rawTitle);
    str::Free(printedLabel);
    title = nullptr;
    rawTitle = nullptr;
    printedLabel = nullptr;
    DeleteExtractedTocItems(children);
}

void FlattenExtractedTocItems(const Vec<ExtractedTocItem*>& nodes, Vec<ExtractedTocItem*>& flat) {
    for (int i = 0; i < nodes.Size(); i++) {
        ExtractedTocItem* n = nodes[i];
        if (!n) {
            continue;
        }
        flat.Append(n);
        FlattenExtractedTocItems(n->children, flat);
    }
}

static void InheritMissingExtractedDests(Vec<ExtractedTocItem*>& roots) {
    Vec<ExtractedTocItem*> flat;
    FlattenExtractedTocItems(roots, flat);
    int prevPage = 0;
    float prevX = 0;
    float prevY = 0;
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* it = flat[i];
        if (!it) {
            continue;
        }
        if (it->pageNo < 1) {
            if (prevPage > 0) {
                it->pageNo = prevPage;
                it->x = prevX;
                it->y = prevY;
            }
            continue;
        }
        prevPage = it->pageNo;
        prevX = it->x;
        prevY = it->y;
    }
}

static bool ExtractedTitleIsContentsPage(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    return str::Eq(s, "目录") || str::Eq(s, "目次") || str::EqI(s, "Contents");
}

bool ExtractedHasPrintedBookCalib(const Vec<ExtractedTocItem*>& roots) {
    Vec<ExtractedTocItem*> flat;
    FlattenExtractedTocItems(roots, flat);
    int n = 0;
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* it = flat[i];
        if (!it || it->printedPage < 1 || ExtractedTitleIsContentsPage(it->title)) {
            continue;
        }
        if (it->source == ExtractedTocSource::PrintedToc) {
            n++;
        }
    }
    return n >= 2;
}

void EngineMupdfFreePageLines(Vec<EngineMupdfPageLine>& lines) {
    for (int i = 0; i < lines.Size(); i++) {
        str::Free(lines[i].text);
        lines[i].text = nullptr;
    }
    lines.Reset();
}

static Kind kNotifExtractToc = "extractPdfToc";
static LONG gExtractCancelSeq = 0;
static LONG gExtractRunning = 0;

bool ExtractPdfTocIsRunning() {
    return InterlockedCompareExchange(&gExtractRunning, 0, 0) != 0;
}

void CancelExtractPdfToc() {
    InterlockedIncrement(&gExtractCancelSeq);
}

static bool ExtractCancelled(LONG seq) {
    return InterlockedCompareExchange(&gExtractCancelSeq, 0, 0) != seq;
}

static char* DupTrimmed(const char* s) {
    char* d = str::Dup(s ? s : "");
    if (d) {
        str::TrimWSInPlace(d, str::TrimOpt::Both);
    }
    return d;
}

static void TrimTitleToFirstSentence(char* s);
static void StripTrailingOcrTitleJunk(char* s);
static void TrimAtNextDiHeading(char* s);
static void TrimAtNextEmbeddedHeading(char* s);
static int FindNextEmbeddedHeading(const char* s);
static void NormalizeTocNumberingParens(char** titleOut);
static void NormalizeTocNumberingDots(char** titleOut);
static void StripNumberingTitleSpace(char** titleOut);

static int GlyphCount(const char* s) {
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
        if (cp > 32 && cp != 0x3000) {
            n++;
        }
    }
    return n;
}

static int GlyphCountRange(const char* s, int from, int to) {
    if (!s || from >= to) {
        return 0;
    }
    int len = (int)str::Len(s);
    if (to > len) {
        to = len;
    }
    if (from < 0) {
        from = 0;
    }
    int i = from;
    int n = 0;
    while (i < to) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (cp > 32 && cp != 0x3000) {
            n++;
        }
    }
    return n;
}

static void StripSpacesCopy(const char* s, char* dst, int dstCap) {
    int o = 0;
    if (!s) {
        dst[0] = 0;
        return;
    }
    for (; *s && o + 1 < dstCap; s++) {
        if (*s != ' ' && *s != '\t' && (u8)*s != 0xC2 /* skip later */) {
            if ((u8)*s == 0xE3 && (u8)s[1] == 0x80 && (u8)s[2] == 0x80) {
                s += 2;
                continue;
            }
            dst[o++] = *s;
        }
    }
    dst[o] = 0;
}

static bool LooksLikePrintedTocHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::ContainsI(s, "table of contents") || str::ContainsI(s, "contents")) {
        int g = GlyphCount(s);
        return g <= 40;
    }
    char compact[192];
    StripSpacesCopy(s, compact, (int)sizeof(compact));
    int g = GlyphCount(compact);
    if (g > 24) {
        return false;
    }
    // 目录, plus common OCR of 目 as 日/自.
    return str::Find(compact, "\xE7\x9B\xAE\xE5\xBD\x95") != nullptr || str::Find(compact, "日录") != nullptr ||
           str::Find(compact, "自录") != nullptr;
}

static bool LooksLikeEnglishTocHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::Find(s, "\xE7\x9B\xAE\xE5\xBD\x95") || str::Find(s, "\xE7\x9B\xAE\xE6\xAC\xA1")) {
        return false;
    }
    if (str::ContainsI(s, "table of contents")) {
        return GlyphCount(s) <= 40;
    }
    if (str::ContainsI(s, "contents")) {
        return GlyphCount(s) <= 24;
    }
    return false;
}

static bool IsListBulletCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x00B7 || cp == 0x2022 || cp == 0x30FB || cp == 0x2024 || cp == 0x2219 ||
           cp == 0xFF65;
}

static bool IsLeaderCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x00B7 || cp == 0x2026 || cp == 0x30FB || cp == 0x2500 || cp == 0x2014 ||
           cp == 0x2013 || cp == '_' || cp == '-' || cp == 0x3000 || cp == ' ' || cp == 0x2022 || cp == 0xFF0D ||
           cp == 0x00B7 || cp == 0x2024 || cp == 0x2219 || cp == 0x00A8 || cp == 0xFF65 || cp == 0x22EF ||
           cp == 0x2025 || cp == '/' || cp == 0xFF0F || cp == 0x2215 || cp == 0x2044;
}

static bool IsDigitCp(int cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19);
}

static int DigitValue(int cp) {
    if (cp >= '0' && cp <= '9') {
        return cp - '0';
    }
    if (cp >= 0xFF10 && cp <= 0xFF19) {
        return cp - 0xFF10;
    }
    return -1;
}

static bool IsCnDateUnitCp(int cp) {
    return cp == 0x5E74 || cp == 0x6708 || cp == 0x65E5; // 年月日
}

// i is the first character after a numbering dot. "4.6月30日" is a list "4." plus a date, not section 4.6.
static bool DottedFollowedByDate(const char* s, int len, int i) {
    if (!s || i >= len) {
        return false;
    }
    int look = Utf8CodepointNext(s, len, i);
    if (!IsDigitCp(look)) {
        return false;
    }
    while (i < len) {
        int save = i;
        int d = Utf8CodepointNext(s, len, i);
        if (!IsDigitCp(d)) {
            i = save;
            break;
        }
    }
    if (i >= len) {
        return false;
    }
    int unit = Utf8CodepointNext(s, len, i);
    return IsCnDateUnitCp(unit);
}

// Consecutive ASCII digits starting at i. Used to reject phone/ID runs after "2."
// (e.g. "2.12333电话…") so they stay simple ArabicDot rank 3, not dotted rank 2.
static int CountAsciiDigitsFrom(const char* s, int len, int i) {
    int n = 0;
    while (i < len) {
        int save = i;
        int d = Utf8CodepointNext(s, len, i);
        if (!IsDigitCp(d)) {
            i = save;
            break;
        }
        n++;
    }
    return n;
}

static bool HasLetterOrCjk(const char* s);
static bool LooksLikeUrlStart(const char* s);
static bool LooksLikeArchiveJunk(const char* s);
static bool LineLooksLikeRomanPage(const char* s);
static bool LineLooksLikePageNumber(const char* s);
static int ParseBarePrintedPage(const char* s);
static bool StartsWithXinDeHeading(const char* s);

static bool ParsePrintedTocLine(const char* raw, char** titleOut, int* pageOut) {
    *titleOut = nullptr;
    *pageOut = 0;
    char* s = DupTrimmed(raw);
    if (!s || !s[0]) {
        str::Free(s);
        return false;
    }
    if (LooksLikePrintedTocHeading(s)) {
        str::Free(s);
        return false;
    }
    int len = (int)str::Len(s);
    int page = 0;
    int nDigits = 0;
    int scan = len;
    int last = Utf8CodepointPrev(s, len, scan);
    while (last > 0 && (last == ' ' || last == 0x3000)) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    int digitBuf[8];
    while (last > 0 && IsDigitCp(last) && nDigits < 8) {
        digitBuf[nDigits++] = DigitValue(last);
        last = Utf8CodepointPrev(s, len, scan);
    }
    if (nDigits < 1) {
        str::Free(s);
        return false;
    }
    for (int i = nDigits - 1; i >= 0; i--) {
        page = page * 10 + digitBuf[i];
    }
    if (page < 1 || page > 9999) {
        str::Free(s);
        return false;
    }
    while (last > 0 && (last == '/' || last == 0xFF0F || last == 0x2215 || last == 0x2044)) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    while (last > 0 && (last == ')' || last == 0xFF09 || last == '(' || last == 0xFF08)) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    while (last > 0 && IsLeaderCp(last)) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    if (last <= 0 || scan < 0) {
        str::Free(s);
        return false;
    }
    int end = scan;
    Utf8CodepointNext(s, len, end);
    s[end] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    if (GlyphCount(s) < 1 || !HasLetterOrCjk(s)) {
        str::Free(s);
        return false;
    }
    *titleOut = s;
    *pageOut = page;
    return true;
}

static bool IsCnNumeral(int cp) {
    return cp == 0x4e00 || cp == 0x4e8c || cp == 0x4e09 || cp == 0x56db || cp == 0x4e94 || cp == 0x516d ||
           cp == 0x4e03 || cp == 0x516b || cp == 0x4e5d || cp == 0x5341 || cp == 0x767e || cp == 0x5343 || cp == 0x96f6;
}

static int CnNumeralValue(int cp) {
    if (cp == 0x4e00) {
        return 1;
    }
    if (cp == 0x4e8c) {
        return 2;
    }
    if (cp == 0x4e09) {
        return 3;
    }
    if (cp == 0x56db) {
        return 4;
    }
    if (cp == 0x4e94) {
        return 5;
    }
    if (cp == 0x516d) {
        return 6;
    }
    if (cp == 0x4e03) {
        return 7;
    }
    if (cp == 0x516b) {
        return 8;
    }
    if (cp == 0x4e5d) {
        return 9;
    }
    if (cp == 0x5341) {
        return 10;
    }
    return 0;
}

// 第X<unit> outline depth: 编/章/篇/回/课/讲=1, 节/条=2, 款=3, 项=4.
static int DepthFromDiUnit(int mark) {
    if (mark == 0x7F16 || mark == 0x7AE0 || mark == 0x7BC7 || mark == 0x56DE || mark == 0x8BFE || mark == 0x8BB2) {
        return 1;
    }
    if (mark == 0x8282 || mark == 0x6761) {
        return 2;
    }
    if (mark == 0x6B3E) {
        return 3;
    }
    if (mark == 0x9879) {
        return 4;
    }
    return 0;
}

static bool DiUnitIsChapter(int mark) {
    return mark == 0x7F16 || mark == 0x7AE0 || mark == 0x7BC7 || mark == 0x56DE || mark == 0x8BFE || mark == 0x8BB2;
}

static bool IsParenOpenCp(int cp) {
    // OCR often reads fullwidth （ as CJK 〈 (U+3008) instead of U+FF08 or '('.
    return cp == 0xFF08 || cp == '(' || cp == 0x3008;
}

static bool IsParenCloseCp(int cp) {
    return cp == 0xFF09 || cp == ')' || cp == 0x3009;
}

// i is already past the opening （ or (. Consume 一/十一/12 then ） or ).
static bool ConsumeParenNumberingAfterOpen(const char* s, int len, int& i) {
    int nInside = 0;
    while (i < len && nInside < 6) {
        int t = i;
        int n = Utf8CodepointNext(s, len, i);
        if (IsCnNumeral(n) || IsDigitCp(n)) {
            nInside++;
            continue;
        }
        i = t;
        break;
    }
    int close = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return nInside > 0 && IsParenCloseCp(close);
}

enum class MarkerType {
    None,
    ChineseDunhao, // 一、 一. 一．
    ChineseParen,  // （一）
    ArabicDot,     // 1.  1、  1.1
    ArabicParen,   // （1）
    Chapter,       // 第一章
    Section,       // 第一节
    Article,       // 第一条
    Appendix,      // 附件
    YiShi,         // 一是
    DiYiComma      // 第一，
};

struct HeadingMarker {
    MarkerType type = MarkerType::None;
    int number = -1;
    int prefixLength = 0;
    int rank = 0;
};

static bool IsDunhaoSepCp(int cp) {
    return cp == 0x3001 || cp == '.' || cp == 0xFF0E;
}

// "1." / "1．" / "1、" / OCR "1。" (ideographic full stop used as a list dot)
static bool IsSimpleArabicListSepCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x3001 || cp == 0x3002;
}

static bool IsIdeoCommaCp(int cp) {
    return cp == 0xFF0C || cp == ',';
}

static int ConsumeOfficialNumber(const char* s, int len, int& i) {
    int n = 0;
    int nTok = 0;
    while (i < len) {
        int save = i;
        int next = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(next)) {
            n = n * 10 + DigitValue(next);
            nTok++;
            continue;
        }
        if (IsCnNumeral(next)) {
            int cv = CnNumeralValue(next);
            if (cv == 10 && n == 0) {
                n = 10;
            } else if (cv == 10 && n > 0 && n < 10) {
                n = n * 10;
            } else if (n == 10 && cv > 0 && cv < 10) {
                n += cv;
            } else if (n == 0 && cv > 0) {
                n = cv;
            } else {
                i = save;
                break;
            }
            nTok++;
            continue;
        }
        i = save;
        break;
    }
    return nTok > 0 ? n : -1;
}

static void SkipSpacesUtf8(const char* s, int len, int& i) {
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 32 && cp != 0x3000) {
            i = save;
            return;
        }
    }
}

static HeadingMarker ParseHeadingMarker(const char* s) {
    HeadingMarker m;
    if (!s || !s[0]) {
        return m;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int start = i;
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp <= 0) {
        return m;
    }
    if (IsListBulletCp(cp)) {
        int after = i;
        SkipSpacesUtf8(s, len, after);
        int look = 0;
        if (after < len) {
            int t = after;
            look = Utf8CodepointNext(s, len, t);
        }
        if (IsDigitCp(look) || IsCnNumeral(look) || IsParenOpenCp(look) || look == 0x9644) {
            i = after;
            start = i;
            cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
            if (cp <= 0) {
                return m;
            }
        }
    }
    if (cp == 0x9644) { // 附
        int cp2 = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (cp2 == 0x4EF6) { // 件
            m.type = MarkerType::Appendix;
            m.number = ConsumeOfficialNumber(s, len, i);
            if (m.number < 0) {
                m.number = 1;
            }
            m.prefixLength = i;
            m.rank = 1;
            return m;
        }
        return m;
    }
    if (cp == 0x7B2C) { // 第
        SkipSpacesUtf8(s, len, i);
        int n = ConsumeOfficialNumber(s, len, i);
        if (n < 0) {
            return m;
        }
        SkipSpacesUtf8(s, len, i);
        int markPos = i;
        int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
        int depth = DepthFromDiUnit(mark);
        if (depth > 0) {
            if (DiUnitIsChapter(mark)) {
                m.type = MarkerType::Chapter;
            } else if (mark == 0x8282) {
                m.type = MarkerType::Section;
            } else {
                m.type = MarkerType::Article;
            }
            m.number = n;
            m.prefixLength = i;
            m.rank = depth;
            return m;
        }
        i = markPos;
        SkipSpacesUtf8(s, len, i);
        int commaPos = i;
        int comma = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (IsIdeoCommaCp(comma)) {
            m.type = MarkerType::DiYiComma;
            m.number = n;
            m.prefixLength = i;
            m.rank = 0;
            return m;
        }
        i = commaPos;
        if (mark == 0x7AE5) { // 童, OCR of 章
            i = markPos;
            Utf8CodepointNext(s, len, i);
            m.type = MarkerType::Chapter;
            m.number = n;
            m.prefixLength = i;
            m.rank = 1;
            return m;
        }
        return m;
    }
    if (cp == 0x5E8F) { // 序章 / 序篇
        SkipSpacesUtf8(s, len, i);
        int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (DiUnitIsChapter(mark)) {
            m.type = MarkerType::Chapter;
            m.number = 0;
            m.prefixLength = i;
            m.rank = 1;
            return m;
        }
        return m;
    }
    if (cp == 0x672B) { // 末章
        SkipSpacesUtf8(s, len, i);
        int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (DiUnitIsChapter(mark)) {
            m.type = MarkerType::Chapter;
            m.number = 0;
            m.prefixLength = i;
            m.rank = 1;
            return m;
        }
        return m;
    }
    if (IsParenOpenCp(cp)) {
        int inner = i;
        int firstInner = inner < len ? Utf8CodepointNext(s, len, inner) : 0;
        int tmp = i;
        if (ConsumeParenNumberingAfterOpen(s, len, tmp)) {
            int numAt = i;
            int n = ConsumeOfficialNumber(s, len, numAt);
            i = tmp;
            m.number = n > 0 ? n : 1;
            m.prefixLength = i;
            if (IsDigitCp(firstInner)) {
                // Books put printed pages in parens: "(144) 不适于…". 公文 clauses are
                // (1)(2)(3) or （十）. A two-digit+ Arabic paren is a page, not a level.
                if (n >= 10) {
                    HeadingMarker empty;
                    return empty;
                }
                m.type = MarkerType::ArabicParen;
                m.rank = 4;
            } else if (IsCnNumeral(firstInner)) {
                m.type = MarkerType::ChineseParen;
                m.rank = 2;
            } else {
                m.type = MarkerType::ChineseParen;
                m.rank = 3;
            }
            return m;
        }
        return m;
    }
    if (IsCnNumeral(cp)) {
        i = start;
        int n = ConsumeOfficialNumber(s, len, i);
        SkipSpacesUtf8(s, len, i);
        int sepPos = i;
        int sep = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (IsDunhaoSepCp(sep)) {
            m.type = MarkerType::ChineseDunhao;
            m.number = n > 0 ? n : 1;
            m.prefixLength = i;
            m.rank = 1;
            return m;
        }
        if (sep == 0x662F) { // 是
            m.type = MarkerType::YiShi;
            m.number = n > 0 ? n : 1;
            m.prefixLength = i;
            m.rank = 0;
            return m;
        }
        i = sepPos;
        return m;
    }
    if (IsDigitCp(cp)) {
        i = start;
        int dots = 0;
        int n = 0;
        while (i < len) {
            int save = i;
            int next = Utf8CodepointNext(s, len, i);
            if (IsDigitCp(next)) {
                n = n * 10 + DigitValue(next);
                continue;
            }
            if (next == '.' || next == 0xFF0E) {
                if (DottedFollowedByDate(s, len, i)) {
                    i = save;
                    break;
                }
                int after = i;
                int look = after < len ? Utf8CodepointNext(s, len, after) : 0;
                if (IsDigitCp(look)) {
                    // "2.10" stays dotted; "2.12333电话…" is a list "2." plus a phone/ID run.
                    if (CountAsciiDigitsFrom(s, len, i) >= 4) {
                        i = save;
                        break;
                    }
                    dots++;
                    i = after;
                    n = DigitValue(look);
                    continue;
                }
                i = save;
                break;
            }
            if (next == 0x3001) {
                i = save;
                break;
            }
            i = save;
            break;
        }
        if (dots >= 1) {
            if (!HasLetterOrCjk(s + i)) {
                return m;
            }
            m.type = MarkerType::ArabicDot;
            m.number = n;
            m.prefixLength = i;
            m.rank = dots + 1;
            return m;
        }
        SkipSpacesUtf8(s, len, i);
        int save = i;
        int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (IsSimpleArabicListSepCp(mark)) {
            const char* rest = s + i;
            if (!HasLetterOrCjk(rest) && !LooksLikeUrlStart(rest)) {
                return m;
            }
            m.type = MarkerType::ArabicDot;
            m.number = n;
            m.prefixLength = i;
            m.rank = 3;
            return m;
        }
        i = save;
        return m;
    }
    return m;
}

static int HeadingLevelFromText(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    if (str::StartsWithI(s, "chapter") || str::StartsWithI(s, "part ") || str::StartsWithI(s, "appendix")) {
        return 1;
    }
    if (str::StartsWithI(s, "section")) {
        return 2;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp <= 0) {
        return 0;
    }
    // 总序
    if (cp == 0x603B) {
        int cp2 = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (cp2 == 0x5E8F) {
            return 1;
        }
    }
    // 后记
    if (cp == 0x540E) {
        int j = i;
        int cp2 = j < len ? Utf8CodepointNext(s, len, j) : 0;
        if (cp2 == 0x8BB0) {
            return 1;
        }
    }
    if (str::Find(s, "\xE5\x8F\x82\xE8\x80\x83\xE6\x96\x87\xE7\x8C\xAE")) { // 参考文献
        return 1;
    }
    // 前言
    if (cp == 0x524D) {
        int j = i;
        int cp2 = j < len ? Utf8CodepointNext(s, len, j) : 0;
        if (cp2 == 0x8A00) {
            return 1;
        }
    }
    if (StartsWithXinDeHeading(s)) {
        return 1;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    return m.rank;
}

// Numbered 公文 headings may skip a level (一、 then 1. with no （一）).
// Only font-guessed headings are squeezed into consecutive outline depths.
static int ClampOutlineLevel(int numbered, int lvl, int prevLevel, bool hasPrev) {
    if (lvl < 1) {
        return lvl;
    }
    if (numbered < 1 && hasPrev && lvl > prevLevel + 1) {
        lvl = prevLevel + 1;
    }
    if (lvl > 6) {
        lvl = 6;
    }
    return lvl;
}

struct ParsedNumbering {
    char raw[40] = {};
    int comp[4] = {};
    int nComp = 0;
    int depth = 0;
    bool isChapter = false;
    bool hadTrailingDot = false;
};

static void ParseHeadingNumbering(const char* s, ParsedNumbering* out) {
    out->raw[0] = 0;
    out->nComp = 0;
    out->depth = 0;
    out->isChapter = false;
    out->hadTrailingDot = false;
    for (int k = 0; k < 4; k++) {
        out->comp[k] = 0;
    }
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int c = Utf8CodepointNext(s, len, i);
        if (c > 32 && c != 0x3000) {
            i = save;
            break;
        }
    }
    int numStart = i;
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp <= 0) {
        return;
    }
    if (cp == 0x7B2C) {
        SkipSpacesUtf8(s, len, i);
        int n = 0;
        int nDigits = 0;
        while (i < len) {
            int save = i;
            int next = Utf8CodepointNext(s, len, i);
            if (IsDigitCp(next)) {
                n = n * 10 + DigitValue(next);
                nDigits++;
                continue;
            }
            if (IsCnNumeral(next)) {
                int cv = CnNumeralValue(next);
                if (cv == 10 && n == 0) {
                    n = 10;
                } else if (n == 10 && cv > 0 && cv < 10) {
                    n += cv;
                } else if (n == 0 && cv > 0) {
                    n = cv;
                }
                nDigits++;
                continue;
            }
            i = save;
            break;
        }
        SkipSpacesUtf8(s, len, i);
        int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
        int depth = DepthFromDiUnit(mark);
        if (nDigits > 0 && (depth > 0 || mark == 0 || mark == 0x7AE5)) {
            out->isChapter = mark == 0 || mark == 0x7AE5 ? true : DiUnitIsChapter(mark);
            out->nComp = 1;
            out->comp[0] = n;
            out->depth = mark == 0 || mark == 0x7AE5 ? 1 : depth;
            int rawLen = i - numStart;
            if (rawLen > 0 && rawLen < (int)sizeof(out->raw)) {
                memcpy(out->raw, s + numStart, (size_t)rawLen);
                out->raw[rawLen] = 0;
            }
            return;
        }
        return;
    }
    if (!IsDigitCp(cp)) {
        return;
    }
    int v = DigitValue(cp);
    int nDig = 1;
    while (i < len && nDig < 4 && out->nComp < 4) {
        int save = i;
        int next = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(next)) {
            v = v * 10 + DigitValue(next);
            nDig++;
            continue;
        }
        if (next == '.' || next == 0xFF0E) {
            int after = i;
            int look = after < len ? Utf8CodepointNext(s, len, after) : 0;
            if (IsDigitCp(look) && DottedFollowedByDate(s, len, i)) {
                out->comp[out->nComp++] = v;
                out->hadTrailingDot = true;
                break;
            }
            out->comp[out->nComp++] = v;
            v = 0;
            nDig = 0;
            if (IsDigitCp(look)) {
                i = after;
                v = DigitValue(look);
                nDig = 1;
                continue;
            }
            out->hadTrailingDot = true;
            i = save;
            break;
        }
        i = save;
        break;
    }
    if (nDig > 0 && out->nComp < 4) {
        out->comp[out->nComp++] = v;
    }
    int rawLen = i - numStart;
    if (rawLen > 0 && rawLen < (int)sizeof(out->raw)) {
        memcpy(out->raw, s + numStart, (size_t)rawLen);
        out->raw[rawLen] = 0;
    }
    if (out->nComp >= 2) {
        out->depth = out->nComp;
    } else if (out->nComp == 1 && out->hadTrailingDot) {
        out->depth = 3;
    }
}

static bool NumberingIsBareArabicChapter(const ParsedNumbering& num, const char* title) {
    if (num.nComp != 1 || num.hadTrailingDot || num.isChapter || num.comp[0] < 1) {
        return false;
    }
    if (!title || !num.raw[0]) {
        return false;
    }
    int len = (int)str::Len(title);
    int start = 0;
    SkipSpacesUtf8(title, len, start);
    int first = start < len ? Utf8CodepointNext(title, len, start) : 0;
    if (!IsDigitCp(first)) {
        return false;
    }
    int n = (int)str::Len(num.raw);
    if (n >= len) {
        return false;
    }
    int i = n;
    int cp = Utf8CodepointNext(title, len, i);
    if (cp > 32 && cp != 0x3000) {
        return false;
    }
    return HasLetterOrCjk(title + i);
}

static int LevelFromPrintedTitle(const char* title) {
    ParsedNumbering num;
    ParseHeadingNumbering(title, &num);
    if (num.isChapter || NumberingIsBareArabicChapter(num, title)) {
        return 1;
    }
    if (num.depth > 0) {
        return num.depth;
    }
    int hl = HeadingLevelFromText(title);
    if (hl > 0) {
        return hl;
    }
    if (num.nComp == 1) {
        return 1;
    }
    return 1;
}

static bool StartsWithXinDeHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int c1 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    int c2 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (c1 != 0x5FC3 || c2 != 0x5F97) {
        return false;
    }
    SkipSpacesUtf8(s, len, i);
    int n = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (IsCnNumeral(n) || IsDigitCp(n) || IsParenOpenCp(n)) {
        return true;
    }
    // OCR often reads 心得一 as 心得－ / 心得—.
    return n == 0xFF0D || n == 0x2013 || n == 0x2014 || n == 0x2015 || n == '-';
}

static bool LineLooksLikeBookPartTitle(const char* s) {
    if (!s || !s[0] || LooksLikePrintedTocHeading(s) || StartsWithXinDeHeading(s)) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 4 || g > 16) {
        return false;
    }
    if (!HasLetterOrCjk(s)) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type != MarkerType::None) {
        return false;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (num.nComp >= 1) {
        return false;
    }
    return !str::Find(s, "，") && !str::Find(s, "。");
}

static bool LineLooksLikeChapterSubtitle(const char* s) {
    if (!s || !s[0] || LooksLikePrintedTocHeading(s) || StartsWithXinDeHeading(s)) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type != MarkerType::None || HeadingLevelFromText(s) >= 1) {
        return false;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (num.nComp >= 1) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 4 || g > 18) {
        return false;
    }
    if (!HasLetterOrCjk(s)) {
        return false;
    }
    if (str::Find(s, "，") || str::Find(s, "。") || str::Find(s, "？") || str::Find(s, "?") || str::Find(s, "！") ||
        str::Find(s, "是错误的") || str::StartsWith(s, "认为")) {
        return false;
    }
    return true;
}

static void StripLeadingSubtitleDash(char** titleOut) {
    char* s = *titleOut;
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp == '~' || cp == 0xFF5E || cp == 0x2014 || cp == 0x2013 || cp == 0xFF0D || cp == '-' || cp == 0x2500 ||
            cp == 0x2015) {
            continue;
        }
        i = save;
        break;
    }
    if (i < 1) {
        return;
    }
    memmove(s, s + i, (size_t)(len - i + 1));
    str::TrimWSInPlace(s, str::TrimOpt::Both);
}

static bool BookTitleIsSpine(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::StartsWithI(s, "chapter") || str::StartsWithI(s, "part ")) {
        return true;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (num.isChapter) {
        return true;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::Chapter) {
        return true;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp != 0x7B2C) {
        return false;
    }
    SkipSpacesUtf8(s, len, i);
    int n = ConsumeOfficialNumber(s, len, i);
    if (n < 0) {
        return false;
    }
    SkipSpacesUtf8(s, len, i);
    int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return mark == 0x8BB2 || mark == 0x8BFE || mark == 0x5377; // 讲 课 卷
}

static int BookSpineNumber(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (num.comp[0] > 0 && (num.isChapter || BookTitleIsSpine(s))) {
        return num.comp[0];
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::Chapter && m.number > 0) {
        return m.number;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp != 0x7B2C) {
        return num.comp[0] > 0 ? num.comp[0] : 0;
    }
    SkipSpacesUtf8(s, len, i);
    int n = ConsumeOfficialNumber(s, len, i);
    return n > 0 ? n : 0;
}

static bool ParseBookLabelSerial(const char* s, int* prefixKey, int* numOut) {
    if (prefixKey) {
        *prefixKey = 0;
    }
    if (numOut) {
        *numOut = 0;
    }
    if (!s || !s[0] || BookTitleIsSpine(s)) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type != MarkerType::None) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int preStart = i;
    int nPre = 0;
    while (i < len && nPre < 4) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp >= 0x4E00 && cp <= 0x9FFF && !IsCnNumeral(cp)) {
            nPre++;
            continue;
        }
        i = save;
        break;
    }
    if (nPre < 1) {
        return false;
    }
    int preEnd = i;
    SkipSpacesUtf8(s, len, i);
    int n = ConsumeOfficialNumber(s, len, i);
    if (n < 1) {
        return false;
    }
    unsigned uh = 0;
    for (int k = preStart; k < preEnd; k++) {
        uh = uh * 131u + (unsigned char)s[k];
    }
    if (prefixKey) {
        *prefixKey = (int)(uh % 100000u);
    }
    if (numOut) {
        *numOut = n;
    }
    return true;
}

static int BookPrintedSchemeKey(const char* title, int* numOut) {
    if (numOut) {
        *numOut = 0;
    }
    if (!title || !title[0]) {
        return 0;
    }
    HeadingMarker m = ParseHeadingMarker(title);
    if (m.type != MarkerType::None && m.number > 0) {
        if (numOut) {
            *numOut = m.number;
        }
        switch (m.type) {
            case MarkerType::Chapter:
                return 10;
            case MarkerType::Appendix:
                return 15;
            case MarkerType::ChineseDunhao:
                return 20;
            case MarkerType::Section:
                return 25;
            case MarkerType::Article:
                return 28;
            case MarkerType::ChineseParen:
                return 30;
            case MarkerType::ArabicDot:
                return m.rank <= 2 ? 35 : 40;
            case MarkerType::ArabicParen:
                if (m.number >= 10) {
                    return 0;
                }
                return 50;
            default:
                break;
        }
    }
    ParsedNumbering num;
    ParseHeadingNumbering(title, &num);
    if (num.isChapter || BookTitleIsSpine(title)) {
        int spine = BookSpineNumber(title);
        if (numOut) {
            *numOut = spine > 0 ? spine : 1;
        }
        return 10;
    }
    if (num.nComp >= 2) {
        if (numOut) {
            *numOut = num.comp[num.nComp - 1];
        }
        return 35;
    }
    int prefixKey = 0;
    int serial = 0;
    if (ParseBookLabelSerial(title, &prefixKey, &serial)) {
        if (numOut) {
            *numOut = serial;
        }
        return 60 + (prefixKey % 30);
    }
    return 0;
}

static bool LooksLikeTocNumToken(const char* s) {
    if (!s || !s[0] || HasLetterOrCjk(s)) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 2 || g > 12) {
        return false;
    }
    if (LineLooksLikePageNumber(s) || ParseBarePrintedPage(s) > 0) {
        return false;
    }
    return HeadingLevelFromText(s) >= 2;
}

static void EnsureNumberingSpaceInTitle(char** titleOut) {
    char* s = *titleOut;
    if (!s || !s[0]) {
        return;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (!num.raw[0]) {
        return;
    }
    int n = (int)str::Len(num.raw);
    if (!s[n] || s[n] == ' ' || s[n] == '\t') {
        return;
    }
    char* neu = str::Join(num.raw, " ", s + n);
    if (!neu) {
        return;
    }
    str::Free(s);
    *titleOut = neu;
}

static void NormalizeDiUnitComma(char** titleOut) {
    char* s = *titleOut;
    if (!s || !s[0]) {
        return;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type != MarkerType::Chapter || m.prefixLength < 1) {
        return;
    }
    int len = (int)str::Len(s);
    int next = m.prefixLength;
    SkipSpacesUtf8(s, len, next);
    int afterMark = next;
    int cp = Utf8CodepointNext(s, len, afterMark);
    if (cp != 0xFF0C && cp != ',' && cp != 0x3001) {
        return;
    }
    char prefix[64];
    if (m.prefixLength >= (int)sizeof(prefix)) {
        return;
    }
    memcpy(prefix, s, (size_t)m.prefixLength);
    prefix[m.prefixLength] = 0;
    char* neu = str::Join(prefix, " ", s + afterMark);
    if (!neu) {
        return;
    }
    str::Free(s);
    *titleOut = neu;
}

static void StripTrailingGluedPage(char* s) {
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int end = len;
    int nDig = 0;
    while (end > 0) {
        int prev = end;
        int cp = Utf8CodepointPrev(s, len, prev);
        if (!IsDigitCp(cp)) {
            break;
        }
        end = prev;
        nDig++;
        if (nDig > 4) {
            return;
        }
    }
    if (nDig < 1) {
        return;
    }
    int digitStart = end;
    int cut = end;
    while (cut > 0) {
        int prev = cut;
        int cp = Utf8CodepointPrev(s, len, prev);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            cut = prev;
            continue;
        }
        break;
    }
    if (cut >= digitStart) {
        return;
    }
    char save = s[cut];
    s[cut] = 0;
    if (!HasLetterOrCjk(s)) {
        s[cut] = save;
        return;
    }
    str::TrimWSInPlace(s, str::TrimOpt::Both);
}

static void PolishPrintedHitTitle(char** titleOut) {
    EnsureNumberingSpaceInTitle(titleOut);
    NormalizeDiUnitComma(titleOut);
    if (titleOut && *titleOut) {
        StripTrailingGluedPage(*titleOut);
    }
}

static float MedianPositive(Vec<float>& v) {
    int n = v.Size();
    if (n < 1) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (v[j] < v[i]) {
                float t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    return v[n / 2];
}

static void FreeScanLines(Vec<ScanLine>& lines) {
    for (int i = 0; i < lines.Size(); i++) {
        str::Free(lines[i].text);
        lines[i].text = nullptr;
    }
    lines.Reset();
}

static void RebuildVisualLines(Vec<ScanLine>& pageLines);

static int LeadingDirtDotBytes(const char* s, const Rect* coords, int n, int* dx0Out, int* dx1Out, int* sameOut) {
    if (dx0Out) {
        *dx0Out = 0;
    }
    if (dx1Out) {
        *dx1Out = 0;
    }
    if (sameOut) {
        *sameOut = 0;
    }
    if (!s || n < 1) {
        return 0;
    }
    int i = 0;
    int cp0 = Utf8CodepointNext(s, n, i);
    if (!IsListBulletCp(cp0) || i >= n) {
        return 0;
    }
    if (ParseHeadingMarker(s + i).rank < 1) {
        return 0;
    }
    int j = i;
    int cp1 = j < n ? Utf8CodepointNext(s, n, j) : 0;
    int cp2 = j < n ? Utf8CodepointNext(s, n, j) : 0;
    Rect r0 = coords ? coords[0] : Rect{};
    Rect r1 = (coords && i < n) ? coords[i] : Rect{};
    if (dx0Out) {
        *dx0Out = r0.dx;
    }
    if (dx1Out) {
        *dx1Out = r1.dx;
    }
    bool same = r0.dx > 0 && r0.x == r1.x && r0.dx == r1.dx && r0.y == r1.y;
    if (sameOut) {
        *sameOut = same ? 1 : 0;
    }
    bool periodAfterDigit = IsDigitCp(cp1) && IsListBulletCp(cp2);
    bool tiny = r0.dx > 0 && r1.dx > 0 && !same && r0.dx * 2 <= r1.dx;
    bool overlap = r0.dx > 0 && r1.dx > 0 && !same && r0.x + r0.dx > r1.x + r1.dx / 4;
    if (periodAfterDigit || tiny || overlap || same) {
        return i;
    }
    return 0;
}

static bool AppendUtf8ScanLine(const char* text, const Rect* coords, int start, int end, int pageNo,
                               Vec<ScanLine>& out) {
    if (!text || !coords || end <= start) {
        return false;
    }
    int n = end - start;
    int dx0 = 0, dx1 = 0, sameBox = 0;
    int skip = LeadingDirtDotBytes(text + start, coords + start, n, &dx0, &dx1, &sameBox);
    int from = start + skip;
    if (from >= end) {
        return false;
    }
    Rect bbox;
    bool has = false;
    float hSum = 0;
    int nH = 0;
    for (int k = from; k < end; k++) {
        Rect r = coords[k];
        if (r.dx || r.x) {
            if (!has) {
                bbox = r;
                has = true;
            } else {
                bbox = bbox.Union(r);
            }
            if (r.dy > 0) {
                hSum += (float)r.dy;
                nH++;
            }
        }
    }
    ScanLine sl;
    sl.text = str::Dup(text + from, (size_t)(end - from));
    str::TrimWSInPlace(sl.text, str::TrimOpt::Both);
    if (!sl.text[0] || LooksLikeArchiveJunk(sl.text)) {
        str::Free(sl.text);
        return false;
    }
    sl.srcPage = pageNo;
    sl.x = (float)bbox.x;
    sl.y = (float)bbox.y;
    sl.dx = (float)bbox.dx;
    sl.dy = (float)bbox.dy;
    sl.fontSize = nH > 0 ? hSum / (float)nH : (float)bbox.dy;
    sl.bold = false;
    out.Append(sl);
    return true;
}

// Rightmost large x-gap whose right span is a printed page token (13 / ··13 / (13)).
static int FindTocPageSpanSplit(const char* text, const Rect* coords, int start, int end) {
    if (!text || !coords || end - start < 3) {
        return -1;
    }
    float dxSum = 0;
    int nDx = 0;
    Rect prev{};
    bool have = false;
    int bestSplit = -1;
    int i = start;
    while (i < end) {
        unsigned char b = (unsigned char)text[i];
        bool glyphStart = (b & 0xC0) != 0x80;
        if (glyphStart) {
            Rect r = coords[i];
            if (r.dx > 0) {
                dxSum += (float)r.dx;
                nDx++;
            }
            if (have && (r.dx || r.x)) {
                float gap = (float)r.x - ((float)prev.x + (float)prev.dx);
                float avg = nDx > 0 ? dxSum / (float)nDx : 8.f;
                float need = avg * 2.5f;
                if (need < 20.f) {
                    need = 20.f;
                }
                if (gap >= need) {
                    bestSplit = i;
                }
            }
            if (r.dx || r.x) {
                prev = r;
                have = true;
            }
        }
        i++;
    }
    if (bestSplit <= start || bestSplit >= end) {
        return -1;
    }
    char* left = str::Dup(text + start, (size_t)(bestSplit - start));
    char* right = str::Dup(text + bestSplit, (size_t)(end - bestSplit));
    str::TrimWSInPlace(left, str::TrimOpt::Both);
    str::TrimWSInPlace(right, str::TrimOpt::Both);
    bool ok = left[0] && right[0] && HasLetterOrCjk(left) && LineLooksLikePageNumber(right);
    str::Free(left);
    str::Free(right);
    return ok ? bestSplit : -1;
}

static void CollectLinesFromUtf8(const char* text, Rect* coords, int len, int pageNo, Vec<ScanLine>& out) {
    if (!text || !coords || len <= 0) {
        return;
    }
    int i = 0;
    while (i < len) {
        while (i < len && text[i] == '\n') {
            i++;
        }
        if (i >= len) {
            break;
        }
        int start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        if (i - start < 1) {
            continue;
        }
        int split = FindTocPageSpanSplit(text, coords, start, i);
        if (split > start) {
            AppendUtf8ScanLine(text, coords, start, split, pageNo, out);
            AppendUtf8ScanLine(text, coords, split, i, pageNo, out);
        } else {
            AppendUtf8ScanLine(text, coords, start, i, pageNo, out);
        }
    }
}

static void CollectPageScanLines(EngineBase* engine, int pageNo, Vec<ScanLine>& out) {
    Vec<ScanLine> page;
    Vec<EngineMupdfPageLine> raw;
    int usedOcr = 0;
    if (engine->HasCachedOcrText(pageNo)) {
        int len = 0;
        Rect* coords = nullptr;
        const char* text = nullptr;
        if (engine->TryGetTextForPageUtf8(pageNo, &len, &coords, &text) && text && coords && len > 0) {
            CollectLinesFromUtf8(text, coords, len, pageNo, page);
            usedOcr = page.Size() > 0 ? 1 : 0;
        }
    }
    if (page.Size() < 1) {
        if (!EngineMupdfCollectPageLines(engine, pageNo, raw)) {
            return;
        }
        for (int i = 0; i < raw.Size(); i++) {
            ScanLine sl;
            sl.text = raw[i].text;
            raw[i].text = nullptr;
            sl.srcPage = pageNo;
            sl.x = raw[i].x;
            sl.y = raw[i].y;
            sl.dx = raw[i].dx;
            sl.dy = raw[i].dy;
            sl.fontSize = raw[i].fontSize;
            sl.bold = raw[i].bold;
            if (!sl.text || LooksLikeArchiveJunk(sl.text)) {
                str::Free(sl.text);
                continue;
            }
            page.Append(sl);
        }
        EngineMupdfFreePageLines(raw);
    }
    RebuildVisualLines(page);
    for (int i = 0; i < page.Size(); i++) {
        out.Append(page[i]);
        page[i].text = nullptr;
    }
}

static int MapPrintedPage(EngineBase* engine, int printed, const Vec<char*>& labels) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", printed);
    for (int i = 0; i < labels.Size(); i++) {
        if (labels[i] && str::Eq(labels[i], buf)) {
            return i + 1;
        }
    }
    if (printed >= 1 && engine && printed <= engine->PageCount()) {
        return printed;
    }
    return 0;
}

static void FindDestOnPage(const Vec<ScanLine>& lines, int destPage, const char* title, float* x, float* y) {
    *x = 0;
    *y = 0;
    if (!title || destPage < 1) {
        return;
    }
    int best = -1;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (sl.srcPage != destPage || !sl.text) {
            continue;
        }
        if (str::StartsWith(sl.text, title) || str::Find(sl.text, title)) {
            best = i;
            break;
        }
    }
    if (best >= 0) {
        *x = lines[best].x;
        *y = lines[best].y;
    }
}

static void StripLeadingListBullet(char** titleOut) {
    char* s = titleOut ? *titleOut : nullptr;
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int bulletStart = i;
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (!IsListBulletCp(cp)) {
        return;
    }
    SkipSpacesUtf8(s, len, i);
    if (i <= bulletStart || ParseHeadingMarker(s + i).rank < 1) {
        return;
    }
    memmove(s + bulletStart, s + i, (size_t)(len - i + 1));
}

static void StripGluedBodyAfterHeading(char* s);

static ExtractedTocItem* NewItem(const char* title, int pageNo, float x, float y, int level, int confidence = 0) {
    auto* n = new ExtractedTocItem;
    n->title = DupTrimmed(title);
    StripLeadingListBullet(&n->title);
    TrimAtNextEmbeddedHeading(n->title);
    TrimTitleToFirstSentence(n->title);
    StripTrailingOcrTitleJunk(n->title);
    StripGluedBodyAfterHeading(n->title);
    TrimAtNextDiHeading(n->title);
    NormalizeTocNumberingParens(&n->title);
    NormalizeTocNumberingDots(&n->title);
    StripNumberingTitleSpace(&n->title);
    n->pageNo = pageNo;
    n->x = x;
    n->y = y;
    n->level = level < 1 ? 1 : level;
    n->confidence = confidence;
    if (n->confidence < 0) {
        n->confidence = 0;
    }
    if (n->confidence > 100) {
        n->confidence = 100;
    }
    return n;
}

static int EnforceMonotonicPages(Vec<ExtractedTocItem*>& flat) {
    int nFix = 0;
    int prev = 0;
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* it = flat[i];
        if (!it) {
            continue;
        }
        int p = it->pageNo;
        if (p < 1) {
            if (prev > 0) {
                it->pageNo = prev;
                nFix++;
            }
            continue;
        }
        if (prev > 0 && p < prev) {
            it->pageNo = prev;
            nFix++;
            p = prev;
        }
        prev = p;
    }
    return nFix;
}

static void BuildTreeFromFlat(Vec<ExtractedTocItem*>& flat, Vec<ExtractedTocItem*>& roots) {
    Vec<ExtractedTocItem*> stack;
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* n = flat[i];
        while (stack.Size() > 0 && stack.Last()->level >= n->level) {
            stack.RemoveLast();
        }
        if (stack.Size() == 0) {
            roots.Append(n);
        } else {
            stack.Last()->children.Append(n);
        }
        stack.Append(n);
    }
    flat.Reset();
}

static int CountExtracted(const Vec<ExtractedTocItem*>& roots) {
    int n = 0;
    for (ExtractedTocItem* r : roots) {
        n++;
        n += CountExtracted(r->children);
    }
    return n;
}

static bool IsPrintedPageParenOpen(int cp) {
    return cp == '(' || cp == 0xFF08;
}

static bool IsPrintedPageParenClose(int cp) {
    return cp == ')' || cp == 0xFF09;
}

static bool LineLooksLikePageNumber(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int afterSpace = i;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 0 && IsLeaderCp(cp) && !IsDigitCp(cp) && cp != '-' && cp != 0x2013 && cp != 0x2014) {
            continue;
        }
        i = save;
        break;
    }
    bool hadLead = i > afterSpace;
    if (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsPrintedPageParenOpen(cp)) {
            hadLead = true;
            SkipSpacesUtf8(s, len, i);
        } else {
            i = save;
        }
    }
    int nDigit = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!IsDigitCp(cp)) {
            i = save;
            break;
        }
        nDigit++;
        if (nDigit > 4) {
            return false;
        }
    }
    if (nDigit < 1) {
        return false;
    }
    int afterDigits = i;
    SkipSpacesUtf8(s, len, i);
    if (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!IsPrintedPageParenClose(cp)) {
            i = save;
        }
    }
    SkipSpacesUtf8(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 0 && IsLeaderCp(cp) && !IsDigitCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    SkipSpacesUtf8(s, len, i);
    if (i < len) {
        return false;
    }
    if (!hadLead && afterDigits < len) {
        return false;
    }
    return true;
}

static bool LineLooksLikeRomanPage(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int n = 0;
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (cp <= 32 || cp == 0x3000) {
            continue;
        }
        if (cp >= 'A' && cp <= 'Z') {
            cp += 32;
        }
        if (cp != 'i' && cp != 'v' && cp != 'x' && cp != 'l' && cp != 'c' && cp != 'm') {
            return false;
        }
        n++;
        if (n > 8) {
            return false;
        }
    }
    return n >= 2 && n <= 8;
}

static int ParseBarePrintedPage(const char* s) {
    int v = 0;
    int n = 0;
    if (!s) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int afterSpace = i;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 0 && IsLeaderCp(cp) && !IsDigitCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    bool hadLead = i > afterSpace;
    if (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsPrintedPageParenOpen(cp)) {
            hadLead = true;
            SkipSpacesUtf8(s, len, i);
        } else {
            i = save;
        }
    }
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(cp)) {
            v = v * 10 + DigitValue(cp);
            n++;
            if (n > 4) {
                return 0;
            }
            continue;
        }
        i = save;
        break;
    }
    int afterDigits = i;
    SkipSpacesUtf8(s, len, i);
    if (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!IsPrintedPageParenClose(cp)) {
            i = save;
        }
    }
    SkipSpacesUtf8(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 0 && IsLeaderCp(cp) && !IsDigitCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    SkipSpacesUtf8(s, len, i);
    if (i < len || n < 1 || v < 1 || v > 9999) {
        return 0;
    }
    if (!hadLead && afterDigits < len) {
        return 0;
    }
    return v;
}

static bool HasLetterOrCjk(const char* s) {
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
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
            return true;
        }
        if (cp >= 0x4E00 && cp <= 0x9FFF) {
            return true;
        }
    }
    return false;
}

static bool LooksLikeUrlStart(const char* s) {
    if (!s) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    return str::StartsWithI(s + i, "http://") || str::StartsWithI(s + i, "https://") ||
           str::StartsWithI(s + i, "ftp://");
}

static bool LooksLikeArchiveJunk(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::Find(s, "扫描全能王") || str::FindI(s, "camscanner")) {
        return true;
    }
    if (str::FindI(s, "annas_archive") || str::FindI(s, "original_files")) {
        return true;
    }
    return str::EndsWithI(s, ".zst") || str::EndsWithI(s, ".json") || str::EndsWithI(s, ".tar") ||
           str::EndsWithI(s, ".zip") || str::EndsWithI(s, ".json.zst") || str::EndsWithI(s, ".tar.zst");
}

static bool LooksLikeOfficialBoilerplate(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::Find(s, "抄送") || str::Find(s, "签发人") || str::Find(s, "主题词") || str::Find(s, "此件") ||
        str::Find(s, "公开属性") || str::StartsWith(s, "主送")) {
        return true;
    }
    if (str::Eq(s, "紧急") || str::Eq(s, "特急") || str::Eq(s, "加急") || str::Eq(s, "内部") || str::Eq(s, "机密") ||
        str::Eq(s, "文件") || str::Eq(s, "明电") || str::Find(s, "等级平急")) {
        return true;
    }
    int g = GlyphCount(s);
    if (g <= 18 && (str::Find(s, "人民政府") || str::Find(s, "办公厅") || str::Find(s, "委员会")) &&
        str::Find(s, "文件")) {
        return true;
    }
    if (g >= 4 && g <= 22 && !str::StartsWith(s, "关于") &&
        (str::EndsWith(s, "厅") || str::EndsWith(s, "局") || str::EndsWith(s, "党组") || str::EndsWith(s, "人民政府") ||
         str::EndsWith(s, "管理局") || str::EndsWith(s, "委员会") || str::EndsWith(s, "办公厅"))) {
        return true;
    }
    return false;
}

static bool LooksLikeLeaderTitle(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::Find(s, "......") || str::Find(s, "．．．．") || str::Find(s, "····")) {
        return true;
    }
    int nLead = 0;
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
        if (IsLeaderCp(cp) && cp > 32 && cp != 0x3000) {
            nLead++;
            if (nLead >= 5) {
                return true;
            }
        }
    }
    return false;
}

static bool LooksLikeDocNumberLine(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 4 || g > 28) {
        return false;
    }
    if (str::StartsWith(s, "关于") || str::Find(s, "通知") || str::Find(s, "印发")) {
        return false;
    }
    bool hasHao = str::Find(s, "号") != nullptr;
    bool hasBracket = str::Find(s, "〔") || str::Find(s, "[") || str::Find(s, "［");
    return hasHao && hasBracket;
}

static float ScanLineMidY(const ScanLine& sl) {
    return sl.y + sl.dy * 0.5f;
}

static bool SamePrintedTocRow(const ScanLine& a, const ScanLine& b) {
    float dy = a.dy > b.dy ? a.dy : b.dy;
    if (dy < 6) {
        dy = 6;
    }
    float d = ScanLineMidY(a) - ScanLineMidY(b);
    if (d < 0) {
        d = -d;
    }
    return d <= dy * 1.15f;
}

static void SortScanLinesVisual(Vec<ScanLine>& v) {
    for (int i = 0; i < v.Size(); i++) {
        for (int j = i + 1; j < v.Size(); j++) {
            float yi = ScanLineMidY(v[i]);
            float yj = ScanLineMidY(v[j]);
            bool less = false;
            if (v[j].srcPage != v[i].srcPage) {
                less = v[j].srcPage < v[i].srcPage;
            } else if (yj + 0.5f < yi) {
                less = true;
            } else if (yj <= yi + 0.5f && v[j].x < v[i].x) {
                less = true;
            }
            if (less) {
                ScanLine t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
}

static void JoinScanLineFragment(ScanLine& acc, const ScanLine& add) {
    if (!add.text || !add.text[0]) {
        return;
    }
    bool tight = false;
    if (acc.text && acc.text[0]) {
        int alen = (int)str::Len(acc.text);
        int ai = alen;
        int last = Utf8CodepointPrev(acc.text, alen, ai);
        int bi = 0;
        int blen = (int)str::Len(add.text);
        int first = Utf8CodepointNext(add.text, blen, bi);
        if (IsDigitCp(last) && IsDigitCp(first)) {
            tight = true;
        }
        if ((last == '.' || last == 0xFF0E) && IsDigitCp(first)) {
            int before = Utf8CodepointPrev(acc.text, alen, ai);
            if (IsDigitCp(before)) {
                tight = true;
            }
        }
        if (IsDigitCp(last) && (first == '.' || first == 0xFF0E || first == 0x3002)) {
            tight = true;
        }
        if (IsCnNumeral(last) && IsDunhaoSepCp(first)) {
            tight = true;
        }
    }
    char* n = nullptr;
    if (!acc.text || !acc.text[0]) {
        n = str::Dup(add.text);
    } else if (tight) {
        n = str::Join(acc.text, add.text);
    } else {
        n = str::Join(acc.text, " ", add.text);
    }
    str::Free(acc.text);
    acc.text = n;
    float ax2 = acc.x + acc.dx;
    float ay2 = acc.y + acc.dy;
    float bx2 = add.x + add.dx;
    float by2 = add.y + add.dy;
    float nx = acc.x < add.x ? acc.x : add.x;
    float ny = acc.y < add.y ? acc.y : add.y;
    float nx2 = ax2 > bx2 ? ax2 : bx2;
    float ny2 = ay2 > by2 ? ay2 : by2;
    acc.x = nx;
    acc.y = ny;
    acc.dx = nx2 - nx;
    acc.dy = ny2 - ny;
}

static void CompactScanLines(Vec<ScanLine>& lines) {
    int i = 0;
    while (i < lines.Size()) {
        if (!lines[i].text || !lines[i].text[0]) {
            str::Free(lines[i].text);
            lines[i].text = nullptr;
            lines.RemoveAt(i);
            continue;
        }
        i++;
    }
}

static bool LineIsBareLessonOrChapter(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if ((m.type == MarkerType::Chapter || m.type == MarkerType::Section) && m.prefixLength >= 1) {
        return GlyphCount(s + m.prefixLength) <= 2;
    }
    return false;
}

static bool TitleIsNumberingOnly(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.prefixLength < 1) {
        return false;
    }
    if (m.type != MarkerType::Chapter && m.type != MarkerType::Section && m.type != MarkerType::Article) {
        return false;
    }
    return GlyphCount(s + m.prefixLength) < 2;
}

static bool TitleNeedsWrapContinuation(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = len;
    int last = 0;
    while (i > 0) {
        last = Utf8CodepointPrev(s, len, i);
        if (last <= 32 || last == 0x3000 || IsLeaderCp(last)) {
            continue;
        }
        break;
    }
    if (last == 0xFF0C || last == ',' || last == 0x3001 || last == 0xFF1F || last == '?' || last == 0xFF01 ||
        last == '!') {
        return true;
    }
    return str::Find(s, "《") && !str::Find(s, "》");
}

static bool LineIsPrintedTocContinuation(const char* s) {
    if (!s || !s[0] || LooksLikePrintedTocHeading(s) || LooksLikeArchiveJunk(s) || LineLooksLikePageNumber(s) ||
        LineLooksLikeRomanPage(s)) {
        return false;
    }
    if (BookTitleIsSpine(s) || HeadingLevelFromText(s) >= 1) {
        return false;
    }
    int g = GlyphCount(s);
    return HasLetterOrCjk(s) && g >= 2 && g <= 40;
}

static int BestPrintedTocGlueSlot(const Vec<ScanLine>& pageLines, int i, bool wrap) {
    int best = -1;
    float bestScore = 1e9f;
    for (int j = 0; j < pageLines.Size(); j++) {
        if (i == j || !pageLines[j].text) {
            continue;
        }
        if (!LineIsPrintedTocContinuation(pageLines[j].text)) {
            continue;
        }
        if (pageLines[j].srcPage != pageLines[i].srcPage) {
            continue;
        }
        bool sameRow = SamePrintedTocRow(pageLines[i], pageLines[j]) && pageLines[j].x > pageLines[i].x + 4;
        float gapY = pageLines[j].y - (pageLines[i].y + pageLines[i].dy);
        float lim = pageLines[i].dy > 6 ? pageLines[i].dy * 1.85f : 16;
        bool wrapBelow = wrap && gapY >= -4 && gapY <= lim && pageLines[j].x + 1 >= pageLines[i].x;
        if (!sameRow && !wrapBelow) {
            continue;
        }
        float score = sameRow ? (pageLines[j].x - pageLines[i].x) : (1000.f + gapY);
        if (score < bestScore) {
            bestScore = score;
            best = j;
        }
    }
    return best;
}

static void RepairXinDeOcrDash(char* s) {
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int c1 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    int c2 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (c1 != 0x5FC3 || c2 != 0x5F97) {
        return;
    }
    SkipSpacesUtf8(s, len, i);
    int dashPos = i;
    int dash = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (dash != 0xFF0D && dash != 0x2013 && dash != 0x2014 && dash != 0x2015) {
        return;
    }
    if (dashPos + 2 >= len) {
        return;
    }
    // 一 and the OCR dashes are all 3-byte UTF-8.
    s[dashPos] = '\xE4';
    s[dashPos + 1] = '\xB8';
    s[dashPos + 2] = '\x80';
}

static void RepairOcrDiUnits(Vec<ScanLine>& pageLines) {
    for (int i = 0; i < pageLines.Size(); i++) {
        char* s = pageLines[i].text;
        if (!s || !s[0]) {
            continue;
        }
        RepairXinDeOcrDash(s);
        int len = (int)str::Len(s);
        int iCp = 0;
        while (iCp < len) {
            int save = iCp;
            int cp = Utf8CodepointNext(s, len, iCp);
            if (cp != 0x7B2C) {
                continue;
            }
            SkipSpacesUtf8(s, len, iCp);
            if (ConsumeOfficialNumber(s, len, iCp) < 0) {
                iCp = save;
                Utf8CodepointNext(s, len, iCp);
                continue;
            }
            SkipSpacesUtf8(s, len, iCp);
            int markPos = iCp;
            int mark = iCp < len ? Utf8CodepointNext(s, len, iCp) : 0;
            if (mark == 0x7AE5 && markPos + 2 < len && (unsigned char)s[markPos] == 0xE7 &&
                (unsigned char)s[markPos + 1] == 0xAB) {
                s[markPos + 2] = '\xA0';
            }
        }
    }
}

static void GluePrintedTocWraps(Vec<ScanLine>& pageLines) {
    bool any = true;
    int guard = 0;
    while (any && guard < 64) {
        any = false;
        guard++;
        SortScanLinesVisual(pageLines);
        for (int i = 0; i < pageLines.Size(); i++) {
            if (!pageLines[i].text) {
                continue;
            }
            bool bare = LineIsBareLessonOrChapter(pageLines[i].text);
            bool wrap = TitleNeedsWrapContinuation(pageLines[i].text) && HeadingLevelFromText(pageLines[i].text) >= 1;
            if (!bare && !wrap) {
                continue;
            }
            int j = BestPrintedTocGlueSlot(pageLines, i, wrap);
            if (j < 0) {
                continue;
            }
            JoinScanLineFragment(pageLines[i], pageLines[j]);
            str::Free(pageLines[j].text);
            pageLines[j].text = nullptr;
            CompactScanLines(pageLines);
            any = true;
            break;
        }
    }
}

static void GlueSplitPrintedPageNums(Vec<ScanLine>& pageLines) {
    bool any = true;
    int guard = 0;
    while (any && guard < 32) {
        any = false;
        guard++;
        SortScanLinesVisual(pageLines);
        for (int i = 0; i < pageLines.Size(); i++) {
            if (!LineLooksLikePageNumber(pageLines[i].text)) {
                continue;
            }
            int best = -1;
            float bestX = 1e9f;
            for (int j = 0; j < pageLines.Size(); j++) {
                if (i == j || !LineLooksLikePageNumber(pageLines[j].text)) {
                    continue;
                }
                if (pageLines[j].srcPage != pageLines[i].srcPage) {
                    continue;
                }
                if (!SamePrintedTocRow(pageLines[i], pageLines[j])) {
                    continue;
                }
                if (pageLines[j].x <= pageLines[i].x + 2) {
                    continue;
                }
                float gap = pageLines[j].x - (pageLines[i].x + pageLines[i].dx);
                if (gap > 28) {
                    continue;
                }
                if (pageLines[j].x < bestX) {
                    bestX = pageLines[j].x;
                    best = j;
                }
            }
            if (best < 0) {
                continue;
            }
            JoinScanLineFragment(pageLines[i], pageLines[best]);
            str::Free(pageLines[best].text);
            pageLines[best].text = nullptr;
            CompactScanLines(pageLines);
            any = true;
            break;
        }
    }
}

static bool IsLoneListBulletText(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    if (i >= len) {
        return false;
    }
    int cp = Utf8CodepointNext(s, len, i);
    if (!IsListBulletCp(cp)) {
        return false;
    }
    SkipSpacesUtf8(s, len, i);
    return i >= len;
}

static bool LooksLikeBodyContinuationStart(const char* s);

static void RebuildVisualLines(Vec<ScanLine>& pageLines) {
    int n = pageLines.Size();
    if (n < 2) {
        return;
    }
    SortScanLinesVisual(pageLines);
    Vec<float> hs;
    for (int i = 0; i < n; i++) {
        if (pageLines[i].dy > 1) {
            hs.Append(pageLines[i].dy);
        }
    }
    float med = MedianPositive(hs);
    if (med < 4) {
        med = 8;
    }
    float tol = med * (float)kExtractPdfToc.lineYTolPct / 100.0f;
    float maxGap = med * (float)kExtractPdfToc.lineMaxGapEm;
    float numGap = med * (float)kExtractPdfToc.lineNumTitleGapEm;
    Vec<int> used;
    for (int i = 0; i < n; i++) {
        used.Append(0);
    }
    Vec<ScanLine> out;
    for (int i = 0; i < n; i++) {
        if (used[i] || !pageLines[i].text) {
            continue;
        }
        Vec<int> grp;
        grp.Append(i);
        used[i] = 1;
        for (int j = 0; j < n; j++) {
            if (used[j] || !pageLines[j].text) {
                continue;
            }
            float ay2 = pageLines[i].y + pageLines[i].dy;
            float by2 = pageLines[j].y + pageLines[j].dy;
            float top = pageLines[i].y > pageLines[j].y ? pageLines[i].y : pageLines[j].y;
            float bot = ay2 < by2 ? ay2 : by2;
            float ov = bot - top;
            float mh = pageLines[i].dy < pageLines[j].dy ? pageLines[i].dy : pageLines[j].dy;
            if (mh < 4) {
                mh = 4;
            }
            float d = ScanLineMidY(pageLines[i]) - ScanLineMidY(pageLines[j]);
            if (d < 0) {
                d = -d;
            }
            if (ov < mh * 0.35f && d > tol) {
                continue;
            }
            grp.Append(j);
            used[j] = 1;
        }
        for (int a = 0; a < grp.Size(); a++) {
            for (int b = a + 1; b < grp.Size(); b++) {
                float ya = ScanLineMidY(pageLines[grp[a]]);
                float yb = ScanLineMidY(pageLines[grp[b]]);
                bool less = false;
                if (yb + 0.5f < ya) {
                    less = true;
                } else if (yb <= ya + 0.5f && pageLines[grp[b]].x < pageLines[grp[a]].x) {
                    less = true;
                }
                if (less) {
                    int t = grp[a];
                    grp[a] = grp[b];
                    grp[b] = t;
                }
            }
        }
        ScanLine acc = pageLines[grp[0]];
        pageLines[grp[0]].text = nullptr;
        for (int g = 1; g < grp.Size(); g++) {
            ScanLine& add = pageLines[grp[g]];
            if (IsLoneListBulletText(acc.text) && add.text && ParseHeadingMarker(add.text).rank >= 1 &&
                (acc.dx < 8 || acc.dx <= add.dy * 0.55f || acc.dy <= add.dy * 0.7f)) {
                str::Free(acc.text);
                acc = add;
                add.text = nullptr;
                continue;
            }
            float gap = add.x - (acc.x + acc.dx);
            bool numTitle = LooksLikeTocNumToken(acc.text) && add.text && HasLetterOrCjk(add.text);
            float lim = numTitle ? numGap : maxGap;
            bool bothHeadings = HeadingLevelFromText(acc.text) > 0 && HeadingLevelFromText(add.text) > 0;
            bool addHeading = HeadingLevelFromText(add.text) > 0;
            bool accHeading = HeadingLevelFromText(acc.text) > 0;
            bool bodyStart = LooksLikeBodyContinuationStart(add.text);
            bool underHeading = accHeading && gap < 0 && !numTitle;
            bool doJoin = gap <= lim && !bothHeadings && (!addHeading || numTitle) && !bodyStart && !underHeading;
            if (doJoin) {
                JoinScanLineFragment(acc, add);
                str::Free(add.text);
                add.text = nullptr;
            } else {
                out.Append(acc);
                acc = add;
                add.text = nullptr;
            }
        }
        out.Append(acc);
    }
    pageLines.Reset();
    for (int i = 0; i < out.Size(); i++) {
        pageLines.Append(out[i]);
    }
}

static bool LooksLikeBodyContinuationStart(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    const char* t = s + i;
    return str::StartsWith(t, "为了") || str::StartsWith(t, "根据") || str::StartsWith(t, "按照") ||
           str::StartsWith(t, "各") || str::StartsWith(t, "坚持") || str::StartsWith(t, "通过") ||
           str::StartsWith(t, "充分") || str::StartsWith(t, "进一步") || str::StartsWith(t, "今后") ||
           str::StartsWith(t, "今") || str::StartsWith(t, "我厅") || str::StartsWith(t, "我局");
}

static bool BodyContinuationAt(const char* s, int len, int i) {
    if (!s || i < 0 || i >= len || !LooksLikeBodyContinuationStart(s + i)) {
        return false;
    }
    if (i <= 0) {
        return true;
    }
    int prevI = i;
    int prev = Utf8CodepointPrev(s, len, prevI);
    if (prev <= 32 || prev == 0x3000 || prev == 0x3002 || prev == 0xFF0C || prev == 0xFF1B) {
        return true;
    }
    // "1.全省人社各业务…" — 各 is part of the heading, not glued body.
    const char* t = s + i;
    if (str::StartsWith(t, "各") || str::StartsWith(t, "今")) {
        return false;
    }
    return true;
}

// "四、下一步工作计划 今后" / "计划今后"：标题后面粘上的正文开头。
static void StripGluedBodyAfterHeading(char* s) {
    if (!s || !s[0]) {
        return;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.rank < 1 || m.prefixLength < 1) {
        return;
    }
    int len = (int)str::Len(s);
    int i = m.prefixLength;
    SkipSpacesUtf8(s, len, i);
    int body0 = i;
    while (i < len) {
        if (GlyphCountRange(s, body0, i) >= 4 && BodyContinuationAt(s, len, i)) {
            s[i] = 0;
            str::TrimWSInPlace(s, str::TrimOpt::Both);
            return;
        }
        int before = i;
        Utf8CodepointNext(s, len, i);
        if (i <= before) {
            break;
        }
    }
}

static bool LineEndsWithPeriod(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = len;
    int last = Utf8CodepointPrev(s, len, i);
    while (last > 0 && (last <= 32 || last == 0x3000)) {
        last = Utf8CodepointPrev(s, len, i);
    }
    return last == 0x3002;
}

static bool LineEndsLikeBodyContinuation(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = len;
    int last = Utf8CodepointPrev(s, len, i);
    while (last > 0 && (last <= 32 || last == 0x3000)) {
        last = Utf8CodepointPrev(s, len, i);
    }
    if (last == 0x3002 || last == 0xFF1B || last == ';') {
        return false;
    }
    return last >= 0x4E00 && last <= 0x9FFF;
}

// "…三口合" + "一、电子证照…" — 折行把「三口合一」拆成假「一、」标题。
static bool ShouldMergeFalseYiDunhaoWrap(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!str::StartsWith(b.text, "一、")) {
        return false;
    }
    HeadingMarker mb = ParseHeadingMarker(b.text);
    if (mb.type != MarkerType::ChineseDunhao || mb.number != 1 || mb.prefixLength < 1) {
        return false;
    }
    HeadingMarker ma = ParseHeadingMarker(a.text);
    if (ma.type == MarkerType::ChineseDunhao && ma.rank <= 2) {
        return false;
    }
    bool listOrPara = (ma.type == MarkerType::ArabicDot && ma.rank >= 3) || ma.rank < 1 ||
                      (ma.type == MarkerType::ArabicDot && GlyphCount(a.text) >= 8);
    if (!listOrPara || !LineEndsLikeBodyContinuation(a.text)) {
        return false;
    }
    if (!HasLetterOrCjk(b.text) || LineLooksLikePageNumber(b.text)) {
        return false;
    }
    float gap = b.y - (a.y + a.dy);
    float lim = a.dy > 6 ? a.dy * 1.55f : 14;
    if (gap < -a.dy * 0.35f || gap > lim) {
        return false;
    }
    float dx = a.x - b.x;
    if (dx < 0) {
        dx = -dx;
    }
    return dx <= 48;
}

static bool ShouldMergeHeadingLines(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    int ga = GlyphCount(a.text);
    int gb = GlyphCount(b.text);
    if (ga < 1 || ga > 36 || gb < 2 || gb > 40) {
        return false;
    }
    if (HeadingLevelFromText(a.text) < 1 || HeadingLevelFromText(b.text) > 0) {
        return false;
    }
    HeadingMarker ma = ParseHeadingMarker(a.text);
    if (ma.rank >= 1 && ma.prefixLength > 0) {
        int rest = GlyphCount(a.text + ma.prefixLength);
        // Wrapped titles are short fragments ("体系建设"). A complete heading
        // plus another title ("关于选择确定采购方式") is a sequence gap, not a wrap.
        if (rest >= 4 && gb > 5) {
            return false;
        }
    }
    if (!HasLetterOrCjk(b.text) || LineLooksLikePageNumber(b.text)) {
        return false;
    }
    if (LooksLikeBodyContinuationStart(b.text) || LineEndsWithPeriod(b.text)) {
        return false;
    }
    float gap = b.y - (a.y + a.dy);
    float lim = a.dy > 6 ? a.dy * 1.35f : 10;
    if (gap < -a.dy * 0.25f || gap > lim) {
        return false;
    }
    float dx = a.x - b.x;
    if (dx < 0) {
        dx = -dx;
    }
    float em = a.dy > 6.f ? a.dy : 12.f;
    // 正文首行缩进两字：不是标题折行。
    if (b.x >= a.x + em * 1.2f) {
        return false;
    }
    return dx <= 36;
}

static char* CandidateHeadingText(const Vec<ScanLine>& lines, int i) {
    if (i >= 0 && i + 1 < lines.Size() && ShouldMergeHeadingLines(lines[i], lines[i + 1])) {
        return str::Join(lines[i].text, " ", lines[i + 1].text);
    }
    return str::Dup(lines[i].text);
}

static bool LineInPageHeaderFooterBand(const ScanLine& sl, float yMin, float yMax) {
    float span = yMax - yMin;
    if (span < 8) {
        return false;
    }
    float top = yMin + span * 0.12f;
    float bot = yMax - span * 0.12f;
    return sl.y <= top || sl.y >= bot;
}

static void CollectHeaderTexts(const Vec<ScanLine>& lines, int nPages, StrVec& headers) {
    Vec<float> yMin;
    Vec<float> yMax;
    for (int p = 0; p <= nPages; p++) {
        yMin.Append(1e9f);
        yMax.Append(-1e9f);
    }
    for (int i = 0; i < lines.Size(); i++) {
        int p = lines[i].srcPage;
        if (p < 1 || p > nPages) {
            continue;
        }
        if (lines[i].y < yMin[p]) {
            yMin[p] = lines[i].y;
        }
        float y1 = lines[i].y + lines[i].dy;
        if (y1 > yMax[p]) {
            yMax[p] = y1;
        }
    }
    for (int i = 0; i < lines.Size(); i++) {
        const char* t = lines[i].text;
        if (!t || GlyphCount(t) > 18 || GlyphCount(t) < 2) {
            continue;
        }
        if (headers.Find(t) >= 0) {
            continue;
        }
        int nBand = 0;
        for (int j = 0; j < lines.Size(); j++) {
            if (!lines[j].text || !str::Eq(lines[j].text, t)) {
                continue;
            }
            int p = lines[j].srcPage;
            if (p < 1 || p > nPages) {
                continue;
            }
            if (!LineInPageHeaderFooterBand(lines[j], yMin[p], yMax[p])) {
                continue;
            }
            bool seen = false;
            for (int k = 0; k < j; k++) {
                if (lines[k].srcPage == p && lines[k].text && str::Eq(lines[k].text, t) &&
                    LineInPageHeaderFooterBand(lines[k], yMin[p], yMax[p])) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                nBand++;
            }
        }
        int need = nPages >= 24 ? 4 : 3;
        if (nBand >= need) {
            headers.Append(t);
        }
    }
}

static bool IsHeaderText(StrVec& headers, const char* t) {
    return t && headers.Find(t) >= 0;
}

struct PrintedHit {
    char* title = nullptr;
    int printed = 0;
    int destPage = 0;
    int srcPage = 0;
    float x = 0;
    float y = 0;
    float srcX = 0;
    float srcY = 0;
    float indent = 0;
    float fontSize = 0;
    bool bold = false;
    int level = 1;
};

static void SortPrintedHits(Vec<PrintedHit>& hits);
static char* CleanTocTitle(const char* raw);

static bool HitLooksLikeBookBanner(const PrintedHit& h, float medFont, int nPaged, const PrintedHit* prev) {
    (void)nPaged;
    if (!h.title || !h.title[0] || h.printed > 0) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(h.title);
    ParsedNumbering num;
    ParseHeadingNumbering(h.title, &num);
    if (m.type != MarkerType::None || num.nComp >= 1 || BookTitleIsSpine(h.title)) {
        return false;
    }
    if (prev && BookTitleIsSpine(prev->title) && LineLooksLikeChapterSubtitle(h.title)) {
        return false;
    }
    if (LineLooksLikeBookPartTitle(h.title)) {
        return true;
    }
    if (LineLooksLikeChapterSubtitle(h.title)) {
        return false;
    }
    int g = GlyphCount(h.title);
    if (g < 2 || g > 20) {
        return false;
    }
    bool bigFont = h.fontSize > 1 && medFont > 1 && h.fontSize >= medFont * 1.12f;
    bool fat = h.bold && (h.fontSize < 1 || h.fontSize >= medFont * 0.98f);
    return bigFont || fat;
}

static void ClusterBookIndentBands(const Vec<PrintedHit>& hits, const Vec<int>& skip, Vec<int>& bandOut,
                                   int* nBandsOut) {
    const float kGap = 18.f;
    bandOut.Reset();
    Vec<float> xs;
    for (int i = 0; i < hits.Size(); i++) {
        bandOut.Append(0);
        if (i < skip.Size() && skip[i]) {
            continue;
        }
        xs.Append(hits[i].srcX);
    }
    int nx = xs.Size();
    for (int i = 0; i < nx; i++) {
        for (int j = i + 1; j < nx; j++) {
            if (xs[j] < xs[i]) {
                float t = xs[i];
                xs[i] = xs[j];
                xs[j] = t;
            }
        }
    }
    Vec<float> starts;
    for (int i = 0; i < nx; i++) {
        if (starts.Size() == 0 || xs[i] - starts.Last() > kGap) {
            starts.Append(xs[i]);
        }
    }
    if (nBandsOut) {
        *nBandsOut = starts.Size();
    }
    for (int i = 0; i < hits.Size(); i++) {
        if (i < skip.Size() && skip[i]) {
            continue;
        }
        int b = 0;
        for (int k = starts.Size() - 1; k >= 0; k--) {
            if (hits[i].srcX + 1.f >= starts[k]) {
                b = k;
                break;
            }
        }
        bandOut[i] = b;
    }
}

static void AssignBookPrintedLevels(Vec<PrintedHit>& hits) {
    int n = hits.Size();
    if (n < 1) {
        return;
    }
    Vec<float> fonts;
    int nPaged = 0;
    for (int i = 0; i < n; i++) {
        if (hits[i].fontSize > 1) {
            fonts.Append(hits[i].fontSize);
        }
        if (hits[i].printed > 0) {
            nPaged++;
        }
    }
    float medFont = MedianPositive(fonts);
    if (medFont < 1) {
        medFont = 12;
    }
    Vec<int> isBanner;
    int nBanners = 0;
    for (int i = 0; i < n; i++) {
        int b = HitLooksLikeBookBanner(hits[i], medFont, nPaged, i > 0 ? &hits[i - 1] : nullptr) ? 1 : 0;
        isBanner.Append(b);
        if (b) {
            nBanners++;
        }
    }
    Vec<int> band;
    int nBands = 0;
    ClusterBookIndentBands(hits, isBanner, band, &nBands);
    int lastLevelForKey[90];
    for (int k = 0; k < 90; k++) {
        lastLevelForKey[k] = 0;
    }
    int prevLevel = 1;
    int leftoverLvl = nBanners > 0 ? 2 : 1;
    for (int i = 0; i < n; i++) {
        if (isBanner[i]) {
            hits[i].level = 1;
            prevLevel = 1;
            leftoverLvl = 2;
            for (int k = 0; k < 90; k++) {
                lastLevelForKey[k] = 0;
            }
            continue;
        }
        int numVal = 0;
        int key = BookPrintedSchemeKey(hits[i].title, &numVal);
        ParsedNumbering pn;
        ParseHeadingNumbering(hits[i].title, &pn);
        bool isChap = BookTitleIsSpine(hits[i].title);
        if (isChap) {
            hits[i].level = 1;
            prevLevel = 1;
            leftoverLvl = 2;
            for (int k = 0; k < 90; k++) {
                lastLevelForKey[k] = 0;
            }
            if (key > 0 && key < 90) {
                lastLevelForKey[key] = 1;
            }
            continue;
        }
        int lvl = leftoverLvl;
        if (key > 0 && nBands >= 2) {
            lvl = band[i] + leftoverLvl;
        }
        if (pn.nComp >= 2) {
            int fromNum = pn.nComp + (nBanners > 0 ? 1 : 0);
            if (fromNum > lvl) {
                lvl = fromNum;
            }
        }
        if (isChap && nBands < 2) {
            lvl = nBanners > 0 ? 2 : 1;
        }
        if (key > 0 && key < 90 && lastLevelForKey[key] > 0) {
            lvl = lastLevelForKey[key];
        } else if (key > 0 && i > 0 && !isChap) {
            bool prevChap = BookTitleIsSpine(hits[i - 1].title);
            int prevNum = 0;
            int prevKey = BookPrintedSchemeKey(hits[i - 1].title, &prevNum);
            if (prevChap || isBanner[i - 1] ||
                (nBands < 2 && numVal == 1 && prevKey != key && hits[i - 1].level >= 1)) {
                lvl = hits[i - 1].level + 1;
            }
        }
        if (lvl < 1) {
            lvl = 1;
        }
        if (lvl > prevLevel + 1) {
            lvl = prevLevel + 1;
        }
        if (lvl > 6) {
            lvl = 6;
        }
        hits[i].level = lvl;
        prevLevel = lvl;
        if (key > 0 && key < 90) {
            lastLevelForKey[key] = lvl;
        }
    }
}

static bool HitListHasSpineNumber(const Vec<PrintedHit>& hits, int n) {
    for (int i = 0; i < hits.Size(); i++) {
        if (BookSpineNumber(hits[i].title) == n) {
            return true;
        }
        HeadingMarker m = ParseHeadingMarker(hits[i].title);
        if (m.type == MarkerType::Chapter && m.number == n) {
            return true;
        }
    }
    return false;
}

static int BookSpineNumberFromLine(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::Chapter && m.number > 0) {
        return m.number;
    }
    return BookSpineNumber(s);
}

static void RecoverMissingBookSpines(Vec<PrintedHit>& hits, const Vec<ScanLine>& lines, int tocStart, int nPages) {
    (void)tocStart;
    int nums[32];
    int nNums = 0;
    for (int i = 0; i < hits.Size(); i++) {
        int n = BookSpineNumberFromLine(hits[i].title);
        if (n < 1 || n > 30) {
            continue;
        }
        bool seen = false;
        for (int k = 0; k < nNums; k++) {
            if (nums[k] == n) {
                seen = true;
                break;
            }
        }
        if (!seen && nNums < 32) {
            nums[nNums++] = n;
        }
    }
    for (int i = 0; i < nNums; i++) {
        for (int j = i + 1; j < nNums; j++) {
            if (nums[j] < nums[i]) {
                int t = nums[i];
                nums[i] = nums[j];
                nums[j] = t;
            }
        }
    }
    for (int i = 0; i + 1 < nNums; i++) {
        for (int want = nums[i] + 1; want < nums[i + 1]; want++) {
            if (HitListHasSpineNumber(hits, want)) {
                continue;
            }
            int best = -1;
            for (int k = 0; k < lines.Size(); k++) {
                if (!lines[k].text || lines[k].srcPage < 1 || lines[k].srcPage > nPages) {
                    continue;
                }
                if (BookSpineNumberFromLine(lines[k].text) != want) {
                    continue;
                }
                if (best < 0 || lines[k].srcPage < lines[best].srcPage ||
                    (lines[k].srcPage == lines[best].srcPage && lines[k].y < lines[best].y)) {
                    best = k;
                }
            }
            if (best < 0) {
                continue;
            }
            PrintedHit h;
            h.title = CleanTocTitle(lines[best].text);
            if (!h.title || !h.title[0]) {
                str::Free(h.title);
                continue;
            }
            h.printed = 0;
            h.srcPage = lines[best].srcPage;
            h.srcX = lines[best].x;
            h.srcY = lines[best].y;
            h.indent = lines[best].x;
            h.fontSize = lines[best].fontSize;
            h.bold = lines[best].bold;
            hits.Append(h);
        }
    }
    SortPrintedHits(hits);
}

static void FreePrintedHits(Vec<PrintedHit>& hits) {
    for (int i = 0; i < hits.Size(); i++) {
        str::Free(hits[i].title);
    }
    hits.Reset();
}

static int EnforceMonotonicPrintedDests(Vec<PrintedHit>& hits) {
    int nFix = 0;
    int prev = 0;
    for (int i = 0; i < hits.Size(); i++) {
        int p = hits[i].destPage;
        if (p < 1) {
            if (prev > 0) {
                hits[i].destPage = prev;
                nFix++;
            }
            continue;
        }
        if (prev > 0 && p < prev) {
            hits[i].destPage = prev;
            nFix++;
            p = prev;
        }
        prev = p;
    }
    return nFix;
}

static char* CleanTocTitle(const char* raw) {
    char* s = DupTrimmed(raw);
    if (!s || !s[0]) {
        return s;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    int end = len;
    while (end > i) {
        int prev = end;
        int cp = Utf8CodepointPrev(s, len, prev);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            end = prev;
            continue;
        }
        break;
    }
    if (i > 0 || end < len) {
        int n = end - i;
        if (n < 1) {
            str::Free(s);
            return str::Dup("");
        }
        memmove(s, s + i, (size_t)n);
        s[n] = 0;
    }
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    StripTrailingOcrTitleJunk(s);
    return s;
}

static void SkipHeadingNumbering(const char* s, int len, int& i) {
    if (i >= len || !s) {
        return;
    }
    HeadingMarker m = ParseHeadingMarker(s + i);
    if (m.rank > 0 && m.prefixLength > 0) {
        i += m.prefixLength;
        if (i > len) {
            i = len;
        }
    }
}

static void SkipWsUtf8(const char* s, int len, int& i) {
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 32 && cp != 0x3000) {
            i = save;
            return;
        }
    }
}

// Keep numbering + first clause. Short titles keep a short tail after ，/：;
// 。/； always end the title (drop the mark and anything after, including a glued page number).
static bool IsLatinLetterCp(int cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xFF21 && cp <= 0xFF3A) ||
           (cp >= 0xFF41 && cp <= 0xFF5A);
}

static bool IsOcrPeriodLookalikeCp(int cp) {
    return cp == 'o' || cp == 'O' || cp == 0xFF4F || cp == 0xFF2F ||     // o O ｏ Ｏ
           cp == 0x25CB || cp == 0x3007 || cp == 0x25EF ||               // ○ 〇 ◯
           cp == 0x00B0 || cp == 0x00BA || cp == 0x2218 || cp == 0x25E6; // ° º ∘ ◦
}

static bool IsOcrMisreadPeriodO(const char* s, int len, int oStart, int afterO, int cp) {
    if (!IsOcrPeriodLookalikeCp(cp)) {
        return false;
    }
    bool latinO = cp == 'o' || cp == 'O' || cp == 0xFF4F || cp == 0xFF2F;
    if (!latinO) {
        return true;
    }
    int prevI = oStart;
    int prevCp = Utf8CodepointPrev(s, len, prevI);
    int nextCp = 0;
    if (afterO < len) {
        int t = afterO;
        nextCp = Utf8CodepointNext(s, len, t);
    }
    if (IsLatinLetterCp(prevCp) || IsLatinLetterCp(nextCp)) {
        return false;
    }
    return true;
}

// Trailing OCR speckle after a CJK title: "政策 o" / "工作 c" / "政策○".
static void StripTrailingOcrTitleJunk(char* s) {
    if (!s || !s[0]) {
        return;
    }
    for (int guard = 0; guard < 4; guard++) {
        int len = (int)str::Len(s);
        if (len < 1) {
            return;
        }
        int end = len;
        int cp = Utf8CodepointPrev(s, len, end);
        if (cp <= 32 || cp == 0x3000) {
            s[end] = 0;
            continue;
        }
        if (IsOcrPeriodLookalikeCp(cp) || cp == 0x3002) {
            int prevI = end;
            int prevCp = Utf8CodepointPrev(s, len, prevI);
            if (IsLatinLetterCp(prevCp)) {
                return;
            }
            s[end] = 0;
            str::TrimWSInPlace(s, str::TrimOpt::Both);
            continue;
        }
        if (IsLatinLetterCp(cp)) {
            int prevI = end;
            int prevCp = Utf8CodepointPrev(s, len, prevI);
            if (prevCp >= 0x4E00 && prevCp <= 0x9FFF) {
                s[end] = 0;
                str::TrimWSInPlace(s, str::TrimOpt::Both);
                continue;
            }
        }
        return;
    }
}

static bool IsTitleClauseBreak(const char* s, int len, int colonStart, int afterColon, int cp) {
    if (cp == 0x3002 || cp == 0xFF1B || cp == ';' || cp == 0xFF0C || cp == ',') { // 。 ； ，
        return true;
    }
    if (IsOcrMisreadPeriodO(s, len, colonStart, afterColon, cp)) {
        return true;
    }
    if (cp == 0xFF1A || cp == ':') { // ： :  only with a long body (see TrimTitleToFirstSentence)
        if (afterColon + 1 < len && s[afterColon] == '/' && s[afterColon + 1] == '/') {
            return false;
        }
        int prevI = colonStart;
        int prevCp = Utf8CodepointPrev(s, len, prevI);
        int nextCp = 0;
        if (afterColon < len) {
            int t = afterColon;
            nextCp = Utf8CodepointNext(s, len, t);
        }
        if (IsDigitCp(prevCp) && IsDigitCp(nextCp)) {
            return false;
        }
        return true;
    }
    return false;
}

static void TrimTitleToFirstSentence(char* s) {
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    SkipHeadingNumbering(s, len, i);
    SkipWsUtf8(s, len, i);
    int bodyStart = i;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            return;
        }
        if (!IsTitleClauseBreak(s, len, save, i, cp)) {
            continue;
        }
        if (save <= bodyStart) {
            continue;
        }
        int prefixG = GlyphCountRange(s, bodyStart, save);
        int suffixG = GlyphCountRange(s, i, len);
        bool sentenceEnd = cp == 0x3002 || cp == 0xFF1B || cp == ';' || IsOcrMisreadPeriodO(s, len, save, i, cp);
        int titleG = GlyphCountRange(s, 0, save);
        if (sentenceEnd) {
            if (titleG < 2 || titleG > kExtractPdfToc.headingMaxGlyphs) {
                continue;
            }
        } else if (prefixG < 1 || prefixG > 40) {
            continue;
        }
        bool colon = cp == 0xFF1A || cp == ':';
        bool cut = false;
        const char* reason = "keep";
        if (sentenceEnd) {
            cut = true;
            reason = "sentence-end";
        } else if (suffixG >= 12) {
            cut = true;
            reason = "suffix-long";
        } else if (suffixG == 0 && !colon) {
            cut = true;
            reason = "suffix-empty";
        }
        if (cut) {
            s[save] = 0;
            str::TrimWSInPlace(s, str::TrimOpt::Both);
            return;
        }
        continue;
    }
}

static void TrimAtNextDiHeading(char* s) {
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    SkipHeadingNumbering(s, len, i);
    SkipWsUtf8(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            return;
        }
        if (cp != 0x7B2C) {
            continue;
        }
        int nNum = 0;
        while (i < len) {
            int t = i;
            int n = Utf8CodepointNext(s, len, i);
            if (IsDigitCp(n) || IsCnNumeral(n)) {
                nNum++;
                continue;
            }
            i = t;
            break;
        }
        int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (nNum > 0 && DepthFromDiUnit(mark) > 0 && save > 0) {
            s[save] = 0;
            str::TrimWSInPlace(s, str::TrimOpt::Both);
            return;
        }
    }
}

static bool IsEmbeddedHeadingBoundaryCp(int cp) {
    return cp <= 32 || cp == 0x3000 || cp == 0x3002 || cp == 0xFF1A || cp == ':' || cp == 0xFF1B || cp == ';' ||
           cp == 0xFF0C || cp == ',';
}

static int FindNextEmbeddedHeading(const char* s) {
    if (!s || !s[0]) {
        return -1;
    }
    HeadingMarker first = ParseHeadingMarker(s);
    int len = (int)str::Len(s);
    int bodyStart = 0;
    int firstRank = 0;
    int i = 0;
    if (first.rank >= 1 && first.prefixLength >= 1) {
        bodyStart = first.prefixLength;
        SkipWsUtf8(s, len, bodyStart);
        i = bodyStart;
        firstRank = first.rank;
    }
    while (i < len) {
        int save = i;
        HeadingMarker m = ParseHeadingMarker(s + save);
        if (m.rank >= 1 && m.number > 0 && m.number < 100 && m.prefixLength > 0 && save > 0) {
            int prevI = save;
            int prev = Utf8CodepointPrev(s, len, prevI);
            bool afterBoundary = IsEmbeddedHeadingBoundaryCp(prev);
            int leadG = GlyphCountRange(s, 0, save);
            int bodyG = firstRank >= 1 ? GlyphCountRange(s, bodyStart, save) : leadG;
            const char* rest = s + save + m.prefixLength;
            int restG = GlyphCount(rest);
            if (HasLetterOrCjk(rest) && restG >= 4) {
                if (firstRank >= 1) {
                    bool spaceBefore = prev <= 32 || prev == 0x3000;
                    if (m.rank > firstRank && bodyG >= 4 && bodyG <= 40 && restG <= 80 && (spaceBefore || bodyG >= 6)) {
                        return save;
                    }
                    // Same-rank 公文 enumerations: "（1）…；（2）…；（3）…"
                    if (m.rank >= 1 && afterBoundary && bodyG >= 2) {
                        return save;
                    }
                } else if (afterBoundary && leadG >= 2) {
                    SkipWsUtf8(s, len, save);
                    return save;
                }
            }
        }
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 0) {
            break;
        }
    }
    return -1;
}

static void TrimAtNextEmbeddedHeading(char* s) {
    int split = FindNextEmbeddedHeading(s);
    if (split < 1 || !s) {
        return;
    }
    s[split] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Both);
}

static void NormalizeTocNumberingParens(char** titleOut) {
    char* s = titleOut ? *titleOut : nullptr;
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    int openStart = i;
    int openCp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (!IsParenOpenCp(openCp)) {
        return;
    }
    int innerStart = i;
    int afterClose = innerStart;
    if (!ConsumeParenNumberingAfterOpen(s, len, afterClose)) {
        return;
    }
    int closeStart = afterClose;
    int closeCp = Utf8CodepointPrev(s, len, closeStart);
    if (!IsParenCloseCp(closeCp)) {
        return;
    }
    if (openCp == 0xFF08 && closeCp == 0xFF09) {
        return;
    }
    int innerLen = closeStart - innerStart;
    if (innerLen < 1) {
        return;
    }
    char buf[512];
    int o = 0;
    if (openStart > 0) {
        if (openStart >= (int)sizeof(buf) - 8) {
            return;
        }
        memcpy(buf, s, (size_t)openStart);
        o = openStart;
    }
    memcpy(buf + o, "\xEF\xBC\x88", 3);
    o += 3;
    if (o + innerLen + 4 >= (int)sizeof(buf)) {
        return;
    }
    memcpy(buf + o, s + innerStart, (size_t)innerLen);
    o += innerLen;
    memcpy(buf + o, "\xEF\xBC\x89", 3);
    o += 3;
    int restLen = len - afterClose;
    if (restLen > 0) {
        if (o + restLen + 1 >= (int)sizeof(buf)) {
            return;
        }
        memcpy(buf + o, s + afterClose, (size_t)restLen);
        o += restLen;
    }
    buf[o] = 0;
    char* neu = str::Dup(buf);
    if (!neu) {
        return;
    }
    str::Free(s);
    *titleOut = neu;
}

static bool IsNumberingDotCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x3002 || cp == 0x2024;
}

static bool IsHeadingNumTokenCp(int cp) {
    return IsDigitCp(cp) || IsCnNumeral(cp);
}

// "1．2．3 标题" / "1。成绩册" / "一．主要任务" → halfwidth '.'
void NormalizeTocNumberingDotsHalfwidth(char** titleOut) {
    char* s = titleOut ? *titleOut : nullptr;
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    int start = i;
    if (start >= len) {
        return;
    }
    int peek = start;
    int first = Utf8CodepointNext(s, len, peek);
    if (!IsHeadingNumTokenCp(first)) {
        return;
    }
    char buf[512];
    if (start >= (int)sizeof(buf) - 8) {
        return;
    }
    int o = 0;
    if (start > 0) {
        memcpy(buf, s, (size_t)start);
        o = start;
    }
    bool sawNum = false;
    bool changed = false;
    int k = start;
    while (k < len) {
        if (o >= (int)sizeof(buf) - 8) {
            return;
        }
        int save = k;
        int cp = Utf8CodepointNext(s, len, k);
        if (cp <= 0) {
            break;
        }
        if (IsHeadingNumTokenCp(cp)) {
            int n = k - save;
            if (o + n >= (int)sizeof(buf) - 1) {
                return;
            }
            memcpy(buf + o, s + save, (size_t)n);
            o += n;
            sawNum = true;
            continue;
        }
        if (sawNum && IsNumberingDotCp(cp)) {
            buf[o++] = '.';
            if (cp != '.') {
                changed = true;
            }
            continue;
        }
        int rest = len - save;
        if (o + rest + 1 >= (int)sizeof(buf)) {
            return;
        }
        memcpy(buf + o, s + save, (size_t)rest);
        o += rest;
        break;
    }
    buf[o] = 0;
    if (!changed) {
        return;
    }
    char* neu = str::Dup(buf);
    if (!neu) {
        return;
    }
    str::Free(s);
    *titleOut = neu;
}

static void NormalizeTocNumberingDots(char** titleOut) {
    NormalizeTocNumberingDotsHalfwidth(titleOut);
}

static void StripNumberingTitleSpace(char** titleOut) {
    char* s = titleOut ? *titleOut : nullptr;
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    int digStart = i;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!IsDigitCp(cp)) {
            i = save;
            break;
        }
    }
    int afterDig = i;
    int afterWs = afterDig;
    SkipWsUtf8(s, len, afterWs);
    if (afterDig > digStart && afterWs > afterDig) {
        int t = afterWs;
        int mark = t < len ? Utf8CodepointNext(s, len, t) : 0;
        if (IsSimpleArabicListSepCp(mark)) {
            memmove(s + afterDig, s + afterWs, (size_t)(len - afterWs + 1));
            len = (int)str::Len(s);
        }
    }
    i = 0;
    SkipWsUtf8(s, len, i);
    SkipHeadingNumbering(s, len, i);
    int prefixEnd = i;
    if (prefixEnd < 1) {
        return;
    }
    int prev = prefixEnd;
    int last = Utf8CodepointPrev(s, len, prev);
    if (!IsSimpleArabicListSepCp(last)) {
        return;
    }
    int body = prefixEnd;
    SkipWsUtf8(s, len, body);
    if (body <= prefixEnd) {
        return;
    }
    memmove(s + prefixEnd, s + body, (size_t)(len - body + 1));
}

static bool LooksLikeStandaloneNumbering(const char* s) {
    if (!s || HeadingLevelFromText(s) < 1) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 1 || g > 8) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    SkipHeadingNumbering(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    return i >= len;
}

static char* TitleWithLeftNumbering(const Vec<ScanLine>& lines, const Vec<int>& idx, Vec<int>& used, int titleSlot,
                                    int* attachedOut) {
    if (attachedOut) {
        *attachedOut = 0;
    }
    char* cleaned = CleanTocTitle(lines[idx[titleSlot]].text);
    const ScanLine& title = lines[idx[titleSlot]];
    if (HeadingLevelFromText(cleaned) > 0) {
        return cleaned;
    }
    int best = -1;
    float bestX = -1e9f;
    for (int t = 0; t < idx.Size(); t++) {
        if (used[t] || t == titleSlot) {
            continue;
        }
        const ScanLine& n = lines[idx[t]];
        if (!LooksLikeStandaloneNumbering(n.text) || LineLooksLikePageNumber(n.text)) {
            continue;
        }
        if (!SamePrintedTocRow(n, title)) {
            continue;
        }
        if (n.x >= title.x - 2) {
            continue;
        }
        if (n.x > bestX) {
            bestX = n.x;
            best = t;
        }
    }
    if (best < 0) {
        return cleaned;
    }
    used[best] = 1;
    if (attachedOut) {
        *attachedOut = 1;
    }
    char* num = CleanTocTitle(lines[idx[best]].text);
    bool tight = false;
    if (num && num[0] && cleaned && cleaned[0]) {
        int nlen = (int)str::Len(num);
        int ni = nlen;
        int last = Utf8CodepointPrev(num, nlen, ni);
        int ci = 0;
        int clen = (int)str::Len(cleaned);
        int first = Utf8CodepointNext(cleaned, clen, ci);
        if ((last == '.' || last == 0xFF0E || IsDigitCp(last)) && IsDigitCp(first)) {
            tight = true;
        }
    }
    char* joined = tight ? str::Join(num, cleaned) : str::Join(num, " ", cleaned);
    str::Free(num);
    str::Free(cleaned);
    return joined;
}

static void AppendPairedTocHit(Vec<PrintedHit>& hits, EngineBase* engine, const Vec<char*>& labels,
                               const Vec<ScanLine>& lines, const Vec<int>& idx, Vec<int>& used, int titleSlot,
                               int printed, int* nSecAttach) {
    int dest = MapPrintedPage(engine, printed, labels);
    int attached = 0;
    PrintedHit h;
    h.title = TitleWithLeftNumbering(lines, idx, used, titleSlot, &attached);
    h.printed = printed;
    h.destPage = dest;
    h.srcPage = lines[idx[titleSlot]].srcPage;
    h.srcX = lines[idx[titleSlot]].x;
    h.srcY = lines[idx[titleSlot]].y;
    h.indent = lines[idx[titleSlot]].x;
    h.fontSize = lines[idx[titleSlot]].fontSize;
    h.bold = lines[idx[titleSlot]].bold;
    hits.Append(h);
    if (attached && nSecAttach) {
        (*nSecAttach)++;
    }
}

static void NormalizeTitle(const char* s, char* dst, int cap, bool stripNum) {
    dst[0] = 0;
    if (!s || cap < 8) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    if (stripNum) {
        SkipHeadingNumbering(s, len, i);
        while (i < len) {
            int save = i;
            int cp = Utf8CodepointNext(s, len, i);
            if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
                continue;
            }
            i = save;
            break;
        }
    }
    char* out = dst;
    char* end = dst + cap - 2;
    while (i < len && out + 4 < end) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || IsLeaderCp(cp)) {
            continue;
        }
        if (cp >= 'A' && cp <= 'Z') {
            cp += 32;
        }
        if (cp >= 0xFF10 && cp <= 0xFF19) {
            cp = '0' + (cp - 0xFF10);
        }
        if (cp >= 0xFF21 && cp <= 0xFF3A) {
            cp = 'a' + (cp - 0xFF21);
        }
        if (cp >= 0xFF41 && cp <= 0xFF5A) {
            cp = 'a' + (cp - 0xFF41);
        }
        str::Utf8Encode(out, cp);
    }
    *out = 0;
}

static int TitleMatchScore(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) {
        return 0;
    }
    if (str::Eq(a, b)) {
        return 100;
    }
    int ga = GlyphCount(a);
    int gb = GlyphCount(b);
    int mn = ga < gb ? ga : gb;
    if (mn < kExtractPdfToc.matchContainMinGlyphs) {
        return 0;
    }
    if (str::Find(a, b) || str::Find(b, a)) {
        return 80;
    }
    return 0;
}

static void CompleteTruncatedBookQuotes(const Vec<ScanLine>& lines, Vec<PrintedHit>& hits) {
    for (int i = 0; i < hits.Size(); i++) {
        char* t = hits[i].title;
        if (!t || !t[0]) {
            continue;
        }
        const char* open = str::Find(t, "《");
        if (!open || str::Find(t, "》")) {
            continue;
        }
        char inside[96];
        NormalizeTitle(open + 3, inside, (int)sizeof(inside), false);
        if (!inside[0] || GlyphCount(inside) < 2) {
            continue;
        }
        int dest = hits[i].destPage;
        for (int k = 0; k < lines.Size(); k++) {
            if (!lines[k].text || lines[k].srcPage == hits[i].srcPage) {
                continue;
            }
            if (dest > 0 && lines[k].srcPage != dest && lines[k].srcPage != dest + 1) {
                continue;
            }
            const char* q = str::Find(lines[k].text, "《");
            const char* close = str::Find(lines[k].text, "》");
            if (!q || !close || close <= q + 3) {
                continue;
            }
            int qlen = (int)(close - (q + 3));
            if (qlen < 2 || qlen >= 90) {
                continue;
            }
            char quoted[96];
            memcpy(quoted, q + 3, (size_t)qlen);
            quoted[qlen] = 0;
            char quotedN[96];
            NormalizeTitle(quoted, quotedN, (int)sizeof(quotedN), false);
            if (!quotedN[0] || !str::StartsWith(quotedN, inside)) {
                continue;
            }
            int gq = GlyphCount(quotedN);
            int gi = GlyphCount(inside);
            if (gq > 24 || gq <= gi) {
                continue;
            }
            int prefixLen = (int)(open - t);
            char prefix[400];
            if (prefixLen < 0 || prefixLen >= (int)sizeof(prefix)) {
                continue;
            }
            memcpy(prefix, t, (size_t)prefixLen);
            prefix[prefixLen] = 0;
            char* wrapped = str::Join("《", quotedN, "》");
            if (!wrapped) {
                continue;
            }
            char* neu = str::Join(prefix, wrapped);
            str::Free(wrapped);
            if (!neu) {
                continue;
            }
            str::Free(hits[i].title);
            hits[i].title = neu;
            PolishPrintedHitTitle(&hits[i].title);
            break;
        }
    }
}

static int MedianInt(Vec<int>& v) {
    int n = v.Size();
    if (n < 1) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (v[j] < v[i]) {
                int t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    return v[n / 2];
}

static void SortPrintedHits(Vec<PrintedHit>& hits) {
    for (int i = 0; i < hits.Size(); i++) {
        for (int j = i + 1; j < hits.Size(); j++) {
            bool less = false;
            if (hits[j].srcPage != hits[i].srcPage) {
                less = hits[j].srcPage < hits[i].srcPage;
            } else if (hits[j].srcY + 0.5f < hits[i].srcY) {
                less = true;
            } else if (hits[j].srcY <= hits[i].srcY + 0.5f && hits[j].srcX < hits[i].srcX) {
                less = true;
            }
            if (less) {
                PrintedHit t = hits[i];
                hits[i] = hits[j];
                hits[j] = t;
            }
        }
    }
}

static bool ParseLeadingSectionInt(const char* s, int* leadOut, const char** restOut) {
    *leadOut = 0;
    *restOut = s;
    if (!s) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int cp = Utf8CodepointNext(s, len, i);
    while (cp > 0 && (cp <= 32 || cp == 0x3000)) {
        cp = Utf8CodepointNext(s, len, i);
    }
    if (!IsDigitCp(cp)) {
        return false;
    }
    int v = DigitValue(cp);
    int n = 1;
    while (i < len) {
        int save = i;
        int next = Utf8CodepointNext(s, len, i);
        if (!IsDigitCp(next)) {
            i = save;
            break;
        }
        v = v * 10 + DigitValue(next);
        n++;
        if (n > 3) {
            return false;
        }
    }
    int saveMark = i;
    int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (mark == '.' || mark == 0xFF0E || mark == 0x3002) {
        return false;
    }
    i = saveMark;
    *leadOut = v;
    while (i < len) {
        int save = i;
        int next = Utf8CodepointNext(s, len, i);
        if (next <= 32 || next == 0x3000) {
            continue;
        }
        i = save;
        break;
    }
    *restOut = s + i;
    return v >= 1 && HasLetterOrCjk(*restOut);
}

static bool PrintedTitleHasExplicitListNumber(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (StartsWithXinDeHeading(s)) {
        return true;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::ArabicDot || m.type == MarkerType::ChineseDunhao || m.type == MarkerType::ChineseParen ||
        m.type == MarkerType::ArabicParen || m.type == MarkerType::YiShi) {
        return true;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    return num.nComp >= 1 && num.hadTrailingDot;
}

static void RepairPrintedHitTitles(Vec<PrintedHit>& hits) {
    int chap = 0;
    int lastSec = 0;
    int nRepairLog = 0;
    for (int i = 0; i < hits.Size(); i++) {
        int lvl = HeadingLevelFromText(hits[i].title);
        int len = hits[i].title ? (int)str::Len(hits[i].title) : 0;
        int ti = 0;
        int first = hits[i].title ? Utf8CodepointNext(hits[i].title, len, ti) : 0;
        if (first == 0x7B2C) {
            chap = 0;
            lastSec = 0;
            SkipSpacesUtf8(hits[i].title, len, ti);
            int n = 0;
            while (ti < len) {
                int save = ti;
                int next = Utf8CodepointNext(hits[i].title, len, ti);
                if (IsDigitCp(next)) {
                    n = n * 10 + DigitValue(next);
                    continue;
                }
                if (IsCnNumeral(next)) {
                    continue;
                }
                ti = save;
                break;
            }
            if (n > 0) {
                chap = n;
            }
            lastSec = 0;
            continue;
        }
        ParsedNumbering curNum;
        ParseHeadingNumbering(hits[i].title, &curNum);
        if (NumberingIsBareArabicChapter(curNum, hits[i].title)) {
            chap = curNum.comp[0];
            lastSec = 0;
            continue;
        }
        if (lvl >= 2 && IsDigitCp(first)) {
            int a = DigitValue(first);
            while (ti < len) {
                int save = ti;
                int next = Utf8CodepointNext(hits[i].title, len, ti);
                if (IsDigitCp(next)) {
                    a = a * 10 + DigitValue(next);
                    continue;
                }
                ti = save;
                break;
            }
            int mark = ti < len ? Utf8CodepointNext(hits[i].title, len, ti) : 0;
            if (mark == '.' || mark == 0xFF0E) {
                int b = 0;
                int nB = 0;
                while (ti < len) {
                    int save = ti;
                    int next = Utf8CodepointNext(hits[i].title, len, ti);
                    if (IsDigitCp(next)) {
                        b = b * 10 + DigitValue(next);
                        nB++;
                        continue;
                    }
                    ti = save;
                    break;
                }
                if (nB > 0) {
                    chap = a;
                    lastSec = b;
                }
            }
            continue;
        }
        if (PrintedTitleHasExplicitListNumber(hits[i].title)) {
            if (curNum.nComp >= 2) {
                chap = curNum.comp[0];
                lastSec = curNum.comp[1];
            } else if (curNum.nComp == 1) {
                lastSec = curNum.comp[0];
            }
            continue;
        }
        int lead = 0;
        const char* rest = nullptr;
        if (!ParseLeadingSectionInt(hits[i].title, &lead, &rest) || chap < 1) {
            continue;
        }
        bool repair = false;
        if (lastSec > 0 && lead == lastSec + 1) {
            repair = true;
        }
        if (lastSec == 0 && lead == 1) {
            repair = true;
        }
        if (i + 1 < hits.Size()) {
            int nlvl = HeadingLevelFromText(hits[i + 1].title);
            int nlen = hits[i + 1].title ? (int)str::Len(hits[i + 1].title) : 0;
            int nj = 0;
            int nf = hits[i + 1].title ? Utf8CodepointNext(hits[i + 1].title, nlen, nj) : 0;
            if (nlvl >= 2 && IsDigitCp(nf)) {
                int na = DigitValue(nf);
                while (nj < nlen) {
                    int save = nj;
                    int next = Utf8CodepointNext(hits[i + 1].title, nlen, nj);
                    if (IsDigitCp(next)) {
                        na = na * 10 + DigitValue(next);
                        continue;
                    }
                    nj = save;
                    break;
                }
                int mark = nj < nlen ? Utf8CodepointNext(hits[i + 1].title, nlen, nj) : 0;
                int nb = 0;
                if (mark == '.' || mark == 0xFF0E) {
                    while (nj < nlen) {
                        int save = nj;
                        int next = Utf8CodepointNext(hits[i + 1].title, nlen, nj);
                        if (IsDigitCp(next)) {
                            nb = nb * 10 + DigitValue(next);
                            continue;
                        }
                        nj = save;
                        break;
                    }
                    if (na == chap && nb == lead + 1) {
                        repair = true;
                    }
                }
            }
        }
        if (!repair || !rest) {
            continue;
        }
        char* old = hits[i].title;
        char* neu = str::Format("%d.%d %s", chap, lead, rest);
        str::Free(old);
        hits[i].title = neu;
        lastSec = lead;
    }
    for (int i = 0; i < hits.Size(); i++) {
        PolishPrintedHitTitle(&hits[i].title);
    }
}

static void PrefixSectionNumber(PrintedHit& h, int chap, int sec) {
    ParsedNumbering have;
    ParseHeadingNumbering(h.title, &have);
    if (!h.title || have.nComp >= 1 || have.isChapter || NumberingIsBareArabicChapter(have, h.title) ||
        PrintedTitleHasExplicitListNumber(h.title)) {
        return;
    }
    char* neu = str::Format("%d.%d %s", chap, sec, h.title);
    str::Free(h.title);
    h.title = neu;
}

static void StripFalseSectionPrefixOnChapter(char** titleOut) {
    char* s = *titleOut;
    if (!s || !s[0]) {
        return;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (num.isChapter || num.nComp < 2) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(cp) || cp == '.' || cp == 0xFF0E || cp <= 32 || cp == 0x3000) {
            continue;
        }
        i = save;
        break;
    }
    if (i <= 0 || i >= len) {
        return;
    }
    ParsedNumbering rest;
    ParseHeadingNumbering(s + i, &rest);
    if (!rest.isChapter && !NumberingIsBareArabicChapter(rest, s + i)) {
        return;
    }
    char* neu = str::Dup(s + i);
    str::Free(s);
    *titleOut = neu;
}

static void PrefixBareChapterNumber(PrintedHit& h, int chap) {
    if (!h.title || chap < 1) {
        return;
    }
    ParsedNumbering have;
    ParseHeadingNumbering(h.title, &have);
    if (have.isChapter || NumberingIsBareArabicChapter(have, h.title) || have.nComp >= 2) {
        return;
    }
    char* neu = str::Format("%d %s", chap, h.title);
    str::Free(h.title);
    h.title = neu;
}

static void RepairMissingSectionNumbers(Vec<PrintedHit>& hits) {
    int chap = 0;
    int lastSec = 0;
    Vec<int> pending;
    int nFill = 0;
    for (int i = 0; i < hits.Size(); i++) {
        ParsedNumbering num;
        ParseHeadingNumbering(hits[i].title, &num);
        if (num.isChapter || NumberingIsBareArabicChapter(num, hits[i].title)) {
            if (pending.Size() > 0 && chap > 0) {
                for (int p = 0; p < pending.Size(); p++) {
                    PrefixSectionNumber(hits[pending[p]], chap, lastSec + 1 + p);
                    nFill++;
                }
            }
            pending.Reset();
            chap = num.comp[0];
            lastSec = 0;
            continue;
        }
        if (num.nComp >= 3 && num.comp[0] > 0) {
            int n = pending.Size();
            if (n > 0) {
                PrefixSectionNumber(hits[pending[n - 1]], num.comp[0], num.comp[1]);
                nFill++;
                for (int p = 0; p < n - 1; p++) {
                    PrefixSectionNumber(hits[pending[p]], num.comp[0], p + 1);
                    nFill++;
                }
                pending.Reset();
            }
            chap = num.comp[0];
            lastSec = num.comp[1];
            continue;
        }
        if (num.nComp == 2 && num.comp[0] > 0) {
            if (num.comp[1] == 1 && pending.Size() == 1) {
                PrefixBareChapterNumber(hits[pending[0]], num.comp[0]);
                pending.Reset();
            } else if (num.comp[1] == 1 && pending.Size() == 0 && i > 0) {
                ParsedNumbering prev;
                ParseHeadingNumbering(hits[i - 1].title, &prev);
                if (prev.nComp < 1 && !prev.isChapter && HeadingLevelFromText(hits[i - 1].title) < 1) {
                    PrefixBareChapterNumber(hits[i - 1], num.comp[0]);
                }
            }
            int n = pending.Size();
            int start = num.comp[1] - n;
            if (start < 1) {
                start = 1;
            }
            for (int p = 0; p < n; p++) {
                PrefixSectionNumber(hits[pending[p]], num.comp[0], start + p);
                nFill++;
            }
            pending.Reset();
            chap = num.comp[0];
            lastSec = num.comp[1];
            continue;
        }
        if (chap > 0 && num.nComp < 1 && !PrintedTitleHasExplicitListNumber(hits[i].title)) {
            pending.Append(i);
        }
    }
    if (pending.Size() > 0 && chap > 0) {
        for (int p = 0; p < pending.Size(); p++) {
            PrefixSectionNumber(hits[pending[p]], chap, lastSec + 1 + p);
            nFill++;
        }
    }
    for (int i = 0; i < hits.Size(); i++) {
        StripFalseSectionPrefixOnChapter(&hits[i].title);
    }
}

static PrintedHit MakeTestHit(const char* title, int srcPage, float srcY) {
    PrintedHit h;
    h.title = str::Dup(title);
    h.srcPage = srcPage;
    h.srcY = srcY;
    h.srcX = 40;
    h.fontSize = 12;
    return h;
}

static PrintedHit MakeBookHit(const char* title, int printed, float srcX, float srcY, float fontSize) {
    PrintedHit h;
    h.title = str::Dup(title);
    h.printed = printed;
    h.srcPage = 1;
    h.srcX = srcX;
    h.srcY = srcY;
    h.indent = srcX;
    h.fontSize = fontSize;
    h.bold = printed < 1 && fontSize >= 14;
    return h;
}

static void BookHitsToTree(Vec<PrintedHit>& hits, Vec<ExtractedTocItem*>& roots) {
    AssignBookPrintedLevels(hits);
    Vec<ExtractedTocItem*> flat;
    for (int i = 0; i < hits.Size(); i++) {
        int page = hits[i].destPage > 0 ? hits[i].destPage : hits[i].printed;
        flat.Append(NewItem(hits[i].title, page, 0, 0, hits[i].level));
    }
    BuildTreeFromFlat(flat, roots);
}

static bool TocTreeHasTitle(const Vec<ExtractedTocItem*>& nodes, const char* t) {
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i]->title && str::Eq(nodes[i]->title, t)) {
            return true;
        }
        if (TocTreeHasTitle(nodes[i]->children, t)) {
            return true;
        }
    }
    return false;
}

static ExtractedTocItem* TocTreeFind(const Vec<ExtractedTocItem*>& nodes, const char* t) {
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i]->title && str::Eq(nodes[i]->title, t)) {
            return nodes[i];
        }
        ExtractedTocItem* c = TocTreeFind(nodes[i]->children, t);
        if (c) {
            return c;
        }
    }
    return nullptr;
}

static void RunPrintedTocLogicTestsPhase2(int* pass, int* fail, int* failMask);
static void InsertOfficialDocTitles(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots);
static ExtractTocDocClass ClassifyExtractTocDoc(const Vec<ScanLine>& lines, int nPages, const char* filePath);
static bool ExtractOfficialToc(EngineBase* engine, Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                               Vec<ExtractedTocItem*>& roots, const char* tocDebugPath);
static bool ExtractContractToc(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots);
static bool ExtractPaperToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                            Vec<ExtractedTocItem*>& roots);
static bool ExtractBookToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                           Vec<ExtractedTocItem*>& roots, bool bornDigital = false);
static void MergeDeeperArabicHeadings(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots);

static void RunPrintedTocLogicTests() {
    int pass = 0;
    int fail = 0;
    int failMask = 0;
    {
        char* title = nullptr;
        int page = 0;
        bool ok = ParsePrintedTocLine("第一章 项目概述..................................................... 1", &title,
                                      &page);
        if (ok && page == 1 && title && str::Eq(title, "第一章 项目概述")) {
            pass++;
        } else {
            fail++;
            failMask |= 1;
        }
        str::Free(title);
    }
    {
        char* t = str::Dup("1．2．3 标题");
        NormalizeTocNumberingDotsHalfwidth(&t);
        bool ok = t && str::Eq(t, "1.2.3 标题");
        str::Free(t);
        t = str::Dup("1。成绩册");
        NormalizeTocNumberingDotsHalfwidth(&t);
        ok = ok && t && str::Eq(t, "1.成绩册");
        str::Free(t);
        t = str::Dup("一．主要任务");
        NormalizeTocNumberingDotsHalfwidth(&t);
        ok = ok && t && str::Eq(t, "一.主要任务");
        str::Free(t);
        t = str::Dup("1.2.3 already");
        NormalizeTocNumberingDotsHalfwidth(&t);
        ok = ok && t && str::Eq(t, "1.2.3 already");
        str::Free(t);
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 1;
            logf("phase1 fail numbering-dots-halfwidth\n");
        }
    }
    {
        char* title = nullptr;
        int page = 0;
        bool ok = ParsePrintedTocLine("心得一 让独特的学习方法成为孩子成功的捷径 /3", &title, &page);
        if (ok && page == 3 && title && str::Find(title, "心得一") && !str::Find(title, "/")) {
            pass++;
        } else {
            fail++;
            failMask |= 1;
        }
        str::Free(title);
    }
    {
        char* title = nullptr;
        int page = 0;
        bool ok = ParsePrintedTocLine("The Summer Olympic Games . . . . . . 4", &title, &page);
        bool head =
            LooksLikeEnglishTocHeading("Table of Contents") && LooksLikePrintedTocHeading("Table of Contents") &&
            !LooksLikeEnglishTocHeading("The Summer Olympic Games") && !LooksLikePrintedTocHeading("The Olympic Torch");
        if (ok && page == 4 && title && str::Eq(title, "The Summer Olympic Games") && head) {
            pass++;
        } else {
            fail++;
            failMask |= 1;
            logf("phase1 fail english-toc-line\n");
        }
        str::Free(title);
    }
    if (!LineLooksLikePageNumber("3.") && !LineLooksLikePageNumber("3.4") && LineLooksLikePageNumber("3") &&
        LineLooksLikePageNumber("27") && LineLooksLikePageNumber("....41") && LineLooksLikePageNumber(".·17") &&
        LineLooksLikePageNumber("···25") && LineLooksLikePageNumber("·49") && ParseBarePrintedPage(".·17") == 17 &&
        ParseBarePrintedPage("....41") == 41 && ParseBarePrintedPage("3.4") == 0 && LineLooksLikePageNumber("(2)") &&
        ParseBarePrintedPage("(2)") == 2 && ParseBarePrintedPage("（13）") == 13) {
        pass++;
    } else {
        fail++;
        failMask |= 2;
    }
    if (LooksLikeArchiveJunk("annas_archive_metadata.json.zst") && LooksLikeArchiveJunk("original_files.tar.zst") &&
        LooksLikeArchiveJunk("扫描全能王 创建") && LooksLikeArchiveJunk("CamScanner") &&
        !LooksLikeArchiveJunk("3.4 面上无光")) {
        pass++;
    } else {
        fail++;
        failMask |= 4;
    }
    if (LevelFromPrintedTitle("第1章 智力加油站") == 1 && LevelFromPrintedTitle("1.1 有记性的数") == 2 &&
        LevelFromPrintedTitle("3.4 面上无光") == 2 && LevelFromPrintedTitle("总序") == 1 &&
        LevelFromPrintedTitle("前言") == 1 && LevelFromPrintedTitle("第 2 章 相关理论基础和技术路线") == 1 &&
        HeadingLevelFromText("第 2 章 相关理论基础和技术路线") == 1 && HeadingLevelFromText("第一课热爱祖国") == 1 &&
        HeadingLevelFromText("第一讲 绪论") == 1 && HeadingLevelFromText("第六课社会主义民主建设") == 1 &&
        HeadingLevelFromText("序章对高考的错误认识") == 1 && HeadingLevelFromText("心得一 让独特的学习方法") == 1 &&
        HeadingLevelFromText("第二童 被忽视了的高考父亲学") == 1 &&
        HeadingLevelFromText("末章资料：当代高中生的实态") == 1 && LooksLikePrintedTocHeading("日录") &&
        LooksLikePrintedTocHeading("目录")) {
        pass++;
    } else {
        fail++;
        failMask |= 8;
    }
    {
        Vec<PrintedHit> hits;
        const char* t1[] = {
            "总序",         "前言",          "第1章 智力加油站", "1.1 有记性的数", "1.2 推陈出新，增加调料",
            "1.3 无视直觉", "1.4 试两次就行"};
        for (int i = 0; i < (int)dimof(t1); i++) {
            hits.Append(MakeTestHit(t1[i], 1, (float)(i + 1) * 12));
        }
        SortPrintedHits(hits);
        RepairPrintedHitTitles(hits);
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        for (int i = 0; i < hits.Size(); i++) {
            flat.Append(NewItem(hits[i].title, i + 1, 0, 0, LevelFromPrintedTitle(hits[i].title)));
        }
        BuildTreeFromFlat(flat, roots);
        bool ok = roots.Size() == 3 && str::Eq(roots[0]->title, "总序") && str::Eq(roots[1]->title, "前言") &&
                  str::Eq(roots[2]->title, "第1章 智力加油站") && roots[2]->children.Size() == 4 &&
                  str::Eq(roots[2]->children[0]->title, "1.1 有记性的数") &&
                  str::Eq(roots[2]->children[2]->title, "1.3 无视直觉");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 16;
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        const char* t2[] = {"2.9 穷小子妙算惊四座", "2.10 菩提明镜",    "2.11 梁羽生与数学",
                            "第3章 数字照妖镜",     "3.1 金箍棒当钉耙", "3.2 大款上当",
                            "3.3 数字照妖镜",       "3.4 面上无光",     "3.5 伤心骰子"};
        for (int i = 0; i < (int)dimof(t2); i++) {
            hits.Append(MakeTestHit(t2[i], 1, (float)(i + 1) * 12));
        }
        SortPrintedHits(hits);
        RepairPrintedHitTitles(hits);
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        for (int i = 0; i < hits.Size(); i++) {
            flat.Append(NewItem(hits[i].title, i + 1, 0, 0, LevelFromPrintedTitle(hits[i].title)));
        }
        BuildTreeFromFlat(flat, roots);
        ExtractedTocItem* ch3 = TocTreeFind(roots, "第3章 数字照妖镜");
        bool nested210 = ch3 && TocTreeHasTitle(ch3->children, "2.10 菩提明镜");
        bool orderOk = hits.Size() >= 5 && str::Eq(hits[0].title, "2.9 穷小子妙算惊四座") &&
                       str::Eq(hits[3].title, "第3章 数字照妖镜") && str::Eq(hits[4].title, "3.1 金箍棒当钉耙");
        bool titleOk = str::Eq(hits[7].title, "3.4 面上无光");
        if (ch3 && !nested210 && orderOk && titleOk && ch3->children.Size() >= 1 &&
            str::StartsWith(ch3->children[0]->title, "3.1")) {
            pass++;
        } else {
            fail++;
            failMask |= 32;
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeTestHit("第3章 数字照妖镜", 1, 10));
        hits.Append(MakeTestHit("3.3 数字照妖镜", 1, 20));
        hits.Append(MakeTestHit("4面上无光", 1, 30));
        hits.Append(MakeTestHit("3.5 伤心骰子", 1, 40));
        hits.Append(MakeTestHit("第4章 自然数群英谱", 1, 50));
        hits.Append(MakeTestHit("1续命金丹", 1, 60));
        hits.Append(MakeTestHit("4.2 无0就乱套", 1, 70));
        RepairPrintedHitTitles(hits);
        bool ok = str::Eq(hits[2].title, "3.4 面上无光") && str::Eq(hits[5].title, "4.1 续命金丹");
        const char* keep[] = {"3.4 面上无光",         "4.1 续命金丹",      "5.7 哈雷数",
                              "6.1 西方的“夸父逐日”", "6.20 向物理讨救兵", "7.16 数学与草书"};
        const char* bad[] = {"4面上无光",         "1续命金丹",      "7哈雷数",
                             "1西方的“夸父逐日”", "20向物理讨救兵", "16数学与草书"};
        for (int k = 0; k < (int)dimof(keep); k++) {
            if (LevelFromPrintedTitle(keep[k]) != 2) {
                ok = false;
            }
        }
        for (int k = 0; k < (int)dimof(bad); k++) {
            if (str::Eq(hits[2].title, bad[k]) || str::Eq(hits[5].title, bad[k])) {
                ok = false;
            }
        }
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 64;
        }
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeTestHit("第一章 项目概述", 1, 10));
        hits.Append(MakeTestHit("项目名称", 1, 20));
        hits.Append(MakeTestHit("项目建设单位、项目负责人", 1, 30));
        hits.Append(MakeTestHit("初设及概算编制单位", 1, 40));
        hits.Append(MakeTestHit("初设及概算编制依据", 1, 50));
        hits.Append(MakeTestHit("项目概况", 1, 60));
        hits.Append(MakeTestHit("1.5.1 项目建设地点", 1, 70));
        RepairMissingSectionNumbers(hits);
        bool ok = str::Eq(hits[1].title, "1.1 项目名称") && str::Eq(hits[2].title, "1.2 项目建设单位、项目负责人") &&
                  str::Eq(hits[4].title, "1.4 初设及概算编制依据") && str::Eq(hits[5].title, "1.5 项目概况");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 128;
        }
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeTestHit("第一章 对考试的错误认识", 1, 10));
        hits.Append(MakeTestHit("1．成绩册是孩子学习成绩的准确资料吗？不是！", 1, 20));
        hits.Append(MakeTestHit("2．“偏差值”是公正客观的评价吗？不是！", 1, 30));
        hits.Append(MakeTestHit("第二章 被忽视了的高考父亲学", 1, 40));
        hits.Append(MakeTestHit("1．善解人意的家长反而教育不了孩子", 1, 50));
        RepairMissingSectionNumbers(hits);
        RepairPrintedHitTitles(hits);
        bool keepOk = str::Find(hits[1].title, "1．") && !str::Find(hits[1].title, "1.1") &&
                      str::Find(hits[2].title, "2．") && !str::Find(hits[2].title, "1.2") &&
                      str::Find(hits[4].title, "1．") && !str::Find(hits[4].title, "2.1");
        if (keepOk) {
            pass++;
        } else {
            fail++;
            logf("phase1 official-keep-1-dot still rewrites 1． (Official frozen)\n");
        }
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeTestHit("项目概述", 1, 10));
        hits.Append(MakeTestHit("1.1 项目背景", 1, 20));
        hits.Append(MakeTestHit("1.5 建设工期", 1, 30));
        hits.Append(MakeTestHit("项目现状及需求分析", 1, 40));
        hits.Append(MakeTestHit("2.1 业务现状", 1, 50));
        hits.Append(MakeTestHit("2.2 信息化现状", 1, 60));
        RepairMissingSectionNumbers(hits);
        bool ok = str::Eq(hits[0].title, "1 项目概述") && str::Eq(hits[3].title, "2 项目现状及需求分析") &&
                  str::Eq(hits[4].title, "2.1 业务现状") && LevelFromPrintedTitle(hits[0].title) == 1 &&
                  LevelFromPrintedTitle(hits[3].title) == 1 && LevelFromPrintedTitle("2 项目现状及需求分析") == 1;
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 512;
        }
        FreePrintedHits(hits);
        hits.Reset();
        hits.Append(MakeTestHit("1 项目概述", 1, 10));
        hits.Append(MakeTestHit("1.1 项目背景", 1, 20));
        hits.Append(MakeTestHit("2 项目现状及需求分析", 1, 30));
        hits.Append(MakeTestHit("2.1 业务现状", 1, 40));
        RepairMissingSectionNumbers(hits);
        ok = str::Eq(hits[2].title, "2 项目现状及需求分析") && !str::Find(hits[2].title, "2.1") &&
             LevelFromPrintedTitle(hits[2].title) == 1;
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 512;
        }
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeTestHit("第1章 绪论", 1, 10));
        hits.Append(MakeTestHit("1.1 系统开发背景", 1, 20));
        hits.Append(MakeTestHit("第 2 章 相关理论基础和技术路线", 1, 30));
        hits.Append(MakeTestHit("2.1 B/S 架构", 1, 40));
        hits.Append(MakeTestHit("第 3 章 系统设计", 1, 50));
        hits.Append(MakeTestHit("3.1 信访案件处理工作流程", 1, 60));
        RepairMissingSectionNumbers(hits);
        bool ok = str::Eq(hits[2].title, "第 2 章 相关理论基础和技术路线") &&
                  str::Eq(hits[4].title, "第 3 章 系统设计") && LevelFromPrintedTitle(hits[2].title) == 1 &&
                  LevelFromPrintedTitle(hits[4].title) == 1;
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= (1 << 31);
        }
        FreePrintedHits(hits);
        hits.Reset();
        hits.Append(MakeTestHit("第1章 绪论", 1, 10));
        hits.Append(MakeTestHit("2.1 第 2 章 相关理论基础和技术路线", 1, 30));
        RepairMissingSectionNumbers(hits);
        ok = hits.Size() >= 2 && str::Eq(hits[1].title, "第 2 章 相关理论基础和技术路线");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= (1 << 31);
        }
        FreePrintedHits(hits);
    }
    {
        bool lvlOk = LevelFromPrintedTitle("第一章 总则") == 1 && LevelFromPrintedTitle("第一条 目的") == 2 &&
                     LevelFromPrintedTitle("第二章 安全保护要求") == 1 && LevelFromPrintedTitle("第七条 职责") == 2 &&
                     LevelFromPrintedTitle("（一） 清单") == 2;
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        const char* t3[] = {"第一章 总则", "第一条 目的", "第二条 范围", "第二章 安全保护要求", "第七条 职责"};
        for (int i = 0; i < (int)dimof(t3); i++) {
            flat.Append(NewItem(t3[i], i + 1, 0, 0, LevelFromPrintedTitle(t3[i])));
        }
        BuildTreeFromFlat(flat, roots);
        bool treeOk = roots.Size() == 2 && str::Eq(roots[0]->title, "第一章 总则") && roots[0]->children.Size() == 2 &&
                      str::Eq(roots[1]->title, "第二章 安全保护要求") && roots[1]->children.Size() == 1 &&
                      str::Eq(roots[1]->children[0]->title, "第七条 职责");
        if (lvlOk && treeOk) {
            pass++;
        } else {
            fail++;
            failMask |= 256;
        }
        DeleteExtractedTocItems(roots);
    }
    {
        bool lvlOk = LevelFromPrintedTitle("（十）打造自助服务升级版") == 2 &&
                     LevelFromPrintedTitle("（十一）强化网上中介服务超市管理使用") == 2 &&
                     LevelFromPrintedTitle("(十二) 持续推进") == 2;
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        const char* t4[] = {"三、加快提升线上服务平台功能", "（十）打造自助服务升级版", "四、持续深化改革创新",
                            "（十一）强化网上中介服务超市管理使用", "（十二）持续推进互联网监管"};
        for (int i = 0; i < (int)dimof(t4); i++) {
            flat.Append(NewItem(t4[i], i + 1, 0, 0, LevelFromPrintedTitle(t4[i])));
        }
        BuildTreeFromFlat(flat, roots);
        bool treeOk = roots.Size() == 2 && str::Eq(roots[0]->title, "三、加快提升线上服务平台功能") &&
                      roots[0]->children.Size() == 1 && str::Eq(roots[1]->title, "四、持续深化改革创新") &&
                      roots[1]->children.Size() == 2;
        if (lvlOk && treeOk) {
            pass++;
        } else {
            fail++;
            failMask |= 512;
        }
        DeleteExtractedTocItems(roots);
    }
    {
        ExtractedTocItem* a = NewItem("（一）健全政务服务机构职能。按照建设全国政务服务满意度", 1, 0, 0, 3);
        ExtractedTocItem* b = NewItem("（九）提升“赣政通”功能。打通“赣政通”“赣服通”服务通道，", 1, 0, 0, 3);
        ExtractedTocItem* c = NewItem("3.4 面上无光", 1, 0, 0, 2);
        ExtractedTocItem* d = NewItem("第一章 总则。", 1, 0, 0, 1);
        ExtractedTocItem* e = NewItem("一、建成高效有力服务体系", 1, 0, 0, 2);
        ExtractedTocItem* f = NewItem("1.省八一保育院：6个小班150人（原托班升小班36人，", 1, 0, 0, 3);
        ExtractedTocItem* g = NewItem("1.小班：入园年龄需年满3周岁，年龄计算日期截至2023年8月31日。", 1, 0, 0, 3);
        ExtractedTocItem* h = NewItem("1.制定实施 十五五 数字江西建设规划 o", 1, 0, 0, 3);
        ExtractedTocItem* k = NewItem("4.健全数据流通交易机制 o 落实国家关于建设全国一体化数", 1, 0, 0, 3);
        ExtractedTocItem* secO = NewItem("第一节强化就业优先政策 o", 1, 0, 0, 2);
        ExtractedTocItem* secCircle = NewItem("第一节强化就业优先政策\xE2\x97\x8B", 1, 0, 0, 2); // ○
        ExtractedTocItem* secC = NewItem("第四节 加强农民工服务保障工作 c", 1, 0, 0, 2);
        ExtractedTocItem* m = NewItem("1.Introduction to services", 1, 0, 0, 3);
        ExtractedTocItem* p = NewItem("2. 细化落实国家数据产权制度", 1, 0, 0, 3);
        ExtractedTocItem* q = NewItem("1\xEF\xBC\x8E单位申报", 1, 0, 0, 3);
        bool ok =
            a && str::Eq(a->title, "（一）健全政务服务机构职能") && b && str::Eq(b->title, "（九）提升“赣政通”功能") &&
            c && str::Eq(c->title, "3.4 面上无光") && d && str::Eq(d->title, "第一章 总则") && e &&
            str::Eq(e->title, "一、建成高效有力服务体系") && f && str::Eq(f->title, "1.省八一保育院") && g &&
            str::Eq(g->title, "1.小班") && h && str::Eq(h->title, "1.制定实施 十五五 数字江西建设规划") && k &&
            str::Eq(k->title, "4.健全数据流通交易机制") && secO && str::Eq(secO->title, "第一节强化就业优先政策") &&
            secCircle && str::Eq(secCircle->title, "第一节强化就业优先政策") && secC &&
            str::Eq(secC->title, "第四节 加强农民工服务保障工作") && m &&
            str::Eq(m->title, "1.Introduction to services") && p && str::Eq(p->title, "2.细化落实国家数据产权制度") &&
            q && str::Eq(q->title, "1.单位申报");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 1024;
        }
        delete a;
        delete b;
        delete c;
        delete d;
        delete e;
        delete f;
        delete g;
        delete h;
        delete k;
        delete secO;
        delete secCircle;
        delete secC;
        delete m;
        delete p;
        delete q;
    }
    {
        ExtractedTocItem* a = NewItem("(一)健全政务服务机构职能", 1, 0, 0, 3);
        ExtractedTocItem* b = NewItem("（七)持续推进“赣服通”迭代升级", 1, 0, 0, 3);
        ExtractedTocItem* c = NewItem("(二十八)常态化开展监督曝光", 1, 0, 0, 3);
        ExtractedTocItem* d = NewItem("(十一)强化网上中介服务超市管理使用", 1, 0, 0, 3);
        bool ok = a && str::Eq(a->title, "（一）健全政务服务机构职能") && b &&
                  str::Eq(b->title, "（七）持续推进“赣服通”迭代升级") && c &&
                  str::Eq(c->title, "（二十八）常态化开展监督曝光") && d &&
                  str::Eq(d->title, "（十一）强化网上中介服务超市管理使用");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 2048;
        }
        delete a;
        delete b;
        delete c;
        delete d;
    }
    {
        bool lvlOk = HeadingLevelFromText("一、招生范围") == 1 && HeadingLevelFromText("（一）重点招生范围") == 2 &&
                     HeadingLevelFromText("1.单位申报") == 3 && HeadingLevelFromText("（1）各省直单位") == 4 &&
                     HeadingLevelFromText("(2)业主申报审核") == 4 && HeadingLevelFromText("1.1 有记性的数") == 2 &&
                     HeadingLevelFromText("(144) 不适于做薪金工作者") == 0 &&
                     HeadingLevelFromText("（147）难报父母的恩情") == 0;
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        const char* t5[] = {"一、招生范围",
                            "（一）重点招生范围",
                            "1.省直实施公务员法机关单位子女",
                            "2.省管局局属幼儿园教职工子女",
                            "3.有办园协议相关规定的招生对象",
                            "（二）补充招生范围",
                            "四、招生流程",
                            "（一）申报园所",
                            "1.单位申报",
                            "（1）各省直单位指定负责人",
                            "2.业主申报"};
        for (int i = 0; i < (int)dimof(t5); i++) {
            flat.Append(NewItem(t5[i], i + 1, 0, 0, HeadingLevelFromText(t5[i])));
        }
        BuildTreeFromFlat(flat, roots);
        bool treeOk = roots.Size() == 2 && str::Eq(roots[0]->title, "一、招生范围") && roots[0]->children.Size() == 2 &&
                      str::Eq(roots[0]->children[0]->title, "（一）重点招生范围") &&
                      roots[0]->children[0]->children.Size() == 3 &&
                      str::Eq(roots[0]->children[1]->title, "（二）补充招生范围") &&
                      str::Eq(roots[1]->title, "四、招生流程") && roots[1]->children.Size() == 1 &&
                      roots[1]->children[0]->children.Size() == 2 &&
                      str::Eq(roots[1]->children[0]->children[0]->title, "1.单位申报") &&
                      roots[1]->children[0]->children[0]->children.Size() == 1;
        if (lvlOk && treeOk) {
            pass++;
        } else {
            fail++;
            failMask |= 4096;
        }
        DeleteExtractedTocItems(roots);
    }
    {
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        const char* tq[] = {"三、招生数量", "1.省八一保育院", "2.省直第一幼儿园", "3.省直第二幼儿园",
                            "4.省直第三幼儿园"};
        int prev = 1;
        for (int i = 0; i < (int)dimof(tq); i++) {
            int numbered = HeadingLevelFromText(tq[i]);
            int lvl = ClampOutlineLevel(numbered, numbered, prev, i > 0);
            prev = lvl;
            flat.Append(NewItem(tq[i], i + 1, 0, 0, lvl));
        }
        BuildTreeFromFlat(flat, roots);
        bool ok = roots.Size() == 1 && roots[0]->children.Size() == 4 &&
                  str::Eq(roots[0]->children[0]->title, "1.省八一保育院") &&
                  str::Eq(roots[0]->children[1]->title, "2.省直第一幼儿园") &&
                  str::Eq(roots[0]->children[3]->title, "4.省直第三幼儿园");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 8192;
        }
        DeleteExtractedTocItems(roots);
    }
    {
        char* j = str::Join("2", ".省直第一幼儿园：4个小班100人（原托班升小班19人，");
        ExtractedTocItem* a = j ? NewItem(j, 1, 0, 0, HeadingLevelFromText(j)) : nullptr;
        bool ok = j && HeadingLevelFromText(j) == 3 && a && str::Eq(a->title, "2.省直第一幼儿园");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 16384;
        }
        str::Free(j);
        delete a;
    }
    {
        bool dateLvl = HeadingLevelFromText("4.6月30日和8月15日前") == 3 && HeadingLevelFromText("5.6月30日前") == 3 &&
                       HeadingLevelFromText("6.8月31日前") == 3 && HeadingLevelFromText("1.1 有记性的数") == 2 &&
                       HeadingLevelFromText("4. 6月30日") == 3;
        Vec<ExtractedTocItem*> flat;
        Vec<ExtractedTocItem*> roots;
        const char* td[] = {"（二）推进落实阶段",
                            "1.省有关单位对堵点问题逐一制定解决方案",
                            "2.省发改委(省信息中心)按阶段归集各部门数据需求后",
                            "3.数据提供部门收到数据需求信息后",
                            "4.6月30日和8月15日前，办事部门通过相关业务系统与平台对接",
                            "5.6月30日前，省有关单位完成自有业务系统",
                            "6.8月31日前，省政府正式向社会发布"};
        int prev = 1;
        for (int i = 0; i < (int)dimof(td); i++) {
            int numbered = HeadingLevelFromText(td[i]);
            int lvl = ClampOutlineLevel(numbered, numbered, prev, i > 0);
            prev = lvl;
            flat.Append(NewItem(td[i], i + 1, 0, 0, lvl));
        }
        BuildTreeFromFlat(flat, roots);
        bool treeOk = roots.Size() == 1 && roots[0]->children.Size() == 6 && roots[0]->children[3]->title &&
                      str::StartsWith(roots[0]->children[3]->title, "4.6月30日") && roots[0]->children[5]->title &&
                      str::StartsWith(roots[0]->children[5]->title, "6.8月31日前");
        if (dateLvl && treeOk) {
            pass++;
        } else {
            fail++;
            failMask |= 32768;
        }
        DeleteExtractedTocItems(roots);
    }
    {
        Vec<ExtractedTocItem*> flat;
        flat.Append(NewItem("一、甲", 5, 0, 0, 1));
        flat.Append(NewItem("二、乙", 8, 0, 0, 1));
        flat.Append(NewItem("三、丙", 3, 0, 0, 1));
        flat.Append(NewItem("四、丁", 10, 0, 0, 1));
        int nFix = EnforceMonotonicPages(flat);
        bool ok =
            nFix == 1 && flat[0]->pageNo == 5 && flat[1]->pageNo == 8 && flat[2]->pageNo == 8 && flat[3]->pageNo == 10;
        Vec<PrintedHit> ph;
        PrintedHit a = MakeTestHit("A", 1, 10);
        PrintedHit b = MakeTestHit("B", 1, 20);
        PrintedHit c = MakeTestHit("C", 1, 30);
        a.destPage = 12;
        b.destPage = 4;
        c.destPage = 15;
        ph.Append(a);
        ph.Append(b);
        ph.Append(c);
        int nFix2 = EnforceMonotonicPrintedDests(ph);
        ok = ok && nFix2 == 1 && ph[0].destPage == 12 && ph[1].destPage == 12 && ph[2].destPage == 15;
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 65536;
        }
        DeleteExtractedTocItems(flat);
        FreePrintedHits(ph);
    }
    {
        ExtractedTocItem* glued = NewItem("第一条为完善网络与数据安全保护制度 第一章总则", 1, 0, 0, 2);
        bool ok = HeadingLevelFromText("第一批次将在2022年8月") == 0 && HeadingLevelFromText("第一章 总则") == 1 &&
                  HeadingLevelFromText("第一条 根据法律") == 2 && HeadingLevelFromText("55.81") == 0 &&
                  HeadingLevelFromText("15.00") == 0 && LooksLikeOfficialBoilerplate("江西省人力资源和社会保障厅") &&
                  LooksLikeOfficialBoilerplate("文件") && LooksLikeLeaderTitle("第一章 发展基础 ．．．．．．．．．") &&
                  glued && glued->title && str::StartsWith(glued->title, "第一条") &&
                  !str::Find(glued->title, "第一章");
        if (ok) {
            pass++;
        } else {
            fail++;
            failMask |= 131072;
        }
        delete glued;
    }
    {
        bool markerOk = HeadingLevelFromText("一是加强组织领导。") == 0 &&
                        HeadingLevelFromText("二是完善工作机制") == 0 &&
                        HeadingLevelFromText("第一，提高政治站位；") == 0 && HeadingLevelFromText("一.总体要求") == 1 &&
                        HeadingLevelFromText("一．主要任务") == 1 && HeadingLevelFromText("一 、组织保障") == 1 &&
                        ParseHeadingMarker("一是加强").type == MarkerType::YiShi &&
                        ParseHeadingMarker("第一，提高").type == MarkerType::DiYiComma;
        if (markerOk) {
            pass++;
        } else {
            fail++;
            failMask |= 262144;
        }
        ExtractedTocItem* split = NewItem("（一）加强组织领导。各地要高度重视此项工作并抓好落实责任", 1, 0, 0, 2);
        ExtractedTocItem* colon = NewItem("一、工作目标：", 1, 0, 0, 1);
        bool splitOk = split && split->title && str::Eq(split->title, "（一）加强组织领导");
        bool colonOk = colon && colon->title && str::Eq(colon->title, "一、工作目标：");
        ScanLine a = {};
        ScanLine b = {};
        ScanLine c = {};
        ScanLine d = {};
        a.text = str::Dup("三、进一步加强基层公共就业服务");
        a.srcPage = 1;
        a.x = 72;
        a.y = 40;
        a.dy = 12;
        b.text = str::Dup("体系建设");
        b.srcPage = 1;
        b.x = 72;
        b.y = 54;
        b.dy = 12;
        c.text = str::Dup("一、总体要求");
        c.srcPage = 1;
        c.x = 72;
        c.y = 80;
        c.dy = 12;
        d.text = str::Dup("为深入贯彻落实有关决策部署");
        d.srcPage = 1;
        d.x = 72;
        d.y = 94;
        d.dy = 12;
        bool mergeOk = ShouldMergeHeadingLines(a, b) && !ShouldMergeHeadingLines(c, d);
        ScanLine plan = {};
        ScanLine after = {};
        plan.text = str::Dup("四、下一步工作计划");
        plan.srcPage = 1;
        plan.x = 72;
        plan.y = 40;
        plan.dy = 14;
        after.text = str::Dup("做好");
        after.srcPage = 1;
        after.x = 72 + 28;
        after.y = 58;
        after.dy = 14;
        bool indentOk = !ShouldMergeHeadingLines(plan, after);
        ScanLine future = {};
        future.text = str::Dup("今后");
        future.srcPage = 1;
        future.x = 72;
        future.y = 58;
        future.dy = 14;
        bool futureOk = !ShouldMergeHeadingLines(plan, future);
        ExtractedTocItem* gluedSp = NewItem("四、下一步工作计划 今后", 1, 0, 0, 1);
        ExtractedTocItem* gluedTight = NewItem("四、下一步工作计划今后", 1, 0, 0, 1);
        bool stripOk = gluedSp && gluedSp->title && str::Eq(gluedSp->title, "四、下一步工作计划") && gluedTight &&
                       gluedTight->title && str::Eq(gluedTight->title, "四、下一步工作计划");
        Vec<ScanLine> headLines;
        Vec<ScanLine> midLines;
        for (int p = 1; p <= 3; p++) {
            ScanLine h = {};
            h.text = str::Dup("江西省人力资源和社会保障厅");
            h.srcPage = p;
            h.y = 8;
            h.dy = 10;
            ScanLine body = {};
            body.text = str::Dup("正文段落内容写在页面中部位置");
            body.srcPage = p;
            body.y = 200;
            body.dy = 12;
            headLines.Append(h);
            headLines.Append(body);
            ScanLine top = {};
            top.text = str::Dup("首页导语");
            top.srcPage = p;
            top.y = 8;
            top.dy = 10;
            ScanLine mid = {};
            mid.text = str::Dup("总体要求");
            mid.srcPage = p;
            mid.y = 110;
            mid.dy = 12;
            ScanLine bot = {};
            bot.text = str::Dup("结尾段落文字");
            bot.srcPage = p;
            bot.y = 220;
            bot.dy = 12;
            midLines.Append(top);
            midLines.Append(mid);
            midLines.Append(bot);
        }
        StrVec headHit;
        StrVec midHit;
        CollectHeaderTexts(headLines, 3, headHit);
        CollectHeaderTexts(midLines, 3, midHit);
        bool headOk = headHit.Find("江西省人力资源和社会保障厅") >= 0 && midHit.Find("总体要求") < 0;
        if (splitOk && colonOk && mergeOk && indentOk && futureOk && stripOk && headOk) {
            pass++;
        } else {
            fail++;
            failMask |= 524288;
        }
        str::Free(a.text);
        str::Free(b.text);
        str::Free(c.text);
        str::Free(d.text);
        str::Free(plan.text);
        str::Free(after.text);
        str::Free(future.text);
        FreeScanLines(headLines);
        FreeScanLines(midLines);
        delete split;
        delete colon;
        delete gluedSp;
        delete gluedTight;
    }
    RunPrintedTocLogicTestsPhase2(&pass, &fail, &failMask);
    if (fail > 0) {
        logf("ExtractPdfToc tests fail=%d mask=%d pass=%d\n", fail, failMask, pass);
    } else {
        static bool loggedPass;
        if (!loggedPass) {
            loggedPass = true;
            logf("ExtractPdfToc tests pass=%d\n", pass);
        }
    }
}

static bool PageLooksLikePrintedToc(const Vec<ScanLine>& lines, int p);
static int TocPageScore(const Vec<ScanLine>& lines, int p);

static void ResolvePrintedDestinations(const Vec<ScanLine>& lines, int tocEnd, int nPages, Vec<PrintedHit>& hits,
                                       int* offsetOut, int* nMatchedOut) {
    *offsetOut = 0;
    *nMatchedOut = 0;
    if (hits.Size() < 1 || tocEnd < 1) {
        return;
    }
    Vec<int> skipPage;
    int skipLim = tocEnd + 24;
    if (skipLim < tocEnd + 1) {
        skipLim = tocEnd + 1;
    }
    if (skipLim > nPages) {
        skipLim = nPages;
    }
    for (int p = 1; p <= nPages; p++) {
        skipPage.Append(0);
    }
    for (int p = 1; p <= skipLim; p++) {
        if (PageLooksLikePrintedToc(lines, p)) {
            skipPage[p - 1] = 1;
        }
    }
    Vec<int> cand;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (sl.srcPage <= tocEnd || !sl.text || skipPage[sl.srcPage - 1]) {
            continue;
        }
        int g = GlyphCount(sl.text);
        if (g < 2 || g > 48) {
            continue;
        }
        if (i > 0 && ShouldMergeHeadingLines(lines[i - 1], sl)) {
            continue;
        }
        if (LineLooksLikePageNumber(sl.text) || LooksLikeArchiveJunk(sl.text)) {
            continue;
        }
        if (!HasLetterOrCjk(sl.text)) {
            continue;
        }
        cand.Append(i);
    }
    Vec<int> usedCand;
    for (int i = 0; i < cand.Size(); i++) {
        usedCand.Append(0);
    }
    Vec<int> offsets;
    Vec<int> hitMatched;
    int nMatched = 0;
    int minDest = 0;
    for (int h = 0; h < hits.Size(); h++) {
        hitMatched.Append(0);
        char a[192];
        char b[192];
        NormalizeTitle(hits[h].title, a, (int)sizeof(a), false);
        NormalizeTitle(hits[h].title, b, (int)sizeof(b), true);
        int best100 = -1;
        int best80 = -1;
        for (int c = 0; c < cand.Size(); c++) {
            if (usedCand[c]) {
                continue;
            }
            if (minDest > 0 && lines[cand[c]].srcPage < minDest) {
                continue;
            }
            char na[192];
            char nb[192];
            char* ct = CandidateHeadingText(lines, cand[c]);
            NormalizeTitle(ct, na, (int)sizeof(na), false);
            NormalizeTitle(ct, nb, (int)sizeof(nb), true);
            str::Free(ct);
            int sc = TitleMatchScore(a, na);
            int sc2 = TitleMatchScore(b, nb);
            if (sc2 > sc) {
                sc = sc2;
            }
            if (sc >= 100 && best100 < 0) {
                best100 = c;
                break;
            }
            if (sc >= 80 && best80 < 0) {
                best80 = c;
            }
        }
        int best = best100 >= 0 ? best100 : best80;
        if (best < 0) {
            continue;
        }
        usedCand[best] = 1;
        const ScanLine& sl = lines[cand[best]];
        hits[h].destPage = sl.srcPage;
        hits[h].x = sl.x;
        hits[h].y = sl.y;
        hitMatched[h] = 1;
        nMatched++;
        minDest = sl.srcPage;
        int off = sl.srcPage - hits[h].printed;
        if (off >= 0 && off < nPages) {
            offsets.Append(off);
        }
    }
    int offset = 0;
    if (offsets.Size() >= 1) {
        offset = MedianInt(offsets);
    } else {
        offset = tocEnd;
    }
    *offsetOut = offset;
    *nMatchedOut = nMatched;
    for (int h = 0; h < hits.Size(); h++) {
        if (hitMatched[h] && hits[h].destPage > tocEnd) {
            continue;
        }
        if (hits[h].printed < 1) {
            continue;
        }
        int dest = hits[h].printed + offset;
        if (dest <= tocEnd) {
            dest = tocEnd + 1;
        }
        if (dest > nPages) {
            dest = nPages;
        }
        if (dest < 1) {
            dest = 1;
        }
        hits[h].destPage = dest;
        FindDestOnPage(lines, dest, hits[h].title, &hits[h].x, &hits[h].y);
    }
}

static void ApplyPrintedTocLinkDests(EngineBase* engine, Vec<PrintedHit>& hits, int tocEnd) {
    if (!engine) {
        return;
    }
    int lastPage = -1;
    int prevDest = 0;
    for (int h = 0; h < hits.Size(); h++) {
        int p = hits[h].srcPage;
        if (p < 1) {
            continue;
        }
        if (p != lastPage) {
            EngineMupdfEnsurePageLinksForHitTest(engine, p);
            lastPage = p;
        }
        Vec<IPageElement*> els = engine->GetElements(p);
        int best = -1;
        float bestD = 1e9f;
        for (int i = 0; i < els.Size(); i++) {
            IPageElement* el = els[i];
            if (!el || !el->IsLink()) {
                continue;
            }
            IPageDestination* dest = el->AsLink();
            if (!dest || dest->pageNo <= tocEnd || dest->pageNo > engine->PageCount()) {
                continue;
            }
            if (prevDest > 0 && dest->pageNo < prevDest) {
                continue;
            }
            RectF r = el->GetRect();
            float elY = (float)r.y;
            float d = elY - hits[h].srcY;
            if (d < 0) {
                d = -d;
            }
            if (d > 16.0f) {
                continue;
            }
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        if (best < 0) {
            if (hits[h].destPage > prevDest) {
                prevDest = hits[h].destPage;
            }
            continue;
        }
        IPageDestination* dest = els[best]->AsLink();
        if (prevDest > 0 && dest->pageNo < prevDest) {
            if (hits[h].destPage > prevDest) {
                prevDest = hits[h].destPage;
            }
            continue;
        }
        int destPage = dest->pageNo;
        if (destPage < 1 || destPage > engine->PageCount()) {
            if (hits[h].destPage > prevDest) {
                prevDest = hits[h].destPage;
            }
            continue;
        }
        hits[h].destPage = destPage;
        hits[h].x = (float)dest->rect.x;
        hits[h].y = (float)dest->rect.y;
        prevDest = dest->pageNo;
    }
}

static bool PageLooksLikePrintedToc(const Vec<ScanLine>& lines, int p) {
    return TocPageScore(lines, p) >= kExtractPdfToc.tocPageScoreMin;
}

static bool PageLooksLikeCipOrColophon(const Vec<ScanLine>& lines, int p) {
    int weak = 0;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage != p || !lines[i].text) {
            continue;
        }
        const char* s = lines[i].text;
        if (str::Find(s, "图书在版编目") || str::Find(s, "中国版本图书馆") ||
            (str::Find(s, "CIP") && (str::Find(s, "数据") || str::Find(s, "核字")))) {
            return true;
        }
        if (str::Find(s, "ISBN") || str::Find(s, "I SBN") || str::Find(s, "SBN978")) {
            weak++;
        }
        if (str::Find(s, "责任编辑") || str::Find(s, "责任校对") || str::Find(s, "责任印制") ||
            str::Find(s, "封面设计")) {
            weak++;
        }
        if (str::Find(s, "出版人") || str::StartsWith(s, "出版发行")) {
            weak++;
        }
        if (str::Find(s, "定价") && (str::Find(s, "元") || str::Find(s, "￥"))) {
            weak++;
        }
    }
    return weak >= 3;
}

static int TocPageScore(const Vec<ScanLine>& lines, int p) {
    if (PageLooksLikeCipOrColophon(lines, p)) {
        return 0;
    }
    int score = 0;
    int nNum = 0;
    int nShort = 0;
    int nLeader = 0;
    int nSlashToc = 0;
    bool hasTocWord = false;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage != p || !lines[i].text) {
            continue;
        }
        if (LooksLikePrintedTocHeading(lines[i].text)) {
            score += 40;
            hasTocWord = true;
        }
        if (LineLooksLikePageNumber(lines[i].text)) {
            nNum++;
            continue;
        }
        int g = GlyphCount(lines[i].text);
        if (g >= 2 && g <= 40 && HasLetterOrCjk(lines[i].text)) {
            nShort++;
        }
        if (LooksLikeLeaderTitle(lines[i].text) || str::Find(lines[i].text, "......") ||
            str::Find(lines[i].text, "\xE2\x80\xA6") || str::Find(lines[i].text, "\xC2\xB7\xC2\xB7")) {
            nLeader++;
        }
        char* title = nullptr;
        int printed = 0;
        if (ParsePrintedTocLine(lines[i].text, &title, &printed) && title && HeadingLevelFromText(title) >= 1) {
            nSlashToc++;
        }
        str::Free(title);
    }
    if (nLeader >= 4) {
        score += nLeader * 3;
    } else {
        score += nLeader;
    }
    if (nSlashToc >= 3) {
        score += 30;
    }
    // Tables of numbered projects look like "short lines + page-ish numbers".
    // Real printed TOCs have a 目录 heading or dotted leaders.
    if (hasTocWord || nLeader >= 4 || nSlashToc >= 3) {
        if (nNum >= 4 && nShort >= 4) {
            score += 25;
        }
        if (nNum >= 8) {
            score += 10;
        }
    }
    return score;
}

static bool PageHasPrintedTocHeading(const Vec<ScanLine>& lines, int p) {
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage == p && LooksLikePrintedTocHeading(lines[i].text)) {
            return true;
        }
    }
    return false;
}

static bool TryPrintedToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                          Vec<ExtractedTocItem*>& roots, const PrintedTocOpts& opts) {
    int frontLim = nPages * opts.searchPct / 100;
    if (frontLim < opts.searchMinPages) {
        frontLim = opts.searchMinPages;
    }
    if (frontLim > opts.searchMaxPages) {
        frontLim = opts.searchMaxPages;
    }
    if (frontLim > nPages) {
        frontLim = nPages;
    }
    int tocStart = 0;
    int headingPage = 0;
    for (int p = 1; p <= frontLim; p++) {
        if (PageLooksLikeCipOrColophon(lines, p)) {
            continue;
        }
        if (opts.acceptTocHeadingPage && PageHasPrintedTocHeading(lines, p)) {
            headingPage = p;
            break;
        }
        if (tocStart < 1 && TocPageScore(lines, p) >= opts.pageScoreMin) {
            tocStart = p;
        }
    }
    if (headingPage > 0) {
        tocStart = headingPage;
    }
    if (tocStart < 1) {
        return false;
    }
    StrVec tocHeaders;
    CollectHeaderTexts(lines, nPages, tocHeaders);
    Vec<PrintedHit> hits;
    int streakEmpty = 0;
    int nPaired = 0;
    int nParsed = 0;
    int nSecAttach = 0;
    int nChapExtra = 0;
    int tocEnd = tocStart;
    for (int p = tocStart; p <= nPages && p <= tocStart + opts.maxSpanPages; p++) {
        if (PageLooksLikeCipOrColophon(lines, p)) {
            continue;
        }
        int nOnPage = 0;
        Vec<ScanLine> pageLines;
        for (int i = 0; i < lines.Size(); i++) {
            if (lines[i].srcPage != p || !lines[i].text || LooksLikeArchiveJunk(lines[i].text)) {
                continue;
            }
            ScanLine sl = lines[i];
            sl.text = str::Dup(lines[i].text);
            pageLines.Append(sl);
        }
        RebuildVisualLines(pageLines);
        RepairOcrDiUnits(pageLines);
        GluePrintedTocWraps(pageLines);
        GlueSplitPrintedPageNums(pageLines);
        Vec<int> idx;
        for (int k = 0; k < pageLines.Size(); k++) {
            idx.Append(k);
        }
        Vec<int> used;
        for (int k = 0; k < idx.Size(); k++) {
            used.Append(0);
        }
        for (int k = 0; k < idx.Size(); k++) {
            if (LooksLikePrintedTocHeading(pageLines[k].text) || LooksLikeArchiveJunk(pageLines[k].text) ||
                LineLooksLikeRomanPage(pageLines[k].text)) {
                used[k] = 1;
                continue;
            }
            char* title = nullptr;
            int printed = 0;
            if (!ParsePrintedTocLine(pageLines[k].text, &title, &printed)) {
                continue;
            }
            if (LooksLikeArchiveJunk(title) || LineLooksLikeRomanPage(title)) {
                str::Free(title);
                continue;
            }
            int dest = MapPrintedPage(engine, printed, labels);
            PrintedHit h;
            h.title = CleanTocTitle(title);
            str::Free(title);
            h.printed = printed;
            h.destPage = dest;
            h.srcPage = p;
            h.srcX = pageLines[k].x;
            h.srcY = pageLines[k].y;
            h.indent = pageLines[k].x;
            h.fontSize = pageLines[k].fontSize;
            h.bold = pageLines[k].bold;
            FindDestOnPage(lines, dest, h.title, &h.x, &h.y);
            hits.Append(h);
            used[k] = 1;
            nOnPage++;
            nParsed++;
        }
        for (int k = 0; k < idx.Size(); k++) {
            if (used[k]) {
                continue;
            }
            const ScanLine& num = pageLines[k];
            if (!LineLooksLikePageNumber(num.text)) {
                continue;
            }
            int printed = ParseBarePrintedPage(num.text);
            if (printed < 1) {
                continue;
            }
            int best = -1;
            float bestX = -1e9f;
            for (int t = 0; t < idx.Size(); t++) {
                if (used[t]) {
                    continue;
                }
                const ScanLine& title = pageLines[t];
                if (LineLooksLikePageNumber(title.text) || LooksLikePrintedTocHeading(title.text) ||
                    LooksLikeArchiveJunk(title.text) || LineLooksLikeRomanPage(title.text)) {
                    continue;
                }
                int g = GlyphCount(title.text);
                if (g < 2 || g > 48 || !HasLetterOrCjk(title.text)) {
                    continue;
                }
                if (!SamePrintedTocRow(title, num)) {
                    continue;
                }
                if (title.x >= num.x - 4) {
                    continue;
                }
                if (title.x > bestX) {
                    bestX = title.x;
                    best = t;
                }
            }
            if (best < 0) {
                continue;
            }
            used[best] = 1;
            used[k] = 1;
            AppendPairedTocHit(hits, engine, labels, pageLines, idx, used, best, printed, &nSecAttach);
            nOnPage++;
            nPaired++;
        }
        for (int k = 0; k < idx.Size(); k++) {
            if (used[k]) {
                continue;
            }
            const ScanLine& num = pageLines[k];
            if (!LineLooksLikePageNumber(num.text)) {
                continue;
            }
            int printed = ParseBarePrintedPage(num.text);
            if (printed < 1) {
                continue;
            }
            int best = -1;
            float bestD = 1e9f;
            for (int t = 0; t < idx.Size(); t++) {
                if (used[t]) {
                    continue;
                }
                const ScanLine& title = pageLines[t];
                if (LineLooksLikePageNumber(title.text) || LooksLikePrintedTocHeading(title.text) ||
                    LooksLikeArchiveJunk(title.text) || LineLooksLikeRomanPage(title.text)) {
                    continue;
                }
                int g = GlyphCount(title.text);
                if (g < 2 || g > 48 || !HasLetterOrCjk(title.text)) {
                    continue;
                }
                if (title.x >= num.x - 4) {
                    continue;
                }
                float dy = title.dy > num.dy ? title.dy : num.dy;
                if (dy < 6) {
                    dy = 6;
                }
                float d = ScanLineMidY(title) - ScanLineMidY(num);
                if (d < 0) {
                    d = -d;
                }
                if (d > dy * 1.4f) {
                    continue;
                }
                if (d < bestD) {
                    bestD = d;
                    best = t;
                }
            }
            if (best < 0) {
                continue;
            }
            used[best] = 1;
            used[k] = 1;
            AppendPairedTocHit(hits, engine, labels, pageLines, idx, used, best, printed, &nSecAttach);
            nOnPage++;
            nPaired++;
        }
        for (int k = 0; k < idx.Size(); k++) {
            if (used[k] || !LooksLikeTocNumToken(pageLines[k].text)) {
                continue;
            }
            int best = -1;
            float bestD = 1e9f;
            for (int h = 0; h < hits.Size(); h++) {
                if (hits[h].srcPage != p || !hits[h].title) {
                    continue;
                }
                ParsedNumbering have;
                ParseHeadingNumbering(hits[h].title, &have);
                if (have.nComp >= 2 || have.isChapter) {
                    continue;
                }
                if (hits[h].srcX <= pageLines[k].x + 1) {
                    continue;
                }
                float d = hits[h].srcY - pageLines[k].y;
                if (d < 0) {
                    d = -d;
                }
                if (d > 18) {
                    continue;
                }
                if (d < bestD) {
                    bestD = d;
                    best = h;
                }
            }
            if (best < 0) {
                continue;
            }
            char* neu = str::Join(pageLines[k].text, " ", hits[best].title);
            str::Free(hits[best].title);
            hits[best].title = neu;
            hits[best].srcX = pageLines[k].x;
            used[k] = 1;
            nSecAttach++;
        }
        bool pageAllowsLeftover = TocPageScore(lines, p) >= opts.pageScoreMin;
        bool enTocPage = false;
        for (int k = 0; k < idx.Size(); k++) {
            if (LooksLikeEnglishTocHeading(pageLines[k].text)) {
                enTocPage = true;
                break;
            }
        }
        float titleColX = 0;
        int nTitleCol = 0;
        for (int h = 0; h < hits.Size(); h++) {
            if (hits[h].srcPage != p) {
                continue;
            }
            titleColX += hits[h].srcX;
            nTitleCol++;
        }
        if (nTitleCol > 0) {
            titleColX /= (float)nTitleCol;
        }
        for (int k = 0; k < idx.Size() && pageAllowsLeftover; k++) {
            if (used[k] || !pageLines[k].text) {
                continue;
            }
            if (LooksLikePrintedTocHeading(pageLines[k].text) || LooksLikeArchiveJunk(pageLines[k].text) ||
                LineLooksLikePageNumber(pageLines[k].text) || LineLooksLikeRomanPage(pageLines[k].text)) {
                continue;
            }
            char* cleaned = CleanTocTitle(pageLines[k].text);
            StripLeadingSubtitleDash(&cleaned);
            if (!cleaned || !cleaned[0] || !HasLetterOrCjk(cleaned) || LineLooksLikePageNumber(cleaned) ||
                LineLooksLikeRomanPage(cleaned)) {
                str::Free(cleaned);
                continue;
            }
            int hl = HeadingLevelFromText(cleaned);
            int g = GlyphCount(cleaned);
            bool partTitle = opts.acceptPartTitles && LineLooksLikeBookPartTitle(cleaned);
            bool xinDe = StartsWithXinDeHeading(cleaned);
            bool leaderTitle = LooksLikeLeaderTitle(pageLines[k].text);
            bool bookLeftover = (opts.acceptPartTitles || enTocPage) && g >= 2 && g <= 48;
            if (nTitleCol > 0 && pageLines[k].x > titleColX + 36 && !partTitle) {
                str::Free(cleaned);
                continue;
            }
            if (!enTocPage && !xinDe && !partTitle && hl < 1 &&
                (str::Find(cleaned, "，") || str::Find(cleaned, "。") || g > 22)) {
                str::Free(cleaned);
                continue;
            }
            if (!bookLeftover && !partTitle && IsHeaderText(tocHeaders, pageLines[k].text)) {
                str::Free(cleaned);
                continue;
            }
            if (!bookLeftover && !partTitle && !leaderTitle && (hl != 1 || g < 2 || g > 40)) {
                str::Free(cleaned);
                continue;
            }
            int prevHit = -1;
            for (int h = hits.Size() - 1; h >= 0; h--) {
                if (hits[h].srcPage == p && hits[h].title) {
                    prevHit = h;
                    break;
                }
            }
            if (prevHit >= 0 && BookTitleIsSpine(hits[prevHit].title) && LineLooksLikeChapterSubtitle(cleaned) &&
                !str::Find(hits[prevHit].title, "——")) {
                char* neu = str::Join(hits[prevHit].title, "——", cleaned);
                str::Free(hits[prevHit].title);
                hits[prevHit].title = neu;
                str::Free(cleaned);
                used[k] = 1;
                continue;
            }
            if (prevHit >= 0 && LineIsPrintedTocContinuation(cleaned) && pageLines[k].x + 1 >= hits[prevHit].srcX &&
                (LineIsBareLessonOrChapter(hits[prevHit].title) || TitleNeedsWrapContinuation(hits[prevHit].title))) {
                char* neu = str::Join(hits[prevHit].title, " ", cleaned);
                str::Free(hits[prevHit].title);
                hits[prevHit].title = neu;
                str::Free(cleaned);
                used[k] = 1;
                continue;
            }
            PrintedHit h;
            h.title = cleaned;
            h.printed = 0;
            h.srcPage = p;
            h.srcX = pageLines[k].x;
            h.srcY = pageLines[k].y;
            h.indent = pageLines[k].x;
            h.fontSize = pageLines[k].fontSize;
            h.bold = pageLines[k].bold;
            hits.Append(h);
            used[k] = 1;
            nOnPage++;
        }
        FreeScanLines(pageLines);
        if (nOnPage >= 3) {
            streakEmpty = 0;
            tocEnd = p;
        } else if (p > tocStart) {
            streakEmpty++;
            if (streakEmpty >= 2) {
                break;
            }
        }
    }
    int nBodyHit = 0;
    int nDestEqPrinted = 0;
    int nMono = 0;
    int nDestBeforeToc = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].x != 0 || hits[i].y != 0) {
            nBodyHit++;
        }
        if (hits[i].destPage == hits[i].printed) {
            nDestEqPrinted++;
        }
        if (hits[i].destPage > 0 && hits[i].destPage < tocStart) {
            nDestBeforeToc++;
        }
        if (i > 0 && hits[i].printed >= hits[i - 1].printed) {
            nMono++;
        }
    }
    int firstLabel1 = 0;
    int nIdent = 0;
    for (int i = 0; i < labels.Size(); i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", i + 1);
        if (labels[i] && str::Eq(labels[i], buf)) {
            nIdent++;
        }
        if (firstLabel1 == 0 && labels[i] && str::Eq(labels[i], "1")) {
            firstLabel1 = i + 1;
        }
    }
    if (opts.bookHierarchy) {
        RecoverMissingBookSpines(hits, lines, tocStart, nPages);
    }
    if (hits.Size() < opts.minHits) {
        FreePrintedHits(hits);
        return false;
    }
    for (int p = tocStart; p <= nPages && p <= tocStart + opts.maxSpanPages; p++) {
        if (PageLooksLikePrintedToc(lines, p)) {
            tocEnd = p;
        } else if (p > tocEnd + 1) {
            break;
        }
    }
    SortPrintedHits(hits);
    if (opts.repairMissingSectionNumbers) {
        RepairPrintedHitTitles(hits);
        RepairMissingSectionNumbers(hits);
    } else {
        for (int i = 0; i < hits.Size(); i++) {
            StripFalseSectionPrefixOnChapter(&hits[i].title);
        }
    }
    for (int i = 0; i < hits.Size(); i++) {
        PolishPrintedHitTitle(&hits[i].title);
    }
    if (!opts.bookHierarchy) {
        for (int i = hits.Size() - 1; i >= 0; i--) {
            if (!TitleIsNumberingOnly(hits[i].title)) {
                continue;
            }
            str::Free(hits[i].title);
            hits[i].title = nullptr;
            hits.RemoveAt((size_t)i);
        }
    }
    int pageOffset = 0;
    int nTitleMatched = 0;
    ResolvePrintedDestinations(lines, tocEnd, nPages, hits, &pageOffset, &nTitleMatched);
    CompleteTruncatedBookQuotes(lines, hits);
    ApplyPrintedTocLinkDests(engine, hits, tocEnd);
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].destPage > 0) {
            continue;
        }
        int dest = tocEnd + 1;
        if (dest > nPages) {
            dest = nPages;
        }
        if (dest < 1) {
            dest = 1;
        }
        hits[i].destPage = dest;
        FindDestOnPage(lines, dest, hits[i].title, &hits[i].x, &hits[i].y);
    }
    EnforceMonotonicPrintedDests(hits);
    int nBodyHit2 = 0;
    int nDestEqPrinted2 = 0;
    int nDestBeforeToc2 = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].x != 0 || hits[i].y != 0) {
            nBodyHit2++;
        }
        if (hits[i].destPage == hits[i].printed) {
            nDestEqPrinted2++;
        }
        if (hits[i].destPage > 0 && hits[i].destPage < tocStart) {
            nDestBeforeToc2++;
        }
    }
    if (opts.requireDestMatch && nTitleMatched * 2 < hits.Size() && nBodyHit2 * 2 < hits.Size()) {
        FreePrintedHits(hits);
        return false;
    }
    Vec<ExtractedTocItem*> flat;
    int prevLevel = 1;
    float lastChapX = 0;
    bool haveChap = false;
    if (opts.bookHierarchy) {
        AssignBookPrintedLevels(hits);
    }
    for (int i = 0; i < hits.Size(); i++) {
        ParsedNumbering num;
        ParseHeadingNumbering(hits[i].title, &num);
        int lvl = opts.bookHierarchy ? hits[i].level : LevelFromPrintedTitle(hits[i].title);
        if (!opts.bookHierarchy && (num.isChapter || NumberingIsBareArabicChapter(num, hits[i].title))) {
            lvl = 1;
            haveChap = true;
            lastChapX = hits[i].srcX;
        } else if (!opts.bookHierarchy && num.depth < 1 && haveChap && hits[i].srcX > lastChapX + 8) {
            lvl = 2;
        }
        if (lvl < 1) {
            lvl = 1;
        }
        if (lvl > prevLevel + 1) {
            lvl = prevLevel + 1;
        }
        if (lvl > 6) {
            lvl = 6;
        }
        prevLevel = lvl;
        ExtractedTocItem* n = NewItem(hits[i].title, hits[i].destPage, hits[i].x, hits[i].y, lvl);
        n->tocPageNo = hits[i].srcPage;
        n->tocX = hits[i].srcX;
        n->tocY = hits[i].srcY;
        flat.Append(n);
    }
    EnforceMonotonicPages(flat);
    FreePrintedHits(hits);
    BuildTreeFromFlat(flat, roots);
    int nItems = CountExtracted(roots);
    return nItems >= opts.minHits;
}

static bool LineIsBareListNumber(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    int nDig = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(cp)) {
            nDig++;
            if (nDig > 3) {
                return false;
            }
            continue;
        }
        if (cp > 32 && cp != 0x3000) {
            return false;
        }
    }
    return nDig >= 1 && nDig <= 3;
}

// "4）" / "4)" with no title — scanned contracts often put the number on its own line.
static bool LineIsBareCloseParenEnum(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    int nDig = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(cp)) {
            nDig++;
            if (nDig > 3) {
                return false;
            }
            continue;
        }
        i = save;
        break;
    }
    if (nDig < 1) {
        return false;
    }
    SkipWsUtf8(s, len, i);
    if (i >= len) {
        return false;
    }
    int cp = Utf8CodepointNext(s, len, i);
    if (!IsParenCloseCp(cp)) {
        return false;
    }
    SkipWsUtf8(s, len, i);
    return i >= len;
}

static bool LineStartsWithDotThenTitle(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    if (i >= len) {
        return false;
    }
    int cp = Utf8CodepointNext(s, len, i);
    if (cp != '.' && cp != 0xFF0E) {
        return false;
    }
    return HasLetterOrCjk(s + i);
}

static bool EndsWithBareEnumMarker(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int lastBare = -1;
    while (i < len) {
        int save = i;
        HeadingMarker m = ParseHeadingMarker(s + save);
        if (m.rank >= 2 && m.prefixLength > 0 &&
            (m.type == MarkerType::ChineseParen || m.type == MarkerType::ArabicParen)) {
            int restG = GlyphCount(s + save + m.prefixLength);
            if (restG < 4) {
                lastBare = save;
            }
            i = save + m.prefixLength;
            if (i <= save) {
                Utf8CodepointNext(s, len, i);
            }
            continue;
        }
        Utf8CodepointNext(s, len, i);
        if (i <= save) {
            break;
        }
    }
    if (lastBare < 0) {
        return false;
    }
    if (lastBare == 0) {
        return true;
    }
    int prevI = lastBare;
    int prev = Utf8CodepointPrev(s, len, prevI);
    return IsEmbeddedHeadingBoundaryCp(prev);
}

static bool ShouldGlueBareEnumToNext(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!EndsWithBareEnumMarker(a.text) || HeadingLevelFromText(b.text) > 0) {
        return false;
    }
    if (!HasLetterOrCjk(b.text) || LineLooksLikePageNumber(b.text)) {
        return false;
    }
    if (a.dy > 1 && b.dy > 1) {
        float da = ScanLineMidY(b) - ScanLineMidY(a);
        if (da < 0) {
            da = -da;
        }
        float lim = a.dy > b.dy ? a.dy : b.dy;
        lim *= 3.5f;
        if (lim < 24) {
            lim = 24;
        }
        if (da > lim) {
            return false;
        }
    }
    return true;
}

static bool ShouldGlueBareDiUnitToNext(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!LineIsBareLessonOrChapter(a.text) || HeadingLevelFromText(b.text) > 0) {
        return false;
    }
    if (!HasLetterOrCjk(b.text) || LineLooksLikePageNumber(b.text) || LooksLikeArchiveJunk(b.text)) {
        return false;
    }
    if (a.dy > 1 && b.dy > 1) {
        float da = ScanLineMidY(b) - ScanLineMidY(a);
        if (da < 0) {
            da = -da;
        }
        float lim = a.dy > b.dy ? a.dy : b.dy;
        lim *= 3.5f;
        if (lim < 24) {
            lim = 24;
        }
        if (da > lim) {
            return false;
        }
    }
    return true;
}

static bool ShouldGlueCloseParenEnumToNext(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!LineIsBareCloseParenEnum(a.text) || HeadingLevelFromText(b.text) > 0) {
        return false;
    }
    if (!HasLetterOrCjk(b.text) || LineLooksLikePageNumber(b.text)) {
        return false;
    }
    if (a.dy > 1 && b.dy > 1) {
        float da = ScanLineMidY(b) - ScanLineMidY(a);
        if (da < 0) {
            da = -da;
        }
        float lim = a.dy > b.dy ? a.dy : b.dy;
        lim *= 3.5f;
        if (lim < 24) {
            lim = 24;
        }
        if (da > lim) {
            return false;
        }
    }
    return true;
}

static bool LineIsBareDottedSection(const char* s) {
    if (!s || !s[0] || HasLetterOrCjk(s)) {
        return false;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    return num.nComp >= 2;
}

static bool ScanLinesCloseEnoughToGlue(const ScanLine& a, const ScanLine& b) {
    if (a.dy <= 1 || b.dy <= 1) {
        return true;
    }
    float da = ScanLineMidY(b) - ScanLineMidY(a);
    if (da < 0) {
        da = -da;
    }
    float lim = a.dy > b.dy ? a.dy : b.dy;
    lim *= 3.5f;
    if (lim < 24) {
        lim = 24;
    }
    return da <= lim;
}

static bool ShouldGlueBareDottedSectionToNext(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!LineIsBareDottedSection(a.text) || HeadingLevelFromText(b.text) > 0) {
        return false;
    }
    if (!HasLetterOrCjk(b.text) || LineLooksLikePageNumber(b.text)) {
        return false;
    }
    ParsedNumbering nb;
    ParseHeadingNumbering(b.text, &nb);
    if (nb.nComp >= 1) {
        return false;
    }
    return ScanLinesCloseEnoughToGlue(a, b);
}

static bool ShouldGlueSplitListNumber(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!LineIsBareListNumber(a.text) || !LineStartsWithDotThenTitle(b.text)) {
        return false;
    }
    if (a.dy > 1 && b.dy > 1) {
        float da = ScanLineMidY(b) - ScanLineMidY(a);
        if (da < 0) {
            da = -da;
        }
        float lim = a.dy > b.dy ? a.dy : b.dy;
        lim *= 3.5f;
        if (lim < 24) {
            lim = 24;
        }
        if (da > lim) {
            return false;
        }
    }
    return true;
}

// PDF often puts "1、" on its own line and the URL on the next.
static bool LineIsBareDunhaoListMarker(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    int n = 0;
    int nDig = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(cp)) {
            n = n * 10 + DigitValue(cp);
            nDig++;
            if (nDig > 3) {
                return false;
            }
            continue;
        }
        i = save;
        break;
    }
    if (n < 1 || nDig < 1) {
        return false;
    }
    SkipWsUtf8(s, len, i);
    int sep = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (sep != 0x3001) {
        return false;
    }
    SkipWsUtf8(s, len, i);
    return i >= len;
}

static bool ShouldGlueDunhaoListToNext(const ScanLine& a, const ScanLine& b) {
    if (a.srcPage != b.srcPage || !a.text || !b.text) {
        return false;
    }
    if (!LineIsBareDunhaoListMarker(a.text)) {
        return false;
    }
    if (!LooksLikeUrlStart(b.text) && !LineStartsWithDotThenTitle(b.text) && !HasLetterOrCjk(b.text)) {
        return false;
    }
    return ScanLinesCloseEnoughToGlue(a, b);
}

struct InferHeadingCand {
    char* title = nullptr;
    int srcPage = 0;
    float x = 0;
    float y = 0;
    float dx = 0;
    float fontSize = 0;
    bool bold = false;
    HeadingMarker marker;
    int glyphs = 0;
    int levelGuess = 0;
    int inferredLevel = 0;
    int localScore = 0;
    int structureScore = 0;
    int transIn = 0;
    int finalScore = 0;
    int confidence = 0;
    bool dpKeep = false;
};

static void FreeInferHeadingCands(Vec<InferHeadingCand>& v) {
    for (int i = 0; i < v.Size(); i++) {
        str::Free(v[i].title);
        v[i].title = nullptr;
    }
    v.Reset();
}

static int HeadingSchemaKey(const HeadingMarker& m) {
    switch (m.type) {
        case MarkerType::Chapter:
            return 10;
        case MarkerType::Appendix:
            return 15;
        case MarkerType::ChineseDunhao:
            return 20;
        case MarkerType::Section:
            return 25;
        case MarkerType::Article:
            return 28;
        case MarkerType::ChineseParen:
            return 30;
        case MarkerType::ArabicDot:
            return m.rank <= 2 ? 35 : 40;
        case MarkerType::ArabicParen:
            return 50;
        default:
            return 0;
    }
}

struct HeadingSchema {
    int keys[12] = {};
    int nKeys = 0;
};

static HeadingSchema BuildHeadingSchema(const Vec<InferHeadingCand>& cands) {
    int seen[61] = {};
    for (int i = 0; i < cands.Size(); i++) {
        int k = HeadingSchemaKey(cands[i].marker);
        if (k > 0 && k < 61) {
            seen[k]++;
        }
    }
    HeadingSchema sc;
    for (int k = 1; k < 61 && sc.nKeys < (int)dimof(sc.keys); k++) {
        if (seen[k] > 0) {
            sc.keys[sc.nKeys++] = k;
        }
    }
    return sc;
}

static int HeadingSchemaMapKey(const HeadingSchema& sc, int key) {
    for (int i = 0; i < sc.nKeys; i++) {
        if (sc.keys[i] == key) {
            return i + 1;
        }
    }
    return 0;
}

static int HeadingSchemaMap(const HeadingSchema& sc, const HeadingMarker& m) {
    int k = HeadingSchemaKey(m);
    if (k < 1) {
        return 0;
    }
    int lvl = HeadingSchemaMapKey(sc, k);
    return lvl > 0 ? lvl : m.rank;
}

static int SequenceAdj(int last, int num) {
    if (num < 1) {
        return 0;
    }
    if (last < 1) {
        if (num == 1) {
            return 3;
        }
        if (num <= 3) {
            return 1;
        }
        if (num <= 5) {
            return 0;
        }
        return -2;
    }
    if (num == last + 1) {
        return 3;
    }
    if (num == last + 2) {
        return 1;
    }
    if (num == last) {
        return -2;
    }
    if (num == 1) {
        return 1;
    }
    if (num < last) {
        return -1;
    }
    if (num <= last + 4) {
        return 0;
    }
    return -2;
}

static int IntPercentile(Vec<int>& v, int pct) {
    int n = v.Size();
    if (n < 1) {
        return 0;
    }
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (v[j] < v[i]) {
                int t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    int idx = (n - 1) * pct / 100;
    return v[idx];
}

static int LastContentCp(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = len;
    int last = Utf8CodepointPrev(s, len, i);
    while (last > 0 && (last <= 32 || last == 0x3000)) {
        last = Utf8CodepointPrev(s, len, i);
    }
    return last;
}

static int MarkerLocalScore(MarkerType t) {
    switch (t) {
        case MarkerType::Chapter:
        case MarkerType::Appendix:
            return 7;
        case MarkerType::ChineseDunhao:
        case MarkerType::ChineseParen:
            return 5;
        case MarkerType::Section:
        case MarkerType::Article:
        case MarkerType::ArabicDot:
        case MarkerType::ArabicParen:
            return 4;
        default:
            return 0;
    }
}

static int PunctPenalty(const char* s) {
    int cp = LastContentCp(s);
    if (cp == 0x3002 || cp == '.' || cp == 0xFF0E) {
        return -4;
    }
    if (cp == 0xFF0C || cp == ',') {
        return -4;
    }
    if (cp == 0xFF1B || cp == ';' || cp == 0x3001) {
        return cp == 0x3001 ? 0 : -3;
    }
    return 0;
}

static int ScoreHeadingLocal(const InferHeadingCand& cand, int p75, int p95, int nLen, float body, float bodyDx) {
    int score = MarkerLocalScore(cand.marker.type);
    if (nLen >= 6) {
        if (p75 > 0 && cand.glyphs <= p75) {
            score += 2;
        } else if (p95 > 0 && cand.glyphs > p95) {
            score -= 3;
        }
        if (cand.glyphs > 40 && p95 >= 20 && cand.glyphs > p95) {
            score -= 1;
        }
    } else {
        if (cand.glyphs <= 20) {
            score += 2;
        } else if (cand.glyphs <= 35) {
            score += 1;
        } else if (cand.glyphs > 50) {
            score -= 2;
        }
    }
    score += PunctPenalty(cand.title);
    if (body > 4 && cand.fontSize >= body * 1.2f) {
        score += 1;
    }
    if (cand.bold) {
        score += 1;
    }
    if (bodyDx > 1 && cand.dx > 0 && cand.dx < bodyDx * 0.7f) {
        score += 1;
    }
    int key = HeadingSchemaKey(cand.marker);
    if (key < 1 && nLen >= 6 && p75 > 0 && cand.glyphs > p75) {
        score -= 20;
    }
    return score;
}

static void ApplySandwichBonus(Vec<InferHeadingCand>& cands) {
    for (int i = 0; i < cands.Size(); i++) {
        int key = HeadingSchemaKey(cands[i].marker);
        int num = cands[i].marker.number;
        if (key < 1 || num < 1) {
            continue;
        }
        bool hasPrev = false;
        bool hasNext = false;
        for (int j = 0; j < cands.Size(); j++) {
            if (j == i || HeadingSchemaKey(cands[j].marker) != key) {
                continue;
            }
            if (cands[j].marker.number == num - 1) {
                hasPrev = true;
            } else if (cands[j].marker.number == num + 1) {
                hasNext = true;
            }
        }
        if (hasPrev && hasNext) {
            cands[i].structureScore += 3;
        } else if (hasPrev || hasNext) {
            cands[i].structureScore += 1;
        }
    }
}

static int TransitionScore(const InferHeadingCand& prev, const InferHeadingCand& cur) {
    int trans = 0;
    int prevKey = HeadingSchemaKey(prev.marker);
    int curKey = HeadingSchemaKey(cur.marker);
    int prevLvl = prev.inferredLevel;
    int curLvl = cur.inferredLevel;
    if (prevLvl < 1) {
        prevLvl = prev.levelGuess;
    }
    if (curLvl < 1) {
        curLvl = cur.levelGuess;
    }
    if (prevKey > 0 && prevKey == curKey) {
        int adj = SequenceAdj(prev.marker.number, cur.marker.number);
        trans += adj * 2;
        if (adj <= -2) {
            trans -= 4;
        }
    } else if (curLvl == prevLvl + 1) {
        trans += 3;
        if (curKey > 0) {
            trans += SequenceAdj(-1, cur.marker.number);
        }
    } else if (curLvl < prevLvl && curLvl >= 1) {
        trans += 2;
        if (prevKey > 0 && prevKey == curKey) {
            int adj = SequenceAdj(prev.marker.number, cur.marker.number);
            trans += adj * 2;
        }
    } else if (curLvl == prevLvl && prevKey != curKey) {
        trans -= 2;
    } else if (curLvl > prevLvl + 1) {
        trans -= 4;
    }
    return trans;
}

static int ExtractTocModeBias() {
    const char* m = (gGlobalPrefs && gGlobalPrefs->extractPdfTocMode) ? gGlobalPrefs->extractPdfTocMode : nullptr;
    if (m && str::EqI(m, "conservative")) {
        return -3;
    }
    if (m && str::EqI(m, "detailed")) {
        return 3;
    }
    return 0;
}

static const char* MarkerTypeName(MarkerType t) {
    switch (t) {
        case MarkerType::ChineseDunhao:
            return "ChineseDunhao";
        case MarkerType::ChineseParen:
            return "ChineseParen";
        case MarkerType::ArabicDot:
            return "ArabicDot";
        case MarkerType::ArabicParen:
            return "ArabicParen";
        case MarkerType::Chapter:
            return "Chapter";
        case MarkerType::Section:
            return "Section";
        case MarkerType::Article:
            return "Article";
        case MarkerType::Appendix:
            return "Appendix";
        case MarkerType::YiShi:
            return "YiShi";
        case MarkerType::DiYiComma:
            return "DiYiComma";
        default:
            return "None";
    }
}

static void WriteTocDebugFile(const char* path, const Vec<InferHeadingCand>& cands) {
    if (!path || !path[0]) {
        return;
    }
    FILE* f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "ExtractPdfToc debug (%d candidates)\n", cands.Size());
    for (int i = 0; i < cands.Size(); i++) {
        const InferHeadingCand& c = cands[i];
        fprintf(f, "----\n");
        fprintf(f, "keep=%s score=%d conf=%d level=%d page=%d\n", c.dpKeep ? "YES" : "NO", c.finalScore, c.confidence,
                c.inferredLevel, c.srcPage);
        fprintf(f, "marker=%s num=%d\n", MarkerTypeName(c.marker.type), c.marker.number);
        fprintf(f, "local=%d struct=%d trans=%d\n", c.localScore, c.structureScore, c.transIn);
        fprintf(f, "title=%s\n", c.title ? c.title : "");
    }
    fclose(f);
}

static void RunHeadingDp(Vec<InferHeadingCand>& cands) {
    int n = cands.Size();
    if (n < 1) {
        return;
    }
    int bias = ExtractTocModeBias();
    Vec<int> keepScore;
    Vec<int> prevIdx;
    for (int i = 0; i < n; i++) {
        InferHeadingCand& cand = cands[i];
        int startAdj = 0;
        if (HeadingSchemaKey(cand.marker) > 0) {
            int adj = SequenceAdj(-1, cand.marker.number);
            startAdj = adj * 2;
            if (adj <= -2) {
                startAdj -= 4;
            }
        }
        int base = cand.localScore + cand.structureScore + bias;
        keepScore.Append(base + startAdj);
        prevIdx.Append(-1);
        cand.transIn = startAdj;
        for (int j = 0; j < i; j++) {
            int t = TransitionScore(cands[j], cand);
            int s = keepScore[j] + t + base;
            if (s > keepScore[i]) {
                keepScore[i] = s;
                prevIdx[i] = j;
                cand.transIn = t;
            }
        }
        cand.finalScore = keepScore[i];
    }
    int bestEnd = -1;
    int bestScore = 3;
    for (int i = 0; i < n; i++) {
        if (keepScore[i] > bestScore) {
            bestScore = keepScore[i];
            bestEnd = i;
        }
    }
    for (int i = bestEnd; i >= 0; i = prevIdx[i]) {
        cands[i].dpKeep = true;
        int conf = 40 + cands[i].localScore + cands[i].structureScore;
        if (cands[i].transIn > 0) {
            conf += cands[i].transIn;
        }
        if (conf < 0) {
            conf = 0;
        }
        if (conf > 100) {
            conf = 100;
        }
        cands[i].confidence = conf;
        if (prevIdx[i] < 0) {
            break;
        }
    }
}

static const char* CnNumeralUtf8(int n) {
    switch (n) {
        case 1:
            return "\xE4\xB8\x80"; // 一
        case 2:
            return "\xE4\xBA\x8C"; // 二
        case 3:
            return "\xE4\xB8\x89"; // 三
        case 4:
            return "\xE5\x9B\x9B"; // 四
        case 5:
            return "\xE4\xBA\x94"; // 五
        case 6:
            return "\xE5\x85\xAD"; // 六
        case 7:
            return "\xE4\xB8\x83"; // 七
        case 8:
            return "\xE5\x85\xAB"; // 八
        case 9:
            return "\xE4\xB9\x9D"; // 九
        case 10:
            return "\xE5\x8D\x81"; // 十
        default:
            return nullptr;
    }
}

static char* PrefixSchemaNumber(int schemaKey, int num, const char* title) {
    if (!title) {
        return nullptr;
    }
    HeadingMarker have = ParseHeadingMarker(title);
    if (have.rank >= 1 && HeadingSchemaKey(have) == schemaKey && have.number == num) {
        return str::Dup(title);
    }
    if (schemaKey == 25 || schemaKey == 28) {
        const char* cn = CnNumeralUtf8(num);
        if (!cn) {
            return str::Dup(title);
        }
        const char* unit = schemaKey == 25 ? "\xE8\x8A\x82" : "\xE6\x9D\xA1"; // 节 条
        char* pre = str::Join("\xE7\xAC\xAC", cn, unit);                      // 第
        char* out = nullptr;
        if (title[0] && title[0] != ' ') {
            out = str::Join(pre, " ", title);
        } else {
            out = str::Join(pre, title);
        }
        str::Free(pre);
        return out;
    }
    if (schemaKey == 20) {
        const char* cn = CnNumeralUtf8(num);
        if (!cn) {
            return str::Dup(title);
        }
        return str::Join(cn, "\xE3\x80\x81", title); // 、
    }
    if (schemaKey == 30) {
        const char* cn = CnNumeralUtf8(num);
        if (!cn) {
            return str::Dup(title);
        }
        char* mid = str::Join("\xEF\xBC\x88", cn, "\xEF\xBC\x89"); // （ ）
        char* out = str::Join(mid, title);
        str::Free(mid);
        return out;
    }
    if (schemaKey == 35 || schemaKey == 40 || schemaKey == 50) {
        char buf[12];
        if (schemaKey == 50) {
            snprintf(buf, (int)sizeof(buf), "(%d)", num);
        } else {
            snprintf(buf, (int)sizeof(buf), "%d.", num);
        }
        return str::Join(buf, title);
    }
    return str::Dup(title);
}

static bool CandPosInWindow(int page, float y, int p0, float y0, int p1, float y1) {
    if (page < p0 || page > p1) {
        return false;
    }
    if (page == p0 && y + 0.5f < y0) {
        return false;
    }
    if (page == p1 && y > y1 + 0.5f) {
        return false;
    }
    return true;
}

static int CandDocOrder(const InferHeadingCand& a, const InferHeadingCand& b) {
    if (a.srcPage != b.srcPage) {
        return a.srcPage < b.srcPage ? -1 : 1;
    }
    if (a.y + 0.5f < b.y) {
        return -1;
    }
    if (a.y > b.y + 0.5f) {
        return 1;
    }
    if (a.x < b.x) {
        return -1;
    }
    if (a.x > b.x) {
        return 1;
    }
    return 0;
}

static void InsertCandSorted(Vec<InferHeadingCand>& cands, InferHeadingCand& add) {
    int at = cands.Size();
    for (int i = 0; i < cands.Size(); i++) {
        if (CandDocOrder(add, cands[i]) < 0) {
            at = i;
            break;
        }
    }
    cands.InsertAt((size_t)at, add);
    add.title = nullptr;
}

static const char* HeadingTitleBody(const char* s) {
    if (!s || !s[0]) {
        return s;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    const char* body = s;
    if (m.prefixLength > 0) {
        body = s + m.prefixLength;
    }
    while (body && (*body == ' ' || *body == '\t' || (unsigned char)*body == 0xA0)) {
        body++;
    }
    return body;
}

static bool SalvageTitleTaken(const Vec<InferHeadingCand>& cands, const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    const char* body = HeadingTitleBody(s);
    if (!body || !body[0]) {
        return false;
    }
    for (int i = 0; i < cands.Size(); i++) {
        if (!cands[i].dpKeep || !cands[i].title) {
            continue;
        }
        if (str::Eq(cands[i].title, s)) {
            return true;
        }
        const char* kb = HeadingTitleBody(cands[i].title);
        if (kb && kb[0] && str::Eq(kb, body)) {
            return true;
        }
    }
    return false;
}

static bool LooksLikeGapFillTitle(const ScanLine& sl, float neighborX) {
    if (!sl.text || !sl.text[0]) {
        return false;
    }
    if (ParseHeadingMarker(sl.text).rank >= 1) {
        return false;
    }
    if (LooksLikeOfficialBoilerplate(sl.text) || LooksLikeDocNumberLine(sl.text) || LineLooksLikePageNumber(sl.text)) {
        return false;
    }
    if (str::Find(sl.text, "\xE3\x80\x82")) { // 。
        return false;
    }
    int gp = GlyphCount(sl.text);
    if (gp < 6 || gp > 24) {
        return false;
    }
    if (!HasLetterOrCjk(sl.text)) {
        return false;
    }
    float dx = sl.x - neighborX;
    if (dx < 0) {
        dx = -dx;
    }
    return dx <= 56;
}

static InferHeadingCand CandFromScanLine(const ScanLine& sl, const HeadingMarker& marker, char* title, int gp,
                                         int inferredLevel) {
    InferHeadingCand c;
    c.title = title;
    c.srcPage = sl.srcPage;
    c.x = sl.x;
    c.y = sl.y;
    c.dx = sl.dx;
    c.fontSize = sl.fontSize;
    c.bold = sl.bold;
    c.marker = marker;
    c.glyphs = gp;
    c.levelGuess = inferredLevel;
    c.inferredLevel = inferredLevel;
    c.localScore = 2;
    c.confidence = 55;
    c.dpKeep = true;
    return c;
}

struct SequenceGap {
    int schemaKey = 0;
    int number = 0;
    int inferredLevel = 1;
    int p0 = 1;
    float y0 = 0;
    int p1 = 1;
    float y1 = 0;
    float neighborX = 0;
};

static void AppendSequenceGapRange(Vec<SequenceGap>& jobs, int schemaKey, int from, int to, int inferredLevel, int p0,
                                   float y0, int p1, float y1, float neighborX) {
    if (from < 1 || to <= from || to - from > 3) {
        return;
    }
    for (int n = from; n < to; n++) {
        SequenceGap g;
        g.schemaKey = schemaKey;
        g.number = n;
        g.inferredLevel = inferredLevel;
        g.p0 = p0;
        g.y0 = y0;
        g.p1 = p1;
        g.y1 = y1;
        g.neighborX = neighborX;
        jobs.Append(g);
    }
}

static void CollectSequenceGaps(const Vec<InferHeadingCand>& cands, Vec<SequenceGap>& jobs) {
    int lastNum[61] = {};
    int lastLevel[61] = {};
    int lastPage[61] = {};
    float lastY[61] = {};
    int prevPage = 1;
    float prevY = 0;
    for (int i = 0; i < cands.Size(); i++) {
        const InferHeadingCand& c = cands[i];
        if (!c.dpKeep || !c.title) {
            continue;
        }
        int lvl = c.inferredLevel < 1 ? c.levelGuess : c.inferredLevel;
        if (lvl < 1) {
            lvl = 1;
        }
        for (int k = 1; k < 61; k++) {
            if (lastNum[k] > 0 && lastLevel[k] > lvl) {
                lastNum[k] = 0;
            }
        }
        int key = HeadingSchemaKey(c.marker);
        int num = c.marker.number;
        if (key == 10) {
            lastNum[25] = 0;
            lastLevel[25] = 0;
            lastNum[28] = 0;
            lastLevel[28] = 0;
        }
        if (key < 1 || num < 1) {
            prevPage = c.srcPage;
            prevY = c.y;
            continue;
        }
        if (lastNum[key] < 1) {
            if (num > 1 && num <= 3) {
                AppendSequenceGapRange(jobs, key, 1, num, lvl, prevPage, prevY, c.srcPage, c.y, c.x);
            }
        } else if (num == lastNum[key] + 1) {
            // consecutive
        } else if (num > lastNum[key] + 1 && num <= lastNum[key] + 4) {
            AppendSequenceGapRange(jobs, key, lastNum[key] + 1, num, lvl, lastPage[key], lastY[key], c.srcPage, c.y,
                                   c.x);
        } else if (num < lastNum[key] && num <= 4) {
            if (num > 1) {
                AppendSequenceGapRange(jobs, key, 1, num, lvl, prevPage, prevY, c.srcPage, c.y, c.x);
            }
        }
        lastNum[key] = num;
        lastLevel[key] = lvl;
        lastPage[key] = c.srcPage;
        lastY[key] = c.y;
        prevPage = c.srcPage;
        prevY = c.y;
    }
}

static void SalvageSequenceGaps(Vec<InferHeadingCand>& cands, Vec<InferHeadingCand>& rejected,
                                const Vec<ScanLine>& lines, const HeadingSchema& schema) {
    Vec<SequenceGap> jobs;
    CollectSequenceGaps(cands, jobs);
    for (int j = 0; j < jobs.Size(); j++) {
        const SequenceGap& job = jobs[j];
        bool placed = false;
        for (int i = 0; i < cands.Size(); i++) {
            InferHeadingCand& c = cands[i];
            if (c.dpKeep || !c.title || HeadingSchemaKey(c.marker) != job.schemaKey || c.marker.number != job.number) {
                continue;
            }
            if (!CandPosInWindow(c.srcPage, c.y, job.p0, job.y0, job.p1, job.y1)) {
                continue;
            }
            c.inferredLevel = job.inferredLevel;
            c.levelGuess = job.inferredLevel;
            c.dpKeep = true;
            if (c.confidence < 55) {
                c.confidence = 55;
            }
            placed = true;
            break;
        }
        if (placed) {
            continue;
        }
        for (int r = 0; r < rejected.Size(); r++) {
            InferHeadingCand& rj = rejected[r];
            if (!rj.title || HeadingSchemaKey(rj.marker) != job.schemaKey || rj.marker.number != job.number ||
                TitleIsNumberingOnly(rj.title)) {
                continue;
            }
            if (!CandPosInWindow(rj.srcPage, rj.y, job.p0, job.y0, job.p1, job.y1)) {
                continue;
            }
            if (SalvageTitleTaken(cands, rj.title)) {
                continue;
            }
            rj.inferredLevel = job.inferredLevel;
            rj.levelGuess = job.inferredLevel;
            rj.dpKeep = true;
            rj.confidence = 58;
            InsertCandSorted(cands, rj);
            placed = true;
            break;
        }
        if (placed) {
            continue;
        }
        int bestLine = -1;
        int bestScore = -1;
        for (int i = 0; i < lines.Size(); i++) {
            const ScanLine& sl = lines[i];
            if (!sl.text || !CandPosInWindow(sl.srcPage, sl.y, job.p0, job.y0, job.p1, job.y1)) {
                continue;
            }
            if (SalvageTitleTaken(cands, sl.text)) {
                continue;
            }
            HeadingMarker m = ParseHeadingMarker(sl.text);
            bool numberedHit =
                HeadingSchemaKey(m) == job.schemaKey && m.number == job.number && !TitleIsNumberingOnly(sl.text);
            bool unmarkedHit = !numberedHit && m.rank < 1 && LooksLikeGapFillTitle(sl, job.neighborX);
            if (!numberedHit && !unmarkedHit) {
                continue;
            }
            if (numberedHit) {
                bestLine = i;
                bestScore = 10000;
                break;
            }
            float dx = sl.x - job.neighborX;
            if (dx < 0) {
                dx = -dx;
            }
            int gp = GlyphCount(sl.text);
            int score = 800 - (int)dx;
            if (gp >= 8 && gp <= 18) {
                score += 20;
            }
            if (score > bestScore) {
                bestScore = score;
                bestLine = i;
            }
        }
        if (bestLine < 0) {
            continue;
        }
        const ScanLine& sl = lines[bestLine];
        char* work = DupTrimmed(sl.text);
        StripLeadingListBullet(&work);
        TrimTitleToFirstSentence(work);
        HeadingMarker m = ParseHeadingMarker(work);
        if (HeadingSchemaKey(m) != job.schemaKey || m.number != job.number) {
            char* prefixed = PrefixSchemaNumber(job.schemaKey, job.number, work);
            str::Free(work);
            work = prefixed;
            m = ParseHeadingMarker(work);
        }
        int gp = GlyphCount(work);
        if (gp < 4 || gp > kExtractPdfToc.headingMaxGlyphs || HeadingSchemaKey(m) != job.schemaKey) {
            str::Free(work);
            continue;
        }
        int lvl = HeadingSchemaMap(schema, m);
        if (lvl < 1) {
            lvl = job.inferredLevel;
        }
        InferHeadingCand add = CandFromScanLine(sl, m, work, gp, lvl);
        InsertCandSorted(cands, add);
    }
}

static bool InferHeadings(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots,
                          const char* debugPath = nullptr) {
    Vec<float> bodySizes;
    Vec<float> bodyWidths;
    for (int i = 0; i < lines.Size(); i++) {
        int g = GlyphCount(lines[i].text);
        if (g >= 12 && lines[i].fontSize > 1) {
            bodySizes.Append(lines[i].fontSize);
        }
        if (g >= 12 && lines[i].dx > 1) {
            bodyWidths.Append(lines[i].dx);
        }
    }
    float body = MedianPositive(bodySizes);
    if (body < 4) {
        body = 10;
    }
    float bodyDx = MedianPositive(bodyWidths);
    StrVec headers;
    CollectHeaderTexts(lines, nPages, headers);
    Vec<int> nArab3;
    Vec<int> nArab4;
    for (int p = 0; p <= nPages; p++) {
        nArab3.Append(0);
        nArab4.Append(0);
    }
    int nArab3All = 0;
    int nArab4All = 0;
    int nOutline = 0;
    for (int i = 0; i < lines.Size(); i++) {
        if (!lines[i].text || lines[i].srcPage < 1 || lines[i].srcPage > nPages) {
            continue;
        }
        HeadingMarker hm = ParseHeadingMarker(lines[i].text);
        int hl = hm.rank > 0 ? hm.rank : HeadingLevelFromText(lines[i].text);
        if (hl == 1 || hl == 2) {
            nOutline++;
        } else if (hm.type == MarkerType::ArabicDot && hm.rank >= 3) {
            nArab3[lines[i].srcPage]++;
            nArab3All++;
        } else if (hm.type == MarkerType::ArabicParen) {
            nArab4[lines[i].srcPage]++;
            nArab4All++;
        }
    }
    Vec<InferHeadingCand> cands;
    Vec<InferHeadingCand> rejected;
    int skipMerged = -1;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (i == skipMerged) {
            continue;
        }
        if (!sl.text) {
            continue;
        }
        if (PageLooksLikeCipOrColophon(lines, sl.srcPage)) {
            continue;
        }
        char* merged = nullptr;
        const char* title = sl.text;
        int g = GlyphCount(sl.text);
        if (i + 1 < lines.Size() &&
            (ShouldGlueSplitListNumber(sl, lines[i + 1]) || ShouldGlueDunhaoListToNext(sl, lines[i + 1]) ||
             ShouldGlueBareEnumToNext(sl, lines[i + 1]))) {
            merged = str::Join(sl.text, lines[i + 1].text);
            if (merged) {
                title = merged;
                skipMerged = i + 1;
                g = GlyphCount(title);
            }
        }
        if (g < 2) {
            str::Free(merged);
            continue;
        }
        if (LooksLikeOfficialBoilerplate(title) || LooksLikeDocNumberLine(title) || LooksLikeLeaderTitle(title)) {
            str::Free(merged);
            continue;
        }
        if (g > kExtractPdfToc.headingMaxGlyphs) {
            bool keepLongNum = false;
            int gpProbe = 0;
            int numberedLong = 0;
            if (HeadingLevelFromText(title) > 0) {
                numberedLong = 1;
                char* probe = DupTrimmed(title);
                TrimTitleToFirstSentence(probe);
                gpProbe = GlyphCount(probe);
                keepLongNum = gpProbe >= 2 && gpProbe <= kExtractPdfToc.headingMaxGlyphs;
                str::Free(probe);
            }
            if (!keepLongNum && FindNextEmbeddedHeading(title) >= 1) {
                keepLongNum = true;
            }
            if (!keepLongNum) {
                str::Free(merged);
                continue;
            }
        }
        if (!merged &&
            (LineLooksLikePageNumber(sl.text) || IsHeaderText(headers, sl.text) || LooksLikeArchiveJunk(sl.text))) {
            continue;
        }
        if (LooksLikePrintedTocHeading(title)) {
            str::Free(merged);
            continue;
        }
        char* extraMerge = nullptr;
        if (!merged && i + 1 < lines.Size() && ShouldMergeFalseYiDunhaoWrap(sl, lines[i + 1])) {
            extraMerge = str::Join(sl.text, lines[i + 1].text);
            if (extraMerge) {
                title = extraMerge;
                skipMerged = i + 1;
                g = GlyphCount(title);
            }
        }
        if (!extraMerge && !merged && i + 1 < lines.Size() &&
            (ShouldMergeHeadingLines(sl, lines[i + 1]) || ShouldGlueBareDiUnitToNext(sl, lines[i + 1]))) {
            extraMerge = str::Join(sl.text, " ", lines[i + 1].text);
            if (extraMerge) {
                title = extraMerge;
                skipMerged = i + 1;
                g = GlyphCount(title);
            }
        }
        if (!merged) {
            merged = extraMerge;
        } else {
            str::Free(extraMerge);
        }
        char* parts[12] = {};
        int nParts = 0;
        const char* src = title;
        while (src && src[0] && nParts < (int)dimof(parts)) {
            int sp = FindNextEmbeddedHeading(src);
            if (sp < 1) {
                parts[nParts++] = str::Dup(src);
                break;
            }
            parts[nParts] = str::Dup(src, (size_t)sp);
            str::TrimWSInPlace(parts[nParts], str::TrimOpt::Both);
            nParts++;
            src += sp;
        }
        str::Free(merged);
        merged = nullptr;
        for (int pi = 0; pi < nParts; pi++) {
            const char* piece = parts[pi];
            if (!piece || !piece[0]) {
                str::Free(parts[pi]);
                parts[pi] = nullptr;
                continue;
            }
            char* work = nullptr;
            HeadingMarker marker = ParseHeadingMarker(piece);
            int numbered = marker.rank > 0 ? marker.rank : HeadingLevelFromText(piece);
            const char* titlePiece = piece;
            int gp = GlyphCount(piece);
            if (numbered > 0) {
                work = DupTrimmed(piece);
                StripLeadingListBullet(&work);
                TrimTitleToFirstSentence(work);
                StripGluedBodyAfterHeading(work);
                titlePiece = work;
                gp = GlyphCount(titlePiece);
                marker = ParseHeadingMarker(titlePiece);
                numbered = marker.rank > 0 ? marker.rank : numbered;
            }
            if (numbered > 0 && TitleIsNumberingOnly(titlePiece)) {
                InferHeadingCand drop;
                drop.title = work ? work : str::Dup(piece);
                work = nullptr;
                drop.srcPage = sl.srcPage;
                drop.x = sl.x;
                drop.y = sl.y + (float)pi * 0.01f;
                drop.dx = sl.dx;
                drop.fontSize = sl.fontSize;
                drop.bold = sl.bold;
                drop.marker = marker;
                drop.glyphs = gp;
                drop.levelGuess = numbered;
                rejected.Append(drop);
                str::Free(parts[pi]);
                parts[pi] = nullptr;
                continue;
            }
            int lvl = 0;
            bool weakArabic = false;
            if (numbered > 0) {
                int len = (int)str::Len(titlePiece);
                int ti = 0;
                int first = Utf8CodepointNext(titlePiece, len, ti);
                while (first > 0 && (first <= 32 || first == 0x3000)) {
                    first = Utf8CodepointNext(titlePiece, len, ti);
                }
                weakArabic = IsDigitCp(first);
                if (weakArabic && HasLetterOrCjk(titlePiece)) {
                    weakArabic = false;
                }
            }
            if (numbered > 0 && !weakArabic) {
                if (gp <= kExtractPdfToc.headingMaxGlyphs) {
                    lvl = numbered;
                }
            } else if (numbered > 0 && weakArabic) {
                bool narrow = bodyDx > 0 && sl.dx > 0 && sl.dx < bodyDx * 0.7f;
                bool larger = sl.fontSize >= body * 1.2f;
                if (gp <= 28 && (larger || sl.bold || narrow)) {
                    lvl = numbered;
                }
            } else if (!str::Find(titlePiece, "\xE3\x80\x82") && !str::Find(titlePiece, "\xEF\xBC\x9A")) {
                bool hasCjk = false;
                int len = (int)str::Len(titlePiece);
                int ti = 0;
                while (ti < len) {
                    int cp = Utf8CodepointNext(titlePiece, len, ti);
                    if (cp >= 0x4E00 && cp <= 0x9FFF) {
                        hasCjk = true;
                        break;
                    }
                }
                bool narrow = bodyDx > 0 && sl.dx > 0 && sl.dx < bodyDx * 0.6f;
                bool bigger = sl.fontSize >= body * 1.45f;
                bool unnumbered = nOutline < 2 && hasCjk && gp >= 8 && gp <= 18 && bigger &&
                                  (narrow || sl.bold || sl.fontSize >= body * 1.7f);
                if (unnumbered) {
                    lvl = sl.fontSize >= body * 1.7f ? 1 : 2;
                }
            }
            int dropArabAll = 0;
            int dropDense = 0;
            int dropNarrow = 0;
            // CJK/letter 1. 2. 3. lines are 公文 outline headings, not table cells.
            // A full 公文 L3 is often one sentence (做好…、会同…), longer than 24 glyphs.
            bool outlineList = gp >= 4 && (HasLetterOrCjk(titlePiece) || str::Find(titlePiece, "http") ||
                                           str::Find(titlePiece, "ftp://"));
            if (marker.type == MarkerType::ArabicDot && marker.rank >= 3 && nArab3All >= 24) {
                if (!(outlineList && gp <= kExtractPdfToc.headingMaxGlyphs)) {
                    lvl = 0;
                    dropArabAll = 1;
                }
            }
            if (marker.type == MarkerType::ArabicParen && nArab4All >= 16) {
                lvl = 0;
            }
            if (lvl >= 3 && numbered >= 3 && sl.srcPage >= 1 && sl.srcPage <= nPages) {
                bool dense = (numbered == 3 && nArab3[sl.srcPage] >= 8) || (numbered == 4 && nArab4[sl.srcPage] >= 8);
                bool narrowCell = bodyDx > 0 && sl.dx > 0 && sl.dx < bodyDx * 0.55f;
                if ((dense || narrowCell) && !outlineList) {
                    dropDense = dense ? 1 : 0;
                    dropNarrow = narrowCell ? 1 : 0;
                    lvl = 0;
                }
            }
            if (lvl < 1) {
                if (numbered > 0 && marker.number >= 1 && HeadingSchemaKey(marker) > 0) {
                    InferHeadingCand drop;
                    drop.title = work ? work : str::Dup(piece);
                    work = nullptr;
                    drop.srcPage = sl.srcPage;
                    drop.x = sl.x;
                    drop.y = sl.y + (float)pi * 0.01f;
                    drop.dx = sl.dx;
                    drop.fontSize = sl.fontSize;
                    drop.bold = sl.bold;
                    drop.marker = marker;
                    drop.glyphs = gp;
                    drop.levelGuess = numbered;
                    rejected.Append(drop);
                } else {
                    str::Free(work);
                }
                str::Free(parts[pi]);
                parts[pi] = nullptr;
                continue;
            }
            InferHeadingCand cand;
            cand.title = work ? work : parts[pi];
            if (work) {
                work = nullptr;
                str::Free(parts[pi]);
            }
            parts[pi] = nullptr;
            cand.srcPage = sl.srcPage;
            cand.x = sl.x;
            cand.y = sl.y + (float)pi * 0.01f;
            cand.dx = sl.dx;
            cand.fontSize = sl.fontSize;
            cand.bold = sl.bold;
            cand.marker = marker;
            cand.glyphs = gp;
            cand.levelGuess = lvl;
            cands.Append(cand);
            if (cands.Size() >= 2000) {
                for (int k = pi + 1; k < nParts; k++) {
                    str::Free(parts[k]);
                    parts[k] = nullptr;
                }
                break;
            }
        }
        if (cands.Size() >= 2000) {
            break;
        }
    }
    HeadingSchema schema = BuildHeadingSchema(cands);
    Vec<int> lens;
    for (int i = 0; i < cands.Size(); i++) {
        if (HeadingSchemaKey(cands[i].marker) > 0) {
            lens.Append(cands[i].glyphs);
        }
    }
    int p75 = IntPercentile(lens, 75);
    int p95 = IntPercentile(lens, 95);
    for (int i = 0; i < cands.Size(); i++) {
        InferHeadingCand& cand = cands[i];
        int key = HeadingSchemaKey(cand.marker);
        if (key > 0) {
            cand.inferredLevel = HeadingSchemaMap(schema, cand.marker);
        } else {
            cand.inferredLevel = cand.levelGuess;
        }
        cand.localScore = ScoreHeadingLocal(cand, p75, p95, lens.Size(), body, bodyDx);
    }
    ApplySandwichBonus(cands);
    RunHeadingDp(cands);
    SalvageSequenceGaps(cands, rejected, lines, schema);
    for (int i = 0; i < cands.Size(); i++) {
        if (cands[i].dpKeep && TitleIsNumberingOnly(cands[i].title)) {
            cands[i].dpKeep = false;
        }
    }
    WriteTocDebugFile(debugPath, cands);
    Vec<ExtractedTocItem*> flat;
    int prevLevel = 1;
    int nNumberedKept = 0;
    for (int i = 0; i < cands.Size(); i++) {
        InferHeadingCand& cand = cands[i];
        if (!cand.dpKeep) {
            continue;
        }
        int key = HeadingSchemaKey(cand.marker);
        int numbered = key > 0 ? 1 : 0;
        int lvl = cand.inferredLevel;
        if (lvl < 1) {
            continue;
        }
        lvl = ClampOutlineLevel(numbered, lvl, prevLevel, flat.Size() > 0);
        prevLevel = lvl;
        if (numbered > 0) {
            nNumberedKept++;
        }
        flat.Append(NewItem(cand.title, cand.srcPage, cand.x, cand.y, lvl, cand.confidence));
    }
    FreeInferHeadingCands(cands);
    FreeInferHeadingCands(rejected);
    if (flat.Size() < 1 || (nNumberedKept < 1 && flat.Size() < 3)) {
        for (int i = 0; i < flat.Size(); i++) {
            delete flat[i];
        }
        return false;
    }
    EnforceMonotonicPages(flat);
    BuildTreeFromFlat(flat, roots);
    return CountExtracted(roots) >= 1;
}

static ScanLine TestScanLine(const char* t, float y) {
    ScanLine sl;
    sl.text = str::Dup(t);
    sl.srcPage = 1;
    sl.x = 72;
    sl.y = y;
    sl.dx = 200;
    sl.dy = 14;
    sl.fontSize = 12;
    sl.bold = false;
    return sl;
}

static ScanLine TestScanLineXY(const char* t, float x, float y) {
    ScanLine sl = TestScanLine(t, y);
    sl.x = x;
    return sl;
}

static ScanLine TestScanLineXYP(const char* t, int page, float x, float y) {
    ScanLine sl = TestScanLineXY(t, x, y);
    sl.srcPage = page;
    return sl;
}

static bool ExtractedHasPrefix(const Vec<ExtractedTocItem*>& nodes, const char* prefix) {
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i]->title && str::StartsWith(nodes[i]->title, prefix)) {
            return true;
        }
        if (ExtractedHasPrefix(nodes[i]->children, prefix)) {
            return true;
        }
    }
    return false;
}

static ExtractedTocItem* ExtractedFindContaining(const Vec<ExtractedTocItem*>& nodes, const char* needle) {
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i]->title && needle && str::Find(nodes[i]->title, needle)) {
            return nodes[i];
        }
        ExtractedTocItem* c = ExtractedFindContaining(nodes[i]->children, needle);
        if (c) {
            return c;
        }
    }
    return nullptr;
}

static bool ExtractedHasPrintedTocBookmark(const Vec<ExtractedTocItem*>& roots, int page) {
    for (int i = 0; i < roots.Size(); i++) {
        const char* t = roots[i]->title;
        if (!t) {
            continue;
        }
        bool name = str::Eq(t, "目录") || str::Eq(t, "目次") || str::EqI(t, "Contents");
        if (name && (page < 1 || roots[i]->pageNo == page)) {
            return true;
        }
    }
    return false;
}

static bool ExtractedIsRootContaining(const Vec<ExtractedTocItem*>& roots, const char* needle) {
    for (int i = 0; i < roots.Size(); i++) {
        if (roots[i]->title && needle && str::Find(roots[i]->title, needle)) {
            return true;
        }
    }
    return false;
}

static bool BookPrintedNestOk(const Vec<ExtractedTocItem*>& roots) {
    ExtractedTocItem* part = ExtractedFindContaining(roots, "成女儿的状元路");
    ExtractedTocItem* xd1 = ExtractedFindContaining(roots, "心得一");
    return part && part->level == 1 && ExtractedHasPrefix(part->children, "心得一") && xd1 &&
           ExtractedHasPrefix(xd1->children, "一、与孩子一起制订") &&
           ExtractedHasPrefix(xd1->children, "二、总结是为了提高") &&
           ExtractedIsRootContaining(roots, "圆张琼的北大梦") && !ExtractedIsRootContaining(roots, "心得一") &&
           !ExtractedIsRootContaining(roots, "一、与孩子");
}

static void LogBookExtractFail(const char* name, const Vec<ExtractedTocItem*>& roots) {
    logf("BookToc fail %s roots=%d\n", name, roots.Size());
    for (int i = 0; i < roots.Size() && i < 14; i++) {
        logf("  [%d] L%d %s kids=%d\n", i, roots[i]->level, roots[i]->title ? roots[i]->title : "?",
             roots[i]->children.Size());
        for (int c = 0; c < roots[i]->children.Size() && c < 5; c++) {
            logf("    - L%d %s\n", roots[i]->children[c]->level,
                 roots[i]->children[c]->title ? roots[i]->children[c]->title : "?");
        }
    }
}

static void RunPrintedTocLogicTestsPhase2(int* pass, int* fail, int* failMask) {
    {
        InferHeadingCand a;
        InferHeadingCand b;
        a.marker = ParseHeadingMarker("（一）重点招生范围");
        b.marker = ParseHeadingMarker("1.单位申报");
        Vec<InferHeadingCand> cands;
        cands.Append(a);
        cands.Append(b);
        HeadingSchema sc = BuildHeadingSchema(cands);
        int lp = HeadingSchemaMap(sc, a.marker);
        int la = HeadingSchemaMap(sc, b.marker);
        bool ok = HeadingLevelFromText("（一）重点招生范围") == 2 && HeadingLevelFromText("1.单位申报") == 3 &&
                  a.marker.type == MarkerType::ChineseParen && b.marker.type == MarkerType::ArabicDot && lp == 1 &&
                  la == 2 && sc.nKeys == 2;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 20);
        }
    }
    {
        bool ok = SequenceAdj(-1, 1) == 3 && SequenceAdj(-1, 8) == -2 && SequenceAdj(1, 2) == 3 &&
                  SequenceAdj(2, 8) == -2 && SequenceAdj(2, 2) == -2 && SequenceAdj(5, 1) == 1 &&
                  SequenceAdj(3, 5) == 1 && SequenceAdj(3, 7) == 0;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 21);
        }
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("（一）重点招生范围", 80));
        lines.Append(TestScanLine("1.单位申报", 100));
        lines.Append(TestScanLine("2.业主申报", 120));
        lines.Append(TestScanLine("（二）补充招生范围", 140));
        lines.Append(TestScanLine("1.申报园所", 160));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool ok = ran && roots.Size() == 2 && roots[0]->level == 1 && roots[1]->level == 1 &&
                  str::StartsWith(roots[0]->title, "（一）") && str::StartsWith(roots[1]->title, "（二）") &&
                  roots[0]->children.Size() == 2 && roots[0]->children[0]->level == 2 &&
                  str::StartsWith(roots[0]->children[0]->title, "1.") &&
                  str::StartsWith(roots[0]->children[1]->title, "2.") && roots[1]->children.Size() == 1 &&
                  str::StartsWith(roots[1]->children[0]->title, "1.");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 22);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 40));
        lines.Append(TestScanLine("二、编制依据", 60));
        lines.Append(TestScanLine("三、编制原则", 80));
        ScanLine t1 = TestScanLineXYP("人力资源和社会保障标准体系", 2, 90, 40);
        t1.fontSize = 20;
        t1.dx = 200;
        t1.bold = true;
        ScanLine t2 = TestScanLineXYP("（2020年）编制说明", 2, 110, 62);
        t2.fontSize = 20;
        t2.dx = 160;
        t2.bold = true;
        ScanLine a1 = TestScanLineXYP("一、编制背景", 2, 72, 100);
        a1.fontSize = 13;
        ScanLine a2 = TestScanLineXYP("二、编制过程", 2, 72, 120);
        a2.fontSize = 13;
        ScanLine body = TestScanLineXYP("党中央高度重视标准化工作需要建立完善的标准体系支撑高质量发展", 2, 72, 160);
        body.fontSize = 11;
        body.dx = 420;
        lines.Append(t1);
        lines.Append(t2);
        lines.Append(a1);
        lines.Append(a2);
        lines.Append(body);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 2, roots);
        InsertOfficialDocTitles(lines, 2, roots);
        ExtractedTocItem* note = ExtractedFindContaining(roots, "编制说明");
        bool ok = ran && ExtractedIsRootContaining(roots, "一、总体要求") && note &&
                  ExtractedFindContaining(note->children, "编制背景") &&
                  !ExtractedIsRootContaining(roots, "一、编制背景");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-file-title", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 40));
        lines.Append(TestScanLine("二、编制依据", 60));
        ScanLine t1 = TestScanLineXYP("人力资源和社会保障标准体系", 2, 90, 40);
        t1.fontSize = 20;
        t1.bold = true;
        ScanLine t2 = TestScanLineXYP("（2020年）编制说明", 2, 110, 62);
        t2.fontSize = 20;
        t2.bold = true;
        ScanLine fake = TestScanLineXYP("标准体系结构图是人力资源和社会保障标准体系的核心", 2, 72, 85);
        fake.fontSize = 20;
        fake.bold = true;
        ScanLine a1 = TestScanLineXYP("一、编制背景", 2, 72, 100);
        a1.fontSize = 13;
        ScanLine from = TestScanLineXYP("从2017年10月起", 2, 72, 160);
        from.fontSize = 20;
        ScanLine cite = TestScanLineXYP("于建立健全基本公共服务标准体系的指导意见", 2, 72, 180);
        cite.fontSize = 20;
        ScanLine part = TestScanLineXYP("组成部分", 2, 72, 200);
        part.fontSize = 18;
        part.bold = true;
        lines.Append(t1);
        lines.Append(t2);
        lines.Append(fake);
        lines.Append(a1);
        lines.Append(from);
        lines.Append(cite);
        lines.Append(part);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 2, roots);
        InsertOfficialDocTitles(lines, 2, roots);
        bool ok = ran && ExtractedIsRootContaining(roots, "一、总体要求") &&
                  ExtractedFindContaining(roots, "编制说明") && ExtractedFindContaining(roots, "编制背景") &&
                  !ExtractedFindContaining(roots, "标准体系结构图是") && !ExtractedFindContaining(roots, "从2017") &&
                  !ExtractedFindContaining(roots, "于建立健全") && !ExtractedFindContaining(roots, "组成部分");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-drop-body-junk", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine header = TestScanLineXYP("山东省人力资源和社会保障厅", 1, 80, 20);
        header.fontSize = 18;
        ScanLine han = TestScanLineXYP("关于赴贵州省开展人社一体化信息平台建设工作调研的函", 1, 72, 50);
        han.fontSize = 16;
        han.bold = true;
        ScanLine yi = TestScanLineXYP("一、调研内容", 1, 72, 90);
        yi.fontSize = 14;
        ScanLine er = TestScanLineXYP("二、时间安排", 1, 72, 110);
        er.fontSize = 14;
        ScanLine body = TestScanLineXYP("根据工作安排现赴贵州省开展人社一体化信息平台建设调研工作", 1, 72, 140);
        body.fontSize = 12;
        body.dx = 400;
        lines.Append(header);
        lines.Append(han);
        lines.Append(yi);
        lines.Append(er);
        lines.Append(body);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        InsertOfficialDocTitles(lines, 1, roots);
        ExtractedTocItem* letter = ExtractedFindContaining(roots, "调研的函");
        bool ok = ran && letter && ExtractedFindContaining(letter->children, "调研内容") &&
                  ExtractedFindContaining(letter->children, "时间安排") &&
                  !ExtractedIsRootContaining(roots, "一、调研内容") &&
                  !ExtractedFindContaining(roots, "山东省人力资源和社会保障厅");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-han-title", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine red = TestScanLineXYP("人力资源社会保障部文件", 1, 72, 20);
        red.fontSize = 22;
        ScanLine num = TestScanLineXYP("人社部发〔2020〕83号", 1, 72, 50);
        num.fontSize = 12;
        ScanLine t1 = TestScanLineXYP("人力资源社会保障部关于印发", 1, 72, 80);
        t1.fontSize = 18;
        t1.bold = true;
        ScanLine t2 = TestScanLineXYP("《人力资源社会保障信息化便民服务", 1, 72, 100);
        t2.fontSize = 18;
        t2.bold = true;
        ScanLine t3 = TestScanLineXYP("创新提升行动方案》的通知", 1, 72, 120);
        t3.fontSize = 18;
        t3.bold = true;
        ScanLine body = TestScanLineXYP("各省自治区直辖市人力资源社会保障厅局", 1, 72, 160);
        body.fontSize = 12;
        body.dx = 400;
        ScanLine yi = TestScanLineXYP("一、工作目标", 2, 72, 80);
        yi.fontSize = 14;
        ScanLine er = TestScanLineXYP("二、重点任务", 2, 72, 100);
        er.fontSize = 14;
        lines.Append(red);
        lines.Append(num);
        lines.Append(t1);
        lines.Append(t2);
        lines.Append(t3);
        lines.Append(body);
        lines.Append(yi);
        lines.Append(er);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 2, roots);
        InsertOfficialDocTitles(lines, 2, roots);
        ExtractedTocItem* note = ExtractedFindContaining(roots, "创新提升行动方案");
        bool ok = ran && note && ExtractedFindContaining(note->children, "工作目标") &&
                  ExtractedFindContaining(note->children, "重点任务") &&
                  !ExtractedIsRootContaining(roots, "一、工作目标");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-main-tongzhi", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine han = TestScanLineXYP(
            "关于征求《江西省人民政府中国移动通信集团有限公司深化战略合作协议（征求意见稿）》意见的函", 1, 72, 50);
        han.fontSize = 16;
        han.bold = true;
        lines.Append(han);
        Vec<ExtractedTocItem*> roots;
        roots.Append(
            NewItem("附件：1.江西省人民政府中国移动通信集团有限公司深化战略合作协议（征求意见稿）", 1, 72, 200, 1));
        roots.Append(NewItem("2.有关单位名单", 1, 72, 220, 1));
        roots.Append(NewItem("江西省人民政府中国移动通信集团有限公司深化战略合作协议（征求意见稿）", 2, 72, 80, 1));
        InsertOfficialDocTitles(lines, 1, roots);
        ExtractedTocItem* letter = ExtractedFindContaining(roots, "意见的函");
        bool ok = letter && letter->title && str::StartsWith(letter->title, "关于征求") && roots.Size() >= 1 &&
                  roots[0] == letter && ExtractedFindContaining(letter->children, "附件");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-zhengqiu-han", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine header = TestScanLineXYP("省推进政府职能转变和数字政府建设领导小组汇报材料", 1, 72, 30);
        header.fontSize = 16;
        ScanLine title = TestScanLineXYP("关于数字政府建设2025年考核结果及2026年考核指标的情况说明", 1, 72, 70);
        title.fontSize = 18;
        title.bold = true;
        ScanLine body = TestScanLineXYP("按照省委综合考核委部署现将有关情况说明如下", 1, 72, 110);
        body.fontSize = 12;
        body.dx = 400;
        ScanLine yi = TestScanLineXYP("一、2025年考核实施情况", 1, 72, 140);
        yi.fontSize = 14;
        ScanLine er = TestScanLineXYP("二、2025年考核结果分析", 1, 72, 160);
        er.fontSize = 14;
        lines.Append(header);
        lines.Append(title);
        lines.Append(body);
        lines.Append(yi);
        lines.Append(er);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        InsertOfficialDocTitles(lines, 1, roots);
        ExtractedTocItem* main = ExtractedFindContaining(roots, "情况说明");
        bool ok = ran && main && ExtractedFindContaining(main->children, "考核实施") &&
                  ExtractedFindContaining(main->children, "考核结果分析") &&
                  !ExtractedIsRootContaining(roots, "一、2025") && !ExtractedIsRootContaining(roots, "汇报材料");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-main-qingkuang", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、调研内容", 40));
        lines.Append(TestScanLine("二、时间安排", 60));
        ScanLine glued = TestScanLineXYP("附件：调研提纲 资源和", 2, 72, 30);
        ScanLine fu = TestScanLineXYP("附件", 2, 200, 50);
        fu.fontSize = 14;
        ScanLine name = TestScanLineXYP("调研提纲", 2, 180, 72);
        name.fontSize = 18;
        name.bold = true;
        ScanLine a1 = TestScanLineXYP("一、一体化信息平台建设整体情况", 2, 72, 100);
        a1.fontSize = 13;
        ScanLine a2 = TestScanLineXYP("二、业务统一与协同联动情况", 2, 72, 120);
        a2.fontSize = 13;
        lines.Append(glued);
        lines.Append(fu);
        lines.Append(name);
        lines.Append(a1);
        lines.Append(a2);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 2, roots);
        InsertOfficialDocTitles(lines, 2, roots);
        bool ok = ran && ExtractedFindContaining(roots, "调研提纲") && !ExtractedFindContaining(roots, "资源和") &&
                  ExtractedFindContaining(roots, "一体化信息平台") && ExtractedIsRootContaining(roots, "一、调研内容");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-fujian-tigang", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        char* cut = str::Dup("1.全省人社各业务领域政策与业务流程统一情况");
        StripGluedBodyAfterHeading(cut);
        bool cutOk = cut && str::Find(cut, "各业务领域");
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、一体化信息平台建设整体情况", 40));
        lines.Append(TestScanLine("二、业务统一与协同联动情况", 60));
        lines.Append(TestScanLine("1.全省人社各业务领域政策与业务流程统一情况", 80));
        lines.Append(TestScanLine("2.省人社网办大厅与省政务服务网整合规划及工作进展", 100));
        lines.Append(TestScanLine("3.市级层面保留部署系统情况", 120));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        ExtractedTocItem* sec2 = ExtractedFindContaining(roots, "业务统一");
        bool ok = cutOk && ran && sec2 && sec2->children.Size() >= 3 &&
                  ExtractedFindContaining(sec2->children, "各业务领域") &&
                  ExtractedFindContaining(sec2->children, "网办大厅") &&
                  ExtractedFindContaining(sec2->children, "市级层面");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-keep-ge-heading", roots);
        }
        str::Free(cut);
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、一体化信息平台建设整体情况", 40));
        ScanLine sec2 = TestScanLineXYP("二、业务统一与协同联动情况", 1, 72, 60);
        sec2.fontSize = 14;
        ScanLine item1 =
            TestScanLineXYP("1.全省人社各业务领域政策与业务流程统一情况业务财务一体化电子档案三口合", 1, 72, 80);
        item1.fontSize = 12;
        item1.dx = 420;
        ScanLine wrap = TestScanLineXYP("一、电子证照等建设及应用情况；新业务模式对各级经办机构设置", 1, 72, 96);
        wrap.fontSize = 12;
        wrap.dx = 420;
        ScanLine item2 = TestScanLineXYP("2.省人社网办大厅与省政务服务网整合规划及工作进展", 1, 72, 112);
        item2.fontSize = 12;
        lines.Append(sec2);
        lines.Append(item1);
        lines.Append(wrap);
        lines.Append(item2);
        bool mergeOk = ShouldMergeFalseYiDunhaoWrap(item1, wrap);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        ExtractedTocItem* sec = ExtractedFindContaining(roots, "业务统一");
        bool ok = mergeOk && ran && sec && sec->children.Size() == 2 &&
                  !ExtractedIsRootContaining(roots, "一、电子证照") && !ExtractedHasPrefix(roots, "一、电子证照") &&
                  ExtractedFindContaining(sec->children, "三口合一") &&
                  ExtractedFindContaining(sec->children, "网办大厅");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-false-yi-wrap", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、自查采集工具下载", 40));
        ScanLine n1 = TestScanLineXYP("1、", 1, 72, 60);
        ScanLine u1 = TestScanLineXYP("https://pan.baidu.com/s/1zWbz5UCUTp5gqmCCahQ3jw", 1, 72, 76);
        ScanLine code1 = TestScanLineXYP("提取码：23py", 1, 72, 92);
        ScanLine n2 = TestScanLineXYP("2、", 1, 72, 108);
        ScanLine u2 = TestScanLineXYP("https://pan.baidu.com/s/1jsEznmNDSTx3UosKgPw6Gg", 1, 72, 124);
        ScanLine n4 = TestScanLineXYP("4、或使用微信扫描以下二维码获取：", 1, 72, 156);
        lines.Append(n1);
        lines.Append(u1);
        lines.Append(code1);
        lines.Append(n2);
        lines.Append(u2);
        lines.Append(n4);
        bool glueOk = ShouldGlueDunhaoListToNext(n1, u1);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        ExtractedTocItem* sec = ExtractedFindContaining(roots, "自查采集");
        bool ok = glueOk && ran && sec && sec->children.Size() >= 3 &&
                  ExtractedFindContaining(sec->children, "pan.baidu.com/s/1zWbz5") &&
                  ExtractedFindContaining(sec->children, "pan.baidu.com/s/1jsEz") &&
                  ExtractedFindContaining(sec->children, "微信扫描");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-url-list", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine ap = TestScanLineXYP("附件1", 3, 72, 30);
        ap.fontSize = 14;
        ScanLine docTitle = TestScanLineXYP("2021年网络安全自查情况汇总表", 3, 72, 55);
        docTitle.fontSize = 18;
        docTitle.bold = true;
        ScanLine body = TestScanLineXYP("各单位应如实填写本表并于规定时限内报送", 3, 72, 80);
        body.fontSize = 12;
        body.dx = 400;
        lines.Append(ap);
        lines.Append(docTitle);
        lines.Append(body);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 3, roots);
        InsertOfficialDocTitles(lines, 3, roots);
        ExtractedTocItem* ap1 = ExtractedFindContaining(roots, "附件1");
        bool ok = ran && ap1 && ExtractedFindContaining(ap1->children, "汇总表") && ap1->children.Size() == 1;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-fujian1-title", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine ap = TestScanLineXYP("附件2", 3, 72, 30);
        ap.fontSize = 14;
        ScanLine docTitle = TestScanLineXYP("2021年公安机关网络安全监督检查自查工作说明", 3, 72, 55);
        docTitle.fontSize = 18;
        docTitle.bold = true;
        ScanLine body = TestScanLineXYP("为做好网络安全监督检查工作现就有关事项说明如下", 3, 72, 80);
        body.fontSize = 12;
        body.dx = 400;
        ScanLine yi = TestScanLineXYP("一、自查采集工具下载", 3, 72, 110);
        yi.fontSize = 14;
        ScanLine er = TestScanLineXYP("二、自查数据填报", 3, 72, 130);
        er.fontSize = 14;
        lines.Append(ap);
        lines.Append(docTitle);
        lines.Append(body);
        lines.Append(yi);
        lines.Append(er);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 3, roots);
        InsertOfficialDocTitles(lines, 3, roots);
        ExtractedTocItem* ap2 = ExtractedFindContaining(roots, "附件2");
        bool ok = ran && ap2 && ExtractedFindContaining(ap2->children, "工作说明") &&
                  ExtractedFindContaining(ap2->children, "自查采集") &&
                  ExtractedFindContaining(ap2->children, "自查数据");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-fujian2-title", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 20));
        for (int n = 1; n <= 18; n++) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%d.占位任务%d", n, n);
            lines.Append(TestScanLine(buf, 30.f + (float)n * 8));
        }
        ScanLine sec = TestScanLineXYP("三、工作步骤和时间安排", 2, 72, 40);
        ScanLine er = TestScanLineXYP("（二）组织实施阶段", 2, 72, 60);
        lines.Append(sec);
        lines.Append(er);
        lines.Append(TestScanLineXYP("1.召开动员部署会", 2, 72, 80));
        lines.Append(TestScanLineXYP("2.做好省就业创业指导中心挂牌", 2, 72, 100));
        lines.Append(
            TestScanLineXYP("3.会同省教育厅做好省高等院校毕业生就业工作办公室的职责划转和人员转隶工作", 2, 72, 120));
        lines.Append(TestScanLineXYP("4.做好厅内涉改事业单位的职责机构划转和人员转隶工作", 2, 72, 140));
        lines.Append(TestScanLineXYP("5.做好涉改人员的工资养老保险医疗保险等移交接收工作", 2, 72, 160));
        lines.Append(TestScanLineXYP("6.做好搬迁事业单位的法人登记注销和新组建事业单位的法人登记", 2, 72, 180));
        lines.Append(TestScanLineXYP("7.做好涉改事业单位的印章废止和启用以及文书档案的转移", 2, 72, 200));
        lines.Append(TestScanLineXYP("8.做好涉改人员的党组织关系转接同步做好党组织组建", 2, 72, 220));
        lines.Append(TestScanLineXYP("9.及时与省财政厅沟通有关经费预算调整事宜开展经费清算", 2, 72, 240));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 2, roots);
        ExtractedTocItem* impl = ExtractedFindContaining(roots, "组织实施阶段");
        bool ok = ran && impl && impl->children.Size() >= 9 && ExtractedFindContaining(impl->children, "召开动员") &&
                  ExtractedFindContaining(impl->children, "就业创业指导中心") &&
                  ExtractedFindContaining(impl->children, "会同省教育厅") &&
                  ExtractedFindContaining(impl->children, "厅内涉改") &&
                  ExtractedFindContaining(impl->children, "工资养老保险") &&
                  ExtractedFindContaining(impl->children, "法人登记") &&
                  ExtractedFindContaining(impl->children, "印章废止") &&
                  ExtractedFindContaining(impl->children, "党组织关系") &&
                  ExtractedFindContaining(impl->children, "省财政厅");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-l3-long-seq", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 80));
        lines.Append(TestScanLine("二、主要任务", 100));
        lines.Append(TestScanLine("八、完全无关条目", 120));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool ok = ran && roots.Size() == 2 && str::StartsWith(roots[0]->title, "一、") &&
                  str::StartsWith(roots[1]->title, "二、") && !ExtractedHasPrefix(roots, "八、");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 23);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        InferHeadingCand a;
        InferHeadingCand b;
        InferHeadingCand c;
        a.marker = ParseHeadingMarker("一、总体要求");
        b.marker = ParseHeadingMarker("二、主要任务");
        c.marker = ParseHeadingMarker("八、完全无关条目");
        a.inferredLevel = 1;
        b.inferredLevel = 1;
        c.inferredLevel = 1;
        int t12 = TransitionScore(a, b);
        int t28 = TransitionScore(b, c);
        bool transOk = t12 > 0 && t28 < 0 && t12 > t28;
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 80));
        lines.Append(TestScanLine("二、主要任务", 100));
        lines.Append(TestScanLine("八、完全无关条目", 120));
        lines.Append(TestScanLine("三、保障措施", 140));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool ok = transOk && ran && roots.Size() == 3 && str::StartsWith(roots[0]->title, "一、") &&
                  str::StartsWith(roots[1]->title, "二、") && str::StartsWith(roots[2]->title, "三、") &&
                  !ExtractedHasPrefix(roots, "八、") && roots[0]->confidence > 50;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 24);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        ExtractedTocItem* mixed = NewItem("三、考核程序和时间安排 （一）关于在编在岗干部职工的考核", 1, 0, 0, 1);
        ExtractedTocItem* glued = NewItem("三、考核程序和时间安排（一）关于在编在岗干部职工的考核", 1, 0, 0, 1);
        ExtractedTocItem* keep = NewItem("三、做好（一）类人员管理工作", 1, 0, 0, 1);
        bool trimOk = mixed && mixed->title && str::Eq(mixed->title, "三、考核程序和时间安排") && glued &&
                      glued->title && str::Eq(glued->title, "三、考核程序和时间安排") && keep && keep->title &&
                      str::Find(keep->title, "（一）");
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("三、考核程序和时间安排 （一）关于在编在岗干部职工的考核", 80));
        lines.Append(TestScanLine("1.中心在编在岗干部职工根据考核内容认真结本年度工作", 100));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool treeOk = ran && roots.Size() == 1 && str::StartsWith(roots[0]->title, "三、") &&
                      !str::Find(roots[0]->title, "（一）") && roots[0]->children.Size() >= 1 &&
                      str::StartsWith(roots[0]->children[0]->title, "（一）");
        if (trimOk && treeOk) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 25);
        }
        delete mixed;
        delete glued;
        delete keep;
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        HeadingMarker phone = ParseHeadingMarker("2.12333电话咨询科编制外聘用人员，共19人");
        HeadingMarker dotted = ParseHeadingMarker("1.1 有记性的数");
        HeadingMarker ten = ParseHeadingMarker("2.10 编制外聘用人员");
        HeadingMarker face = ParseHeadingMarker("3.4 面上无光");
        bool parseOk = phone.type == MarkerType::ArabicDot && phone.rank == 3 && phone.number == 2 &&
                       HeadingLevelFromText("2.12333电话咨询科编制外聘用人员") == 3 &&
                       dotted.type == MarkerType::ArabicDot && dotted.rank == 2 &&
                       HeadingLevelFromText("1.1 有记性的数") == 2 && ten.type == MarkerType::ArabicDot &&
                       ten.rank == 2 && ten.number == 10 && face.type == MarkerType::ArabicDot && face.rank == 2;
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、考核对象", 80));
        lines.Append(TestScanLine("1.中心正处级领导以外的在编在岗干部职工", 100));
        lines.Append(TestScanLine("2.12333电话咨询科编制外聘用人员，共19人", 120));
        lines.Append(TestScanLine("3.社保卡科技服务站编制外聘用人员，共12人", 140));
        lines.Append(TestScanLine("4.中心其他科室编制外聘用人员，共5人", 160));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool sibOk = parseOk && ran && roots.Size() == 1 && roots[0]->children.Size() == 4 &&
                     roots[0]->children[1]->children.Size() == 0 && roots[0]->children[2]->children.Size() == 0 &&
                     roots[0]->children[3]->children.Size() == 0 &&
                     str::StartsWith(roots[0]->children[1]->title, "2.") &&
                     str::StartsWith(roots[0]->children[2]->title, "3.") &&
                     str::StartsWith(roots[0]->children[3]->title, "4.") &&
                     roots[0]->children[1]->level == roots[0]->children[2]->level &&
                     roots[0]->children[2]->level == roots[0]->children[3]->level;
        if (sibOk) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 26);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        const char* long2 =
            "2."
            "中心其他科室编制外聘用人员的民主测评与中心在编在岗干部职工民主测评同步进行，测评范围为中心在编在岗人员，推"
            "荐表分 "
            "A、B 两种，中心领导填A票，科级及以下人员填B票，A、B表按4:6的权重，以百分制量化计分。";
        char* trimmed = str::Dup(long2);
        TrimTitleToFirstSentence(trimmed);
        char* keepShort = str::Dup("2.12333电话咨询科编制外聘用人员，共19人");
        TrimTitleToFirstSentence(keepShort);
        bool trimOk =
            trimmed &&
            str::Eq(trimmed, "2.中心其他科室编制外聘用人员的民主测评与中心在编在岗干部职工民主测评同步进行") &&
            keepShort && str::Eq(keepShort, "2.12333电话咨询科编制外聘用人员，共19人");
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("（二）关于编制外聘用人员的考核", 80));
        lines.Append(TestScanLine("1.12333电话咨询科、社保卡科技服务站自行组织述职", 100));
        lines.Append(TestScanLine(long2, 120));
        lines.Append(TestScanLine("3.召开中心主任会，研究确定6名编制外聘用优秀等次人员", 140));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool keepOk = trimOk && ran && roots.Size() == 1 && roots[0]->children.Size() == 3 &&
                      str::StartsWith(roots[0]->children[1]->title, "2.中心其他科室") &&
                      roots[0]->children[1]->children.Size() == 0 &&
                      str::StartsWith(roots[0]->children[2]->title, "3.") &&
                      GlyphCount(roots[0]->children[1]->title) <= 80;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 27);
        }
        str::Free(trimmed);
        str::Free(keepShort);
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        char* gluedPage = str::Dup("2.2019-2020年，提升信息化应用水平。3");
        TrimTitleToFirstSentence(gluedPage);
        bool periodOk =
            gluedPage && str::Eq(gluedPage, "2.2019-2020年，提升信息化应用水平") && !str::Find(gluedPage, "。");
        if (periodOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail periodOk\n");
        }
        str::Free(gluedPage);
    }
    {
        const char* wrapped =
            "完成时间：8月上旬 2 .各处室单位、处级干部要认真撰写对照检查材料并报厅主题教育办指导组备案。";
        int sp = FindNextEmbeddedHeading(wrapped);
        HeadingMarker spaced = ParseHeadingMarker("2 .各处室单位、处级干部要认真撰写");
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("（四）深刻剖析撰写对照材料", 80));
        lines.Append(TestScanLine("1.厅领导班子问题对照检查材料由党组书记主持起草", 100));
        lines.Append(TestScanLine(wrapped, 120));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool wrapOk = sp > 0 && spaced.rank == 3 && spaced.number == 2 && ran &&
                      ExtractedHasPrefix(roots, "2.各处室单位") && ExtractedHasPrefix(roots, "1.厅领导班子");
        if (wrapOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail wrapOk\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        const char* item1 = "．1.社保基金管理巩固提升行动工作强基固本阶段工作进展情况的报告（省级、有关市县);";
        HeadingMarker lead = ParseHeadingMarker(item1);
        int tlen = (int)str::Len(item1);
        int ti = 0;
        int cp0 = Utf8CodepointNext(item1, tlen, ti);
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("附件：准备资料", 80));
        lines.Append(TestScanLine(item1, 100));
        lines.Append(TestScanLine("2.社保基金管理巩固提升行动强基固本阶段工作要点或工作方案", 120));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool keepOk = ran && lead.rank == 3 && lead.number == 1 && ExtractedHasPrefix(roots, "1.社保基金管理") &&
                      ExtractedHasPrefix(roots, "2.社保基金管理");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail keep-lead-1\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        const char* raw = "．1.社保基金管理巩固提升行动\n2.社保基金管理方案";
        int len = (int)str::Len(raw);
        Rect* coords = AllocArray<Rect>(len);
        for (int i = 0; i < len; i++) {
            if (raw[i] == '\n') {
                coords[i] = Rect{};
            } else if (i < 3) {
                coords[i] = Rect{10, 100, 3, 14};
            } else {
                coords[i] = Rect{16, 100, 8, 14};
            }
        }
        Vec<ScanLine> glyphLines;
        CollectLinesFromUtf8(raw, coords, len, 1, glyphLines);
        bool glyphOk = glyphLines.Size() >= 2 && str::StartsWith(glyphLines[0].text, "1.社保基金管理") &&
                       str::StartsWith(glyphLines[1].text, "2.社保基金管理");
        if (glyphOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail glyph-split\n");
        }
        FreeScanLines(glyphLines);
        free(coords);
    }
    {
        const char* raw = "第一章引言13";
        int len = (int)str::Len(raw);
        Rect* coords = AllocArray<Rect>(len);
        int i = 0;
        int x = 40;
        while (i < len) {
            int save = i;
            int cp = Utf8CodepointNext(raw, len, i);
            bool page = cp == '1' || cp == '3';
            int w = page ? 8 : 14;
            int gx = page ? (cp == '1' ? 280 : 288) : x;
            if (!page) {
                x += w;
            }
            for (int k = save; k < i; k++) {
                coords[k] = Rect{gx, 80, w, 14};
            }
        }
        Vec<ScanLine> spanLines;
        CollectLinesFromUtf8(raw, coords, len, 1, spanLines);
        bool spanOk = spanLines.Size() >= 2 && str::Find(spanLines[0].text, "引言") &&
                      !str::Find(spanLines[0].text, "13") && LineLooksLikePageNumber(spanLines[1].text);
        if (spanOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail toc-page-span\n");
        }
        FreeScanLines(spanLines);
        free(coords);
    }
    {
        Vec<ScanLine> lines;
        ScanLine sec = TestScanLine("（五）加强信息化基础工作", 80);
        sec.dx = 200;
        ScanLine one = TestScanLine("1.制定标准规范", 100);
        one.dx = 130;
        ScanLine body = TestScanLine(
            "我部将在2017年6月底前制定全国统一的公共就业服务信息指标体系，并相应建立指标完善和扩充机制。", 120);
        body.dx = 420;
        ScanLine two = TestScanLine("2.推进社会保障卡应用", 200);
        two.dx = 180;
        ScanLine three = TestScanLine("3.加强安全体系建设", 280);
        three.dx = 170;
        lines.Append(sec);
        lines.Append(one);
        lines.Append(body);
        lines.Append(two);
        lines.Append(three);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool has1 = ExtractedHasPrefix(roots, "1.制定标准规范");
        bool has2 = ExtractedHasPrefix(roots, "2.推进社会保障卡");
        bool has5 = ExtractedHasPrefix(roots, "（五）加强信息化");
        bool keepOk = ran && has1 && has2 && has5;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail keep-body-1-2-5\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine cap = TestScanLine("合同基本条款", 40);
        cap.dx = 84;
        ScanLine one = TestScanLine("1．合同定义", 60);
        one.dx = 60;
        ScanLine two = TestScanLine("2.服务", 80);
        two.dx = 38;
        ScanLine three = TestScanLine("3．专利权", 100);
        three.dx = 50;
        ScanLine four = TestScanLine("4．合同款的支付", 120);
        four.dx = 83;
        ScanLine pay = TestScanLine("4.1付款条件：", 140);
        pay.dx = 90;
        lines.Append(cap);
        lines.Append(one);
        lines.Append(two);
        lines.Append(three);
        lines.Append(four);
        lines.Append(pay);
        for (int n = 0; n < 24; n++) {
            char buf[160];
            snprintf(buf, (int)sizeof(buf), "%d.这是一段足够长的正文条款用于模拟合同编号条款填充。", n + 10);
            ScanLine body = TestScanLine(buf, 200.f + (float)n * 16.f);
            body.dx = 420;
            lines.Append(body);
        }
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool has1 = ExtractedHasPrefix(roots, "1．合同定义") || ExtractedHasPrefix(roots, "1.合同定义");
        bool has2 = ExtractedHasPrefix(roots, "2.服务");
        bool has3 = ExtractedHasPrefix(roots, "3．专利权") || ExtractedHasPrefix(roots, "3.专利权");
        bool has4 = ExtractedHasPrefix(roots, "4．合同款的支付") || ExtractedHasPrefix(roots, "4.合同款的支付");
        bool has41 = ExtractedHasPrefix(roots, "4.1付款条件");
        bool noBody = !ExtractedHasPrefix(roots, "10.这是一段足够长");
        bool keepOk = ran && has1 && has2 && has3 && has4 && has41 && noBody;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail contract-clauses\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        HeadingMarker period = ParseHeadingMarker("1。任务目标。在养老保险、医疗保险等领域中广泛开展应用。");
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("（一）社会保险领域", 80));
        ScanLine one = TestScanLine(
            "1。任务目标。在养老保险、医疗保险、失业保险、工伤保险、生育保险、社会保险公共业务等领域中广泛开展社会保障"
            "卡应用。",
            100);
        one.dx = 450;
        lines.Append(one);
        ScanLine two = TestScanLine("2.责任单位。厅医保处、省医保局、厅信息中心、设区市人力资源社会保障局。", 140);
        two.dx = 200;
        lines.Append(two);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool parseOk = period.type == MarkerType::ArabicDot && period.rank == 3 && period.number == 1;
        bool has1 = ExtractedHasPrefix(roots, "1.任务目标");
        bool has2 = ExtractedHasPrefix(roots, "2.责任单位");
        bool keepOk = parseOk && ran && has1 && has2;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail arabic-period\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 80));
        lines.Append(TestScanLine("关于选择确定采购方式", 110));
        lines.Append(TestScanLine("三、工作措施", 200));
        lines.Append(TestScanLine("四、保障措施", 280));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool has1 = ExtractedHasPrefix(roots, "一、总体要求");
        bool has2 = ExtractedHasPrefix(roots, "二、关于选择确定采购方式");
        bool has3 = ExtractedHasPrefix(roots, "三、工作措施");
        bool keepOk = ran && has1 && has2 && has3;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail invent-er-heading\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 60));
        lines.Append(TestScanLine("（五）加强信息化基础工作", 80));
        ScanLine missing = TestScanLine("制定标准规范", 100);
        missing.dx = 130;
        lines.Append(missing);
        ScanLine two = TestScanLine("2.推进社会保障卡应用", 200);
        two.dx = 180;
        ScanLine three = TestScanLine("3.加强安全体系建设", 280);
        three.dx = 170;
        lines.Append(two);
        lines.Append(three);
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool has1 = ExtractedHasPrefix(roots, "1.制定标准规范");
        bool has2 = ExtractedHasPrefix(roots, "2.推进社会保障卡");
        bool keepOk = ran && has1 && has2;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail invent-1-dot\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("一、总体要求", 80));
        lines.Append(TestScanLine("二、工作目标", 110));
        lines.Append(TestScanLine("工作要求说明", 130));
        lines.Append(TestScanLine("三、工作措施", 200));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool noFake = !ExtractedHasPrefix(roots, "工作要求说明") && !ExtractedHasPrefix(roots, "四、工作要求");
        bool keepOk = ran && ExtractedHasPrefix(roots, "一、总体要求") && ExtractedHasPrefix(roots, "二、工作目标") &&
                      ExtractedHasPrefix(roots, "三、工作措施") && noFake;
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail no-fake-si\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        HeadingMarker a = ParseHeadingMarker("〈一)启动报名 (2026年6月3日-6月29日)");
        HeadingMarker b = ParseHeadingMarker("(二〉初赛评审 (2026年6月30日-7月10日)");
        HeadingMarker c = ParseHeadingMarker("〈三)初赛公示");
        HeadingMarker d = ParseHeadingMarker("(四)决赛阶段");
        HeadingMarker e = ParseHeadingMarker("〈五)作品公示");
        HeadingMarker f = ParseHeadingMarker("〈六)总结推广");
        HeadingMarker both = ParseHeadingMarker("〈一〉启动报名");
        bool parseOk = a.rank == 2 && a.number == 1 && a.type == MarkerType::ChineseParen && b.rank == 2 &&
                       b.number == 2 && c.rank == 2 && c.number == 3 && d.rank == 2 && d.number == 4 && e.rank == 2 &&
                       e.number == 5 && f.rank == 2 && f.number == 6 && both.rank == 2 && both.number == 1 &&
                       FindNextEmbeddedHeading("三、做好（一）类人员管理工作") < 0 &&
                       FindNextEmbeddedHeading("三、做好〈一〉类人员管理工作") < 0;
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("四、赛程安排", 80));
        lines.Append(TestScanLine("〈一)启动报名 (2026年6月3日-6月29日)", 100));
        lines.Append(TestScanLine("(二〉初赛评审 (2026年6月30日-7月10日)", 120));
        lines.Append(TestScanLine("〈三)初赛公示 (2026年7月11日-7月20日)", 140));
        lines.Append(TestScanLine("(四)决赛阶段 (2026年8月1日-8月10日)", 160));
        lines.Append(TestScanLine("〈五)作品公示 (2026年8月11日-8月20日)", 180));
        lines.Append(TestScanLine("〈六)总结推广 (2026年8月21日至9月10日)", 200));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        ExtractedTocItem* sec = nullptr;
        for (int i = 0; i < roots.Size(); i++) {
            if (roots[i]->title && str::Find(roots[i]->title, "赛程安排")) {
                sec = roots[i];
                break;
            }
        }
        bool keepOk = parseOk && ran && sec && sec->children.Size() == 6 &&
                      ExtractedHasPrefix(sec->children, "（一）") && ExtractedHasPrefix(sec->children, "（二）") &&
                      ExtractedHasPrefix(sec->children, "（三）") && ExtractedHasPrefix(sec->children, "（四）") &&
                      ExtractedHasPrefix(sec->children, "（五）") && ExtractedHasPrefix(sec->children, "（六）");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail paren-variants\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        const char* long1 =
            "1．《年度考核登记表》（要求正反一张A4纸）于2020年12月25日16:"
            "00前提交综合科。《年度考核登记表》中的出生年月、"
            "任现职时间等信息要严格按照组织部门认定的时间进行填写。“个人总结”“本人意见”两栏的“签名”处由本人签字，其中，"
            "“个人总结”一栏还须填写填表时间，“本人意见”一栏不能填写时间。";
        char* trimmed = str::Dup(long1);
        TrimTitleToFirstSentence(trimmed);
        bool trimOk = trimmed && str::Find(trimmed, "年度考核登记表") && str::Find(trimmed, "提交综合科") &&
                      !str::Find(trimmed, "出生年月") && GlyphCount(trimmed) <= 80 && GlyphCount(trimmed) > 40;
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("四、其他事项", 80));
        lines.Append(TestScanLine(long1, 100));
        lines.Append(TestScanLine("2.招聘的新参加工作人员，在试用期（见习期）内参加年度考核", 120));
        lines.Append(TestScanLine("3.中心优秀等次的在编在岗人员，将被推荐至厅年度考核工作领导小组", 140));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool keepOk = trimOk && ran && roots.Size() == 1 && roots[0]->children.Size() == 3 &&
                      str::Find(roots[0]->children[0]->title, "年度考核登记表") &&
                      GlyphCount(roots[0]->children[0]->title) <= 80 &&
                      str::StartsWith(roots[0]->children[1]->title, "2.") &&
                      str::StartsWith(roots[0]->children[2]->title, "3.");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 28);
        }
        str::Free(trimmed);
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        const char* glued1 = "24日举行。现就做好相关工作通知如下： 一、社工人才重点领域";
        const char* glued2 = "工作部门担任业务主管单位的社会组织。 （二）政法系统：市、县、乡镇（街道）";
        const char* glued4 =
            "相关人员，各级信访部门担任业务主管单位的社会组织。 （四）法院系统：法院系统从事社会工作服务";
        int sp1 = FindNextEmbeddedHeading(glued1);
        int sp2 = FindNextEmbeddedHeading(glued2);
        int sp4 = FindNextEmbeddedHeading(glued4);
        int spKeep = FindNextEmbeddedHeading("三、做好（一）类人员管理工作");
        int at1 = sp1;
        int at2 = sp2;
        int at4 = sp4;
        if (at1 >= 0) {
            SkipWsUtf8(glued1, (int)str::Len(glued1), at1);
        }
        if (at2 >= 0) {
            SkipWsUtf8(glued2, (int)str::Len(glued2), at2);
        }
        if (at4 >= 0) {
            SkipWsUtf8(glued4, (int)str::Len(glued4), at4);
        }
        bool splitOk = at1 > 0 && str::StartsWith(glued1 + at1, "一、") && at2 > 0 &&
                       str::StartsWith(glued2 + at2, "（二）") && at4 > 0 && str::StartsWith(glued4 + at4, "（四）") &&
                       spKeep < 0;
        Vec<ScanLine> lines;
        lines.Append(TestScanLine(glued1, 80));
        lines.Append(TestScanLine("（一）社会工作系统：基层群众自治组织、党群服务中心", 100));
        lines.Append(TestScanLine(glued2, 120));
        lines.Append(TestScanLine(glued4, 140));
        lines.Append(TestScanLine("二、报考条件和科目设置", 160));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool keepOk = splitOk && ran && ExtractedHasPrefix(roots, "一、社工人才") &&
                      ExtractedHasPrefix(roots, "（一）社会工作系统") && ExtractedHasPrefix(roots, "（二）政法系统") &&
                      ExtractedHasPrefix(roots, "（四）法院系统") && ExtractedHasPrefix(roots, "二、报考条件");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 29);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        // Tall OCR boxes can overlap a heading and the next line; grouping must
        // keep top-to-bottom order, not left-to-right (x of 提高站位 is 1pt smaller).
        Vec<ScanLine> lines;
        ScanLine subj = TestScanLine("（二）科目设置", 255.2f);
        subj.x = 108;
        subj.dy = 20;
        ScanLine raise = TestScanLine("（一）提高站位，精心组织。社会工作人才队伍是党的六支", 647.0f);
        raise.x = 106;
        raise.dy = 57.2f;
        ScanLine lead = TestScanLine("三、加强组织领导", 623.1f);
        lead.x = 107;
        lead.dy = 54.3f;
        lines.Append(subj);
        lines.Append(raise);
        lines.Append(lead);
        RebuildVisualLines(lines);
        bool orderOk = lines.Size() == 3 && lines[0].text && str::StartsWith(lines[0].text, "（二）科目") &&
                       lines[1].text && str::StartsWith(lines[1].text, "三、加强组织") && lines[2].text &&
                       str::StartsWith(lines[2].text, "（一）提高站位");
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        ExtractedTocItem* leadItem = nullptr;
        for (int i = 0; i < roots.Size(); i++) {
            if (roots[i]->title && str::StartsWith(roots[i]->title, "三、加强组织")) {
                leadItem = roots[i];
                break;
            }
        }
        bool nestOk = ran && leadItem && leadItem->children.Size() >= 1 && leadItem->children[0]->title &&
                      str::StartsWith(leadItem->children[0]->title, "（一）提高站位");
        if (orderOk && nestOk) {
            (*pass)++;
        } else {
            (*fail)++;
            *failMask |= (1 << 30);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("（二）社会工作师考试报名条件", 80));
        lines.Append(TestScanLine("1.取得高中或者中专学历，从事社会工作满4年；", 100));
        lines.Append(TestScanLine("2.取得社会工作专业大专学历，从事社会工作满2年；", 120));
        lines.Append(TestScanLine("3.取得社会工作专业大学本科学历，从事社会工作满3年；", 140));
        lines.Append(TestScanLine("4.取得社会工作专业硕士学位，从事社会工作满1年；", 160));
        lines.Append(TestScanLine("5.取得社会工作专业博士学位；", 180));
        lines.Append(TestScanLine("6.取得其他专业大专学历，从事社会工作满6年；", 200));
        lines.Append(TestScanLine("7.取得其他专业本科学历，从事社会工作满5年；", 220));
        lines.Append(TestScanLine("8.取得其他专业硕士学位，从事社会工作满3年；", 240));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        ExtractedTocItem* sec = nullptr;
        for (int i = 0; i < roots.Size(); i++) {
            if (roots[i]->title && str::StartsWith(roots[i]->title, "（二）社会工作师")) {
                sec = roots[i];
                break;
            }
        }
        bool keepOk = ran && sec && sec->children.Size() >= 5 && sec->children[0]->title &&
                      str::StartsWith(sec->children[0]->title, "1.取得高中") && sec->children[4]->title &&
                      str::StartsWith(sec->children[4]->title, "5.取得社会工作专业博士");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail social-worker-5\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        const char* glued =
            "主要包括：（1）生活帮扶、生计发展、就业援助服务；（2）情绪疏导、精神抚慰服务；（3）"
            "矛盾纠纷调节、家庭与社会关系调适服务；（4）针对特殊困难群体的权益维护、政策咨询、资源链接、"
            "能力提升及社会支持网络建设服务；（5）行为矫治、戒毒康复、危机干预服务；（6）推动社区发展，"
            "促进社会融入、社会参与的服务；（7）其他旨在满足服务对象心理和社会服务需求、增强社会功能的服务。";
        int spKeep = FindNextEmbeddedHeading("三、做好（一）类人员管理工作");
        int nSp = 0;
        const char* walk = glued;
        while (walk && walk[0]) {
            int sp = FindNextEmbeddedHeading(walk);
            if (sp < 1) {
                break;
            }
            nSp++;
            walk += sp;
        }
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("6.取得其他专业大专及以上学历或学位", 80));
        lines.Append(TestScanLine(glued, 100));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool keepOk = ran && spKeep < 0 && nSp >= 6 && ExtractedHasPrefix(roots, "（1）生活帮扶") &&
                      ExtractedHasPrefix(roots, "（2）情绪疏导") && ExtractedHasPrefix(roots, "（6）推动社区");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail glued-parens\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("6.取得其他专业大专及以上学历或学位", 80));
        lines.Append(TestScanLine(
            "（4）针对特殊困难群体的权益维护、政策咨询、资源链接、能力提升及社会支持网络建设服务；（5）", 100));
        lines.Append(TestScanLine("行为矫治、戒毒康复、危机干预服务；（6）推动社区发展", 120));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 1, roots);
        bool keepOk = ran && ExtractedHasPrefix(roots, "（5）行为矫治") && ExtractedHasPrefix(roots, "（6）推动社区");
        if (keepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail wrap-parens\n");
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("可行性研究报告", 20));
        lines.Append(TestScanLine("第一章 项目概述", 40));
        lines.Append(TestScanLine("建设规模", 60));
        bool feasOff = ClassifyExtractTocDoc(lines, 1, "foo/可行性研究报告.pdf") == ExtractTocDocClass::Official;
        FreeScanLines(lines);
        lines.Append(TestScanLine("摘要", 20));
        lines.Append(TestScanLine("关键词：目录 提取", 40));
        lines.Append(TestScanLine("第1章 绪论", 60));
        bool paperOk = ClassifyExtractTocDoc(lines, 1, "thesis.pdf") == ExtractTocDocClass::Paper;
        bool paperUnderFeasDir =
            ClassifyExtractTocDoc(lines, 1,
                                  "C:\\Data\\02.工作\\07.纪检监察\\可行性研究\\参考文献\\"
                                  "国有企业纪检监察信访案件管理系统的分析与设计.pdf") == ExtractTocDocClass::Paper;
        FreeScanLines(lines);
        lines.Append(TestScanLine("关于进一步加强某某工作的通知", 20));
        lines.Append(TestScanLine("一、总体要求", 40));
        bool noticeOk =
            ClassifyExtractTocDoc(lines, 1, "关于进一步加强某某工作的通知.pdf") == ExtractTocDocClass::Official;
        FreeScanLines(lines);
        lines.Append(TestScanLine("第1章 智力加油站", 20));
        bool chapAlone = ClassifyExtractTocDoc(lines, 1, "math.pdf") == ExtractTocDocClass::Official;
        FreeScanLines(lines);
        lines.Append(TestScanLine("前言", 20));
        lines.Append(TestScanLine("第1章 智力加油站", 40));
        lines.Append(TestScanLine("第2章 有记性的数", 60));
        bool bookOk = ClassifyExtractTocDoc(lines, 1, "math.pdf") == ExtractTocDocClass::Book;
        FreeScanLines(lines);
        lines.Append(TestScanLine("目录", 20));
        lines.Append(TestScanLine("做一个优秀中学生····", 40));
        lines.Append(TestScanLine("第一课热爱祖国··", 60));
        lines.Append(TestScanLine("第二课热爱人民⋯··", 80));
        bool lessonBook = ClassifyExtractTocDoc(lines, 1, "青少年修养.pdf") == ExtractTocDocClass::Book;
        FreeScanLines(lines);
        lines.Append(TestScanLine("目录", 20));
        lines.Append(TestScanLine("第六课社会主义民主建设", 40));
        lines.Append(TestScanLine("第七课我国宪法", 60));
        bool jiansheBook = ClassifyExtractTocDoc(lines, 1, "中国社会主义建设常识.pdf") == ExtractTocDocClass::Book;
        FreeScanLines(lines);
        bool contractPath =
            ClassifyExtractTocDoc(lines, 1,
                                  "C:\\Data\\02.工作\\合同\\江西省金保工程期建设项目省厅统一运维实施商合同.pdf") ==
            ExtractTocDocClass::Contract;
        lines.Append(TestScanLine("甲方：江西省人力资源和社会保障厅", 20));
        lines.Append(TestScanLine("乙方：某某公司", 40));
        bool contractParties = ClassifyExtractTocDoc(lines, 1, "scan.pdf") == ExtractTocDocClass::Contract;
        FreeScanLines(lines);
        lines.Append(TestScanLineXYP("版权页", 1, 40, 20));
        lines.Append(TestScanLineXYP("目录", 14, 160, 40));
        lines.Append(TestScanLineXYP("第一章 概论", 14, 48, 80));
        lines.Append(TestScanLineXYP("第二章 方法", 15, 48, 100));
        bool lateTocBook = ClassifyExtractTocDoc(lines, 20, "情感智力.pdf") == ExtractTocDocClass::Book;
        FreeScanLines(lines);
        lines.Append(TestScanLine("赣人社发〔2026〕7号", 20));
        lines.Append(TestScanLine("关于印发《江西省人力资源和社会保障数据安全管理办法》的通知", 40));
        lines.Append(TestScanLine("日录", 60));
        lines.Append(TestScanLine("第四章数据全生命周期安全管理", 80));
        bool banfaOfficial = ClassifyExtractTocDoc(lines, 1, "管理办法.pdf") == ExtractTocDocClass::Official;
        FreeScanLines(lines);
        bool clsOk = feasOff && paperOk && paperUnderFeasDir && noticeOk && chapAlone && bookOk && lessonBook &&
                     jiansheBook && contractPath && contractParties && lateTocBook && banfaOfficial;
        if (clsOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail classify feas=%d paper=%d book=%d lesson=%d jianshe=%d late=%d banfa=%d\n",
                 feasOff ? 1 : 0, paperOk ? 1 : 0, bookOk ? 1 : 0, lessonBook ? 1 : 0, jiansheBook ? 1 : 0,
                 lateTocBook ? 1 : 0, banfaOfficial ? 1 : 0);
        }
    }
    {
        Vec<ScanLine> lines;
        ScanLine two = TestScanLine("2.服务", 80);
        two.dx = 38;
        ScanLine two1 = TestScanLine("2.1 服务范围", 100);
        two1.dx = 70;
        ScanLine three = TestScanLine("3．专利权", 120);
        three.dx = 50;
        ScanLine three1 = TestScanLine("3.1 乙方在工作如需提供软件", 140);
        three1.dx = 160;
        ScanLine four = TestScanLine("4．合同款的支付", 160);
        four.dx = 83;
        ScanLine four1 = TestScanLine("4.1付款条件：", 180);
        four1.dx = 90;
        ScanLine five = TestScanLine("5. 技术资料", 200);
        five.dx = 70;
        lines.Append(two);
        lines.Append(two1);
        lines.Append(three);
        lines.Append(three1);
        lines.Append(four);
        lines.Append(four1);
        lines.Append(five);
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractContractToc(lines, 1, roots);
        ExtractedTocItem* n31 = ExtractedFindContaining(roots, "3.1");
        bool treeOk = ran && ExtractedIsRootContaining(roots, "2.服务") && ExtractedIsRootContaining(roots, "专利权") &&
                      ExtractedIsRootContaining(roots, "合同款的支付") &&
                      ExtractedIsRootContaining(roots, "技术资料") && n31 &&
                      !ExtractedHasPrefix(n31->children, "4．") && !ExtractedHasPrefix(n31->children, "4.") &&
                      !ExtractedHasPrefix(n31->children, "5.");
        if (treeOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("contract-tree", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("1、合同文件。", 20));
        lines.Append(TestScanLine("1）招标文件；", 40));
        lines.Append(TestScanLine("2）投标文件；", 60));
        lines.Append(TestScanLine("3）投标人就其投标文件在评标质疑、澄清时认定的资料；", 80));
        lines.Append(TestScanLine("4）", 100));
        lines.Append(TestScanLine("中标通知书；", 114));
        lines.Append(TestScanLine("5）本合同协议书；", 130));
        lines.Append(TestScanLine("8）本合同附件。", 160));
        lines.Append(TestScanLine("2、合同范围和条件。", 180));
        lines.Append(TestScanLine("4、合同金额。", 200));
        lines.Append(TestScanLine("(1)集成实施费", 220));
        lines.Append(TestScanLine("5、履约保证金和付款方式", 240));
        lines.Append(TestScanLine("1）履约保证金", 260));
        lines.Append(TestScanLine("2）付款方式", 280));
        lines.Append(TestScanLine("第6条规定，相应延长服务期", 300));
        lines.Append(TestScanLine("v 第8条规定，相应延长服务期", 320));
        lines.Append(TestScanLine("1.合同定义", 340));
        lines.Append(TestScanLine("2.服务", 360));
        lines.Append(TestScanLine("14.甲方的权利和义务", 380));
        lines.Append(TestScanLine("1）行使项目建设管理的各项权利", 400));
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractContractToc(lines, 1, roots);
        ExtractedTocItem* files = ExtractedFindContaining(roots, "合同文件");
        ExtractedTocItem* amount = ExtractedFindContaining(roots, "合同金额");
        ExtractedTocItem* pay = ExtractedFindContaining(roots, "履约保证金和付款方式");
        ExtractedTocItem* rights = ExtractedFindContaining(roots, "甲方的权利和义务");
        bool nestOk =
            ran && files && files->level == 1 && ExtractedHasPrefix(files->children, "1）招标文件") &&
            ExtractedHasPrefix(files->children, "4）中标通知书") &&
            ExtractedHasPrefix(files->children, "8）本合同附件") && !ExtractedIsRootContaining(roots, "招标文件") &&
            amount && ExtractedHasPrefix(amount->children, "（1）") && pay &&
            ExtractedHasPrefix(pay->children, "1）履约保证金") && ExtractedHasPrefix(pay->children, "2）付款方式") &&
            ExtractedIsRootContaining(roots, "合同定义") && ExtractedIsRootContaining(roots, "2.服务") && rights &&
            ExtractedHasPrefix(rights->children, "1）行使项目建设管理") && !ExtractedHasPrefix(roots, "第6条规定") &&
            !ExtractedHasPrefix(roots, "第8条规定");
        if (nestOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("contract-nest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("9. 索赔", 20));
        lines.Append(
            TestScanLine("9.1 "
                         "如果在合同服务期内服务质量不符合合同约定的要求，甲方有权向乙方提出索赔。这是足够长的一段用来"
                         "超过八十个汉字阈值的条款正文所以必须被截成标题。",
                         40));
        lines.Append(
            TestScanLine("9.2 "
                         "在合同服务期内如果乙方对质量和进度承担责任甲方可采取下列方式索赔，这段同样必须写得很长才会被"
                         "旧规则当成正文丢掉。",
                         60));
        lines.Append(TestScanLine("(1)要求乙方立即采取补救措施", 80));
        lines.Append(TestScanLine("(2)扣除相应合同款", 100));
        lines.Append(TestScanLine(
            "9.3 甲方发出索赔通知后二十天内乙方应予答复，这也是一段很长的条款说明文字用来超过正文过滤阈值。", 120));
        lines.Append(TestScanLine(
            "9.4 在整个合同有效期间发生下列情形甲方可扣除费用，这同样是一段很长的条款所以旧逻辑会把它丢掉。", 140));
        lines.Append(TestScanLine("(1)更换项目经理或架构师", 160));
        lines.Append(TestScanLine("(2)累计更换集成人员数量达到或超过三分之一", 180));
        lines.Append(TestScanLine("13. 履约保证金", 200));
        lines.Append(
            TestScanLine("13.1 乙方应提交履约保证金，这是一段足够长的条款说明用于进入目录而不是被当成正文。", 220));
        lines.Append(TestScanLine("13.2 履约保证金的金额为合同额的百分之五，同样需要足够长才会被旧规则丢掉。", 240));
        lines.Append(TestScanLine("13.3 服务期满后无息退还履约保证金，这段也要写得足够长以便覆盖过滤。", 260));
        lines.Append(TestScanLine("14. 甲方的权利和义务", 280));
        lines.Append(TestScanLine("14.1 权利", 300));
        lines.Append(TestScanLine("1）行使项目建设管理的各项权利", 320));
        lines.Append(TestScanLine(
            "14.2 甲方应按合同约定向乙方支付合同款，这段文字必须足够长才会被当成正文丢掉所以要继续加字。", 340));
        lines.Append(TestScanLine("附件一：项目技术及其他要求", 360));
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractContractToc(lines, 1, roots);
        ExtractedTocItem* claim = ExtractedFindContaining(roots, "索赔");
        ExtractedTocItem* n92 = ExtractedFindContaining(roots, "9.2");
        ExtractedTocItem* n94 = ExtractedFindContaining(roots, "9.4");
        ExtractedTocItem* bond = ExtractedFindContaining(roots, "履约保证金");
        ExtractedTocItem* rights = ExtractedFindContaining(roots, "甲方的权利和义务");
        ExtractedTocItem* r141 = ExtractedFindContaining(roots, "14.1");
        bool clauseOk =
            ran && claim && ExtractedHasPrefix(claim->children, "9.1") && ExtractedHasPrefix(claim->children, "9.2") &&
            ExtractedHasPrefix(claim->children, "9.3") && ExtractedHasPrefix(claim->children, "9.4") && n92 &&
            ExtractedHasPrefix(n92->children, "（1）") && n94 && ExtractedHasPrefix(n94->children, "（1）") && bond &&
            ExtractedHasPrefix(bond->children, "13.1") && ExtractedHasPrefix(bond->children, "13.2") &&
            ExtractedHasPrefix(bond->children, "13.3") && rights && ExtractedHasPrefix(rights->children, "14.1") &&
            ExtractedHasPrefix(rights->children, "14.2") && r141 &&
            ExtractedHasPrefix(r141->children, "1）行使项目建设管理") && ExtractedIsRootContaining(roots, "附件一");
        if (clauseOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("contract-clause", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("摘要", 20));
        lines.Append(TestScanLine("第1章 绪论", 40));
        lines.Append(TestScanLine("1.1 研究背景", 60));
        lines.Append(TestScanLine("第 2 章 相关理论基础和技术路线", 80));
        lines.Append(TestScanLine("2.1 B/S 架构", 100));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractPaperToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "相关理论基础和技术路线");
        bool paperTree = ran && ExtractedIsRootContaining(roots, "绪论") &&
                         ExtractedIsRootContaining(roots, "相关理论基础和技术路线") && ch2 && ch2->level == 1 &&
                         (!ch2->title || !str::Find(ch2->title, "2.1"));
        if (paperTree) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("paper-tree", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("第1章 绪论", 40));
        lines.Append(TestScanLine("1.1 研究背景", 60));
        lines.Append(TestScanLine("2.1 第 2 章 相关理论基础和技术路线", 80));
        lines.Append(TestScanLine("2.1 B/S 架构", 100));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractPaperToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "相关理论基础和技术路线");
        bool stripOk = ran && ExtractedIsRootContaining(roots, "相关理论基础和技术路线") && ch2 &&
                       (!ch2->title || !str::Find(ch2->title, "2.1"));
        if (stripOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("paper-strip-2.1", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("第1章 绪论", 40));
        lines.Append(TestScanLine("1.1 研究背景", 60));
        lines.Append(TestScanLine("第 2 章 相关理论基础和技术路线", 80));
        lines.Append(TestScanLine("2.1 B/S 架构", 100));
        lines.Append(TestScanLine("致谢", 120));
        lines.Append(TestScanLine("第1 章 绪论", 140));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractPaperToc(nullptr, lines, labels, 1, roots);
        int nCh1 = 0;
        for (int i = 0; i < roots.Size(); i++) {
            if (roots[i]->title && str::Find(roots[i]->title, "绪论")) {
                nCh1++;
            }
        }
        bool dropOk = ran && nCh1 == 1 && ExtractedIsRootContaining(roots, "致谢") &&
                      ExtractedIsRootContaining(roots, "相关理论基础和技术路线");
        if (dropOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("paper-drop-dup-ch1", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("前言", 20));
        lines.Append(TestScanLine("第1章 智力加油站", 40));
        lines.Append(TestScanLine("1.1 有记性的数", 60));
        lines.Append(TestScanLine("第2章 推陈出新", 80));
        lines.Append(TestScanLine("一、不应作为书的骨架", 100));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        bool bookTree = ran && ExtractedIsRootContaining(roots, "前言") && ExtractedIsRootContaining(roots, "第1章") &&
                        ExtractedIsRootContaining(roots, "第2章") && !ExtractedHasPrefix(roots, "一、不应作为书的骨架");
        if (bookTree) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-no-printed", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXY("目录", 147, 58));
        lines.Append(TestScanLineXY("做一个优秀中学生····", 38, 92));
        lines.Append(TestScanLineXY("第一课热爱祖国··", 38, 112));
        lines.Append(TestScanLineXY("第二课热爱人民⋯··", 38, 133));
        lines.Append(TestScanLineXY(".·17", 287, 137));
        lines.Append(TestScanLineXY("第三课热爱中国共产党···", 38, 155));
        lines.Append(TestScanLineXY("···25", 284, 157));
        lines.Append(TestScanLineXY("第四课热爱科学···", 38, 175));
        lines.Append(TestScanLineXY("··32", 284, 177));
        lines.Append(TestScanLineXY("32 ....41", 281, 185));
        lines.Append(TestScanLineXY("第五课热爱劳动⋯··", 38, 196));
        lines.Append(TestScanLineXY("....41", 281, 198));
        lines.Append(TestScanLineXY("第六课热爱集体···", 38, 216));
        lines.Append(TestScanLineXY("·49", 284, 218));
        lines.Append(TestScanLineXY("第七课尊敬师长···", 38, 236));
        lines.Append(TestScanLineXY("·59", 284, 238));
        lines.Append(TestScanLineXY("第八课遵守社会公德···", 38, 256));
        lines.Append(TestScanLineXY("·66", 284, 258));
        lines.Append(TestScanLineXYP("做一个优秀中学生", 2, 72, 80));
        lines.Append(TestScanLineXYP("我们上中学了", 2, 72, 140));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 2, roots);
        ExtractedTocItem* ke4 = ExtractedFindContaining(roots, "第四课");
        ExtractedTocItem* ke8 = ExtractedFindContaining(roots, "第八课");
        bool printedOk =
            ran && ExtractedHasPrintedTocBookmark(roots, 1) && ExtractedFindContaining(roots, "做一个优秀中学生") &&
            ExtractedIsRootContaining(roots, "第一课") && ExtractedIsRootContaining(roots, "第二课") &&
            ExtractedIsRootContaining(roots, "第五课") && ExtractedIsRootContaining(roots, "第八课") && ke4 &&
            ke4->children.Size() == 0 && ke8 && ke8->children.Size() == 0 && !ExtractedHasPrefix(roots, "4.1") &&
            !ExtractedHasPrefix(roots, "8.1") && !ExtractedHasPrefix(roots, "8.2") &&
            !ExtractedFindContaining(roots, "我们上中学了") && !ExtractedHasPrefix(roots, "一、热爱祖国");
        if (printedOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("youth-xiuyang", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXY("目录", 144, 66));
        lines.Append(TestScanLineXY("第六课社会主义民主建设", 32, 110));
        lines.Append(TestScanLineXY("社会主义民主··", 48, 129));
        lines.Append(TestScanLineXY("社会主义民主建设的重要性··", 48, 140));
        lines.Append(TestScanLineXY("加强社会主义民主建设", 49, 156));
        lines.Append(TestScanLineXY("10", 293, 159));
        lines.Append(TestScanLineXY("第七课我国宪法·····", 32, 172));
        lines.Append(TestScanLineXY("18", 291, 176));
        lines.Append(TestScanLineXY("宪法是国家的根本大法", 49, 188));
        lines.Append(TestScanLineXY("19", 292, 190));
        lines.Append(TestScanLineXY("我国宪法的基本内容", 50, 202));
        lines.Append(TestScanLineXY("23", 292, 203));
        lines.Append(TestScanLineXY("第八课民法通则婚姻法继承法经济合同法", 32, 214));
        lines.Append(TestScanLineXY("40", 292, 216));
        lines.Append(TestScanLineXY("民法通则", 49, 234));
        lines.Append(TestScanLineXY("41", 292, 235));
        lines.Append(TestScanLineXY("婚姻法", 49, 247));
        lines.Append(TestScanLineXY("51", 287, 249));
        lines.Append(TestScanLineXY("继承法", 49, 262));
        lines.Append(TestScanLineXY("58", 288, 262));
        lines.Append(TestScanLineXY("经济合同法", 50, 276));
        lines.Append(TestScanLineXY("63", 292, 277));
        lines.Append(TestScanLineXY("第九课青年在社会主义建设中的责任", 32, 289));
        lines.Append(TestScanLineXY("青年是社会主义建设事业的生力军", 50, 307));
        lines.Append(TestScanLineXY("72", 292, 311));
        lines.Append(TestScanLineXYP("社会主义民主建设的重要性表现在哪里？", 2, 40, 80));
        lines.Append(TestScanLineXYP("一、序言", 2, 40, 120));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 2, roots);
        ExtractedTocItem* ke6 = ExtractedFindContaining(roots, "第六课");
        ExtractedTocItem* ke7 = ExtractedFindContaining(roots, "第七课");
        bool jiansheOk = ran && ExtractedIsRootContaining(roots, "第六课") &&
                         ExtractedIsRootContaining(roots, "第七课") && ExtractedIsRootContaining(roots, "第八课") &&
                         ExtractedIsRootContaining(roots, "第九课") && ke6 &&
                         ExtractedHasPrefix(ke6->children, "社会主义民主") && ke7 &&
                         ExtractedHasPrefix(ke7->children, "宪法是国家的根本大法") &&
                         !ExtractedFindContaining(roots, "表现在哪里") && !ExtractedIsRootContaining(roots, "一、序言");
        if (jiansheOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("jianshe-changshi", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXY("目录", 40, 40));
        lines.Append(TestScanLineXY("第五章反映在作文中的高中生", 32, 80));
        lines.Append(TestScanLineXY("140", 290, 80));
        lines.Append(TestScanLineXY("(144) 不适于做薪金工作者", 48, 110));
        lines.Append(TestScanLineXY("150", 290, 110));
        lines.Append(TestScanLineXY("(145) 父母关系不好，真痛苦", 56, 130));
        lines.Append(TestScanLineXY("150", 290, 130));
        lines.Append(TestScanLineXY("（147）难报父母的恩情", 64, 150));
        lines.Append(TestScanLineXY("150", 290, 150));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* ch5 = ExtractedFindContaining(roots, "第五章");
        ExtractedTocItem* a = ExtractedFindContaining(roots, "不适于做薪金");
        ExtractedTocItem* b = ExtractedFindContaining(roots, "父母关系不好");
        ExtractedTocItem* c = ExtractedFindContaining(roots, "难报父母的恩情");
        bool pagePrefixOk = ran && ch5 && a && b && c && a->parent == ch5 && b->parent == ch5 && c->parent == ch5 &&
                            a->title && !str::Find(a->title, "144") && !str::Find(a->title, "(") && b->title &&
                            !str::Find(b->title, "145") && c->title && !str::Find(c->title, "147") &&
                            a->children.Size() == 0 && b->children.Size() == 0;
        if (pagePrefixOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-paren-page-not-numbering", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXY("目录", 40, 40));
        lines.Append(TestScanLineXY("(144) 不适于做薪金工作者", 48, 80));
        ScanLine p150 = TestScanLineXY("150", 290, 80);
        p150.dx = 16;
        lines.Append(p150);
        lines.Append(TestScanLineXY("(155) 在任何方面我都比不上别人", 48, 110));
        ScanLine p158 = TestScanLineXY("158", 290, 110);
        p158.dx = 16;
        lines.Append(p158);
        lines.Append(TestScanLineXY("收在后面的一篇（160）", 48, 140));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* a = ExtractedFindContaining(roots, "不适于做薪金");
        ExtractedTocItem* b = ExtractedFindContaining(roots, "在任何方面");
        ExtractedTocItem* c = ExtractedFindContaining(roots, "收在后面");
        bool stripOk = ran && a && a->title && !str::Find(a->title, "144") && !str::Find(a->title, "(") &&
                       a->printedPage == 150 && b && b->title && !str::Find(b->title, "155") &&
                       !str::Find(b->title, "(") && b->printedPage == 158 && c && c->title &&
                       !str::Find(c->title, "160") && !str::Find(c->title, "(") && c->printedPage == 160;
        if (stripOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-paren-page-is-page", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("版权页", 1, 40, 20));
        lines.Append(TestScanLineXYP("序言", 8, 40, 40));
        lines.Append(TestScanLineXYP("目录", 22, 160, 40));
        lines.Append(TestScanLineXYP("第一章 概论", 22, 48, 80));
        ScanLine p3 = TestScanLineXYP("3", 22, 292, 80);
        p3.dx = 10;
        lines.Append(p3);
        lines.Append(TestScanLineXYP("第二章 方法", 23, 48, 60));
        ScanLine p18 = TestScanLineXYP("18", 23, 292, 60);
        p18.dx = 12;
        lines.Append(p18);
        lines.Append(TestScanLineXYP("第三章 结论", 23, 48, 90));
        ScanLine p40 = TestScanLineXYP("40", 23, 292, 90);
        p40.dx = 12;
        lines.Append(p40);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 80, roots);
        bool lateOk = ran && ExtractedFindContaining(roots, "第一章") && ExtractedFindContaining(roots, "第二章") &&
                      ExtractedFindContaining(roots, "第三章");
        if (lateOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-late-multipage-toc", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        lines.Append(TestScanLineXYP("第一章 开篇", 1, 48, 80));
        ScanLine p1 = TestScanLineXYP("1", 1, 292, 80);
        p1.dx = 10;
        lines.Append(p1);
        lines.Append(TestScanLineXYP("插图", 2, 120, 80));
        lines.Append(TestScanLineXYP("说明", 3, 120, 80));
        lines.Append(TestScanLineXYP("第二章 展开", 4, 48, 60));
        ScanLine p20 = TestScanLineXYP("20", 4, 292, 60);
        p20.dx = 12;
        lines.Append(p20);
        lines.Append(TestScanLineXYP("第三章 收束", 4, 48, 90));
        ScanLine p44 = TestScanLineXYP("44", 4, 292, 90);
        p44.dx = 12;
        lines.Append(p44);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 40, roots);
        bool gapOk = ran && ExtractedFindContaining(roots, "第一章") && ExtractedFindContaining(roots, "第二章") &&
                     ExtractedFindContaining(roots, "第三章");
        if (gapOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-toc-gap-pages", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        lines.Append(TestScanLineXYP("第一章 这是一个很长的标题，", 1, 48, 80));
        lines.Append(TestScanLineXYP("还没写完的后半段", 2, 48, 40));
        ScanLine p13 = TestScanLineXYP("13", 2, 292, 40);
        p13.dx = 12;
        lines.Append(p13);
        lines.Append(TestScanLineXYP("第二章 下一章", 2, 48, 80));
        ScanLine p28 = TestScanLineXYP("28", 2, 292, 80);
        p28.dx = 12;
        lines.Append(p28);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 10, roots);
        ExtractedTocItem* ch1 = ExtractedFindContaining(roots, "很长的标题");
        ExtractedTocItem* half = ExtractedFindContaining(roots, "还没写完的后半段");
        bool wrapOk =
            ran && ch1 && half == ch1 && str::Find(ch1->title, "后半段") && ExtractedFindContaining(roots, "第二章");
        if (wrapOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-cross-page-wrap", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        ScanLine body1 = TestScanLineXYP("正文里普通的一段话写得很长所以不是标题", 2, 48, 40);
        body1.fontSize = 11;
        body1.dy = 11;
        lines.Append(body1);
        ScanLine h1 = TestScanLineXYP("过去也不是没有厌学的孩子", 2, 72, 80);
        h1.fontSize = 18;
        h1.dy = 18;
        h1.bold = true;
        lines.Append(h1);
        ScanLine body2 = TestScanLineXYP("接着又是一段很长的说明文字用来充当正文", 2, 48, 120);
        body2.fontSize = 11;
        body2.dy = 11;
        lines.Append(body2);
        ScanLine h2 = TestScanLineXYP("成绩册背后的故事", 3, 72, 60);
        h2.fontSize = 18;
        h2.dy = 18;
        h2.bold = true;
        lines.Append(h2);
        ScanLine clause = TestScanLineXYP("一、不应作为书的骨架", 3, 48, 200);
        clause.fontSize = 11;
        clause.dy = 11;
        lines.Append(clause);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 8, roots);
        bool styleOk =
            ran && ExtractedFindContaining(roots, "厌学的孩子") && ExtractedFindContaining(roots, "成绩册背后") &&
            !ExtractedHasPrefix(roots, "一、不应作为书的骨架") && !ExtractedFindContaining(roots, "普通的一段话");
        if (styleOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-no-printed-style", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 5, 144, 66));
        lines.Append(TestScanLineXYP("第六课社会主义民主建设", 5, 32, 110));
        lines.Append(TestScanLineXYP("加强社会主义民主建设", 5, 49, 156));
        ScanLine p10 = TestScanLineXYP("10", 5, 293, 159);
        p10.dx = 12;
        lines.Append(p10);
        lines.Append(TestScanLineXYP("1819 宪法是国家的根本大法", 5, 49, 188));
        lines.Append(TestScanLineXYP("正确对待就业和升学", 5, 50, 337));
        ScanLine p79 = TestScanLineXYP("79", 5, 292, 339);
        p79.dx = 12;
        lines.Append(p79);
        lines.Append(TestScanLineXYP("加强社会主义", 15, 40, 80));
        lines.Append(TestScanLineXYP("民主建设", 15, 40, 96));
        Vec<char*> labels;
        for (int p = 1; p <= 90; p++) {
            if (p <= 5) {
                labels.Append(str::Dup(str::FormatTemp("%d", p)));
            } else {
                labels.Append(str::Dup(str::FormatTemp("%d", p - 5)));
            }
        }
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractBookToc(nullptr, lines, labels, 90, roots);
        ExtractedTocItem* strengthen = ExtractedFindContaining(roots, "加强社会主义民主建设");
        ExtractedTocItem* job = ExtractedFindContaining(roots, "正确对待就业和升学");
        ExtractedTocItem* law = ExtractedFindContaining(roots, "宪法是国家的根本大法");
        bool destOk = ran && strengthen && strengthen->pageNo == 15 && job && job->pageNo == 84 && law && law->title &&
                      !str::Find(law->title, "1819") && !str::StartsWith(law->title, "19");
        if (destOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("jianshe-label-dest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
        for (int i = 0; i < labels.Size(); i++) {
            str::Free(labels[i]);
        }
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 5, 144, 66));
        lines.Append(TestScanLineXYP("第六课社会主义民主建设", 5, 32, 110));
        ScanLine p1 = TestScanLineXYP("1", 5, 293, 112);
        p1.dx = 8;
        lines.Append(p1);
        lines.Append(TestScanLineXYP("加强社会主义民主建设", 5, 49, 156));
        ScanLine p10 = TestScanLineXYP("10", 5, 293, 159);
        p10.dx = 12;
        lines.Append(p10);
        lines.Append(TestScanLineXYP("加强社会主义民主建设", 6, 40, 80));
        Vec<char*> labels;
        for (int p = 1; p <= 90; p++) {
            if (p <= 5) {
                labels.Append(str::Dup(str::FormatTemp("%d", p)));
            } else {
                labels.Append(str::Dup(str::FormatTemp("%d", p - 5)));
            }
        }
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractBookToc(nullptr, lines, labels, 90, roots);
        ExtractedTocItem* strengthen = ExtractedFindContaining(roots, "加强社会主义民主建设");
        bool destOk = ran && strengthen && strengthen->pageNo == 15;
        if (destOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("jianshe-printed-over-blurb", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
        for (int i = 0; i < labels.Size(); i++) {
            str::Free(labels[i]);
        }
    }
    {
        // Identity page labels + no body footers. Front matter is PDF 1-5;
        // printed page 1 is PDF 8. Two body title hits vote offset=7; a third
        // TOC row with no body text must still land on printed+offset.
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 3, 144, 66));
        lines.Append(TestScanLineXYP("第一章 智力加油站", 3, 32, 110));
        ScanLine p1 = TestScanLineXYP("1", 3, 293, 110);
        p1.dx = 12;
        lines.Append(p1);
        lines.Append(TestScanLineXYP("第二章 相关理论基础", 3, 32, 140));
        ScanLine p12 = TestScanLineXYP("12", 3, 293, 140);
        p12.dx = 12;
        lines.Append(p12);
        lines.Append(TestScanLineXYP("第三章 结束语", 3, 32, 170));
        ScanLine p30 = TestScanLineXYP("30", 3, 293, 170);
        p30.dx = 12;
        lines.Append(p30);
        lines.Append(TestScanLineXYP("第一章 智力加油站", 8, 40, 80));
        lines.Append(TestScanLineXYP("第二章 相关理论基础", 19, 40, 80));
        Vec<char*> labels;
        for (int p = 1; p <= 50; p++) {
            labels.Append(str::Dup(str::FormatTemp("%d", p)));
        }
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractBookToc(nullptr, lines, labels, 50, roots);
        ExtractedTocItem* ch1 = ExtractedFindContaining(roots, "智力加油站");
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "相关理论基础");
        ExtractedTocItem* ch3 = ExtractedFindContaining(roots, "结束语");
        bool destOk = ran && ch1 && ch1->pageNo == 8 && ch1->printedPage == 1 && ch2 && ch2->pageNo == 19 && ch3 &&
                      ch3->pageNo == 37 && ExtractedHasPrintedBookCalib(roots);
        if (destOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-title-offset", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
        for (int i = 0; i < labels.Size(); i++) {
            str::Free(labels[i]);
        }
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 164, 80));
        lines.Append(TestScanLineXYP("1．善解人意的家长反而教育不了孩子", 1, 58, 120));
        ScanLine p46 = TestScanLineXYP("(46)", 1, 300, 120);
        p46.dx = 28;
        lines.Append(p46);
        lines.Append(TestScanLineXYP("2．不了解孩子的家长不会成功", 1, 58, 140));
        ScanLine p47 = TestScanLineXYP("(47)", 1, 300, 140);
        p47.dx = 28;
        lines.Append(p47);
        lines.Append(TestScanLineXYP("1．善解人意的家长反而教育不了孩子", 60, 40, 80));
        lines.Append(TestScanLineXYP("2．不了解孩子的家长不会成功", 58, 40, 80));
        Vec<char*> labels;
        for (int p = 1; p <= 80; p++) {
            labels.Append(str::Dup(str::FormatTemp("%d", p)));
        }
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractBookToc(nullptr, lines, labels, 80, roots);
        ExtractedTocItem* a = ExtractedFindContaining(roots, "善解人意");
        ExtractedTocItem* b = ExtractedFindContaining(roots, "不了解孩子");
        bool orderOk = ran && a && b && a->printedPage == 46 && b->printedPage == 47 && a->pageNo > 0 &&
                       b->pageNo >= a->pageNo && b->pageNo != 58;
        if (orderOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-mono-dest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
        for (int i = 0; i < labels.Size(); i++) {
            str::Free(labels[i]);
        }
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("1．善解人意");
        a->rawTitle = str::Dup("1．善解人意");
        a->printedPage = 46;
        a->pageNo = 60;
        a->bodyMatched = true;
        a->source = ExtractedTocSource::PrintedToc;
        auto* b = new ExtractedTocItem;
        b->title = str::Dup("2．不了解孩子");
        b->rawTitle = str::Dup("2．不了解孩子");
        b->printedPage = 47;
        b->pageNo = 58;
        b->bodyMatched = true;
        b->source = ExtractedTocSource::PrintedToc;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        roots.Append(b);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        bool ok = sess && sess->map.offset == 14 && a->pageNo == 60 && b->pageNo >= 60 && b->pageNo != 58;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-mono dest a=%d b=%d off=%d\n", a->pageNo, b->pageNo,
                 sess ? sess->map.offset : -1);
        }
        DeleteTocCalibSession(sess);
    }
    {
        Vec<TocCalibMapRow> rows;
        TocCalibMapRow a;
        a.printedPage = 13;
        a.pdfPage = 25;
        a.identPage = 25;
        a.bodyMatched = true;
        a.verified = true;
        rows.Append(a);
        TocCalibMapRow b;
        b.printedPage = 35;
        b.pdfPage = 47;
        b.identPage = 47;
        b.bodyMatched = true;
        rows.Append(b);
        TocCalibMapRow c;
        c.printedPage = 80;
        c.pdfPage = 0;
        c.identPage = 0;
        rows.Append(c);
        TocCalibMapRow d;
        d.printedPage = 50;
        d.pdfPage = 99;
        d.identPage = 99;
        rows.Append(d);
        int off = TocCalibSolveOffset(rows);
        TocCalibApplyOffset(rows, off);
        bool first = off == 12 && rows[2].pdfPage == 92;
        rows[3].printedPage = 51;
        off = TocCalibSolveOffset(rows);
        bool majority = off == 12;
        if (first && majority) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-offset first=%d majority=%d off=%d pdf3=%d\n", first ? 1 : 0,
                 majority ? 1 : 0, off, rows[2].pdfPage);
        }
    }
    {
        int exact = TocCalibTitleMatchScore("第一章 智力加油站", "第一章 智力加油站");
        int compact = TocCalibTitleMatchScore("第一章智力加油站", "第一章 智力加油站");
        int wrap = TocCalibTitleMatchScore("第一章 智力", "第一章 智力加油站");
        int miss = TocCalibTitleMatchScore("相关理论基础", "第一章 智力加油站");
        int joined = TocCalibTitleMatchScore("心得二—良好的素质是孩子成功的保证", "心得二良好的素质是孩子成功的保证");
        int shortT = TocCalibTitleMatchScore("后记", "后记");
        ExtractedTocItem it;
        it.title = (char*)"结束语";
        it.printedPage = 30;
        it.pageNo = 0;
        it.confidence = 40;
        bool applied = TocCalibApplyNearHit(&it, 37, 12.f, 80.f, 8, 37);
        bool nearOk = exact >= 6 && compact >= 6 && wrap >= 4 && miss == 0 && joined >= 8 && shortT >= 2 && applied &&
                      it.bodyMatched && it.pageNo == 37 && it.verified && it.confidence >= 85;
        if (nearOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-near exact=%d compact=%d wrap=%d miss=%d applied=%d page=%d ver=%d conf=%d\n",
                 exact, compact, wrap, miss, applied ? 1 : 0, it.pageNo, it.verified ? 1 : 0, it.confidence);
        }
        it.title = nullptr;
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("第一章 智力加油站");
        a->rawTitle = str::Dup("第一章 智力加油站");
        a->printedPage = 1;
        a->pageNo = 8;
        a->bodyMatched = true;
        a->verified = true;
        a->confidence = 90;
        a->source = ExtractedTocSource::PrintedToc;
        a->tocPageNo = 3;
        auto* b = new ExtractedTocItem;
        b->title = str::Dup("第二章 相关理论基础");
        b->rawTitle = str::Dup("第二章 相关理论基础");
        b->printedPage = 12;
        b->pageNo = 19;
        b->bodyMatched = true;
        b->verified = true;
        b->confidence = 90;
        b->source = ExtractedTocSource::PrintedToc;
        b->tocPageNo = 3;
        auto* c = new ExtractedTocItem;
        c->title = str::Dup("第三章 结束语");
        c->rawTitle = str::Dup("第三章 结束语");
        c->printedPage = 30;
        c->pageNo = 0;
        c->bodyMatched = false;
        c->confidence = 40;
        c->source = ExtractedTocSource::PrintedToc;
        c->tocPageNo = 3;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        roots.Append(b);
        roots.Append(c);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        bool sessOk = sess && sess->rows.Size() == 3 && sess->map.offset == 7 && c->pageNo == 37;
        bool committed = sessOk && TocCalibCommitPrinted(sess, 0, 2);
        bool after = committed && sess->map.offset == 7 && c->pageNo == 37 && a->verified && a->printedPage == 2 &&
                     a->pageNo == 9;
        if (sessOk && after) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-session ok=%d after=%d off=%d pdf3=%d\n", sessOk ? 1 : 0, after ? 1 : 0,
                 sess ? sess->map.offset : -1, c->pageNo);
        }
        DeleteTocCalibSession(sess);
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("第一章");
        a->rawTitle = str::Dup("第一章");
        a->printedPage = 1;
        a->pageNo = 8;
        a->bodyMatched = true;
        a->source = ExtractedTocSource::PrintedToc;
        auto* b = new ExtractedTocItem;
        b->title = str::Dup("结束语");
        b->rawTitle = str::Dup("结束语");
        b->printedPage = 30;
        b->pageNo = 37;
        b->bodyMatched = false;
        b->source = ExtractedTocSource::PrintedToc;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        roots.Append(b);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        bool locked = sess && TocCalibSetOffset(sess, 14);
        bool ok = locked && sess->offsetLocked && sess->map.offset == 14 && a->pageNo == 15 && b->pageNo == 44;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-set-offset locked=%d off=%d a=%d b=%d\n", locked ? 1 : 0,
                 sess ? sess->map.offset : -1, a->pageNo, b->pageNo);
        }
        DeleteTocCalibSession(sess);
    }
    {
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
        bool autoOff =
            sess && sess->map.offset == 20 && ch->pageNo == 21 && pref->pageNo == 17 && pref->printedPage == 0;
        bool forced = autoOff && TocCalibSetOffset(sess, 21);
        forced = forced && pref->pageNo == 17 && ch->pageNo == 22 && ch2->pageNo == 36;
        if (forced) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-front-label off=%d pref=%d printed=%d ch=%d locked=%d\n",
                 sess ? sess->map.offset : -1, pref->pageNo, pref->printedPage, ch->pageNo,
                 sess && sess->offsetLocked ? 1 : 0);
        }
        DeleteTocCalibSession(sess);
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("第三课");
        a->rawTitle = str::Dup("第三课");
        a->printedPage = 0;
        a->pageNo = 59;
        a->source = ExtractedTocSource::PrintedToc;
        auto* b = new ExtractedTocItem;
        b->title = str::Dup("第五课");
        b->rawTitle = str::Dup("第五课");
        b->printedPage = 1;
        b->pageNo = 59;
        b->bodyMatched = true;
        b->source = ExtractedTocSource::PrintedToc;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        roots.Append(b);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        bool locked = sess && TocCalibSetOffset(sess, 58);
        int guessEmpty = TocCalibDisplayPrinted(sess, 0);
        int guessKept = TocCalibDisplayPrinted(sess, 1);
        bool ok = locked && guessEmpty == 1 && guessKept == 1 && a->printedPage == 1;
        a->printedPage = 12;
        ok = ok && TocCalibDisplayPrinted(sess, 0) == 12;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-display-printed locked=%d empty=%d kept=%d stored=%d\n", locked ? 1 : 0,
                 guessEmpty, guessKept, a->printedPage);
        }
        DeleteTocCalibSession(sess);
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("第八课");
        a->rawTitle = str::Dup("第八课");
        a->printedPage = 0;
        a->pageNo = 45;
        a->source = ExtractedTocSource::PrintedToc;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        int prefill = TocCalibDisplayPrinted(sess, 0);
        bool ok = sess && sess->editPdf && a->printedPage == 0 && prefill == 0 && a->pageNo == 45;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-prefill-pdf printed=%d prefill=%d pdf=%d editPdf=%d\n", a->printedPage,
                 prefill, a->pageNo, sess && sess->editPdf ? 1 : 0);
        }
        DeleteTocCalibSession(sess);
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("译者序");
        a->rawTitle = str::Dup("译者序");
        a->printedPage = 1;
        a->pageNo = 22;
        a->source = ExtractedTocSource::PrintedToc;
        auto* b = new ExtractedTocItem;
        b->title = str::Dup("第一章");
        b->rawTitle = str::Dup("第一章");
        b->printedPage = 4;
        b->pageNo = 25;
        b->source = ExtractedTocSource::PrintedToc;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        roots.Append(b);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        bool noAutoPin = sess && sess->rows.Size() == 2 && !sess->rows[0].pdfPinned;
        bool ok = noAutoPin && TocCalibCommitRow(sess, 0, 1, 15, 14, false);
        ok = ok && a->pageNo == 15 && b->pageNo == 18 && sess->map.offset == 14 && !sess->rows[0].pdfPinned;
        bool pin = ok && TocCalibCommitRow(sess, 0, 1, 16, 14, true);
        pin = pin && a->pageNo == 16 && b->pageNo == 18 && sess->rows[0].pdfPinned && sess->map.offset == 14;
        if (ok && pin) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-commit-row auto=%d ok=%d pin=%d a=%d b=%d off=%d pinned=%d\n",
                 noAutoPin ? 1 : 0, ok ? 1 : 0, pin ? 1 : 0, a->pageNo, b->pageNo, sess ? sess->map.offset : -1,
                 sess && sess->rows.Size() > 0 && sess->rows[0].pdfPinned ? 1 : 0);
        }
        DeleteTocCalibSession(sess);
    }
    {
        auto* a = new ExtractedTocItem;
        a->title = str::Dup("第一章");
        a->rawTitle = str::Dup("第一章");
        a->printedPage = 1;
        a->pageNo = 8;
        a->bodyMatched = true;
        a->source = ExtractedTocSource::PrintedToc;
        auto* b = new ExtractedTocItem;
        b->title = str::Dup("结束语");
        b->rawTitle = str::Dup("结束语");
        b->printedPage = 30;
        b->pageNo = 37;
        b->bodyMatched = true;
        b->source = ExtractedTocSource::PrintedToc;
        Vec<ExtractedTocItem*> roots;
        roots.Append(a);
        roots.Append(b);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        int idx = sess ? TocCalibAddManualItem(sess, "新增章节", 22, 10.f, 20.f, 0) : -1;
        bool added = idx == 1 && sess && sess->rows.Size() == 3 && sess->rows[1].pdfPinned && sess->rows[1].item &&
                     sess->rows[1].item->pageNo == 22 && str::Eq(sess->rows[1].item->title, "新增章节");
        bool kept = added && TocCalibSetOffset(sess, 14) && sess->rows[1].item->pageNo == 22 && a->pageNo == 15 &&
                    b->pageNo == 44;
        if (kept) {
            (*pass)++;
        } else {
            (*fail)++;
            logf(
                "BookToc fail book-calib-add-manual idx=%d n=%d pin=%d pdf=%d a=%d b=%d\n", idx,
                sess ? sess->rows.Size() : -1,
                sess && idx >= 0 && idx < sess->rows.Size() && sess->rows[idx].pdfPinned ? 1 : 0,
                sess && idx >= 0 && idx < sess->rows.Size() && sess->rows[idx].item ? sess->rows[idx].item->pageNo : -1,
                a->pageNo, b->pageNo);
        }
        DeleteTocCalibSession(sess);
    }
    {
        auto* part = new ExtractedTocItem;
        part->title = str::Dup("第一部分");
        part->rawTitle = str::Dup("第一部分");
        part->printedPage = 1;
        part->pageNo = 8;
        part->level = 1;
        part->source = ExtractedTocSource::PrintedToc;
        auto* ch = new ExtractedTocItem;
        ch->title = str::Dup("第一章");
        ch->rawTitle = str::Dup("第一章");
        ch->printedPage = 3;
        ch->pageNo = 10;
        ch->level = 2;
        ch->source = ExtractedTocSource::PrintedToc;
        auto* sec = new ExtractedTocItem;
        sec->title = str::Dup("什么是情感智力");
        sec->rawTitle = str::Dup("什么是情感智力");
        sec->printedPage = 4;
        sec->pageNo = 11;
        sec->level = 3;
        sec->source = ExtractedTocSource::PrintedToc;
        ch->children.Append(sec);
        part->children.Append(ch);
        Vec<ExtractedTocItem*> roots;
        roots.Append(part);
        TocCalibSession* sess = TocCalibSessionFromExtracted(roots, nullptr, false);
        bool ok = sess && sess->rows.Size() == 3 && sess->rows[0].depth == 1 && sess->rows[1].depth == 2 &&
                  sess->rows[2].depth == 3 && ch->parent == part && sec->parent == ch && ch->expanded;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("BookToc fail book-calib-depth n=%d d0=%d d1=%d d2=%d\n", sess ? sess->rows.Size() : -1,
                 sess && sess->rows.Size() > 0 ? sess->rows[0].depth : -1,
                 sess && sess->rows.Size() > 1 ? sess->rows[1].depth : -1,
                 sess && sess->rows.Size() > 2 ? sess->rows[2].depth : -1);
        }
        DeleteTocCalibSession(sess);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 144, 66));
        lines.Append(TestScanLineXYP("第七课我国宪法·····18宪法是国家的根本大法", 1, 32, 172));
        lines.Append(TestScanLineXYP("第八课民法通则", 1, 32, 214));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* ke7 = ExtractedFindContaining(roots, "第七课");
        ExtractedTocItem* law = ExtractedFindContaining(roots, "宪法是国家的根本大法");
        bool splitOk = ran && ke7 && law && ke7 != law && !str::Find(ke7->title, "宪法是国家的根本大法");
        if (splitOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-split-mid-page", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 10, 160, 40));
        lines.Append(TestScanLineXYP("第一章对考试的错误认识", 11, 40, 80));
        ScanLine p13 = TestScanLineXYP("(13)", 11, 304, 80);
        p13.dx = 32;
        lines.Append(p13);
        lines.Append(TestScanLineXYP("第四章有关高考的重要知识", 12, 40, 80));
        ScanLine p100 = TestScanLineXYP("(100)", 12, 304, 80);
        p100.dx = 36;
        lines.Append(p100);
        lines.Append(TestScanLineXYP("第五章反映在作文中的高中生心理活动", 13, 61, 260));
        ScanLine p140 = TestScanLineXYP("(140)", 13, 304, 260);
        p140.dx = 40;
        lines.Append(p140);
        lines.Append(TestScanLineXYP("哪些父亲能赢得孩子的尊敬", 154, 40, 80));
        Vec<char*> labels;
        for (int p = 1; p <= 160; p++) {
            if (p <= 9) {
                labels.Append(str::Dup(str::FormatTemp("%c", 'A' + (p - 1))));
            } else if (p <= 14) {
                const char* rom[] = {"i", "ii", "iii", "iv", "v"};
                labels.Append(str::Dup(rom[p - 10]));
            } else {
                labels.Append(str::Dup(str::FormatTemp("%d", p - 14)));
            }
        }
        Vec<ExtractedTocItem*> roots;
        bool ran = ExtractBookToc(nullptr, lines, labels, 160, roots);
        ExtractedTocItem* ch5 = ExtractedFindContaining(roots, "第五章");
        bool destOk = ran && ch5 && ch5->pageNo == 154 && ch5->pageNo != 13 && ch5->pageNo != 10;
        if (destOk) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("gaokao-not-toc-dest page=%d\n", ch5 ? ch5->pageNo : -1);
            LogBookExtractFail("gaokao-not-toc-dest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
        for (int i = 0; i < labels.Size(); i++) {
            str::Free(labels[i]);
        }
    }
    {
        ExtractedTocItem* ke8cut = NewItem("第八课一般违法行为应受到的行政制裁", 1, 0, 0, 1);
        bool keepWei =
            ke8cut && ke8cut->title && str::Find(ke8cut->title, "行政制裁") && str::Find(ke8cut->title, "一般违法");
        delete ke8cut;
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXY("目录", 145, 143));
        ScanLine ke1a = TestScanLineXY("第一课", 30, 185);
        ke1a.dx = 40;
        ScanLine ke1b = TestScanLineXY("青少年要学习《法律常", 82, 185);
        ke1b.dx = 90;
        lines.Append(ke1a);
        lines.Append(ke1b);
        ScanLine ke2a = TestScanLineXY("第二课", 30, 203);
        ke2a.dx = 32;
        ScanLine ke2b = TestScanLineXY("我国的法律·", 72, 203);
        ke2b.dx = 60;
        lines.Append(ke2a);
        lines.Append(ke2b);
        ScanLine ke6a = TestScanLineXY("第六课", 31, 270);
        ke6a.dx = 32;
        ke6a.dy = 13;
        ScanLine ke6b = TestScanLineXY("我国的国家机构⋯····", 72, 270.8f);
        ke6b.dx = 101;
        ke6b.dy = 11;
        lines.Append(ke6a);
        lines.Append(ke6b);
        ScanLine p6 = TestScanLineXY("56", 297, 273);
        p6.dx = 12;
        p6.dy = 6;
        lines.Append(p6);
        ScanLine ke8 = TestScanLineXY("第八课一般违法行为应受到的行政制裁·", 31, 303);
        ke8.dx = 195;
        lines.Append(ke8);
        ScanLine p8 = TestScanLineXY("-77", 293, 307);
        p8.dx = 16;
        p8.dy = 7;
        lines.Append(p8);
        ScanLine ke10 = TestScanLineXY("第十课，犯罪应受刑罚制裁···", 32, 337);
        ke10.dx = 144;
        lines.Append(ke10);
        ScanLine p10a = TestScanLineXY("1", 283, 340);
        p10a.dx = 8;
        p10a.dy = 9;
        ScanLine p10b = TestScanLineXY("05", 295, 340);
        p10b.dx = 14;
        p10b.dy = 9;
        lines.Append(p10a);
        lines.Append(p10b);
        ScanLine ke12 = TestScanLineXY("第十二课增强社会主义法制观念，", 32, 371);
        ke12.dx = 165;
        ScanLine ke12b = TestScanLineXY("提高守法自觉性··", 84, 388);
        ke12b.dx = 101;
        lines.Append(ke12);
        lines.Append(ke12b);
        ScanLine p12 = TestScanLineXY("·..·124", 277, 392);
        p12.dx = 31;
        p12.dy = 8;
        lines.Append(p12);
        lines.Append(TestScanLineXYP("青少年要学习《法律常识》", 2, 83, 88));
        lines.Append(TestScanLineXYP("《法律常识》", 2, 140, 111));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 2, roots);
        ExtractedTocItem* ke1 = ExtractedFindContaining(roots, "第一课");
        ExtractedTocItem* ke6n = ExtractedFindContaining(roots, "第六课");
        ExtractedTocItem* ke8n = ExtractedFindContaining(roots, "第八课");
        ExtractedTocItem* ke10n = ExtractedFindContaining(roots, "第十课");
        ExtractedTocItem* ke12n = ExtractedFindContaining(roots, "第十二课");
        bool falvOk = keepWei && ran && ke1 && str::Find(ke1->title, "法律常") && ke1->children.Size() == 0 && ke6n &&
                      str::Find(ke6n->title, "国家机构") && ke6n->children.Size() == 0 && ke8n &&
                      str::Find(ke8n->title, "行政制裁") && ke10n && str::Find(ke10n->title, "犯罪应受刑罚制裁") &&
                      ke12n && str::Find(ke12n->title, "提高守法自觉性") && ke12n->children.Size() == 0;
        if (falvOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("falv-changshi", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        char* slashTitle = nullptr;
        int slashPage = 0;
        bool slashOk = ParsePrintedTocLine("第一章普通高中新课程简介／1", &slashTitle, &slashPage) && slashPage == 1 &&
                       slashTitle && str::Find(slashTitle, "普通高中新课程简介");
        str::Free(slashTitle);
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("出版人：所广一", 1, 25, 77));
        lines.Append(TestScanLineXYP("责任编辑殷欢", 1, 25, 93));
        lines.Append(TestScanLineXYP("责任校对贾静芳", 1, 25, 109));
        lines.Append(TestScanLineXYP("图书在版编目（CIP）数据", 1, 45, 183));
        lines.Append(TestScanLineXYP("I.①学⋯Ⅱ.①赵⋯②蔡·⋯Ⅲ.①课程一高中一教", 1, 47, 256));
        lines.Append(TestScanLineXYP("ISBN978-7-5041-6381-3", 1, 47, 237));
        lines.Append(TestScanLineXYP("定价26.00元", 1, 247, 541));
        lines.Append(TestScanLineXYP("目录", 2, 169, 192));
        lines.Append(TestScanLineXYP("第一章普通高中新课程简介／1", 2, 49, 281));
        lines.Append(TestScanLineXYP("第一节普通高中新课程特点／1", 2, 69, 305));
        lines.Append(TestScanLineXYP("第二节普通高中新课程设置／3", 2, 69, 321));
        lines.Append(TestScanLineXYP("第三节普通高中新课程内容／11", 2, 69, 337));
        lines.Append(TestScanLineXYP("第四节普通高中新课程评价／13", 2, 69, 354));
        lines.Append(TestScanLineXYP("第二章高中各学科学习方法指导／15", 2, 49, 378));
        lines.Append(TestScanLineXYP("第一节语文学习方法指导／15", 2, 69, 401));
        ScanLine ke4a = TestScanLineXYP("第四节", 2, 69, 451);
        ke4a.dx = 32;
        ScanLine ke4b = TestScanLineXYP("物理学习方法指导／68", 2, 111, 451.8f);
        ke4b.dx = 116;
        lines.Append(ke4a);
        lines.Append(ke4b);
        lines.Append(TestScanLineXYP("第一章普通高中新课程简介", 3, 91, 150));
        lines.Append(TestScanLineXYP("第一节普通高中新课程特点", 3, 118, 307));
        bool xuehuiBook = ClassifyExtractTocDoc(lines, 3, "学会学习高中生学习指导.pdf") == ExtractTocDocClass::Book;
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 3, roots);
        ExtractedTocItem* ch1 = ExtractedFindContaining(roots, "第一章");
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "第二章");
        bool xuehuiOk = slashOk && xuehuiBook && ran && ExtractedHasPrintedTocBookmark(roots, 2) && ch1 &&
                        str::Find(ch1->title, "普通高中新课程简介") && ExtractedHasPrefix(ch1->children, "第一节") &&
                        ch2 && ExtractedFindContaining(ch2->children, "物理学习方法指导") &&
                        !ExtractedFindContaining(roots, "出版人") && !ExtractedFindContaining(roots, "图书在版编目") &&
                        !ExtractedFindContaining(roots, "责任编辑");
        if (xuehuiOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("xuehui-xuexi", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 164, 161));
        lines.Append(TestScanLineXYP("序章对高考的错误认识", 1, 46, 241));
        lines.Append(TestScanLineXYP("——家长的旧观念拖孩子后腿", 1, 111, 260));
        ScanLine a1 = TestScanLineXYP("·认为“高考是万恶之源”是错误的", 1, 61, 279);
        a1.dx = 166;
        lines.Append(a1);
        ScanLine p1 = TestScanLineXYP("(2)", 1, 310, 281);
        p1.dx = 24;
        p1.dy = 10;
        lines.Append(p1);
        ScanLine a2 = TestScanLineXYP("·认为“高考竞争对孩子不利”是错误的", 1, 61, 297);
        a2.dx = 188;
        lines.Append(a2);
        ScanLine p2 = TestScanLineXYP("(4)", 1, 310, 297);
        p2.dx = 27;
        p2.dy = 12;
        lines.Append(p2);
        ScanLine ch1a = TestScanLineXYP("第一章", 1, 46, 384);
        ch1a.dx = 38;
        ScanLine ch1b = TestScanLineXYP("对考试的错误认识", 1, 102, 384);
        ch1b.dx = 100;
        lines.Append(ch1a);
        lines.Append(ch1b);
        lines.Append(TestScanLineXYP("~家长的无知害了孩子", 1, 125, 402));
        lines.Append(TestScanLineXYP("1．成绩册是孩子学习成绩的准确资料吗？不是！", 1, 58, 420));
        ScanLine p13 = TestScanLineXYP("(13)", 1, 300, 420);
        p13.dx = 38;
        lines.Append(p13);
        lines.Append(TestScanLineXYP("2．“偏差值”是公正客观的评价吗？不是！", 1, 57, 439));
        ScanLine p15 = TestScanLineXYP("(15)", 1, 300, 438);
        p15.dx = 40;
        lines.Append(p15);
        lines.Append(TestScanLineXYP("3．数学、英语成绩好的孩子就一定聪明吗?", 1, 58, 457));
        lines.Append(TestScanLineXYP("不是！", 1, 74, 474));
        ScanLine p19 = TestScanLineXYP("(19)", 1, 306, 475);
        p19.dx = 31;
        lines.Append(p19);
        ScanLine ch2a = TestScanLineXYP("第二童", 1, 59, 500);
        ch2a.dx = 36;
        ch2a.y = 500;
        ScanLine ch2b = TestScanLineXYP("被忽视了的“高考父亲学”", 1, 114, 500);
        ch2b.dx = 144;
        lines.Append(ch2a);
        lines.Append(ch2b);
        lines.Append(TestScanLineXYP("“考试制度使学生学坏”的想法是错误的", 1, 69, 315));
        lines.Append(TestScanLineXYP("1．善解人意的家长反而教育不了孩子", 1, 71, 520));
        lines.Append(TestScanLineXYP("2．不了解孩子的家长不会成功", 1, 58, 538));
        lines.Append(TestScanLineXYP("6．什么时候拒绝孩子、什么时候同意孩子？", 1, 71, 556));
        lines.Append(TestScanLineXYP("7.高考正是父亲发挥作用的时候", 1, 58, 574));
        lines.Append(TestScanLineXYP("第三章值得注意的高考新常识", 1, 49, 600));
        lines.Append(TestScanLineXYP("家长的疑虑与苦恼都迎刃而解", 1, 127, 618));
        lines.Append(TestScanLineXYP("1．一天学习多少小时为宜？", 1, 64, 636));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* xu = ExtractedFindContaining(roots, "序章");
        ExtractedTocItem* ke1 = ExtractedFindContaining(roots, "第一章");
        ExtractedTocItem* ke2 = ExtractedFindContaining(roots, "第二童");
        if (!ke2) {
            ke2 = ExtractedFindContaining(roots, "第二章");
        }
        ExtractedTocItem* ke3 = ExtractedFindContaining(roots, "第三章");
        ExtractedTocItem* grade = ExtractedFindContaining(roots, "成绩册");
        ExtractedTocItem* bias = ExtractedFindContaining(roots, "偏差值");
        ExtractedTocItem* evil = ExtractedFindContaining(roots, "高考是万恶之源");
        ExtractedTocItem* race = ExtractedFindContaining(roots, "高考竞争对孩子不利");
        ExtractedTocItem* exam = ExtractedFindContaining(roots, "考试制度使学生学坏");
        ExtractedTocItem* kind = ExtractedFindContaining(roots, "善解人意");
        ExtractedTocItem* refuse = ExtractedFindContaining(roots, "什么时候拒绝");
        ExtractedTocItem* father = ExtractedFindContaining(roots, "高考正是父亲");
        ExtractedTocItem* ign = ExtractedFindContaining(roots, "家长的无知");
        ExtractedTocItem* smart = ExtractedFindContaining(roots, "一定聪明");
        bool titlesOk =
            ran && xu && str::Find(xu->title, "对高考的错误认识") && str::Find(xu->title, "家长的旧观念拖孩子后腿") &&
            !ExtractedFindContaining(xu->children, "家长的旧观念") && evil && race && exam && ke1 &&
            str::Find(ke1->title, "对考试的错误认识") && ign && str::Find(ke1->title, "家长的无知") &&
            !ExtractedFindContaining(ke1->children, "家长的无知") && ke2 && str::Find(ke2->title, "高考父亲学") &&
            grade && bias && grade->title && !str::Find(grade->title, "(13)") && evil->title &&
            !str::Find(evil->title, "(2)") && smart && str::Find(smart->title, "不是") && smart->printedPage == 19;
        bool nestOk = titlesOk && xu && ExtractedFindContaining(xu->children, "万恶之源") &&
                      ExtractedFindContaining(xu->children, "高考竞争") &&
                      !ExtractedFindContaining(evil->children, "高考竞争") &&
                      !ExtractedFindContaining(race->children, "考试制度") && ke1 && ke2 && kind && refuse && father &&
                      ExtractedFindContaining(ke2->children, "善解人意") &&
                      ExtractedFindContaining(ke2->children, "什么时候拒绝") &&
                      ExtractedFindContaining(ke2->children, "高考正是父亲") &&
                      !ExtractedFindContaining(kind->children, "什么时候拒绝") &&
                      !ExtractedFindContaining(kind->children, "高考正是父亲") &&
                      !ExtractedFindContaining(refuse->children, "高考正是父亲") && ke3 &&
                      ExtractedIsRootContaining(roots, "第三章") && !ExtractedFindContaining(ke2->children, "第三章") &&
                      ExtractedFindContaining(ke3->children, "一天学习");
        if (nestOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("gaokao-mijue", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 164, 40));
        lines.Append(TestScanLineXYP("序章 对高考的错误认识", 1, 46, 80));
        lines.Append(TestScanLineXYP("——家长的旧观念拖孩子后腿", 1, 80, 98));
        lines.Append(TestScanLineXYP("第一章 对考试的错误认识", 1, 46, 140));
        lines.Append(TestScanLineXYP("——家长的无知害了孩子", 1, 80, 158));
        lines.Append(TestScanLineXYP("3. 数学、英语成绩好的孩子就一定聪明吗？", 1, 58, 180));
        lines.Append(TestScanLineXYP("不是！ .................................................... (19)", 1, 74, 198));
        lines.Append(TestScanLineXYP("4. 下一节 ............................................... (21)", 1, 58, 220));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* xu = ExtractedFindContaining(roots, "序章");
        ExtractedTocItem* ch = ExtractedFindContaining(roots, "第一章");
        ExtractedTocItem* q = ExtractedFindContaining(roots, "一定聪明");
        bool ok = ran && xu && ch && q && str::Find(xu->title, "家长的旧观念") && str::Find(ch->title, "家长的无知") &&
                  str::Find(q->title, "不是") && q->printedPage == 19 &&
                  !ExtractedFindContaining(xu->children, "家长的旧观念") &&
                  !ExtractedFindContaining(ch->children, "家长的无知") && !ExtractedIsRootContaining(roots, "不是！");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("gaokao-dash-wrap", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 164, 40));
        lines.Append(TestScanLineXYP("第二章 被忽视了的高考父亲学", 1, 46, 80));
        lines.Append(TestScanLineXYP("3．孩子讨厌父母的原因 ................................ (19)", 1, 58, 100));
        lines.Append(TestScanLineXYP("4．电视中的“知心老师”不能说是最好的老师。⋯⋯⋯(21)", 1, 58, 118));
        lines.Append(TestScanLineXYP("5．家长可以教给孩子什么? .............................. (23)", 1, 58, 136));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* four = ExtractedFindContaining(roots, "知心老师");
        ExtractedTocItem* five = ExtractedFindContaining(roots, "家长可以教给孩子");
        bool ok = ran && four && five && four->printedPage == 21 && !str::Find(four->title, "。");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("gaokao-sentence-dot", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("Cover", 1, 80, 40));
        lines.Append(TestScanLineXYP("Table of Contents", 3, 160, 40));
        lines.Append(TestScanLineXYP("The Summer Olympic Games . . . . . . 4", 3, 48, 80));
        lines.Append(TestScanLineXYP("The Olympic Torch . . . . . . . . . . 5", 3, 48, 98));
        lines.Append(TestScanLineXYP("Events . . . . . . . . . . . . . . . 7", 3, 48, 116));
        lines.Append(TestScanLineXYP("Water Sports . . . . . . . . . . . . 8", 3, 48, 134));
        lines.Append(TestScanLineXYP("Track and Field . . . . . . . . . . 11", 3, 48, 152));
        lines.Append(TestScanLineXYP("Gymnastics . . . . . . . . . . . . 15", 3, 48, 170));
        lines.Append(TestScanLineXYP("Glossary . . . . . . . . . . . . . 20", 3, 48, 188));
        lines.Append(TestScanLineXYP("Index . . . . . . . . . . . . . . . 20", 3, 48, 206));
        bool cls = ClassifyExtractTocDoc(lines, 21, "Summer Olympics Events.pdf") == ExtractTocDocClass::Book;
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 21, roots);
        ExtractedTocItem* games = ExtractedFindContaining(roots, "Summer Olympic");
        ExtractedTocItem* torch = ExtractedFindContaining(roots, "Olympic Torch");
        ExtractedTocItem* water = ExtractedFindContaining(roots, "Water Sports");
        ExtractedTocItem* contents = ExtractedFindContaining(roots, "Contents");
        bool ok = cls && ran && contents && games && games->printedPage == 4 && torch && torch->printedPage == 5 &&
                  water && water->printedPage == 8;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("english-toc-inline", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("Table of Contents", 3, 160, 40));
        lines.Append(TestScanLineXYP("The Summer Olympic Games", 3, 48, 80));
        lines.Append(TestScanLineXYP("4", 3, 300, 80));
        lines.Append(TestScanLineXYP("The Olympic Torch", 3, 48, 98));
        lines.Append(TestScanLineXYP("5", 3, 300, 98));
        lines.Append(TestScanLineXYP("Events", 3, 48, 116));
        lines.Append(TestScanLineXYP("7", 3, 300, 116));
        lines.Append(TestScanLineXYP("Water Sports", 3, 48, 134));
        lines.Append(TestScanLineXYP("8", 3, 300, 134));
        lines.Append(TestScanLineXYP("Glossary", 3, 48, 152));
        lines.Append(TestScanLineXYP("20", 3, 300, 152));
        bool cls = ClassifyExtractTocDoc(lines, 21, "Summer Olympics Events.pdf") == ExtractTocDocClass::Book;
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 21, roots);
        ExtractedTocItem* games = ExtractedFindContaining(roots, "Summer Olympic");
        ExtractedTocItem* events = ExtractedFindContaining(roots, "Events");
        bool ok = cls && ran && games && games->printedPage == 4 && events && events->printedPage == 7;
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("english-toc-split", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLine("前言", 20));
        lines.Append(TestScanLine("目录", 40));
        lines.Append(TestScanLine("心得一 让独特的学习方法成为孩子成功的捷径 /3", 60));
        bool bookXinDe = ClassifyExtractTocDoc(lines, 1, "家教心得.pdf") == ExtractTocDocClass::Book;
        FreeScanLines(lines);
        bool partOk = LineLooksLikeBookPartTitle("成女儿的状元路") && LineLooksLikeBookPartTitle("圆张琼的北大梦") &&
                      LineLooksLikeBookPartTitle("助一个进取者一臂之力") &&
                      !LineLooksLikeBookPartTitle("她是一个普通的女孩却走出了不普通的路");
        bool haveXin = StartsWithXinDeHeading("心得一 让独特的学习方法成为孩子成功的捷径");
        Vec<PrintedHit> hits;
        hits.Append(MakeBookHit("成女儿的状元路", 0, 120, 10, 16));
        hits.Append(MakeBookHit("心得一 让独特的学习方法成为孩子成功的捷径", 3, 60, 30, 12));
        hits.Append(MakeBookHit("一、与孩子一起制订学习计划", 8, 80, 50, 11));
        hits.Append(MakeBookHit("二、总结是为了提高得更快", 12, 80, 70, 11));
        hits.Append(MakeBookHit("心得二 良好的素质是孩子成功的保证", 30, 60, 90, 12));
        hits.Append(MakeBookHit("圆张琼的北大梦", 0, 120, 110, 16));
        Vec<ExtractedTocItem*> roots;
        BookHitsToTree(hits, roots);
        bool nestOk = bookXinDe && partOk && haveXin && BookPrintedNestOk(roots);
        if (nestOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("xinde-hits-nest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeBookHit("成女儿的状元路", 0, 60, 10, 16));
        hits.Append(MakeBookHit("心得一 让独特的学习方法成为孩子成功的捷径", 3, 60, 30, 12));
        hits.Append(MakeBookHit("一、与孩子一起制订学习计划", 8, 60, 50, 12));
        hits.Append(MakeBookHit("二、总结是为了提高得更快", 12, 60, 70, 12));
        hits.Append(MakeBookHit("心得二 良好的素质是孩子成功的保证", 30, 60, 90, 12));
        hits.Append(MakeBookHit("圆张琼的北大梦", 0, 60, 110, 16));
        Vec<ExtractedTocItem*> roots;
        BookHitsToTree(hits, roots);
        if (BookPrintedNestOk(roots)) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("xinde-hits-flatx", roots);
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("日录", 1, 206, 96));
        lines.Append(TestScanLineXYP("成女儿的状元路", 1, 181, 219));
        lines.Append(TestScanLineXYP("以前听人们说高考进入前十名，凭的是实力，而要考状元", 1, 97, 249));
        lines.Append(TestScanLineXYP("心得－让独特的学习方法成为孩子成功的捷径/3", 1, 85, 314));
        ScanLine yi = TestScanLineXYP("一", 1, 118, 335);
        yi.dx = 13;
        ScanLine yiRest = TestScanLineXYP("、与孩子一起制订学习计划/3", 1, 120, 335);
        yiRest.dx = 162;
        lines.Append(yi);
        lines.Append(yiRest);
        lines.Append(TestScanLineXYP("二、总结是为了提高得更快/8", 1, 107, 354));
        lines.Append(TestScanLineXYP("三、假期——不要忽视的时间/10", 1, 106, 371));
        lines.Append(TestScanLineXYP("心得二良好的素质是孩子成功的保证/30", 1, 84, 424));
        lines.Append(TestScanLineXYP("圆张琼的北大梦", 1, 179, 514));
        lines.Append(TestScanLineXYP("一、高考准备，应当从高二做起/43", 1, 105, 560));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 1, roots);
        ExtractedTocItem* part = ExtractedFindContaining(roots, "成女儿的状元路");
        ExtractedTocItem* xd1 = ExtractedFindContaining(roots, "心得");
        ExtractedTocItem* yiItem = ExtractedFindContaining(roots, "与孩子一起制订");
        bool xindePageOk = ran && ClassifyExtractTocDoc(lines, 1, "家教心得.pdf") == ExtractTocDocClass::Book &&
                           ExtractedHasPrintedTocBookmark(roots, 1) && part && part->level == 1 && xd1 &&
                           ExtractedHasPrefix(part->children, "心得") && yiItem && str::Find(yiItem->title, "一") &&
                           !str::Find(yiItem->title, "/") && ExtractedIsRootContaining(roots, "圆张琼的北大梦") &&
                           !ExtractedFindContaining(roots, "以前听人们说") &&
                           !ExtractedIsRootContaining(roots, "一、与孩子") && StartsWithXinDeHeading("心得－让独特");
        if (xindePageOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("jiajiao-xinde", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeBookHit("前言", 1, 40, 10, 12));
        hits.Append(MakeBookHit("第1章 智力加油站", 3, 40, 30, 12));
        hits.Append(MakeBookHit("1.1 有记性的数", 5, 56, 50, 12));
        hits.Append(MakeBookHit("第2章 推陈出新", 20, 40, 70, 12));
        hits.Append(MakeBookHit("2.1 无视直觉", 22, 56, 90, 12));
        Vec<ExtractedTocItem*> roots;
        BookHitsToTree(hits, roots);
        ExtractedTocItem* ch1 = ExtractedFindContaining(roots, "第1章");
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "第2章");
        bool chapOk = ExtractedIsRootContaining(roots, "前言") && ExtractedIsRootContaining(roots, "第1章") &&
                      ExtractedIsRootContaining(roots, "第2章") && ch1 && ExtractedHasPrefix(ch1->children, "1.1") &&
                      ch2 && ExtractedHasPrefix(ch2->children, "2.1") && !ExtractedIsRootContaining(roots, "1.1");
        if (chapOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("chap-1.1-nest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeBookHit("第二章 考生喜欢的和不喜欢的父亲", 41, 59, 10, 14));
        hits.Append(MakeBookHit("1．善解人意的家长反而教育不了孩子", 46, 71, 30, 12));
        hits.Append(MakeBookHit("2．不了解孩子的家长不会成功", 47, 58, 50, 12));
        hits.Append(MakeBookHit("6．什么时候拒绝孩子、什么时候同意孩子？", 55, 71, 70, 12));
        hits.Append(MakeBookHit("7.高考正是父亲发挥作用的时候", 58, 58, 90, 12));
        hits.Append(MakeBookHit("第三章值得注意的高考新常识", 0, 49, 120, 14));
        hits.Append(MakeBookHit("1．一天学习多少小时为宜？", 74, 64, 140, 12));
        Vec<ExtractedTocItem*> roots;
        BookHitsToTree(hits, roots);
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "第二章");
        ExtractedTocItem* ch3 = ExtractedFindContaining(roots, "第三章");
        ExtractedTocItem* one = ExtractedFindContaining(roots, "善解人意");
        ExtractedTocItem* six = ExtractedFindContaining(roots, "什么时候拒绝");
        bool listSib = ExtractedIsRootContaining(roots, "第二章") && ExtractedIsRootContaining(roots, "第三章") &&
                       ch2 && one && six && ExtractedFindContaining(ch2->children, "善解人意") &&
                       ExtractedFindContaining(ch2->children, "不了解孩子") &&
                       ExtractedFindContaining(ch2->children, "什么时候拒绝") &&
                       ExtractedFindContaining(ch2->children, "高考正是父亲") &&
                       !ExtractedFindContaining(one->children, "什么时候拒绝") &&
                       !ExtractedFindContaining(six->children, "高考正是父亲") && ch3 &&
                       !ExtractedFindContaining(ch2->children, "第三章") &&
                       ExtractedFindContaining(ch3->children, "一天学习");
        if (listSib) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("list-x-jitter-nest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeBookHit("第三章值得注意的高考新常识", 0, 49, 85, 14));
        hits.Append(MakeBookHit("15．家务劳动对高考学习有无妨碍?", 107, 63, 200, 12));
        hits.Append(MakeBookHit("1．“文武双全”对高考的作用", 110, 64, 250, 12));
        hits.Append(MakeBookHit("第五章反映在作文中的高中生心理活动", 0, 61, 400, 15));
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("第三章值得注意的高考新常识", 1, 49, 85));
        lines.Append(TestScanLineXYP("第四章有关高考的重要知识", 1, 51, 220));
        lines.Append(TestScanLineXYP("第五章反映在作文中的高中生心理活动", 1, 61, 400));
        RecoverMissingBookSpines(hits, lines, 1, 2);
        Vec<ExtractedTocItem*> roots;
        BookHitsToTree(hits, roots);
        ExtractedTocItem* ch3 = ExtractedFindContaining(roots, "第三章");
        ExtractedTocItem* ch4 = ExtractedFindContaining(roots, "第四章");
        ExtractedTocItem* ch5 = ExtractedFindContaining(roots, "第五章");
        bool skipOk = ExtractedIsRootContaining(roots, "第三章") && ExtractedIsRootContaining(roots, "第四章") &&
                      ExtractedIsRootContaining(roots, "第五章") && ch3 && ch4 && ch5 && ch5->level == 1 &&
                      !ExtractedFindContaining(ch3->children, "第五章") &&
                      ExtractedFindContaining(ch4->children, "文武双全");
        if (skipOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("recover-ch4", roots);
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
        FreeScanLines(lines);
    }
    {
        Vec<PrintedHit> hits;
        hits.Append(MakeBookHit("第一讲 开场", 1, 40, 10, 12));
        hits.Append(MakeBookHit("一、准备", 3, 40, 30, 12));
        hits.Append(MakeBookHit("二、练习", 8, 40, 50, 12));
        hits.Append(MakeBookHit("第二讲 深入", 20, 40, 70, 12));
        hits.Append(MakeBookHit("一、复习", 22, 40, 90, 12));
        Vec<ExtractedTocItem*> roots;
        BookHitsToTree(hits, roots);
        ExtractedTocItem* t1 = ExtractedFindContaining(roots, "第一讲");
        ExtractedTocItem* t2 = ExtractedFindContaining(roots, "第二讲");
        bool talkOk = ExtractedIsRootContaining(roots, "第一讲") && ExtractedIsRootContaining(roots, "第二讲") && t1 &&
                      ExtractedHasPrefix(t1->children, "一、准备") && ExtractedHasPrefix(t1->children, "二、练习") &&
                      t2 && ExtractedHasPrefix(t2->children, "一、复习") &&
                      !ExtractedIsRootContaining(roots, "一、准备");
        if (talkOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("talk-nest", roots);
        }
        DeleteExtractedTocItems(roots);
        FreePrintedHits(hits);
    }
    {
        Vec<ExtractedTocItem*> flat;
        flat.Append(NewItem("2 项目现状及需求分析", 8, 40, 10, 1));
        flat.Append(NewItem("2.1 业务现状", 8, 40, 30, 2));
        Vec<ExtractedTocItem*> roots;
        BuildTreeFromFlat(flat, roots);
        Vec<ScanLine> lines;
        ScanLine h1 = TestScanLine("2 项目现状及需求分析", 10);
        h1.srcPage = 8;
        ScanLine h2 = TestScanLine("2.1 业务现状", 30);
        h2.srcPage = 8;
        ScanLine h3 = TestScanLine("2.1.1 审计业务现状", 50);
        h3.srcPage = 8;
        ScanLine h4 = TestScanLine("2.1.1.1 信息反馈能力弱，与发展不相适应", 70);
        h4.srcPage = 8;
        lines.Append(h1);
        lines.Append(h2);
        lines.Append(h3);
        lines.Append(h4);
        MergeDeeperArabicHeadings(lines, 8, roots);
        ExtractedTocItem* sec211 = ExtractedFindContaining(roots, "2.1.1 审计业务现状");
        bool deepOk = ExtractedIsRootContaining(roots, "2 项目现状及需求分析") &&
                      ExtractedHasPrefix(roots, "2.1.1 审计业务现状") && ExtractedHasPrefix(roots, "2.1.1.1") &&
                      sec211 && sec211->level == 3 && sec211->children.Size() >= 1;
        if (deepOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("merge-deeper-arabic", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        lines.Append(TestScanLineXYP("第一章 人工智能的诞生／1", 1, 48, 80));
        lines.Append(TestScanLineXYP("第二章 神经网络／10", 1, 48, 100));
        lines.Append(TestScanLineXYP("第三章 大模型时代／20", 1, 48, 120));
        lines.Append(TestScanLineXYP("第五章 展望／40", 1, 48, 140));
        ScanLine b1 = TestScanLineXYP("第一章 人工智能的诞生", 3, 72, 90);
        b1.fontSize = 22;
        b1.bold = true;
        b1.dy = 24;
        lines.Append(b1);
        ScanLine para = TestScanLineXYP("这一章讲述早期研究脉络和几个关键实验。", 3, 72, 140);
        para.fontSize = 12;
        para.dy = 14;
        lines.Append(para);
        ScanLine head = TestScanLineXYP("第一章 人工智能的诞生", 5, 72, 20);
        head.fontSize = 9;
        head.dy = 10;
        lines.Append(head);
        ScanLine b2 = TestScanLineXYP("第二章 神经网络", 4, 72, 88);
        b2.fontSize = 22;
        b2.bold = true;
        b2.dy = 24;
        lines.Append(b2);
        ScanLine b4 = TestScanLineXYP("第四章 对齐与安全", 5, 72, 90);
        b4.fontSize = 22;
        b4.bold = true;
        b4.dy = 24;
        lines.Append(b4);
        ScanLine note = TestScanLineXYP("本章要点如下", 5, 72, 160);
        note.fontSize = 12;
        note.dy = 14;
        lines.Append(note);
        ScanLine b5 = TestScanLineXYP("第五章 展望", 6, 72, 88);
        b5.fontSize = 22;
        b5.bold = true;
        b5.dy = 24;
        lines.Append(b5);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 6, roots);
        ExtractedTocItem* ch1 = ExtractedFindContaining(roots, "第一章");
        ExtractedTocItem* ch3 = ExtractedFindContaining(roots, "第三章");
        ExtractedTocItem* ch4 = ExtractedFindContaining(roots, "第四章");
        ExtractedTocItem* ch5 = ExtractedFindContaining(roots, "第五章");
        bool styleOk = ran && ch1 && ch1->pageNo == 3 && ExtractedIsRootContaining(roots, "第一章") &&
                       ExtractedIsRootContaining(roots, "第五章") && ch3 && ch5 && !ch4 &&
                       !ExtractedFindContaining(roots, "本章要点") && !ExtractedFindContaining(roots, "早期研究");
        if (styleOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-style-learn", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        // 部分 → 章 → 节, even when all titles share the same x.
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        lines.Append(TestScanLineXYP("第一部分 基础／1", 1, 48, 80));
        lines.Append(TestScanLineXYP("第一章 概论／3", 1, 48, 100));
        lines.Append(TestScanLineXYP("第一节 背景／3", 1, 48, 120));
        lines.Append(TestScanLineXYP("第二节 方法／8", 1, 48, 140));
        lines.Append(TestScanLineXYP("第二部分 实践／20", 1, 48, 160));
        lines.Append(TestScanLineXYP("第三章 案例／21", 1, 48, 180));
        lines.Append(TestScanLineXYP("第一节 现场／21", 1, 48, 200));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 3, roots);
        ExtractedTocItem* part1 = ExtractedFindContaining(roots, "第一部分");
        ExtractedTocItem* part2 = ExtractedFindContaining(roots, "第二部分");
        ExtractedTocItem* ch1 = part1 ? ExtractedFindContaining(part1->children, "第一章") : nullptr;
        ExtractedTocItem* ch3 = part2 ? ExtractedFindContaining(part2->children, "第三章") : nullptr;
        bool partOk = ran && ExtractedHasPrintedTocBookmark(roots, 1) && ExtractedIsRootContaining(roots, "第一部分") &&
                      ExtractedIsRootContaining(roots, "第二部分") && part1 && part1->level == 1 && part2 &&
                      part2->level == 1 && ch1 && ch1->level == 2 && ExtractedHasPrefix(ch1->children, "第一节") &&
                      ExtractedHasPrefix(ch1->children, "第二节") && ch3 && ch3->level == 2 &&
                      ExtractedFindContaining(ch3->children, "现场") && !ExtractedIsRootContaining(roots, "第一章") &&
                      !ExtractedIsRootContaining(roots, "第三章") &&
                      !ExtractedFindContaining(part1->children, "第三章");
        if (partOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-part-chapter-section", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        // 无「部分」时：章 → 节（同列也不升成平级）。
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        lines.Append(TestScanLineXYP("第一章 概论／1", 1, 48, 80));
        lines.Append(TestScanLineXYP("第一节 背景／1", 1, 48, 100));
        lines.Append(TestScanLineXYP("第二节 方法／5", 1, 48, 120));
        lines.Append(TestScanLineXYP("第二章 进阶／10", 1, 48, 140));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 3, roots);
        ExtractedTocItem* ch1 = ExtractedFindContaining(roots, "第一章");
        ExtractedTocItem* ch2 = ExtractedFindContaining(roots, "第二章");
        bool chapOk = ran && ExtractedHasPrintedTocBookmark(roots, 1) && ExtractedIsRootContaining(roots, "第一章") &&
                      ExtractedIsRootContaining(roots, "第二章") && ch1 && ch1->level == 1 &&
                      ExtractedHasPrefix(ch1->children, "第一节") && ExtractedHasPrefix(ch1->children, "第二节") &&
                      ch2 && ch2->level == 1 && !ExtractedIsRootContaining(roots, "第一节");
        if (chapOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-chapter-section-samex", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        ScanLine t1 = TestScanLineXYP("第一课 青少年要学学习《法律常识》", 1, 40, 80);
        t1.dx = 190;
        lines.Append(t1);
        ScanLine d1 = TestScanLineXYP("....................", 1, 236, 82);
        d1.dx = 48;
        d1.dy = 6;
        lines.Append(d1);
        ScanLine p1 = TestScanLineXYP("1", 1, 300, 81);
        p1.dx = 8;
        p1.dy = 8;
        lines.Append(p1);
        ScanLine t2 = TestScanLineXYP("第二课 我国的法律", 1, 40, 102);
        t2.dx = 110;
        lines.Append(t2);
        ScanLine d2 = TestScanLineXYP(".....................", 1, 236, 104);
        d2.dx = 48;
        d2.dy = 6;
        lines.Append(d2);
        ScanLine p2 = TestScanLineXYP("8", 1, 300, 103);
        p2.dx = 8;
        p2.dy = 8;
        lines.Append(p2);
        ScanLine t3 = TestScanLineXYP("第三课 社会主义制度是我国的根本制度", 1, 40, 124);
        t3.dx = 200;
        lines.Append(t3);
        ScanLine p3 = TestScanLineXYP("19", 1, 296, 125);
        p3.dx = 12;
        p3.dy = 8;
        lines.Append(p3);
        ScanLine t10 = TestScanLineXYP("第十课 犯罪应受刑罚制裁", 1, 40, 146);
        t10.dx = 150;
        lines.Append(t10);
        ScanLine ocrPage = TestScanLineXYP("1O5", 1, 296, 147);
        ocrPage.dx = 16;
        ocrPage.dy = 8;
        lines.Append(ocrPage);
        ScanLine t12 = TestScanLineXYP("第十二课 增强社会主义法制观念，", 1, 40, 168);
        t12.dx = 180;
        lines.Append(t12);
        ScanLine t12b = TestScanLineXYP("提高守法自觉性", 1, 58, 186);
        t12b.dx = 90;
        lines.Append(t12b);
        ScanLine d12 = TestScanLineXYP("................", 1, 236, 188);
        d12.dx = 40;
        d12.dy = 6;
        lines.Append(d12);
        ScanLine p12 = TestScanLineXYP("124", 1, 292, 187);
        p12.dx = 16;
        p12.dy = 8;
        lines.Append(p12);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 2, roots);
        ExtractedTocItem* ke1 = ExtractedFindContaining(roots, "第一课");
        ExtractedTocItem* ke2 = ExtractedFindContaining(roots, "第二课");
        ExtractedTocItem* ke3 = ExtractedFindContaining(roots, "第三课");
        ExtractedTocItem* ke10 = ExtractedFindContaining(roots, "第十课");
        ExtractedTocItem* ke12 = ExtractedFindContaining(roots, "第十二课");
        bool printed2dOk = ran && ExtractedHasPrintedTocBookmark(roots, 1) && ke1 &&
                           str::Find(ke1->title, "青少年要学学习") && str::Find(ke1->title, "法律常识") &&
                           ke1->level == 1 && ke1->children.Size() == 0 && ke2 && str::Find(ke2->title, "我国的法律") &&
                           ke3 && str::Find(ke3->title, "社会主义制度") && ke12 &&
                           str::Find(ke12->title, "增强社会主义法制观念") && str::Find(ke12->title, "提高守法自觉性") &&
                           ke12->children.Size() == 0 && ke10 && str::Find(ke10->title, "犯罪应受刑罚制裁") &&
                           !str::Find(ke1->title, "....");
        if (printed2dOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-printed-2d", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        ScanLine glued = TestScanLineXYP("第一课青少年要学习《法律常识》第二课我国的法律", 1, 40, 80);
        glued.dx = 220;
        glued.dy = 16;
        lines.Append(glued);
        ScanLine ke3 = TestScanLineXYP("第三课 社会主义制度是我国的根本制度", 1, 40, 100);
        ke3.dx = 200;
        ke3.dy = 28;
        lines.Append(ke3);
        ScanLine ke4 = TestScanLineXYP("第四课 我国公民的基本权利和义务（一）", 1, 40, 116);
        ke4.dx = 210;
        ke4.dy = 28;
        lines.Append(ke4);
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 2, roots);
        ExtractedTocItem* ke1 = ExtractedFindContaining(roots, "第一课");
        ExtractedTocItem* ke2 = ExtractedFindContaining(roots, "第二课");
        ExtractedTocItem* ke3n = ExtractedFindContaining(roots, "第三课");
        ExtractedTocItem* ke4n = ExtractedFindContaining(roots, "第四课");
        bool splitOk = ran && ke1 && ke2 && ke3n && ke4n && str::Find(ke1->title, "法律常识") &&
                       !str::Find(ke1->title, "第二课") && str::Find(ke2->title, "我国的法律") &&
                       !str::Find(ke3n->title, "第四课") && str::Find(ke4n->title, "基本权利");
        if (splitOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-split-glued-ke", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("目录", 1, 160, 40));
        lines.Append(TestScanLineXYP("第一部分：介绍", 1, 140, 70));
        ScanLine ch1 = TestScanLineXYP("第一章 情感智力：培养孩子的新方法", 1, 48, 100);
        ch1.dx = 200;
        ch1.dy = 28;
        lines.Append(ch1);
        ScanLine s1 = TestScanLineXYP("什么是情感智力", 1, 70, 116);
        s1.dx = 90;
        s1.dy = 28;
        lines.Append(s1);
        ScanLine p4 = TestScanLineXYP("(4)", 1, 300, 118);
        p4.dx = 18;
        p4.dy = 10;
        lines.Append(p4);
        lines.Append(TestScanLineXYP("情商与智商......(7)为什么智商高时情商却低", 1, 70, 150));
        lines.Append(TestScanLineXYP("培养孩子的新方法......(10)", 1, 70, 180));
        lines.Append(TestScanLineXYP("发展性地理解情商b", 1, 70, 210));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool ran = ExtractBookToc(nullptr, lines, labels, 2, roots);
        ExtractedTocItem* part = ExtractedFindContaining(roots, "第一部分");
        ExtractedTocItem* ch = ExtractedFindContaining(roots, "第一章");
        ExtractedTocItem* what = ExtractedFindContaining(roots, "什么是情感智力");
        ExtractedTocItem* eq = ExtractedFindContaining(roots, "情商与智商");
        ExtractedTocItem* why = ExtractedFindContaining(roots, "为什么智商高时情商却低");
        ExtractedTocItem* grow = ExtractedFindContaining(roots, "培养孩子的新方法");
        ExtractedTocItem* dev = ExtractedFindContaining(roots, "发展性地理解情商");
        bool eqOk = ran && part && ch && !str::Find(ch->title, "什么是情感智力") && what &&
                    !str::Find(what->title, "第一章") && eq && !str::Find(eq->title, "为什么") && why && grow &&
                    grow->title && !str::Find(grow->title, "(10)") && !str::Find(grow->title, "10") && dev &&
                    dev->title && !str::Find(dev->title, "情商b");
        if (eqOk) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("book-split-eq-sections", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("第三章 健全多层次社会保障体系", 10, 72, 40));
        lines.Append(TestScanLineXYP("第二节", 10, 72, 60));
        lines.Append(TestScanLineXYP("第一节", 10, 72, 80));
        lines.Append(TestScanLineXYP("第一节 完善社会保障制度体系", 12, 72, 40));
        lines.Append(TestScanLineXYP("第四节 加强社会保险基金监管", 25, 72, 80));
        lines.Append(TestScanLineXYP("第五节 提升社会保险经办管理服务", 25, 72, 140));
        lines.Append(TestScanLineXYP("第二章 实施就业优先战略", 6, 72, 40));
        lines.Append(TestScanLineXYP("第一节强化就业优先政策 o", 6, 72, 70));
        Vec<ExtractedTocItem*> roots;
        bool ran = InferHeadings(lines, 25, roots);
        ExtractedTocItem* ch3 = ExtractedFindContaining(roots, "第三章");
        ExtractedTocItem* sec1 = ExtractedFindContaining(roots, "完善社会保障制度体系");
        ExtractedTocItem* emp = ExtractedFindContaining(roots, "强化就业优先政策");
        bool bare = false;
        if (ch3) {
            for (int i = 0; i < ch3->children.Size(); i++) {
                if (TitleIsNumberingOnly(ch3->children[i]->title)) {
                    bare = true;
                }
            }
        }
        bool ok = ran && ch3 && sec1 && !bare && emp && emp->title && !str::Find(emp->title, " o") &&
                  !str::EndsWith(emp->title, "o") && ExtractedFindContaining(roots, "加强社会保险基金监管");
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-bare-jie-ocr-o", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        Vec<ScanLine> lines;
        lines.Append(TestScanLineXYP("赣人社发〔2026〕7号", 1, 80, 20));
        lines.Append(TestScanLineXYP("关于印发《江西省人力资源和社会保障数据安全管理办法》的通知", 1, 72, 40));
        lines.Append(TestScanLineXYP("日录", 1, 160, 70));
        lines.Append(TestScanLineXYP("第一章 总则", 2, 72, 40));
        lines.Append(TestScanLineXYP("第一条 目的", 2, 72, 60));
        lines.Append(TestScanLineXYP("（二）涉及敏感个人信息", 2, 72, 80));
        lines.Append(TestScanLineXYP("第二章 数据分类分级", 3, 72, 40));
        lines.Append(TestScanLineXYP("第四章 数据全生命周期安全管理", 4, 72, 40));
        Vec<ExtractedTocItem*> roots;
        Vec<char*> labels;
        bool cls = ClassifyExtractTocDoc(lines, 4, "管理办法.pdf") == ExtractTocDocClass::Official;
        bool ran = ExtractOfficialToc(nullptr, lines, labels, 4, roots, nullptr);
        bool ok = cls && ran && ExtractedFindContaining(roots, "第一章") && ExtractedFindContaining(roots, "第一条") &&
                  ExtractedFindContaining(roots, "第二章") && ExtractedFindContaining(roots, "第四章") &&
                  !ExtractedHasPrintedTocBookmark(roots, 0);
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            LogBookExtractFail("official-banfa-not-book-toc", roots);
        }
        DeleteExtractedTocItems(roots);
        FreeScanLines(lines);
    }
    {
        bool ok = TocCalibTestDeletePromotesChildren();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-delete-promote\n");
        }
    }
    {
        bool ok = TocCalibTestPromoteDemote();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-promote-demote\n");
        }
    }
    {
        bool ok = TocCalibTestDropMoveAndNest();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-drop-move\n");
        }
    }
    {
        bool ok = TocCalibTestBm25Locate();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-bm25-locate\n");
        }
    }
    {
        bool ok = TocCalibTestFindQuery();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-find-query\n");
        }
    }
    {
        bool ok = TocCalibTestInterpolatePrinted();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-interpolate-printed\n");
        }
    }
    {
        bool ok = TocCalibTestPinOverwritesPrinted();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-pin-overwrite-printed\n");
        }
    }
    {
        bool ok = TocCalibTestFrontMatterUsesLabel();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-front-matter-label\n");
        }
    }
    {
        bool ok = TocCalibTestClearTocDests();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-clear-toc-dest\n");
        }
    }
    {
        bool ok = TocCalibTestAddChildManual();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-add-child\n");
        }
    }
    {
        bool ok = TocCalibTestMergeWithNext();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-merge-next\n");
        }
    }
    {
        bool ok = TocCalibTestUndo();
        if (ok) {
            (*pass)++;
        } else {
            (*fail)++;
            logf("phase2 fail calib-undo\n");
        }
    }
}

static bool OfficialHanComplete(const char* s) {
    return s && (str::EndsWith(s, "通知") || str::EndsWith(s, "的函") || str::EndsWith(s, "请示") ||
                 str::EndsWith(s, "通报") || str::EndsWith(s, "批复") || str::EndsWith(s, "意见的函") ||
                 str::EndsWith(s, "情况说明"));
}

static bool OfficialTitleRunComplete(const char* s) {
    return OfficialHanComplete(s) || str::EndsWith(s, "情况说明");
}

static bool LooksLikeOfficialTitle(const char* s) {
    if (!s || LooksLikeOfficialBoilerplate(s) || LooksLikeDocNumberLine(s) || LooksLikeArchiveJunk(s) ||
        LooksLikeLeaderTitle(s)) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 6 || g > 80) {
        return false;
    }
    if (str::Find(s, "关于印发") || str::Find(s, "印发《")) {
        return true;
    }
    if (str::StartsWith(s, "关于") && OfficialHanComplete(s)) {
        return true;
    }
    return OfficialHanComplete(s) && g >= 10;
}

static char* JoinOfficialTitleRun(const Vec<ScanLine>& lines, int start, int* usedExtra) {
    *usedExtra = 0;
    if (start < 0 || start >= lines.Size() || !lines[start].text) {
        return nullptr;
    }
    char* s = str::Dup(lines[start].text);
    int page = lines[start].srcPage;
    for (int k = 1; k <= 3 && start + k < lines.Size(); k++) {
        if (OfficialTitleRunComplete(s)) {
            break;
        }
        const ScanLine& b = lines[start + k];
        if (!b.text || b.srcPage != page) {
            break;
        }
        int ga = GlyphCount(s);
        int gb = GlyphCount(b.text);
        if (ga + gb > 80 || gb < 1 || gb > 40) {
            break;
        }
        if (HeadingLevelFromText(b.text) > 0 || LooksLikeOfficialBoilerplate(b.text) ||
            LooksLikeDocNumberLine(b.text)) {
            break;
        }
        float gap = b.y - (lines[start + k - 1].y + lines[start + k - 1].dy);
        if (gap > 22) {
            break;
        }
        char* joined = str::Join(s, b.text);
        str::Free(s);
        s = joined;
        *usedExtra = k;
        if (OfficialTitleRunComplete(s)) {
            break;
        }
    }
    return s;
}

static bool OfficialTitleStartsLikeFragment(const char* s) {
    if (!s || !s[0]) {
        return true;
    }
    return str::StartsWith(s, "从") || str::StartsWith(s, "于") || str::StartsWith(s, "在") ||
           str::StartsWith(s, "对") || str::StartsWith(s, "将") || str::StartsWith(s, "期间") ||
           str::StartsWith(s, "以及") || str::StartsWith(s, "同时") || str::StartsWith(s, "按照") ||
           str::StartsWith(s, "通过") || str::StartsWith(s, "结合") || str::StartsWith(s, "围绕") ||
           str::StartsWith(s, "突出") || str::StartsWith(s, "并且") || str::StartsWith(s, "资源和社会");
}

static bool OfficialTitleLooksLikeSentence(const char* s) {
    if (!s || !s[0]) {
        return true;
    }
    if (str::Find(s, "。") || str::Find(s, "；") || str::Find(s, "需要") || str::Find(s, "应当") ||
        str::Find(s, "必须") || str::Find(s, "的")) {
        return true;
    }
    int g = GlyphCount(s);
    if (g >= 14 && str::Find(s, "是")) {
        return true;
    }
    int len = (int)str::Len(s);
    int i = len;
    int last = i > 0 ? Utf8CodepointPrev(s, len, i) : 0;
    while (last > 0 && (last <= 32 || last == 0x3000 || last == ')' || last == 0xFF09)) {
        last = i > 0 ? Utf8CodepointPrev(s, len, i) : 0;
    }
    // 从2017年10月起 / 突出政府……中的
    return last == 0x7684 || last == 0x4E86 || last == 0x8D77 || last == 0x4E2D;
}

static bool OfficialTitleHasDocSuffix(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    char* t = DupTrimmed(s);
    if (!t) {
        return false;
    }
    int len = (int)str::Len(t);
    int i = len;
    while (i > 0) {
        int prev = i;
        int cp = Utf8CodepointPrev(t, len, prev);
        if (cp == ')' || cp == 0xFF09 || cp == 0x300B) {
            t[prev] = 0;
            len = prev;
            i = prev;
            continue;
        }
        break;
    }
    str::TrimWSInPlace(t, str::TrimOpt::Both);
    while (t[0] == '"' || t[0] == '\'' || t[0] == 0x201C || t[0] == 0x300A) {
        memmove(t, t + 1, str::Len(t));
        str::TrimWSInPlace(t, str::TrimOpt::Both);
    }
    int tl = (int)str::Len(t);
    while (tl > 0) {
        int pi = tl;
        int cp = Utf8CodepointPrev(t, tl, pi);
        if (cp == '"' || cp == '\'' || cp == 0x201D || cp == 0x300B) {
            t[pi] = 0;
            tl = pi;
            str::TrimWSInPlace(t, str::TrimOpt::Both);
            tl = (int)str::Len(t);
            continue;
        }
        break;
    }
    bool ok = str::EndsWith(t, "编制说明") || str::EndsWith(t, "征求意见稿") || str::EndsWith(t, "办法") ||
              str::EndsWith(t, "方案") || str::EndsWith(t, "细则") || str::EndsWith(t, "规定") ||
              str::EndsWith(t, "报告") || str::EndsWith(t, "纪要") || str::EndsWith(t, "指南") ||
              str::EndsWith(t, "规划") || str::EndsWith(t, "计划") || str::EndsWith(t, "要点") ||
              str::EndsWith(t, "公告") || str::EndsWith(t, "通告") || str::EndsWith(t, "决定") ||
              str::EndsWith(t, "清单") || str::EndsWith(t, "标准体系") || str::EndsWith(t, "工作说明") ||
              str::EndsWith(t, "填报说明") || str::EndsWith(t, "自查说明") || str::EndsWith(t, "情况说明") ||
              (str::EndsWith(t, "表") && GlyphCount(t) >= 4);
    str::Free(t);
    return ok;
}

static bool LooksLikeOfficialAppendixName(const char* s) {
    if (!s || !s[0] || LooksLikeOfficialBoilerplate(s) || LooksLikeLeaderTitle(s) || LooksLikeArchiveJunk(s)) {
        return false;
    }
    const char* name = s;
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::Appendix && m.prefixLength > 0) {
        name = s + m.prefixLength;
        int len = (int)str::Len(name);
        int i = 0;
        SkipSpacesUtf8(name, len, i);
        int cp = i < len ? Utf8CodepointNext(name, len, i) : 0;
        if (cp == ':' || cp == 0xFF1A) {
            SkipSpacesUtf8(name, len, i);
            name += i;
        } else if (i > 0) {
            name += i;
        }
    } else if (HeadingLevelFromText(s) > 0) {
        return false;
    }
    int g = GlyphCount(name);
    if (g < 2 || g > 16) {
        return false;
    }
    if (OfficialTitleStartsLikeFragment(name) || OfficialTitleLooksLikeSentence(name)) {
        return false;
    }
    return str::EndsWith(name, "提纲") || str::EndsWith(name, "名单") || str::EndsWith(name, "名册") ||
           str::EndsWith(name, "一览表") || str::EndsWith(name, "统计表") || str::EndsWith(name, "明细表") ||
           str::EndsWith(name, "安排表") || str::EndsWith(name, "日程");
}

static bool LooksLikeOfficialFileTitle(const char* s) {
    if (LooksLikeOfficialTitle(s) || LooksLikeOfficialAppendixName(s)) {
        return true;
    }
    if (!s || LooksLikeOfficialBoilerplate(s) || LooksLikeDocNumberLine(s) || LooksLikeArchiveJunk(s) ||
        LooksLikeLeaderTitle(s) || HeadingLevelFromText(s) > 0) {
        return false;
    }
    int g = GlyphCount(s);
    if (g < 6 || g > 60) {
        return false;
    }
    if (OfficialTitleStartsLikeFragment(s) || OfficialTitleLooksLikeSentence(s)) {
        return false;
    }
    return OfficialTitleHasDocSuffix(s);
}

static bool IsOfficialYiDunhaoItem(const char* s) {
    HeadingMarker m = ParseHeadingMarker(s);
    return m.type == MarkerType::ChineseDunhao && m.number == 1;
}

static bool IsOfficialExtractJunkTitle(const char* s) {
    if (!s || !s[0]) {
        return true;
    }
    if (HeadingLevelFromText(s) > 0) {
        return false;
    }
    if (LooksLikeOfficialTitle(s) || LooksLikeOfficialFileTitle(s)) {
        return false;
    }
    return true;
}

static char* JoinLargeOfficialTitleRun(const Vec<ScanLine>& lines, int start, float minFont, int* usedExtra) {
    *usedExtra = 0;
    if (start < 0 || start >= lines.Size() || !lines[start].text) {
        return nullptr;
    }
    char* s = str::Dup(lines[start].text);
    int page = lines[start].srcPage;
    for (int k = 1; k <= 2 && start + k < lines.Size(); k++) {
        const ScanLine& b = lines[start + k];
        if (!b.text || b.srcPage != page) {
            break;
        }
        bool needHan = str::StartsWith(s, "关于") && !OfficialHanComplete(s);
        float joinMin = needHan ? minFont * 0.8f : minFont;
        if (b.fontSize + 0.2f < joinMin) {
            break;
        }
        if (HeadingLevelFromText(b.text) > 0 || LooksLikeOfficialBoilerplate(b.text) ||
            LooksLikeDocNumberLine(b.text)) {
            break;
        }
        float prevBottom = lines[start + k - 1].y + (lines[start + k - 1].dy > 1 ? lines[start + k - 1].dy : 14);
        if (b.y - prevBottom > 22) {
            break;
        }
        int gb = GlyphCount(b.text);
        if (gb < 2 || gb > 28 || GlyphCount(s) + gb > 60) {
            break;
        }
        char* joined = str::Join(s, b.text);
        str::Free(s);
        s = joined;
        *usedExtra = k;
    }
    return s;
}

static bool OfficialFlatQuotedAttachmentName(const char* flatTitle, const char* letterTitle) {
    if (!flatTitle || !letterTitle || !str::StartsWith(letterTitle, "关于")) {
        return false;
    }
    if (str::StartsWith(flatTitle, "关于")) {
        return false;
    }
    if (ParseHeadingMarker(flatTitle).type == MarkerType::Appendix) {
        return false;
    }
    if (!LooksLikeOfficialFileTitle(flatTitle)) {
        return false;
    }
    return str::Find(letterTitle, flatTitle) != nullptr;
}

static bool OfficialFlatTitleOverlapsTitle(const char* flatTitle, const char* title) {
    if (!flatTitle || !title) {
        return false;
    }
    if (str::Eq(flatTitle, title)) {
        return true;
    }
    // Covering letters quote attachment names inside 《…》; do not treat the glued 附件：1.行 as already having that
    // title.
    if (ParseHeadingMarker(flatTitle).type == MarkerType::Appendix) {
        return false;
    }
    if (OfficialFlatQuotedAttachmentName(flatTitle, title)) {
        return false;
    }
    return str::Find(flatTitle, title) || str::Find(title, flatTitle);
}

static bool OfficialTitleTextTaken(const Vec<ExtractedTocItem*>& flat, const char* title) {
    if (!title) {
        return true;
    }
    for (int i = 0; i < flat.Size(); i++) {
        if (flat[i] && flat[i]->title && OfficialFlatTitleOverlapsTitle(flat[i]->title, title)) {
            return true;
        }
    }
    return false;
}

static void FlattenExtractedDetach(Vec<ExtractedTocItem*>& nodes, Vec<ExtractedTocItem*>& flat) {
    for (int i = 0; i < nodes.Size(); i++) {
        ExtractedTocItem* n = nodes[i];
        if (!n) {
            continue;
        }
        flat.Append(n);
        FlattenExtractedDetach(n->children, flat);
        n->children.Reset();
    }
    nodes.Reset();
}

static float OfficialBodyFontSize(const Vec<ScanLine>& lines) {
    Vec<float> sizes;
    for (int i = 0; i < lines.Size(); i++) {
        int g = GlyphCount(lines[i].text);
        if (g >= 16 && lines[i].fontSize > 1) {
            sizes.Append(lines[i].fontSize);
        }
    }
    float mid = MedianPositive(sizes);
    if (mid < 4) {
        return 10;
    }
    Vec<float> body;
    for (int i = 0; i < sizes.Size(); i++) {
        if (sizes[i] <= mid * 1.2f) {
            body.Append(sizes[i]);
        }
    }
    float out = MedianPositive(body);
    return out >= 4 ? out : mid;
}

static float OfficialYiHeadingFont(const Vec<ScanLine>& lines, int nPages) {
    Vec<float> sizes;
    for (int i = 0; i < lines.Size(); i++) {
        if (!lines[i].text || lines[i].srcPage < 1 || lines[i].srcPage > nPages || lines[i].fontSize < 4) {
            continue;
        }
        HeadingMarker m = ParseHeadingMarker(lines[i].text);
        if (m.type == MarkerType::ChineseDunhao && m.number >= 1) {
            sizes.Append(lines[i].fontSize);
        }
    }
    float mid = MedianPositive(sizes);
    return mid >= 4 ? mid : 0;
}

static void TightenOfficialAppendixTitle(ExtractedTocItem* it) {
    if (!it || !it->title) {
        return;
    }
    HeadingMarker m = ParseHeadingMarker(it->title);
    if (m.type != MarkerType::Appendix) {
        return;
    }
    const char* suffixes[] = {"提纲", "名单", "名册", "一览表", "统计表", "明细表", "安排表", "日程"};
    for (int i = 0; i < (int)dimof(suffixes); i++) {
        const char* hit = str::Find(it->title, suffixes[i]);
        if (!hit) {
            continue;
        }
        int end = (int)(hit - it->title) + (int)str::Len(suffixes[i]);
        if (it->title[end] == 0) {
            return;
        }
        it->title[end] = 0;
        str::TrimWSInPlace(it->title, str::TrimOpt::Both);
        return;
    }
}

struct OfficialTitleInsert {
    ExtractedTocItem* item = nullptr;
    int beforeFlat = 0;
    bool underAppendix = false;
};

static bool OfficialTitleInsertDuplicate(const Vec<OfficialTitleInsert>& inserts, const char* title) {
    if (!title) {
        return true;
    }
    for (int t = 0; t < inserts.Size(); t++) {
        if (inserts[t].item && inserts[t].item->title &&
            OfficialFlatTitleOverlapsTitle(inserts[t].item->title, title)) {
            return true;
        }
    }
    return false;
}

static char* OfficialAppendixGluedDocTitle(const char* s) {
    if (!s) {
        return nullptr;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type != MarkerType::Appendix || m.prefixLength <= 0) {
        return nullptr;
    }
    const char* name = s + m.prefixLength;
    int len = (int)str::Len(name);
    int i = 0;
    SkipSpacesUtf8(name, len, i);
    int cp = i < len ? Utf8CodepointNext(name, len, i) : 0;
    if (cp == ':' || cp == 0xFF1A) {
        SkipSpacesUtf8(name, len, i);
        name += i;
    } else if (i > 0) {
        name += i;
    }
    if (!LooksLikeOfficialFileTitle(name) || GlyphCount(name) < 6) {
        return nullptr;
    }
    return str::Dup(name);
}

static bool OfficialAppendixHasInlineName(const char* s) {
    if (!s) {
        return false;
    }
    if (LooksLikeOfficialAppendixName(s)) {
        return true;
    }
    char* glued = OfficialAppendixGluedDocTitle(s);
    if (glued) {
        str::Free(glued);
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type != MarkerType::Appendix || m.prefixLength <= 0) {
        return false;
    }
    const char* name = s + m.prefixLength;
    int len = (int)str::Len(name);
    int i = 0;
    SkipSpacesUtf8(name, len, i);
    int cp = i < len ? Utf8CodepointNext(name, len, i) : 0;
    if (cp == ':' || cp == 0xFF1A) {
        SkipSpacesUtf8(name, len, i);
        name += i;
    } else if (i > 0) {
        name += i;
    }
    int g = GlyphCount(name);
    if (g < 2 || g > 16) {
        return false;
    }
    return !OfficialTitleStartsLikeFragment(name) && !OfficialTitleLooksLikeSentence(name);
}

static void TrimOfficialAppendixToLabel(ExtractedTocItem* ap) {
    if (!ap || !ap->title) {
        return;
    }
    HeadingMarker m = ParseHeadingMarker(ap->title);
    if (m.type != MarkerType::Appendix || m.prefixLength <= 0) {
        return;
    }
    char* trimmed = str::Dup(ap->title);
    if (!trimmed) {
        return;
    }
    trimmed[m.prefixLength] = 0;
    str::TrimWSInPlace(trimmed, str::TrimOpt::Both);
    str::Free(ap->title);
    ap->title = trimmed;
}

static bool AppendixFlatAlreadyHasDocTitle(const Vec<ExtractedTocItem*>& flat, int apIdx) {
    if (apIdx < 0 || apIdx >= flat.Size() || !flat[apIdx]) {
        return false;
    }
    int apLevel = flat[apIdx]->level;
    for (int j = apIdx + 1; j < flat.Size(); j++) {
        ExtractedTocItem* it = flat[j];
        if (!it || !it->title) {
            continue;
        }
        if (it->level <= apLevel) {
            break;
        }
        if (LooksLikeOfficialFileTitle(it->title) || LooksLikeOfficialAppendixName(it->title)) {
            return true;
        }
        if (HeadingLevelFromText(it->title) > 0) {
            break;
        }
    }
    return false;
}

static int OfficialAppendixChildInsertAt(const Vec<ExtractedTocItem*>& flat, int apIdx) {
    if (apIdx < 0 || apIdx >= flat.Size() || !flat[apIdx]) {
        return apIdx + 1;
    }
    int apLevel = flat[apIdx]->level;
    for (int j = apIdx + 1; j < flat.Size(); j++) {
        if (!flat[j]) {
            continue;
        }
        if (flat[j]->level <= apLevel) {
            return j;
        }
        return apIdx + 1;
    }
    return flat.Size();
}

static float OfficialAppendixTitleUpperY(const ExtractedTocItem* ap, int apIdx, const Vec<ExtractedTocItem*>& flat) {
    if (!ap) {
        return 1e9f;
    }
    float upperY = 1e9f;
    int apLevel = ap->level;
    for (int j = apIdx + 1; j < flat.Size(); j++) {
        ExtractedTocItem* it = flat[j];
        if (!it) {
            continue;
        }
        if (it->level <= apLevel) {
            break;
        }
        if (it->pageNo == ap->pageNo && it->y + 0.5f < upperY) {
            upperY = it->y;
        }
        if (it->pageNo > ap->pageNo) {
            break;
        }
    }
    return upperY;
}

static bool OfficialLineInAppendixTitleBand(const ScanLine& sl, int apPage, float apY, float upperY, int maxPage) {
    if (!sl.text || sl.srcPage < apPage || sl.srcPage > maxPage) {
        return false;
    }
    if (sl.srcPage == apPage) {
        if (sl.y + 0.5f <= apY) {
            return false;
        }
        if (upperY < 1e8f && sl.y + 0.5f >= upperY) {
            return false;
        }
        return true;
    }
    return sl.srcPage == apPage + 1 && upperY > 1e8f;
}

static void CollectOfficialTitleForAppendix(const Vec<ScanLine>& lines, int nPages, int apFlatIdx,
                                            const Vec<ExtractedTocItem*>& flat, float minFont, float namedMin,
                                            Vec<OfficialTitleInsert>& inserts) {
    if (apFlatIdx < 0 || apFlatIdx >= flat.Size()) {
        return;
    }
    ExtractedTocItem* apItem = flat[apFlatIdx];
    if (!apItem || !apItem->title) {
        return;
    }
    if (ParseHeadingMarker(apItem->title).type != MarkerType::Appendix) {
        return;
    }
    if (OfficialAppendixHasInlineName(apItem->title) || AppendixFlatAlreadyHasDocTitle(flat, apFlatIdx)) {
        return;
    }
    char* glued = OfficialAppendixGluedDocTitle(apItem->title);
    if (glued) {
        if (!OfficialTitleInsertDuplicate(inserts, glued)) {
            TrimOfficialAppendixToLabel(apItem);
            OfficialTitleInsert ins;
            ins.item = NewItem(glued, apItem->pageNo, apItem->x, apItem->y + 12, apItem->level + 1, 90);
            ins.beforeFlat = OfficialAppendixChildInsertAt(flat, apFlatIdx);
            ins.underAppendix = true;
            inserts.Append(ins);
        }
        str::Free(glued);
        return;
    }
    int page = apItem->pageNo;
    float apY = apItem->y;
    float upperY = OfficialAppendixTitleUpperY(apItem, apFlatIdx, flat);
    if (upperY > 1e8f) {
        upperY = apY + 110;
    }
    int maxPage = page;
    if (OfficialAppendixTitleUpperY(apItem, apFlatIdx, flat) > 1e8f) {
        maxPage = page + 1;
    }
    int titleLevel = apItem->level + 1;
    int insertAt = OfficialAppendixChildInsertAt(flat, apFlatIdx);
    int skip = -1;
    float bestFont = 0;
    char* bestTitle = nullptr;
    ScanLine bestSl = {};
    for (int i = 0; i < lines.Size(); i++) {
        if (i == skip) {
            continue;
        }
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.srcPage < 1 || sl.srcPage > nPages) {
            continue;
        }
        if (!OfficialLineInAppendixTitleBand(sl, page, apY, upperY, maxPage)) {
            continue;
        }
        if (HeadingLevelFromText(sl.text) > 0 || LooksLikeOfficialBoilerplate(sl.text) ||
            LooksLikeDocNumberLine(sl.text) || LineLooksLikePageNumber(sl.text)) {
            continue;
        }
        int extra = 0;
        char* joined = JoinLargeOfficialTitleRun(lines, i, namedMin, &extra);
        const char* title = joined ? joined : sl.text;
        bool named = LooksLikeOfficialTitle(title) || LooksLikeOfficialAppendixName(title);
        float need = named ? namedMin : minFont;
        if (sl.fontSize + 0.05f < need) {
            str::Free(joined);
            continue;
        }
        if (!LooksLikeOfficialFileTitle(title) || OfficialTitleTextTaken(flat, title) ||
            OfficialTitleInsertDuplicate(inserts, title)) {
            str::Free(joined);
            if (extra > 0) {
                skip = i + extra;
            }
            continue;
        }
        if (sl.fontSize > bestFont) {
            str::Free(bestTitle);
            bestFont = sl.fontSize;
            bestTitle = joined ? joined : str::Dup(sl.text);
            bestSl = sl;
            joined = nullptr;
        } else {
            str::Free(joined);
        }
        if (extra > 0) {
            skip = i + extra;
        }
    }
    if (!bestTitle) {
        return;
    }
    OfficialTitleInsert ins;
    ins.item = NewItem(bestTitle, bestSl.srcPage, bestSl.x, bestSl.y, titleLevel, 90);
    ins.beforeFlat = insertAt;
    ins.underAppendix = true;
    inserts.Append(ins);
    str::Free(bestTitle);
}

static bool OfficialPageHasAppendix(const Vec<ScanLine>& lines, const Vec<ExtractedTocItem*>& flat,
                                    const ExtractedTocItem* yi, int yiFlatIdx) {
    if (!yi) {
        return false;
    }
    for (int i = yiFlatIdx - 1; i >= 0; i--) {
        ExtractedTocItem* p = flat[i];
        if (!p || !p->title) {
            continue;
        }
        if (p->pageNo < yi->pageNo) {
            break;
        }
        if (p->pageNo == yi->pageNo && p->y + 0.5f < yi->y) {
            HeadingMarker m = ParseHeadingMarker(p->title);
            if (m.type == MarkerType::Appendix) {
                return true;
            }
        }
    }
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.srcPage != yi->pageNo || sl.y + 0.5f >= yi->y) {
            continue;
        }
        if (ParseHeadingMarker(sl.text).type == MarkerType::Appendix) {
            return true;
        }
    }
    return false;
}

static void CollectOfficialTitlesAboveYi(const Vec<ScanLine>& lines, int nPages, const ExtractedTocItem* yiItem,
                                         int yiFlatIdx, const Vec<ExtractedTocItem*>& flat, float minFont,
                                         float namedMin, Vec<OfficialTitleInsert>& inserts) {
    if (!yiItem) {
        return;
    }
    int page = yiItem->pageNo;
    float yiY = yiItem->y;
    bool underAp = OfficialPageHasAppendix(lines, flat, yiItem, yiFlatIdx);
    int titleLevel = underAp ? yiItem->level : 1;
    int skip = -1;
    for (int i = 0; i < lines.Size(); i++) {
        if (i == skip) {
            continue;
        }
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.srcPage < 1 || sl.srcPage > nPages) {
            continue;
        }
        if (sl.srcPage < page - 1 || sl.srcPage > page) {
            continue;
        }
        if (sl.srcPage == page && sl.y + 0.5f >= yiY) {
            continue;
        }
        if (HeadingLevelFromText(sl.text) > 0 || LooksLikeOfficialBoilerplate(sl.text) ||
            LooksLikeDocNumberLine(sl.text) || LineLooksLikePageNumber(sl.text)) {
            continue;
        }
        int extra = 0;
        char* joined = JoinLargeOfficialTitleRun(lines, i, namedMin, &extra);
        const char* title = joined ? joined : sl.text;
        bool named = LooksLikeOfficialTitle(title) || LooksLikeOfficialAppendixName(title);
        float need = named ? namedMin : minFont;
        if (sl.fontSize + 0.05f < need) {
            str::Free(joined);
            continue;
        }
        if (!LooksLikeOfficialFileTitle(title) || OfficialTitleTextTaken(flat, title)) {
            str::Free(joined);
            if (extra > 0) {
                skip = i + extra;
            }
            continue;
        }
        if (OfficialTitleInsertDuplicate(inserts, title)) {
            str::Free(joined);
            if (extra > 0) {
                skip = i + extra;
            }
            goto next_line;
        }
        {
            OfficialTitleInsert ins;
            ins.item = NewItem(title, sl.srcPage, sl.x, sl.y, titleLevel, 90);
            ins.beforeFlat = yiFlatIdx;
            ins.underAppendix = underAp;
            inserts.Append(ins);
        }
        str::Free(joined);
        if (extra > 0) {
            skip = i + extra;
        }
    next_line:;
    }
}

static bool OfficialCountsAsMainDocFrontTitle(const char* title) {
    if (!title) {
        return false;
    }
    if (LooksLikeOfficialTitle(title)) {
        return true;
    }
    // 附件：1.文件名（征求意见稿） matches LooksLikeOfficialFileTitle but is not the covering letter title.
    if (ParseHeadingMarker(title).type == MarkerType::Appendix) {
        return false;
    }
    return LooksLikeOfficialFileTitle(title) && str::StartsWith(title, "关于");
}

static bool OfficialFlatHasFrontTitle(const Vec<ExtractedTocItem*>& flat, const Vec<OfficialTitleInsert>& inserts) {
    for (int t = 0; t < inserts.Size(); t++) {
        if (inserts[t].item && inserts[t].item->title && inserts[t].item->pageNo <= 3 &&
            OfficialCountsAsMainDocFrontTitle(inserts[t].item->title)) {
            return true;
        }
    }
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* it = flat[i];
        if (!it || !it->title || it->pageNo > 3) {
            continue;
        }
        if (OfficialCountsAsMainDocFrontTitle(it->title)) {
            return true;
        }
    }
    return false;
}

static int ScoreMainOfficialDocTitle(const char* title, const ScanLine& sl) {
    if (!title) {
        return 0;
    }
    int score = 10;
    int g = GlyphCount(title);
    if (str::StartsWith(title, "关于")) {
        score += 12;
    }
    if (str::Find(title, "关于印发") || str::Find(title, "印发《")) {
        score += 12;
    }
    if (str::EndsWith(title, "通知")) {
        score += 8;
    }
    if (str::EndsWith(title, "情况说明")) {
        score += 10;
    }
    if (str::EndsWith(title, "的函") || str::EndsWith(title, "请示")) {
        score += 6;
    }
    if (g >= 10 && g <= 50) {
        score += 4;
    }
    if (sl.fontSize >= 12) {
        score += 2;
    }
    if (sl.bold) {
        score += 3;
    }
    score += (int)(sl.fontSize * 0.5f);
    return score;
}

static void CollectMainOfficialDocTitle(const Vec<ScanLine>& lines, int nPages, const Vec<ExtractedTocItem*>& flat,
                                        float namedMin, Vec<OfficialTitleInsert>& inserts) {
    if (OfficialFlatHasFrontTitle(flat, inserts)) {
        return;
    }
    int firstYiFlat = -1;
    for (int i = 0; i < flat.Size(); i++) {
        if (flat[i] && IsOfficialYiDunhaoItem(flat[i]->title)) {
            firstYiFlat = i;
            break;
        }
    }
    int bestScore = 0;
    char* bestText = nullptr;
    ScanLine bestSl = {};
    int skip = -1;
    for (int i = 0; i < lines.Size(); i++) {
        if (i == skip) {
            continue;
        }
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.srcPage < 1 || sl.srcPage > nPages || sl.srcPage > 3) {
            continue;
        }
        if (firstYiFlat >= 0) {
            const ExtractedTocItem* yi = flat[firstYiFlat];
            if (yi && sl.srcPage > yi->pageNo) {
                continue;
            }
            if (yi && sl.srcPage == yi->pageNo && sl.y + 0.5f >= yi->y) {
                continue;
            }
        }
        if (HeadingLevelFromText(sl.text) > 0 || LooksLikeOfficialBoilerplate(sl.text) ||
            LooksLikeDocNumberLine(sl.text) || LineLooksLikePageNumber(sl.text)) {
            continue;
        }
        if (str::EndsWith(sl.text, "汇报材料") && !str::StartsWith(sl.text, "关于")) {
            continue;
        }
        int extra = 0;
        char* cand = JoinOfficialTitleRun(lines, i, &extra);
        const char* title = cand ? cand : sl.text;
        if (!LooksLikeOfficialTitle(title) && !LooksLikeOfficialFileTitle(title)) {
            str::Free(cand);
            if (extra > 0) {
                skip = i + extra;
            }
            continue;
        }
        bool named = LooksLikeOfficialTitle(title);
        if (named && sl.fontSize + 0.05f < namedMin && !sl.bold) {
            str::Free(cand);
            if (extra > 0) {
                skip = i + extra;
            }
            continue;
        }
        int score = ScoreMainOfficialDocTitle(title, sl);
        if (score > bestScore) {
            bestScore = score;
            str::Free(bestText);
            bestText = cand ? cand : str::Dup(sl.text);
            bestSl = sl;
            cand = nullptr;
        }
        str::Free(cand);
        if (extra > 0) {
            skip = i + extra;
        }
    }
    if (!bestText || bestScore < 12) {
        str::Free(bestText);
        return;
    }
    if (OfficialTitleTextTaken(flat, bestText) || OfficialTitleInsertDuplicate(inserts, bestText)) {
        str::Free(bestText);
        return;
    }
    OfficialTitleInsert ins;
    ins.item = NewItem(bestText, bestSl.srcPage, bestSl.x, bestSl.y, 1, 95);
    ins.beforeFlat = 0;
    ins.underAppendix = false;
    inserts.Append(ins);
    str::Free(bestText);
}

static void DropOfficialExtractJunk(Vec<ExtractedTocItem*>& nodes) {
    Vec<ExtractedTocItem*> keep;
    for (int i = 0; i < nodes.Size(); i++) {
        ExtractedTocItem* n = nodes[i];
        if (!n) {
            continue;
        }
        DropOfficialExtractJunk(n->children);
        if (IsOfficialExtractJunkTitle(n->title)) {
            for (int c = 0; c < n->children.Size(); c++) {
                keep.Append(n->children[c]);
            }
            n->children.Reset();
            delete n;
            continue;
        }
        keep.Append(n);
    }
    nodes.Reset();
    for (int i = 0; i < keep.Size(); i++) {
        nodes.Append(keep[i]);
    }
}

// Merged 公文 PDFs restart at 一、. Only the large file title sitting above
// that 一、 is inserted as L1; body sentences that merely mention 标准体系
// are not titles.
static void InsertOfficialDocTitles(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots) {
    if (roots.Size() < 1) {
        return;
    }
    float body = OfficialBodyFontSize(lines);
    float yiFont = OfficialYiHeadingFont(lines, nPages);
    float minFont = body * 1.35f;
    if (yiFont >= 4) {
        float fromYi = yiFont * 1.25f;
        if (fromYi > minFont) {
            minFont = fromYi;
        }
    }
    float namedMin = body >= 4 ? body : 10;
    if (yiFont >= 4 && yiFont * 0.92f > namedMin) {
        namedMin = yiFont * 0.92f;
    }
    Vec<ExtractedTocItem*> flat;
    FlattenExtractedDetach(roots, flat);
    for (int i = 0; i < flat.Size(); i++) {
        TightenOfficialAppendixTitle(flat[i]);
    }
    Vec<OfficialTitleInsert> inserts;
    CollectMainOfficialDocTitle(lines, nPages, flat, namedMin, inserts);
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* it = flat[i];
        if (!it || !IsOfficialYiDunhaoItem(it->title)) {
            continue;
        }
        CollectOfficialTitlesAboveYi(lines, nPages, it, i, flat, minFont, namedMin, inserts);
    }
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* it = flat[i];
        if (!it || !it->title || ParseHeadingMarker(it->title).type != MarkerType::Appendix) {
            continue;
        }
        CollectOfficialTitleForAppendix(lines, nPages, i, flat, minFont, namedMin, inserts);
    }
    if (inserts.Size() < 1) {
        BuildTreeFromFlat(flat, roots);
        DropOfficialExtractJunk(roots);
        return;
    }
    for (int t = inserts.Size() - 1; t >= 0; t--) {
        int at = inserts[t].beforeFlat;
        if (at < 0) {
            at = 0;
        }
        if (at > flat.Size()) {
            at = flat.Size();
        }
        flat.InsertAt(at, inserts[t].item);
    }
    int bumpAfter = -1;
    for (int t = 0; t < inserts.Size(); t++) {
        if (inserts[t].underAppendix) {
            continue;
        }
        for (int i = 0; i < flat.Size(); i++) {
            if (flat[i] == inserts[t].item) {
                flat[i]->level = 1;
                if (bumpAfter < 0 || i < bumpAfter) {
                    bumpAfter = i;
                }
                break;
            }
        }
    }
    if (bumpAfter >= 0) {
        for (int i = bumpAfter + 1; i < flat.Size(); i++) {
            if (flat[i] && flat[i]->level < 6) {
                flat[i]->level += 1;
            }
        }
    }
    BuildTreeFromFlat(flat, roots);
    DropOfficialExtractJunk(roots);
}

static bool InferDocTitle(const Vec<ScanLine>& lines, Vec<ExtractedTocItem*>& roots) {
    int best = -1;
    int bestScore = 0;
    char* bestText = nullptr;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.srcPage > 2) {
            continue;
        }
        int extra = 0;
        char* cand = JoinOfficialTitleRun(lines, i, &extra);
        const char* title = cand ? cand : sl.text;
        if (!LooksLikeOfficialTitle(title)) {
            str::Free(cand);
            continue;
        }
        int g = GlyphCount(title);
        int score = 10;
        if (str::StartsWith(title, "关于")) {
            score += 8;
        }
        if (str::EndsWith(title, "通知") || str::EndsWith(title, "的函") || str::EndsWith(title, "请示")) {
            score += 4;
        }
        if (g >= 10 && g <= 40) {
            score += 3;
        }
        if (sl.fontSize >= 12) {
            score += 2;
        }
        if (sl.bold) {
            score += 2;
        }
        if (score > bestScore) {
            bestScore = score;
            best = i;
            str::Free(bestText);
            bestText = cand;
            cand = nullptr;
        }
        str::Free(cand);
        if (extra > 0) {
            i += extra;
        }
    }
    if (best < 0) {
        str::Free(bestText);
        return false;
    }
    const char* use = bestText ? bestText : lines[best].text;
    roots.Append(NewItem(use, lines[best].srcPage, lines[best].x, lines[best].y, 1));
    str::Free(bestText);
    return CountExtracted(roots) >= 1;
}

static bool FrontMatterHas(const Vec<ScanLine>& lines, int maxPage, const char* needle) {
    if (!needle || !needle[0]) {
        return false;
    }
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > maxPage || !lines[i].text) {
            continue;
        }
        if (str::Find(lines[i].text, needle)) {
            return true;
        }
    }
    return false;
}

static bool PathHasNeedle(const char* filePath, const char* needle) {
    return filePath && needle && str::Find(filePath, needle);
}

static bool LineIsChapterHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::Chapter) {
        return true;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    return num.isChapter;
}

static int CountChapterHeadingLines(const Vec<ScanLine>& lines, int maxPage) {
    int n = 0;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > maxPage || !lines[i].text) {
            continue;
        }
        if (LineIsChapterHeading(lines[i].text)) {
            n++;
        }
    }
    return n;
}

static bool LooksLikeFeasibilityPath(const char* filePath) {
    return PathHasNeedle(filePath, "可行性") || PathHasNeedle(filePath, "可研报告") || PathHasNeedle(filePath, "可研");
}

static bool LooksLikeFeasibilityBody(const Vec<ScanLine>& lines, int maxPage) {
    return FrontMatterHas(lines, maxPage, "可行性研究") || FrontMatterHas(lines, maxPage, "可行性分析") ||
           FrontMatterHas(lines, maxPage, "可研报告") || FrontMatterHas(lines, maxPage, "项目概述") ||
           FrontMatterHas(lines, maxPage, "建设规模");
}

static bool FrontHasThesisIntroChapter(const Vec<ScanLine>& lines, int maxPage) {
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > maxPage || !lines[i].text) {
            continue;
        }
        if (LineIsChapterHeading(lines[i].text) && str::Find(lines[i].text, "绪论")) {
            return true;
        }
    }
    return false;
}

static bool FrontHasShortAbstract(const Vec<ScanLine>& lines, int maxPage) {
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > maxPage || !lines[i].text) {
            continue;
        }
        int g = GlyphCount(lines[i].text);
        if (g > 24) {
            continue;
        }
        if (str::Find(lines[i].text, "摘要") || str::ContainsI(lines[i].text, "abstract")) {
            return true;
        }
    }
    return false;
}

static bool FrontHasEnglishPrintedToc(const Vec<ScanLine>& lines, int nPages) {
    int front = nPages;
    if (front > 20) {
        front = 20;
    }
    if (front < 1) {
        front = 1;
    }
    bool enHead = false;
    int nInline = 0;
    int nPage = 0;
    int nLeader = 0;
    int nShort = 0;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > front || !lines[i].text) {
            continue;
        }
        const char* s = lines[i].text;
        if (LooksLikeEnglishTocHeading(s)) {
            enHead = true;
            continue;
        }
        char* title = nullptr;
        int printed = 0;
        if (ParsePrintedTocLine(s, &title, &printed) && title) {
            nInline++;
            str::Free(title);
            continue;
        }
        str::Free(title);
        if (LineLooksLikePageNumber(s)) {
            nPage++;
            continue;
        }
        if (LooksLikeLeaderTitle(s)) {
            nLeader++;
            continue;
        }
        int g = GlyphCount(s);
        if (g >= 2 && g <= 48 && HasLetterOrCjk(s)) {
            nShort++;
        }
    }
    if (!enHead) {
        return false;
    }
    if (nInline >= 3) {
        return true;
    }
    if (nPage >= 3 && nShort >= 3) {
        return true;
    }
    if (nLeader >= 3 && nShort >= 3) {
        return true;
    }
    return false;
}

// 文号 / 印发 / 关于…的通知：管理办法、厅发通知等公文。压过「第X章 + 目录/日录」。
static bool LooksLikeOfficialFrontMatter(const Vec<ScanLine>& lines, int maxPage) {
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > maxPage || !lines[i].text) {
            continue;
        }
        const char* s = lines[i].text;
        if (LooksLikeDocNumberLine(s)) {
            return true;
        }
        if (str::Find(s, "关于印发") || str::Find(s, "印发《")) {
            return true;
        }
        if (str::StartsWith(s, "关于") && (str::Find(s, "的通知") || str::Find(s, "的函") || str::Find(s, "请示"))) {
            return true;
        }
    }
    return false;
}

static ExtractTocDocClass ClassifyExtractTocDoc(const Vec<ScanLine>& lines, int nPages, const char* filePath) {
    int front = nPages;
    if (front > 20) {
        front = 20;
    }
    if (front < 1) {
        front = 1;
    }
    if (PathHasNeedle(filePath, "合同") || PathHasNeedle(filePath, "协议")) {
        return ExtractTocDocClass::Contract;
    }
    if (FrontMatterHas(lines, front, "甲方") && FrontMatterHas(lines, front, "乙方")) {
        return ExtractTocDocClass::Contract;
    }
    bool feasPath = LooksLikeFeasibilityPath(filePath);
    bool feasBody = LooksLikeFeasibilityBody(lines, front);
    bool advisor = FrontMatterHas(lines, front, "指导教师") || FrontMatterHas(lines, front, "学位论文");
    bool keywords = FrontMatterHas(lines, front, "关键词");
    bool intro = FrontHasThesisIntroChapter(lines, front);
    bool abs = FrontHasShortAbstract(lines, front);
    // Thesis cues beat a parent folder named 可行性研究 (e.g. 参考文献 under a 可研 project).
    // Body 项目概述/建设规模 without 绪论/导师 still stays Official.
    if (advisor || intro || (abs && keywords && !feasBody)) {
        return ExtractTocDocClass::Paper;
    }
    if (LooksLikeOfficialFrontMatter(lines, front)) {
        return ExtractTocDocClass::Official;
    }
    int nChap = CountChapterHeadingLines(lines, front);
    bool preface = FrontMatterHas(lines, front, "前言") || FrontMatterHas(lines, front, "后记");
    bool tocHead = false;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > front || !lines[i].text) {
            continue;
        }
        if (LooksLikePrintedTocHeading(lines[i].text)) {
            tocHead = true;
            break;
        }
    }
    bool feas = feasPath || feasBody;
    if (!feas && nChap >= 1 && preface) {
        return ExtractTocDocClass::Book;
    }
    if (!feas && nChap >= 1 && tocHead) {
        return ExtractTocDocClass::Book;
    }
    bool xinDe = false;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage < 1 || lines[i].srcPage > front || !lines[i].text) {
            continue;
        }
        if (StartsWithXinDeHeading(lines[i].text)) {
            xinDe = true;
            break;
        }
    }
    if (!feas && xinDe) {
        return ExtractTocDocClass::Book;
    }
    if (!feas && tocHead && (preface || xinDe)) {
        return ExtractTocDocClass::Book;
    }
    if (!feas && FrontHasEnglishPrintedToc(lines, nPages)) {
        return ExtractTocDocClass::Book;
    }
    return ExtractTocDocClass::Official;
}

static bool LooksLikeFigureOrTableCaption(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int start = i;
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp == 0x56FE || cp == 0x8868) {
        SkipSpacesUtf8(s, len, i);
        int d = i < len ? Utf8CodepointNext(s, len, i) : 0;
        return IsDigitCp(d);
    }
    i = start;
    if (str::StartsWithI(s + start, "figure") || str::StartsWithI(s + start, "table")) {
        return GlyphCount(s) <= 40;
    }
    return false;
}

static bool LooksLikeBibItem(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp != '[' && cp != 0xFF3B) {
        return false;
    }
    int d = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return IsDigitCp(d);
}

static bool LooksLikeContractPartyLine(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::StartsWith(s, "甲方") || str::StartsWith(s, "乙方") || str::StartsWith(s, "丙方") ||
        str::StartsWith(s, "鉴于")) {
        return true;
    }
    int g = GlyphCount(s);
    return g <= 24 && (str::Find(s, "签字") || str::Find(s, "签署页"));
}

// "1）招标文件" / "1) 付款方式" — digit + closing paren, not "1." / "1、".
static bool ContractStartsWithCloseParenList(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int nDig = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (IsDigitCp(cp)) {
            nDig++;
            if (nDig > 3) {
                return false;
            }
            continue;
        }
        i = save;
        break;
    }
    if (nDig < 1) {
        return false;
    }
    SkipSpacesUtf8(s, len, i);
    if (i >= len) {
        return false;
    }
    int cp = Utf8CodepointNext(s, len, i);
    if (!IsParenCloseCp(cp)) {
        return false;
    }
    return HasLetterOrCjk(s + i);
}

// Body cites like "第6条规定，相应延长服务期" / "v 第8条规定" — not a heading.
static bool LooksLikeInlineArticleCite(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (!str::Find(s, "第") || !str::Find(s, "条")) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipSpacesUtf8(s, len, i);
    int first = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (first != 0x7B2C) {
        return true;
    }
    int g = GlyphCount(s);
    if (g > 16) {
        return true;
    }
    return str::Find(s, "规定") || str::Find(s, "相应") || str::Find(s, "按照") || str::Find(s, "根据") ||
           str::Find(s, "所述") || str::Find(s, "执行");
}

static bool LooksLikeContractBodyClause(const char* s, float dx) {
    if (!s || !s[0]) {
        return false;
    }
    if (LooksLikeInlineArticleCite(s)) {
        return true;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    HeadingMarker m = ParseHeadingMarker(s);
    if (m.type == MarkerType::Appendix || num.nComp >= 2) {
        return false;
    }
    if (ContractStartsWithCloseParenList(s) || m.type == MarkerType::ArabicParen ||
        m.type == MarkerType::ChineseParen) {
        return false;
    }
    if (num.nComp != 1) {
        return false;
    }
    int g = GlyphCount(s);
    if (g <= 16) {
        return false;
    }
    if (g > 28) {
        return true;
    }
    if (dx > 280) {
        return true;
    }
    return LineEndsWithPeriod(s);
}

static bool ContractKeepLongNumberedHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(s, &num);
    if (num.nComp >= 2) {
        return true;
    }
    HeadingMarker m = ParseHeadingMarker(s);
    return m.type == MarkerType::Appendix;
}

static void CapTitleAfterNumbering(char* s, int maxBodyGlyphs) {
    if (!s || !s[0] || maxBodyGlyphs < 4) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    SkipWsUtf8(s, len, i);
    SkipHeadingNumbering(s, len, i);
    SkipWsUtf8(s, len, i);
    int g = 0;
    while (i < len) {
        int save = i;
        if (Utf8CodepointNext(s, len, i) <= 0) {
            break;
        }
        g++;
        if (g >= maxBodyGlyphs) {
            s[save] = 0;
            str::TrimWSInPlace(s, str::TrimOpt::Both);
            return;
        }
    }
}

static bool LinesHaveArticleHeadings(const Vec<ScanLine>& lines) {
    for (int i = 0; i < lines.Size(); i++) {
        if (!lines[i].text) {
            continue;
        }
        HeadingMarker m = ParseHeadingMarker(lines[i].text);
        if (m.type == MarkerType::Article) {
            return true;
        }
    }
    return false;
}

static int OutlineLevelForProfile(TocProfileKind kind, bool hasArticles, const char* title) {
    (void)hasArticles;
    if (!title || !title[0]) {
        return 0;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(title, &num);
    HeadingMarker m = ParseHeadingMarker(title);
    int g = GlyphCount(title);
    if (kind == TocProfileKind::Contract) {
        if (LooksLikeInlineArticleCite(title)) {
            return 0;
        }
        if (m.type == MarkerType::Article) {
            return 1;
        }
        if (m.type == MarkerType::Appendix) {
            return 1;
        }
        if (m.type == MarkerType::ChineseDunhao) {
            return 0;
        }
        if (m.type == MarkerType::ChineseParen || m.type == MarkerType::ArabicParen) {
            return 3;
        }
        if (ContractStartsWithCloseParenList(title)) {
            return 3;
        }
        if (num.nComp >= 2) {
            return num.nComp;
        }
        if (num.nComp == 1) {
            return 1;
        }
        return 0;
    }
    if (kind == TocProfileKind::Paper) {
        if (m.type == MarkerType::Chapter || num.isChapter) {
            return 1;
        }
        if (m.type == MarkerType::Section) {
            return 2;
        }
        if (num.nComp >= 2) {
            return num.nComp;
        }
        if (g <= 12 && (str::Find(title, "摘要") || str::ContainsI(title, "abstract"))) {
            return 1;
        }
        if (g <= 16 && (str::Find(title, "关键词") || str::Find(title, "参考文献") || str::Find(title, "致谢"))) {
            return 1;
        }
        return 0;
    }
    if (kind == TocProfileKind::Book) {
        if (m.type == MarkerType::ChineseDunhao || m.type == MarkerType::ChineseParen ||
            m.type == MarkerType::ArabicParen || m.type == MarkerType::YiShi) {
            return 0;
        }
        if (m.type == MarkerType::Chapter || num.isChapter) {
            return 1;
        }
        if (StartsWithXinDeHeading(title)) {
            return 1;
        }
        if (m.type == MarkerType::Section) {
            return 2;
        }
        if (num.nComp >= 2) {
            return num.nComp;
        }
        if (g <= 12 && (str::Find(title, "前言") || str::Find(title, "后记") || str::Find(title, "总序"))) {
            return 1;
        }
        if (str::StartsWithI(title, "chapter")) {
            return 1;
        }
        return 0;
    }
    if (m.type == MarkerType::ChineseDunhao || m.type == MarkerType::ChineseParen ||
        m.type == MarkerType::ArabicParen || m.type == MarkerType::YiShi) {
        return 0;
    }
    if (m.type == MarkerType::Chapter || num.isChapter) {
        return 1;
    }
    if (m.type == MarkerType::Section) {
        return 2;
    }
    if (m.type == MarkerType::Appendix) {
        return 1;
    }
    if (num.nComp >= 2) {
        return num.nComp;
    }
    int hl = HeadingLevelFromText(title);
    if (hl == 1) {
        return 1;
    }
    if (str::StartsWithI(title, "chapter")) {
        return 1;
    }
    if (str::StartsWithI(title, "section")) {
        return 2;
    }
    return 0;
}

static void StealExtractedInOrder(Vec<ExtractedTocItem*>& nodes, Vec<ExtractedTocItem*>& flat) {
    Vec<ExtractedTocItem*> copy;
    for (int i = 0; i < nodes.Size(); i++) {
        copy.Append(nodes[i]);
    }
    nodes.Reset();
    for (int i = 0; i < copy.Size(); i++) {
        ExtractedTocItem* it = copy[i];
        Vec<ExtractedTocItem*> kids;
        for (int k = 0; k < it->children.Size(); k++) {
            kids.Append(it->children[k]);
        }
        it->children.Reset();
        flat.Append(it);
        StealExtractedInOrder(kids, flat);
    }
}

static void CountArabicDecimalSpine(const Vec<ExtractedTocItem*>& nodes, int* nDec, int* nCn) {
    for (int i = 0; i < nodes.Size(); i++) {
        const char* t = nodes[i]->title;
        if (t) {
            ParsedNumbering num;
            ParseHeadingNumbering(t, &num);
            HeadingMarker m = ParseHeadingMarker(t);
            if (num.nComp >= 1) {
                (*nDec)++;
            }
            if (m.type == MarkerType::ChineseDunhao || m.type == MarkerType::ChineseParen) {
                (*nCn)++;
            }
        }
        CountArabicDecimalSpine(nodes[i]->children, nDec, nCn);
    }
}

static bool TocHasNumberingRaw(const Vec<ExtractedTocItem*>& nodes, const char* raw) {
    if (!raw || !raw[0]) {
        return false;
    }
    for (int i = 0; i < nodes.Size(); i++) {
        if (nodes[i]->title) {
            ParsedNumbering num;
            ParseHeadingNumbering(nodes[i]->title, &num);
            if (num.raw[0] && str::Eq(num.raw, raw)) {
                return true;
            }
        }
        if (TocHasNumberingRaw(nodes[i]->children, raw)) {
            return true;
        }
    }
    return false;
}

static int TocItemPosCmp(const ExtractedTocItem* a, const ExtractedTocItem* b) {
    if (a->pageNo != b->pageNo) {
        return a->pageNo < b->pageNo ? -1 : 1;
    }
    if (a->y + 0.5f < b->y) {
        return -1;
    }
    if (a->y > b->y + 0.5f) {
        return 1;
    }
    return 0;
}

static void MergeDeeperArabicHeadings(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots) {
    int nDec = 0;
    int nCn = 0;
    CountArabicDecimalSpine(roots, &nDec, &nCn);
    if (nDec < 2 || nDec <= nCn * 2) {
        return;
    }
    Vec<ExtractedTocItem*> extra;
    int skipNext = -1;
    for (int i = 0; i < lines.Size(); i++) {
        if (i == skipNext) {
            continue;
        }
        const ScanLine& sl = lines[i];
        if (!sl.text || sl.srcPage < 1 || sl.srcPage > nPages) {
            continue;
        }
        char* joined = nullptr;
        const char* text = sl.text;
        if (i + 1 < lines.Size()) {
            ParsedNumbering n0;
            ParseHeadingNumbering(sl.text, &n0);
            int raw0 = (int)str::Len(n0.raw);
            bool bareNum = n0.nComp >= 3 && n0.comp[0] >= 1 && raw0 > 0 && !HasLetterOrCjk(sl.text + raw0);
            const ScanLine& nx = lines[i + 1];
            if (bareNum && nx.srcPage == sl.srcPage && nx.text && HasLetterOrCjk(nx.text)) {
                ParsedNumbering n1;
                ParseHeadingNumbering(nx.text, &n1);
                int g1 = GlyphCount(nx.text);
                if (n1.nComp < 1 && g1 >= 2 && g1 <= 40) {
                    joined = str::Join(sl.text, " ", nx.text);
                    if (joined) {
                        text = joined;
                        skipNext = i + 1;
                    }
                }
            }
        }
        ParsedNumbering num;
        ParseHeadingNumbering(text, &num);
        if (num.nComp < 3 || !num.raw[0] || num.comp[0] < 1) {
            str::Free(joined);
            continue;
        }
        bool badComp = false;
        for (int k = 0; k < num.nComp; k++) {
            if (num.comp[k] < 1 || num.comp[k] > 40) {
                badComp = true;
                break;
            }
        }
        if (badComp) {
            str::Free(joined);
            continue;
        }
        int rawLen = (int)str::Len(num.raw);
        if (!HasLetterOrCjk(text + rawLen)) {
            str::Free(joined);
            continue;
        }
        int g = GlyphCount(text);
        if (g < 5 || g > kExtractPdfToc.headingMaxGlyphs) {
            str::Free(joined);
            continue;
        }
        if (LooksLikeFigureOrTableCaption(text) || LooksLikeLeaderTitle(text) || LooksLikeArchiveJunk(text) ||
            LooksLikePrintedTocHeading(text) || LooksLikeBibItem(text) || LineLooksLikePageNumber(text)) {
            str::Free(joined);
            continue;
        }
        if (TocHasNumberingRaw(roots, num.raw)) {
            str::Free(joined);
            continue;
        }
        extra.Append(NewItem(text, sl.srcPage, sl.x, sl.y, num.nComp));
        str::Free(joined);
    }
    if (extra.Size() < 1) {
        return;
    }
    Vec<ExtractedTocItem*> flat;
    StealExtractedInOrder(roots, flat);
    Vec<ExtractedTocItem*> merged;
    int ia = 0;
    int ib = 0;
    while (ia < flat.Size() || ib < extra.Size()) {
        if (ia >= flat.Size()) {
            merged.Append(extra[ib++]);
            continue;
        }
        if (ib >= extra.Size()) {
            merged.Append(flat[ia++]);
            continue;
        }
        if (TocItemPosCmp(extra[ib], flat[ia]) < 0) {
            merged.Append(extra[ib++]);
        } else {
            merged.Append(flat[ia++]);
        }
    }
    BuildTreeFromFlat(merged, roots);
}

static int PaperChapterNumber(const char* title) {
    if (!title || !title[0]) {
        return 0;
    }
    ParsedNumbering num;
    ParseHeadingNumbering(title, &num);
    if (num.isChapter && num.nComp >= 1) {
        return num.comp[0];
    }
    HeadingMarker m = ParseHeadingMarker(title);
    if (m.type == MarkerType::Chapter && m.number > 0) {
        return m.number;
    }
    return 0;
}

static void DropBackwardPaperChapters(Vec<ExtractedTocItem*>& flat) {
    int maxChap = 0;
    Vec<ExtractedTocItem*> keep;
    for (int i = 0; i < flat.Size(); i++) {
        int ch = PaperChapterNumber(flat[i]->title);
        if (ch > 0 && maxChap > 0 && ch < maxChap) {
            delete flat[i];
            continue;
        }
        if (ch > maxChap) {
            maxChap = ch;
        }
        keep.Append(flat[i]);
    }
    flat.Reset();
    for (int i = 0; i < keep.Size(); i++) {
        flat.Append(keep[i]);
    }
}

static void RelayoutExtractedByProfile(Vec<ExtractedTocItem*>& roots, TocProfileKind kind, bool hasArticles) {
    Vec<ExtractedTocItem*> flat;
    StealExtractedInOrder(roots, flat);
    for (int i = 0; i < flat.Size(); i++) {
        if (kind == TocProfileKind::Paper || kind == TocProfileKind::Book) {
            StripFalseSectionPrefixOnChapter(&flat[i]->title);
        }
        if (kind == TocProfileKind::Book) {
            continue;
        }
        int lvl = OutlineLevelForProfile(kind, hasArticles, flat[i]->title);
        if (lvl < 1) {
            lvl = 1;
        }
        flat[i]->level = lvl;
    }
    if (kind == TocProfileKind::Paper) {
        DropBackwardPaperChapters(flat);
    }
    BuildTreeFromFlat(flat, roots);
}

static bool InferProfileHeadings(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots,
                                 TocProfileKind kind) {
    bool hasArticles = kind == TocProfileKind::Contract && LinesHaveArticleHeadings(lines);
    StrVec headers;
    CollectHeaderTexts(lines, nPages, headers);
    Vec<ExtractedTocItem*> flat;
    int skipMerged = -1;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (i == skipMerged) {
            continue;
        }
        if (!sl.text) {
            continue;
        }
        if (PageLooksLikeCipOrColophon(lines, sl.srcPage)) {
            continue;
        }
        char* merged = nullptr;
        const char* title = sl.text;
        int g = GlyphCount(sl.text);
        if (i + 1 < lines.Size() &&
            (ShouldGlueSplitListNumber(sl, lines[i + 1]) || ShouldGlueDunhaoListToNext(sl, lines[i + 1]) ||
             ShouldGlueBareEnumToNext(sl, lines[i + 1]) || ShouldGlueCloseParenEnumToNext(sl, lines[i + 1]) ||
             ShouldGlueBareDottedSectionToNext(sl, lines[i + 1]))) {
            merged = str::Join(sl.text, lines[i + 1].text);
            if (merged) {
                title = merged;
                skipMerged = i + 1;
                g = GlyphCount(title);
            }
        }
        if (g < 2) {
            str::Free(merged);
            continue;
        }
        if (LooksLikeOfficialBoilerplate(title) || LooksLikeDocNumberLine(title) || LooksLikeLeaderTitle(title) ||
            LooksLikeArchiveJunk(title) || LooksLikePrintedTocHeading(title) || LooksLikeFigureOrTableCaption(title) ||
            LooksLikeBibItem(title)) {
            str::Free(merged);
            continue;
        }
        if (kind == TocProfileKind::Contract &&
            (LooksLikeContractPartyLine(title) || LooksLikeInlineArticleCite(title))) {
            str::Free(merged);
            continue;
        }
        if (g > kExtractPdfToc.headingMaxGlyphs && !ContractKeepLongNumberedHeading(title)) {
            str::Free(merged);
            continue;
        }
        if (!merged && (LineLooksLikePageNumber(sl.text) || IsHeaderText(headers, sl.text))) {
            continue;
        }
        char* extraMerge = nullptr;
        if (!merged && i + 1 < lines.Size() && ShouldMergeHeadingLines(sl, lines[i + 1])) {
            int selfLvl = OutlineLevelForProfile(kind, hasArticles, sl.text);
            int nextLvl = OutlineLevelForProfile(kind, hasArticles, lines[i + 1].text);
            if (nextLvl < 1 && !(kind == TocProfileKind::Paper && selfLvl >= 1)) {
                extraMerge = str::Join(sl.text, " ", lines[i + 1].text);
                if (extraMerge) {
                    title = extraMerge;
                    skipMerged = i + 1;
                    g = GlyphCount(title);
                }
            }
        }
        if (!merged) {
            merged = extraMerge;
        } else {
            str::Free(extraMerge);
        }
        char* work = DupTrimmed(title);
        str::Free(merged);
        merged = nullptr;
        if (kind == TocProfileKind::Paper || kind == TocProfileKind::Book) {
            StripFalseSectionPrefixOnChapter(&work);
        }
        if (!work || !HasLetterOrCjk(work)) {
            str::Free(work);
            continue;
        }
        if (kind == TocProfileKind::Contract && LooksLikeContractBodyClause(work, sl.dx)) {
            str::Free(work);
            continue;
        }
        if (kind == TocProfileKind::Contract) {
            TrimTitleToFirstSentence(work);
            CapTitleAfterNumbering(work, 28);
        }
        int lvl = OutlineLevelForProfile(kind, hasArticles, work);
        if (lvl < 1) {
            str::Free(work);
            continue;
        }
        int gp = GlyphCount(work);
        if (gp < 2 || gp > kExtractPdfToc.headingMaxGlyphs) {
            str::Free(work);
            continue;
        }
        flat.Append(NewItem(work, sl.srcPage, sl.x, sl.y, lvl));
        str::Free(work);
    }
    if (flat.Size() < 1) {
        return false;
    }
    EnforceMonotonicPages(flat);
    if (kind == TocProfileKind::Paper) {
        DropBackwardPaperChapters(flat);
    }
    BuildTreeFromFlat(flat, roots);
    return CountExtracted(roots) >= 1;
}

static bool ExtractOfficialToc(EngineBase* engine, Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                               Vec<ExtractedTocItem*>& roots, const char* tocDebugPath) {
    if (TryPrintedToc(engine, lines, labels, nPages, roots, OfficialPrintedTocOpts())) {
        MergeDeeperArabicHeadings(lines, nPages, roots);
        InsertOfficialDocTitles(lines, nPages, roots);
        InheritMissingExtractedDests(roots);
        return true;
    }
    DeleteExtractedTocItems(roots);
    if (InferHeadings(lines, nPages, roots, tocDebugPath)) {
        InsertOfficialDocTitles(lines, nPages, roots);
        InheritMissingExtractedDests(roots);
        return true;
    }
    DeleteExtractedTocItems(roots);
    return InferDocTitle(lines, roots);
}

static bool ExtractContractToc(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots) {
    return InferProfileHeadings(lines, nPages, roots, TocProfileKind::Contract);
}

static bool ExtractPaperToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                            Vec<ExtractedTocItem*>& roots) {
    if (engine && TryPrintedToc(engine, lines, labels, nPages, roots, PaperPrintedTocOpts())) {
        RelayoutExtractedByProfile(roots, TocProfileKind::Paper, false);
        if (CountExtracted(roots) >= 1) {
            return true;
        }
    }
    DeleteExtractedTocItems(roots);
    return InferProfileHeadings(lines, nPages, roots, TocProfileKind::Paper);
}

static bool ExtractBookToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                           Vec<ExtractedTocItem*>& roots, bool bornDigital) {
    char* bookDebug = nullptr;
    if (gCli && gCli->extractTocDebug && engine && engine->FilePath()) {
        bookDebug = str::Join(path::GetPathNoExtTemp(engine->FilePath()), ".book-toc-debug.txt");
    }
    bool printed = ExtractBookPrintedToc(engine, lines, labels, nPages, roots, bookDebug);
    str::Free(bookDebug);
    if (printed) {
        // Scans: optional dest refine on short books. Born-digital already has a
        // text layer and page labels; full-document BM25 is the scan path.
        if (!bornDigital && nPages <= kExtractPdfToc.tocSearchMaxPages && ExtractedHasPrintedBookCalib(roots)) {
            TocCalibRefineExtracted(roots, engine);
        }
        return true;
    }
    DeleteExtractedTocItems(roots);
    if (bornDigital) {
        // Digital books without a parsed Contents are not heading-clustered from
        // the whole file; that harvest is for OCR/scans with no printed TOC.
        return false;
    }
    if (ExtractBookBodyHeadings(lines, nPages, roots)) {
        return true;
    }
    DeleteExtractedTocItems(roots);
    // Numbered-only fallback when style clustering finds nothing.
    return InferProfileHeadings(lines, nPages, roots, TocProfileKind::Book);
}

static ExtractPdfTocKind ExtractFromCollectedLines(EngineBase* engine, Vec<ScanLine>& lines, int nPages, int nText,
                                                   Vec<ExtractedTocItem*>& roots, int* nItemsOut, bool forceExtract,
                                                   bool bornDigital) {
    if (nItemsOut) {
        *nItemsOut = 0;
    }
    RunPrintedTocLogicTests();
    // Printed TOC lives in the first pages. A 169-page scan with text only on
    // those pages is still extractable; do not treat "most pages are images" as
    // no text (that used to kick off a full-document OCR).
    if (nText < 1 && !forceExtract) {
        return ExtractPdfTocKind::NoText;
    }
    if (nText < 1) {
        return ExtractPdfTocKind::NoText;
    }
    Vec<char*> labels;
    for (int p = 1; p <= nPages; p++) {
        labels.Append(str::Dup(engine->GetPageLabeTemp(p)));
    }
    char* tocDebugPath = nullptr;
    if (gCli && gCli->extractTocDebug && engine && engine->FilePath()) {
        tocDebugPath = str::Join(path::GetPathNoExtTemp(engine->FilePath()), ".toc-debug.txt");
    }
    ExtractTocDocClass cls = ClassifyExtractTocDoc(lines, nPages, engine ? engine->FilePath() : nullptr);
    bool ok = false;
    if (cls == ExtractTocDocClass::Contract) {
        ok = ExtractContractToc(lines, nPages, roots);
    } else if (cls == ExtractTocDocClass::Paper) {
        ok = ExtractPaperToc(engine, lines, labels, nPages, roots);
    } else if (cls == ExtractTocDocClass::Book) {
        ok = ExtractBookToc(engine, lines, labels, nPages, roots, bornDigital);
    } else {
        ok = ExtractOfficialToc(engine, lines, labels, nPages, roots, tocDebugPath);
    }
    str::Free(tocDebugPath);
    int n = CountExtracted(roots);
    if ((!ok || n < 1) && cls != ExtractTocDocClass::Book && FrontHasEnglishPrintedToc(lines, nPages)) {
        DeleteExtractedTocItems(roots);
        ok = ExtractBookToc(engine, lines, labels, nPages, roots, bornDigital);
        n = CountExtracted(roots);
    }
    for (int i = 0; i < labels.Size(); i++) {
        str::Free(labels[i]);
    }
    if (!ok || n < 1) {
        DeleteExtractedTocItems(roots);
        return ExtractPdfTocKind::NoHeadings;
    }
    if (nItemsOut) {
        *nItemsOut = n;
    }
    return ExtractPdfTocKind::Ok;
}

// Native PDF text (not this-session OCR). Covers/artwork may be empty; Contents
// and body pages of a born-digital textbook are not.
static bool ExtractPdfLooksBornDigital(EngineBase* engine) {
    if (!engine) {
        return false;
    }
    int n = engine->PageCount();
    int native = 0;
    const int probes[] = {1, 2, 3, 8, 9, 10, 15};
    for (int i = 0; i < dimofi(probes); i++) {
        int p = probes[i];
        if (p < 1 || p > n) {
            continue;
        }
        if (engine->HasCachedOcrText(p)) {
            continue;
        }
        int len = 0;
        engine->GetTextForPage(p, &len);
        if (len >= 40) {
            native++;
        }
    }
    return native >= 2;
}

static int ExtractFrontPageCap(bool bornDigital, int nPages) {
    int cap = bornDigital ? 80 : kExtractPdfToc.tocSearchMaxPages;
    if (nPages < cap) {
        return nPages;
    }
    return cap;
}

static int ExtractProgressTotal(EngineBase* engine, bool bornDigital) {
    int n = engine->PageCount();
    if (bornDigital) {
        return ExtractFrontPageCap(true, n);
    }
    return n;
}

ExtractPdfTocKind ExtractPdfTocFromEngine(EngineBase* engine, Vec<ExtractedTocItem*>& roots, int* nItemsOut) {
    roots.Reset();
    if (nItemsOut) {
        *nItemsOut = 0;
    }
    if (!engine) {
        return ExtractPdfTocKind::Failed;
    }
    bool bornDigital = ExtractPdfLooksBornDigital(engine);
    int nPages = engine->PageCount();
    int to = ExtractFrontPageCap(bornDigital, nPages);
    if (!bornDigital) {
        to = nPages;
    }
    Vec<ScanLine> lines;
    int nText = 0;
    for (int p = 1; p <= to; p++) {
        int before = lines.Size();
        CollectPageScanLines(engine, p, lines);
        if (lines.Size() > before) {
            nText++;
        }
    }
    ExtractPdfTocKind k = ExtractFromCollectedLines(engine, lines, nPages, nText, roots, nItemsOut, false, bornDigital);
    FreeScanLines(lines);
    return k;
}

enum class ExtractPdfTocStatus {
    Ok,
    Cancelled,
    NoText,
    NoHeadings,
    WriteFailed
};

struct ExtractWork {
    EngineBase* engine = nullptr;
    HWND hwndCanvas = nullptr;
    HWND hwndFrame = nullptr;
    LONG cancelSeq = 0;
    ExtractPdfTocStatus status = ExtractPdfTocStatus::NoHeadings;
    Vec<ExtractedTocItem*> roots;
    int nItems = 0;
    int nTextPages = 0;
    bool skipConfirm = false;
    bool persistToDisk = true;
    bool bornDigital = false;
    char* error = nullptr;
};

struct ExtractProgressUi {
    HWND hwnd = nullptr;
    int done = 0;
    int total = 0;
};

static void ExtractProgressOnUi(ExtractProgressUi* p) {
    if (p && p->hwnd && IsWindow(p->hwnd)) {
        TempStr msg = str::FormatTemp(_TRA("Extracting bookmarks… %d / %d"), p->done, p->total);
        NotificationCreateArgs args;
        args.hwndParent = p->hwnd;
        args.groupId = kNotifExtractToc;
        args.msg = msg;
        args.timeoutMs = kNotifNoTimeout;
        ShowNotification(args);
    }
    delete p;
}

static void ShowExtractProgress(HWND hwnd, int done, int total) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }
    ExtractProgressOnUi(new ExtractProgressUi{hwnd, done, total});
}

static void PostExtractProgress(HWND hwnd, int done, int total) {
    if (!hwnd) {
        return;
    }
    uitask::Post(MkFunc0(ExtractProgressOnUi, new ExtractProgressUi{hwnd, done, total}), "ExtractPdfTocProgress");
}

static bool CollectScanLineRange(EngineBase* engine, Vec<ScanLine>& lines, int* nText, int fromPage, int toPage,
                                 LONG cancelSeq, HWND hwndCanvas, int nPages) {
    for (int p = fromPage; p <= toPage; p++) {
        if (ExtractCancelled(cancelSeq)) {
            return false;
        }
        int before = lines.Size();
        CollectPageScanLines(engine, p, lines);
        if (lines.Size() > before && nText) {
            (*nText)++;
        }
        if (p == toPage || (p % 2) == 0) {
            PostExtractProgress(hwndCanvas, p, nPages);
        }
    }
    return true;
}

static void HideExtractProgress(HWND hwnd) {
    if (hwnd && IsWindow(hwnd)) {
        RemoveNotificationsForGroup(hwnd, kNotifExtractToc);
    }
}

static void ShowExtractDone(HWND hwnd, const char* msg, bool warning) {
    if (!hwnd || !IsWindow(hwnd) || !msg) {
        return;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwnd;
    args.groupId = kNotifExtractToc;
    args.msg = msg;
    args.warning = warning;
    args.timeoutMs = kNotif5SecsTimeOut;
    ShowNotification(args);
}

bool ConfirmReplaceExistingPdfToc(MainWindow* win) {
    TASKDIALOG_BUTTON buttons[2]{};
    buttons[0].nButtonID = IDYES;
    buttons[0].pszButtonText = ToWStrTemp(_TRA("Yes"));
    buttons[1].nButtonID = IDNO;
    buttons[1].pszButtonText = ToWStrTemp(_TRA("No"));
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = win->hwndFrame;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = ToWStrTemp(_TRA("Extract Table of Contents"));
    config.pszContent = ToWStrTemp(_TRA("Replace the existing PDF bookmarks with extracted headings?"));
    config.pszMainIcon = TD_WARNING_ICON;
    config.nDefaultButton = IDNO;
    config.cButtons = dimof(buttons);
    config.pButtons = buttons;
    int pressed = 0;
    HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    return hr == S_OK && pressed == IDYES;
}

static EngineBase* ExtractEngineForWin(MainWindow* win) {
    DisplayModel* dm = win && win->ctrl ? win->ctrl->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    return engine && EngineMupdfCanEditPdfToc(engine) ? engine : nullptr;
}

static void ExtractApplyOnUi(ExtractWork* w);

static void ExtractThread(ExtractWork* w) {
    EngineBase* engine = w->engine;
    int nPages = engine->PageCount();
    bool bornDigital = w->bornDigital;
    int front = ExtractFrontPageCap(bornDigital, nPages);
    int workTotal = bornDigital ? front : nPages;
    Vec<ScanLine> lines;
    int nText = 0;
    if (!CollectScanLineRange(engine, lines, &nText, 1, front, w->cancelSeq, w->hwndCanvas, workTotal)) {
        w->status = ExtractPdfTocStatus::Cancelled;
        w->nTextPages = nText;
        FreeScanLines(lines);
        uitask::Post(MkFunc0(ExtractApplyOnUi, w), "ExtractPdfTocDone");
        return;
    }
    int nItems = 0;
    bool force = w->skipConfirm && nText >= 1;
    ExtractPdfTocKind k =
        ExtractFromCollectedLines(engine, lines, nPages, nText, w->roots, &nItems, force, bornDigital);
    // Scans may hide the printed 目录 later, or need body clustering. A
    // born-digital textbook with Contents is finished after the front pages.
    if (k != ExtractPdfTocKind::Ok && k != ExtractPdfTocKind::NoText && !bornDigital && nPages > front) {
        DeleteExtractedTocItems(w->roots);
        w->roots.Reset();
        nItems = 0;
        if (!CollectScanLineRange(engine, lines, &nText, front + 1, nPages, w->cancelSeq, w->hwndCanvas, workTotal)) {
            w->status = ExtractPdfTocStatus::Cancelled;
            w->nTextPages = nText;
            FreeScanLines(lines);
            uitask::Post(MkFunc0(ExtractApplyOnUi, w), "ExtractPdfTocDone");
            return;
        }
        k = ExtractFromCollectedLines(engine, lines, nPages, nText, w->roots, &nItems, force, bornDigital);
    }
    w->nTextPages = nText;
    FreeScanLines(lines);
    if (k == ExtractPdfTocKind::NoText) {
        w->status = ExtractPdfTocStatus::NoText;
    } else if (k == ExtractPdfTocKind::Ok) {
        w->status = ExtractPdfTocStatus::Ok;
        w->nItems = nItems;
    } else {
        DeleteExtractedTocItems(w->roots);
        w->status = ExtractPdfTocStatus::NoHeadings;
    }
    uitask::Post(MkFunc0(ExtractApplyOnUi, w), "ExtractPdfTocDone");
}

static void ExtractApplyOnUi(ExtractWork* w) {
    InterlockedExchange(&gExtractRunning, 0);
    HideExtractProgress(w->hwndCanvas);
    MainWindow* win = FindMainWindowByHwnd(w->hwndCanvas);
    if (!win) {
        win = FindMainWindowByHwnd(w->hwndFrame);
    }
    EngineBase* live = ExtractEngineForWin(win);
    bool sameDoc = win && live == w->engine;
    if (w->status == ExtractPdfTocStatus::Cancelled) {
        if (sameDoc) {
            ShowExtractDone(w->hwndCanvas, _TRA("Bookmark extraction cancelled."), false);
        }
        DeleteExtractedTocItems(w->roots);
        str::Free(w->error);
        delete w;
        return;
    }
    if (!sameDoc) {
        DeleteExtractedTocItems(w->roots);
        str::Free(w->error);
        delete w;
        return;
    }
    if (w->status == ExtractPdfTocStatus::NoText) {
        bool autoAfter = w->skipConfirm;
        bool hasLayer = OcrDocumentHasFileTextLayer(w->engine);
        bool offer = !autoAfter && !hasLayer && !gPluginMode && OcrEngineKindSupported(w->engine);
        HWND canvas = w->hwndCanvas;
        DeleteExtractedTocItems(w->roots);
        delete w;
        if (hasLayer) {
            ShowExtractDone(canvas, _TRA("No headings found. Recognize the pages first, then try again."), true);
        } else if (offer) {
            OcrExtractTocAfterDocumentOcr(win);
        } else {
            const char* skip = _TRA("This file has no text layer. Recognize the pages first, then extract bookmarks.");
            ShowExtractDone(canvas, skip, true);
        }
        return;
    }
    if (w->status != ExtractPdfTocStatus::Ok) {
        const char* msg = w->bornDigital ? _TRA("No printed table of contents found in the first pages.")
                                         : _TRA("No headings found. Recognize the pages first, then try again.");
        ShowExtractDone(w->hwndCanvas, msg, true);
        DeleteExtractedTocItems(w->roots);
        delete w;
        return;
    }
    HWND canvas = w->hwndCanvas;
    // Extract always writes the outline. 对准印刷目录 is opened only by the
    // header button / CmdPdfTocCalibrate — not automatically after extract.
    if (TocCalibIsActive(win)) {
        WindowTab* tab = win->CurrentTab();
        CloseTocCalibForTab(tab);
        HideTocCalib(win);
    }
    bool ok = WriteExtractedPdfToc(win, w->engine, w->roots, w->persistToDisk);
    int nItems = w->nItems;
    DeleteExtractedTocItems(w->roots);
    if (!ok) {
        ShowExtractDone(canvas, _TRA("Could not write the PDF table of contents."), true);
    } else {
        ShowExtractDone(canvas, str::FormatTemp(_TRA("Extracted %d bookmarks."), nItems), false);
    }
    delete w;
}

bool WriteExtractedPdfToc(MainWindow* win, EngineBase* engine, Vec<ExtractedTocItem*>& roots, bool persistToDisk) {
    if (!win || !engine) {
        return false;
    }
    InheritMissingExtractedDests(roots);
    char* err = nullptr;
    bool ok = EngineMupdfReplacePdfToc(engine, roots, &err);
    if (!ok) {
        ShowExtractDone(win->hwndCanvas, err ? err : _TRA("Could not write the PDF table of contents."), true);
        str::Free(err);
        return false;
    }
    if (persistToDisk) {
        WindowTab* tab = win->CurrentTab();
        if (tab) {
            tab->ignoreNextAutoReload = true;
        }
        char* tmp = nullptr;
        bool saved = EngineMupdfSaveUpdated(engine, nullptr, {}, &tmp);
        if (saved) {
            engine->ClearUnsavedOcrText();
            const char* path = engine->FilePath();
            if (tmp) {
                SwitchCurrentTabToSavedFile(win, path, tmp);
                str::Free(tmp);
            } else {
                ReloadDocument(win, false);
            }
            SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
            ToolbarUpdateStateForWindow(win, false);
            return true;
        }
        if (tab) {
            tab->ignoreNextAutoReload = false;
        }
        logf("ExtractPdfToc: extracted bookmarks but saving the PDF failed\n");
        ShowExtractDone(win->hwndCanvas, _TRA("Could not write the PDF table of contents."), true);
        return false;
    }
    ReloadPdfTocTree(win);
    SetSidebarVisibility(win, true, gGlobalPrefs->showFavorites);
    ToolbarUpdateStateForWindow(win, false);
    return true;
}

static bool ConfirmPdfTocSignatureExtract(MainWindow* win, EngineBase* engine) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || tab->acceptedPdfTocSignatureWarning || !EngineMupdfPdfHasSignatures(engine)) {
        return true;
    }
    int res = MessageBoxW(win->hwndFrame,
                          L"Editing the table of contents changes this PDF after it was digitally signed. The "
                          "existing signature will remain, but viewers will report that the document was modified. "
                          "Continue?",
                          L"Digitally signed PDF", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (res != IDYES) {
        return false;
    }
    tab->acceptedPdfTocSignatureWarning = true;
    return true;
}

bool HandleExtractPdfTocCommand(MainWindow* win, bool skipConfirm, bool persistToDisk) {
    EngineBase* engine = ExtractEngineForWin(win);
    if (!engine || !win) {
        return true;
    }
    bool bornDigital = ExtractPdfLooksBornDigital(engine);
    int workTotal = ExtractProgressTotal(engine, bornDigital);
    if (ExtractPdfTocIsRunning()) {
        ShowExtractProgress(win->hwndCanvas, 0, workTotal);
        return true;
    }
    if (!skipConfirm && !ConfirmPdfTocSignatureExtract(win, engine)) {
        return true;
    }
    if (!skipConfirm && EngineMupdfHasStoredOutline(engine) && !ConfirmReplaceExistingPdfToc(win)) {
        return true;
    }
    InterlockedIncrement(&gExtractCancelSeq);
    auto* w = new ExtractWork;
    w->engine = engine;
    w->hwndCanvas = win->hwndCanvas;
    w->hwndFrame = win->hwndFrame;
    w->skipConfirm = skipConfirm;
    w->persistToDisk = persistToDisk;
    w->bornDigital = bornDigital;
    w->cancelSeq = InterlockedCompareExchange(&gExtractCancelSeq, 0, 0);
    InterlockedExchange(&gExtractRunning, 1);
    ShowExtractProgress(win->hwndCanvas, 0, workTotal);
    RunAsync(MkFunc0(ExtractThread, w), "ExtractPdfToc");
    return true;
}
