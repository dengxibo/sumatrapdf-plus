/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "DocController.h"
#include "EngineBase.h"
#include "Selection.h"
#include "TextSelection.h"

uint distSq(int x, int y) {
    return x * x + y * y;
}
// underscore is mainly used for programming and is thus considered a word character
bool isCjkWordChar(WCHAR c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF);
}

bool isWordChar(WCHAR c) {
    return IsCharAlphaNumeric(c) || c == '_' || isCjkWordChar(c);
}

bool isWordChar(int codepoint) {
    if (codepoint <= 0 || codepoint > 0x10FFFF) {
        return false;
    }
    return isWordChar((WCHAR)codepoint);
}

bool isNonCjkWordChar(WCHAR c) {
    return isWordChar(c) && (unsigned short)c < 0x2E80;
}

static bool isDigit(WCHAR c) {
    return c >= '0' && c <= '9';
}

TextSelection::TextSelection(EngineBase* engine) : engine(engine) {}

TextSelection::~TextSelection() {
    Reset();
}

void TextSelection::Reset() {
    result.len = 0;
    result.cap = 0;
    free(result.pages);
    result.pages = nullptr;
    free(result.rects);
    result.rects = nullptr;
    startPage = -1;
    endPage = -1;
    startGlyph = -1;
    endGlyph = -1;
    startDragX = 0;
    startDragY = 0;
    dragHoriz = 0;
    dragVert = 0;
}

static bool IsSelectableWhitespace(WCHAR c) {
    return str::IsWs(c);
}

static bool PageUsesVerticalGlyphLayout(TextSelection* ts, int pageNo);
static bool GlyphSharesAnchorColumn(TextSelection* ts, int pageNo, int anchor, int glyph);

static int GlyphIndexAtColumnPoint(TextSelection* ts, int pageNo, double x, double y) {
    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!text || !coords || textLen <= 0) {
        return -1;
    }
    float px = (float)x;
    float pickY = (float)y;
    int containing = -1;
    float containingY = -FLT_MAX;
    int nearest = -1;
    float nearestDy = FLT_MAX;
    for (int i = 0; i < textLen; i++) {
        if (!coords[i].dx && !coords[i].dy) {
            continue;
        }
        if (IsSelectableWhitespace(text[i])) {
            continue;
        }
        Rect& b = coords[i];
        float hitW = std::min((float)b.dx, std::max((float)b.dy * 1.2f, 6.0f));
        float cx = (float)b.x + (float)b.dx * 0.5f;
        if (px < cx - hitW * 0.5f || px > cx + hitW * 0.5f) {
            continue;
        }
        float cy = (float)b.y + (float)b.dy * 0.5f;
        float dy = std::abs(pickY - cy);
        if (dy < nearestDy) {
            nearestDy = dy;
            nearest = i;
        }
        if (pickY >= (float)b.y && pickY < (float)b.y + (float)b.dy) {
            if ((float)b.y > containingY) {
                containingY = (float)b.y;
                containing = i;
            }
        }
    }
    int best = containing >= 0 ? containing : nearest;
    return best;
}

// returns the glyph index under (x,y), or -1 if the click is not inside any glyph bbox
static int GlyphIndexUnderPoint(TextSelection* ts, int pageNo, double x, double y) {
    if (PageUsesVerticalGlyphLayout(ts, pageNo)) {
        return GlyphIndexAtColumnPoint(ts, pageNo, x, y);
    }
    int textLen;
    Rect* coords;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || textLen <= 0) {
        return -1;
    }
    Point pt = ToPoint(PointF(x, y));
    int best = -1;
    unsigned int bestDist = UINT_MAX;
    for (int i = 0; i < textLen; i++) {
        Rect& coord = coords[i];
        if (!coord.dx && !coord.dy) {
            continue;
        }
        if (!coord.Contains(pt)) {
            continue;
        }
        uint dist = distSq(pt.x - coord.x - coord.dx / 2, pt.y - coord.y - coord.dy / 2);
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

static bool GlyphColumnOverlaps(RectF a, RectF b) {
    if (a.dx <= 0 || b.dx <= 0) {
        return false;
    }
    float overlap = std::min(a.x + a.dx, b.x + b.dx) - std::max(a.x, b.x);
    float minW = std::min(a.dx, b.dx);
    return overlap > 0.45f * minW;
}

// True when anchor sits in a vertical column (stacked glyphs sharing X, differing Y).
static bool GlyphInVerticalColumn(TextSelection* ts, int pageNo, int anchor) {
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || anchor < 0 || anchor >= textLen) {
        return false;
    }
    RectF ab = ts->engine->Transform(ToRectF(coords[anchor]), pageNo, 1.0, 0);
    if (ab.dx <= 0 || ab.dy <= 0) {
        return false;
    }
    auto stackedNeighbor = [&](int j) -> bool {
        if (j < 0 || j >= textLen || j == anchor) {
            return false;
        }
        if (!coords[j].dx && !coords[j].dy) {
            return false;
        }
        RectF b = ts->engine->Transform(ToRectF(coords[j]), pageNo, 1.0, 0);
        if (b.dx <= 0 || b.dy <= 0) {
            return false;
        }
        if (!GlyphColumnOverlaps(ab, b)) {
            return false;
        }
        return std::abs(b.y - ab.y) > ab.dy * 0.35f;
    };
    for (int j = anchor + 1; j < textLen && j <= anchor + 8; j++) {
        if (!coords[j].dx && !coords[j].dy) {
            continue;
        }
        if (stackedNeighbor(j)) {
            return true;
        }
        break;
    }
    for (int j = anchor - 1; j >= 0 && j >= anchor - 8; j--) {
        if (!coords[j].dx && !coords[j].dy) {
            continue;
        }
        if (stackedNeighbor(j)) {
            return true;
        }
        break;
    }
    return false;
}

