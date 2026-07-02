/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/Dpi.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "GlobalPrefs.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "TextSelection.h"

#include "utils/Log.h"
#include "TextToSpeech.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Selection.h"
#include "SumatraPDF.h"
#include "ReadAloudHighlight.h"

struct ReadAloudRawByte {
    char c = 0;
    ReadAloudByteLoc loc{};
};

static bool IsReadAloudLowerAscii(char c) {
    return c >= 'a' && c <= 'z';
}

static bool IsReadAloudLineBreak(char c) {
    return c == '\r' || c == '\n';
}

static bool IsReadAloudHorizontalSpace(char c) {
    return c == ' ' || c == '\t';
}

static bool ReadAloudIsCjkForLineJoin(char32_t cp) {
    return (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

static bool ReadAloudDecodeUtf8One(const char*& s, char32_t* cpOut) {
    if (!s || !*s || !cpOut) {
        return false;
    }
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *cpOut = c0;
        s += 1;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && s[1]) {
        *cpOut = ((c0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
        s += 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cpOut = ((c0 & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
        s += 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *cpOut = ((c0 & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) | (((unsigned char)s[2] & 0x3F) << 6) |
                 ((unsigned char)s[3] & 0x3F);
        s += 4;
        return true;
    }
    *cpOut = c0;
    s += 1;
    return true;
}

static char32_t ReadAloudLastCodepointUtf8(const char* s, int len) {
    if (!s || len <= 0) {
        return 0;
    }
    const char* p = s;
    const char* end = s + len;
    const char* lastStart = p;
    char32_t cp = 0;
    while (p < end) {
        lastStart = p;
        ReadAloudDecodeUtf8One(p, &cp);
    }
    if (lastStart < end) {
        p = lastStart;
        ReadAloudDecodeUtf8One(p, &cp);
    }
    return cp;
}

static char32_t ReadAloudPeekRawCp(const Vec<ReadAloudRawByte>& raw, size_t i) {
    if (i >= raw.size()) {
        return 0;
    }
    unsigned char c0 = (unsigned char)raw[i].c;
    int nbytes = 1;
    if (c0 < 0x80) {
        nbytes = 1;
    } else if ((c0 & 0xE0) == 0xC0) {
        nbytes = 2;
    } else if ((c0 & 0xF0) == 0xE0) {
        nbytes = 3;
    } else if ((c0 & 0xF8) == 0xF0) {
        nbytes = 4;
    }
    char buf[5] = {0};
    for (int b = 0; b < nbytes && i + (size_t)b < raw.size(); b++) {
        char c = raw[i + (size_t)b].c;
        if (IsReadAloudLineBreak(c) || IsReadAloudHorizontalSpace(c)) {
            break;
        }
        buf[b] = c;
    }
    const char* p = buf;
    char32_t cp = 0;
    ReadAloudDecodeUtf8One(p, &cp);
    return cp;
}

static bool ReadAloudShouldJoinCpAtLineBreak(char32_t prevCp, char32_t nextCp) {
    if (prevCp == 0 || nextCp == 0) {
        return false;
    }
    if (ReadAloudIsCjkForLineJoin(prevCp) || ReadAloudIsCjkForLineJoin(nextCp)) {
        return true;
    }
    if (prevCp >= 'a' && prevCp <= 'z' && nextCp >= 'a' && nextCp <= 'z') {
        return true;
    }
    return false;
}

bool ReadAloudShouldJoinAtWrappedLine(const char* beforeEnd, int beforeLen, const char* afterStart) {
    char32_t prevCp = ReadAloudLastCodepointUtf8(beforeEnd, beforeLen);
    if (!afterStart || !*afterStart) {
        return false;
    }
    const char* p = afterStart;
    char32_t nextCp = 0;
    ReadAloudDecodeUtf8One(p, &nextCp);
    return ReadAloudShouldJoinCpAtLineBreak(prevCp, nextCp);
}

static bool ReadAloudHighlightGrow(ReadAloudHighlightMap* map) {
    if (map->len + 1 < map->cap) {
        return true;
    }
    int newCap = map->cap == 0 ? 256 : map->cap * 2;
    ReadAloudByteLoc* newLocs = (ReadAloudByteLoc*)realloc(map->locs, sizeof(ReadAloudByteLoc) * (size_t)newCap);
    if (!newLocs) {
        return false;
    }
    map->locs = newLocs;
    map->cap = newCap;
    return true;
}

static bool ReadAloudHighlightAppend(ReadAloudHighlightMap* map, const ReadAloudByteLoc& loc) {
    if (!ReadAloudHighlightGrow(map)) {
        return false;
    }
    map->locs[map->len] = loc;
    map->len++;
    return true;
}

static bool ReadAloudHighlightAppendRaw(Vec<ReadAloudRawByte>& raw, char c, const ReadAloudByteLoc& loc) {
    ReadAloudRawByte rb;
    rb.c = c;
    rb.loc = loc;
    raw.Append(rb);
    return true;
}

static void ReadAloudByteLocSetFromRect(ReadAloudByteLoc& loc, int pageNo, const Rect& r) {
    loc.pageNo = pageNo;
    loc.x = r.x;
    loc.y = r.y;
    loc.dx = r.dx;
    loc.dy = r.dy;
}

static bool ReadAloudByteLocHasRect(const ReadAloudByteLoc& loc) {
    return loc.pageNo > 0 && (loc.x || loc.dx);
}

static Rect ReadAloudByteLocToRect(const ReadAloudByteLoc& loc) {
    return Rect(loc.x, loc.y, loc.dx, loc.dy);
}

static bool IsLineBreakGlyph(const WCHAR* text, const Rect* coords, int idx, int textLen) {
    return idx >= 0 && idx < textLen && text[idx] == '\n' && !coords[idx].x && !coords[idx].dx;
}

static void ReadAloudAppendPageUtf8(Vec<ReadAloudRawByte>& raw, EngineBase* engine, int pageNo) {
    PageTextUtf8 pageText = engine->ExtractPageTextUtf8(pageNo);
    if (!pageText.text || pageText.len <= 0) {
        FreePageTextUtf8(&pageText);
        return;
    }

    for (int i = 0; i < pageText.len; i++) {
        ReadAloudByteLoc loc;
        Rect r = pageText.coords[i];
        if (r.x || r.dx) {
            ReadAloudByteLocSetFromRect(loc, pageNo, r);
        }
        ReadAloudHighlightAppendRaw(raw, pageText.text[i], loc);
    }
    FreePageTextUtf8(&pageText);
}

static bool ReadAloudAppendRawToHighlight(Vec<ReadAloudRawByte>& raw, ReadAloudHighlightMap* map,
                                          StrBuilder& cleanedOut, bool append) {
    if (!map) {
        return false;
    }

    if (!append) {
        cleanedOut.Reset();
        map->len = 0;
    }

    bool lastWasSpace = false;
    if (append && cleanedOut.len > 0) {
        lastWasSpace = IsReadAloudHorizontalSpace(cleanedOut.Last());
    }

    for (size_t i = 0; i < raw.size();) {
        char c = raw[i].c;
        ReadAloudByteLoc loc = raw[i].loc;

        if (c == '-' && i + 1 < raw.size() && IsReadAloudLineBreak(raw[i + 1].c)) {
            size_t after = i + 1;
            while (after < raw.size() && IsReadAloudLineBreak(raw[after].c)) {
                after++;
            }
            while (after < raw.size() && IsReadAloudHorizontalSpace(raw[after].c)) {
                after++;
            }

            bool prevIsLower = i > 0 && IsReadAloudLowerAscii(raw[i - 1].c);
            bool nextIsLower = after < raw.size() && IsReadAloudLowerAscii(raw[after].c);
            if (prevIsLower && nextIsLower) {
                i = after;
                lastWasSpace = false;
                continue;
            }
        }

        if (IsReadAloudLineBreak(c)) {
            int lineBreaks = 0;
            while (i < raw.size() && IsReadAloudLineBreak(raw[i].c)) {
                if (raw[i].c == '\n') {
                    lineBreaks++;
                }
                i++;
            }
            while (i < raw.size() && IsReadAloudHorizontalSpace(raw[i].c)) {
                i++;
            }

            bool joinLine = false;
            if (lineBreaks == 1 && map->len > 0) {
                char32_t prevCp = ReadAloudLastCodepointUtf8(cleanedOut.Get(), cleanedOut.len);
                char32_t nextCp = ReadAloudPeekRawCp(raw, i);
                joinLine = ReadAloudShouldJoinCpAtLineBreak(prevCp, nextCp);
            }

            if (!joinLine && !lastWasSpace && map->len > 0) {
                ReadAloudByteLoc spaceLoc;
                if (!ReadAloudHighlightAppend(map, spaceLoc) || !cleanedOut.AppendChar(' ')) {
                    return false;
                }
                lastWasSpace = true;
            } else if (joinLine) {
                lastWasSpace = false;
            }
            if (lineBreaks >= 2) {
                ReadAloudByteLoc spaceLoc;
                if (!ReadAloudHighlightAppend(map, spaceLoc) || !cleanedOut.AppendChar(' ')) {
                    return false;
                }
            }
            continue;
        }

        if (IsReadAloudHorizontalSpace(c)) {
            if (!lastWasSpace && map->len > 0) {
                ReadAloudByteLoc spaceLoc;
                if (!ReadAloudHighlightAppend(map, spaceLoc) || !cleanedOut.AppendChar(' ')) {
                    return false;
                }
                lastWasSpace = true;
            }
            i++;
            continue;
        }

        if (!ReadAloudHighlightAppend(map, loc) || !cleanedOut.AppendChar(c)) {
            return false;
        }
        lastWasSpace = false;
        i++;
    }

    return true;
}

static bool CleanRawBytes(Vec<ReadAloudRawByte>& raw, ReadAloudHighlightMap* map, StrBuilder& cleanedOut) {
    return ReadAloudAppendRawToHighlight(raw, map, cleanedOut, false);
}

static void ReadAloudAppendPageGlyphs(Vec<ReadAloudRawByte>& raw, EngineBase* engine, int pageNo, int startGlyph,
                                      int endGlyph);

static bool ReadAloudCollectDocumentRaw(Vec<ReadAloudRawByte>& raw, EngineBase* engine, int startPage, int startGlyph,
                                          int endPage) {
    for (int page = startPage; page <= endPage; page++) {
        if (page == startPage && startGlyph > 0) {
            ReadAloudAppendPageGlyphs(raw, engine, page, startGlyph, -1);
        } else {
            ReadAloudAppendPageUtf8(raw, engine, page);
        }
    }
    return raw.size() > 0;
}

void ReadAloudHighlightFree(ReadAloudHighlightMap* map) {
    if (!map) {
        return;
    }
    free(map->locs);
    map->locs = nullptr;
    map->len = 0;
    map->cap = 0;
}

bool ReadAloudHighlightBuildFromPage(EngineBase* engine, int pageNo, ReadAloudHighlightMap* map,
                                     StrBuilder& cleanedOut) {
    if (!engine || !map) {
        return false;
    }

    PageTextUtf8 pageText = engine->ExtractPageTextUtf8(pageNo);
    if (!pageText.text || pageText.len <= 0) {
        FreePageTextUtf8(&pageText);
        return false;
    }

    Vec<ReadAloudRawByte> raw;
    for (int i = 0; i < pageText.len; i++) {
        ReadAloudByteLoc loc;
        Rect r = pageText.coords[i];
        if (r.x || r.dx) {
            ReadAloudByteLocSetFromRect(loc, pageNo, r);
        }
        ReadAloudHighlightAppendRaw(raw, pageText.text[i], loc);
    }
    FreePageTextUtf8(&pageText);

    return CleanRawBytes(raw, map, cleanedOut);
}

static void ReadAloudAppendPageGlyphs(Vec<ReadAloudRawByte>& raw, EngineBase* engine, int pageNo, int startGlyph,
                                      int endGlyph) {
    int textLen = 0;
    Rect* coords = nullptr;
    const WCHAR* text = engine->GetTextForPage(pageNo, &textLen, &coords);
    if (!text || textLen <= 0) {
        logf("ReadAloud: AppendPageGlyphs: page %d has no text (textLen=%d)\n", pageNo, textLen);
        return;
    }

    if (startGlyph < 0) {
        startGlyph = 0;
    }
    if (endGlyph < 0 || endGlyph > textLen) {
        endGlyph = textLen;
    }

    ReadAloudByteLoc noLoc;
    for (int g = startGlyph; g < endGlyph; g++) {
        if (IsLineBreakGlyph(text, coords, g, textLen)) {
            ReadAloudHighlightAppendRaw(raw, '\r', noLoc);
            ReadAloudHighlightAppendRaw(raw, '\n', noLoc);
            continue;
        }

        ReadAloudByteLoc loc;
        Rect r = coords[g];
        if (r.x || r.dx) {
            ReadAloudByteLocSetFromRect(loc, pageNo, r);
        }

        WCHAR wc[2] = {text[g], 0};
        TempStr utf8 = ToUtf8Temp(wc);
        if (str::IsEmpty(utf8)) {
            continue;
        }
        for (const char* p = utf8; *p; p++) {
            ReadAloudHighlightAppendRaw(raw, *p, loc);
        }
    }
}

bool ReadAloudHighlightBuildFromTextSelection(TextSelection* ts, ReadAloudHighlightMap* map, StrBuilder& cleanedOut) {
    if (!ts || !ts->engine || !map) {
        return false;
    }

    int fromPage = 0, fromGlyph = 0, toPage = 0, toGlyph = 0;
    ts->GetGlyphRange(&fromPage, &fromGlyph, &toPage, &toGlyph);

    Vec<ReadAloudRawByte> raw;
    for (int page = fromPage; page <= toPage; page++) {
        int glyph = page == fromPage ? fromGlyph : 0;
        int endGlyph = page == toPage ? toGlyph : -1;
        ReadAloudAppendPageGlyphs(raw, ts->engine, page, glyph, endGlyph);
    }

    return CleanRawBytes(raw, map, cleanedOut);
}

bool ReadAloudGetViewportStart(DisplayModel* dm, int* startPageOut, int* startGlyphOut) {
    if (!dm || !startPageOut || !startGlyphOut) {
        logf("ReadAloud: GetViewportStart: null args (dm=%p)\n", dm);
        return false;
    }

    *startPageOut = 0;
    *startGlyphOut = 0;

    int pageCount = dm->PageCount();
    Rect viewArea = dm->GetViewPort();
    viewArea.x = 0;
    viewArea.y = 0;
    logf("ReadAloud: GetViewportStart: viewArea=(%d,%d %dx%d) pageCount=%d\n", viewArea.x, viewArea.y, viewArea.dx,
         viewArea.dy, pageCount);

    int firstVisiblePage = 0;
    for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
        PageInfo* pageInfo = dm->GetPageInfo(pageNo);
        if (pageInfo && pageInfo->visibleRatio > 0.0) {
            firstVisiblePage = pageNo;
            break;
        }
    }

    if (firstVisiblePage == 0) {
        logf("ReadAloud: GetViewportStart: no visible pages (pageCount=%d)\n", pageCount);
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    int scanEnd = firstVisiblePage + 8;
    if (scanEnd > pageCount) {
        scanEnd = pageCount;
    }

    for (int pageNo = firstVisiblePage; pageNo <= scanEnd; pageNo++) {
        PageInfo* pageInfo = dm->GetPageInfo(pageNo);
        if (!pageInfo || pageInfo->visibleRatio <= 0.0) {
            continue;
        }

        int textLen = 0;
        Rect* coords = nullptr;
        const WCHAR* text = engine->GetTextForPage(pageNo, &textLen, &coords);
        if (!text || textLen <= 0) {
            continue;
        }

        int g = 0;
        while (g < textLen) {
            while (g < textLen && IsLineBreakGlyph(text, coords, g, textLen)) {
                g++;
            }
            if (g >= textLen) {
                break;
            }

            int lineStart = g;
            while (g < textLen && !IsLineBreakGlyph(text, coords, g, textLen)) {
                g++;
            }

            Rect lineBbox;
            for (int i = lineStart; i < g; i++) {
                Rect r = coords[i];
                if (r.x || r.dx) {
                    lineBbox = lineBbox.IsEmpty() ? r : lineBbox.Union(r);
                }
            }
            if (lineBbox.IsEmpty()) {
                continue;
            }

            Rect screenLine = dm->CvtToScreen(pageNo, ToRectF(lineBbox));
            if (!screenLine.Intersect(viewArea).IsEmpty()) {
                logf("ReadAloud: GetViewportStart: found visible line at page %d glyph %d (screenLine=%d,%d %dx%d)\n",
                     pageNo, lineStart, screenLine.x, screenLine.y, screenLine.dx, screenLine.dy);
                *startPageOut = pageNo;
                *startGlyphOut = lineStart;
                return true;
            }
        }
    }

    logf("ReadAloud: GetViewportStart: no visible line in viewport, falling back to page %d glyph 0\n",
         firstVisiblePage);
    *startPageOut = firstVisiblePage;
    *startGlyphOut = 0;
    return true;
}

static bool ReadAloudGetGlyphAtCursor(DisplayModel* dm, Point screenPt, int* pageOut, int* glyphOut) {
    if (!dm || !pageOut || !glyphOut || !dm->textSelection) {
        return false;
    }
    if (!dm->IsOverText(screenPt)) {
        return false;
    }

    int pageNo = dm->GetPageNoByPoint(screenPt);
    if (!dm->ValidPageNo(pageNo)) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    PointF pt = dm->CvtFromScreen(screenPt, pageNo);
    dm->textSelection->StartAt(pageNo, pt.x, pt.y);

    int textLen = 0;
    Rect* coords = nullptr;
    engine->GetTextForPage(pageNo, &textLen, &coords);
    if (textLen <= 0) {
        return false;
    }

    // Same adjustment as TextSelection::IsOverGlyph: FindClosestGlyph can return
    // the index after the glyph under the cursor when clicking its right half.
    int glyph = dm->textSelection->startGlyph;
    Point pti = ToPoint(pt);
    if (glyph == textLen || (glyph >= 0 && glyph < textLen && !coords[glyph].Contains(pti))) {
        glyph--;
    }
    if (glyph < 0 || glyph >= textLen) {
        return false;
    }

    *pageOut = pageNo;
    *glyphOut = glyph;
    return true;
}

bool ReadAloudCanReadFromCursor(DisplayModel* dm, Point screenPt) {
    int pageNo = 0;
    int glyph = 0;
    return ReadAloudGetGlyphAtCursor(dm, screenPt, &pageNo, &glyph);
}

bool ReadAloudGetCursorStart(DisplayModel* dm, Point screenPt, int* startPageOut, int* startGlyphOut) {
    if (!startPageOut || !startGlyphOut) {
        logf("ReadAloud: GetCursorStart: null args\n");
        return false;
    }

    *startPageOut = 0;
    *startGlyphOut = 0;

    int pageNo = 0;
    int glyph = 0;
    if (!ReadAloudGetGlyphAtCursor(dm, screenPt, &pageNo, &glyph)) {
        logf("ReadAloud: GetCursorStart: no text at cursor (%d,%d)\n", screenPt.x, screenPt.y);
        return false;
    }

    logf("ReadAloud: GetCursorStart: page %d glyph %d\n", pageNo, glyph);
    *startPageOut = pageNo;
    *startGlyphOut = glyph;
    return true;
}

bool ReadAloudHighlightBuildFromDocument(DisplayModel* dm, int startPage, int startGlyph, int endPageInclusive,
                                         ReadAloudHighlightMap* map, StrBuilder& cleanedOut) {
    if (!dm || !map || !dm->ValidPageNo(startPage)) {
        logf("ReadAloud: BuildFromDocument: invalid args (dm=%p map=%p startPage=%d)\n", dm, map, startPage);
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        logf("ReadAloud: BuildFromDocument: no engine\n");
        return false;
    }

    int pageCount = dm->PageCount();
    int endPage = endPageInclusive > 0 ? endPageInclusive : pageCount;
    if (endPage > pageCount) {
        endPage = pageCount;
    }
    if (endPage < startPage) {
        logf("ReadAloud: BuildFromDocument: endPage %d < startPage %d\n", endPage, startPage);
        return false;
    }

    Vec<ReadAloudRawByte> raw;
    logf("ReadAloud: BuildFromDocument: startPage=%d startGlyph=%d endPage=%d pageCount=%d\n", startPage, startGlyph,
         endPage, pageCount);
    if (!ReadAloudCollectDocumentRaw(raw, engine, startPage, startGlyph, endPage)) {
        logf("ReadAloud: BuildFromDocument: no raw bytes extracted\n");
        return false;
    }

    if (!CleanRawBytes(raw, map, cleanedOut)) {
        logf("ReadAloud: BuildFromDocument: CleanRawBytes failed (raw.size=%zu)\n", raw.size());
        return false;
    }

    logf("ReadAloud: BuildFromDocument: ok raw=%zu cleanedLen=%d mapLen=%d\n", raw.size(), (int)cleanedOut.len,
         map->len);
    return true;
}

bool ReadAloudHighlightAppendDocumentPages(DisplayModel* dm, int startPage, int endPageInclusive,
                                           ReadAloudHighlightMap* map, char** textInOut) {
    if (!dm || !map || !textInOut || !*textInOut || !dm->ValidPageNo(startPage) || startPage > endPageInclusive) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    Vec<ReadAloudRawByte> raw;
    if (!ReadAloudCollectDocumentRaw(raw, engine, startPage, 0, endPageInclusive)) {
        return false;
    }

    ReadAloudHighlightMap appendMap{};
    StrBuilder cleanedAppend;
    if (!ReadAloudAppendRawToHighlight(raw, &appendMap, cleanedAppend, false)) {
        ReadAloudHighlightFree(&appendMap);
        return false;
    }

    for (int i = 0; i < appendMap.len; i++) {
        if (!ReadAloudHighlightAppend(map, appendMap.locs[i])) {
            ReadAloudHighlightFree(&appendMap);
            return false;
        }
    }
    ReadAloudHighlightFree(&appendMap);

    if (cleanedAppend.len <= 0) {
        return false;
    }

    char* combined = str::Join(*textInOut, cleanedAppend.Get());
    str::ReplaceWithCopy(textInOut, combined);
    str::Free(combined);

    logf("ReadAloud: AppendDocumentPages: pages %d..%d appendedLen=%d totalLen=%d mapLen=%d\n", startPage,
         endPageInclusive, (int)cleanedAppend.len, str::Leni(*textInOut), map->len);
    return true;
}

void ReadAloudHighlightTimerStart(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    SetTimer(win->hwndCanvas, READ_ALOUD_HIGHLIGHT_TIMER_ID, READ_ALOUD_HIGHLIGHT_DELAY_IN_MS, nullptr);
}

void ReadAloudHighlightTimerStop(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    KillTimer(win->hwndCanvas, READ_ALOUD_HIGHLIGHT_TIMER_ID);
}

static int gReadAloudPaintLogState = 0;

static void ReadAloudPaintLogOnce(int code, const char* fmt, ...) {
    if (gReadAloudPaintLogState == code) {
        return;
    }
    gReadAloudPaintLogState = code;
    logf(fmt);
}

static int ReadAloudUtf8Next(const char* text, int pos) {
    if (!text || text[pos] == 0) {
        return pos;
    }
    unsigned char c = (unsigned char)text[pos];
    int n = 1;
    if ((c & 0x80) == 0) {
        n = 1;
    } else if ((c & 0xE0) == 0xC0) {
        n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
    }
    if (text[pos + n - 1] == 0) {
        return pos + 1;
    }
    return pos + n;
}

static bool ReadAloudIsCjkIdeograph(char32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

static bool ReadAloudTokenHasCjk(const char* text, int start, int end) {
    const char* p = text + start;
    const char* limit = text + end;
    while (p < limit) {
        char32_t cp = 0;
        if (!ReadAloudDecodeUtf8One(p, &cp)) {
            break;
        }
        if (ReadAloudIsCjkIdeograph(cp)) {
            return true;
        }
    }
    return false;
}

static int ReadAloudWordEndUtf8(const char* text, int pos) {
    if (!text || pos < 0) {
        return pos;
    }
    int len = str::Leni(text);
    if (pos >= len) {
        return len;
    }
    while (pos < len && (IsReadAloudHorizontalSpace(text[pos]) || IsReadAloudLineBreak(text[pos]))) {
        pos++;
    }
    int start = pos;
    int end = pos;
    while (end < len && !IsReadAloudHorizontalSpace(text[end]) && !IsReadAloudLineBreak(text[end])) {
        end++;
    }
    if (end <= start) {
        return end;
    }

    if (ReadAloudTokenHasCjk(text, start, end)) {
        int cappedEnd = start;
        for (int chars = 0; chars < 4 && cappedEnd < end; chars++) {
            cappedEnd = ReadAloudUtf8Next(text, cappedEnd);
        }
        return cappedEnd;
    }
    return end;
}

static bool ReadAloudSameTextLine(const RectF& a, const RectF& b) {
    if (a.IsEmpty() || b.IsEmpty()) {
        return false;
    }
    float lineH = std::min(a.dy, b.dy);
    if (lineH <= 0) {
        lineH = 1;
    }
    float centerA = a.y + a.dy / 2;
    float centerB = b.y + b.dy / 2;
    return std::abs(centerA - centerB) < lineH * 0.45f;
}

static void ReadAloudAppendLineRect(Vec<RectF>& lineRects, RectF rf) {
    for (size_t i = 0; i < lineRects.size(); i++) {
        if (ReadAloudSameTextLine(lineRects[i], rf)) {
            lineRects[i] = MergeHighlightLineRect(lineRects[i], rf);
            return;
        }
    }
    lineRects.Append(rf);
}

static bool ReadAloudCollectWordHighlightScreenRects(MainWindow* win, WindowTab* tab, DisplayModel* dm,
                                                     int wordStartAbs, int wordEndAbs, Vec<Rect>& screenRects) {
    if (!win || !tab || !dm || !tab->readAloudHighlight) {
        return false;
    }

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    int pageCount = dm->GetEngine()->PageCount();

    for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
        Vec<RectF> lineRects;
        for (int i = wordStartAbs; i < wordEndAbs; i++) {
            ReadAloudByteLoc& loc = map->locs[i];
            if (loc.pageNo != pageNo || !ReadAloudByteLocHasRect(loc)) {
                continue;
            }
            PageInfo* pi = dm->GetPageInfo(loc.pageNo);
            if (!pi || pi->visibleRatio <= 0.0) {
                continue;
            }
            RectF rf = ToRectF(ReadAloudByteLocToRect(loc));
            ReadAloudAppendLineRect(lineRects, rf);
        }

        for (RectF& u : lineRects) {
            if (u.IsEmpty()) {
                continue;
            }
            u = ScaleHighlightBandRect(u, kReadAloudHighlightBandRatio);
            Rect sr = dm->CvtToScreen(pageNo, u);
            sr = sr.Intersect(win->canvasRc);
            if (!sr.IsEmpty()) {
                screenRects.Append(sr);
            }
        }
    }

    return screenRects.size() > 0;
}

bool ReadAloudGetProgressPage(WindowTab* tab, int* pageOut, int* pageCountOut) {
    if (!tab || !pageOut || !pageCountOut) {
        return false;
    }

    *pageOut = 0;
    *pageCountOut = 0;

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return false;
    }
    *pageCountOut = dm->PageCount();

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    if (!map || !map->locs || map->len <= 0) {
        return false;
    }

    int absPos = -1;
    WindowTab* sourceTab = GetReadAloudSourceTab();
    if (sourceTab == tab && TtsIsSpeaking()) {
        int spokenPos = TtsGetSpokenPosUtf8();
        if (spokenPos >= 0) {
            absPos = tab->readAloudHighlightBase + tab->readAloudChunkStart + spokenPos;
        }
    } else if (tab->readAloudResumePos > 0) {
        absPos = tab->readAloudResumePos;
    } else if (tab->readAloudChunkEnd > 0) {
        absPos = tab->readAloudHighlightBase + tab->readAloudChunkStart;
    }

    if (absPos < 0 || absPos >= map->len) {
        return false;
    }

    int pageNo = map->locs[absPos].pageNo;
    if (pageNo <= 0) {
        return false;
    }

    *pageOut = pageNo;
    return true;
}

static bool ReadAloudGetCurrentWordAbsRange(WindowTab* tab, int* startAbsOut, int* endAbsOut) {
    if (!tab || !startAbsOut || !endAbsOut) {
        return false;
    }

    *startAbsOut = 0;
    *endAbsOut = 0;

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    if (!map || !map->locs || map->len <= 0 || str::IsEmpty(tab->readAloudText)) {
        return false;
    }

    int spokenPos = TtsGetSpokenPosUtf8();
    if (spokenPos < 0) {
        return false;
    }

    const char* chunkText = tab->readAloudText + tab->readAloudChunkStart;
    int wordStartAbs = tab->readAloudHighlightBase + tab->readAloudChunkStart + spokenPos;
    int wordEndInChunk = TtsGetSpokenWordEndUtf8();
    if (wordEndInChunk <= spokenPos) {
        wordEndInChunk = ReadAloudWordEndUtf8(chunkText, spokenPos);
    }
    int wordEndAbs = tab->readAloudHighlightBase + tab->readAloudChunkStart + wordEndInChunk;
    if (wordStartAbs < 0 || wordStartAbs >= map->len) {
        return false;
    }
    if (wordEndAbs > map->len) {
        wordEndAbs = map->len;
    }
    if (wordEndAbs <= wordStartAbs) {
        return false;
    }

    *startAbsOut = wordStartAbs;
    *endAbsOut = wordEndAbs;
    return true;
}

static bool ReadAloudGetCurrentWordScreenRect(MainWindow* win, Rect* rectOut) {
    if (!rectOut || !win) {
        return false;
    }

    *rectOut = Rect();

    WindowTab* tab = GetReadAloudSourceTab();
    if (!tab || tab->win != win) {
        return false;
    }

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return false;
    }

    int wordStartAbs = 0;
    int wordEndAbs = 0;
    if (!ReadAloudGetCurrentWordAbsRange(tab, &wordStartAbs, &wordEndAbs)) {
        return false;
    }

    Vec<Rect> screenRects;
    if (!ReadAloudCollectWordHighlightScreenRects(win, tab, dm, wordStartAbs, wordEndAbs, screenRects)) {
        return false;
    }

    Rect unionRect = screenRects[0];
    for (size_t i = 1; i < screenRects.size(); i++) {
        unionRect = unionRect.Union(screenRects[i]);
    }
    *rectOut = unionRect;
    return true;
}

static bool ReadAloudIsWordRectVisibleInViewport(MainWindow* win, const Rect& wordRect) {
    if (!win) {
        return false;
    }
    return !wordRect.Intersect(win->canvasRc).IsEmpty();
}

static bool ReadAloudIsWordRectFullyVisibleInViewport(MainWindow* win, const Rect& wordRect, int margin) {
    if (!win) {
        return false;
    }
    Rect canvas = win->canvasRc;
    if (wordRect.x < margin || wordRect.y < margin) {
        return false;
    }
    if (wordRect.x + wordRect.dx > canvas.dx - margin) {
        return false;
    }
    if (wordRect.y + wordRect.dy > canvas.dy - margin) {
        return false;
    }
    return true;
}

void ReadAloudOnUserViewChanged(MainWindow* win) {
    if (!win || win->readAloudScrollFromCode || !TtsIsSpeaking()) {
        return;
    }

    WindowTab* tab = GetReadAloudSourceTab();
    if (!tab || tab->win != win || !tab->readAloudAutoScroll) {
        return;
    }

    Rect wordRect;
    if (!ReadAloudGetCurrentWordScreenRect(win, &wordRect) || !ReadAloudIsWordRectVisibleInViewport(win, wordRect)) {
        tab->readAloudAutoScroll = false;
        logf("ReadAloud: auto-scroll disabled (user scrolled away from highlight)\n");
    }
}

void ReadAloudUpdateAutoScroll(MainWindow* win) {
    if (!win || !TtsIsSpeaking()) {
        return;
    }

    WindowTab* tab = GetReadAloudSourceTab();
    if (!tab || tab->win != win || !tab->readAloudAutoScroll) {
        return;
    }

    Rect wordRect;
    if (!ReadAloudGetCurrentWordScreenRect(win, &wordRect)) {
        return;
    }

    int margin = DpiScale(win->hwndCanvas, 48);
    if (ReadAloudIsWordRectFullyVisibleInViewport(win, wordRect, margin)) {
        return;
    }

    Rect canvas = win->canvasRc;

    int dx = 0;
    int dy = 0;
    if (wordRect.y < margin) {
        dy = wordRect.y - margin;
    } else if (wordRect.y + wordRect.dy > canvas.dy - margin) {
        dy = wordRect.y + wordRect.dy - (canvas.dy - margin);
    }
    if (wordRect.x < margin) {
        dx = wordRect.x - margin;
    } else if (wordRect.x + wordRect.dx > canvas.dx - margin) {
        dx = wordRect.x + wordRect.dx - (canvas.dx - margin);
    }

    if (dx == 0 && dy == 0) {
        return;
    }

    int maxStep = std::max(canvas.dy / 4, DpiScale(win->hwndCanvas, 120));
    if (dx > maxStep) {
        dx = maxStep;
    } else if (dx < -maxStep) {
        dx = -maxStep;
    }
    if (dy > maxStep) {
        dy = maxStep;
    } else if (dy < -maxStep) {
        dy = -maxStep;
    }

    win->readAloudScrollFromCode = true;
    win->MoveDocBy(dx, dy);
    win->readAloudScrollFromCode = false;
}

void PaintReadAloudHighlight(MainWindow* win, HDC hdc) {
    if (!TtsIsSpeaking()) {
        gReadAloudPaintLogState = 0;
        return;
    }
    if (!win) {
        return;
    }

    WindowTab* tab = GetReadAloudSourceTab();
    if (!tab || tab->win != win) {
        ReadAloudPaintLogOnce(1, "ReadAloud: PaintHighlight: no matching source tab");
        return;
    }

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    if (!map || !map->locs || map->len <= 0) {
        ReadAloudPaintLogOnce(2, "ReadAloud: PaintHighlight: no highlight map");
        return;
    }

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        ReadAloudPaintLogOnce(3, "ReadAloud: PaintHighlight: tab is not a fixed-layout document");
        return;
    }

    int wordStartAbs = 0;
    int wordEndAbs = 0;
    if (!ReadAloudGetCurrentWordAbsRange(tab, &wordStartAbs, &wordEndAbs)) {
        if (gReadAloudPaintLogState != 4) {
            gReadAloudPaintLogState = 4;
            logf("ReadAloud: PaintHighlight: no spoken position (textLen=%d)\n", str::Leni(tab->readAloudText));
        }
        return;
    }

    if (wordStartAbs < 0 || wordStartAbs >= map->len) {
        ReadAloudPaintLogOnce(5, "ReadAloud: PaintHighlight: wordStartAbs out of range");
        return;
    }
    if (wordEndAbs > map->len) {
        wordEndAbs = map->len;
    }
    if (wordEndAbs <= wordStartAbs) {
        ReadAloudPaintLogOnce(6, "ReadAloud: PaintHighlight: empty word range");
        return;
    }

    Vec<Rect> screenRects;
    if (!ReadAloudCollectWordHighlightScreenRects(win, tab, dm, wordStartAbs, wordEndAbs, screenRects)) {
        ReadAloudPaintLogOnce(7, "ReadAloud: PaintHighlight: no screen rects for current word");
        return;
    }

    PaintTransparentRectangles(hdc, win->canvasRc, screenRects, GetSelectionHighlightColor(), kSelectionHighlightAlpha,
                               0);
}