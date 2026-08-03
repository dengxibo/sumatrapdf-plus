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
#include "EngineAll.h"
#include "DisplayModel.h"
#include "DisplayMode.h"
#include "TextSelection.h"

#include "utils/Log.h"
#include "TextToSpeech.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Canvas.h"
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

bool ReadAloudDecodeUtf8One(const char*& s, char32_t* cpOut) {
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

static void ReadAloudByteLocSetChapter(ReadAloudByteLoc& loc, EngineBase* engine, int pageNo, int byteOff) {
    loc.chapter = -1;
    loc.byteOff = byteOff;
    if (engine && engine->kind == kindEngineMupdf && str::EqI(engine->defaultExt, ".epub")) {
        int ch = 0;
        int startPage = 0;
        if (EngineMupdfGetReflowPageChapter(engine, pageNo, &ch, &startPage)) {
            loc.chapter = ch;
        }
    }
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
        ReadAloudByteLocSetChapter(loc, engine, pageNo, i);
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
        // Keep SAPI/WinRT event queues drained while extracting many pages so that
        // a mid-read restart can purge and start speaking again without hanging.
        TtsProcessEvents();
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
        return false;
    }

    *startPageOut = 0;
    *startGlyphOut = 0;

    int pageCount = dm->PageCount();
    Rect viewArea = dm->GetViewPort();
    viewArea.x = 0;
    viewArea.y = 0;

    int firstVisiblePage = 0;
    for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
        PageInfo* pageInfo = dm->GetPageInfo(pageNo);
        if (pageInfo && pageInfo->visibleRatio > 0.0) {
            firstVisiblePage = pageNo;
            break;
        }
    }

    if (firstVisiblePage == 0) {
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
                *startPageOut = pageNo;
                *startGlyphOut = lineStart;
                return true;
            }
        }
    }

    *startPageOut = firstVisiblePage;
    *startGlyphOut = 0;
    return true;
}

// find the first text line at or below screenPt in reading order, so that
// read-aloud can start from a click on an empty spot of the page
bool ReadAloudGetStartBelowPoint(DisplayModel* dm, Point screenPt, int* startPageOut, int* startGlyphOut) {
    if (!dm || !startPageOut || !startGlyphOut) {
        return false;
    }

    *startPageOut = 0;
    *startGlyphOut = 0;

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    int pageCount = dm->PageCount();
    int firstPage = dm->GetPageNoByPoint(screenPt);
    if (!dm->ValidPageNo(firstPage)) {
        // clicked between/outside pages: fall back to the first visible page
        firstPage = 0;
        for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
            PageInfo* pageInfo = dm->GetPageInfo(pageNo);
            if (pageInfo && pageInfo->visibleRatio > 0.0) {
                firstPage = pageNo;
                break;
            }
        }
        if (firstPage == 0) {
            return false;
        }
    }

    int scanEnd = firstPage + 8;
    if (scanEnd > pageCount) {
        scanEnd = pageCount;
    }

    for (int pageNo = firstPage; pageNo <= scanEnd; pageNo++) {
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
            // on the clicked page take the first line whose bottom edge is at or
            // below the click; on later pages take the first line
            if (pageNo > firstPage || screenLine.y + screenLine.dy >= screenPt.y) {
                *startPageOut = pageNo;
                *startGlyphOut = lineStart;
                return true;
            }
        }
    }

    return false;
}

static bool ReadAloudGetGlyphAtCursor(DisplayModel* dm, Point screenPt, int* pageOut, int* glyphOut) {
    if (!dm || !pageOut || !glyphOut || !dm->textSelection) {
        return false;
    }
    // This is only called for an explicit cursor-reading action (context menu
    // or command), so load text on demand. Hover hit-testing remains cache-only
    // to avoid repeatedly extracting a large reflow chapter on mouse moves.
    if (!dm->IsOverText(screenPt, true)) {
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
    int glyph = dm->textSelection->GlyphIndexAt(pageNo, pt.x, pt.y);

    int textLen = 0;
    Rect* coords = nullptr;
    engine->GetTextForPage(pageNo, &textLen, &coords);
    if (textLen <= 0) {
        return false;
    }

    // Same adjustment as TextSelection::IsOverGlyph: FindClosestGlyph can return
    // the index after the glyph under the cursor when clicking its right half.
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
        return false;
    }

    *startPageOut = 0;
    *startGlyphOut = 0;

    int pageNo = 0;
    int glyph = 0;
    if (!ReadAloudGetGlyphAtCursor(dm, screenPt, &pageNo, &glyph)) {
        return false;
    }

    *startPageOut = pageNo;
    *startGlyphOut = glyph;
    return true;
}

