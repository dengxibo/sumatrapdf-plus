/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/GdiPlusUtil.h"
#include "utils/WinUtil.h"
#include "utils/Archive.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "mui/Mui.h"

#include "wingui/UIModels.h"

#include "DocProperties.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EbookBase.h"
#include "EbookTypography.h"
#include "EbookDoc.h"
#include "PalmDbReader.h"
#include "MobiDoc.h"
#include "HtmlFormatter.h"
#include "EbookFormatter.h"
#include "utils/Log.h"

/* Mobi-specific formatting methods */

static bool TextContainsUtf8(const char* s, size_t sLen, const char* needle) {
    char* buf = str::DupTemp(s, sLen);
    return buf && strstr(buf, needle);
}

// kindle:embed:000H?mime=image/jpeg (KF8/AZW3)
static bool ParseKindleEmbedResourceIndex(const char* s, size_t sLen, size_t* indexOut) {
    const char* prefix = "kindle:embed:";
    size_t prefixLen = 13;
    if (sLen < prefixLen + 1 || !str::EqN(s, prefix, prefixLen)) {
        return false;
    }
    const char* p = s + prefixLen;
    size_t rem = sLen - prefixLen;
    size_t n = 0;
    while (n < rem) {
        char c = p[n];
        if (c == '?' || c == '"' || c == '\'' || c == ' ' || c == '\t') {
            break;
        }
        bool isDigit = c >= '0' && c <= '9';
        bool isUpper = c >= 'A' && c <= 'V';
        bool isLower = c >= 'a' && c <= 'v';
        if (!isDigit && !isUpper && !isLower) {
            break;
        }
        n++;
    }
    if (0 == n) {
        return false;
    }
    u64 val = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        int d = -1;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'A' && c <= 'V') {
            d = c - 'A' + 10;
        } else if (c >= 'a' && c <= 'v') {
            d = c - 'a' + 10;
        } else {
            return false;
        }
        val = val * 32 + (u64)d;
    }
    *indexOut = (size_t)val;
    return true;
}

static bool MobiHtmlHasEarlyCoverImage(ByteSlice html);

MobiFormatter::MobiFormatter(HtmlFormatterArgs* args, MobiDoc* doc, EbookTypographyKind typographyKind,
                             bool readerStyle)
    : HtmlFormatter(args), doc(doc), typographyKind(typographyKind), readerStyle(readerStyle) {
    // MOBI/AZW3 reads better left-aligned; justify widens CJK lines awkwardly
    styleStack.Last().align = AlignAttr::Left;
    nextPageStyle.align = AlignAttr::Left;
    if (readerStyle) {
        if (typographyKind == EbookTypographyKind::Cjk) {
            lineSpacing *= 1.14f;
        } else if (typographyKind == EbookTypographyKind::Bilingual) {
            lineSpacing *= 1.1f;
        } else {
            lineSpacing *= 1.04f;
        }
    }

    bool fromBeginning = (0 == args->reparseIdx);
    if (!doc || !fromBeginning) {
        return;
    }

    ByteSlice* img = doc->GetCoverImage();
    if (!img) {
        return;
    }
    if (MobiHtmlHasEarlyCoverImage(doc->GetHtmlData())) {
        return;
    }

    // Scale metadata cover to fill most of the page (EXTH cover is often a low-res thumbnail).
    SizeF coverMax{pageDx * 0.7f, pageDy * 0.7f};
    EmitImage(img, &coverMax, true);
    // only add a new page if the image isn't broken
    if (currLineInstr.size() > 0) {
        injectedExthCover = true;
        ForceNewPage();
    }
}

void MobiFormatter::UpdateTocState() {
    if (!doc) {
        return;
    }
    if (tocStartIdx == (size_t)-1) {
        if (doc->HasToc()) {
            tocStartIdx = doc->GetTocFilePos();
        } else {
            tocStartIdx = (size_t)-2;
        }
    }
    if (tocStartIdx != (size_t)-1 && tocStartIdx != (size_t)-2 && currReparseIdx >= (ptrdiff_t)tocStartIdx) {
        inTocRegion = true;
    }
}

static bool IsTrimmedUtf8(const char* s, size_t sLen, const char* utf8) {
    char* buf = str::DupTemp(s, sLen);
    str::TrimWSInPlace(buf, str::TrimOpt::Both);
    return str::Eq(buf, utf8);
}