// Vertical reflow EPUBs often store one glyph per stext line (newline between each char).
static bool PageUsesVerticalGlyphLayout(TextSelection* ts, int pageNo) {
    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!text || !coords || textLen < 6) {
        return false;
    }
    int withBox = 0;
    int stacked = 0;
    for (int i = 0; i < textLen; i++) {
        if (!coords[i].dx && !coords[i].dy) {
            continue;
        }
        withBox++;
        RectF a = ts->engine->Transform(ToRectF(coords[i]), pageNo, 1.0, 0);
        for (int j = i + 1; j < textLen && j <= i + 8; j++) {
            if (!coords[j].dx && !coords[j].dy) {
                continue;
            }
            RectF b = ts->engine->Transform(ToRectF(coords[j]), pageNo, 1.0, 0);
            if (!GlyphColumnOverlaps(a, b)) {
                break;
            }
            if (std::abs(b.y - a.y) > a.dy * 0.35f) {
                stacked++;
            }
            break;
        }
    }
    if (withBox >= 6 && stacked * 4 >= withBox) {
        return true;
    }

    // Vertical reflow EPUBs often insert a newline between every glyph.
    int printable = 0;
    int nlStacked = 0;
    for (int i = 0; i + 2 < textLen; i++) {
        if (!coords[i].dx && !coords[i].dy) {
            continue;
        }
        if (IsSelectableWhitespace(text[i])) {
            continue;
        }
        printable++;
        if (coords[i + 1].dx || coords[i + 1].dy) {
            continue;
        }
        int j = i + 2;
        if (j >= textLen || (!coords[j].dx && !coords[j].dy)) {
            continue;
        }
        RectF a = ts->engine->Transform(ToRectF(coords[i]), pageNo, 1.0, 0);
        RectF b = ts->engine->Transform(ToRectF(coords[j]), pageNo, 1.0, 0);
        if (!GlyphColumnOverlaps(a, b)) {
            continue;
        }
        if (std::abs(b.y - a.y) > a.dy * 0.35f) {
            nlStacked++;
        }
    }
    return printable >= 6 && nlStacked * 3 >= printable;
}

static int CompareGlyphVisualOrder(TextSelection* ts, int pageNo, bool vertical, int a, int b) {
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || a < 0 || b < 0 || a >= textLen || b >= textLen) {
        return 0;
    }
    RectF ra = ts->engine->Transform(ToRectF(coords[a]), pageNo, 1.0, 0);
    RectF rb = ts->engine->Transform(ToRectF(coords[b]), pageNo, 1.0, 0);
    if (vertical) {
        float overlap = std::min(ra.x + ra.dx, rb.x + rb.dx) - std::max(ra.x, rb.x);
        float minW = std::min(ra.dx, rb.dx);
        if (minW > 0 && overlap <= minW * 0.45f) {
            return ra.x > rb.x ? -1 : 1;
        }
        if (ra.y < rb.y) {
            return -1;
        }
        if (ra.y > rb.y) {
            return 1;
        }
        return 0;
    }
    if (ra.y < rb.y) {
        return -1;
    }
    if (ra.y > rb.y) {
        return 1;
    }
    if (ra.x < rb.x) {
        return -1;
    }
    if (ra.x > rb.x) {
        return 1;
    }
    return 0;
}

static void AppendPageGlyphsInVisualOrder(TextSelection* ts, int pageNo, int fromGlyph, int toGlyph, StrVec& lines,
                                          bool vertical) {
    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!text || !coords || textLen <= 0) {
        return;
    }
    fromGlyph = limitValue(fromGlyph, 0, textLen);
    toGlyph = limitValue(toGlyph, 0, textLen);
    if (fromGlyph >= toGlyph) {
        return;
    }

    Vec<int> glyphs;
    for (int i = fromGlyph; i < toGlyph; i++) {
        if (!coords[i].dx && !coords[i].dy) {
            continue;
        }
        if (IsSelectableWhitespace(text[i])) {
            continue;
        }
        glyphs.Append(i);
    }
    if (glyphs.Size() == 0) {
        return;
    }

    for (int i = 0; i < glyphs.Size(); i++) {
        for (int j = i + 1; j < glyphs.Size(); j++) {
            if (CompareGlyphVisualOrder(ts, pageNo, vertical, glyphs[i], glyphs[j]) > 0) {
                int tmp = glyphs[i];
                glyphs[i] = glyphs[j];
                glyphs[j] = tmp;
            }
        }
    }

    for (int i = 0; i < glyphs.Size(); i++) {
        WCHAR ch[2] = {text[glyphs[i]], 0};
        lines.Append(ToUtf8Temp(ch));
    }
}

