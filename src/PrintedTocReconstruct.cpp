/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Printed TOC reconstruction: P1 pure algorithm stages.
//
// Every function here is deterministic and side-effect free: inputs are a
// PtPageData (from PtocLoadOcrPageJson or live OCR) plus PrintedTocConfig.
// "Geometry first, text second": page anchors are recognized by position
// (right-aligned tail) and validated by shape (label grammar), never by
// content keywords. See PrintedTocModel.h for the data model and the stage
// contracts.

#include "PrintedTocModel.h"

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static bool PtIsDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool PtIsAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool PtIsAlnum(char c) {
    return PtIsDigit(c) || PtIsAlpha(c);
}

static int PtCmpFloat(const float* a, const float* b) {
    if (*a < *b) {
        return -1;
    }
    if (*a > *b) {
        return 1;
    }
    return 0;
}

// sorts v in place, returns the median (0 for empty)
static float PtMedianOf(Vec<float>& v) {
    int n = v.Size();
    if (n == 0) {
        return 0;
    }
    v.SortTyped(PtCmpFloat);
    if (n % 2 == 1) {
        return v[n / 2];
    }
    return (v[n / 2 - 1] + v[n / 2]) * 0.5f;
}

static bool PtIsCjkCodepoint(int cp) {
    if (cp >= 0x2E80 && cp <= 0x9FFF) {
        return true; // radicals, CJK unify, Yi
    }
    if (cp >= 0x3400 && cp <= 0x4DBF) {
        return true; // ext A
    }
    if (cp >= 0xF900 && cp <= 0xFAFF) {
        return true; // compatibility ideographs
    }
    if (cp >= 0xFF00 && cp <= 0xFFEF) {
        return true; // fullwidth forms + CJK punctuation （）
    }
    return cp >= 0x3000 && cp <= 0x303F; // CJK punctuation/symbols
}

static bool PtStartsWithCjk(const char* s) {
    if (!s || !*s) {
        return false;
    }
    int idx = 0;
    int cp = Utf8CodepointNext(s, (int)str::Len(s), idx);
    return PtIsCjkCodepoint(cp);
}

static bool PtEndsWithCjk(const char* s) {
    if (!s || !*s) {
        return false;
    }
    int len = (int)str::Len(s);
    int idx = len;
    int cp = Utf8CodepointPrev(s, len, idx);
    return PtIsCjkCodepoint(cp);
}

// multi-byte dot-leader characters (matched as byte suffixes)
static bool PtIsDotLeaderSuffix(const char* s, int len, int* suffixLen) {
    static const char* kDots[] = {
        "\xe2\x80\xa6", // … U+2026 horizontal ellipsis
        "\xe2\x80\xa5", // ‥ U+2025 two-dot leader
        "\xe2\x80\xa2", // • U+2022 bullet
        "\xe2\x80\x94", // — U+2014 em dash
        "\xc2\xb7",     // · U+00B7 middle dot
    };
    for (int i = 0; i < (int)dimof(kDots); i++) {
        int kl = (int)str::Len(kDots[i]);
        if (len >= kl && memcmp(s + len - kl, kDots[i], kl) == 0) {
            *suffixLen = kl;
            return true;
        }
    }
    return false;
}

// strips trailing leader noise (' ', '.', '-', dot chars) from s[0..*lenInOut)
static void PtStripTrailingLeaders(const char* s, int* lenInOut) {
    int len = *lenInOut;
    for (;;) {
        if (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '.' || s[len - 1] == '-')) {
            len--;
            continue;
        }
        int sl = 0;
        if (PtIsDotLeaderSuffix(s, len, &sl)) {
            len -= sl;
            continue;
        }
        break;
    }
    *lenInOut = len;
}

static char* PtDupRange(const char* s, int len) {
    if (len < 0) {
        len = 0;
    }
    return str::Dup(s, (size_t)len);
}

// joins token texts [0..count) with single spaces; heap, caller frees
static char* PtJoinTokenTexts(const Vec<PtToken>& toks, int count) {
    if (count <= 0) {
        return str::Dup("");
    }
    if (count > toks.Size()) {
        count = toks.Size();
    }
    int total = count - 1; // separating spaces
    for (int i = 0; i < count; i++) {
        total += (int)str::Len(toks[i].text ? toks[i].text : "");
    }
    char* s = AllocArray<char>(total + 1);
    int w = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            s[w++] = ' ';
        }
        const char* t = toks[i].text ? toks[i].text : "";
        int l = (int)str::Len(t);
        memcpy(s + w, t, (size_t)l);
        w += l;
    }
    s[w] = 0;
    return s;
}

// a line is a TOC heading when its space-stripped text contains 目录 /
// contents. Only ever consulted for anchorless lines, so body sentences that
// merely mention them don't reach this.
static bool PtIsHeadingText(const char* text) {
    if (!text) {
        return false;
    }
    char buf[128];
    int w = 0;
    for (const char* p = text; *p && w < (int)dimof(buf) - 1; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '.') {
            continue;
        }
        // fullwidth space U+3000 (e3 80 80)
        if ((unsigned char)c == 0xe3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x80) {
            p += 2;
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        buf[w++] = c;
    }
    buf[w] = 0;
    if (w == 0) {
        return false;
    }
    if (strstr(buf, "\xe7\x9b\xae\xe5\xbd\x95")) { // 目录
        return true;
    }
    if (strstr(buf, "contents")) {
        return true;
    }
    return false;
}

// a line is a chapter heading when it opens with 第<number>章/篇/部/回
// (chinese or arabic numeral). Only ever consulted for anchorless lines, so
// entry titles that merely reference a chapter don't reach this.
static bool PtIsChapterHeadingText(const char* text) {
    if (!text) {
        return false;
    }
    const char* p = text;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if ((unsigned char)p[0] != 0xe7 || (unsigned char)p[1] != 0xac || (unsigned char)p[2] != 0xac) {
        return false; // 第
    }
    p += 3;
    static const char* const kCjkNums[] = {
        "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89", "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad", "\xe4\xb8\x83",
        "\xe5\x85\xab", "\xe4\xb9\x9d", "\xe5\x8d\x81", "\xe7\x99\xbe", "\xe5\x8d\x83", "\xe4\xb8\xa4", "\xe9\x9b\xb6",
    };
    bool anyNum = false;
    bool num = true;
    while (num && *p) {
        num = false;
        if (PtIsDigit(*p)) {
            anyNum = num = true;
            p++;
            continue;
        }
        if ((unsigned char)p[0] == 0xe3 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0x87) {
            anyNum = num = true; // 〇
            p += 3;
            continue;
        }
        for (const char* cn : kCjkNums) {
            if (strncmp(p, cn, 3) == 0) {
                anyNum = num = true;
                p += 3;
                break;
            }
        }
    }
    if (!anyNum) {
        return false;
    }
    if (strncmp(p, "\xe7\xab\xa0", 3) == 0) {
        return true; // 章
    }
    if (strncmp(p, "\xe7\xaf\x87", 3) == 0) {
        return true; // 篇
    }
    if (strncmp(p, "\xe9\x83\xa8", 3) == 0) {
        return true; // 部
    }
    if (strncmp(p, "\xe5\x9b\x9e", 3) == 0) {
        return true; // 回
    }
    return false;
}

// ---------------------------------------------------------------------------
// printed page label parsing
// ---------------------------------------------------------------------------

static int PtRomanCharValue(int c) {
    switch (c) {
        case 'i':
        case 'I':
            return 1;
        case 'v':
        case 'V':
            return 5;
        case 'x':
        case 'X':
            return 10;
        case 'l':
        case 'L':
            return 50;
        case 'c':
        case 'C':
            return 100;
        case 'd':
        case 'D':
            return 500;
        case 'm':
        case 'M':
            return 1000;
    }
    return 0;
}

// strict roman grammar: valid subtractive pairs only, 1..3999
static bool PtParseRoman(const char* s, int len, int* valueOut, bool* hasLowerOut) {
    if (len <= 0 || len > 15) {
        return false;
    }
    bool hasLower = false;
    bool hasUpper = false;
    int total = 0;
    int prev = 1000;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            hasLower = true;
        } else if (c >= 'A' && c <= 'Z') {
            hasUpper = true;
        }
        int v = PtRomanCharValue(c);
        if (!v) {
            return false;
        }
        if (v > prev) {
            // only canonical subtractive pairs
            if (!((prev == 1 && (v == 5 || v == 10)) || (prev == 10 && (v == 50 || v == 100)) ||
                  (prev == 100 && (v == 500 || v == 1000)))) {
                return false;
            }
            total += v - 2 * prev;
        } else {
            total += v;
        }
        prev = v;
    }
    if (hasLower && hasUpper) {
        return false; // mixed case is not a numeral
    }
    if (total < 1 || total > 3999) {
        return false;
    }
    *valueOut = total;
    *hasLowerOut = hasLower;
    return true;
}

