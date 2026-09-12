/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the PrintedToc data model: JSON writer/reader round-trip,
// golden OCR fixtures (testdata/printed-toc) and the debug overlay store.
// The fixtures freeze OCR output so parser regressions (P1+) can be
// distinguished from OCR model regressions.

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/Log.h" // logf (otherwise shadowed by <cmath> float logf(float))
#include "PrintedTocModel.h"
#include "ExtractPdfToc.h" // ScanLine
#include "utils/UtAssert.h"

#define ZH_MU_LU "\xe7\x9b\xae\xe5\xbd\x95"                           // 目录
#define ZH_MU_LU_XU ZH_MU_LU "\xef\xbc\x88\xe7\xbb\xad\xef\xbc\x89"   // 目录（续）
#define ZH_Q1 "\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0"                  // 第一章
#define ZH_Q2 "\xe7\xac\xac\xe4\xba\x8c\xe7\xab\xa0"                  // 第二章
#define ZH_Q3 "\xe7\xac\xac\xe4\xb8\x89\xe7\xab\xa0"                  // 第三章
#define ZH_Q4 "\xe7\xac\xac\xe5\x9b\x9b\xe7\xab\xa0"                  // 第四章
#define ZH_Q5 "\xe7\xac\xac\xe4\xba\x94\xe7\xab\xa0"                  // 第五章
#define ZH_Q6 "\xe7\xac\xac\xe5\x85\xad\xe7\xab\xa0"                  // 第六章
#define ZH_Q7 "\xe7\xac\xac\xe4\xb8\x83\xe7\xab\xa0"                  // 第七章
#define ZH_Q8 "\xe7\xac\xac\xe5\x85\xab\xe7\xab\xa0"                  // 第八章
#define ZH_S1 "\xe7\xac\xac\xe4\xb8\x80\xe8\x8a\x82"                  // 第一节
#define ZH_ZONG_LUN "\xe6\x80\xbb\xe8\xae\xba"                        // 总论
#define ZH_FANG_FA "\xe6\x96\xb9\xe6\xb3\x95"                         // 方法
#define ZH_CAN_KAO "\xe5\x8f\x82\xe8\x80\x83\xe6\x96\x87\xe7\x8c\xae" // 参考文献

static void PtJsonBufTests() {
    {
        PtJsonBuf jb;
        jb.Escaped("plain");
        char* s = jb.Steal();
        utassert(str::Eq(s, "\"plain\""));
        str::Free(s);
    }
    {
        PtJsonBuf jb;
        jb.Escaped("quote\" back\\ nl\n\t\x01 \xe7\x9b\xae\xe5\xbd\x95"); // 目录
        char* s = jb.Steal();
        utassert(str::Eq(s, "\"quote\\\" back\\\\ nl\\n\\t\\u0001 \xe7\x9b\xae\xe5\xbd\x95\""));
        str::Free(s);
    }
    {
        PtJsonBuf jb;
        jb.Escaped(nullptr);
        char* s = jb.Steal();
        utassert(str::Eq(s, "\"\""));
        str::Free(s);
    }
    {
        PtJsonBuf jb;
        jb.Int(-42);
        jb.Raw("|");
        jb.Int(0);
        jb.Raw("|");
        jb.Float(1.5);
        char* s = jb.Steal();
        utassert(str::Eq(s, "-42|0|1.500"));
        str::Free(s);
    }
}

static void PtBoxFTests() {
    PtBoxF a;
    utassert(a.IsEmpty());
    PtBoxF b{10, 20, 60, 90};
    utassert(!b.IsEmpty());
    utassert(b.Dx() == 50 && b.Dy() == 70);
    utassert(b.MidY() == 55);
    b.UnionWith(PtBoxF{100, -10, 120, 30});
    utassert(b.x0 == 10 && b.y0 == -10 && b.x1 == 120 && b.y1 == 90);
    // union with empty keeps a
    b.UnionWith(PtBoxF{});
    utassert(b.x0 == 10 && b.x1 == 120);
}

// builds a small OcrBox page (CJK + quotes + charX) for the dump round-trip
static void MakeSampleBoxes(Vec<OcrBox>& boxes) {
    OcrBox b1;
    b1.rect = Rect(230, 320, 940, 40);
    b1.text = str::Dup("\xe7\xac\xac\xe4\xb8\x80\xe7\xab\xa0 \"\xe6\x80\xbb\xe8\xae\xba\""); // 第一章 "总论"
    boxes.Append(b1);

    OcrBox b2;
    b2.rect = Rect(100, 400, 300, 40);
    b2.text = str::Dup("1.1\nTest");
    b2.nChar = 3;
    b2.charX = AllocArray<int>(6);
    b2.charX[0] = 0;
    b2.charX[1] = 10;
    b2.charX[2] = 20;
    b2.charX[3] = 30;
    b2.charX[4] = 40;
    b2.charX[5] = 50;
    boxes.Append(b2);
}

static void DumpRoundTripTest() {
    // enable dumping for the duration of this test
    SetEnvironmentVariableA("SUMATRA_PTOC_DUMP", "1");

    Vec<OcrBox> boxes;
    MakeSampleBoxes(boxes);

    // dumps land in "<pdf-no-ext>.ptoc" next to this fake pdf path
    TempStr tmpPdf = GetTempFilePathTemp("ptoc_ut_roundtrip");
    PtocDumpOcrPageJson(tmpPdf, 3, 1410, 2000, OcrProfile::Balanced, boxes);

    TempStr dir = PtocDumpDirForFileTemp(tmpPdf);
    TempStr jsonPath = path::JoinTemp(dir, "ocr-p3.json");
    utassert(file::Exists(jsonPath));

    PtPageData page;
    bool ok = PtocLoadOcrPageJson(jsonPath, &page);
    utassert(ok);
    utassert(page.pageIndex == 3);
    utassert(page.width == 1410 && page.height == 2000);
    utassert(page.tokens.Size() == 2);
    if (page.tokens.Size() == 2) {
        PtToken& t0 = page.tokens[0];
        utassert(str::Eq(t0.text, boxes[0].text));
        utassert(t0.box.x0 == 230 && t0.box.y0 == 320 && t0.box.x1 == 1170 && t0.box.y1 == 360);
        PtToken& t1 = page.tokens[1];
        utassert(str::Eq(t1.text, "1.1\nTest"));
        utassert(t1.box.x1 == 400 && t1.box.y1 == 440);
    }
    page.Free();

    for (int i = 0; i < boxes.Size(); i++) {
        str::Free(boxes[i].text);
        free(boxes[i].charX);
    }

    // scan-line dump: smoke test the file gets written and holds the text
    Vec<ScanLine> lines;
    ScanLine sl;
    sl.text = str::Dup("\xe7\x9b\xae\xe5\xbd\x95 line"); // 目录 line
    sl.srcPage = 3;
    sl.x = 100;
    sl.y = 200;
    sl.dx = 300;
    sl.dy = 30;
    lines.Append(sl);
    PtocDumpScanLinesJson(tmpPdf, lines, 300, "collected");
    TempStr linesPath = path::JoinTemp(dir, "toc-lines.json");
    utassert(file::Exists(linesPath));
    {
        ByteSlice data = file::ReadFile(linesPath);
        char* txt = str::Dup(data);
        data.Free();
        utassert(str::Find(txt, "\"page\":3") != nullptr);
        utassert(str::Find(txt, sl.text) != nullptr);
        utassert(str::Find(txt, "\"stage\":\"collected\"") != nullptr);
        str::Free(txt);
    }
    str::Free(sl.text);

    SetEnvironmentVariableA("SUMATRA_PTOC_DUMP", nullptr);
}