// returns the index of the glyph closest to the right of the given coordinates
// (i.e. when over the right half of a glyph, the returned index will be for the
// glyph following it, which will be the first glyph (not) to be selected)
static int FindClosestGlyph(TextSelection* ts, int pageNo, double x, double y) {
    if (PageUsesVerticalGlyphLayout(ts, pageNo)) {
        int g = GlyphIndexAtColumnPoint(ts, pageNo, x, y);
        if (g >= 0) {
            return g;
        }
    }
    int textLen;
    Rect* coords;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || textLen <= 0) {
        return 0;
    }
    PointF pt = PointF(x, y);

    unsigned int maxDist = UINT_MAX;
    Point pti = ToPoint(pt);
    bool overGlyph = false;
    int result = -1;

    for (int i = 0; i < textLen; i++) {
        Rect& coord = coords[i];
        if (!coord.x && !coord.dx) {
            continue;
        }
        if (overGlyph && !coord.Contains(pti)) {
            continue;
        }

        uint dist = distSq((int)x - coord.x - coord.dx / 2, (int)y - coord.y - coord.dy / 2);
        if (dist < maxDist) {
            result = i;
            maxDist = dist;
        }
        // prefer glyphs the cursor is actually over
        if (!overGlyph && coord.Contains(pti)) {
            overGlyph = true;
            result = i;
            maxDist = dist;
        }
    }

    if (-1 == result) {
        return 0;
    }
    ReportIf(result < 0 || result >= textLen);

    // click is inside this glyph's bbox: return it directly (needed for CJK where
    // each character is one glyph; the right-half rule below would select the next char)
    if (coords[result].Contains(pti)) {
        return result;
    }

    RectF bbox = ts->engine->Transform(ToRectF(coords[result]), pageNo, 1.0, 0);
    pt = ts->engine->Transform(pt, pageNo, 1.0, 0);
    if (GlyphInVerticalColumn(ts, pageNo, result)) {
        if (pt.y > bbox.y + 0.5f * bbox.dy) {
            result++;
            while (result < textLen && coords[result - 1] == coords[result]) {
                result++;
            }
        }
        return result;
    }

    // the result indexes the first glyph to be selected in a forward selection
    bbox = ts->engine->Transform(ToRectF(coords[result]), pageNo, 1.0, 0);
    pt = ts->engine->Transform(pt, pageNo, 1.0, 0);
    if (pt.x > bbox.x + 0.5 * bbox.dx) {
        result++;
        // for some (DjVu) documents, all glyphs of a word share the same bbox
        while (result < textLen && coords[result - 1] == coords[result]) {
            result++;
        }
    }
    ReportIf(result > 0 && result < textLen && coords[result] == coords[result - 1]);

    return result;
}

static void ComputeGlyphRangeFromEndpoints(const TextSelection* ts, int* fromPage, int* toPage, int* fromGlyph,
                                           int* toGlyph) {
    *fromPage = std::min(ts->startPage, ts->endPage);
    *toPage = std::max(ts->startPage, ts->endPage);

    // Right-to-left on one page: [endGlyph, anchor + 1) with inclusive endGlyph.
    if (ts->startPage == ts->endPage && ts->startGlyph > ts->endGlyph) {
        int textLen = 0;
        ts->engine->GetTextForPage(ts->startPage, &textLen);
        int anchor = ts->startGlyph;
        int toExclusive = anchor + 1;
        if (toExclusive > textLen) {
            toExclusive = textLen;
        }
        *fromGlyph = ts->endGlyph;
        *toGlyph = toExclusive;
        if (*fromGlyph > *toGlyph) {
            std::swap(*fromGlyph, *toGlyph);
        }
        return;
    }

    *fromGlyph = (*fromPage == ts->endPage ? ts->endGlyph : ts->startGlyph);
    *toGlyph = (*fromPage == ts->endPage ? ts->startGlyph : ts->endGlyph);
    if (*fromPage == *toPage && *fromGlyph > *toGlyph) {
        std::swap(*fromGlyph, *toGlyph);
    }
}

static bool GlyphOnDragRow(TextSelection* ts, int pageNo, double x, double y, int glyph) {
    (void)x;
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || glyph < 0 || glyph >= textLen) {
        return false;
    }
    RectF gb = ts->engine->Transform(ToRectF(coords[glyph]), pageNo, 1.0, 0);
    if (gb.dy <= 0) {
        return false;
    }
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);
    float slack = gb.dy * 0.35f;
    return pt.y >= gb.y - slack && pt.y <= gb.y + gb.dy + slack;
}

static bool GlyphSharesAnchorRow(TextSelection* ts, int pageNo, int anchor, int glyph) {
    if (glyph < 0) {
        return false;
    }
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || anchor < 0 || anchor >= textLen || glyph >= textLen) {
        return false;
    }
    RectF ab = ts->engine->Transform(ToRectF(coords[anchor]), pageNo, 1.0, 0);
    RectF b = ts->engine->Transform(ToRectF(coords[glyph]), pageNo, 1.0, 0);
    if (ab.dy <= 0 || b.dy <= 0) {
        return false;
    }
    // Overlap test: anchor midline row was too strict for math (subscripts / arg max blocks).
    return ab.y < b.y + b.dy && b.y < ab.y + ab.dy;
}