bool ReadAloudHighlightBuildFromDocument(DisplayModel* dm, int startPage, int startGlyph, int endPageInclusive,
                                         ReadAloudHighlightMap* map, StrBuilder& cleanedOut) {
    if (!dm || !map || !dm->ValidPageNo(startPage)) {
        return false;
    }

    EngineBase* engine = dm->GetEngine();
    if (!engine) {
        return false;
    }

    int pageCount = dm->PageCount();
    int endPage = endPageInclusive > 0 ? endPageInclusive : pageCount;
    if (endPage > pageCount) {
        endPage = pageCount;
    }
    if (endPage < startPage) {
        return false;
    }

    Vec<ReadAloudRawByte> raw;
    if (!ReadAloudCollectDocumentRaw(raw, engine, startPage, startGlyph, endPage)) {
        return false;
    }

    if (!CleanRawBytes(raw, map, cleanedOut)) {
        return false;
    }

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

struct ReadAloudLineMetrics {
    RectF sample;
    float centerY = 0;
    float dy = 0;
};

static void ReadAloudUpdateLineMetrics(ReadAloudLineMetrics& m, int* glyphCount, const RectF& g) {
    float cy = g.y + g.dy * 0.5f;
    if (*glyphCount <= 0) {
        m.sample = g;
        m.centerY = cy;
        m.dy = g.dy;
        *glyphCount = 1;
        return;
    }
    (*glyphCount)++;
    m.centerY += (cy - m.centerY) / *glyphCount;
    m.dy = std::max(m.dy, g.dy);
}

static void ReadAloudBuildPageLineMetrics(ReadAloudHighlightMap* map, int pageNo, Vec<ReadAloudLineMetrics>& lines) {
    lines.Reset();
    if (!map || !map->locs || map->len <= 0) {
        return;
    }

    Vec<int> lineGlyphCounts;
    for (int i = 0; i < map->len; i++) {
        ReadAloudByteLoc& loc = map->locs[i];
        if (loc.pageNo != pageNo || !ReadAloudByteLocHasRect(loc)) {
            continue;
        }
        RectF g = ToRectF(ReadAloudByteLocToRect(loc));
        if (g.dx <= 0) {
            continue;
        }

        ptrdiff_t lineIdx = -1;
        for (size_t li = 0; li < lines.size(); li++) {
            if (ReadAloudSameTextLine(lines[li].sample, g)) {
                lineIdx = (ptrdiff_t)li;
                break;
            }
        }

        if (lineIdx < 0) {
            ReadAloudLineMetrics m;
            m.sample = g;
            m.centerY = g.y + g.dy * 0.5f;
            m.dy = g.dy;
            lines.Append(m);
            lineGlyphCounts.Append(1);
        } else {
            ReadAloudUpdateLineMetrics(lines[(size_t)lineIdx], &lineGlyphCounts[(size_t)lineIdx], g);
        }
    }
}

static const ReadAloudLineMetrics* ReadAloudFindLineMetrics(const Vec<ReadAloudLineMetrics>& lines, const RectF& rf) {
    for (const ReadAloudLineMetrics& m : lines) {
        if (ReadAloudSameTextLine(m.sample, rf)) {
            return &m;
        }
    }
    return nullptr;
}

static RectF ReadAloudSnapRectToLineMetrics(const RectF& horizontal, const ReadAloudLineMetrics& line) {
    if (horizontal.dx <= 0 || line.dy <= 0) {
        return RectF();
    }
    RectF rf = horizontal;
    rf.y = line.centerY - line.dy * 0.5f;
    rf.dy = line.dy;
    return ScaleHighlightBandRect(rf, kReadAloudHighlightBandRatio);
}

static void ReadAloudAppendWordLineRects(Vec<RectF>& lineRects, const Vec<RectF>& wordGlyphs,
                                         const Vec<ReadAloudLineMetrics>& pageLines) {
    Vec<RectF> groupHorizontal;
    Vec<RectF> groupSample;
    Vec<ReadAloudLineMetrics> groupMetrics;
    Vec<int> groupGlyphCounts;

    for (size_t i = 0; i < wordGlyphs.size(); i++) {
        const RectF& g = wordGlyphs[i];
        if (g.dx <= 0) {
            continue;
        }

        ptrdiff_t groupIdx = -1;
        for (size_t gi = 0; gi < groupSample.size(); gi++) {
            if (ReadAloudSameTextLine(groupSample[gi], g)) {
                groupIdx = (ptrdiff_t)gi;
                break;
            }
        }

        if (groupIdx < 0) {
            groupSample.Append(g);
            groupHorizontal.Append(RectF(g.x, 0, g.dx, 0));
            ReadAloudLineMetrics m;
            m.sample = g;
            m.centerY = g.y + g.dy * 0.5f;
            m.dy = g.dy;
            groupMetrics.Append(m);
            groupGlyphCounts.Append(1);
            continue;
        }

        RectF& horizontal = groupHorizontal[(size_t)groupIdx];
        float x0 = std::min(horizontal.x, g.x);
        float x1 = std::max(horizontal.x + horizontal.dx, g.x + g.dx);
        horizontal.x = x0;
        horizontal.dx = x1 - x0;
        ReadAloudUpdateLineMetrics(groupMetrics[(size_t)groupIdx], &groupGlyphCounts[(size_t)groupIdx], g);
    }

    for (size_t gi = 0; gi < groupHorizontal.size(); gi++) {
        const RectF& horizontal = groupHorizontal[gi];
        if (horizontal.dx <= 0) {
            continue;
        }
        ReadAloudLineMetrics lineMetrics;
        const ReadAloudLineMetrics* line = ReadAloudFindLineMetrics(pageLines, groupSample[gi]);
        if (line) {
            lineMetrics = *line;
        } else {
            lineMetrics = groupMetrics[gi];
        }
        if (lineMetrics.dy <= 0) {
            continue;
        }
        RectF rf = ReadAloudSnapRectToLineMetrics(horizontal, lineMetrics);
        if (rf.IsEmpty()) {
            continue;
        }
        bool merged = false;
        for (size_t i = 0; i < lineRects.size(); i++) {
            if (ReadAloudSameTextLine(lineRects[i], rf)) {
                lineRects[i] = MergeHighlightLineRect(lineRects[i], rf);
                merged = true;
                break;
            }
        }
        if (!merged) {
            lineRects.Append(rf);
        }
    }
}

static bool ReadAloudCollectWordHighlightScreenRects(MainWindow* win, WindowTab* tab, DisplayModel* dm,
                                                     int wordStartAbs, int wordEndAbs, Vec<Rect>& screenRects) {
    if (!win || !tab || !dm || !tab->readAloudHighlight) {
        return false;
    }

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    int wordPageMin = 0;
    int wordPageMax = 0;
    for (int i = wordStartAbs; i < wordEndAbs; i++) {
        int pageNo = map->locs[i].pageNo;
        if (pageNo <= 0 || !ReadAloudByteLocHasRect(map->locs[i])) {
            continue;
        }
        if (wordPageMin <= 0) {
            wordPageMin = pageNo;
            wordPageMax = pageNo;
        } else {
            wordPageMin = std::min(wordPageMin, pageNo);
            wordPageMax = std::max(wordPageMax, pageNo);
        }
    }
    if (wordPageMin <= 0) {
        return false;
    }

    for (int pageNo = wordPageMin; pageNo <= wordPageMax; pageNo++) {
        Vec<RectF> wordGlyphs;
        for (int i = wordStartAbs; i < wordEndAbs; i++) {
            ReadAloudByteLoc& loc = map->locs[i];
            if (loc.pageNo != pageNo || !ReadAloudByteLocHasRect(loc)) {
                continue;
            }
            PageInfo* pi = dm->GetPageInfo(loc.pageNo);
            if (!pi || pi->visibleRatio <= 0.0) {
                continue;
            }
            wordGlyphs.Append(ToRectF(ReadAloudByteLocToRect(loc)));
        }
        if (wordGlyphs.empty()) {
            continue;
        }

        Vec<ReadAloudLineMetrics> pageLines;
        ReadAloudBuildPageLineMetrics(map, pageNo, pageLines);

        Vec<RectF> lineRects;
        ReadAloudAppendWordLineRects(lineRects, wordGlyphs, pageLines);

        for (RectF& u : lineRects) {
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

// Reading-band layout: follow target is 10% from the top (page turn and same-page scroll).
// Reading progresses downward, so lines above 78% are intentional context — only
// scroll when the anchor drops below 78%.
static constexpr float kReadAloudOuterBottomRatio = 0.78f;
static constexpr float kReadAloudTargetRatio = 0.10f;

static bool ReadAloudCollectAnchorPageRect(WindowTab* tab, int wordStartAbs, int wordEndAbs, int* pageNoOut,
                                           RectF* pageRectOut) {
    if (!tab || !pageNoOut || !pageRectOut || !tab->readAloudHighlight) {
        return false;
    }

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    if (wordStartAbs < 0 || wordStartAbs >= map->len) {
        return false;
    }

    int pageNo = map->locs[wordStartAbs].pageNo;
    if (pageNo <= 0) {
        return false;
    }

    Vec<RectF> wordGlyphs;
    for (int i = wordStartAbs; i < wordEndAbs; i++) {
        ReadAloudByteLoc& loc = map->locs[i];
        if (loc.pageNo != pageNo || !ReadAloudByteLocHasRect(loc)) {
            continue;
        }
        wordGlyphs.Append(ToRectF(ReadAloudByteLocToRect(loc)));
    }

    if (wordGlyphs.size() == 0) {
        return false;
    }

    Vec<ReadAloudLineMetrics> pageLines;
    ReadAloudBuildPageLineMetrics(map, pageNo, pageLines);

    Vec<RectF> lineRects;
    ReadAloudAppendWordLineRects(lineRects, wordGlyphs, pageLines);

    if (lineRects.size() == 0) {
        return false;
    }

    RectF anchor = lineRects[0];
    for (size_t i = 1; i < lineRects.size(); i++) {
        anchor = anchor.Union(lineRects[i]);
    }

    *pageNoOut = pageNo;
    *pageRectOut = anchor;
    return true;
}

static bool ReadAloudGetCurrentAnchor(WindowTab* tab, DisplayModel* dm, int* pageNoOut, RectF* pageRectOut,
                                      Rect* screenRectOut) {
    if (!tab || !dm || !pageNoOut || !pageRectOut || !screenRectOut) {
        return false;
    }

    int wordStartAbs = 0;
    int wordEndAbs = 0;
    if (!ReadAloudGetCurrentWordAbsRange(tab, &wordStartAbs, &wordEndAbs)) {
        return false;
    }

    int pageNo = 0;
    RectF pageRect;
    if (!ReadAloudCollectAnchorPageRect(tab, wordStartAbs, wordEndAbs, &pageNo, &pageRect)) {
        return false;
    }

    if (!dm->ValidPageNo(pageNo)) {
        return false;
    }

    dm->EnsurePagesInfoForPage(pageNo);
    dm->PreparePageNavigation(pageNo);
    Rect screenRect = dm->CvtToScreen(pageNo, pageRect);
    if (screenRect.IsEmpty()) {
        return false;
    }

    *pageNoOut = pageNo;
    *pageRectOut = pageRect;
    *screenRectOut = screenRect;
    return true;
}

static int ReadAloudScrollYForAnchorAtRatio(DisplayModel* dm, int pageNo, const RectF& anchorPageRect, float ratio) {
    if (!dm || pageNo <= 0 || anchorPageRect.IsEmpty()) {
        return 0;
    }

    dm->EnsurePagesInfoForPage(pageNo);
    PageInfo* pi = dm->GetPageInfo(pageNo);
    if (!pi) {
        return 0;
    }

    float zoom = dm->GetZoomReal(pageNo);
    if (zoom <= 0) {
        zoom = 1.f;
    }
    RectF tr = dm->GetEngine()->Transform(anchorPageRect, pageNo, zoom, dm->GetRotation());
    int centerY = (int)(tr.y + tr.dy / 2);
    int targetY = (int)(dm->GetViewPort().dy * ratio);
    return std::max(0, centerY - targetY);
}

static int ReadAloudClampViewY(DisplayModel* dm, int viewY) {
    if (!dm) {
        return 0;
    }
    int maxY = dm->canvasSize.dy - dm->GetViewPort().dy;
    if (maxY < 0) {
        maxY = 0;
    }
    return limitValue(viewY, 0, maxY);
}

static int ReadAloudTargetViewYForPage(DisplayModel* dm, int pageNo, const RectF& pageRect, float ratio) {
    int scrollY = ReadAloudScrollYForAnchorAtRatio(dm, pageNo, pageRect, ratio);
    dm->EnsurePagesInfoForPage(pageNo);
    PageInfo* pi = dm->GetPageInfo(pageNo);
    if (!pi) {
        return dm->yOffset();
    }
    if (IsContinuous(dm->GetDisplayMode())) {
        return ReadAloudClampViewY(dm, pi->pos.y - dm->windowMargin.top + scrollY);
    }
    return ReadAloudClampViewY(dm, scrollY);
}

static void ReadAloudAnimateViewYTo(MainWindow* win, DisplayModel* dm, int targetViewY) {
    if (!win || !dm || !win->hwndCanvas) {
        return;
    }

    targetViewY = ReadAloudClampViewY(dm, targetViewY);
    int current = dm->yOffset();
    if (current == targetViewY) {
        return;
    }

    constexpr int kMinAnimateDeltaPx = 12;
    if (std::abs(targetViewY - current) < kMinAnimateDeltaPx) {
        win->readAloudScrollFromCode = true;
        dm->ScrollYTo(targetViewY);
        win->readAloudScrollFromCode = false;
        return;
    }

    win->scrollTargetY = targetViewY;
    win->readAloudScrollFromCode = true;
    SetTimer(win->hwndCanvas, kSmoothScrollTimerID, USER_TIMER_MINIMUM, nullptr);
}

static void ReadAloudScrollScreenAnchorToRatio(MainWindow* win, DisplayModel* dm, const Rect& anchorScreen,
                                               float ratio) {
    if (!win || !dm || anchorScreen.IsEmpty()) {
        return;
    }

    Size viewPort = dm->GetViewPort().Size();
    if (viewPort.dy <= 0) {
        return;
    }

    float centerY = anchorScreen.y + anchorScreen.dy / 2.0f;
    int targetY = (int)(viewPort.dy * ratio);
    int sy = (int)(centerY - targetY);
    if (sy != 0) {
        ReadAloudAnimateViewYTo(win, dm, dm->yOffset() + sy);
    }
}

static float ReadAloudAnchorLineCenterY(const RectF& pageRect) {
    return pageRect.y + pageRect.dy / 2.0f;
}

static bool ReadAloudIsSameAnchorLine(int pageNo, const RectF& pageRect, int holdPageNo, float holdLineY) {
    if (holdPageNo <= 0 || holdLineY < 0) {
        return false;
    }
    if (pageNo != holdPageNo) {
        return false;
    }
    float centerY = ReadAloudAnchorLineCenterY(pageRect);
    float tolerance = std::max(pageRect.dy * 0.75f, 8.f);
    return std::abs(centerY - holdLineY) <= tolerance;
}

static bool ReadAloudAnchorNeedsViewSync(const Rect& anchorScreen, Size viewSize) {
    if (anchorScreen.IsEmpty() || viewSize.dx <= 0 || viewSize.dy <= 0) {
        return false;
    }

    float centerY = anchorScreen.y + anchorScreen.dy / 2.0f;
    float ratio = centerY / viewSize.dy;
    return ratio > kReadAloudOuterBottomRatio;
}

static bool ReadAloudAnchorVisibleInCanvas(MainWindow* win, const Rect& anchorScreen) {
    if (!win || anchorScreen.IsEmpty()) {
        return false;
    }
    return !anchorScreen.Intersect(win->canvasRc).IsEmpty();
}

static void ReadAloudSyncViewToAnchor(MainWindow* win, WindowTab* tab, DisplayModel* dm, int pageNo,
                                      const RectF& pageRect, const Rect& anchorScreen) {
    DisplayMode mode = dm->GetDisplayMode();
    bool anchorOnScreen = ReadAloudAnchorVisibleInCanvas(win, anchorScreen);

    if (!IsContinuous(mode) && !dm->PageVisible(pageNo)) {
        win->readAloudScrollFromCode = true;
        defer {
            win->readAloudScrollFromCode = false;
        };
        int scrollY = ReadAloudScrollYForAnchorAtRatio(dm, pageNo, pageRect, kReadAloudTargetRatio);
        dm->GoToPage(pageNo, scrollY, false);
        return;
    }

    if (anchorOnScreen) {
        ReadAloudScrollScreenAnchorToRatio(win, dm, anchorScreen, kReadAloudTargetRatio);
        return;
    }

    int targetViewY = ReadAloudTargetViewYForPage(dm, pageNo, pageRect, kReadAloudTargetRatio);
    ReadAloudAnimateViewYTo(win, dm, targetViewY);
}

void ReadAloudOnUserViewChanged(MainWindow* win) {
    if (!win || win->readAloudScrollFromCode || !TtsIsSpeaking()) {
        return;
    }

    WindowTab* tab = GetReadAloudSourceTab();
    if (!tab || tab->win != win || win->CurrentTab() != tab || !tab->readAloudAutoScroll) {
        return;
    }

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return;
    }

    int pageNo = 0;
    RectF pageRect;
    Rect anchorScreen;
    if (!ReadAloudGetCurrentAnchor(tab, dm, &pageNo, &pageRect, &anchorScreen) ||
        !ReadAloudAnchorVisibleInCanvas(win, anchorScreen)) {
        tab->readAloudAutoScroll = false;
    }
}

void ReadAloudUpdateAutoScroll(MainWindow* win) {
    if (!win || !TtsIsSpeaking()) {
        return;
    }

    WindowTab* tab = GetReadAloudSourceTab();
    if (!tab || tab->win != win || win->CurrentTab() != tab || !tab->readAloudAutoScroll) {
        return;
    }

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return;
    }

    int pageNo = 0;
    RectF pageRect;
    Rect anchorScreen;
    if (!ReadAloudGetCurrentAnchor(tab, dm, &pageNo, &pageRect, &anchorScreen)) {
        return;
    }

    Size viewSize = dm->GetViewPort().Size();

    // After start/resume, keep the view stable on the first visible line. TTS advances
    // spokenPos within a word immediately, so word-index comparisons are unreliable here.
    if (tab->readAloudAutoScrollHold) {
        float lineY = ReadAloudAnchorLineCenterY(pageRect);
        if (tab->readAloudAutoScrollHoldPageNo < 0) {
            if (!ReadAloudAnchorVisibleInCanvas(win, anchorScreen)) {
                tab->readAloudAutoScrollHold = false;
            } else {
                tab->readAloudAutoScrollHoldPageNo = pageNo;
                tab->readAloudAutoScrollHoldLineY = lineY;
                return;
            }
        } else if (ReadAloudIsSameAnchorLine(pageNo, pageRect, tab->readAloudAutoScrollHoldPageNo,
                                             tab->readAloudAutoScrollHoldLineY)) {
            return;
        } else {
            tab->readAloudAutoScrollHold = false;
        }
    }

    if (!ReadAloudAnchorNeedsViewSync(anchorScreen, viewSize)) {
        return;
    }

    ReadAloudSyncViewToAnchor(win, tab, dm, pageNo, pageRect, anchorScreen);
}

static bool ReadAloudSourceTabIsCurrentTab(MainWindow* win) {
    if (!win) {
        return false;
    }
    WindowTab* tab = GetReadAloudSourceTab();
    return tab && tab->win == win && win->CurrentTab() == tab;
}

void PaintReadAloudHighlight(MainWindow* win, HDC hdc) {
    if (!TtsIsSpeaking()) {
        return;
    }
    if (!win) {
        return;
    }

    WindowTab* tab = GetReadAloudSourceTab();
    if (!ReadAloudSourceTabIsCurrentTab(win)) {
        return;
    }

    ReadAloudHighlightMap* map = tab->readAloudHighlight;
    if (!map || !map->locs || map->len <= 0) {
        return;
    }

    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return;
    }

    int wordStartAbs = 0;
    int wordEndAbs = 0;
    if (!ReadAloudGetCurrentWordAbsRange(tab, &wordStartAbs, &wordEndAbs)) {
        return;
    }

    if (wordStartAbs < 0 || wordStartAbs >= map->len) {
        return;
    }
    if (wordEndAbs > map->len) {
        wordEndAbs = map->len;
    }
    if (wordEndAbs <= wordStartAbs) {
        return;
    }

    Vec<Rect> screenRects;
    if (!ReadAloudCollectWordHighlightScreenRects(win, tab, dm, wordStartAbs, wordEndAbs, screenRects)) {
        return;
    }

    PaintFindMatchHighlightRectangles(hdc, win->canvasRc, screenRects, GetReadAloudHighlightColor(),
                                      kSelectionHighlightAlpha);
}

static void ReadAloudEnsureLayoutSynced(WindowTab* tab, MainWindow* win) {
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!dm) {
        return;
    }
    bool isCurrent = win && win->CurrentTab() == tab;
    if (isCurrent) {
        win->UpdateCanvasSize();
    }
    dm->OnMorePagesAvailablePreservingScroll(isCurrent, true);
}