static void FixtureTests() {
    // fixtures live in <repo>/testdata/printed-toc, resolved from __FILE__
    TempStr testDir = path::GetDirTemp(__FILE__);
    TempStr fixtureDir = path::JoinTemp(testDir, "..\\..\\..\\testdata\\printed-toc");

    {
        TempStr p = path::JoinTemp(fixtureDir, "zh-single-leader-p3.json");
        utassert(file::Exists(p));
        PtPageData page;
        utassert(PtocLoadOcrPageJson(p, &page));
        utassert(page.pageIndex == 3);
        utassert(page.tokens.Size() == 7);
        if (page.tokens.Size() == 7) {
            // a title with a year inside must survive loading untouched;
            // tokens are whole OCR det lines (第三章 中国经济 2025 展望 ..... 33)
            utassert(str::Eq(page.tokens[5].text,
                             "\xe7\xac\xac\xe4\xb8\x89\xe7\xab\xa0 \xe4\xb8\xad\xe5\x9b\xbd\xe7\xbb\x8f\xe6\xb5\x8e "
                             "2025 \xe5\xb1\x95\xe6\x9c\x9b ..... 33"));
            // anchor column x-alignment: rightmost boxes end near the same x
            utassert(page.tokens[1].box.x1 == 1170);
            utassert(page.tokens[6].box.x1 == 1160);
        }
        page.Free();
    }
    {
        TempStr p = path::JoinTemp(fixtureDir, "zh-two-col-p5.json");
        PtPageData page;
        utassert(PtocLoadOcrPageJson(p, &page));
        utassert(page.tokens.Size() == 11);
        if (page.tokens.Size() == 11) {
            // left column anchors ~x=640, right column anchors ~x=1300
            utassert(page.tokens[1].box.x1 == 640);
            utassert(page.tokens[10].box.x1 == 1300);
        }
        page.Free();
    }
    {
        TempStr p = path::JoinTemp(fixtureDir, "en-roman-p2.json");
        PtPageData page;
        utassert(PtocLoadOcrPageJson(p, &page));
        utassert(page.tokens.Size() == 13);
        if (page.tokens.Size() == 13) {
            // separate anchor tokens to the right of the title tokens
            utassert(str::Eq(page.tokens[2].text, "iii"));
            utassert(str::Eq(page.tokens[10].text, "11"));
        }
        page.Free();
    }
}

static void OverlayStoreTests() {
    utassert(!PtocOverlayEnabled()); // env must not leak between tests

    const char* pdf = "C:\\fake\\book.pdf";
    Vec<PtocOverlayPage*> pages;
    for (int i = 0; i < 2; i++) {
        PtocOverlayPage* pg = new PtocOverlayPage();
        pg->pageNo = 3 + i;
        pg->isTocPage = i == 0;
        pg->pageScore = 0.75f;
        PtBoxF col{100, 200, 1300, 1800};
        pg->columns.Append(col);
        pg->indentBands.Append(0.05f);
        PtocOverlayEntry e;
        e.titleBox = PtBoxF{110, 300, 800, 340};
        e.pageNumberBox = PtBoxF{1250, 300, 1300, 340};
        e.level = 1;
        e.parseConf = 0.9f;
        pg->entries.Append(e);
        pages.Append(pg);
    }
    PtocOverlaySet(pdf, pages);
    utassert(pages.Size() == 2 && pages[0] == nullptr); // stolen

    Vec<PtocOverlayPage*> out;
    PtocOverlayCopyFor(pdf, out);
    utassert(out.Size() == 2);
    utassert(out[0]->pageNo == 3 && out[0]->isTocPage && out[0]->pageScore == 0.75f);
    utassert(out[0]->columns.Size() == 1 && out[0]->columns[0].x1 == 1300);
    utassert(out[0]->entries.Size() == 1 && out[0]->entries[0].level == 1);
    for (int i = 0; i < out.Size(); i++) {
        delete out[i];
    }
    out.Reset();

    // replace with empty clears the document
    Vec<PtocOverlayPage*> empty;
    PtocOverlaySet(pdf, empty);
    PtocOverlayCopyFor(pdf, out);
    utassert(out.Size() == 0);

    PtocOverlaySet("C:\\other\\book.pdf", empty); // no-op on unknown doc
    PtocOverlayClear();
    PtocOverlayCopyFor("C:\\other\\book.pdf", out);
    utassert(out.Size() == 0);
}

// ---------------------------------------------------------------------------
// P1 stage tests
// ---------------------------------------------------------------------------

static TempStr FixtureDirTemp() {
    TempStr testDir = path::GetDirTemp(__FILE__);
    return path::JoinTemp(testDir, "..\\..\\..\\testdata\\printed-toc");
}

static void PtLoadFixture(const char* name, PtPageData* page) {
    TempStr p = path::JoinTemp(FixtureDirTemp(), name);
    utassert(file::Exists(p));
    utassert(PtocLoadOcrPageJson(p, page));
}

static void PtPageLabelTests() {
    PtPageLabel lbl;
    utassert(PtParsePageLabel("12", &lbl));
    utassert(lbl.type == PtPageLabelType::Arabic && lbl.ordinal == 12 && lbl.hasOrdinal);
    utassert(str::Eq(lbl.rawText, "12"));

    utassert(PtParsePageLabel(" 017 ", &lbl) && lbl.type == PtPageLabelType::Arabic && lbl.ordinal == 17);

    utassert(PtParsePageLabel("xiv", &lbl) && lbl.type == PtPageLabelType::RomanLower && lbl.ordinal == 14);
    utassert(PtParsePageLabel("XII", &lbl) && lbl.type == PtPageLabelType::RomanUpper && lbl.ordinal == 12);
    utassert(PtParsePageLabel("iv", &lbl) && lbl.type == PtPageLabelType::RomanLower && lbl.ordinal == 4);
    utassert(!PtParsePageLabel("XiI", &lbl));  // mixed case
    utassert(!PtParsePageLabel("mmmm", &lbl)); // 4000 out of range
    utassert(!PtParsePageLabel("piv", &lbl));  // not a numeral

    utassert(PtParsePageLabel("2-18", &lbl) && lbl.type == PtPageLabelType::Compound);
    utassert(lbl.number == 18 && str::Eq(lbl.section, "2") && !lbl.hasOrdinal);
    utassert(PtParsePageLabel("A-12", &lbl) && lbl.type == PtPageLabelType::Compound);
    utassert(lbl.number == 12 && str::Eq(lbl.section, "A"));

    utassert(!PtParsePageLabel("abc", &lbl));
    utassert(!PtParsePageLabel("", &lbl));
    utassert(!PtParsePageLabel(nullptr, &lbl));
    utassert(!PtParsePageLabel("1.2.3", &lbl));
    utassert(!PtParsePageLabel("2025", &lbl) == false); // years parse as arabic - position decides
    utassert(PtParsePageLabel("2025", &lbl) && lbl.ordinal == 2025);

    // OCR confusion retry: only otherwise-numeric runs with a real digit
    utassert(PtParsePageLabelOcrFixed("O1O", &lbl) && lbl.type == PtPageLabelType::Arabic && lbl.ordinal == 10);
    utassert(PtParsePageLabelOcrFixed("O19", &lbl) && lbl.ordinal == 19);
    utassert(PtParsePageLabelOcrFixed("20I", &lbl) && lbl.ordinal == 201);
    utassert(PtParsePageLabelOcrFixed(" 05o ", &lbl) && lbl.ordinal == 50);
    utassert(!PtParsePageLabelOcrFixed("Oil", &lbl)); // no real digit -> word
    utassert(!PtParsePageLabelOcrFixed("abc", &lbl)); // unmappable letters
    utassert(!PtParsePageLabelOcrFixed("12x", &lbl));
    utassert(!PtParsePageLabelOcrFixed("", &lbl));
    utassert(!PtParsePageLabelOcrFixed(nullptr, &lbl));
    // plain parse still wins on its own (no confusion needed)
    utassert(PtParsePageLabelOcrFixed("12", &lbl) && lbl.ordinal == 12);
}

static void PtReconstructTests() {
    {
        // EN fixture: title and page label are separate tokens on one line
        PtPageData page;
        PtLoadFixture("en-roman-p2.json", &page);
        PtReconstructLines(page);
        utassert(page.lines.Size() == 7);
        utassert(page.tokens.Size() == 0); // ownership moved
        if (page.lines.Size() == 7) {
            utassert(page.lines[0]->tokens.Size() == 1 && str::Eq(page.lines[0]->tokens[0].text, "Contents"));
            utassert(page.lines[1]->tokens.Size() == 2);
            if (page.lines[1]->tokens.Size() == 2) {
                utassert(str::Eq(page.lines[1]->tokens[0].text, "Preface"));
                utassert(str::Eq(page.lines[1]->tokens[1].text, "iii"));
            }
            utassert(page.lines[1]->box.x0 == 180 && page.lines[1]->box.x1 == 1115);
            for (int i = 0; i < 7; i++) {
                utassert(page.lines[i]->columnIndex == 0);
            }
        }
        page.Free();
    }
    {
        // ZH fixture: whole OCR det lines, one token each, single column
        PtPageData page;
        PtLoadFixture("zh-single-leader-p3.json", &page);
        PtReconstructLines(page);
        utassert(page.lines.Size() == 7);
        for (int i = 0; i < page.lines.Size(); i++) {
            utassert(page.lines[i]->columnIndex == 0);
            utassert(page.lines[i]->tokens.Size() == 1);
        }
        page.Free();
    }
    {
        // ZH fixture: two parallel columns + a heading crossing the cut
        PtPageData page;
        PtLoadFixture("zh-two-col-p5.json", &page);
        PtReconstructLines(page);
        utassert(page.lines.Size() == 11);
        int nCol1 = 0;
        for (int i = 0; i < page.lines.Size(); i++) {
            if (page.lines[i]->columnIndex == 1) {
                nCol1++;
            }
        }
        utassert(nCol1 == 5);
        if (page.lines.Size() == 11) {
            // column 0 lines come first (incl. heading), then column 1
            utassert(str::StartsWith(page.lines[0]->tokens[0].text, ZH_MU_LU_XU));
            utassert(str::StartsWith(page.lines[1]->tokens[0].text, ZH_Q5));
            utassert(str::StartsWith(page.lines[6]->tokens[0].text, ZH_Q7));
        }
        page.Free();
    }
}

