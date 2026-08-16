/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "ProgressUpdateUI.h"
#include "TextSelection.h"
#include "TextSearch.h"

// ignore spaces between CJK glyphs but not between Latin, Greek, Cyrillic, etc. letters
// cf. http://code.google.com/p/sumatrapdf/issues/detail?id=959
#define isnoncjkwordchar(c) (isWordChar(c) && (unsigned)(c) < 0x2E80)

static int TextByteLen(const char* s) {
    return s ? (int)str::Len(s) : 0;
}

static int Utf16CodeUnitsForCodepoint(int codepoint) {
    return codepoint >= 0x10000 && codepoint <= 0x10FFFF ? 2 : 1;
}

static int ReadWcharCodepoint(const WCHAR* text, int len, int& idx) {
    if (!text || idx < 0 || idx >= len) {
        return 0;
    }
    WCHAR wc = text[idx++];
    if (wc >= 0xD800 && wc < 0xDC00 && idx < len) {
        WCHAR w2 = text[idx++];
        return 0x10000 + ((wc - 0xD800) << 10) + (w2 - 0xDC00);
    }
    return (int)wc;
}

// Fetch page text for search. When *abortSearch is set, the caller should stop
// immediately (search was cancelled while engine locks were contended).
// lenOut is the number of Unicode codepoints (same indexing as WCHAR page text).
static const char* GetTextForPageUtf8ForSearch(EngineBase* engine, int pageNo, int* lenOut,
                                               const ProgressUpdateCb& progressCb, bool* abortSearch) {
    if (abortSearch) {
        *abortSearch = false;
    }
    int byteLen = 0;
    const char* text = nullptr;
    if (engine->TryGetTextForPageUtf8(pageNo, &byteLen, nullptr, &text)) {
        if (lenOut) {
            if (text && byteLen > 0) {
                *lenOut = Utf8CodepointCountN(text, byteLen);
            } else {
                *lenOut = 0;
            }
        }
        return text;
    }
    if (WasCanceled(progressCb)) {
        if (abortSearch) {
            *abortSearch = true;
        }
        if (lenOut) {
            *lenOut = 0;
        }
        return nullptr;
    }
    text = engine->GetTextForPageUtf8(pageNo, &byteLen);
    if (lenOut) {
        if (text && byteLen > 0) {
            *lenOut = Utf8CodepointCountN(text, byteLen);
        } else {
            *lenOut = 0;
        }
    }
    return text;
}

static void SkipWhitespace(const char* text, int textLen, int& idx, int& byteIdx) {
    while (idx < textLen) {
        int nextByte = byteIdx;
        int c = Utf8CodepointNext(text, TextByteLen(text), nextByte);
        if (!str::IsWs((char)c)) {
            break;
        }
        byteIdx = nextByte;
        idx++;
    }
}

static void markAllPagesNonSkip(Vec<bool>& pagesToSkip) {
    for (size_t i = 0; i < pagesToSkip.size(); i++) {
        pagesToSkip[i] = false;
    }
}

static void markAllPagesMatchCacheInvalid(Vec<bool>& pageMatchesCached) {
    for (size_t i = 0; i < pageMatchesCached.size(); i++) {
        pageMatchesCached[i] = false;
    }
}

static void RebuildAnchorAsciiMask(TextSearch* ts) {
    ts->anchorAsciiMask = 0;
    if (!ts->anchor || ts->anchorLen <= 0) {
        return;
    }
    int anchorByteLen = TextByteLen(ts->anchor);
    u32 mask = 0;
    for (int i = 0; i < anchorByteLen; i++) {
        unsigned char b = (unsigned char)ts->anchor[i];
        if (b < 0x80) {
            int c = b;
            if (!ts->matchCase && c >= 'A' && c <= 'Z') {
                c += 32;
            }
            if (c >= 'a' && c <= 'z') {
                mask |= (1u << (c - 'a'));
            }
        }
    }
    ts->anchorAsciiMask = mask;
}

static bool AnchorSupportsUtf8ByteSearch(const char* anchor, int anchorByteLen, bool matchCase) {
    if (!anchor || anchorByteLen <= 0) {
        return false;
    }
    if (!matchCase) {
        for (int i = 0; i < anchorByteLen; i++) {
            unsigned char b = (unsigned char)anchor[i];
            if (b < 0x80 && ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z'))) {
                return false;
            }
        }
    }
    return true;
}

static void FreePageMatchList(TextSearch::PageMatchList& list) {
    free(list.data);
    list.data = nullptr;
    list.count = 0;
}

static void MarkSkippedPageMatchCache(TextSearch* ts, int pageNo) {
    FreePageMatchList(ts->pageMatchesCache[pageNo - 1]);
    ts->pageMatchesCached[pageNo - 1] = true;
}

void TextSearch::SetPageMatchCache(int pageNo, const PageMatchList& spans) {
    ReportIf(pageNo < 1 || pageNo > nPages);
    PageMatchList& cache = pageMatchesCache[pageNo - 1];
    FreePageMatchList(cache);
    if (spans.count > 0 && spans.data) {
        cache.data = (MatchSpan*)memdup(spans.data, spans.count * sizeof(MatchSpan));
        cache.count = spans.count;
    }
    pageMatchesCached[pageNo - 1] = true;
}

void TextSearch::SetPageMatchCache(int pageNo, const Vec<MatchSpan>& spans) {
    ReportIf(pageNo < 1 || pageNo > nPages);
    PageMatchList& cache = pageMatchesCache[pageNo - 1];
    FreePageMatchList(cache);
    int n = (int)spans.size();
    if (n > 0) {
        cache.data = (MatchSpan*)memdup(spans.LendData(), n * sizeof(MatchSpan));
        cache.count = n;
    }
    pageMatchesCached[pageNo - 1] = true;
}

