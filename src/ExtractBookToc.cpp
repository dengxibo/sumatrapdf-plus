/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"

#include "ExtractPdfToc.h"
#include "ExtractBookToc.h"

#include "utils/Log.h"

// Book TOC extraction is not administrative TOC extraction.
// Phase 1: recover the author's printed 目录, then bind printed page -> PDF page
// by searching those titles in the body. Do not invent 1.1 / 一、 hierarchy.
// Phase 2: the printed titles are ground truth; learn each book's heading style
// from their body occurrences, then only fill missing spines / fix destinations.
// Phase 4: keep OCR x-gaps (title vs right page), continue multi-page 目录,
// and cluster body styles when there is no printed TOC.

struct BookTocEntry {
    char* title = nullptr;
    int printedPage = 0;
    int pdfPage = 0;
    float x = 0;
    float y = 0;
    float srcX = 0;
    float srcY = 0;
    float indent = 0;
    float fontSize = 0;
    bool bold = false;
    int srcPage = 0;
    int inferredLevel = 1;
    float confidence = 0;
    float bodyFontSize = 0;
    float bodyDy = 0;
    float bodyGap = 0;
    bool bodyBold = false;
    bool bodyMatched = false;
    int source = 0; // 0 printed TOC, 1 style learner
    char* raw = nullptr;
    char* reason = nullptr;
};

struct BookLine {
    char* text = nullptr;
    int page = 0;
    float x = 0;
    float y = 0;
    float dx = 0;
    float dy = 0;
    float fontSize = 0;
    bool bold = false;
    bool used = false;
};

static char* BookDupTrim(const char* s) {
    char* d = str::Dup(s ? s : "");
    if (d) {
        str::TrimWSInPlace(d, str::TrimOpt::Both);
    }
    return d;
}

static bool BookIsDigit(int cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19);
}

static bool BookIsParenOpen(int cp) {
    return cp == '(' || cp == 0xFF08 || cp == 0xFE59 || cp == 0xFE35;
}

static bool BookIsParenClose(int cp) {
    return cp == ')' || cp == 0xFF09 || cp == 0xFE5A || cp == 0xFE36;
}

static int BookDigitVal(int cp) {
    if (cp >= '0' && cp <= '9') {
        return cp - '0';
    }
    if (cp >= 0xFF10 && cp <= 0xFF19) {
        return cp - 0xFF10;
    }
    return -1;
}

static bool BookIsLeader(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x00B7 || cp == 0x2026 || cp == 0x30FB || cp == 0x2500 || cp == 0x2014 ||
           cp == 0x2013 || cp == '_' || cp == '-' || cp == 0x3000 || cp == ' ' || cp == 0x2022 || cp == 0xFF0D ||
           cp == 0x2024 || cp == 0x2219 || cp == 0x00A8 || cp == 0xFF65 || cp == 0x22EF || cp == 0x2025;
}

static bool BookIsSlash(int cp) {
    return cp == '/' || cp == 0xFF0F || cp == 0x2215 || cp == 0x2044;
}

static int BookGlyphCount(const char* s) {
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

static void BookSkipWs(const char* s, int len, int& i) {
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 32 && cp != 0x3000) {
            i = save;
            return;
        }
    }
}

static bool BookHasLetterOrCjk(const char* s) {
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

static bool BookIsCnNumeral(int cp) {
    return cp == 0x4E00 || cp == 0x4E8C || cp == 0x4E09 || cp == 0x56DB || cp == 0x4E94 || cp == 0x516D ||
           cp == 0x4E03 || cp == 0x516B || cp == 0x4E5D || cp == 0x5341 || cp == 0x767E || cp == 0x5343;
}

static int BookCnNumeralVal(int cp) {
    if (cp == 0x4E00) {
        return 1;
    }
    if (cp == 0x4E8C) {
        return 2;
    }
    if (cp == 0x4E09) {
        return 3;
    }
    if (cp == 0x56DB) {
        return 4;
    }
    if (cp == 0x4E94) {
        return 5;
    }
    if (cp == 0x516D) {
        return 6;
    }
    if (cp == 0x4E03) {
        return 7;
    }
    if (cp == 0x516B) {
        return 8;
    }
    if (cp == 0x4E5D) {
        return 9;
    }
    if (cp == 0x5341) {
        return 10;
    }
    return 0;
}

static int BookParseCnOrDigitRun(const char* s, int len, int& i) {
    int n = 0;
    int got = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (BookIsDigit(cp)) {
            n = n * 10 + BookDigitVal(cp);
            got = 1;
            continue;
        }
        if (BookIsCnNumeral(cp)) {
            int v = BookCnNumeralVal(cp);
            if (v == 10) {
                n = n == 0 ? 10 : n + 10;
            } else {
                n = n * 10 + v;
            }
            got = 1;
            continue;
        }
        i = save;
        break;
    }
    return got ? n : 0;
}

static bool BookLooksLikeTocHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (str::ContainsI(s, "table of contents")) {
        return BookGlyphCount(s) <= 40;
    }
    if (str::ContainsI(s, "contents") && BookGlyphCount(s) <= 24) {
        return true;
    }
    char compact[192];
    int n = 0;
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len && n < (int)sizeof(compact) - 1) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000) {
            continue;
        }
        int add = i - save;
        if (n + add >= (int)sizeof(compact) - 1) {
            break;
        }
        memcpy(compact + n, s + save, (size_t)add);
        n += add;
    }
    compact[n] = 0;
    if (BookGlyphCount(compact) > 24) {
        return false;
    }
    return str::Find(compact, "\xE7\x9B\xAE\xE5\xBD\x95") != nullptr || // 目录
           str::Find(compact, "\xE7\x9B\xAE\xE6\xAC\xA1") != nullptr || // 目次
           str::Find(compact, "日录") != nullptr || str::Find(compact, "自录") != nullptr;
}

static bool BookLooksLikeCipPage(const Vec<ScanLine>& lines, int p) {
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

static bool BookLooksLikeJunk(const char* s) {
    if (!s || !s[0]) {
        return true;
    }
    if (str::Find(s, "ISBN") || str::Find(s, "www.") || str::Find(s, "http")) {
        return true;
    }
    return false;
}

static float BookMidY(const BookLine& sl) {
    return sl.y + sl.dy * 0.5f;
}

static float BookPageWidth(const Vec<BookLine>& page) {
    float w = 0;
    for (int i = 0; i < page.Size(); i++) {
        float r = page[i].x + page[i].dx;
        if (r > w) {
            w = r;
        }
    }
    if (w < 200) {
        w = 300;
    }
    return w;
}

static int BookMapPageOcrCp(int cp) {
    if (cp == 'O' || cp == 'o' || cp == 0xFF2F || cp == 0xFF4F) {
        return '0';
    }
    if (cp == 'I' || cp == 'l' || cp == '|' || cp == 0xFF29 || cp == 0xFF4C) {
        return '1';
    }
    return cp;
}

// Map OCR page tokens (1O5, 1 05) without gluing "32 ....41" into 3241.
static char* BookNormalizePageToken(const char* s) {
    if (!s || !s[0]) {
        return nullptr;
    }
    int len = (int)str::Len(s);
    char* out = AllocArray<char>(len + 1);
    if (!out) {
        return nullptr;
    }
    int n = 0;
    int i = 0;
    int prevDigit = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000) {
            continue;
        }
        int mapped = BookMapPageOcrCp(cp);
        if (BookIsDigit(mapped)) {
            out[n++] = (char)('0' + BookDigitVal(mapped));
            prevDigit = 1;
            continue;
        }
        if (BookIsLeader(cp) && !BookIsDigit(cp)) {
            if (prevDigit && n > 0 && n < len) {
                out[n++] = '.';
            }
            prevDigit = 0;
            continue;
        }
        if (cp == '(' || cp == 0xFF08 || cp == ')' || cp == 0xFF09) {
            continue;
        }
        str::Free(out);
        return nullptr;
    }
    out[n] = 0;
    return out;
}

// Right-column / isolated printed page number: 1, 25, (2), .·17, -77, ·..·124, 1O5
static int BookParseIsolatedPage(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    char* norm = BookNormalizePageToken(s);
    if (!norm || !norm[0]) {
        str::Free(norm);
        return 0;
    }
    int len = (int)str::Len(norm);
    int i = 0;
    int last = 0;
    int nDig = 0;
    int n = 0;
    while (i < len) {
        int cp = norm[i++];
        if (cp == '.') {
            if (nDig >= 1 && nDig <= 4) {
                last = n;
            }
            n = 0;
            nDig = 0;
            continue;
        }
        if (cp >= '0' && cp <= '9') {
            n = n * 10 + (cp - '0');
            nDig++;
            if (nDig > 4) {
                str::Free(norm);
                return 0;
            }
            continue;
        }
    }
    str::Free(norm);
    if (nDig >= 1 && nDig <= 4) {
        last = n;
    }
    return last;
}

static bool BookLineIsPageNum(const char* s) {
    return BookParseIsolatedPage(s) > 0;
}

static bool BookIsLeaderOnly(const char* s) {
    if (!s || !s[0]) {
        return true;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || BookIsLeader(cp)) {
            n++;
            continue;
        }
        return false;
    }
    return n > 0;
}

static int BookParsePageCandidate(const char* s, float x, float pageW) {
    if (!s || !s[0] || !BookLineIsPageNum(s)) {
        return 0;
    }
    int page = BookParseIsolatedPage(s);
    if (page < 1) {
        return 0;
    }
    // Isolated "(144)" is a printed page even on the left. Do not glue it
    // onto the title as 公文-style (1)(2) numbering.
    if (page >= 10 && !BookHasLetterOrCjk(s)) {
        return page;
    }
    if (x > pageW * 0.75f) {
        return page;
    }
    if (x > pageW * 0.55f && !BookHasLetterOrCjk(s)) {
        return page;
    }
    return 0;
}

static void BookBufCat(char* buf, int cap, const char* bit, const char* sep = nullptr) {
    if (!buf || cap < 2 || !bit || !bit[0]) {
        return;
    }
    int n = (int)str::Len(buf);
    if (n > 0 && sep && sep[0] && n + (int)str::Len(sep) < cap) {
        int sn = (int)str::Len(sep);
        memcpy(buf + n, sep, (size_t)sn);
        n += sn;
        buf[n] = 0;
    }
    int add = (int)str::Len(bit);
    if (n + add >= cap) {
        add = cap - 1 - n;
    }
    if (add > 0) {
        memcpy(buf + n, bit, (size_t)add);
        buf[n + add] = 0;
    }
}

static void BookReasonAdd(char* buf, int cap, const char* bit) {
    BookBufCat(buf, cap, bit, "; ");
}

// "第一章 xxx／1" / "心得一 ... /3" / "第一课热爱祖国··" with trailing leaders only.
static bool BookSplitInlinePage(const char* raw, char** titleOut, int* pageOut) {
    *titleOut = nullptr;
    *pageOut = 0;
    char* s = BookDupTrim(raw);
    if (!s || !s[0] || BookLooksLikeTocHeading(s)) {
        str::Free(s);
        return false;
    }
    int len = (int)str::Len(s);
    int scan = len;
    int last = Utf8CodepointPrev(s, len, scan);
    while (last > 0 && (last <= 32 || last == 0x3000 || last == '+')) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    bool closeParen = last == ')' || last == 0xFF09;
    if (closeParen) {
        last = Utf8CodepointPrev(s, len, scan);
        while (last > 0 && (last <= 32 || last == 0x3000)) {
            last = Utf8CodepointPrev(s, len, scan);
        }
    }
    int digitBuf[8];
    int nDigits = 0;
    while (last > 0 && BookIsDigit(last) && nDigits < 8) {
        digitBuf[nDigits++] = BookDigitVal(last);
        last = Utf8CodepointPrev(s, len, scan);
    }
    if (nDigits < 1 || nDigits > 4) {
        str::Free(s);
        return false;
    }
    int page = 0;
    for (int k = nDigits - 1; k >= 0; k--) {
        page = page * 10 + digitBuf[k];
    }
    while (last > 0 && (last <= 32 || last == 0x3000 || BookIsLeader(last) || BookIsSlash(last) || last == ')' ||
                        last == 0xFF09 || last == '(' || last == 0xFF08 || last == '+')) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    if (scan < 1) {
        str::Free(s);
        return false;
    }
    // scan is the start of the last kept title character; include that glyph.
    if (last > 0 && !BookIsDigit(last) && !BookIsLeader(last) && !BookIsSlash(last)) {
        int keep = scan;
        Utf8CodepointNext(s, len, keep);
        scan = keep;
    }
    if (scan < 1) {
        str::Free(s);
        return false;
    }
    s[scan] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    if (!BookHasLetterOrCjk(s) || BookGlyphCount(s) < 2) {
        str::Free(s);
        return false;
    }
    *titleOut = s;
    *pageOut = page;
    return true;
}