static void PtAddToken(PtPageData& page, const char* text, int x, int y, int dx, int dy) {
    PtToken t;
    t.text = str::Dup(text);
    t.box = PtBoxF{(float)x, (float)y, (float)(x + dx), (float)(y + dy)};
    page.tokens.Append(t);
}

static void PtScoreTests() {
    // fixtures score as TOC pages
    {
        PtPageData page;
        PtLoadFixture("zh-single-leader-p3.json", &page);
        PtTocPageScores sc = PtProcessPage(page, nullptr, nullptr);
        utassert(sc.total >= PtocConfig().pageScoreMin);
        utassert(sc.endingPageNumberRatio == 1.0f);
        utassert(sc.tocKeywordScore == 1.0f);
        page.Free();
    }
    {
        PtPageData page;
        PtLoadFixture("en-roman-p2.json", &page);
        PtTocPageScores sc = PtProcessPage(page, nullptr, nullptr);
        utassert(sc.total >= PtocConfig().pageScoreMin);
        utassert(sc.monotonicPageNumberScore > 0.7f);
        page.Free();
    }
    {
        // a body page: long uniform lines, no anchors, no keyword
        PtPageData page;
        page.pageIndex = 40;
        page.width = 1410;
        page.height = 2000;
        for (int i = 0; i < 6; i++) {
            PtAddToken(
                page,
                "\xe8\xbf\x99\xe6\x98\xaf\xe6\xad\xa3\xe6\x96\x87\xe5\x86\x85\xe5\xae\xb9\xe7\x94\xa8\xe4\xba"
                "\x8e\xe9\xaa\x8c\xe8\xaf\x81\xe8\xaf\x84\xe5\x88\x86\xe9\x80\xbb\xe8\xbe\x91", // 这是正文内容用于验证评分逻辑
                100, 100 + i * 60, 1200, 36);
        }
        PtTocPageScores sc = PtProcessPage(page, nullptr, nullptr);
        utassert(sc.total < PtocConfig().pageScoreMin);
        utassert(sc.endingPageNumberRatio == 0);
        page.Free();
    }
}

static void PtCandidateTests() {
    {
        PtPageData page;
        PtLoadFixture("zh-single-leader-p3.json", &page);
        Vec<PtEntryCandidate> cands;
        PtocOverlayPage overlay;
        PtTocPageScores sc = PtProcessPage(page, &cands, &overlay);
        utassert(sc.total >= PtocConfig().pageScoreMin);
        utassert(cands.Size() == 6); // heading dropped
        utassert(overlay.isTocPage && overlay.entries.Size() == 6);
        utassert(overlay.columns.Size() == 1);
        utassert(overlay.indentBands.Size() == 2);
        if (cands.Size() == 6) {
            utassert(str::Eq(cands[0].title, ZH_Q1 " " ZH_ZONG_LUN));
            utassert(cands[0].printedPage.type == PtPageLabelType::Arabic && cands[0].printedPage.ordinal == 1);
            utassert(cands[0].level == 1 && cands[0].column == 0);
            utassert(str::StartsWith(cands[1].title, ZH_S1));
            utassert(cands[1].printedPage.ordinal == 3 && cands[1].level == 2);
            utassert(cands[2].printedPage.ordinal == 8 && cands[2].level == 2);
            utassert(str::StartsWith(cands[3].title, ZH_Q2));
            utassert(cands[3].printedPage.ordinal == 12 && cands[3].level == 1);
            utassert(cands[4].printedPage.ordinal == 33 && cands[4].level == 1);
            utassert(cands[5].printedPage.ordinal == 41 && cands[5].level == 1);
            // title box must sit left of the anchor box
            utassert(cands[0].titleBox.x1 <= cands[0].pageNumberBox.x0);
        }
        for (int i = 0; i < cands.Size(); i++) {
            cands[i].Free();
        }
        overlay.Free();
        page.Free();
    }
    {
        PtPageData page;
        PtLoadFixture("zh-two-col-p5.json", &page);
        Vec<PtEntryCandidate> cands;
        PtocOverlayPage overlay;
        PtProcessPage(page, &cands, &overlay);
        utassert(cands.Size() == 10);
        utassert(overlay.columns.Size() == 2);
        if (cands.Size() == 10) {
            // reading order: left column first, then right column
            utassert(cands[0].printedPage.ordinal == 45 && cands[0].column == 0);
            utassert(cands[4].printedPage.ordinal == 63 && cands[4].column == 0);
            utassert(str::StartsWith(cands[5].title, ZH_Q7));
            utassert(cands[5].printedPage.ordinal == 72 && cands[5].column == 1);
            utassert(str::Eq(cands[9].title, ZH_CAN_KAO));
            utassert(cands[9].printedPage.ordinal == 95);
        }
        for (int i = 0; i < cands.Size(); i++) {
            cands[i].Free();
        }
        overlay.Free();
        page.Free();
    }
    {
        PtPageData page;
        PtLoadFixture("en-roman-p2.json", &page);
        Vec<PtEntryCandidate> cands;
        PtocOverlayPage overlay;
        PtProcessPage(page, &cands, &overlay);
        utassert(cands.Size() == 6);
        if (cands.Size() == 6) {
            utassert(str::Eq(cands[0].title, "Preface"));
            utassert(cands[0].printedPage.type == PtPageLabelType::RomanLower && cands[0].printedPage.ordinal == 3);
            utassert(str::Eq(cands[1].title, "Acknowledgments"));
            utassert(cands[1].printedPage.ordinal == 7 && cands[1].level == 1);
            utassert(str::Eq(cands[2].title, "Chapter 1 Introduction"));
            utassert(cands[2].printedPage.type == PtPageLabelType::Arabic && cands[2].printedPage.ordinal == 1);
            utassert(str::Eq(cands[3].title, "1.1 C++ and .NET Background"));
            utassert(cands[3].printedPage.ordinal == 4 && cands[3].level == 2);
            utassert(cands[4].printedPage.ordinal == 11 && cands[4].level == 2);
            utassert(str::Eq(cands[5].title, "List of Abbreviations"));
            utassert(cands[5].printedPage.type == PtPageLabelType::RomanLower && cands[5].printedPage.ordinal == 14);
            // separate-token geometry: exact boxes
            utassert(cands[0].titleBox.x0 == 180 && cands[0].pageNumberBox.x1 == 1115);
        }
        for (int i = 0; i < cands.Size(); i++) {
            cands[i].Free();
        }
        overlay.Free();
        page.Free();
    }
}

static void PtMergeTests() {
    // a wrapped title line folds into its anchored predecessor
    PtPageData page;
    page.pageIndex = 9;
    page.width = 1410;
    page.height = 2000;
    PtAddToken(page, ZH_Q1 " " ZH_ZONG_LUN " ........ 1", 230, 320, 940, 40);
    PtAddToken(page,
               "\xef\xbc\x88\xe7\xbb\xad\xef\xbc\x89\xe7\xa0\x94\xe7\xa9\xb6\xe8\x8c\x83\xe5\x9b\xb4\xe7\x95\x8c"
               "\xe5\xae\x9a", // （续）研究范围界定
               280, 380, 400, 36);
    PtAddToken(page, ZH_Q2 " " ZH_FANG_FA " ........ 12", 230, 470, 940, 40);

    Vec<PtEntryCandidate> cands;
    PtProcessPage(page, &cands, nullptr);
    utassert(cands.Size() == 2);
    if (cands.Size() == 2) {
        utassert(cands[0].mergedWithPrev == 1);
        utassert(cands[0].printedPage.ordinal == 1);
        utassert(str::StartsWith(cands[0].title, ZH_Q1 " " ZH_ZONG_LUN));
        utassert(str::Find(cands[0].title,
                           "\xef\xbc\x88\xe7\xbb\xad\xef\xbc\x89") != nullptr); // （续） appended without space
        utassert(cands[1].printedPage.ordinal == 12 && cands[1].mergedWithPrev == 0);
        utassert(cands[0].level == 1 && cands[1].level == 1);
    }
    for (int i = 0; i < cands.Size(); i++) {
        cands[i].Free();
    }
    page.Free();
}