static bool QuickRejectWholeWordMatch(const char* pageText, int pageTextByteLen, int pageTextLen, int found,
                                      int findTextLen, bool matchWordStart, bool matchWordEnd) {
    if (matchWordStart && found > 0) {
        int byteIdx = Utf8CodepointToByteIndex(pageText, pageTextByteLen, found);
        int prevByteIdx = byteIdx;
        int prevCh = Utf8CodepointPrev(pageText, pageTextByteLen, prevByteIdx);
        int curByteIdx = byteIdx;
        int curCh = Utf8CodepointNext(pageText, pageTextByteLen, curByteIdx);
        if (isWordChar(prevCh) && isWordChar(curCh)) {
            return true;
        }
    }
    if (matchWordEnd && found + findTextLen < pageTextLen) {
        int endIdx = found + findTextLen;
        int endByteIdx = Utf8CodepointToByteIndex(pageText, pageTextByteLen, endIdx);
        int prevByteIdx = endByteIdx;
        int prevCh = Utf8CodepointPrev(pageText, pageTextByteLen, prevByteIdx);
        int curCh = Utf8CodepointNext(pageText, pageTextByteLen, endByteIdx);
        if (isWordChar(prevCh) && isWordChar(curCh)) {
            return true;
        }
    }
    return false;
}

static void FreeAllPageMatchLists(Vec<TextSearch::PageMatchList>& cache) {
    int n = (int)cache.size();
    for (int i = 0; i < n; i++) {
        FreePageMatchList(cache[i]);
    }
}

TextSearch::TextSearch(EngineBase* engine) : TextSelection(engine) {
    nPages = engine->PageCount();
    pagesToSkip.SetSize(nPages);
    markAllPagesNonSkip(pagesToSkip);
    EnsurePageMatchCacheSize();
    markAllPagesMatchCacheInvalid(pageMatchesCached);
}

TextSearch::~TextSearch() {
    FreeAllPageMatchLists(pageMatchesCache);
    Clear();
}

void TextSearch::SetMaxPageCount(int max) {
    maxPageCount = max;
    SyncPageCount();
}

void TextSearch::EnsurePageMatchCacheSize() {
    int oldSize = (int)pageMatchesCache.size();
    if (nPages <= 0) {
        FreeAllPageMatchLists(pageMatchesCache);
        pageMatchesCache.Reset();
        pageMatchesCached.Reset();
        return;
    }
    if (oldSize == nPages) {
        return;
    }
    FreeAllPageMatchLists(pageMatchesCache);
    pageMatchesCache.SetSize(nPages);
    pageMatchesCached.SetSize(nPages);
    for (int i = 0; i < nPages; i++) {
        pageMatchesCache[i].data = nullptr;
        pageMatchesCache[i].count = 0;
        pageMatchesCached[i] = false;
    }
}

void TextSearch::ApplyCachedAsciiPageSkip() {
    if (anchorAsciiMask == 0 || nPages <= 0) {
        return;
    }
    EnsurePageMatchCacheSize();
    engine->ApplyAsciiMaskPageSkip(anchorAsciiMask, nPages, pagesToSkip);
    for (int pageNo = 1; pageNo <= nPages; pageNo++) {
        if (pagesToSkip[pageNo - 1] && !pageMatchesCached[pageNo - 1]) {
            MarkSkippedPageMatchCache(this, pageNo);
        }
    }
}

void TextSearch::ApplyCachedUtf8AnchorPageSkip() {
    if (!anchor || nPages <= 0) {
        return;
    }
    int anchorByteLen = TextByteLen(anchor);
    if (!AnchorSupportsUtf8ByteSearch(anchor, anchorByteLen, matchCase)) {
        return;
    }
    EnsurePageMatchCacheSize();
    engine->ApplyUtf8AnchorPageSkip(anchor, anchorByteLen, nPages, pagesToSkip);
    for (int pageNo = 1; pageNo <= nPages; pageNo++) {
        if (pagesToSkip[pageNo - 1] && !pageMatchesCached[pageNo - 1]) {
            MarkSkippedPageMatchCache(this, pageNo);
        }
    }
}

const char* TextSearch::LoadPageText(int pageNo, int* lenOut, bool* abortSearch) {
    if (pageNo == pageTextPage && pageText) {
        if (lenOut) {
            *lenOut = pageTextLen;
        }
        return pageText;
    }
    pageText = GetTextForPageUtf8ForSearch(engine, pageNo, &pageTextLen, progressCb, abortSearch);
    pageTextPage = pageNo;
    if (lenOut) {
        *lenOut = pageTextLen;
    }
    return pageText;
}

const char* TextSearch::PreparePageOffsetMap(int pageNo, int* byteLenOut, int* codepointLenOut) {
    int textByteLen = 0;
    const char* text = engine->GetTextForPageUtf8(pageNo, &textByteLen);
    int wlen = 0;
    const WCHAR* wtext = engine->GetTextForPage(pageNo, &wlen);
    if (offsetMapPage != pageNo || offsetMapText != text || offsetMapTextByteLen != textByteLen) {
        offsetMapPage = pageNo;
        offsetMapText = text;
        offsetMapTextByteLen = textByteLen;
        offsetMapCodepointBytes.Reset();
        offsetMapCodepointToGlyph.Reset();
        offsetMapGlyphToCodepoint.Reset();
        offsetMapCodepointBytes.Append(0);
        offsetMapCodepointToGlyph.Append(0);
        offsetMapGlyphToCodepoint.SetSize(wlen + 1);
        if (wlen >= 0) {
            offsetMapGlyphToCodepoint[0] = 0;
        }
        int uByte = 0;
        int wIdx = 0;
        int cp = 0;
        int fallbackGlyph = 0;
        while (text && uByte < textByteLen) {
            int uCp = Utf8CodepointNext(text, textByteLen, uByte);
            bool matched = !wtext;
            int oldWIdx = wIdx;
            while (!matched && wIdx < wlen) {
                int wCp = ReadWcharCodepoint(wtext, wlen, wIdx);
                if (wCp == uCp) {
                    matched = true;
                }
            }
            cp++;
            fallbackGlyph += Utf16CodeUnitsForCodepoint(uCp);
            offsetMapCodepointBytes.Append(uByte);
            offsetMapCodepointToGlyph.Append(wtext ? wIdx : fallbackGlyph);
            for (int i = oldWIdx + 1; i <= wIdx && i <= wlen; i++) {
                offsetMapGlyphToCodepoint[i] = cp;
            }
        }
        for (int i = wIdx + 1; i <= wlen; i++) {
            offsetMapGlyphToCodepoint[i] = cp;
        }
    }
    if (byteLenOut) {
        *byteLenOut = textByteLen;
    }
    if (codepointLenOut) {
        *codepointLenOut = std::max(0, (int)offsetMapCodepointBytes.size() - 1);
    }
    return text;
}