static bool IsTocTitleText(const char* s, size_t sLen) {
    if (IsTrimmedUtf8(s, sLen, "\xe7\x9b\xae\xe5\xbd\x95")) {
        return true;
    }
    char* buf = str::DupTemp(s, sLen);
    str::TrimWSInPlace(buf, str::TrimOpt::Both);
    WCHAR* w = ToWStrTemp(buf);
    return w && str::Eq(w, L"目录");
}

static bool IsDecorativeLineText(const char* s, size_t sLen) {
    if (sLen < 3) {
        return false;
    }
    WCHAR* w = ToWStrTemp(s, sLen);
    if (!w) {
        return false;
    }
    size_t n = 0;
    for (const WCHAR* p = w; *p; p++) {
        WCHAR c = *p;
        if (c == L'-' || c == L'_' || c == L'=' || c == L'~' || c == L'·' || c == L'—' || c == L'─' || c == L'━' ||
            c == L' ' || c == L'\t') {
            n++;
            continue;
        }
        return false;
    }
    return n >= 3;
}

void MobiFormatter::StartTocPage() {
    if (tocPageBreakDone) {
        return;
    }
    FlushCurrLine(true);
    UpdateLinkBboxes(currPage);
    pagesToSend.Append(currPage);
    EmitNewPage();
    currX = NewLineX();
    currLineTopPadding = 0.f;
    tocPageBreakDone = true;
    inToc = true;
    blockquoteDepth = 0;
    listDepth = 0;
}

bool MobiFormatter::BeforeTextRun(const char* s, size_t sLen) {
    UpdateTocState();
    if (InTocLikeRegion() && IsDecorativeLineText(s, sLen)) {
        return false;
    }
    if (!tocPageBreakDone) {
        bool isPhone = TextContainsUtf8(s, sLen, "\xe5\xae\xa2\xe6\x9c\x8d"); // 客服
        bool isTocTitle = IsTocTitleText(s, sLen);
        if (isPhone || TextContainsUtf8(s, sLen, "\xe5\xae\xa2\xe6\x9c\x8d\xe7\x94\xb5\xe8\xae\xb1")) {
            sawColophonPhone = true;
        }
        if (IsTrimmedUtf8(s, sLen, "\xe7\x9b\xae")) { // 目
            if (sawColophonPhone || inTocRegion) {
                StartTocPage();
            }
            pendingMu = true;
        } else if (pendingMu && IsTrimmedUtf8(s, sLen, "\xe5\xbd\x95")) { // 录
            pendingMu = false;
            if (!tocPageBreakDone) {
                StartTocPage();
            }
        } else {
            pendingMu = false;
        }
        if (isTocTitle) {
            StartTocPage();
        }
    }
    return true;
}

void MobiFormatter::OnParserProgress() {
    UpdateTocState();
}

float MobiFormatter::ListIndentDx() const {
    float em = defaultFontSize > 0 ? defaultFontSize : 12.f;
    if (inToc) {
        return (float)blockquoteDepth * em * 1.25f;
    }
    return (float)listDepth * em * 0.75f + (float)blockquoteDepth * em * 0.5f;
}

float MobiFormatter::ExtraParagraphDy() {
    float em = defaultFontSize > 0 ? defaultFontSize : 12.f;
    if (inToc) {
        return 0.08f * em;
    }
    if (readerStyle) {
        if (typographyKind == EbookTypographyKind::Cjk) {
            return 0.08f * em;
        }
        if (typographyKind == EbookTypographyKind::Bilingual) {
            return 0.1f * em;
        }
        return 0.12f * em;
    }
    // metadata/colophon pages use many short <p> tags; keep gaps tight
    return 0.15f * em;
}

void MobiFormatter::HandleTagBlockquote(HtmlToken* t) {
    if (t->IsStartTag()) {
        FlushCurrLine(true);
        blockquoteDepth++;
    } else if (t->IsEndTag() && blockquoteDepth > 0) {
        FlushCurrLine(true);
        blockquoteDepth--;
    }
    currX = NewLineX();
}