// ---------------------------------------------------------------------------
// P2 / P3 stage tests
// ---------------------------------------------------------------------------

// builds one candidate with geometry context (caller frees the returned
// entry's strings, or ownership moves into the Vec it is appended to)
static PtEntryCandidate PtMakeCand(const char* title, PtPageLabelType type, int ordinal, bool hasOrdinal, int srcPage,
                                   int col, float indent, float x0) {
    PtEntryCandidate c;
    c.title = str::Dup(title);
    c.printedPage.type = type;
    c.printedPage.ordinal = ordinal;
    c.printedPage.hasOrdinal = hasOrdinal;
    c.tocSourcePage = srcPage;
    c.column = col;
    c.indentNormalized = indent;
    c.titleBox = PtBoxF{x0, 100, x0 + 400, 140};
    c.charWidth = 40;
    c.columnWidth = 1000; // eps = 0.6 * 40 / 1000 = 0.024
    return c;
}

static void PtFreeCands(Vec<PtEntryCandidate>& cands) {
    for (int i = 0; i < cands.Size(); i++) {
        cands[i].Free();
    }
}

#define ZH_YAN_JIU "\xe7\xa0\x94\xe7\xa9\xb6\xe8\x8c\x83\xe5\x9b\xb4" // 研究范围

static void PtCrossPageMergeTests() {
    {
        // 第X章 heading stays a standalone level-1 entry and borrows the
        // next anchored entry's printed page instead of folding upwards
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 1, true, 2, 0, 0.0f, 230));
        cands.Append(
            PtMakeCand("\xe7\xac\xac"
                       "3"
                       "\xe7\xab\xa0 title",
                       PtPageLabelType::Unknown, 0, false, 3, 0, 0.0f, 280));
        cands.Append(PtMakeCand("first section", PtPageLabelType::Arabic, 61, true, 3, 0, 0.0f, 280));
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 3);
        if (cands.Size() == 3) {
            utassert(cands[1].chapterHeading);
            utassert(cands[1].printedPage.ordinal == 61); // borrowed from the next entry
            utassert(cands[1].level == 1);
            utassert(str::Eq(cands[1].title,
                             "\xe7\xac\xac"
                             "3"
                             "\xe7\xab\xa0 title")); // not folded into [0]
            utassert(cands[0].mergedWithPrev == 0);
        }
        PtFreeCands(cands);
    }
    {
        // orphan at the top of the next TOC page folds into the last
        // anchored entry of the previous page (CJK + CJK: no joining space)
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 1, true, 2, 0, 0.0f, 230));
        cands.Append(PtMakeCand(ZH_YAN_JIU, PtPageLabelType::Unknown, 0, false, 3, 0, 0.0f, 280));
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 1);
        if (cands.Size() == 1) {
            utassert(cands[0].mergedWithPrev == 1);
            utassert(cands[0].printedPage.ordinal == 1);
            utassert(str::Eq(cands[0].title, ZH_Q1 " " ZH_ZONG_LUN ZH_YAN_JIU));
            utassert(cands[0].titleBox.x0 == 230 && cands[0].titleBox.x1 == 680); // unioned
        }
        PtFreeCands(cands);
    }
    {
        // continuation wrapped into the next column of the same page
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q2 " " ZH_FANG_FA, PtPageLabelType::Arabic, 12, true, 5, 0, 0.0f, 230));
        cands.Append(PtMakeCand(ZH_YAN_JIU, PtPageLabelType::Unknown, 0, false, 5, 1, 0.0f, 760));
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 1 && cands[0].mergedWithPrev == 1);
        PtFreeCands(cands);
    }
    {
        // continuationMaxChain: 4 orphans merge, the 5th is dropped as noise
        const PrintedTocConfig& cfg = PtocConfig();
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("Entry", PtPageLabelType::Arabic, 1, true, 2, 0, 0.0f, 230));
        for (int i = 0; i < 5; i++) {
            cands.Append(PtMakeCand("more", PtPageLabelType::Unknown, 0, false, 2, 0, 0.0f, 280));
        }
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 1);
        utassert(cands[0].mergedWithPrev == cfg.continuationMaxChain);
        utassert(str::Eq(cands[0].title, "Entry more more more more"));
        PtFreeCands(cands);
    }
    {
        // headings never survive - even when they carry a page anchor
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_MU_LU, PtPageLabelType::Arabic, 1, true, 2, 0, 0.0f, 230));
        cands.Append(PtMakeCand("Contents", PtPageLabelType::Arabic, 2, true, 2, 0, 0.0f, 230));
        cands.Append(PtMakeCand("Chapter 1", PtPageLabelType::Arabic, 3, true, 2, 0, 0.0f, 230));
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 1);
        utassert(str::Eq(cands[0].title, "Chapter 1"));
        PtFreeCands(cands);
    }
    {
        // non-contiguous orphans and orphans following orphans are dropped
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("A", PtPageLabelType::Arabic, 1, true, 2, 0, 0.0f, 230));
        cands.Append(PtMakeCand("B", PtPageLabelType::Unknown, 0, false, 4, 0, 0.0f, 280)); // page gap
        cands.Append(PtMakeCand("C", PtPageLabelType::Unknown, 0, false, 4, 0, 0.0f, 280)); // prev unanchored
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 1 && cands[0].mergedWithPrev == 0);
        PtFreeCands(cands);
    }
    {
        // a shallower continuation line does not fold into the entry
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("A", PtPageLabelType::Arabic, 1, true, 2, 0, 0.10f, 230));
        cands.Append(PtMakeCand("B", PtPageLabelType::Unknown, 0, false, 2, 0, 0.05f, 180));
        PtMergeCrossPage(&cands);
        utassert(cands.Size() == 1 && str::Eq(cands[0].title, "A"));
        PtFreeCands(cands);
    }
}

static void PtGlobalLevelTests() {
    {
        // same indent ratio on pages with different column widths must land
        // in one global band; a clearly deeper line starts a new level
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("p1-a", PtPageLabelType::Arabic, 1, true, 1, 0, 0.100f, 230));
        cands.Append(PtMakeCand("p2-a", PtPageLabelType::Arabic, 5, true, 2, 0, 0.108f, 230));
        cands[1].columnWidth = 500; // eps = 0.048, so 0.100 and 0.108 still cluster
        cands.Append(PtMakeCand("p2-b", PtPageLabelType::Arabic, 6, true, 2, 0, 0.300f, 380));
        PtNormalizeLevelsAcrossPages(&cands);
        utassert(cands[0].level == 1 && cands[1].level == 1 && cands[2].level == 2);
        utassert(cands[0].hierarchyConf == 0.8f); // band of 2 >= indentMinBandLines
        utassert(cands[2].hierarchyConf == 0.4f); // single-line band
        PtFreeCands(cands);
    }
    {
        // band ranks are global: 0.0 / 0.1 / 0.25 -> levels 1 / 2 / 3
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("a", PtPageLabelType::Arabic, 1, true, 1, 0, 0.0f, 230));
        cands.Append(PtMakeCand("b", PtPageLabelType::Arabic, 2, true, 1, 0, 0.1f, 270));
        cands.Append(PtMakeCand("c", PtPageLabelType::Arabic, 3, true, 1, 0, 0.25f, 330));
        PtNormalizeLevelsAcrossPages(&cands);
        utassert(cands[0].level == 1 && cands[1].level == 2 && cands[2].level == 3);
        PtFreeCands(cands);
    }
    {
        // candidates without geometry context keep their per-page level
        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("a", PtPageLabelType::Arabic, 1, true, 1, 0, 0.1f, 230));
        cands[0].charWidth = 0; // per-page stage did not run
        cands[0].level = 7;
        PtNormalizeLevelsAcrossPages(&cands);
        utassert(cands[0].level == 7);
        PtFreeCands(cands);
    }
}