static int ReadAloudWordStartUtf8(const char* text, int pos) {
    if (!text || pos < 0) {
        return pos;
    }
    int len = str::Leni(text);
    if (pos >= len) {
        return len;
    }
    while (pos > 0 && !IsReadAloudHorizontalSpace(text[pos - 1]) && !IsReadAloudLineBreak(text[pos - 1])) {
        pos--;
    }
    return pos;
}

static int ReadAloudFindContextInNewText(const char* oldText, int oldAbs, const char* newText) {
    if (!oldText || !newText || oldAbs < 0) {
        return -1;
    }
    int oldLen = str::Leni(oldText);
    if (oldAbs >= oldLen) {
        return -1;
    }

    static const int contextRadii[] = {96, 48, 24};
    for (int radius : contextRadii) {
        int contextStart = std::max(0, oldAbs - radius / 2);
        int contextEnd = std::min(oldLen, oldAbs + radius);
        while (contextStart < oldAbs && (oldText[contextStart] & 0xc0) == 0x80) {
            contextStart++;
        }
        while (contextEnd < oldLen && (oldText[contextEnd] & 0xc0) == 0x80) {
            contextEnd++;
        }
        if (contextEnd <= contextStart) {
            continue;
        }
        TempStr context = str::DupTemp(oldText + contextStart, (size_t)(contextEnd - contextStart));
        const char* contextHit = str::Find(newText, context);
        if (contextHit) {
            return (int)(contextHit - newText) + oldAbs - contextStart;
        }
    }
    return -1;
}

