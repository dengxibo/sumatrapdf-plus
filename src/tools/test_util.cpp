#include "utils/BaseUtil.h"
#include "utils/WinDynCalls.h"
#include "utils/FileUtil.h"
#include "PrintedTocPageDetector.h"
#include "EpubMediaOverlay.h"
#include "utils/Bm25.h"
#include "utils/SearchText.h"

// must be last due to assert() over-write
#include "utils/UtAssert.h"

// in src/util/tests/UtilTests.cpp
extern void BaseUtils_UnitTests();

// in src/UnitTests.cpp
extern void SumatraPDF_UnitTests();

extern void BaseUtilTest();
extern void ByteOrderTests();
extern void CryptoUtilTest();
extern void CssParser_UnitTests();
extern void FreeTextMru_UnitTests();
extern void DictTest();
extern void FileUtilTest();
extern void HtmlPrettyPrintTest();
extern void HtmlPullParser_UnitTests();
extern void JsonTest();
extern void OcrTextMerge_UnitTests();
extern void LookupTtsVoice_UnitTests();
extern void EbookImagePaper_UnitTests();
extern void FlattenedCutout_UnitTests();
extern void SettingsUtilTest();
extern void SimpleLogTest();
extern void SquareTreeTest();
extern void StrFormatTest();
extern void StrTest();
extern void TrivialHtmlParser_UnitTests();
extern void PdfDarkModeOklab_UnitTests();
extern void PdfDarkModeV2_UnitTests();
extern void PdfJoinSplitImages_UnitTests();
extern void PdfDarkModeImageClassifier_UnitTests();
extern void PdfTocEditModel_UnitTests();
extern void PrintedTocModel_UnitTests();
extern void TtsPronunciation_UnitTests();
extern void EpubMediaOverlay_UnitTests();
extern void InlineTranslateLang_UnitTests();
extern void VecTest();
extern void WinUtilTest();
extern void StrFormatTest();
extern void StrVecTest();

void GetPrintersInfo(struct StrBuilder&) {
    /* stub: do nothing */
}

void MaybeDelayedWarningNotification(const char*, ...) {
    // a stub to make this compile
}

// Media Overlays timeline of an unpacked EPUB directory, for checking the parser against real books.
static int EpubMoDumpUnpacked(const char* dir) {
    ByteSlice container = file::ReadFile(path::JoinTemp(dir, "META-INF", "container.xml"));
    char* opfPath = container ? EpubParseContainerXml((const char*)container.data(), container.size()) : nullptr;
    container.Free();
    if (!opfPath) {
        printf("no META-INF/container.xml rootfile\n");
        return 1;
    }
    ByteSlice opf = file::ReadFile(path::JoinTemp(dir, opfPath));
    EpubPackage pkg;
    bool ok = opf && EpubParsePackage(&pkg, opfPath, (const char*)opf.data(), opf.size());
    opf.Free();
    str::Free(opfPath);
    if (!ok) {
        printf("cannot parse package document\n");
        return 1;
    }
    for (EpubMoDocument* doc : pkg.overlays) {
        ByteSlice smil = file::ReadFile(path::JoinTemp(dir, doc->smilPath));
        if (!smil || !EpubParseSmil(&pkg, doc, (const char*)smil.data(), smil.size())) {
            doc->failed = true;
            pkg.AddDiag("cannot read or parse %s", doc->smilPath);
        }
        smil.Free();
    }
    StrBuilder out;
    EpubMoDumpTimeline(&pkg, out);
    fwrite(out.CStr(), 1, out.size(), stdout);
    return 0;
}

