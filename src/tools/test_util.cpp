#include "utils/BaseUtil.h"
#include "utils/WinDynCalls.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"
#include "utils/Log.h"
#include "EdgeSpeechResponse.h"
#include "EdgeSpeechTransport.h"
#include "LookupAudio.h"
#include "../../ext/edge-gec/EdgeGec.h"
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
extern void EdgeSpeechResponse_UnitTests();
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

struct EdgePrefixProbe {
    struct Sample {
        char* wav;
        DWORD size;
    };
    Vec<Sample> samples;
    ULONGLONG started = 0;
    char* wav = nullptr;
    DWORD size = 0;
    HANDLE cancel = nullptr;
    int nextProbe = 2000;
};

static void ProbeEdgePrefix(void* context, EdgeSpeechResponse& response) {
    auto* probe = (EdgePrefixProbe*)context;
    if (probe->samples.Size() >= 6 || response.mp3.Size() < probe->nextProbe) {
        return;
    }
    probe->nextProbe = response.mp3.Size() + 4000;
    probe->wav = DecodeSpeechMp3ToWave((const u8*)response.mp3.Get(), response.mp3.size(), probe->cancel, &probe->size);
    if (probe->wav) {
        probe->samples.Append({probe->wav, probe->size});
        if (probe->samples.Size() == 1)
            printf("Edge first decoded audio after %llu ms, %lu WAV bytes\n", GetTickCount64() - probe->started,
                   probe->size);
    }
}

static int ProbeEdgeSpeech(const char* output, const EdgeReadAloudVoice& edgeVoice) {
    const char* voice = edgeVoice.shortName;
    const char* text = "这是一段中英混读测试。The same voice reads both languages.";
    if (edgeVoice.language == EdgeVoiceLanguage::Chinese) {
        text = "这是一段中文语音试听。";
    } else if (edgeVoice.language == EdgeVoiceLanguage::English) {
        text = "This is how the selected English voice sounds.";
    }
    if (const char* sample = GetEnvVariableTemp("SUMATRA_EDGE_PROBE_TEXT"); !str::IsEmpty(sample)) {
        text = sample;
    }
    const char* resource = GetEnvVariableTemp("SUMATRA_EDGE_PROBE_RESOURCE");
    const char* headers = GetEnvVariableTemp("SUMATRA_EDGE_PROBE_HEADERS");
    WStrBuilder requestTarget;
    if (!str::IsEmpty(resource)) {
        requestTarget.Append(ToWStrTemp(resource));
    }
    if (!str::IsEmpty(resource) && str::Contains(resource, "{Sec-MS-GEC}")) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER ticks;
        ticks.LowPart = now.dwLowDateTime;
        ticks.HighPart = now.dwHighDateTime;
        char validation[82];
        EdgeGec::Generate(ticks.QuadPart, validation);
        str::Replace(requestTarget, L"{Sec-MS-GEC}", ToWStrTemp(validation));
    }
    EdgeSpeechResponse response(text);
    ULONGLONG start = GetTickCount64();
    HRESULT prefixCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    EdgePrefixProbe prefix;
    prefix.started = start;
    prefix.cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    defer {
        for (const auto& sample : prefix.samples) free(sample.wav);
        if (prefix.cancel) CloseHandle(prefix.cancel);
        if (SUCCEEDED(prefixCom)) CoUninitialize();
    };
    if (SUCCEEDED(prefixCom) && prefix.cancel) {
        response.audioContext = &prefix;
        response.audioReceived = ProbeEdgePrefix;
    }
    auto result = str::IsEmpty(resource) ? EdgeSpeechSynthesizeOnline(text, voice, 1.0f, nullptr, response)
                                         : EdgeSpeechSynthesize(requestTarget.Get(), ToWStrTemp(headers), text, voice,
                                                                1.0f, nullptr, response);
    printf("Edge HTTP %lu, WinHTTP error %lu, elapsed %llu ms, audio %d bytes, word boundaries %d\n", result.httpStatus,
           result.error, GetTickCount64() - start, response.mp3.Size(), response.words.Size());
    if (result.error != ERROR_SUCCESS) {
        return 1;
    }
    if (str::EndsWithI(output, ".wav")) {
        HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            return 2;
        }
        defer {
            CoUninitialize();
        };
        HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!cancel) {
            return 2;
        }
        defer {
            CloseHandle(cancel);
        };
        DWORD size = 0;
        char* wav = DecodeSpeechMp3ToWave((const u8*)response.mp3.Get(), response.mp3.size(), cancel, &size);
        defer {
            free(wav);
        };
        for (const auto& sample : prefix.samples) {
            if (sample.wav && wav && sample.size > 44 + 2304 && size >= sample.size) {
                DWORD bytes = sample.size - 44 - 2304;
                if (memcmp(sample.wav + 44, wav + 44, bytes) != 0) {
                    fprintf(stderr, "Edge prefix differs from full decode\n");
                    return 3;
                }
                printf("Edge streaming splice verified for %lu PCM bytes\n", bytes);
            }
        }
        return wav && file::WriteFile(output, ByteSlice((const u8*)wav, size)) ? 0 : 2;
    }
    return file::WriteFile(output, response.mp3.AsByteSlice()) ? 0 : 2;
}

int main(int argc, char** argv) {
    if ((argc == 3 || argc == 4) && str::Eq(argv[1], "--edge-speech-probe")) {
        const auto* voice = argc == 4 ? FindEdgeReadAloudVoice(argv[3]) : &gEdgeReadAloudVoices[0];
        if (!voice) {
            fprintf(stderr, "Unknown Edge voice: %s\n", argv[3]);
            return 1;
        }
        gLogToConsole = true;
        if (const char* warm = GetEnvVariableTemp("SUMATRA_EDGE_PROBE_PREPARE_MS")) {
            EdgeSpeechPrepare();
            Sleep((DWORD)atoi(warm));
        }
        int repeats = 1;
        if (const char* count = GetEnvVariableTemp("SUMATRA_EDGE_PROBE_REPEATS")) repeats = atoi(count);
        for (int i = 0; i < repeats; i++) {
            int result = ProbeEdgeSpeech(argv[2], *voice);
            if (result) return result;
        }
        return 0;
    }
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
    EdgeSpeechResponse_UnitTests();
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