// SelectUpTo uses exclusive end indices [start, end) for left-to-right drags.
static int ForwardExclusiveEndFromX(TextSelection* ts, int pageNo, double x, double y, int startGlyph) {
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || textLen <= 0 || startGlyph < 0 || startGlyph >= textLen) {
        return startGlyph + 1;
    }
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);
    int end = startGlyph + 1;
    for (int i = startGlyph; i < textLen; i++) {
        RectF b = ts->engine->Transform(ToRectF(coords[i]), pageNo, 1.0, 0);
        if (b.dx <= 0 && b.dy <= 0) {
            continue;
        }
        if (i > startGlyph && !GlyphOnDragRow(ts, pageNo, x, y, i)) {
            // Skip subscripts etc.; keep scanning for later glyphs on the drag row (e.g. Q after arg max).
            if (pt.x < b.x) {
                break;
            }
            continue;
        }
        float right = b.x + b.dx;
        float mid = b.x + 0.5f * b.dx;
        if (pt.x < b.x) {
            break;
        }
        if (i == startGlyph) {
            end = startGlyph + 1;
            if (pt.x <= right) {
                break;
            }
            continue;
        }
        if (pt.x <= mid) {
            end = i + 1;
            break;
        }
        // Right half of glyph i: still on this character — exclusive end is after i, not i+1.
        // (Previously end = i + 2 here selected the next glyph while the cursor was still on i.)
        end = i + 1;
        if (pt.x <= right) {
            break;
        }
        end = i + 2;
    }
    if (end > textLen) {
        end = textLen;
    }
    if (end <= startGlyph) {
        end = startGlyph + 1;
    }
    return end;
}

// Right-to-left drags use inclusive start indices [end, anchor + 1).
static int BackwardInclusiveStartFromX(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || textLen <= 0 || anchor <= 0) {
        return anchor;
    }
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);

    int from = anchor;
    for (int i = anchor - 1; i >= 0; i--) {
        RectF b = ts->engine->Transform(ToRectF(coords[i]), pageNo, 1.0, 0);
        if (b.dx <= 0 && b.dy <= 0) {
            continue;
        }
        if (!GlyphOnDragRow(ts, pageNo, x, y, i)) {
            continue;
        }
        float right = b.x + b.dx;
        if (pt.x < b.x) {
            continue;
        }
        if (pt.x < right) {
            from = i;
            break;
        }
        // Cursor is to the right of glyph i (gap); try glyphs further left on this line.
    }
    if (from >= anchor) {
        return anchor;
    }
    return from;
}

static bool GlyphOnDragColumn(TextSelection* ts, int pageNo, double x, double y, int glyph) {
    (void)y;
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || glyph < 0 || glyph >= textLen) {
        return false;
    }
    RectF gb = ts->engine->Transform(ToRectF(coords[glyph]), pageNo, 1.0, 0);
    if (gb.dx <= 0) {
        return false;
    }
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);
    float slack = gb.dx * 0.35f;
    return pt.x >= gb.x - slack && pt.x <= gb.x + gb.dx + slack;
}

static bool GlyphSharesAnchorColumn(TextSelection* ts, int pageNo, int anchor, int glyph) {
    if (glyph < 0) {
        return false;
    }
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || anchor < 0 || anchor >= textLen || glyph >= textLen) {
        return false;
    }
    RectF ab = ts->engine->Transform(ToRectF(coords[anchor]), pageNo, 1.0, 0);
    RectF b = ts->engine->Transform(ToRectF(coords[glyph]), pageNo, 1.0, 0);
    if (ab.dx <= 0 || b.dx <= 0) {
        return false;
    }
    return ab.x < b.x + b.dx && b.x < ab.x + ab.dx;
}

static int ForwardExclusiveEndFromY(TextSelection* ts, int pageNo, double x, double y, int startGlyph) {
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || textLen <= 0 || startGlyph < 0 || startGlyph >= textLen) {
        return startGlyph + 1;
    }
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);
    int end = startGlyph + 1;
    for (int i = startGlyph; i < textLen; i++) {
        RectF b = ts->engine->Transform(ToRectF(coords[i]), pageNo, 1.0, 0);
        if (b.dx <= 0 && b.dy <= 0) {
            continue;
        }
        if (i > startGlyph && !GlyphOnDragColumn(ts, pageNo, x, y, i)) {
            if (pt.y < b.y) {
                break;
            }
            continue;
        }
        float bottom = b.y + b.dy;
        float mid = b.y + 0.5f * b.dy;
        if (pt.y < b.y) {
            break;
        }
        if (i == startGlyph) {
            end = startGlyph + 1;
            if (pt.y <= bottom) {
                break;
            }
            continue;
        }
        if (pt.y <= mid) {
            end = i + 1;
            break;
        }
        end = i + 1;
        if (pt.y <= bottom) {
            break;
        }
        end = i + 2;
    }
    if (end > textLen) {
        end = textLen;
    }
    if (end <= startGlyph) {
        end = startGlyph + 1;
    }
    return end;
}

static bool CursorSharesAnchorColumn(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    (void)y;
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || anchor < 0 || anchor >= textLen) {
        return true;
    }
    RectF ab = ts->engine->Transform(ToRectF(coords[anchor]), pageNo, 1.0, 0);
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);
    float slack = ab.dx > 0 ? ab.dx * 0.4f : 3.0f;
    return pt.x >= ab.x - slack && pt.x <= ab.x + ab.dx + slack;
}