bool PtParsePageLabel(const char* sIn, PtPageLabel* out) {
    *out = PtPageLabel();
    if (!sIn) {
        return false;
    }
    const char* s = sIn;
    while (*s == ' ') {
        s++;
    }
    int len = (int)str::Len(s);
    while (len > 0 && s[len - 1] == ' ') {
        len--;
    }
    if (len == 0 || len >= 32) {
        return false;
    }

    // arabic (page numbers beyond 9999 don't exist in books)
    bool allDigits = true;
    for (int i = 0; i < len; i++) {
        if (!PtIsDigit(s[i])) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        if (len > 4) {
            return false;
        }
        int v = 0;
        for (int i = 0; i < len; i++) {
            v = v * 10 + (s[i] - '0');
        }
        if (v < 1) {
            return false;
        }
        out->type = PtPageLabelType::Arabic;
        out->ordinal = v;
        out->hasOrdinal = true;
        memcpy(out->rawText, s, (size_t)len);
        return true;
    }

    // roman
    {
        int v = 0;
        bool hasLower = false;
        if (PtParseRoman(s, len, &v, &hasLower)) {
            out->type = hasLower ? PtPageLabelType::RomanLower : PtPageLabelType::RomanUpper;
            out->ordinal = v;
            out->hasOrdinal = true;
            memcpy(out->rawText, s, (size_t)len);
            return true;
        }
    }

    // compound "sec-num" (A-12, 2-18): section-scoped, no global ordinal
    {
        int dash = -1;
        for (int i = len - 1; i > 0; i--) {
            if (s[i] == '-') {
                dash = i;
                break;
            }
        }
        if (dash > 0) {
            int secLen = dash;
            int numLen = len - dash - 1;
            if (secLen <= 4 && numLen >= 1 && numLen <= 4) {
                bool secOk = secLen >= 1;
                for (int i = 0; i < secLen && secOk; i++) {
                    if (!PtIsAlnum(s[i])) {
                        secOk = false;
                    }
                }
                bool numOk = true;
                int num = 0;
                for (int i = 0; i < numLen; i++) {
                    if (!PtIsDigit(s[dash + 1 + i])) {
                        numOk = false;
                        break;
                    }
                    num = num * 10 + (s[dash + 1 + i] - '0');
                }
                if (secOk && numOk && num >= 1) {
                    out->type = PtPageLabelType::Compound;
                    out->number = num;
                    int cch = secLen < (int)dimof(out->section) - 1 ? secLen : (int)dimof(out->section) - 1;
                    memcpy(out->section, s, (size_t)cch);
                    memcpy(out->rawText, s, (size_t)len);
                    return true;
                }
            }
        }
    }
    return false;
}

// retry a failed label parse with common OCR confusions applied (O/o -> 0,
// I/i/l -> 1). The run must be all digits after substitution and contain at
// least one real digit, so ordinary words never turn into page numbers.
bool PtParsePageLabelOcrFixed(const char* sIn, PtPageLabel* out) {
    if (!sIn) {
        return false;
    }
    const char* s = sIn;
    while (*s == ' ') {
        s++;
    }
    int len = (int)str::Len(s);
    while (len > 0 && s[len - 1] == ' ') {
        len--;
    }
    if (len == 0 || len >= 32) {
        return false;
    }
    char fixed[32];
    bool anyDigit = false;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (PtIsDigit(c)) {
            anyDigit = true;
        } else if (c == 'O' || c == 'o') {
            c = '0';
        } else if (c == 'I' || c == 'i' || c == 'l') {
            c = '1';
        } else {
            return false;
        }
        fixed[i] = c;
    }
    fixed[len] = 0;
    if (!anyDigit) {
        return false;
    }
    return PtParsePageLabel(fixed, out);
}

// ---------------------------------------------------------------------------
// line-level anchor split (shared by scoring and candidate parsing)
// ---------------------------------------------------------------------------

struct PtLineSplit {
    bool hasAnchor = false;
    bool fromToken = false; // anchor was a whole trailing OCR token
    PtPageLabel label;
    PtBoxF anchorBox;
    PtBoxF titleBox;
    char* titleText = nullptr; // heap, Free() below
    float titleCharW = 0;      // titleBox.Dx() per codepoint (indent epsilon unit)

    void Free() {
        str::Free(titleText);
        titleText = nullptr;
    }
};

static void PtFinishTitleSplit(PtLineSplit* out) {
    int cp = out->titleText ? Utf8CodepointCountN(out->titleText, (int)str::Len(out->titleText)) : 0;
    if (cp > 0 && out->titleBox.Dx() > 0) {
        out->titleCharW = out->titleBox.Dx() / (float)cp;
    }
}

// estimate the sub-box of text range [startByte..endByte) within tok by
// codepoint share (OCR gives no per-char boxes at this layer)
static PtBoxF PtSubBoxByChars(const PtToken& tok, const char* text, int startByte, int endByte) {
    int totalCp = Utf8CodepointCountN(text, (int)str::Len(text));
    float dx = tok.box.Dx();
    float x0 = tok.box.x0;
    float x1 = tok.box.x1;
    if (totalCp > 0 && dx > 0) {
        int cpStart = startByte <= 0 ? 0 : Utf8CodepointCountN(text, startByte);
        int cpEnd = Utf8CodepointCountN(text, endByte);
        x0 = tok.box.x0 + dx * (float)cpStart / (float)totalCp;
        x1 = tok.box.x0 + dx * (float)cpEnd / (float)totalCp;
    }
    return PtBoxF{x0, tok.box.y0, x1, tok.box.y1};
}

// A label ending at labelStart is accepted when preceded (after optional
// whitespace) by dot leaders, by a CJK char (arabic/compound only - this keeps
// "Chapter 1" out while letting "第一章 总论 1" in), or when the label is the
// whole text.
static bool PtLabelPrefixOk(const char* s, int labelStart, PtPageLabelType type) {
    if (labelStart == 0) {
        return true;
    }
    int k = labelStart;
    while (k > 0 && s[k - 1] == ' ') {
        k--;
    }
    if (k == 0) {
        return false; // only whitespace before the label
    }
    if (s[k - 1] == '.') {
        return true;
    }
    // a dash right after whitespace is leader noise (OCR reads dot leaders
    // ".... 28" as "- 28" / "-28"); a compound "A-12" label never reaches this
    // branch because its prefix starts before the section letter
    if (s[k - 1] == '-' && (k == 1 || s[k - 2] == ' ')) {
        return true;
    }
    int sl = 0;
    if (PtIsDotLeaderSuffix(s, k, &sl)) {
        return true;
    }
    int len = k;
    int idx = len;
    int cp = Utf8CodepointPrev(s, len, idx);
    if (!PtIsCjkCodepoint(cp)) {
        return false;
    }
    return type == PtPageLabelType::Arabic || type == PtPageLabelType::Compound;
}

// finds a trailing page label in s[0..len). Returns the byte index of the
// label start, or -1. Compound labels ("A-12") are tried first so that
// "附录 A-12" does not degrade to arabic "12".
static int PtFindTrailingLabel(const char* s, int len, PtPageLabel* out) {
    int end = len;
    while (end > 0 && s[end - 1] == ' ') {
        end--;
    }
    if (end == 0) {
        return -1;
    }

    // compound: <alnum{1,4}>-<digit{1,4}> at the very end
    {
        int e = end;
        while (e > 0 && PtIsDigit(s[e - 1])) {
            e--;
        }
        if (e < end && e > 0 && s[e - 1] == '-') {
            int b = e - 1;
            int secStart = b;
            while (secStart > 0 && PtIsAlnum(s[secStart - 1]) && (b - secStart) < 4) {
                secStart--;
            }
            int secLen = b - secStart;
            if (secLen >= 1) {
                char buf[16];
                int w = 0;
                for (int k = secStart; k < b && w < 8; k++) {
                    buf[w++] = s[k];
                }
                buf[w++] = '-';
                for (int k = e; k < end && w < 15; k++) {
                    buf[w++] = s[k];
                }
                buf[w] = 0;
                PtPageLabel lbl;
                if (PtParsePageLabel(buf, &lbl) && lbl.type == PtPageLabelType::Compound) {
                    if (PtLabelPrefixOk(s, secStart, lbl.type)) {
                        *out = lbl;
                        return secStart;
                    }
                }
            }
        }
    }

    // plain trailing alnum run
    int e2 = end;
    while (e2 > 0 && PtIsAlnum(s[e2 - 1])) {
        e2--;
    }
    if (e2 == end || end - e2 >= 32) {
        return -1;
    }
    char buf[32];
    memcpy(buf, s + e2, (size_t)(end - e2));
    buf[end - e2] = 0;
    PtPageLabel lbl;
    if (!PtParsePageLabel(buf, &lbl) && !PtParsePageLabelOcrFixed(buf, &lbl)) {
        return -1;
    }
    if (!PtLabelPrefixOk(s, e2, lbl.type)) {
        return -1;
    }
    *out = lbl;
    return e2;
}