int TextSearch::CodepointToGlyph(int pageNo, int codepointOffset) {
    PreparePageOffsetMap(pageNo);
    if (offsetMapCodepointToGlyph.size() == 0) {
        return 0;
    }
    int idx = limitValue(codepointOffset, 0, (int)offsetMapCodepointToGlyph.size() - 1);
    return offsetMapCodepointToGlyph[idx];
}

int TextSearch::GlyphToCodepoint(int pageNo, int glyphOffset) {
    PreparePageOffsetMap(pageNo);
    if (offsetMapGlyphToCodepoint.size() == 0) {
        return 0;
    }
    int idx = limitValue(glyphOffset, 0, (int)offsetMapGlyphToCodepoint.size() - 1);
    return offsetMapGlyphToCodepoint[idx];
}

int TextSearch::PageCodepointAt(int pageNo, int codepointOffset) {
    int byteLen = 0;
    const char* text = PreparePageOffsetMap(pageNo, &byteLen);
    if (!text || codepointOffset < 0 || codepointOffset >= (int)offsetMapCodepointBytes.size() - 1) {
        return 0;
    }
    return Utf8CodepointAtByte(text, byteLen, offsetMapCodepointBytes[codepointOffset]);
}

int TextSearch::PageCodepointByteOffset(int pageNo, int codepointOffset) {
    PreparePageOffsetMap(pageNo);
    if (offsetMapCodepointBytes.size() == 0) {
        return 0;
    }
    int idx = limitValue(codepointOffset, 0, (int)offsetMapCodepointBytes.size() - 1);
    return offsetMapCodepointBytes[idx];
}

void TextSearch::InvalidatePageMatchCache() {
    markAllPagesMatchCacheInvalid(pageMatchesCached);
    offsetMapPage = 0;
    offsetMapText = nullptr;
    offsetMapTextByteLen = 0;
    offsetMapCodepointBytes.Reset();
    offsetMapCodepointToGlyph.Reset();
    offsetMapGlyphToCodepoint.Reset();
}

bool TextSearch::TryGetCachedPageMatches(int pageNo, Vec<MatchSpan>* out) const {
    if (!out || pageNo < 1 || pageNo > nPages) {
        return false;
    }
    if ((int)pageMatchesCached.size() < pageNo || !pageMatchesCached[pageNo - 1]) {
        return false;
    }
    const PageMatchList& cached = pageMatchesCache[pageNo - 1];
    for (int i = 0; i < cached.count; i++) {
        out->Append(cached.data[i]);
    }
    return true;
}

bool TextSearch::PageMightContainAnchor(int pageNo) const {
    if (anchorAsciiMask != 0) {
        u32 pageMask = engine->GetPageAsciiLetterMask(pageNo);
        if (pageMask == UINT32_MAX) {
            return true;
        }
        return (pageMask & anchorAsciiMask) == anchorAsciiMask;
    }
    int anchorByteLen = TextByteLen(anchor);
    if (anchor && anchorByteLen > 0 && AnchorSupportsUtf8ByteSearch(anchor, anchorByteLen, matchCase)) {
        return engine->CachedPageContainsUtf8Bytes(pageNo, anchor, anchorByteLen);
    }
    return true;
}

void TextSearch::SyncPageCount() {
    int count = engine->PageCount();
    if (maxPageCount > 0 && maxPageCount < count) {
        count = maxPageCount;
    }
    if (count <= 0) {
        return;
    }
    if (count == nPages) {
        return;
    }
    int oldCount = nPages;
    nPages = count;
    pagesToSkip.SetSize(nPages);
    for (int i = oldCount; i < nPages; i++) {
        pagesToSkip[i] = false;
    }
    EnsurePageMatchCacheSize();
}

void TextSearch::Clear() {
    str::FreePtr(&findText);
    str::FreePtr(&anchor);
    str::FreePtr(&lastText);
    findTextLen = 0;
    anchorLen = 0;
    offsetMapPage = 0;
    offsetMapText = nullptr;
    offsetMapTextByteLen = 0;
    offsetMapCodepointBytes.Reset();
    offsetMapCodepointToGlyph.Reset();
    offsetMapGlyphToCodepoint.Reset();
    Reset();
}

void TextSearch::Reset() {
    pageText = nullptr;
    pageTextLen = 0;
    pageTextPage = 0;
    TextSelection::Reset();
}

int TextSearch::GetCurrentPageNo() const {
    return findPage;
}

// note: the result might not be a valid page number!
int TextSearch::GetSearchHitStartPageNo() const {
    return searchHitStartAt;
}

void TextSearch::SetText(const WCHAR* text) {
    this->matchWordStart = matchWholeWord || (text[0] == ' ' && text[1] != ' ');
    this->matchWordEnd = matchWholeWord || (str::EndsWith(text, L" ") && !str::EndsWith(text, L"  "));

    const WCHAR* searchText = text;
    if (searchText[0] == ' ') {
        searchText++;
    }

    if (str::Eq(this->lastText, searchText)) {
        return;
    }

    this->Clear();
    this->lastText = str::Dup(searchText);
    this->findText = ToUtf8(searchText);
    this->findTextLen = Utf8CodepointCountN(this->findText, TextByteLen(this->findText));

    int searchTextByteLen = TextByteLen(this->findText);
    int firstCharEndByte = 0;
    int firstChar = Utf8CodepointNext(this->findText, searchTextByteLen, firstCharEndByte);
    if (findTextLen > 0 && isnoncjkwordchar(firstChar)) {
        int end = 1;
        int endByte = firstCharEndByte;
        while (end < findTextLen) {
            int nextByte = endByte;
            int c = Utf8CodepointNext(this->findText, searchTextByteLen, nextByte);
            if (!isnoncjkwordchar(c)) {
                break;
            }
            endByte = nextByte;
            end++;
        }
        anchor = str::Dup(this->findText, endByte);
        anchorLen = end;
    } else if (findTextLen > 0 && (firstChar == '-' || firstChar == '\'' || firstChar == '"')) {
        anchor = nullptr;
        anchorLen = 0;
    } else if (findTextLen > 0) {
        anchor = str::Dup(this->findText, firstCharEndByte);
        anchorLen = 1;
    } else {
        anchor = nullptr;
        anchorLen = 0;
    }

    if (findText && findText[searchTextByteLen - 1] == ' ') {
        findText[searchTextByteLen - 1] = '\0';
        findTextLen--;
    }

    markAllPagesNonSkip(pagesToSkip);
    EnsurePageMatchCacheSize();
    markAllPagesMatchCacheInvalid(pageMatchesCached);
    RebuildAnchorAsciiMask(this);
    ApplyCachedAsciiPageSkip();
    ApplyCachedUtf8AnchorPageSkip();
}