static bool BookStartsWithDi(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return cp == 0x7B2C; // 第
}

static bool BookIsDashSubtitleCp(int cp) {
    return cp == 0x2014 || cp == 0x2013 || cp == 0x2015 || cp == 0xFF0D || cp == '-' || cp == 0x2500;
}

static bool BookStartsWithDashSubtitle(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return BookIsDashSubtitleCp(cp) || cp == '~' || cp == 0xFF5E;
}

static void BookStripLeadingSubtitleDashInPlace(char* s) {
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp == '~' || cp == 0xFF5E || BookIsDashSubtitleCp(cp)) {
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

// "1．成绩册" / "2. 偏差值" — not "3.4 面上无光".
static int BookParseListPrefix(const char* s, int from, int* numberOut) {
    if (!s || from < 0) {
        return -1;
    }
    int len = (int)str::Len(s);
    int i = from;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp == 0x00B7 || cp == 0x2022 || cp == 0x30FB) {
        BookSkipWs(s, len, i);
    } else {
        // rewind if we consumed a non-bullet
        i = from;
        BookSkipWs(s, len, i);
    }
    int n = 0;
    int digits = 0;
    while (i < len) {
        int save = i;
        cp = Utf8CodepointNext(s, len, i);
        if (!BookIsDigit(cp)) {
            i = save;
            break;
        }
        n = n * 10 + BookDigitVal(cp);
        digits++;
        if (digits > 3) {
            return -1;
        }
    }
    if (digits < 1) {
        return -1;
    }
    BookSkipWs(s, len, i);
    if (i >= len) {
        return -1;
    }
    int save = i;
    cp = Utf8CodepointNext(s, len, i);
    if (cp != '.' && cp != 0xFF0E && cp != 0x3002 && cp != 0x3001) {
        return -1;
    }
    int peek = i;
    BookSkipWs(s, len, peek);
    int nx = peek < len ? Utf8CodepointNext(s, len, peek) : 0;
    if (BookIsDigit(nx)) {
        return -1;
    }
    if (numberOut) {
        *numberOut = n;
    }
    return i;
}

static bool BookStartsWithListNumber(const char* s) {
    return BookParseListPrefix(s, 0, nullptr) > 0;
}

// "一、准备" / "二、练习" — same sibling series as 1. 2. 3.
static int BookParseCnDunhaoPrefix(const char* s, int from, int* numberOut) {
    if (!s || from < 0) {
        return -1;
    }
    int len = (int)str::Len(s);
    int i = from;
    BookSkipWs(s, len, i);
    if (i >= len) {
        return -1;
    }
    int save = i;
    int cp = Utf8CodepointNext(s, len, i);
    if (!BookIsCnNumeral(cp)) {
        return -1;
    }
    i = save;
    int n = BookParseCnOrDigitRun(s, len, i);
    if (n < 1) {
        return -1;
    }
    BookSkipWs(s, len, i);
    if (i >= len) {
        return -1;
    }
    cp = Utf8CodepointNext(s, len, i);
    if (cp != 0x3001 && cp != 0xFF0C && cp != ',') {
        return -1;
    }
    if (numberOut) {
        *numberOut = n;
    }
    return i;
}

static int BookEntryScheme(const char* s, int* numOut) {
    if (numOut) {
        *numOut = 0;
    }
    int n = 0;
    if (BookParseListPrefix(s, 0, &n) > 0) {
        if (numOut) {
            *numOut = n;
        }
        return 35;
    }
    if (BookParseCnDunhaoPrefix(s, 0, &n) > 0) {
        if (numOut) {
            *numOut = n;
        }
        return 20;
    }
    return 0;
}

static int BookFindNextListOffset(const char* s, int from) {
    if (!s || from < 0) {
        return -1;
    }
    int len = (int)str::Len(s);
    int i = from;
    while (i < len) {
        int n = 0;
        int end = BookParseListPrefix(s, i, &n);
        if (end > i && i > 0) {
            return i;
        }
        Utf8CodepointNext(s, len, i);
    }
    return -1;
}

static int BookSecondListOffset(const char* s) {
    int firstEnd = BookParseListPrefix(s, 0, nullptr);
    int from = firstEnd > 0 ? firstEnd : 1;
    return BookFindNextListOffset(s, from);
}

enum class BookUnitKind {
    None = 0,
    Part = 1,    // 第X部分 / 篇 / 卷
    Chapter = 2, // 第X章 / 课 / 讲
    Section = 3  // 第X节
};

struct BookUnit {
    BookUnitKind kind = BookUnitKind::None;
    int number = 0;
    int prefixBytes = 0;
};

static BookUnit BookParseUnit(const char* s) {
    BookUnit u;
    if (!s || !s[0]) {
        return u;
    }
    if (str::StartsWithI(s, "part ")) {
        u.kind = BookUnitKind::Part;
        u.number = 1;
        u.prefixBytes = 5;
        return u;
    }
    if (str::StartsWithI(s, "chapter")) {
        u.kind = BookUnitKind::Chapter;
        u.number = 1;
        u.prefixBytes = 7;
        return u;
    }
    if (str::StartsWithI(s, "section")) {
        u.kind = BookUnitKind::Section;
        u.number = 1;
        u.prefixBytes = 7;
        return u;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp == 0x5E8F || cp == 0x672B) { // 序 末
        int n = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (n == 0x7AE0) { // 章
            u.kind = BookUnitKind::Chapter;
            u.number = cp == 0x5E8F ? 0 : 99;
            u.prefixBytes = i;
        }
        return u;
    }
    if (cp != 0x7B2C) {
        return u;
    }
    BookSkipWs(s, len, i);
    int n = BookParseCnOrDigitRun(s, len, i);
    if (n < 1) {
        return u;
    }
    BookSkipWs(s, len, i);
    int mark = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (mark == 0x90E8) { // 部（部分 / 部份）
        u.kind = BookUnitKind::Part;
        u.number = n;
        u.prefixBytes = i;
        return u;
    }
    if (mark == 0x7BC7 || mark == 0x5377) { // 篇 卷
        u.kind = BookUnitKind::Part;
        u.number = n;
        u.prefixBytes = i;
        return u;
    }
    if (mark == 0x7AE0 || mark == 0x7AE5 || mark == 0x8BB2 || mark == 0x8BFE) { // 章 童 讲 课
        u.kind = BookUnitKind::Chapter;
        u.number = n;
        u.prefixBytes = i;
        return u;
    }
    if (mark == 0x8282 || mark == 0x7BC0) { // 节 節
        u.kind = BookUnitKind::Section;
        u.number = n;
        u.prefixBytes = i;
        return u;
    }
    return u;
}

static bool BookIsChapterUnit(const char* s) {
    return BookParseUnit(s).kind == BookUnitKind::Chapter;
}

static bool BookIsStructTitle(const char* s) {
    return BookParseUnit(s).kind != BookUnitKind::None;
}

// Chapter-level spine: 第X章 / 课 / 讲. Not 节, not 部分.
static bool BookIsSpineTitle(const char* s) {
    return BookIsChapterUnit(s);
}

static int BookFindNextUnitOffset(const char* s, int from) {
    if (!s || from < 0) {
        return -1;
    }
    int len = (int)str::Len(s);
    int i = from;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp != 0x7B2C) {
            continue;
        }
        BookUnit u = BookParseUnit(s + save);
        if (u.kind != BookUnitKind::None && save > 0) {
            return save;
        }
    }
    return -1;
}

static int BookSecondUnitOffset(const char* s) {
    BookUnit first = BookParseUnit(s);
    int from = first.prefixBytes > 0 ? first.prefixBytes : 1;
    return BookFindNextUnitOffset(s, from);
}

static int BookStructOutlineLevel(BookUnitKind kind, bool hasPart, bool hasChap) {
    if (kind == BookUnitKind::Part) {
        return 1;
    }
    if (kind == BookUnitKind::Chapter) {
        return hasPart ? 2 : 1;
    }
    if (kind == BookUnitKind::Section) {
        if (hasPart) {
            return 3;
        }
        return hasChap ? 2 : 1;
    }
    return 0;
}

static bool BookIsXinDe(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int c1 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    int c2 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (c1 != 0x5FC3 || c2 != 0x5F97) {
        return false;
    }
    BookSkipWs(s, len, i);
    int n = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return BookIsCnNumeral(n) || BookIsDigit(n) || n == '(' || n == 0xFF08 || n == 0xFF0D || n == 0x2013 ||
           n == 0x2014 || n == 0x2015 || n == '-';
}

static bool BookIsPartTitle(const char* s) {
    if (!s || !s[0] || BookLooksLikeTocHeading(s) || BookIsXinDe(s) || BookIsStructTitle(s)) {
        return false;
    }
    int g = BookGlyphCount(s);
    if (g < 4 || g > 16) {
        return false;
    }
    if (!BookHasLetterOrCjk(s)) {
        return false;
    }
    if (str::Find(s, "，") || str::Find(s, "。") || str::Find(s, "？") || str::Find(s, "?")) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (BookIsDigit(cp) || BookIsCnNumeral(cp) || BookStartsWithDi(s)) {
        return false;
    }
    return true;
}

// "——家长的旧观念拖孩子后腿" under 序章 / 第一章 — merge into the chapter title.
static bool BookLooksLikeChapSubtitle(const char* s) {
    if (!s || !s[0] || BookLooksLikeTocHeading(s) || BookIsXinDe(s) || BookIsStructTitle(s) ||
        BookStartsWithListNumber(s) || BookEntryScheme(s, nullptr) > 0) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp == '~' || cp == 0xFF5E || BookIsDashSubtitleCp(cp)) {
            continue;
        }
        i = save;
        break;
    }
    const char* t = s + i;
    if (!t[0]) {
        return false;
    }
    int g = BookGlyphCount(t);
    if (g < 4 || g > 18) {
        return false;
    }
    if (!BookHasLetterOrCjk(t)) {
        return false;
    }
    if (str::Find(t, "，") || str::Find(t, "。") || str::Find(t, "？") || str::Find(t, "?") || str::Find(t, "！") ||
        str::Find(t, "是错误的") || str::StartsWith(t, "认为")) {
        return false;
    }
    return true;
}

static bool BookLooksLikeBodyBlurb(const char* s) {
    if (!s) {
        return false;
    }
    // Numbered TOC rows are often full sentences: "4．电视中的“知心老师”不能说是最好的老师。"
    if (BookIsStructTitle(s) || BookIsXinDe(s) || BookStartsWithListNumber(s)) {
        return false;
    }
    int g = BookGlyphCount(s);
    if (str::Find(s, "。")) {
        return true;
    }
    if (g > 22 && str::Find(s, "，")) {
        return true;
    }
    if (str::Find(s, "以前听") || str::Find(s, "表现在哪里")) {
        return true;
    }
    return false;
}

static void BookStripTrailingTocStop(char* s) {
    if (!s || !s[0] || !BookStartsWithListNumber(s)) {
        return;
    }
    int len = (int)str::Len(s);
    int end = len;
    int cp = Utf8CodepointPrev(s, len, end);
    if (cp != 0x3002) {
        return;
    }
    s[end] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Both);
}

static bool BookIsDotLeader(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x00B7 || cp == 0x2026 || cp == 0x30FB || cp == 0x2022 || cp == 0x2024 ||
           cp == 0x2219 || cp == 0x00A8 || cp == 0xFF65 || cp == 0x22EF || cp == 0x2025 || cp == '_';
}

static void BookStripLeadersInPlace(char* s) {
    if (!s) {
        return;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        // Keep a leading em-dash; that is a chapter subtitle, not a leader.
        if (cp <= 32 || cp == 0x3000 || BookIsDotLeader(cp)) {
            continue;
        }
        i = save;
        break;
    }
    int end = len;
    while (end > i) {
        int prev = end;
        int cp = Utf8CodepointPrev(s, len, prev);
        if (cp <= 32 || cp == 0x3000 || BookIsLeader(cp)) {
            end = prev;
            continue;
        }
        break;
    }
    if (i > 0 || end < len) {
        int n = end - i;
        if (n < 1) {
            s[0] = 0;
            return;
        }
        memmove(s, s + i, (size_t)n);
        s[n] = 0;
    }
    str::TrimWSInPlace(s, str::TrimOpt::Both);
}

static char* BookJoinText(const char* a, const char* b, const char* sep) {
    if (!a || !a[0]) {
        return BookDupTrim(b);
    }
    if (!b || !b[0]) {
        return BookDupTrim(a);
    }
    return str::Join(a, sep ? sep : "", b);
}

