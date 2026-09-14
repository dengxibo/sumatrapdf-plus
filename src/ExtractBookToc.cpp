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
#include "ExtractPdfToc.h"
#include "ExtractBookToc.h"

#include "utils/Log.h"

void EngineMupdfEnsurePageLinksForHitTest(EngineBase* engine, int pageNo);

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
    int linkPage = 0; // pdf dest from a GoTo link on the printed Contents row
    bool linkFixed = false;
    int source = 0; // 0 printed TOC, 1 style learner
    char* raw = nullptr;
    char* reason = nullptr;
};

struct BookLine {
    char* text = nullptr;
    int page = 0;
    int linkPage = 0; // GoTo dest sharing this row's band (strong evidence)
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
    // Decorative running headers OCR into fragments of "CONTENTS" that no
    // real title contains ("ONTENTS", "C ONTENTS第警量票量量集章").
    if (str::ContainsI(s, "ONTENTS")) {
        return true;
    }
    return false;
}

// Running-footer furniture like "第 12 页" / "第 i 页" / "第页", optionally
// behind a label prefix ("文档编号： 第IV页"). This is page numbering, never a
// TOC entry title, so it is dropped even when shaped like a structural title
// ("第X章" parsers happily accept "第页").
static bool BookLooksLikePageFooter(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    // Optional "标签：" prefix before the 第...页 core.
    int j = i;
    while (j < len) {
        int save = j;
        int cp = Utf8CodepointNext(s, len, j);
        if (cp == 0xFF1A || cp == ':') {
            BookSkipWs(s, len, j);
            i = j;
            break;
        }
        if (cp == 0x7B2C) { // 第 already reached: no prefix
            j = save;
            break;
        }
    }
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp != 0x7B2C) { // 第
        return false;
    }
    BookSkipWs(s, len, i);
    int run = 0;
    while (i < len && run < 6) {
        int save = i;
        cp = Utf8CodepointNext(s, len, i);
        if (BookIsDigit(cp) || BookIsCnNumeral(cp) || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
            run++;
            continue;
        }
        i = save;
        break;
    }
    BookSkipWs(s, len, i);
    cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp != 0x9875) { // 页
        return false;
    }
    BookSkipWs(s, len, i);
    return i >= len;
}

// Sentence punctuation marks a row as prose (chapter intros bleed into the
// TOC text layer with oversized OCR boxes). Real titles keep enumeration
// marks like 、 and separators like ：, so those stay allowed.
static bool BookHasSentencePunct(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    int i = 0;
    while (i < len) {
        int cp = Utf8CodepointNext(s, len, i);
        switch (cp) {
            case 0xFF0C: // ，
            case 0x3002: // 。
            case 0xFF1B: // ；
            case 0xFF01: // ！
            case 0xFF1F: // ？
            case 0x2026: // …
            case 0x2014: // —
            case ',':
            case '.':
            case ';':
            case '!':
            case '?':
                return true;
        }
    }
    return false;
}

static bool BookHasAsciiDigit(const char* s) {
    if (!s) {
        return false;
    }
    for (int i = 0; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            return true;
        }
    }
    return false;
}

// Some RAZ / scanned fonts report a bbox several times taller than the
// actual glyphs. Using that dy as line height glues consecutive TOC rows.
static float BookVisualHeight(const BookLine& sl) {
    float h = sl.dy;
    if (sl.fontSize > 4.f && sl.dy > sl.fontSize * 1.75f) {
        h = sl.fontSize;
    }
    if (h < 4.f) {
        h = 4.f;
    }
    return h;
}