void TextSearch::SetMatchCase(bool newMatchCase) {
    if (matchCase == newMatchCase) {
        return;
    }
    this->matchCase = newMatchCase;

    markAllPagesNonSkip(pagesToSkip);
    EnsurePageMatchCacheSize();
    markAllPagesMatchCacheInvalid(pageMatchesCached);
    RebuildAnchorAsciiMask(this);
    ApplyCachedAsciiPageSkip();
    ApplyCachedUtf8AnchorPageSkip();
}

void TextSearch::SetMatchWholeWord(bool newMatchWholeWord) {
    if (matchWholeWord == newMatchWholeWord) {
        return;
    }
    this->matchWholeWord = newMatchWholeWord;
    markAllPagesNonSkip(pagesToSkip);
    EnsurePageMatchCacheSize();
    markAllPagesMatchCacheInvalid(pageMatchesCached);
}

void TextSearch::SetDirection(TextSearch::Direction direction) {
    bool fwd = TextSearch::Direction::Forward == direction;
    if (fwd == forward) {
        return;
    }
    forward = fwd;
    if (findText) {
        int n = findTextLen;
        if (fwd) {
            findIndex += n;
        } else {
            findIndex -= n;
        }
    }
}

void TextSearch::SetLastResult(TextSelection* sel) {
    if (!sel) {
        return;
    }
    // `sel` may be this TextSearch itself (e.g. a match chosen from the
    // floating results list). CopySelection() resets its destination first,
    // which would also erase the source endpoints in that case. Preserve the
    // glyph range before rebuilding the selection.
    int selectedStartPage = sel->startPage;
    int selectedStartGlyph = sel->startGlyph;
    int selectedEndPage = sel->endPage;
    int selectedEndGlyph = sel->endGlyph;
    if (selectedStartPage < 1 || selectedEndPage < 1 || selectedStartGlyph < 0 || selectedEndGlyph < 0) {
        return;
    }

    Reset();
    StartAt(selectedStartPage, selectedStartGlyph);
    SelectUpTo(selectedEndPage, selectedEndGlyph);

    AutoFreeWStr selection(ExtractText(" "));
    if (!selection) {
        return;
    }
    str::NormalizeWSInPlace(selection);
    SetText(selection);

    // SetText() clears the selection when the spelling/case of the concrete
    // hit differs from the search query. Restore the exact cached match so the
    // canvas can paint it as the active hit and the n/m counter can locate it.
    StartAt(selectedStartPage, selectedStartGlyph);
    SelectUpTo(selectedEndPage, selectedEndGlyph);

    searchHitStartAt = findPage = std::min(startPage, endPage);
    findPage = std::max(startPage, endPage);
    findIndex = GlyphToCodepoint(findPage, findPage == endPage ? endGlyph : startGlyph);
    pageText = LoadPageText(findPage, &pageTextLen, nullptr);
    forward = true;
}

static int FirstCachedMatchIndex(const Vec<u64>& positions, int startPage, bool forward) {
    int n = (int)positions.size();
    if (n == 0) {
        return -1;
    }
    if (forward) {
        for (int i = 0; i < n; i++) {
            int page = (int)(positions[i] >> 32);
            if (page >= startPage) {
                return i;
            }
        }
        for (int i = 0; i < n; i++) {
            int page = (int)(positions[i] >> 32);
            if (page < startPage) {
                return i;
            }
        }
    } else {
        for (int i = n - 1; i >= 0; i--) {
            int page = (int)(positions[i] >> 32);
            if (page <= startPage) {
                return i;
            }
        }
        for (int i = n - 1; i >= 0; i--) {
            int page = (int)(positions[i] >> 32);
            if (page > startPage) {
                return i;
            }
        }
    }
    return -1;
}

bool TextSearch::TryFindFromCachedPositions(const Vec<u64>& positions, int startPage) {
    if (!findText || findTextLen == 0 || positions.size() == 0) {
        return false;
    }
    int n = (int)positions.size();
    int idx = FirstCachedMatchIndex(positions, startPage, forward);
    if (idx < 0) {
        return false;
    }
    for (int tried = 0; tried < n; tried++) {
        u64 key = positions[idx];
        int page = (int)(key >> 32);
        int glyph = (int)(key & 0xffffffff);
        findPage = page;
        bool abortSearch = false;
        if (!LoadPageText(page, &pageTextLen, &abortSearch) || abortSearch || !pageText) {
            return false;
        }
        int offset = GlyphToCodepoint(page, glyph);
        PageAndOffset end = MatchEnd(offset);
        if (end.page > 0) {
            searchHitStartAt = page;
            StartAt(page, glyph);
            SelectUpTo(end.page, CodepointToGlyph(end.page, end.offset));
            findIndex = forward ? end.offset : offset;
            if (result.len > 0) {
                return true;
            }
        }
        if (forward) {
            idx = (idx + 1) % n;
        } else {
            idx = (idx + n - 1) % n;
        }
    }
    return false;
}

static bool IsSharpS(int c) {
    return c != 0 && FoldCaseForSearch(c) == 0x00DF;
}

static bool IsLatinS(int c) {
    return c != 0 && FoldCaseForSearch(c) == L's';
}