static void BookFreeLines(Vec<BookLine>& v) {
    for (int i = 0; i < v.Size(); i++) {
        str::Free(v[i].text);
        v[i].text = nullptr;
    }
    v.Reset();
}

static void BookFreeEntries(Vec<BookTocEntry>& v) {
    for (int i = 0; i < v.Size(); i++) {
        str::Free(v[i].title);
        str::Free(v[i].raw);
        str::Free(v[i].reason);
        v[i].title = nullptr;
        v[i].raw = nullptr;
        v[i].reason = nullptr;
    }
    v.Reset();
}

static void BookSortVisual(Vec<BookLine>& v) {
    for (int i = 0; i < v.Size(); i++) {
        for (int j = i + 1; j < v.Size(); j++) {
            float yi = BookMidY(v[i]);
            float yj = BookMidY(v[j]);
            float dyAbs = yj - yi;
            if (dyAbs < 0) {
                dyAbs = -dyAbs;
            }
            if (yj < yi - 0.5f || (dyAbs <= 0.5f && v[j].x < v[i].x)) {
                BookLine t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
}

static bool BookSameRow(const BookLine& a, const BookLine& b) {
    float dy = a.dy > b.dy ? a.dy : b.dy;
    float tol = dy * 0.65f;
    if (tol < 10) {
        tol = 10;
    }
    float d = BookMidY(a) - BookMidY(b);
    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

static void BookJoinLine(BookLine& acc, const BookLine& add) {
    char* neu = BookJoinText(acc.text, add.text, "");
    str::Free(acc.text);
    acc.text = neu;
    float r = acc.x + acc.dx;
    float r2 = add.x + add.dx;
    if (add.x < acc.x) {
        acc.x = add.x;
    }
    acc.dx = (r2 > r ? r2 : r) - acc.x;
    if (add.y < acc.y) {
        acc.y = add.y;
    }
    if (add.dy > acc.dy) {
        acc.dy = add.dy;
    }
    if (add.fontSize > acc.fontSize) {
        acc.fontSize = add.fontSize;
    }
    acc.bold = acc.bold || add.bold;
}

static void BookMergeSameRow(Vec<BookLine>& page) {
    BookSortVisual(page);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < page.Size(); i++) {
            if (!page[i].text) {
                continue;
            }
            for (int j = i + 1; j < page.Size(); j++) {
                if (!page[j].text || !BookSameRow(page[i], page[j])) {
                    continue;
                }
                bool iNum = BookLineIsPageNum(page[i].text);
                bool jNum = BookLineIsPageNum(page[j].text);
                float gap = page[j].x - (page[i].x + page[i].dx);
                if (page[i].x > page[j].x) {
                    gap = page[i].x - (page[j].x + page[j].dx);
                }
                // Stacked left-column titles overlap in x (gap << 0). Only join
                // sideways neighbors on the same visual row.
                if (gap < -8) {
                    continue;
                }
                if (BookIsStructTitle(page[i].text) && BookIsStructTitle(page[j].text)) {
                    continue;
                }
                if (iNum && jNum && gap < 20) {
                    float mid = BookMidY(page[i]) - BookMidY(page[j]);
                    if (mid < 0) {
                        mid = -mid;
                    }
                    if (mid > 6) {
                        continue;
                    }
                    if (page[j].x < page[i].x) {
                        BookJoinLine(page[j], page[i]);
                        str::Free(page[i].text);
                        page[i].text = nullptr;
                    } else {
                        BookJoinLine(page[i], page[j]);
                        str::Free(page[j].text);
                        page[j].text = nullptr;
                    }
                    changed = true;
                    break;
                }
                if (!iNum && !jNum && gap < 28) {
                    if (page[j].x < page[i].x) {
                        BookJoinLine(page[j], page[i]);
                        str::Free(page[i].text);
                        page[i].text = nullptr;
                    } else {
                        BookJoinLine(page[i], page[j]);
                        str::Free(page[j].text);
                        page[j].text = nullptr;
                    }
                    changed = true;
                    break;
                }
            }
            if (changed) {
                break;
            }
        }
        if (changed) {
            Vec<BookLine> compact;
            for (int i = 0; i < page.Size(); i++) {
                if (page[i].text) {
                    compact.Append(page[i]);
                    page[i].text = nullptr;
                }
            }
            page.Reset();
            for (int i = 0; i < compact.Size(); i++) {
                page.Append(compact[i]);
                compact[i].text = nullptr;
            }
            BookSortVisual(page);
        }
    }
}

static void BookCollectPage(const Vec<ScanLine>& lines, int p, Vec<BookLine>& page) {
    BookFreeLines(page);
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage != p || !lines[i].text) {
            continue;
        }
        BookLine sl;
        sl.text = str::Dup(lines[i].text);
        sl.page = p;
        sl.x = lines[i].x;
        sl.y = lines[i].y;
        sl.dx = lines[i].dx > 1 ? lines[i].dx : 8;
        sl.dy = lines[i].dy > 1 ? lines[i].dy : 10;
        sl.fontSize = lines[i].fontSize;
        sl.bold = lines[i].bold;
        page.Append(sl);
    }
    BookMergeSameRow(page);
}

static int BookScoreTocPage(const Vec<ScanLine>& lines, int p) {
    if (BookLooksLikeCipPage(lines, p)) {
        return 0;
    }
    Vec<BookLine> page;
    BookCollectPage(lines, p, page);
    float pageW = BookPageWidth(page);
    int score = 0;
    int nRight = 0;
    int nShort = 0;
    int nLeader = 0;
    int nStruct = 0;
    bool heading = false;
    for (int i = 0; i < page.Size(); i++) {
        const char* s = page[i].text;
        if (!s) {
            continue;
        }
        if (BookLooksLikeTocHeading(s)) {
            score += 50;
            heading = true;
            continue;
        }
        if (BookIsStructTitle(s) || BookStartsWithListNumber(s)) {
            nStruct++;
        }
        int printed = BookParseIsolatedPage(s);
        bool right = page[i].x > pageW * 0.70f;
        if (printed > 0 && (right || BookLineIsPageNum(s))) {
            nRight++;
            continue;
        }
        char* title = nullptr;
        int pg = 0;
        if (BookSplitInlinePage(s, &title, &pg)) {
            nRight++;
            nShort++;
            str::Free(title);
            continue;
        }
        int g = BookGlyphCount(s);
        if (g >= 2 && g <= 40 && BookHasLetterOrCjk(s)) {
            nShort++;
        }
        if (str::Find(s, "......") || str::Find(s, "\xE2\x80\xA6") || str::Find(s, "····")) {
            nLeader++;
        }
    }
    BookFreeLines(page);
    score += nRight * 6;
    score += nShort;
    score += nLeader * 2;
    if (nStruct >= 1 && nRight >= 1) {
        score += 16;
    }
    if (heading && nRight >= 2) {
        score += 20;
    }
    if (!heading && nRight < 3 && nStruct < 1) {
        score /= 2;
    }
    return score;
}

static bool BookFindTocRange(const Vec<ScanLine>& lines, int nPages, int* startOut, int* endOut) {
    int front = nPages < 40 ? nPages : 40;
    int start = 0;
    int headingPage = 0;
    int bestScore = 0;
    int bestPage = 0;
    for (int p = 1; p <= front; p++) {
        if (BookLooksLikeCipPage(lines, p)) {
            continue;
        }
        bool head = false;
        for (int i = 0; i < lines.Size(); i++) {
            if (lines[i].srcPage == p && BookLooksLikeTocHeading(lines[i].text)) {
                head = true;
                break;
            }
        }
        int sc = BookScoreTocPage(lines, p);
        if (head && headingPage < 1) {
            headingPage = p;
        }
        if (sc > bestScore) {
            bestScore = sc;
            bestPage = p;
        }
    }
    if (headingPage > 0) {
        start = headingPage;
    } else if (bestScore >= 16 && bestPage > 0) {
        start = bestPage;
    }
    if (start < 1) {
        return false;
    }
    int end = start;
    int empty = 0;
    int lim = start + 48;
    if (lim > nPages) {
        lim = nPages;
    }
    for (int p = start; p <= lim; p++) {
        if (BookLooksLikeCipPage(lines, p)) {
            continue;
        }
        int sc = BookScoreTocPage(lines, p);
        bool head = false;
        for (int i = 0; i < lines.Size(); i++) {
            if (lines[i].srcPage == p && BookLooksLikeTocHeading(lines[i].text)) {
                head = true;
                break;
            }
        }
        if (sc >= 12 || head || (p == start)) {
            end = p;
            empty = 0;
        } else if (p > start) {
            empty++;
            if (empty >= 3) {
                bool more = false;
                int peekLim = p + 2;
                if (peekLim > lim) {
                    peekLim = lim;
                }
                for (int q = p + 1; q <= peekLim; q++) {
                    if (BookLooksLikeCipPage(lines, q)) {
                        continue;
                    }
                    if (BookScoreTocPage(lines, q) >= 12) {
                        more = true;
                        break;
                    }
                }
                if (!more) {
                    break;
                }
                empty = 1;
            }
        }
    }
    *startOut = start;
    *endOut = end;
    return true;
}

static void BookAppendEntry(Vec<BookTocEntry>& hits, const char* rawTitle, int printed, const BookLine& sl,
                            const char* rawLine = nullptr, const char* reason = nullptr, float conf = -1.f) {
    char* title = BookDupTrim(rawTitle);
    if (!title) {
        return;
    }
    BookStripLeadersInPlace(title);
    BookStripTrailingTocStop(title);
    int fromTitle = StripBookPrintedPageFromTitle(title);
    if (printed < 1 && fromTitle > 0) {
        printed = fromTitle;
    }
    str::TrimWSInPlace(title, str::TrimOpt::Both);
    if (!title[0] || BookLooksLikeTocHeading(title) || BookLooksLikeJunk(title) || BookLooksLikeBodyBlurb(title) ||
        BookLineIsPageNum(title)) {
        str::Free(title);
        return;
    }
    if (BookGlyphCount(title) < 2) {
        str::Free(title);
        return;
    }
    BookTocEntry h;
    h.title = title;
    h.printedPage = printed;
    h.srcPage = sl.page;
    h.srcX = sl.x;
    h.srcY = sl.y;
    h.indent = sl.x;
    h.fontSize = sl.fontSize;
    h.bold = sl.bold;
    h.x = sl.x;
    h.y = sl.y;
    h.raw = BookDupTrim(rawLine);
    h.reason = BookDupTrim(reason);
    if (conf >= 0) {
        h.confidence = conf;
    } else {
        h.confidence = printed > 0 ? 0.85f : 0.65f;
    }
    hits.Append(h);
}

static bool BookTitleNeedsWrap(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int scan = len;
    int last = Utf8CodepointPrev(s, len, scan);
    while (last > 0 && (last <= 32 || last == 0x3000 || BookIsLeader(last))) {
        last = Utf8CodepointPrev(s, len, scan);
    }
    if (last == 0xFF0C || last == ',' || last == 0xFF1F || last == '?' || last == 0x3001 || last == 0xFF1A ||
        last == 0xFF01 || last == '!' || last == 0x5417 || last == 0x5462) {
        return true;
    }
    // bare 第N课 / 第N章
    if (BookIsSpineTitle(s) && BookGlyphCount(s) <= 5) {
        return true;
    }
    return false;
}

static bool BookIsWrapFragment(const char* s) {
    if (!s || BookIsStructTitle(s) || BookIsXinDe(s) || BookStartsWithDi(s)) {
        return false;
    }
    int g = BookGlyphCount(s);
    if (g < 2 || g > 18) {
        return false;
    }
    return BookHasLetterOrCjk(s);
}

struct BookTocRow {
    char* raw = nullptr;
    char* title = nullptr;
    int printedPage = 0;
    int page = 0;
    float x = 0;
    float y = 0;
    float dx = 0;
    float dy = 0;
    float fontSize = 0;
    bool bold = false;
    bool hasLeader = false;
    bool hasRightPage = false;
    bool keepDashSubtitle = false;
    float confidence = 0.5f;
    char reason[192]{};
};

static void BookFreeRows(Vec<BookTocRow>& rows) {
    for (int i = 0; i < rows.Size(); i++) {
        str::Free(rows[i].raw);
        str::Free(rows[i].title);
        rows[i].raw = nullptr;
        rows[i].title = nullptr;
    }
    rows.Reset();
}

static bool BookTitleNeedsContinuation(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (BookTitleNeedsWrap(s)) {
        return true;
    }
    if (BookIsSpineTitle(s) && BookGlyphCount(s) <= 5) {
        return true;
    }
    return str::Find(s, "《") && !str::Find(s, "》");
}