// splits one reconstructed line into title + printed page anchor.
// Returns false when the line carries no anchor (continuation / heading).
static bool PtSplitLineAnchor(const PtRecLine& line, PtLineSplit* out) {
    int n = line.tokens.Size();
    if (n == 0) {
        return false;
    }

    if (n >= 2) {
        // whole trailing token as anchor ("Preface" + "iii")
        const PtToken& last = line.tokens[n - 1];
        PtPageLabel lbl;
        if (PtParsePageLabel(last.text, &lbl) || PtParsePageLabelOcrFixed(last.text, &lbl)) {
            out->hasAnchor = true;
            out->fromToken = true;
            out->label = lbl;
            out->anchorBox = last.box;
            out->titleText = PtJoinTokenTexts(line.tokens, n - 1);
            for (int i = 0; i < n - 1; i++) {
                out->titleBox.UnionWith(line.tokens[i].box);
            }
            PtFinishTitleSplit(out);
            return true;
        }
        // anchor embedded in the trailing token text ("总论 ...... 1")
        const char* text = last.text ? last.text : "";
        int len = (int)str::Len(text);
        int ls = PtFindTrailingLabel(text, len, &lbl);
        if (ls >= 0) {
            out->hasAnchor = true;
            out->label = lbl;
            out->anchorBox = PtSubBoxByChars(last, text, ls, len);
            int tLen = ls;
            PtStripTrailingLeaders(text, &tLen);
            char* prefix = PtDupRange(text, tLen);
            char* rest = PtJoinTokenTexts(line.tokens, n - 1);
            out->titleText = str::Join(rest, " ", prefix);
            str::Free(rest);
            str::Free(prefix);
            for (int i = 0; i < n - 1; i++) {
                out->titleBox.UnionWith(line.tokens[i].box);
            }
            out->titleBox.UnionWith(PtSubBoxByChars(last, text, 0, tLen));
            PtFinishTitleSplit(out);
            return true;
        }
        return false;
    }

    // single token: the whole OCR line is one string ("第一章 总论 ...... 1")
    const PtToken& tok = line.tokens[0];
    const char* text = tok.text ? tok.text : "";
    int len = (int)str::Len(text);
    PtPageLabel lbl;
    int ls = PtFindTrailingLabel(text, len, &lbl);
    if (ls < 0) {
        return false;
    }
    out->hasAnchor = true;
    out->label = lbl;
    out->anchorBox = PtSubBoxByChars(tok, text, ls, len);
    int tLen = ls;
    PtStripTrailingLeaders(text, &tLen);
    out->titleText = PtDupRange(text, tLen);
    out->titleBox = PtSubBoxByChars(tok, text, 0, tLen);
    PtFinishTitleSplit(out);
    return true;
}

// ---------------------------------------------------------------------------
// P1-B: token columns + line reconstruction
// ---------------------------------------------------------------------------

static int PtCmpTokenY(const PtToken* a, const PtToken* b) {
    float ya = a->box.MidY();
    float yb = b->box.MidY();
    if (ya < yb) {
        return -1;
    }
    if (ya > yb) {
        return 1;
    }
    if (a->box.x0 < b->box.x0) {
        return -1;
    }
    if (a->box.x0 > b->box.x0) {
        return 1;
    }
    return 0;
}

static int PtCmpTokenX(const PtToken* a, const PtToken* b) {
    if (a->box.x0 < b->box.x0) {
        return -1;
    }
    if (a->box.x0 > b->box.x0) {
        return 1;
    }
    return 0;
}

// token-level 1/2-column detection. A column cut needs: an interior
// low-coverage band wide enough (bins covered by at most maxCross tokens, so
// a heading crossing the cut cannot hide the gap), few tokens crossing it,
// both sides wide enough, and non-label (title) tokens on both sides - the
// last rule keeps detached page-number columns ("Preface    iii") from
// reading as a 2-col layout. tokenCol is only written when 2 columns are
// accepted; it stays all-zero otherwise. Returns the column count.
static int PtDetectTokenColumns(PtPageData& page, Vec<int>& tokenCol) {
    int n = page.tokens.Size();
    tokenCol.Reset();
    for (int i = 0; i < n; i++) {
        tokenCol.Append(0);
    }
    if (n < 5 || page.width <= 0) {
        return 1;
    }
    const PrintedTocConfig& cfg = PtocConfig();

    const int kBins = 128;
    u8 cnt[kBins] = {0};
    for (int i = 0; i < n; i++) {
        const PtBoxF& b = page.tokens[i].box;
        int lo = (int)(b.x0 * kBins / page.width);
        int hi = (int)(b.x1 * kBins / page.width);
        if (lo < 0) {
            lo = 0;
        }
        if (hi >= kBins) {
            hi = kBins - 1;
        }
        for (int k = lo; k <= hi; k++) {
            cnt[k]++;
        }
    }
    int maxCross = (int)(cfg.colMaxCrossRatio * (float)n);
    if (maxCross < 1) {
        maxCross = 1;
    }

    // widest interior low-coverage run (a run touching either edge is a
    // margin)
    int bestLo = -1;
    int bestHi = -1;
    int bestW = 0;
    int k = 1;
    while (k < kBins - 1) {
        if (cnt[k] > maxCross) {
            k++;
            continue;
        }
        int lo = k;
        while (k < kBins - 1 && cnt[k] <= maxCross) {
            k++;
        }
        if (k == kBins - 1 && cnt[k] <= maxCross) {
            break; // touches the right margin
        }
        int hi = k - 1;
        if (lo == 1 && cnt[0] <= maxCross) {
            continue; // touches the left margin
        }
        int w = hi - lo + 1;
        if (w > bestW) {
            bestW = w;
            bestLo = lo;
            bestHi = hi;
        }
    }
    if (bestLo < 0) {
        return 1;
    }
    float binW = page.width / (float)kBins;
    if ((float)bestW * binW < cfg.colGapMinRatio * page.width) {
        return 1;
    }
    float cut = (float)(bestLo + bestHi + 1) * 0.5f * binW;

    int cross = 0;
    for (int i = 0; i < n; i++) {
        const PtBoxF& b = page.tokens[i].box;
        if (b.x0 < cut && b.x1 > cut) {
            cross++;
        }
    }
    if (cross > maxCross) {
        return 1;
    }

    // side statistics (no tokenCol writes before all checks pass)
    int nL = 0;
    int nR = 0;
    float lX0 = 1e9f;
    float lX1 = -1e9f;
    float rX0 = 1e9f;
    float rX1 = -1e9f;
    bool lNonLabel = false;
    bool rNonLabel = false;
    for (int i = 0; i < n; i++) {
        const PtToken& t = page.tokens[i];
        PtPageLabel tmp;
        bool isLabel = PtParsePageLabel(t.text, &tmp);
        if (t.box.x1 <= cut) {
            nL++;
            lX0 = std::min(lX0, t.box.x0);
            lX1 = std::max(lX1, t.box.x1);
            if (!isLabel) {
                lNonLabel = true;
            }
        } else if (t.box.x0 >= cut) {
            nR++;
            rX0 = std::min(rX0, t.box.x0);
            rX1 = std::max(rX1, t.box.x1);
            if (!isLabel) {
                rNonLabel = true;
            }
        }
    }
    if (nL < 2 || nR < 2) {
        return 1;
    }
    if (lX1 - lX0 < cfg.colMinSideWidth * page.width) {
        return 1;
    }
    if (rX1 - rX0 < cfg.colMinSideWidth * page.width) {
        return 1;
    }
    if (!lNonLabel || !rNonLabel) {
        return 1;
    }

    // accepted: assign columns (crossing tokens go to column 0)
    for (int i = 0; i < n; i++) {
        const PtBoxF& b = page.tokens[i].box;
        if (b.x0 >= cut && b.x1 > cut) {
            tokenCol[i] = 1;
        }
    }
    return 2;
}

void PtReconstructLines(PtPageData& page) {
    for (PtRecLine* l : page.lines) {
        delete l; // lines are heap objects (see PtPageData ownership note)
    }
    page.lines.Reset();
    int n = page.tokens.Size();
    if (n == 0) {
        return;
    }
    Vec<int> tokenCol;
    int nCols = PtDetectTokenColumns(page, tokenCol);

    Vec<float> hs;
    for (int i = 0; i < n; i++) {
        hs.Append(page.tokens[i].box.Dy());
    }
    float medH = PtMedianOf(hs);
    if (medH <= 0) {
        medH = 10;
    }
    float tol = PtocConfig().lineMidYTol * medH;

    for (int col = 0; col < nCols; col++) {
        // shallow token copies (ownership stays with page.tokens until the
        // final Reset; char* pointers are moved into the lines)
        Vec<PtToken> toks;
        for (int i = 0; i < n; i++) {
            if (tokenCol[i] == col) {
                toks.Append(page.tokens[i]);
            }
        }
        if (toks.Size() == 0) {
            continue;
        }
        toks.SortTyped(PtCmpTokenY);
        float sumY = 0;
        int cnt = 0;
        int start = 0;
        for (int i = 0; i <= toks.Size(); i++) {
            bool flush = i == toks.Size();
            if (!flush) {
                float my = toks[i].box.MidY();
                float d = my - (cnt > 0 ? sumY / (float)cnt : my);
                if (d < 0) {
                    d = -d;
                }
                if (cnt == 0 || d <= tol) {
                    sumY += my;
                    cnt++;
                    continue;
                }
            }
            // flush cluster [start..i)
            if (i > start) {
                PtRecLine* ln = new PtRecLine();
                for (int k2 = start; k2 < i; k2++) {
                    ln->tokens.Append(toks[k2]);
                    ln->box.UnionWith(toks[k2].box);
                }
                if (ln->tokens.Size() > 1) {
                    ln->tokens.SortTyped(PtCmpTokenX);
                }
                ln->pageIndex = page.pageIndex;
                ln->columnIndex = col;
                ln->baselineY = ln->box.y1;
                page.lines.Append(ln);
            }
            start = i;
            sumY = 0;
            cnt = 0;
            if (!flush) {
                sumY = toks[i].box.MidY();
                cnt = 1;
            }
        }
    }
    page.tokens.Reset(); // ownership moved into the lines
}