static bool MatchSearchUnit(const char* h, int hLen, int hByteLen, int hIdx, int hByteIdx, const char* n, int nLen,
                            int nByteLen, int nIdx, int nByteIdx, int& hAdv, int& nAdv, int& hByteAdv, int& nByteAdv) {
    hAdv = nAdv = hByteAdv = nByteAdv = 0;
    if (hIdx >= hLen || nIdx >= nLen) {
        return false;
    }
    int hNextByte = hByteIdx;
    int hc = Utf8CodepointNext(h, hByteLen, hNextByte);
    int nNextByte = nByteIdx;
    int nc = Utf8CodepointNext(n, nByteLen, nNextByte);
    if (IsSharpS(nc) && hIdx + 1 < hLen && IsLatinS(hc)) {
        int hAfterNextByte = hNextByte;
        int hNextChar = Utf8CodepointNext(h, hByteLen, hAfterNextByte);
        if (IsLatinS(hNextChar)) {
            hAdv = 2;
            nAdv = 1;
            hByteAdv = hAfterNextByte - hByteIdx;
            nByteAdv = nNextByte - nByteIdx;
            return true;
        }
    }
    if (nIdx + 1 < nLen && IsLatinS(nc) && IsSharpS(hc)) {
        int nAfterNextByte = nNextByte;
        int nNextChar = Utf8CodepointNext(n, nByteLen, nAfterNextByte);
        if (IsLatinS(nNextChar)) {
            hAdv = 1;
            nAdv = 2;
            hByteAdv = hNextByte - hByteIdx;
            nByteAdv = nAfterNextByte - nByteIdx;
            return true;
        }
    }
    if (FoldCaseForSearch(hc) == FoldCaseForSearch(nc)) {
        hAdv = 1;
        nAdv = 1;
        hByteAdv = hNextByte - hByteIdx;
        nByteAdv = nNextByte - nByteIdx;
        return true;
    }
    return false;
}

static bool IsUtf8LeadByte(unsigned char b) {
    return (b & 0xC0) != 0x80;
}

static int StrStrFoldCase(const char* haystack, int haystackByteLen, int haystackLen, int startOff, const char* needle,
                          int needleByteLen, int needleLen) {
    if (!haystack || !needle) {
        return startOff;
    }
    int nFirstByte = 0;
    int nFirst = Utf8CodepointNext(needle, needleByteLen, nFirstByte);
    int nFirstFolded = FoldCaseForSearch(nFirst);
    int byteIdx = Utf8CodepointToByteIndex(haystack, haystackByteLen, startOff);
    for (int i = startOff; i < haystackLen; i++) {
        int hFirstByte = byteIdx;
        int hFirst = Utf8CodepointNext(haystack, haystackByteLen, hFirstByte);
        if (FoldCaseForSearch(hFirst) != nFirstFolded) {
            Utf8CodepointNext(haystack, haystackByteLen, byteIdx);
            continue;
        }
        int hIdx = i;
        int hByteIdx = byteIdx;
        int nIdx = 0;
        int nByteIdx = 0;
        bool isMatch = true;
        while (nIdx < needleLen) {
            if (hIdx >= haystackLen) {
                isMatch = false;
                break;
            }
            int hAdv, nAdv, hByteAdv, nByteAdv;
            if (!MatchSearchUnit(haystack, haystackLen, haystackByteLen, hIdx, hByteIdx, needle, needleLen,
                                 needleByteLen, nIdx, nByteIdx, hAdv, nAdv, hByteAdv, nByteAdv)) {
                isMatch = false;
                break;
            }
            hIdx += hAdv;
            nIdx += nAdv;
            hByteIdx += hByteAdv;
            nByteIdx += nByteAdv;
        }
        if (isMatch) {
            return i;
        }
        Utf8CodepointNext(haystack, haystackByteLen, byteIdx);
    }
    return -1;
}

static bool StartsWithAtByte(const char* text, int textByteLen, int byteIdx, const char* prefix, int prefixByteLen) {
    return text && prefix && byteIdx >= 0 && byteIdx + prefixByteLen <= textByteLen &&
           memcmp(text + byteIdx, prefix, (size_t)prefixByteLen) == 0;
}

static int StrStr(const char* haystack, int haystackByteLen, int haystackLen, int startOff, const char* needle,
                  int needleByteLen, int needleLen) {
    if (!haystack || !needle || needleLen <= 0) {
        return -1;
    }
    int byteIdx = Utf8CodepointToByteIndex(haystack, haystackByteLen, startOff);
    if (needleByteLen > 0 && (unsigned char)needle[0] < 0x80) {
        char first = needle[0];
        while (byteIdx < haystackByteLen) {
            const char* p = (const char*)memchr(haystack + byteIdx, (unsigned char)first, haystackByteLen - byteIdx);
            if (!p) {
                return -1;
            }
            int i = Utf8CodepointCountN(haystack, (int)(p - haystack));
            if (StartsWithAtByte(haystack, haystackByteLen, (int)(p - haystack), needle, needleByteLen)) {
                return i;
            }
            byteIdx = (int)(p - haystack) + 1;
        }
        return -1;
    }
    for (int i = startOff; i <= haystackLen - needleLen; i++) {
        if (StartsWithAtByte(haystack, haystackByteLen, byteIdx, needle, needleByteLen)) {
            return i;
        }
        Utf8CodepointNext(haystack, haystackByteLen, byteIdx);
    }
    return -1;
}

static int StrRStr(const char* text, int textByteLen, int textLen, int endOff, const char* needle, int needleByteLen,
                   int needleLen) {
    if (!text || !needle || endOff <= 0 || endOff > textLen) {
        return -1;
    }
    if (needleLen <= 0 || needleLen > endOff) {
        return -1;
    }
    int result = -1;
    int byteIdx = 0;
    for (int i = 0; i <= endOff - needleLen; i++) {
        if (StartsWithAtByte(text, textByteLen, byteIdx, needle, needleByteLen)) {
            result = i;
        }
        Utf8CodepointNext(text, textByteLen, byteIdx);
    }
    return result;
}

static int StrRStrFoldCase(const char* text, int textByteLen, int textLen, int endOff, const char* needle,
                           int needleByteLen, int needleLen) {
    if (!text || !needle || endOff <= 0 || endOff > textLen) {
        return -1;
    }
    int nLastByte = 0;
    int nLastIdx = needleLen - 1;
    for (int i = 0; i < nLastIdx; i++) {
        Utf8CodepointNext(needle, needleByteLen, nLastByte);
    }
    int nLast = Utf8CodepointNext(needle, needleByteLen, nLastByte);
    int nLastFolded = FoldCaseForSearch(nLast);
    int result = -1;
    int byteIdx = 0;
    for (int i = 0; i < endOff; i++) {
        if (i + needleLen <= endOff) {
            int walkByte = byteIdx;
            for (int j = 0; j < needleLen - 1; j++) {
                Utf8CodepointNext(text, textByteLen, walkByte);
            }
            int hLast = Utf8CodepointNext(text, textByteLen, walkByte);
            if (FoldCaseForSearch(hLast) == nLastFolded) {
                int hIdx = i;
                int hByteIdx = byteIdx;
                int nIdx = 0;
                int nByteIdx = 0;
                bool isMatch = true;
                while (nIdx < needleLen) {
                    if (hIdx >= endOff) {
                        isMatch = false;
                        break;
                    }
                    int hAdv, nAdv, hByteAdv, nByteAdv;
                    if (!MatchSearchUnit(text, textLen, textByteLen, hIdx, hByteIdx, needle, needleLen, needleByteLen,
                                         nIdx, nByteIdx, hAdv, nAdv, hByteAdv, nByteAdv)) {
                        isMatch = false;
                        break;
                    }
                    hIdx += hAdv;
                    nIdx += nAdv;
                    hByteIdx += hByteAdv;
                    nByteIdx += nByteAdv;
                }
                if (isMatch) {
                    result = i;
                }
            }
        }
        Utf8CodepointNext(text, textByteLen, byteIdx);
    }
    return result;
}