// parses size in the form "1em" or "3pt". To interpret ems we need emInPoints
// to be passed by the caller
static float ParseSizeAsPixels(const char* s, size_t len, float emInPoints) {
    float sizeInPoints = 0;
    if (str::Parse(s, len, "%fem", &sizeInPoints)) {
        sizeInPoints *= emInPoints;
    } else if (str::Parse(s, len, "%fin", &sizeInPoints)) {
        sizeInPoints *= 72;
    } else if (str::Parse(s, len, "%fpt", &sizeInPoints)) {
        // no conversion needed
    } else if (str::Parse(s, len, "%fpx", &sizeInPoints)) {
        return sizeInPoints;
    } else {
        return 0;
    }
    // TODO: take dpi into account
    float sizeInPixels = sizeInPoints;
    return sizeInPixels;
}

void MobiFormatter::HandleSpacing_Mobi(HtmlToken* t) {
    if (!t->IsStartTag()) {
        return;
    }

    // best I can tell, in mobi <p width="1em" height="3pt> means that
    // the first line of the paragrap is indented by 1em and there's
    // 3pt top padding (the same seems to apply for <blockquote>)
    AttrInfo* attr = t->GetAttrByName("width");
    if (attr && !inToc) {
        float lineIndent = ParseSizeAsPixels(attr->val, attr->valLen, CurrFont()->GetSize());
        // there are files with negative width which produces partially invisible
        // text, so don't allow that
        if (lineIndent > 0) {
            // this should replace the previously emitted paragraph/quote block
            EmitParagraph(lineIndent);
        }
    }
    attr = t->GetAttrByName("height");
    if (attr) {
        // for use it in FlushCurrLine()
        currLineTopPadding = ParseSizeAsPixels(attr->val, attr->valLen, CurrFont()->GetSize());
    }
}

static bool ClassAttrContains(const char* val, size_t valLen, const char* needle) {
    char* buf = str::DupTemp(val, valLen);
    return buf && strstr(buf, needle);
}

static bool MobiHtmlHasEarlyCoverImage(ByteSlice html) {
    if (html.empty()) {
        return false;
    }
    size_t scanLen = std::min(html.size(), (size_t)8192);
    char* buf = str::DupTemp((const char*)html.data(), scanLen);
    if (!buf) {
        return false;
    }
    char* pageBreak = strstr(buf, "mbp:pagebreak");
    if (pageBreak) {
        *pageBreak = 0;
    }
    if (!strstr(buf, "<img") && !strstr(buf, "<IMG")) {
        return false;
    }
    return strstr(buf, "recindex") || strstr(buf, "kindle:embed");
}

static ByteSlice* MobiImageFromToken(MobiDoc* doc, HtmlToken* t) {
    if (!doc || !t) {
        return nullptr;
    }
    AttrInfo* attr = t->GetAttrByName("recindex");
    if (attr) {
        int n;
        if (str::Parse(attr->val, attr->valLen, "%d", &n)) {
            return doc->GetImage(n);
        }
    }
    attr = t->GetAttrByName("src");
    if (attr) {
        size_t resourceIndex = 0;
        if (ParseKindleEmbedResourceIndex(attr->val, attr->valLen, &resourceIndex)) {
            return doc->GetImageByResourceIndex(resourceIndex);
        }
    }
    return nullptr;
}

static bool IsInlineMobiImage(HtmlToken* t, MobiDoc* doc) {
    AttrInfo* attr = t->GetAttrByName("class");
    if (attr && ClassAttrContains(attr->val, attr->valLen, "inline")) {
        return true;
    }
    // Classic MOBI embeds small in-text images via recindex; KF8 chapter art uses kindle:embed
    if (t->GetAttrByName("recindex") == nullptr) {
        return false;
    }
    // Some MOBI books use recindex for full-size illustrations with explicit dimensions
    int w = 0, h = 0;
    attr = t->GetAttrByName("width");
    if (attr) {
        str::Parse(attr->val, attr->valLen, "%d", &w);
    }
    attr = t->GetAttrByName("height");
    if (attr) {
        str::Parse(attr->val, attr->valLen, "%d", &h);
    }
    if (w > 64 || h > 64) {
        return false;
    }
    ByteSlice* img = MobiImageFromToken(doc, t);
    if (img) {
        Size imgSize = ImageSizeFromData(*img);
        if (imgSize.dx > 64 || imgSize.dy > 64) {
            return false;
        }
    }
    return true;
}