// ---------------------------------------------------------------------------
// P1-A: TOC page scoring
// ---------------------------------------------------------------------------

PtTocPageScores PtScoreTocPage(const PtPageData& page) {
    PtTocPageScores sc;
    int nLines = page.lines.Size();
    if (nLines == 0) {
        return sc;
    }
    const PrintedTocConfig& cfg = PtocConfig();

    Vec<float> hs;
    for (int i = 0; i < nLines; i++) {
        hs.Append(page.lines[i]->box.Dy());
    }
    float medH = PtMedianOf(hs);
    if (medH <= 0) {
        medH = 10;
    }

    struct LineInfo {
        int col = 0;
        bool anchored = false;
        bool heading = false;
        bool hasOrdinal = false;
        int ordinal = 0;
        float anchorX0 = 0;
        float anchorX1 = 0;
        float titleX0 = 0;
    };
    Vec<LineInfo> infos;
    Vec<float> charWs; // title width per codepoint of anchored lines
    int nAnchored = 0;
    int nHeading = 0;
    for (int i = 0; i < nLines; i++) {
        const PtRecLine& line = *page.lines[i];
        LineInfo li;
        li.col = line.columnIndex > 0 ? 1 : 0;
        char* text = PtJoinTokenTexts(line.tokens, line.tokens.Size());
        PtLineSplit sp;
        bool ok = PtSplitLineAnchor(line, &sp);
        if (ok) {
            li.anchored = true;
            li.hasOrdinal = sp.label.hasOrdinal;
            li.ordinal = sp.label.ordinal;
            li.anchorX0 = sp.anchorBox.x0;
            li.anchorX1 = sp.anchorBox.x1;
            li.titleX0 = sp.titleBox.x0;
            nAnchored++;
            if (sp.titleCharW > 0) {
                charWs.Append(sp.titleCharW);
            }
        } else if (PtIsHeadingText(text)) {
            li.heading = true;
            nHeading++;
        }
        sp.Free();
        str::Free(text);
        infos.Append(li);
    }
    float medCharW = PtMedianOf(charWs);
    if (medCharW <= 0) {
        medCharW = 10;
    }

    int nText = nLines - nHeading;
    sc.endingPageNumberRatio = nText > 0 ? (float)nAnchored / (float)nText : 0;
    sc.tocKeywordScore = nHeading > 0 ? 1.0f : 0.0f;

    float alignSum = 0;
    float monoSum = 0;
    float indentSum = 0;
    int alignW = 0;
    int monoW = 0;
    int indentW = 0;
    Vec<float> gaps;
    for (int col = 0; col < 2; col++) {
        Vec<int> idx;
        for (int i = 0; i < nLines; i++) {
            if (infos[i].anchored && infos[i].col == col) {
                idx.Append(i);
            }
        }
        int cnt = idx.Size();
        if (cnt == 0) {
            continue;
        }
        // anchor x-alignment (sorted 1D clustering)
        Vec<float> x1s;
        for (int k2 = 0; k2 < cnt; k2++) {
            x1s.Append(infos[idx[k2]].anchorX1);
        }
        x1s.SortTyped(PtCmpFloat);
        float epsA = cfg.anchorXClusterEps * medH;
        int best = 1;
        int cur = 1;
        for (int k2 = 1; k2 < cnt; k2++) {
            if (x1s[k2] - x1s[k2 - 1] <= epsA) {
                cur++;
                if (cur > best) {
                    best = cur;
                }
            } else {
                cur = 1;
            }
        }
        alignSum += (float)best / (float)cnt;
        alignW++;
        // monotonic page numbers down the column
        int pairs = 0;
        int good = 0;
        for (int k2 = 1; k2 < cnt; k2++) {
            const LineInfo& a = infos[idx[k2 - 1]];
            const LineInfo& b = infos[idx[k2]];
            if (a.hasOrdinal && b.hasOrdinal) {
                pairs++;
                if (b.ordinal >= a.ordinal) {
                    good++;
                }
            }
        }
        if (pairs > 0) {
            monoSum += (float)good / (float)pairs;
            monoW++;
        }
        // indentation bands (absolute px clustering)
        Vec<float> x0s;
        for (int k2 = 0; k2 < cnt; k2++) {
            x0s.Append(infos[idx[k2]].titleX0);
        }
        x0s.SortTyped(PtCmpFloat);
        float epsI = cfg.indentClusterEpsilon * medCharW;
        int bands = 0;
        int curB = 1;
        for (int k2 = 1; k2 < cnt; k2++) {
            if (x0s[k2] - x0s[k2 - 1] <= epsI) {
                curB++;
            } else {
                if (curB >= cfg.indentMinBandLines) {
                    bands++;
                }
                curB = 1;
            }
        }
        if (curB >= cfg.indentMinBandLines) {
            bands++;
        }
        indentSum += bands >= 2 ? 1.0f : (bands == 1 ? 0.3f : 0.0f);
        indentW++;
        // title->anchor gap (informational feature)
        for (int k2 = 0; k2 < cnt; k2++) {
            const LineInfo& li = infos[idx[k2]];
            float colW = page.width > 0 ? page.width : 1;
            gaps.Append((li.anchorX0 - li.titleX0) / colW);
        }
    }
    sc.pageNumberXAlignment = alignW > 0 ? alignSum / (float)alignW : 0;
    sc.monotonicPageNumberScore = monoW > 0 ? monoSum / (float)monoW : 0;
    sc.indentationBandScore = indentW > 0 ? indentSum / (float)indentW : 0;
    sc.titleToPageGapRatio = PtMedianOf(gaps);

    int shortL = 0;
    for (int i = 0; i < nLines; i++) {
        if (page.lines[i]->box.Dx() < 0.5f * page.width) {
            shortL++;
        }
    }
    sc.shortLineRatio = nLines > 0 ? (float)shortL / (float)nLines : 0;
    // repeatedRightAnchorScore needs cross-page context (P2); stays 0 here.

    sc.total = 0.30f * sc.endingPageNumberRatio + 0.20f * sc.pageNumberXAlignment +
               0.15f * sc.monotonicPageNumberScore + 0.15f * sc.indentationBandScore + 0.10f * sc.tocKeywordScore;
    return sc;
}

// ---------------------------------------------------------------------------
// P1-C: entry candidates
// ---------------------------------------------------------------------------

void PtParseEntryCandidates(const PtPageData& page, Vec<PtEntryCandidate>* out) {
    out->Reset();
    if (page.lines.Size() == 0) {
        return;
    }
    PtBoxF colBoxes[2];
    for (int i = 0; i < page.lines.Size(); i++) {
        int col = page.lines[i]->columnIndex > 0 ? 1 : 0;
        colBoxes[col].UnionWith(page.lines[i]->box);
    }
    // page.lines are already in reading order (column 0 then 1, y ascending)
    for (int i = 0; i < page.lines.Size(); i++) {
        const PtRecLine& line = *page.lines[i];
        PtEntryCandidate c;
        c.tocSourcePage = page.pageIndex;
        c.column = line.columnIndex > 0 ? 1 : 0;
        PtLineSplit sp;
        bool ok = PtSplitLineAnchor(line, &sp);
        if (ok && (!sp.titleText || !sp.titleText[0])) {
            // bare page label: the page's own footer or a stray page number,
            // never a TOC entry
            sp.Free();
            continue;
        }
        if (ok) {
            c.printedPage = sp.label;
            c.pageNumberBox = sp.anchorBox;
            c.titleBox = sp.titleBox;
            c.title = sp.titleText;
            sp.titleText = nullptr;
            c.rawText = PtJoinTokenTexts(line.tokens, line.tokens.Size());
            float pc = sp.fromToken ? 0.9f : 0.75f;
            c.parseConf = pc;
            c.pageAnchorConf = pc;
        } else {
            c.title = PtJoinTokenTexts(line.tokens, line.tokens.Size());
            c.rawText = str::Dup(c.title);
            c.titleBox = line.box;
            c.parseConf = 0.30f;
        }
        sp.Free();
        const PtBoxF& cb = colBoxes[c.column];
        if (!cb.IsEmpty() && cb.Dx() > 0 && !c.titleBox.IsEmpty()) {
            float ind = (c.titleBox.x0 - cb.x0) / cb.Dx();
            if (ind < 0) {
                ind = 0;
            }
            c.indentNormalized = ind;
            c.columnWidth = cb.Dx();
        }
        out->Append(c);
    }
}