static int StrStrUtf8Anchor(const char* haystack, int haystackByteLen, int haystackLen, int startOff,
                            const char* needle, int needleByteLen, int needleLen) {
    if (!haystack || !needle || needleLen <= 0 || needleByteLen <= 0) {
        return -1;
    }
    int byteIdx = Utf8CodepointToByteIndex(haystack, haystackByteLen, startOff);
    unsigned char first = (unsigned char)needle[0];
    while (byteIdx <= haystackByteLen - needleByteLen) {
        const char* scan = haystack + byteIdx;
        const char* limit = haystack + haystackByteLen - needleByteLen + 1;
        while (scan < limit) {
            scan = (const char*)memchr(scan, first, (size_t)(limit - scan));
            if (!scan) {
                break;
            }
            if (IsUtf8LeadByte((unsigned char)scan[0]) && memcmp(scan, needle, (size_t)needleByteLen) == 0) {
                return Utf8CodepointCountN(haystack, (int)(scan - haystack));
            }
            scan++;
        }
        int next = byteIdx;
        Utf8CodepointNext(haystack, haystackByteLen, next);
        if (next <= byteIdx) {
            break;
        }
        byteIdx = next;
    }
    return -1;
}

static int StrRStrUtf8Anchor(const char* text, int textByteLen, int textLen, int endOff, const char* needle,
                             int needleByteLen, int needleLen) {
    if (!text || !needle || endOff <= 0 || needleLen <= 0 || needleByteLen <= 0) {
        return -1;
    }
    int result = -1;
    int byteIdx = 0;
    for (int i = 0; i < endOff; i++) {
        if (IsUtf8LeadByte((unsigned char)text[byteIdx]) && byteIdx + needleByteLen <= textByteLen &&
            memcmp(text + byteIdx, needle, (size_t)needleByteLen) == 0) {
            result = i;
        }
        Utf8CodepointNext(text, textByteLen, byteIdx);
    }
    return result;
}

TextSearch::PageAndOffset TextSearch::MatchEnd(int startOff, bool* needsMorePages) const {
    const PageAndOffset notFound = {-1, -1};
    int currentPage = findPage;
    const char* currentPageText = pageText;
    int currentPageTextLen = pageTextLen;
    int currentPageTextByteLen = TextByteLen(currentPageText);
    int findTextByteLen = TextByteLen(findText);
    bool lookingAtWs;

    if (needsMorePages) {
        *needsMorePages = false;
    }
    if (!findText) {
        return notFound;
    }

    int matchIdx = 0;
    int matchByteIdx = 0;
    int endIdx = startOff;
    int endByteIdx = Utf8CodepointToByteIndex(currentPageText, currentPageTextByteLen, endIdx);

    if (matchWordStart && startOff > 0) {
        int prevByteIdx = endByteIdx;
        int prevCh = Utf8CodepointPrev(pageText, currentPageTextByteLen, prevByteIdx);
        int nextByteIdx = endByteIdx;
        int curCh = Utf8CodepointNext(pageText, currentPageTextByteLen, nextByteIdx);
        if (isWordChar(prevCh) && isWordChar(curCh)) {
            return notFound;
        }
    }

    while (matchIdx < findTextLen) {
        bool atPageEnd = endIdx >= currentPageTextLen;
        if (atPageEnd && currentPage >= nPages) {
            if (needsMorePages) {
                *needsMorePages = true;
            }
            return notFound;
        }
        int endNextByteIdx = endByteIdx;
        int endCh = atPageEnd ? 0 : Utf8CodepointNext(currentPageText, currentPageTextByteLen, endNextByteIdx);
        lookingAtWs = (atPageEnd && (currentPage < nPages)) || str::IsWs((char)endCh);
        bool isMatch = false;
        int extraMatchAdv = 0;
        int extraEndAdv = 0;
        int matchNextByteIdx = matchByteIdx;
        int matchCh = Utf8CodepointNext(findText, findTextByteLen, matchNextByteIdx);
        if (matchCase) {
            isMatch = matchCh == endCh;
        } else {
            isMatch = FoldCaseForSearch(matchCh) == FoldCaseForSearch(endCh);
            if (!isMatch) {
                if (IsSharpS(matchCh) && !atPageEnd && endIdx + 1 < currentPageTextLen && IsLatinS(endCh)) {
                    int endAfterNextByteIdx = endNextByteIdx;
                    int nextEndCh = Utf8CodepointNext(currentPageText, currentPageTextByteLen, endAfterNextByteIdx);
                    if (IsLatinS(nextEndCh)) {
                        isMatch = true;
                        extraEndAdv = 1;
                        endNextByteIdx = endAfterNextByteIdx;
                    }
                } else if (matchIdx + 1 < findTextLen && IsLatinS(matchCh) && IsSharpS(endCh)) {
                    int matchAfterNextByteIdx = matchNextByteIdx;
                    int nextMatchCh = Utf8CodepointNext(findText, findTextByteLen, matchAfterNextByteIdx);
                    if (IsLatinS(nextMatchCh)) {
                        isMatch = true;
                        extraMatchAdv = 1;
                        matchNextByteIdx = matchAfterNextByteIdx;
                    }
                }
            }
        }
        if (isMatch) {
            ;
        } else if (str::IsWs((char)matchCh) && lookingAtWs) {
            ;
        } else if (matchCh == '-' && (0x2010 <= endCh && endCh <= 0x2014)) {
            ;
        } else if (matchCh == '\'' && (0x2018 <= endCh && endCh <= 0x201b)) {
            ;
        } else if (matchCh == '"' && (0x201c <= endCh && endCh <= 0x201f)) {
            ;
        } else {
            return notFound;
        }
        int matchAdv = 1 + extraMatchAdv;
        matchByteIdx = matchNextByteIdx;
        matchIdx += matchAdv;
        if (!atPageEnd && endCh) {
            int endAdv = 1 + extraEndAdv;
            endByteIdx = endNextByteIdx;
            endIdx += endAdv;
        } else {
            ++currentPage;
            bool abortSearch = false;
            currentPageText =
                GetTextForPageUtf8ForSearch(engine, currentPage, &currentPageTextLen, progressCb, &abortSearch);
            if (abortSearch) {
                return notFound;
            }
            currentPageTextByteLen = TextByteLen(currentPageText);
            endIdx = 0;
            endByteIdx = 0;
        }
        int prevMatchByteIdx = matchByteIdx;
        int prevMatchCh = Utf8CodepointPrev(findText, findTextByteLen, prevMatchByteIdx);
        int curMatchCh = Utf8CodepointAtByte(findText, findTextByteLen, matchByteIdx);
        if (matchIdx < findTextLen && ((!isnoncjkwordchar(prevMatchCh) && (prevMatchCh != '?' || curMatchCh != '?')) ||
                                       (lookingAtWs && str::IsWs((char)prevMatchCh)))) {
            SkipWhitespace(findText, findTextLen, matchIdx, matchByteIdx);
            SkipWhitespace(currentPageText, currentPageTextLen, endIdx, endByteIdx);
            while (endIdx >= currentPageTextLen && currentPage < nPages) {
                ++currentPage;
                bool abortSearch = false;
                currentPageText =
                    GetTextForPageUtf8ForSearch(engine, currentPage, &currentPageTextLen, progressCb, &abortSearch);
                if (abortSearch) {
                    return notFound;
                }
                currentPageTextByteLen = TextByteLen(currentPageText);
                endIdx = 0;
                endByteIdx = 0;
                SkipWhitespace(currentPageText, currentPageTextLen, endIdx, endByteIdx);
            }
        }
    }
    if (matchWordEnd && endIdx > 0 && endIdx < currentPageTextLen) {
        int prevByteIdx = endByteIdx;
        int prevCh = Utf8CodepointPrev(currentPageText, currentPageTextByteLen, prevByteIdx);
        int nextByteIdx = endByteIdx;
        int curCh = Utf8CodepointNext(currentPageText, currentPageTextByteLen, nextByteIdx);
        if (isWordChar(prevCh) && isWordChar(curCh)) {
            return notFound;
        }
    }

    return {currentPage, endIdx};
}