static bool IsEarlyMobiCoverImage(HtmlToken* t, MobiDoc* doc, ptrdiff_t reparseIdx) {
    if (!t || !t->IsStartTag() || reparseIdx < 0 || reparseIdx > 4096) {
        return false;
    }
    if (IsInlineMobiImage(t, doc)) {
        return false;
    }
    return MobiImageFromToken(doc, t) != nullptr;
}

SizeF MobiFormatter::MaxImageSize(HtmlToken* t) {
    float em = CurrFont()->GetSize();
    if (em <= 0) {
        em = 12.f;
    }
    if (IsInlineMobiImage(t, doc)) {
        // KF8 heading icons (class="inline1") and classic MOBI recindex images sit beside text
        return {2.5f * em, 2.f * em};
    }
    if (IsEarlyMobiCoverImage(t, doc, currReparseIdx)) {
        return {pageDx * 0.7f, pageDy * 0.7f};
    }
    if (readerStyle) {
        return {pageDx * 0.84f, pageDy * 0.55f};
    }
    // Block illustrations (e.g. 700x1027 chapter art): well below full page
    return {pageDx * 0.38f, pageDy * 0.22f};
}

// mobi format has image tags in the form:
// <img recindex="0000n" alt=""/>
// where recindex is the record number of pdb record
// that holds the image (within image record array, not a
// global record)
void MobiFormatter::HandleTagImg(HtmlToken* t) {
    // we allow formatting raw html which can't require doc
    if (!doc || !t->IsStartTag()) {
        return;
    }
    if (injectedExthCover && IsEarlyMobiCoverImage(t, doc, currReparseIdx)) {
        injectedExthCover = false;
        return;
    }
    bool needAlt = true;
    ByteSlice* img = MobiImageFromToken(doc, t);
    if (img) {
        SizeF maxSize = MaxImageSize(t);
        bool center = !IsInlineMobiImage(t, doc);
        needAlt = !EmitImage(img, &maxSize, center);
    }
    AttrInfo* attr = t->GetAttrByName("alt");
    if (needAlt && attr != nullptr) {
        HandleText(attr->val, attr->valLen);
    }
}

void MobiFormatter::HandleHtmlTag(HtmlToken* t) {
    ReportIf(!t->IsTag());
    UpdateTocState();

    if (Tag_Blockquote == t->tag) {
        UpdateTagNesting(t);
        HandleTagBlockquote(t);
        HandleSpacing_Mobi(t);
    } else if (Tag_P == t->tag) {
        if (!tocPageBreakDone && t->IsStartTag()) {
            AttrInfo* attr = t->GetAttrByName("align");
            if (attr && FindAlignAttr(attr->val, attr->valLen) == AlignAttr::Center) {
                if (sawColophonPhone || inTocRegion) {
                    StartTocPage();
                }
            }
        }
        HtmlFormatter::HandleHtmlTag(t);
        HandleSpacing_Mobi(t);
    } else if (Tag_Center == t->tag) {
        if (!tocPageBreakDone && t->IsStartTag() && (sawColophonPhone || inTocRegion)) {
            StartTocPage();
        }
        HtmlFormatter::HandleHtmlTag(t);
    } else if (Tag_H1 == t->tag || Tag_H2 == t->tag || Tag_H3 == t->tag) {
        UpdateTagNesting(t);
        HandleTagHx(t);
        if (inToc && t->IsStartTag()) {
            CurrStyle()->align = AlignAttr::Center;
        }
    } else if (Tag_Mbp_Pagebreak == t->tag) {
        inToc = false;
        inTocRegion = false;
        sawColophonPhone = false;
        pendingMu = false;
        ForceNewPage();
    } else if (Tag_A == t->tag) {
        HandleAnchorAttr(t);
        // handle internal and external links (prefer internal ones)
        if (!HandleTagA(t, "filepos")) {
            HandleTagA(t);
        }
    } else if (Tag_Hr == t->tag) {
        if (!InTocLikeRegion()) {
            // imitating Kindle: hr is proceeded by an empty line
            FlushCurrLine(false);
            EmitEmptyLine(lineSpacing);
            EmitHr();
        }
    } else if (Tag_Image == t->tag) {
        HandleTagImg(t);
    } else {
        HtmlFormatter::HandleHtmlTag(t);
    }
}

/* EPUB-specific formatting methods */