void PtMergeWrappedEntries(const PtPageData& page, Vec<PtEntryCandidate>* cands, bool keepTopOrphans) {
    int n = cands->Size();
    if (n == 0) {
        return;
    }
    const PrintedTocConfig& cfg = PtocConfig();
    Vec<float> hs;
    for (int i = 0; i < page.lines.Size(); i++) {
        hs.Append(page.lines[i]->box.Dy());
    }
    float medH = PtMedianOf(hs);
    if (medH <= 0) {
        medH = 10;
    }
    float maxGap = cfg.continuationMaxGap * medH;

    bool anchoredSeen[2] = {false, false};
    // in-place compaction: anchored candidates, mergeable continuations and
    // (when keepTopOrphans) column-leading orphans
    int w = 0;
    for (int i = 0; i < n; i++) {
        PtEntryCandidate& c = (*cands)[i];
        bool hasAnchor = c.printedPage.type != PtPageLabelType::Unknown;
        int col = c.column > 0 ? 1 : 0;
        if (hasAnchor) {
            anchoredSeen[col] = true;
        }
        if (!hasAnchor && w > 0 && !(keepTopOrphans && !anchoredSeen[col])) {
            PtEntryCandidate& last = (*cands)[w - 1];
            bool lastAnchored = last.printedPage.type != PtPageLabelType::Unknown;
            float gap = c.titleBox.y0 - last.titleBox.y1;
            bool mergeable = lastAnchored && c.column == last.column &&
                             last.mergedWithPrev < cfg.continuationMaxChain && gap >= -medH && gap <= maxGap &&
                             !c.titleBox.IsEmpty() && !last.titleBox.IsEmpty() &&
                             c.indentNormalized >= last.indentNormalized - 0.02f && !PtIsChapterHeadingText(c.title);
            if (mergeable) {
                // CJK titles concatenate without a space
                bool needSpace = !PtEndsWithCjk(last.title) && !PtStartsWithCjk(c.title);
                char* joined = needSpace ? str::Join(last.title, " ", c.title) : str::Join(last.title, c.title);
                str::Free(last.title);
                last.title = joined;
                last.titleBox.UnionWith(c.titleBox);
                last.mergedWithPrev++;
                c.Free();
                continue;
            }
        }
        if (w != i) {
            (*cands)[w] = (*cands)[i]; // move (trivially copyable)
        }
        w++;
    }
    cands->RemoveAt(w, cands->Size() - w);

    if (keepTopOrphans) {
        return; // column-leading orphans survive for PtMergeCrossPage
    }
    // whatever is still anchorless is a heading or a far orphan: drop it
    for (int i = cands->Size() - 1; i >= 0; i--) {
        if ((*cands)[i].printedPage.type == PtPageLabelType::Unknown) {
            (*cands)[i].Free();
            cands->RemoveAt(i);
        }
    }
}

// ---------------------------------------------------------------------------
// P2-A preview: indent bands + hierarchy levels
// ---------------------------------------------------------------------------

static float PtMedianCharW(const Vec<PtEntryCandidate>& cands) {
    Vec<float> cws;
    for (int i = 0; i < cands.Size(); i++) {
        const PtEntryCandidate& c = cands[i];
        if (c.printedPage.type == PtPageLabelType::Unknown || c.mergedWithPrev > 0) {
            continue; // merged titles have inflated widths
        }
        int cp = c.title ? Utf8CodepointCountN(c.title, (int)str::Len(c.title)) : 0;
        if (cp > 0 && c.titleBox.Dx() > 0) {
            cws.Append(c.titleBox.Dx() / (float)cp);
        }
    }
    float m = PtMedianOf(cws);
    return m > 0 ? m : 10;
}

// clusters anchored candidates of one column into indent bands; appends band
// centers (normalized) and fills counts (parallel to centers)
static void PtIndentBandsForCol(const PtPageData& page, const Vec<PtEntryCandidate>& cands, int col, float medCharW,
                                Vec<float>* centers, Vec<int>* counts) {
    centers->Reset();
    counts->Reset();
    PtBoxF cb;
    for (int i = 0; i < page.lines.Size(); i++) {
        if ((page.lines[i]->columnIndex > 0 ? 1 : 0) == col) {
            cb.UnionWith(page.lines[i]->box);
        }
    }
    if (cb.IsEmpty() || cb.Dx() <= 0) {
        return;
    }
    const PrintedTocConfig& cfg = PtocConfig();
    float epsN = cfg.indentClusterEpsilon * medCharW / cb.Dx();

    Vec<float> indents;
    for (int i = 0; i < cands.Size(); i++) {
        const PtEntryCandidate& c = cands[i];
        if (c.printedPage.type == PtPageLabelType::Unknown || c.column != col || c.titleBox.IsEmpty()) {
            continue;
        }
        indents.Append(c.indentNormalized);
    }
    if (indents.Size() == 0) {
        return;
    }
    indents.SortTyped(PtCmpFloat);
    float bandStart = indents[0];
    float bandLast = indents[0];
    float bandSum = indents[0];
    int bandCnt = 1;
    for (int i = 1; i < indents.Size(); i++) {
        float v = indents[i];
        if (v - bandLast <= epsN) {
            bandLast = v;
            bandCnt++;
            bandSum += v;
        } else {
            centers->Append(bandSum / (float)bandCnt);
            counts->Append(bandCnt);
            bandStart = bandLast = v;
            bandSum = v;
            bandCnt = 1;
        }
    }
    centers->Append(bandSum / (float)bandCnt);
    counts->Append(bandCnt);
}

void PtAssignHierarchyLevels(const PtPageData& page, Vec<PtEntryCandidate>* cands) {
    if (cands->Size() == 0) {
        return;
    }
    const PrintedTocConfig& cfg = PtocConfig();
    float medCharW = PtMedianCharW(*cands);
    for (int i = 0; i < cands->Size(); i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (c.printedPage.type != PtPageLabelType::Unknown) {
            c.charWidth = medCharW; // context for PtNormalizeLevelsAcrossPages
        }
    }
    for (int col = 0; col < 2; col++) {
        Vec<float> centers;
        Vec<int> counts;
        PtIndentBandsForCol(page, *cands, col, medCharW, &centers, &counts);
        if (centers.Size() == 0) {
            continue;
        }
        // centers are in ascending order (indents were sorted): rank = index+1
        for (int i = 0; i < cands->Size(); i++) {
            PtEntryCandidate& c = (*cands)[i];
            if (c.printedPage.type == PtPageLabelType::Unknown || c.column != col) {
                continue;
            }
            int best = 0;
            float bestD = 1e9f;
            for (int b = 0; b < centers.Size(); b++) {
                float d = c.indentNormalized - centers[b];
                if (d < 0) {
                    d = -d;
                }
                if (d < bestD) {
                    bestD = d;
                    best = b;
                }
            }
            c.level = best + 1;
            c.hierarchyConf = counts[best] >= cfg.indentMinBandLines ? 0.8f : 0.4f;
        }
    }
}

// ---------------------------------------------------------------------------
// P2: document-level stages
// ---------------------------------------------------------------------------

static int PtCmpInt(const int* a, const int* b) {
    if (*a < *b) {
        return -1;
    }
    if (*a > *b) {
        return 1;
    }
    return 0;
}

// joins c's title into prev's (CJK-aware spacing); boxes are unioned
static void PtJoinEntryTitles(PtEntryCandidate* prev, PtEntryCandidate* c) {
    bool needSpace = !PtEndsWithCjk(prev->title) && !PtStartsWithCjk(c->title);
    char* joined = needSpace ? str::Join(prev->title, " ", c->title) : str::Join(prev->title, c->title);
    str::Free(prev->title);
    prev->title = joined;
    prev->titleBox.UnionWith(c->titleBox);
}

// P2-C: document-wide continuation merge. cands must be in document reading
// order (ascending tocSourcePage, per-page reading order).
void PtMergeCrossPage(Vec<PtEntryCandidate>* cands) {
    int n = cands->Size();
    if (n == 0) {
        return;
    }
    const PrintedTocConfig& cfg = PtocConfig();

    int w = 0;
    for (int i = 0; i < n; i++) {
        PtEntryCandidate& c = (*cands)[i];
        // headings never become entries (and never absorb continuations)
        if (PtIsHeadingText(c.title ? c.title : c.rawText)) {
            c.Free();
            continue;
        }
        bool hasAnchor = c.printedPage.type != PtPageLabelType::Unknown;
        // chapter headings stay standalone entries (page inferred below);
        // they never absorb into the previous entry nor absorb continuations
        bool isChapter = !hasAnchor && PtIsChapterHeadingText(c.title);
        if (!hasAnchor && !isChapter && w > 0) {
            PtEntryCandidate& prev = (*cands)[w - 1];
            bool prevAnchored = prev.printedPage.type != PtPageLabelType::Unknown;
            // same page (wrapped into a following column) or the page above
            bool contiguous = (c.tocSourcePage == prev.tocSourcePage && c.column >= prev.column) ||
                              (c.tocSourcePage == prev.tocSourcePage + 1 && c.column == prev.column);
            bool mergeable = prevAnchored && contiguous && prev.mergedWithPrev < cfg.continuationMaxChain &&
                             !c.titleBox.IsEmpty() && !prev.titleBox.IsEmpty() &&
                             c.indentNormalized >= prev.indentNormalized - 0.02f;
            if (mergeable) {
                PtJoinEntryTitles(&prev, &c);
                prev.mergedWithPrev++;
                c.Free();
                continue;
            }
        }
        if (w != i) {
            (*cands)[w] = (*cands)[i];
        }
        w++;
    }
    cands->RemoveAt(w, cands->Size() - w);

    // chapter headings have no anchor of their own: a chapter starts on the
    // page of its first section, so borrow the next anchored entry's label
    for (int i = 0; i < cands->Size(); i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (c.printedPage.type != PtPageLabelType::Unknown) {
            continue;
        }
        if (!PtIsChapterHeadingText(c.title)) {
            continue;
        }
        for (int j = i + 1; j < cands->Size(); j++) {
            PtEntryCandidate& nx = (*cands)[j];
            if (nx.printedPage.type == PtPageLabelType::Unknown) {
                continue;
            }
            c.printedPage = nx.printedPage;
            c.pageAnchorConf = 0.4f; // inferred, not measured
            c.chapterHeading = true;
            c.level = 1;
            break;
        }
    }

    // unmatched orphans are noise (mid-page headings, stray short lines)
    for (int i = cands->Size() - 1; i >= 0; i--) {
        if ((*cands)[i].printedPage.type == PtPageLabelType::Unknown) {
            (*cands)[i].Free();
            cands->RemoveAt(i);
        }
    }
}