static void PtPageMappingTests() {
    {
        // consistent offset 3 across 3 anchors: one segment, full confidence
        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 3;
        a.pdfPage = 6;
        anchors.Append(a);
        a.printed = 1;
        a.pdfPage = 4;
        anchors.Append(a);
        a.printed = 2;
        a.pdfPage = 5;
        anchors.Append(a);
        Vec<PtPageMappingSegment> segs;
        PtBuildPageMapping(anchors, &segs);
        utassert(segs.Size() == 1);
        utassert(segs[0].labelType == PtPageLabelType::Arabic);
        utassert(segs[0].printedBegin == 1 && segs[0].printedEnd == 3 && segs[0].offset == 3);
        utassert(segs[0].confidence == 1.0f); // 3 samples, unanimous vote

        float conf;
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::Arabic, 2, &conf) == 5);
        utassert(conf == 1.0f);
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::Arabic, 4, nullptr) == -1); // outside range
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::RomanLower, 2, nullptr) == -1);
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::Arabic, 0, nullptr) == -1);
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::Arabic, 1, nullptr) == 4);
    }
    {
        // two label types with their own offsets form two segments
        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 2;
        a.pdfPage = 11;
        anchors.Append(a);
        a.printed = 1;
        a.pdfPage = 10;
        anchors.Append(a);
        a.type = PtPageLabelType::RomanLower;
        a.printed = 2;
        a.pdfPage = 4;
        anchors.Append(a);
        a.printed = 1;
        a.pdfPage = 3;
        anchors.Append(a);
        Vec<PtPageMappingSegment> segs;
        PtBuildPageMapping(anchors, &segs);
        utassert(segs.Size() == 2);
        // the offsets tie 2:2 -> vote share 0.5 < mappingVoteMinRatio halves
        // every confidence; each run has 2 samples -> base 0.8 -> 0.4
        utassert(segs[0].labelType == PtPageLabelType::Arabic && segs[0].offset == 9);
        utassert(segs[0].confidence == 0.4f);
        utassert(segs[1].labelType == PtPageLabelType::RomanLower && segs[1].offset == 2);
        utassert(segs[1].confidence == 0.4f);
    }
    {
        // a dominant offset keeps full confidence; a single-evidence segment
        // is capped at 0.5
        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 4;
        a.pdfPage = 8;
        anchors.Append(a);
        a.printed = 1;
        a.pdfPage = 5;
        anchors.Append(a);
        a.printed = 2;
        a.pdfPage = 6;
        anchors.Append(a);
        a.printed = 3;
        a.pdfPage = 7;
        anchors.Append(a);
        a.type = PtPageLabelType::RomanLower;
        a.printed = 1;
        a.pdfPage = 10;
        anchors.Append(a);
        Vec<PtPageMappingSegment> segs;
        PtBuildPageMapping(anchors, &segs);
        utassert(segs.Size() == 2);
        utassert(segs[0].labelType == PtPageLabelType::Arabic && segs[0].confidence == 1.0f);
        utassert(segs[1].labelType == PtPageLabelType::RomanLower && segs[1].confidence == 0.5f);
    }
    {
        // a mid-sequence outlier splits one type into three 1-sample runs
        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 3;
        a.pdfPage = 7;
        anchors.Append(a);
        a.printed = 2;
        a.pdfPage = 2;
        anchors.Append(a);
        a.printed = 1;
        a.pdfPage = 5;
        anchors.Append(a);
        Vec<PtPageMappingSegment> segs;
        PtBuildPageMapping(anchors, &segs);
        utassert(segs.Size() == 3);
        float conf;
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::Arabic, 2, &conf) == 2 && conf == 0.5f);
        utassert(PtMapPrintedToPdf(segs, PtPageLabelType::Arabic, 3, nullptr) == 7);
    }
    {
        // invalid anchors are ignored
        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 0;
        a.pdfPage = 4;
        anchors.Append(a);
        a.printed = 2;
        a.pdfPage = -1;
        anchors.Append(a);
        Vec<PtPageMappingSegment> segs;
        PtBuildPageMapping(anchors, &segs);
        utassert(segs.Size() == 0);
    }
    {
        // PtResolvePdfPages applies the mapping, skipping non-convertible,
        // uncovered and out-of-range candidates
        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 1;
        a.pdfPage = 4;
        anchors.Append(a);
        a.printed = 3;
        a.pdfPage = 6;
        anchors.Append(a);
        Vec<PtPageMappingSegment> segs;
        PtBuildPageMapping(anchors, &segs); // offset 3, 2 samples -> conf 0.8

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand("t1", PtPageLabelType::Arabic, 2, true, 1, 0, 0, 100));
        cands.Append(PtMakeCand("t2", PtPageLabelType::Compound, 12, false, 1, 0, 0, 100));
        cands.Append(PtMakeCand("t3", PtPageLabelType::RomanLower, 5, true, 1, 0, 0, 100));
        cands.Append(PtMakeCand("t4", PtPageLabelType::Arabic, 60, true, 1, 0, 0, 100));
        cands.Append(PtMakeCand("t5", PtPageLabelType::Arabic, 999, true, 1, 0, 0, 100));
        cands.Append(PtMakeCand("t6", PtPageLabelType::Arabic, 9, true, 1, 0, 0, 100));
        cands[5].resolvedPdfPage = 99; // already resolved: left untouched

        PtResolvePdfPages(segs, 50, &cands);
        utassert(cands[0].resolvedPdfPage.value_or(-1) == 5);
        utassert(cands[0].pageMappingConf == 0.8f);
        utassert(!cands[1].resolvedPdfPage.has_value()); // no ordinal
        utassert(!cands[2].resolvedPdfPage.has_value()); // no segment
        utassert(!cands[3].resolvedPdfPage.has_value()); // beyond nPdfPages
        utassert(!cands[4].resolvedPdfPage.has_value());
        utassert(cands[5].resolvedPdfPage.value_or(-1) == 99);
        // unknown document length: the segment bounds still apply, resolved
        // entries are not recomputed
        PtResolvePdfPages(segs, 0, &cands);
        utassert(!cands[3].resolvedPdfPage.has_value()); // outside segment range
        utassert(!cands[4].resolvedPdfPage.has_value());
        utassert(cands[0].resolvedPdfPage.value_or(-1) == 5); // unchanged
        PtFreeCands(cands);
    }
}

static void PtSimilarityTests() {
    utassert(PtTitleSimilarity("Chapter 1 Introduction", "Chapter 1 Introduction") == 1.0f);
    utassert(PtTitleSimilarity("Introduction", "INTRODUCTION") == 1.0f);           // case folded
    utassert(PtTitleSimilarity("1.1 C++ and .NET", "11 C and NET") == 1.0f);       // punctuation dropped
    utassert(PtTitleSimilarity(ZH_Q1 " " ZH_ZONG_LUN, ZH_Q1 ZH_ZONG_LUN) == 1.0f); // spaces dropped
    utassert(PtTitleSimilarity(nullptr, "abc") == 0.0f);
    utassert(PtTitleSimilarity("", "abc") == 0.0f);
    utassert(PtTitleSimilarity("abc", "xyz") == 0.0f);

    // LCS of "abcdef" / "abxdxf" is 4 -> 2*4/12
    float sim = PtTitleSimilarity("abcdef", "abxdxf");
    utassert(sim > 0.66f && sim < 0.67f);

    // 第一章总论 vs 第一章方法: shared prefix of 3 -> 2*3/10
    sim = PtTitleSimilarity(ZH_Q1 ZH_ZONG_LUN, ZH_Q1 ZH_FANG_FA);
    utassert(sim > 0.59f && sim < 0.61f);
}

// appends a body page with plain text tokens and reconstructs its lines
// (PtValidateEntriesWithBody consumes page.lines)
static void PtBuildBodyPages(Vec<PtPageData*>& bodyPages, int pageIndex, const char* line1, const char* line2) {
    PtPageData* bp = new PtPageData();
    bp->pageIndex = pageIndex;
    bp->width = 1410;
    bp->height = 2000;
    if (line1) {
        PtAddToken(*bp, line1, 100, 200, 500, 40);
    }
    if (line2) {
        PtAddToken(*bp, line2, 100, 300, 500, 40);
    }
    PtReconstructLines(*bp);
    bodyPages.Append(bp);
}