static void Bm25SearchTests() {
    auto tokenize = [](const char* text, Vec<Bm25Token>& tokens) {
        Vec<int> chars;
        int bytes = (int)str::Len(text);
        for (int i = 0; i < bytes;) {
            chars.Append(Utf8CodepointNext(text, bytes, i));
        }
        Bm25Tokenize(chars.els, chars.Size(), tokens);
    };
    Vec<Bm25Token> query, fullName, unrelated, latin, wrapped;
    tokenize("社保卡", query);
    tokenize("请携带社会保障卡办理相关业务。", fullName);
    tokenize("购买明信片和银行卡。", unrelated);
    tokenize("Social SECURITY card", latin);
    tokenize("社会保\n障卡", wrapped);
    int recalled = 0, noise = 0;
    for (auto term : query) {
        if (!term.primary) continue;
        bool inFullName = false, inNoise = false;
        for (auto token : fullName) inFullName |= token.key == term.key;
        for (auto token : unrelated) inNoise |= token.key == term.key;
        recalled += inFullName;
        noise += inNoise;
    }
    utassert(recalled == 3 && noise == 1);
    Vec<Bm25Token> exact;
    tokenize("社保卡补办指南", exact);
    Vec<Bm25Token>* passages[] = {&fullName, &exact, &unrelated};
    int tf[3][64]{}, df[64]{}, lengths[3]{};
    for (int p = 0; p < 3; p++) {
        for (auto token : *passages[p]) {
            lengths[p] += token.primary;
            for (int t = 0; t < query.Size(); t++) tf[p][t] += token.key == query[t].key;
        }
        for (int t = 0; t < query.Size(); t++) df[t] += tf[p][t] > 0;
    }
    float avg = (lengths[0] + lengths[1] + lengths[2]) / 3.f;
    float fullScore = Bm25PassageScore(query, tf[0], df, 3, lengths[0], avg);
    float exactScore = Bm25PassageScore(query, tf[1], df, 3, lengths[1], avg);
    float noiseScore = Bm25PassageScore(query, tf[2], df, 3, lengths[2], avg);
    utassert(exactScore > fullScore && fullScore > 0 && noiseScore == 0);
    utassert(Bm25PhraseOffset(exact, query) == 0);
    utassert(Bm25PhraseOffset(fullName, query) == -1); // recall is not literal equality
    utassert(latin.Size() == 3);                       // English words, not matching individual letters.
    Vec<Bm25Token> lower;
    tokenize("social security CARD", lower);
    for (int i = 0; i < latin.Size(); i++) utassert(latin[i].key == lower[i].key);
    utassert(Bm25PhraseOffset(latin, lower) == 0);
    Vec<Bm25Token> repeatQuery, repeatPassage;
    tokenize("考试考试", repeatQuery);
    tokenize("需要考试考试之后", repeatPassage);
    utassert(Bm25PhraseOffset(repeatPassage, repeatQuery) == 2);
    utassert(Bm25PhraseOffset(exact, repeatQuery) == -1);
    Vec<Bm25Token> unwrapped;
    tokenize("社会保障卡", unwrapped);
    utassert(wrapped.Size() == unwrapped.Size());
    for (int i = 0; i < wrapped.Size(); i++) utassert(wrapped[i].key == unwrapped[i].key);
    int reverseTf[64]{}, reverseDf[64]{};
    for (int t = 0; t < unwrapped.Size(); t++) {
        for (auto token : exact) reverseTf[t] += token.key == unwrapped[t].key;
        reverseDf[t] = reverseTf[t] > 0;
    }
    utassert(Bm25PassageScore(unwrapped, reverseTf, reverseDf, 3, lengths[1], avg) > 0);
    utassert(Bm25TermScore(1, 1, 100, 20, 100) > Bm25TermScore(1, 1, 100, 200, 100));
    utassert(Bm25TermScore(1, 1, 100, 50, 100) > Bm25TermScore(1, 90, 100, 50, 100));
    utassert(Bm25TermScore(2, 1, 100, 50, 100) > Bm25TermScore(1, 1, 100, 50, 100));
}