static bool BookIsDotLeaderCp(int cp) {
    return cp == '.' || cp == 0xFF0E || cp == 0x00B7 || cp == 0x2026 || cp == 0x30FB || cp == 0x2022 || cp == 0x2024 ||
           cp == 0x2025 || cp == 0x22EF || cp == 0xFF65 || cp == 0x2500;
}

static bool BookRangeHasLetterOrCjk(const char* s, int from, int to) {
    if (!s || from >= to) {
        return false;
    }
    int i = from;
    while (i < to) {
        int cp = Utf8CodepointNext(s, to, i);
        if (BookIsDigit(cp) || BookIsLeader(cp) || cp <= 32) {
            continue;
        }
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp >= 0x80) {
            return true;
        }
    }
    return false;
}

static int BookFindDashSplitOffset(const char* s) {
    if (!s || !s[0]) {
        return -1;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!BookIsDashSubtitleCp(cp) || save < 3) {
            continue;
        }
        while (i < len) {
            int hold = i;
            int n = Utf8CodepointNext(s, len, i);
            if (!BookIsDashSubtitleCp(n)) {
                i = hold;
                break;
            }
        }
        if (BookRangeHasLetterOrCjk(s, 0, save) && BookRangeHasLetterOrCjk(s, i, len)) {
            return save;
        }
    }
    return -1;
}

static bool BookLeftEndsWithDi(const char* s, int end) {
    if (!s || end < 1) {
        return false;
    }
    int i = end;
    int cp = Utf8CodepointPrev(s, end, i);
    return cp == 0x7B2C; // 第
}

static bool BookRightStartsWithUnitWord(const char* s, int from, int len) {
    if (!s || from >= len) {
        return false;
    }
    int i = from;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return cp == 0x8BFE || cp == 0x7AE0 || cp == 0x8282 || cp == 0x8BB2; // 课 章 节 讲
}

// Isolated 1-3 digit printed page, not "第18课" and not "1.1".
static bool BookTakeBarePage(const char* s, int len, int i, int* pageOut, int* afterOut) {
    BookSkipWs(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!BookIsDotLeaderCp(cp)) {
            i = save;
            break;
        }
    }
    if (i >= len) {
        return false;
    }
    int save = i;
    int cp = Utf8CodepointNext(s, len, i);
    if (!BookIsDigit(cp)) {
        return false;
    }
    int page = BookDigitVal(cp);
    int n = 1;
    while (i < len) {
        save = i;
        cp = Utf8CodepointNext(s, len, i);
        if (!BookIsDigit(cp)) {
            i = save;
            break;
        }
        page = page * 10 + BookDigitVal(cp);
        n++;
        if (n > 3) {
            return false;
        }
    }
    if (n < 1 || page < 1 || page > 400) {
        return false;
    }
    if (i < len) {
        save = i;
        cp = Utf8CodepointNext(s, len, i);
        i = save;
        if (cp == '.' || cp == 0xFF0E) {
            int j = save;
            Utf8CodepointNext(s, len, j);
            int next = j < len ? Utf8CodepointNext(s, len, j) : 0;
            if (BookIsDigit(next)) {
                return false;
            }
        }
    }
    BookSkipWs(s, len, i);
    while (i < len) {
        save = i;
        cp = Utf8CodepointNext(s, len, i);
        if (!BookIsDotLeaderCp(cp)) {
            i = save;
            break;
        }
    }
    *pageOut = page;
    *afterOut = i;
    return true;
}

static bool BookTakeParenPage(const char* s, int len, int i, int* pageOut, int* afterOut) {
    BookSkipWs(s, len, i);
    if (i >= len) {
        return false;
    }
    int save = i;
    int cp = Utf8CodepointNext(s, len, i);
    if (cp != '(' && cp != 0xFF08) {
        i = save;
        int n = 0;
        int page = 0;
        while (i < len) {
            save = i;
            cp = Utf8CodepointNext(s, len, i);
            if (!BookIsDigit(cp)) {
                i = save;
                break;
            }
            page = page * 10 + BookDigitVal(cp);
            n++;
            if (n > 4) {
                return false;
            }
        }
        if (n < 1 || page < 1) {
            return false;
        }
        *pageOut = page;
        *afterOut = i;
        return true;
    }
    int n = 0;
    int page = 0;
    while (i < len) {
        save = i;
        cp = Utf8CodepointNext(s, len, i);
        if (BookIsDigit(cp)) {
            page = page * 10 + BookDigitVal(cp);
            n++;
            if (n > 4) {
                return false;
            }
            continue;
        }
        if (cp == ')' || cp == 0xFF09) {
            if (n < 1 || page < 1) {
                return false;
            }
            *pageOut = page;
            *afterOut = i;
            return true;
        }
        return false;
    }
    return false;
}

// One printed TOC line glued to the next: "章题......小节" or "小节......(7)下一小节".
static bool BookSplitGluedTocLine(const char* s, char** leftOut, int* pageOut, char** rightOut) {
    *leftOut = nullptr;
    *rightOut = nullptr;
    *pageOut = 0;
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        int page = 0;
        int after = 0;
        if (save > 0 && BookTakeBarePage(s, len, save, &page, &after) && BookRangeHasLetterOrCjk(s, 0, save) &&
            BookRangeHasLetterOrCjk(s, after, len) && !BookLeftEndsWithDi(s, save) &&
            !BookRightStartsWithUnitWord(s, after, len)) {
            char* left = (char*)memdup(s, (size_t)save, 1);
            if (!left) {
                return false;
            }
            left[save] = 0;
            str::TrimWSInPlace(left, str::TrimOpt::Both);
            BookStripLeadersInPlace(left);
            char* right = BookDupTrim(s + after);
            if (!left[0] || !right || !right[0]) {
                str::Free(left);
                str::Free(right);
                return false;
            }
            *leftOut = left;
            *rightOut = right;
            *pageOut = page;
            return true;
        }
        if ((cp == '(' || cp == 0xFF08) && save > 0 && BookTakeParenPage(s, len, save, &page, &after) &&
            BookRangeHasLetterOrCjk(s, 0, save) && BookRangeHasLetterOrCjk(s, after, len)) {
            char* left = (char*)memdup(s, (size_t)save, 1);
            if (!left) {
                return false;
            }
            left[save] = 0;
            str::TrimWSInPlace(left, str::TrimOpt::Both);
            BookStripLeadersInPlace(left);
            char* right = BookDupTrim(s + after);
            if (!left[0] || !right || !right[0]) {
                str::Free(left);
                str::Free(right);
                return false;
            }
            *leftOut = left;
            *rightOut = right;
            *pageOut = page;
            return true;
        }
        i = save;
        int nLead = 0;
        while (i < len) {
            save = i;
            cp = Utf8CodepointNext(s, len, i);
            if (!BookIsDotLeaderCp(cp)) {
                i = save;
                break;
            }
            nLead++;
        }
        if (nLead >= 2 && save > 0) {
            page = 0;
            after = i;
            BookTakeParenPage(s, len, i, &page, &after);
            if (BookRangeHasLetterOrCjk(s, 0, save) && BookRangeHasLetterOrCjk(s, after, len)) {
                char* left = (char*)memdup(s, (size_t)save, 1);
                if (!left) {
                    return false;
                }
                left[save] = 0;
                str::TrimWSInPlace(left, str::TrimOpt::Both);
                BookStripLeadersInPlace(left);
                char* right = BookDupTrim(s + after);
                if (!left[0] || !right || !right[0]) {
                    str::Free(left);
                    str::Free(right);
                    return false;
                }
                *leftOut = left;
                *rightOut = right;
                *pageOut = page;
                return true;
            }
        }
        if (nLead < 1) {
            Utf8CodepointNext(s, len, i);
        }
    }
    return false;
}

static bool BookIsTitleJunkCp(int cp) {
    return cp == '?' || cp == '*' || cp == 0xFF1F || BookIsDotLeaderCp(cp);
}

static int BookStripLeadingParenPage(char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000 || BookIsLeader(cp)) {
            continue;
        }
        i = save;
        break;
    }
    if (i >= len) {
        return 0;
    }
    int cp = Utf8CodepointNext(s, len, i);
    if (!BookIsParenOpen(cp)) {
        return 0;
    }
    BookSkipWs(s, len, i);
    int n = 0;
    int nDig = 0;
    while (i < len && nDig < 5) {
        int at = i;
        cp = Utf8CodepointNext(s, len, i);
        if (!BookIsDigit(cp)) {
            i = at;
            break;
        }
        n = n * 10 + BookDigitVal(cp);
        nDig++;
    }
    if (nDig < 2 || nDig > 4 || n < 10) {
        return 0;
    }
    BookSkipWs(s, len, i);
    if (i >= len) {
        return 0;
    }
    cp = Utf8CodepointNext(s, len, i);
    if (!BookIsParenClose(cp)) {
        return 0;
    }
    BookSkipWs(s, len, i);
    if (!BookRangeHasLetterOrCjk(s, i, len)) {
        return 0;
    }
    if (i > 0) {
        memmove(s, s + i, (size_t)(len - i + 1));
        str::TrimWSInPlace(s, str::TrimOpt::Both);
    }
    return n;
}

static int BookStripTrailingParenPage(char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int end = len;
    int cp = Utf8CodepointPrev(s, len, end);
    while (cp > 0 && (cp <= 32 || cp == 0x3000 || BookIsLeader(cp))) {
        cp = Utf8CodepointPrev(s, len, end);
    }
    if (!BookIsParenClose(cp)) {
        return 0;
    }
    int n = 0;
    int nDig = 0;
    int place = 1;
    cp = Utf8CodepointPrev(s, len, end);
    while (cp > 0 && BookIsDigit(cp) && nDig < 5) {
        n += BookDigitVal(cp) * place;
        place *= 10;
        nDig++;
        cp = Utf8CodepointPrev(s, len, end);
    }
    if (nDig < 2 || nDig > 4 || n < 10) {
        return 0;
    }
    while (cp > 0 && (cp <= 32 || cp == 0x3000)) {
        cp = Utf8CodepointPrev(s, len, end);
    }
    if (!BookIsParenOpen(cp)) {
        return 0;
    }
    while (end > 0) {
        int save = end;
        int prev = Utf8CodepointPrev(s, len, save);
        if (prev <= 32 || prev == 0x3000 || BookIsLeader(prev)) {
            end = save;
            continue;
        }
        break;
    }
    if (end < 1 || !BookRangeHasLetterOrCjk(s, 0, end)) {
        return 0;
    }
    s[end] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Both);
    return n;
}

int StripBookPrintedPageFromTitle(char* title) {
    if (!title || !title[0]) {
        return 0;
    }
    int page = 0;
    for (int k = 0; k < 3; k++) {
        int lead = BookStripLeadingParenPage(title);
        int tail = BookStripTrailingParenPage(title);
        if (lead < 1 && tail < 1) {
            break;
        }
        if (lead > 0) {
            page = lead;
        }
        if (tail > 0) {
            page = tail;
        }
    }
    return page;
}

// "1819 宪法…" / "23 我国宪法…" / "(144) 不适于…" — keep list "1．成绩册" and 公文 "(2)条款".
static int BookStripLeadingPrintedPrefix(char* s) {
    if (!s || !s[0] || BookStartsWithListNumber(s)) {
        return 0;
    }
    int parenPage = StripBookPrintedPageFromTitle(s);
    if (parenPage > 0) {
        return parenPage;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp > 32 && cp != 0x3000 && !BookIsTitleJunkCp(cp)) {
            i = save;
            break;
        }
    }
    if (i >= len || s[i] < '0' || s[i] > '9') {
        return 0;
    }
    int nDig = 0;
    int page = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9' && nDig < 5) {
        page = page * 10 + (s[i] - '0');
        nDig++;
        i++;
    }
    if (nDig < 1 || nDig > 4) {
        return 0;
    }
    if (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        i = save;
        if (cp == '.' || cp == 0xFF0E || cp == 0x3001 || cp == 0xFF0C || cp == ',') {
            return 0;
        }
    }
    BookSkipWs(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!BookIsTitleJunkCp(cp) && cp > 32 && cp != 0x3000) {
            i = save;
            break;
        }
    }
    if (!BookRangeHasLetterOrCjk(s, i, len)) {
        return 0;
    }
    if (nDig == 4) {
        page = page % 100;
    }
    if (page < 1 || page > 400) {
        return 0;
    }
    if (i > 0) {
        int left = len - i;
        memmove(s, s + i, (size_t)left + 1);
        str::TrimWSInPlace(s, str::TrimOpt::Both);
    }
    int again = StripBookPrintedPageFromTitle(s);
    return again > 0 ? again : page;
}