static bool CursorSharesAnchorLine(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    (void)x;
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!coords || anchor < 0 || anchor >= textLen) {
        return true;
    }
    RectF ab = ts->engine->Transform(ToRectF(coords[anchor]), pageNo, 1.0, 0);
    PointF pt = ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0);
    float slack = ab.dy > 0 ? ab.dy * 0.4f : 3.0f;
    return pt.y >= ab.y - slack && pt.y <= ab.y + ab.dy + slack;
}

static bool DragExtendsLeftOfAnchor(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    (void)y;
    (void)anchor;
    if (pageNo != ts->startPage) {
        return false;
    }
    return x < ts->startDragX;
}

static void UpdateDragHorizLock(TextSelection* ts, double x) {
    if (ts->dragHoriz != 0) {
        return;
    }
    const float kLockEps = 1.0f;
    if (x < ts->startDragX - kLockEps) {
        ts->dragHoriz = -1;
    } else if (x > ts->startDragX + kLockEps) {
        ts->dragHoriz = 1;
    }
}

static bool DragUsesRtlEndpoint(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    UpdateDragHorizLock(ts, x);
    if (ts->dragHoriz < 0) {
        return true;
    }
    if (ts->dragHoriz > 0) {
        return false;
    }
    return DragExtendsLeftOfAnchor(ts, pageNo, x, y, anchor);
}

static bool DragExtendsAboveAnchor(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    (void)x;
    (void)anchor;
    if (pageNo != ts->startPage) {
        return false;
    }
    return y < ts->startDragY;
}

static void UpdateDragVertLock(TextSelection* ts, double y) {
    if (ts->dragVert != 0) {
        return;
    }
    const float kLockEps = 1.0f;
    if (y < ts->startDragY - kLockEps) {
        ts->dragVert = -1;
    } else if (y > ts->startDragY + kLockEps) {
        ts->dragVert = 1;
    }
}

static bool DragUsesBackwardVerticalEndpoint(TextSelection* ts, int pageNo, double x, double y, int anchor) {
    UpdateDragVertLock(ts, y);
    if (ts->dragVert < 0) {
        return true;
    }
    if (ts->dragVert > 0) {
        return false;
    }
    return DragExtendsAboveAnchor(ts, pageNo, x, y, anchor);
}

static int GlyphIndexForDragEndpoint(TextSelection* ts, int pageNo, double x, double y) {
    if (pageNo == ts->startPage && ts->startGlyph >= 0) {
        int anchor = ts->startGlyph;
        bool vertical = PageUsesVerticalGlyphLayout(ts, pageNo) || GlyphInVerticalColumn(ts, pageNo, anchor);

        if (vertical) {
            bool sameColumn = CursorSharesAnchorColumn(ts, pageNo, x, y, anchor);
            int offColG = -1;
            if (!sameColumn) {
                offColG = FindClosestGlyph(ts, pageNo, x, y);
                if (GlyphSharesAnchorColumn(ts, pageNo, anchor, offColG)) {
                    sameColumn = true;
                }
            }
            if (!sameColumn) {
                return offColG;
            }

            if (DragUsesBackwardVerticalEndpoint(ts, pageNo, x, y, anchor)) {
                int under = GlyphIndexAtColumnPoint(ts, pageNo, x, y);
                if (under >= 0 && under < anchor) {
                    return under;
                }
                return anchor + 1;
            }
            int under = GlyphIndexAtColumnPoint(ts, pageNo, x, y);
            int end = anchor + 1;
            if (under >= anchor) {
                end = under + 1;
            }
            return end;
        }

        bool sameLine = CursorSharesAnchorLine(ts, pageNo, x, y, anchor);
        int offLineG = -1;
        if (!sameLine) {
            offLineG = FindClosestGlyph(ts, pageNo, x, y);
            if (GlyphSharesAnchorRow(ts, pageNo, anchor, offLineG)) {
                sameLine = true;
            }
        }
        if (!sameLine) {
            return offLineG;
        }

        if (DragUsesRtlEndpoint(ts, pageNo, x, y, anchor)) {
            int bwd = BackwardInclusiveStartFromX(ts, pageNo, x, y, anchor);
            if (bwd < anchor) {
                return bwd;
            }
            int closest = FindClosestGlyph(ts, pageNo, x, y);
            if (closest < anchor && GlyphOnDragRow(ts, pageNo, x, y, closest)) {
                return closest;
            }
            return anchor + 1;
        }
        int fwd = ForwardExclusiveEndFromX(ts, pageNo, x, y, anchor);
        int closestFwd = FindClosestGlyph(ts, pageNo, x, y);
        if (closestFwd > anchor && GlyphOnDragRow(ts, pageNo, x, y, closestFwd)) {
            int textLen = 0;
            Rect* coords = nullptr;
            ts->engine->GetTextForPage(pageNo, &textLen, &coords);
            int cand = closestFwd;
            if (coords && closestFwd >= 0 && closestFwd < textLen) {
                Point pti = ToPoint(ts->engine->Transform(PointF(x, y), pageNo, 1.0, 0));
                if (coords[closestFwd].Contains(pti)) {
                    cand = closestFwd + 1;
                }
            }
            if (cand > fwd) {
                fwd = cand;
            }
        }
        return fwd;
    }
    int g = FindClosestGlyph(ts, pageNo, x, y);
    int textLen = 0;
    Rect* coords = nullptr;
    ts->engine->GetTextForPage(pageNo, &textLen, &coords);
    Point pti = ToPoint(PointF(x, y));
    if (coords && g >= 0 && g < textLen && coords[g].Contains(pti)) {
        int anchor = ts->startGlyph;
        if (anchor >= 0 && GlyphInVerticalColumn(ts, pageNo, anchor)) {
            return ForwardExclusiveEndFromY(ts, pageNo, x, y, anchor);
        }
        return ForwardExclusiveEndFromX(ts, pageNo, x, y, anchor);
    }
    return g;
}

