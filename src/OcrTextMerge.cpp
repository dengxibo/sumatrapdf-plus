/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "OcrTextMerge.h"

static int LastCodepoint(const char* s, int len) {
    if (!s || len <= 0) {
        return 0;
    }
    int i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
    }
    int idx = i;
    return Utf8CodepointNext(s, len, idx);
}

static int FirstCodepoint(const char* s) {
    if (!s || !s[0]) {
        return 0;
    }
    int idx = 0;
    return Utf8CodepointNext(s, (int)str::Len(s), idx);
}

static bool IsAsciiAlnum(int cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9');
}

static bool IsHanCp(int cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F);
}

bool OcrTextEndsWithTerminalPunct(const char* s) {
    if (!s || !s[0]) {
        return false;
    }
    int len = (int)str::Len(s);
    while (len > 0 && (u8)s[len - 1] == ' ') {
        len--;
    }
    int cp = LastCodepoint(s, len);
    switch (cp) {
        case '.':
        case '!':
        case '?':
        case ':':
        case ';':
        case 0x3002: // 。
        case 0xFF01: // ！
        case 0xFF1F: // ？
        case 0xFF1A: // ：
        case 0xFF1B: // ；
        case 0x2026: // …
        case 0x201D: // ”
        case 0x300D: // 」
        case 0x300F: // 』
        case 0xFF09: // ）
            return true;
    }
    return false;
}

// Rows of one page ordered in reading order. The stats mimic
// OcrShouldJoinDocLines (src/OcrService.cpp) but at line granularity and with
// axis transposition for vertical columns.
struct MergeStats {
    int em = 16;     // median line thickness (dy horizontal / dx vertical)
    int bodyLo = 0;  // body start on the reading axis (left / top)
    int bodyHi = 0;  // body end on the reading axis (right / bottom)
    int pitch = 0;   // median gap between consecutive lines
    int measure = 0; // bodyHi - bodyLo
};

static int MedianOf(Vec<int>& v, int def) {
    int n = v.Size();
    if (n == 0) {
        return def;
    }
    for (int i = 1; i < n; i++) {
        int key = v.At(i);
        int j = i - 1;
        while (j >= 0 && v.At(j) > key) {
            v.At(j + 1) = v.At(j);
            j--;
        }
        v.At(j + 1) = key;
    }
    return v.At(n / 2);
}

static int PercentileOf(Vec<int>& v, int pct, int def) {
    int n = v.Size();
    if (n == 0) {
        return def;
    }
    for (int i = 1; i < n; i++) {
        int key = v.At(i);
        int j = i - 1;
        while (j >= 0 && v.At(j) > key) {
            v.At(j + 1) = v.At(j);
            j--;
        }
        v.At(j + 1) = key;
    }
    int idx = n * pct / 100;
    if (idx >= n) {
        idx = n - 1;
    }
    return v.At(idx);
}

static MergeStats ComputeMergeStats(const Vec<OcrMergeLine>& lines, bool vertical) {
    MergeStats st;
    Vec<int> thick;
    Vec<int> lo;
    Vec<int> hi;
    Vec<int> gaps;
    int n = lines.Size();
    for (int i = 0; i < n; i++) {
        const Rect& r = lines.At(i).bbox;
        if (r.dx <= 0 && r.dy <= 0) {
            continue;
        }
        if (vertical) {
            if (r.dx > 0) {
                thick.Append(r.dx);
            }
            lo.Append(r.y);
            hi.Append(r.y + r.dy);
        } else {
            if (r.dy > 0) {
                thick.Append(r.dy);
            }
            lo.Append(r.x);
            hi.Append(r.x + r.dx);
        }
    }
    st.em = MedianOf(thick, 16);
    if (st.em < 4) {
        st.em = 16;
    }
    st.bodyLo = PercentileOf(lo, 15, 0);
    st.bodyHi = PercentileOf(hi, 85, st.bodyLo + st.em * 8);
    st.measure = st.bodyHi - st.bodyLo;
    if (st.measure < st.em * 4) {
        st.measure = st.em * 8;
    }
    for (int i = 1; i < n; i++) {
        Rect a = lines.At(i - 1).bbox;
        Rect b = lines.At(i).bbox;
        int gap;
        if (vertical) {
            // columns progress right to left
            gap = a.x - (b.x + b.dx);
            if (gap < 0) {
                gap = b.x - (a.x + a.dx);
            }
        } else {
            gap = b.y - (a.y + a.dy);
        }
        if (gap > 0) {
            gaps.Append(gap);
        }
    }
    st.pitch = MedianOf(gaps, 0);
    return st;
}

static bool CenteredShort(const Rect& r, const MergeStats& st, bool vertical) {
    // horizontal: short line floating between the body margins;
    // vertical: short column hanging from the top of the body.
    if (vertical) {
        int topPad = r.y - st.bodyLo;
        int bottomPad = st.bodyHi - (r.y + r.dy);
        return topPad < st.em && bottomPad > st.em * 3 / 2 && r.dy < st.measure * 2 / 3;
    }
    int leftPad = r.x - st.bodyLo;
    int rightPad = st.bodyHi - (r.x + r.dx);
    return leftPad > st.em * 3 / 2 && rightPad > st.em * 3 / 2 && r.dx < st.measure * 2 / 3;
}