static void PtBodyValidationTests() {
    {
        // exact hit on the resolved page: full confidence
        Vec<PtPageData*> bodyPages;
        PtBuildBodyPages(bodyPages, 4, ZH_Q1 " " ZH_ZONG_LUN, "plain body text line");

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 1, true, 3, 0, 0, 230));
        cands[0].resolvedPdfPage = 4;
        cands.Append(PtMakeCand(ZH_Q2 " " ZH_FANG_FA, PtPageLabelType::Arabic, 12, true, 3, 0, 0, 230));
        cands[1].resolvedPdfPage = 44; // no body evidence within the radius

        PtValidateEntriesWithBody(bodyPages, &cands);
        utassert(cands[0].bodyValidationConf == 1.0f);
        utassert(*cands[0].resolvedPdfPage == 4); // confident mapping: not re-anchored
        // default (low) mapping conf counts as suspect and searches the whole
        // body; the different title only weakly resembles the body line, so
        // the entry stays below the match threshold and is not re-anchored.
        utassert(cands[1].bodyValidationConf < 0.55f);
        utassert(*cands[1].resolvedPdfPage == 44);
        PtFreeCands(cands);
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
    }
    {
        // far (but in-radius) hits are weighted 0.5
        Vec<PtPageData*> bodyPages;
        PtBuildBodyPages(bodyPages, 14, ZH_Q1 " " ZH_ZONG_LUN, nullptr);

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 1, true, 3, 0, 0, 230));
        cands[0].resolvedPdfPage = 10; // |14 - 10| = 4 > bodySearchNearPages
        cands[0].pageMappingConf = 0.6f;

        PtValidateEntriesWithBody(bodyPages, &cands);
        utassert(cands[0].bodyValidationConf == 0.5f);
        utassert(*cands[0].resolvedPdfPage == 10); // mapping not weak enough to re-anchor
        PtFreeCands(cands);
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
    }
    {
        // a strong hit on a different page overrides a weak mapping
        Vec<PtPageData*> bodyPages;
        PtBuildBodyPages(bodyPages, 12, ZH_Q1 " " ZH_ZONG_LUN, nullptr);

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 1, true, 3, 0, 0, 230));
        cands[0].resolvedPdfPage = 10;
        cands[0].pageMappingConf = 0.4f; // < 0.5 and best hit is >= 0.75

        PtValidateEntriesWithBody(bodyPages, &cands);
        utassert(*cands[0].resolvedPdfPage == 12);
        utassert(cands[0].bodyValidationConf == 1.0f);
        PtFreeCands(cands);
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
    }
    {
        // a suspect entry re-anchors onto a far heading page and restores its
        // printed ordinal via its own (still valid) mapping offset: ordinal
        // 14 + offset 6 predicted pdf 20, the true heading sits on pdf 150,
        // so the ordinal must be repaired to 150 - (20 - 14) = 144. The real
        // heading wraps onto two OCR lines, so only the adjacent-line join
        // (weighted 0.85) reaches the re-anchor bar.
        Vec<PtPageData*> bodyPages;
        PtBuildBodyPages(bodyPages, 150,
                         "\xe5\xae\x9e\xe7\x8e\xb0\xe4\xba\xba\xe7\x94\x9f\xe4\xbb\xb7\xe5\x80\xbc",  // 实现人生价值
                         "\xe7\x9a\x84\xe6\x9d\xa1\xe4\xbb\xb6\xe5\x92\x8c\xe9\x80\x94\xe5\xbe\x84"); // 的条件和途径

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(
            "\xe5\xae\x9e\xe7\x8e\xb0\xe4\xba\xba\xe7\x94\x9f\xe4\xbb\xb7\xe5\x80\xbc"
            "\xe7\x9a\x84\xe6\x9d\xa1\xe4\xbb\xb6\xe5\x92\x8c\xe9\x80\x94\xe5\xbe\x84", // 实现人生价值的条件和途径
            PtPageLabelType::Arabic, 14, true, 3, 0, 0, 230));
        cands[0].resolvedPdfPage = 20;
        cands[0].pageMappingConf = 0.3f; // flagged ordinal monotonicity break

        PtValidateEntriesWithBody(bodyPages, &cands);
        utassert(*cands[0].resolvedPdfPage == 150);
        utassert(cands[0].printedPage.ordinal == 144);
        utassert(cands[0].bodyValidationConf >= 0.75f);
        PtFreeCands(cands);
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
    }
    {
        // boxed sidebar headings interleave with body lines in y order, so
        // the two halves of the heading are NOT adjacent lines and only the
        // geometric line-pair join (close vertically, overlapping
        // horizontally) can reconstruct the title for re-anchoring
        Vec<PtPageData*> bodyPages;
        const char* kHead1 = "\xe5\xae\x9e\xe7\x8e\xb0\xe4\xba\xba\xe7\x94\x9f\xe4\xbb\xb7\xe5\x80\xbc"; // 实现人生价值
        const char* kHead2 = "\xe7\x9a\x84\xe6\x9d\xa1\xe4\xbb\xb6\xe5\x92\x8c\xe9\x80\x94\xe5\xbe\x84"; // 的条件和途径
        const char* kBody1 =
            "\xe6\x97\xa0\xe4\xba\xa7\xe9\x98\xb6\xe7\xba\xa7\xe7\x9a\x84\xe4\xba\xba\xe7\x94\x9f\xe4\xbb\xb7"
            "\xe5\x80\xbc\xe8\xa7\x82\xe7\x82\xb9\xe6\x97\xa2\xe7\x84\xb6"; // 无产阶级的人生价值观点既然
        const char* kBody2 =
            "\xe6\x98\xaf\xe4\xbb\xa5\xe4\xb8\xaa\xe4\xba\xba\xe5\xaf\xb9\xe7\xa4\xbe\xe4\xbc\x9a\xe7\x9a\x84"
            "\xe8\xb4\xa1\xe7\x8c\xae\xe4\xbd\x9c\xe4\xb8\xba\xe4\xb8\xbb\xe8\xa6\x81\xe6\xa0\x87"; // 是以个人对社会的贡献作为主要标
        PtPageData* bp = new PtPageData();
        bp->pageIndex = 150;
        bp->width = 1410;
        bp->height = 2000;
        PtAddToken(*bp, kBody1, 100, 200, 1200, 40);
        PtAddToken(*bp, kHead1, 600, 255, 300, 40);
        PtAddToken(*bp, kBody2, 100, 310, 1200, 40);
        PtAddToken(*bp, kHead2, 600, 365, 300, 40);
        PtReconstructLines(*bp);
        bodyPages.Append(bp);
        utassert(bp->lines.Size() == 4); // interleaving must survive line recon

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(
            "\xe5\xae\x9e\xe7\x8e\xb0\xe4\xba\xba\xe7\x94\x9f\xe4\xbb\xb7\xe5\x80\xbc"
            "\xe7\x9a\x84\xe6\x9d\xa1\xe4\xbb\xb6\xe5\x92\x8c\xe9\x80\x94\xe5\xbe\x84", // 实现人生价值的条件和途径
            PtPageLabelType::Arabic, 14, true, 3, 0, 0, 230));
        cands[0].resolvedPdfPage = 20;
        cands[0].pageMappingConf = 0.3f; // flagged ordinal monotonicity break

        PtValidateEntriesWithBody(bodyPages, &cands);
        utassert(*cands[0].resolvedPdfPage == 150);
        utassert(cands[0].printedPage.ordinal == 144);
        utassert(cands[0].bodyValidationConf >= 0.75f);
        PtFreeCands(cands);
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
    }
    {
        // TOC pages must never act as body evidence: the entry title appears
        // verbatim on its own source page (pageIndex == tocSourcePage), which
        // must not become the re-anchor target
        Vec<PtPageData*> bodyPages;
        PtBuildBodyPages(bodyPages, 3, ZH_Q1 " " ZH_ZONG_LUN, "another plain body line");

        Vec<PtEntryCandidate> cands;
        cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 1, true, 3, 0, 0, 230));
        cands[0].resolvedPdfPage = 10;
        cands[0].pageMappingConf = 0.3f; // suspect: searches the whole body

        PtValidateEntriesWithBody(bodyPages, &cands);
        utassert(*cands[0].resolvedPdfPage == 10); // TOC page excluded, no evidence
        utassert(cands[0].bodyValidationConf == 0.0f);
        PtFreeCands(cands);
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
    }
}

static void PtOrdinalMonotonicityTests() {
    // arabic ordinals must be non-decreasing; a dip is flagged with low
    // mapping conf so body validation re-anchors the entry
    Vec<PtEntryCandidate> cands;
    cands.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::Arabic, 135, true, 3, 0, 0, 230));
    cands[0].pageMappingConf = 0.8f;
    cands.Append(PtMakeCand(ZH_Q2 " " ZH_FANG_FA, PtPageLabelType::Arabic, 14, true, 3, 0, 0, 260));
    cands[1].pageMappingConf = 0.8f; // 14 < 135: must be flagged
    cands.Append(PtMakeCand(ZH_Q3 " " ZH_CAN_KAO, PtPageLabelType::Arabic, 150, true, 3, 0, 0, 290));
    cands[2].pageMappingConf = 0.8f;

    PtFlagOrdinalMonotonicityBreaks(&cands);
    utassert(cands[0].pageMappingConf == 0.8f);
    utassert(cands[1].pageMappingConf == 0.3f);
    utassert(cands[2].pageMappingConf == 0.8f);
    PtFreeCands(cands);

    // roman front matter does not participate: an arabic restart after it is
    // legitimate (front matter numbered separately from the body)
    Vec<PtEntryCandidate> cands2;
    cands2.Append(PtMakeCand(ZH_Q1 " " ZH_ZONG_LUN, PtPageLabelType::RomanUpper, 4, true, 3, 0, 0, 230));
    cands2[0].pageMappingConf = 0.9f;
    cands2.Append(PtMakeCand(ZH_Q2 " " ZH_FANG_FA, PtPageLabelType::Arabic, 1, true, 3, 0, 0, 260));
    cands2[1].pageMappingConf = 0.9f;
    PtFlagOrdinalMonotonicityBreaks(&cands2);
    utassert(cands2[0].pageMappingConf == 0.9f);
    utassert(cands2[1].pageMappingConf == 0.9f);
    PtFreeCands(cands2);
}

