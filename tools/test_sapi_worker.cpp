// Real COM integration probe: uses the same worker as TextToSpeech.cpp.
#include "../src/SapiSpeechWorker.h"
#include <stdio.h>
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")

static const WCHAR* text =
    L"Alpha paragraph explains continuous reading. Each word must belong to its own generation and paragraph.";
static const WCHAR* nextText =
    L"Bravo paragraph is prepared while the first paragraph is playing. Cached audio must be queued once.";
static const WCHAR* lastText = L"Charlie paragraph concludes this reading test.";
static bool Check(bool ok, const char* why) {
    if (!ok) printf("FAIL %s\n", why);
    return ok;
}
int wmain(int argc, WCHAR** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const WCHAR* token = argv[1];
    const WCHAR* mode = argv[2];
    bool unavailable = wcscmp(mode, L"unavailable") == 0;
    CLSID missingClass{};
    SapiSpeechWorker worker(unavailable ? missingClass : CLSID_HdSpeechPreparer);
    ULONGLONG generation = worker.Advance();
    worker.Submit(generation, 1, text, token, 0, false);
    worker.Submit(generation, 2, nextText, token, 0, true);
    bool control = wcscmp(mode, L"abc") != 0 && !unavailable;
    bool isStop =
        wcscmp(mode, L"stop") == 0 || wcscmp(mode, L"prepared-stop") == 0 || wcscmp(mode, L"repeated-stop") == 0;
    bool duringPrepared = wcscmp(mode, L"prepared-stop") == 0 || wcscmp(mode, L"prepared-jump") == 0;
    bool acted = false;
    bool early = wcscmp(mode, L"early-jump") == 0;
    ULONG chunk = 1;
    bool started = false;
    DWORD startedAt = 0;
    bool segmentPrepared = false;
    int segmentWords = 0;
    int starts = 0, ends = 0, words = 0, prepared = 0;
    auto change = [&]() {
        ULONGLONG old = generation;
        generation = worker.Advance();
        worker.Submit(generation, 0, nullptr, nullptr, 0, false);
        if (wcscmp(mode, L"repeated-stop") == 0) {
            // Match the UI's repeated-stop path: each Advance supersedes the
            // pending commands, so each latest generation needs its own barrier.
            for (int i = 0; i < 4; i++) {
                generation = worker.Advance();
                if (worker.IsRunning()) worker.Submit(generation, 0, nullptr, nullptr, 0, false);
            }
        }
        SapiSpeechWorker::Event stale;
        if (!Check(!worker.Pop(old, 1, stale) && !worker.Pop(old, 2, stale), "obsolete events survived Advance"))
            return false;
        if (isStop) {
            Sleep(1200);
            return Check(!worker.Pop(old, 1, stale) && !worker.Pop(old, 2, stale),
                         "late obsolete events survived stop");
        }
        WCHAR other[2048];
        wcscpy_s(other, token);
        if (wcscmp(mode, L"voice") == 0) {
            WCHAR* part = wcsstr(other, L"Xiaoxiao");
            if (!part) return false;
            wcscpy_s(part, 2048 - (part - other), L"Yunyi");
        }
        chunk = 100;
        started = false;
        starts = ends = words = prepared = 0;
        LONG rate = wcscmp(mode, L"rate") == 0 ? 4 : 0;
        worker.Submit(generation, 100, lastText, other, rate, false);
        worker.Submit(generation, 101, nextText, other, rate, true);
        return true;
    };
    if (early) {
        if (!change()) return 1;
        acted = true;
    }
    DWORD deadline = GetTickCount() + 120000;
    while ((LONG)(deadline - GetTickCount()) > 0) {
        SapiSpeechWorker::Event e;
        if (!worker.Pop(generation, chunk, e)) {
            Sleep(2);
            continue;
        }
        if (!Check(e.generation == generation && e.chunk == chunk, "wrong identity")) return 1;
        if (e.kind == SPEI_START_INPUT_STREAM) {
            if (!Check(!started, "duplicate START")) return 1;
            started = true;
            startedAt = GetTickCount();
            segmentPrepared = e.prepared;
            segmentWords = 0;
            starts++;
            if (e.prepared) prepared++;
            printf("START generation=%llu chunk=%lu prepared=%d\n", generation, chunk, e.prepared);
        } else if (e.kind == SPEI_WORD_BOUNDARY) {
            const WCHAR* actual = chunk == 1 ? text : (chunk == 2 || chunk == 101) ? nextText : lastText;
            if (!Check(started && e.position + e.length <= wcslen(actual), "word outside its paragraph")) return 1;
            if (argc > 3)
                printf("WORD tick=%lu chunk=%lu pos=%lu len=%lu\n", GetTickCount(), chunk, e.position, e.length);
            if (!Check(segmentWords != 0 || e.position == 0, "first word did not start at zero")) return 1;
            segmentWords++;
            words++;
            if (control && !acted && (!duringPrepared || chunk == 2)) {
                if (!change()) return 1;
                acted = true;
                if (isStop) {
                    printf("PASS stop\n");
                    return 0;
                }
            }
        } else if (e.kind == SPEI_END_INPUT_STREAM) {
            if (!Check(SUCCEEDED(e.error) && started, "failed stream or END before START")) return 1;
            if (segmentPrepared && !Check(GetTickCount() - startedAt > 1000 && segmentWords >= 5,
                                          "prepared audio ended without complete playback"))
                return 1;
            ends++;
            if (!control && chunk == 1) worker.Submit(generation, 3, lastText, token, 0, true);
            if ((!control && chunk == 3) || (control && chunk == 101)) {
                int expected = control ? 2 : 3;
                if (!Check(starts == expected && ends == expected && words > expected, "event counts")) return 1;
                if (!unavailable && wcsstr(token, L"TtsApplicationHD-") &&
                    !Check(prepared > 0, "HD service was not used"))
                    return 1;
                if (unavailable && !Check(prepared == 0, "missing-service fallback used prepared audio")) return 1;
                printf("PASS starts=%d ends=%d words=%d prepared=%d\n", starts, ends, words, prepared);
                return 0;
            }
            chunk++;
            started = false;
        }
    }
    printf("FAIL timeout\n");
    return 1;
}