void EpubFormatter::HandleTagImg(HtmlToken* t) {
    ReportIf(!epubDoc);
    if (t->IsEndTag()) {
        return;
    }
    bool needAlt = true;
    AttrInfo* attr = t->GetAttrByName("src");
    if (attr) {
        TempStr src = str::DupTemp(attr->val, attr->valLen);
        url::DecodeInPlace(src);
        ByteSlice* img = epubDoc->GetImageData(src, pagePath);
        needAlt = !img || !EmitImage(img, nullptr, true);
    }
    if (needAlt && (attr = t->GetAttrByName("alt")) != nullptr) {
        HandleText(attr->val, attr->valLen);
    }
}

void EpubFormatter::HandleTagPagebreak(HtmlToken* t) {
    AttrInfo* attr = t->GetAttrByName("page_path");
    if (!attr || pagePath) {
        ForceNewPage();
    }
    if (attr) {
        Gdiplus::RectF bbox(0, currY, pageDx, 0);
        currPage->instructions.Append(DrawInstr::Anchor(attr->val, attr->valLen, bbox));
        pagePath.Set(str::Dup(attr->val, attr->valLen));
        // reset CSS style rules for the new document
        styleRules.Reset();
    }
}

void EpubFormatter::HandleTagLink(HtmlToken* t) {
    ReportIf(!epubDoc);
    if (t->IsEndTag()) {
        return;
    }
    AttrInfo* attr = t->GetAttrByName("rel");
    if (!attr || !attr->ValIs("stylesheet")) {
        return;
    }
    attr = t->GetAttrByName("type");
    if (attr && !attr->ValIs("text/css")) {
        return;
    }
    attr = t->GetAttrByName("href");
    if (!attr) {
        return;
    }

    char* src = str::DupTemp(attr->val, attr->valLen);
    url::DecodeInPlace(src);
    ByteSlice data = epubDoc->GetFileData(src, pagePath);
    if (data) {
        ParseStyleSheet(data, data.size());
        data.Free();
    }
}

void EpubFormatter::HandleTagSvgImage(HtmlToken* t) {
    ReportIf(!epubDoc);
    if (t->IsEndTag()) {
        return;
    }
    if (!tagNesting.Contains(Tag_Svg) && Tag_Svg_Image != t->tag) {
        return;
    }
    AttrInfo* attr = t->GetAttrByNameNS("href", "http://www.w3.org/1999/xlink");
    if (!attr) {
        return;
    }
    TempStr src = str::DupTemp(attr->val, attr->valLen);
    url::DecodeInPlace(src);
    ByteSlice* img = epubDoc->GetImageData(src, pagePath);
    if (img) {
        EmitImage(img, nullptr, true);
    }
}

void EpubFormatter::HandleHtmlTag(HtmlToken* t) {
    ReportIf(!t->IsTag());
    if (hiddenDepth && t->IsEndTag() && tagNesting.size() == hiddenDepth && t->tag == tagNesting.Last()) {
        hiddenDepth = 0;
        UpdateTagNesting(t);
        return;
    }
    if (0 == hiddenDepth && t->IsStartTag() && t->GetAttrByName("hidden")) {
        hiddenDepth = tagNesting.size() + 1;
    }
    if (hiddenDepth > 0) {
        UpdateTagNesting(t);
    } else if (Tag_Image == t->tag || Tag_Svg_Image == t->tag) {
        HandleTagSvgImage(t);
    } else {
        HtmlFormatter::HandleHtmlTag(t);
    }
}

bool EpubFormatter::IgnoreText() {
    return hiddenDepth > 0 || HtmlFormatter::IgnoreText();
}

/* FictionBook-specific formatting methods */

Fb2Formatter::Fb2Formatter(HtmlFormatterArgs* args, Fb2Doc* doc)
    : HtmlFormatter(args), section(1), fb2Doc(doc), titleCount(0) {
    if (args->reparseIdx != 0) {
        return;
    }
    ByteSlice* cover = doc->GetCoverImage();
    if (!cover) {
        return;
    }
    EmitImage(cover);
    // render larger images alone on the cover page,
    // smaller images just separated by a horizontal line
    if (0 == currLineInstr.size()) {
        /* the image was broken */;
    } else if (currLineInstr.Last().bbox.dy > args->pageDy / 2) {
        ForceNewPage();
    } else {
        EmitHr();
    }
}