static void PtDocumentDriverTests() {
    {
        // two TOC fixtures + a rejected body page; explicit footer anchors
        Vec<PtPageData*> pages;
        pages.Append(new PtPageData());
        pages.Append(new PtPageData());
        pages.Append(new PtPageData());
        pages[0]->pageIndex = 1;
        pages[0]->width = 1410;
        pages[0]->height = 2000;
        for (int i = 0; i < 6; i++) {
            PtAddToken(*pages[0],
                       "\xe8\xbf\x99\xe6\x98\xaf\xe6\xad\xa3\xe6\x96\x87\xe5\x86\x85\xe5\xae\xb9\xe7\x94\xa8\xe4\xba"
                       "\x8e\xe9\xaa\x8c\xe8\xaf\x81\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8", // 这是正文内容用于验证驱动器
                       100, 100 + i * 60, 1200, 36);
        }
        PtLoadFixture("zh-single-leader-p3.json", pages[1]);
        PtLoadFixture("zh-two-col-p5.json", pages[2]);

        Vec<PtPageAnchor> anchors;
        PtPageAnchor a;
        a.type = PtPageLabelType::Arabic;
        a.printed = 1;
        a.pdfPage = 4; // offset 3 for the whole arabic range below
        anchors.Append(a);
        a.printed = 12;
        a.pdfPage = 15;
        anchors.Append(a);
        a.printed = 41;
        a.pdfPage = 44;
        anchors.Append(a);
        a.printed = 95;
        a.pdfPage = 98;
        anchors.Append(a);

        Vec<PtPageData*> bodyPages;
        PtBuildBodyPages(bodyPages, 4, ZH_Q1 " " ZH_ZONG_LUN, nullptr);

        PtDocTocInput in;
        in.pages = &pages;
        in.nPdfPages = 200;
        in.anchors = &anchors;
        in.bodyPages = &bodyPages;

        Vec<PtEntryCandidate> out;
        Vec<PtocOverlayPage*> overlay;
        bool ok = PtBuildDocumentToc(in, &out, &overlay);
        utassert(ok);
        utassert(out.Size() == 16); // 6 + 10 entries; heading dropped
        utassert(overlay.Size() == 3);
        utassert(overlay[0]->pageNo == 1 && !overlay[0]->isTocPage);
        utassert(overlay[1]->pageNo == 3 && overlay[1]->isTocPage && overlay[1]->entries.Size() == 6);
        utassert(overlay[2]->pageNo == 5 && overlay[2]->isTocPage);

        if (out.Size() == 16) {
            utassert(str::Eq(out[0].title, ZH_Q1 " " ZH_ZONG_LUN));
            utassert(out[0].level == 1);
            utassert(out[0].printedPage.ordinal == 1);
            utassert(out[0].resolvedPdfPage.value_or(-1) == 4);
            utassert(out[0].pageMappingConf == 1.0f); // 4 samples, unanimous vote
            utassert(out[0].bodyValidationConf == 1.0f);
            utassert(str::StartsWith(out[1].title, ZH_S1));
            utassert(out[1].level == 2);
            utassert(out[1].resolvedPdfPage.value_or(-1) == 6); // printed 3 -> pdf 6
            // body page 4 is in radius: only a weak partial title overlap
            utassert(out[1].bodyValidationConf < 0.5f);
            // last entry of the single-column page: printed 41 -> pdf 44
            utassert(out[5].printedPage.ordinal == 41);
            utassert(out[5].resolvedPdfPage.value_or(-1) == 44);
            // right column of page 5 survives the cross-page stages
            utassert(str::Eq(out[15].title, ZH_CAN_KAO));
            utassert(out[15].printedPage.ordinal == 95);
            utassert(out[15].resolvedPdfPage.value_or(-1) == 98);
        }
        PtFreeCands(out);
        for (int i = 0; i < pages.Size(); i++) {
            delete pages[i];
        }
        for (int i = 0; i < bodyPages.Size(); i++) {
            delete bodyPages[i];
        }
        for (int i = 0; i < overlay.Size(); i++) {
            delete overlay[i];
        }
    }
    {
        // without anchors, a seed from the TOC extent maps printed 1 to the
        // page after the last TOC page, with low confidence
        Vec<PtPageData*> pages;
        pages.Append(new PtPageData());
        PtLoadFixture("zh-single-leader-p3.json", pages[0]);

        PtDocTocInput in;
        in.pages = &pages;
        in.nPdfPages = 100;

        Vec<PtEntryCandidate> out;
        bool ok = PtBuildDocumentToc(in, &out, nullptr);
        utassert(ok);
        utassert(out.Size() == 6);
        if (out.Size() == 6) {
            utassert(out[0].printedPage.ordinal == 1);
            utassert(out[0].resolvedPdfPage.value_or(-1) == 4); // lastToc(3) + 1
            utassert(out[0].pageMappingConf == 0.5f);           // single-seed anchor
            utassert(out[3].resolvedPdfPage.value_or(-1) == 15);
        }
        PtFreeCands(out);
        delete pages[0];
    }
    {
        // no TOC pages -> no TOC
        Vec<PtPageData*> pages;
        pages.Append(new PtPageData());
        pages[0]->pageIndex = 1;
        pages[0]->width = 1410;
        pages[0]->height = 2000;
        for (int i = 0; i < 6; i++) {
            PtAddToken(*pages[0],
                       "\xe8\xbf\x99\xe6\x98\xaf\xe6\xad\xa3\xe6\x96\x87\xe5\x86\x85\xe5\xae\xb9\xe7\x94\xa8\xe4\xba"
                       "\x8e\xe9\xaa\x8c\xe8\xaf\x81\xe9\xa9\xb1\xe5\x8a\xa8\xe5\x99\xa8",
                       100, 100 + i * 60, 1200, 36);
        }
        PtDocTocInput in;
        in.pages = &pages;
        Vec<PtEntryCandidate> out;
        utassert(!PtBuildDocumentToc(in, &out, nullptr));
        utassert(out.Size() == 0);
        delete pages[0];
    }
}

// Debug harness (env-gated, not part of CI): set SUMATRA_PTOC_LOAD_DIR to a
// "<pdf>.ptoc" directory with ocr-p*.json dumps to run the PtBuildDocumentToc
// stage sequence over a real book's captured OCR pages. Each stage (and each
// page of the parse loop) is logged to <dir>\..\e2e-stage.log with an explicit
// flush, so a heap corruption caught by the CRT is localized to a stage.
static void E2eStageLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, dimof(buf), fmt, args);
    va_end(args);
    FILE* f = fopen("c:/src/sumatrapdf/tmp/ptoc-book1/e2e-stage.log", "a");
    if (f) {
        fprintf(f, "%s", buf);
        fclose(f);
    }
}

