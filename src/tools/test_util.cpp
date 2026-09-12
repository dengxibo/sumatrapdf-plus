#include "utils/BaseUtil.h"
#include "utils/WinDynCalls.h"
#include "utils/FileUtil.h"
#include "PrintedTocPageDetector.h"

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
extern void DictTest();
extern void FileUtilTest();
extern void HtmlPrettyPrintTest();
extern void HtmlPullParser_UnitTests();
extern void JsonTest();
extern void OcrTextMerge_UnitTests();
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

int main(int argc, char** argv) {
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
    printf("Running unit tests\n");
    fflush(stdout);

    InitDynCalls();
    BaseUtilTest();
    ByteOrderTests();
    CryptoUtilTest();
    CssParser_UnitTests();
    DictTest();
    FileUtilTest();
    HtmlPrettyPrintTest();
    HtmlPullParser_UnitTests();
    JsonTest();
    OcrTextMerge_UnitTests();
    SettingsUtilTest();
    SimpleLogTest();
    SquareTreeTest();
    StrFormatTest();
    StrTest();
    StrVecTest();
    TrivialHtmlParser_UnitTests();
    PdfDarkModeOklab_UnitTests();
    PdfDarkModeV2_UnitTests();
    PdfJoinSplitImages_UnitTests();
    PdfDarkModeImageClassifier_UnitTests();
    PdfTocEditModel_UnitTests();
    PrintedTocModel_UnitTests();
    TtsPronunciation_UnitTests();
    VecTest();
    WinUtilTest();
    SumatraPDF_UnitTests();

    int res = utassert_print_results();
    DestroyTempAllocator();
    return res;
}