static int ReadAloudFindAnchorInNewText(const char* oldText, int oldAbs, const char* newText) {
    if (!oldText || !newText || oldAbs < 0) {
        return -1;
    }
    int oldLen = str::Leni(oldText);
    if (oldAbs >= oldLen) {
        return -1;
    }

    // Prefer some surrounding text over the current word. Short words (and CJK
    // characters in particular) often occur many times in a chapter, so a word-only
    // search can resume at the wrong occurrence after pagination changes.
    int contextAnchor = ReadAloudFindContextInNewText(oldText, oldAbs, newText);
    if (contextAnchor >= 0) {
        return contextAnchor;
    }

    int wordStart = ReadAloudWordStartUtf8(oldText, oldAbs);
    int wordEnd = ReadAloudWordEndUtf8(oldText, oldAbs);
    if (wordEnd <= wordStart) {
        return -1;
    }

    TempStr needle = str::DupTemp(oldText + wordStart, (size_t)(wordEnd - wordStart));
    if (str::IsEmpty(needle)) {
        return -1;
    }

    int newLen = str::Leni(newText);
    int searchFrom = std::max(0, std::min(oldAbs, newLen - 1) - 80);
    const char* hit = str::FindI(newText + searchFrom, needle);
    if (!hit) {
        hit = str::FindI(newText, needle);
    }
    if (!hit) {
        return -1;
    }
    return (int)(hit - newText);
}