static void PtBookLoadDebugTests() {
    char dir[512]{};
    if (GetEnvironmentVariableA("SUMATRA_PTOC_LOAD_DIR", dir, dimof(dir)) == 0 || !dir[0]) {
        return;
    }
    DeleteFileA("c:/src/sumatrapdf/tmp/ptoc-book1/e2e-stage.log");
    Vec<PtPageData*> pages;
    for (int p = 1; p < 2000; p++) {
        TempStr jsonPath = path::JoinTemp(dir, str::FormatTemp("ocr-p%d.json", p));
        if (!file::Exists(jsonPath)) {
            break;
        }
        E2eStageLog("load p%d start\n", p);
        PtPageData* page = new PtPageData();
        if (!PtocLoadOcrPageJson(jsonPath, page)) {
            E2eStageLog("load p%d FAILED\n", p);
            delete page;
            continue;
        }
        pages.Append(page);
        PtPageData& rp = *page;
        if (rp.tokens.Size() > 0) {
            E2eStageLog("recon p%d start\n", p);
            PtReconstructLines(rp);
            E2eStageLog("recon p%d done: %d lines\n", p, rp.lines.Size());
        }
        E2eStageLog("load p%d done: %d lines\n", p, page->lines.Size());
    }
    E2eStageLog("load done: %d pages\n", pages.Size());
    utassert(pages.Size() > 0);

    const PrintedTocConfig& cfg = PtocConfig();
    Vec<PtEntryCandidate> all;
    for (int i = 0; i < pages.Size(); i++) {
        PtPageData& page = *pages[i];
        E2eStageLog("parse p%d start\n", page.pageIndex);
        if (page.pageIndex == 14) {
            for (int li2 = 0; li2 < page.lines.Size(); li2++) {
                const PtRecLine& rl = *page.lines[li2];
                E2eStageLog("  L%d nt=%d t0=%s\n", li2, rl.tokens.Size(),
                            rl.tokens.Size() > 0 && rl.tokens[0].text ? rl.tokens[0].text : "(null)");
            }
        }
        PtTocPageScores sc = PtScoreTocPage(page);
        E2eStageLog("  scored %.2f\n", (double)sc.total);
        bool accepted = sc.total >= cfg.pageScoreMin;
        if (!accepted) {
            continue;
        }
        Vec<PtEntryCandidate> cands;
        PtParseEntryCandidates(page, &cands);
        E2eStageLog("  parsed %d cands\n", cands.Size());
        PtMergeWrappedEntries(page, &cands, /*keepTopOrphans=*/true);
        E2eStageLog("  merged %d cands\n", cands.Size());
        PtAssignHierarchyLevels(page, &cands);
        for (int k = 0; k < cands.Size(); k++) {
            all.Append(cands[k]); // steal strings
        }
        cands.Reset();
        E2eStageLog("  accepted: total=%d cands\n", all.Size());
    }
    E2eStageLog("parse loop done: %d candidates\n", all.Size());
    if (all.Size() == 0) {
        return;
    }

    PtMergeCrossPage(&all);
    E2eStageLog("cross-page merge done: %d candidates\n", all.Size());
    if (all.Size() == 0) {
        return;
    }
    PtNormalizeLevelsAcrossPages(&all);
    PtPromoteChapterHeadings(&all);
    E2eStageLog("normalize levels done: %d candidates\n", all.Size());

    int nPdfPages = pages.Size();
    int lastToc = 0;
    for (int i = 0; i < all.Size(); i++) {
        if (all[i].tocSourcePage > lastToc) {
            lastToc = all[i].tocSourcePage;
        }
    }
    Vec<PtPageAnchor> seedAnchors;
    PtPageAnchor a;
    a.type = PtPageLabelType::Arabic;
    a.printed = 1;
    a.pdfPage = lastToc + 1;
    a.conf = 0.3f;
    seedAnchors.Append(a);
    Vec<PtPageMappingSegment> segs;
    PtBuildPageMapping(seedAnchors, &segs);
    E2eStageLog("page mapping done: %d segments\n", segs.Size());
    for (int i = 0; i < segs.Size(); i++) {
        segs[i].printedEnd = nPdfPages > 0 ? nPdfPages - segs[i].offset : INT_MAX - segs[i].offset;
    }
    PtResolvePdfPages(segs, nPdfPages, &all);
    PtFlagOrdinalMonotonicityBreaks(&all);
    E2eStageLog("resolve pdf pages done\n");

    PtValidateEntriesWithBody(pages, &all);
    E2eStageLog("body validation done\n");

    int withPage = 0;
    for (int i = 0; i < all.Size(); i++) {
        if (all[i].resolvedPdfPage.has_value()) {
            withPage++;
        }
    }
    E2eStageLog("final: %d entries, %d with pdf page\n", all.Size(), withPage);
    for (int i = 0; i < all.Size(); i++) {
        const PtEntryCandidate& c = all[i];
        int pdfPage = c.resolvedPdfPage.has_value() ? *c.resolvedPdfPage : 0;
        int printed = c.printedPage.hasOrdinal ? c.printedPage.ordinal : 0;
        E2eStageLog("e: lvl=%d pdf=%d printed=%d tocPage=%d conf=%.2f %s\n", (int)c.level, pdfPage, printed,
                    c.tocSourcePage, (double)c.parseConf, c.title ? c.title : "");
    }
    for (int i = 0; i < all.Size(); i++) {
        all[i].Free();
    }
    for (int i = 0; i < pages.Size(); i++) {
        delete pages[i];
    }
}

// Documents Vec copy semantics for structs containing Vecs (e.g. PtPageData):
// Vec<T>::Append(el) memberwise-copies the element, so one level of inner
// Vecs is re-owned safely - but Vec<T>::operator=/copy-ctor memcpy the whole
// ELEMENT ARRAY, so inner Vec members of elements keep pointing into the
// source's storage. Never copy a struct that already holds rebuilt inner
// Vecs (e.g. reconstructed lines); transfer tokens-only pages, then rebuild
// in place inside the destination array.
static void PtVecCopySemanticsTest() {
    Vec<PtPageData> pages;
    {
        PtPageData a;
        a.pageIndex = 1;
        a.width = 100;
        PtToken t;
        t.text = str::Dup("hello");
        t.box = PtBoxF{0, 0, 10, 10};
        a.tokens.Append(t);
        pages.Append(a);
    } // local `a` destructs here; its token array is freed, strings live on
    utassert(pages.Size() == 1);
    utassert(pages[0].tokens.Size() == 1);
    utassert(str::Eq(pages[0].tokens[0].text, "hello"));
    // rebuild lines in place inside the destination array (pipeline pattern)
    PtReconstructLines(pages[0]);
    utassert(pages[0].lines.Size() == 1);
    utassert(pages[0].lines[0]->tokens.Size() == 1);
    utassert(str::Eq(pages[0].lines[0]->tokens[0].text, "hello"));
    pages[0].Free();
    pages.Reset();
}

static void PtCaptureStoreTests() {
    Vec<OcrBox> boxes;
    MakeSampleBoxes(boxes);

    // capture two pages and read them back as token pages
    PtocCaptureOcrPage("C:\\fake\\cap.pdf", 3, 1410, 2000, boxes);
    PtocCaptureOcrPage("C:\\fake\\cap.pdf", 1, 1410, 2000, boxes);
    Vec<PtPageData*> pages;
    utassert(PtocCaptureCopyFor("C:\\fake\\cap.pdf", &pages));
    utassert(pages.Size() == 2);
    if (pages.Size() == 2) {
        // ascending pageNo; tokens mirror the boxes (image space)
        utassert(pages[0]->pageIndex == 1 && pages[1]->pageIndex == 3);
        utassert(pages[1]->width == 1410 && pages[1]->height == 2000);
        utassert(pages[1]->tokens.Size() == 2);
        if (pages[1]->tokens.Size() == 2) {
            utassert(str::Eq(pages[1]->tokens[0].text, boxes[0].text));
            utassert(pages[1]->tokens[0].box.x1 == 1170);
        }
    }
    for (int i = 0; i < pages.Size(); i++) {
        delete pages[i];
    }
    pages.Reset();

    // recapturing page 1 restarts the capture; an unknown path is empty
    PtocCaptureOcrPage("C:\\fake\\cap.pdf", 1, 900, 1200, boxes);
    utassert(PtocCaptureCopyFor("C:\\fake\\cap.pdf", &pages));
    utassert(pages.Size() == 1 && pages[0]->pageIndex == 1);
    if (pages.Size() == 1) {
        utassert(pages[0]->width == 900);
    }
    for (int i = 0; i < pages.Size(); i++) {
        delete pages[i];
    }
    pages.Reset();
    utassert(!PtocCaptureCopyFor("C:\\fake\\other.pdf", &pages));

    // forget drops only the named document's capture
    PtocCaptureOcrPage("C:\\fake\\cap2.pdf", 2, 1410, 2000, boxes);
    PtocCaptureForget("C:\\fake\\cap.pdf");
    utassert(!PtocCaptureCopyFor("C:\\fake\\cap.pdf", &pages));
    utassert(PtocCaptureCopyFor("C:\\fake\\cap2.pdf", &pages));
    utassert(pages.Size() == 1);
    for (int i = 0; i < pages.Size(); i++) {
        delete pages[i];
    }
    pages.Reset();
    PtocCaptureClear();
    utassert(!PtocCaptureCopyFor("C:\\fake\\cap2.pdf", &pages));

    for (int i = 0; i < boxes.Size(); i++) {
        str::Free(boxes[i].text);
        free(boxes[i].charX);
    }
}

void PrintedTocModel_UnitTests() {
    PtJsonBufTests();
    PtBoxFTests();
    DumpRoundTripTest();
    FixtureTests();
    OverlayStoreTests();
    PtPageLabelTests();
    PtReconstructTests();
    PtScoreTests();
    PtCandidateTests();
    PtMergeTests();
    PtCrossPageMergeTests();
    PtGlobalLevelTests();
    PtPageMappingTests();
    PtSimilarityTests();
    PtBodyValidationTests();
    PtOrdinalMonotonicityTests();
    PtDocumentDriverTests();
    PtCaptureStoreTests();
    PtVecCopySemanticsTest();
    PtBookLoadDebugTests();
}