static bool GapOk(const Rect& a, const Rect& b, const MergeStats& st, bool vertical) {
    int gap;
    if (vertical) {
        gap = a.x - (b.x + b.dx);
        if (gap < 0) {
            gap = b.x - (a.x + a.dx);
        }
        if (gap < 0) {
            gap = 0;
        }
        return st.pitch <= 0 || gap <= st.pitch * 3 + st.em;
    }
    gap = b.y - (a.y + a.dy);
    if (gap < 0) {
        gap = 0;
    }
    return st.pitch <= 0 || gap <= st.pitch * 2 + st.em / 2;
}

// similar line thickness (same font size): a wrapped title continues in the
// same size, while a centered heading is usually set larger
static bool SameLineThickness(const Rect& a, const Rect& b, bool vertical) {
    int ta = vertical ? a.dx : a.dy;
    int tb = vertical ? b.dx : b.dy;
    if (ta < 1) {
        ta = 1;
    }
    if (tb < 1) {
        tb = 1;
    }
    int diff = ta > tb ? ta - tb : tb - ta;
    return diff * 4 <= ta;
}

static bool CenteredInsidePreviousLine(const Rect& a, const Rect& b, int em) {
    int leftInset = b.x - a.x;
    int rightInset = (a.x + a.dx) - (b.x + b.dx);
    if (leftInset <= 0 || rightInset <= 0) {
        return false;
    }
    int insetDiff = leftInset > rightInset ? leftInset - rightInset : rightInset - leftInset;
    return insetDiff <= em;
}

static bool ShouldJoinLines(const OcrMergeLine& a, const OcrMergeLine& b, const MergeStats& st, bool vertical) {
    if (!a.text || !a.text[0] || !b.text || !b.text[0]) {
        return false;
    }
    if (OcrTextEndsWithTerminalPunct(a.text)) {
        return false;
    }
    if (!GapOk(a.bbox, b.bbox, st, vertical)) {
        return false;
    }
    bool aCentered = CenteredShort(a.bbox, st, vertical);
    bool bCentered = CenteredShort(b.bbox, st, vertical);
    if (vertical) {
        int aBottom = a.bbox.y + a.bbox.dy;
        bool aFull = aBottom >= st.bodyHi - st.em;
        int bTopPad = b.bbox.y - st.bodyLo;
        if (aFull) {
            // next column continues the paragraph when it starts at the top
            if (bTopPad < st.em + st.em / 2) {
                return true;
            }
            return bCentered && aCentered;
        }
        // vertical titles: several top-aligned short columns
        return aCentered && bCentered;
    }
    int aRight = a.bbox.x + a.bbox.dx;
    bool aFull = aRight >= st.bodyHi - st.em;
    if (aCentered && !bCentered) {
        // a centered block (title) must not absorb left-aligned body text
        return false;
    }
    if (!aFull) {
        // a short line ends its block unless both lines are centered title rows
        if (!aCentered || !bCentered) {
            return false;
        }
        return SameLineThickness(a.bbox, b.bbox, vertical);
    }
    // Native PDF title lines can be wider than the generic "centered short"
    // threshold. A shorter row inset equally on both sides is still an
    // unmistakable centered continuation, unlike a first-line indent whose
    // right edge remains aligned with the body measure.
    if (SameLineThickness(a.bbox, b.bbox, vertical) && CenteredInsidePreviousLine(a.bbox, b.bbox, st.em)) {
        return true;
    }
    int bLeftPad = b.bbox.x - st.bodyLo;
    if (bLeftPad < st.em + st.em / 2) {
        // no first-line indent: soft wrap
        return !bCentered || aCentered;
    }
    if (bCentered) {
        // indented centered line starts a new paragraph unless it continues a
        // wrapped title: previous line filled the measure, same glyph height
        return SameLineThickness(a.bbox, b.bbox, vertical);
    }
    return false;
}

static void AppendJoined(StrBuilder& sb, const char* s) {
    // CJK text flows without a separator; Latin words need one space.
    if (sb.Size() > 0 && s && s[0]) {
        int prev = LastCodepoint(sb.LendData(), (int)sb.Size());
        int next = FirstCodepoint(s);
        if (IsAsciiAlnum(prev) && IsAsciiAlnum(next)) {
            sb.Append(" ");
        } else if (!IsHanCp(prev) && !IsHanCp(next) && (IsAsciiAlnum(prev) || IsAsciiAlnum(next))) {
            sb.Append(" ");
        }
    }
    sb.Append(s);
}