static void BookStripTrailingLoneLatin(char* s) {
    if (!s || !s[0]) {
        return;
    }
    int len = (int)str::Len(s);
    int end = len;
    int last = Utf8CodepointPrev(s, len, end);
    if (last < 'A' || (last > 'Z' && last < 'a') || last > 'z') {
        return;
    }
    int prev = end;
    int before = Utf8CodepointPrev(s, len, prev);
    if (before < 0x80) {
        return;
    }
    s[end] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Both);
}

static bool BookIsTitleContinuation(const BookTocRow& cur, const BookTocRow& nxt) {
    if (!cur.title || !nxt.title || cur.page != nxt.page) {
        return false;
    }
    if (cur.printedPage > 0 && nxt.printedPage > 0) {
        return false;
    }
    if (cur.printedPage > 0 && !BookTitleNeedsWrap(cur.title)) {
        return false;
    }
    if (!BookTitleNeedsContinuation(cur.title)) {
        return false;
    }
    if (BookIsStructTitle(nxt.title) || BookIsXinDe(nxt.title) || BookLooksLikeTocHeading(nxt.title) ||
        BookStartsWithListNumber(nxt.title) || BookStartsWithDashSubtitle(nxt.title)) {
        return false;
    }
    if (BookLooksLikeBodyBlurb(nxt.title)) {
        return false;
    }
    if (nxt.x + 1 < cur.x - 12) {
        return false;
    }
    float dy = nxt.y - cur.y;
    if (dy < 0) {
        dy = -dy;
    }
    if (dy > 32) {
        return false;
    }
    int g = BookGlyphCount(nxt.title);
    return g >= 2 && g <= 22;
}

static bool BookIsChapSubtitleContinuation(const BookTocRow& cur, const BookTocRow& nxt) {
    if (!cur.title || !nxt.title || cur.page != nxt.page) {
        return false;
    }
    if (!BookIsChapterUnit(cur.title) || str::Find(cur.title, "——")) {
        return false;
    }
    if (nxt.printedPage > 0) {
        return false;
    }
    if (BookIsStructTitle(nxt.title) || BookIsXinDe(nxt.title) || BookLooksLikeTocHeading(nxt.title) ||
        BookStartsWithListNumber(nxt.title) || BookEntryScheme(nxt.title, nullptr) > 0) {
        return false;
    }
    if (!BookStartsWithDashSubtitle(nxt.title) || !BookLooksLikeChapSubtitle(nxt.title)) {
        return false;
    }
    if (nxt.x + 1 < cur.x - 12) {
        return false;
    }
    float dy = nxt.y - cur.y;
    if (dy < 2 || dy > 36) {
        return false;
    }
    return true;
}

static float BookRowConfidence(const BookTocRow& row, bool merged) {
    float c = 0.45f;
    if (row.hasRightPage) {
        c += 0.25f;
    } else if (row.printedPage > 0) {
        c += 0.15f;
    } else {
        c -= 0.12f;
    }
    if (row.hasLeader) {
        c += 0.12f;
    }
    int g = BookGlyphCount(row.title);
    if (g >= 2 && g <= 40) {
        c += 0.12f;
    }
    if (BookIsStructTitle(row.title) || BookIsXinDe(row.title)) {
        c += 0.10f;
    }
    if (merged) {
        c += 0.10f;
    }
    if (c < 0.15f) {
        c = 0.15f;
    }
    if (c > 0.99f) {
        c = 0.99f;
    }
    return c;
}

static void BookParseTocPage(Vec<BookLine>& page, Vec<BookTocEntry>& hits) {
    float pageW = BookPageWidth(page);
    BookSortVisual(page);
    for (int i = 0; i < page.Size(); i++) {
        if (!page[i].text || BookLooksLikeTocHeading(page[i].text) || BookLooksLikeJunk(page[i].text) ||
            BookIsLeaderOnly(page[i].text)) {
            page[i].used = true;
        }
    }
    Vec<BookTocRow> rows;
    int i = 0;
    while (i < page.Size()) {
        if (page[i].used || !page[i].text) {
            i++;
            continue;
        }
        Vec<int> idx;
        idx.Append(i);
        char soFar[512];
        soFar[0] = 0;
        bool gotPage = BookLineIsPageNum(page[i].text);
        if (!BookIsLeaderOnly(page[i].text) && !gotPage && BookHasLetterOrCjk(page[i].text)) {
            BookBufCat(soFar, (int)sizeof(soFar), page[i].text, nullptr);
        }
        for (int j = i + 1; j < page.Size(); j++) {
            if (!page[j].text) {
                continue;
            }
            if (!BookSameRow(page[i], page[j])) {
                break;
            }
            if (BookIsStructTitle(page[i].text) && BookIsStructTitle(page[j].text) &&
                !BookLineIsPageNum(page[j].text)) {
                break;
            }
            bool jPage = BookLineIsPageNum(page[j].text);
            bool jLead = BookIsLeaderOnly(page[j].text);
            bool jTitle = BookHasLetterOrCjk(page[j].text) && !jPage && !jLead;
            if (jTitle && soFar[0] && !BookTitleNeedsContinuation(soFar)) {
                break;
            }
            if (jTitle && soFar[0] &&
                (BookStartsWithListNumber(page[j].text) || BookStartsWithDashSubtitle(page[j].text))) {
                break;
            }
            if (jTitle && gotPage) {
                break;
            }
            idx.Append(j);
            if (jTitle) {
                BookBufCat(soFar, (int)sizeof(soFar), page[j].text, nullptr);
            }
            if (jPage) {
                gotPage = true;
            }
        }
        char rawBuf[512];
        rawBuf[0] = 0;
        char titleBuf[512];
        titleBuf[0] = 0;
        char pageDigits[16];
        pageDigits[0] = 0;
        int printed = 0;
        bool hasLeader = false;
        bool hasRightPage = false;
        BookLine titleSl = page[idx[0]];
        bool haveTitleBox = false;
        for (int k = 0; k < idx.Size(); k++) {
            BookLine& sl = page[idx[k]];
            if (!sl.text) {
                continue;
            }
            BookBufCat(rawBuf, (int)sizeof(rawBuf), sl.text, rawBuf[0] ? " " : nullptr);
            if (BookIsLeaderOnly(sl.text)) {
                hasLeader = true;
                sl.used = true;
                continue;
            }
            int pg = BookParsePageCandidate(sl.text, sl.x, pageW);
            if (pg < 1 && BookLineIsPageNum(sl.text) && sl.x > pageW * 0.70f) {
                pg = BookParseIsolatedPage(sl.text);
            }
            if (pg > 0) {
                printed = pg;
                hasRightPage = sl.x > pageW * 0.55f;
                char* tok = BookNormalizePageToken(sl.text);
                BookBufCat(pageDigits, (int)sizeof(pageDigits), tok);
                str::Free(tok);
                sl.used = true;
                continue;
            }
            if (str::Find(sl.text, "......") || str::Find(sl.text, "····") || str::Find(sl.text, "⋯")) {
                hasLeader = true;
            }
            BookBufCat(titleBuf, (int)sizeof(titleBuf), sl.text, nullptr);
            if (!haveTitleBox || sl.x < titleSl.x) {
                titleSl = sl;
                haveTitleBox = true;
            }
            sl.used = true;
        }
        if (pageDigits[0] && printed < 1) {
            printed = BookParseIsolatedPage(pageDigits);
        }
        char* title = nullptr;
        int inlinePage = 0;
        if (BookSplitInlinePage(titleBuf, &title, &inlinePage)) {
            if (printed < 1) {
                printed = inlinePage;
                hasLeader = true;
            }
        } else {
            title = BookDupTrim(titleBuf);
        }
        if (title) {
            BookStripLeadersInPlace(title);
            BookStripTrailingTocStop(title);
            BookStripTrailingLoneLatin(title);
            int leadPage = BookStripLeadingPrintedPrefix(title);
            if (printed < 1 && leadPage > 0) {
                printed = leadPage;
                hasLeader = true;
            }
            str::TrimWSInPlace(title, str::TrimOpt::Both);
        }
        if (!title || !title[0]) {
            if (printed > 0 && rows.Size() > 0) {
                int best = -1;
                float bestD = 24.f;
                for (int r = rows.Size() - 1; r >= 0; r--) {
                    float d = titleSl.y - rows[r].y;
                    if (d < 0) {
                        d = -d;
                    }
                    if (d > 24.f) {
                        break;
                    }
                    if (rows[r].printedPage < 1 && d <= bestD) {
                        best = r;
                        bestD = d;
                    }
                }
                if (best >= 0) {
                    rows[best].printedPage = printed;
                    rows[best].hasRightPage = true;
                    BookReasonAdd(rows[best].reason, (int)sizeof(rows[best].reason), "page attached from next row");
                }
            }
            str::Free(title);
            i = idx[idx.Size() - 1] + 1;
            continue;
        }
        BookTocRow row;
        row.raw = BookDupTrim(rawBuf);
        row.title = title;
        row.printedPage = printed;
        row.page = titleSl.page;
        row.x = titleSl.x;
        row.y = titleSl.y;
        row.dx = titleSl.dx;
        row.dy = titleSl.dy;
        row.fontSize = titleSl.fontSize;
        row.bold = titleSl.bold;
        row.hasLeader = hasLeader;
        row.hasRightPage = hasRightPage;
        if (hasRightPage) {
            BookReasonAdd(row.reason, (int)sizeof(row.reason), "page detected from right area");
        } else if (printed > 0) {
            BookReasonAdd(row.reason, (int)sizeof(row.reason), "page split from title/leader");
        } else {
            BookReasonAdd(row.reason, (int)sizeof(row.reason), "no printed page on row");
        }
        if (hasLeader) {
            BookReasonAdd(row.reason, (int)sizeof(row.reason), "leader separator");
        }
        if (BookIsStructTitle(title) || BookIsXinDe(title)) {
            BookReasonAdd(row.reason, (int)sizeof(row.reason), "numbering pattern matched");
        }
        row.confidence = BookRowConfidence(row, false);
        rows.Append(row);
        i = idx[idx.Size() - 1] + 1;
    }

    for (int r = 1; r < rows.Size();) {
        BookTocRow& prev = rows[r - 1];
        BookTocRow& cur = rows[r];
        bool chapSub = BookIsChapSubtitleContinuation(prev, cur);
        if (chapSub || BookIsTitleContinuation(prev, cur)) {
            if (chapSub) {
                BookStripLeadingSubtitleDashInPlace(cur.title);
            }
            const char* sep = chapSub ? "——" : (BookTitleNeedsWrap(prev.title) ? "" : " ");
            char* neu = BookJoinText(prev.title, cur.title, sep);
            char* rawNeu = BookJoinText(prev.raw, cur.raw, "\n");
            str::Free(prev.title);
            str::Free(prev.raw);
            prev.title = neu;
            prev.raw = rawNeu;
            BookStripLeadersInPlace(prev.title);
            str::TrimWSInPlace(prev.title, str::TrimOpt::Both);
            if (prev.printedPage < 1) {
                prev.printedPage = cur.printedPage;
                prev.hasRightPage = prev.hasRightPage || cur.hasRightPage;
            }
            prev.hasLeader = prev.hasLeader || cur.hasLeader;
            if (chapSub) {
                prev.keepDashSubtitle = true;
            }
            BookReasonAdd(prev.reason, (int)sizeof(prev.reason), "multiline merged");
            prev.confidence = BookRowConfidence(prev, true);
            str::Free(cur.raw);
            str::Free(cur.title);
            rows.RemoveAt(r);
            continue;
        }
        r++;
    }

    for (int r = 0; r < rows.Size();) {
        char* left = nullptr;
        char* right = nullptr;
        int splitPage = 0;
        const char* why = nullptr;
        int second = BookSecondUnitOffset(rows[r].title);
        if (second >= 1) {
            right = BookDupTrim(rows[r].title + second);
            left = (char*)memdup(rows[r].title, (size_t)second, 1);
            if (left) {
                left[second] = 0;
                str::TrimWSInPlace(left, str::TrimOpt::Both);
            }
            why = "split glued 第N unit";
        } else if (BookSplitGluedTocLine(rows[r].title, &left, &splitPage, &right)) {
            why = "split glued printed line";
        } else {
            int listAt = BookSecondListOffset(rows[r].title);
            int dashAt = BookFindDashSplitOffset(rows[r].title);
            if (dashAt >= 3 && rows[r].keepDashSubtitle) {
                dashAt = -1;
            }
            int at = -1;
            if (listAt >= 1 && (dashAt < 1 || listAt < dashAt)) {
                at = listAt;
                why = "split glued 1. item";
            } else if (dashAt >= 3) {
                at = dashAt;
                why = "split chapter subtitle dash";
            }
            if (at >= 1) {
                right = BookDupTrim(rows[r].title + at);
                left = (char*)memdup(rows[r].title, (size_t)at, 1);
                if (left) {
                    left[at] = 0;
                    str::TrimWSInPlace(left, str::TrimOpt::Both);
                }
            }
        }
        if (!left || !right || !why) {
            str::Free(left);
            str::Free(right);
            r++;
            continue;
        }
        str::Free(rows[r].title);
        rows[r].title = left;
        if (splitPage > 0 && rows[r].printedPage < 1) {
            rows[r].printedPage = splitPage;
        }
        BookReasonAdd(rows[r].reason, (int)sizeof(rows[r].reason), why);
        BookTocRow extra;
        extra.title = right;
        extra.raw = BookDupTrim(right);
        extra.page = rows[r].page;
        extra.x = rows[r].x;
        extra.y = rows[r].y + 1;
        extra.dx = rows[r].dx;
        extra.dy = rows[r].dy;
        extra.fontSize = rows[r].fontSize;
        extra.bold = rows[r].bold;
        extra.hasLeader = false;
        extra.hasRightPage = false;
        extra.printedPage = 0;
        BookReasonAdd(extra.reason, (int)sizeof(extra.reason), why);
        extra.confidence = BookRowConfidence(extra, false);
        rows.InsertAt(r + 1, extra);
        r++;
    }

    for (int r = 0; r < rows.Size(); r++) {
        BookTocRow& row = rows[r];
        if (!row.title || BookLooksLikeTocHeading(row.title) || BookLooksLikeJunk(row.title) ||
            BookLooksLikeBodyBlurb(row.title)) {
            continue;
        }
        int g = BookGlyphCount(row.title);
        bool keep = BookIsStructTitle(row.title) || BookIsXinDe(row.title) || BookIsPartTitle(row.title) ||
                    row.printedPage > 0 || BookStartsWithListNumber(row.title);
        if (!keep && (g > 22 || g < 2 || !BookHasLetterOrCjk(row.title))) {
            continue;
        }
        BookLine sl;
        sl.text = row.title;
        sl.page = row.page;
        sl.x = row.x;
        sl.y = row.y;
        sl.dx = row.dx;
        sl.dy = row.dy;
        sl.fontSize = row.fontSize;
        sl.bold = row.bold;
        BookAppendEntry(hits, row.title, row.printedPage, sl, row.raw, row.reason, row.confidence);
        sl.text = nullptr;
    }
    BookFreeRows(rows);
}