// Detects the first glyph of the next visual line when joined stext lines share
// no zero-width separator glyph (e.g. CJK paragraph join in EngineMupdf): the
// glyph is vertically separated from the current line band, unlike sub/superscripts
// which overlap their base glyph substantially. Works for both top-aligned and
// centered/indented lines where consecutive lines need not overlap in X.
static bool GlyphJumpsToNextBandLine(const Rect& band, const Rect& c) {
    if (band.dy <= 0 || c.dy <= 0) {
        return false;
    }
    int yOverlap = std::min(band.y + band.dy, c.y + c.dy) - std::max(band.y, c.y);
    int minDy = std::min(band.dy, c.dy);
    return yOverlap * 10 < minDy * 3;
}

static void FillResultRects(TextSelection* ts, int pageNo, int glyph, int length, StrVec* lines = nullptr) {
    int len;
    Rect* coords;
    const WCHAR* text = ts->engine->GetTextForPage(pageNo, &len, &coords);
    // Selection endpoints can go stale after a relayout (e.g. toggling dark/light
    // theme re-flows an EPUB and changes a page's text length). Clamp the range
    // instead of reading past the coords/text arrays (out-of-bounds in release).
    if (!text || !coords || len <= 0) {
        return;
    }
    glyph = limitValue(glyph, 0, len);
    if (length < 0) {
        length = 0;
    }
    if (glyph + length > len) {
        length = len - glyph;
    }
    if (length <= 0) {
        return;
    }
    Rect mediabox = ts->engine->PageMediabox(pageNo).Round();
    // Joined stext lines (e.g. CJK paragraph join) share no zero-width separator
    // glyph, so visual lines must additionally be split geometrically. Vertical
    // glyph layouts stack glyphs along Y inside a column and must not be split.
    bool geoSplit = !PageUsesVerticalGlyphLayout(ts, pageNo);
    Rect *c = &coords[glyph], *end = c + length;
    while (c < end) {
        // skip line breaks
        for (; c < end && !c->x && !c->dx; c++) {
            // no-op
        }
        if (c >= end) {
            break;
        }

        Rect* c0 = c;
        Rect band = *c0;
        for (; c < end && (c->x || c->dx); c++) {
            if (geoSplit && c != c0 && GlyphJumpsToNextBandLine(band, *c)) {
                break;
            }
            band = band.Union(*c);
            // find the end of this visual line
        }

        Rect bbox = BuildHighlightLineRect(c0, c);
        if (bbox.IsEmpty()) {
            continue;
        }
        bbox = bbox.Intersect(mediabox);
        // skip text that's completely outside a page's mediabox
        if (bbox.IsEmpty()) {
            continue;
        }

        if (lines) {
            char* s = ToUtf8Temp(text + (c0 - coords), c - c0);
            lines->Append(s);
            continue;
        }

        // cut the right edge, if it overlaps the next character on the same line
        if (c < coords + len && (c->x || c->dx) && bbox.x < c->x && bbox.x + bbox.dx > c->x &&
            bbox.y < c->y + c->dy && c->y < bbox.y + bbox.dy) {
            bbox.dx = c->x - bbox.x;
        }

        int currLen = ts->result.len;
        int left = ts->result.cap - currLen;
        ReportIf(left < 0);
        if (left == 0) {
            int newCap = ts->result.cap * 2;
            if (newCap < 64) {
                newCap = 64;
            }
            int* newPages = (int*)realloc(ts->result.pages, sizeof(int) * newCap);
            Rect* newRects = (Rect*)realloc(ts->result.rects, sizeof(Rect) * newCap);
            ReportIf(!newPages);
            ReportIf(!newRects);
            ts->result.pages = newPages;
            ts->result.rects = newRects;
            ts->result.cap = newCap;
        }

        ts->result.pages[currLen] = pageNo;
        ts->result.rects[currLen] = bbox;
        ts->result.len++;
    }
}

bool TextSelection::IsOverGlyph(int pageNo, double x, double y) {
    int glyphIx = GlyphIndexUnderPoint(this, pageNo, x, y);
    if (glyphIx < 0) {
        return false;
    }
    int textLen;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
    if (!text || glyphIx >= textLen) {
        return false;
    }
    return !IsSelectableWhitespace(text[glyphIx]);
}