void OcrMergeLayoutLines(StrVec& out, const Vec<OcrMergeLine>& lines, bool vertical) {
    int n = lines.Size();
    if (n == 0) {
        return;
    }
    MergeStats st = ComputeMergeStats(lines, vertical);
    StrBuilder cur;
    for (int i = 0; i < n; i++) {
        const OcrMergeLine& line = lines.At(i);
        if (!line.text || !line.text[0]) {
            continue;
        }
        if (cur.Size() == 0) {
            cur.Append(line.text);
            continue;
        }
        bool join = ShouldJoinLines(lines.At(i - 1), line, st, vertical);
        if (!join) {
            out.Append(cur.StealData());
            cur = StrBuilder();
            cur.Append(line.text);
            continue;
        }
        AppendJoined(cur, line.text);
    }
    if (cur.Size() > 0) {
        out.Append(cur.StealData());
    }
}

// vertical visual order: different columns read right to left, glyphs within
// a column read top to bottom. Pairwise overlap comparators are not
// transitive, so instead of sorting with one we cluster glyphs into columns
// by x-overlap (deterministic) and order clusters right to left.
void OcrMergeVerticalGlyphs(StrVec& out, const Vec<OcrMergeGlyph>& glyphs) {
    int n = glyphs.Size();
    if (n == 0) {
        return;
    }
    Vec<OcrMergeGlyph> gs;
    for (int i = 0; i < n; i++) {
        const OcrMergeGlyph& g = glyphs.At(i);
        if (str::IsWs(g.ch) || (g.bbox.dx <= 0 && g.bbox.dy <= 0)) {
            continue;
        }
        gs.Append(g);
    }
    int m = gs.Size();
    if (m == 0) {
        return;
    }

    // cluster glyphs into columns: walk glyphs sorted by x center and open a
    // new column whenever a glyph doesn't overlap the current column's box
    Vec<int> order; // indices into gs, sorted by x center ascending
    for (int i = 0; i < m; i++) {
        order.Append(i);
    }
    auto centerX = [&](int idx) { return gs.At(idx).bbox.x + gs.At(idx).bbox.dx * 0.5f; };
    for (int i = 1; i < m; i++) {
        int key = order.At(i);
        int j = i - 1;
        while (j >= 0 && centerX(order.At(j)) > centerX(key)) {
            order.At(j + 1) = order.At(j);
            j--;
        }
        order.At(j + 1) = key;
    }

    Vec<Vec<int>> columns;
    RectF colBox{};
    bool colOpen = false;
    for (int i = 0; i < m; i++) {
        const OcrMergeGlyph& g = gs.At(order.At(i));
        bool join = false;
        if (colOpen) {
            float overlap = std::min(colBox.x + colBox.dx, g.bbox.x + g.bbox.dx) - std::max(colBox.x, g.bbox.x);
            float minW = std::min(colBox.dx, g.bbox.dx);
            join = minW > 0 && overlap > 0.45f * minW;
        }
        if (!join) {
            Vec<int> col;
            columns.Append(col);
            colBox = g.bbox;
            colOpen = true;
        } else {
            colBox = colBox.Union(g.bbox);
        }
        columns.Last().Append(order.At(i));
    }

    // build the glyph sequence: clusters were formed left to right, vertical
    // text reads right to left, so walk clusters in reverse; within a column
    // sort glyphs top to bottom (stable by stream order)
    Vec<OcrMergeGlyph> mgs;
    for (int c = columns.Size() - 1; c >= 0; c--) {
        Vec<int>& col = columns.At(c);
        int cn = col.Size();
        for (int i = 1; i < cn; i++) {
            int key = col.At(i);
            int j = i - 1;
            while (j >= 0 && gs.At(col.At(j)).bbox.y > gs.At(key).bbox.y) {
                col.At(j + 1) = col.At(j);
                j--;
            }
            col.At(j + 1) = key;
        }
        for (int i = 0; i < cn; i++) {
            mgs.Append(gs.At(col.At(i)));
        }
    }

    // band the ordered glyphs into column lines and merge them
    Vec<OcrMergeLine> mls;
    Vec<char*> ownedTexts;
    WStrBuilder colText;
    RectF bandBox{};
    bool bandOpen = false;
    auto flushBand = [&]() {
        if (bandOpen && colText.size() > 0) {
            char* stolen = ToUtf8(colText.Get());
            OcrMergeLine ml;
            ml.text = stolen;
            ownedTexts.Append(stolen);
            ml.bbox = bandBox.Round();
            mls.Append(ml);
        }
        colText.Reset();
        bandOpen = false;
    };
    for (int i = 0; i < mgs.Size(); i++) {
        const OcrMergeGlyph& g = mgs.At(i);
        if (bandOpen) {
            float overlap = std::min(bandBox.x + bandBox.dx, g.bbox.x + g.bbox.dx) - std::max(bandBox.x, g.bbox.x);
            float minW = std::min(bandBox.dx, g.bbox.dx);
            if (minW <= 0 || overlap <= 0.45f * minW) {
                flushBand();
            }
        }
        if (!bandOpen) {
            bandBox = g.bbox;
            bandOpen = true;
        } else {
            bandBox = bandBox.Union(g.bbox);
        }
        colText.AppendChar(g.ch);
    }
    flushBand();
    OcrMergeLayoutLines(out, mls, true);
    DeleteVecMembers(ownedTexts);
}