void Fb2Formatter::HandleTagImg(HtmlToken* t) {
    ReportIf(!fb2Doc);
    if (t->IsEndTag()) {
        return;
    }
    ByteSlice* img = nullptr;
    AttrInfo* attr = t->GetAttrByNameNS("href", "http://www.w3.org/1999/xlink");
    if (attr) {
        TempStr src = str::DupTemp(attr->val, attr->valLen);
        url::DecodeInPlace(src);
        img = fb2Doc->GetImageData(src);
    }
    if (img) {
        EmitImage(img);
    }
}

void Fb2Formatter::HandleTagAsHtml(HtmlToken* t, const char* name) {
    HtmlToken tok;
    tok.SetTag(t->type, name, name + str::Len(name));
    HtmlFormatter::HandleHtmlTag(&tok);
}

// the name doesn't quite fit: this handles FB2 tags
void Fb2Formatter::HandleHtmlTag(HtmlToken* t) {
    if (Tag_Title == t->tag || Tag_Subtitle == t->tag) {
        bool isSubtitle = Tag_Subtitle == t->tag;
        TempStr name = str::FormatTemp("h%d", section + (isSubtitle ? 1 : 0));
        HtmlToken tok;
        tok.SetTag(t->type, name, name + str::Len(name));
        HandleTagHx(&tok);
        HandleAnchorAttr(t);
        if (!isSubtitle && t->IsStartTag()) {
            char* link = (char*)Alloc(textAllocator, 24);
            sprintf_s(link, 24, FB2_TOC_ENTRY_MARK "%d", ++titleCount);
            currPage->instructions.Append(DrawInstr::Anchor(link, str::Len(link), Gdiplus::RectF(0, currY, pageDx, 0)));
        }
    } else if (Tag_Section == t->tag) {
        if (t->IsStartTag()) {
            section++;
        } else if (t->IsEndTag() && section > 1) {
            section--;
        }
        FlushCurrLine(true);
        HandleAnchorAttr(t);
    } else if (Tag_P == t->tag) {
        if (!tagNesting.Contains(Tag_Title)) {
            HtmlFormatter::HandleHtmlTag(t);
        }
    } else if (Tag_Image == t->tag) {
        HandleTagImg(t);
        HandleAnchorAttr(t);
    } else if (Tag_A == t->tag) {
        HandleTagA(t, "href", "http://www.w3.org/1999/xlink");
        HandleAnchorAttr(t, true);
    } else if (Tag_Pagebreak == t->tag) {
        ForceNewPage();
    } else if (Tag_Strong == t->tag) {
        HandleTagAsHtml(t, "b");
    } else if (t->NameIs("emphasis")) {
        HandleTagAsHtml(t, "i");
    } else if (t->NameIs("epigraph")) {
        HandleTagAsHtml(t, "blockquote");
    } else if (t->NameIs("empty-line")) {
        if (!t->IsEndTag()) {
            EmitParagraph(0);
        }
    } else if (t->NameIs("stylesheet")) {
        HandleTagAsHtml(t, "style");
    }
}

/* standalone HTML-specific formatting methods */

void HtmlFileFormatter::HandleTagImg(HtmlToken* t) {
    ReportIf(!htmlDoc);
    if (t->IsEndTag()) {
        return;
    }
    bool needAlt = true;
    AttrInfo* attr = t->GetAttrByName("src");
    if (attr) {
        TempStr src = str::DupTemp(attr->val, attr->valLen);
        url::DecodeInPlace(src);
        ByteSlice* img = htmlDoc->GetImageData(src);
        needAlt = !img || !EmitImage(img);
    }
    if (needAlt && (attr = t->GetAttrByName("alt")) != nullptr) {
        HandleText(attr->val, attr->valLen);
    }
}

void HtmlFileFormatter::HandleTagLink(HtmlToken* t) {
    ReportIf(!htmlDoc);
    if (t->IsEndTag()) {
        return;
    }
    AttrInfo* attr = t->GetAttrByName("rel");
    if (!attr || !attr->ValIs("stylesheet")) {
        return;
    }
    attr = t->GetAttrByName("type");
    if (attr && !attr->ValIs("text/css")) {
        return;
    }
    attr = t->GetAttrByName("href");
    if (!attr) {
        return;
    }

    char* src = str::DupTemp(attr->val, attr->valLen);
    url::DecodeInPlace(src);
    ByteSlice data = htmlDoc->GetFileData(src);
    if (data) {
        ParseStyleSheet(data, data.size());
    }
    data.Free();
}