void TextSelection::StartAt(int pageNo, int glyphIx) {
    startPage = pageNo;
    startGlyph = glyphIx;
    endPage = -1;
    endGlyph = -1;
    if (glyphIx < 0) {
        int textLen;
        engine->GetTextForPage(pageNo, &textLen);
        startGlyph += textLen + 1;
    }
}

void TextSelection::StartAt(int pageNo, double x, double y) {
    startDragX = (float)x;
    startDragY = (float)y;
    dragHoriz = 0;
    dragVert = 0;
    int ix = GlyphIndexUnderPoint(this, pageNo, x, y);
    if (ix < 0) {
        ix = FindClosestGlyph(this, pageNo, x, y);
    }
    StartAt(pageNo, ix);
}

void TextSelection::SelectUpTo(int pageNo, double x, double y) {
    SelectUpTo(pageNo, GlyphIndexForDragEndpoint(this, pageNo, x, y));
}

void TextSelection::SelectUpTo(int pageNo, int glyphIx) {
    if (startPage == -1 || startGlyph == -1) {
        return;
    }

    endPage = pageNo;
    endGlyph = glyphIx;
    if (glyphIx < 0) {
        int textLen;
        engine->GetTextForPage(pageNo, &textLen);
        endGlyph = textLen + glyphIx + 1;
    }

    result.len = 0;
    int fromPage, toPage, fromGlyph, toGlyph;
    ComputeGlyphRangeFromEndpoints(this, &fromPage, &toPage, &fromGlyph, &toGlyph);

    for (int page = fromPage; page <= toPage; page++) {
        int textLen;
        engine->GetTextForPage(page, &textLen);

        int glyph = page == fromPage ? fromGlyph : 0;
        int length = (page == toPage ? toGlyph : textLen) - glyph;
        if (length > 0) {
            FillResultRects(this, page, glyph, length);
        }
    }
}

// extend backward across comma-separated digit groups (e.g. "1,234,567")
// returns the new start position if valid grouping found, otherwise returns curStart
static int ExtendBackAcrossCommaGroups(const WCHAR* text, int curStart) {
    int pos = curStart;
    while (pos >= 2 && text[pos - 1] == ',') {
        // count digits before the comma
        int j = pos - 2;
        int nDigits = 0;
        while (j >= 0 && isDigit(text[j])) {
            nDigits++;
            j--;
        }
        if (nDigits == 0) {
            break;
        }
        pos = j + 1;
    }
    return pos;
}

// extend forward across comma-separated digit groups (e.g. ",234,567")
// returns the new end position
static int ExtendForwardAcrossCommaGroups(const WCHAR* text, int textLen, int curEnd) {
    int pos = curEnd;
    while (pos < textLen && text[pos] == ',') {
        // count digits after the comma
        int j = pos + 1;
        int nDigits = 0;
        while (j < textLen && isDigit(text[j])) {
            nDigits++;
            j++;
        }
        if (nDigits == 0) {
            break;
        }
        pos = j;
    }
    return pos;
}

void TextSelection::SelectWordAt(int pageNo, double x, double y) {
    int glyphIx = GlyphIndexUnderPoint(this, pageNo, x, y);
    int textLen;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
    if (!text || textLen <= 0 || glyphIx < 0 || glyphIx >= textLen) {
        return;
    }

    WCHAR clickChar = text[glyphIx];
    if (IsSelectableWhitespace(clickChar)) {
        return;
    }
    if (!isWordChar(clickChar)) {
        StartAt(pageNo, glyphIx);
        SelectUpTo(pageNo, glyphIx + 1);
        return;
    }

    int i = glyphIx;
    bool isAllDigits = true;
    WCHAR c = 0;
    for (; i > 0; i--) {
        c = text[i - 1];
        if (!isWordChar(c)) {
            break;
        }
        if (!isDigit(c)) {
            isAllDigits = false;
        }
    }
    int wordStart = i;
    int maybeNumberStart = i;
    int nDigits = 0;
    if (isAllDigits && (c == '.' || c == ',')) {
        // walk backward across a pattern like "1,234." or "1,234,567,"
        int j = i - 2;
        // first skip one group of digits (before the separator we stopped at)
        nDigits = 0;
        while (j >= 0 && isDigit(text[j])) {
            nDigits++;
            j--;
        }
        if (nDigits > 0) {
            maybeNumberStart = j + 1;
            // continue backward across comma-separated groups
            maybeNumberStart = ExtendBackAcrossCommaGroups(text, maybeNumberStart);
        } else {
            isAllDigits = false;
        }
    }

    for (; i < textLen; i++) {
        c = text[i];
        if (!isWordChar(c)) {
            break;
        }
        if (!isDigit(c)) {
            isAllDigits = false;
        }
    }

    // try to select numbers with commas and decimal points
    // e.g. "1,234.56" or "1,234,567" or "123.45"
    int wordEnd = i;
    if (isAllDigits) {
        // extend forward across comma groups
        wordEnd = ExtendForwardAcrossCommaGroups(text, textLen, wordEnd);
        // extend forward across decimal point + digits
        if (wordEnd < textLen && text[wordEnd] == '.') {
            int j = wordEnd + 1;
            nDigits = 0;
            while (j < textLen && isDigit(text[j])) {
                nDigits++;
                j++;
            }
            if (nDigits > 0) {
                wordEnd = j;
            }
        }
        // extend backward across comma groups
        wordStart = ExtendBackAcrossCommaGroups(text, wordStart);
        if (maybeNumberStart < wordStart) {
            wordStart = maybeNumberStart;
        }
    }

    int clickGlyph = glyphIx;
    if (wordEnd > wordStart && clickGlyph >= wordStart && clickGlyph < wordEnd && isCjkWordChar(text[clickGlyph]) &&
        !isAllDigits) {
        wordStart = clickGlyph;
        wordEnd = clickGlyph + 1;
    }
    StartAt(pageNo, wordStart);
    SelectUpTo(pageNo, wordEnd);
}