static int GetNextIndex(int textLen, int offset, bool forward) {
    int idx = offset + (forward ? 0 : -1);
    if (idx < 0 || idx >= textLen) {
        return -1;
    }
    return idx;
}

static int FindAnchorInPage(const char* pageText, int pageTextByteLen, int pageTextLen, int startOff,
                            const char* anchor, int anchorByteLen, int anchorLen, bool matchCase, bool forward,
                            int endOff = -1) {
    if (!anchor) {
        return GetNextIndex(pageTextLen, startOff, forward);
    }
    if (AnchorSupportsUtf8ByteSearch(anchor, anchorByteLen, matchCase)) {
        if (forward) {
            return StrStrUtf8Anchor(pageText, pageTextByteLen, pageTextLen, startOff, anchor, anchorByteLen, anchorLen);
        }
        if (endOff < 0) {
            endOff = startOff;
        }
        return StrRStrUtf8Anchor(pageText, pageTextByteLen, pageTextLen, endOff, anchor, anchorByteLen, anchorLen);
    }
    if (forward) {
        if (matchCase) {
            return StrStr(pageText, pageTextByteLen, pageTextLen, startOff, anchor, anchorByteLen, anchorLen);
        }
        return StrStrFoldCase(pageText, pageTextByteLen, pageTextLen, startOff, anchor, anchorByteLen, anchorLen);
    }
    if (endOff < 0) {
        endOff = startOff;
    }
    if (matchCase) {
        return StrRStr(pageText, pageTextByteLen, pageTextLen, endOff, anchor, anchorByteLen, anchorLen);
    }
    return StrRStrFoldCase(pageText, pageTextByteLen, pageTextLen, endOff, anchor, anchorByteLen, anchorLen);
}

void TextSearch::CollectMatchesOnPage(int pageNo, Vec<MatchSpan>* out, int* continuationPage) {
    if (!out || !findText || findTextLen == 0) {
        return;
    }
    if (pageNo < 1 || pageNo > nPages) {
        return;
    }

    EnsurePageMatchCacheSize();
    if (TryGetCachedPageMatches(pageNo, out)) {
        return;
    }

    if (!PageMightContainAnchor(pageNo)) {
        MarkSkippedPageMatchCache(this, pageNo);
        return;
    }

    findPage = pageNo;
    bool abortSearch = false;
    if (!LoadPageText(pageNo, &pageTextLen, &abortSearch)) {
        if (abortSearch || !pageText) {
            MarkSkippedPageMatchCache(this, pageNo);
        }
        return;
    }

    int pageTextByteLen = TextByteLen(pageText);
    int anchorByteLen = TextByteLen(anchor);
    int idx = 0;
    bool pageNeedsMorePages = false;
    Vec<MatchSpan> pageSpans;
    while (idx <= pageTextLen) {
        if (WasCanceled(progressCb)) {
            return;
        }

        int found = FindAnchorInPage(pageText, pageTextByteLen, pageTextLen, idx, anchor, anchorByteLen, anchorLen,
                                     matchCase, true);
        if (found < 0) {
            break;
        }

        if (QuickRejectWholeWordMatch(pageText, pageTextByteLen, pageTextLen, found, findTextLen, matchWordStart,
                                      matchWordEnd)) {
            idx = found + 1;
            continue;
        }

        findPage = pageNo;
        bool needsMorePages = false;
        PageAndOffset fg = MatchEnd(found, &needsMorePages);
        pageNeedsMorePages = pageNeedsMorePages || needsMorePages;
        if (needsMorePages && continuationPage && (*continuationPage <= 0 || pageNo < *continuationPage)) {
            *continuationPage = pageNo;
        }
        if (fg.page > 0 && fg.offset > found) {
            MatchSpan ms;
            ms.startPage = pageNo;
            ms.startGlyph = CodepointToGlyph(pageNo, found);
            ms.endPage = fg.page;
            ms.endGlyph = CodepointToGlyph(fg.page, fg.offset);
            int wlen = 0;
            engine->GetTextForPage(pageNo, &wlen);
            bool valid = ms.startGlyph >= 0 && ms.endGlyph > ms.startGlyph;
            if (ms.endPage == pageNo) {
                valid = valid && ms.endGlyph <= wlen;
            } else {
                int wlenEnd = 0;
                engine->GetTextForPage(ms.endPage, &wlenEnd);
                valid = valid && ms.endGlyph <= wlenEnd;
            }
            if (valid) {
                pageSpans.Append(ms);
                idx = fg.page == pageNo ? fg.offset : found + 1;
                continue;
            }
        }
        idx = found + 1;
    }

    // A candidate that ran into the current progressive frontier must be
    // reconsidered after more pages arrive. Do not cache this page as final.
    if (!pageNeedsMorePages) {
        SetPageMatchCache(pageNo, pageSpans);
    }
    for (int i = 0; i < (int)pageSpans.size(); i++) {
        out->Append(pageSpans[i]);
    }
}