static int ReadAloudChapterAtAnchor(const ReadAloudHighlightMap* map, int anchorAbs) {
    if (!map || !map->locs || map->len <= 0 || anchorAbs < 0) {
        return -1;
    }
    anchorAbs = std::min(anchorAbs, map->len - 1);
    for (int distance = 0; distance < map->len; distance++) {
        int after = anchorAbs + distance;
        if (after < map->len && map->locs[after].chapter >= 0) {
            return map->locs[after].chapter;
        }
        int before = anchorAbs - distance;
        if (before >= 0 && map->locs[before].chapter >= 0) {
            return map->locs[before].chapter;
        }
    }
    return -1;
}

static int ReadAloudPageAtAnchor(const ReadAloudHighlightMap* map, int anchorAbs) {
    if (!map || !map->locs || map->len <= 0 || anchorAbs < 0) {
        return -1;
    }
    anchorAbs = std::min(anchorAbs, map->len - 1);
    for (int distance = 0; distance < map->len; distance++) {
        int after = anchorAbs + distance;
        if (after < map->len && map->locs[after].pageNo > 0) {
            return map->locs[after].pageNo;
        }
        int before = anchorAbs - distance;
        if (before >= 0 && map->locs[before].pageNo > 0) {
            return map->locs[before].pageNo;
        }
    }
    return -1;
}