static void SearchTextTests() {
    utassert(SearchTextIsHanQuery(L"考试"));
    utassert(SearchTextIsHanQuery(L"考\u3000试"));
    utassert(SearchTextIsHanQuery(L"\U00020000试"));
    utassert(!SearchTextIsHanQuery(L"考试PDF"));
    utassert(!SearchTextIsHanQuery(L"考试2026"));
    utassert(!SearchTextIsHanQuery(L"test"));
    utassert(!SearchTextIsHanQuery(L" \t"));
    utassert(SearchTextIsWhitespace(0x3000));
    utassert(SearchTextIsWhitespace(0xa0));
    utassert(SearchTextIsWhitespace(0x202f));
    utassert(SearchTextIsWhitespace('\n'));
    utassert(!SearchTextIsWhitespace(0x9009)); // 选 must not become a tab.
    utassert(!SearchTextIsWhitespace(0x8bd5)); // 试
    utassert(!SearchTextIsWhitespace(0x20000));
}

int main(int argc, char** argv) {
    if (argc == 2 && str::Eq(argv[1], "--search-text-test")) {
        SearchTextTests();
        printf("Search text tests passed\n");
        return 0;
    }
    if (argc == 2 && str::Eq(argv[1], "--bm25-test")) {
        Bm25SearchTests();
        printf("BM25 search tests passed\n");
        return 0;
    }
    if (argc == 5 && str::Eq(argv[1], "--toc-page-replay")) {
        Vec<TocPageFeatures> features;
        int count = atoi(argv[3]);
        for (int p = 1; p <= count; p++) {
            PtPageData page;
            TempStr input = path::JoinTemp(argv[2], str::FormatTemp("ocr-p%d.json", p));
            if (PtocLoadOcrPageJson(input, &page)) {
                auto f = MeasureTocPage(page, count);
                auto old = PtProcessPage(page, nullptr, nullptr);
                printf(
                    "page=%d legacy=%.3f keyword=%.2f anchors=%.3f alignment=%.3f indent=%.3f new=%.3f entries=%d "
                    "paragraph=%.3f\n",
                    p, old.total, old.tocKeywordScore, old.endingPageNumberRatio, old.pageNumberXAlignment,
                    old.indentationBandScore, f.raw, f.entries, f.paragraph);
                features.Append(f);
            }
            page.Free();
        }
        auto interval = DetectTocPageInterval(features);
        WriteTocPageDiagnostics(argv[4], features, interval);
        return 0;
    }
    if (argc == 3 && str::Eq(argv[1], "--epub-mo-dump")) {
        return EpubMoDumpUnpacked(argv[2]);
    }
    printf("Running unit tests\n");
    fflush(stdout);

    InitDynCalls();
    BaseUtilTest();
    ByteOrderTests();
    CryptoUtilTest();
    CssParser_UnitTests();
    FreeTextMru_UnitTests();
    DictTest();
    FileUtilTest();
    HtmlPrettyPrintTest();
    HtmlPullParser_UnitTests();
    JsonTest();
    OcrTextMerge_UnitTests();
    LookupTtsVoice_UnitTests();
    EbookImagePaper_UnitTests();
    FlattenedCutout_UnitTests();
    SettingsUtilTest();
    SimpleLogTest();
    SquareTreeTest();
    StrFormatTest();
    StrTest();
    SearchTextTests();
    Bm25SearchTests();
    StrVecTest();
    TrivialHtmlParser_UnitTests();
    PdfDarkModeOklab_UnitTests();
    PdfDarkModeV2_UnitTests();
    PdfJoinSplitImages_UnitTests();
    PdfDarkModeImageClassifier_UnitTests();
    PdfTocEditModel_UnitTests();
    PrintedTocModel_UnitTests();
    TtsPronunciation_UnitTests();
    EpubMediaOverlay_UnitTests();
    InlineTranslateLang_UnitTests();
    VecTest();
    WinUtilTest();
    SumatraPDF_UnitTests();

    int res = utassert_print_results();
    DestroyTempAllocator();
    return res;
}