void TextSelection::SelectPageBbox(int pageNo, RectF bbox) {
    Reset();
    startPage = pageNo;
    endPage = pageNo;
    startGlyph = 0;
    endGlyph = 1;

    result.cap = 64;
    result.pages = (int*)malloc(sizeof(int) * result.cap);
    result.rects = (Rect*)malloc(sizeof(Rect) * result.cap);
    if (!result.pages || !result.rects) {
        Reset();
        return;
    }
    result.pages[0] = pageNo;
    result.rects[0] = bbox.Round();
    result.len = 1;
}

int TextSelection::GlyphIndexAt(int pageNo, double x, double y) {
    return FindClosestGlyph(this, pageNo, x, y);
}

void TextSelection::SelectGlyphRange(int pageNo, int startGlyph, int endGlyph) {
    if (startGlyph < 0 || endGlyph <= startGlyph) {
        return;
    }
    StartAt(pageNo, startGlyph);
    SelectUpTo(pageNo, endGlyph);
}

char* TextSelection::ExtractWordAt(int pageNo, double x, double y) {
    int glyphIx = GlyphIndexUnderPoint(this, pageNo, x, y);
    int textLen;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen);
    if (!text || textLen <= 0 || glyphIx < 0 || glyphIx >= textLen) {
        return nullptr;
    }
    if (IsSelectableWhitespace(text[glyphIx])) {
        return nullptr;
    }

    int i = glyphIx;
    if (!isWordChar(text[glyphIx])) {
        return ToUtf8(text + glyphIx, 1);
    }
    for (; i > 0; i--) {
        if (!isWordChar(text[i - 1])) {
            break;
        }
    }
    int wordStart = i;
    for (; i < textLen; i++) {
        if (!isWordChar(text[i])) {
            break;
        }
    }
    int wordEnd = i;
    if (wordEnd <= wordStart) {
        return nullptr;
    }
    return ToUtf8(text + wordStart, wordEnd - wordStart);
}

void TextSelection::CopySelection(TextSelection* orig) {
    Reset();
    StartAt(orig->startPage, orig->startGlyph);
    SelectUpTo(orig->endPage, orig->endGlyph);
}

static WCHAR* ExtractTextFromGlyphRange(TextSelection* ts, int fromPage, int fromGlyph, int toPage, int toGlyph,
                                        const char* lineSep) {
    StrVec lines;

    bool vertical = PageUsesVerticalGlyphLayout(ts, fromPage);
    if (vertical) {
        for (int page = fromPage; page <= toPage; page++) {
            int textLen;
            ts->engine->GetTextForPage(page, &textLen);
            int glyph = page == fromPage ? fromGlyph : 0;
            int length = (page == toPage ? toGlyph : textLen) - glyph;
            if (length > 0) {
                AppendPageGlyphsInVisualOrder(ts, page, glyph, glyph + length, lines, true);
            }
        }
        TempStr res = JoinTemp(&lines, "");
        return ToWStr(res);
    }

    for (int page = fromPage; page <= toPage; page++) {
        int textLen;
        ts->engine->GetTextForPage(page, &textLen);
        int glyph = page == fromPage ? fromGlyph : 0;
        int length = (page == toPage ? toGlyph : textLen) - glyph;
        if (length > 0) {
            FillResultRects(ts, page, glyph, length, &lines);
        }
    }

    const char* joinSep = lineSep;
    if (lines.Size() > 1) {
        bool allSingleCjk = true;
        for (int i = 0; i < lines.Size(); i++) {
            const char* s = lines.At(i);
            if (str::IsEmpty(s)) {
                allSingleCjk = false;
                break;
            }
            unsigned char c = (unsigned char)s[0];
            int len = 1;
            if ((c & 0xE0) == 0xC0) {
                len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                len = 4;
            }
            if (len < 3 || s[len] != 0) {
                allSingleCjk = false;
                break;
            }
        }
        if (allSingleCjk) {
            joinSep = "";
        }
    }

    TempStr res = JoinTemp(&lines, joinSep);
    return ToWStr(res);
}

WCHAR* TextSelection::ExtractText(const char* lineSep) {
    int fromPage, fromGlyph, toPage, toGlyph;
    GetGlyphRange(&fromPage, &fromGlyph, &toPage, &toGlyph);
    return ExtractTextFromGlyphRange(this, fromPage, fromGlyph, toPage, toGlyph, lineSep);
}

void TextSelection::GetGlyphRange(int* fromPage, int* fromGlyph, int* toPage, int* toGlyph) const {
    ComputeGlyphRangeFromEndpoints(this, fromPage, toPage, fromGlyph, toGlyph);
}