static void BookSortEntries(Vec<BookTocEntry>& hits) {
    for (int i = 0; i < hits.Size(); i++) {
        for (int j = i + 1; j < hits.Size(); j++) {
            bool before = hits[j].srcPage < hits[i].srcPage ||
                          (hits[j].srcPage == hits[i].srcPage && hits[j].srcY < hits[i].srcY - 0.5f);
            if (before) {
                BookTocEntry t = hits[i];
                hits[i] = hits[j];
                hits[j] = t;
            }
        }
    }
}

static void BookMergeWrapEntries(Vec<BookTocEntry>& hits) {
    for (int i = 1; i < hits.Size();) {
        bool samePage = hits[i].srcPage == hits[i - 1].srcPage;
        bool nextPage = hits[i].srcPage == hits[i - 1].srcPage + 1;
        if (!samePage && !nextPage) {
            i++;
            continue;
        }
        if (hits[i - 1].printedPage > 0 && hits[i].printedPage > 0) {
            i++;
            continue;
        }
        bool wrap = BookTitleNeedsContinuation(hits[i - 1].title) && BookIsWrapFragment(hits[i].title);
        float gapY = hits[i].srcY - hits[i - 1].srcY;
        if (nextPage) {
            gapY = 12;
        }
        bool chapSub = BookIsChapterUnit(hits[i - 1].title) && BookStartsWithDashSubtitle(hits[i].title) &&
                       BookLooksLikeChapSubtitle(hits[i].title) && hits[i].printedPage < 1 &&
                       !str::Find(hits[i - 1].title, "——") && gapY >= 2 && gapY <= 36;
        if (!wrap && !chapSub) {
            i++;
            continue;
        }
        if (chapSub) {
            BookStripLeadingSubtitleDashInPlace(hits[i].title);
        }
        const char* sep = chapSub ? "——" : (BookTitleNeedsWrap(hits[i - 1].title) ? "" : " ");
        char* neu = BookJoinText(hits[i - 1].title, hits[i].title, sep);
        char* rawNeu = BookJoinText(hits[i - 1].raw, hits[i].raw, "\n");
        str::Free(hits[i - 1].title);
        str::Free(hits[i - 1].raw);
        hits[i - 1].title = neu;
        hits[i - 1].raw = rawNeu;
        BookStripLeadersInPlace(hits[i - 1].title);
        str::TrimWSInPlace(hits[i - 1].title, str::TrimOpt::Both);
        if (hits[i - 1].printedPage < 1) {
            hits[i - 1].printedPage = hits[i].printedPage;
        }
        char reason[192];
        reason[0] = 0;
        BookBufCat(reason, (int)sizeof(reason), hits[i - 1].reason, nullptr);
        BookReasonAdd(reason, (int)sizeof(reason), "multiline merged");
        str::Free(hits[i - 1].reason);
        hits[i - 1].reason = BookDupTrim(reason);
        str::Free(hits[i].title);
        str::Free(hits[i].raw);
        str::Free(hits[i].reason);
        hits.RemoveAt(i);
    }
}

static void BookAssignLevels(Vec<BookTocEntry>& hits) {
    int n = hits.Size();
    if (n < 1) {
        return;
    }
    float titleCol = 0;
    int nCol = 0;
    int nPartUnit = 0;
    int nChap = 0;
    int nSec = 0;
    for (int i = 0; i < n; i++) {
        if (hits[i].printedPage > 0 || BookIsStructTitle(hits[i].title) || BookIsXinDe(hits[i].title)) {
            titleCol += hits[i].srcX;
            nCol++;
        }
        BookUnitKind kind = BookParseUnit(hits[i].title).kind;
        if (kind == BookUnitKind::Part) {
            nPartUnit++;
        }
        if (kind == BookUnitKind::Chapter) {
            nChap++;
        }
        if (kind == BookUnitKind::Section) {
            nSec++;
        }
    }
    if (nCol > 0) {
        titleCol /= (float)nCol;
    }
    // Centered 心得-style part titles only when the book has no 第X章.
    // With chapters, a right-shifted line is a chapter subtitle, not a Part.
    int nBanner = 0;
    Vec<int> isBanner;
    for (int i = 0; i < n; i++) {
        bool banner = false;
        if (nChap < 1) {
            banner = hits[i].printedPage < 1 && BookIsPartTitle(hits[i].title) &&
                     (titleCol < 1 || hits[i].srcX > titleCol + 36);
        }
        isBanner.Append(banner ? 1 : 0);
        if (banner) {
            nBanner++;
        }
    }
    bool hasPart = nPartUnit > 0 || nBanner > 0;
    bool hasChap = nChap > 0;
    int leftoverBase = 1;
    if (hasPart && hasChap && nSec > 0) {
        leftoverBase = 4;
    } else if (hasPart && hasChap) {
        leftoverBase = 3;
    } else if (hasPart || hasChap || nSec > 0) {
        leftoverBase = 2;
    }
    int prev = 1;
    int containerLvl = 1;
    int lastScheme = 0;
    int lastSchemeLvl = 0;
    bool prevWasChap = false;
    for (int i = 0; i < n; i++) {
        BookUnitKind kind = BookParseUnit(hits[i].title).kind;
        bool banner = isBanner[i] != 0;
        bool subtitle =
            kind == BookUnitKind::None && !banner && prevWasChap && BookLooksLikeChapSubtitle(hits[i].title);
        int scheme = BookEntryScheme(hits[i].title, nullptr);
        int lvl = leftoverBase;
        if (kind == BookUnitKind::Part || banner) {
            lvl = 1;
            containerLvl = 1;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
        } else if (kind == BookUnitKind::Chapter || kind == BookUnitKind::Section) {
            lvl = BookStructOutlineLevel(kind, hasPart, hasChap);
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = kind == BookUnitKind::Chapter;
        } else if (BookIsXinDe(hits[i].title) && hasPart) {
            lvl = 2;
            containerLvl = 2;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
        } else if (subtitle) {
            lvl = containerLvl + 1;
            if (lvl < 2) {
                lvl = 2;
            }
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
        } else if (scheme > 0 && lastScheme == scheme && lastSchemeLvl > 0) {
            lvl = lastSchemeLvl;
            prevWasChap = false;
        } else if (scheme > 0) {
            lvl = containerLvl + 1;
            prevWasChap = false;
        } else {
            // Unnumbered leftovers stay one step under the current 章/课.
            // Indent-chasing turned printed pages like "(144)" / "(145)" into a
            // fake deep outline; books do not nest that way (公文 does).
            lvl = containerLvl + 1;
            prevWasChap = false;
        }
        if (lvl < 1) {
            lvl = 1;
        }
        bool absolute = kind != BookUnitKind::None || banner;
        if (!absolute && lvl > prev + 1) {
            lvl = prev + 1;
        }
        if (lvl > 6) {
            lvl = 6;
        }
        hits[i].inferredLevel = lvl;
        prev = lvl;
        if (scheme > 0) {
            lastScheme = scheme;
            lastSchemeLvl = lvl;
        }
    }
}

static int BookTitleMatchScore(const char* body, const char* title) {
    if (!body || !title || !title[0]) {
        return 0;
    }
    if (str::Eq(body, title) || str::StartsWith(body, title) || str::Find(body, title)) {
        return BookGlyphCount(title);
    }
    int tg = BookGlyphCount(title);
    int bg = BookGlyphCount(body);
    if (tg < 4) {
        return 0;
    }
    // Wrapped body heading: "加强社会主义" + "民主建设" on the next line.
    if (bg >= 4 && bg * 2 >= tg && str::StartsWith(title, body)) {
        return bg;
    }
    if (str::Find(body, title)) {
        return tg;
    }
    return 0;
}

static int BookParsePageLabel(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    while (i < len) {
        int save = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (cp >= '0' && cp <= '9') {
            i = save;
            break;
        }
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF)) {
            return 0;
        }
    }
    if (i >= len || s[i] < '0' || s[i] > '9') {
        return 0;
    }
    int v = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
        if (v > 9999) {
            return 0;
        }
    }
    BookSkipWs(s, len, i);
    if (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF)) {
            return 0;
        }
    }
    return v;
}

// Identity labels ("1","2",...) are just PDF page numbers. Using them as
// printed-page dests is the old printed==pdf bug.
static bool BookLabelsAreCustom(const Vec<char*>& labels) {
    if (labels.Size() < 2) {
        return false;
    }
    int nCustom = 0;
    for (int i = 0; i < labels.Size(); i++) {
        int v = BookParsePageLabel(labels[i]);
        if (v != i + 1) {
            nCustom++;
        }
    }
    return nCustom >= 2;
}

// Footer/header like "1 .", "·11·", "1 0·", "1 8。" — digits only plus leaders.
static int BookParseLoosePageNum(const char* s) {
    if (!s || !s[0] || BookHasLetterOrCjk(s)) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    int page = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        if (BookIsDigit(cp)) {
            page = page * 10 + BookDigitVal(cp);
            n++;
            if (n > 3) {
                return 0;
            }
            continue;
        }
        if (cp > 32 && cp != 0x3000 && !BookIsDotLeaderCp(cp) && cp != '*' && cp != 0x3002 && cp != 0xFF0E) {
            return 0;
        }
    }
    if (n < 1 || page < 1 || page > 400) {
        return 0;
    }
    return page;
}

static void BookFillPrintedDestMap(const Vec<ScanLine>& lines, const Vec<char*>& labels, int tocEnd, int nPages,
                                   int* toPdf, int cap) {
    for (int i = 0; i < cap; i++) {
        toPdf[i] = 0;
    }
    if (BookLabelsAreCustom(labels)) {
        for (int i = 0; i < labels.Size(); i++) {
            int pr = BookParsePageLabel(labels[i]);
            if (pr < 1 || pr >= cap || toPdf[pr] > 0) {
                continue;
            }
            int p = i + 1;
            if (p > tocEnd) {
                toPdf[pr] = p;
            }
        }
    }
    Vec<float> pageH;
    for (int p = 0; p <= nPages; p++) {
        pageH.Append(0);
    }
    for (int i = 0; i < lines.Size(); i++) {
        int p = lines[i].srcPage;
        if (p < 1 || p > nPages) {
            continue;
        }
        float b = lines[i].y + (lines[i].dy > 1 ? lines[i].dy : 10);
        if (b > pageH[p]) {
            pageH[p] = b;
        }
    }
    for (int i = 0; i < lines.Size(); i++) {
        int p = lines[i].srcPage;
        if (p <= tocEnd || p > nPages || !lines[i].text) {
            continue;
        }
        float y = lines[i].y;
        if (pageH[p] > 40 && y < pageH[p] * 0.78f && y > pageH[p] * 0.12f) {
            continue;
        }
        int pr = BookParseLoosePageNum(lines[i].text);
        if (pr < 1 || pr >= cap || toPdf[pr] > 0) {
            continue;
        }
        toPdf[pr] = p;
    }
}