// file-scope for the sort comparator
struct PtIndPoint {
    float v;   // indentNormalized
    float eps; // per-candidate cluster epsilon (normalized)
    int idx;   // candidate index
};

static int PtCmpIndPoint(const PtIndPoint* a, const PtIndPoint* b) {
    if (a->v < b->v) {
        return -1;
    }
    if (a->v > b->v) {
        return 1;
    }
    return 0;
}

// P2-A: global hierarchy. Re-clusters indentNormalized across all pages;
// each candidate carries its own epsilon (charWidth/columnWidth), so pages
// with different column widths still land in shared bands.
void PtNormalizeLevelsAcrossPages(Vec<PtEntryCandidate>* cands) {
    int n = cands->Size();
    if (n == 0) {
        return;
    }
    const PrintedTocConfig& cfg = PtocConfig();

    Vec<PtIndPoint> pts;
    for (int i = 0; i < n; i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (c.printedPage.type == PtPageLabelType::Unknown || c.titleBox.IsEmpty()) {
            continue;
        }
        if (c.charWidth <= 0 || c.columnWidth <= 0) {
            continue; // per-page stage did not run; keep its level
        }
        PtIndPoint pt;
        pt.v = c.indentNormalized;
        pt.eps = cfg.indentClusterEpsilon * c.charWidth / c.columnWidth;
        pt.idx = i;
        pts.Append(pt);
    }
    if (pts.Size() == 0) {
        return;
    }
    pts.SortTyped(PtCmpIndPoint);

    int start = 0;
    int level = 0;
    while (start < pts.Size()) {
        level++;
        int last = start;
        int end = start + 1;
        while (end < pts.Size()) {
            float eps = 0.5f * (pts[end].eps + pts[last].eps);
            if (pts[end].v - pts[last].v <= eps) {
                last = end;
                end++;
                continue;
            }
            break;
        }
        float conf = (end - start) >= cfg.indentMinBandLines ? 0.8f : 0.4f;
        for (int k = start; k < end; k++) {
            PtEntryCandidate& c = (*cands)[pts[k].idx];
            c.level = level;
            c.hierarchyConf = conf;
        }
        start = end;
    }
}

// P2-A finalize: chapter-heading entries are level 1; every other entry sits
// at least one level below them (indent bands only see anchored lines, so the
// headings never took part in the clustering).
void PtPromoteChapterHeadings(Vec<PtEntryCandidate>* cands) {
    bool hasChapter = false;
    for (int i = 0; i < cands->Size(); i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (c.chapterHeading) {
            c.level = 1;
            hasChapter = true;
        }
    }
    if (!hasChapter) {
        return;
    }
    for (int i = 0; i < cands->Size(); i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (!c.chapterHeading && c.level < 2) {
            c.level = 2;
        }
    }
}

// file-scope for the sort comparator
struct PtAnchorPoint {
    PtPageLabelType type;
    int printed;
    int pdfPage;
};

static int PtCmpAnchorPoint(const PtAnchorPoint* a, const PtAnchorPoint* b) {
    if (a->type != b->type) {
        return a->type < b->type ? -1 : 1;
    }
    if (a->printed != b->printed) {
        return a->printed < b->printed ? -1 : 1;
    }
    if (a->pdfPage != b->pdfPage) {
        return a->pdfPage < b->pdfPage ? -1 : 1;
    }
    return 0;
}

// P2-B: group anchors by label type, then split each type's ascending
// printed sequence into runs of equal offset (pdfPage - printed). The
// dominant offset's vote share scales every segment confidence.
void PtBuildPageMapping(const Vec<PtPageAnchor>& anchors, Vec<PtPageMappingSegment>* segsOut) {
    segsOut->Reset();
    Vec<PtAnchorPoint> sorted;
    for (int i = 0; i < anchors.Size(); i++) {
        const PtPageAnchor& a = anchors[i];
        if (a.printed <= 0 || a.pdfPage <= 0) {
            continue;
        }
        PtAnchorPoint pt;
        pt.type = a.type;
        pt.printed = a.printed;
        pt.pdfPage = a.pdfPage;
        sorted.Append(pt);
    }
    if (sorted.Size() == 0) {
        return;
    }
    sorted.SortTyped(PtCmpAnchorPoint);
    const PrintedTocConfig& cfg = PtocConfig();

    Vec<int> runCounts;
    int start = 0;
    while (start < sorted.Size()) {
        int off = sorted[start].pdfPage - sorted[start].printed;
        int end = start + 1;
        while (end < sorted.Size() && sorted[end].type == sorted[start].type &&
               sorted[end].pdfPage - sorted[end].printed == off) {
            end++;
        }
        PtPageMappingSegment seg;
        seg.labelType = sorted[start].type;
        seg.printedBegin = sorted[start].printed;
        seg.printedEnd = sorted[end - 1].printed;
        seg.offset = off;
        segsOut->Append(seg);
        runCounts.Append(end - start);
        start = end;
    }

    // dominant offset vote share (document-wide uncertainty)
    Vec<int> offs;
    for (int i = 0; i < sorted.Size(); i++) {
        offs.Append(sorted[i].pdfPage - sorted[i].printed);
    }
    offs.SortTyped(PtCmpInt);
    int bestRun = 1;
    int cur = 1;
    for (int i = 1; i < offs.Size(); i++) {
        if (offs[i] == offs[i - 1]) {
            cur++;
            if (cur > bestRun) {
                bestRun = cur;
            }
        } else {
            cur = 1;
        }
    }
    float share = (float)bestRun / (float)sorted.Size();
    float scale = share >= cfg.mappingVoteMinRatio ? 1.0f : 0.5f;
    for (int s = 0; s < segsOut->Size(); s++) {
        int cnt = runCounts[s];
        float base = cnt >= cfg.mappingMinSamples ? 1.0f : (cnt == 1 ? 0.5f : 0.8f);
        (*segsOut)[s].confidence = base * scale;
    }
}

int PtMapPrintedToPdf(const Vec<PtPageMappingSegment>& segs, PtPageLabelType type, int printed, float* confOut) {
    if (confOut) {
        *confOut = 0;
    }
    if (printed <= 0) {
        return -1;
    }
    for (int i = 0; i < segs.Size(); i++) {
        const PtPageMappingSegment& seg = segs[i];
        if (seg.labelType != type || printed < seg.printedBegin || printed > seg.printedEnd) {
            continue;
        }
        if (confOut) {
            *confOut = seg.confidence;
        }
        return printed + seg.offset;
    }
    return -1;
}

void PtResolvePdfPages(const Vec<PtPageMappingSegment>& segs, int nPdfPages, Vec<PtEntryCandidate>* cands) {
    for (int i = 0; i < cands->Size(); i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (!c.printedPage.hasOrdinal || c.resolvedPdfPage.has_value()) {
            continue;
        }
        float conf = 0;
        int pdf = PtMapPrintedToPdf(segs, c.printedPage.type, c.printedPage.ordinal, &conf);
        if (pdf < 1 || (nPdfPages > 0 && pdf > nPdfPages)) {
            continue;
        }
        c.resolvedPdfPage = pdf;
        c.pageMappingConf = conf;
    }
}

// ---------------------------------------------------------------------------
// P3: body text validation
// ---------------------------------------------------------------------------

// codepoints kept for similarity: ASCII alnum (lowercased) and CJK ideographs
static bool PtSimKeepCp(int cp) {
    if (cp >= 'a' && cp <= 'z') {
        return true;
    }
    if (cp >= 'A' && cp <= 'Z') {
        return true;
    }
    if (cp >= '0' && cp <= '9') {
        return true;
    }
    return cp >= 0x2E80 && cp <= 0x9FFF;
}

// normalizes s into codepoints (max nMax); returns the count
static int PtNormalizeCps(const char* s, int* cps, int nMax) {
    if (!s) {
        return 0;
    }
    int len = (int)str::Len(s);
    int idx = 0;
    int n = 0;
    while (idx < len && n < nMax) {
        int cp = Utf8CodepointNext(s, len, idx);
        if (cp >= 'A' && cp <= 'Z') {
            cp += 32;
        }
        if (PtSimKeepCp(cp)) {
            cps[n++] = cp;
        }
    }
    return n;
}

