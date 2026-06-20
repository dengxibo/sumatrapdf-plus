/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

/* formatting extensions for Mobi */

#include "EbookTypography.h"

struct MobiDoc;

class MobiFormatter : public HtmlFormatter {
    // accessor to images (and other format-specific data)
    // it can be nullptr (enables testing by feeding raw html)
    MobiDoc* doc;
    EbookTypographyKind typographyKind = EbookTypographyKind::Latin;
    bool readerStyle = false;

    size_t tocStartIdx = (size_t)-1;
    bool inTocRegion = false;
    bool inToc = false;
    bool tocPageBreakDone = false;
    bool sawColophonPhone = false;
    bool pendingMu = false;
    int blockquoteDepth = 0;
    bool injectedExthCover = false;

    bool InTocLikeRegion() const { return inToc || (sawColophonPhone && !tocPageBreakDone); }

    void UpdateTocState();
    void StartTocPage();
    void OnParserProgress() override;
    bool BeforeTextRun(const char* s, size_t sLen) override;
    void HandleTagBlockquote(HtmlToken* t);
    void HandleSpacing_Mobi(HtmlToken* t);
    SizeF MaxImageSize(HtmlToken* t);
    void HandleTagImg(HtmlToken* t) override;
    void HandleHtmlTag(HtmlToken* t) override;
    float ListIndentDx() const override;
    float ExtraParagraphDy() override;

  public:
    MobiFormatter(HtmlFormatterArgs* args, MobiDoc* doc, EbookTypographyKind typographyKind, bool readerStyle);
};

/* formatting extensions for EPUB */

class EpubDoc;

class EpubFormatter : public HtmlFormatter {
    void HandleTagImg(HtmlToken* t) override;
    void HandleTagPagebreak(HtmlToken* t) override;
    void HandleTagLink(HtmlToken* t) override;
    void HandleHtmlTag(HtmlToken* t) override;
    bool IgnoreText() override;

    void HandleTagSvgImage(HtmlToken* t);

    EpubDoc* epubDoc;
    AutoFreeStr pagePath;
    size_t hiddenDepth;

  public:
    EpubFormatter(HtmlFormatterArgs* args, EpubDoc* doc) : HtmlFormatter(args), epubDoc(doc), hiddenDepth(0) {}
};

/* formatting extensions for FictionBook */

class Fb2Doc;

class Fb2Formatter : public HtmlFormatter {
    int section;
    int titleCount;

    void HandleTagImg(HtmlToken* t) override;
    void HandleTagAsHtml(HtmlToken* t, const char* name);
    void HandleHtmlTag(HtmlToken* t) override;

    bool IgnoreText() override { return false; }

    Fb2Doc* fb2Doc;

  public:
    Fb2Formatter(HtmlFormatterArgs* args, Fb2Doc* doc);
};

/* formatting extensions for standalone HTML */

class HtmlDoc;

class HtmlFileFormatter : public HtmlFormatter {
  protected:
    void HandleTagImg(HtmlToken* t) override;
    void HandleTagLink(HtmlToken* t) override;

    HtmlDoc* htmlDoc;

  public:
    HtmlFileFormatter(HtmlFormatterArgs* args, HtmlDoc* doc) : HtmlFormatter(args), htmlDoc(doc) {}
};

/* formatting extensions for TXT */

class TxtFormatter : public HtmlFormatter {
  protected:
    void HandleTagPagebreak(HtmlToken*) override { ForceNewPage(); }

  public:
    explicit TxtFormatter(HtmlFormatterArgs* args) : HtmlFormatter(args) {}
};