static int ReadAloudFindAnchorPageAfterReflow(EngineBase* engine, const char* oldText, int anchorAbs, int oldPageNo) {
    if (!engine || !oldText || anchorAbs < 0) {
        return -1;
    }
    int pageCount = engine->PageCount();
    oldPageNo = limitValue(oldPageNo, 1, pageCount);
    for (int distance = 0; distance < pageCount; distance++) {
        int candidates[] = {oldPageNo + distance, oldPageNo - distance};
        int candidateCount = distance == 0 ? 1 : 2;
        for (int i = 0; i < candidateCount; i++) {
            int pageNo = candidates[i];
            if (pageNo < 1 || pageNo > pageCount) {
                continue;
            }
            StrBuilder pageText;
            ReadAloudHighlightMap pageMap{};
            bool built = ReadAloudHighlightBuildFromPage(engine, pageNo, &pageMap, pageText);
            ReadAloudHighlightFree(&pageMap);
            if (built && ReadAloudFindContextInNewText(oldText, anchorAbs, pageText.Get()) >= 0) {
                return pageNo;
            }
        }
    }
    return -1;
}

static bool ReadAloudStealHighlightMap(WindowTab* tab, ReadAloudHighlightMap* newMap) {
    if (!tab || !tab->readAloudHighlight || !newMap || !newMap->locs || newMap->len <= 0) {
        return false;
    }
    ReadAloudHighlightFree(tab->readAloudHighlight);
    *tab->readAloudHighlight = *newMap;
    newMap->locs = nullptr;
    newMap->len = 0;
    newMap->cap = 0;
    return true;
}