static int BookMedianAgreeOffset(const int* offs, int n) {
    if (!offs || n < 2) {
        return 0;
    }
    int sorted[32];
    int m = n < 32 ? n : 32;
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
    int med = sorted[m / 2];
    int agree = 0;
    for (int i = 0; i < m; i++) {
        int d = sorted[i] - med;
        if (d < 0) {
            d = -d;
        }
        if (d <= 1) {
            agree++;
        }
    }
    return agree >= 2 ? med : 0;
}

static int BookCalibratedPrintedOffset(const int* toPdf, int cap, int tocEnd, int nPages) {
    int offs[24];
    int n = 0;
    for (int pr = 1; pr < cap && n < 24; pr++) {
        if (toPdf[pr] <= tocEnd) {
            continue;
        }
        int off = toPdf[pr] - pr;
        if (off < 0 || toPdf[pr] > nPages) {
            continue;
        }
        offs[n++] = off;
    }
    return BookMedianAgreeOffset(offs, n);
}

static int BookResolvePrintedDest(const int* toPdf, int cap, int printed, int offset, int tocEnd, int nPages) {
    if (printed < 1) {
        return 0;
    }
    if (printed < cap && toPdf[printed] > tocEnd) {
        return toPdf[printed];
    }
    if (offset > 0) {
        int p = printed + offset;
        if (p > tocEnd && p <= nPages) {
            return p;
        }
    }
    return 0;
}

static void BookApplyLineDest(BookTocEntry& hit, const ScanLine& sl) {
    hit.pdfPage = sl.srcPage;
    hit.x = sl.x;
    hit.y = sl.y;
    hit.bodyFontSize = sl.fontSize;
    hit.bodyDy = sl.dy > 1 ? sl.dy : sl.fontSize;
    hit.bodyBold = sl.bold;
    hit.bodyMatched = true;
}

// Printed page number is the dest. Map it through page labels or body
// footers; title search only picks x,y on that PDF page. Never use the
// printed number as a raw PDF page index.
static void BookSanitizePrintedPages(Vec<BookTocEntry>& hits) {
    int last = 0;
    for (int i = 0; i < hits.Size(); i++) {
        int p = hits[i].printedPage;
        if (p < 1) {
            continue;
        }
        if (last > 0 && p < last) {
            hits[i].printedPage = 0;
            continue;
        }
        last = p;
    }
}

static bool BookDestIsTocPage(int page, int tocStart, int tocEnd) {
    if (page < 1 || tocEnd < 1) {
        return false;
    }
    int start = tocStart > 0 ? tocStart : 1;
    return page >= start && page <= tocEnd;
}

struct BookTitleBodyHit {
    int line = -1;
    int page = 0;
    int score = 0;
};

static void BookFindTitleBodyHits(const Vec<ScanLine>& lines, int tocStart, int tocEnd, const char* title,
                                  Vec<BookTitleBodyHit>& out) {
    out.Reset();
    if (!title || !title[0]) {
        return;
    }
    for (int i = 0; i < lines.Size(); i++) {
        if (!lines[i].text || BookDestIsTocPage(lines[i].srcPage, tocStart, tocEnd)) {
            continue;
        }
        int sc = BookTitleMatchScore(lines[i].text, title);
        if (sc < 4) {
            continue;
        }
        BookTitleBodyHit h;
        h.line = i;
        h.page = lines[i].srcPage;
        h.score = sc;
        out.Append(h);
    }
}

static int BookBestTitleBodyIndex(const Vec<BookTitleBodyHit>& found, int destPage) {
    int best = -1;
    int bestScore = 0;
    for (int i = 0; i < found.Size(); i++) {
        if (destPage > 0 && found[i].page != destPage) {
            continue;
        }
        if (found[i].score > bestScore) {
            bestScore = found[i].score;
            best = i;
        }
    }
    return best;
}

// Directory row (title + printed page) vs the same title in the body. Front
// matter (copyright, TOC) is not a printed page, so pdf - printed is the offset.
static int BookCalibrateTitlePrintedOffset(const Vec<ScanLine>& lines, int tocStart, int tocEnd, int nPages,
                                           const Vec<BookTocEntry>& hits) {
    int offs[32];
    int n = 0;
    for (int h = 0; h < hits.Size() && n < 32; h++) {
        int printed = hits[h].printedPage;
        if (printed < 1 || !hits[h].title || BookGlyphCount(hits[h].title) < 4) {
            continue;
        }
        Vec<BookTitleBodyHit> found;
        BookFindTitleBodyHits(lines, tocStart, tocEnd, hits[h].title, found);
        int bestPage = 0;
        int bestScore = 0;
        int nBestPages = 0;
        for (int i = 0; i < found.Size(); i++) {
            if (found[i].score > bestScore) {
                bestScore = found[i].score;
                bestPage = found[i].page;
                nBestPages = 1;
            } else if (found[i].score == bestScore && found[i].page != bestPage) {
                nBestPages++;
            }
        }
        if (nBestPages != 1 || bestPage < 1 || bestPage > nPages) {
            continue;
        }
        int off = bestPage - printed;
        if (off < 0) {
            continue;
        }
        offs[n++] = off;
    }
    return BookMedianAgreeOffset(offs, n);
}

static void BookApplyPrintedOffsetMap(int* toPdf, int cap, int offset, int tocEnd, int nPages,
                                      const Vec<BookTocEntry>& hits) {
    if (!toPdf || offset < 1 || cap < 2) {
        return;
    }
    for (int pr = 1; pr < cap; pr++) {
        if (toPdf[pr] < 1) {
            continue;
        }
        int d = toPdf[pr] - pr - offset;
        if (d < 0) {
            d = -d;
        }
        if (d > 1) {
            toPdf[pr] = 0;
        }
    }
    for (int h = 0; h < hits.Size(); h++) {
        int pr = hits[h].printedPage;
        if (pr < 1 || pr >= cap || toPdf[pr] > 0) {
            continue;
        }
        int p = pr + offset;
        if (p > tocEnd && p <= nPages) {
            toPdf[pr] = p;
        }
    }
}

static void BookMapPrintedPages(const Vec<ScanLine>& lines, int tocStart, int tocEnd, int nPages,
                                const Vec<char*>& labels, Vec<BookTocEntry>& hits, int* offsetOut) {
    int toPdf[401];
    BookFillPrintedDestMap(lines, labels, tocEnd, nPages, toPdf, 401);
    int footerOff = BookCalibratedPrintedOffset(toPdf, 401, tocEnd, nPages);
    int titleOff = BookCalibrateTitlePrintedOffset(lines, tocStart, tocEnd, nPages, hits);
    int offset = 0;
    if (BookLabelsAreCustom(labels)) {
        offset = footerOff > 0 ? footerOff : titleOff;
    } else if (titleOff > 0) {
        offset = titleOff;
        BookApplyPrintedOffsetMap(toPdf, 401, offset, tocEnd, nPages, hits);
    } else {
        offset = footerOff;
    }
    if (offsetOut) {
        *offsetOut = offset;
    }
    Vec<int> matched;
    for (int h = 0; h < hits.Size(); h++) {
        matched.Append(0);
        int dest = BookResolvePrintedDest(toPdf, 401, hits[h].printedPage, offset, tocEnd, nPages);
        if (BookDestIsTocPage(dest, tocStart, tocEnd)) {
            dest = 0;
        }
        Vec<BookTitleBodyHit> found;
        BookFindTitleBodyHits(lines, tocStart, tocEnd, hits[h].title, found);
        int best = BookBestTitleBodyIndex(found, dest);
        int bestPage = best >= 0 ? found[best].page : 0;
        int bestLine = best >= 0 ? found[best].line : -1;
        if (dest > 0) {
            hits[h].pdfPage = dest;
            hits[h].x = hits[h].srcX;
            hits[h].y = 0;
            matched[h] = 1;
            if (bestLine >= 0) {
                BookApplyLineDest(hits[h], lines[bestLine]);
                hits[h].pdfPage = dest;
            }
        } else if (bestLine >= 0 && !BookDestIsTocPage(bestPage, tocStart, tocEnd)) {
            BookApplyLineDest(hits[h], lines[bestLine]);
            hits[h].pdfPage = bestPage;
            matched[h] = 1;
        }
    }
    for (int h = 0; h < hits.Size(); h++) {
        if (matched[h] && hits[h].pdfPage > 0) {
            continue;
        }
        int nb = -1;
        for (int k = h + 1; k < hits.Size(); k++) {
            if (matched[k] && hits[k].pdfPage > 0) {
                nb = k;
                break;
            }
        }
        if (nb < 0) {
            for (int k = h - 1; k >= 0; k--) {
                if (matched[k] && hits[k].pdfPage > 0) {
                    nb = k;
                    break;
                }
            }
        }
        bool samePrinted = nb >= 0 && (hits[h].printedPage < 1 || hits[nb].printedPage < 1 ||
                                       hits[h].printedPage == hits[nb].printedPage);
        if (nb >= 0 && samePrinted) {
            hits[h].pdfPage = hits[nb].pdfPage;
            hits[h].x = hits[nb].x;
            hits[h].y = hits[nb].y;
        } else if (hits[h].srcPage > 0) {
            hits[h].pdfPage = 0;
            hits[h].x = hits[h].srcX;
            hits[h].y = hits[h].srcY;
        } else {
            hits[h].pdfPage = 0;
        }
    }
    for (int h = 0; h < hits.Size(); h++) {
        if (BookDestIsTocPage(hits[h].pdfPage, tocStart, tocEnd)) {
            hits[h].pdfPage = 0;
        }
    }
}

// Printed TOC order is reading order. A dest or printed page that goes
// backwards is a bad body match or a bad page token — not a real TOC.
static void BookEnforceReadingOrder(Vec<BookTocEntry>& hits, int offset, int tocEnd, int nPages) {
    int prevPr = 0;
    int prevPdf = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (BookLooksLikeTocHeading(hits[i].title)) {
            continue;
        }
        int pr = hits[i].printedPage;
        if (pr > 0 && prevPr > 0 && pr < prevPr) {
            hits[i].printedPage = 0;
            pr = 0;
        }
        if (pr > 0) {
            prevPr = pr;
        }
        int pred = 0;
        if (pr > 0 && offset > 0) {
            pred = pr + offset;
            if (pred <= tocEnd || pred > nPages) {
                pred = 0;
            }
        }
        int pdf = hits[i].pdfPage;
        if (pdf > 0 && prevPdf > 0 && pdf < prevPdf) {
            pdf = 0;
            hits[i].bodyMatched = false;
        }
        if (pdf < 1) {
            if (pred >= prevPdf && pred > 0) {
                pdf = pred;
            } else if (prevPdf > 0) {
                pdf = prevPdf;
            }
        }
        if (pdf > 0) {
            hits[i].pdfPage = pdf;
            prevPdf = pdf;
        }
    }
}

static ExtractedTocItem* BookNewItem(const char* title, int pageNo, float x, float y, int level, int confidence,
                                     ExtractedTocSource source, const char* rawTitle) {
    auto* n = new ExtractedTocItem;
    n->title = BookDupTrim(title);
    n->rawTitle = BookDupTrim(rawTitle && rawTitle[0] ? rawTitle : title);
    StripBookPrintedPageFromTitle(n->title);
    StripBookPrintedPageFromTitle(n->rawTitle);
    NormalizeTocNumberingDotsHalfwidth(&n->title);
    NormalizeTocNumberingDotsHalfwidth(&n->rawTitle);
    n->pageNo = pageNo;
    n->x = x;
    n->y = y;
    n->level = level < 1 ? 1 : level;
    n->confidence = confidence;
    n->source = source;
    if (n->confidence < 0) {
        n->confidence = 0;
    }
    if (n->confidence > 100) {
        n->confidence = 100;
    }
    return n;
}

static char* BookPrintedTocBookmarkTitle(const char* raw) {
    if (raw && str::Find(raw, "\xE7\x9B\xAE\xE6\xAC\xA1")) { // 目次
        return str::Dup("目次");
    }
    if (raw && str::ContainsI(raw, "table of contents")) {
        return str::Dup("Contents");
    }
    if (raw && str::ContainsI(raw, "contents")) {
        return str::Dup("Contents");
    }
    return str::Dup("目录");
}