bool TextSearch::FindTextInPage(int pageNo, TextSearch::PageAndOffset* finalGlyph) {
    if (!findText || findTextLen == 0) {
        return false;
    }
    if (!pageNo) {
        pageNo = findPage;
    }
    findPage = pageNo;

    int pageTextByteLen = TextByteLen(pageText);
    int anchorByteLen = TextByteLen(anchor);
    int found = -1;
    PageAndOffset fg;
    do {
        if (WasCanceled(progressCb)) {
            return false;
        }
        found = FindAnchorInPage(pageText, pageTextByteLen, pageTextLen, findIndex, anchor, anchorByteLen, anchorLen,
                                 matchCase, forward, findIndex);
        if (found < 0) {
            return false;
        }
        if (QuickRejectWholeWordMatch(pageText, pageTextByteLen, pageTextLen, found, findTextLen, matchWordStart,
                                      matchWordEnd)) {
            findIndex = found + (forward ? 1 : 0);
            continue;
        }
        findIndex = found + (forward ? 1 : 0);
        fg = MatchEnd(found);
    } while (fg.page <= 0);

    int offset = found;
    searchHitStartAt = pageNo;
    StartAt(pageNo, CodepointToGlyph(pageNo, offset));
    SelectUpTo(fg.page, CodepointToGlyph(fg.page, fg.offset));
    // MatchEnd() can finish on a later page. Keep forward navigation on the
    // start page in that case so another valid start inside this page is not
    // skipped merely because its range overlaps the cross-page result.
    findIndex = forward ? (fg.page == pageNo ? fg.offset : offset + 1) : offset;

    if (result.len == 0) {
        return FindTextInPage(pageNo, finalGlyph);
    }

    if (finalGlyph) {
        *finalGlyph = fg;
    }
    return true;
}

bool TextSearch::FindStartingAtPage(int pageNo) {
    if (!findText || findTextLen == 0) {
        return false;
    }

    SyncPageCount();

    int next = forward ? 1 : -1;
    while ((1 <= pageNo) && (pageNo <= nPages) && !WasCanceled(progressCb)) {
        UpdateProgress(progressCb, pageNo, nPages);

        if (pagesToSkip[pageNo - 1]) {
            pageNo += next;
            continue;
        }

        if (!PageMightContainAnchor(pageNo)) {
            pagesToSkip[pageNo - 1] = true;
            EnsurePageMatchCacheSize();
            MarkSkippedPageMatchCache(this, pageNo);
            pageNo += next;
            continue;
        }

        Reset();

        bool abortSearch = false;
        if (!LoadPageText(pageNo, &pageTextLen, &abortSearch)) {
            if (abortSearch) {
                break;
            }
        } else if (pageText) {
            if (forward) {
                findIndex = 0;
            } else {
                findIndex = pageTextLen;
            }
            PageAndOffset r;
            if (FindTextInPage(pageNo, &r)) {
                int selStartPage = startPage;
                int selStartGlyph = startGlyph;
                int selEndPage = endPage;
                int selEndGlyph = endGlyph;

                EnsurePageMatchCacheSize();
                if (!pageMatchesCached[pageNo - 1]) {
                    Vec<MatchSpan> discard;
                    CollectMatchesOnPage(pageNo, &discard);
                    StartAt(selStartPage, selStartGlyph);
                    SelectUpTo(selEndPage, selEndGlyph);
                }

                return true;
            }
            pagesToSkip[pageNo - 1] = true;
            MarkSkippedPageMatchCache(this, pageNo);
        }

        pageNo += next;
    }

    searchHitStartAt = findPage = forward ? nPages + 1 : 0;

    return false;
}

TextSel* TextSearch::FindFirst(int page, const WCHAR* text) {
    SetText(text);

    if (FindStartingAtPage(page)) {
        return &result;
    }
    return nullptr;
}

TextSel* TextSearch::FindNext() {
    ReportIf(!findText);
    if (!findText) {
        return nullptr;
    }

    if (WasCanceled(progressCb)) {
        return nullptr;
    }
    int currentPage = startPage;
    int currentGlyph = startGlyph;
    if (currentPage >= 1 && currentPage <= nPages && currentGlyph >= 0) {
        Vec<MatchSpan> spans;
        CollectMatchesOnPage(currentPage, &spans);
        int currentIdx = -1;
        for (int i = 0; i < (int)spans.size(); i++) {
            if (spans[i].startGlyph == currentGlyph) {
                currentIdx = i;
                break;
            }
        }
        int targetIdx = currentIdx + (forward ? 1 : -1);
        if (currentIdx >= 0 && targetIdx >= 0 && targetIdx < (int)spans.size()) {
            const MatchSpan& ms = spans[targetIdx];
            Reset();
            StartAt(ms.startPage, ms.startGlyph);
            SelectUpTo(ms.endPage, ms.endGlyph);
            searchHitStartAt = findPage = ms.startPage;
            bool abortSearch = false;
            LoadPageText(findPage, &pageTextLen, &abortSearch);
            findIndex = GlyphToCodepoint(findPage, ms.startGlyph);
            return &result;
        }
    }

    int nextPage = currentPage + (forward ? 1 : -1);
    if (FindStartingAtPage(nextPage)) {
        return &result;
    }
    return nullptr;
}