static bool ReadAloudReplaceHighlightMap(WindowTab* tab, ReadAloudHighlightMap* newMap, const char* rebuiltText) {
    if (!tab || !tab->readAloudHighlight || str::IsEmpty(tab->readAloudText) || !newMap || !rebuiltText) {
        return false;
    }
    int textLen = str::Leni(tab->readAloudText);
    if (newMap->len != textLen || newMap->len != str::Leni(rebuiltText)) {
        return false;
    }
    // Equal byte lengths do not mean equal text. Reflow can produce another
    // page batch of exactly the same length; attaching its coordinates to the
    // old TTS buffer makes the highlight land on unrelated text or images.
    if (!str::Eq(tab->readAloudText, rebuiltText)) {
        return false;
    }
    return ReadAloudStealHighlightMap(tab, newMap);
}

static bool ReadAloudRelocateTextAndMap(WindowTab* tab, ReadAloudHighlightMap* newMap, const char* rebuiltText,
                                        int anchorAbs, bool* textRelocatedOut) {
    if (!tab || !newMap || str::IsEmpty(rebuiltText) || newMap->len != str::Leni(rebuiltText)) {
        return false;
    }
    int newLen = newMap->len;
    if (anchorAbs >= 0) {
        int relocated = ReadAloudFindAnchorInNewText(tab->readAloudText, anchorAbs, rebuiltText);
        if (relocated >= 0) {
            anchorAbs = relocated;
        }
    }
    if (anchorAbs < 0 || anchorAbs >= newLen) {
        int oldLen = str::Leni(tab->readAloudText);
        if (oldLen > 0 && anchorAbs >= 0) {
            anchorAbs = (int)((int64_t)anchorAbs * newLen / oldLen);
        } else {
            anchorAbs = 0;
        }
        anchorAbs = limitValue(anchorAbs, 0, newLen - 1);
    }

    str::ReplaceWithCopy(&tab->readAloudText, rebuiltText);
    if (!ReadAloudStealHighlightMap(tab, newMap)) {
        return false;
    }

    tab->readAloudChunkStart = anchorAbs;
    tab->readAloudChunkEnd = anchorAbs;
    if (tab->readAloudResumePos > 0) {
        tab->readAloudResumePos = anchorAbs + tab->readAloudHighlightBase;
    }
    if (textRelocatedOut) {
        *textRelocatedOut = true;
    }
    return true;
}