// Put the printed 目录 page itself in the outline so a reviewer can jump there
// and check extracted entries against the author's TOC.
static void BookInsertPrintedTocBookmark(const Vec<ScanLine>& lines, int tocStart, int tocEnd,
                                         Vec<BookTocEntry>& hits) {
    if (tocStart < 1) {
        return;
    }
    for (int i = 0; i < hits.Size(); i++) {
        if (BookLooksLikeTocHeading(hits[i].title)) {
            return;
        }
    }
    int page = tocStart;
    float x = 0;
    float y = 0;
    const char* raw = nullptr;
    for (int p = tocStart; p <= tocEnd && !raw; p++) {
        for (int i = 0; i < lines.Size(); i++) {
            if (lines[i].srcPage != p || !lines[i].text || !BookLooksLikeTocHeading(lines[i].text)) {
                continue;
            }
            page = p;
            x = lines[i].x;
            y = lines[i].y;
            raw = lines[i].text;
            break;
        }
    }
    BookTocEntry h;
    h.title = BookPrintedTocBookmarkTitle(raw);
    if (!h.title) {
        return;
    }
    h.pdfPage = page;
    h.srcPage = page;
    h.x = x;
    h.y = y;
    h.srcX = x;
    h.srcY = y;
    h.inferredLevel = 1;
    h.confidence = 0.99f;
    h.bodyMatched = true;
    hits.InsertAt(0, h);
}

static void BookBuildTree(Vec<BookTocEntry>& hits, Vec<ExtractedTocItem*>& roots) {
    Vec<ExtractedTocItem*> flat;
    for (int i = 0; i < hits.Size(); i++) {
        int conf = (int)(hits[i].confidence * 100.f);
        ExtractedTocSource src =
            hits[i].source == 1 ? ExtractedTocSource::BodyInference : ExtractedTocSource::PrintedToc;
        ExtractedTocItem* n = BookNewItem(hits[i].title, hits[i].pdfPage, hits[i].x, hits[i].y, hits[i].inferredLevel,
                                          conf, src, hits[i].title);
        n->printedPage = hits[i].printedPage;
        n->tocPageNo = hits[i].srcPage;
        n->tocX = hits[i].srcX;
        n->tocY = hits[i].srcY;
        n->bodyMatched = hits[i].bodyMatched;
        n->verified = hits[i].bodyMatched && hits[i].printedPage > 0 && hits[i].pdfPage > 0;
        flat.Append(n);
    }
    Vec<ExtractedTocItem*> stack;
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* n = flat[i];
        while (stack.Size() > 0 && stack.Last()->level >= n->level) {
            stack.RemoveLast();
        }
        if (stack.Size() == 0) {
            n->parent = nullptr;
            roots.Append(n);
        } else {
            n->parent = stack.Last();
            stack.Last()->children.Append(n);
        }
        stack.Append(n);
    }
}

static void BookWriteDebug(const char* path, int tocStart, int tocEnd, int printedOffset,
                           const Vec<BookTocEntry>& hits) {
    if (!path) {
        return;
    }
    FILE* f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Book printed TOC debug\n");
    fprintf(f, "TOC pages: %d-%d\n", tocStart, tocEnd);
    fprintf(f, "printed offset: %d\n", printedOffset);
    fprintf(f, "entries: %d\n\n", hits.Size());
    for (int i = 0; i < hits.Size(); i++) {
        const BookTocEntry& h = hits[i];
        fprintf(f, "RAW:\n%s\n\n", h.raw && h.raw[0] ? h.raw : (h.title ? h.title : ""));
        fprintf(f, "PARSED:\n");
        fprintf(f, "title:\n%s\n\n", h.title ? h.title : "");
        fprintf(f, "page:\n%d\n\n", h.printedPage);
        if (h.printedPage > 0 && h.pdfPage > 0) {
            fprintf(f, "printed %d -> pdf %d (offset %d)\n\n", h.printedPage, h.pdfPage, h.pdfPage - h.printedPage);
        }
        fprintf(f, "pdf page:\n%d\n\n", h.pdfPage);
        fprintf(f, "level:\n%d\n\n", h.inferredLevel);
        fprintf(f, "confidence:\n%.2f\n\n", h.confidence);
        fprintf(f, "reason:\n%s\n\n", h.reason && h.reason[0] ? h.reason : "-");
        fprintf(f, "Source:\n%s\n\n", h.source == 1 ? "BODY_INFERRED" : "PRINTED_TOC");
        fprintf(f, "Original:\n%s\n\n", h.title ? h.title : "");
        fprintf(f, "Final:\n%s\n\n", h.title ? h.title : "");
        if (h.raw && h.title && !str::Eq(h.title, h.raw) && !str::Find(h.raw, h.title)) {
            fprintf(f, "WARNING: title was rewritten from reconstructed line\n\n");
        }
        fprintf(f, "Indent/x: %.1f\n", h.srcX);
        fprintf(f, "Body matched: %s\n", h.bodyMatched ? "YES" : "NO");
        fprintf(f, "Verified: %s\n", (h.bodyMatched && h.printedPage > 0 && h.pdfPage > 0) ? "YES" : "NO");
        fprintf(f, "Accepted: YES\n\n");
    }
    fclose(f);
}

static bool BookLooksLikeOfficialClause(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (BookParseCnDunhaoPrefix(s, 0, nullptr) > 0) {
        return true;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int c0 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    int c1 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (c0 == 0x4E00 && (c1 == 0x662F || c1 == 0xFF0C || c1 == ',')) {
        return true; // 一是 / 一，
    }
    i = 0;
    BookSkipWs(s, len, i);
    int open = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (open != '(' && open != 0xFF08) {
        return false;
    }
    int n = BookParseCnOrDigitRun(s, len, i);
    if (n < 1 || n > 9) {
        return false;
    }
    int close = i < len ? Utf8CodepointNext(s, len, i) : 0;
    return close == ')' || close == 0xFF09;
}

static bool BookLooksLikeCaption(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int c0 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    int c1 = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if ((c0 == 0x56FE || c0 == 0x8868) && (BookIsDigit(c1) || BookIsCnNumeral(c1))) {
        return true;
    }
    if (str::StartsWithI(s, "fig.") || str::StartsWithI(s, "figure ") || str::StartsWithI(s, "table ")) {
        return true;
    }
    return false;
}

static bool BookIsFrontMatterTitle(const char* s) {
    if (!s || !s[0] || BookGlyphCount(s) > 12) {
        return false;
    }
    return str::Find(s, "前言") || str::Find(s, "后记") || str::Find(s, "总序") || str::Find(s, "序言") ||
           str::Find(s, "绪论");
}

static bool BookIsNumberedBodyHeading(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    if (BookLooksLikeOfficialClause(s)) {
        return false;
    }
    if (BookIsStructTitle(s) || BookIsXinDe(s) || BookIsFrontMatterTitle(s)) {
        return true;
    }
    if (BookStartsWithListNumber(s) && BookGlyphCount(s) <= 28) {
        return true;
    }
    return str::StartsWithI(s, "chapter") || str::StartsWithI(s, "section");
}

static int BookCountExactRepeats(const Vec<ScanLine>& lines, const char* text) {
    if (!text || !text[0]) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].text && str::Eq(lines[i].text, text)) {
            n++;
        }
    }
    return n;
}

static float BookMedianFontSize(const Vec<ScanLine>& lines) {
    Vec<float> v;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].fontSize > 1.f) {
            v.Append(lines[i].fontSize);
        }
    }
    int n = v.Size();
    if (n < 1) {
        return 12.f;
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

bool ExtractBookBodyHeadings(const Vec<ScanLine>& lines, int nPages, Vec<ExtractedTocItem*>& roots) {
    int tocStart = 0;
    int tocEnd = 0;
    bool haveToc = BookFindTocRange(lines, nPages, &tocStart, &tocEnd);
    float med = BookMedianFontSize(lines);
    if (med < 8.f) {
        med = 12.f;
    }
    Vec<BookTocEntry> hits;
    bool anyStruct = false;
    for (int i = 0; i < lines.Size(); i++) {
        const ScanLine& sl = lines[i];
        if (!sl.text) {
            continue;
        }
        if (haveToc && sl.srcPage >= tocStart && sl.srcPage <= tocEnd) {
            continue;
        }
        if (BookLooksLikeCipPage(lines, sl.srcPage)) {
            continue;
        }
        if (BookLooksLikeTocHeading(sl.text) || BookLooksLikeJunk(sl.text) || BookLooksLikeBodyBlurb(sl.text) ||
            BookLineIsPageNum(sl.text) || BookLooksLikeCaption(sl.text) || BookLooksLikeOfficialClause(sl.text)) {
            continue;
        }
        if (nPages >= 3 && BookCountExactRepeats(lines, sl.text) >= 3) {
            continue;
        }
        int g = BookGlyphCount(sl.text);
        if (g < 2 || g > 28 || !BookHasLetterOrCjk(sl.text)) {
            continue;
        }
        bool numbered = BookIsNumberedBodyHeading(sl.text);
        bool style = (sl.fontSize >= med * 1.18f || (sl.bold && sl.fontSize >= med * 1.02f)) && g <= 22;
        if (!numbered && !style) {
            continue;
        }
        if (numbered && BookIsStructTitle(sl.text)) {
            anyStruct = true;
        }
        BookTocEntry h;
        h.title = BookDupTrim(sl.text);
        if (!h.title) {
            continue;
        }
        h.pdfPage = sl.srcPage;
        h.srcPage = sl.srcPage;
        h.x = sl.x;
        h.y = sl.y;
        h.srcX = sl.x;
        h.srcY = sl.y;
        h.fontSize = sl.fontSize;
        h.bold = sl.bold;
        h.bodyFontSize = sl.fontSize;
        h.bodyBold = sl.bold;
        h.bodyMatched = true;
        h.source = 1;
        h.confidence = numbered ? 0.72f : 0.58f;
        if (BookIsChapterUnit(h.title) || BookIsFrontMatterTitle(h.title)) {
            h.inferredLevel = 1;
        } else if (sl.fontSize >= med * 1.40f) {
            h.inferredLevel = 1;
        } else {
            h.inferredLevel = 2;
        }
        hits.Append(h);
    }
    if (hits.Size() > 80) {
        float keepMin = med * 1.32f;
        for (int i = 0; i < hits.Size();) {
            if (hits[i].source == 1 && !BookIsNumberedBodyHeading(hits[i].title) && hits[i].fontSize < keepMin) {
                str::Free(hits[i].title);
                str::Free(hits[i].raw);
                str::Free(hits[i].reason);
                hits.RemoveAt(i);
                continue;
            }
            i++;
        }
    }
    if (hits.Size() < 2) {
        BookFreeEntries(hits);
        return false;
    }
    BookSortEntries(hits);
    if (anyStruct) {
        BookAssignLevels(hits);
    }
    BookBuildTree(hits, roots);
    BookFreeEntries(hits);
    int n = 0;
    for (int i = 0; i < roots.Size(); i++) {
        n++;
        n += roots[i]->children.Size();
    }
    return n >= 2;
}

bool ExtractBookPrintedToc(EngineBase* engine, const Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                           Vec<ExtractedTocItem*>& roots, const char* debugPath) {
    (void)engine;
    int tocStart = 0;
    int tocEnd = 0;
    if (!BookFindTocRange(lines, nPages, &tocStart, &tocEnd)) {
        return false;
    }
    Vec<BookTocEntry> hits;
    for (int p = tocStart; p <= tocEnd; p++) {
        if (BookLooksLikeCipPage(lines, p)) {
            continue;
        }
        Vec<BookLine> page;
        BookCollectPage(lines, p, page);
        BookParseTocPage(page, hits);
        BookFreeLines(page);
    }
    BookSortEntries(hits);
    BookMergeWrapEntries(hits);
    BookSortEntries(hits);
    if (hits.Size() < 2) {
        BookFreeEntries(hits);
        return false;
    }
    BookAssignLevels(hits);
    BookSanitizePrintedPages(hits);
    int printedOffset = 0;
    BookMapPrintedPages(lines, tocStart, tocEnd, nPages, labels, hits, &printedOffset);
    BookEnforceReadingOrder(hits, printedOffset, tocEnd, nPages);
    BookInsertPrintedTocBookmark(lines, tocStart, tocEnd, hits);
    BookBuildTree(hits, roots);
    if (debugPath) {
        BookWriteDebug(debugPath, tocStart, tocEnd, printedOffset, hits);
    }
    BookFreeEntries(hits);
    int n = 0;
    for (int i = 0; i < roots.Size(); i++) {
        n++;
        n += roots[i]->children.Size();
    }
    return n >= 4 || roots.Size() >= 1;
}