static float PtLcsSim(const int* ca, int na, const int* cb, int nb) {
    if (na == 0 || nb == 0) {
        return 0;
    }
    int dp[33][33];
    for (int i = 0; i <= na; i++) {
        dp[i][0] = 0;
    }
    for (int j = 0; j <= nb; j++) {
        dp[0][j] = 0;
    }
    for (int i = 1; i <= na; i++) {
        for (int j = 1; j <= nb; j++) {
            dp[i][j] = ca[i - 1] == cb[j - 1] ? dp[i - 1][j - 1] + 1 : std::max(dp[i - 1][j], dp[i][j - 1]);
        }
    }
    return 2.0f * (float)dp[na][nb] / (float)(na + nb);
}

float PtTitleSimilarity(const char* a, const char* b) {
    const int kMax = 32;
    int ca[kMax];
    int cb[kMax];
    int na = PtNormalizeCps(a, ca, kMax);
    int nb = PtNormalizeCps(b, cb, kMax);
    return PtLcsSim(ca, na, cb, nb);
}

// similarity of pre-normalized title cps against a body line
static float PtSimLine(const int* tcps, int nt, const char* lineText) {
    const int kMax = 32;
    int lcps[kMax];
    int nl = PtNormalizeCps(lineText, lcps, kMax);
    return PtLcsSim(tcps, nt, lcps, nl);
}

static bool PtVecContains(const Vec<int>& v, int x) {
    for (int i = 0; i < v.Size(); i++) {
        if (v[i] == x) {
            return true;
        }
    }
    return false;
}

// flattened body line: text + geometry (image space of the capture)
struct PtFlatLine {
    char* text;
    float x0, y0, x1, y1;
};

// Section headings often wrap onto two OCR lines, and textbook margin/boxed
// headings interleave with body lines in y order, so adjacency alone does
// not reconstruct them. Pre-build joined pairs of lines that sit close
// together vertically and share horizontal extent; a true wrapped heading
// joins to an exact title match (1.0 * 0.85 = 0.85, still above the 0.75
// re-anchor bar) while accidental cross-line substrings score far lower.
constexpr float kPtocJoinWeight = 0.85f;
constexpr float kPtocJoinMaxDyHeights = 3.0f;
constexpr float kPtocJoinMinXOverlap = 0.6f;
constexpr int kPtocJoinMaxPairsPerPage = 64;

static void PtBuildLinePairs(const Vec<PtFlatLine>& lines, int from, int to, Vec<int>& pairFirst, Vec<int>& pairSecond,
                             Vec<char*>& pairJoin) {
    if (to - from < 2) {
        return;
    }
    Vec<float> heights;
    for (int i = from; i < to; i++) {
        heights.Append(lines[i].y1 - lines[i].y0);
    }
    float medH = PtMedianOf(heights);
    if (medH <= 0) {
        return;
    }
    int nPairs = 0;
    for (int i = from; i < to && nPairs < kPtocJoinMaxPairsPerPage; i++) {
        float yci = 0.5f * (lines[i].y0 + lines[i].y1);
        float wi = lines[i].x1 - lines[i].x0;
        for (int j = i + 1; j < to && nPairs < kPtocJoinMaxPairsPerPage; j++) {
            float ycj = 0.5f * (lines[j].y0 + lines[j].y1);
            float dy = ycj - yci;
            if (dy > kPtocJoinMaxDyHeights * medH) {
                break; // lines are in y order: all further pairs are too far
            }
            float oxMin = lines[i].x1 < lines[j].x1 ? lines[i].x1 : lines[j].x1;
            float oxMax = lines[i].x0 > lines[j].x0 ? lines[i].x0 : lines[j].x0;
            float overlap = oxMin - oxMax;
            float wj = lines[j].x1 - lines[j].x0;
            float wMin = wi < wj ? wi : wj;
            if (wMin > 0 && overlap >= kPtocJoinMinXOverlap * wMin) {
                pairFirst.Append(i);
                pairSecond.Append(j);
                pairJoin.Append(str::FormatTemp("%s%s", lines[i].text, lines[j].text));
                nPairs++;
            }
        }
    }
}

void PtValidateEntriesWithBody(const Vec<PtPageData*>& bodyPages, Vec<PtEntryCandidate>* cands) {
    if (bodyPages.Size() == 0 || cands->Size() == 0) {
        return;
    }
    const PrintedTocConfig& cfg = PtocConfig();

    // flatten body line texts once (pageOff[p]..pageOff[p+1) = page p's lines)
    Vec<int> pageOff;
    Vec<char*> texts;
    for (int p = 0; p < bodyPages.Size(); p++) {
        pageOff.Append((int)texts.Size());
        const PtPageData& pg = *bodyPages[p];
        for (int l = 0; l < pg.lines.Size(); l++) {
            const PtRecLine& ln = *pg.lines[l];
            texts.Append(PtJoinTokenTexts(ln.tokens, ln.tokens.Size()));
        }
    }
    pageOff.Append((int)texts.Size());

    // Section headings often wrap onto two OCR lines, and textbook margin/boxed
    // headings interleave with body lines in y order, so plain adjacency misses
    // them (e.g. "实现人生价值" + "的条件和途径" separated by body lines). Match
    // against geometric line pairs instead: the join is weighted slightly below
    // a single-line match, so an exact joined heading (1.0 * 0.85 = 0.85) still
    // clears the 0.75 re-anchor bar, while accidental cross-line substrings
    // (e.g. "...人际" + "关系密切..." for title 人际关系) do not.
    const float kJoinWeight = 0.85f;
    Vec<int> pairFirst;   // global text index of the first line
    Vec<int> pairSecond;  // global text index of the second line
    Vec<char*> pairJoin;  // joined text, temp-allocated
    Vec<int> pairPageOff; // pairPageOff[p]..[p+1] = page p's pairs
    for (int p = 0; p < bodyPages.Size(); p++) {
        pairPageOff.Append((int)pairFirst.Size());
        const PtPageData& pg = *bodyPages[p];
        int nLines = pg.lines.Size();
        if (nLines < 2) {
            continue;
        }
        Vec<PtFlatLine> flat; // text borrowed from texts (no ownership)
        for (int l = 0; l < nLines; l++) {
            const PtRecLine& ln = *pg.lines[l];
            PtFlatLine fl;
            fl.text = texts[pageOff[p] + l];
            fl.x0 = ln.box.x0;
            fl.y0 = ln.box.y0;
            fl.x1 = ln.box.x1;
            fl.y1 = ln.box.y1;
            flat.Append(fl);
        }
        Vec<int> pf, ps;
        Vec<char*> pj;
        PtBuildLinePairs(flat, 0, nLines, pf, ps, pj);
        for (int k = 0; k < pj.Size(); k++) {
            pairFirst.Append(pageOff[p] + pf[k]);
            pairSecond.Append(pageOff[p] + ps[k]);
            pairJoin.Append(pj[k]);
        }
    }
    pairPageOff.Append((int)pairFirst.Size());

    // TOC pages themselves contain every entry title verbatim; they must never
    // act as body evidence, or a suspect entry would re-anchor onto the very
    // TOC page it was parsed from. Collect them from all candidates: a title
    // wrapped across TOC pages must not match its other half either.
    Vec<int> tocPages;
    for (int e2 = 0; e2 < cands->Size(); e2++) {
        int tp = (*cands)[e2].tocSourcePage;
        if (tp > 0 && !PtVecContains(tocPages, tp)) {
            tocPages.Append(tp);
        }
    }

    for (int e = 0; e < cands->Size(); e++) {
        PtEntryCandidate& c = (*cands)[e];
        if (!c.resolvedPdfPage.has_value()) {
            continue;
        }
        const int kMax = 32;
        int tcps[kMax];
        int nt = PtNormalizeCps(c.title ? c.title : "", tcps, kMax);
        if (nt < 2) {
            continue; // too short to validate meaningfully
        }
        int resolved = *c.resolvedPdfPage;
        // Suspect mappings (e.g. ordinal monotonicity breaks flagged after
        // PtResolvePdfPages) search the whole body: their prediction is the
        // part that is wrong, so the usual +/- few pages window would miss
        // the true heading page.
        bool suspect = c.pageMappingConf < 0.5f;
        float bestW = 0;   // distance-weighted best
        float bestRaw = 0; // raw best (for re-anchoring)
        int bestRawPage = -1;
        for (int p = 0; p < bodyPages.Size(); p++) {
            if (PtVecContains(tocPages, bodyPages[p]->pageIndex)) {
                continue;
            }
            int d = bodyPages[p]->pageIndex - resolved;
            int ad = d < 0 ? -d : d;
            if (!suspect && ad > cfg.bodySearchFarPages) {
                continue;
            }
            float weight = (ad <= cfg.bodySearchNearPages || suspect) ? 1.0f : 0.5f;
            for (int t = pageOff[p]; t < pageOff[p + 1]; t++) {
                float sim = PtSimLine(tcps, nt, texts[t]);
                if (sim * weight > bestW) {
                    bestW = sim * weight;
                }
                if (sim > bestRaw) {
                    bestRaw = sim;
                    bestRawPage = bodyPages[p]->pageIndex;
                }
            }
            for (int k = pairPageOff[p]; k < pairPageOff[p + 1]; k++) {
                float jsim = PtSimLine(tcps, nt, pairJoin[k]) * kJoinWeight;
                if (jsim * weight > bestW) {
                    bestW = jsim * weight;
                }
                if (jsim > bestRaw) {
                    bestRaw = jsim;
                    bestRawPage = bodyPages[p]->pageIndex;
                }
            }
        }
        c.bodyValidationConf = bestW;
        // a strong hit away from the prediction overrides a weak mapping
        if (bestRaw >= 0.75f && bestRawPage != resolved && c.pageMappingConf < 0.5f) {
            int ordinalOld = c.printedPage.hasOrdinal ? c.printedPage.ordinal : 0;
            c.resolvedPdfPage = bestRawPage;
            // The mapping offset (resolved - ordinal) was voted for by the
            // well-parsed majority; the misread was the ordinal itself (e.g.
            // OCR clipping a trailing digit). Restore the ordinal from the
            // re-anchored page so the entry stays internally consistent.
            if (ordinalOld > 0) {
                int repaired = bestRawPage - (resolved - ordinalOld);
                if (repaired >= 1) {
                    c.printedPage.ordinal = repaired;
                }
            }
            c.bodyValidationConf = bestRaw;
        }
    }

    for (int t2 = 0; t2 < (int)texts.Size(); t2++) {
        str::Free(texts[t2]);
    }
}