bool RefreshReadAloudHighlightAfterLayoutChange(WindowTab* tab, MainWindow* win, bool* textRelocatedOut) {
    if (textRelocatedOut) {
        *textRelocatedOut = false;
    }
    if (!tab || str::IsEmpty(tab->readAloudText) || !tab->readAloudHighlight) {
        return false;
    }
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return false;
    }

    ReadAloudEnsureLayoutSynced(tab, win);

    EngineBase* engine = dm->GetEngine();
    if (engine) {
        engine->ClearTextCache();
    }

    int anchorAbs = -1;
    if (TtsIsSpeaking() && GetReadAloudSourceTab() == tab) {
        int spokenPos = TtsGetSpokenPosUtf8();
        if (spokenPos >= 0) {
            anchorAbs = tab->readAloudHighlightBase + tab->readAloudChunkStart + spokenPos;
        }
    } else if (tab->readAloudResumePos > 0) {
        anchorAbs = tab->readAloudResumePos - tab->readAloudHighlightBase;
    }

    StrBuilder cleaned;
    ReadAloudHighlightMap newMap{};
    bool ok = false;

    // Reflow invalidates global page and glyph numbers. For EPUB, rebuild from
    // the stable chapter containing the spoken byte instead of reusing the old
    // page interval, then relocate the spoken text within that chapter.
    int anchorChapter = ReadAloudChapterAtAnchor(tab->readAloudHighlight, anchorAbs);
    if (anchorChapter >= 0) {
        int chapterStartPage = 0;
        int chapterEndPage = 0;
        if (EngineMupdfGetReflowChapterPageRange(engine, anchorChapter, &chapterStartPage, &chapterEndPage)) {
            ok = ReadAloudHighlightBuildFromDocument(dm, chapterStartPage, 0, chapterEndPage, &newMap, cleaned);
            if (ok) {
                tab->readAloudStartPage = chapterStartPage;
                tab->readAloudStartGlyph = 0;
                tab->readAloudBuiltEndPage = chapterEndPage;
            }
        }
    }

    // Legacy MOBI/EPUB engines are recreated for a font-size change and don't
    // expose chapter locations. Find the same surrounding text in the new page
    // stream, then rebuild the normal lazy read-aloud batch around that page.
    if (!ok && anchorAbs >= 0 && tab->readAloudBuiltEndPage > 0) {
        int oldAnchorPage = ReadAloudPageAtAnchor(tab->readAloudHighlight, anchorAbs);
        int anchorPage = ReadAloudFindAnchorPageAfterReflow(engine, tab->readAloudText, anchorAbs, oldAnchorPage);
        if (anchorPage > 0) {
            int endPage = std::min(dm->PageCount(), anchorPage + kReadAloudBuildPagesPerBatch - 1);
            ok = ReadAloudHighlightBuildFromDocument(dm, anchorPage, 0, endPage, &newMap, cleaned);
            if (ok) {
                tab->readAloudStartPage = anchorPage;
                tab->readAloudStartGlyph = 0;
                tab->readAloudBuiltEndPage = endPage;
            }
        }
    }

    if (!ok && tab->readAloudScope == WindowTab::ReadAloudScopeSelection) {
        if (dm->textSelection && dm->textSelection->result.len > 0) {
            ok = ReadAloudHighlightBuildFromTextSelection(dm->textSelection, &newMap, cleaned);
        }
    } else if (!ok && tab->readAloudBuiltEndPage > 0) {
        int startPage = tab->readAloudStartPage;
        int startGlyph = tab->readAloudStartGlyph;
        if (startPage <= 0) {
            for (int i = 0; i < tab->readAloudHighlight->len; i++) {
                if (tab->readAloudHighlight->locs[i].pageNo > 0) {
                    startPage = tab->readAloudHighlight->locs[i].pageNo;
                    break;
                }
            }
        }
        if (startPage > 0) {
            ok = ReadAloudHighlightBuildFromDocument(dm, startPage, startGlyph, tab->readAloudBuiltEndPage, &newMap,
                                                     cleaned);
        }
    } else if (!ok && dm->textSelection && dm->textSelection->result.len > 0) {
        ok = ReadAloudHighlightBuildFromTextSelection(dm->textSelection, &newMap, cleaned);
    }

    if (!ok) {
        ReadAloudHighlightFree(&newMap);
        return false;
    }

    bool updated = false;
    if (ReadAloudReplaceHighlightMap(tab, &newMap, cleaned.Get())) {
        updated = true;
    } else if (ReadAloudRelocateTextAndMap(tab, &newMap, cleaned.Get(), anchorAbs, textRelocatedOut)) {
        updated = true;
    } else {
        ReadAloudHighlightFree(&newMap);
        return false;
    }

    // re-anchor auto-scroll after layout shift so the view doesn't drift on its own
    tab->readAloudAutoScrollHold = true;
    tab->readAloudAutoScrollHoldPageNo = -1;
    tab->readAloudAutoScrollHoldLineY = -1.f;

    if (win && win->CurrentTab() == tab && TtsIsSpeaking() && GetReadAloudSourceTab() == tab) {
        tab->readAloudAutoScroll = true;
        int pageNo = 0;
        RectF pageRect;
        Rect anchorScreen;
        if (ReadAloudGetCurrentAnchor(tab, dm, &pageNo, &pageRect, &anchorScreen)) {
            tab->readAloudAutoScrollHold = false;
            ReadAloudSyncViewToAnchor(win, tab, dm, pageNo, pageRect, anchorScreen);
        }
    }

    if (win && win->CurrentTab() == tab) {
        ScheduleRepaint(win, 0);
    }
    return updated;
}