static float BookMidY(const BookLine& sl) {
    return sl.y + BookVisualHeight(sl) * 0.5f;
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

// OCR occasionally keeps a whole Contents row as one text line, so the first
// parsing pass may leave its leaders and printed page in the title. Run the
// same conservative splitter once more after wrapped rows have been merged.
static int BookExtractTrailingInlinePage(char* title) {
    char* clean = nullptr;
    int page = 0;
    if (!BookSplitInlinePage(title, &clean, &page) || !clean || page < 1) {
        str::Free(clean);
        return 0;
    }
    memmove(title, clean, str::Len(clean) + 1);
    str::Free(clean);
    return page;
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

// "1.1 项目名称" / "1.4.1 政策及标准规范依据" — multi-segment dotted
// numbering. The segment count itself declares the outline depth, a signal
// that is independent of layout: scanned TOCs often set the first chapter's
// children flush with the chapter row, which makes indent bands misread them
// as siblings of the chapter. Returns the byte length of the numbering prefix
// (not counting trailing whitespace), stores the segment count and the first
// segment's numeric value (used to tie "1.x" rows to the running 第X章);
// -1 when s does not start with 2+ dot-separated numbers. Single-segment
// "1." stays with BookParseListPrefix, which deliberately rejects these.
static int BookParseMultiDotPrefix(const char* s, int* depthOut, int* firstOut) {
    if (depthOut) {
        *depthOut = 0;
    }
    if (firstOut) {
        *firstOut = 0;
    }
    if (!s) {
        return -1;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int segs = 0;
    int firstVal = 0;
    int end = -1;
    for (;;) {
        int digits = 0;
        int val = 0;
        while (i < len) {
            int save = i;
            int cp = Utf8CodepointNext(s, len, i);
            if (!BookIsDigit(cp)) {
                i = save;
                break;
            }
            val = val * 10 + BookDigitVal(cp);
            digits++;
            if (digits > 3) {
                return -1;
            }
        }
        if (digits < 1) {
            return -1;
        }
        segs++;
        if (segs == 1) {
            firstVal = val;
        }
        end = i;
        // a dot followed by another digit continues the series; anything
        // else (whitespace, title text, end of line) ends the prefix
        int save = i;
        BookSkipWs(s, len, i);
        int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if ((cp == '.' || cp == 0xFF0E) && i < len) {
            int peek = i;
            BookSkipWs(s, len, peek);
            int nx = peek < len ? Utf8CodepointNext(s, len, peek) : 0;
            if (BookIsDigit(nx)) {
                continue;
            }
        }
        i = save;
        break;
    }
    if (segs < 2) {
        return -1;
    }
    if (depthOut) {
        *depthOut = segs;
    }
    if (firstOut) {
        *firstOut = firstVal;
    }
    return end;
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

static int BookParseAsciiIntAt(const char* s, int from) {
    if (!s) {
        return 1;
    }
    while (s[from] == ' ' || s[from] == '.') {
        from++;
    }
    int n = 0;
    bool any = false;
    while (s[from] >= '0' && s[from] <= '9') {
        any = true;
        n = n * 10 + (s[from] - '0');
        from++;
        if (n > 9999) {
            break;
        }
    }
    return any && n > 0 ? n : 1;
}

static BookUnit BookParseUnit(const char* s) {
    BookUnit u;
    if (!s || !s[0]) {
        return u;
    }
    if (str::StartsWithI(s, "unit ")) {
        u.kind = BookUnitKind::Part;
        u.number = BookParseAsciiIntAt(s, 5);
        u.prefixBytes = 5;
        return u;
    }
    if (str::StartsWithI(s, "part ")) {
        u.kind = BookUnitKind::Part;
        u.number = BookParseAsciiIntAt(s, 5);
        u.prefixBytes = 5;
        return u;
    }
    if (str::StartsWithI(s, "chapter")) {
        u.kind = BookUnitKind::Chapter;
        u.number = BookParseAsciiIntAt(s, 7);
        u.prefixBytes = 7;
        return u;
    }
    if (str::StartsWithI(s, "section")) {
        u.kind = BookUnitKind::Section;
        u.number = BookParseAsciiIntAt(s, 7);
        u.prefixBytes = 7;
        return u;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    int cp = i < len ? Utf8CodepointNext(s, len, i) : 0;
    if (cp == 0x5E8F || cp == 0x672B || cp == 0x7EEA) { // 序 末 绪
        // OCR drops random spaces inside the unit ("绪 论"): skip before
        // reading the unit word's second glyph.
        BookSkipWs(s, len, i);
        int n = i < len ? Utf8CodepointNext(s, len, i) : 0;
        if (n == 0x7AE0 || (cp == 0x7EEA && n == 0x8BBA)) { // 章 / 绪论
            u.kind = BookUnitKind::Chapter;
            u.number = cp == 0x672B ? 99 : 0;
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

// Broken OCR occasionally turns the dot leader and a two-digit page number
// into a long digit run (e.g. "……600080800002"). It is neither a title nor
// a usable page label; keep the title before it so the normal duplicate pass
// can match the correctly recognized row.
static void BookStripBrokenTrailingLeaderNumber(char* s) {
    if (!s) {
        return;
    }
    int len = (int)str::Len(s);
    for (int i = 0; i < len;) {
        int leaderAt = i;
        int cp = Utf8CodepointNext(s, len, i);
        if (!BookIsLeader(cp)) {
            continue;
        }
        int j = i;
        while (j < len) {
            int save = j;
            cp = Utf8CodepointNext(s, len, j);
            if (!BookIsLeader(cp)) {
                j = save;
                break;
            }
        }
        int digits = 0;
        int k = j;
        while (k < len) {
            int save = k;
            cp = Utf8CodepointNext(s, len, k);
            if (!BookIsDigit(cp)) {
                k = save;
                break;
            }
            digits++;
        }
        if (digits >= 5 && k >= len) {
            s[leaderAt] = 0;
            str::TrimWSInPlace(s, str::TrimOpt::Both);
            return;
        }
        i = j > i ? j : i + 1;
    }
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
    float dy = BookVisualHeight(a);
    float bh = BookVisualHeight(b);
    if (bh > dy) {
        dy = bh;
    }
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
                // OCR commonly puts a printed TOC row's title/leaders and
                // right-aligned page number in separate boxes. They still
                // belong to one visual row even with a wide column gap.
                // Do not apply this to vertically stacked body text: that
                // was filtered by BookSameRow() above.
                if (iNum != jNum && gap < 520) {
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
                // Title fragments of one visual row. The gap budget must scale
                // with the raw bbox height: OCR drops glyphs inside large-font
                // chapter banners ("第 一章帮助孩 | 地融入高中生活", gap 37pt at
                // dy 42.9), and a fixed 28pt budget left those halves as two
                // rows - the second then nested as a fake subsection. Keep the
                // 28pt floor for body-size rows and cap the scaled budget so
                // two-column entries never fuse across the column gap.
                float hRaw = page[i].dy > page[j].dy ? page[i].dy : page[j].dy;
                float gapMax = 28.f;
                if (hRaw > 31.f) {
                    gapMax = hRaw * 0.9f;
                    if (gapMax > 56.f) {
                        gapMax = 56.f;
                    }
                }
                if (!iNum && !jNum && gap < gapMax) {
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

struct BookTocPageFeatures {
    int meaningful = 0;
    int entries = 0;
    int leaders = 0;
    int prose = 0;
    int aligned = 0;
    bool heading = false;
    int score = 0;
};

// A printed Contents page is a repeated page layout, not a collection of
// chapter-shaped strings. In particular, a body page's single footer number
// must never turn its chapter heading into a TOC row.
static BookTocPageFeatures BookAnalyzeTocPage(const Vec<ScanLine>& lines, int p) {
    BookTocPageFeatures f;
    if (BookLooksLikeCipPage(lines, p)) {
        return f;
    }
    Vec<BookLine> page;
    BookCollectPage(lines, p, page);
    float rightEdges[64]{};
    int nEdges = 0;
    for (int i = 0; i < page.Size(); i++) {
        const BookLine& ln = page[i];
        const char* s = ln.text;
        if (!s || !s[0]) {
            continue;
        }
        if (BookLooksLikeTocHeading(s)) {
            f.heading = true;
            continue;
        }
        int glyphs = BookGlyphCount(s);
        if (glyphs >= 2 && BookHasLetterOrCjk(s)) {
            f.meaningful++;
            if (glyphs > 52 || BookLooksLikeBodyBlurb(s)) {
                f.prose++;
            }
        }
        bool leader =
            str::Find(s, "......") || str::Find(s, "\xE2\x80\xA6") || str::Find(s, "····") || str::Find(s, "⋯");
        if (leader) {
            f.leaders++;
        }
        // OCR can collapse an entire lesson-style Contents page into one
        // box. Three or more chapter units plus leaders is still a repeated
        // TOC layout, whereas normal body prose does not have that shape.
        int packedLessons = 0;
        int len = (int)str::Len(s);
        for (int at = 0; at < len;) {
            int pos = at;
            int cp = Utf8CodepointNext(s, len, at);
            if (cp == 0x7B2C && BookParseUnit(s + pos).kind == BookUnitKind::Chapter) {
                packedLessons++;
            }
        }
        if (leader && packedLessons >= 3) {
            f.entries += packedLessons;
            f.leaders += packedLessons - 1;
            f.aligned = packedLessons;
            continue;
        }
        char* title = nullptr;
        int printed = 0;
        if (BookSplitInlinePage(s, &title, &printed) && title && printed > 0) {
            f.entries++;
            if (nEdges < dimof(rightEdges)) {
                rightEdges[nEdges++] = ln.x + ln.dx;
            }
        }
        str::Free(title);
    }
    BookFreeLines(page);
    if (nEdges >= 2) {
        for (int i = 0; i < nEdges; i++) {
            int nAlignedHere = 0;
            for (int j = 0; j < nEdges; j++) {
                float d = rightEdges[i] - rightEdges[j];
                if (d < 0) {
                    d = -d;
                }
                if (d <= 24.f) {
                    nAlignedHere++;
                }
            }
            if (nAlignedHere > f.aligned) {
                f.aligned = nAlignedHere;
            }
        }
    }
    int ratio = f.meaningful > 0 ? f.entries * 100 / f.meaningful : 0;
    f.score = f.entries * 12 + ratio / 2 + f.leaders * 3 + f.aligned * 5 - f.prose * 5;
    if (f.score < 0) {
        f.score = 0;
    }
    return f;
}

static bool BookIsTocStartPage(const BookTocPageFeatures& f) {
    // A Contents opener whose rows lost their inline page numbers to OCR
    // (numbers drifting into separate right-edge fragments, leader dots
    // reduced to single '.') still starts the printed Contents when the page
    // carries the 目录/Contents heading ("日录" included, see the matcher)
    // and reads as short rows rather than prose.
    if (f.heading) {
        // Born-digital Contents rows run long (dot leaders inflate the glyph
        // count past the prose threshold), so a heading page that clearly has
        // a repeated title+printed-page layout must not be filtered as prose.
        // The prose check stays for weak heading pages (cover/notice blurbs).
        if (f.entries >= 5 && f.leaders >= 5) {
            return true;
        }
        return (f.entries >= 1 || f.meaningful >= 6) && f.prose * 2 < f.meaningful + 1;
    }
    return f.entries >= 3 && f.score >= 48;
}

static bool BookIsTocContinuationPage(const BookTocPageFeatures& f) {
    // The final Contents page can be short, but it still needs at least two
    // independently parsed title+printed-page rows. A lone body footer fails.
    // Born-digital rows with long dot leaders inflate the prose count (see
    // BookIsTocStartPage), so a strong repeated layout overrides the prose cap.
    // Leaders stay mandatory: the first body page after the Contents opens
    // with the chapter banner plus numbered section headings, which scores
    // high on rows alone but has no dot leaders at all.
    if (f.entries >= 5 && f.leaders >= 5) {
        return true;
    }
    return f.entries >= 2 && f.score >= 30 && f.leaders >= 2 && f.prose * 2 < f.meaningful + 1;
}

static bool BookFindTocRange(const Vec<ScanLine>& lines, int nPages, int* startOut, int* endOut, bool debug = false) {
    int front = nPages < 80 ? nPages : 80;
    // Front-matter pages before the Contents often mimic its shape (design
    // transmittal sheets, distribution lists with "单位+数字" rows). Instead
    // of committing to the first page that looks like a Contents opener,
    // evaluate every candidate run and keep the strongest one.
    int scanLim = front + 48;
    if (scanLim > nPages) {
        scanLim = nPages;
    }
    Vec<BookTocPageFeatures> feats;
    for (int p = 1; p <= scanLim; p++) {
        BookTocPageFeatures f = BookAnalyzeTocPage(lines, p);
        feats.Append(f);
        if (debug) {
            logf(
                "book-toc page=%d entries=%d meaningful=%d leaders=%d aligned=%d prose=%d score=%d start=%d "
                "continue=%d\n",
                p, f.entries, f.meaningful, f.leaders, f.aligned, f.prose, f.score, BookIsTocStartPage(f) ? 1 : 0,
                BookIsTocContinuationPage(f) ? 1 : 0);
        }
    }
    auto at = [&](int p) -> BookTocPageFeatures& { return feats[p - 1]; };

    int bestStart = 0;
    int bestEnd = 0;
    long long bestScore = -1;
    int p = 1;
    while (p <= front) {
        if (!BookIsTocStartPage(at(p))) {
            p++;
            continue;
        }
        // State machine: once the repeated TOC layout ends, never restart it
        // in the body. One weak page is tolerated for OCR damage or a sparse
        // final TOC sheet; two consecutive body pages terminate the region.
        int end = p;
        int misses = 0;
        long long runScore = at(p).score;
        int q = p + 1;
        for (; q <= scanLim; q++) {
            if (BookIsTocContinuationPage(at(q))) {
                end = q;
                misses = 0;
                runScore += at(q).score;
                continue;
            }
            misses++;
            if (misses >= 2) {
                break;
            }
        }
        if (runScore > bestScore) {
            bestScore = runScore;
            bestStart = p;
            bestEnd = end;
        }
        p = end + 1;
    }
    if (bestStart < 1) {
        return false;
    }
    *startOut = bestStart;
    *endOut = bestEnd;
    return true;
}

static void BookWriteTocRangeTrace(const char* path, const Vec<ScanLine>& lines, int nPages, int start, int end) {
    if (!path) {
        return;
    }
    FILE* f = _wfopen(ToWStrTemp(path), L"w");
    if (!f) {
        return;
    }
    fprintf(f, "Book printed TOC range trace\nrange: %d-%d\n\n", start, end);
    int front = nPages < 80 ? nPages : 80;
    for (int p = 1; p <= front; p++) {
        BookTocPageFeatures pf = BookAnalyzeTocPage(lines, p);
        if (pf.score > 0 || pf.heading || (start > 0 && p >= start - 1 && p <= end + 2)) {
            fprintf(f,
                    "page=%d entries=%d meaningful=%d leaders=%d aligned=%d prose=%d score=%d start=%d continue=%d%s\n",
                    p, pf.entries, pf.meaningful, pf.leaders, pf.aligned, pf.prose, pf.score,
                    BookIsTocStartPage(pf) ? 1 : 0, BookIsTocContinuationPage(pf) ? 1 : 0,
                    p >= start && p <= end ? " selected" : "");
        }
    }
    if (start > 0) {
        Vec<BookLine> page;
        BookCollectPage(lines, start, page);
        fprintf(f, "\nCollected visual lines for selected TOC page %d: %d\n", start, page.Size());
        for (int i = 0; i < page.Size(); i++) {
            const BookLine& line = page[i];
            fprintf(f, "L idx=%d x=%.1f y=%.1f dx=%.1f dy=%.1f text=%s\n", i, line.x, line.y, line.dx, line.dy,
                    line.text ? line.text : "");
        }
        BookFreeLines(page);
    }
    fclose(f);
}

// Clickable internal links on a printed Contents page are the strongest
// possible row evidence: the publisher themselves marked the row as an entry
// and told us the destination. Collected once per TOC page.
struct BookLinkHit {
    RectF r;
    int page = 0;
};

struct BookTocRow;

static void BookAppendEntry(Vec<BookTocEntry>& hits, const char* rawTitle, int printed, const BookLine& sl,
                            const char* rawLine = nullptr, const char* reason = nullptr, float conf = -1.f) {
    char* title = BookDupTrim(rawTitle);
    if (!title) {
        return;
    }
    BookStripLeadersInPlace(title);
    BookStripBrokenTrailingLeaderNumber(title);
    BookStripTrailingTocStop(title);
    int fromTitle = StripBookPrintedPageFromTitle(title);
    if (printed < 1 && fromTitle > 0) {
        printed = fromTitle;
    }
    str::TrimWSInPlace(title, str::TrimOpt::Both);
    // A linked row is publisher-marked; accept it even if its shape looks
    // unusual (short title, no printed number).
    if (!sl.linkPage &&
        (!title[0] || BookLooksLikeTocHeading(title) || BookLooksLikeJunk(title) || BookLooksLikeBodyBlurb(title) ||
         BookLooksLikePageFooter(title) || BookLineIsPageNum(title))) {
        str::Free(title);
        return;
    }
    if (!sl.linkPage && BookGlyphCount(title) < 2) {
        str::Free(title);
        return;
    }
    BookTocEntry h;
    h.title = title;
    h.printedPage = printed;
    h.linkPage = sl.linkPage;
    h.linkFixed = sl.linkPage > 0;
    if (h.linkFixed) {
        h.pdfPage = h.linkPage;
        h.bodyMatched = true;
    }
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

// Some low-resolution scans are recognized as one long text box even though
// the image plainly contains a multi-row Contents list. Do not run this on
// prose: require several repeated Chinese lesson headings in that one box.
static bool BookParsePackedLessonTocLine(const BookLine& sl, Vec<BookTocEntry>& hits) {
    if (!sl.text) {
        return false;
    }
    int starts[32];
    int nStarts = 0;
    int len = (int)str::Len(sl.text);
    for (int i = 0; i < len;) {
        int at = i;
        int cp = Utf8CodepointNext(sl.text, len, i);
        if (cp == 0x7B2C && nStarts < dimof(starts)) { // 第
            BookUnit unit = BookParseUnit(sl.text + at);
            if (unit.kind == BookUnitKind::Chapter) {
                starts[nStarts++] = at;
            }
        }
    }
    if (nStarts < 3) {
        return false;
    }
    int added = 0;
    for (int i = 0; i < nStarts; i++) {
        int end = i + 1 < nStarts ? starts[i + 1] : len;
        char* raw = str::Dup(sl.text + starts[i], end - starts[i]);
        char* title = nullptr;
        int printed = 0;
        if (BookSplitInlinePage(raw, &title, &printed) && title && printed > 0) {
            BookAppendEntry(hits, title, printed, sl, raw, "packed TOC OCR row", 0.72f);
            added++;
        }
        str::Free(title);
        str::Free(raw);
    }
    return added >= 2;
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

static void BookCollectPageLinks(EngineBase* engine, int page, Vec<BookLinkHit>& out) {
    if (!engine || page < 1 || page > engine->PageCount()) {
        return;
    }
    Vec<IPageElement*> elements = EngineMupdfGetPageElementsForExtraction(engine, page);
    for (IPageElement* el : elements) {
        IPageDestination* dest = el && el->IsLink() ? el->AsLink() : nullptr;
        if (!dest || dest->pageNo < 1 || dest->pageNo > engine->PageCount()) {
            continue;
        }
        BookLinkHit hit;
        hit.r = el->GetRect();
        hit.page = dest->pageNo;
        out.Append(hit);
    }
}

// A link whose rect overlaps the row's band marks the row as a real entry
// and hands us the destination page directly.
static int BookRowLinkPage(const Vec<BookLinkHit>& links, const BookTocRow& row) {
    for (const BookLinkHit& lh : links) {
        if (lh.r.x + lh.r.dx < row.x - 3 || lh.r.x > row.x + row.dx + 3) {
            continue;
        }
        if (lh.r.y + lh.r.dy < row.y - 4 || lh.r.y > row.y + row.dy + 4) {
            continue;
        }
        return lh.page;
    }
    return 0;
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

// "IPV6" / "H.264" / "B12" / "COVID-19" — a digit run that extends a Latin
// token is part of that word, not a glued printed page. Splitting there
// shredded "7.10.4 IPV6 地址规划" into "7.10.4 IPV" + page 6 + "地址规划".
// Walk back over the token's [0-9.-] prefix; a Latin letter there means the
// number is word-embedded. Page numbers glued to the preceding title touch
// CJK glyphs ("总论12第二章"), not Latin ones.
static bool BookDigitRunExtendsLatinToken(const char* s, int digStart) {
    if (!s || digStart < 1) {
        return false;
    }
    int i = digStart;
    for (;;) {
        int cp = Utf8CodepointPrev(s, digStart, i);
        if (cp == 0) {
            return false; // walked to the start of the string
        }
        if (BookIsDigit(cp) || cp == '.' || cp == '-') {
            continue;
        }
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    }
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
    // "IPV6", "H.264", "B12": the digit run extends a Latin token, so it is
    // part of that word and never an isolated printed page.
    if (BookDigitRunExtendsLatinToken(s, save)) {
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
        // A bare number immediately followed by 年 is a year ("2021 年度",
        // "2021 年政务…"), not a glued printed page. Splitting there shreds
        // titles into junk rows ("年度", "年政务…").
        bool yearFollows = false;
        if (save > 0) {
            int j = save;
            BookSkipWs(s, len, j);
            while (j < len) {
                int dj = j;
                int dcp = Utf8CodepointNext(s, len, j);
                if (!BookIsDigit(dcp)) {
                    j = dj;
                    break;
                }
            }
            int k = j;
            BookSkipWs(s, len, k);
            if (k < len) {
                int kcp = k;
                yearFollows = Utf8CodepointNext(s, len, kcp) == 0x5E74; // 年
            }
        }
        if (save > 0 && !yearFollows && BookTakeBarePage(s, len, save, &page, &after) && BookRangeHasLetterOrCjk(s, 0, save) &&
            BookRangeHasLetterOrCjk(s, after, len) && !BookLeftEndsWithDi(s, save) &&
            !BookRightStartsWithUnitWord(s, after, len)) {
            // A number directly followed by a colon is a label ("附表1：...",
            // "图3：..."), not a glued printed page. Splitting there shreds the
            // title into "附表" + "：..." junk rows.
            {
                int cj = after;
                BookSkipWs(s, len, cj);
                if (cj < len) {
                    int ccp = Utf8CodepointNext(s, len, cj);
                    if (ccp == 0xFF1A || ccp == ':') {
                        i = save;
                        int nSkip = 0;
                        while (i < len) {
                            int s2 = i;
                            int c2 = Utf8CodepointNext(s, len, i);
                            if (!BookIsDigit(c2)) {
                                i = s2;
                                break;
                            }
                            nSkip++;
                        }
                        if (nSkip < 1) {
                            Utf8CodepointNext(s, len, i);
                        }
                        continue;
                    }
                }
            }
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

static void BookParseTocPage(Vec<BookLine>& page, Vec<BookTocEntry>& hits, const Vec<BookLinkHit>& links) {
    float pageW = BookPageWidth(page);
    BookSortVisual(page);
    for (int i = 0; i < page.Size(); i++) {
        if (page[i].text && !page[i].used && BookParsePackedLessonTocLine(page[i], hits)) {
            page[i].used = true;
            continue;
        }
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
            // A colon label binds the dash suffix to the same entry
            // ("附表6：情报指挥中心配套改造估算表-配套拆除" is one table, not
            // a chapter + dash subtitle pair).
            if (dashAt >= 3) {
                bool hasColonLabel = false;
                for (int q = 0; q < dashAt && !hasColonLabel; ) {
                    int qcp = Utf8CodepointNext(rows[r].title, dashAt, q);
                    if (qcp == 0xFF1A || qcp == ':') {
                        hasColonLabel = true;
                    }
                }
                if (hasColonLabel) {
                    dashAt = -1;
                }
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

    // Rows above the 目录/Contents heading on its page are page furniture
    // (document title, running header), not TOC entries.
    int headingPage = 0;
    float headingY = 0;
    for (int r = 0; r < rows.Size(); r++) {
        if (rows[r].title && BookLooksLikeTocHeading(rows[r].title)) {
            headingPage = rows[r].page;
            headingY = rows[r].y;
            break;
        }
    }
    for (int r = 0; r < rows.Size(); r++) {
        BookTocRow& row = rows[r];
        int linkPage = BookRowLinkPage(links, row);
        // A GoTo link on the row is publisher evidence that this is a real
        // entry: it overrides junk/keep heuristics and page furniture drops.
        if (!row.title) {
            continue;
        }
        if (BookLooksLikeTocHeading(row.title) || BookLooksLikePageFooter(row.title)) {
            continue;
        }
        if (!linkPage &&
            (BookLooksLikeJunk(row.title) || BookLooksLikeBodyBlurb(row.title) ||
             (headingPage > 0 && row.page == headingPage && row.y < headingY))) {
            continue;
        }
        // On a publisher-linked TOC page every real entry is clickable (wrap
        // continuation rows carry the row's link too). A row without a link is
        // page furniture (running document title on a later TOC page) unless
        // it carries its own strong entry shape.
        if (!linkPage && links.Size() > 0 && !BookStartsWithListNumber(row.title) && !BookIsStructTitle(row.title) &&
            !BookIsPartTitle(row.title) && !BookIsXinDe(row.title)) {
            continue;
        }
        int g = BookGlyphCount(row.title);
        bool keep = BookIsStructTitle(row.title) || BookIsXinDe(row.title) || BookIsPartTitle(row.title) ||
                    row.printedPage > 0 || BookStartsWithListNumber(row.title);
        if (!keep && !linkPage && (g > 22 || g < 2 || !BookHasLetterOrCjk(row.title))) {
            continue;
        }
        BookLine sl;
        sl.text = row.title;
        sl.page = row.page;
        sl.linkPage = linkPage;
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

// Use only content glyphs for duplicate detection. OCR commonly alternates
// between fullwidth and ASCII punctuation in the two boxes for the same row.
static int BookTitleKeyGlyphs(const char* s, int out[192]) {
    if (!s) {
        return 0;
    }
    int len = (int)str::Len(s);
    int n = 0;
    for (int i = 0; i < len && n < 192;) {
        int cp = Utf8CodepointNext(s, len, i);
        bool keep =
            BookIsDigit(cp) || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x4E00 && cp <= 0x9FFF);
        if (keep) {
            out[n++] = cp;
        }
    }
    return n;
}

static bool BookTitlesSameOrContainedLoose(const char* a, const char* b) {
    int ka[192];
    int kb[192];
    int na = BookTitleKeyGlyphs(a, ka);
    int nb = BookTitleKeyGlyphs(b, kb);
    if (na < 2 || nb < 2) {
        return false;
    }
    if (na == nb) {
        for (int i = 0; i < na; i++) {
            if (ka[i] != kb[i]) {
                return false;
            }
        }
        return true;
    }
    // A duplicate OCR box can lose a short unit prefix (e.g. "第十二课")
    // or append a stray leader/page fragment. Only fold that containment when
    // the two forms are close in length.
    const int* shortKey = na < nb ? ka : kb;
    const int* longKey = na < nb ? kb : ka;
    int ns = std::min(na, nb);
    int nl = std::max(na, nb);
    if (nl - ns > 5) {
        return false;
    }
    for (int at = 0; at <= nl - ns; at++) {
        int i = 0;
        for (; i < ns; i++) {
            if (shortKey[i] != longKey[at + i]) {
                break;
            }
        }
        if (i == ns) {
            return true;
        }
    }
    return false;
}

// OCR can expose one printed-TOC row both as a packed text box and as its
// individual title/page boxes. Both parsing paths are useful for damaged
// scans, but they must not turn into two identical bookmarks. OCR often gets
// the title from both boxes but gets the printed page from only one of them.
// Restrict this to one source page; different known printed destinations on
// that page remain separate in case the book really repeats a title.
static void BookDropExactDuplicateEntries(Vec<BookTocEntry>& hits) {
    int dropped = 0;
    for (int i = 0; i < hits.Size(); i++) {
        for (int j = i + 1; j < hits.Size();) {
            bool samePrinted =
                hits[i].printedPage > 0 && hits[j].printedPage > 0 && hits[i].printedPage != hits[j].printedPage;
            bool same = hits[i].srcPage == hits[j].srcPage && !samePrinted &&
                        BookTitlesSameOrContainedLoose(hits[i].title, hits[j].title);
            if (!same) {
                j++;
                continue;
            }
            // A known printed page is stronger evidence than OCR confidence.
            // Only compare confidence when both candidates have (or lack) it.
            bool preferJ = hits[j].printedPage > 0 && hits[i].printedPage < 1;
            preferJ =
                preferJ || (hits[j].printedPage == hits[i].printedPage && hits[j].confidence > hits[i].confidence);
            if (preferJ) {
                BookTocEntry t = hits[i];
                hits[i] = hits[j];
                hits[j] = t;
            }
            str::Free(hits[j].title);
            str::Free(hits[j].raw);
            str::Free(hits[j].reason);
            hits.RemoveAt(j);
            dropped++;
        }
    }
    if (dropped > 0) {
        logf("Book TOC: removed exact duplicate printed rows=%d\n", dropped);
    }
}

// A damaged page-number token can differ between two OCR layers. Once both
// rows have been mapped, the PDF destination is the authoritative identity.
// This catches e.g. "第五节假言判断……" and "第五节假言判断" without merging
// same-titled entries that genuinely point to different pages.
static void BookDropMappedDuplicateEntries(Vec<BookTocEntry>& hits) {
    int dropped = 0;
    for (int i = 0; i < hits.Size(); i++) {
        for (int j = i + 1; j < hits.Size();) {
            bool same = hits[i].srcPage == hits[j].srcPage && hits[i].pdfPage > 0 &&
                        hits[i].pdfPage == hits[j].pdfPage &&
                        BookTitlesSameOrContainedLoose(hits[i].title, hits[j].title);
            if (!same) {
                j++;
                continue;
            }
            bool preferJ = hits[j].printedPage > 0 && hits[i].printedPage < 1;
            preferJ =
                preferJ || (hits[j].printedPage == hits[i].printedPage && hits[j].confidence > hits[i].confidence);
            if (preferJ) {
                BookTocEntry t = hits[i];
                hits[i] = hits[j];
                hits[j] = t;
            }
            str::Free(hits[j].title);
            str::Free(hits[j].raw);
            str::Free(hits[j].reason);
            hits.RemoveAt(j);
            dropped++;
        }
    }
    if (dropped > 0) {
        logf("Book TOC: removed mapped duplicate printed rows=%d\n", dropped);
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
    // Large-font chapter recovery. Scanned TOCs set chapter banners in a much
    // larger font, and OCR routinely eats the "第X章" prefix ("章教会孩子..." /
    // "子健康生活的坚实后盾" / "力孩子度渡叛逆期" all parsed as unnumbered L3
    // rows). Once the book is known to use 章 structure, an unnumbered row
    // with real geometry that is markedly larger than the median body row,
    // carries no printed page, and reads as a title (>= 4 glyphs, no sentence
    // punctuation - intro prose bleeds in with oversized boxes but always
    // carries 。， etc.) is a chapter whose prefix was truncated. Books without
    // any 第X章 keep the banner logic untouched.
    int chapLvl = hasPart ? 2 : 1;
    float medFont = 0;
    Vec<int> promoted;
    if (nChap >= 1) {
        Vec<float> body;
        for (int i = 0; i < n; i++) {
            if (hits[i].printedPage >= 1 && hits[i].fontSize > 1.f) {
                body.Append(hits[i].fontSize);
            }
        }
        if (body.Size() >= 4) {
            for (int a = 1; a < body.Size(); a++) { // insertion sort, n is small
                float v = body[a];
                int b = a - 1;
                while (b >= 0 && body[b] > v) {
                    body[b + 1] = body[b];
                    b--;
                }
                body[b + 1] = v;
            }
            medFont = body[body.Size() / 2];
        }
    }
    for (int i = 0; i < n; i++) {
        bool prom = false;
        if (medFont > 1.f) {
            prom = BookParseUnit(hits[i].title).kind == BookUnitKind::None && !isBanner[i] && hits[i].printedPage < 1 &&
                   hits[i].srcX > 1.f && hits[i].fontSize > 1.f && hits[i].fontSize >= medFont * 1.55f &&
                   BookGlyphCount(hits[i].title) >= 4 && !BookHasSentencePunct(hits[i].title);
        }
        promoted.Append(prom ? 1 : 0);
    }
    // Effective structural level per row (0 = content row), used by the
    // indent-band walk so promoted banners act as chapter anchors there too.
    Vec<int> outline;
    for (int i = 0; i < n; i++) {
        BookUnitKind kind2 = BookParseUnit(hits[i].title).kind;
        int lv = 0;
        if (kind2 == BookUnitKind::Part || isBanner[i]) {
            lv = 1;
        } else if (kind2 == BookUnitKind::Chapter || kind2 == BookUnitKind::Section) {
            lv = BookStructOutlineLevel(kind2, hasPart, hasChap);
        } else if (promoted[i]) {
            lv = chapLvl;
        }
        outline.Append(lv);
    }
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
    // Running 第X章 anchor for multi-dot rows: "1.4" ties to chapter 1 by
    // number, not by layout, so its depth reliably nests under that chapter.
    int chapNum = 0;
    int chapLvlCur = 1;
    int lastScheme = 0;
    int lastSchemeLvl = 0;
    bool prevWasChap = false;
    // Level-decision provenance counters: they make the mix of signals
    // observable in the trace so new failure patterns surface as skewed
    // ratios instead of silent mis-nesting.
    int nDecidedStruct = 0;
    int nDecidedMultidot = 0;
    int nDecidedBandPeer = 0;
    int nDecidedBandChild = 0;
    int nDecidedBandLeft = 0;
    int nDecidedOther = 0;
    int nDecidedCapped = 0;
    // (firstSeg, depth, level) triples of earlier multi-dot rows, used to
    // chain-resolve decimal outlines in books that have no structural units.
    Vec<int> multiHist;
    for (int i = 0; i < n; i++) {
        const char* lvlWhy = nullptr;
        BookUnit unit = BookParseUnit(hits[i].title);
        BookUnitKind kind = unit.kind;
        bool banner = isBanner[i] != 0;
        bool promotedChap = promoted[i] != 0;
        bool subtitle = kind == BookUnitKind::None && !banner && !promotedChap && prevWasChap &&
                        BookLooksLikeChapSubtitle(hits[i].title);
        int scheme = BookEntryScheme(hits[i].title, nullptr);
        // Indent-band hierarchy for unnumbered rows, resolved against the
        // nearest preceding structural row instead of a global nearest-x
        // match. Structural rows drift across scanned TOC pages (one book
        // had 章 rows at x=62..119 and 节 rows at x=78..84), so a fixed
        // global tolerance matched content rows to the wrong band and made
        // them siblings of the chapter. Sequence-local rules: within a
        // small tolerance of the predecessor's left edge the row is that
        // row's peer; clearly right of it nests one level deeper; clearly
        // left of it belongs to an earlier, shallower band. Only real page
        // coordinates, so synthetic/text-only inputs keep their old rules.
        int bandLevel = 0;
        bool bandPeer = false;
        bool bandLeft = false;
        if (kind == BookUnitKind::None && !banner && !promotedChap && hits[i].srcX > 1.f) {
            const float peerTol = 5.f;
            int k = i - 1;
            while (k >= 0) {
                int otherLevel = outline[k];
                float otherX = hits[k].srcX;
                k--;
                if (otherLevel <= 0 || otherX <= 1.f) {
                    continue;
                }
                float dx = hits[i].srcX - otherX;
                if (dx < -peerTol) {
                    // Left of its predecessor: peer of a shallower band.
                    // Walk back to the nearest earlier row it reaches.
                    bandLeft = true;
                    while (k >= 0) {
                        int lv2 = outline[k];
                        float otherX2 = hits[k].srcX;
                        k--;
                        if (lv2 <= 0 || otherX2 <= 1.f) {
                            continue;
                        }
                        if (hits[i].srcX <= otherX2 + peerTol) {
                            bandLevel = lv2;
                            bandPeer = true;
                        } else {
                            bandLevel = lv2 + 1;
                        }
                        break;
                    }
                } else if (dx <= peerTol) {
                    bandLevel = otherLevel; // same band: peer of that row
                    bandPeer = true;
                } else {
                    bandLevel = otherLevel + 1; // indented under that row
                }
                break;
            }
        }
        // Multi-dot numbering ("1.1", "1.4.1") declares outline depth
        // independently of layout; decided ahead of the indent bands below,
        // which misread flush-left first-chapter children as the chapter's
        // siblings.
        int multiDotDepth = 0;
        int multiDotFirst = 0;
        bool isMultiDot = kind == BookUnitKind::None && !banner && !promotedChap &&
                          BookParseMultiDotPrefix(hits[i].title, &multiDotDepth, &multiDotFirst) > 0;
        // Books with no structural units at all (no 篇/章/节 rows) are pure
        // decimal outlines: match the nearest earlier row with the same first
        // segment - equal depth is a sibling, one less is its parent.
        int chainLvl = 0;
        bool chainBook = !hasPart && !hasChap && nSec == 0;
        if (isMultiDot && chainBook) {
            for (int h = multiHist.Size() - 3; h >= 0; h -= 3) {
                if (multiHist[h] != multiDotFirst) {
                    continue;
                }
                if (multiHist[h + 1] == multiDotDepth) {
                    chainLvl = multiHist[h + 2];
                    break;
                }
                if (multiHist[h + 1] == multiDotDepth - 1) {
                    chainLvl = multiHist[h + 2] + 1;
                    break;
                }
                if (multiHist[h + 1] < multiDotDepth - 1) {
                    break; // the intermediate parent never appeared
                }
            }
        }
        int lvl = leftoverBase;
        if (kind == BookUnitKind::Part || banner) {
            lvl = 1;
            containerLvl = 1;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
            nDecidedStruct++;
        } else if (kind == BookUnitKind::Chapter || kind == BookUnitKind::Section) {
            lvl = BookStructOutlineLevel(kind, hasPart, hasChap);
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = kind == BookUnitKind::Chapter;
            if (kind == BookUnitKind::Chapter) {
                chapNum = unit.number;
                chapLvlCur = lvl;
            }
            nDecidedStruct++;
        } else if (promotedChap) {
            lvl = chapLvl;
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = true;
            lvlWhy = "lvl:bigfont-chapter";
            nDecidedStruct++;
        } else if (isMultiDot && chapNum > 0 && multiDotFirst == chapNum) {
            // Numbering wins over band geometry: "1.4" under 第一章 is depth
            // 2 => one level below the chapter, "1.4.1" is depth 3 => two.
            lvl = chapLvlCur + (multiDotDepth - 1);
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
            lvlWhy = "lvl:multidot";
            nDecidedMultidot++;
        } else if (isMultiDot && chainLvl > 0) {
            lvl = chainLvl;
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
            lvlWhy = "lvl:multidot-chain";
            nDecidedMultidot++;
        } else if (bandLevel > 0) {
            lvl = bandLevel;
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
            if (bandLeft) {
                lvlWhy = "lvl:band-left";
                nDecidedBandLeft++;
            } else if (bandPeer) {
                lvlWhy = "lvl:band-peer";
                nDecidedBandPeer++;
            } else {
                lvlWhy = "lvl:band-child";
                nDecidedBandChild++;
            }
        } else if (BookIsXinDe(hits[i].title) && hasPart) {
            lvl = 2;
            containerLvl = 2;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
            lvlWhy = "lvl:xinde";
            nDecidedOther++;
        } else if (subtitle) {
            lvl = containerLvl + 1;
            if (lvl < 2) {
                lvl = 2;
            }
            containerLvl = lvl;
            lastScheme = 0;
            lastSchemeLvl = 0;
            prevWasChap = false;
            lvlWhy = "lvl:subtitle";
            nDecidedOther++;
        } else if (scheme > 0 && lastScheme == scheme && lastSchemeLvl > 0) {
            lvl = lastSchemeLvl;
            prevWasChap = false;
            lvlWhy = "lvl:scheme-cont";
            nDecidedOther++;
        } else if (scheme > 0) {
            lvl = containerLvl + 1;
            prevWasChap = false;
            lvlWhy = "lvl:scheme-new";
            nDecidedOther++;
        } else {
            // Unnumbered leftovers stay one step under the current 章/课.
            // Indent-chasing turned printed pages like "(144)" / "(145)" into a
            // fake deep outline; books do not nest that way (公文 does).
            lvl = containerLvl + 1;
            prevWasChap = false;
            lvlWhy = "lvl:leftover";
            nDecidedOther++;
        }
        if (lvl < 1) {
            lvl = 1;
        }
        bool absolute = kind != BookUnitKind::None || banner || promotedChap;
        if (!absolute && lvl > prev + 1) {
            lvl = prev + 1;
            lvlWhy = "lvl:cap";
            nDecidedCapped++;
        }
        if (lvl > 6) {
            lvl = 6;
        }
        hits[i].inferredLevel = lvl;
        if (isMultiDot && chainBook) {
            multiHist.Append(multiDotFirst);
            multiHist.Append(multiDotDepth);
            multiHist.Append(lvl);
        }
        if (lvlWhy) {
            char reason[320];
            reason[0] = 0;
            if (hits[i].reason) {
                BookBufCat(reason, (int)sizeof(reason), hits[i].reason, nullptr);
            }
            BookReasonAdd(reason, (int)sizeof(reason), lvlWhy);
            str::Free(hits[i].reason);
            hits[i].reason = BookDupTrim(reason);
        }
        prev = lvl;
        if (scheme > 0) {
            lastScheme = scheme;
            lastSchemeLvl = lvl;
        }
    }
    logf("BookAssignLevels: n=%d struct=%d multidot=%d band-peer=%d band-child=%d band-left=%d other=%d capped=%d\n", n,
         nDecidedStruct, nDecidedMultidot, nDecidedBandPeer, nDecidedBandChild, nDecidedBandLeft, nDecidedOther,
         nDecidedCapped);
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

static int BookCollectSpaceFreeCps(const char* s, int* out, int cap) {
    if (!s) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    int n = 0;
    while (i < len && n < cap) {
        int cp = Utf8CodepointNext(s, len, i);
        if (cp <= 32 || cp == 0x3000) {
            continue;
        }
        out[n++] = cp;
    }
    return n;
}

// OCR inserts spaces at random and drops glyphs inside titles ("第 一章帮助
// 孩地融入高中生活"), so exact substring search fails. Compare on the
// whitespace-free codepoint runs instead.
static int BookSpaceFreePrefixRun(const int* a, int na, const int* b, int nb) {
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return n;
}

// Longest common subsequence in glyphs; OCR scrambles single glyphs
// ("度过" -> "度渡") inside otherwise intact titles. Inputs are capped small.
static int BookGlyphLcs(const int* a, int na, const int* b, int nb) {
    int prev[129] = {0};
    int cur[129] = {0};
    for (int i = 1; i <= na; i++) {
        for (int j = 1; j <= nb; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
            } else {
                cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
            }
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(nb + 1));
    }
    return prev[nb];
}

// Score a body line as a reading-order anchor for a TOC title whose OCR text
// may be truncated, space-glitched or scrambled. Exact / substring matches
// win; otherwise a long whitespace-free common prefix or a dominant glyph
// subsequence counts.
static int BookBodyAnchorScore(const char* body, const char* title) {
    int sc = BookTitleMatchScore(body, title);
    if (sc > 0) {
        return sc;
    }
    const int kCap = 128;
    int cb[128];
    int ct[128];
    int nb = BookCollectSpaceFreeCps(body, cb, kCap);
    int nt = BookCollectSpaceFreeCps(title, ct, kCap);
    if (nt < 4) {
        return 0;
    }
    int run = BookSpaceFreePrefixRun(cb, nb, ct, nt);
    if (run >= 6) {
        return run;
    }
    int lcs = BookGlyphLcs(ct, nt, cb, nb);
    if (lcs >= 5 && lcs * 2 >= nt) {
        return lcs;
    }
    return 0;
}

static void BookApplyLineDest(BookTocEntry& hit, const ScanLine& sl);
static bool BookDestIsTocPage(int page, int tocStart, int tocEnd);

// Body-anchor evidence tiers. A real heading line is set in a larger font
// than the page's body text (chapter openers run 2-3x body size); running
// headers, prose references and book-end appendix lists quote the very same
// titles in body size, and scans often carry no text on the real opener page
// at all. Anchoring therefore prefers heading-size evidence and ignores
// body-size matches on pages that read like a secondary contents list.
struct BookAnchorHit {
    int score = 0;
    int page = 0;
    int line = -1;
    bool inExtra = false;
    bool strong = false;
};

struct BookPageFontStats {
    float median = 0;          // body text size on the page
    int nStruct = 0;           // lines with a 第X章/节/篇 prefix
    bool strongStruct = false; // one of them set in heading size
};

static bool BookIsHeadingSizeLine(float fontSize, float pageMedian) {
    // Display-size evidence. OCR text layers report unreliable font sizes for
    // prose and running headers alike (relative-to-page thresholds misfire on
    // opener pages whose intro text is set nearly as large as the title), but
    // real display titles are always set >= ~2.5x the common body size.
    return fontSize >= 26.f;
}

static float BookMedianOf(Vec<float>& v) {
    int n = v.Size();
    if (n < 1) {
        return 0.f;
    }
    for (int a = 1; a < n; a++) {
        float val = v[a];
        int b = a - 1;
        while (b >= 0 && v[b] > val) {
            v[b + 1] = v[b];
            b--;
        }
        v[b + 1] = val;
    }
    return v[n / 2];
}

// Per-page body font median + structural line census across both scan sets.
static void BookCollectPageFontStats(const Vec<ScanLine>& a, const Vec<ScanLine>& b, int nPages,
                                     Vec<BookPageFontStats>& out) {
    out.Reset();
    BookPageFontStats none;
    for (int p = 0; p <= nPages + 1; p++) {
        out.Append(none);
    }
    Vec<float> fonts;
    Vec<float> structFonts;
    for (int p = 1; p <= nPages; p++) {
        fonts.Reset();
        structFonts.Reset();
        for (int pass = 0; pass < 2; pass++) {
            const Vec<ScanLine>& ls = pass == 0 ? a : b;
            for (int k = 0; k < ls.Size(); k++) {
                const ScanLine& sl = ls[k];
                if (sl.srcPage != p) {
                    continue;
                }
                if (sl.fontSize > 1.f) {
                    fonts.Append(sl.fontSize);
                }
                if (sl.text && BookParseUnit(sl.text).kind != BookUnitKind::None) {
                    structFonts.Append(sl.fontSize > 1.f ? sl.fontSize : 0.f);
                }
            }
        }
        if (fonts.Size() < 1 && structFonts.Size() < 1) {
            continue;
        }
        BookPageFontStats st;
        st.median = BookMedianOf(fonts);
        st.nStruct = structFonts.Size();
        for (int k = 0; k < structFonts.Size(); k++) {
            if (BookIsHeadingSizeLine(structFonts[k], st.median)) {
                st.strongStruct = true;
                break;
            }
        }
        out[p] = st;
    }
}

// Pages quoting >= 2 structural titles without any heading-size structural
// line are a secondary contents list (book-end appendix, 目录 recap); they
// match every row perfectly and must not anchor body-size (weak) matches.
static bool BookWeakAnchorsBlockedOnPage(const BookPageFontStats& st) {
    return st.nStruct >= 2 && !st.strongStruct;
}

static bool BookAnchorTraceEnabled() {
    static int gTrace = -1;
    if (gTrace < 0) {
        char buf[8]{};
        gTrace = GetEnvironmentVariableA("SUMATRA_TOC_TRACE", buf, dimof(buf)) > 0 ? 1 : 0;
    }
    return gTrace == 1;
}

static void BookBestAnchorInVec(const Vec<ScanLine>& ls, const char* title, int lastAnchor, int tocStart, int tocEnd,
                                const Vec<BookPageFontStats>& stats, bool inExtra, BookAnchorHit* io) {
    for (int k = 0; k < ls.Size(); k++) {
        const ScanLine& sl = ls[k];
        if (!sl.text || sl.srcPage <= lastAnchor || BookDestIsTocPage(sl.srcPage, tocStart, tocEnd)) {
            continue;
        }
        int sc = BookBodyAnchorScore(sl.text, title);
        if (sc < 6) {
            continue;
        }
        const BookPageFontStats& st = sl.srcPage < stats.Size() ? stats[sl.srcPage] : BookPageFontStats();
        bool strong = BookIsHeadingSizeLine(sl.fontSize, st.median);
        if (!strong && BookWeakAnchorsBlockedOnPage(st)) {
            continue;
        }
        bool better;
        if (io->line < 0) {
            better = true;
        } else if (strong != io->strong) {
            better = strong;
        } else if (sc != io->score) {
            better = sc > io->score;
        } else {
            better = sl.srcPage < io->page;
        }
        if (better) {
            io->score = sc;
            io->page = sl.srcPage;
            io->line = k;
            io->inExtra = inExtra;
            io->strong = strong;
        }
    }
}

// Chapter banners and other OCR-mangled rows can end up with no destination:
// banners carry no printed page, and the body ScanLine set may be
// front-capped (an embedded OCR text layer makes the scan classify as
// born-digital, so only the first ~80 pages were collected). Collect the
// remaining pages and resolve each still-unresolved row against the first
// body anchor after the previous resolved row. In TOC reading order a
// chapter's title headers begin on the chapter's opener page, so ties
// (running headers repeat on every page of the chapter) resolve to the
// earliest page - exactly the destination a TOC banner needs. Exact printed
// offsets stay authoritative: rows with a destination are never touched.
// The TOC scan set is front-capped (an embedded OCR text layer makes the scan
// classify as born-digital, so only the first ~80 pages were collected).
// Chapter anchoring needs the whole body, so collect the remaining pages once
// and share them (plus the per-page font stats) with every anchor pass.
static bool BookCollectExtraScanLines(EngineBase* engine, const Vec<ScanLine>& lines, int nPages,
                                      Vec<ScanLine>& extra, const TocExtractProgress* prog) {
    extra.Reset();
    if (!engine || nPages < 2) {
        return true;
    }
    int lastCovered = 0;
    for (int i = 0; i < lines.Size(); i++) {
        if (lines[i].srcPage > lastCovered) {
            lastCovered = lines[i].srcPage;
        }
    }
    if (lastCovered >= 1 && lastCovered < nPages) {
        // The printed-TOC scan usually stops at the front-page cap, so this
        // collects the remaining body pages (hundreds on big scans). Report
        // each page: this stage is the long tail the progress bar must show,
        // and it stays cancellable.
        int total = nPages - lastCovered;
        int done = 0;
        logf("TOC extract stage bodyCollect start pages=%d..%d\n", lastCovered + 1, nPages);
        for (int p = lastCovered + 1; p <= nPages; p++) {
            if (TocExtractCancelled(prog)) {
                logf("TOC extract stage bodyCollect cancelled done=%d\n", done);
                return false;
            }
            PtocCollectPageScanLines(engine, p, extra);
            done++;
            TocExtractReportProgress(prog, done, total, true);
        }
        logf("TOC extract stage bodyCollect done pages=%d lines=%d\n", done, extra.Size());
    }
    return true;
}

static void BookResolveMissingDestsByBody(const Vec<ScanLine>& lines, const Vec<ScanLine>& extra,
                                          const Vec<BookPageFontStats>& stats, int tocStart, int tocEnd, int nPages,
                                          Vec<BookTocEntry>& hits) {
    if (nPages < 2 || hits.Size() < 1) {
        return;
    }
    int nUnresolved = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].pdfPage < 1 && hits[i].title) {
            nUnresolved++;
        }
    }
    if (nUnresolved < 1) {
        return;
    }
    logf("BookResolveMissingDestsByBody: unresolved=%d nPages=%d lines=%d extra=%d toc=%d..%d\n", nUnresolved, nPages,
         lines.Size(), extra.Size(), tocStart, tocEnd);
    if (BookAnchorTraceEnabled()) {
        FILE* ef = fopen("c:\\src\\sumatrapdf\\_toc_bench\\extra-lines.txt", "w");
        if (ef) {
            for (int k = 0; k < extra.Size(); k++) {
                const char* t = extra[k].text ? extra[k].text : "";
                fprintf(ef, "p=%d x=%.1f y=%.1f fs=%.1f | %s\n", extra[k].srcPage, extra[k].x, extra[k].y,
                        extra[k].fontSize, t);
            }
            fclose(ef);
        }
    }
    int lastAnchor = 0;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].pdfPage > lastAnchor) {
            lastAnchor = hits[i].pdfPage;
        }
        if (hits[i].pdfPage >= 1 || !hits[i].title) {
            continue;
        }
        BookAnchorHit best;
        BookBestAnchorInVec(lines, hits[i].title, lastAnchor, tocStart, tocEnd, stats, false, &best);
        if (extra.Size() > 0) {
            BookBestAnchorInVec(extra, hits[i].title, lastAnchor, tocStart, tocEnd, stats, true, &best);
        }
        if (best.line >= 0) {
            BookApplyLineDest(hits[i], best.inExtra ? extra[best.line] : lines[best.line]);
        }
        if (BookAnchorTraceEnabled() && hits[i].title) {
            const ScanLine& sl = best.line >= 0 ? (best.inExtra ? extra[best.line] : lines[best.line]) : ScanLine{};
            logf("anchor '%s' -> p%d sc=%d strong=%d fs=%.1f med=%.1f\n", hits[i].title, best.page, best.score,
                 (int)best.strong, best.line >= 0 ? sl.fontSize : 0.f,
                 best.page > 0 && best.page < stats.Size() ? stats[best.page].median : 0.f);
        }
    }
}

// --- Chapter dest refinement + title prefix repair -------------------------
//
// The scanned-TOC path assigns a chapter row a dest by layout estimation when
// its printed page was lost to OCR. Those estimates can be tens of pages off,
// and they are final: BookResolveMissingDestsByBody only fills rows that have
// no dest at all. The opener page itself carries the strongest possible
// evidence - the chapter banner in display size - so two extra passes use it:
//
//   BookRefineChapterDestsByBody  re-anchor a chapter row whose dest is not
//                                 backed by a display-size banner match
//   BookRepairChapterTitlePrefixes  restore the "第X章" unit prefix that OCR
//                                 ate ("章教会孩子..." -> "第三章教会孩子...")

// Configurable knobs (see tmp/ptoc-book2/TocChapterDiagnosis.md):
static const float kBannerAbsFontSize = 26.f;    // display size in absolute pt...
static const float kBannerRelRatio = 1.2f;       // ...or relative to the page's body size
static const int kChapterAnchorMinScore = 5;     // min matching glyphs
static const float kChapterAnchorMinSim = 0.55f; // LCS / longer-title glyph ratio

// Banners are either set in an absolute display size or, in textbooks with
// modest size contrast, merely ~1.2x the page's body size. Enlarged lead prose
// can reach the relative bar too, so relative-size lines only ever count
// together with a dominant title-glyph overlap (checked by the caller).
static bool BookIsBannerSizeLine(float fontSize, float pageMedian) {
    if (fontSize >= kBannerAbsFontSize) {
        return true;
    }
    return fontSize > 1.f && pageMedian > 1.f && fontSize >= pageMedian * kBannerRelRatio;
}

// One page's banner block: OCR splits a title across adjacent rows
// ("做好孩子健康生" + "活的坚实后盾"), so the display-size lines that stack
// vertically are joined before matching. Long runs (enlarged lead prose) are
// rejected by the caller's length cap; giant decorative glyphs ("范教") add no
// title glyphs and never win a score.
struct BookBannerGroup {
    int page = 0;
    char* text = nullptr; // whitespace-free concatenation
    int glyphs = 0;
};

static void BookFreeBannerGroups(Vec<BookBannerGroup>& groups) {
    for (int i = 0; i < groups.Size(); i++) {
        str::Free(groups[i].text);
    }
    groups.Reset();
}

struct BookBannerLine {
    const char* text;
    float y;
    float dy;
};

static void BookCollectPageBannerGroups(const Vec<ScanLine>& a, const Vec<ScanLine>& b,
                                        const Vec<BookPageFontStats>& stats, int tocStart, int tocEnd, int nPages,
                                        Vec<BookBannerGroup>& out) {
    out.Reset();
    if (nPages < 2) {
        return;
    }
    // Running headers quote the chapter title at the top of every body page,
    // occasionally in a larger-than-body font ("第二章帮助孩子为高考打下扎实
    // 的基础" on all 60+ chapter-2 pages, sz 8..16). A one-time opener banner
    // never recurs. The header's line split, bbox height and glyphs vary per
    // page ("第二章"+"帮助孩子..." vs one 17-glyph line, OCR even doubles a
    // glyph), so the fingerprint is the per-page concatenation of top-band
    // lines compared with an LCS ratio - a header repeats, a banner does not.
    // Band membership tests the line TOP y: merged bboxes inflate dy so much
    // that y+dy of a 16pt header can reach 75pt while its top stays at 29pt,
    // overlapping the body band.
    const float kBannerHeaderBandMaxY = 42.f;
    struct BookBandKey {
        int page;
        char* text; // owned, whitespace-free band concatenation
    };
    Vec<BookBandKey> bandKeys;
    Vec<BookBannerLine> ls;
    Vec<BookBannerLine> band;
    for (int p = 1; p <= nPages; p++) {
        if (BookDestIsTocPage(p, tocStart, tocEnd)) {
            continue;
        }
        band.Reset();
        for (int pass = 0; pass < 2; pass++) {
            const Vec<ScanLine>& src = pass == 0 ? a : b;
            for (int k = 0; k < src.Size(); k++) {
                const ScanLine& sl = src[k];
                if (sl.srcPage != p || !sl.text || !sl.text[0]) {
                    continue;
                }
                if (sl.y > kBannerHeaderBandMaxY) {
                    continue;
                }
                BookBannerLine bl;
                bl.text = sl.text;
                bl.y = sl.y;
                bl.dy = sl.dy > 1.f ? sl.dy : sl.fontSize;
                band.Append(bl);
            }
        }
        if (band.Size() < 1) {
            continue;
        }
        // stable y order for the concatenation
        for (int i = 1; i < band.Size(); i++) {
            BookBannerLine bl = band[i];
            int j = i - 1;
            while (j >= 0 && band[j].y > bl.y) {
                band[j + 1] = band[j];
                j--;
            }
            band[j + 1] = bl;
        }
        StrBuilder sb;
        for (int i = 0; i < band.Size(); i++) {
            sb.Append(band[i].text);
        }
        BookBandKey bk;
        bk.page = p;
        bk.text = str::Dup(sb.Get());
        bandKeys.Append(bk);
    }
    auto IsRunningHeader = [&bandKeys](int page, const char* text) {
        int cb[128];
        int ncb = BookCollectSpaceFreeCps(text, cb, 128);
        if (ncb < 4) {
            return false; // too short to fingerprint ("中" side artifacts)
        }
        for (int i = 0; i < bandKeys.Size(); i++) {
            if (bandKeys[i].page == page) {
                continue;
            }
            int bb[128];
            int nbb = BookCollectSpaceFreeCps(bandKeys[i].text, bb, 128);
            if (nbb < 4) {
                continue;
            }
            int lcs = BookGlyphLcs(cb, ncb, bb, nbb);
            if (lcs * 5 >= (ncb < nbb ? ncb : nbb) * 4) {
                return true;
            }
        }
        return false;
    };
    for (int p = 1; p <= nPages; p++) {
        if (BookDestIsTocPage(p, tocStart, tocEnd)) {
            continue;
        }
        float med = p < stats.Size() ? stats[p].median : 0.f;
        ls.Reset();
        for (int pass = 0; pass < 2; pass++) {
            const Vec<ScanLine>& src = pass == 0 ? a : b;
            for (int k = 0; k < src.Size(); k++) {
                const ScanLine& sl = src[k];
                if (sl.srcPage != p || !sl.text || !sl.text[0]) {
                    continue;
                }
                if (!BookIsBannerSizeLine(sl.fontSize, med)) {
                    continue;
                }
                // A display-size line is never header furniture; only lines
                // admitted by the relative-size rule can be running headers
                // (the opener banner spells the same title as the header, so
                // the fingerprint alone would kill the real evidence).
                if (sl.fontSize < kBannerAbsFontSize && sl.y <= kBannerHeaderBandMaxY && IsRunningHeader(p, sl.text)) {
                    continue;
                }
                BookBannerLine bl;
                bl.text = sl.text;
                bl.y = sl.y;
                bl.dy = sl.dy > 1.f ? sl.dy : sl.fontSize;
                ls.Append(bl);
            }
        }
        if (ls.Size() < 1) {
            continue;
        }
        // group display-size lines that stack vertically (gap <= 1.8x line
        // height): cluster from the first unassigned line, absorbing every
        // line reachable through overlapping/near-stacking rows
        bool assigned[64] = {};
        int nAssigned = 0;
        int n = ls.Size() < 64 ? ls.Size() : 64;
        while (nAssigned < n) {
            int seed = -1;
            for (int i = 0; i < n; i++) {
                if (!assigned[i]) {
                    seed = i;
                    break;
                }
            }
            float lo = ls[seed].y;
            float hi = ls[seed].y + ls[seed].dy;
            bool grown = true;
            bool inGroup[64] = {};
            inGroup[seed] = true;
            assigned[seed] = true;
            nAssigned++;
            while (grown) {
                grown = false;
                for (int j = 0; j < n; j++) {
                    if (assigned[j] && !inGroup[j]) {
                        continue; // belongs to another group
                    }
                    if (inGroup[j]) {
                        continue;
                    }
                    float gap =
                        ls[j].y > hi ? ls[j].y - hi : (ls[j].y + ls[j].dy < lo ? lo - (ls[j].y + ls[j].dy) : 0.f);
                    float dy = ls[j].dy > (hi - lo) ? ls[j].dy : (hi - lo);
                    if (gap <= 1.8f * dy) {
                        inGroup[j] = true;
                        assigned[j] = true;
                        nAssigned++;
                        lo = ls[j].y < lo ? ls[j].y : lo;
                        hi = ls[j].y + ls[j].dy > hi ? ls[j].y + ls[j].dy : hi;
                        grown = true;
                    }
                }
            }
            StrBuilder sb;
            for (int j = 0; j < n; j++) {
                if (inGroup[j]) {
                    sb.Append(ls[j].text);
                }
            }
            BookBannerGroup g;
            g.page = p;
            g.text = str::Dup(sb.Get());
            g.glyphs = BookGlyphCount(g.text);
            out.Append(g);
        }
    }
    for (int i = 0; i < bandKeys.Size(); i++) {
        str::Free(bandKeys[i].text);
    }
    bandKeys.Reset();
    band.Reset();
    ls.Reset();
}

// Re-anchor top-level chapter rows against display-size banner groups on the
// opener pages. The TOC-page layout estimate for an OCR-mangled banner row is
// a guess; the banner itself is ground truth. Constraints that keep a prose
// line or a section banner from stealing the anchor:
//   - the row is searched in (previous chapter's dest, next chapter's dest),
//     so reading order bounds the window;
//   - a group must be title-sized (<= title glyphs + 10), cover >= half the
//     row's glyphs with >= kChapterAnchorMinScore, and reach a majority
//     similarity (LCS / longer title);
//   - the earliest passing page wins (the opener precedes every page that
//     merely quotes the title).
static void BookRefineChapterDestsByBody(const Vec<ScanLine>& lines, const Vec<ScanLine>& extra,
                                         const Vec<BookPageFontStats>& stats, int tocStart, int tocEnd, int nPages,
                                         Vec<BookTocEntry>& hits) {
    if (nPages < 2 || hits.Size() < 1) {
        return;
    }
    Vec<int> chapIdx;
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].inferredLevel != 1 || !hits[i].title) {
            continue;
        }
        if (BookLooksLikeTocHeading(hits[i].title)) {
            continue;
        }
        chapIdx.Append(i);
    }
    if (chapIdx.Size() < 1) {
        return;
    }
    Vec<BookBannerGroup> groups;
    BookCollectPageBannerGroups(lines, extra, stats, tocStart, tocEnd, nPages, groups);
    if (groups.Size() < 1) {
        return;
    }
    int prevDest = tocEnd > 0 ? tocEnd : 0;
    for (int c = 0; c < chapIdx.Size(); c++) {
        BookTocEntry& hit = hits[chapIdx[c]];
        int nt = BookGlyphCount(hit.title);
        int nextDest = nPages + 1;
        for (int k = c + 1; k < chapIdx.Size(); k++) {
            int d = hits[chapIdx[k]].pdfPage;
            if (d > prevDest && d <= nPages) {
                nextDest = d;
                break;
            }
        }
        if (nt >= 4) {
            Vec<int> candPage;
            Vec<int> candScore;
            int bestScore = 0;
            for (int g = 0; g < groups.Size(); g++) {
                const BookBannerGroup& bg = groups[g];
                if (bg.page <= prevDest || bg.page >= nextDest) {
                    continue;
                }
                if (bg.glyphs < 4) {
                    continue;
                }
                int cg[128];
                int ncg = BookCollectSpaceFreeCps(bg.text, cg, 128);
                int ct[128];
                int nct = BookCollectSpaceFreeCps(hit.title, ct, 128);
                if (nct < 4) {
                    continue;
                }
                // The vertical grouping can absorb stacked lead prose or
                // decorative glyphs around the banner ("第章帮助孩子为高考
                // 打下扎实的基础" + 170 prose glyphs on one opener). Score
                // the best title-sized window of the concatenation instead of
                // the whole run - the banner is a y-ordered prefix or an
                // island inside it, and prose windows never reach the bar.
                int wMax = nct + 10;
                if (wMax > ncg) {
                    wMax = ncg;
                }
                int sc = 0;
                for (int start = 0; start + 4 <= ncg; start++) {
                    int wLen = ncg - start < wMax ? ncg - start : wMax;
                    if (wLen < 4) {
                        break;
                    }
                    const int* win = cg + start;
                    int run = BookSpaceFreePrefixRun(win, wLen, ct, nct);
                    int wsc = run >= 6 ? run : 0;
                    int lcs = BookGlyphLcs(ct, nct, win, wLen);
                    if (lcs >= 5 && lcs * 2 >= nct && lcs > wsc) {
                        wsc = lcs;
                    }
                    if (wsc < kChapterAnchorMinScore) {
                        continue;
                    }
                    if (lcs < wLen && lcs < nct) {
                        // containment already scored above; partial overlap
                        // needs a majority of the longer side's glyphs
                        float sim = (float)lcs / (float)(wLen > nct ? wLen : nct);
                        if (sim < kChapterAnchorMinSim) {
                            continue;
                        }
                    }
                    if (wsc > sc) {
                        sc = wsc;
                    }
                }
                if (sc < kChapterAnchorMinScore) {
                    continue;
                }
                candPage.Append(bg.page);
                candScore.Append(sc);
                if (sc > bestScore) {
                    bestScore = sc;
                }
            }
            // The opener precedes every page that merely quotes the title, so
            // take the earliest candidate whose evidence is within OCR-noise
            // distance of the strongest one (a damaged opener banner can lose
            // a glyph or two against a pristine quotation).
            const int kChapterAnchorScoreTol = 2;
            int bestPage = 0;
            for (int i = 0; i < candPage.Size(); i++) {
                if (candScore[i] >= bestScore - kChapterAnchorScoreTol) {
                    bestPage = candPage[i];
                    break;
                }
            }
            if (bestPage > 0 && bestPage != hit.pdfPage) {
                logf("chapter dest refined '%s' p%d -> p%d (score %d)\n", hit.title, hit.pdfPage, bestPage, bestScore);
                hit.pdfPage = bestPage;
                hit.bodyMatched = true;
                hit.x = hit.srcX;
                hit.y = 0;
            }
        }
        if (hit.pdfPage > prevDest && hit.pdfPage <= nPages) {
            prevDest = hit.pdfPage;
        }
    }
    BookFreeBannerGroups(groups);
}

// OCR eats the leading "第X章" of a banner row outright ("章教会孩子...",
// "力孩子度渡叛逆期"): the row still reads as a chapter by size, but the unit
// anchor is gone, so unit-based matching and grouping misclassify it. The
// complete title recurs verbatim elsewhere - the book-end recap list and
// running references all carry the prefix - so re-adopt the cleanest prefixed
// line that covers the row's glyphs. Guards: a valid leading 第X unit, no
// sentence punctuation, length within [title, title+8], glyph overlap >= half
// the row with >= kChapterAnchorMinScore.
static void BookRepairChapterTitlePrefixes(const Vec<ScanLine>& lines, const Vec<ScanLine>& extra, int tocStart,
                                           int tocEnd, Vec<BookTocEntry>& hits) {
    for (int i = 0; i < hits.Size(); i++) {
        BookTocEntry& hit = hits[i];
        if (hit.inferredLevel != 1 || !hit.title) {
            continue;
        }
        BookUnit u = BookParseUnit(hit.title);
        bool damaged = u.kind == BookUnitKind::None;
        if (!damaged) {
            int pos = 0;
            int cp = Utf8CodepointNext(hit.title, (int)str::Len(hit.title), pos);
            // also treat a bare trailing unit word ("章教会..." / "节 ...") as
            // a prefix that lost its number
            if (cp == 0x7AE0 || cp == 0x8282 || cp == 0x7BC7) { // 章 节 篇
                damaged = true;
            }
        }
        if (!damaged) {
            continue;
        }
        int nt = BookGlyphCount(hit.title);
        if (nt < 4) {
            continue;
        }
        int bestScore = 0;
        int bestLen = 0;
        const char* bestText = nullptr;
        for (int pass = 0; pass < 2 && !bestText; pass++) {
            const Vec<ScanLine>& src = pass == 0 ? lines : extra;
            for (int k = 0; k < src.Size(); k++) {
                const ScanLine& sl = src[k];
                if (!sl.text || BookDestIsTocPage(sl.srcPage, tocStart, tocEnd)) {
                    continue;
                }
                if (BookParseUnit(sl.text).kind == BookUnitKind::None) {
                    continue;
                }
                if (BookHasSentencePunct(sl.text)) {
                    continue;
                }
                int nc = BookGlyphCount(sl.text);
                if (nc < nt || nc > nt + 8) {
                    continue;
                }
                int sc = BookBodyAnchorScore(sl.text, hit.title);
                if (sc < kChapterAnchorMinScore || sc * 2 < nt) {
                    continue;
                }
                if (sc > bestScore || (sc == bestScore && nc < bestLen)) {
                    bestScore = sc;
                    bestLen = nc;
                    bestText = sl.text;
                }
            }
        }
        if (bestText) {
            logf("chapter title repaired '%s' -> '%s' (score %d)\n", hit.title, bestText, bestScore);
            str::Free(hit.title);
            hit.title = BookDupTrim(bestText);
        }
    }
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
        return -1;
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
    return agree >= 2 ? med : -1;
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
        if (offset < 0 || toPdf[printed] - printed == offset) {
            return toPdf[printed];
        }
        // The map claim fights the calibrated offset. That claim is usually a
        // stray footer-band number (a table reference like "7" near the
        // footer) or a renumbered appendix page that grabbed the slot
        // first-come; the calibrated offset is the median of many agreeing
        // pages, so it wins and the map claim stays a fallback.
        int p = printed + offset;
        if (p > tocEnd && p <= nPages) {
            return p;
        }
        return toPdf[printed];
    }
    if (offset >= 0) {
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
    if (!toPdf || offset < 0 || cap < 2) {
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
    int offset = -1;
    if (BookLabelsAreCustom(labels)) {
        offset = footerOff >= 0 ? footerOff : titleOff;
    } else if (titleOff >= 0) {
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
        // Link-backed rows carry the publisher's own destination; never let
        // printed-page mapping or body search move them.
        if (hits[h].linkFixed) {
            matched[h] = 1;
            continue;
        }
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
        // An omitted/unreadable printed number is not evidence that this row
        // has the same destination as its neighbour. Sharing it turns a whole
        // run of uncertain TOC rows into plausible-but-wrong bookmarks.
        bool samePrinted = nb >= 0 && hits[h].printedPage > 0 && hits[h].printedPage == hits[nb].printedPage;
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
        if (!hits[h].linkFixed && BookDestIsTocPage(hits[h].pdfPage, tocStart, tocEnd)) {
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
        if (pr > 0 && offset >= 0) {
            pred = pr + offset;
            if (pred <= tocEnd || pred > nPages) {
                pred = 0;
            }
        }
        int pdf = hits[i].pdfPage;
        if (pdf > 0 && prevPdf > 0 && pdf < prevPdf && !hits[i].linkFixed) {
            pdf = 0;
            hits[i].pdfPage = 0;
            hits[i].bodyMatched = false;
        }
        if (pdf < 1) {
            if (pred >= prevPdf && pred > 0) {
                pdf = pred;
            }
        }
        if (pdf > 0) {
            hits[i].pdfPage = pdf;
            prevPdf = pdf;
        }
    }
}

// A parent row with no destination (OCR-mangled banner, no printed page)
// opens at or before its first resolved descendant: inherit that page. Only
// when no resolved row of the parent's own level or higher separates them,
// so the descendant really belongs to this parent's subtree, and only when
// the inherited page keeps reading order monotonic.
static void BookInheritParentDestsFromChildren(Vec<BookTocEntry>& hits) {
    for (int i = 0; i < hits.Size(); i++) {
        if (hits[i].pdfPage >= 1 || !hits[i].title) {
            continue;
        }
        int lvl = hits[i].inferredLevel;
        int page = 0;
        for (int j = i + 1; j < hits.Size(); j++) {
            if (hits[j].inferredLevel <= lvl) {
                break; // subtree ended without a resolved descendant
            }
            if (hits[j].pdfPage >= 1) {
                page = hits[j].pdfPage;
                break;
            }
        }
        if (page < 1) {
            continue;
        }
        int prevPdf = 0;
        for (int k = 0; k < i; k++) {
            if (hits[k].pdfPage > prevPdf) {
                prevPdf = hits[k].pdfPage;
            }
        }
        if (page < prevPdf) {
            continue;
        }
        hits[i].pdfPage = page;
        hits[i].bodyMatched = true;
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
        int conf = hits[i].linkFixed ? 100 : (int)(hits[i].confidence * 100.f);
        ExtractedTocSource src =
            hits[i].source == 1 ? ExtractedTocSource::BodyInference : ExtractedTocSource::PrintedToc;
        ExtractedTocItem* n = BookNewItem(hits[i].title, hits[i].pdfPage, hits[i].x, hits[i].y, hits[i].inferredLevel,
                                          conf, src, hits[i].title);
        n->printedPage = hits[i].printedPage;
        n->tocPageNo = hits[i].srcPage;
        n->tocX = hits[i].srcX;
        n->tocY = hits[i].srcY;
        n->bodyMatched = hits[i].bodyMatched || hits[i].linkFixed;
        n->verified = (hits[i].bodyMatched && hits[i].printedPage > 0 && hits[i].pdfPage > 0) || hits[i].linkFixed;
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
    FILE* f = _wfopen(ToWStrTemp(path), L"a");
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
        if (h.linkPage > 0) {
            fprintf(f, "link dest: %d\n\n", h.linkPage);
        }
        fprintf(f, "src: page=%d y=%.1f size=%.1f bold=%d\n", h.srcPage, h.srcY, h.fontSize, h.bold ? 1 : 0);
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

// ============================================================================
// Native Linked TOC path.
//
// For born-digital PDFs whose printed Contents rows already carry internal
// GoTo link annotations. The PDF has already done the hard work: the row text
// is the title and the link destination is the target. This path only
//   1. merges link rects with the visual line they share a band with,
//   2. strips leader dots and the trailing displayed page number,
//   3. derives levels from the hierarchical numbering prefix (12.5.1. -> 3),
// and never runs the OCR printed-TOC cleanup, glued-line splitting, body
// re-matching or candidate deletion. Principle: keep the text, trust the link.
// ============================================================================

// "12.5.1." -> 3, "12.1." -> 2, "3." -> 1, "第十二章" -> 0. The whole dotted
// run is ONE numbering prefix; it must never be treated as multiple fields.
static int BookCountNumberingDepth(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int i = 0;
    BookSkipWs(s, len, i);
    if (i >= len || s[i] < '0' || s[i] > '9') {
        return 0;
    }
    int depth = 0;
    while (i < len) {
        int n = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9' && n < 4) {
            n++;
            i++;
        }
        if (n < 1) {
            break;
        }
        if (i < len && s[i] == '.') {
            depth++;
            i++;
            BookSkipWs(s, len, i);
            continue;
        }
        if (i + 2 < len && (unsigned char)s[i] == 0xEF && (unsigned char)s[i + 1] == 0xBC &&
            (unsigned char)s[i + 2] == 0x8E) { // full-width ．
            depth++;
            i += 3;
            BookSkipWs(s, len, i);
            continue;
        }
        break;
    }
    return depth;
}

// Strip only the trailing displayed page number ("............174" / " 174").
// A number glued to a word ("IPv6") or a numbering prefix ("12.5.1.") stays.
static int BookStripTrailingDisplayPage(char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int len = (int)str::Len(s);
    int end = len;
    while (end > 0 && s[end - 1] >= '0' && s[end - 1] <= '9') {
        end--;
    }
    int digits = len - end;
    if (digits < 1 || digits > 4) {
        return 0;
    }
    int j = end;
    while (j > 0 && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == 0x3000)) {
        j--;
    }
    bool hadWs = j < end;
    if (!hadWs) {
        // directly preceded by leader dots ("估算表......174")?
        int k = j;
        while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) {
            k--;
        }
        if (k > 0) {
            k--; // step onto the lead byte / ASCII char
        }
        int kk = k;
        int cp = k < j ? Utf8CodepointNext(s, j, kk) : 0;
        bool dotLeader = cp == '.' || cp == 0xFF0E || cp == 0x2026 || cp == 0x00B7 || cp == 0x30FB || cp == 0x2024 ||
                         cp == 0x2219 || cp == 0x22EF;
        if (!dotLeader) {
            return 0;
        }
    }
    int page = 0;
    for (int q = end; q < len; q++) {
        page = page * 10 + (s[q] - '0');
    }
    s[j] = 0;
    str::TrimWSInPlace(s, str::TrimOpt::Right);
    return page;
}

struct BookNativeTocPageStats {
    int matched = 0;
    int distinctTargets = 0;
};

// Match one visual line against the page's link annotations. A link belongs
// to the line when the rects overlap: generators cover the full row, the
// title only, the page number only, or the leader dots — any non-empty band
// overlap with >= 50% of the line height counts.
static int BookNativeLinkForLine(const Vec<BookLinkHit>& links, const BookLine& ln) {
    float y0 = ln.y;
    float y1 = ln.y + ln.dy;
    float x0 = ln.x;
    float x1 = ln.x + ln.dx;
    int best = 0;
    float bestArea = 0;
    for (int i = 0; i < links.Size(); i++) {
        const RectF& r = links[i].r;
        float ry0 = r.y;
        float ry1 = r.y + r.dy;
        float rx0 = r.x;
        float rx1 = r.x + r.dx;
        float oy = std::min(y1, ry1) - std::max(y0, ry0);
        if (oy < (y1 - y0) * 0.5f) {
            continue;
        }
        float ox = std::min(x1, rx1) - std::max(x0, rx0);
        if (ox <= 0) {
            continue;
        }
        if (oy * ox > bestArea) {
            bestArea = oy * ox;
            best = links[i].page;
        }
    }
    return best;
}

// Build TOC items from one page's native linked Contents rows. Returns the
// number of entries appended and fills stats for page detection.
static int BookNativeLinkedPageItems(EngineBase* engine, const Vec<ScanLine>& lines, int p,
                                     Vec<ExtractedTocItem*>& out, BookNativeTocPageStats* stats, FILE* dbg) {
    stats->matched = 0;
    stats->distinctTargets = 0;
    Vec<BookLine> page;
    BookCollectPage(lines, p, page);
    Vec<BookLinkHit> links;
    BookCollectPageLinks(engine, p, links);
    if (page.Size() < 3 || links.Size() < 3) {
        BookFreeLines(page);
        return 0;
    }
    int prevTarget = 0;
    int appended = 0;
    for (int i = 0; i < page.Size(); i++) {
        BookLine& ln = page[i];
        const char* raw = ln.text;
        if (!raw || !raw[0]) {
            continue;
        }
        // Page furniture, not entries. The link itself never sits on these.
        if (BookLooksLikePageFooter(raw) || BookLineIsPageNum(raw) || BookLooksLikeTocHeading(raw)) {
            continue;
        }
        int target = BookNativeLinkForLine(links, ln);
        if (target < 1 || target > engine->PageCount()) {
            continue;
        }
        stats->matched++;
        if (target != prevTarget) {
            stats->distinctTargets++;
            prevTarget = target;
        }
        char* title = BookDupTrim(raw);
        if (!title || !title[0]) {
            str::Free(title);
            continue;
        }
        int printed = BookStripTrailingDisplayPage(title);
        BookStripLeadersInPlace(title);
        str::TrimWSInPlace(title, str::TrimOpt::Both);
        if (!title[0]) {
            str::Free(title);
            continue;
        }
        // Level: numbering depth first, chapter/section keywords second,
        // inherit-last as a last resort. Never a reason to drop the entry.
        // Depth is the SEGMENT count, not the dot count: "1.1 项目名称" has
        // one dot but two segments, so counting dots put every x.y row one
        // level too shallow (flush with its chapter). BookParseMultiDotPrefix
        // counts segments for both "1.1" and the trailing-dot "12.5.1." style;
        // BookCountNumberingDepth stays as the fallback for single-segment
        // "12.5.1." outlines without a parent row.
        int level = 0;
        int multiSegs = 0;
        if (BookParseMultiDotPrefix(title, &multiSegs, nullptr) > 0) {
            level = multiSegs;
        } else {
            level = BookCountNumberingDepth(title);
        }
        if (level < 1) {
            BookUnit u = BookParseUnit(title);
            if (u.kind == BookUnitKind::Part || u.kind == BookUnitKind::Chapter) {
                level = 1;
            } else if (u.kind == BookUnitKind::Section) {
                level = 2;
            }
        }
        if (dbg) {
            fprintf(dbg, "NATIVE:\nRAW: %s\nLINK: -> page %d\nAFTER MINIMAL CLEAN: %s\nNUMBERING DEPTH: %d\nLEVEL: %d\n\n",
                    raw, target, title, level, level < 1 ? 1 : level);
        }
        ExtractedTocItem* n = BookNewItem(title, target, ln.x, ln.y, level, 95, ExtractedTocSource::PrintedToc, raw);
        n->printedPage = printed;
        n->tocPageNo = p;
        n->tocX = ln.x;
        n->tocY = ln.y;
        n->destinationSource = TocDestinationSource::PdfLink;
        n->verified = true;
        n->bodyMatched = false;
        out.Append(n);
        appended++;
        str::Free(title);
    }
    BookFreeLines(page);
    return appended;
}

// Detect the multi-page native linked Contents span and, when the evidence is
// strong, build the whole outline from it. Multi-page TOCs merge fully; a
// weak page (few links) inside the run still extends it.
static bool TryNativeLinkedBookToc(EngineBase* engine, const Vec<ScanLine>& lines, int nPages,
                                   Vec<ExtractedTocItem*>& roots, const char* debugPath) {
    if (!engine || nPages < 1) {
        return false;
    }
    FILE* dbg = debugPath ? _wfopen(ToWStrTemp(debugPath), L"a") : nullptr;
    int scanCap = nPages < 200 ? nPages : 200;
    Vec<ExtractedTocItem*> flat;
    int pagesLo = 0;
    int pagesHi = 0;
    int totalMatched = 0;
    int totalLinks = 0;
    bool inRun = false;
    for (int p = 1; p <= scanCap; p++) {
        Vec<ExtractedTocItem*> pageItems;
        BookNativeTocPageStats st;
        int got = BookNativeLinkedPageItems(engine, lines, p, pageItems, &st, dbg);
        bool strong = st.matched >= 6 && st.distinctTargets >= 4;
        bool weak = st.matched >= 3;
        if (strong || (inRun && weak && got >= 3)) {
            inRun = true;
            if (pagesLo == 0) {
                pagesLo = p;
            }
            pagesHi = p;
            totalMatched += st.matched;
            totalLinks += got;
            for (int i = 0; i < pageItems.Size(); i++) {
                flat.Append(pageItems[i]);
            }
            continue;
        }
        if (inRun) {
            break; // Contents span ended
        }
        // before the run: drop non-TOC page results (stray links in front matter)
        for (int i = 0; i < pageItems.Size(); i++) {
            delete pageItems[i];
        }
    }
    // Acceptance: a real clickable Contents has many link-bound rows spread
    // over several distinct targets. Everything else falls back to the
    // printed/OCR pipeline untouched.
    int nDistinct = 0;
    {
        int prev = 0;
        for (int i = 0; i < flat.Size(); i++) {
            if (flat[i]->pageNo != prev) {
                prev = flat[i]->pageNo;
                nDistinct++;
            }
        }
    }
    bool ok = flat.Size() >= 8 && nDistinct >= 6 && pagesLo > 0;
    if (!ok) {
        logf("native-linked-toc: rejected pages=%d-%d entries=%d distinctTargets=%d\n", pagesLo, pagesHi, flat.Size(),
             nDistinct);
        if (dbg) {
            fprintf(dbg, "native-linked-toc REJECTED entries=%d distinctTargets=%d\n", flat.Size(), nDistinct);
            fclose(dbg);
        }
        for (int i = 0; i < flat.Size(); i++) {
            delete flat[i];
        }
        return false;
    }
    // Put the 目录 heading itself in front, like the printed path does.
    for (int p = pagesLo; p <= pagesHi; p++) {
        bool found = false;
        for (int i = 0; i < lines.Size() && !found; i++) {
            if (lines[i].srcPage != p || !lines[i].text || !BookLooksLikeTocHeading(lines[i].text)) {
                continue;
            }
            ExtractedTocItem* n =
                BookNewItem(BookPrintedTocBookmarkTitle(lines[i].text), p, lines[i].x, lines[i].y, 1, 99,
                            ExtractedTocSource::PrintedToc, lines[i].text);
            n->tocPageNo = p;
            n->tocX = lines[i].x;
            n->tocY = lines[i].y;
            n->destinationSource = TocDestinationSource::PdfLink;
            n->verified = true;
            roots.Append(n);
            found = true;
        }
        if (found) {
            break;
        }
    }
    // Tree by level with a level stack (same shape as BookBuildTree).
    Vec<ExtractedTocItem*> stack;
    for (int i = 0; i < flat.Size(); i++) {
        ExtractedTocItem* n = flat[i];
        if (n->level < 1) {
            n->level = 1;
        }
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
    logf("native-linked-toc: pages=%d-%d linkLines=%d entries=%d distinctTargets=%d\n", pagesLo, pagesHi, totalLinks,
         flat.Size(), nDistinct);
    if (dbg) {
        fprintf(dbg, "Native Linked TOC debug\npages: %d-%d\nlink lines: %d\nentries: %d\ndistinct targets: %d\n\n",
                pagesLo, pagesHi, totalMatched, flat.Size(), nDistinct);
        fclose(dbg);
    }
    return true;
}

bool ExtractBookPrintedToc(EngineBase* engine, Vec<ScanLine>& lines, const Vec<char*>& labels, int nPages,
                           Vec<ExtractedTocItem*>& roots, const char* debugPath, const TocExtractProgress* prog) {
    // Native Linked TOC first: when the Contents rows already carry internal
    // GoTo links, convert them directly to the outline. The OCR printed-TOC
    // pipeline below stays untouched for scans and link-less books.
    if (engine && TryNativeLinkedBookToc(engine, lines, nPages, roots, debugPath)) {
        return true;
    }
    char tracePath[MAX_PATH]{};
    const char* trace = debugPath;
    if (!trace && GetEnvironmentVariableA("SUMATRA_TOC_TRACE", tracePath, dimof(tracePath)) > 0) {
        trace = tracePath;
    }
    int tocStart = 0;
    int tocEnd = 0;
    if (!BookFindTocRange(lines, nPages, &tocStart, &tocEnd, trace != nullptr)) {
        BookWriteTocRangeTrace(trace, lines, nPages, 0, 0);
        return false;
    }
    // Hybrid PDFs can contain real text on some Contents sheets and scans on
    // their interleaved neighbours. The document-level "born digital" result
    // then skips OCR, leaving a hole such as page 9 between parsed pages 8 and
    // 10. OCR only empty pages already proven to lie in the Contents range;
    // this keeps extraction bounded and never turns body pages into OCR work.
    if (engine) {
        int nOcrPages = 0;
        for (int p = tocStart; p <= tocEnd; p++) {
            bool hasLines = false;
            for (int i = 0; i < lines.Size(); i++) {
                if (lines[i].srcPage == p) {
                    hasLines = true;
                    break;
                }
            }
            if (!hasLines && PtocOcrAndCollectPageScanLines(engine, p, lines)) {
                nOcrPages++;
            }
        }
        if (nOcrPages > 0) {
            logf("book-toc OCR-filled-pages=%d range=%d-%d\n", nOcrPages, tocStart, tocEnd);
            BookFindTocRange(lines, nPages, &tocStart, &tocEnd, trace != nullptr);
        }
    }
    BookWriteTocRangeTrace(trace, lines, nPages, tocStart, tocEnd);
    // Running-header band detection. Scanned TOC pages repeat a decorative
    // page header near the top of every page ("ONTENTS", glyph salads like
    // "量量整系最服票"); the text is garbled differently on each page, so the
    // reliable signature is positional: short digit-free non-structural lines
    // hugging the top margin on >= 2 pages, never interrupted by a
    // page-numbered row. Once proven, drop every such candidate on all pages.
    const float kHeaderBandBottom = 56.f; // TOC content starts below y=56
    auto IsHeaderBandCandidate = [](const BookLine& sl) {
        if (!sl.text || sl.y + sl.dy > kHeaderBandBottom) {
            return false;
        }
        if (BookHasAsciiDigit(sl.text) || !BookHasLetterOrCjk(sl.text)) {
            return false;
        }
        int glyphs = BookGlyphCount(sl.text);
        if (glyphs < 1 || glyphs > 24) {
            return false;
        }
        return !BookIsStructTitle(sl.text) && !BookLooksLikeTocHeading(sl.text);
    };
    int nBandPages = 0;
    bool bandBlocked = false;
    for (int p = tocStart; p <= tocEnd; p++) {
        if (BookLooksLikeCipPage(lines, p)) {
            continue;
        }
        Vec<BookLine> page;
        BookCollectPage(lines, p, page);
        bool got = false;
        for (int i = 0; i < page.Size(); i++) {
            if (page[i].text && page[i].y + page[i].dy <= kHeaderBandBottom && BookHasAsciiDigit(page[i].text)) {
                bandBlocked = true; // real content reaches into the band
            }
            if (IsHeaderBandCandidate(page[i])) {
                got = true;
            }
        }
        if (got) {
            nBandPages++;
        }
        BookFreeLines(page);
    }
    bool dropHeaderBand = nBandPages >= 2 && !bandBlocked;
    Vec<BookTocEntry> hits;
    for (int p = tocStart; p <= tocEnd; p++) {
        if (BookLooksLikeCipPage(lines, p)) {
            continue;
        }
        Vec<BookLine> page;
        BookCollectPage(lines, p, page);
        Vec<BookLinkHit> links;
        if (engine) {
            BookCollectPageLinks(engine, p, links);
        }
        if (dropHeaderBand) {
            for (int i = 0; i < page.Size(); i++) {
                if (IsHeaderBandCandidate(page[i])) {
                    page[i].used = true;
                }
            }
        }
        BookParseTocPage(page, hits, links);
        BookFreeLines(page);
    }
    BookSortEntries(hits);
    BookMergeWrapEntries(hits);
    BookSortEntries(hits);
    BookDropExactDuplicateEntries(hits);
    for (int i = 0; i < hits.Size(); i++) {
        int page = BookExtractTrailingInlinePage(hits[i].title);
        if (hits[i].printedPage < 1 && page > 0) {
            hits[i].printedPage = page;
        }
    }
    if (hits.Size() < 2) {
        BookFreeEntries(hits);
        return false;
    }
    BookAssignLevels(hits);
    BookSanitizePrintedPages(hits);
    int printedOffset = 0;
    BookMapPrintedPages(lines, tocStart, tocEnd, nPages, labels, hits, &printedOffset);
    Vec<ScanLine> bodyExtra;
    if (!BookCollectExtraScanLines(engine, lines, nPages, bodyExtra, prog)) {
        PtocFreeScanLines(bodyExtra);
        BookFreeEntries(hits);
        return false;
    }
    Vec<BookPageFontStats> anchorStats;
    BookCollectPageFontStats(lines, bodyExtra, nPages, anchorStats);
    BookResolveMissingDestsByBody(lines, bodyExtra, anchorStats, tocStart, tocEnd, nPages, hits);
    // Repair damaged titles first: the refiner matches whole titles against
    // opener banners, and a repaired "第五章..." prefix lifts the LCS
    // similarity of a mangled row back over the acceptance bar.
    BookRepairChapterTitlePrefixes(lines, bodyExtra, tocStart, tocEnd, hits);
    BookRefineChapterDestsByBody(lines, bodyExtra, anchorStats, tocStart, tocEnd, nPages, hits);
    PtocFreeScanLines(bodyExtra);
    BookEnforceReadingOrder(hits, printedOffset, tocEnd, nPages);
    BookInheritParentDestsFromChildren(hits);
    BookDropMappedDuplicateEntries(hits);
    // A TOC-heading row that slipped through the append filter (wrap-merge
    // re-joins "目" + "录" fragments afterwards) would shadow the deliberate
    // synthetic TOC bookmark below; drop them all here.
    for (int i = 0; i < hits.Size();) {
        if (BookLooksLikeTocHeading(hits[i].title)) {
            str::Free(hits[i].title);
            str::Free(hits[i].raw);
            str::Free(hits[i].reason);
            hits.RemoveAt(i);
        } else {
            i++;
        }
    }
    BookInsertPrintedTocBookmark(lines, tocStart, tocEnd, hits);
    BookBuildTree(hits, roots);
    if (trace) {
        BookWriteDebug(trace, tocStart, tocEnd, printedOffset, hits);
    }
    BookFreeEntries(hits);
    int n = 0;
    for (int i = 0; i < roots.Size(); i++) {
        n++;
        n += roots[i]->children.Size();
    }
    return n >= 4 || roots.Size() >= 1;
}