// A printed TOC lists its entries in reading order, so arabic ordinals must
// be non-decreasing. An ordinal that dips below its predecessor is a parse
// error - typically OCR clipping a digit off the trailing page number (e.g.
// "144" read as "14"). Flag it with low mapping confidence so body validation
// may re-anchor the entry (and repair the ordinal) from body evidence.
constexpr float kPtocSuspectMappingConf = 0.3f;

void PtFlagOrdinalMonotonicityBreaks(Vec<PtEntryCandidate>* cands) {
    if (!cands || cands->Size() == 0) {
        return;
    }
    int prevOrdinal = 0;
    for (int i = 0; i < cands->Size(); i++) {
        PtEntryCandidate& c = (*cands)[i];
        if (!c.printedPage.hasOrdinal || c.printedPage.type != PtPageLabelType::Arabic) {
            continue;
        }
        if (prevOrdinal > 0 && c.printedPage.ordinal < prevOrdinal && c.pageMappingConf > kPtocSuspectMappingConf) {
            c.pageMappingConf = kPtocSuspectMappingConf;
        }
        prevOrdinal = c.printedPage.ordinal;
    }
}

// ---------------------------------------------------------------------------
// document driver
// ---------------------------------------------------------------------------

struct PtPageOrder {
    int pageIndex;
    int srcIdx;
};

static int PtCmpPageOrder(const PtPageOrder* a, const PtPageOrder* b) {
    if (a->pageIndex != b->pageIndex) {
        return a->pageIndex < b->pageIndex ? -1 : 1;
    }
    return 0;
}

static void PtBuildOverlay(const PtPageData& page, const Vec<PtEntryCandidate>* cands, const PtTocPageScores& sc,
                           PtocOverlayPage* opOut);

bool PtBuildDocumentToc(const PtDocTocInput& in, Vec<PtEntryCandidate>* out, Vec<PtocOverlayPage*>* overlayOut) {
    out->Reset();
    if (overlayOut) {
        overlayOut->Reset();
    }
    if (!in.pages || in.pages->Size() == 0) {
        return false;
    }
    const PrintedTocConfig& cfg = PtocConfig();

    // process pages in ascending page order
    Vec<PtPageOrder> order;
    for (int i = 0; i < in.pages->Size(); i++) {
        PtPageOrder po;
        po.pageIndex = (*in.pages)[i]->pageIndex;
        po.srcIdx = i;
        order.Append(po);
    }
    order.SortTyped(PtCmpPageOrder);

    Vec<PtEntryCandidate> all;
    for (int k = 0; k < order.Size(); k++) {
        PtPageData& page = *(*in.pages)[order[k].srcIdx];
        if (page.tokens.Size() > 0) {
            PtReconstructLines(page);
        }
        PtTocPageScores sc = PtScoreTocPage(page);
        bool accepted = sc.total >= cfg.pageScoreMin;

        Vec<PtEntryCandidate> cands;
        if (accepted) {
            PtParseEntryCandidates(page, &cands);
            PtMergeWrappedEntries(page, &cands, /*keepTopOrphans=*/true);
            PtAssignHierarchyLevels(page, &cands);
            for (int i = 0; i < cands.Size(); i++) {
                all.Append(cands[i]); // steal strings
            }
        }
        if (overlayOut) {
            // heap page: ownership transfers to the caller's vec (no value
            // copy - PtocOverlayPage carries Vec members)
            PtocOverlayPage* op = new PtocOverlayPage();
            PtBuildOverlay(page, accepted ? &cands : nullptr, sc, op);
            op->isTocPage = accepted;
            overlayOut->Append(op);
        }
        cands.Reset(); // strings moved into all
    }
    if (all.Size() == 0) {
        return false;
    }

    PtMergeCrossPage(&all);
    if (all.Size() == 0) {
        return false;
    }
    PtNormalizeLevelsAcrossPages(&all);
    PtPromoteChapterHeadings(&all);

    // printed -> pdf mapping (footer anchors, or a seed from the TOC extent)
    Vec<PtPageMappingSegment> segs;
    Vec<PtPageAnchor> seedAnchors;
    const Vec<PtPageAnchor>* anchors = in.anchors;
    bool usedSeed = !anchors || anchors->Size() == 0;
    if (usedSeed) {
        int lastToc = 0;
        for (int i = 0; i < all.Size(); i++) {
            if (all[i].tocSourcePage > lastToc) {
                lastToc = all[i].tocSourcePage;
            }
        }
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 1;
        a.pdfPage = lastToc + 1; // body page 1 follows the last TOC page
        a.conf = 0.3f;
        seedAnchors.Append(a);
        anchors = &seedAnchors;
    }
    PtBuildPageMapping(*anchors, &segs);
    if (usedSeed) {
        // the seed is a whole-body assumption, not a measured pair: extend
        // its coverage past printed 1 so higher ordinals still map (staying
        // at the seed's low confidence)
        for (int i = 0; i < segs.Size(); i++) {
            PtPageMappingSegment& seg = segs[i];
            seg.printedEnd = in.nPdfPages > 0 ? in.nPdfPages - seg.offset : INT_MAX - seg.offset;
        }
    }
    PtResolvePdfPages(segs, in.nPdfPages, &all);
    PtFlagOrdinalMonotonicityBreaks(&all);

    if (in.bodyPages && in.bodyPages->Size() > 0) {
        for (int i = 0; i < in.bodyPages->Size(); i++) {
            PtPageData& bp = *(*in.bodyPages)[i];
            if (bp.tokens.Size() > 0) {
                PtReconstructLines(bp);
            }
        }
        PtValidateEntriesWithBody(*in.bodyPages, &all);
    }

    for (int i = 0; i < all.Size(); i++) {
        out->Append(all[i]); // steal strings
    }
    all.Reset();
    return out->Size() > 0;
}

// ---------------------------------------------------------------------------
// overlay + driver
// ---------------------------------------------------------------------------

static void PtBuildOverlay(const PtPageData& page, const Vec<PtEntryCandidate>* cands, const PtTocPageScores& sc,
                           PtocOverlayPage* out) {
    out->pageNo = page.pageIndex;
    out->pageScore = sc.total;
    out->isTocPage = sc.total >= PtocConfig().pageScoreMin;

    PtBoxF colBoxes[2];
    for (int i = 0; i < page.lines.Size(); i++) {
        const PtRecLine& ln = *page.lines[i];
        int col = ln.columnIndex > 0 ? 1 : 0;
        colBoxes[col].UnionWith(ln.box);
    }
    for (int c2 = 0; c2 < 2; c2++) {
        if (!colBoxes[c2].IsEmpty()) {
            out->columns.Append(colBoxes[c2]);
        }
    }

    if (cands && cands->Size() > 0) {
        float medCharW = PtMedianCharW(*cands);
        for (int col = 0; col < 2; col++) {
            Vec<float> centers;
            Vec<int> counts;
            PtIndentBandsForCol(page, *cands, col, medCharW, &centers, &counts);
            for (int b = 0; b < centers.Size(); b++) {
                out->indentBands.Append(centers[b]);
            }
        }
        for (int i = 0; i < cands->Size(); i++) {
            const PtEntryCandidate& c = (*cands)[i];
            if (c.printedPage.type == PtPageLabelType::Unknown) {
                continue;
            }
            PtocOverlayEntry e;
            e.titleBox = c.titleBox;
            e.pageNumberBox = c.pageNumberBox;
            e.level = c.level;
            e.mergedWithPrev = c.mergedWithPrev;
            e.parseConf = c.parseConf;
            out->entries.Append(e);
        }
    }
}

PtTocPageScores PtProcessPage(PtPageData& page, Vec<PtEntryCandidate>* candsOut, PtocOverlayPage* overlayOut) {
    PtReconstructLines(page);
    PtTocPageScores sc = PtScoreTocPage(page);
    if (candsOut) {
        PtParseEntryCandidates(page, candsOut);
        PtMergeWrappedEntries(page, candsOut);
        PtAssignHierarchyLevels(page, candsOut);
    }
    if (overlayOut) {
        PtBuildOverlay(page, candsOut, sc, overlayOut);
    }
    return sc;
}
